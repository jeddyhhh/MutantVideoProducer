#include "core/LiveCapture.h"

#include "core/Deinterlacer.h"
#include "core/FFmpeg.h"
#include "core/FrameConverter.h"

extern "C" {
#include <libavdevice/avdevice.h>
}

#include <QElapsedTimer>
#include <QPainter>
#include <QFileInfo>
#include <QGuiApplication>
#include <QScreen>
#include <QStringList>
#include <atomic>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <mutex>
#include <thread>

#ifdef Q_OS_WIN
#include <windows.h>
#include <dwmapi.h>
#endif
#ifdef Q_OS_LINUX
#include <dlfcn.h>
#endif

namespace {

void registerDevices()
{
    static std::once_flag once;
    std::call_once(once, [] {
        ff::initialize();
        avdevice_register_all();
    });
}

// Video capture devices of an FFmpeg input device (dshow, video4linux2).
void listDevices(const char* format, std::vector<LiveSource>& out)
{
    const AVInputFormat* fmt = av_find_input_format(format);
    if (!fmt)
        return;
    AVDeviceInfoList* list = nullptr;
    if (avdevice_list_input_sources(fmt, nullptr, nullptr, &list) < 0 || !list) {
        avdevice_free_list_devices(&list);
        return;
    }
    for (int i = 0; i < list->nb_devices; ++i) {
        const AVDeviceInfo* d = list->devices[i];
        bool video = d->nb_media_types == 0; // type unknown (e.g. a virtual camera that is not running)
        for (int k = 0; k < d->nb_media_types; ++k)
            video |= d->media_types[k] == AVMEDIA_TYPE_VIDEO;
        if (!video || !d->device_name)
            continue;
        LiveSource s;
        s.kind = LiveSource::Kind::Device;
        s.id = QString::fromUtf8(d->device_name);
        s.name = d->device_description && *d->device_description ? QString::fromUtf8(d->device_description) : s.id;
        out.push_back(s);
    }
    avdevice_free_list_devices(&list);
}

#ifdef Q_OS_WIN
BOOL CALLBACK addMonitor(HMONITOR monitor, HDC, LPRECT, LPARAM data)
{
    auto& out = *reinterpret_cast<std::vector<LiveSource>*>(data);
    MONITORINFOEXW info{};
    info.cbSize = sizeof(info);
    if (!GetMonitorInfoW(monitor, &info))
        return TRUE;
    const RECT& r = info.rcMonitor;
    LiveSource s;
    s.kind = LiveSource::Kind::Screen;
    s.id = QString::number(quint64(reinterpret_cast<quintptr>(monitor)));
    s.geometry = QRect(r.left, r.top, r.right - r.left, r.bottom - r.top);
    int screens = 0;
    for (const auto& o : out)
        screens += o.kind == LiveSource::Kind::Screen;
    s.name = QStringLiteral("Screen %1 (%2x%3%4)")
                 .arg(screens + 1)
                 .arg(s.geometry.width())
                 .arg(s.geometry.height())
                 .arg((info.dwFlags & MONITORINFOF_PRIMARY) ? QStringLiteral(", main") : QString());
    out.push_back(s);
    return TRUE;
}

BOOL CALLBACK addWindow(HWND hwnd, LPARAM data)
{
    auto& out = *reinterpret_cast<std::vector<LiveSource>*>(data);
    if (!IsWindowVisible(hwnd) || GetWindow(hwnd, GW_OWNER) || (GetWindowLongW(hwnd, GWL_EXSTYLE) & WS_EX_TOOLWINDOW))
        return TRUE;
    BOOL cloaked = FALSE; // hidden UWP / other-desktop windows
    if (SUCCEEDED(DwmGetWindowAttribute(hwnd, DWMWA_CLOAKED, &cloaked, sizeof(cloaked))) && cloaked)
        return TRUE;
    RECT r{};
    if (!GetWindowRect(hwnd, &r) || r.right - r.left < 32 || r.bottom - r.top < 32)
        return TRUE;
    wchar_t title[512];
    const int n = GetWindowTextW(hwnd, title, 512);
    if (n <= 0)
        return TRUE;
    const QString t = QString::fromWCharArray(title, n);
    if (t == QLatin1String("Program Manager"))
        return TRUE;
    QString exe;
    DWORD pid = 0;
    GetWindowThreadProcessId(hwnd, &pid);
    if (HANDLE proc = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid)) {
        wchar_t path[MAX_PATH];
        DWORD len = MAX_PATH;
        if (QueryFullProcessImageNameW(proc, 0, path, &len))
            exe = QString::fromWCharArray(path, int(len)).section(QLatin1Char('\\'), -1);
        CloseHandle(proc);
    }
    LiveSource s;
    s.kind = LiveSource::Kind::Window;
    s.id = QString::number(quint64(reinterpret_cast<quintptr>(hwnd)));
    s.name = (t.size() > 70 ? t.left(67) + QStringLiteral("...") : t) + (exe.isEmpty() ? QString() : QStringLiteral("  (%1)").arg(exe));
    s.geometry = QRect(r.left, r.top, r.right - r.left, r.bottom - r.top);
    out.push_back(s);
    return TRUE;
}
#endif

#ifdef Q_OS_LINUX
// Top-level windows from the window manager (_NET_CLIENT_LIST), via libX11
// loaded at run time so the program does not depend on it.
void listX11Windows(std::vector<LiveSource>& out)
{
    void* x11 = dlopen("libX11.so.6", RTLD_LAZY);
    if (!x11)
        return;
    using Display = void;
    using Window = unsigned long;
    using Atom = unsigned long;
    auto openDisplay = reinterpret_cast<Display* (*)(const char*)>(dlsym(x11, "XOpenDisplay"));
    auto closeDisplay = reinterpret_cast<int (*)(Display*)>(dlsym(x11, "XCloseDisplay"));
    auto internAtom = reinterpret_cast<Atom (*)(Display*, const char*, int)>(dlsym(x11, "XInternAtom"));
    auto defaultRoot = reinterpret_cast<Window (*)(Display*)>(dlsym(x11, "XDefaultRootWindow"));
    auto getProperty = reinterpret_cast<int (*)(Display*, Window, Atom, long, long, int, Atom, Atom*, int*, unsigned long*,
                                                unsigned long*, unsigned char**)>(dlsym(x11, "XGetWindowProperty"));
    auto xfree = reinterpret_cast<int (*)(void*)>(dlsym(x11, "XFree"));
    if (!openDisplay || !closeDisplay || !internAtom || !defaultRoot || !getProperty || !xfree) {
        dlclose(x11);
        return;
    }
    if (Display* dpy = openDisplay(nullptr)) {
        const Atom clientList = internAtom(dpy, "_NET_CLIENT_LIST", 1), netName = internAtom(dpy, "_NET_WM_NAME", 0),
                   utf8 = internAtom(dpy, "UTF8_STRING", 0);
        Atom type = 0;
        int format = 0;
        unsigned long count = 0, after = 0;
        unsigned char* data = nullptr;
        if (clientList && getProperty(dpy, defaultRoot(dpy), clientList, 0, 4096, 0, 33 /* XA_WINDOW */, &type, &format, &count, &after, &data) == 0
            && data) {
            const auto* windows = reinterpret_cast<const Window*>(data);
            for (unsigned long i = 0; i < count; ++i) {
                unsigned char* name = nullptr;
                unsigned long n = 0;
                QString title;
                if (getProperty(dpy, windows[i], netName, 0, 1024, 0, utf8, &type, &format, &n, &after, &name) == 0 && name) {
                    title = QString::fromUtf8(reinterpret_cast<const char*>(name), int(n));
                    xfree(name);
                }
                if (title.isEmpty())
                    continue;
                LiveSource s;
                s.kind = LiveSource::Kind::Window;
                s.id = QString::number(quint64(windows[i]));
                s.name = title.size() > 70 ? title.left(67) + QStringLiteral("...") : title;
                out.push_back(s);
            }
            xfree(data);
        }
        closeDisplay(dpy);
    }
    dlclose(x11);
}
#endif

QSize fitInside(QSize src, QSize box)
{
    if (!box.isValid() || box.isEmpty() || (src.width() <= box.width() && src.height() <= box.height()))
        return QSize(src.width() & ~1, src.height() & ~1).expandedTo(QSize(2, 2));
    const QSize s = src.scaled(box, Qt::KeepAspectRatio);
    return QSize(std::max(2, s.width() & ~1), std::max(2, s.height() & ~1));
}

} // namespace

QSize coverSize(QSize source, QSize canvas)
{
    if (!source.isValid() || !canvas.isValid())
        return canvas;
    const QSize s = source.scaled(canvas, Qt::KeepAspectRatioByExpanding);
    return QSize(std::max(canvas.width(), s.width() + 1) & ~1, std::max(canvas.height(), s.height() + 1) & ~1);
}

QRectF sourceRectInCanvas(QSizeF source, QSizeF canvas, LiveFraming framing, const LivePanZoom& pz, QPointF* used)
{
    if (source.isEmpty() || canvas.isEmpty()) {
        if (used)
            *used = pz.center;
        return QRectF(QPointF(0, 0), canvas);
    }
    const QSizeF base = source.scaled(canvas, framing == LiveFraming::CropFill ? Qt::KeepAspectRatioByExpanding : Qt::KeepAspectRatio);
    const QSizeF size = base * std::clamp(pz.zoom, 1.0, kLiveMaxZoom);
    auto place = [](double length, double room, double center) {
        if (length <= room + 1e-6) // smaller than the output: in the middle
            return (room - length) / 2;
        return std::clamp(room / 2 - center * length, room - length, 0.0);
    };
    const QRectF r(place(size.width(), canvas.width(), pz.center.x()), place(size.height(), canvas.height(), pz.center.y()), size.width(),
                   size.height());
    if (used)
        *used = QPointF((canvas.width() / 2 - r.x()) / r.width(), (canvas.height() / 2 - r.y()) / r.height());
    return r;
}

QImage frameToCanvas(const QImage& picture, QSize canvas, LiveFraming framing, const LivePanZoom& pz)
{
    if (picture.isNull() || !canvas.isValid() || (picture.size() == canvas && pz.isDefault()))
        return picture;
    const QImage in = picture.format() == QImage::Format_ARGB32_Premultiplied ? picture : picture.convertToFormat(QImage::Format_ARGB32_Premultiplied);
    const QRectF where = sourceRectInCanvas(QSizeF(in.size()), QSizeF(canvas), framing, pz);
    if (framing == LiveFraming::CropFill) {
        // Usually captured at about this size already (coverSize x zoom): then it is only a crop.
        if (std::abs(where.width() - in.width()) < 1.5 && std::abs(where.height() - in.height()) < 1.5) {
            const int x = std::clamp(int(std::lround(-where.x())), 0, std::max(0, in.width() - canvas.width()));
            const int y = std::clamp(int(std::lround(-where.y())), 0, std::max(0, in.height() - canvas.height()));
            return in.copy(QRect(x, y, canvas.width(), canvas.height()));
        }
        QImage out(canvas, QImage::Format_ARGB32_Premultiplied);
        out.fill(Qt::black);
        QPainter p(&out);
        p.setRenderHint(QPainter::SmoothPixmapTransform);
        p.drawImage(where, in);
        return out;
    }
    QImage out(canvas, QImage::Format_ARGB32_Premultiplied);
    out.fill(Qt::black);
    QPainter p(&out);
    p.setRenderHint(QPainter::SmoothPixmapTransform);
    if (framing == LiveFraming::FitBlur) {
        // Behind: the picture filling the frame, blurred (shrunk a lot and grown back) and darkened.
        const QSize cover = in.size().scaled(canvas, Qt::KeepAspectRatioByExpanding);
        const QSize tiny = cover.scaled(QSize(std::max(4, canvas.width() / 24), std::max(4, canvas.height() / 24)), Qt::KeepAspectRatioByExpanding);
        const QImage blurred = in.scaled(tiny, Qt::IgnoreAspectRatio, Qt::SmoothTransformation)
                                 .scaled(tiny * 3, Qt::IgnoreAspectRatio, Qt::SmoothTransformation);
        p.drawImage(QRectF((canvas.width() - cover.width()) / 2.0, (canvas.height() - cover.height()) / 2.0, cover.width(), cover.height()), blurred);
        p.fillRect(out.rect(), QColor(0, 0, 0, 110));
    }
    if (where.size().toSize() == in.size())
        p.drawImage(where.topLeft().toPoint(), in);
    else
        p.drawImage(where, in);
    return out;
}

std::vector<LiveSource> listLiveSources()
{
    if (qEnvironmentVariableIsSet("MVP_NO_DEVICES")) // tests: no cameras, screens or windows (video files only)
        return {};
    registerDevices();
    std::vector<LiveSource> out;
#ifdef Q_OS_WIN
    // DirectShow listing initialises and uninitialises COM on the calling
    // thread: keep that off the GUI thread.
    std::thread([&out] { listDevices("dshow", out); }).join();
    EnumDisplayMonitors(nullptr, nullptr, addMonitor, reinterpret_cast<LPARAM>(&out));
    EnumWindows(addWindow, reinterpret_cast<LPARAM>(&out));
#else
    listDevices("video4linux2", out);
    if (qobject_cast<QGuiApplication*>(QCoreApplication::instance())) {
        int i = 0;
        for (QScreen* screen : QGuiApplication::screens()) {
            LiveSource s;
            s.kind = LiveSource::Kind::Screen;
            const QRect g = screen->geometry();
            const double dpr = screen->devicePixelRatio();
            s.geometry = QRect(int(g.x() * dpr), int(g.y() * dpr), int(g.width() * dpr), int(g.height() * dpr));
            s.id = QString::number(i);
            s.name = QStringLiteral("Screen %1 (%2x%3)").arg(++i).arg(s.geometry.width()).arg(s.geometry.height());
            out.push_back(s);
        }
    }
#ifdef Q_OS_LINUX
    listX11Windows(out);
#endif
#endif
    return out;
}

LiveSource editorLiveSource()
{
    LiveSource s;
    s.kind = LiveSource::Kind::Editor;
    s.id = QStringLiteral("editor");
    s.name = QStringLiteral("Editor timeline");
    return s;
}

namespace {
std::mutex g_feedMutex;
QImage g_feed;
uint64_t g_feedSerial = 0;
} // namespace

void setEditorFeed(const QImage& picture)
{
    std::lock_guard lock(g_feedMutex);
    g_feed = picture;
    ++g_feedSerial;
}

// ---- Capture ---------------------------------------------------------------------------

struct LiveCapture::Impl {
    AVFormatContext* fmt = nullptr;
    AVCodecContext* dec = nullptr;
    int stream = -1;
    bool wrapped = false; // lavfi: packets carry AVFrames
    std::thread thread;
    std::atomic<bool> quit{false};
    mutable std::mutex mutex;
    std::condition_variable doneCv;
    bool done = false;    // the reading thread has finished and closed the source
    bool started = false; // a reading thread owns the source (it closes it)
    QImage latest;
    uint64_t serial = 0;
    QString failure, description;
    double fps = 0;
    QSize srcSize, maxSize;
    FrameConverter converter;
    // Interlaced pictures (a DV file, a 1080i capture card): made whole first, one frame behind.
    Deinterlacer deint;
    ff::FramePtr whole;
    bool interlaced = false;
    // Files: the playlist, the one playing, and pacing by their timestamps.
    bool file = false;
    QStringList playlist;
    int playing = 0;
    std::atomic<bool> skip{false};
    double firstPts = -1;
    std::chrono::steady_clock::time_point wallStart;
    // The Editor: no reader; latest() takes the newest preview picture.
    bool editor = false;
    uint64_t feedSerial = 0;
    QElapsedTimer feedTimer;
    int feedFrames = 0;

    void takeFeed()
    {
        QImage picture;
        uint64_t s;
        {
            std::lock_guard lock(g_feedMutex);
            picture = g_feed;
            s = g_feedSerial;
        }
        std::lock_guard lock(mutex);
        if (s == feedSerial || picture.isNull())
            return;
        feedSerial = s;
        srcSize = picture.size();
        const QSize want = fitInside(picture.size(), maxSize);
        if (want != picture.size())
            picture = picture.scaled(want, Qt::IgnoreAspectRatio, Qt::SmoothTransformation);
        latest = picture.format() == QImage::Format_ARGB32_Premultiplied ? picture : picture.convertToFormat(QImage::Format_ARGB32_Premultiplied);
        ++serial;
        if (!feedTimer.isValid())
            feedTimer.start();
        ++feedFrames;
        if (feedTimer.elapsed() >= 1000) {
            fps = feedFrames * 1000.0 / double(feedTimer.restart());
            feedFrames = 0;
        }
    }

    static int interrupt(void* p) { return static_cast<Impl*>(p)->quit.load() ? 1 : 0; }

    bool open(const char* formatName, const QString& url, const std::vector<std::pair<const char*, QString>>& options, QString* error)
    {
        close();
        const AVInputFormat* f = formatName ? av_find_input_format(formatName) : nullptr; // null: a file, found by its contents
        if (formatName && !f) {
            *error = QStringLiteral("FFmpeg has no %1 capture support").arg(QString::fromLatin1(formatName));
            return false;
        }
        fmt = avformat_alloc_context();
        fmt->interrupt_callback = {&Impl::interrupt, this};
        if (formatName)
            fmt->flags |= AVFMT_FLAG_NONBLOCK;
        AVDictionary* opts = nullptr;
        for (const auto& [k, v] : options)
            if (!v.isEmpty())
                av_dict_set(&opts, k, v.toUtf8().constData(), 0);
        const int r = avformat_open_input(&fmt, url.toUtf8().constData(), f, &opts);
        av_dict_free(&opts);
        if (r < 0) {
            fmt = nullptr; // freed by avformat_open_input
            *error = ff::errorString(r);
            return false;
        }
        fmt->flags &= ~AVFMT_FLAG_NONBLOCK; // stream info needs real frames
        if (avformat_find_stream_info(fmt, nullptr) < 0 || (stream = av_find_best_stream(fmt, AVMEDIA_TYPE_VIDEO, -1, -1, nullptr, 0)) < 0) {
            *error = QStringLiteral("no video from this source");
            close();
            return false;
        }
        if (formatName)
            fmt->flags |= AVFMT_FLAG_NONBLOCK;
        const AVCodecParameters* par = fmt->streams[stream]->codecpar;
        wrapped = par->codec_id == AV_CODEC_ID_WRAPPED_AVFRAME;
        if (!wrapped) {
            const AVCodec* codec = avcodec_find_decoder(par->codec_id);
            if (!codec || !(dec = avcodec_alloc_context3(codec)) || avcodec_parameters_to_context(dec, par) < 0) {
                *error = QStringLiteral("cannot decode %1").arg(QString::fromLatin1(avcodec_get_name(par->codec_id)));
                close();
                return false;
            }
            dec->thread_count = formatName ? 2 : 0; // files: as many as it likes (4K clips)
            if (formatName)
                dec->flags |= AV_CODEC_FLAG_LOW_DELAY;
            if (avcodec_open2(dec, codec, nullptr) < 0) {
                *error = QStringLiteral("cannot open the %1 decoder").arg(QString::fromLatin1(codec->name));
                close();
                return false;
            }
        }
        srcSize = QSize(par->width, par->height);
        return true;
    }

    void close()
    {
        avcodec_free_context(&dec);
        if (fmt)
            avformat_close_input(&fmt);
        stream = -1;
    }

    void deliver(const AVFrame* frame, QElapsedTimer& timer, int& frames)
    {
        if (frame->width <= 0 || frame->height <= 0)
            return;
        QSize box;
        {
            std::lock_guard lock(mutex);
            box = maxSize;
        }
        QImage img = converter.convert(frame, fitInside(QSize(frame->width, frame->height), box), false);
        if (img.isNull())
            return;
        ++frames;
        std::lock_guard lock(mutex);
        latest = std::move(img);
        ++serial;
        srcSize = QSize(frame->width, frame->height);
        if (timer.elapsed() >= 1000) {
            fps = frames * 1000.0 / double(timer.restart());
            frames = 0;
        }
    }

    void deliverWhole(const AVFrame* frame, QElapsedTimer& timer, int& frames)
    {
        interlaced = interlaced || (frame->flags & AV_FRAME_FLAG_INTERLACED);
        if (!interlaced || !deint.push(frame)) {
            deliver(frame, timer, frames);
            return;
        }
        if (!whole)
            whole.reset(av_frame_alloc());
        while (deint.pull(whole.get()))
            deliver(whole.get(), timer, frames);
    }

    // Files: opens the next one in the playlist that opens (from `from`), restarting the pacing.
    bool openFile(int from)
    {
        deint.reset();
        interlaced = false;
        for (int tries = 0; tries < int(playlist.size()); ++tries) {
            const int i = (from + tries) % int(playlist.size());
            QString e;
            if (open(nullptr, playlist[i], {}, &e)) {
                std::lock_guard lock(mutex);
                playing = i;
                firstPts = -1;
                return true;
            }
        }
        return false;
    }

    // Files: waits until the frame's time comes (or a skip / stop).
    void pace(const AVFrame* f)
    {
        const AVRational tb = fmt->streams[stream]->time_base;
        const int64_t ts = f->best_effort_timestamp != AV_NOPTS_VALUE ? f->best_effort_timestamp : f->pts;
        if (ts == AV_NOPTS_VALUE)
            return;
        const double pts = double(ts) * av_q2d(tb);
        const auto now = std::chrono::steady_clock::now();
        if (firstPts < 0 || pts < firstPts) {
            firstPts = pts;
            wallStart = now;
            return;
        }
        const auto due = wallStart + std::chrono::microseconds(int64_t((pts - firstPts) * 1e6));
        if (due > now + std::chrono::seconds(2)) { // a jump in the timestamps: start the clock again
            firstPts = pts;
            wallStart = now;
            return;
        }
        while (!quit && !skip && std::chrono::steady_clock::now() < due)
            std::this_thread::sleep_for(std::min<std::chrono::steady_clock::duration>(due - std::chrono::steady_clock::now(), std::chrono::milliseconds(5)));
    }

    void run()
    {
        ff::PacketPtr pkt = ff::makePacket();
        ff::FramePtr frame = ff::makeFrame();
        QElapsedTimer timer;
        timer.start();
        int frames = 0;
        while (!quit) {
            if (file && skip.exchange(false)) {
                if (!openFile(playing + 1))
                    break;
                continue;
            }
            const int r = av_read_frame(fmt, pkt.get());
            if (r == AVERROR(EAGAIN)) {
                std::this_thread::sleep_for(std::chrono::milliseconds(2));
                continue;
            }
            if (r == AVERROR_EOF && file) { // the end: the next file (or this one again)
                if (dec) { // the last pictures still in the decoder
                    avcodec_send_packet(dec, nullptr);
                    while (avcodec_receive_frame(dec, frame.get()) == 0) {
                        pace(frame.get());
                        deliverWhole(frame.get(), timer, frames);
                        av_frame_unref(frame.get());
                    }
                }
                if (!openFile(playlist.size() > 1 ? playing + 1 : playing)) {
                    std::lock_guard lock(mutex);
                    failure = QStringLiteral("the video files could not be opened again");
                    break;
                }
                continue;
            }
            if (r < 0) {
                if (!quit) {
                    std::lock_guard lock(mutex);
                    failure = r == AVERROR_EOF ? QStringLiteral("the source stopped sending pictures") : ff::errorString(r);
                }
                break;
            }
            if (pkt->stream_index == stream) {
                if (wrapped) {
                    if (pkt->data && pkt->size >= int(sizeof(AVFrame)))
                        deliver(reinterpret_cast<const AVFrame*>(pkt->data), timer, frames);
                } else if (avcodec_send_packet(dec, pkt.get()) >= 0) {
                    while (avcodec_receive_frame(dec, frame.get()) == 0) {
                        if (file)
                            pace(frame.get());
                        deliverWhole(frame.get(), timer, frames);
                        av_frame_unref(frame.get());
                    }
                }
            }
            av_packet_unref(pkt.get());
        }
        close();
        std::lock_guard lock(mutex);
        done = true;
        doneCv.notify_all();
    }
};

LiveCapture::LiveCapture() : d(std::make_shared<Impl>()) {}

LiveCapture::~LiveCapture()
{
    stop();
}

bool LiveCapture::start(const LiveSource& source, const LiveFormat& format, QSize maxSize, QString* error)
{
    stop();
    registerDevices();
    d = std::make_shared<Impl>(); // fresh state: a previous reader may still be winding down
    d->maxSize = maxSize;
    const QString fps = format.fps > 0 ? QString::number(format.fps) : QString();
    const QString size = format.size.isValid() && !format.size.isEmpty() ? QStringLiteral("%1x%2").arg(format.size.width()).arg(format.size.height()) : QString();
    const QString captureFps = QString::number(format.fps > 0 ? format.fps : 60.0);
    bool ok = false;
    QStringList tried;
    auto attempt = [&](const char* fmt, const QString& url, const std::vector<std::pair<const char*, QString>>& options, const QString& what) {
        if (ok)
            return;
        QString e;
        ok = d->open(fmt, url, options, &e);
        if (ok)
            d->description = what;
        else
            tried << QStringLiteral("%1: %2").arg(what, e);
    };
    switch (source.kind) {
    case LiveSource::Kind::Editor:
        d->editor = true;
        d->description = QStringLiteral("Editor timeline");
        return true;
    case LiveSource::Kind::Device:
#ifdef Q_OS_WIN
        attempt("dshow", QStringLiteral("video=") + source.id, {{"rtbufsize", QStringLiteral("256M")}, {"video_size", size}, {"framerate", fps}},
                QStringLiteral("DirectShow"));
        if (!size.isEmpty() || !fps.isEmpty()) // the camera may not offer that format
            attempt("dshow", QStringLiteral("video=") + source.id, {{"rtbufsize", QStringLiteral("256M")}}, QStringLiteral("DirectShow (default format)"));
#else
        attempt("video4linux2", source.id, {{"video_size", size}, {"framerate", fps}}, QStringLiteral("V4L2"));
        if (!size.isEmpty() || !fps.isEmpty())
            attempt("video4linux2", source.id, {}, QStringLiteral("V4L2 (default format)"));
#endif
        break;
    case LiveSource::Kind::Screen:
#ifdef Q_OS_WIN
        attempt("lavfi", QStringLiteral("gfxcapture=hmonitor=%1:max_framerate=%2:capture_cursor=1,hwdownload,format=bgra").arg(source.id, captureFps), {},
                QStringLiteral("Windows.Graphics.Capture"));
        attempt("gdigrab", QStringLiteral("desktop"),
                {{"offset_x", QString::number(source.geometry.x())}, {"offset_y", QString::number(source.geometry.y())},
                 {"video_size", QStringLiteral("%1x%2").arg(source.geometry.width()).arg(source.geometry.height())},
                 {"framerate", captureFps}, {"draw_mouse", QStringLiteral("1")}},
                QStringLiteral("GDI"));
#else
        attempt("x11grab", QString::fromLocal8Bit(qgetenv("DISPLAY")) + QStringLiteral("+%1,%2").arg(source.geometry.x()).arg(source.geometry.y()),
                {{"video_size", QStringLiteral("%1x%2").arg(source.geometry.width()).arg(source.geometry.height())},
                 {"framerate", captureFps}, {"draw_mouse", QStringLiteral("1")}},
                QStringLiteral("X11"));
#endif
        break;
    case LiveSource::Kind::Window:
#ifdef Q_OS_WIN
        attempt("lavfi", QStringLiteral("gfxcapture=hwnd=%1:max_framerate=%2:capture_cursor=1,hwdownload,format=bgra").arg(source.id, captureFps), {},
                QStringLiteral("Windows.Graphics.Capture"));
        attempt("gdigrab", QStringLiteral("hwnd=0x%1").arg(source.id.toULongLong(), 0, 16), {{"framerate", captureFps}}, QStringLiteral("GDI"));
#else
        attempt("x11grab", QString::fromLocal8Bit(qgetenv("DISPLAY")), {{"window_id", source.id}, {"framerate", captureFps}}, QStringLiteral("X11"));
#endif
        break;
    case LiveSource::Kind::File:
        d->file = true;
        d->playlist = source.id.split(QLatin1Char('\n'), Qt::SkipEmptyParts);
        for (int i = 0; i < int(d->playlist.size()) && !ok; ++i) {
            attempt(nullptr, d->playlist[i], {}, QStringLiteral("File %1").arg(QFileInfo(d->playlist[i]).fileName()));
            if (ok)
                d->playing = i;
        }
        if (ok && d->playlist.size() > 1)
            d->description = QStringLiteral("Playlist of %1 videos").arg(d->playlist.size());
        break;
    }
    if (!ok) {
        if (error)
            *error = tried.join(QStringLiteral("; "));
        return false;
    }
    const AVCodecParameters* par = d->fmt->streams[d->stream]->codecpar;
    d->description += QStringLiteral(" %1x%2 %3").arg(par->width).arg(par->height).arg(QString::fromLatin1(avcodec_get_name(par->codec_id)));
    d->started = true;
    d->thread = std::thread([self = d] { self->run(); });
    return true;
}

void LiveCapture::stop()
{
    d->quit = true;
    if (!d->thread.joinable()) {
        if (!d->started) // opened but never read; a reader (even a detached one) closes its own source
            d->close();
        return;
    }
    // Some sources (an unchanging window) block in av_read_frame until the next
    // picture: don't wait for that; the thread closes the source when it returns.
    bool finished;
    {
        std::unique_lock lock(d->mutex);
        finished = d->doneCv.wait_for(lock, std::chrono::milliseconds(500), [&] { return d->done; });
    }
    if (finished)
        d->thread.join();
    else
        d->thread.detach();
}

void LiveCapture::setMaxSize(QSize maxSize)
{
    std::lock_guard lock(d->mutex);
    d->maxSize = maxSize;
}

QImage LiveCapture::latest(uint64_t* serial) const
{
    if (d->editor)
        d->takeFeed();
    std::lock_guard lock(d->mutex);
    if (serial)
        *serial = d->serial;
    return d->latest;
}

QSize LiveCapture::sourceSize() const
{
    std::lock_guard lock(d->mutex);
    return d->srcSize;
}

double LiveCapture::captureFps() const
{
    std::lock_guard lock(d->mutex);
    return d->fps;
}

QString LiveCapture::failure() const
{
    std::lock_guard lock(d->mutex);
    return d->failure;
}

QString LiveCapture::description() const
{
    return d->description;
}

void LiveCapture::nextFile()
{
    d->skip = true;
}

QString LiveCapture::currentFile() const
{
    std::lock_guard lock(d->mutex);
    return d->file && d->playing < int(d->playlist.size()) ? d->playlist[d->playing] : QString();
}
