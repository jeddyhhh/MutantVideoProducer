#include "mcp-fuzz.h"
#include "core/Project.h"
#include "ui/McpServer.h"

#include <QApplication>
#include <QDir>
#include <QElapsedTimer>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QMessageBox>
#include <QRegularExpression>
#include <QTimer>
#include <algorithm>
#include <climits>
#include <cstdio>
#include <map>
#include <functional>
#include <random>
#include <set>

namespace {
// Tools left out: they write where they are told, render, download, run engines for minutes or use cameras and sound
// cards (their arguments are covered by the editor test's own calls).
const std::set<QString> kSkipped = {
    QStringLiteral("render"),         QStringLiteral("render_queue"),     QStringLiteral("collect_project"),
    QStringLiteral("ai_components"),  QStringLiteral("generate_voice"),   QStringLiteral("generate_dialogue"),
    QStringLiteral("youtube_clip"),   QStringLiteral("youtube_publish"),  QStringLiteral("new_project"),
    QStringLiteral("open_project"),   QStringLiteral("save_project"),     QStringLiteral("captions"),
    QStringLiteral("separate_stems"), QStringLiteral("stabilize"),        QStringLiteral("track_motion"),
    QStringLiteral("auto_reframe"),   QStringLiteral("deliver"),
    QStringLiteral("node_effect"),    QStringLiteral("presets"),          QStringLiteral("live_action"),
    QStringLiteral("set_live_source"), QStringLiteral("live_switcher")};
} // namespace

int runMcpFuzz(McpServer* server, Project* project, QStringList& log, int rounds)
{
    int failures = 0;
    auto check = [&](bool ok, const QString& what) {
        log << QStringLiteral("%1  %2").arg(ok ? QStringLiteral("PASS") : QStringLiteral("FAIL"), what);
        failures += !ok;
    };
    const uint32_t seed = qEnvironmentVariableIntValue("MVP_MCP_FUZZ_SEED");
    std::mt19937 rng(seed ? seed : 4242);
    const QString scratch = QDir::temp().filePath(QStringLiteral("mvp-mcp-fuzz"));
    QDir().mkpath(scratch);

    std::vector<QJsonObject> tools;
    for (const QJsonValue v : McpServer::toolList())
        if (!kSkipped.count(v.toObject()[QStringLiteral("name")].toString()))
            tools.push_back(v.toObject());

    // Ids that are there now: events, tracks (indexes), effects, markers, media.
    auto known = [&] {
        std::vector<int> ids;
        const Timeline& tl = project->timeline();
        for (size_t ti = 0; ti < tl.tracks.size(); ++ti) {
            ids.push_back(int(ti));
            for (const Clip& c : tl.tracks[ti].clips) {
                ids.push_back(c.id);
                for (const EffectInstance& e : c.effects)
                    ids.push_back(e.id);
            }
        }
        for (const Marker& m : tl.markers)
            ids.push_back(m.id);
        for (const auto& [id, m] : project->media())
            ids.push_back(id);
        return ids;
    };
    std::function<QJsonValue(const QString&, const QJsonObject&, int)> value = [&](const QString& name, const QJsonObject& schema,
                                                                                  int depth) -> QJsonValue {
        const QString type = schema[QStringLiteral("type")].toString();
        if (schema.contains(QStringLiteral("enum"))) {
            const QJsonArray choices = schema[QStringLiteral("enum")].toArray();
            return rng() % 10 == 0 || choices.isEmpty() ? QJsonValue(QStringLiteral("bogus")) : choices[qsizetype(rng() % choices.size())];
        }
        const QString n = name.toLower();
        if (n == QLatin1String("action") && type == QLatin1String("string") && rng() % 10) { // (its words, from the description)
            static const QRegularExpression word(QStringLiteral("[a-z][a-z_]{2,}"));
            QStringList words;
            for (auto m = word.globalMatch(schema[QStringLiteral("description")].toString()); m.hasNext();)
                words << m.next().captured();
            if (!words.isEmpty())
                return words[qsizetype(rng() % words.size())];
        }
        if (type == QLatin1String("string") && (n.contains(QLatin1String("path")) || n.contains(QLatin1String("file"))
                                                || n.contains(QLatin1String("folder")) || n == QLatin1String("save_to")))
            return QDir(scratch).filePath(QStringLiteral("out-%1.%2").arg(rng() % 1000).arg(rng() % 2 ? "png" : "json"));
        if (type == QLatin1String("integer")) {
            const auto ids = known();
            switch (rng() % 8) {
            case 0: return 0;
            case 1: return -1;
            case 2: return int(rng() % 5);
            case 3: return INT_MAX;
            case 4: return INT_MIN;
            case 5: return 1000000000;
            default: return ids.empty() ? 1 : ids[rng() % ids.size()];
            }
        }
        if (type == QLatin1String("number") || type == QLatin1String("any") || type.isEmpty()) {
            switch (rng() % 9) {
            case 0: return 0;
            case 1: return -1;
            case 2: return 1e300;
            case 3: return -1e300;
            case 4: return 1e-300;
            case 5: return 86400.0 * 400;
            default: return (rng() % 60001) / 1000.0;
            }
        }
        if (type == QLatin1String("boolean"))
            return bool(rng() % 2);
        if (type == QLatin1String("string")) {
            switch (rng() % 6) {
            case 0: return QString();
            case 1: return QString(5000, QLatin1Char('x'));
            case 2: return QStringLiteral("nonexistent");
            case 3: return QStringLiteral("../..\\:*?\"<>|");
            default: return QStringLiteral("name %1").arg(rng() % 100);
            }
        }
        if (type == QLatin1String("array")) {
            QJsonArray a;
            const QJsonObject items = schema[QStringLiteral("items")].toObject();
            const int n = rng() % 6 == 0 ? 300 : int(rng() % 4);
            for (int k = 0; k < n && depth < 3; ++k)
                a.append(value(name, items, depth + 1));
            return a;
        }
        if (type == QLatin1String("object"))
            return rng() % 2 ? QJsonObject{} : QJsonObject{{QStringLiteral("x"), 1}, {QStringLiteral("value"), -1e300}};
        return QJsonValue();
    };

    // A message box opened by a tool would hold up the assistant: note it and close it.
    QStringList boxes;
    QTimer closer;
    QObject::connect(&closer, &QTimer::timeout, [&] {
        if (auto* box = qobject_cast<QMessageBox*>(QApplication::activeModalWidget())) {
            boxes << box->windowTitle() + QStringLiteral(": ") + box->text().left(80);
            box->reject();
        }
    });
    closer.start(300);

    QString problem;
    int answered = 0, errors = 0;
    std::map<QString, int> reasons; // (why calls were refused: one reason refusing most of them hides the rest)
    QElapsedTimer slowest;
    qint64 worst = 0;
    QString worstCall;
    for (int i = 0; i < rounds && problem.isEmpty(); ++i) {
        const QJsonObject tool = tools[rng() % tools.size()];
        const QString name = tool[QStringLiteral("name")].toString();
        QJsonObject args;
        const QJsonObject props = tool[QStringLiteral("inputSchema")].toObject()[QStringLiteral("properties")].toObject();
        for (auto it = props.begin(); it != props.end(); ++it)
            if (rng() % 3)
                args[it.key()] = value(it.key(), it.value().toObject(), 0);
        // (not into the user's own library folders, nor proxies made in the background)
        const QString action = args.value(QStringLiteral("action")).toString(); // (value(): [] would add the key)
        if ((name == QLatin1String("graphics_overlay") && (action == QLatin1String("save_character") || action == QLatin1String("brand")))
            || (name == QLatin1String("media") && action.contains(QLatin1String("prox"))))
            args.remove(QStringLiteral("action"));
        const QJsonObject message{{QStringLiteral("jsonrpc"), QStringLiteral("2.0")},
                                  {QStringLiteral("id"), i + 1},
                                  {QStringLiteral("method"), QStringLiteral("tools/call")},
                                  {QStringLiteral("params"), QJsonObject{{QStringLiteral("name"), name}, {QStringLiteral("arguments"), args}}}};
        const QString call = name + QStringLiteral(" ") + QString::fromUtf8(QJsonDocument(args).toJson(QJsonDocument::Compact)).left(400);
        if (qEnvironmentVariableIsSet("MVP_MCP_FUZZ_TRACE")) { // (as it goes: a call that hangs is the last one shown)
            std::fprintf(stderr, "CALL  %d  %s\n", i, qPrintable(call));
            std::fflush(stderr);
        }
        slowest.start();
        const QJsonObject answer = server->handle(message);
        if (const qint64 ms = slowest.elapsed(); ms > 3000 && qEnvironmentVariableIsSet("MVP_MCP_FUZZ_TRACE")) {
            std::fprintf(stderr, "SLOW  %d  %lld ms\n", i, (long long)ms);
            std::fflush(stderr);
        }
        if (const qint64 ms = slowest.elapsed(); ms > worst) {
            worst = ms;
            worstCall = call;
        }
        if (!answer.contains(QStringLiteral("result")) && !answer.contains(QStringLiteral("error")))
            problem = QStringLiteral("call %1: no answer to %2").arg(i).arg(call);
        else if (answer[QStringLiteral("result")].toObject()[QStringLiteral("isError")].toBool() || answer.contains(QStringLiteral("error"))) {
            ++errors;
            const QJsonArray content = answer[QStringLiteral("result")].toObject()[QStringLiteral("content")].toArray();
            ++reasons[(content.isEmpty() ? answer[QStringLiteral("error")].toObject()[QStringLiteral("message")].toString()
                                         : content[0].toObject()[QStringLiteral("text")].toString())
                          .left(70)];
        }
        else
            ++answered;
        if (const QString p = timelineProblem(project->timeline()); !p.isEmpty())
            problem = QStringLiteral("call %1 (%2) left the timeline unsound: %3").arg(i).arg(call, p);
        if (!boxes.isEmpty())
            problem = QStringLiteral("call %1 (%2) opened a message box: %3").arg(i).arg(call, boxes.join(QStringLiteral("; ")));
        QApplication::processEvents();
    }
    closer.stop();
    std::vector<std::pair<int, QString>> top;
    for (const auto& [why, count] : reasons)
        top.emplace_back(count, why);
    std::sort(top.rbegin(), top.rend());
    for (size_t k = 0; k < std::min<size_t>(4, top.size()); ++k)
        log << QStringLiteral("NOTE  refused %1x: %2").arg(top[k].first).arg(top[k].second);
    check(top.empty() || top.front().first < (answered + errors) / 3,
          QStringLiteral("no one reason refuses most calls (%1)").arg(top.empty() ? QString() : top.front().second));
    check(problem.isEmpty(),
          QStringLiteral("AI control with hostile arguments: %1 calls of %2 tools (%3 done, %4 refused with a reason; slowest %5 ms: %6)%7")
              .arg(answered + errors)
              .arg(tools.size())
              .arg(answered)
              .arg(errors)
              .arg(worst)
              .arg(worstCall.left(160))
              .arg(problem.isEmpty() ? QString() : QStringLiteral(": ") + problem));
    QDir(scratch).removeRecursively();
    return failures;
}
