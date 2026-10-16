#include "core/Brand.h"
#include "ui/McpServer.h"

#include "core/AudioEffects.h"
#include "core/AudioFinishing.h"
#include "core/GraphicsWorkflow.h"
#include "ui/AudioFinishingWindow.h"
#include "core/BeatMarkers.h"
#include "core/BeatTracker.h"
#include "core/Captions.h"
#include "core/Voice.h"
#include "core/AiRuntime.h"
#include "ui/VoiceJob.h"
#include "ui/RenderStatusDialog.h"
#include "core/Profiles.h"
#include "core/Compositor.h"
#include "core/EffectMask.h"
#include "core/Effects.h"
#include "core/Exporter.h"
#include "core/FrameRenderer.h"
#include "core/Generators.h"
#include "core/Graphics.h"
#include "core/MotionGraphics.h"
#include "core/Hardware.h"
#include "core/MediaInfo.h"
#include "core/Project.h"
#include "core/Proxy.h"
#include "core/TimelineOps.h"
#include "core/Vst3Host.h"
#include "engine/LiveEngine.h"
#include "engine/PlaybackEngine.h"
#include "ui/LivePadPanel.h"
#include "ui/LiveView.h"
#include "ui/MainWindow.h"
#include "core/Story.h"
#include "ui/PreviewWidget.h"
#include "ui/TimelineWidget.h"

#include <QTimer>
#include <QBuffer>
#include <QComboBox>
#include <QCoreApplication>
#include <QFile>
#include <QFileInfo>
#include <QHostAddress>
#include <QJsonDocument>
#include <QLabel>
#include <QOffscreenSurface>
#include <QSettings>
#include <QSaveFile>
#include <QPointer>
#include <QTcpServer>
#include <QTcpSocket>
#include <QToolButton>
#include <QUrl>
#include <algorithm>
#include <cmath>
#include <functional>
#include <set>

#ifdef Q_OS_WIN
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

#ifndef MVP_VERSION
#define MVP_VERSION "dev"
#endif

namespace {

constexpr int kMaxBody = 16 * 1024 * 1024;

// Seconds from a client, kept within ±11.5 days (as project files are):
// anything bigger would overflow the times built from it.
TimeUs us(double seconds)
{
    return TimeUs(std::llround(std::clamp(std::isfinite(seconds) ? seconds : 0.0, -1e6, 1e6) * double(kUsPerSecond)));
}

double sec(TimeUs t) { return double(t) / double(kUsPerSecond); }

// ---- Tool descriptions --------------------------------------------------------

QJsonObject prop(const char* type, const char* description)
{
    if (QString::fromLatin1(type) == "any")
        return QJsonObject{{QStringLiteral("description"), QString::fromUtf8(description)}};
    return QJsonObject{{QStringLiteral("type"), QString::fromLatin1(type)},
                       {QStringLiteral("description"), QString::fromUtf8(description)}};
}

QJsonObject enumProp(const QStringList& values, const char* description)
{
    QJsonObject o = prop("string", description);
    o[QStringLiteral("enum")] = QJsonArray::fromStringList(values);
    return o;
}

QJsonObject arrayProp(const char* itemType, const char* description)
{
    QJsonObject o = prop("array", description);
    o[QStringLiteral("items")] = QJsonObject{{QStringLiteral("type"), QString::fromLatin1(itemType)}};
    return o;
}

QJsonObject tool(const char* name, const char* description, const QJsonObject& properties = {}, const QStringList& required = {},
                 bool readOnly = false)
{
    QJsonObject schema{{QStringLiteral("type"), QStringLiteral("object")}, {QStringLiteral("properties"), properties}};
    if (!required.isEmpty())
        schema[QStringLiteral("required")] = QJsonArray::fromStringList(required);
    QJsonObject t{{QStringLiteral("name"), QString::fromLatin1(name)},
                  {QStringLiteral("description"), QString::fromUtf8(description)},
                  {QStringLiteral("inputSchema"), schema}};
    if (readOnly)
        t[QStringLiteral("annotations")] = QJsonObject{{QStringLiteral("readOnlyHint"), true}};
    return t;
}

// ---- Results --------------------------------------------------------------------

QJsonObject textResult(const QString& text)
{
    return QJsonObject{{QStringLiteral("content"),
                        QJsonArray{QJsonObject{{QStringLiteral("type"), QStringLiteral("text")}, {QStringLiteral("text"), text}}}}};
}

QJsonObject jsonResult(const QJsonObject& data)
{
    QJsonObject r = textResult(QString::fromUtf8(QJsonDocument(data).toJson(QJsonDocument::Compact)));
    r[QStringLiteral("structuredContent")] = data;
    return r;
}

QJsonObject errorResult(const QString& why)
{
    QJsonObject r = textResult(why);
    r[QStringLiteral("isError")] = true;
    return r;
}

QJsonObject imageResult(const QImage& img, const QString& caption)
{
    QByteArray bytes;
    QBuffer buf(&bytes);
    buf.open(QIODevice::WriteOnly);
    img.convertToFormat(QImage::Format_RGB32).save(&buf, "JPG", 88);
    QJsonArray content{QJsonObject{{QStringLiteral("type"), QStringLiteral("image")},
                                   {QStringLiteral("data"), QString::fromLatin1(bytes.toBase64())},
                                   {QStringLiteral("mimeType"), QStringLiteral("image/jpeg")}},
                       QJsonObject{{QStringLiteral("type"), QStringLiteral("text")}, {QStringLiteral("text"), caption}}};
    return QJsonObject{{QStringLiteral("content"), content}};
}

// ---- Effects ----------------------------------------------------------------------

std::vector<ParamDef> paramDefsFor(const QString& type)
{
    std::vector<ParamDef> defs;
    if (type == QLatin1String(kRackType)) {
        defs = rackParams();
        return defs;
    }
    if (const auto meta = effectMeta(type); meta && meta->params) {
        defs = *meta->params;
        if (!meta->audio) // every video effect can have a mask
            for (const ParamDef& d : effectMaskParams())
                defs.push_back(d);
    }
    return defs;
}

QJsonValue paramValueJson(const ParamDef& def, const ParamValue& v)
{
    if (def.type == ParamType::Text || def.type == ParamType::Font)
        return v.text;
    auto plain = [&](double x) -> QJsonValue {
        switch (def.type) {
        case ParamType::Bool: return x > 0.5;
        case ParamType::Choice: {
            const int i = int(std::lround(x));
            return i >= 0 && i < def.choices.size() ? QJsonValue(def.choices[i]) : QJsonValue(i);
        }
        case ParamType::Color: return QStringLiteral("#%1").arg(uint(x) & 0xffffffu, 6, 16, QLatin1Char('0'));
        default: return x;
        }
    };
    if (v.keys.empty())
        return plain(v.value);
    QJsonArray keys;
    for (const Keyframe& k : v.keys) {
        QJsonArray key{sec(k.time), plain(k.value)};
        if (k.interp != Keyframe::Interp::Linear) // the curve to the next keyframe (and Bezier handles)
            key.append(keyInterpId(k.interp));
        if (k.interp == Keyframe::Interp::Bezier)
            key.append(QJsonArray{k.x1, k.y1, k.x2, k.y2});
        keys.append(key);
    }
    return QJsonObject{{QStringLiteral("keyframes"), keys}};
}

QJsonObject effectJson(const EffectInstance& e)
{
    QJsonObject o{{QStringLiteral("effect_id"), e.id}, {QStringLiteral("type"), e.type}, {QStringLiteral("enabled"), e.enabled}};
    if (e.isRack()) {
        o[QStringLiteral("name")] = QStringLiteral("Rack");
        QJsonArray chains;
        for (const RackChain& c : e.chains) {
            QJsonArray fx;
            for (const EffectInstance& sub : c.effects)
                fx.append(effectJson(sub));
            chains.append(QJsonObject{{QStringLiteral("name"), c.name}, {QStringLiteral("effects"), fx}});
        }
        o[QStringLiteral("chains")] = chains;
        return o;
    }
    if (const auto meta = effectMeta(e.type))
        o[QStringLiteral("name")] = meta->name;
    QJsonObject params;
    for (const ParamDef& d : paramDefsFor(e.type)) {
        const auto it = e.params.find(d.id);
        if (it != e.params.end())
            params[d.id] = paramValueJson(d, it->second);
    }
    o[QStringLiteral("params")] = params;
    return o;
}

QJsonArray chainJson(const EffectChain& chain)
{
    QJsonArray a;
    for (const EffectInstance& e : chain)
        a.append(effectJson(e));
    return a;
}

// One value: a number, true / false, a choice (its name or number), a colour
// "#rrggbb", text, or {"keyframes": [[seconds, value], ...]}.
QString setParam(ParamMap& params, const std::vector<ParamDef>& defs, const QString& id, const QJsonValue& v)
{
    const auto def = std::find_if(defs.begin(), defs.end(), [&](const ParamDef& d) { return d.id == id; });
    if (def == defs.end()) {
        QStringList ids;
        for (const ParamDef& d : defs)
            ids << d.id;
        return QStringLiteral("no setting \"%1\" (it has: %2)").arg(id, ids.join(QStringLiteral(", ")));
    }
    auto number = [&](const QJsonValue& x, double* out) -> QString {
        switch (def->type) {
        case ParamType::Bool: *out = x.isBool() ? (x.toBool() ? 1.0 : 0.0) : (x.toDouble() > 0.5 ? 1.0 : 0.0); return {};
        case ParamType::Choice:
            if (x.isString()) {
                const int i = int(def->choices.indexOf(x.toString(), 0));
                int found = i;
                if (found < 0)
                    for (int c = 0; c < def->choices.size(); ++c)
                        if (def->choices[c].compare(x.toString(), Qt::CaseInsensitive) == 0)
                            found = c;
                if (found < 0)
                    return QStringLiteral("\"%1\" isn't one of %2's choices: %3")
                        .arg(x.toString(), id, def->choices.join(QStringLiteral(", ")));
                *out = found;
                return {};
            }
            *out = std::clamp(double(x.toInt()), 0.0, double(std::max<qsizetype>(0, def->choices.size() - 1)));
            return {};
        case ParamType::Color: {
            if (x.isString()) {
                const QColor c(x.toString());
                if (!c.isValid())
                    return QStringLiteral("\"%1\" isn't a colour (use #rrggbb)").arg(x.toString());
                *out = double(c.rgb() & 0xffffffu);
            } else {
                *out = std::clamp(std::floor(x.toDouble()), 0.0, double(0xffffff));
            }
            return {};
        }
        default:
            if (!x.isDouble())
                return QStringLiteral("%1 takes a number").arg(id);
            *out = std::clamp(x.toDouble(), def->min, def->max);
            return {};
        }
    };
    ParamValue& pv = params[id];
    if (def->type == ParamType::Text || def->type == ParamType::Font) {
        pv.text = v.toString();
        return {};
    }
    if (v.isObject() && v.toObject().contains(QStringLiteral("keyframes"))) {
        if (!def->animatable())
            return QStringLiteral("%1 can't be keyframed").arg(id);
        pv.keys.clear();
        for (const QJsonValue& k : v.toObject()[QStringLiteral("keyframes")].toArray()) {
            const QJsonArray pair = k.toArray();
            double x = 0;
            if (pair.size() < 2 || pair.size() > 4)
                return QStringLiteral(
                    "keyframes are [seconds, value] or [seconds, value, curve] or [seconds, value, \"bezier\", [x1, y1, x2, y2]]");
            if (const QString e = number(pair[1], &x); !e.isEmpty())
                return e;
            const TimeUs t = us(pair[0].toDouble());
            pv.addKey(t, x);
            if (pair.size() >= 3) {
                const auto interp = keyInterpFromId(pair[2].toString());
                if (!interp)
                    return QStringLiteral("unknown keyframe curve \"%1\" (linear, hold, smooth, ease_in, ease_out, ease_in_out, bezier, "
                                          "bounce, elastic)")
                        .arg(pair[2].toString());
                for (Keyframe& key : pv.keys)
                    if (key.time == t) {
                        key.interp = *interp;
                        const QJsonArray h = pair.size() == 4 ? pair[3].toArray() : QJsonArray{};
                        if (h.size() == 4 && std::all_of(h.begin(), h.end(), [](const QJsonValue& v) {
                                return v.isDouble() && std::isfinite(v.toDouble());
                            }))
                            key.x1 = std::clamp(h[0].toDouble(), 0.0, 1.0), key.y1 = h[1].toDouble(),
                            key.x2 = std::clamp(h[2].toDouble(), 0.0, 1.0), key.y2 = h[3].toDouble();
                    }
            }
        }
        return {};
    }
    double x = 0;
    if (const QString e = number(v, &x); !e.isEmpty())
        return e;
    pv.keys.clear();
    pv.value = x;
    return {};
}

QString setParams(EffectInstance& e, const QJsonObject& params)
{
    const std::vector<ParamDef> defs = paramDefsFor(e.type);
    for (auto it = params.begin(); it != params.end(); ++it)
        if (const QString err = setParam(e.params, defs, it.key(), it.value()); !err.isEmpty())
            return err;
    return {};
}

// Every chain in the project an effect can be in, with a name for it.
void forEachChain(Timeline& tl, const std::function<bool(EffectChain&, const QString&)>& fn)
{
    std::function<bool(EffectChain&, const QString&)> walk = [&](EffectChain& chain, const QString& where) {
        if (fn(chain, where))
            return true;
        for (EffectInstance& e : chain)
            for (RackChain& c : e.chains)
                if (walk(c.effects, where + QStringLiteral(" > rack")))
                    return true;
        return false;
    };
    for (Track& t : tl.tracks) {
        if (walk(t.effects, QStringLiteral("track %1").arg(t.name)))
            return;
        for (Clip& c : t.clips)
            if (walk(c.effects, QStringLiteral("event %1").arg(c.id)))
                return;
    }
    if (walk(tl.outputEffects, QStringLiteral("video output")) || walk(tl.audioMasterEffects, QStringLiteral("audio master"))
        || walk(tl.liveEffects, QStringLiteral("Live FX")) || walk(tl.liveLayerEffects, QStringLiteral("Live layer")))
        return;
    for (int i = 0; i < kLivePads; ++i)
        if (walk(tl.livePads[size_t(i)].effects, QStringLiteral("pad %1").arg(i + 1)))
            return;
}

EffectInstance* findEffect(Timeline& tl, int id, EffectChain** owner = nullptr)
{
    EffectInstance* found = nullptr;
    forEachChain(tl, [&](EffectChain& chain, const QString&) {
        for (EffectInstance& e : chain)
            if (e.id == id) {
                found = &e;
                if (owner)
                    *owner = &chain;
                return true;
            }
        return false;
    });
    return found;
}

QJsonObject paramDefJson(const ParamDef& d)
{
    QJsonObject o{{QStringLiteral("id"), d.id}, {QStringLiteral("name"), d.name}};
    switch (d.type) {
    case ParamType::Number:
        o[QStringLiteral("type")] = QStringLiteral("number");
        o[QStringLiteral("min")] = d.min;
        o[QStringLiteral("max")] = d.max;
        o[QStringLiteral("default")] = d.def;
        if (!d.suffix.isEmpty())
            o[QStringLiteral("unit")] = d.suffix.trimmed();
        break;
    case ParamType::Choice:
        o[QStringLiteral("type")] = QStringLiteral("choice");
        o[QStringLiteral("choices")] = QJsonArray::fromStringList(d.choices);
        o[QStringLiteral("default")] = d.choices.value(int(d.def));
        break;
    case ParamType::Bool:
        o[QStringLiteral("type")] = QStringLiteral("bool");
        o[QStringLiteral("default")] = d.def > 0.5;
        break;
    case ParamType::Color:
        o[QStringLiteral("type")] = QStringLiteral("color");
        o[QStringLiteral("default")] = QStringLiteral("#%1").arg(uint(d.def) & 0xffffffu, 6, 16, QLatin1Char('0'));
        break;
    default:
        o[QStringLiteral("type")] = QStringLiteral("text");
        o[QStringLiteral("default")] = d.defText;
        break;
    }
    if (!d.tooltip.isEmpty())
        o[QStringLiteral("help")] = d.tooltip;
    return o;
}

const char* kInstructions = "Mutant Video Producer is a video editor with a Live tab for real-time effects. Times are in seconds. "
                            "Start with get_project to see the media, tracks, events (clips) and markers. "
                            "Event ids, effect ids and media ids come from get_project. Effects are found with list_effects "
                            "(video, audio, transition and generator types and their settings). Every change can be undone "
                            "(undo). Use get_frame to look at the picture after a change. Live: live_status and live_action.";

} // namespace

// ---------------------------------------------------------------------------
// Rendering in the background
// ---------------------------------------------------------------------------

struct McpServer::Render {
    std::thread thread;
    std::atomic<double> progress{0};
    std::atomic<bool> done{false}, ok{false}, cancel{false};
    QString path, error, preset;
    std::shared_ptr<RenderMonitor> monitor = std::make_shared<RenderMonitor>();
    std::unique_ptr<QOffscreenSurface> surface;
    ~Render()
    {
        cancel = true;
        if (thread.joinable())
            thread.join();
    }
};

// ---------------------------------------------------------------------------
// Server
// ---------------------------------------------------------------------------

McpServer::McpServer(MainWindow* window, QObject* parent) : QObject(parent), m_window(window), m_server(new QTcpServer(this))
{
    connect(m_server, &QTcpServer::newConnection, this, &McpServer::onConnection);
}

McpServer::~McpServer()
{
    if (m_render) {
        m_render->cancel = true;
        if (m_render->thread.joinable())
            m_render->thread.join();
    }
    stop();
}

bool McpServer::start(quint16 port, QString* error)
{
    stop();
    if (!m_server->listen(QHostAddress::LocalHost, port)) { // this computer only
        if (error)
            *error = m_server->errorString();
        return false;
    }
    return true;
}

void McpServer::stop()
{
    m_server->close();
    for (QTcpSocket* s : m_buffers.keys())
        s->abort();
    m_buffers.clear();
    m_reading.clear();
}

bool McpServer::isRunning() const { return m_server->isListening(); }

quint16 McpServer::port() const { return m_server->serverPort(); }

void McpServer::onConnection()
{
    while (QTcpSocket* s = m_server->nextPendingConnection()) {
        if (m_buffers.size() >= 128) {
            s->abort();
            s->deleteLater();
            continue;
        }
        s->setReadBufferSize(kMaxBody + 64 * 1024 + 4);
        auto* deadline = new QTimer(s);
        deadline->setObjectName(QStringLiteral("mcpRequestDeadline"));
        deadline->setSingleShot(true);
        deadline->setInterval(30000);
        connect(deadline, &QTimer::timeout, s, [this, s] {
            if (m_buffers.contains(s) && !m_buffers.value(s).isEmpty())
                s->abort();
        });
        m_buffers.insert(s, {});
        connect(s, &QTcpSocket::readyRead, this, [this, s] { onReadable(s); });
        connect(s, &QTcpSocket::disconnected, this, [this, s] {
            m_buffers.remove(s);
            // Qt emits channelReadyRead after readyRead. If a tool spins a
            // nested event loop, defer deletion until that read handler exits.
            if (!m_reading.contains(s))
                s->deleteLater();
        });
        // A request can be here already (it came with the connection): no readyRead follows for it.
        if (s->bytesAvailable() > 0)
            onReadable(s);
    }
}

void McpServer::onReadable(QTcpSocket* s)
{
    if (!m_buffers.contains(s) || m_reading.contains(s))
        return;
    QPointer<QTcpSocket> socket = s;
    m_reading.insert(s);
    // A tool can open a progress event loop. Its client may disconnect or other
    // clients may arrive there; never retain a reference into the socket hash.
    QByteArray buf = std::move(m_buffers[s]);
    struct Finish {
        std::function<void()> action;
        ~Finish() { action(); }
    } finish{[&] {
        if (!socket)
            return;
        m_reading.remove(socket.data());
        if (!m_buffers.contains(socket.data())) {
            socket->deleteLater();
            return;
        }
        if (socket->state() != QAbstractSocket::ConnectedState)
            return;
        m_buffers[socket.data()] = buf;
        if (!buf.isEmpty())
            if (auto* timer = socket->findChild<QTimer*>("mcpRequestDeadline"); timer && !timer->isActive())
                timer->start();
        if (socket->bytesAvailable() > 0)
            QTimer::singleShot(0, this, [this, socket] {
                if (socket)
                    onReadable(socket.data());
            });
    }};
    auto* deadline = s->findChild<QTimer*>(QStringLiteral("mcpRequestDeadline"));
    if (deadline && !deadline->isActive())
        deadline->start();
    buf += s->readAll();
    auto reject = [&](const char* reason) {
        const QByteArray body =
            QJsonDocument(QJsonObject{{QStringLiteral("error"), QString::fromLatin1(reason)}}).toJson(QJsonDocument::Compact);
        s->write("HTTP/1.1 400 Bad Request\r\nContent-Type: application/json\r\nConnection: close\r\nContent-Length: "
                 + QByteArray::number(body.size()) + "\r\n\r\n" + body);
        s->disconnectFromHost();
    };
    if (buf.size() > kMaxBody + 64 * 1024 + 4) {
        reject("Request buffer limit exceeded");
        return;
    }
    for (;;) {
        const int headerEnd = int(buf.indexOf("\r\n\r\n"));
        if (headerEnd < 0) {
            if (buf.isEmpty() && deadline)
                deadline->stop();
            if (buf.size() > 64 * 1024)
                reject("HTTP headers exceed 64 KB");
            return;
        }
        if (headerEnd > 64 * 1024) {
            reject("HTTP headers exceed 64 KB");
            return;
        }
        const QList<QByteArray> lines = buf.left(headerEnd).split('\n');
        const QList<QByteArray> request = lines.value(0).trimmed().split(' ');
        if (request.size() != 3 || request[0].isEmpty() || request[1].isEmpty() || (request[2] != "HTTP/1.1" && request[2] != "HTTP/1.0")) {
            reject("Invalid HTTP request line");
            return;
        }
        QHash<QByteArray, QByteArray> headers;
        for (int i = 1; i < lines.size(); ++i) {
            const int colon = int(lines[i].indexOf(':'));
            const QByteArray name = lines[i].left(colon).toLower();
            if (colon <= 0 || !std::all_of(name.begin(), name.end(), [](char c) { return c > 32 && c < 127; })) {
                reject("Invalid HTTP header");
                return;
            }
            if (headers.contains(name) && (name == "content-length" || name == "transfer-encoding" || name == "host" || name == "origin")) {
                reject("Duplicate HTTP framing or origin header");
                return;
            }
            headers.insert(name, lines[i].mid(colon + 1).trimmed());
        }
        if (headers.contains("transfer-encoding")) {
            reject("Transfer-Encoding is not supported");
            return;
        }
        const QByteArray rawLength = headers.value("content-length", "0");
        bool lengthOk = false;
        const qlonglong bodyLength = rawLength.toLongLong(&lengthOk);
        if (!lengthOk || rawLength.isEmpty()
            || !std::all_of(rawLength.begin(), rawLength.end(), [](char c) { return c >= '0' && c <= '9'; }) || bodyLength > kMaxBody
            || (request[0] == "POST" && !headers.contains("content-length"))) {
            reject("Invalid or missing Content-Length");
            return;
        }
        const int length = int(bodyLength);
        if (buf.size() < headerEnd + 4 + length)
            return; // the rest of the body is on its way
        const QByteArray body = buf.mid(headerEnd + 4, length);
        buf.remove(0, headerEnd + 4 + length);
        const QByteArray method = request.value(0), path = request.value(1);

        auto reply = [&](int code, const char* status, const QByteArray& content, const QByteArray& type = "application/json",
                         const QByteArray& extra = {}) {
            if (!socket || !m_buffers.contains(socket.data()))
                return;
            QByteArray out = "HTTP/1.1 " + QByteArray::number(code) + ' ' + status + "\r\n";
            if (!content.isEmpty())
                out += "Content-Type: " + type + "\r\n";
            out += "Content-Length: " + QByteArray::number(content.size()) + "\r\n" + extra + "\r\n";
            s->write(out + content);
        };
        auto closeRequested = [&] {
            if (!socket || !m_buffers.contains(socket.data()))
                return true;
            if (headers.value("connection").toLower() != "close")
                return false;
            s->disconnectFromHost();
            return true;
        };
        // Only this computer's own pages and programs: a web page somewhere
        // (an Origin header from elsewhere, or a Host that isn't us) is refused.
        auto local = [](const QByteArray& host) {
            const QString h = QUrl(QStringLiteral("http://") + QString::fromLatin1(host)).host().toLower();
            return h == QLatin1String("127.0.0.1") || h == QLatin1String("localhost") || h == QLatin1String("::1");
        };
        const QByteArray origin = headers.value("origin");
        const bool originOk =
            origin.isEmpty() || origin == "null" ? origin.isEmpty() : local(QUrl(QString::fromLatin1(origin)).authority().toLatin1());
        if (!originOk || (headers.contains("host") && !local(headers.value("host")))) {
            reply(403, "Forbidden", R"({"error":"requests from web pages are not accepted"})");
            if (closeRequested())
                return;
            continue;
        }
        const QByteArray route = path.split('?').value(0);
        if (route != "/mcp" && route != "/") {
            reply(404, "Not Found", R"({"error":"the MCP endpoint is /mcp"})");
            if (closeRequested())
                return;
            continue;
        }
        if (method == "GET") { // no server-initiated stream
            reply(405, "Method Not Allowed", {}, {}, "Allow: POST, DELETE\r\n");
            if (closeRequested())
                return;
            continue;
        }
        if (method == "DELETE") {
            reply(200, "OK", {});
            if (closeRequested())
                return;
            continue;
        }
        if (method != "POST") {
            reply(405, "Method Not Allowed", {}, {}, "Allow: POST, DELETE\r\n");
            if (closeRequested())
                return;
            continue;
        }
        QJsonParseError perr;
        const QJsonDocument doc = QJsonDocument::fromJson(body, &perr);
        if (perr.error != QJsonParseError::NoError) {
            const QJsonObject err{
                {QStringLiteral("jsonrpc"), QStringLiteral("2.0")},
                {QStringLiteral("id"), QJsonValue()},
                {QStringLiteral("error"), QJsonObject{{QStringLiteral("code"), -32700}, {QStringLiteral("message"), perr.errorString()}}}};
            reply(400, "Bad Request", QJsonDocument(err).toJson(QJsonDocument::Compact));
            if (closeRequested())
                return;
            continue;
        }
        if (doc.isArray()) { // a batch
            QJsonArray out;
            for (const QJsonValue& v : doc.array())
                if (const QJsonObject r = handle(v.toObject()); !r.isEmpty())
                    out.append(r);
            if (out.isEmpty())
                reply(202, "Accepted", {});
            else
                reply(200, "OK", QJsonDocument(out).toJson(QJsonDocument::Compact));
        } else {
            const QJsonObject r = handle(doc.object());
            if (r.isEmpty())
                reply(202, "Accepted", {});
            else
                reply(200, "OK", QJsonDocument(r).toJson(QJsonDocument::Compact));
        }
        if (!socket || !m_buffers.contains(socket.data()))
            return;
        if (headers.value("connection").toLower() == "close") {
            s->disconnectFromHost();
            return;
        }
    }
}

QJsonObject McpServer::initializeResult(const QString& requested)
{
    static const QStringList versions{QStringLiteral("2025-06-18"), QStringLiteral("2025-03-26"), QStringLiteral("2024-11-05")};
    return QJsonObject{
        {QStringLiteral("protocolVersion"), versions.contains(requested) ? requested : versions.front()},
        {QStringLiteral("capabilities"), QJsonObject{{QStringLiteral("tools"), QJsonObject{{QStringLiteral("listChanged"), false}}}}},
        {QStringLiteral("serverInfo"),
         QJsonObject{{QStringLiteral("name"), QStringLiteral("mvp")},
                     {QStringLiteral("title"), QStringLiteral("Mutant Video Producer video editor")},
                     {QStringLiteral("version"), QStringLiteral(MVP_VERSION)}}},
        {QStringLiteral("instructions"), QString::fromLatin1(kInstructions)}};
}

QJsonObject McpServer::handle(const QJsonObject& msg)
{
    const QString method = msg[QStringLiteral("method")].toString();
    const bool notification = !msg.contains(QStringLiteral("id"));
    if (notification)
        return {}; // notifications/initialized, cancelled...: nothing to answer
    QJsonObject response{{QStringLiteral("jsonrpc"), QStringLiteral("2.0")}, {QStringLiteral("id"), msg[QStringLiteral("id")]}};
    const QJsonObject params = msg[QStringLiteral("params")].toObject();
    if (method == QLatin1String("initialize")) {
        response[QStringLiteral("result")] = initializeResult(params[QStringLiteral("protocolVersion")].toString());
    } else if (method == QLatin1String("ping")) {
        response[QStringLiteral("result")] = QJsonObject();
    } else if (method == QLatin1String("tools/list")) {
        response[QStringLiteral("result")] = QJsonObject{{QStringLiteral("tools"), toolList()}};
    } else if (method == QLatin1String("tools/call")) {
        const QString name = params[QStringLiteral("name")].toString();
        emit activity(name);
        response[QStringLiteral("result")] = callTool(name, params[QStringLiteral("arguments")].toObject());
    } else {
        response[QStringLiteral("error")] =
            QJsonObject{{QStringLiteral("code"), -32601}, {QStringLiteral("message"), QStringLiteral("unknown method %1").arg(method)}};
    }
    return response;
}

// ---------------------------------------------------------------------------
// Tools
// ---------------------------------------------------------------------------

QJsonArray McpServer::toolList()
{
    const QStringList targets{QStringLiteral("event"), QStringLiteral("track"), QStringLiteral("output"), QStringLiteral("audio_master"),
                              QStringLiteral("live"),  QStringLiteral("pad"),   QStringLiteral("layer")};
    QJsonArray t;
    // Project.
    t.append(tool("get_project",
                  "The whole project: settings, media (ids, files, length, size), tracks with their events (event ids, where they "
                  "sit, what they show, their effects with ids and settings), track and output effects, markers, the cursor.",
                  {}, {}, true));
    t.append(tool("import_media", "Adds files (video, audio, pictures) to Project Media. Returns their media ids.",
                  {{QStringLiteral("paths"), arrayProp("string", "Full file paths")}}, {QStringLiteral("paths")}));
    t.append(tool("add_media_to_timeline",
                  "Places a media file on the timeline at a time: its picture on a video track and its sound on an audio track, "
                  "grouped. Tracks are created when missing. Returns the new event ids.",
                  {{QStringLiteral("media_id"), prop("integer", "From get_project / import_media")},
                   {QStringLiteral("at"), prop("number", "Timeline time in seconds (default: the cursor)")},
                   {QStringLiteral("track"), prop("integer", "Track index to prefer (0 = top); default: automatic")}},
                  {QStringLiteral("media_id")}));
    t.append(
        tool("add_text", "Adds a text / title event (or another generated picture: see list_effects kind=generator).",
             {{QStringLiteral("text"), prop("string", "The words")},
              {QStringLiteral("at"), prop("number", "Start, seconds (default: the cursor)")},
              {QStringLiteral("length"), prop("number", "Seconds (default 5)")},
              {QStringLiteral("preset"), prop("string", "Generator preset id (default \"title\"; e.g. lower_third, credits, solid_black)")},
              {QStringLiteral("track"), prop("integer", "Video track index (default: the first)")}}));
    t.append(tool(
        "audio_finishing",
        "Measure the entire final stereo mix after all audio effects using EBU R128 and oversampled true peaks. analyze returns LUFS, LRA, "
        "dBTP, sample peaks, stereo checks, silence and warnings. apply_gain reanalyzes and adds an undoable master gain limited by "
        "true-peak headroom; reanalyze afterward. A cancellable progress window appears. Targets are editable, not platform guarantees.",
        {{"action", prop("string", "analyze or apply_gain")},
         {"lufs", prop("number", "Target -36 to -5 LUFS (default -16)")},
         {"true_peak", prop("number", "Ceiling -12 to 0 dBTP (default -1.5)")}},
        {"action"}));
    t.append(tool(
        "graphics_overlay",
        "Native scene graph and animation engine. Actions "
        "capabilities/presets/validate/create/get/update/keyframe/bind_audio/export_template/import_template/layout/lock/brand. layout "
        "arranges selected layer_ids by operation and reference, preserving animation offsets. lock protects layer edits and descendants. "
        "brand applies a reusable font/palette kit to tagged static brand_role fills and static text fonts; locked/animated styles are "
        "preserved. v1 scenes retain their appearance; v2 adds paths (M/L/Q/C/Z commands), parented groups, symbols/instances, particles, "
        "repeaters, charts, camera, masks, gradients, shadows/glow, trails/motion_blur and character/word/line text_animation. Animate "
        "properties with {keyframes:[[event-local seconds,value,easing?],...],easing,loop,ping_pong}; numeric, colour, discrete and "
        "compatible path morph values work. follow:{path:layer_id,progress,orient,loop} follows curved paths. Scene variables substitute "
        "{{name}} in text; datasets drive charts. bind_audio embeds real source envelopes and native detected beats; property "
        "{audio:binding,base,amount,channel:peak|left|right|beat} reacts without adding sound. Read capabilities and "
        "docs/animation-scenes.md for limits and examples. All edits are saved and undoable.",
        {{"action",
          prop("string",
               "capabilities, presets, validate, create, get, update, keyframe, bind_audio, export_template, import_template, layout, "
               "lock, brand")},
         {"event_id", prop("integer", "Existing graphics event")},
         {"preset",
          prop("string", "Existing broadcast layouts or motion_race/titles/chart/route/particles/morph/audio/reusable (motion_ prefix)")},
         {"scene", prop("object", "Complete editable scene; update replaces it")},
         {"params", prop("object", "Generator opacity")},
         {"at", prop("number", "Timeline start, seconds")},
         {"length", prop("number", "Event duration, seconds")},
         {"track", prop("integer", "Preferred video track index")},
         {"layer_id", prop("string", "Layer ID for keyframe, or $camera/$variables")},
         {"property", prop("string", "Property path, e.g. x, fill, follow.progress")},
         {"time", prop("number", "Keyframe event-local seconds")},
         {"value", prop("any", "Keyframe value")},
         {"easing", prop("any", "Curve name or [x1,y1,x2,y2]")},
         {"binding", prop("string", "Audio binding name (default source)")},
         {"source_event_id", prop("integer", "Straight audio/video source event")},
         {"media_id", prop("integer", "Imported source media")},
         {"waveform_event_id", prop("integer", "Reuse an existing waveform envelope")},
         {"source_in", prop("number", "Audio source offset, seconds")},
         {"duration", prop("number", "Audio source span, seconds")},
         {"bins", prop("integer", "Envelope bins: 64–8192")},
         {"path", prop("string", "Native .json template path")},
         {"overwrite", prop("boolean", "Allow overwriting an exported template")},
         {"layer_ids", arrayProp("string", "Layer IDs for layout or lock; get returns addressable IDs, including legacy scenes")},
         {"operation", prop("string", "left, hcenter, right, top, vcenter, bottom, distribute_h, distribute_v")},
         {"reference", prop("string", "canvas (default), safe or selection")},
         {"locked", prop("boolean", "Protect selected layers (true), or unlock them (false)")},
         {"brand", prop("object", "version:1, name, font and primary/secondary/accent/background/foreground colours")}},
        {"action"}));
    t.append(tool("waveform_graphic",
                  "Draws real source audio as a native waveform. Actions create/get/update/refresh. Supply source_event_id or media_id. "
                  "Audio peaks are decoded once and embedded in the project. source_in and duration are source seconds; length is timeline "
                  "duration. Source-event speed is reflected in progress; reverse, loops and speed ramps are rejected. Shows source audio "
                  "before effects, not a master mix. It does not add audio. params control x,y,width,height,color,played_color,style "
                  "(Bars/Filled envelope/Line),stereo,normalize,progress,scroll,window,amplitude,rate,offset.",
                  {{"action", prop("string", "create, get, update, refresh")},
                   {"event_id", prop("integer", "Waveform event for get/update/refresh")},
                   {"source_event_id", prop("integer", "Existing audio or video event with audio")},
                   {"media_id", prop("integer", "Imported media with audio")},
                   {"source_in", prop("number", "Source offset, seconds")},
                   {"duration", prop("number", "Source span up to 3600 seconds")},
                   {"bins", prop("integer", "Peak envelope resolution: 64–8192 (default 2048)")},
                   {"params", prop("object", "Waveform style settings; numeric settings support keyframes")},
                   {"at", prop("number", "Timeline start, defaults to source event start or cursor")},
                   {"length", prop("number", "Timeline duration, defaults to source event length or source span")},
                   {"track", prop("integer", "Preferred video track index")}},
                  {"action"}));
    t.append(tool("edit_event",
                  "Changes an event: where it starts, how long it is, where in its file it starts, its track, name, mute, opacity / "
                  "volume (gain), speed, fades. Grouped events move with it.",
                  {{QStringLiteral("event_id"), prop("integer", "Event (clip) id")},
                   {QStringLiteral("start"), prop("number", "Timeline position, seconds")},
                   {QStringLiteral("length"), prop("number", "Seconds")},
                   {QStringLiteral("source_in"), prop("number", "Where in the media it starts, seconds")},
                   {QStringLiteral("track"), prop("integer", "Move to this track index (same kind: video / audio / image)")},
                   {QStringLiteral("name"), prop("string", "Label")},
                   {QStringLiteral("muted"), prop("boolean", "Mute")},
                   {QStringLiteral("gain"), prop("number", "Opacity (video) or volume (audio), 0..1 (audio up to 4)")},
                   {QStringLiteral("speed"), prop("number", "Playback rate 0.05..20 (the length follows)")},
                   {QStringLiteral("fade_in"), prop("number", "Fade in, seconds")},
                   {QStringLiteral("fade_out"), prop("number", "Fade out, seconds")}},
                  {QStringLiteral("event_id")}));
    t.append(tool("split", "Cuts events at a time: the ones given, or every event under that time.",
                  {{QStringLiteral("time"), prop("number", "Seconds")},
                   {QStringLiteral("event_ids"), arrayProp("integer", "Events to cut (default: all under the time)")}},
                  {QStringLiteral("time")}));
    t.append(tool("delete_events", "Removes events (with their group).",
                  {{QStringLiteral("event_ids"), arrayProp("integer", "Events")},
                   {QStringLiteral("ripple"), prop("boolean", "Close the gap (later events on those tracks move up)")}},
                  {QStringLiteral("event_ids")}));
    t.append(tool("add_track", "Adds a video, audio or image track.",
                  {{QStringLiteral("type"), enumProp({QStringLiteral("video"), QStringLiteral("audio"), QStringLiteral("image")}, "Kind")},
                   {QStringLiteral("index"), prop("integer", "Where (default: video on top, audio at the bottom)")}},
                  {QStringLiteral("type")}));
    t.append(tool("list_effects",
                  "The effect types: video, audio, transition or generator, with their settings (ids, ranges, choices). "
                  "Search by name or category to keep it short.",
                  {{QStringLiteral("kind"),
                    enumProp({QStringLiteral("video"), QStringLiteral("audio"), QStringLiteral("transition"), QStringLiteral("generator")},
                             "Which list")},
                   {QStringLiteral("search"), prop("string", "Words in the name, category or description")},
                   {QStringLiteral("details"), prop("boolean", "Include every setting (default: when searching)")}},
                  {QStringLiteral("kind")}, true));
    t.append(
        tool("add_effect",
             "Adds an effect at the end of a chain: an event's, a track's, the video output's, the audio master's, the Live "
             "FX, a Live pad's or the Live layer's. Settings as {setting_id: value}; a value can be a number, true/false, a "
             "choice's name, \"#rrggbb\", text, or {\"keyframes\": [[seconds, value, curve?, bezier handles?], ...]} (curve: linear, hold, "
             "smooth, ease_in, ease_out, ease_in_out, bezier with [x1, y1, x2, y2], bounce, elastic; event effects: seconds from the "
             "event's start). Video effects also take mask.shape (Off/Rectangle/Ellipse/Freehand), mask.x/y/w/h (0..1), mask.feather, "
             "mask.invert; a freehand mask mask.points (3-16) and each point mask.p<i>x / mask.p<i>y (-0.5..0.5 across the mask's box). "
             "remove_bg removes the background with AI (model Fast or Quality); a_vst3 runs a VST3 plugin: params plugin (its file, "
             "see list_vst3_plugins) and class (for shells). Returns the effect id.",
             {{QStringLiteral("target"), enumProp(targets, "Which chain")},
              {QStringLiteral("id"), prop("integer", "Event id (event), track index (track) or pad number 1-16 (pad)")},
              {QStringLiteral("effect"), prop("string", "Effect type id from list_effects")},
              {QStringLiteral("params"), prop("object", "Settings to change from the defaults")}},
             {QStringLiteral("target"), QStringLiteral("effect")}));
    t.append(tool("set_effect", "Changes an effect's settings (same values as add_effect) or switches it on / off.",
                  {{QStringLiteral("effect_id"), prop("integer", "From get_project / add_effect")},
                   {QStringLiteral("params"), prop("object", "Settings to change")},
                   {QStringLiteral("enabled"), prop("boolean", "On / off")}},
                  {QStringLiteral("effect_id")}));
    t.append(tool("remove_effect", "Removes an effect.", {{QStringLiteral("effect_id"), prop("integer", "Effect id")}},
                  {QStringLiteral("effect_id")}));
    t.append(tool("add_marker", "Adds a marker, or a region when it has a length.",
                  {{QStringLiteral("time"), prop("number", "Seconds")},
                   {QStringLiteral("name"), prop("string", "Label")},
                   {QStringLiteral("length"), prop("number", "Region length, seconds (0 = a marker)")}},
                  {QStringLiteral("time")}));
    t.append(tool("detect_beats", "Finds the beat of the music on the timeline and puts a marker on every beat or bar. Returns the tempo.",
                  {{QStringLiteral("every"), prop("integer", "Marker every N beats: 1, 2, 4 (a bar), 8, 16 (default 4)")},
                   {QStringLiteral("from"), prop("number", "Seconds (default 0)")},
                   {QStringLiteral("to"), prop("number", "Seconds (default: the end)")}}));
    t.append(tool("get_frame", "The picture at a time (default: the cursor), as an image, with the effects.",
                  {{QStringLiteral("time"), prop("number", "Seconds")},
                   {QStringLiteral("width"), prop("integer", "Image width, 160-1920 (default 640)")}},
                  {}, true));
    t.append(tool("transport", "Moves the cursor, plays or stops the preview.",
                  {{QStringLiteral("action"), enumProp({QStringLiteral("seek"), QStringLiteral("play"), QStringLiteral("stop")}, "What")},
                   {QStringLiteral("time"), prop("number", "seek: seconds")}},
                  {QStringLiteral("action")}));
    t.append(tool("render",
                  "Renders the project (or a part) to a video file in the background; render_status tells how far it is. "
                  "Won't overwrite a file unless overwrite is true.",
                  {{QStringLiteral("path"), prop("string", "Output file, e.g. C:/Users/me/Videos/out.mp4")},
                   {QStringLiteral("preset"), prop("string", "Words from a Render As template name (default: MP4 H.264)")},
                   {QStringLiteral("hardware_encoding"),
                    prop("boolean", "Prefer an available GPU encoder (default true); false preserves the selected template encoder")},
                   {QStringLiteral("from"), prop("number", "Seconds")},
                   {QStringLiteral("to"), prop("number", "Seconds")},
                   {QStringLiteral("overwrite"), prop("boolean", "Replace an existing file")}},
                  {QStringLiteral("path")}));
    t.append(tool("render_status", "How the render started with render is going (progress 0..1, done, error); cancel stops it.",
                  {{QStringLiteral("cancel"), prop("boolean", "Stop it")}}));
    t.append(tool("undo", "Undoes the last change(s), the same as Ctrl+Z.",
                  {{QStringLiteral("steps"), prop("integer", "How many (default 1)")}}));
    t.append(tool("redo", "Redoes undone changes.", {{QStringLiteral("steps"), prop("integer", "How many (default 1)")}}));
    t.append(tool("save_project", "Saves the project (to its file, or to a new .mvp path).",
                  {{QStringLiteral("path"), prop("string", "A .mvp file (default: the project's own file)")}}));
    t.append(tool("collect_project",
                  "Creates a portable project and media folder from the current edit, with progress and a missing-file/dependency report. "
                  "Does not change the open project.",
                  {{"destination", prop("string", "New folder inside an existing parent; existing folders are never overwritten")},
                   {"include_unused_media", prop("boolean", "Include unused Project Media (default true)")},
                   {"include_voice_models", prop("boolean", "Include saved RVC models (default true)")}},
                  {"destination"}));
    t.append(
        tool("captions",
             "Captions on a new Captions track: auto (speech to text with Whisper over the time range), import (an SRT / "
             "WebVTT file) or export (the captions track to SRT / WebVTT). auto needs the model on this computer (Tools > "
             "Captions > Auto Captions downloads them; the offline release has them all).",
             {{QStringLiteral("action"), enumProp({QStringLiteral("auto"), QStringLiteral("import"), QStringLiteral("export")}, "What")},
              {QStringLiteral("path"), prop("string", "import / export: the .srt or .vtt file")},
              {QStringLiteral("model"),
               enumProp({QStringLiteral("tiny"), QStringLiteral("base"), QStringLiteral("small"), QStringLiteral("speakers")},
                        "auto: the speech model (default base; speakers = English, marks who is speaking)")},
              {QStringLiteral("language"), prop("string", "auto: \"auto\" (default), \"en\", \"de\"...")},
              {QStringLiteral("translate"), prop("boolean", "auto: into English")},
              {QStringLiteral("style"),
               enumProp({QStringLiteral("subtitles"), QStringLiteral("kinetic")}, "auto / import: how they look (default subtitles)")},
              {QStringLiteral("max_chars"), prop("integer", "auto: longest caption (default 42)")},
              {QStringLiteral("from"), prop("number", "auto: seconds (default 0)")},
              {QStringLiteral("to"), prop("number", "auto: seconds (default: the end)")}},
             {QStringLiteral("action")}));
    t.append(tool("list_voices", "Lists the local Kokoro voice catalog, licenses and installed/download status. No internet access.", {},
                  {}, true));
    t.append(tool(
        "ai_components",
        "Lists optional Windows AI engines. action install downloads hash-verified dependencies directly from Astral, PyPI and PyTorch "
        "into private app environments. Visible setup progress supports cancellation; failed repairs preserve existing engines.",
        {{QStringLiteral("action"), prop("string", "list (default) or install")},
         {QStringLiteral("component"), prop("string", "speech or rvc")},
         {QStringLiteral("device"), prop("string", "cpu (default) or cuda; CUDA speech uses libraries from CUDA RVC")}}));
    t.append(tool(
        "generate_voice",
        "Starts local AI speech from a captions track. Adds a new audio track as one undo step when finished. "
        "Download voices first in Tools > Captions > Generate Voice. Poll voice_status. Do not edit or switch projects until it finishes.",
        {{QStringLiteral("voice"), prop("string", "Kokoro voice ID from list_voices (default af_heart)")},
         {QStringLiteral("caption_event_id"), prop("integer", "A text event on the captions track (default topmost text track)")},
         {QStringLiteral("from"), prop("number", "Only captions starting at or after this second")},
         {QStringLiteral("to"), prop("number", "Only captions starting before this second")},
         {QStringLiteral("rate"), prop("number", "Speech rate 0.5 to 2 (default 1)")},
         {QStringLiteral("device"),
          enumProp({QStringLiteral("auto"), QStringLiteral("cpu"), QStringLiteral("gpu")},
                   "Speech inference device (default auto, GPU preferred with CPU fallback); RVC supports its own device field")},
         {QStringLiteral("timing"),
          enumProp({QStringLiteral("fit"), QStringLiteral("natural")}, "Fit up to 1.35x faster, or natural duration")},
         {QStringLiteral("group"), prop("boolean", "Join short cues into sentences (default true)")},
         {QStringLiteral("rvc"),
          prop("object",
               "Optional bundled RVC conversion: model (exported .pth), index, pitch, index_rate, protect, speaker_id, f0_method (rmvpe or "
               "pm). No Python or RVC installation needed; shared support models must be downloaded in Generate Voice first.")}}));
    // Additional sentence-level generation options share the existing worker.
    for (int i = 0; i < t.size(); ++i) {
        auto def = t[i].toObject();
        if (def["name"].toString() != "generate_voice")
            continue;
        auto schema = def["inputSchema"].toObject();
        auto props = schema["properties"].toObject();
        props["script"] = prop("string", "Speak this script instead of a captions track");
        props["start"] = prop("number", "Script start seconds");
        props["window"] = prop("number", "Script narration window seconds (default 15)");
        props["replace_event"] = prop("integer", "Regenerate one existing audio narration event; retain earlier takes");
        props["ripple"] = prop("boolean", "Ripple following events when replacement length changes");
        props["chapter_id"] = prop("integer", "Chapter to own generated voice and captions");
        props["align_captions"] = prop("boolean", "Generate captions with actual local Whisper word timestamps");
        props["pronunciations"] = prop("object", "Word to spoken pronunciation dictionary");
        for (const auto& key : {"leading_pause", "trailing_pause", "sentence_pause"})
            props[key] = prop("number", "Real silence in seconds, 0 to 10; inline [pause=0.5] also supported");
        schema["properties"] = props;
        def["inputSchema"] = schema;
        t[i] = def;
    }
    t.append(tool("voice_status",
                  "Progress and result of generate_voice, including timing warnings and output folder. cancel stops the worker.",
                  {{QStringLiteral("cancel"), prop("boolean", "Cancel generation")}}));
    t.append(tool("list_vst3_plugins",
                  "The VST3 audio plugins installed on this computer, with the effects in each (for a_vst3's plugin / class).", {}, {},
                  true));
    t.append(tool("set_project_settings", "The project's picture size and frame rate.",
                  {{QStringLiteral("width"), prop("integer", "Pixels")},
                   {QStringLiteral("height"), prop("integer", "Pixels")},
                   {QStringLiteral("fps"), prop("number", "Frames per second (23.976, 25, 29.97, 30, 50, 59.94, 60...)")}}));
    t.append(tool("story_chapters",
                  "Plan documentary chapters with scripts, footage/music and a narration-free listening section. Duration changes move "
                  "following material together. Times are seconds.",
                  {{"action", enumProp({"list", "add", "adopt", "update", "move", "delete", "bind"}, "Chapter operation")},
                   {"chapter_id", prop("integer", "Existing chapter ID")},
                   {"name", prop("string", "Chapter title")},
                   {"script", prop("string", "Narration script")},
                   {"narration", prop("number", "Narration window seconds")},
                   {"listening", prop("number", "Listening-only seconds")},
                   {"source_media_id", prop("integer", "Optional footage/music from project pool")},
                   {"source_in", prop("number", "Source offset seconds")},
                   {"event_ids", arrayProp("integer", "Clips to assign")},
                   {"direction", prop("integer", "Move -1 up or 1 down")},
                   {"from", prop("number", "Adopt an existing range: start seconds")},
                   {"to", prop("number", "Adopt an existing range: end seconds")}},
                  {"action"}));
    t.append(tool("compound_sequence",
                  "Pack clips and their editable tracks into paired picture/audio containers. get returns an embedded timeline JSON; "
                  "update replaces it as one undo step. Unpack requires an untransformed full-range container.",
                  {{"action", enumProp({"create", "get", "update", "unpack"}, "Sequence operation")},
                   {"event_ids", arrayProp("integer", "Clips to pack; groups included")},
                   {"event_id", prop("integer", "Container event")},
                   {"name", prop("string", "Sequence title")},
                   {"sequence", prop("object", "Embedded timeline JSON from get")},
                   {"ripple", prop("boolean", "Move following material if full-range duration changes (default true)")}},
                  {"action"}));
    t.append(tool(
        "directors_dice",
        "Seeded visual variation on selected footage/images. Keeps clip timing and sound; one undo step. Styles: measured, restless, riot.",
        {{"event_ids", arrayProp("integer", "Selected footage/image events")},
         {"seed", prop("integer", "Repeatable random seed")},
         {"style", enumProp({"measured", "restless", "riot"}, "Visual intensity style")},
         {"intensity", prop("number", "0 to 1 (default .6)")}},
        {"event_ids"}));
    // Live.
    t.append(tool("live_status",
                  "The Live tab: capturing or not, the sources to choose from, the pads (names, effects), which is on, scenes, "
                  "the setlist, the tempo, recording / streaming / Spout / virtual camera, the cue.",
                  {}, {}, true));
    t.append(tool(
        "live_action",
        "Plays the Live tab: start / stop capturing, switch a pad on or off, recall a scene, a setlist song (or the "
        "next / previous), tap the tempo, Set 1, find the tempo again, lock / unlock it, start / stop recording, save "
        "the replay, cue a pad or scene and take it. (Going live on a streaming site is left to the person.)",
        {{QStringLiteral("action"),
          enumProp({QStringLiteral("start"), QStringLiteral("stop"), QStringLiteral("pad"), QStringLiteral("scene"), QStringLiteral("song"),
                    QStringLiteral("next_song"), QStringLiteral("previous_song"), QStringLiteral("tap"), QStringLiteral("set_one"),
                    QStringLiteral("find_tempo"), QStringLiteral("lock_tempo"), QStringLiteral("unlock_tempo"), QStringLiteral("record"),
                    QStringLiteral("stop_recording"), QStringLiteral("save_replay"), QStringLiteral("cue_pad"), QStringLiteral("cue_scene"),
                    QStringLiteral("take"), QStringLiteral("close_cue")},
                   "What")},
         {QStringLiteral("number"), prop("integer", "pad 1-16, scene 1-8, song 1-N")}},
        {QStringLiteral("action")}));
    t.append(tool("set_live_source", "Chooses the Live tab's picture source and / or audio input by (part of) its name (see live_status).",
                  {{QStringLiteral("video"), prop("string", "Camera, screen, window or video name")},
                   {QStringLiteral("audio"), prop("string", "Audio input name, or \"none\"")}}));
    t.append(tool(
        "set_tempo", "Sets the Live tempo (BPM), locked or as a starting point to listen from.",
        {{QStringLiteral("bpm"), prop("number", "Beats per minute")}, {QStringLiteral("lock"), prop("boolean", "Lock it (default true)")}},
        {QStringLiteral("bpm")}));
    t.append(tool("set_pad",
                  "Fills a Live pad: a name and an effect chain (each {\"type\": id, \"params\": {...}}), replacing what was there; "
                  "or just renames it / makes it momentary. Locked pads are left alone.",
                  {{QStringLiteral("pad"), prop("integer", "1-16")},
                   {QStringLiteral("name"), prop("string", "Label")},
                   {QStringLiteral("effects"), arrayProp("object", "The chain, first to last")},
                   {QStringLiteral("momentary"), prop("boolean", "On only while held")}},
                  {QStringLiteral("pad")}));
    t.append(tool("save_scene", "Saves the current Live look (Live FX, pad, layer, overlays) as a scene.",
                  {{QStringLiteral("scene"), prop("integer", "1-8")}, {QStringLiteral("name"), prop("string", "Label")}},
                  {QStringLiteral("scene")}));
    t.append(tool("live_switcher",
                  "The Live switcher: the inputs kept open beside the Source (input 1), and which one is on air. "
                  "inputs sets inputs 2-5 by (part of) their names (see live_status; \"\" = off); cut puts an input on air at "
                  "once, auto with the pads' transition, preview picks the next one.",
                  {{QStringLiteral("inputs"), arrayProp("string", "Inputs 2-5, up to four names")},
                   {QStringLiteral("action"),
                    enumProp({QStringLiteral("cut"), QStringLiteral("auto"), QStringLiteral("preview")}, "What to do with input")},
                   {QStringLiteral("input"), prop("integer", "1-5 (1 = the Source)")}}));
    t.append(tool("get_live_frame", "The Live output as it is now (with effects and overlays), as an image.",
                  {{QStringLiteral("width"), prop("integer", "160-1920 (default 640)")}}, {}, true));
    return t;
}

// One MCP tool call: the arguments and what every tool works on. Each tool is a method, found by
// name in callTool's table.
struct McpServer::ToolCall {
    McpServer& server;
    const QString& name;
    const QJsonObject& a;
    MainWindow* w;
    Project* project;
    const ProjectSettings& ps;
    LiveView* live;
    LivePadPanel* pads;
    void apply(const Timeline& tl, const QString& what)
    {
        project->applyEdit(tl, QStringLiteral("AI: ") + what);
        w->refreshPreview();
    }
    bool has(const char* key) const { return a.contains(QLatin1String(key)); }
    double num(const char* key, double def = 0) const { return a.value(QLatin1String(key)).toDouble(def); }
    int integer(const char* key, int def = 0) const { return a.value(QLatin1String(key)).toInt(def); }
    QString str(const char* key) const { return a.value(QLatin1String(key)).toString(); }

    QJsonObject tool_story_chapters();
    QJsonObject tool_compound_sequence();
    QJsonObject tool_directors_dice();
    QJsonObject tool_get_project();
    QJsonObject tool_import_media();
    QJsonObject tool_add_media_to_timeline();
    QJsonObject tool_add_text();
    QJsonObject tool_audio_finishing();
    QJsonObject tool_graphics_overlay();
    QJsonObject tool_edit_event();
    QJsonObject tool_split();
    QJsonObject tool_delete_events();
    QJsonObject tool_add_track();
    QJsonObject tool_list_effects();
    QJsonObject tool_add_effect();
    QJsonObject tool_set_effect();
    QJsonObject tool_remove_effect();
    QJsonObject tool_add_marker();
    QJsonObject tool_detect_beats();
    QJsonObject tool_get_frame();
    QJsonObject tool_transport();
    QJsonObject tool_render();
    QJsonObject tool_render_status();
    QJsonObject tool_undo();
    QJsonObject tool_collect_project();
    QJsonObject tool_save_project();
    QJsonObject tool_captions();
    QJsonObject tool_list_voices();
    QJsonObject tool_ai_components();
    QJsonObject tool_voice_status();
    QJsonObject tool_generate_voice();
    QJsonObject tool_list_vst3_plugins();
    QJsonObject tool_set_project_settings();
    QJsonObject tool_live_status();
    QJsonObject tool_live_action();
    QJsonObject tool_set_live_source();
    QJsonObject tool_set_tempo();
    QJsonObject tool_set_pad();
    QJsonObject tool_save_scene();
    QJsonObject tool_live_switcher();
    QJsonObject tool_get_live_frame();
};

QJsonObject McpServer::ToolCall::tool_story_chapters()
{
    Timeline tl = project->timeline();
    const QString action = str("action");
    QString error;
    int id = integer("chapter_id");
    if (action == "list")
        return jsonResult(QJsonObject{{"chapters", tl.chapters}});
    if (w->voiceJob()->busy())
        return errorResult("Wait for voice generation to finish.");
    if (action == "add") {
        auto source = project->mediaItem(integer("source_media_id"));
        if (has("source_media_id") && integer("source_media_id") && !source)
            return errorResult("Unknown source media.");
        id = story::addChapter(tl, str("name"), str("script"), us(num("narration")), us(num("listening")), source.get(),
                               us(num("source_in")), ps.fps, &error);
    } else if (action == "adopt")
        id = story::adoptChapter(tl, str("name"), str("script"), ps.fps.snap(us(num("from"))), ps.fps.snap(us(num("to"))),
                                 ps.fps.snap(us(num("narration"))), &error);
    else if (action == "update") {
        QJsonObject old;
        for (auto v : tl.chapters)
            if (v.toObject()["id"].toInt() == id)
                old = v.toObject();
        story::updateChapter(
            tl, id, has("name") ? str("name") : old["name"].toString(), has("script") ? str("script") : old["script"].toString(),
            has("narration") ? ps.fps.snap(us(num("narration"))) : old["narration"].toString().toLongLong(),
            has("listening") ? ps.fps.snap(us(num("listening"))) : old["listening"].toString().toLongLong(), ps.fps, &error);
    } else if (action == "move")
        story::moveChapter(tl, id, integer("direction", 1), &error);
    else if (action == "delete")
        story::removeChapter(tl, id, &error);
    else if (action == "bind") {
        std::set<int> ids;
        for (auto v : a["event_ids"].toArray())
            ids.insert(v.toInt());
        story::bindChapter(tl, id, ids, &error);
    } else
        return errorResult("Unknown chapter action.");
    if (!error.isEmpty())
        return errorResult(error);
    apply(tl, "Story chapter " + action);
    return jsonResult(QJsonObject{{"chapter_id", id}, {"chapters", tl.chapters}});
}

QJsonObject McpServer::ToolCall::tool_compound_sequence()
{
    Timeline tl = project->timeline();
    const QString action = str("action");
    const int id = integer("event_id");
    const auto* clip = tl.findClip(id);
    QString error;
    QJsonArray result;
    if (action == "get") {
        if (!clip || !clip->isSequence())
            return errorResult("Choose a compound sequence event.");
        return jsonResult(QJsonObject{{"sequence", nestedTimelineToJson(*clip->sequence.value)}});
    }
    if (w->voiceJob()->busy())
        return errorResult("Wait for voice generation to finish.");
    if (action == "create") {
        std::set<int> ids;
        for (auto v : a["event_ids"].toArray())
            ids.insert(v.toInt());
        for (int created : story::compound(tl, ids, has("name") ? str("name") : QStringLiteral("Sequence"), &error))
            result.append(created);
    } else if (action == "unpack")
        story::unpack(tl, id, &error);
    else if (action == "update") {
        if (!clip || !clip->isSequence() || !a["sequence"].isObject())
            return errorResult("Provide a container and embedded timeline JSON from get.");
        const Clip container = *clip;
        auto content = nestedTimelineFromJson(a["sequence"].toObject());
        error = timelineProblem(content);
        if (content.duration() <= 0)
            return errorResult("A sequence must contain at least one clip.");
        if (!error.isEmpty())
            return errorResult(error);
        std::function<bool(const Timeline&)> validMedia = [&](const Timeline& t) {
            for (const auto& tr : t.tracks)
                for (const auto& c : tr.clips) {
                    if (c.mediaId && !project->mediaItem(c.mediaId))
                        return false;
                    if (c.isSequence() && !validMedia(*c.sequence.value))
                        return false;
                }
            return true;
        };
        if (!validMedia(content))
            return errorResult("Import sequence media into the parent project first.");
        if (!story::replaceCompound(tl, id, content, a["ripple"].toBool(true), &error))
            return errorResult(error);
        result.append(id);
    } else
        return errorResult("Unknown sequence action.");
    if (!error.isEmpty())
        return errorResult(error);
    apply(tl, "Compound sequence " + action);
    return jsonResult(QJsonObject{{"event_ids", result}});
}

QJsonObject McpServer::ToolCall::tool_directors_dice()
{
    if (w->voiceJob()->busy())
        return errorResult("Wait for voice generation to finish.");
    std::set<int> ids;
    for (auto v : a["event_ids"].toArray())
        ids.insert(v.toInt());
    Timeline tl = project->timeline();
    int style = str("style") == "riot" ? 2 : str("style") == "restless" ? 1 : 0;
    int count = story::directorsDice(tl, ids, unsigned(integer("seed")), style, num("intensity", .6));
    if (!count)
        return errorResult("Choose footage or image events.");
    apply(tl, "Director's Dice");
    return jsonResult(QJsonObject{{"changed", count}, {"seed", integer("seed")}});
}

// ---- Project ----
QJsonObject McpServer::ToolCall::tool_get_project()
{
    const Timeline& tl = project->timeline();
    QJsonArray media;
    for (const auto& [id, m] : project->media()) {
        QJsonObject o{{QStringLiteral("media_id"), id},
                      {QStringLiteral("name"), m->displayName()},
                      {QStringLiteral("path"), m->path},
                      {QStringLiteral("online"), m->info.ok()}};
        if (m->info.ok()) {
            if (!m->origin.isEmpty())
                o[QStringLiteral("origin")] = m->origin;
            o[QStringLiteral("seconds")] = sec(m->info.duration);
            o[QStringLiteral("still")] = m->info.isStill;
            o[QStringLiteral("audio")] = !m->info.audio.empty();
            if (m->info.hasVideo()) {
                const VideoStreamInfo& v = m->info.video.front();
                o[QStringLiteral("width")] = v.displaySize().width();
                o[QStringLiteral("height")] = v.displaySize().height();
                o[QStringLiteral("fps")] = v.frameRate.toDouble();
                o[QStringLiteral("codec")] = v.codecName;
                o[QStringLiteral("proxy")] = !m->info.isStill && proxy::exists(m->path, v.index);
            }
        }
        media.append(o);
    }
    QJsonArray tracks;
    for (int i = 0; i < int(tl.tracks.size()); ++i) {
        const Track& t = tl.tracks[size_t(i)];
        QJsonArray clips;
        for (const Clip& c : t.clips) {
            QJsonObject o{{QStringLiteral("event_id"), c.id},
                          {QStringLiteral("start"), sec(c.start)},
                          {QStringLiteral("length"), sec(c.length)},
                          {QStringLiteral("end"), sec(c.end())}};
            if (c.isSequence()) {
                o["sequence"] = true;
                o["sequence_duration"] = sec(c.sequence.value->duration());
                o["sequence_tracks"] = int(c.sequence.value->tracks.size());
            }
            if (!c.narration.empty())
                o["narration"] = c.narration;
            if (c.isGenerated()) {
                o[QStringLiteral("generator")] = c.generator.type;
                if (c.generator.type == "graphics_overlay" || c.generator.type == "audio_waveform")
                    o["generator_data"] = c.generator.data;
                const auto text = c.generator.params.find(QStringLiteral("text"));
                if (text != c.generator.params.end())
                    o[QStringLiteral("text")] = text->second.text;
            } else {
                o[QStringLiteral("media_id")] = c.mediaId;
                o[QStringLiteral("source_in")] = sec(c.sourceIn);
            }
            if (!c.name.isEmpty())
                o[QStringLiteral("name")] = c.name;
            if (c.muted)
                o[QStringLiteral("muted")] = true;
            if (c.groupId)
                o[QStringLiteral("group")] = c.groupId;
            if (c.speed != 1.0)
                o[QStringLiteral("speed")] = c.speed;
            if (c.reverse)
                o[QStringLiteral("reverse")] = true;
            if (c.gain != 1.0f)
                o[QStringLiteral("gain")] = double(c.gain);
            if (c.fadeIn > 0)
                o[QStringLiteral("fade_in")] = sec(c.fadeIn);
            if (c.fadeOut > 0)
                o[QStringLiteral("fade_out")] = sec(c.fadeOut);
            if (!c.effects.empty())
                o[QStringLiteral("effects")] = chainJson(c.effects);
            clips.append(o);
        }
        QJsonObject o{{QStringLiteral("index"), i},
                      {QStringLiteral("type"), trackTypeName(t.type)},
                      {QStringLiteral("name"), t.name},
                      {QStringLiteral("events"), clips}};
        if (t.muted)
            o[QStringLiteral("muted")] = true;
        if (t.locked)
            o[QStringLiteral("locked")] = true; // the user's: leave its events alone
        if (!t.effects.empty())
            o[QStringLiteral("effects")] = chainJson(t.effects);
        tracks.append(o);
    }
    QJsonArray markers;
    for (const Marker& m : tl.markers) {
        QJsonObject o{{QStringLiteral("time"), sec(m.time)}};
        if (!m.name.isEmpty())
            o[QStringLiteral("name")] = m.name;
        if (m.length > 0)
            o[QStringLiteral("length")] = sec(m.length);
        markers.append(o);
    }
    QJsonObject out{{QStringLiteral("file"), project->filePath()},
                    {QStringLiteral("unsaved_changes"), project->isModified()},
                    {QStringLiteral("width"), ps.width},
                    {QStringLiteral("height"), ps.height},
                    {QStringLiteral("fps"), ps.fps.toDouble()},
                    {QStringLiteral("duration"), sec(tl.duration())},
                    {QStringLiteral("cursor"), sec(w->m_timeline->cursorTime())},
                    {QStringLiteral("media"), media},
                    {QStringLiteral("tracks"), tracks},
                    {QStringLiteral("markers"), markers}};
    out["chapters"] = tl.chapters;
    out["narration_settings"] = tl.narrationSettings;
    if (!tl.outputEffects.empty())
        out[QStringLiteral("output_effects")] = chainJson(tl.outputEffects);
    if (!tl.audioMasterEffects.empty())
        out[QStringLiteral("audio_master_effects")] = chainJson(tl.audioMasterEffects);
    return jsonResult(out);
}

QJsonObject McpServer::ToolCall::tool_import_media()
{
    QJsonArray ids, problems;
    for (const QJsonValue& v : a.value(QStringLiteral("paths")).toArray()) {
        const QString path = QFileInfo(v.toString()).absoluteFilePath();
        int existing = 0;
        for (const auto& [id, m] : project->media())
            if (QFileInfo(m->path).absoluteFilePath().compare(path, Qt::CaseInsensitive) == 0)
                existing = id;
        if (existing) {
            ids.append(existing);
            continue;
        }
        const MediaInfo info = probeMedia(path);
        if (!info.ok()) {
            problems.append(QStringLiteral("%1: %2").arg(path, info.error.isEmpty() ? QStringLiteral("can't open it") : info.error));
            continue;
        }
        const int id = project->addMedia(info);
        ids.append(id);
        if (QSettings().value(QStringLiteral("proxy/auto"), true).toBool() && proxy::recommended(info))
            w->createProxies({id}, true);
    }
    QJsonObject out{{QStringLiteral("media_ids"), ids}};
    if (!problems.isEmpty())
        out[QStringLiteral("problems")] = problems;
    return ids.isEmpty() ? errorResult(QJsonDocument(problems).toJson(QJsonDocument::Compact)) : jsonResult(out);
}

QJsonObject McpServer::ToolCall::tool_add_media_to_timeline()
{
    const auto media = project->mediaItem(integer("media_id"));
    if (!media || !media->info.ok())
        return errorResult(QStringLiteral("no media %1 (or it is offline)").arg(integer("media_id")));
    Timeline tl = project->timeline();
    const TimeUs at = has("at") ? us(std::max(0.0, num("at"))) : w->m_timeline->cursorTime();
    const std::vector<int> ids = ops::insertMedia(tl, *media, at, has("track") ? integer("track") : -1, ps.fps);
    if (ids.empty())
        return errorResult(QStringLiteral("nothing to place"));
    apply(tl, QStringLiteral("Add %1").arg(media->displayName()));
    QJsonArray out;
    for (int id : ids)
        out.append(id);
    return jsonResult({{QStringLiteral("event_ids"), out}});
}

QJsonObject McpServer::ToolCall::tool_add_text()
{
    const QString presetId = has("preset") ? str("preset") : QStringLiteral("title");
    const GeneratorPreset* preset = findGeneratorPreset(presetId);
    if (!preset)
        return errorResult(QStringLiteral("no generator preset \"%1\" (list_effects kind=generator)").arg(presetId));
    Timeline tl = project->timeline();
    const TimeUs at = has("at") ? us(std::max(0.0, num("at"))) : w->m_timeline->cursorTime();
    const int id = ops::insertGenerated(tl, *preset, at, has("track") ? integer("track") : -1, ps.fps);
    Clip* c = tl.findClip(id);
    if (!c)
        return errorResult(QStringLiteral("couldn't add it"));
    if (has("length"))
        c->length = std::max<TimeUs>(us(0.1), us(num("length")));
    if (has("text")) {
        const GeneratorDef* def = findGenerator(c->generator.type);
        if (def && std::any_of(def->params.begin(), def->params.end(), [](const ParamDef& d) { return d.id == QLatin1String("text"); }))
            c->generator.params[QStringLiteral("text")].text = str("text");
    }
    apply(tl, QStringLiteral("Add Text"));
    return jsonResult({{QStringLiteral("event_id"), id}});
}

QJsonObject McpServer::ToolCall::tool_audio_finishing()
{
    auto action = str("action");
    if (action != "analyze" && action != "apply_gain")
        return errorResult("action must be analyze or apply_gain.");
    finishing::Target target{"Custom", num("lufs", -16), num("true_peak", -1.5)};
    if (!std::isfinite(target.lufs) || !std::isfinite(target.truePeak) || target.lufs < -36 || target.lufs > -5 || target.truePeak < -12
        || target.truePeak > 0)
        return errorResult("Use -36 to -5 LUFS and -12 to 0 dBTP.");
    auto measured = project->snapshot();
    auto report = analyzeAudioMix(server.m_window, *measured);
    auto result = report.json(target);
    bool stale = project->snapshot() != measured;
    result["stale"] = stale;
    if (!report.error.isEmpty() || report.cancelled)
        return errorResult(report.cancelled ? "Audio analysis cancelled." : report.error);
    if (action == "apply_gain") {
        if (stale)
            return errorResult("The project changed during analysis. Reanalyze before applying gain.");
        auto tl = project->timeline();
        QString error;
        if (!finishing::applyGain(tl, report, target, &error))
            return errorResult(error);
        apply(tl, "Finish Audio / Safe Master Gain");
        result["applied_gain_db"] = finishing::safeGain(report, target);
        result["reanalyze_required"] = true;
    }
    return jsonResult(result);
}

QJsonObject McpServer::ToolCall::tool_graphics_overlay()
{
    const bool wave = name == "waveform_graphic";
    const QString action = str("action"), type = wave ? "audio_waveform" : "graphics_overlay";
    if (!wave && action == "capabilities") {
        auto info = motion::capabilities();
        info["workflow"] = QJsonArray{"layout", "lock", "brand", "canvas / safe / selection references", "equal-gap distribution"};
        return jsonResult(info);
    }
    if (!wave && action == "validate") {
        if (!a["scene"].isObject())
            return errorResult("scene must be an object");
        auto error = graphics::validateScene(a["scene"].toObject());
        return jsonResult({{"valid", error.isEmpty()}, {"error", error}});
    }
    if (!wave && action == "presets") {
        QJsonArray out;
        for (auto id : graphics::presets())
            out.append(QJsonObject{{"id", id}, {"scene", graphics::scene(id)}});
        return jsonResult({{"presets", out}});
    }
    if (!QStringList{"create", "get", "update", "refresh", "keyframe", "bind_audio", "export_template", "import_template", "layout",
                     "lock", "brand"}
             .contains(action)
        || (wave
            && QStringList{"keyframe", "bind_audio", "export_template", "import_template", "layout", "lock", "brand"}.contains(action))
        || (!wave && action == "refresh"))
        return errorResult("Unsupported graphics action.");
    Timeline tl = project->timeline();
    Clip* c = nullptr;
    int eventId = integer("event_id");
    if (action != "create" && action != "import_template") {
        c = tl.findClip(eventId);
        if (!c || c->generator.type != type)
            return errorResult("Choose an event of the matching graphics generator type.");
        if (!wave)
            c->generator.data = graphics_workflow::prepare(c->generator.data);
        if (action == "get")
            return jsonResult({{"event_id", eventId}, {"data", c->generator.data}, {"generator", generatorToJson(c->generator)}});
    }
    QJsonObject data = c ? c->generator.data : QJsonObject{};
    double at = has("at") ? num("at") : sec(w->m_timeline->cursorTime()), length = has("length") ? num("length") : 10;
    double rate = 1;
    if (!wave && QStringList{"layout", "lock", "brand"}.contains(action)) {
        QString error;
        QJsonObject changed;
        if (action == "brand") {
            if (!a["brand"].isObject())
                return errorResult("brand must be a kit object.");
            changed = graphics_workflow::applyBrand(data, a["brand"].toObject(), &error);
        } else {
            if (!a["layer_ids"].isArray())
                return errorResult("layer_ids must be an array of layer IDs.");
            QStringList ids;
            for (auto id : a["layer_ids"].toArray()) {
                if (!id.isString())
                    return errorResult("Layer IDs must be strings.");
                ids.append(id.toString());
            }
            if (action == "lock") {
                if (!a["locked"].isBool())
                    return errorResult("locked must be true or false.");
                changed = graphics_workflow::setLocked(data, ids, a["locked"].toBool(), &error);
            } else
                changed = graphics_workflow::layout(data, ids, str("operation"),
                                                    has("reference") ? str("reference") : QString("canvas"), num("time"), &error);
        }
        if (changed.isEmpty())
            return errorResult(error);
        c->generator.data = changed;
        apply(tl, "Graphics / " + action);
        return jsonResult({{"event_id", eventId}, {"data", changed}});
    }
    if (!wave && action == "export_template") {
        if (str("path").isEmpty())
            return errorResult("Choose a template path.");
        if (QFileInfo::exists(str("path")) && !a["overwrite"].toBool())
            return errorResult("Template exists; supply overwrite:true.");
        if (auto error = graphics::validateScene(data); !error.isEmpty())
            return errorResult(error);
        QSaveFile file(str("path"));
        if (!file.open(QIODevice::WriteOnly) || file.write(QJsonDocument(data).toJson()) < 0 || !file.commit())
            return errorResult("Could not save graphics template.");
        return jsonResult({{"path", str("path")}});
    }
    if (!wave && action == "import_template") {
        QFile file(str("path"));
        if (!file.open(QIODevice::ReadOnly) || file.size() > 4 * 1024 * 1024)
            return errorResult("Choose a graphics JSON template up to 4 MB.");
        QJsonParseError error;
        auto document = QJsonDocument::fromJson(file.readAll(), &error);
        if (error.error != QJsonParseError::NoError || !document.isObject())
            return errorResult("Invalid graphics JSON template.");
        data = document.object();
    }
    if (!wave && action == "keyframe") {
        if (graphics_workflow::locked(data, str("layer_id")))
            return errorResult("Unlock the layer or its parent before editing keys.");
        QString error;
        data = motion::withKeyframe(data, str("layer_id"), str("property"), num("time"), a["value"],
                                    has("easing") ? a["easing"] : QJsonValue("linear"), &error);
        if (data.isEmpty())
            return errorResult(error);
    }
    if (!wave && action == "bind_audio") {
        QJsonObject envelope;
        double speed = 1, offset = 0;
        if (has("waveform_event_id")) {
            auto source = tl.findClip(integer("waveform_event_id"));
            if (!source || source->generator.type != "audio_waveform")
                return errorResult("Choose a waveform event.");
            envelope = source->generator.data;
            speed = source->generator.params["rate"].value;
            offset = source->generator.params["offset"].value + (sec(c->start) - sec(source->start)) * speed;
        } else {
            int mediaId = integer("media_id"), stream = -1;
            double sourceIn = num("source_in"), duration = num("duration", c ? sec(c->length) : 10);
            if (has("source_event_id")) {
                auto source = tl.findClip(integer("source_event_id"));
                if (!source || source->isGenerated() || source->isSequence() || source->reverse || source->hasRamp()
                    || source->loopLength > 0)
                    return errorResult("Choose a straight file-backed source with audio.");
                mediaId = source->mediaId;
                stream = source->streamIndex;
                speed = source->speed;
                offset = (sec(c->start) - sec(source->start)) * speed + sec(source->sourceIn) - sourceIn;
                if (!has("source_in")) {
                    sourceIn = sec(source->sourceIn);
                    offset = (sec(c->start) - sec(source->start)) * speed;
                }
                if (!has("duration"))
                    duration = sec(source->length) * speed;
            }
            auto media = project->mediaItem(mediaId);
            if (!media || !media->info.hasAudio())
                return errorResult("Import media with audio first.");
            if (!media->info.audioStream(stream))
                stream = media->info.audio.front().index;
            if (!std::isfinite(sourceIn) || !std::isfinite(duration) || sourceIn < 0 || duration <= 0
                || sourceIn + duration > sec(media->info.duration) + .05)
                return errorResult("Audio binding range is outside the source.");
            QString error;
            envelope = graphics::buildWaveform(media->path, stream, sourceIn, duration, integer("bins", 4096), &error, {}, true);
            if (envelope.isEmpty())
                return errorResult(error);
            envelope["source_media_id"] = mediaId;
        }
        if (auto error = graphics::validateWaveform(envelope); !error.isEmpty())
            return errorResult(error);
        envelope["rate"] = speed;
        envelope["offset"] = offset;
        auto audio = data["audio"].toObject();
        audio[has("binding") ? str("binding") : "source"] = envelope;
        data["audio"] = audio;
        data["version"] = 2;
    }
    if (wave) {
        const bool rebuild = action == "create" || action == "refresh" || has("media_id") || has("source_event_id") || has("source_in")
            || has("duration") || has("bins");
        if (rebuild) {
            int mediaId = has("media_id") ? integer("media_id") : data["source_media_id"].toInt();
            double sourceIn = has("source_in") ? num("source_in") : data["source_in"].toDouble(),
                   duration = has("duration") ? num("duration") : data["duration"].toDouble(length);
            int stream = -1;
            if (has("source_event_id")) {
                const Clip* source = tl.findClip(integer("source_event_id"));
                if (!source || source->isGenerated() || source->isSequence())
                    return errorResult("Choose a file-backed event with audio.");
                if (source->reverse || source->hasRamp() || source->loopLength > 0)
                    return errorResult("Use a straight source excerpt; reverse, loops and speed ramps need a rendered audio file.");
                mediaId = source->mediaId;
                stream = source->streamIndex;
                rate = source->speed;
                if (!has("source_in"))
                    sourceIn = sec(source->sourceIn);
                if (!has("duration"))
                    duration = sec(source->length) * source->speed;
                if (!has("at"))
                    at = sec(source->start);
                if (!has("length"))
                    length = sec(source->length);
            }
            auto media = project->mediaItem(mediaId);
            if (!media || !media->info.hasAudio())
                return errorResult("The waveform source must be imported media with audio.");
            if (!media->info.audioStream(stream))
                stream = media->info.audio.front().index;
            if (action == "create" && !has("source_event_id") && !has("duration") && !has("length"))
                duration = std::min(3600., sec(media->info.duration) - sourceIn);
            if (!has("source_event_id") && !has("length") && action == "create")
                length = duration;
            if (!std::isfinite(sourceIn) || !std::isfinite(duration) || sourceIn < 0 || duration <= 0
                || sourceIn + duration > sec(media->info.duration) + .05)
                return errorResult("The waveform range is outside the source audio.");
            QString error;
            data = graphics::buildWaveform(media->path, stream, sourceIn, duration, has("bins") ? integer("bins") : 2048, &error);
            if (data.isEmpty())
                return errorResult(error);
            data["source_media_id"] = mediaId;
            if (has("source_event_id"))
                data["source_event_id"] = integer("source_event_id");
        }
    } else {
        if (has("scene") && action != "keyframe" && action != "bind_audio" && action != "import_template") {
            if (!a["scene"].isObject())
                return errorResult("scene must be an object");
            data = a["scene"].toObject();
        } else if (action == "create" || has("preset")) {
            QString preset = has("preset") ? str("preset") : "broadcast";
            if (!graphics::presets().contains(preset))
                return errorResult("Unknown graphics preset.");
            data = graphics::scene(preset);
        }
    }
    if (QString error = wave ? graphics::validateWaveform(data) : graphics::validateScene(data); !error.isEmpty())
        return errorResult(error);
    if (!std::isfinite(at) || !std::isfinite(length) || at < 0 || length <= 0 || length > 3600)
        return errorResult("Use a nonnegative start and an event duration up to one hour.");
    if (action == "create" || action == "import_template") {
        const auto* preset = findGeneratorPreset(wave ? "audio_waveform" : "graphics_blank");
        auto insertion = *preset;
        insertion.length = us(length);
        eventId = ops::insertGenerated(tl, insertion, us(at), has("track") ? integer("track") : -1, ps.fps);
        c = tl.findClip(eventId);
        if (!c)
            return errorResult("Could not insert graphics event.");
        if (wave)
            c->generator.params["rate"].value = rate;
    }
    c->generator.data = data;
    if (has("length"))
        c->length = ps.fps.snap(us(length));
    if (has("params")) {
        if (!a["params"].isObject())
            return errorResult("params must be an object");
        const auto* def = findGenerator(type);
        const auto settings = a["params"].toObject();
        for (auto it = settings.begin(); it != settings.end(); ++it)
            if (QString error = setParam(c->generator.params, def->params, it.key(), it.value()); !error.isEmpty())
                return errorResult(error);
    }
    apply(tl, wave ? "Edit Audio Waveform Graphic" : "Compose Graphics Overlay");
    return jsonResult({{"event_id", eventId}, {"data", data}});
}

QJsonObject McpServer::ToolCall::tool_edit_event()
{
    Timeline tl = project->timeline();
    int trackIndex = -1;
    Clip* c = tl.findClip(integer("event_id"), &trackIndex);
    if (!c)
        return errorResult(QStringLiteral("no event %1").arg(integer("event_id")));
    const auto media = project->mediaItem(c->mediaId);
    if (has("speed"))
        ops::setClipSpeed(*c, media.get(), std::clamp(num("speed"), 0.05, 20.0), false, ps.fps);
    if (has("source_in"))
        c->sourceIn = std::max<TimeUs>(0, us(num("source_in")));
    if (has("length")) {
        TimeUs len = std::max<TimeUs>(us(0.04), us(num("length")));
        if (media && !c->isGenerated() && !media->info.isStill)
            len = std::min(len, ops::maxClipLength(media.get(), *c));
        c->length = len;
    }
    if (has("name"))
        c->name = str("name");
    if (has("muted"))
        c->muted = a.value(QStringLiteral("muted")).toBool();
    if (has("gain"))
        c->gain = float(std::clamp(num("gain"), 0.0, 4.0));
    if (has("fade_in"))
        c->fadeIn = std::clamp<TimeUs>(us(num("fade_in")), 0, c->length);
    if (has("fade_out"))
        c->fadeOut = std::clamp<TimeUs>(us(num("fade_out")), 0, c->length);
    const int id = c->id;
    if (has("start")) { // its group moves along
        const TimeUs delta = std::max<TimeUs>(0, us(num("start"))) - c->start;
        for (int gid : ops::expandToGroups(tl, {id}))
            if (Clip* g = tl.findClip(gid))
                g->start = std::max<TimeUs>(0, g->start + delta);
    }
    if (has("track")) {
        const int to = integer("track");
        if (to < 0 || to >= int(tl.tracks.size()))
            return errorResult(QStringLiteral("no track %1").arg(to));
        if (tl.tracks[size_t(to)].type != tl.tracks[size_t(trackIndex)].type)
            return errorResult(QStringLiteral("track %1 is a different kind (video / audio / image)").arg(to));
        if (to != trackIndex) {
            auto& from = tl.tracks[size_t(trackIndex)].clips;
            const auto it = std::find_if(from.begin(), from.end(), [&](const Clip& x) { return x.id == id; });
            Clip moved = *it;
            from.erase(it);
            tl.tracks[size_t(to)].clips.push_back(moved);
        }
    }
    tl.sortClips();
    apply(tl, QStringLiteral("Edit Event"));
    const Clip* now = tl.findClip(id);
    return jsonResult(
        {{QStringLiteral("event_id"), id}, {QStringLiteral("start"), sec(now->start)}, {QStringLiteral("length"), sec(now->length)}});
}

QJsonObject McpServer::ToolCall::tool_split()
{
    Timeline tl = project->timeline();
    std::set<int> ids;
    for (const QJsonValue& v : a.value(QStringLiteral("event_ids")).toArray())
        ids.insert(v.toInt());
    const std::vector<int> made = ops::splitClips(tl, ids, us(num("time")));
    if (made.empty())
        return errorResult(QStringLiteral("nothing to cut at %1 s").arg(num("time")));
    apply(tl, QStringLiteral("Split"));
    QJsonArray out;
    for (int id : made)
        out.append(id);
    return jsonResult({{QStringLiteral("new_event_ids"), out}});
}

QJsonObject McpServer::ToolCall::tool_delete_events()
{
    Timeline tl = project->timeline();
    std::set<int> ids;
    for (const QJsonValue& v : a.value(QStringLiteral("event_ids")).toArray())
        if (tl.findClip(v.toInt()))
            ids.insert(v.toInt());
    if (ids.empty())
        return errorResult(QStringLiteral("none of those events exist"));
    ids = ops::expandToGroups(tl, ids);
    if (a.value(QStringLiteral("ripple")).toBool())
        ops::rippleRemoveClips(tl, ids);
    else
        ops::removeClips(tl, ids);
    apply(tl, QStringLiteral("Delete Events"));
    return jsonResult({{QStringLiteral("deleted"), int(ids.size())}});
}

QJsonObject McpServer::ToolCall::tool_add_track()
{
    Timeline tl = project->timeline();
    const QString type = str("type");
    const TrackType trackType = type == QLatin1String("image") ? TrackType::Image
        : type == QLatin1String("audio")                       ? TrackType::Audio
                                                               : TrackType::Video;
    const int index = ops::addTrack(tl, trackType, has("index") ? integer("index") : -1);
    apply(tl, QStringLiteral("Add Track"));
    return jsonResult({{QStringLiteral("index"), index}});
}

QJsonObject McpServer::ToolCall::tool_list_effects()
{
    const QString kind = str("kind");
    const QString search = str("search").trimmed();
    const bool details = has("details") ? a.value(QStringLiteral("details")).toBool() : !search.isEmpty();
    auto matches = [&](const QString& id, const QString& n, const QString& cat, const QString& desc) {
        if (search.isEmpty())
            return true;
        for (const QString& word : search.split(QLatin1Char(' '), Qt::SkipEmptyParts))
            if (!(id + QLatin1Char(' ') + n + QLatin1Char(' ') + cat + QLatin1Char(' ') + desc).contains(word, Qt::CaseInsensitive))
                return false;
        return true;
    };
    QJsonArray out;
    auto add = [&](const QString& id, const QString& n, const QString& cat, const QString& desc, const std::vector<ParamDef>& params) {
        if (!matches(id, n, cat, desc))
            return;
        QJsonObject o{{QStringLiteral("id"), id}, {QStringLiteral("name"), n}};
        if (!cat.isEmpty())
            o[QStringLiteral("category")] = cat;
        if (details) {
            if (!desc.isEmpty())
                o[QStringLiteral("description")] = desc;
            QJsonArray ps2;
            for (const ParamDef& d : params)
                ps2.append(paramDefJson(d));
            o[QStringLiteral("params")] = ps2;
        }
        out.append(o);
    };
    if (kind == QLatin1String("video"))
        for (const EffectDef& d : videoEffects())
            add(d.id, d.name, d.category, d.description, d.params);
    else if (kind == QLatin1String("audio"))
        for (const AudioEffectDef& d : audioEffects())
            add(d.id, d.name, d.category, d.description, d.params);
    else if (kind == QLatin1String("transition"))
        for (const TransitionDef& d : transitions())
            add(d.id, d.name, QString(), d.description, d.params);
    else if (kind == QLatin1String("generator")) {
        for (const GeneratorPreset& p : generatorPresets()) {
            const GeneratorDef* def = findGenerator(p.generator);
            add(p.id, p.name, p.category, def ? def->description : QString(), def ? def->params : std::vector<ParamDef>());
        }
    } else
        return errorResult(QStringLiteral("kind is video, audio, transition or generator"));
    return jsonResult({{QStringLiteral("count"), out.size()}, {QStringLiteral("effects"), out}});
}

QJsonObject McpServer::ToolCall::tool_add_effect()
{
    const QString type = str("effect");
    const auto meta = effectMeta(type);
    if (!meta)
        return errorResult(QStringLiteral("no effect \"%1\" (list_effects)").arg(type));
    Timeline tl = project->timeline();
    const QString target = str("target");
    const int id = integer("id", -1);
    EffectChain* chain = nullptr;
    bool audioChain = false;
    if (target == QLatin1String("event")) {
        int ti = -1;
        Clip* c = tl.findClip(id, &ti);
        if (!c)
            return errorResult(QStringLiteral("no event %1").arg(id));
        chain = &c->effects;
        audioChain = tl.tracks[size_t(ti)].type == TrackType::Audio;
    } else if (target == QLatin1String("track")) {
        if (id < 0 || id >= int(tl.tracks.size()))
            return errorResult(QStringLiteral("no track %1").arg(id));
        chain = &tl.tracks[size_t(id)].effects;
        audioChain = tl.tracks[size_t(id)].type == TrackType::Audio;
    } else if (target == QLatin1String("output")) {
        chain = &tl.outputEffects;
    } else if (target == QLatin1String("audio_master")) {
        chain = &tl.audioMasterEffects;
        audioChain = true;
    } else if (target == QLatin1String("live")) {
        chain = &tl.liveEffects;
    } else if (target == QLatin1String("layer")) {
        chain = &tl.liveLayerEffects;
    } else if (target == QLatin1String("pad")) {
        if (id < 1 || id > kLivePads)
            return errorResult(QStringLiteral("pads are 1-16"));
        if (tl.livePads[size_t(id - 1)].locked)
            return errorResult(QStringLiteral("pad %1 is locked").arg(id));
        chain = &tl.livePads[size_t(id - 1)].effects;
    } else {
        return errorResult(QStringLiteral("target is event, track, output, audio_master, live, pad or layer"));
    }
    if (meta->audio != audioChain)
        return errorResult(
            meta->audio ? QStringLiteral("%1 is an audio effect: put it on an audio event, audio track or the audio master").arg(type)
                        : QStringLiteral("%1 is a video effect: that chain is for sound").arg(type));
    EffectInstance e = makeEffectOfType(type, tl.allocId());
    if (const QString err = setParams(e, a.value(QStringLiteral("params")).toObject()); !err.isEmpty())
        return errorResult(err);
    const int effectId = e.id;
    chain->push_back(std::move(e));
    apply(tl, QStringLiteral("Add %1").arg(meta->name));
    return jsonResult({{QStringLiteral("effect_id"), effectId}, {QStringLiteral("name"), meta->name}});
}

QJsonObject McpServer::ToolCall::tool_set_effect()
{
    Timeline tl = project->timeline();
    EffectInstance* e = findEffect(tl, integer("effect_id", -1));
    if (!e)
        return errorResult(QStringLiteral("no effect %1").arg(integer("effect_id", -1)));
    if (const QString err = setParams(*e, a.value(QStringLiteral("params")).toObject()); !err.isEmpty())
        return errorResult(err);
    if (has("enabled"))
        e->enabled = a.value(QStringLiteral("enabled")).toBool();
    const QJsonObject now = effectJson(*e);
    apply(tl, QStringLiteral("Change Effect"));
    return jsonResult(now);
}

QJsonObject McpServer::ToolCall::tool_remove_effect()
{
    Timeline tl = project->timeline();
    EffectChain* owner = nullptr;
    EffectInstance* e = findEffect(tl, integer("effect_id", -1), &owner);
    if (!e || !owner)
        return errorResult(QStringLiteral("no effect %1").arg(integer("effect_id", -1)));
    owner->erase(owner->begin() + (e - owner->data()));
    apply(tl, QStringLiteral("Remove Effect"));
    return jsonResult({{QStringLiteral("removed"), integer("effect_id")}});
}

QJsonObject McpServer::ToolCall::tool_add_marker()
{
    Timeline tl = project->timeline();
    Marker m;
    m.id = tl.allocId();
    m.time = std::max<TimeUs>(0, us(num("time")));
    m.length = std::max<TimeUs>(0, us(num("length")));
    m.name = str("name");
    tl.markers.push_back(m);
    tl.sortMarkers();
    apply(tl, m.length > 0 ? QStringLiteral("Add Region") : QStringLiteral("Add Marker"));
    return jsonResult({{QStringLiteral("marker_id"), m.id}});
}

QJsonObject McpServer::ToolCall::tool_detect_beats()
{
    const auto snap = project->snapshot();
    const TimeUs from = us(std::max(0.0, num("from"))), to = has("to") ? us(num("to")) : snap->timeline.duration();
    if (to <= from)
        return errorResult(QStringLiteral("the timeline is empty there"));
    BeatSettings bs;
    bs.band = BeatSettings::Full;
    const BeatScan scan = scanBeats(*snap, from, to, {}, bs);
    if (scan.beats.empty())
        return errorResult(QStringLiteral("no beat found (is there music on the timeline?)"));
    Timeline tl = project->timeline();
    const int n = addBeatMarkers(tl, scan.beats, std::clamp(integer("every", 4), 1, 16), true, from, to);
    apply(tl, QStringLiteral("Detect Beats"));
    return jsonResult(
        {{QStringLiteral("bpm"), scan.bpm}, {QStringLiteral("markers"), n}, {QStringLiteral("first_beat"), sec(scan.beats.front())}});
}

QJsonObject McpServer::ToolCall::tool_get_frame()
{
    const TimeUs t = has("time") ? us(num("time")) : w->m_timeline->cursorTime();
    const int width = std::clamp(integer("width", 640), 160, 1920);
    const QSize size(width, std::max(2, int(std::lround(double(width) * ps.height / std::max(1, ps.width)))));
    Compositor renderer(true);
    const QImage img = renderer.render(*project->snapshot(), t, size);
    if (img.isNull())
        return errorResult(QStringLiteral("nothing rendered"));
    return imageResult(img, QStringLiteral("The picture at %1 s (%2x%3)").arg(sec(t), 0, 'f', 2).arg(size.width()).arg(size.height()));
}

QJsonObject McpServer::ToolCall::tool_transport()
{
    const QString action = str("action");
    if (action == QLatin1String("seek")) {
        w->stopPlayback(false);
        w->seekTo(std::max<TimeUs>(0, us(num("time"))));
    } else if (action == QLatin1String("play")) {
        if (!w->m_engine->isPlaying())
            w->pausePlay();
    } else if (action == QLatin1String("stop")) {
        w->stopPlayback(false);
    } else {
        return errorResult(QStringLiteral("action is seek, play or stop"));
    }
    return jsonResult(
        {{QStringLiteral("cursor"), sec(w->m_timeline->cursorTime())}, {QStringLiteral("playing"), w->m_engine->isPlaying()}});
}

QJsonObject McpServer::ToolCall::tool_render()
{
    if (server.m_render && !server.m_render->done)
        return errorResult(QStringLiteral("a render is already running (render_status)"));
    const QString path = QFileInfo(str("path")).absoluteFilePath();
    if (str("path").isEmpty())
        return errorResult(QStringLiteral("give a path"));
    if (QFileInfo::exists(path) && !a.value(QStringLiteral("overwrite")).toBool())
        return errorResult(QStringLiteral("%1 exists (overwrite: true to replace it)").arg(path));
    const QString want = has("preset") ? str("preset") : QStringLiteral("MP4 H.264");
    const ExportPreset* chosen = nullptr;
    for (const ExportPreset& p : exportPresets()) {
        if (!presetAvailable(p))
            continue;
        bool all = true;
        for (const QString& word : want.split(QLatin1Char(' '), Qt::SkipEmptyParts))
            all = all && (p.name + QLatin1Char(' ') + p.category).contains(word, Qt::CaseInsensitive);
        if (all) {
            chosen = &p;
            break;
        }
    }
    if (!chosen)
        return errorResult(QStringLiteral("no Render As template matches \"%1\"").arg(want));
    ExportSettings s = settingsFromPreset(*chosen, ps);
    if (a.value(QStringLiteral("hardware_encoding")).toBool(true))
        s = withGpuEncoder(s);
    s.path = path;
    const auto snap = project->snapshot();
    s.rangeStart = us(std::max(0.0, num("from")));
    s.rangeEnd = has("to") ? us(num("to")) : snap->timeline.duration();
    if (s.rangeEnd <= s.rangeStart)
        return errorResult(QStringLiteral("nothing to render there"));
    server.m_render = std::make_shared<Render>();
    server.m_render->path = path;
    server.m_render->preset = chosen->name;
    if (hw::settings().gpuCompositing)
        server.m_render->surface = createGpuSurface();
    std::shared_ptr<Render> r = server.m_render;
    r->thread = std::thread([r, snap, s] {
        Exporter exporter;
        const bool ok = exporter.run(
            *snap, s,
            [&](double p) {
                r->progress = p;
                return !r->cancel.load() && !r->monitor->cancel.load();
            },
            r->surface.get(), [&](const ExportStatus& value) { r->monitor->update(value); });
        r->error = ok ? QString() : (r->cancel ? QStringLiteral("cancelled") : exporter.error());
        r->ok = ok;
        r->monitor->done = true;
        r->done = true;
    });
    auto* status = new RenderStatusDialog(s, *snap, r->monitor, server.m_window);
    status->setAttribute(Qt::WA_DeleteOnClose);
    status->show();
    auto* completion = new QTimer(status);
    QObject::connect(completion, &QTimer::timeout, status, [r, completion] {
        if (!r->done)
            return;
        completion->stop();
        if (r->ok)
            Profiles::instance().remember({QJsonObject{{"path", r->path}, {"folder", "Exports"}, {"origin", "render"}}});
    });
    completion->start(200);
    return jsonResult({{QStringLiteral("rendering"), path},
                       {QStringLiteral("template"), chosen->name},
                       {QStringLiteral("seconds"), sec(s.rangeEnd - s.rangeStart)}});
}

QJsonObject McpServer::ToolCall::tool_render_status()
{
    if (!server.m_render)
        return jsonResult({{QStringLiteral("running"), false}});
    if (a.value(QStringLiteral("cancel")).toBool())
        server.m_render->cancel = true;
    QJsonObject o{{QStringLiteral("path"), server.m_render->path},
                  {QStringLiteral("template"), server.m_render->preset},
                  {QStringLiteral("progress"), server.m_render->progress.load()},
                  {QStringLiteral("done"), server.m_render->done.load()}};
    o[QStringLiteral("details")] = server.m_render->monitor->json();
    if (server.m_render->done) {
        o[QStringLiteral("ok")] = server.m_render->ok.load();
        if (!server.m_render->error.isEmpty())
            o[QStringLiteral("error")] = server.m_render->error;
    }
    return jsonResult(o);
}

QJsonObject McpServer::ToolCall::tool_undo()
{
    const bool undo = name == QLatin1String("undo");
    int done = 0;
    QStringList what;
    for (int i = 0; i < std::clamp(integer("steps", 1), 1, 100); ++i) {
        QUndoStack* stack = project->undoStack();
        if (undo ? !stack->canUndo() : !stack->canRedo())
            break;
        what << (undo ? stack->undoText() : stack->redoText());
        undo ? stack->undo() : stack->redo();
        ++done;
    }
    w->refreshPreview();
    return jsonResult({{undo ? QStringLiteral("undone") : QStringLiteral("redone"), QJsonArray::fromStringList(what)}});
}

QJsonObject McpServer::ToolCall::tool_collect_project()
{
    if (!a["destination"].isString() || str("destination").trimmed().isEmpty())
        return errorResult("destination must be a new collection folder");
    for (const auto& key : {"include_unused_media", "include_voice_models"})
        if (has(key) && !a[key].isBool())
            return errorResult(QString(key) + " must be boolean");
    const auto result =
        w->collectProjectTo(str("destination"), {a["include_unused_media"].toBool(true), a["include_voice_models"].toBool(true)});
    if (result.cancelled)
        return errorResult("Collection cancelled; no collection folder was published.");
    if (!result.ok)
        return errorResult(result.error);
    return jsonResult({{"project", result.projectPath},
                       {"report", result.reportPath},
                       {"copied_files", result.copiedFiles},
                       {"copied_bytes", double(result.copiedBytes)},
                       {"missing", QJsonArray::fromStringList(result.missing)},
                       {"warnings", QJsonArray::fromStringList(result.warnings)}});
}

QJsonObject McpServer::ToolCall::tool_save_project()
{
    QString path = has("path") ? QFileInfo(str("path")).absoluteFilePath() : project->filePath();
    if (path.isEmpty())
        return errorResult(QStringLiteral("the project has no file yet: give a path (.mvp)"));
    if (!path.endsWith(QLatin1String(".mvp"), Qt::CaseInsensitive))
        return errorResult(QStringLiteral("project files end in .mvp"));
    QString error;
    if (!project->save(path, &error))
        return errorResult(error);
    w->updateTitle();
    return jsonResult({{QStringLiteral("saved"), path}});
}

QJsonObject McpServer::ToolCall::tool_captions()
{
    const QString action = str("action");
    const bool kinetic = str("style") == QLatin1String("kinetic");
    if (action == QLatin1String("auto")) {
        static const std::map<QString, QString> files = {{QStringLiteral("tiny"), QStringLiteral("ggml-tiny.bin")},
                                                         {QStringLiteral("base"), QStringLiteral("ggml-base.bin")},
                                                         {QStringLiteral("small"), QStringLiteral("ggml-small.bin")},
                                                         {QStringLiteral("speakers"), QStringLiteral("ggml-small.en-tdrz.bin")}};
        const auto file = files.find(has("model") ? str("model") : QStringLiteral("base"));
        if (file == files.end())
            return errorResult(QStringLiteral("model: tiny, base, small or speakers"));
        const QString path = MainWindow::whisperModelPath(file->second);
        if (!QFileInfo::exists(path))
            return errorResult(
                QStringLiteral("the %1 model isn't on this computer: download it once with Tools > Captions > Auto Captions")
                    .arg(file->first));
        TranscribeOptions o;
        o.language = has("language") ? str("language") : QStringLiteral("auto");
        o.maxChars = std::clamp(integer("max_chars", 42), 16, 120);
        o.translate = a.value(QStringLiteral("translate")).toBool();
        const TimeUs from = us(std::max(0.0, num("from"))), to = has("to") ? us(num("to")) : project->timeline().duration();
        int added = 0;
        QString error;
        if (to <= from || !w->captionTimeline(path, o, from, to, kinetic, nullptr, &added, &error))
            return errorResult(to <= from ? QStringLiteral("nothing to listen to there") : error);
        return jsonResult(
            {{QStringLiteral("captions"), added}, {QStringLiteral("track"), QStringLiteral("Captions (the top video track)")}});
    }
    const QString path = QFileInfo(str("path")).absoluteFilePath();
    if (str("path").isEmpty())
        return errorResult(QStringLiteral("give a path"));
    if (action == QLatin1String("import")) {
        QFile f(path);
        const std::vector<Caption> caps =
            f.open(QIODevice::ReadOnly) ? parseSubtitles(QString::fromUtf8(f.readAll())) : std::vector<Caption>();
        if (caps.empty())
            return errorResult(QStringLiteral("no subtitles in %1").arg(path));
        w->addCaptions(caps, 0, kinetic);
        return jsonResult({{QStringLiteral("captions"), int(caps.size())}});
    }
    if (action == QLatin1String("export")) {
        const std::vector<Caption> caps = timelineCaptions(project->timeline());
        QFile f(path);
        if (caps.empty())
            return errorResult(QStringLiteral("there are no text events to export"));
        if (!f.open(QIODevice::WriteOnly)
            || f.write((path.endsWith(QLatin1String(".vtt"), Qt::CaseInsensitive) ? writeVtt(caps) : writeSrt(caps)).toUtf8()) < 0)
            return errorResult(QStringLiteral("could not write %1").arg(path));
        return jsonResult({{QStringLiteral("captions"), int(caps.size())}, {QStringLiteral("path"), path}});
    }
    return errorResult(QStringLiteral("action: auto, import or export"));
}

QJsonObject McpServer::ToolCall::tool_list_voices()
{
    QJsonArray list;
    for (const auto& v : localVoiceCatalog())
        list.append(QJsonObject{{"id", v.id},
                                {"name", v.name},
                                {"language", v.language},
                                {"creator", v.creator},
                                {"license", v.license},
                                {"bytes", double(v.asset.size)},
                                {"installed", voiceAssetInstalled(v.asset)}});
    return jsonResult({{"voices", list},
                       {"model_installed", voiceAssetInstalled(kokoroModelAsset())},
                       {"gpu_model_installed", voiceAssetInstalled(kokoroGpuModelAsset())},
                       {"worker_installed", QFileInfo::exists(VoiceJob::workerPath())}});
}

QJsonObject McpServer::ToolCall::tool_ai_components()
{
    const QString action = str("action");
    QString error;
    if (action == "install") {
        if (!w->installAiComponent(str("component"), str("device"), &error))
            return errorResult(error);
    } else if (!action.isEmpty() && action != "list")
        return errorResult("action: list or install");
    const auto catalog = aiRuntimeCatalog();
    QJsonArray components;
    for (const auto& runtime : parseAiRuntimes(catalog)) {
        const QString path = runtime.id == "speech" ? VoiceJob::workerPath() : rvcWorkerPath();
        components.append(QJsonObject{{"id", runtime.id},
                                      {"name", runtime.name},
                                      {"installed", QFileInfo::exists(path)},
                                      {"worker", path},
                                      {"sources", QJsonArray{"Astral Python", "PyPI", "PyTorch"}},
                                      {"devices", QJsonArray{"cpu", "cuda"}}});
    }
    return jsonResult({{"components", components},
                       {"installing", w->m_aiInstallBusy},
                       {"requires_browser_download", false},
                       {"install_folder", aiRuntimeRoot()}});
}

QJsonObject McpServer::ToolCall::tool_voice_status()
{
    if (a.value("cancel").toBool())
        w->voiceJob()->cancel();
    return jsonResult(w->voiceJob()->status());
}

QJsonObject McpServer::ToolCall::tool_generate_voice()
{
    const auto voices = localVoiceCatalog();
    const QString id = has("voice") ? str("voice") : QStringLiteral("af_heart");
    const auto v = std::find_if(voices.begin(), voices.end(), [&](const LocalVoice& voice) { return voice.id == id; });
    if (v == voices.end())
        return errorResult(QStringLiteral("Unknown voice: use list_voices."));
    const int event = integer("caption_event_id");
    if (event) {
        const auto* c = project->timeline().findClip(event);
        if (!c || (c->generator.type != QLatin1String("text") && c->generator.type != QLatin1String("kinetic_text")))
            return errorResult(QStringLiteral("caption_event_id must be a text event."));
    }
    auto caps = timelineCaptions(project->timeline(), event);
    if (has("script") || has("replace_event")) {
        const auto* c = project->timeline().findClip(integer("replace_event"));
        if (has("replace_event") && !c)
            return errorResult("Replacement narration event not found.");
        const TimeUs start = c ? c->start : us(num("start"));
        const TimeUs length = has("window") ? us(num("window")) : c ? c->length : 15 * kUsPerSecond;
        caps = {Caption{start, start + length, has("script") ? str("script") : c->narration["text"].toString(c->name)}};
    }
    std::erase_if(caps, [&](const Caption& c) {
        return double(c.start) / kUsPerSecond < num("from", 0) || (has("to") && double(c.start) / kUsPerSecond >= num("to"));
    });
    QJsonObject options = a;
    if (options.value("align_captions").toBool())
        for (const auto& candidate : {"ggml-base.bin", "ggml-small.bin", "ggml-tiny.bin", "ggml-medium.bin"})
            if (QFileInfo::exists(w->whisperModelPath(candidate))) {
                options["alignment_model"] = w->whisperModelPath(candidate);
                break;
            }
    QString error;
    if (!w->voiceJob()->start(caps, *v, options, false, &error))
        return errorResult(error);
    return jsonResult(w->voiceJob()->status());
}

QJsonObject McpServer::ToolCall::tool_list_vst3_plugins()
{
    QJsonArray list;
    for (const QString& path : vst3InstalledPlugins())
        list.append(QJsonObject{{QStringLiteral("plugin"), path},
                                {QStringLiteral("effects"), QJsonArray::fromStringList(vst3EffectClasses(path))}});
    return jsonResult({{QStringLiteral("plugins"), list}, {QStringLiteral("supported"), vst3Available()}});
}

QJsonObject McpServer::ToolCall::tool_set_project_settings()
{
    ProjectSettings s = ps;
    if (has("width"))
        s.width = std::clamp(integer("width"), 16, 8192) & ~1;
    if (has("height"))
        s.height = std::clamp(integer("height"), 16, 8192) & ~1;
    if (has("fps")) {
        const double f = num("fps");
        static const std::pair<double, FrameRate> ntsc[] = {{23.976, {24000, 1001}}, {29.97, {30000, 1001}}, {59.94, {60000, 1001}}};
        s.fps = FrameRate{int(std::lround(f * 1000)), 1000};
        for (const auto& [v, r] : ntsc)
            if (std::abs(f - v) < 0.01)
                s.fps = r;
        if (std::abs(f - std::round(f)) < 1e-6)
            s.fps = FrameRate{int(std::lround(f)), 1};
    }
    project->setSettings(s);
    w->updatePreviewInfo();
    w->refreshPreview();
    return jsonResult(
        {{QStringLiteral("width"), s.width}, {QStringLiteral("height"), s.height}, {QStringLiteral("fps"), s.fps.toDouble()}});
}

// ---- Live ----
QJsonObject McpServer::ToolCall::tool_live_status()
{
    const Timeline& tl = project->timeline();
    QJsonArray padList, scenes, songs, sources, audio;
    for (int i = 0; i < kLivePads; ++i) {
        const LivePad& p = tl.livePads[size_t(i)];
        QJsonObject o{
            {QStringLiteral("pad"), i + 1}, {QStringLiteral("name"), p.name}, {QStringLiteral("effects"), chainJson(p.effects)}};
        if (p.locked)
            o[QStringLiteral("locked")] = true;
        if (p.momentary)
            o[QStringLiteral("momentary")] = true;
        padList.append(o);
    }
    for (int i = 0; i < kLiveScenes; ++i)
        if (tl.liveScenes[size_t(i)].used)
            scenes.append(QJsonObject{{QStringLiteral("scene"), i + 1}, {QStringLiteral("name"), tl.liveScenes[size_t(i)].name}});
    for (int i = 0; i < int(tl.liveSetlist.size()); ++i) {
        const LiveSong& s = tl.liveSetlist[size_t(i)];
        songs.append(QJsonObject{{QStringLiteral("song"), i + 1},
                                 {QStringLiteral("name"), s.name},
                                 {QStringLiteral("bpm"), s.bpm},
                                 {QStringLiteral("pad_set"), s.padSet}});
    }
    for (int i = 0; i < live->m_source->count(); ++i)
        sources.append(live->m_source->itemText(i));
    for (int i = 0; i < live->m_audio->count(); ++i)
        audio.append(live->m_audio->itemText(i));
    LiveEngine* engine = live->m_engine;
    QJsonObject o{{QStringLiteral("capturing"), engine->isRunning()},
                  {QStringLiteral("video_source"), live->m_source->currentText()},
                  {QStringLiteral("audio_input"), live->m_audio->currentText()},
                  {QStringLiteral("video_sources"), sources},
                  {QStringLiteral("audio_inputs"), audio},
                  {QStringLiteral("active_pad"), live->m_activePad >= 0 ? live->m_activePad + 1 : 0},
                  {QStringLiteral("pads"), padList},
                  {QStringLiteral("live_fx"), chainJson(tl.liveEffects)},
                  {QStringLiteral("scenes"), scenes},
                  {QStringLiteral("setlist"), songs},
                  {QStringLiteral("current_song"), live->m_currentSong + 1},
                  {QStringLiteral("bpm"), pads->bpm()},
                  {QStringLiteral("tempo_locked"), pads->lockBpmButton()->isChecked()},
                  {QStringLiteral("recording"), engine->isRecording()},
                  {QStringLiteral("streaming"), engine->isStreaming()},
                  {QStringLiteral("stream_live"), live->m_streamLive},
                  {QStringLiteral("spout"), live->m_spoutSender},
                  {QStringLiteral("virtual_camera"), live->m_cameraSize.isValid()},
                  {QStringLiteral("pad_set"), pads->currentSet()}};
    if (live->m_cueKind)
        o[QStringLiteral("cue")] = QJsonObject{
            {QStringLiteral("kind"), live->m_cueKind == LiveEngine::CuePad ? QStringLiteral("pad") : QStringLiteral("scene")},
            {QStringLiteral("number"), live->m_cueIndex + 1}};
    return jsonResult(o);
}

QJsonObject McpServer::ToolCall::tool_live_action()
{
    const QString action = str("action");
    const int n = integer("number", 0);
    if (action == QLatin1String("start") || action == QLatin1String("stop")) {
        if (live->m_engine->isRunning() != (action == QLatin1String("start")))
            live->startStop();
    } else if (action == QLatin1String("pad") || action == QLatin1String("cue_pad")) {
        if (n < 1 || n > kLivePads)
            return errorResult(QStringLiteral("pads are 1-16"));
        if (action == QLatin1String("pad"))
            pads->pressPad(n - 1);
        else
            live->cuePad(n - 1);
    } else if (action == QLatin1String("scene") || action == QLatin1String("cue_scene")) {
        if (n < 1 || n > kLiveScenes || !project->timeline().liveScenes[size_t(n - 1)].used)
            return errorResult(QStringLiteral("no scene %1").arg(n));
        if (action == QLatin1String("scene"))
            live->recallScene(n - 1);
        else
            live->cueScene(n - 1);
    } else if (action == QLatin1String("song")) {
        if (n < 1 || n > int(project->timeline().liveSetlist.size()))
            return errorResult(QStringLiteral("no song %1").arg(n));
        live->recallSong(n - 1);
    } else if (action == QLatin1String("next_song") || action == QLatin1String("previous_song")) {
        live->stepSong(action == QLatin1String("next_song") ? 1 : -1);
    } else if (action == QLatin1String("tap")) {
        pads->tap();
    } else if (action == QLatin1String("set_one")) {
        pads->setDownbeat();
    } else if (action == QLatin1String("find_tempo")) {
        pads->findTempoAgain();
    } else if (action == QLatin1String("lock_tempo") || action == QLatin1String("unlock_tempo")) {
        pads->setTempoLocked(action == QLatin1String("lock_tempo"));
    } else if (action == QLatin1String("record") || action == QLatin1String("stop_recording")) {
        if (live->m_engine->isRecording() != (action == QLatin1String("record")))
            live->toggleRecording();
    } else if (action == QLatin1String("save_replay")) {
        live->saveReplay();
    } else if (action == QLatin1String("take")) {
        live->takeCue();
    } else if (action == QLatin1String("close_cue")) {
        live->clearCue();
    } else {
        return errorResult(QStringLiteral("unknown action \"%1\"").arg(action));
    }
    return jsonResult({{QStringLiteral("done"), action},
                       {QStringLiteral("capturing"), live->m_engine->isRunning()},
                       {QStringLiteral("active_pad"), live->m_activePad >= 0 ? live->m_activePad + 1 : 0},
                       {QStringLiteral("status"), live->m_status->text()}});
}

QJsonObject McpServer::ToolCall::tool_set_live_source()
{
    auto pick = [](QComboBox* combo, const QString& want) {
        for (int i = 0; i < combo->count(); ++i)
            if (combo->itemText(i).compare(want, Qt::CaseInsensitive) == 0) {
                combo->setCurrentIndex(i);
                return true;
            }
        for (int i = 0; i < combo->count(); ++i)
            if (combo->itemText(i).contains(want, Qt::CaseInsensitive)) {
                combo->setCurrentIndex(i);
                return true;
            }
        return false;
    };
    if (has("video") && !pick(live->m_source, str("video")))
        return errorResult(QStringLiteral("no video source like \"%1\" (live_status lists them)").arg(str("video")));
    if (has("audio")) {
        if (str("audio").compare(QLatin1String("none"), Qt::CaseInsensitive) == 0)
            live->m_audio->setCurrentIndex(0);
        else if (!pick(live->m_audio, str("audio")))
            return errorResult(QStringLiteral("no audio input like \"%1\"").arg(str("audio")));
    }
    return jsonResult({{QStringLiteral("video_source"), live->m_source->currentText()},
                       {QStringLiteral("audio_input"), live->m_audio->currentText()}});
}

QJsonObject McpServer::ToolCall::tool_set_tempo()
{
    const double bpm = num("bpm");
    if (bpm < 20 || bpm > 400)
        return errorResult(QStringLiteral("a tempo from 20 to 400 BPM"));
    pads->setSongTempo(bpm, has("lock") ? a.value(QStringLiteral("lock")).toBool() : true);
    return jsonResult({{QStringLiteral("bpm"), pads->bpm()}, {QStringLiteral("locked"), pads->lockBpmButton()->isChecked()}});
}

QJsonObject McpServer::ToolCall::tool_set_pad()
{
    const int n = integer("pad");
    if (n < 1 || n > kLivePads)
        return errorResult(QStringLiteral("pads are 1-16"));
    Timeline tl = project->timeline();
    LivePad& p = tl.livePads[size_t(n - 1)];
    if (p.locked)
        return errorResult(QStringLiteral("pad %1 is locked").arg(n));
    if (has("name"))
        p.name = str("name");
    if (has("momentary"))
        p.momentary = a.value(QStringLiteral("momentary")).toBool();
    if (has("effects")) {
        EffectChain chain;
        for (const QJsonValue& v : a.value(QStringLiteral("effects")).toArray()) {
            const QJsonObject fx = v.toObject();
            const QString type = fx[QStringLiteral("type")].toString();
            const auto meta = effectMeta(type);
            if (!meta || meta->audio)
                return errorResult(QStringLiteral("no video effect \"%1\" (list_effects kind=video)").arg(type));
            EffectInstance e = makeEffectOfType(type, tl.allocId());
            if (const QString err = setParams(e, fx[QStringLiteral("params")].toObject()); !err.isEmpty())
                return errorResult(err);
            chain.push_back(std::move(e));
        }
        p.effects = std::move(chain);
    }
    const QJsonArray fx = chainJson(p.effects);
    apply(tl, QStringLiteral("Set Pad %1").arg(n));
    return jsonResult({{QStringLiteral("pad"), n}, {QStringLiteral("effects"), fx}});
}

QJsonObject McpServer::ToolCall::tool_save_scene()
{
    const int n = integer("scene");
    if (n < 1 || n > kLiveScenes)
        return errorResult(QStringLiteral("scenes are 1-8"));
    live->saveScene(n - 1);
    if (has("name")) {
        Timeline tl = project->timeline();
        tl.liveScenes[size_t(n - 1)].name = str("name");
        apply(tl, QStringLiteral("Name Scene"));
    }
    return jsonResult({{QStringLiteral("scene"), n}});
}

QJsonObject McpServer::ToolCall::tool_live_switcher()
{
    if (has("inputs")) {
        const QJsonArray names = a.value(QStringLiteral("inputs")).toArray();
        for (int k = 0; k < LiveEngine::kMaxInputs; ++k) {
            QComboBox* c = live->m_inputs[size_t(k)];
            const QString want = names.at(k).toString();
            int found = 0; // (off)
            for (int i = 1; i < c->count() && !want.isEmpty() && !found; ++i)
                if (c->itemText(i).contains(want, Qt::CaseInsensitive))
                    found = i;
            if (!want.isEmpty() && !found)
                return errorResult(QStringLiteral("no source like \"%1\" (live_status lists them)").arg(want));
            c->setCurrentIndex(found);
        }
        if (live->m_engine->isRunning())
            live->m_engine->setInputs(live->inputSources());
        live->updateSwitcher();
        live->saveSettings();
    }
    if (has("action")) {
        const int input = integer("input", 0) - 1;
        if (input < 0 || input > LiveEngine::kMaxInputs)
            return errorResult(QStringLiteral("input: 1-5"));
        if (str("action") == QLatin1String("preview"))
            live->setPreviewInput(input);
        else
            live->cutTo(input, str("action") == QLatin1String("auto"));
    }
    QJsonArray inputs;
    for (const LiveSource& s : live->inputSources())
        inputs.append(s.name);
    return jsonResult({{QStringLiteral("inputs"), inputs},
                       {QStringLiteral("on_air"), live->programInput() + 1},
                       {QStringLiteral("preview"), live->m_preview + 1}});
}

QJsonObject McpServer::ToolCall::tool_get_live_frame()
{
    const QImage img = live->m_display->image();
    if (img.isNull() || !live->m_engine->isRunning())
        return errorResult(QStringLiteral("the Live tab isn't capturing (live_action start)"));
    const int width = std::clamp(integer("width", 640), 160, 1920);
    return imageResult(img.scaledToWidth(std::min(width, img.width()), Qt::SmoothTransformation),
                       QStringLiteral("The Live output (%1x%2)").arg(img.width()).arg(img.height()));
}

QJsonObject McpServer::callTool(const QString& name, const QJsonObject& a)
{
    using Tool = QJsonObject (ToolCall::*)();
    struct Entry {
        Tool tool;
        bool live; // needs the Live tab
    };
    static const QHash<QString, Entry> tools = {
        {QStringLiteral("story_chapters"), {&ToolCall::tool_story_chapters, false}},
        {QStringLiteral("compound_sequence"), {&ToolCall::tool_compound_sequence, false}},
        {QStringLiteral("directors_dice"), {&ToolCall::tool_directors_dice, false}},
        {QStringLiteral("get_project"), {&ToolCall::tool_get_project, false}},
        {QStringLiteral("import_media"), {&ToolCall::tool_import_media, false}},
        {QStringLiteral("add_media_to_timeline"), {&ToolCall::tool_add_media_to_timeline, false}},
        {QStringLiteral("add_text"), {&ToolCall::tool_add_text, false}},
        {QStringLiteral("audio_finishing"), {&ToolCall::tool_audio_finishing, false}},
        {QStringLiteral("graphics_overlay"), {&ToolCall::tool_graphics_overlay, false}},
        {QStringLiteral("waveform_graphic"), {&ToolCall::tool_graphics_overlay, false}},
        {QStringLiteral("edit_event"), {&ToolCall::tool_edit_event, false}},
        {QStringLiteral("split"), {&ToolCall::tool_split, false}},
        {QStringLiteral("delete_events"), {&ToolCall::tool_delete_events, false}},
        {QStringLiteral("add_track"), {&ToolCall::tool_add_track, false}},
        {QStringLiteral("list_effects"), {&ToolCall::tool_list_effects, false}},
        {QStringLiteral("add_effect"), {&ToolCall::tool_add_effect, false}},
        {QStringLiteral("set_effect"), {&ToolCall::tool_set_effect, false}},
        {QStringLiteral("remove_effect"), {&ToolCall::tool_remove_effect, false}},
        {QStringLiteral("add_marker"), {&ToolCall::tool_add_marker, false}},
        {QStringLiteral("detect_beats"), {&ToolCall::tool_detect_beats, false}},
        {QStringLiteral("get_frame"), {&ToolCall::tool_get_frame, false}},
        {QStringLiteral("transport"), {&ToolCall::tool_transport, false}},
        {QStringLiteral("render"), {&ToolCall::tool_render, false}},
        {QStringLiteral("render_status"), {&ToolCall::tool_render_status, false}},
        {QStringLiteral("undo"), {&ToolCall::tool_undo, false}},
        {QStringLiteral("redo"), {&ToolCall::tool_undo, false}},
        {QStringLiteral("collect_project"), {&ToolCall::tool_collect_project, false}},
        {QStringLiteral("save_project"), {&ToolCall::tool_save_project, false}},
        {QStringLiteral("captions"), {&ToolCall::tool_captions, false}},
        {QStringLiteral("list_voices"), {&ToolCall::tool_list_voices, false}},
        {QStringLiteral("ai_components"), {&ToolCall::tool_ai_components, false}},
        {QStringLiteral("voice_status"), {&ToolCall::tool_voice_status, false}},
        {QStringLiteral("generate_voice"), {&ToolCall::tool_generate_voice, false}},
        {QStringLiteral("list_vst3_plugins"), {&ToolCall::tool_list_vst3_plugins, false}},
        {QStringLiteral("set_project_settings"), {&ToolCall::tool_set_project_settings, false}},
        {QStringLiteral("live_status"), {&ToolCall::tool_live_status, true}},
        {QStringLiteral("live_action"), {&ToolCall::tool_live_action, true}},
        {QStringLiteral("set_live_source"), {&ToolCall::tool_set_live_source, true}},
        {QStringLiteral("set_tempo"), {&ToolCall::tool_set_tempo, true}},
        {QStringLiteral("set_pad"), {&ToolCall::tool_set_pad, true}},
        {QStringLiteral("save_scene"), {&ToolCall::tool_save_scene, true}},
        {QStringLiteral("live_switcher"), {&ToolCall::tool_live_switcher, true}},
        {QStringLiteral("get_live_frame"), {&ToolCall::tool_get_live_frame, true}},
    };
    MainWindow* w = m_window;
    if (w->m_editorTransaction && name != "get_project" && name != "get_frame" && name != "list_effects" && name != "list_generators"
        && name != "render_status" && name != "voice_status" && name != "list_voices")
        return errorResult("Finish or cancel the open sequence/variation editor before editing through MCP.");
    const auto it = tools.constFind(name);
    if (it == tools.cend())
        return errorResult(QStringLiteral("no tool \"%1\"").arg(name));
    LiveView* live = w->m_live;
    if (it->live && !live)
        return errorResult(QStringLiteral("no Live tab"));
    ToolCall call{*this, name, a, w, w->m_project, w->m_project->settings(), live, live ? live->m_pads : nullptr};
    return (call.*(it->tool))();
}

// ---------------------------------------------------------------------------
// stdio bridge
// ---------------------------------------------------------------------------

namespace {

bool readLine(QByteArray& line)
{
    line.clear();
#ifdef Q_OS_WIN
    HANDLE in = GetStdHandle(STD_INPUT_HANDLE);
    char c;
    DWORD got = 0;
    for (;;) {
        if (!ReadFile(in, &c, 1, &got, nullptr) || got == 0)
            return !line.isEmpty();
        if (c == '\n')
            return true;
        if (c != '\r')
            line.append(c);
    }
#else
    int c;
    while ((c = std::getchar()) != EOF) {
        if (c == '\n')
            return true;
        if (c != '\r')
            line.append(char(c));
    }
    return !line.isEmpty();
#endif
}

void writeLine(const QByteArray& data)
{
    const QByteArray out = data + '\n';
#ifdef Q_OS_WIN
    HANDLE h = GetStdHandle(STD_OUTPUT_HANDLE);
    DWORD written = 0;
    WriteFile(h, out.constData(), DWORD(out.size()), &written, nullptr);
    FlushFileBuffers(h);
#else
    std::fwrite(out.constData(), 1, size_t(out.size()), stdout);
    std::fflush(stdout);
#endif
}

// POSTs one message to the running Mutant Video Producer; the body of its answer (empty for
// 202), or nothing with ok false when it can't be reached.
QByteArray post(quint16 port, const QByteArray& body, bool* ok)
{
    *ok = false;
    QTcpSocket s;
    s.connectToHost(QHostAddress::LocalHost, port);
    if (!s.waitForConnected(1500))
        return {};
    s.write("POST /mcp HTTP/1.1\r\nHost: 127.0.0.1:" + QByteArray::number(port)
            + "\r\nContent-Type: application/json\r\nAccept: application/json, text/event-stream\r\nConnection: close\r\nContent-Length: "
            + QByteArray::number(body.size()) + "\r\n\r\n" + body);
    if (!s.waitForBytesWritten(5000))
        return {};
    QByteArray response;
    // Tools can take a while (rendering a frame, detecting beats): wait up to 10 minutes.
    while (s.state() == QAbstractSocket::ConnectedState || s.bytesAvailable()) {
        if (!s.waitForReadyRead(600000) && s.state() != QAbstractSocket::ConnectedState) {
            response += s.readAll();
            break;
        }
        response += s.readAll();
        const int end = int(response.indexOf("\r\n\r\n"));
        if (end > 0) {
            const QList<QByteArray> lines = response.left(end).split('\n');
            int length = 0;
            for (const QByteArray& l : lines)
                if (l.toLower().startsWith("content-length:"))
                    length = l.mid(15).trimmed().toInt();
            if (response.size() >= end + 4 + length) {
                *ok = true;
                return response.mid(end + 4, length);
            }
        }
    }
    return {};
}

} // namespace

int runMcpStdioBridge(int argc, char** argv)
{
    QCoreApplication app(argc, argv);
    QCoreApplication::setOrganizationName(QStringLiteral("Mutant Video Producer"));
    QCoreApplication::setApplicationName(QStringLiteral("Mutant Video Producer"));
    brand::initializeCompatibility();
    const QStringList args = app.arguments();
    quint16 port = quint16(QSettings().value(QStringLiteral("mcp/port"), McpServer::kDefaultPort).toInt());
    if (const int i = args.indexOf(QStringLiteral("--port")); i >= 0 && i + 1 < args.size())
        port = quint16(args[i + 1].toInt());
    QByteArray line;
    while (readLine(line)) {
        if (line.trimmed().isEmpty())
            continue;
        bool ok = false;
        const QByteArray answer = post(port, line, &ok);
        if (ok) {
            if (!answer.isEmpty())
                writeLine(answer);
            continue;
        }
        // Mutant Video Producer isn't running (or AI Control is off): answer what can be
        // answered here, so the client still connects and shows the tools.
        const QJsonObject msg = QJsonDocument::fromJson(line).object();
        if (!msg.contains(QStringLiteral("id")))
            continue;
        const QString method = msg[QStringLiteral("method")].toString();
        QJsonObject r{{QStringLiteral("jsonrpc"), QStringLiteral("2.0")}, {QStringLiteral("id"), msg[QStringLiteral("id")]}};
        const QString why = QStringLiteral("Mutant Video Producer isn't running, or AI Control is off: open Mutant Video Producer and tick "
                                           "Options > Allow AI Control (MCP)");
        if (method == QLatin1String("initialize"))
            r[QStringLiteral("result")] =
                McpServer::initializeResult(msg[QStringLiteral("params")].toObject()[QStringLiteral("protocolVersion")].toString());
        else if (method == QLatin1String("ping"))
            r[QStringLiteral("result")] = QJsonObject();
        else if (method == QLatin1String("tools/list"))
            r[QStringLiteral("result")] = QJsonObject{{QStringLiteral("tools"), McpServer::toolList()}};
        else if (method == QLatin1String("tools/call"))
            r[QStringLiteral("result")] = errorResult(why);
        else
            r[QStringLiteral("error")] = QJsonObject{{QStringLiteral("code"), -32000}, {QStringLiteral("message"), why}};
        writeLine(QJsonDocument(r).toJson(QJsonDocument::Compact));
    }
    return 0;
}
