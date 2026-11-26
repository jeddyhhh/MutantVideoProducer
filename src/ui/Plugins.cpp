#include "ui/Plugins.h"

#include "core/Brand.h"
#include "ui/McpServer.h"

#include <QCheckBox>
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
#include <QSettings>
#include <QVersionNumber>

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

// What a script sees as `mvp`.
class Api : public QObject {
    Q_OBJECT
public:
    Api(QJSEngine& engine, McpServer& server, const QStringList& files, QStringList* log, std::function<void(bool)> scriptTime)
        : m_engine(engine), m_server(server), m_log(log), m_scriptTime(std::move(scriptTime))
    {
        for (const QString& f : files)
            m_files << QFileInfo(f).canonicalFilePath();
    }

    // One of the program's commands (the AI Control tools), its answer as an object. Throws when it fails.
    Q_INVOKABLE QJSValue call(const QString& tool, const QJSValue& args = QJSValue())
    {
        // (The script works on the open project, in one undo step: these would take it away from under it.)
        static const QStringList refused{QStringLiteral("new_project"), QStringLiteral("open_project"), QStringLiteral("undo"),
                                         QStringLiteral("redo")};
        if (refused.contains(tool)) {
            m_engine.throwError(QJSValue::GenericError, QStringLiteral("%1 isn't available to plugins").arg(tool));
            return {};
        }
        const QJsonObject message{{QStringLiteral("jsonrpc"), QStringLiteral("2.0")},
                                  {QStringLiteral("id"), 1},
                                  {QStringLiteral("method"), QStringLiteral("tools/call")},
                                  {QStringLiteral("params"),
                                   QJsonObject{{QStringLiteral("name"), tool},
                                               {QStringLiteral("arguments"), QJsonObject::fromVariantMap(args.toVariant().toMap())}}}};
        m_scriptTime(false);
        const QJsonObject response = m_server.handle(message);
        m_scriptTime(true);
        if (response.contains(QStringLiteral("error"))) {
            m_engine.throwError(QJSValue::GenericError,
                                QStringLiteral("%1: %2").arg(tool, response[QStringLiteral("error")][QStringLiteral("message")].toString()));
            return {};
        }
        const QJsonObject result = response[QStringLiteral("result")].toObject();
        QString text;
        for (const QJsonValue c : result[QStringLiteral("content")].toArray())
            if (c[QStringLiteral("type")].toString() == QLatin1String("text"))
                text += c[QStringLiteral("text")].toString();
        if (result[QStringLiteral("isError")].toBool()) {
            m_engine.throwError(QJSValue::GenericError, QStringLiteral("%1: %2").arg(tool, text));
            return {};
        }
        if (result.contains(QStringLiteral("structuredContent")))
            return m_engine.toScriptValue(result[QStringLiteral("structuredContent")].toVariant());
        QJsonParseError err;
        const QJsonDocument doc = QJsonDocument::fromJson(text.toUtf8(), &err);
        if (err.error == QJsonParseError::NoError)
            return m_engine.toScriptValue(doc.isArray() ? QVariant(doc.array().toVariantList()) : QVariant(doc.object().toVariantMap()));
        return QJSValue(text);
    }

    // A text file the user chose in the command's form (no others).
    Q_INVOKABLE QJSValue readText(const QString& path)
    {
        const QString canonical = QFileInfo(path).canonicalFilePath();
        if (canonical.isEmpty() || !m_files.contains(canonical)) {
            m_engine.throwError(QJSValue::GenericError, QStringLiteral("readText: only a file chosen in the form can be read (%1)").arg(path));
            return {};
        }
        QString error;
        const QByteArray bytes = readSmall(canonical, kMostReadBytes, &error);
        if (!error.isEmpty()) {
            m_engine.throwError(QJSValue::GenericError, QStringLiteral("readText: %1: %2").arg(path, error));
            return {};
        }
        return QJSValue(QString::fromUtf8(bytes));
    }

    // A line for the status bar (and the log shown if the script fails).
    Q_INVOKABLE void log(const QString& text)
    {
        if (m_log)
            *m_log << text;
    }

    // The commands mvp.call takes: [{name, description, inputSchema}].
    Q_INVOKABLE QJSValue tools() { return m_engine.toScriptValue(McpServer::toolList().toVariantList()); }

private:
    QJSEngine& m_engine;
    McpServer& m_server;
    QStringList m_files;
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
    static const QStringList types{QStringLiteral("file"), QStringLiteral("choice"), QStringLiteral("number"), QStringLiteral("bool"),
                                   QStringLiteral("text")};
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

bool ask(QWidget* parent, const Plugin& plugin, int command, QJsonObject* args, QStringList* files)
{
    const Command& c = plugin.commands[size_t(command)];
    const QJsonObject last = QJsonDocument::fromJson(QSettings().value(settingsKey(plugin, command)).toByteArray()).object();
    QDialog dlg(parent);
    dlg.setObjectName(QStringLiteral("pluginForm"));
    dlg.setWindowTitle(QString(c.label).remove(QStringLiteral("...")));
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
        if (f.type == QLatin1String("file")) {
            auto* row = new QWidget;
            auto* line = new QLineEdit(start.toString());
            auto* browse = new QPushButton(QObject::tr("Browse..."));
            auto* h = new QHBoxLayout(row);
            h->setContentsMargins(0, 0, 0, 0);
            h->addWidget(line, 1);
            h->addWidget(browse);
            QObject::connect(browse, &QPushButton::clicked, &dlg, [&dlg, line, filter = f.filter, title = f.label] {
                const QString path = QFileDialog::getOpenFileName(&dlg, title, line->text(), filter);
                if (!path.isEmpty())
                    line->setText(QDir::toNativeSeparators(path));
            });
            form->addRow(f.label, row);
            values.push_back([line] { return QJsonValue(QDir::fromNativeSeparators(line->text().trimmed())); });
            problems.push_back([line, label = f.label] {
                return QFileInfo(line->text().trimmed()).isFile() ? QString() : QObject::tr("%1: choose a file").arg(label);
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
    files->clear();
    for (size_t i = 0; i < c.fields.size(); ++i) {
        (*args)[c.fields[i].id] = values[i]();
        if (c.fields[i].type == QLatin1String("file"))
            *files << (*args)[c.fields[i].id].toString();
    }
    QSettings().setValue(settingsKey(plugin, command), QJsonDocument(*args).toJson(QJsonDocument::Compact));
    return true;
}

QString run(McpServer& server, const Plugin& plugin, int command, const QJsonObject& args, const QStringList& files, QStringList* log)
{
    const Command& c = plugin.commands[size_t(command)];
    const QString path = scriptPath(plugin, c);
    QString error;
    const QByteArray code = path.isEmpty() ? QByteArray() : readSmall(path, kMostScriptBytes, &error);
    if (path.isEmpty() || !error.isEmpty())
        return QStringLiteral("%1: %2").arg(c.script, path.isEmpty() ? QStringLiteral("not found") : error);
    QJSEngine engine;
    engine.installExtensions(QJSEngine::ConsoleExtension);
    Watchdog watchdog(engine);
    Api api(engine, server, files, log, [&watchdog](bool on) { watchdog.running(on); });
    QJSEngine::setObjectOwnership(&api, QJSEngine::CppOwnership);
    QJSValue mvp = engine.newQObject(&api);
    mvp.setProperty(QStringLiteral("args"), engine.toScriptValue(args.toVariantMap()));
    mvp.setProperty(QStringLiteral("plugin"), engine.toScriptValue(QVariantMap{{QStringLiteral("id"), plugin.id},
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
