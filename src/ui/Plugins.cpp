#include "ui/Plugins.h"

#include "core/Brand.h"
#include "core/Vst3Host.h"
#include "ui/McpServer.h"

#include <QCheckBox>
#include <QColorDialog>
#include <QComboBox>
#include <QCoreApplication>
#include <QCryptographicHash>
#include <QDialog>
#include <QDialogButtonBox>
#include <QDir>
#include <QDirIterator>
#include <QDoubleSpinBox>
#include <QFile>
#include <QFileDialog>
#include <QFileInfo>
#include <QFormLayout>
#include <QHBoxLayout>
#include <QJSEngine>
#include <QJsonDocument>
#include <QLabel>
#include <QLineEdit>
#include <QMessageBox>
#include <QPushButton>
#include <QRegularExpression>
#include <QSaveFile>
#include <QSettings>
#include <QVersionNumber>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <map>
#include <mutex>
#include <thread>

namespace plugins {

namespace {

constexpr qint64 kMostScriptBytes = 1024 * 1024;     // a script, plugin.json
constexpr qint64 kMostReadBytes = 16 * 1024 * 1024;  // mvp.readText
constexpr int kScriptSeconds = 20;                   // the script's own time (the commands it calls don't count)

QByteArray readSmall(const QString& path, qint64 most, QString* error)
{
    QFile f(path);
    if (!f.open(QIODevice::ReadOnly)) {
        *error = f.errorString();
        return {};
    }
    if (f.size() > most) {
        *error = QStringLiteral("too big (over %1 KB)").arg(most / 1024);
        return {};
    }
    return f.readAll();
}

// The script's own file, inside its plugin's folder.
QString scriptPath(const Plugin& p, const Command& c)
{
    const QString path = QFileInfo(QDir(p.folder).filePath(c.script)).canonicalFilePath();
    const QString root = QFileInfo(p.folder).canonicalFilePath();
    return !path.isEmpty() && path.startsWith(root + QLatin1Char('/')) ? path : QString();
}

#ifdef Q_OS_WIN
constexpr Qt::CaseSensitivity kPathCase = Qt::CaseInsensitive;
#else
constexpr Qt::CaseSensitivity kPathCase = Qt::CaseSensitive;
#endif

// \\server\share, //server or \\?\UNC\...: even looking to see whether one is there connects to another computer
// (and hands it the Windows sign-in), so these are compared as text only.
bool isNetworkPath(const QString& p)
{
    return p.size() > 1 && (p[0] == u'\\' || p[0] == u'/') && (p[1] == u'\\' || p[1] == u'/');
}

// A path as the sandbox compares it: the real file (links and ".." resolved; empty when it isn't there), or, for a
// network path, its text tidied.
QString realPath(const QString& p)
{
    const QString path = QDir::fromNativeSeparators(p);
    return isNetworkPath(p) ? QDir::cleanPath(path) : QFileInfo(path).canonicalFilePath();
}

// Where a file may be written (it needn't be there yet).
QString writePath(const QString& p)
{
    const QString path = QDir::fromNativeSeparators(p);
    return QDir::cleanPath(isNetworkPath(p) ? path : QFileInfo(path).absoluteFilePath());
}

bool samePath(const QString& a, const QString& b) { return !a.isEmpty() && a.compare(b, kPathCase) == 0; }

bool inside(const QString& path, const QString& folder)
{
    if (folder.isEmpty())
        return false;
    return path.startsWith(folder.endsWith(u'/') ? folder : folder + u'/', kPathCase);
}

// JSON as plain JavaScript objects and arrays (JSON.parse): every array method works on them, and the same object
// read twice is the same object (QJSEngine's own conversion of a QVariant gives wrappers that are neither).
QJSValue fromJson(QJSEngine& engine, const QJsonValue& value)
{
    const QByteArray text = value.isObject() ? QJsonDocument(value.toObject()).toJson(QJsonDocument::Compact)
                            : value.isArray() ? QJsonDocument(value.toArray()).toJson(QJsonDocument::Compact)
                                              : QJsonDocument(QJsonArray{value}).toJson(QJsonDocument::Compact);
    QJSValue parsed = engine.globalObject().property(QStringLiteral("JSON")).property(QStringLiteral("parse")).call({QString::fromUtf8(text)});
    return value.isObject() || value.isArray() ? parsed : parsed.property(0);
}

// What a script sees as `mvp` (its methods; the script never gets this object itself).
class Api : public QObject {
    Q_OBJECT
public:
    Api(QJSEngine& engine, McpServer& server, const Access& access, const QString& pluginFolder, std::function<void()> rollback,
        QStringList* log, std::function<void(bool)> scriptTime)
        : m_engine(engine), m_server(server), m_rollback(std::move(rollback)), m_log(log), m_scriptTime(std::move(scriptTime))
    {
        for (const QString& f : access.read)
            m_files << realPath(f);
        for (const QString& f : access.folders)
            m_folders << realPath(f);
        for (const QString& f : access.write)
            m_writes << writePath(f);
        m_own << realPath(QCoreApplication::applicationDirPath()) << realPath(pluginFolder);
        for (const QString& d : vst3Folders()) // (the installed VST3 plugins, for a_vst3: no other DLL is ever loaded)
            m_own << realPath(d);
    }

    // One of the program's commands (the AI Control tools), its answer as an object. Throws when it fails, or when
    // the command would go beyond what the form allows.
    Q_INVOKABLE QJSValue call(const QString& tool, const QJSValue& args = QJSValue())
    {
        const QJsonObject a = QJsonObject::fromVariantMap(args.toVariant().toMap());
        if (const QString no = refusal(tool, a); !no.isEmpty()) {
            m_engine.throwError(QJSValue::GenericError, no);
            return {};
        }
        QString error;
        const QJsonValue result = command(tool, a, &error);
        if (!error.isEmpty()) {
            m_engine.throwError(QJSValue::GenericError, QStringLiteral("%1: %2").arg(tool, error));
            return {};
        }
        return result.isString() ? QJSValue(result.toString()) : fromJson(m_engine, result);
    }

    // A text file the user chose in the form, or one in a folder chosen there (no others).
    Q_INVOKABLE QJSValue readText(const QString& path)
    {
        const QString real = realPath(path);
        if (isNetworkPath(path) || !chosen(real)) {
            m_engine.throwError(QJSValue::GenericError,
                                QStringLiteral("readText: only a file chosen in the form, or in a folder chosen there, can be read (%1)").arg(path));
            return {};
        }
        QString error;
        const QByteArray bytes = readSmall(real, kMostReadBytes, &error);
        if (!error.isEmpty()) {
            m_engine.throwError(QJSValue::GenericError, QStringLiteral("readText: %1: %2").arg(path, error));
            return {};
        }
        return QJSValue(QString::fromUtf8(bytes));
    }

    // The files in a folder chosen in the form (full paths, sorted by name), those matching the patterns
    // (["*.jpg", "*.png"]; none: all of them).
    Q_INVOKABLE QJSValue listFolder(const QString& path, const QStringList& patterns = {})
    {
        const QString real = realPath(path);
        if (isNetworkPath(path) || !std::any_of(m_folders.begin(), m_folders.end(), [&](const QString& d) { return samePath(real, d); })) {
            m_engine.throwError(QJSValue::GenericError, QStringLiteral("listFolder: only a folder chosen in the form can be listed (%1)").arg(path));
            return {};
        }
        QJsonArray out;
        for (const QFileInfo& f : QDir(real).entryInfoList(patterns, QDir::Files, QDir::Name | QDir::IgnoreCase))
            out.append(f.absoluteFilePath());
        return fromJson(m_engine, out);
    }

    // Writes a text file (UTF-8): only the one a save field of the form named.
    Q_INVOKABLE void writeText(const QString& path, const QString& text)
    {
        if (!mayWrite(path)) {
            m_engine.throwError(QJSValue::GenericError, QStringLiteral("writeText: only the file a save field names can be written (%1)").arg(path));
            return;
        }
        const QByteArray bytes = text.toUtf8();
        QSaveFile f(writePath(path));
        if (bytes.size() > kMostReadBytes || !f.open(QIODevice::WriteOnly) || f.write(bytes) != bytes.size() || !f.commit())
            m_engine.throwError(QJSValue::GenericError,
                                QStringLiteral("writeText: %1: %2").arg(path, bytes.size() > kMostReadBytes ? QStringLiteral("over 16 MB") : f.errorString()));
    }

    // Takes back everything this run has changed so far (e.g. after queueing a render of a changed copy).
    Q_INVOKABLE void rollback()
    {
        if (m_rollback)
            m_rollback();
    }

    // A line for the status bar (and the log shown if the script fails).
    Q_INVOKABLE void log(const QString& text)
    {
        if (m_log)
            *m_log << text;
    }

    // The commands mvp.call takes: [{name, description, inputSchema}].
    Q_INVOKABLE QJSValue tools() { return fromJson(m_engine, McpServer::toolList()); }

private:
    // A command's answer as JSON (or its text), or the error.
    QJsonValue command(const QString& tool, const QJsonObject& args, QString* error)
    {
        const QJsonObject message{{QStringLiteral("jsonrpc"), QStringLiteral("2.0")},
                                  {QStringLiteral("id"), 1},
                                  {QStringLiteral("method"), QStringLiteral("tools/call")},
                                  {QStringLiteral("params"), QJsonObject{{QStringLiteral("name"), tool}, {QStringLiteral("arguments"), args}}}};
        m_scriptTime(false);
        const QJsonObject response = m_server.handle(message);
        m_scriptTime(true);
        if (response.contains(QStringLiteral("error"))) {
            *error = response[QStringLiteral("error")][QStringLiteral("message")].toString();
            return {};
        }
        const QJsonObject result = response[QStringLiteral("result")].toObject();
        QString text;
        for (const QJsonValue c : result[QStringLiteral("content")].toArray())
            if (c[QStringLiteral("type")].toString() == QLatin1String("text"))
                text += c[QStringLiteral("text")].toString();
        if (result[QStringLiteral("isError")].toBool()) {
            *error = text;
            return {};
        }
        if (result.contains(QStringLiteral("structuredContent")))
            return result[QStringLiteral("structuredContent")];
        QJsonParseError err;
        const QJsonDocument doc = QJsonDocument::fromJson(text.toUtf8(), &err);
        if (err.error == QJsonParseError::NoError)
            return doc.isArray() ? QJsonValue(doc.array()) : QJsonValue(doc.object());
        return text;
    }

    // Why a command may not run with these arguments (empty: it may). A plugin is someone else's code: it works on
    // the open project, opens only what the form chose (or the program's and its own files, or media already in the
    // project), writes only a save field's file, and doesn't go online or turn on a camera or microphone.
    QString refusal(const QString& tool, const QJsonObject& a)
    {
        static const QStringList refused{
            QStringLiteral("new_project"), QStringLiteral("open_project"), QStringLiteral("undo"), QStringLiteral("redo"), // (the run is one undo step of the open project)
            QStringLiteral("youtube_clip"), QStringLiteral("ai_components"), // (they go online)
            QStringLiteral("collect_project"),                               // (copies all the media to any folder)
            QStringLiteral("set_live_source")};                              // (cameras, microphones, screens)
        const QString action = a[QStringLiteral("action")].toString();
        if (refused.contains(tool))
            return QStringLiteral("%1 isn't available to plugins").arg(tool);
        if (tool == QLatin1String("live_action")
            && QStringList{QStringLiteral("start"), QStringLiteral("record"), QStringLiteral("save_replay")}.contains(action))
            return QStringLiteral("live_action %1 isn't available to plugins (it turns on the camera, the microphone or a recording)").arg(action);
        if (tool == QLatin1String("live_switcher") && a.contains(QStringLiteral("inputs")))
            return QStringLiteral("live_switcher inputs isn't available to plugins (it opens cameras and screens)");
        if (tool == QLatin1String("render_queue") && a.contains(QStringLiteral("project_file")))
            return QStringLiteral("render_queue project_file isn't available to plugins (only the open project)");
        if (tool == QLatin1String("deliver") && a.contains(QStringLiteral("folder")))
            return QStringLiteral("deliver folder isn't available to plugins (the outputs go where the user set)");
        if (tool == QLatin1String("presets") && action == QLatin1String("remove"))
            return QStringLiteral("presets remove isn't available to plugins (undo can't bring a preset back)");
        // The one argument a command writes to.
        QString writes;
        if (QStringList{QStringLiteral("render"), QStringLiteral("render_queue"), QStringLiteral("save_project"), QStringLiteral("youtube_publish")}.contains(tool)
            || (tool == QLatin1String("captions") && action == QLatin1String("export"))
            || (tool == QLatin1String("graphics_overlay") && action == QLatin1String("export_template")))
            writes = QStringLiteral("path");
        else if (tool == QLatin1String("get_frame"))
            writes = QStringLiteral("save_to");
        const bool writesAFile = !writes.isEmpty() && a.contains(writes);
        if (writesAFile && !mayWrite(a[writes].toString()))
            return QStringLiteral("%1: only the file a save field names can be written (%2)").arg(tool, a[writes].toString());
        if (a[QStringLiteral("overwrite")].toBool() && !writesAFile)
            return QStringLiteral("%1: overwrite isn't available to plugins here (only the file a save field names can be replaced)").arg(tool);
        for (auto it = a.begin(); it != a.end(); ++it)
            if (it.key() != writes || !writesAFile)
                if (QString why = opens(tool, it.key(), *it); !why.isEmpty())
                    return why;
        return {};
    }

    // Every file path in an argument (at any depth: effect settings, scene layers) must be one the plugin may open.
    QString opens(const QString& tool, const QString& key, const QJsonValue& v)
    {
        if (v.isArray()) {
            for (const QJsonValue x : v.toArray())
                if (QString why = opens(tool, key, x); !why.isEmpty())
                    return why;
        } else if (v.isObject()) {
            const QJsonObject o = v.toObject();
            for (auto it = o.begin(); it != o.end(); ++it)
                if (QString why = opens(tool, it.key(), *it); !why.isEmpty())
                    return why;
        } else if (v.isString() && looksLikeAPath(key, v.toString()) && !mayRead(v.toString())) {
            return QStringLiteral("%1: %2 isn't a file this plugin may open (only files and folders chosen in its form, the program's "
                                  "and the plugin's own files, and media already in the project)")
                .arg(tool, v.toString());
        }
        return {};
    }

    static bool looksLikeAPath(const QString& key, const QString& s)
    {
        static const QStringList keys{QStringLiteral("path"),         QStringLiteral("paths"),   QStringLiteral("file"),
                                      QStringLiteral("files"),        QStringLiteral("destination"), QStringLiteral("save_to"),
                                      QStringLiteral("project_file"), QStringLiteral("plugin")};
        if (s.isEmpty())
            return false;
        if (keys.contains(key) || isNetworkPath(s) || s.startsWith(QLatin1String("file:"), Qt::CaseInsensitive))
            return true;
        // (any other words are a path when they name a file on this computer: "C:\...", "/...")
        static const QRegularExpression drive(QStringLiteral("^[A-Za-z]:[\\\\/]"));
        return (drive.match(s).hasMatch() || s.startsWith(u'/')) && QFileInfo::exists(s);
    }

    // Chosen in the form: the file, or inside the folder.
    bool chosen(const QString& real) const
    {
        return std::any_of(m_files.begin(), m_files.end(), [&](const QString& f) { return samePath(real, f); })
               || std::any_of(m_folders.begin(), m_folders.end(), [&](const QString& d) { return inside(real, d); });
    }

    bool mayRead(const QString& path)
    {
        if (path.startsWith(QLatin1String("file:"), Qt::CaseInsensitive))
            return false; // (paths are given as paths)
        const QString real = realPath(path);
        if (real.isEmpty())
            return false;
        if (chosen(real) || std::any_of(m_writes.begin(), m_writes.end(), [&](const QString& w) { return samePath(real, w); })
            || (!isNetworkPath(path) && std::any_of(m_own.begin(), m_own.end(), [&](const QString& d) { return inside(real, d); })))
            return true;
        if (!m_mediaRead) { // (the project's own media, looked up once a run)
            m_mediaRead = true;
            QString error;
            for (const QJsonValue m : command(QStringLiteral("get_project"), {}, &error)[QStringLiteral("media")].toArray())
                m_media << realPath(m[QStringLiteral("path")].toString());
        }
        return std::any_of(m_media.begin(), m_media.end(), [&](const QString& m) { return samePath(real, m); });
    }

    bool mayWrite(const QString& path) const
    {
        const QString w = writePath(path);
        return !path.startsWith(QLatin1String("file:"), Qt::CaseInsensitive)
               && std::any_of(m_writes.begin(), m_writes.end(), [&](const QString& x) { return samePath(w, x); });
    }

    QJSEngine& m_engine;
    McpServer& m_server;
    QStringList m_files, m_folders, m_writes, m_own, m_media;
    bool m_mediaRead = false;
    std::function<void()> m_rollback;
    QStringList* m_log;
    std::function<void(bool)> m_scriptTime;
};

// Stops a script that has run kScriptSeconds of its own time (a loop that never ends): only while it is running, not
// while one of its commands is.
class Watchdog {
public:
    explicit Watchdog(QJSEngine& engine) : m_thread([this, &engine] {
        std::unique_lock lock(m_mutex);
        auto used = std::chrono::steady_clock::duration::zero();
        auto since = std::chrono::steady_clock::now();
        while (!m_stop) {
            m_wake.wait_for(lock, std::chrono::milliseconds(100));
            const auto now = std::chrono::steady_clock::now();
            if (m_running)
                used += now - since;
            since = now;
            if (used > std::chrono::seconds(kScriptSeconds)) {
                engine.setInterrupted(true); // (thread safe)
                return;
            }
        }
    }) {}
    ~Watchdog()
    {
        {
            std::lock_guard lock(m_mutex);
            m_stop = true;
        }
        m_wake.notify_all();
        m_thread.join();
    }
    void running(bool on)
    {
        std::lock_guard lock(m_mutex);
        m_running = on;
    }

private:
    std::mutex m_mutex;
    std::condition_variable m_wake;
    bool m_stop = false, m_running = true;
    std::thread m_thread; // (last: it starts once the rest is ready)
};

QString settingsKey(const Plugin& p, int command) { return QStringLiteral("plugins/values/%1/%2").arg(p.id).arg(command); }

} // namespace

QString userFolder() { return brand::storageLocation(QStandardPaths::AppDataLocation) + QStringLiteral("/plugins"); }

QString bundledFolder() { return QDir(QCoreApplication::applicationDirPath()).filePath(QStringLiteral("plugins")); }

bool read(const QString& folder, Plugin* out, QString* error)
{
    QString why;
    const QByteArray bytes = readSmall(QDir(folder).filePath(QStringLiteral("plugin.json")), kMostScriptBytes, &why);
    if (!why.isEmpty()) {
        *error = QStringLiteral("plugin.json: %1").arg(why);
        return false;
    }
    QJsonParseError err;
    const QJsonObject o = QJsonDocument::fromJson(bytes, &err).object();
    if (err.error != QJsonParseError::NoError) {
        *error = QStringLiteral("plugin.json: %1 (at character %2)").arg(err.errorString()).arg(err.offset);
        return false;
    }
    Plugin p;
    p.folder = QFileInfo(folder).absoluteFilePath();
    p.id = o[QStringLiteral("id")].toString();
    p.name = o[QStringLiteral("name")].toString(p.id);
    p.version = o[QStringLiteral("version")].toString();
    p.author = o[QStringLiteral("author")].toString();
    p.description = o[QStringLiteral("description")].toString();
    static const QRegularExpression idShape(QStringLiteral("^[a-z0-9][a-z0-9._-]{0,63}$"));
    if (!idShape.match(p.id).hasMatch()) {
        *error = QStringLiteral("plugin.json: \"id\" must be lower-case letters, digits, '.', '-' or '_'");
        return false;
    }
    const QString needs = o[QStringLiteral("mvp")].toString();
    if (!needs.isEmpty() && QVersionNumber::fromString(QCoreApplication::applicationVersion()) < QVersionNumber::fromString(needs)) {
        *error = QStringLiteral("needs Mutant Video Producer %1 or later").arg(needs);
        return false;
    }
    static const QStringList types{QStringLiteral("file"),   QStringLiteral("files"),  QStringLiteral("folder"),
                                   QStringLiteral("save"),   QStringLiteral("choice"), QStringLiteral("number"),
                                   QStringLiteral("bool"),   QStringLiteral("text"),   QStringLiteral("color")};
    for (const QJsonValue cv : o[QStringLiteral("commands")].toArray()) {
        Command c;
        c.label = cv[QStringLiteral("label")].toString();
        c.script = cv[QStringLiteral("script")].toString();
        c.description = cv[QStringLiteral("description")].toString();
        if (c.label.isEmpty() || c.script.isEmpty()) {
            *error = QStringLiteral("plugin.json: every command needs a \"label\" and a \"script\"");
            return false;
        }
        if (scriptPath(p, c).isEmpty()) {
            *error = QStringLiteral("%1: not found in the plugin's folder").arg(c.script);
            return false;
        }
        for (const QJsonValue fv : cv[QStringLiteral("fields")].toArray()) {
            Field f;
            f.id = fv[QStringLiteral("id")].toString();
            f.label = fv[QStringLiteral("label")].toString(f.id);
            f.type = fv[QStringLiteral("type")].toString(QStringLiteral("text"));
            f.value = fv[QStringLiteral("default")];
            f.filter = fv[QStringLiteral("filter")].toString();
            f.min = fv[QStringLiteral("min")].toDouble(-1e9);
            f.max = fv[QStringLiteral("max")].toDouble(1e9);
            f.optional = fv[QStringLiteral("optional")].toBool(false);
            f.withFolder = fv[QStringLiteral("withFolder")].toBool(false);
            for (const QJsonValue v : fv[QStringLiteral("choices")].toArray())
                f.choices << v.toString();
            for (const QJsonValue v : fv[QStringLiteral("labels")].toArray())
                f.choiceLabels << v.toString();
            if (f.id.isEmpty() || !types.contains(f.type) || (f.type == QLatin1String("choice") && f.choices.isEmpty())) {
                *error = QStringLiteral("plugin.json: field \"%1\": needs an id, a type (%2) and, for a choice, its choices")
                             .arg(f.id, types.join(QStringLiteral(", ")));
                return false;
            }
            c.fields.push_back(f);
        }
        p.commands.push_back(c);
    }
    if (p.commands.empty()) {
        *error = QStringLiteral("plugin.json: no commands");
        return false;
    }
    *out = p;
    return true;
}

std::vector<Plugin> load(QStringList* problems)
{
    std::vector<Plugin> out;
    std::map<QString, bool> seen;
    for (const auto& [root, bundled] : {std::pair{userFolder(), false}, std::pair{bundledFolder(), true}})
        for (const QFileInfo& dir : QDir(root).entryInfoList(QDir::Dirs | QDir::NoDotAndDotDot, QDir::Name)) {
            if (!QFileInfo::exists(QDir(dir.filePath()).filePath(QStringLiteral("plugin.json"))))
                continue;
            Plugin p;
            QString error;
            if (!read(dir.filePath(), &p, &error)) {
                if (problems)
                    *problems << QStringLiteral("%1: %2").arg(dir.fileName(), error);
                continue;
            }
            if (seen[p.id])
                continue; // (the user's copy wins)
            seen[p.id] = true;
            p.bundled = bundled;
            out.push_back(p);
        }
    return out;
}

QString fingerprint(const Plugin& plugin)
{
    QCryptographicHash hash(QCryptographicHash::Sha256);
    QStringList files;
    QDirIterator it(plugin.folder, QDir::Files, QDirIterator::Subdirectories);
    while (it.hasNext())
        files << it.next();
    files.sort();
    const QDir root(plugin.folder);
    for (const QString& f : files) {
        hash.addData(root.relativeFilePath(f).toUtf8());
        QFile file(f);
        if (file.open(QIODevice::ReadOnly))
            hash.addData(&file);
    }
    return QString::fromLatin1(hash.result().toHex());
}

bool ask(QWidget* parent, const Plugin& plugin, int command, QJsonObject* args, Access* access)
{
    const Command& c = plugin.commands[size_t(command)];
    const QJsonObject last = QJsonDocument::fromJson(QSettings().value(settingsKey(plugin, command)).toByteArray()).object();
    QDialog dlg(parent);
    dlg.setObjectName(QStringLiteral("pluginForm"));
    dlg.setWindowTitle(QString(c.label).remove(QStringLiteral("...")));
    dlg.setMinimumWidth(480);
    auto* form = new QFormLayout(&dlg);
    if (!c.description.isEmpty() || !plugin.description.isEmpty()) {
        auto* about = new QLabel(c.description.isEmpty() ? plugin.description : c.description);
        about->setWordWrap(true);
        form->addRow(about);
    }
    std::vector<std::function<QJsonValue()>> values;
    std::vector<std::function<QString()>> problems;
    for (const Field& f : c.fields) {
        const QJsonValue start = last.contains(f.id) ? last[f.id] : f.value;
        const bool isPath = f.type == QLatin1String("file") || f.type == QLatin1String("files") || f.type == QLatin1String("folder")
                            || f.type == QLatin1String("save");
        if (isPath) {
            QStringList shown;
            if (start.isArray())
                for (const QJsonValue v : start.toArray())
                    shown << QDir::toNativeSeparators(v.toString());
            else if (!start.toString().isEmpty())
                shown << QDir::toNativeSeparators(start.toString());
            auto* row = new QWidget;
            auto* line = new QLineEdit(shown.join(QStringLiteral("; ")));
            if (f.optional)
                line->setPlaceholderText(QObject::tr("(optional)"));
            auto* browse = new QPushButton(QObject::tr("Browse..."));
            auto* h = new QHBoxLayout(row);
            h->setContentsMargins(0, 0, 0, 0);
            h->addWidget(line, 1);
            h->addWidget(browse);
            QObject::connect(browse, &QPushButton::clicked, &dlg, [&dlg, line, f] {
                const QString at = line->text().section(QLatin1Char(';'), 0, 0).trimmed();
                QStringList chosen;
                if (f.type == QLatin1String("file"))
                    chosen << QFileDialog::getOpenFileName(&dlg, f.label, at, f.filter);
                else if (f.type == QLatin1String("files"))
                    chosen = QFileDialog::getOpenFileNames(&dlg, f.label, at, f.filter);
                else if (f.type == QLatin1String("folder"))
                    chosen << QFileDialog::getExistingDirectory(&dlg, f.label, at);
                else
                    chosen << QFileDialog::getSaveFileName(&dlg, f.label, at, f.filter);
                chosen.removeAll(QString());
                if (!chosen.isEmpty()) {
                    for (QString& p : chosen)
                        p = QDir::toNativeSeparators(p);
                    line->setText(chosen.join(QStringLiteral("; ")));
                }
            });
            form->addRow(f.label, row);
            auto paths = [line] {
                QStringList out;
                for (const QString& p : line->text().split(QLatin1Char(';'), Qt::SkipEmptyParts))
                    if (!p.trimmed().isEmpty())
                        out << QDir::fromNativeSeparators(p.trimmed());
                return out;
            };
            if (f.type == QLatin1String("files"))
                values.push_back([paths] { return QJsonValue(QJsonArray::fromStringList(paths())); });
            else
                values.push_back([paths] { return QJsonValue(paths().value(0)); });
            problems.push_back([paths, f] {
                const QStringList p = paths();
                if (p.isEmpty())
                    return f.optional ? QString() : QObject::tr("%1: choose one").arg(f.label);
                for (const QString& one : p) {
                    const QFileInfo fi(one);
                    const bool ok = f.type == QLatin1String("folder") ? fi.isDir()
                                    : f.type == QLatin1String("save")  ? fi.absoluteDir().exists() && !fi.isDir()
                                                                       : fi.isFile();
                    if (!ok)
                        return QObject::tr("%1: %2 isn't there").arg(f.label, QDir::toNativeSeparators(one));
                }
                return QString();
            });
        } else if (f.type == QLatin1String("choice")) {
            auto* box = new QComboBox;
            for (int i = 0; i < f.choices.size(); ++i)
                box->addItem(i < f.choiceLabels.size() ? f.choiceLabels[i] : f.choices[i], f.choices[i]);
            box->setCurrentIndex(std::max(0, int(f.choices.indexOf(start.toString()))));
            form->addRow(f.label, box);
            values.push_back([box] { return QJsonValue(box->currentData().toString()); });
        } else if (f.type == QLatin1String("number")) {
            auto* spin = new QDoubleSpinBox;
            spin->setRange(f.min, f.max);
            spin->setDecimals(2);
            spin->setValue(start.toDouble());
            form->addRow(f.label, spin);
            values.push_back([spin] { return QJsonValue(spin->value()); });
        } else if (f.type == QLatin1String("bool")) {
            auto* box = new QCheckBox(f.label);
            box->setChecked(start.toBool());
            form->addRow(box);
            values.push_back([box] { return QJsonValue(box->isChecked()); });
        } else if (f.type == QLatin1String("color")) {
            auto* button = new QPushButton;
            auto show = [button](const QColor& colour) {
                button->setText(colour.name());
                button->setStyleSheet(QStringLiteral("QPushButton { background: %1; color: %2; }")
                                          .arg(colour.name(), colour.lightnessF() > 0.55 ? QStringLiteral("#000000") : QStringLiteral("#ffffff")));
            };
            const QColor first(start.toString());
            show(first.isValid() ? first : QColor(Qt::white));
            QObject::connect(button, &QPushButton::clicked, &dlg, [&dlg, button, show, label = f.label] {
                const QColor chosen = QColorDialog::getColor(QColor(button->text()), &dlg, label);
                if (chosen.isValid())
                    show(chosen);
            });
            form->addRow(f.label, button);
            values.push_back([button] { return QJsonValue(button->text()); });
        } else {
            auto* line = new QLineEdit(start.toString());
            form->addRow(f.label, line);
            values.push_back([line] { return QJsonValue(line->text()); });
        }
        if (problems.size() < values.size())
            problems.push_back([] { return QString(); });
    }
    auto* buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel);
    form->addRow(buttons);
    QObject::connect(buttons, &QDialogButtonBox::rejected, &dlg, &QDialog::reject);
    QObject::connect(buttons, &QDialogButtonBox::accepted, &dlg, [&] {
        for (const auto& problem : problems)
            if (const QString p = problem(); !p.isEmpty()) {
                QMessageBox::warning(&dlg, dlg.windowTitle(), p);
                return;
            }
        dlg.accept();
    });
    if (dlg.exec() != QDialog::Accepted)
        return false;
    *args = {};
    *access = {};
    for (size_t i = 0; i < c.fields.size(); ++i) {
        const Field& f = c.fields[i];
        QJsonValue v = values[i]();
        // (a save path typed without an extension gets the filter's first, as the file dialog would give it)
        static const QRegularExpression firstExtension(QStringLiteral("\\*\\.(\\w+)"));
        if (const auto m = firstExtension.match(f.filter);
            f.type == QLatin1String("save") && m.hasMatch() && !v.toString().isEmpty() && QFileInfo(v.toString()).suffix().isEmpty())
            v = v.toString() + QLatin1Char('.') + m.captured(1);
        (*args)[f.id] = v;
        if (f.type == QLatin1String("file") && !v.toString().isEmpty())
            access->read << v.toString();
        else if (f.type == QLatin1String("files"))
            for (const QJsonValue p : v.toArray())
                access->read << p.toString();
        else if (f.type == QLatin1String("folder") && !v.toString().isEmpty())
            access->folders << v.toString();
        else if (f.type == QLatin1String("save") && !v.toString().isEmpty())
            access->write << v.toString();
    }
    QSettings().setValue(settingsKey(plugin, command), QJsonDocument(*args).toJson(QJsonDocument::Compact));
    return true;
}

QString run(McpServer& server, const Plugin& plugin, int command, const QJsonObject& args, const Access& access,
            const std::function<void()>& rollback, QStringList* log)
{
    const Command& c = plugin.commands[size_t(command)];
    const QString path = scriptPath(plugin, c);
    QString error;
    const QByteArray code = path.isEmpty() ? QByteArray() : readSmall(path, kMostScriptBytes, &error);
    if (path.isEmpty() || !error.isEmpty())
        return QStringLiteral("%1: %2").arg(c.script, path.isEmpty() ? QStringLiteral("not found") : error);
    // A file field with withFolder lets the script read the files beside the chosen one too.
    Access allowed = access;
    for (const Field& f : c.fields)
        if (f.withFolder)
            for (const QJsonValue v : args[f.id].isArray() ? args[f.id].toArray() : QJsonArray{args[f.id]})
                if (!v.toString().isEmpty())
                    allowed.folders << QFileInfo(v.toString()).absolutePath();
    QJSEngine engine;
    engine.installExtensions(QJSEngine::ConsoleExtension);
    Watchdog watchdog(engine);
    Api api(engine, server, allowed, plugin.folder, rollback, log, [&watchdog](bool on) { watchdog.running(on); });
    QJSEngine::setObjectOwnership(&api, QJSEngine::CppOwnership);
    // `mvp` is a plain object holding the methods (bound to the Api), not the QObject itself: a script can't reach
    // QObject's own deleteLater, destroyed or objectName.
    const QJSValue wrapped = engine.newQObject(&api);
    QJSValue mvp = engine.newObject();
    for (const char* name : {"call", "readText", "listFolder", "writeText", "rollback", "log", "tools"}) {
        const QJSValue method = wrapped.property(QLatin1String(name));
        mvp.setProperty(QLatin1String(name), method.property(QStringLiteral("bind")).callWithInstance(method, {wrapped}));
    }
    mvp.setProperty(QStringLiteral("args"), fromJson(engine, args));
    mvp.setProperty(QStringLiteral("plugin"), fromJson(engine, QJsonObject{{QStringLiteral("id"), plugin.id},
                                                                           {QStringLiteral("version"), plugin.version},
                                                                           {QStringLiteral("folder"), plugin.folder}}));
    engine.globalObject().setProperty(QStringLiteral("mvp"), mvp);
    const QJSValue result = engine.evaluate(QString::fromUtf8(code), QFileInfo(path).fileName(), 1);
    if (engine.isInterrupted())
        return QStringLiteral("%1: stopped after %2 seconds of running (a loop that never ends?)").arg(c.script).arg(kScriptSeconds);
    if (result.isError())
        return QStringLiteral("%1:%2: %3")
            .arg(QFileInfo(path).fileName())
            .arg(result.property(QStringLiteral("lineNumber")).toInt())
            .arg(result.toString());
    return {};
}

} // namespace plugins

#include "Plugins.moc"
