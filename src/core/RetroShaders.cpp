#include "core/RetroShaders.h"

#include <QtCore/private/qzipreader_p.h>

#include <QDir>
#include <QDirIterator>
#include <QFile>
#include <QFileInfo>
#include <QRegularExpression>
#include <QStandardPaths>
#include <QTextStream>
#include <algorithm>
#include <cmath>
#include <deque>
#include <memory>
#include <mutex>
#include <set>

namespace retro {

namespace {

// key = value lines (values may be quoted; # starts a comment).
std::map<QString, QString> readKeys(const QString& path)
{
    std::map<QString, QString> keys;
    QFile f(path);
    if (!f.open(QIODevice::ReadOnly | QIODevice::Text))
        return keys;
    for (const QString& raw : QString::fromUtf8(f.readAll()).split(QLatin1Char('\n'))) {
        QString line = raw.trimmed();
        if (line.startsWith(QLatin1Char('#')) || line.isEmpty())
            continue;
        const int eq = line.indexOf(QLatin1Char('='));
        if (eq <= 0)
            continue;
        QString key = line.left(eq).trimmed(), value = line.mid(eq + 1).trimmed();
        if (value.startsWith(QLatin1Char('"'))) {
            const int end = value.indexOf(QLatin1Char('"'), 1);
            value = end > 0 ? value.mid(1, end - 1) : value.mid(1);
        } else if (const int hash = value.indexOf(QLatin1Char('#')); hash >= 0) {
            value = value.left(hash).trimmed();
        }
        keys[key] = value;
    }
    return keys;
}

bool truthy(const QString& v) { return v == QLatin1String("true") || v == QLatin1String("1"); }

Wrap wrapOf(const QString& v)
{
    if (v == QLatin1String("clamp_to_edge"))
        return Wrap::Edge;
    if (v == QLatin1String("repeat"))
        return Wrap::Repeat;
    if (v == QLatin1String("mirrored_repeat"))
        return Wrap::Mirror;
    return Wrap::Border;
}

Scale scaleOf(const QString& v)
{
    if (v == QLatin1String("viewport"))
        return Scale::Viewport;
    if (v == QLatin1String("absolute"))
        return Scale::Absolute;
    return Scale::Source;
}

QString readText(const QString& path)
{
    QFile f(path);
    return f.open(QIODevice::ReadOnly | QIODevice::Text) ? QString::fromUtf8(f.readAll()) : QString();
}

} // namespace

bool loadPreset(const QString& path, Preset* out, QString* error)
{
    auto fail = [&](const QString& why) {
        if (error)
            *error = why;
        return false;
    };
    const auto keys = readKeys(path);
    auto key = [&](const QString& k) {
        const auto it = keys.find(k);
        return it == keys.end() ? QString() : it->second;
    };
    const int count = key(QStringLiteral("shaders")).toInt();
    if (count <= 0 || count > 32)
        return fail(QStringLiteral("no passes"));
    const QDir dir = QFileInfo(path).absoluteDir();
    Preset p;
    p.path = path;
    std::set<QString> seen;
    static const QRegularExpression pragma(QStringLiteral(R"re(^\s*#pragma\s+parameter\s+(\w+)\s+"([^"]*)"\s+([-+.\deE]+)\s+([-+.\deE]+)\s+([-+.\deE]+)(?:\s+([-+.\deE]+))?)re"),
                                           QRegularExpression::MultilineOption);
    static const QRegularExpression prev(QStringLiteral(R"(\bPrev(\d?)Texture\b)"));
    for (int i = 0; i < count; ++i) {
        const QString n = QString::number(i);
        Pass pass;
        const QString shader = key(QStringLiteral("shader") + n);
        if (shader.isEmpty())
            return fail(QStringLiteral("pass %1 has no shader").arg(i));
        pass.path = QDir::cleanPath(dir.filePath(shader));
        const QString src = readText(pass.path);
        if (src.isEmpty())
            return fail(QStringLiteral("can't read %1").arg(shader));
        pass.linear = truthy(key(QStringLiteral("filter_linear") + n));
        pass.wrap = wrapOf(key(QStringLiteral("wrap_mode") + n));
        pass.mipmapInput = truthy(key(QStringLiteral("mipmap_input") + n));
        const QString type = key(QStringLiteral("scale_type") + n);
        const QString typeX = key(QStringLiteral("scale_type_x") + n), typeY = key(QStringLiteral("scale_type_y") + n);
        pass.scaleSet = !type.isEmpty() || !typeX.isEmpty() || !typeY.isEmpty();
        pass.scaleX = scaleOf(typeX.isEmpty() ? type : typeX);
        pass.scaleY = scaleOf(typeY.isEmpty() ? type : typeY);
        const QString both = key(QStringLiteral("scale") + n);
        const QString sx = key(QStringLiteral("scale_x") + n), sy = key(QStringLiteral("scale_y") + n);
        pass.sx = (sx.isEmpty() ? both : sx).isEmpty() ? 1.0 : (sx.isEmpty() ? both : sx).toDouble();
        pass.sy = (sy.isEmpty() ? both : sy).isEmpty() ? 1.0 : (sy.isEmpty() ? both : sy).toDouble();
        pass.floatBuffer = truthy(key(QStringLiteral("float_framebuffer") + n));
        pass.srgbBuffer = truthy(key(QStringLiteral("srgb_framebuffer") + n));
        pass.frameCountMod = key(QStringLiteral("frame_count_mod") + n).toInt();
        pass.alias = key(QStringLiteral("alias") + n);
        for (auto it = pragma.globalMatch(src); it.hasNext();) {
            const auto m = it.next();
            if (!seen.insert(m.captured(1)).second)
                continue;
            Parameter par{m.captured(1), m.captured(2), m.captured(3).toDouble(), m.captured(4).toDouble(), m.captured(5).toDouble(),
                          m.captured(6).isEmpty() ? 0.01 : m.captured(6).toDouble()};
            if (par.max < par.min)
                std::swap(par.min, par.max);
            par.def = std::clamp(par.def, par.min, par.max);
            p.parameters.push_back(par);
        }
        for (auto it = prev.globalMatch(src); it.hasNext();) {
            const auto m = it.next();
            p.history = std::max(p.history, m.captured(1).isEmpty() ? 1 : m.captured(1).toInt() + 1);
        }
        p.passes.push_back(pass);
    }
    for (const QString& name : key(QStringLiteral("textures")).split(QLatin1Char(';'), Qt::SkipEmptyParts)) {
        Lut lut;
        lut.name = name.trimmed();
        lut.path = QDir::cleanPath(dir.filePath(key(lut.name)));
        lut.linear = key(lut.name + QStringLiteral("_linear")).isEmpty() || truthy(key(lut.name + QStringLiteral("_linear")));
        lut.wrap = wrapOf(key(lut.name + QStringLiteral("_wrap_mode")));
        lut.mipmap = truthy(key(lut.name + QStringLiteral("_mipmap")));
        if (!QFileInfo::exists(lut.path))
            return fail(QStringLiteral("missing texture %1").arg(key(lut.name)));
        p.luts.push_back(lut);
    }
    // The preset's own values for the parameters.
    for (Parameter& par : p.parameters)
        if (const QString v = key(par.id); !v.isEmpty())
            par.def = std::clamp(v.toDouble(), par.min, par.max);
    *out = std::move(p);
    return true;
}

QString passSource(const QString& glsl, bool vertex)
{
    QString body = glsl;
    // (Its own #version, if any, goes: the context is OpenGL 3.3 core.)
    static const QRegularExpression version(QStringLiteral(R"(^\s*#version[^\n]*\n?)"), QRegularExpression::MultilineOption);
    body.remove(version);
    return QStringLiteral("#version 330 core\n#define %1\n#define PARAMETER_UNIFORM\n#define texture2D texture\n#line 1\n")
               .arg(vertex ? QStringLiteral("VERTEX") : QStringLiteral("FRAGMENT"))
           + body;
}

QString folder()
{
    if (qEnvironmentVariableIsSet("MVP_SCREEN_SHADERS_DIR")) // (tests)
        return qEnvironmentVariable("MVP_SCREEN_SHADERS_DIR");
    return QDir(QStandardPaths::writableLocation(QStandardPaths::AppDataLocation)).filePath(QStringLiteral("screen-shaders/glsl-shaders"));
}

bool installed()
{
    QDirIterator it(folder(), {QStringLiteral("*.glslp")}, QDir::Files, QDirIterator::Subdirectories);
    return it.hasNext();
}

bool installZip(const QString& zipPath, QString* error, const std::function<bool(double)>& progress)
{
    QZipReader zip(zipPath);
    if (!zip.isReadable() || zip.status() != QZipReader::NoError) {
        if (error)
            *error = QStringLiteral("not a zip file");
        return false;
    }
    const QList<QZipReader::FileInfo> files = zip.fileInfoList();
    // GitHub's zips hold everything in one folder ("glsl-shaders-master/"): what is inside it goes in.
    // (Qt gives a folder's entry without its slash: "glsl-shaders-master", then "glsl-shaders-master/Makefile"...)
    QString root;
    if (!files.isEmpty()) {
        const QString top = files.front().filePath.section(QLatin1Char('/'), 0, 0);
        root = top + QLatin1Char('/');
        for (const auto& f : files)
            if (f.filePath != top && !f.filePath.startsWith(root))
                root.clear();
    }
    const QString target = folder(), fresh = target + QStringLiteral(".new");
    QDir(fresh).removeRecursively();
    int done = 0;
    for (const auto& f : files) {
        if (progress && !progress(double(done++) / std::max<qsizetype>(1, files.size()))) {
            QDir(fresh).removeRecursively();
            return false;
        }
        const QString rel = f.filePath.mid(root.size());
        if (rel.isEmpty() || rel.contains(QLatin1String("..")))
            continue;
        const QString dst = QDir(fresh).filePath(rel);
        if (f.isDir) {
            QDir().mkpath(dst);
            continue;
        }
        if (!f.isFile)
            continue;
        QDir().mkpath(QFileInfo(dst).absolutePath());
        QFile out(dst);
        if (!out.open(QIODevice::WriteOnly) || out.write(zip.fileData(f.filePath)) < 0) {
            if (error)
                *error = QStringLiteral("couldn't write %1").arg(dst);
            QDir(fresh).removeRecursively();
            return false;
        }
    }
    QDir(target).removeRecursively();
    if (!QDir().rename(fresh, target)) {
        if (error)
            *error = QStringLiteral("couldn't move the shaders into %1").arg(target);
        return false;
    }
    reload();
    return true;
}

namespace {
struct Generation {
    std::vector<EffectDef> defs;
    std::map<QString, Preset> presets;
    std::map<QString, size_t> index; // id -> place in defs
};
struct Registry {
    std::mutex mutex;
    bool loaded = false;
    std::deque<Generation> generations; // (old ones kept: renderers may still point at them)
};
Registry& registry()
{
    static Registry r;
    return r;
}

void loadLocked(Registry& r)
{
    if (r.loaded)
        return;
    r.loaded = true;
    r.generations.emplace_back();
    Generation& g = r.generations.back();
    std::vector<EffectDef>& defs = g.defs;
    // The presets' folder (or GitHub's folder alone in it, "glsl-shaders-master": a download unpacked a folder too deep).
    QDir base(folder());
    if (const QStringList entries = base.entryList(QDir::AllEntries | QDir::NoDotAndDotDot);
        entries.size() == 1 && entries.front().startsWith(QLatin1String("glsl-shaders")) && QFileInfo(base.filePath(entries.front())).isDir())
        base.cd(entries.front());
    std::vector<QString> paths;
    for (QDirIterator it(base.absolutePath(), {QStringLiteral("*.glslp")}, QDir::Files, QDirIterator::Subdirectories); it.hasNext();)
        paths.push_back(it.next());
    std::sort(paths.begin(), paths.end());
    for (const QString& path : paths) {
        Preset p;
        if (!loadPreset(path, &p))
            continue;
        const QString rel = base.relativeFilePath(path);
        const QString id = QStringLiteral("retro.") + rel.left(rel.size() - 6); // (less ".glslp")
        EffectDef d;
        d.id = id;
        d.name = QFileInfo(path).completeBaseName();
        const QString sub = QFileInfo(rel).path();
        d.category = sub == QLatin1String(".") ? QStringLiteral("Retro Shaders") : QStringLiteral("Retro Shaders/") + sub;
        d.description = QStringLiteral("A libretro screen shader (%1, %2 pass%3), as RetroArch runs it. GPU renderer only: the CPU renderer "
                                       "shows the picture unchanged. Its licence is in the shader's file.")
                            .arg(rel)
                            .arg(p.passes.size())
                            .arg(p.passes.size() == 1 ? QString() : QStringLiteral("es"));
        ParamDef lines;
        lines.id = QString::fromLatin1(kSourceLines);
        lines.name = QStringLiteral("Picture fed in");
        lines.type = ParamType::Choice;
        lines.choices = {QStringLiteral("As it is"), QStringLiteral("1080 lines"), QStringLiteral("720 lines"), QStringLiteral("480 lines"),
                         QStringLiteral("360 lines"), QStringLiteral("288 lines"), QStringLiteral("240 lines"), QStringLiteral("224 lines"),
                         QStringLiteral("160 lines (Game Boy Advance)"), QStringLiteral("144 lines (Game Boy)")};
        lines.def = 6;
        lines.tooltip = QStringLiteral("Screen shaders draw a scanline, a pixel, a phosphor per line of the picture they get, as from an old console "
                                       "(240 lines). As it is: the project's own lines (scanlines too fine to see on HD).");
        d.params.push_back(lines);
        for (const Parameter& par : p.parameters) {
            ParamDef pd;
            pd.id = par.id;
            pd.name = par.name.trimmed().isEmpty() ? par.id : par.name.trimmed();
            pd.min = par.min;
            pd.max = par.max;
            pd.def = par.def;
            pd.decimals = par.step >= 1 ? 0 : std::clamp(int(std::ceil(-std::log10(std::max(1e-6, par.step)) - 1e-9)), 0, 4);
            pd.tooltip = par.id;
            d.params.push_back(pd);
        }
        d.apply = [](QImage&, const Params&, const RenderContext&, std::unique_ptr<EffectState>&) {}; // (the GPU renderer's only)
        g.index[id] = defs.size();
        defs.push_back(std::move(d));
        g.presets.emplace(id, std::move(p));
    }
}
} // namespace

std::vector<const EffectDef*> effects()
{
    Registry& r = registry();
    std::lock_guard lock(r.mutex);
    loadLocked(r);
    std::vector<const EffectDef*> out;
    for (const EffectDef& d : r.generations.back().defs)
        out.push_back(&d);
    return out;
}

const EffectDef* findEffect(const QString& id)
{
    Registry& r = registry();
    std::lock_guard lock(r.mutex);
    loadLocked(r);
    const Generation& g = r.generations.back();
    const auto it = g.index.find(id);
    return it == g.index.end() ? nullptr : &g.defs[it->second];
}

const Preset* preset(const QString& id)
{
    Registry& r = registry();
    std::lock_guard lock(r.mutex);
    loadLocked(r);
    const Generation& g = r.generations.back();
    const auto it = g.presets.find(id);
    return it == g.presets.end() ? nullptr : &it->second;
}

void reload()
{
    Registry& r = registry();
    std::lock_guard lock(r.mutex);
    r.loaded = false;
    loadLocked(r);
}

} // namespace retro
