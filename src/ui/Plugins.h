#pragma once
// Script plugins (Tools > Plugins): a folder with a plugin.json and JavaScript files. Each command in it gets a menu
// entry and a form made from its fields; its script runs inside the program (QJSEngine: no file or network access of
// its own) and changes the project through the same commands AI Control has (mvp.call). See docs/plugins.md.
#include <QJsonArray>
#include <QJsonObject>
#include <QString>
#include <QStringList>
#include <vector>

class McpServer;
class QWidget;

namespace plugins {

struct Field {
    QString id, label, type; // type: file, choice, number, bool or text
    QJsonValue value;        // the default
    QStringList choices, choiceLabels;
    QString filter;          // file: the open dialog's filter
    double min = -1e9, max = 1e9;
};

struct Command {
    QString label, script, description;
    std::vector<Field> fields;
};

struct Plugin {
    QString id, name, version, author, description, folder;
    bool bundled = false; // shipped with the program (trusted); else the user's own, run only once they say so
    std::vector<Command> commands;
};

QString userFolder();    // the profile's plugins (created when asked for)
QString bundledFolder(); // the ones that come with the program
// Every plugin in both folders (the user's first, so one can replace a bundled plugin of the same id).
std::vector<Plugin> load(QStringList* problems = nullptr);
// Reads one plugin folder; false (and why) when its plugin.json is missing or wrong.
bool read(const QString& folder, Plugin* out, QString* error);
// The plugin's files, hashed: a changed plugin is asked about again before it runs.
QString fingerprint(const Plugin& plugin);
// The command's form (fields remembered from the last time). False: cancelled. `files`: the files chosen in it,
// the only ones the script may read.
bool ask(QWidget* parent, const Plugin& plugin, int command, QJsonObject* args, QStringList* files);
// Runs a command's script with these arguments. Empty on success, otherwise what went wrong
// ("main.js:12: TypeError: ..."). `log`: what the script wrote with mvp.log.
QString run(McpServer& server, const Plugin& plugin, int command, const QJsonObject& args, const QStringList& files,
            QStringList* log = nullptr);

} // namespace plugins
