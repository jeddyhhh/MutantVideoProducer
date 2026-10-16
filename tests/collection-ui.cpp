#include "collection-ui.h"
#include "ui/MainWindow.h"
#include "ui/McpServer.h"
#include "core/Project.h"
#include <QDir>
#include <QFileInfo>
#include <QFile>
#include <QDateTime>
#include <QJsonDocument>
#include <QTcpSocket>
#include <QHostAddress>
#include <QEventLoop>
#include <QTimer>

int runCollectionUiTests(MainWindow* window, QStringList& log)
{
    int failures = 0;
    auto check = [&](bool ok, const QString& label) {
        log << QString("%1  %2").arg(ok ? "PASS" : "FAIL", label);
        failures += !ok;
        QFile progress(QDir(QFileInfo(MainWindow::autosaveDir()).absolutePath()).filePath("progress.txt"));
        if (progress.open(QIODevice::Append | QIODevice::Text))
            progress.write(log.back().toUtf8() + '\n');
    };
    auto wait = [](int ms) {
        QEventLoop loop;
        QTimer::singleShot(ms, &loop, &QEventLoop::quit);
        loop.exec();
    };
    auto* project = window->findChild<Project*>();
    auto* server = window->findChild<McpServer*>();
    check(project && server, "collection UI and MCP fixture available");
    if (!project || !server)
        return failures;
    const QDir work(QFileInfo(MainWindow::autosaveDir()).absolutePath());
    QDir().mkpath(work.absolutePath());
    if (project->filePath().isEmpty()) {
        QString error;
        check(project->save(work.filePath("collection-source.mvp"), &error), "collection source fixture saved");
        auto edit = project->timeline();
        Marker marker;
        marker.id = edit.allocId();
        marker.time = 100000;
        edit.markers.push_back(marker);
        project->applyEdit(edit, "unsaved collection marker");
    }
    const QString destination = work.filePath(QString("collection-ui-%1").arg(QDateTime::currentMSecsSinceEpoch()));
    auto before = project->snapshot();
    const QString file = project->filePath();
    const bool dirty = project->isModified();
    auto call = [&](QJsonObject args) {
        return server->handle(QJsonObject{{"jsonrpc", "2.0"},
                                          {"id", 900},
                                          {"method", "tools/call"},
                                          {"params", QJsonObject{{"name", "collect_project"}, {"arguments", args}}}});
    };
    const auto reply = call({{"destination", destination}, {"include_unused_media", false}, {"include_voice_models", false}});
    check(reply.contains("result") && !reply["result"].toObject()["isError"].toBool()
              && QFileInfo::exists(QDir(destination).filePath(QFileInfo(file).fileName())),
          "MCP collection publishes a portable project through the progress window");
    check(project->snapshot() == before && project->filePath() == file && project->isModified() == dirty,
          "MCP collection preserves current project and unsaved state");
    check(call({{"destination", 3}})["result"].toObject()["isError"].toBool(),
          "MCP collection rejects malformed destination without writing files");
    QString networkError;
    if (server->start(0, &networkError)) {
        const auto port = server->port();
        auto packet = [&](QByteArray body) {
            return "POST /mcp HTTP/1.1\r\nHost: 127.0.0.1:" + QByteArray::number(port)
                + "\r\nContent-Length: " + QByteArray::number(body.size()) + "\r\nConnection: close\r\n\r\n" + body;
        };
        const QString abandonedDestination = destination + "-abandoned";
        const auto request = QJsonDocument(QJsonObject{{"jsonrpc", "2.0"},
                                                       {"id", 902},
                                                       {"method", "tools/call"},
                                                       {"params",
                                                        QJsonObject{{"name", "collect_project"},
                                                                    {"arguments",
                                                                     QJsonObject{{"destination", abandonedDestination},
                                                                                 {"include_unused_media", false},
                                                                                 {"include_voice_models", false}}}}}})
                                 .toJson(QJsonDocument::Compact);
        QTcpSocket client;
        client.connectToHost(QHostAddress::LocalHost, port);
        client.waitForConnected(1000);
        client.write(packet(request));
        client.flush();
        QTimer::singleShot(10, &client, [&] { client.abort(); });
        wait(100);
        check(QFileInfo::exists(QDir(abandonedDestination).filePath(QFileInfo(file).fileName())),
              "collection safely completes if its MCP client disconnects during progress");
        QTcpSocket next;
        next.connectToHost(QHostAddress::LocalHost, port);
        next.waitForConnected(1000);
        next.write(packet(R"({"jsonrpc":"2.0","id":903,"method":"ping"})"));
        next.flush();
        wait(80);
        check(next.readAll().startsWith("HTTP/1.1 200"), "MCP transport remains responsive after a disconnected collection client");
        server->stop();
    } else
        check(false, "collection HTTP fixture: " + networkError);
    return failures;
}
