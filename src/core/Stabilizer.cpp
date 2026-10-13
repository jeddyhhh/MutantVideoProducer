#include "core/Brand.h"
#include "core/Stabilizer.h"

#include "core/FrameConverter.h"
#include "core/MediaInfo.h"
#include "core/VideoDecoder.h"

#include <QCryptographicHash>
#include <QDataStream>
#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QImage>
#include <QStandardPaths>
#include <algorithm>
#include <cmath>
#include <map>
#include <memory>
#include <mutex>
#include <numbers>
#include <climits>

namespace stabilize {
namespace {

constexpr int kWidth = 320; // analysis width
constexpr quint32 kMagic = 0x53544231; // "STB1"

struct Gray {
    int w = 0, h = 0;
    std::vector<uint8_t> p;
    int at(int x, int y) const { return p[size_t(y) * size_t(w) + size_t(x)]; }
};

Gray toGray(const QImage& img)
{
    Gray g;
    g.w = img.width();
    g.h = img.height();
    g.p.resize(size_t(g.w) * size_t(g.h));
    for (int y = 0; y < g.h; ++y) {
        const QRgb* row = reinterpret_cast<const QRgb*>(img.constScanLine(y));
        for (int x = 0; x < g.w; ++x)
            g.p[size_t(y) * size_t(g.w) + size_t(x)] = uint8_t((qRed(row[x]) * 77 + qGreen(row[x]) * 150 + qBlue(row[x]) * 29) >> 8);
    }
    return g;
}

// Shift and rotation (about the centre) that take frame a to frame b.
bool estimate(const Gray& a, const Gray& b, float& dx, float& dy, float& da)
{
    constexpr int B = 16, R = 12;
    struct Vec {
        float px, py, vx, vy;
    };
    std::vector<Vec> vecs;
    const int cols = 8, rows = 6;
    for (int cy = 0; cy < rows; ++cy)
        for (int cx = 0; cx < cols; ++cx) {
            // The most textured block in the cell (flat sky can't be matched).
            const int x0 = std::max(R, cx * a.w / cols), x1 = std::min(a.w - B - R - 1, (cx + 1) * a.w / cols - B);
            const int y0 = std::max(R, cy * a.h / rows), y1 = std::min(a.h - B - R - 1, (cy + 1) * a.h / rows - B);
            if (x1 < x0 || y1 < y0)
                continue;
            int bx = x0, by = y0, best = -1;
            for (int y = y0; y <= y1; y += std::max(1, (y1 - y0) / 2))
                for (int x = x0; x <= x1; x += std::max(1, (x1 - x0) / 2)) {
                    int tex = 0;
                    for (int j = 0; j < B; j += 2)
                        for (int i = 0; i < B; i += 2)
                            tex += std::abs(a.at(x + i + 1, y + j) - a.at(x + i, y + j)) + std::abs(a.at(x + i, y + j + 1) - a.at(x + i, y + j));
                    if (tex > best) {
                        best = tex;
                        bx = x;
                        by = y;
                    }
                }
            if (best < B * B / 4 * 6)
                continue;
            // Where it went: the smallest difference within +-R, then a parabola for a fraction of a pixel.
            int sad[2 * R + 1][2 * R + 1];
            int minSad = INT_MAX, mx = 0, my = 0;
            for (int vy = -R; vy <= R; ++vy)
                for (int vx = -R; vx <= R; ++vx) {
                    int s = 0;
                    for (int j = 0; j < B && s < minSad; ++j)
                        for (int i = 0; i < B; ++i)
                            s += std::abs(a.at(bx + i, by + j) - b.at(bx + i + vx, by + j + vy));
                    sad[vy + R][vx + R] = s;
                    if (s < minSad) {
                        minSad = s;
                        mx = vx;
                        my = vy;
                    }
                }
            if (std::abs(mx) == R || std::abs(my) == R)
                continue; // at the edge of the search: unreliable
            auto sub = [](int l, int c, int r) {
                const double d = double(l) - 2.0 * c + r;
                return d > 0 ? std::clamp(0.5 * (l - r) / d, -0.5, 0.5) : 0.0;
            };
            const float fx = float(mx + sub(sad[my + R][mx + R - 1], minSad, sad[my + R][mx + R + 1]));
            const float fy = float(my + sub(sad[my + R - 1][mx + R], minSad, sad[my + R + 1][mx + R]));
            vecs.push_back({float(bx + B / 2) - a.w / 2.0f, float(by + B / 2) - a.h / 2.0f, fx, fy});
        }
    if (vecs.size() < 6)
        return false;
    // Least squares for v = (-a py + tx, a px + ty), dropping blocks that disagree
    // (things moving on their own) a few times.
    std::vector<bool> use(vecs.size(), true);
    double tx = 0, ty = 0, an = 0;
    for (int iter = 0; iter < 4; ++iter) {
        double n = 0, spx = 0, spy = 0, svx = 0, svy = 0, spp = 0, cross = 0;
        for (size_t i = 0; i < vecs.size(); ++i)
            if (use[i]) {
                const Vec& v = vecs[i];
                n += 1;
                spx += v.px;
                spy += v.py;
                svx += v.vx;
                svy += v.vy;
                spp += v.px * v.px + v.py * v.py;
                cross += v.vy * v.px - v.vx * v.py;
            }
        if (n < 4)
            break;
        const double den = spp - (spx * spx + spy * spy) / n;
        an = den > 1e-6 ? (cross + (spy * svx - spx * svy) / n) / den : 0.0;
        tx = (svx + an * spy) / n;
        ty = (svy - an * spx) / n;
        std::vector<double> res;
        for (const Vec& v : vecs)
            res.push_back(std::hypot(v.vx - (-an * v.py + tx), v.vy - (an * v.px + ty)));
        std::vector<double> sorted = res;
        std::nth_element(sorted.begin(), sorted.begin() + std::ptrdiff_t(sorted.size() / 2), sorted.end());
        const double limit = std::max(1.0, 2.5 * sorted[sorted.size() / 2]);
        for (size_t i = 0; i < vecs.size(); ++i)
            use[i] = res[i] <= limit;
    }
    dx = float(tx);
    dy = float(ty);
    da = float(an);
    return true;
}

QString cacheFile(const QString& path, int stream)
{
    const QFileInfo fi(path);
    const QByteArray key = (fi.absoluteFilePath() + QString::number(fi.size()) + QString::number(fi.lastModified().toMSecsSinceEpoch())
                            + QLatin1Char('#') + QString::number(stream))
                               .toUtf8();
    const QString dir = QDir(brand::storageLocation(QStandardPaths::CacheLocation)).filePath(QStringLiteral("stabilize"));
    QDir().mkpath(dir);
    return QDir(dir).filePath(QString::fromLatin1(QCryptographicHash::hash(key, QCryptographicHash::Md5).toHex()) + QStringLiteral(".stab"));
}

std::mutex g_mutex;
std::map<QString, std::shared_ptr<const Motion>> g_motion; // by cache file
struct Smooth {
    std::vector<float> cx, cy, ca;
    float zoom = 1;
};
std::map<QString, std::shared_ptr<const Smooth>> g_smooth;

std::shared_ptr<const Motion> load(const QString& file)
{
    {
        std::lock_guard lock(g_mutex);
        if (auto it = g_motion.find(file); it != g_motion.end())
            return it->second;
    }
    QFile f(file);
    if (!f.open(QIODevice::ReadOnly))
        return nullptr;
    QDataStream in(&f);
    quint32 magic = 0, n = 0;
    qint32 w = 0, h = 0;
    in >> magic >> w >> h >> n;
    if (magic != kMagic || n > 10'000'000)
        return nullptr;
    auto m = std::make_shared<Motion>();
    m->width = w;
    m->height = h;
    m->pts.resize(n);
    m->dx.resize(n);
    m->dy.resize(n);
    m->da.resize(n);
    for (quint32 i = 0; i < n; ++i) {
        qint64 pts;
        in >> pts >> m->dx[i] >> m->dy[i] >> m->da[i];
        m->pts[i] = pts;
    }
    if (in.status() != QDataStream::Ok)
        return nullptr;
    std::lock_guard lock(g_mutex);
    g_motion[file] = m;
    return m;
}

} // namespace

bool analyze(const QString& path, int stream, const std::function<bool(double)>& progress, QString* error)
{
    const QString file = cacheFile(path, stream);
    if (load(file))
        return true;
    const MediaInfo info = probeMedia(path);
    const VideoStreamInfo* vs = info.videoStream(stream);
    VideoDecoder dec;
    QString err;
    if (!vs || !dec.open(path, stream, &err, false)) {
        if (error)
            *error = vs ? err : QStringLiteral("no video");
        return false;
    }
    FrameConverter conv;
    const QSize size(kWidth, std::max(16, int(std::lround(double(kWidth) * vs->height / std::max(1, vs->width)))));
    Motion m;
    m.width = size.width();
    m.height = size.height();
    Gray prev;
    const AVFrame* f = dec.frameAt(0);
    while (f) {
        const TimeUs pts = dec.currentPts();
        const Gray cur = toGray(conv.convert(f, size, false).convertToFormat(QImage::Format_RGB32));
        float dx = 0, dy = 0, da = 0;
        if (!prev.p.empty() && !estimate(prev, cur, dx, dy, da))
            dx = dy = da = 0; // nothing to go on (a flat or dark picture): no motion
        m.pts.push_back(pts);
        m.dx.push_back(dx);
        m.dy.push_back(dy);
        m.da.push_back(da);
        prev = cur;
        if (progress && info.duration > 0 && !progress(std::clamp(double(pts) / double(info.duration), 0.0, 1.0)))
            return false;
        if (!dec.nextFrame())
            break;
        const TimeUs next = dec.nextPts();
        f = dec.frameAt(next);
        if (dec.currentPts() <= pts)
            break; // no further
    }
    QFile out(file);
    if (!out.open(QIODevice::WriteOnly)) {
        if (error)
            *error = QStringLiteral("cannot write the analysis to %1").arg(file);
        return false;
    }
    QDataStream o(&out);
    o << kMagic << qint32(m.width) << qint32(m.height) << quint32(m.pts.size());
    for (size_t i = 0; i < m.pts.size(); ++i)
        o << qint64(m.pts[i]) << m.dx[i] << m.dy[i] << m.da[i];
    out.close();
    std::lock_guard lock(g_mutex);
    g_motion[file] = std::make_shared<Motion>(std::move(m));
    return true;
}

bool isAnalyzed(const QString& path, int stream)
{
    return load(cacheFile(path, stream)) != nullptr;
}

Motion motion(const QString& path, int stream)
{
    const auto m = load(cacheFile(path, stream));
    return m ? *m : Motion();
}

QTransform correction(const QString& path, const VideoStreamInfo& vs, TimeUs srcT, double smoothing, bool zoom, TimeUs spanFrom, TimeUs spanTo)
{
    const QString file = cacheFile(path, vs.index);
    const auto m = load(file);
    if (!m || m->pts.size() < 2 || smoothing <= 0)
        return {};
    const size_t n = m->pts.size();
    auto frameOf = [&](TimeUs t) {
        const auto it = std::upper_bound(m->pts.begin(), m->pts.end(), t);
        return it == m->pts.begin() ? size_t(0) : size_t(it - m->pts.begin() - 1);
    };
    const size_t k0 = frameOf(spanFrom), k1 = std::max(k0, frameOf(spanTo));
    const QString key = file + QStringLiteral("|%1|%2|%3|%4").arg(smoothing).arg(zoom).arg(k0).arg(k1);
    std::shared_ptr<const Smooth> s;
    {
        std::lock_guard lock(g_mutex);
        if (auto it = g_smooth.find(key); it != g_smooth.end())
            s = it->second;
    }
    if (!s) {
        // The camera's path, and the path smoothed (a Gaussian over `smoothing` seconds).
        std::vector<double> tx(n), ty(n), ta(n);
        double x = 0, y = 0, a = 0;
        for (size_t i = 0; i < n; ++i) {
            x += m->dx[i];
            y += m->dy[i];
            a += m->da[i];
            tx[i] = x;
            ty[i] = y;
            ta[i] = a;
        }
        const double fps = double(n - 1) / std::max(1e-6, double(m->pts.back() - m->pts.front()) / 1e6);
        const int radius = std::max(1, int(std::lround(smoothing * fps)));
        const double sigma = radius / 2.0;
        auto out = std::make_shared<Smooth>();
        out->cx.resize(n);
        out->cy.resize(n);
        out->ca.resize(n);
        for (size_t i = 0; i < n; ++i) {
            double sx = 0, sy = 0, sa = 0, sw = 0;
            for (int d = -radius; d <= radius; ++d) {
                const ptrdiff_t j = ptrdiff_t(i) + d;
                if (j < 0 || j >= ptrdiff_t(n))
                    continue;
                const double w = std::exp(-0.5 * d * d / (sigma * sigma));
                sx += w * tx[size_t(j)];
                sy += w * ty[size_t(j)];
                sa += w * ta[size_t(j)];
                sw += w;
            }
            out->cx[i] = float(sx / sw - tx[i]);
            out->cy[i] = float(sy / sw - ty[i]);
            out->ca[i] = float(sa / sw - ta[i]);
        }
        // Zoom just enough that the moved and turned picture still covers the frame.
        float z = 1;
        if (zoom) {
            const double aspect = std::max(double(m->width) / m->height, double(m->height) / m->width);
            for (size_t i = k0; i <= k1 && i < n; ++i) {
                const double turn = std::abs(std::cos(out->ca[i])) + std::abs(std::sin(out->ca[i])) * aspect;
                const double shift = 1 + 2 * std::max(std::abs(out->cx[i]) / m->width, std::abs(out->cy[i]) / m->height);
                z = std::max(z, float(turn * shift));
            }
        }
        out->zoom = std::min(z, 2.0f);
        s = out;
        std::lock_guard lock(g_mutex);
        if (g_smooth.size() > 64)
            g_smooth.clear();
        g_smooth[key] = s;
    }
    const size_t k = frameOf(srcT);
    // Analysis pixels -> the upright picture -> display (rotation metadata).
    const QSizeF disp(vs.displaySize());
    const bool sideways = vs.rotation == 90 || vs.rotation == 270;
    const QSizeF upright = sideways ? QSizeF(disp.height(), disp.width()) : disp;
    QPointF shift(s->cx[k] * upright.width() / m->width, s->cy[k] * upright.height() / m->height);
    if (vs.rotation) {
        QTransform r;
        r.rotate(vs.rotation);
        shift = r.map(shift);
    }
    const QPointF c(disp.width() / 2, disp.height() / 2);
    QTransform t;
    t.translate(c.x() + shift.x(), c.y() + shift.y());
    t.rotateRadians(s->ca[k]);
    t.scale(s->zoom, s->zoom);
    t.translate(-c.x(), -c.y());
    return t;
}

} // namespace stabilize
