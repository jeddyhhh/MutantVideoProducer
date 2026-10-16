#pragma once

// AI control over MCP (the Model Context Protocol): Mutant Video Producer as a tool server
// an AI assistant (Claude Code, Claude Desktop, any MCP client) connects to,
// to look at and edit the project and play the Live tab.
//
// Transport: MCP's Streamable HTTP, on this computer only (127.0.0.1, port
// 47811 by default; Options > AI Control). Requests are JSON-RPC POSTed to
// /mcp and answered with JSON; requests from web pages (an Origin header
// that isn't this computer) are refused. Clients that start servers as a
// program use the stdio bridge: `MutantVideoProducer --mcp-stdio` passes each line on
// to the running Mutant Video Producer and its answer back.
//
// Every edit goes through the undo stack ("AI: ..."), so Ctrl+Z takes it
// back. Going live (streaming) is left to the person: there is no tool for it.

#include <QHash>
#include <QJsonArray>
#include <QJsonObject>
#include <QSet>
#include <QObject>
#include <atomic>
#include <memory>
#include <thread>

class MainWindow;
class QTcpServer;
class QTcpSocket;

class McpServer : public QObject {
    Q_OBJECT
public:
    static constexpr quint16 kDefaultPort = 47811;

    explicit McpServer(MainWindow* window, QObject* parent = nullptr);
    ~McpServer() override;

    bool start(quint16 port, QString* error = nullptr);
    void stop();
    bool isRunning() const;
    quint16 port() const;

    // One JSON-RPC message (a request or a notification); the response, or an
    // empty object for notifications. (The HTTP side and tests use it.)
    QJsonObject handle(const QJsonObject& message);
    // The tools, as tools/list describes them (the stdio bridge answers with
    // these when Mutant Video Producer isn't running).
    static QJsonArray toolList();
    static QJsonObject initializeResult(const QString& requestedVersion);

signals:
    // A tool was called (for the status bar).
    void activity(const QString& tool);

private:
    void onConnection();
    void onReadable(QTcpSocket* socket);
    QJsonObject callTool(const QString& name, const QJsonObject& args);
    struct ToolCall; // one call's arguments and context; each tool is a method

    MainWindow* m_window;
    QTcpServer* m_server;
    QHash<QTcpSocket*, QByteArray> m_buffers;
    QSet<QTcpSocket*> m_reading;
    // A render started by the render tool.
    struct Render;
    std::shared_ptr<Render> m_render;
};

// `MutantVideoProducer --mcp-stdio [--port N]`: reads JSON-RPC messages, one per line, from
// stdin, passes them to the running Mutant Video Producer and writes the answers to stdout.
int runMcpStdioBridge(int argc, char** argv);
