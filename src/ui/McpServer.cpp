#include "core/FileNames.h"
#include "core/Brand.h"
#include "ui/McpServer.h"
#include "ui/StemJob.h"
#include "core/StemSeparation.h"

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
#include "core/AudioSync.h"
#include "core/Multicam.h"
#include "core/ColorMatch.h"
#include "core/RetroShaders.h"
#include "core/NodeGraph.h"
#include "core/Presets.h"
#include "core/Stabilizer.h"
#include "core/YouTube.h"
#include "ui/DeviceChainView.h"
#include "ui/FxTarget.h"
#include <atomic>
#include <thread>
#include "core/Story.h"
#include "core/PanCrop.h"
#include "core/Cuts.h"
#include "core/Reframe.h"
#include "core/SceneDetect.h"
#include "core/YouTubeUpload.h"
#include "ui/RenderQueue.h"
#include "ui/DeliverView.h"
#include "ui/PreviewWidget.h"
#include "ui/TimelineWidget.h"

#include <QApplication>
#include <QDirIterator>
#include <QRegularExpression>
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

// Where an inserted generated event went: its track, and a note when it couldn't use the one asked for
// (that track had an event covering the time) or a track was added, which moves the indices below it.
QJsonObject placement(const Timeline& tl, int eventId, size_t tracksBefore)
{
    int ti = -1;
    tl.findClip(eventId, &ti);
    QJsonObject r{{QStringLiteral("track"), ti}};
    if (tl.tracks.size() > tracksBefore)
        r[QStringLiteral("note")] = QStringLiteral("a new track was added at index %1 for it (the time was taken); tracks from there down moved "
                                                   "one index lower")
                                        .arg(ti);
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
    for (const auto& [id, value] : e.params) // a freehand mask's points (not listed settings)
        if (id.startsWith(QLatin1String("mask.p")) && id != QLatin1String("mask.points")) {
            ParamDef point;
            point.id = id;
            point.type = ParamType::Number;
            params[id] = paramValueJson(point, value);
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
    // A freehand mask's points (mask.p0x, mask.p0y ... mask.p15y) aren't listed settings: the preview's handles write them.
    static const QRegularExpression pointId(QStringLiteral("^mask\\.p(1[0-5]|[0-9])[xy]$"));
    const auto listed = [&](const QString& name) { return std::any_of(defs.begin(), defs.end(), [&](const ParamDef& d) { return d.id == name; }); };
    if (!listed(id) && listed(QStringLiteral("mask.points")) && pointId.match(id).hasMatch()) {
        ParamDef point;
        point.id = id;
        point.type = ParamType::Number;
        point.min = -1;
        point.max = 1;
        std::vector<ParamDef> withPoint = defs;
        withPoint.push_back(point);
        return setParam(params, withPoint, id, v);
    }
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
                        if (def->choices[c].compare(x.toString(), Qt::CaseInsensitive) == 0
                            || def->choices[c].section(QStringLiteral(" ("), 0, 0).compare(x.toString(), Qt::CaseInsensitive) == 0)
                            found = c; // (also the name without its explanation: "Off" for "Off (everywhere)")
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
    // A chain FFmpeg can't parse would pass the sound through unchanged, silently.
    if (e.type == QLatin1String("a_ffmpeg") && params.contains(QStringLiteral("filter"))
        && !audioFilterSpecValid(e.params[QStringLiteral("filter")].text))
        return QStringLiteral("FFmpeg can't use the filter chain \"%1\" (e.g. \"highpass=f=80,lowpass=f=14000\")")
            .arg(e.params[QStringLiteral("filter")].text);
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
        || walk(tl.liveEffects, QStringLiteral("Live FX")))
        return;
    for (int i = 0; i < kLiveLayers; ++i)
        if (walk(tl.liveLayerEffects[size_t(i)], QStringLiteral("Live layer %1").arg(i + 1)))
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

const char* kInstructions = "Mutant Video Producer is a video editor with an animation studio (cartoon characters with lip sync, "
                            "sets, a speaker camera), voices, captions, audio finishing, a Deliver tab and a Live tab for real-time "
                            "effects. Times are in seconds. To make a whole video (an animated episode, a documentary, a music video), "
                            "first read user_guide page ai-workflow.md: the order of the work, the tools for each step and the checks; "
                            "user_guide lists every other page (animation-scenes.md for scenes). "
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
    QString path, error, preset, smartNote;
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
        // While a dialog of the program's is open (Stabilize, a graphics composition, a render's progress...), it holds
        // its event and commits on OK: an edit from here meanwhile could pull the event from under it. Looking is fine.
        static const QSet<QString> readOnly = [] {
            QSet<QString> out;
            for (const QJsonValue t : toolList())
                if (t[QStringLiteral("annotations")][QStringLiteral("readOnlyHint")].toBool())
                    out.insert(t[QStringLiteral("name")].toString());
            return out;
        }();
        if (QWidget* modal = QApplication::activeModalWidget(); modal && !readOnly.contains(name)) {
            response[QStringLiteral("result")] = errorResult(
                QStringLiteral("a window is open in Mutant Video Producer (\"%1\"): it needs closing first, then try again").arg(modal->windowTitle()));
            return response;
        }
        // Likewise a menu that's open (its actions hold the indices of when it opened) and a drag under way (a timeline
        // drag rebuilds from the timeline as it was at the press, and would undo the edit).
        if ((QApplication::activePopupWidget() || QGuiApplication::mouseButtons() != Qt::NoButton) && !readOnly.contains(name)) {
            response[QStringLiteral("result")] =
                errorResult(QStringLiteral("the user has a menu open or is dragging something in Mutant Video Producer: try again in a moment"));
            return response;
        }
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
              {QStringLiteral("track"), prop("integer", "Video track index (default: the first)")},
              {QStringLiteral("params"), prop("object", "The generator's settings (list_effects kind=generator details=true): font, size, color, "
                                                         "speed, palette... as add_effect params, keyframes included")}}));
    t.append(tool(
        "audio_finishing",
        "Measure the entire final stereo mix after all audio effects using EBU R128 and oversampled true peaks. analyze returns LUFS, LRA, "
        "dBTP, sample peaks, stereo checks, silence and warnings. apply_gain reanalyzes and adds an undoable master gain (in front of a "
        "Master FX Limiter if there is one, up to the loudness target; else at the end,) limited by "
        "true-peak headroom; reanalyze afterward. A cancellable progress window appears. Targets are editable, not platform guarantees.",
        {{"action", prop("string", "analyze or apply_gain")},
         {"lufs", prop("number", "Target -36 to -5 LUFS (default -16)")},
         {"true_peak", prop("number", "Ceiling -12 to 0 dBTP (default -1.5)")}},
        {"action"}));
    t.append(tool(
        "graphics_overlay",
        "Native scene graph and animation engine, for Graphics Overlay events (version 1 scenes: text and graphics) and Animation "
        "events (version 2 scenes; create with a motion_ preset or a v2 scene makes one; every action takes both). Actions "
        "capabilities/presets/validate/create/get/update/keyframe/bind_audio/export_template/import_template/layout/lock/brand. layout "
        "arranges selected layer_ids by operation and reference, preserving animation offsets. lock protects layer edits and descendants. "
        "brand applies a reusable font/palette kit to tagged static brand_role fills and static text fonts; locked/animated styles are "
        "preserved. v1 scenes retain their appearance; v2 adds paths (M/L/Q/C/Z commands), parented groups, symbols/instances, particles, "
        "repeaters, charts, camera, masks, gradients, shadows/glow, trails/motion_blur and character/word/line text_animation. Layer "
        "shading (0-1, light from shading_angle, default 225 = top left), contact_shadow (0-1), blur (fraction of height) and depth (0-1, "
        "blurred by camera.depth_of_field as the camera zooms in); fixed: true keeps a layer (and a group's children) still on the picture when the camera moves (lyrics, captions, logos); scene motion_blur {samples, shutter seconds}. A track binding's "
        "visemes give mouth shapes: {audio:binding, viseme:A|E|O|L|F|M|rest} is 1 while speaking that shape. Characters "
        "(action characters / add_character) come with such mouths; source_track bindings follow later edits of the track. Their "
        "expression (action expression: neutral, angry, sad, surprised, smug) is the scene variable <id>_mood. Sets (action sets / "
        "add_set) are ready backgrounds, replaced by the next add_set. Layer type video {path, source_in, speed, loop, fit: "
        "cover|contain} plays a file inside its box; a layer's effects [{type: video effect id, params}] apply to its own picture "
        "only. camera.speaker {zoom (1.7), window (0.5 s), quiet (0.06)} frames whichever character (a layer whose id matches its "
        "binding) is loudest. patch sets or removes properties by dotted path (operations [{layer_id, property, value | "
        "remove}]) without sending the scene; batch runs many move/expression/keyframe/patch steps as one undo step (both answer "
        "briefly). props / add_prop (layer_id: a character, prop, hand R|L) put a prop in a hand; it moves with the arm. dance keys moves on the beats of the music under the scene (dancers, moves, every). Animate "
        "properties with {keyframes:[[event-local seconds,value,easing?],...],easing,loop,ping_pong}; numeric, colour, discrete and "
        "compatible path morph values work. follow:{path:layer_id,progress,orient,loop} follows curved paths. Scene variables substitute "
        "{{name}} in text; datasets drive charts. Colours are #RRGGBB, or #AARRGGBB with the alpha FIRST (#00000088 is clear: use "
        "opacity for see-through). bind_audio embeds real source envelopes and native detected beats (source_track: "
        "every audio event on a track during the scene, e.g. one binding per character's voice track for lip sync); property "
        "{audio:binding,base,amount,channel:peak|left|right|beat} reacts without adding sound. Read capabilities and "
        "user_guide page animation-scenes.md for limits and examples. All edits are saved and undoable.",
        {{"action",
          prop("string",
               "capabilities, presets, validate, create, get, update, keyframe, bind_audio, export_template, import_template, layout, "
               "lock, brand, move, characters, add_character, save_character, expression, sets, add_set, props, add_prop, patch, batch, dance")},
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
         {"source_track", prop("integer", "Track index: every audio event on it during this scene (no beats)")},
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
         {"brand", prop("object", "version:1, name, font and primary/secondary/accent/background/foreground colours")},
         {"move", prop("string", "move: hop, jump, shake, nod, wave, spin, walk, pop, squash, blink, point, arms up or shrug, on layer_id at time")},
         {"mood", prop("string", "expression: neutral, angry, sad, surprised or smug, for character layer_id from time on")},
         {"operations", arrayProp("object", "patch: [{layer_id ($camera, $variables, $scene too), property (dotted path), value | remove: true}]; "
                                      "batch: [{action: move|expression|keyframe|patch, its other arguments}], in order, as one undo step")},
         {"dancers", arrayProp("string", "dance: the characters' layer IDs; on each beat of the music under the scene each does the next "
                                         "move (from, to: event-local seconds; every: beats, default 1; moves: names, default hop, "
                                         "spin, jump, squash, arms up, shake)")},
         {"moves", arrayProp("string", "dance: the moves, in turn")},
         {"every", prop("integer", "dance: a move every this many beats (default 1)")},
         {"from", prop("number", "dance: event-local seconds to start from (default 0)")},
         {"set", prop("string", "add_set: a name or file from action sets")},
         {"prop", prop("string", "add_prop: a name or file from action props, put in character layer_id's hand")},
         {"hand", prop("string", "add_prop: R (default) or L")},
         {"amount", prop("number", "move: how big (default 1)")},
         {"to", prop("number", "move walk: the x to walk to")},
         {"character", prop("string", "add_character: a name or file from action characters")},
         {"name", prop("string", "add_character: who they are (their binding is the name in lower case); save_character: its library name")},
         {"x", prop("number", "add_character: left edge, 0 to 1")},
         {"ground", prop("number", "add_character: where their feet are, 0 to 1 (default 0.9)")},
         {"colours", prop("object", "add_character: their colour settings, e.g. {jacket: '#d7263d'}")}},
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
                  "volume (gain), speed, fades. Its group moves with it and takes the same speed, trim and reverse.",
                  {{QStringLiteral("event_id"), prop("integer", "Event (clip) id")},
                   {QStringLiteral("start"), prop("number", "Timeline position, seconds")},
                   {QStringLiteral("length"), prop("number", "Seconds")},
                   {QStringLiteral("source_in"), prop("number", "Where in the media it starts, seconds")},
                   {QStringLiteral("track"), prop("integer", "Move to this track index (same kind: video / audio / image)")},
                   {QStringLiteral("transition_in"),
                    prop("object", "Video: the transition at its start (its fade in, or the crossfade with the event it overlaps): "
                                   "{type: a transition id from list_effects kind=transition, params: {setting: value}}")},
                   {QStringLiteral("transition_out"), prop("object", "Video: the transition at its end, as transition_in")},
                   {QStringLiteral("reverse"), prop("boolean", "Play the source span backwards")},
                   {QStringLiteral("frame_blend"), prop("boolean", "Video slow motion: blend neighbouring frames")},
                   {QStringLiteral("optical_flow"), prop("boolean", "Video slow motion: make in-between frames by following the motion "
                                                                     "(the smoothest; slower to preview and render)")},
                   {QStringLiteral("denoise"), prop("number", "Video: noise reduction 0 (off) .. 1 (strong; 0.35 light, 0.65 medium), "
                                                              "for grainy dark or phone footage (slower to preview)")},
                   {QStringLiteral("loop"), prop("boolean", "Repeat the file so the event can run on past its end (false: it stops there; a "
                                                            "length past the end without loop is shortened, with a warning)")},
                   {QStringLiteral("params"), prop("object", "A generated event's settings (as add_text params)")},
                   {QStringLiteral("pan_crop"),
                    prop("object", "Event Pan/Crop settings (mode Fit/Fill/Stretch, centerX, centerY, zoom, rotation, flipH, flipV), "
                                   "values or {keyframes: [[seconds from the event's start, value, curve?], ...]}")},
                   {QStringLiteral("ramp"),
                    enumProp({QStringLiteral("speed_up"), QStringLiteral("slow_down"), QStringLiteral("slow_middle"), QStringLiteral("fast_middle"),
                              QStringLiteral("freeze_middle"), QStringLiteral("none")},
                             "A speed ramp preset (its grouped sound follows); none removes the ramp")},
                   {QStringLiteral("name"), prop("string", "Label")},
                   {QStringLiteral("label"), prop("string", "Colour label on the timeline: red, orange, yellow, green, teal, blue, purple, pink or none")},
                   {QStringLiteral("muted"), prop("boolean", "Mute")},
                   {QStringLiteral("gain"), prop("number", "Opacity (video) or volume (audio), 0..1 (audio up to 4)")},
                   {QStringLiteral("speed"), prop("number", "Playback rate 0.05..20 (the length follows: half speed, twice as long)")},
                   {QStringLiteral("keep_length"), prop("boolean", "With speed: keep the event's length and show more or less of the file instead")},
                   {QStringLiteral("fade_in"), prop("number", "Fade in, seconds")},
                   {QStringLiteral("fade_out"), prop("number", "Fade out, seconds")}},
                  {QStringLiteral("event_id")}));
    t.append(tool("split", "Cuts events at a time: the ones given (with their group), or every event under that time.",
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
              {QStringLiteral("id"), prop("integer", "Event id (event), track index (track), pad number 1-16 (pad) or layer 1-4 (layer, default 1)")},
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
    t.append(tool("remove_markers", "Removes the markers between two times (e.g. the beat markers once a montage is cut); regions "
                                    "too with regions: true.",
                  {{QStringLiteral("from"), prop("number", "Seconds (default 0)")},
                   {QStringLiteral("to"), prop("number", "Seconds (default: the end)")},
                   {QStringLiteral("regions"), prop("boolean", "Also regions (markers with a length; default false)")}}));
    t.append(tool("detect_beats", "Finds the beat of the music on the timeline and puts a marker on every beat or bar. Returns the tempo.",
                  {{QStringLiteral("every"), prop("integer", "Marker every N beats: 1, 2, 4 (a bar), 8, 16 (default 4)")},
                   {QStringLiteral("from"), prop("number", "Seconds (default 0)")},
                   {QStringLiteral("to"), prop("number", "Seconds (default: the end; at most 15 minutes after from a call, see scanned_to)")}}));
    t.append(tool("get_frame", "The picture at a time (default: the cursor), as an image, with the effects.",
                  {{QStringLiteral("time"), prop("number", "Seconds")},
                   {QStringLiteral("width"), prop("integer", "Image width, 160-1920 (default 640)")},
                   {QStringLiteral("save_to"), prop("string", "Also save it to this file (.png or .jpg; File > Export Frame), at the project's size "
                                                              "unless width is given")}},
                  {}, true));
    t.append(tool("transport", "Moves the cursor, plays or stops the preview, or sets the time selection (select: from-to; "
                               "to at or before from clears it). loop (with any action) switches Loop Playback: Play goes round the "
                               "time selection until stopped.",
                  {{QStringLiteral("action"), enumProp({QStringLiteral("seek"), QStringLiteral("play"), QStringLiteral("stop"), QStringLiteral("select")}, "What")},
                   {QStringLiteral("time"), prop("number", "seek: seconds")},
                   {QStringLiteral("from"), prop("number", "select: seconds")},
                   {QStringLiteral("to"), prop("number", "select: seconds")},
                   {QStringLiteral("loop"), prop("boolean", "Loop Playback on or off")}},
                  {QStringLiteral("action")}));
    t.append(tool("render",
                  "Renders the project (or a part) to a video file in the background (or in place onto a Rendered track, or as "
                  "stems); render_status tells how far it is. Won't overwrite a file unless overwrite is true.",
                  {{QStringLiteral("path"), prop("string", "Output file, e.g. C:/Users/me/Videos/out.mp4 (stems: the folder)")},
                   {QStringLiteral("in_place"), prop("boolean", "Render from-to (the picture as it is, without Output FX) to a ProRes file that goes onto "
                                                               "a Rendered track on top when done, so it plays smoothly (Tools > Render Selection to "
                                                               "New Track); no path or preset")},
                   {QStringLiteral("stems"), prop("boolean", "Each audio track with sound to its own 24-bit WAV in the folder path, after its "
                                                            "effects, level, pan and ducking, before Master FX (File > Export Stems)")},
                   {QStringLiteral("preset"), prop("string", "Words from a Render As template name (default: MP4 H.264)")},
                   {QStringLiteral("hardware_encoding"),
                    prop("boolean", "Prefer an available GPU encoder (default true); false preserves the selected template encoder")},
                   {QStringLiteral("from"), prop("number", "Seconds")},
                   {QStringLiteral("to"), prop("number", "Seconds")},
                   {QStringLiteral("overwrite"), prop("boolean", "Replace an existing file")},
                   {QStringLiteral("smart_render"), prop("boolean", "Copy the video where it is plain cuts of files already in the template's "
                                                                    "codec, size and frame rate (MP4/MOV); render_status says whether it did")}}));
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
    t.append(tool("user_guide",
                  "Mutant Video Producer's User Guide and tutorials (Markdown), the same pages as Help > User Guide. Without page: the "
                  "list of pages with their titles. Read ai-workflow.md first to make a whole video (an animated episode, a "
                  "documentary, a music video): the order of the work, the tools for each step and the checks.",
                  {{QStringLiteral("page"), prop("string", "A page from the list, e.g. ai-workflow.md, animation-scenes.md, tutorials/17-animated-short.md")}},
                  {}, true));
    t.append(tool("list_sound_effects",
                  "The built-in sound effects library (whooshes, booms, dings, comedy stings...), all CC0: category, name and file path "
                  "for import_media or add_media_to_timeline.",
                  {{QStringLiteral("search"), prop("string", "Only names containing this")}}, {}, true));
    t.append(tool("generate_dialogue",
                  "Speaks a script of \"Name: line\" lines with a voice per character, each on their own new audio track named after "
                  "them (for lip sync: graphics_overlay bind_audio with source_track), lines in script order, as one undo step. Poll "
                  "voice_status (its dialogue field). Do not edit or switch projects until it finishes.",
                  {{QStringLiteral("script"), prop("string", "One line per speech: Name: what they say. A line (pause 2) adds two seconds of silence "
                                                              "before the next; [radio], [phone], [megaphone], [echo], [hall], [dream] or "
                                                              "[backwards] at the start of a line gives it that sound (not spoken)")},
                   {QStringLiteral("cast"),
                    prop("object", "Per character name: {voice (list_voices id, default af_heart), rate 0.5 to 2, pitch semitones -24 to 24}")},
                   {QStringLiteral("start"), prop("number", "Seconds (default the cursor)")},
                   {QStringLiteral("gap"), prop("number", "Seconds between lines, 0 to 10 (default 0.3)")},
                   {QStringLiteral("align"), prop("boolean", "Time each word with the Auto Captions model for exact mouth shapes (default true when "
                                                             "a model is downloaded)")}},
                  {QStringLiteral("script")}));
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
    t.append(tool("edit_track", "Changes a track: lock it (its events can't be edited, ripples pass it by), mute, solo, rename, set its level, "
                  "or make it a track matte.",
                  {{QStringLiteral("track"), prop("integer", "Track index (see get_project)")},
                   {QStringLiteral("locked"), prop("boolean", "Lock or unlock")},
                   {QStringLiteral("muted"), prop("boolean", "Mute")},
                   {QStringLiteral("solo"), prop("boolean", "Solo")},
                   {QStringLiteral("name"), prop("string", "New name")},
                   {QStringLiteral("level"), prop("number", "Video: opacity 0..1; audio: volume 0..2 (1 = 0 dB)")},
                   {QStringLiteral("matte"), enumProp({QStringLiteral("off"), QStringLiteral("alpha"), QStringLiteral("alpha_inverted"),
                                                       QStringLiteral("luma"), QStringLiteral("luma_inverted")},
                                                      "Video: make this a track matte for the visual track below it (not shown itself; that "
                                                      "track shows where this one is drawn, alpha, or bright, luma), or off")},
                   {QStringLiteral("pan"), prop("number", "Audio: -1 (left) .. 1 (right)")},
                   {QStringLiteral("blend"), prop("string", "Video: compositing mode over the tracks below: Normal, Add, Screen, Multiply, Overlay, "
                                                            "Lighten, Darken or Difference")},
                   {QStringLiteral("duck_under"), prop("integer", "Audio: turn this track down while that track (index) has sound; -1 stops")},
                   {QStringLiteral("duck_amount"), prop("number", "Audio: how far, dB (-60..0, default -12)")}},
                  {QStringLiteral("track")}));
    t.append(tool("new_project", "Starts a new, empty project (File > New). Refuses while there are unsaved changes unless discard is true.",
                  {{QStringLiteral("width"), prop("integer", "Picture width (default 1920)")},
                   {QStringLiteral("height"), prop("integer", "Picture height (default 1080)")},
                   {QStringLiteral("fps"), prop("number", "Frame rate (default 30)")},
                   {QStringLiteral("discard"), prop("boolean", "Drop unsaved changes")}}));
    t.append(tool("open_project", "Opens a .mvp project (File > Open). Refuses while there are unsaved changes unless discard is true. Lists files it can't find.",
                  {{QStringLiteral("path"), prop("string", "The .mvp file")}, {QStringLiteral("discard"), prop("boolean", "Drop unsaved changes")}},
                  {QStringLiteral("path")}));
    t.append(tool("delete_track", "Removes a track and its events, or (empty: true) every track that has no events.",
                  {{QStringLiteral("track"), prop("integer", "Track index (see get_project)")},
                   {QStringLiteral("empty"), prop("boolean", "Remove every empty track instead")}}));
    t.append(tool("freeze_frame", "Holds a video event's picture: at the time, the event is split and its frame there is held for the length (later events on that track move along; with in_place, the hold covers the time after it instead and nothing moves). For an animation scene, key its \"time\" instead (graphics_overlay).",
                  {{QStringLiteral("time"), prop("number", "Seconds, over a video event")},
                   {QStringLiteral("length"), prop("number", "Seconds held (default 2)")},
                   {QStringLiteral("in_place"), prop("boolean", "Hold over the following time, moving nothing later (its sound plays on)")}},
                  {QStringLiteral("time")}));
    t.append(tool("scene_changes",
                  "Finds the cuts between shots inside video events (Tools > Split Selected Events at Scene Changes) and, unless "
                  "split is false, splits them (with their grouped sound) there. Returns the timeline times.",
                  {{QStringLiteral("event_ids"), arrayProp("integer", "Video events")},
                   {QStringLiteral("sensitivity"), prop("number", "0..1 (default 0.5): higher finds subtler cuts")},
                   {QStringLiteral("split"), prop("boolean", "Split at them (default true)")}},
                  {QStringLiteral("event_ids")}));
    t.append(tool("envelope",
                  "Automation lines: a track's volume (0..2, 1 = 0 dB), pan (-1 left .. 1 right) or composite level (video, 0..1), "
                  "in timeline seconds; or an event's velocity (speed ramp: a rate multiplier, -1 backwards .. 10, 1 = as set), in "
                  "seconds from its start (the grouped sound follows). points replaces the line ([seconds, value, curve?]; curve as "
                  "keyframes: linear, hold, smooth, ease_in...); active switches it on / off; clear removes it. Returns the line.",
                  {{QStringLiteral("kind"), enumProp({QStringLiteral("volume"), QStringLiteral("pan"), QStringLiteral("composite"), QStringLiteral("velocity")}, "Which")},
                   {QStringLiteral("track"), prop("integer", "volume / pan / composite: the track index")},
                   {QStringLiteral("event_id"), prop("integer", "velocity: the event")},
                   {QStringLiteral("points"), arrayProp("array", "[[seconds, value, curve?], ...]")},
                   {QStringLiteral("active"), prop("boolean", "On or off (default: on when points are given)")},
                   {QStringLiteral("clear"), prop("boolean", "Remove the line")}},
                  {QStringLiteral("kind")}));
    t.append(tool("group", "Groups events (they move, trim and fade together; Edit > Group) or ungroups them (e.g. to delete a video "
                           "event's sound without its picture).",
                  {{QStringLiteral("action"), enumProp({QStringLiteral("group"), QStringLiteral("ungroup")}, "What")},
                   {QStringLiteral("event_ids"), arrayProp("integer", "Events")}},
                  {QStringLiteral("action"), QStringLiteral("event_ids")}));
    t.append(tool("duplicate_events", "Copies events (with their effects, fades and groups) to a new time on the same tracks (Edit > Copy, "
                                      "Paste at the cursor). Returns the new event ids.",
                  {{QStringLiteral("event_ids"), arrayProp("integer", "Events")},
                   {QStringLiteral("at"), prop("number", "Seconds: where the earliest of them goes (default: the cursor)")}},
                  {QStringLiteral("event_ids")}));
    t.append(tool("paste_attributes",
                  "Edit > Selectively Paste Event Attributes: event from's effects (replacing theirs), Pan/Crop, fades... onto "
                  "event_ids of the same kind (picture or sound). attributes: effects, pan_crop, fades, transitions, level, speed "
                  "(each keeps its length), fixes (stabilize, noise reduction, slow-motion blending), label (default all). One undo step.",
                  {{QStringLiteral("from"), prop("integer", "The event to copy from")},
                   {QStringLiteral("event_ids"), arrayProp("integer", "Events to paste onto")},
                   {QStringLiteral("attributes"), arrayProp("string", "Which (default all)")}},
                  {QStringLiteral("from"), QStringLiteral("event_ids")}));
    t.append(tool("separate_stems",
                  "Right-click > Separate Stems (the Stem Separation engine: Tools > AI Components): start splits audio event "
                  "event_id into voice and background (stems 2) or vocals, drums, bass and other (stems 4), in the background; "
                  "status says how far it is, then the new events (each stem on a track under the event, which is muted). "
                  "best: the fine-tuned model, about 4x slower. cancel stops it.",
                  {{QStringLiteral("action"), enumProp({QStringLiteral("start"), QStringLiteral("status"), QStringLiteral("cancel")}, "What")},
                   {QStringLiteral("event_id"), prop("integer", "start: an audio event from a file")},
                   {QStringLiteral("stems"), prop("integer", "start: 2 or 4 (default 2)")},
                   {QStringLiteral("best"), prop("boolean", "start: the slower, better model (default false)")}},
                  {QStringLiteral("action")}));
    t.append(tool("replace_media",
                  "Right-click > Replace Media: event_ids (and their grouped sound or picture) show media_id instead, keeping "
                  "their place, trims (where the new file is long enough), effects and keyframes. One undo step.",
                  {{QStringLiteral("event_ids"), arrayProp("integer", "Events")}, {QStringLiteral("media_id"), prop("integer", "The new file (import_media first)")}},
                  {QStringLiteral("event_ids"), QStringLiteral("media_id")}));
    t.append(tool("stabilize", "Smooths camera shake out of a video event (right-click > Stabilize), analysing the file the first time. "
                               "smoothing: seconds of shake smoothed (0 off, 0.5 light, 1.5 medium, 3 strong, 8 nearly a tripod).",
                  {{QStringLiteral("event_id"), prop("integer", "A video event")},
                   {QStringLiteral("smoothing"), prop("number", "Seconds, 0..10 (default 1.5)")},
                   {QStringLiteral("zoom"), prop("boolean", "Zoom in to hide the moving edges (default true)")}},
                  {QStringLiteral("event_id")}));
    t.append(tool("beat_montage", "A montage cut to the markers (Tools > Make Beat Montage; put markers on the beat with detect_beats first): a "
                                  "new video track on top with a clip from the video files between the cuts, each from a random part of its file.",
                  {{QStringLiteral("media_ids"), arrayProp("integer", "Video files to use (default: every video file)")},
                   {QStringLiteral("every"), prop("integer", "Cut every n markers (1, 2, 4, 8 or 16; default 4)")},
                   {QStringLiteral("shuffle"), prop("boolean", "Clips in a random order (default true; false: in turn)")},
                   {QStringLiteral("from"), prop("number", "Seconds: only markers from here (default 0)")},
                   {QStringLiteral("to"), prop("number", "Seconds: only markers to here (default: the end)")},
                   {QStringLiteral("seed"), prop("integer", "For the random choices (default 1)")}}));
    t.append(tool("media", "Project Media: relink a missing (or moved) file, remove a file nothing uses, organize one (folder, rating, Mark "
                           "In / Out: the part placed from it), or make proxies (smaller copies that play smoothly; in the background).",
                  {{QStringLiteral("action"), enumProp({QStringLiteral("relink"), QStringLiteral("remove"), QStringLiteral("organize"), QStringLiteral("proxies")}, "What")},
                   {QStringLiteral("media_id"), prop("integer", "relink / remove / organize: the file (see get_project)")},
                   {QStringLiteral("media_ids"), arrayProp("integer", "proxies: the files (default: every heavy one)")},
                   {QStringLiteral("path"), prop("string", "relink: the file where it is now")},
                   {QStringLiteral("folder"), prop("string", "organize: e.g. Footage/Day 1")},
                   {QStringLiteral("rating"), prop("integer", "organize: 0..5 stars")},
                   {QStringLiteral("mark_in"), prop("number", "organize: seconds")},
                   {QStringLiteral("mark_out"), prop("number", "organize: seconds (0: to the end)")},
                   {QStringLiteral("replace"), prop("boolean", "proxies: make them again")}},
                  {QStringLiteral("action")}));
    t.append(tool("track_motion", "Track Motion: an effect's mask follows what is inside it, from a time to the end of the video event, "
                                  "keyframing the mask's position on every frame. The effect needs a mask (add_effect params mask.shape, "
                                  "mask.x / y / w / h) and the event a steady forward speed. On a corner_pin effect (Track Corners): its "
                                  "four corners, put on the corners of a screen or sign in the shot under the event (a lower track), "
                                  "follow them, as keyframes on tlX/tlY, trX/trY, brX/brY, blX/blY.",
                  {{QStringLiteral("event_id"), prop("integer", "The video event (for a corner_pin, the event pinned over the shot)")},
                   {QStringLiteral("effect_id"), prop("integer", "The masked effect on it, or its corner_pin")},
                   {QStringLiteral("from"), prop("number", "Seconds on the timeline (default: the cursor, else the event's start)")}},
                  {QStringLiteral("event_id"), QStringLiteral("effect_id")}));
    t.append(tool("presets", "Effect presets. list: the factory and saved ones (search narrows; audio: the audio ones). apply: a preset onto "
                             "a chain (target and id as add_effect), or a single-effect preset onto that effect (effect_id). save: an effect "
                             "(effect_id) or a whole chain (target, id) as a preset named name. remove: a saved preset.",
                  {{QStringLiteral("action"), enumProp({QStringLiteral("list"), QStringLiteral("apply"), QStringLiteral("save"), QStringLiteral("remove")}, "What")},
                   {QStringLiteral("search"), prop("string", "list: words to look for")},
                   {QStringLiteral("audio"), prop("boolean", "list: the audio presets (default: video)")},
                   {QStringLiteral("preset"), prop("string", "apply / remove: its id (from list)")},
                   {QStringLiteral("target"), enumProp({QStringLiteral("event"), QStringLiteral("track"), QStringLiteral("output"), QStringLiteral("audio_master"),
                                                        QStringLiteral("live"), QStringLiteral("pad"), QStringLiteral("layer")},
                                                       "apply / save: the chain")},
                   {QStringLiteral("id"), prop("integer", "apply / save: event id, track index, pad number or layer number")},
                   {QStringLiteral("effect_id"), prop("integer", "apply / save: one effect")},
                   {QStringLiteral("name"), prop("string", "save: the preset's name")}},
                  {QStringLiteral("action")}));
    t.append(tool("normalize_audio", "Edit > Normalize Audio Events: each audio event's loudest peak to -0.1 dBFS (a Gain effect at the front "
                                     "of its chain).",
                  {{QStringLiteral("event_ids"), arrayProp("integer", "Audio events")}},
                  {QStringLiteral("event_ids")}));
    t.append(tool("youtube_clip", "File > Create Clip from YouTube: downloads a video (or a part of it) with yt-dlp into the YouTube folder, "
                                  "imports it and (unless place is false) puts it on the timeline. Only for videos you may use (your own, "
                                  "Creative Commons, or with permission). Takes as long as the download.",
                  {{QStringLiteral("url"), prop("string", "The video's link")},
                   {QStringLiteral("from"), prop("number", "Seconds: the part's start (default: the start)")},
                   {QStringLiteral("to"), prop("number", "Seconds: the part's end (default: the end)")},
                   {QStringLiteral("quality"), prop("integer", "Up to 1080, 720, 480 or 360 lines; 0: the sound only (default 1080)")},
                   {QStringLiteral("place"), prop("boolean", "Put it on the timeline (default true)")},
                   {QStringLiteral("at"), prop("number", "Seconds: where (default: the cursor)")},
                   {QStringLiteral("browser"), prop("string", "Use this browser's session if YouTube asks to verify: firefox, chrome, edge or brave")}},
                  {QStringLiteral("url")}));
    t.append(tool("node_effect",
                  "Makes video effects from a graph of nodes (Tools > Effect Builder): saved, an effect is in the User Created "
                  "category, for add_effect on any event, track or adjustment layer. nodes: the node types (search narrows; with "
                  "details or a search, each one's inputs and settings). preview: runs graph on the picture at time (default: the "
                  "cursor) and returns the image. save: saves graph as an effect (overwrite replaces one of the same name) and "
                  "returns its effect id. list: the saved effects. get: one's graph (effect: its id or name), to change and save "
                  "again. A graph: {name, description, nodes: [{id, type, params: {setting: value}}], links: [[from, to, input]], "
                  "exposed: [{node, param, name}]}. It needs one node of type output (and source: the picture the effect is on). "
                  "Node ids are yours (whole numbers from 1). A link wires node from's output into input number `input` (from 0, "
                  "in the order nodes lists them) of node to; nothing wired in reads as transparent. Setting values as add_effect's. "
                  "exposed settings become the effect's own, keyframable with set_effect.",
                  {{QStringLiteral("action"), enumProp({QStringLiteral("nodes"), QStringLiteral("preview"), QStringLiteral("save"), QStringLiteral("list"),
                                                        QStringLiteral("get")},
                                                       "What to do")},
                   {QStringLiteral("search"), prop("string", "nodes: words to look for (e.g. blur, key, glitch, text)")},
                   {QStringLiteral("details"), prop("boolean", "nodes: each one's description, inputs and settings")},
                   {QStringLiteral("graph"), prop("object", "preview / save: the graph (see above)")},
                   {QStringLiteral("time"), prop("number", "preview: seconds (default: the cursor)")},
                   {QStringLiteral("width"), prop("integer", "preview: picture width (160..1280, default 640)")},
                   {QStringLiteral("overwrite"), prop("boolean", "save: replace a saved effect of the same name")},
                   {QStringLiteral("effect"), prop("string", "get: the effect's id (user....) or name")}},
                  {QStringLiteral("action")}));
    t.append(tool("match_colour",
                  "Gives video events the colours of the shot at time (Tools > Match Colour to the Cursor Frame): each gets a "
                  "Color Match effect (red, green and blue curves; its amount tones it down) at the end of its FX. time must be "
                  "outside the events. auto: true is Auto Colour instead (Tools > Auto Colour): each event's own black and white "
                  "points and white balance, from it at time (or half way through), as a White Balance and Auto Colour effect.",
                  {{QStringLiteral("event_ids"), arrayProp("integer", "Video events to change")},
                   {QStringLiteral("time"), prop("number", "Seconds: the frame to match (default: the cursor)")},
                   {QStringLiteral("auto"), prop("boolean", "Auto Colour each event from itself instead of matching (default false)")}},
                  {QStringLiteral("event_ids")}));
    t.append(tool("multicam",
                  "Several cameras of the same moment. sync: moves events (with their groups) so their sound lines up with the "
                  "event on the topmost track (Tools > Sync Selected Events by Audio), and says which couldn't be matched. create: the "
                  "video events event_ids (angle 1, 2... in that order) go under a new Multicam track on top (their tracks muted). cut: "
                  "from time on, the Multicam track shows angle (what pressing its number does while playing). switch: the stretch "
                  "under time shows angle. angles: the angles and the cuts. Each is one undo step.",
                  {{QStringLiteral("action"),
                    enumProp({QStringLiteral("sync"), QStringLiteral("create"), QStringLiteral("cut"), QStringLiteral("switch"), QStringLiteral("angles")},
                             "What")},
                   {QStringLiteral("event_ids"), arrayProp("integer", "sync / create: the events")},
                   {QStringLiteral("angle"), prop("integer", "cut / switch: 1-9")},
                   {QStringLiteral("time"), prop("number", "cut / switch: seconds")}},
                  {QStringLiteral("action")}));
    t.append(tool("cut_time",
                  "Takes time out of the whole edit, on every unlocked track at once, closing the gaps (markers follow), or puts it in "
                  "(insert: length seconds of empty time at `at`, what follows moving later): silences "
                  "(Tools > Remove Silences), ranges (seconds), transcribe (Edit by Transcript's words with their times; changes "
                  "nothing) or words (cut the words given by index from a transcribe result, with the pause after each run).",
                  {{QStringLiteral("action"),
                    enumProp({QStringLiteral("silences"), QStringLiteral("ranges"), QStringLiteral("transcribe"), QStringLiteral("words"),
                              QStringLiteral("insert")}, "What")},
                   {QStringLiteral("at"), prop("number", "insert: seconds (default: the cursor)")},
                   {QStringLiteral("length"), prop("number", "insert: seconds of empty time")},
                   {QStringLiteral("from"), prop("number", "silences / transcribe: seconds (default 0)")},
                   {QStringLiteral("to"), prop("number", "silences / transcribe: seconds (default: the end)")},
                   {QStringLiteral("threshold_db"), prop("number", "silences: quieter than this is silence (default -40)")},
                   {QStringLiteral("min_length"), prop("number", "silences: seconds (default 0.7)")},
                   {QStringLiteral("keep"), prop("number", "silences: seconds left either side (default 0.15)")},
                   {QStringLiteral("ranges"), prop("array", "ranges: [[from, to], ...] in seconds")},
                   {QStringLiteral("model"), enumProp({QStringLiteral("tiny"), QStringLiteral("base"), QStringLiteral("small")}, "transcribe: speech model (default base)")},
                   {QStringLiteral("words"), prop("array", "words: the words from transcribe ([{start, end, text}, ...])")},
                   {QStringLiteral("remove"), arrayProp("integer", "words: indexes of the words to cut")}},
                  {QStringLiteral("action")}));
    t.append(tool("auto_reframe",
                  "Tools > Auto Reframe for Vertical: gives video events a Fill Pan/Crop that follows the subject (a person, else "
                  "the movement) for the project's frame. portrait switches a landscape project to 1080x1920 first.",
                  {{QStringLiteral("event_ids"), arrayProp("integer", "Video events")},
                   {QStringLiteral("portrait"), prop("boolean", "Switch a landscape project to 1080x1920 first (default false)")}},
                  {QStringLiteral("event_ids")}));
    t.append(tool("deliver",
                  "The Deliver tab: getting a finished edit out. check (readiness, the checks with what is worth a look, the "
                  "outputs with files and size estimates, the publish pack; measure_loudness scans the mix first), outputs (change "
                  "the outputs: add / remove templates or a pack, tick, trim to a platform limit, folder; files are named after the "
                  "project and template, with the right extension), render (every ticked output through the render queue; "
                  "render_queue status follows it), pack (set the title, description with {chapters} {tracklist} {title} {length} "
                  "{date} placeholders, tags, the thumbnail frame; save writes description, thumbnail and captions to the folder) "
                  "or history (past renders of the project, with their lengths checked).",
                  {{QStringLiteral("action"),
                    enumProp({QStringLiteral("check"), QStringLiteral("outputs"), QStringLiteral("render"), QStringLiteral("pack"), QStringLiteral("history")},
                             "What")},
                   {QStringLiteral("measure_loudness"), prop("boolean", "check: scan the final mix's loudness first (takes a while)")},
                   {QStringLiteral("loudness_target"), prop("integer", "check: 0 online video / speech -16, 1 louder online -14, 2 EBU -23, 3 US -24")},
                   {QStringLiteral("add"), arrayProp("string", "outputs: Render As templates to add (words from their names)")},
                   {QStringLiteral("remove"), arrayProp("string", "outputs: templates to take out (words from their names)")},
                   {QStringLiteral("pack"), enumProp({QStringLiteral("youtube"), QStringLiteral("social"), QStringLiteral("audio"), QStringLiteral("masters"),
                                                      QStringLiteral("share")},
                                                     "outputs: add a pack of templates")},
                   {QStringLiteral("ticked"), arrayProp("string", "outputs: tick only these (words from their names); the rest are unticked")},
                   {QStringLiteral("trim"), prop("object", "outputs: {template words: seconds} renders only the start (0: the whole edit)")},
                   {QStringLiteral("folder"), prop("string", "outputs: the folder the files go in")},
                   {QStringLiteral("title"), prop("string", "pack: the video title (drawn on the thumbnail)")},
                   {QStringLiteral("description"), prop("string", "pack: the description; placeholders are filled in when saved")},
                   {QStringLiteral("tags"), prop("string", "pack: comma-separated tags (YouTube takes 500 characters)")},
                   {QStringLiteral("thumbnail_time"), prop("number", "pack: seconds; that frame becomes the thumbnail")},
                   {QStringLiteral("save"), prop("boolean", "pack: write the description, thumbnail and captions into the folder")}},
                  {QStringLiteral("action")}));
    t.append(tool("youtube_publish",
                  "For the upload: chapters (\"00:00 Name\" lines from story chapters or markers, with warnings when YouTube "
                  "wouldn't show them), track_listing (an audio track's events as \"01. Name - 04:19\") or thumbnail (a 1280x720 "
                  "JPEG of the frame at a time with an optional title, written to path).",
                  {{QStringLiteral("action"), enumProp({QStringLiteral("chapters"), QStringLiteral("track_listing"), QStringLiteral("thumbnail")}, "What")},
                   {QStringLiteral("track"), prop("integer", "track_listing: audio track index")},
                   {QStringLiteral("time"), prop("number", "thumbnail: seconds (default: the cursor)")},
                   {QStringLiteral("title"), prop("string", "thumbnail: words on the picture")},
                   {QStringLiteral("path"), prop("string", "thumbnail: the .jpg or .png to write")}},
                  {QStringLiteral("action")}));
    t.append(tool("render_queue",
                  "File > Render Queue: renders one after another in the background. add queues the open project as it is now "
                  "(or project_file, without opening it); start, stop (cancels the running one); status lists the jobs.",
                  {{QStringLiteral("action"), enumProp({QStringLiteral("add"), QStringLiteral("start"), QStringLiteral("stop"), QStringLiteral("status")}, "What")},
                   {QStringLiteral("path"), prop("string", "add: output file")},
                   {QStringLiteral("preset"), prop("string", "add: words from a Render As template name (default: MP4 H.264)")},
                   {QStringLiteral("hardware_encoding"), prop("boolean", "add: prefer an available GPU encoder (default true)")},
                   {QStringLiteral("from"), prop("number", "add: seconds")},
                   {QStringLiteral("to"), prop("number", "add: seconds")},
                   {QStringLiteral("project_file"), prop("string", "add: another .mvp to render instead of the open project")}},
                  {QStringLiteral("action")}));
    t.append(tool("live_status",
                  "The Live tab: capturing or not, the sources to choose from, the pads (names, effects), which is on, scenes, "
                  "the setlist, the tempo, recording (last_recording: the latest clip's file) / streaming / Spout / virtual camera, the cue.",
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
// The first available Render As template whose name and category contain every word of `want`.
// The first template whose name and category hold every word; one that writes the output's extension wins
// ("MP3" for a .mp3 is the MP3 template, not the DivX AVI with MP3 sound).
static const ExportPreset* matchPreset(const QString& want, const QString& path = {})
{
    const QString suffix = QFileInfo(path).suffix();
    const ExportPreset* first = nullptr;
    for (const ExportPreset& p : exportPresets()) {
        if (!presetAvailable(p))
            continue;
        bool all = true;
        for (const QString& word : want.split(QLatin1Char(' '), Qt::SkipEmptyParts))
            all = all && (p.name + QLatin1Char(' ') + p.category).contains(word, Qt::CaseInsensitive);
        if (!all)
            continue;
        if (suffix.isEmpty() || p.extension.compare(suffix, Qt::CaseInsensitive) == 0)
            return &p;
        if (!first)
            first = &p;
    }
    return first;
}

// Why an AI edit can't go ahead: it changes the events of a track that is locked before and after it (the
// timeline won't let those be selected or edited; unlocking with edit_track is still allowed).
static QString lockedChange(const Timeline& before, const Timeline& after)
{
    for (size_t i = 0; i < before.tracks.size(); ++i) {
        const Track& t = before.tracks[i];
        if (!t.locked)
            continue;
        const Track* now = nullptr;
        for (size_t j = 0; j < after.tracks.size() && !now; ++j)
            if (t.id != 0 ? after.tracks[j].id == t.id : j == i)
                now = &after.tracks[j];
        if (now && now->locked && now->clips != t.clips)
            return QStringLiteral("track %1%2 is locked: unlock it first (edit_track locked false)")
                .arg(i)
                .arg(t.name.isEmpty() ? QString() : QStringLiteral(" (%1)").arg(t.name));
    }
    return {};
}

struct McpServer::ToolCall {
    McpServer& server;
    const QString& name;
    const QJsonObject& a;
    MainWindow* w;
    Project* project;
    const ProjectSettings& ps;
    LiveView* live;
    LivePadPanel* pads;
    QString refusal; // set when apply() wouldn't make an edit; the tool then answers with it
    void apply(const Timeline& tl, const QString& what)
    {
        if (QString why = lockedChange(project->timeline(), tl); !why.isEmpty()) {
            refusal = why;
            return;
        }
        project->applyEdit(tl, QStringLiteral("AI: ") + what);
        w->refreshPreview();
    }
    bool has(const char* key) const { return a.contains(QLatin1String(key)); }
    // The "params" argument into a generated event's settings (as add_effect sets an effect's).
    QString setGeneratorParams(Clip& c) const
    {
        const GeneratorDef* def = findGenerator(c.generator.type);
        if (!def)
            return QStringLiteral("this event isn't generated: params are for text, backgrounds and other generated events");
        if (!a.value(QStringLiteral("params")).isObject())
            return QStringLiteral("params must be an object");
        const QJsonObject settings = a.value(QStringLiteral("params")).toObject();
        for (auto it = settings.begin(); it != settings.end(); ++it)
            if (const QString error = setParam(c.generator.params, def->params, it.key(), it.value()); !error.isEmpty())
                return error;
        return {};
    }
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
    QJsonObject tool_remove_markers();
    QJsonObject tool_detect_beats();
    QJsonObject tool_get_frame();
    QJsonObject tool_transport();
    QJsonObject tool_render();
    QJsonObject tool_render_status();
    QJsonObject tool_undo();
    QJsonObject tool_collect_project();
    QJsonObject tool_save_project();
    QJsonObject tool_captions();
    QJsonObject tool_edit_track();
    QJsonObject tool_freeze_frame();
    QJsonObject tool_new_project();
    QJsonObject tool_open_project();
    QJsonObject tool_delete_track();
    QJsonObject tool_scene_changes();
    QJsonObject tool_match_colour();
    QJsonObject tool_node_effect();
    QJsonObject tool_envelope();
    QJsonObject tool_group();
    QJsonObject tool_duplicate_events();
    QJsonObject tool_paste_attributes();
    QJsonObject tool_replace_media();
    QJsonObject tool_separate_stems();
    QJsonObject tool_stabilize();
    QJsonObject tool_beat_montage();
    QJsonObject tool_media();
    QJsonObject tool_track_motion();
    QJsonObject tool_presets();
    QJsonObject tool_normalize_audio();
    QJsonObject tool_youtube_clip();
    // The chain `target` / `id` name (as add_effect): null with `error` set.
    EffectChain* chainFor(Timeline& tl, bool* audio, QString* error) const;
    QJsonObject tool_cut_time();
    QJsonObject tool_auto_reframe();
    QJsonObject tool_youtube_publish();
    QJsonObject tool_deliver();
    QJsonObject tool_render_queue();
    QJsonObject tool_list_voices();
    QJsonObject tool_ai_components();
    QJsonObject tool_voice_status();
    QJsonObject tool_generate_voice();
    QJsonObject tool_generate_dialogue();
    QJsonObject tool_list_sound_effects();
    QJsonObject tool_user_guide();
    QJsonObject tool_multicam();
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
    const QString styleName = has("style") ? str("style") : QStringLiteral("measured");
    if (styleName != "measured" && styleName != "restless" && styleName != "riot")
        return errorResult(QStringLiteral("style: measured, restless or riot"));
    int style = styleName == "riot" ? 2 : styleName == "restless" ? 1 : 0;
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
                if (graphics::isScene(c.generator.type) || c.generator.type == "audio_waveform")
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
            if (c.label)
                o[QStringLiteral("label")] = clipLabelName(c.label).toLower();
            if (c.groupId)
                o[QStringLiteral("group")] = c.groupId;
            if (c.speed != 1.0)
                o[QStringLiteral("speed")] = c.speed;
            if (c.reverse)
                o[QStringLiteral("reverse")] = true;
            if (c.frameBlend)
                o[QStringLiteral("frame_blend")] = true;
            if (c.opticalFlow)
                o[QStringLiteral("optical_flow")] = true;
            if (c.denoise > 0)
                o[QStringLiteral("denoise")] = c.denoise;
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
        if (t.matte)
            o[QStringLiteral("matte")] = QStringList{QString(), QStringLiteral("alpha"), QStringLiteral("alpha_inverted"), QStringLiteral("luma"),
                                                     QStringLiteral("luma_inverted")}
                                             .value(t.matte);
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
    std::optional<Project::MediaBatch> batch(project); // (the bins and the profile library: once for all the files)
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
    batch.reset();
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
    GeneratorPreset insertion = *preset;
    if (has("length")) // its real length decides whether it would sit inside another event
        insertion.length = std::max<TimeUs>(us(0.1), us(num("length")));
    const size_t tracksBefore = tl.tracks.size();
    const int id = ops::insertGenerated(tl, insertion, at, has("track") ? integer("track") : -1, ps.fps);
    Clip* c = tl.findClip(id);
    if (!c)
        return errorResult(QStringLiteral("couldn't add it"));
    if (has("length"))
        c->length = std::max(ps.fps.frameDuration(), ps.fps.snap(std::max<TimeUs>(us(0.1), us(num("length")))));
    QJsonObject result = placement(tl, id, tracksBefore);
    result[QStringLiteral("event_id")] = id;
    if (has("text") && !str("text").isEmpty()) {
        const GeneratorDef* def = findGenerator(c->generator.type);
        if (def && std::any_of(def->params.begin(), def->params.end(), [](const ParamDef& d) { return d.id == QLatin1String("text"); }))
            c->generator.params[QStringLiteral("text")].text = str("text");
        else
            result[QStringLiteral("note")] = QStringLiteral("this preset has no text setting, so the text wasn't used (graphics_* presets: change "
                                                            "their words with graphics_overlay update)");
    }
    if (has("params"))
        if (const QString error = setGeneratorParams(*c); !error.isEmpty())
            return errorResult(error);
    apply(tl, QStringLiteral("Add Text"));
    return jsonResult(result);
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
        result["before_limiter"] = finishing::masterLimiter(tl) >= 0;
        if (!finishing::applyGain(tl, report, target, &error))
            return errorResult(error);
        apply(tl, "Finish Audio / Safe Master Gain");
        result["applied_gain_db"] = finishing::safeGain(report, target, finishing::masterLimiter(project->timeline()) >= 0);
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
    if (!wave && action == "characters") {
        QJsonArray out;
        for (const auto& c : graphics::characterLibrary())
            out.append(QJsonObject{{"name", c.character["name"]}, {"path", c.path}, {"built_in", c.builtIn}, {"kind", c.character["kind"]},
                                   {"colours", c.character["variables"]}});
        return jsonResult({{"characters", out}});
    }
    if (!wave && action == "props") {
        QJsonArray out;
        for (const auto& c : graphics::propLibrary())
            out.append(QJsonObject{{"name", c.character["name"]}, {"path", c.path}, {"built_in", c.builtIn}});
        return jsonResult({{"props", out}});
    }
    if (!wave && action == "sets") {
        QJsonArray out;
        for (const auto& c : graphics::setLibrary())
            out.append(QJsonObject{{"name", c.character["name"]}, {"path", c.path}, {"built_in", c.builtIn}});
        return jsonResult({{"sets", out}});
    }
    if (!QStringList{"create", "get", "update", "refresh", "keyframe", "bind_audio", "export_template", "import_template", "layout",
                     "lock", "brand", "move", "add_character", "save_character", "expression", "add_set", "add_prop", "patch", "batch", "dance"}
             .contains(action)
        || (wave
            && QStringList{"keyframe", "bind_audio", "export_template", "import_template", "layout", "lock", "brand", "move", "add_character",
                           "save_character", "expression", "add_set", "add_prop", "patch", "batch", "dance"}.contains(action))
        || (!wave && action == "refresh"))
        return errorResult("Unsupported graphics action.");
    Timeline tl = project->timeline();
    Clip* c = nullptr;
    int eventId = integer("event_id");
    size_t tracksBefore = 0; // set when a new event is inserted
    if (action != "create" && action != "import_template") {
        c = tl.findClip(eventId);
        if (!c || (wave ? c->generator.type != type : !graphics::isScene(c->generator.type)))
            return errorResult("Choose an event of the matching graphics generator type.");
        if (!wave)
            c->generator.data = graphics_workflow::prepare(c->generator.data);
        if (action == "get")
            return jsonResult({{"event_id", eventId}, {"data", c->generator.data}, {"generator", generatorToJson(c->generator)}});
    }
    QJsonObject data = c ? c->generator.data : QJsonObject{};
    double at = has("at") ? num("at") : sec(w->m_timeline->cursorTime()), length = has("length") ? num("length") : 10;
    double rate = 1;
    if (!wave && QStringList{"move", "add_character", "save_character", "expression", "add_set", "add_prop", "patch", "batch", "dance"}.contains(action)) {
        QString error;
        QJsonObject changed;
        if (action == "move") {
            changed = motion::withMove(data, str("layer_id"), str("move"), num("time"), num("duration", 0), num("amount", 1),
                                       has("to") ? num("to") : std::nan(""), &error);
        } else if (action == "expression") {
            changed = motion::withExpression(data, str("layer_id"), str("mood"), num("time"), &error);
        } else if (action == "dance") {
            // Every dancer moves on the music's beats under the scene, the moves in turn (a bear swaps arm gestures for a hop).
            const double from = std::max(0.0, has("from") ? num("from") : 0.0), to = has("to") ? num("to") : sec(c->length);
            const auto snap = project->snapshot();
            BeatSettings bs;
            bs.band = BeatSettings::Full;
            const BeatScan scan = scanBeats(*snap, c->start + us(from), c->start + us(std::min(to, sec(c->length))), {}, bs);
            QStringList moves;
            for (auto v : a["moves"].toArray())
                moves << v.toString();
            if (moves.isEmpty())
                moves = {"hop", "spin", "jump", "squash", "arms up", "shake"};
            QStringList dancers;
            for (auto v : a["dancers"].toArray())
                dancers << v.toString();
            if (dancers.isEmpty() || scan.beats.size() < 2)
                return errorResult(dancers.isEmpty() ? "dance: name the dancers (their layer IDs)." : "dance: no beat found under this scene.");
            const int every = std::clamp(integer("every", 1), 1, 16);
            const double beat = 60.0 / std::max(1.0, scan.bpm);
            changed = data;
            int keyed = 0;
            for (size_t b = 0; b < scan.beats.size(); b += size_t(every)) {
                const double at = sec(scan.beats[b] - c->start);
                for (int n = 0; n < dancers.size(); ++n) {
                    const QString move = moves[int((b / size_t(every) + size_t(n)) % size_t(moves.size()))];
                    QString e;
                    QJsonObject next = motion::withMove(changed, dancers[n], move, at, beat * every * 0.6, has("amount") ? num("amount") : 1,
                                                        std::nan(""), &e);
                    if (next.isEmpty()) // (no arms, or not a move for this layer: a hop instead)
                        next = motion::withMove(changed, dancers[n], "hop", at, beat * every * 0.6, 1, std::nan(""), &e);
                    if (next.isEmpty())
                        return errorResult("dance: " + e);
                    changed = next;
                    ++keyed;
                }
            }
            c->generator.data = changed;
            apply(tl, "Graphics / dance");
            return jsonResult({{"event_id", eventId}, {"bpm", scan.bpm}, {"moves", keyed}});
        } else if (action == "patch" || action == "batch") {
            // (locked layers stay as they are)
            auto lockedIn = [&](const QJsonArray& ops) {
                for (auto v : ops)
                    if (graphics_workflow::locked(data, v.toObject().value("layer_id").toString()))
                        return v.toObject().value("layer_id").toString();
                return QString();
            };
            const QJsonArray ops = a["operations"].toArray();
            if (action == "patch") {
                if (const QString l = lockedIn(ops); !l.isEmpty())
                    return errorResult("Unlock " + l + " (or its parent) first.");
                changed = motion::withPatch(data, ops, &error);
            } else {
                if (ops.isEmpty() || ops.size() > 2048)
                    return errorResult("batch: give 1 to 2048 operations.");
                changed = data;
                for (int i = 0; i < ops.size() && !changed.isEmpty(); ++i) {
                    const QJsonObject o = ops[i].toObject();
                    const QString kind = o.value("action").toString(), layer = o.value("layer_id").toString();
                    auto n = [&](const char* k, double d) { return o.contains(k) ? o.value(k).toDouble() : d; };
                    QString e;
                    if (kind != "patch" && graphics_workflow::locked(changed, layer))
                        return errorResult(QStringLiteral("batch step %1: unlock %2 (or its parent) first.").arg(i + 1).arg(layer));
                    if (kind == "move")
                        changed = motion::withMove(changed, layer, o.value("move").toString(), n("time", 0), n("duration", 0), n("amount", 1),
                                                   n("to", std::nan("")), &e);
                    else if (kind == "expression")
                        changed = motion::withExpression(changed, layer, o.value("mood").toString(), n("time", 0), &e);
                    else if (kind == "keyframe")
                        changed = motion::withKeyframe(changed, layer, o.value("property").toString(), n("time", 0), o.value("value"),
                                                       o.contains("easing") ? o.value("easing") : QJsonValue("linear"), &e);
                    else if (kind == "patch") {
                        if (const QString l = lockedIn(o.value("operations").toArray()); !l.isEmpty())
                            return errorResult(QStringLiteral("batch step %1: unlock %2 (or its parent) first.").arg(i + 1).arg(l));
                        changed = motion::withPatch(changed, o.value("operations").toArray(), &e);
                    } else
                        return errorResult(QStringLiteral("batch step %1: actions are move, expression, keyframe and patch.").arg(i + 1));
                    if (changed.isEmpty())
                        return errorResult(QStringLiteral("batch step %1 (%2): %3").arg(i + 1).arg(kind, e));
                }
            }
            if (changed.isEmpty())
                return errorResult(error);
            c->generator.data = changed;
            apply(tl, "Graphics / " + action);
            return jsonResult({{"event_id", eventId}, {"applied", action == "batch" ? int(ops.size()) : 1},
                               {"layers", int(changed["layers"].toArray().size())}});
        } else if (action == "add_prop") {
            const auto library = graphics::propLibrary();
            const QString wanted = str("prop");
            auto it = std::find_if(library.begin(), library.end(), [&](const graphics::CharacterFile& f) {
                return f.character["name"].toString().compare(wanted, Qt::CaseInsensitive) == 0 || f.path == wanted
                       || QFileInfo(f.path).completeBaseName().compare(wanted, Qt::CaseInsensitive) == 0;
            });
            if (it == library.end())
                return errorResult("Unknown prop: action props lists the library.");
            changed = motion::withProp(data, it->character, str("layer_id"), has("hand") ? str("hand") : QStringLiteral("R"), &error);
        } else if (action == "add_set") {
            const auto library = graphics::setLibrary();
            const QString wanted = str("set");
            auto it = std::find_if(library.begin(), library.end(), [&](const graphics::CharacterFile& f) {
                return f.character["name"].toString().compare(wanted, Qt::CaseInsensitive) == 0 || f.path == wanted
                       || QFileInfo(f.path).completeBaseName().compare(wanted, Qt::CaseInsensitive) == 0;
            });
            if (it == library.end())
                return errorResult("Unknown set: action sets lists the library.");
            data["version"] = 2;
            changed = motion::withSet(data, it->character, &error);
        } else if (action == "save_character") {
            QString path;
            const QJsonObject character = motion::characterFrom(data, str("layer_id"), str("name"), &error);
            if (character.isEmpty() || !graphics::saveCharacter(character, &path, &error))
                return errorResult(error);
            return jsonResult({{"saved", path}, {"character", character["name"]}});
        } else {
            const auto library = graphics::characterLibrary();
            const QString wanted = str("character");
            auto it = std::find_if(library.begin(), library.end(), [&](const graphics::CharacterFile& f) {
                return f.character["name"].toString().compare(wanted, Qt::CaseInsensitive) == 0 || f.path == wanted
                       || QFileInfo(f.path).completeBaseName().compare(wanted, Qt::CaseInsensitive) == 0;
            });
            if (it == library.end())
                return errorResult("Unknown character: action characters lists the library.");
            data["version"] = 2;
            const int before = int(data.value("layers").toArray().size());
            changed = motion::withCharacter(data, it->character, str("name"), has("x") ? num("x") : std::nan(""), num("ground", .9), &error);
            if (!changed.isEmpty() && a["colours"].isObject()) {
                const QString key = changed["layers"].toArray()[before].toObject()["id"].toString();
                QJsonObject vars = changed["variables"].toObject();
                const QJsonObject colours = a["colours"].toObject();
                for (auto c = colours.begin(); c != colours.end(); ++c)
                    if (vars.contains(key + "_" + c.key()))
                        vars[key + "_" + c.key()] = c.value();
                changed["variables"] = vars;
            }
            if (!changed.isEmpty()) {
                c->generator.data = changed;
                apply(tl, "Graphics / add character");
                const QString key = changed["layers"].toArray()[before].toObject()["id"].toString();
                return jsonResult({{"event_id", eventId}, {"character", key}, {"binding", key},
                                   {"hint", "bind_audio with source_track and binding \"" + key + "\": the mouth takes the shape of the track's words"}});
            }
        }
        if (changed.isEmpty())
            return errorResult(error);
        c->generator.data = changed;
        apply(tl, "Graphics / " + action);
        return jsonResult({{"event_id", eventId}, {"data", changed}});
    }
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
        } else if (has("source_track")) {
            const int index = integer("source_track");
            if (!c || index < 0 || index >= int(tl.tracks.size()))
                return errorResult(QStringLiteral("No track %1.").arg(index));
            QString error;
            envelope = graphics::trackEnvelope(tl.tracks[size_t(index)], project->snapshot()->media, sec(c->start - c->sourceIn), sec(c->generatedLength()),
                                               integer("bins", 4096), &error);
            if (envelope.isEmpty())
                return errorResult(error);
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
        const auto* preset = findGeneratorPreset(wave ? "audio_waveform" : graphics::sceneType(data) == "animation" ? "animation_blank" : "graphics_blank");
        auto insertion = *preset;
        insertion.length = us(length);
        tracksBefore = tl.tracks.size();
        eventId = ops::insertGenerated(tl, insertion, us(at), has("track") ? integer("track") : -1, ps.fps);
        c = tl.findClip(eventId);
        if (!c)
            return errorResult("Could not insert graphics event.");
        if (wave)
            c->generator.params["rate"].value = rate;
    }
    c->generator.data = data;
    if (!wave)
        c->generator.type = graphics::sceneType(data); // (an overlay given a version 2 scene becomes an Animation clip)
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
    QJsonObject result = tracksBefore ? placement(project->timeline(), eventId, tracksBefore) : QJsonObject{};
    result["event_id"] = eventId;
    result["data"] = data;
    return jsonResult(result);
}

QJsonObject McpServer::ToolCall::tool_edit_event()
{
    Timeline tl = project->timeline();
    TimeUs shortened = 0; // the length asked for, when the file ends before it
    int trackIndex = -1;
    Clip* c = tl.findClip(integer("event_id"), &trackIndex);
    if (!c)
        return errorResult(QStringLiteral("no event %1").arg(integer("event_id")));
    const auto media = project->mediaItem(c->mediaId);
    // Timing goes to the whole group, as in the timeline (where clicking an event selects its group): a speed,
    // trim or reverse on the picture alone left its sound out of step.
    for (const int gid : ops::expandToGroups(tl, {c->id})) {
        Clip* g = tl.findClip(gid);
        const auto gm = g ? project->mediaItem(g->mediaId) : nullptr;
        if (!g)
            continue;
        if (has("speed") && !g->isGenerated())
            // (keepSpan: the same footage, so the length follows the speed, as the description says; it used to keep the length)
            ops::setClipSpeed(*g, gm.get(), std::clamp(num("speed"), 0.05, 20.0), !a.value(QStringLiteral("keep_length")).toBool(), ps.fps);
        // Within the source: at least a frame of it left after source_in, and the length no longer than what is
        // left (source_in past the end made a zero-length event).
        const bool fromSource = gm && !g->isGenerated() && !gm->info.isStill && gm->info.duration > 0;
        if (has("source_in")) {
            TimeUs in = std::max<TimeUs>(0, us(num("source_in")));
            if (fromSource)
                in = std::min(in, std::max<TimeUs>(0, gm->info.duration - ps.fps.frameDuration()));
            g->sourceIn = in;
        }
        if (has("loop") && fromSource) // (the file repeating, so the event can run on past its end)
            g->loopLength = a.value(QStringLiteral("loop")).toBool() ? gm->info.duration : 0;
        if (has("length"))
            g->length = std::max(ps.fps.frameDuration(), ps.fps.snap(us(num("length")))); // whole frames, as placed events start on one
        if (fromSource && (has("length") || has("source_in")) && g->loopLength <= 0) {
            const TimeUs asked = g->length;
            g->length = std::clamp(g->length, ps.fps.frameDuration(), std::max(ps.fps.frameDuration(), ops::maxClipLength(gm.get(), *g)));
            if (g->id == c->id && g->length < asked - ps.fps.frameDuration())
                shortened = asked;
        }
        if (has("reverse"))
            g->reverse = a.value(QStringLiteral("reverse")).toBool();
    }
    if (has("frame_blend"))
        c->frameBlend = a.value(QStringLiteral("frame_blend")).toBool();
    if (has("optical_flow"))
        c->opticalFlow = a.value(QStringLiteral("optical_flow")).toBool();
    if (has("denoise"))
        c->denoise = c->isGenerated() ? 0.0 : std::clamp(num("denoise", 0), 0.0, 1.0);
    if (has("name"))
        c->name = str("name");
    if (has("label")) { // a colour's name, or none
        int label = 0;
        for (int l = 1; l <= kClipLabels; ++l)
            if (str("label").compare(clipLabelName(l), Qt::CaseInsensitive) == 0)
                label = l;
        if (!label && str("label").compare(QLatin1String("none"), Qt::CaseInsensitive) != 0 && !str("label").isEmpty())
            return errorResult(QStringLiteral("label is red, orange, yellow, green, teal, blue, purple, pink or none"));
        c->label = label;
    }
    if (has("muted"))
        c->muted = a.value(QStringLiteral("muted")).toBool();
    if (has("gain"))
        c->gain = float(std::clamp(num("gain"), 0.0, 4.0));
    if (has("fade_in"))
        c->fadeIn = std::clamp<TimeUs>(us(num("fade_in")), 0, c->length);
    if (has("fade_out"))
        c->fadeOut = std::clamp<TimeUs>(us(num("fade_out")), 0, c->length);
    if (has("params"))
        if (const QString error = setGeneratorParams(*c); !error.isEmpty())
            return errorResult(error);
    if (has("pan_crop")) {
        if (!isVisualTrack(tl.tracks[size_t(trackIndex)].type))
            return errorResult(QStringLiteral("Pan/Crop is for video and image events"));
        if (c->panCrop.empty())
            c->panCrop = defaultParams(panCropParams());
        const QJsonObject pc = a.value(QStringLiteral("pan_crop")).toObject();
        for (auto it = pc.begin(); it != pc.end(); ++it)
            if (const QString err = setParam(c->panCrop, panCropParams(), it.key(), it.value()); !err.isEmpty())
                return errorResult(err);
    }
    if (has("ramp")) {
        static const std::map<QString, ops::RampPreset> ramps = {
            {QStringLiteral("speed_up"), ops::RampPreset::SpeedUp}, {QStringLiteral("slow_down"), ops::RampPreset::SlowDown},
            {QStringLiteral("slow_middle"), ops::RampPreset::SlowMiddle}, {QStringLiteral("fast_middle"), ops::RampPreset::FastMiddle},
            {QStringLiteral("freeze_middle"), ops::RampPreset::FreezeMiddle}};
        if (str("ramp") == QLatin1String("none"))
            c->velocity = Envelope{false, 1.0, {}};
        else if (const auto r = ramps.find(str("ramp")); r != ramps.end())
            ops::applyRampPreset(*c, media.get(), r->second, ps.fps);
        else
            return errorResult(QStringLiteral("ramp: speed_up, slow_down, slow_middle, fast_middle, freeze_middle or none"));
        ops::syncGroupRamps(tl, c->id);
        c = tl.findClip(integer("event_id"), &trackIndex);
    }
    for (const bool atStart : {true, false}) {
        const QString key = atStart ? QStringLiteral("transition_in") : QStringLiteral("transition_out");
        if (!has(atStart ? "transition_in" : "transition_out"))
            continue;
        const QJsonObject spec = a.value(key).toObject();
        const QString type = spec.value(QStringLiteral("type")).toString(QStringLiteral("dissolve"));
        const TransitionDef* def = findTransition(type);
        if (!def)
            return errorResult(QStringLiteral("no transition \"%1\" (see list_effects kind=transition)").arg(type));
        if (!isVisualTrack(tl.tracks[size_t(trackIndex)].type))
            return errorResult(QStringLiteral("transitions are for video and image events"));
        const auto [holder, holderIn] = w->m_timeline->setTransition(tl, c->id, atStart, type);
        Clip* h = tl.findClip(holder);
        if (!h)
            return errorResult(QStringLiteral("no transition there"));
        TransitionSpec& t = holderIn ? h->transitionIn : h->transitionOut;
        const QJsonObject params = spec.value(QStringLiteral("params")).toObject();
        for (auto it = params.begin(); it != params.end(); ++it)
            if (const QString err = setParam(t.params, def->params, it.key(), it.value()); !err.isEmpty())
                return errorResult(err);
        c = tl.findClip(integer("event_id"), &trackIndex);
    }
    const int id = c->id;
    if (has("start")) { // its group moves along
        const TimeUs delta = std::max<TimeUs>(0, ps.fps.snap(us(num("start")))) - c->start;
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
    QJsonObject out{{QStringLiteral("event_id"), id}, {QStringLiteral("start"), sec(now->start)}, {QStringLiteral("length"), sec(now->length)}};
    if (shortened > 0)
        out[QStringLiteral("warning")] = QStringLiteral("The file ends there: the event is %1 s long, not %2 s. loop: true repeats the file to "
                                                        "fill it, or a lower speed stretches it.")
                                             .arg(sec(now->length), 0, 'f', 2).arg(sec(shortened), 0, 'f', 2);
    return jsonResult(out);
}

QJsonObject McpServer::ToolCall::tool_split()
{
    Timeline tl = project->timeline();
    std::set<int> ids;
    for (const QJsonValue& v : a.value(QStringLiteral("event_ids")).toArray())
        ids.insert(v.toInt());
    if (!ids.empty()) // the sound with its picture, as Split does on the timeline's selection
        ids = ops::expandToGroups(tl, ids);
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
    const QString type = has("type") ? str("type") : QStringLiteral("video");
    if (type != QLatin1String("video") && type != QLatin1String("audio") && type != QLatin1String("image"))
        return errorResult(QStringLiteral("type: video, audio or image"));
    const TrackType trackType = type == QLatin1String("image") ? TrackType::Image
        : type == QLatin1String("audio")                       ? TrackType::Audio
                                                               : TrackType::Video;
    // As documented: a picture track goes on top, a sound track at the bottom (every kind went to the bottom).
    const int index = ops::addTrack(tl, trackType, has("index") ? integer("index") : trackType == TrackType::Audio ? -1 : 0);
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
    if (kind == QLatin1String("video")) {
        for (const EffectDef& d : videoEffects())
            add(d.id, d.name, d.category, d.description, d.params);
        for (const EffectDef* d : nodes::userEffects()) // (made in the Effect Builder)
            add(d->id, d->name, d->category, d->description, d->params);
        for (const EffectDef* d : retro::effects()) // (screen shaders, downloaded: Tools > Get Screen Shaders)
            add(d->id, d->name, d->category, d->description, d->params);
    }
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
    bool audioChain = false;
    QString chainError;
    EffectChain* chain = chainFor(tl, &audioChain, &chainError);
    if (!chain)
        return errorResult(chainError);
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

QJsonObject McpServer::ToolCall::tool_remove_markers()
{
    Timeline tl = project->timeline();
    const TimeUs from = has("from") ? us(num("from")) : 0, to = has("to") ? us(num("to")) : std::numeric_limits<TimeUs>::max();
    const bool regions = a.value(QStringLiteral("regions")).toBool(false);
    const auto removed = std::erase_if(tl.markers, [&](const Marker& m) { return m.time >= from && m.time <= to && (regions || !m.isRegion()); });
    if (removed)
        apply(tl, QStringLiteral("Remove Markers"));
    return jsonResult({{QStringLiteral("removed"), qint64(removed)}});
}

QJsonObject McpServer::ToolCall::tool_detect_beats()
{
    const auto snap = project->snapshot();
    const TimeUs from = us(std::max(0.0, num("from")));
    // At most 15 minutes a call (about 20 s of mixing; hours froze the app for minutes): the AI goes on from scanned_to.
    constexpr TimeUs kMostPerCall = 15 * 60 * kUsPerSecond;
    const TimeUs asked = has("to") ? us(num("to")) : snap->timeline.duration();
    const TimeUs to = std::min({asked, snap->timeline.soundEnd(), from + kMostPerCall});
    if (to <= from)
        return errorResult(QStringLiteral("the timeline has no sound there"));
    BeatSettings bs;
    bs.band = BeatSettings::Full;
    const BeatScan scan = scanBeats(*snap, from, to, {}, bs);
    if (scan.beats.empty())
        return errorResult(QStringLiteral("no beat found (is there music on the timeline?)"));
    Timeline tl = project->timeline();
    const int n = addBeatMarkers(tl, scan.beats, std::clamp(integer("every", 4), 1, 16), true, from, to);
    apply(tl, QStringLiteral("Detect Beats"));
    return jsonResult(
        {{QStringLiteral("bpm"), scan.bpm}, {QStringLiteral("markers"), n}, {QStringLiteral("first_beat"), sec(scan.beats.front())},
         {QStringLiteral("scanned_to"), sec(to)}});
}

QJsonObject McpServer::ToolCall::tool_get_frame()
{
    const TimeUs t = has("time") ? us(num("time")) : w->m_timeline->cursorTime();
    const QString saveTo = str("save_to");
    // (saved: the project's size unless a width is given)
    const int maxSide = saveTo.isEmpty() ? 1920 : 8192;
    const int width = std::clamp(integer("width", saveTo.isEmpty() ? 640 : ps.width), 160, maxSide);
    const QSize size = sizeForWidth(width, ps.width, ps.height, maxSide);
    if (!server.m_frameRenderer) { // (the GPU renderer, as the preview and renders have it; the CPU's without a GPU)
        server.m_frameSurface = createGpuSurface();
        server.m_frameRenderer = createFrameRenderer(true, server.m_frameSurface.get(), nullptr);
    }
    const QImage img = server.m_frameRenderer->render(*project->snapshot(), t, size);
    if (img.isNull())
        return errorResult(QStringLiteral("nothing rendered"));
    QString saved;
    if (!saveTo.isEmpty()) {
        const QString suffix = QFileInfo(saveTo).suffix().toLower();
        if (suffix != QLatin1String("png") && suffix != QLatin1String("jpg") && suffix != QLatin1String("jpeg"))
            return errorResult(QStringLiteral("save_to: a .png or .jpg file"));
        QDir().mkpath(QFileInfo(saveTo).absolutePath());
        if (!img.convertToFormat(QImage::Format_RGB32).save(saveTo, nullptr, 92))
            return errorResult(QStringLiteral("couldn't write %1").arg(saveTo));
        saved = QStringLiteral(", saved to %1").arg(saveTo);
    }
    const QImage shown = img.width() > 1280 ? img.scaledToWidth(1280, Qt::SmoothTransformation) : img;
    return imageResult(shown, QStringLiteral("The picture at %1 s (%2x%3)%4").arg(sec(t), 0, 'f', 2).arg(size.width()).arg(size.height()).arg(saved));
}

QJsonObject McpServer::ToolCall::tool_transport()
{
    const QString action = str("action");
    if (has("loop"))
        w->m_loopAction->setChecked(a.value(QStringLiteral("loop")).toBool());
    if (action == QLatin1String("seek")) {
        w->stopPlayback(false);
        w->seekTo(std::max<TimeUs>(0, us(num("time"))));
    } else if (action == QLatin1String("select")) {
        const TimeUs from = us(std::max(0.0, num("from"))), to = us(num("to"));
        w->m_timeline->setRegion(from, std::max(from, to));
    } else if (action == QLatin1String("play")) {
        if (!w->m_engine->isPlaying())
            w->pausePlay();
    } else if (action == QLatin1String("stop")) {
        w->stopPlayback(false);
    } else {
        return errorResult(QStringLiteral("action is seek, play, stop or select"));
    }
    QJsonObject o{{QStringLiteral("cursor"), sec(w->m_timeline->cursorTime())},
                  {QStringLiteral("playing"), w->m_engine->isPlaying()},
                  {QStringLiteral("loop"), w->m_loopAction->isChecked()}};
    if (w->m_timeline->hasRegion())
        o[QStringLiteral("selection")] = QJsonArray{sec(w->m_timeline->regionStart()), sec(w->m_timeline->regionEnd())};
    return jsonResult(o);
}

QJsonObject McpServer::ToolCall::tool_render()
{
    if (server.m_render && !server.m_render->done)
        return errorResult(QStringLiteral("a render is already running (render_status)"));
    // The finished worker may still hold its copy of the render for a moment: wait for it, or the render would be
    // destroyed on that thread (joining itself) when it's replaced below.
    if (server.m_render && server.m_render->thread.joinable())
        server.m_render->thread.join();
    if (a.value(QStringLiteral("in_place")).toBool() || a.value(QStringLiteral("stems")).toBool()) {
        // Render Selection to New Track (from-to onto a Rendered track on top, when done), or File > Export Stems
        // (each sounding audio track to a WAV in the folder `path`), in the background as any render.
        const bool inPlace = a.value(QStringLiteral("in_place")).toBool();
        const TimeUs from = us(std::max(0.0, num("from"))), to = has("to") ? us(num("to")) : project->timeline().duration();
        if (to <= from)
            return errorResult(QStringLiteral("nothing to render there"));
        std::vector<ExportSettings> jobs;
        std::shared_ptr<ProjectSnapshot> snap;
        QString where;
        if (inPlace) {
            jobs = {w->inPlaceSettings(from, to)};
            snap = w->inPlaceSnapshot();
            where = jobs.front().path;
        } else {
            if (str("path").isEmpty())
                return errorResult(QStringLiteral("stems: give the folder as path"));
            where = QFileInfo(str("path")).absoluteFilePath();
            QDir().mkpath(where);
            jobs = w->stemSettings(where, from, to);
            snap = w->stemSnapshot();
            if (jobs.empty())
                return errorResult(QStringLiteral("there are no audio tracks with sound"));
        }
        server.m_render = std::make_shared<Render>();
        std::shared_ptr<Render> r = server.m_render;
        r->path = where;
        r->preset = inPlace ? QStringLiteral("Render in place (ProRes 422 LT)") : QStringLiteral("Stems (24-bit WAV)");
        if (inPlace && hw::settings().gpuCompositing)
            r->surface = createGpuSurface();
        r->thread = std::thread([r, snap, jobs] {
            bool ok = true;
            for (size_t k = 0; k < jobs.size() && ok; ++k) {
                Exporter exporter;
                ok = exporter.run(
                    *snap, jobs[k],
                    [&](double p) {
                        r->progress = (double(k) + p) / double(jobs.size());
                        return !r->cancel.load() && !r->monitor->cancel.load();
                    },
                    r->surface.get(), [&](const ExportStatus& value) { r->monitor->update(value); });
                if (!ok)
                    r->error = r->cancel ? QStringLiteral("cancelled") : exporter.error();
            }
            r->ok = ok;
            r->monitor->done = true;
            r->done = true;
        });
        if (inPlace) { // (placed when it is done)
            auto* completion = new QTimer(w);
            MainWindow* win = w;
            const std::weak_ptr<const int> session = project->session(); // (another project opened meanwhile: not placed in it)
            QObject::connect(completion, &QTimer::timeout, win, [r, completion, win, where, from, to, session] {
                if (!r->done)
                    return;
                completion->stop();
                completion->deleteLater();
                if (r->ok && session.lock() == win->m_project->session())
                    win->placeRendered(where, from, to);
                else if (r->ok)
                    r->error = QStringLiteral("rendered, but not placed: another project was opened meanwhile (the file is %1)").arg(where);
            });
            completion->start(200);
        }
        QJsonArray files;
        for (const ExportSettings& job : jobs)
            files.append(job.path);
        return jsonResult({{QStringLiteral("rendering"), where}, {QStringLiteral("files"), files}, {QStringLiteral("seconds"), sec(to - from)},
                           {QStringLiteral("note"), inPlace ? QStringLiteral("it goes onto a Rendered track on top when render_status says done")
                                                            : QStringLiteral("each track's stem, before Master FX")}});
    }
    const QString path = QFileInfo(str("path")).absoluteFilePath();
    if (str("path").isEmpty())
        return errorResult(QStringLiteral("give a path"));
    if (QFileInfo::exists(path) && !a.value(QStringLiteral("overwrite")).toBool())
        return errorResult(QStringLiteral("%1 exists (overwrite: true to replace it)").arg(path));
    const QString want = has("preset") ? str("preset") : QStringLiteral("MP4 H.264");
    const ExportPreset* chosen = matchPreset(want, path);
    if (!chosen)
        return errorResult(QStringLiteral("no Render As template matches \"%1\"").arg(want));
    if (!chosen->extension.isEmpty() && QFileInfo(path).suffix().compare(chosen->extension, Qt::CaseInsensitive) != 0)
        return errorResult(QStringLiteral("\"%1\" writes .%2 files: give a path ending in .%2").arg(chosen->name, chosen->extension));
    ExportSettings s = settingsFromPreset(*chosen, ps);
    if (a.value(QStringLiteral("hardware_encoding")).toBool(true))
        s = withGpuEncoder(s);
    s.path = path;
    s.smartRender = a.value(QStringLiteral("smart_render")).toBool();
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
        r->smartNote = exporter.smartRenderNote();
        r->ok = ok;
        r->monitor->done = true;
        r->done = true;
    });
    auto* status = new RenderStatusDialog(s, *snap, r->monitor, server.m_window);
    status->setAttribute(Qt::WA_DeleteOnClose);
    status->show();
    auto* completion = new QTimer(status);
    QObject::connect(completion, &QTimer::timeout, status, [r, completion, projectPath = project->filePath(), templateName = chosen->name] {
        if (!r->done)
            return;
        completion->stop();
        if (r->ok) {
            DeliverView::remember(projectPath, r->path, templateName);
            Profiles::instance().remember({QJsonObject{{"path", r->path}, {"folder", "Exports"}, {"origin", "render"}}});
        }
    });
    completion->start(200);
    QJsonObject result{{QStringLiteral("rendering"), path},
                       {QStringLiteral("template"), chosen->name},
                       {QStringLiteral("seconds"), sec(s.rangeEnd - s.rangeStart)}};
    if (chosen->maxSeconds > 0 && s.rangeEnd - s.rangeStart > TimeUs(chosen->maxSeconds) * kUsPerSecond) // as the dialog warns
        result[QStringLiteral("warning")] = QStringLiteral("this render is %1 long; %2 accepts up to %3")
                                                .arg(formatDuration(s.rangeEnd - s.rangeStart), chosen->name.section(QLatin1Char(' '), 0, 0),
                                                     formatDuration(TimeUs(chosen->maxSeconds) * kUsPerSecond));
    return jsonResult(result);
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
        if (!server.m_render->smartNote.isEmpty())
            o[QStringLiteral("smart_render")] = server.m_render->smartNote;
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
    QDir().mkpath(QFileInfo(path).absolutePath()); // a new folder is fine, as for renders
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
            f.open(QIODevice::ReadOnly) ? parseSubtitles(subtitleText(f.readAll())) : std::vector<Caption>();
        if (caps.empty())
            return errorResult(QStringLiteral("no subtitles in %1").arg(path));
        w->addCaptions(caps, 0, kinetic);
        return jsonResult({{QStringLiteral("captions"), int(caps.size())}});
    }
    if (action == QLatin1String("export")) {
        const std::vector<Caption> caps = timelineCaptions(project->timeline());
        QSaveFile f(path);
        if (caps.empty())
            return errorResult(QStringLiteral("there are no text events to export"));
        const QByteArray bytes = (path.endsWith(QLatin1String(".vtt"), Qt::CaseInsensitive) ? writeVtt(caps) : writeSrt(caps)).toUtf8();
        if (!f.open(QIODevice::WriteOnly) || f.write(bytes) != bytes.size() || !f.commit())
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
        const QString path = runtime.id == "speech" ? VoiceJob::workerPath() : runtime.id == "stems" ? stems::workerPath() : rvcWorkerPath();
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
    if (a.value("cancel").toBool()) {
        w->dialogueJob()->cancel();
        w->voiceJob()->cancel();
    }
    QJsonObject status = w->voiceJob()->status();
    if (w->m_dialogueJob)
        status["dialogue"] = w->m_dialogueJob->status();
    return jsonResult(status);
}

QJsonObject McpServer::ToolCall::tool_user_guide()
{
    const QDir root(MainWindow::docsPath({}));
    if (!has("page")) {
        QJsonArray pages;
        QDirIterator it(root.path(), {QStringLiteral("*.md")}, QDir::Files, QDirIterator::Subdirectories);
        while (it.hasNext()) {
            const QString path = it.next();
            const QString page = root.relativeFilePath(path);
            if (page.startsWith(QLatin1String("developer/")))
                continue;
            QFile f(path);
            QString title;
            if (f.open(QIODevice::ReadOnly | QIODevice::Text))
                title = QString::fromUtf8(f.readLine()).remove(QLatin1Char('#')).trimmed();
            pages.append(QJsonObject{{"page", page}, {"title", title}});
        }
        return jsonResult({{QStringLiteral("pages"), pages}});
    }
    // (only pages inside the guide's folder)
    const QString page = QDir::cleanPath(str("page"));
    const QString path = root.absoluteFilePath(page);
    QFile f(path);
    if (page.startsWith(QLatin1String("..")) || QDir::isAbsolutePath(page) || !page.endsWith(QLatin1String(".md")) || !path.startsWith(root.absolutePath())
        || !f.open(QIODevice::ReadOnly | QIODevice::Text))
        return errorResult(QStringLiteral("No page %1: call user_guide without page for the list.").arg(str("page")));
    return textResult(QString::fromUtf8(f.readAll()));
}

QJsonObject McpServer::ToolCall::tool_list_sound_effects()
{
    QJsonArray sounds;
    const QDir root(MainWindow::soundLibraryDir());
    QDirIterator it(root.path(), {QStringLiteral("*.ogg")}, QDir::Files, QDirIterator::Subdirectories);
    while (it.hasNext()) {
        const QFileInfo f(it.next());
        if (f.completeBaseName().contains(str("search"), Qt::CaseInsensitive))
            sounds.append(QJsonObject{{"category", f.dir().dirName()}, {"name", f.completeBaseName()}, {"path", f.absoluteFilePath()}});
    }
    return jsonResult({{QStringLiteral("sounds"), sounds}, {QStringLiteral("credits"), root.filePath(QStringLiteral("CREDITS.md"))}});
}

QJsonObject McpServer::ToolCall::tool_generate_dialogue()
{
    const auto voices = localVoiceCatalog();
    const QJsonObject castJson = a.value("cast").toObject();
    std::vector<DialogueVoice> cast;
    for (const QString& who : dialogueSpeakers(dialogueLines(str("script")))) {
        const QJsonObject c = castJson.value(who).toObject();
        const QString id = c.value("voice").toString(QStringLiteral("af_heart"));
        const auto v = std::find_if(voices.begin(), voices.end(), [&](const LocalVoice& voice) { return voice.id == id; });
        if (v == voices.end())
            return errorResult(QStringLiteral("%1: unknown voice %2, use list_voices.").arg(who, id));
        if (!voiceAssetInstalled(v->asset) || !voiceAssetInstalled(kokoroModelAsset()))
            return errorResult(QStringLiteral("%1: voice %2 isn't downloaded yet (Tools > Captions > Generate Voice).").arg(who, id));
        cast.push_back({who, *v, c.value("rate").toDouble(1), c.value("pitch").toDouble(0)});
    }
    QString error;
    const TimeUs start = has("start") ? us(num("start")) : w->m_timeline->cursorTime();
    QString alignmentModel; // (word times for exact mouth shapes, when a speech model is there and not turned off)
    if (!has("align") || a.value("align").toBool())
        for (const auto& candidate : {"ggml-base.bin", "ggml-small.bin", "ggml-tiny.bin", "ggml-medium.bin"})
            if (alignmentModel.isEmpty() && QFileInfo::exists(w->whisperModelPath(candidate)))
                alignmentModel = w->whisperModelPath(candidate);
    if (!w->dialogueJob()->start(str("script"), cast, start, us(num("gap", 0.3)), &error, alignmentModel))
        return errorResult(error);
    return jsonResult(w->dialogueJob()->status());
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
        if (!(f >= 1 && f <= 240)) // 0 fps made a frame rate every time calculation divides by
            return errorResult(QStringLiteral("fps: 1 to 240"));
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
                  {QStringLiteral("last_recording"), live->m_lastRecording},
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

QJsonObject McpServer::ToolCall::tool_edit_track()
{
    Timeline tl = project->timeline();
    const int index = integer("track", -1);
    if (index < 0 || index >= int(tl.tracks.size()))
        return errorResult(QStringLiteral("no track %1").arg(index));
    Track& t = tl.tracks[size_t(index)];
    if (has("locked"))
        t.locked = a.value(QStringLiteral("locked")).toBool();
    if (has("muted"))
        t.muted = a.value(QStringLiteral("muted")).toBool();
    if (has("solo"))
        t.solo = a.value(QStringLiteral("solo")).toBool();
    if (has("name"))
        t.name = str("name").left(80);
    if (has("level"))
        t.level = float(std::clamp(num("level", 1), 0.0, isVisualTrack(t.type) ? 1.0 : 2.0));
    if (has("pan")) {
        if (t.type != TrackType::Audio)
            return errorResult(QStringLiteral("pan is for audio tracks"));
        t.pan = float(std::clamp(num("pan", 0), -1.0, 1.0));
    }
    if (has("blend")) {
        const QStringList names = blendModeNames();
        const auto found = std::find_if(names.begin(), names.end(), [&](const QString& n) { return n.compare(str("blend"), Qt::CaseInsensitive) == 0; });
        const int b = found == names.end() ? -1 : int(found - names.begin());
        if (b < 0 || !isVisualTrack(t.type))
            return errorResult(QStringLiteral("blend is one of %1, on a video track").arg(blendModeNames().join(QStringLiteral(", "))));
        t.blend = b;
    }
    if (has("matte")) {
        const int m = QStringList{QStringLiteral("off"), QStringLiteral("alpha"), QStringLiteral("alpha_inverted"), QStringLiteral("luma"),
                                  QStringLiteral("luma_inverted")}
                          .indexOf(str("matte"));
        if (m < 0 || !isVisualTrack(t.type))
            return errorResult(QStringLiteral("matte is off, alpha, alpha_inverted, luma or luma_inverted, on a video track"));
        t.matte = m;
    }
    if (has("duck_under") || has("duck_amount")) {
        if (t.type != TrackType::Audio)
            return errorResult(QStringLiteral("ducking is for audio tracks"));
        if (has("duck_under")) {
            const int under = integer("duck_under", -1);
            if (under >= int(tl.tracks.size()) || under == index)
                return errorResult(QStringLiteral("no other track %1").arg(under));
            if (under >= 0 && tl.tracks[size_t(under)].type != TrackType::Audio)
                return errorResult(QStringLiteral("track %1 is not an audio track: ducking follows another track's sound").arg(under));
            t.duckUnder = under < 0 ? 0 : tl.tracks[size_t(under)].id;
        }
        if (has("duck_amount"))
            t.duckAmount = float(std::clamp(num("duck_amount", -12), -60.0, 0.0));
    }
    apply(tl, QStringLiteral("Edit track"));
    return jsonResult({{QStringLiteral("track"), index}, {QStringLiteral("locked"), t.locked}, {QStringLiteral("muted"), t.muted},
                       {QStringLiteral("solo"), t.solo}, {QStringLiteral("name"), t.name}});
}

QJsonObject McpServer::ToolCall::tool_new_project()
{
    if (project->isModified() && !a.value(QStringLiteral("discard")).toBool())
        return errorResult(QStringLiteral("the project has unsaved changes: save_project first, or pass discard: true"));
    if (w->voiceJob()->busy())
        return errorResult(QStringLiteral("Wait for voice generation to finish."));
    ProjectSettings settings;
    settings.width = std::clamp(integer("width", 1920), 16, 8192) & ~1;
    settings.height = std::clamp(integer("height", 1080), 16, 8192) & ~1;
    const double fps = std::clamp(num("fps", 30), 1.0, 240.0);
    // Whole rates exactly; NTSC rates (23.976, 29.97, 59.94) as n*1000/1001; anything else to a thousandth.
    const double ntsc = fps * 1.001;
    settings.fps = std::abs(fps - std::round(fps)) < 1e-6        ? FrameRate{int(std::round(fps)), 1}
                   : std::abs(ntsc - std::round(ntsc)) < 0.005 ? FrameRate{int(std::round(ntsc)) * 1000, 1001}
                                                                 : FrameRate{int(std::round(fps * 1000)), 1000};
    w->newProject(settings);
    return jsonResult({{QStringLiteral("width"), settings.width}, {QStringLiteral("height"), settings.height}, {QStringLiteral("fps"), settings.fps.toDouble()}});
}

QJsonObject McpServer::ToolCall::tool_open_project()
{
    if (project->isModified() && !a.value(QStringLiteral("discard")).toBool())
        return errorResult(QStringLiteral("the project has unsaved changes: save_project first, or pass discard: true"));
    if (w->voiceJob()->busy())
        return errorResult(QStringLiteral("Wait for voice generation to finish."));
    const QString path = QFileInfo(str("path")).absoluteFilePath();
    w->stopPlayback(false);
    QString error;
    QStringList missing;
    if (str("path").isEmpty() || !project->load(path, &error, &missing))
        return errorResult(error.isEmpty() ? QStringLiteral("give a path") : error);
    w->removeAutosave();
    w->addRecent(path);
    w->m_settingsMatched = true;
    w->m_timeline->setRegion(0, 0);
    w->m_timeline->setCursorTime(0);
    w->m_timeline->zoomToFit();
    w->refreshPreview();
    w->updateTitle();
    return jsonResult({{QStringLiteral("opened"), path}, {QStringLiteral("missing"), QJsonArray::fromStringList(missing)},
                       {QStringLiteral("seconds"), sec(project->timeline().duration())}});
}

QJsonObject McpServer::ToolCall::tool_delete_track()
{
    Timeline tl = project->timeline();
    int removed = 0;
    if (a.value(QStringLiteral("empty")).toBool()) {
        for (int i = int(tl.tracks.size()) - 1; i >= 0 && tl.tracks.size() > 1; --i)
            if (tl.tracks[size_t(i)].clips.empty()) {
                ops::removeTrack(tl, i);
                ++removed;
            }
    } else {
        const int index = integer("track", -1);
        if (index < 0 || index >= int(tl.tracks.size()))
            return errorResult(QStringLiteral("no track %1").arg(index));
        if (tl.tracks.size() == 1)
            return errorResult(QStringLiteral("a project keeps at least one track"));
        ops::removeTrack(tl, index);
        removed = 1;
    }
    if (removed)
        apply(tl, QStringLiteral("Delete track"));
    return jsonResult({{QStringLiteral("removed"), removed}, {QStringLiteral("tracks"), int(tl.tracks.size())}});
}

QJsonObject McpServer::ToolCall::tool_freeze_frame()
{
    const TimeUs length = us(std::clamp(num("length", 2), 0.1, 60.0));
    w->m_timeline->setCursorTime(us(std::max(0.0, num("time"))), false);
    const int id = w->m_timeline->insertFreezeFrame(length, a.value(QStringLiteral("in_place")).toBool());
    if (!id)
        return errorResult(QStringLiteral("no video event from a file at %1 s (generated pictures can't be frozen)").arg(num("time")));
    return jsonResult({{QStringLiteral("event_id"), id}, {QStringLiteral("length"), sec(length)}});
}

QJsonObject McpServer::ToolCall::tool_scene_changes()
{
    Timeline tl = project->timeline();
    std::set<int> ids;
    for (const QJsonValue v : a.value(QStringLiteral("event_ids")).toArray())
        ids.insert(v.toInt());
    const MediaMap media = project->snapshot()->media;
    int skipped = 0;
    const std::vector<int> events = scenes::videoEvents(tl, media, ids, &skipped);
    if (events.empty())
        return errorResult(skipped ? QStringLiteral("reversed or speed-ramped events aren't split") : QStringLiteral("give video events"));
    const double sensitivity = std::clamp(num("sensitivity", 0.5), 0.0, 1.0);
    QJsonArray times;
    if (!a.value(QStringLiteral("split")).toBool(true)) {
        for (int id : events) {
            const Clip& c = *tl.findClip(id);
            const TimeUs to = c.sourceIn + TimeUs(double(c.length) * c.speed);
            for (TimeUs cut : scenes::detect(*media.at(c.mediaId), c.streamIndex, c.sourceIn, to, sensitivity))
                times.append(sec(c.start + TimeUs(double(cut - c.sourceIn) / c.speed)));
        }
        return jsonResult({{QStringLiteral("cuts"), times}});
    }
    const auto cuts = scenes::splitEvents(tl, media, events, sensitivity, ps.fps);
    for (TimeUs t : cuts.value_or(std::vector<TimeUs>{}))
        times.append(sec(t));
    if (!times.isEmpty())
        apply(tl, QStringLiteral("Split at Scene Changes"));
    return jsonResult({{QStringLiteral("cuts"), times}, {QStringLiteral("split"), !times.isEmpty()}});
}

namespace {
// An AI's graph ({name, description, nodes, links, exposed}) into a node graph, laid out in columns by how far each node
// is from the start. Empty when it's fine, else what's wrong.
QString graphFromJson(const QJsonObject& o, nodes::Graph* g)
{
    g->name = o.value(QStringLiteral("name")).toString().trimmed();
    g->description = o.value(QStringLiteral("description")).toString().trimmed();
    int top = 0, outputs = 0;
    for (const QJsonValue v : o.value(QStringLiteral("nodes")).toArray()) {
        const QJsonObject n = v.toObject();
        nodes::Node node;
        node.id = n.value(QStringLiteral("id")).toInt();
        node.type = n.value(QStringLiteral("type")).toString();
        const nodes::NodeDef* def = nodes::findNode(node.type);
        if (node.id <= 0 || node.id >= nodes::kMaxNodeId || g->find(node.id))
            return QStringLiteral("each node needs its own id (a whole number from 1 to %1): %2").arg(nodes::kMaxNodeId - 1).arg(node.id);
        if (int(g->nodes.size()) >= nodes::kMaxNodes)
            return QStringLiteral("a graph can have at most %1 nodes").arg(nodes::kMaxNodes);
        if (!def)
            return QStringLiteral("there is no node type \"%1\" (node_effect action nodes lists them)").arg(node.type);
        node.params = defaultParams(def->params);
        const QJsonObject params = n.value(QStringLiteral("params")).toObject();
        for (auto it = params.begin(); it != params.end(); ++it)
            if (const QString error = setParam(node.params, def->params, it.key(), it.value()); !error.isEmpty())
                return QStringLiteral("node %1 (%2): %3").arg(node.id).arg(node.type, error);
        outputs += node.type == QLatin1String("output");
        top = std::max(top, node.id);
        g->nodes.push_back(std::move(node));
    }
    if (outputs != 1)
        return QStringLiteral("the graph needs exactly one node of type output (it has %1)").arg(outputs);
    g->nextId = top + 1;
    for (const QJsonValue v : o.value(QStringLiteral("links")).toArray()) {
        int from = 0, to = 0, port = 0;
        if (v.isArray()) {
            const QJsonArray l = v.toArray();
            from = l.at(0).toInt();
            to = l.at(1).toInt();
            port = l.at(2).toInt();
        } else {
            const QJsonObject l = v.toObject();
            from = l.value(QStringLiteral("from")).toInt();
            to = l.value(QStringLiteral("to")).toInt();
            port = l.value(QStringLiteral("input")).toInt();
        }
        const nodes::Node* a = g->find(from);
        const nodes::Node* b = g->find(to);
        const nodes::NodeDef* bd = b ? nodes::findNode(b->type) : nullptr;
        if (!a || !b)
            return QStringLiteral("a link names a node that isn't there: [%1, %2, %3]").arg(from).arg(to).arg(port);
        if (!nodes::findNode(a->type)->output)
            return QStringLiteral("node %1 is the output: nothing comes out of it").arg(from);
        if (port < 0 || port >= int(bd->inputs.size()))
            return QStringLiteral("node %1 (%2) has inputs 0..%3 (%4)").arg(to).arg(b->type).arg(int(bd->inputs.size()) - 1).arg(bd->inputs.join(QStringLiteral(", ")));
        if (!g->connect(from, to, port))
            return QStringLiteral("linking %1 into %2 would make a loop").arg(from).arg(to);
    }
    for (const QJsonValue v : o.value(QStringLiteral("exposed")).toArray()) {
        const QJsonObject x = v.toObject();
        const int node = x.value(QStringLiteral("node")).toInt();
        const QString param = x.value(QStringLiteral("param")).toString();
        const nodes::Node* n = g->find(node);
        const nodes::NodeDef* def = n ? nodes::findNode(n->type) : nullptr;
        const auto pd = def ? std::find_if(def->params.begin(), def->params.end(), [&](const ParamDef& p) { return p.id == param; })
                            : std::vector<ParamDef>::const_iterator{};
        if (!def || pd == def->params.end())
            return QStringLiteral("exposed: node %1 has no setting \"%2\"").arg(node).arg(param);
        if (pd->type == ParamType::Text || pd->type == ParamType::Font)
            return QStringLiteral("exposed: text settings can't be the effect's (%1)").arg(param);
        const QString name = x.value(QStringLiteral("name")).toString().trimmed();
        g->exposed.push_back({QStringLiteral("n%1_%2").arg(node).arg(param), name.isEmpty() ? pd->name : name, node, param});
    }
    // Columns by depth (how many nodes lie between it and the start), rows in order.
    std::map<int, int> depth;
    std::function<int(int, int)> deep = [&](int id, int guard) {
        if (auto it = depth.find(id); it != depth.end())
            return it->second;
        int d = 0;
        for (const nodes::Link& l : g->links)
            if (l.to == id && guard < 512)
                d = std::max(d, deep(l.from, guard + 1) + 1);
        return depth[id] = d;
    };
    std::map<int, int> rows;
    for (nodes::Node& n : g->nodes) {
        const int d = deep(n.id, 0);
        n.pos = QPointF(d * 250.0, rows[d]++ * 210.0);
    }
    return {};
}

// A node graph as an AI writes one (setting values plain).
QJsonObject graphToJson(const nodes::Graph& g)
{
    QJsonArray ns, ls, xs;
    for (const nodes::Node& n : g.nodes) {
        QJsonObject params;
        for (const auto& [id, v] : n.params)
            params[id] = v.text.isEmpty() ? QJsonValue(v.value) : QJsonValue(v.text);
        ns.append(QJsonObject{{QStringLiteral("id"), n.id}, {QStringLiteral("type"), n.type}, {QStringLiteral("params"), params}});
    }
    for (const nodes::Link& l : g.links)
        ls.append(QJsonArray{l.from, l.to, l.port});
    for (const nodes::Exposed& x : g.exposed)
        xs.append(QJsonObject{{QStringLiteral("node"), x.node}, {QStringLiteral("param"), x.param}, {QStringLiteral("name"), x.name}, {QStringLiteral("id"), x.id}});
    return {{QStringLiteral("name"), g.name}, {QStringLiteral("description"), g.description}, {QStringLiteral("nodes"), ns},
            {QStringLiteral("links"), ls}, {QStringLiteral("exposed"), xs}};
}
} // namespace

QJsonObject McpServer::ToolCall::tool_node_effect()
{
    const QString action = str("action");
    if (action == QLatin1String("nodes")) {
        const QString search = str("search");
        const bool details = a.value(QStringLiteral("details")).toBool() || !search.isEmpty();
        QJsonArray out;
        for (const nodes::NodeDef& d : nodes::nodeDefs()) {
            bool match = true;
            for (const QString& word : search.split(QLatin1Char(' '), Qt::SkipEmptyParts))
                match = match && (d.id + QLatin1Char(' ') + d.name + QLatin1Char(' ') + d.category + QLatin1Char(' ') + d.description).contains(word, Qt::CaseInsensitive);
            if (!match)
                continue;
            QJsonObject o{{QStringLiteral("type"), d.id}, {QStringLiteral("name"), d.name}, {QStringLiteral("category"), d.category},
                          {QStringLiteral("inputs"), QJsonArray::fromStringList(d.inputs)}};
            if (details) {
                o[QStringLiteral("description")] = d.description;
                QJsonArray ps2;
                for (const ParamDef& p : d.params)
                    if (!p.hidden)
                        ps2.append(paramDefJson(p));
                o[QStringLiteral("params")] = ps2;
            }
            out.append(o);
        }
        return jsonResult({{QStringLiteral("nodes"), out}, {QStringLiteral("count"), out.size()}});
    }
    if (action == QLatin1String("list")) {
        QJsonArray out;
        for (const EffectDef* d : nodes::userEffects()) {
            QJsonArray ps2;
            for (const ParamDef& p : d->params)
                if (!p.hidden)
                    ps2.append(p.id);
            out.append(QJsonObject{{QStringLiteral("id"), d->id}, {QStringLiteral("name"), d->name}, {QStringLiteral("description"), d->description},
                                   {QStringLiteral("params"), ps2}});
        }
        return jsonResult({{QStringLiteral("effects"), out}, {QStringLiteral("folder"), nodes::userEffectsFolder()}});
    }
    if (action == QLatin1String("get")) {
        const QString which = str("effect");
        const EffectDef* def = nodes::findUserEffect(which);
        for (const EffectDef* d : nodes::userEffects())
            if (!def && d->name.compare(which, Qt::CaseInsensitive) == 0)
                def = d;
        if (!def)
            return errorResult(QStringLiteral("no user effect \"%1\" (action list)").arg(which));
        const auto graph = std::find_if(def->params.begin(), def->params.end(), [](const ParamDef& p) { return p.id == QLatin1String("graph"); });
        if (graph == def->params.end())
            return errorResult(QStringLiteral("that effect has no graph"));
        QJsonObject o = graphToJson(nodes::fromJson(QJsonDocument::fromJson(graph->defText.toUtf8()).object()));
        o[QStringLiteral("effect")] = def->id;
        return jsonResult(o);
    }
    if (action != QLatin1String("preview") && action != QLatin1String("save"))
        return errorResult(QStringLiteral("action is nodes, preview, save, list or get"));
    nodes::Graph g;
    if (const QString error = graphFromJson(a.value(QStringLiteral("graph")).toObject(), &g); !error.isEmpty())
        return errorResult(error);
    if (action == QLatin1String("preview")) {
        const TimeUs t = has("time") ? us(num("time")) : w->m_timeline->cursorTime();
        const int width = std::clamp(integer("width", 640), 160, 1280);
        const QSize size = sizeForWidth(width, ps.width, ps.height, 1280);
        Compositor renderer(true);
        const QImage frame = renderer.render(*project->snapshot(), t, size);
        RenderContext ctx;
        ctx.time = t;
        ctx.timelineTime = t;
        ctx.scale = double(frame.width()) / std::max(1, ps.width); // (a tall project's frame is narrower than asked)
        ctx.fps = ps.fps.toDouble();
        ctx.contentRect = QRectF(frame.rect());
        nodes::Evaluator eval;
        const QImage out = eval.run(g, frame, ctx);
        return imageResult(out, eval.error.isEmpty() ? QStringLiteral("The graph on the picture at %1 s").arg(sec(t), 0, 'f', 2)
                                                     : QStringLiteral("The graph at %1 s: %2").arg(sec(t), 0, 'f', 2).arg(eval.error));
    }
    if (g.name.isEmpty())
        return errorResult(QStringLiteral("give the graph a name"));
    const QString file = QDir(nodes::userEffectsFolder()).filePath(safeFileName(g.name, QStringLiteral("Effect")) + QStringLiteral(".mvpfx"));
    const QString id = nodes::effectId(g.name);
    if (!a.value(QStringLiteral("overwrite")).toBool() && (QFileInfo::exists(file) || nodes::findUserEffect(id)))
        return errorResult(QStringLiteral("there is already an effect called \"%1\" (overwrite true replaces it)").arg(g.name));
    QString error;
    if (!nodes::save(g, file, &error))
        return errorResult(QStringLiteral("couldn't save: %1").arg(error));
    const EffectDef* def = nodes::registerEffect(g);
    QJsonArray params;
    for (const ParamDef& p : def->params)
        if (!p.hidden)
            params.append(p.id);
    return jsonResult({{QStringLiteral("effect"), def->id}, {QStringLiteral("name"), def->name}, {QStringLiteral("file"), file}, {QStringLiteral("params"), params},
                       {QStringLiteral("note"), QStringLiteral("add it with add_effect effect %1").arg(def->id)}});
}

EffectChain* McpServer::ToolCall::chainFor(Timeline& tl, bool* audio, QString* error) const
{
    const QString target = str("target");
    const int id = integer("id", -1);
    *audio = false;
    auto fail = [&](const QString& why) -> EffectChain* {
        *error = why;
        return nullptr;
    };
    if (target == QLatin1String("event")) {
        int ti = -1;
        Clip* c = tl.findClip(id, &ti);
        if (!c)
            return fail(QStringLiteral("no event %1").arg(id));
        *audio = tl.tracks[size_t(ti)].type == TrackType::Audio;
        return &c->effects;
    }
    if (target == QLatin1String("track")) {
        if (id < 0 || id >= int(tl.tracks.size()))
            return fail(QStringLiteral("no track %1").arg(id));
        *audio = tl.tracks[size_t(id)].type == TrackType::Audio;
        return &tl.tracks[size_t(id)].effects;
    }
    if (target == QLatin1String("output"))
        return &tl.outputEffects;
    if (target == QLatin1String("audio_master")) {
        *audio = true;
        return &tl.audioMasterEffects;
    }
    if (target == QLatin1String("live"))
        return &tl.liveEffects;
    if (target == QLatin1String("layer")) { // (id: the layer, 1-4; default 1)
        const int layer = id < 0 ? 1 : id;
        if (layer < 1 || layer > kLiveLayers)
            return fail(QStringLiteral("layers are 1-%1").arg(kLiveLayers));
        return &tl.liveLayerEffects[size_t(layer - 1)];
    }
    if (target == QLatin1String("pad")) {
        if (id < 1 || id > kLivePads)
            return fail(QStringLiteral("pads are 1-16"));
        if (tl.livePads[size_t(id - 1)].locked)
            return fail(QStringLiteral("pad %1 is locked").arg(id));
        return &tl.livePads[size_t(id - 1)].effects;
    }
    return fail(QStringLiteral("target is event, track, output, audio_master, live, pad or layer"));
}

namespace {
QJsonArray envelopeJson(const Envelope& e)
{
    QJsonArray out;
    for (const Keyframe& k : e.points)
        out.append(QJsonArray{sec(k.time), k.value, keyInterpId(k.interp)});
    return out;
}

// An effect anywhere in the project, by its id.
EffectInstance* effectById(Timeline& tl, int id)
{
    EffectInstance* found = nullptr;
    std::function<void(EffectChain&)> in = [&](EffectChain& chain) {
        for (EffectInstance& e : chain) {
            if (e.id == id)
                found = &e;
            for (RackChain& c : e.chains)
                in(c.effects);
        }
    };
    forEachChain(tl, [&](EffectChain& chain, const QString&) {
        in(chain);
        return found != nullptr; // (true stops the walk)
    });
    return found;
}

std::set<int> idSet(const QJsonValue& v)
{
    std::set<int> ids;
    for (const QJsonValue x : v.toArray())
        ids.insert(x.toInt());
    return ids;
}
} // namespace

QJsonObject McpServer::ToolCall::tool_envelope()
{
    const QString kind = str("kind");
    Timeline tl = project->timeline();
    Envelope* env = nullptr;
    double lo = 0, hi = 2, def = 1;
    Clip* clip = nullptr;
    if (kind == QLatin1String("velocity")) {
        clip = tl.findClip(integer("event_id", -1));
        if (!clip)
            return errorResult(QStringLiteral("velocity: give the event_id"));
        env = &clip->velocity;
        lo = kMinVelocity;
        hi = kMaxVelocity;
    } else {
        const int ti = integer("track", -1);
        if (ti < 0 || ti >= int(tl.tracks.size()))
            return errorResult(QStringLiteral("give the track (index)"));
        Track& t = tl.tracks[size_t(ti)];
        if (kind == QLatin1String("volume") || kind == QLatin1String("pan")) {
            if (t.type != TrackType::Audio)
                return errorResult(QStringLiteral("%1 is for audio tracks (video tracks have composite)").arg(kind));
            env = kind == QLatin1String("volume") ? &t.volume : &t.panEnvelope;
            if (kind == QLatin1String("pan")) {
                lo = -1;
                hi = 1;
                def = 0;
            }
        } else if (kind == QLatin1String("composite")) {
            if (!isVisualTrack(t.type))
                return errorResult(QStringLiteral("composite is for video tracks"));
            env = &t.composite;
            hi = 1;
        } else {
            return errorResult(QStringLiteral("kind is volume, pan, composite or velocity"));
        }
    }
    if (a.value(QStringLiteral("clear")).toBool()) {
        *env = Envelope{false, def, {}};
    } else {
        if (has("points")) {
            std::vector<Keyframe> points;
            for (const QJsonValue v : a.value(QStringLiteral("points")).toArray()) {
                const QJsonArray p = v.toArray();
                if (p.size() < 2)
                    return errorResult(QStringLiteral("points are [seconds, value, curve?]"));
                Keyframe k;
                k.time = us(std::max(0.0, p[0].toDouble()));
                k.value = std::clamp(p[1].toDouble(), lo, hi);
                if (p.size() >= 3) {
                    const auto interp = keyInterpFromId(p[2].toString());
                    if (!interp)
                        return errorResult(QStringLiteral("unknown curve \"%1\"").arg(p[2].toString()));
                    k.interp = *interp;
                }
                points.push_back(k);
            }
            std::sort(points.begin(), points.end(), [](const Keyframe& x, const Keyframe& y) { return x.time < y.time; });
            env->points = std::move(points);
            env->active = true;
        }
        if (has("active"))
            env->active = a.value(QStringLiteral("active")).toBool();
    }
    if (clip)
        ops::syncGroupRamps(tl, clip->id); // (the grouped sound keeps pace)
    apply(tl, QStringLiteral("Envelope"));
    return jsonResult({{QStringLiteral("kind"), kind}, {QStringLiteral("active"), env->active}, {QStringLiteral("points"), envelopeJson(*env)}});
}

QJsonObject McpServer::ToolCall::tool_group()
{
    Timeline tl = project->timeline();
    const std::set<int> ids = idSet(a.value(QStringLiteral("event_ids")));
    for (int id : ids)
        if (!tl.findClip(id))
            return errorResult(QStringLiteral("no event %1").arg(id));
    if (str("action") == QLatin1String("group")) {
        if (ids.size() < 2)
            return errorResult(QStringLiteral("group needs two events or more"));
        ops::groupClips(tl, ids);
    } else if (str("action") == QLatin1String("ungroup")) {
        if (ids.empty())
            return errorResult(QStringLiteral("ungroup needs event_ids"));
        ops::ungroupClips(tl, ids);
    } else {
        return errorResult(QStringLiteral("action is group or ungroup"));
    }
    apply(tl, str("action") == QLatin1String("group") ? QStringLiteral("Group") : QStringLiteral("Ungroup"));
    const Clip* c = project->timeline().findClip(*ids.begin());
    return jsonResult({{QStringLiteral("group"), c ? c->groupId : 0}});
}

QJsonObject McpServer::ToolCall::tool_duplicate_events()
{
    Timeline tl = project->timeline();
    const std::set<int> ids = idSet(a.value(QStringLiteral("event_ids")));
    std::vector<std::pair<int, Clip>> copies;
    TimeUs earliest = std::numeric_limits<TimeUs>::max();
    for (size_t ti = 0; ti < tl.tracks.size(); ++ti)
        for (const Clip& c : tl.tracks[ti].clips)
            if (ids.count(c.id)) {
                if (tl.tracks[ti].locked)
                    return errorResult(QStringLiteral("event %1 is on a locked track").arg(c.id));
                copies.emplace_back(int(ti), c);
                earliest = std::min(earliest, c.start);
            }
    if (copies.empty())
        return errorResult(QStringLiteral("give event_ids"));
    const TimeUs at = has("at") ? us(std::max(0.0, num("at"))) : w->m_timeline->cursorTime();
    std::map<int, int> groups;
    QJsonArray made;
    for (auto& [ti, c] : copies) {
        c.id = tl.allocId();
        c.start = at + (c.start - earliest);
        reassignEffectIds(c.effects, [&] { return tl.allocId(); });
        if (c.groupId) {
            auto it = groups.find(c.groupId);
            if (it == groups.end())
                it = groups.emplace(c.groupId, tl.allocId()).first;
            c.groupId = it->second;
        }
        made.append(c.id);
        tl.tracks[size_t(ti)].clips.push_back(c);
    }
    tl.sortClips();
    apply(tl, QStringLiteral("Paste"));
    return jsonResult({{QStringLiteral("event_ids"), made}});
}

QJsonObject McpServer::ToolCall::tool_paste_attributes()
{
    Timeline tl = project->timeline();
    int ti = -1;
    const Clip* from = tl.findClip(integer("from", -1), &ti);
    if (!from)
        return errorResult(QStringLiteral("no event %1").arg(integer("from", -1)));
    static const std::pair<const char*, unsigned> names[] = {
        {"effects", ops::AttrEffects}, {"pan_crop", ops::AttrPanCrop}, {"fades", ops::AttrFades}, {"transitions", ops::AttrTransitions},
        {"level", ops::AttrLevel},     {"speed", ops::AttrSpeed},      {"fixes", ops::AttrFixes}, {"label", ops::AttrLabel}};
    unsigned what = has("attributes") ? 0 : ops::AttrAll;
    for (const QJsonValue& v : a.value(QStringLiteral("attributes")).toArray()) {
        const auto it = std::find_if(std::begin(names), std::end(names), [&](const auto& n) { return v.toString() == QLatin1String(n.first); });
        if (it == std::end(names))
            return errorResult(QStringLiteral("attributes are effects, pan_crop, fades, transitions, level, speed, fixes and label"));
        what |= it->second;
    }
    const Clip source = *from;
    const int changed = ops::pasteAttributes(tl, project->media(), source, isVisualTrack(tl.tracks[size_t(ti)].type),
                                             idSet(a.value(QStringLiteral("event_ids"))), what, project->settings().fps);
    if (!changed)
        return errorResult(QStringLiteral("nothing changed: event_ids must be of the same kind as from (picture or sound), on unlocked tracks"));
    apply(tl, QStringLiteral("Paste Event Attributes"));
    return jsonResult({{QStringLiteral("changed"), changed}});
}

QJsonObject McpServer::ToolCall::tool_replace_media()
{
    Timeline tl = project->timeline();
    QString why;
    const int changed = ops::replaceMedia(tl, project->media(), idSet(a.value(QStringLiteral("event_ids"))), integer("media_id", -1),
                                          project->settings().fps, &why);
    if (!changed)
        return errorResult(why);
    apply(tl, QStringLiteral("Replace Media"));
    return jsonResult({{QStringLiteral("changed"), changed}});
}

QJsonObject McpServer::ToolCall::tool_separate_stems()
{
    const QString action = str("action");
    if (action == QLatin1String("status"))
        return jsonResult(w->stemJob()->status());
    if (action == QLatin1String("cancel")) {
        w->stemJob()->cancel();
        return jsonResult(w->stemJob()->status());
    }
    if (action != QLatin1String("start"))
        return errorResult(QStringLiteral("action is start, status or cancel"));
    const int stems = integer("stems", 2);
    if (stems != 2 && stems != 4)
        return errorResult(QStringLiteral("stems is 2 or 4"));
    QString error;
    if (!w->stemJob()->start(integer("event_id", -1), stems, a.value(QStringLiteral("best")).toBool(), &error))
        return errorResult(error);
    return jsonResult({{QStringLiteral("started"), true}, {QStringLiteral("note"), QStringLiteral("poll action status until done")}});
}

QJsonObject McpServer::ToolCall::tool_stabilize()
{
    Timeline tl = project->timeline();
    const Clip* clip = tl.findClip(integer("event_id", -1));
    const auto media = clip && !clip->isGenerated() ? project->mediaItem(clip->mediaId) : nullptr;
    if (!media || media->info.isStill || !media->info.videoStream(clip->streamIndex))
        return errorResult(QStringLiteral("give a video event (from a video file)"));
    const double amount = std::clamp(num("smoothing", 1.5), 0.0, 10.0);
    const QString path = media->path;
    const int stream = clip->streamIndex, clipId = clip->id, mediaId = media->id;
    if (amount > 0 && !stabilize::isAnalyzed(path, stream)) {
        // The analysis on a thread, the window kept going (as the menu's progress window does).
        std::atomic<bool> finished{false}, ok{false};
        QString error;
        std::thread worker([&] {
            ok = stabilize::analyze(path, stream, [](double) { return true; }, &error);
            finished = true;
        });
        while (!finished)
            QCoreApplication::processEvents(QEventLoop::AllEvents, 30);
        worker.join();
        if (!ok)
            return errorResult(QStringLiteral("couldn't analyse the footage: %1").arg(error));
    }
    tl = project->timeline();
    for (int id : ops::expandToGroups(tl, {clipId}))
        if (Clip* c = tl.findClip(id); c && c->mediaId == mediaId && c->streamIndex == stream) {
            c->stabilize = amount;
            c->stabilizeZoom = a.value(QStringLiteral("zoom")).toBool(true);
        }
    apply(tl, amount > 0 ? QStringLiteral("Stabilize") : QStringLiteral("Stabilize Off"));
    return jsonResult({{QStringLiteral("event_id"), clipId}, {QStringLiteral("smoothing"), amount}});
}

QJsonObject McpServer::ToolCall::tool_beat_montage()
{
    std::vector<std::shared_ptr<const MediaItem>> media;
    const std::set<int> wanted = idSet(a.value(QStringLiteral("media_ids")));
    for (const auto& [id, m] : project->media())
        if (m->info.hasVideo() && !m->info.isStill && (wanted.empty() || wanted.count(id)))
            media.push_back(m);
    if (media.empty())
        return errorResult(QStringLiteral("no video files to cut from (import_media first)"));
    Timeline tl = project->timeline();
    const TimeUs from = us(std::max(0.0, num("from", 0))), to = has("to") ? us(num("to")) : std::numeric_limits<TimeUs>::max();
    const int every = std::clamp(integer("every", 4), 1, 64);
    std::vector<TimeUs> markers;
    for (const Marker& m : tl.markers)
        if (!m.isRegion() && m.time >= from && m.time <= to)
            markers.push_back(m.time);
    std::sort(markers.begin(), markers.end());
    std::vector<TimeUs> cuts;
    for (size_t i = 0; i < markers.size(); i += size_t(every))
        cuts.push_back(markers[i]);
    if (cuts.size() < 2)
        return errorResult(QStringLiteral("put markers on the beat first (detect_beats): %1 marker(s) there, a cut every %2").arg(markers.size()).arg(every));
    const std::vector<int> made = ops::beatMontage(tl, media, cuts, a.value(QStringLiteral("shuffle")).toBool(true), uint32_t(integer("seed", 1)));
    if (made.empty())
        return errorResult(QStringLiteral("nothing was made"));
    apply(tl, QStringLiteral("Beat Montage"));
    QJsonArray ids;
    for (int id : made)
        ids.append(id);
    return jsonResult({{QStringLiteral("event_ids"), ids}, {QStringLiteral("cuts"), int(cuts.size())}});
}

QJsonObject McpServer::ToolCall::tool_media()
{
    const QString action = str("action");
    if (action == QLatin1String("proxies")) {
        QList<int> ids;
        for (int id : idSet(a.value(QStringLiteral("media_ids"))))
            ids << id;
        const bool heavyOnly = ids.isEmpty();
        if (heavyOnly)
            for (const auto& [id, m] : project->media())
                ids << id;
        w->createProxies(ids, heavyOnly, a.value(QStringLiteral("replace")).toBool());
        return jsonResult({{QStringLiteral("queued"), ids.size()}, {QStringLiteral("note"), QStringLiteral("proxies are made in the background; "
                                                                                                         "get_project shows proxy: true when ready")}});
    }
    const int id = integer("media_id", -1);
    const auto m = project->mediaItem(id);
    if (!m)
        return errorResult(QStringLiteral("no media %1 (get_project lists them)").arg(id));
    if (action == QLatin1String("relink")) {
        QString error;
        if (!project->relinkMedia(id, str("path"), &error))
            return errorResult(QStringLiteral("couldn't relink: %1").arg(error));
        w->refreshPreview();
        return jsonResult({{QStringLiteral("media_id"), id}, {QStringLiteral("path"), project->mediaItem(id)->path}});
    }
    if (action == QLatin1String("remove")) {
        if (!project->removeMedia(id))
            return errorResult(QStringLiteral("media %1 is used on the timeline: delete its events first").arg(id));
        return jsonResult({{QStringLiteral("removed"), id}});
    }
    if (action == QLatin1String("organize")) {
        const TimeUs in = has("mark_in") ? us(std::max(0.0, num("mark_in"))) : m->markIn;
        const TimeUs out = has("mark_out") ? us(std::max(0.0, num("mark_out"))) : m->markOut;
        if (out > 0 && out <= in)
            return errorResult(QStringLiteral("mark_out must be after mark_in"));
        project->setMediaDetails(id, has("folder") ? str("folder").trimmed() : m->folder, has("rating") ? std::clamp(integer("rating"), 0, 5) : m->rating,
                                 in, out, m->origin);
        const auto now = project->mediaItem(id);
        return jsonResult({{QStringLiteral("media_id"), id}, {QStringLiteral("folder"), now->folder}, {QStringLiteral("rating"), now->rating},
                           {QStringLiteral("mark_in"), sec(now->markIn)}, {QStringLiteral("mark_out"), sec(now->markOut)}});
    }
    return errorResult(QStringLiteral("action is relink, remove, organize or proxies"));
}

QJsonObject McpServer::ToolCall::tool_track_motion()
{
    const Timeline& tl = project->timeline();
    const Clip* clip = tl.findClip(integer("event_id", -1));
    if (!clip)
        return errorResult(QStringLiteral("no event %1").arg(integer("event_id", -1)));
    for (int i = 0; i < int(clip->effects.size()); ++i)
        if (clip->effects[size_t(i)].id == integer("effect_id", -1) && clip->effects[size_t(i)].type == QLatin1String("corner_pin")) {
            // Track Corners: on the shot under the event.
            const int clipId = clip->id, effectId = integer("effect_id", -1);
            const TimeUs from = has("from") ? us(num("from")) : std::clamp(w->m_timeline->cursorTime(), clip->start, clip->end() - 1);
            w->stopPlayback(false);
            w->seekTo(std::clamp(from, clip->start, clip->end() - 1));
            w->m_devices->setTarget({FxTarget::Kind::Clip, clipId});
            const int before = project->undoStack()->index();
            const QString said = w->m_devices->trackCorners(ChainPath{}, i, true);
            if (project->undoStack()->index() == before)
                return errorResult(said.isEmpty() ? QStringLiteral("nothing was tracked") : said);
            Timeline now = project->timeline();
            const EffectInstance* e = effectById(now, effectId);
            const auto x = e ? e->params.find(QStringLiteral("tlX")) : ParamMap::const_iterator{};
            return jsonResult({{QStringLiteral("effect_id"), effectId},
                               {QStringLiteral("keyframes"), e && x != e->params.end() ? int(x->second.keys.size()) : 0},
                               {QStringLiteral("note"), said}});
        }
    const auto media = clip->isGenerated() ? nullptr : project->mediaItem(clip->mediaId);
    if (!media || media->info.isStill || !media->info.videoStream(clip->streamIndex))
        return errorResult(QStringLiteral("Track Motion follows the footage of a video event"));
    if (clip->reverse || clip->hasRamp())
        return errorResult(QStringLiteral("Track Motion needs the event playing forwards at a steady speed (no reverse or speed ramp)"));
    int index = -1;
    for (int i = 0; i < int(clip->effects.size()); ++i)
        if (clip->effects[size_t(i)].id == integer("effect_id", -1))
            index = i;
    if (index < 0)
        return errorResult(QStringLiteral("effect %1 isn't on that event (only its own effects, not ones inside racks)").arg(integer("effect_id", -1)));
    const auto shape = clip->effects[size_t(index)].params.find(QStringLiteral("mask.shape"));
    if (shape == clip->effects[size_t(index)].params.end() || shape->second.value < 0.5)
        return errorResult(QStringLiteral("give the effect a mask first (add_effect / set_effect params mask.shape, mask.x, mask.y, mask.w, mask.h)"));
    const int clipId = clip->id, effectId = integer("effect_id", -1);
    const TimeUs from = has("from") ? us(num("from")) : std::clamp(w->m_timeline->cursorTime(), clip->start, clip->end() - 1);
    w->stopPlayback(false);
    w->seekTo(std::clamp(from, clip->start, clip->end() - 1));
    w->m_devices->setTarget({FxTarget::Kind::Clip, clipId});
    const int before = project->undoStack()->index();
    const QString said = w->m_devices->trackMask(ChainPath{}, index, true);
    if (project->undoStack()->index() == before)
        return errorResult(said.isEmpty() ? QStringLiteral("nothing was tracked") : said);
    Timeline now = project->timeline();
    const EffectInstance* e = effectById(now, effectId);
    const auto x = e ? e->params.find(QStringLiteral("mask.x")) : ParamMap::const_iterator{};
    return jsonResult({{QStringLiteral("effect_id"), effectId}, {QStringLiteral("keyframes"), e && x != e->params.end() ? int(x->second.keys.size()) : 0},
                       {QStringLiteral("note"), said}});
}

QJsonObject McpServer::ToolCall::tool_presets()
{
    const QString action = str("action");
    PresetLibrary& lib = PresetLibrary::instance();
    if (action == QLatin1String("list")) {
        QJsonArray out;
        for (const Preset& p : lib.all(a.value(QStringLiteral("audio")).toBool())) {
            bool match = true;
            for (const QString& word : str("search").split(QLatin1Char(' '), Qt::SkipEmptyParts))
                match = match && (p.name + QLatin1Char(' ') + p.category + QLatin1Char(' ') + p.effectType + QLatin1Char(' ') + p.description).contains(word, Qt::CaseInsensitive);
            if (!match)
                continue;
            QJsonObject o{{QStringLiteral("id"), p.id}, {QStringLiteral("name"), p.name}, {QStringLiteral("user"), p.user}};
            if (p.isChain()) {
                o[QStringLiteral("chain")] = true;
                o[QStringLiteral("category")] = p.category;
            } else {
                o[QStringLiteral("effect")] = p.effectType;
            }
            if (!p.description.isEmpty())
                o[QStringLiteral("description")] = p.description;
            out.append(o);
        }
        return jsonResult({{QStringLiteral("presets"), out}});
    }
    if (action == QLatin1String("remove")) {
        const auto p = lib.find(str("preset"));
        if (!p || !p->user)
            return errorResult(QStringLiteral("only saved (user) presets can be removed"));
        lib.remove(p->id);
        return jsonResult({{QStringLiteral("removed"), p->id}});
    }
    Timeline tl = project->timeline();
    if (action == QLatin1String("apply")) {
        const auto p = lib.find(str("preset"));
        if (!p)
            return errorResult(QStringLiteral("no preset \"%1\" (action list)").arg(str("preset")));
        if (has("effect_id")) {
            EffectInstance* e = effectById(tl, integer("effect_id"));
            if (!e)
                return errorResult(QStringLiteral("no effect %1").arg(integer("effect_id")));
            if (p->isChain() || p->effectType != e->type)
                return errorResult(QStringLiteral("that preset is for %1, not this effect (%2)").arg(p->isChain() ? QStringLiteral("a chain") : p->effectType, e->type));
            applyPresetTo(*p, *e, [&] { return tl.allocId(); });
            apply(tl, QStringLiteral("Apply Preset"));
            return jsonResult({{QStringLiteral("effect_id"), integer("effect_id")}, {QStringLiteral("preset"), p->name}});
        }
        bool audio = false;
        QString error;
        EffectChain* chain = chainFor(tl, &audio, &error);
        if (!chain)
            return errorResult(error);
        if (p->audio != audio)
            return errorResult(p->audio ? QStringLiteral("that's an audio preset: that chain is for pictures") : QStringLiteral("that's a video preset: that chain is for sound"));
        QJsonArray ids;
        for (EffectInstance& e : instantiatePreset(*p, [&] { return tl.allocId(); })) {
            ids.append(e.id);
            chain->push_back(std::move(e));
        }
        apply(tl, QStringLiteral("Add Preset"));
        return jsonResult({{QStringLiteral("effect_ids"), ids}, {QStringLiteral("preset"), p->name}});
    }
    if (action == QLatin1String("save")) {
        const QString name = str("name").trimmed();
        if (name.isEmpty())
            return errorResult(QStringLiteral("give the preset a name"));
        EffectChain chain;
        bool audio = false;
        QString type;
        if (has("effect_id")) {
            const EffectInstance* e = effectById(tl, integer("effect_id"));
            if (!e)
                return errorResult(QStringLiteral("no effect %1").arg(integer("effect_id")));
            chain = {*e};
            type = e->isRack() ? QString() : e->type;
            audio = e->type.startsWith(QLatin1String("a_")) || (e->isRack() && !e->chains.empty() && !e->chains.front().effects.empty()
                                                                 && e->chains.front().effects.front().type.startsWith(QLatin1String("a_")));
        } else {
            QString error;
            const EffectChain* c = chainFor(tl, &audio, &error);
            if (!c)
                return errorResult(error);
            if (c->empty())
                return errorResult(QStringLiteral("that chain has no effects"));
            chain = *c;
        }
        QString error;
        const QString id = lib.save(name, audio, type, chain, &error);
        if (id.isEmpty())
            return errorResult(QStringLiteral("couldn't save: %1").arg(error));
        return jsonResult({{QStringLiteral("preset"), id}, {QStringLiteral("name"), name}, {QStringLiteral("chain"), type.isEmpty()}});
    }
    return errorResult(QStringLiteral("action is list, apply, save or remove"));
}

QJsonObject McpServer::ToolCall::tool_normalize_audio()
{
    const std::set<int> ids = idSet(a.value(QStringLiteral("event_ids")));
    const Timeline& tl = project->timeline();
    std::set<int> audio;
    for (const Track& t : tl.tracks)
        if (t.type == TrackType::Audio)
            for (const Clip& c : t.clips)
                if (ids.count(c.id))
                    audio.insert(c.id);
    if (audio.empty())
        return errorResult(QStringLiteral("give audio events"));
    const int before = project->undoStack()->index();
    const std::set<int> selection = w->m_timeline->selection();
    w->m_timeline->selectClips(audio);
    w->m_timeline->normalizeSelection();
    w->m_timeline->selectClips(selection);
    QJsonArray gains;
    for (int id : audio)
        if (const Clip* c = project->timeline().findClip(id); c && !c->effects.empty() && c->effects.front().type == QLatin1String("a_gain")) {
            // (a silent event is left as it was, and its Gain, from a damaged project, may have no gain)
            const auto& p = c->effects.front().params;
            if (const auto g = p.find(QStringLiteral("gain")); g != p.end())
                gains.append(QJsonObject{{QStringLiteral("event_id"), id}, {QStringLiteral("gain_db"), g->second.value}});
        }
    return jsonResult({{QStringLiteral("normalized"), project->undoStack()->index() != before}, {QStringLiteral("gains"), gains}});
}

QJsonObject McpServer::ToolCall::tool_youtube_clip()
{
    YouTubeOptions o;
    o.url = str("url").trimmed();
    if (!o.url.startsWith(QLatin1String("http")))
        return errorResult(QStringLiteral("give the video's link (https://...)"));
    o.maxHeight = std::clamp(integer("quality", 1080), 0, 2160);
    o.folder = w->youtubeFolder();
    o.jsRuntime = youtubeJsRuntime();
    o.cookiesBrowser = str("browser").toLower();
    const TimeUs from = has("from") ? us(std::max(0.0, num("from"))) : 0, to = has("to") ? us(num("to")) : -1;
    if (to >= 0 && to <= from)
        return errorResult(QStringLiteral("to must be after from"));
    // yt-dlp: fetched once.
    const QString ytdlp = ytdlpPath();
    QString error;
    if (!QFileInfo::exists(ytdlp)) {
        if (!w->downloadFile(w, ytdlpDownloadUrl(), ytdlp, QStringLiteral("yt-dlp"), &error))
            return errorResult(QStringLiteral("yt-dlp (the downloader) could not be fetched: %1").arg(error));
        QFile(ytdlp).setPermissions(QFile(ytdlp).permissions() | QFileDevice::ExeOwner | QFileDevice::ExeUser);
    }
    // The download on a thread, the window kept going.
    std::atomic<bool> finished{false};
    QString path;
    std::thread worker([&] {
        path = youtubeDownload(ytdlp, ffmpegFolder(), o, [](double) { return true; }, &error);
        finished = true;
    });
    while (!finished)
        QCoreApplication::processEvents(QEventLoop::AllEvents, 50);
    worker.join();
    if (path.isEmpty())
        return errorResult(QStringLiteral("the download failed: %1").arg(youtubeFailureHelp(error)));
    int mediaId = 0;
    if (a.value(QStringLiteral("place")).toBool(true)) {
        if (has("at"))
            w->seekTo(us(std::max(0.0, num("at"))));
        mediaId = w->placeDownloadedClip(path, from, to);
    } else {
        w->importFiles({path});
        for (const auto& [id, m] : project->media())
            if (QFileInfo(m->path) == QFileInfo(path))
                mediaId = id;
        if (mediaId && (from > 0 || to >= 0)) {
            const auto m = project->mediaItem(mediaId);
            project->setMediaDetails(mediaId, QStringLiteral("YouTube"), m->rating, from, to >= 0 ? std::min(to, m->info.duration) : 0, QStringLiteral("youtube"));
        }
    }
    return jsonResult({{QStringLiteral("media_id"), mediaId}, {QStringLiteral("path"), path}, {QStringLiteral("placed"), a.value(QStringLiteral("place")).toBool(true)}});
}

QJsonObject McpServer::ToolCall::tool_match_colour()
{
    Timeline tl = project->timeline();
    std::set<int> ids;
    for (const QJsonValue v : a.value(QStringLiteral("event_ids")).toArray())
        ids.insert(v.toInt());
    const TimeUs at = has("time") ? us(num("time")) : w->m_timeline->cursorTime();
    QString error;
    const bool autoColour = a.value(QStringLiteral("auto")).toBool();
    const int n = autoColour ? colormatch::autoColourEvents(tl, *project->snapshot(), ids, at, &error)
                             : colormatch::match(tl, *project->snapshot(), ids, at, &error);
    if (!n)
        return errorResult(error);
    apply(tl, autoColour ? QStringLiteral("Auto Colour") : QStringLiteral("Match Colour"));
    return jsonResult({{QStringLiteral("matched"), n}, {QStringLiteral("reference"), sec(at)}});
}

QJsonObject McpServer::ToolCall::tool_multicam()
{
    const QString action = str("action");
    std::vector<int> ids;
    for (const QJsonValue v : a.value(QStringLiteral("event_ids")).toArray())
        ids.push_back(v.toInt());
    Timeline tl = project->timeline();
    QString error;
    if (action == QLatin1String("sync")) {
        const audiosync::Result r = audiosync::syncByAudio(tl, project->media(), ids);
        if (!r.error.isEmpty())
            return errorResult(r.error);
        if (!r.moved.empty())
            apply(tl, QStringLiteral("Sync by Audio"));
        QJsonObject moved;
        for (const auto& [id, delta] : r.moved)
            moved[QString::number(id)] = sec(delta);
        QJsonArray unmatched;
        for (int id : r.unmatched)
            unmatched.append(id);
        return jsonResult({{QStringLiteral("moved_seconds"), moved}, {QStringLiteral("unmatched"), unmatched}});
    }
    if (action == QLatin1String("create") || action == QLatin1String("cut") || action == QLatin1String("switch")) {
        const bool ok = action == QLatin1String("create") ? multicam::create(tl, project->media(), ids, &error)
                        : action == QLatin1String("cut")  ? multicam::cut(tl, project->media(), integer("angle"), us(num("time")), &error)
                                                          : multicam::switchAngle(tl, project->media(), integer("angle"), us(num("time")), &error);
        if (!ok)
            return errorResult(error);
        if (!(tl == project->timeline()))
            apply(tl, action == QLatin1String("create") ? QStringLiteral("Make Multicam") : QStringLiteral("Multicam: Angle %1").arg(integer("angle")));
    } else if (action != QLatin1String("angles")) {
        return errorResult(QStringLiteral("action: sync, create, cut, switch or angles"));
    }
    const Timeline& now = project->timeline();
    QJsonArray angles, cuts;
    for (const auto& ang : multicam::angles(now, project->media()))
        angles.append(QJsonObject{{QStringLiteral("angle"), ang.number}, {QStringLiteral("event_id"), ang.clipId}, {QStringLiteral("name"), ang.name}});
    if (const int mi = multicam::trackIndex(now); mi >= 0)
        for (const Clip& c : now.tracks[size_t(mi)].clips)
            cuts.append(QJsonObject{{QStringLiteral("angle"), multicam::angleOf(c)}, {QStringLiteral("start"), sec(c.start)}, {QStringLiteral("end"), sec(c.end())},
                                    {QStringLiteral("event_id"), c.id}});
    return jsonResult({{QStringLiteral("track"), multicam::trackIndex(now)}, {QStringLiteral("angles"), angles}, {QStringLiteral("cuts"), cuts}});
}

QJsonObject McpServer::ToolCall::tool_cut_time()
{
    const QString action = str("action");
    const TimeUs from = us(std::max(0.0, num("from"))), to = has("to") ? us(num("to")) : project->timeline().duration();
    std::vector<cuts::Range> ranges;
    if (action == QLatin1String("insert")) {
        const double length = num("length");
        if (!(length > 0) || length > 3600)
            return errorResult(QStringLiteral("length: 0 to 3600 seconds"));
        Timeline tl = project->timeline();
        const TimeUs added = cuts::insertTime(tl, has("at") ? us(std::max(0.0, num("at"))) : w->m_timeline->cursorTime(), us(length), ps.fps);
        apply(tl, QStringLiteral("Insert Time"));
        return jsonResult({{QStringLiteral("inserted_seconds"), sec(added)}});
    }
    if (action == QLatin1String("silences")) {
        if (to <= from)
            return errorResult(QStringLiteral("nothing to listen to there"));
        const std::vector<float> audio = timelineSpeechAudio(*project->snapshot(), from, to);
        ranges = cuts::silences(audio, 16000, from, std::clamp(num("threshold_db", -40), -90.0, -5.0), us(std::max(0.05, num("min_length", 0.7))),
                                us(std::clamp(num("keep", 0.15), 0.0, 2.0)));
    } else if (action == QLatin1String("ranges")) {
        for (const QJsonValue v : a.value(QStringLiteral("ranges")).toArray()) {
            const QJsonArray r = v.toArray();
            // Only what is on the timeline: a range past the end was counted as removed time.
            const TimeUs end = project->timeline().duration(), from = std::max<TimeUs>(0, us(r.size() == 2 ? r[0].toDouble() : 0)),
                         to = std::min(end, us(r.size() == 2 ? r[1].toDouble() : 0));
            if (r.size() == 2 && to > from)
                ranges.push_back({from, to});
        }
    } else if (action == QLatin1String("transcribe")) {
        static const std::map<QString, QString> files = {{QStringLiteral("tiny"), QStringLiteral("ggml-tiny.bin")},
                                                         {QStringLiteral("base"), QStringLiteral("ggml-base.bin")},
                                                         {QStringLiteral("small"), QStringLiteral("ggml-small.bin")}};
        const auto file = files.find(has("model") ? str("model") : QStringLiteral("base"));
        if (file == files.end())
            return errorResult(QStringLiteral("model: tiny, base or small"));
        const QString path = MainWindow::whisperModelPath(file->second);
        if (!QFileInfo::exists(path))
            return errorResult(QStringLiteral("the %1 model isn't on this computer: download it once with Tools > Captions > Auto Captions").arg(file->first));
        if (to <= from)
            return errorResult(QStringLiteral("nothing to listen to there"));
        TranscribeOptions o;
        o.wordTimestamps = true;
        std::vector<Caption> found;
        QString error;
        if (!w->transcribeTimeline(path, o, from, to, nullptr, found, &error))
            return errorResult(error);
        QJsonArray words;
        for (const Caption& c : found)
            if (!c.text.simplified().isEmpty())
                words.append(QJsonObject{{QStringLiteral("start"), sec(c.start + from)}, {QStringLiteral("end"), sec(c.end + from)},
                                         {QStringLiteral("text"), c.text.simplified()}});
        return jsonResult({{QStringLiteral("words"), words}});
    } else if (action == QLatin1String("words")) {
        std::vector<Caption> words;
        for (const QJsonValue v : a.value(QStringLiteral("words")).toArray()) {
            const QJsonObject o = v.toObject();
            words.push_back({us(o.value(QStringLiteral("start")).toDouble()), us(o.value(QStringLiteral("end")).toDouble()),
                             o.value(QStringLiteral("text")).toString(), 0});
        }
        std::vector<bool> removed(words.size(), false);
        for (const QJsonValue v : a.value(QStringLiteral("remove")).toArray())
            if (v.toInt() >= 0 && v.toInt() < int(words.size()))
                removed[size_t(v.toInt())] = true;
        ranges = cuts::wordCuts(words, removed);
    } else {
        return errorResult(QStringLiteral("action: silences, ranges, transcribe, words or insert"));
    }
    if (ranges.empty())
        return jsonResult({{QStringLiteral("removed_seconds"), 0}, {QStringLiteral("ranges"), QJsonArray()}});
    QJsonArray list;
    for (const cuts::Range& r : ranges)
        list.append(QJsonArray{sec(r.from), sec(r.to)});
    Timeline tl = project->timeline();
    const TimeUs removed = cuts::removeRanges(tl, ranges, ps.fps);
    apply(tl, action == QLatin1String("silences") ? QStringLiteral("Remove Silences")
              : action == QLatin1String("words")  ? QStringLiteral("Edit by Transcript")
                                                  : QStringLiteral("Cut Time"));
    return jsonResult({{QStringLiteral("removed_seconds"), sec(removed)}, {QStringLiteral("ranges"), list}});
}

QJsonObject McpServer::ToolCall::tool_auto_reframe()
{
    Timeline tl = project->timeline();
    std::vector<int> events;
    for (const QJsonValue v : a.value(QStringLiteral("event_ids")).toArray())
        if (tl.findClip(v.toInt()))
            events.push_back(v.toInt());
    if (events.empty())
        return errorResult(QStringLiteral("give video events"));
    ProjectSettings settings = ps;
    if (a.value(QStringLiteral("portrait")).toBool() && settings.width >= settings.height) {
        settings.width = 1080;
        settings.height = 1920;
        project->setSettings(settings);
    }
    const int framed = reframe::apply(tl, project->snapshot()->media, events, QSize(settings.width, settings.height));
    if (framed > 0)
        apply(tl, QStringLiteral("Auto Reframe"));
    return jsonResult({{QStringLiteral("reframed"), std::max(0, framed)},
                       {QStringLiteral("frame"), QStringLiteral("%1x%2").arg(settings.width).arg(settings.height)}});
}

QJsonObject McpServer::ToolCall::tool_deliver()
{
    DeliverView* deliver = w->m_deliver;
    if (!deliver)
        return errorResult(QStringLiteral("the Deliver tab isn't there"));
    const QString action = str("action");
    auto strings = [&](const char* key) {
        QStringList out;
        for (const auto& v : a.value(QLatin1String(key)).toArray())
            if (v.isString() && !v.toString().trimmed().isEmpty())
                out << v.toString();
        return out;
    };
    // A listed output by words from its name (or the template they match).
    auto listed = [&](const QString& words, QString* error) -> QString {
        const QStringList current = deliver->outputs();
        if (current.contains(words))
            return words;
        if (const ExportPreset* p = matchPreset(words); p && current.contains(p->name))
            return p->name;
        for (const QString& name : current) {
            bool all = true;
            for (const QString& word : words.split(QLatin1Char(' '), Qt::SkipEmptyParts))
                all = all && name.contains(word, Qt::CaseInsensitive);
            if (all)
                return name;
        }
        *error = QStringLiteral("no output matches \"%1\" (outputs: %2)").arg(words, current.join(QStringLiteral(", ")));
        return {};
    };
    if (action == QLatin1String("check")) {
        deliver->refresh();
        if (has("loudness_target")) {
            auto* target = deliver->findChild<QComboBox*>(QStringLiteral("deliverTarget"));
            const int i = integer("loudness_target");
            if (!target || i < 0 || i >= target->count())
                return errorResult(QStringLiteral("loudness_target: 0 to %1").arg(target ? target->count() - 1 : 0));
            target->setCurrentIndex(i);
        }
        if (a.value(QStringLiteral("measure_loudness")).toBool())
            deliver->measureLoudness();
        return jsonResult(deliver->report());
    }
    if (action == QLatin1String("outputs")) {
        deliver->refresh();
        QStringList list = deliver->outputs();
        for (const QString& words : strings("add")) {
            const ExportPreset* p = matchPreset(words);
            if (!p)
                return errorResult(QStringLiteral("no Render As template matches \"%1\"").arg(words));
            if (!list.contains(p->name))
                list << p->name;
        }
        if (has("pack")) {
            bool known = false;
            for (const auto& pk : DeliverView::packs())
                if (pk.id == str("pack")) {
                    known = true;
                    for (const QString& n : pk.templates)
                        if (!list.contains(n))
                            list << n;
                }
            if (!known)
                return errorResult(QStringLiteral("pack: youtube, social, audio, masters or share"));
        }
        if (list != deliver->outputs())
            deliver->setOutputs(list);
        QString error;
        for (const QString& words : strings("remove")) {
            const QString name = listed(words, &error);
            if (name.isEmpty())
                return errorResult(error);
            list.removeAll(name);
            deliver->setOutputs(list);
        }
        if (has("ticked")) {
            QStringList on;
            for (const QString& words : strings("ticked")) {
                const QString name = listed(words, &error);
                if (name.isEmpty())
                    return errorResult(error);
                on << name;
            }
            for (const QString& name : deliver->outputs())
                deliver->setTicked(name, on.contains(name));
        }
        if (has("trim")) {
            const QJsonObject trim = a.value(QStringLiteral("trim")).toObject();
            for (auto it = trim.begin(); it != trim.end(); ++it) {
                const QString name = listed(it.key(), &error);
                if (name.isEmpty())
                    return errorResult(error);
                const double seconds = it.value().toDouble();
                if (!std::isfinite(seconds) || seconds < 0 || seconds > 86400)
                    return errorResult(QStringLiteral("trim: seconds from 0 (the whole edit) to 86400"));
                deliver->setTrim(name, us(seconds));
            }
        }
        if (has("folder")) {
            if (str("folder").trimmed().isEmpty())
                return errorResult(QStringLiteral("folder: give a folder"));
            deliver->setFolder(QFileInfo(str("folder")).absoluteFilePath());
        }
        return jsonResult(deliver->report());
    }
    if (action == QLatin1String("render")) {
        QString error;
        const int queued = deliver->renderOutputs(&error);
        if (queued == 0)
            return errorResult(error.isEmpty() ? QStringLiteral("no output is ticked (deliver outputs)") : error);
        QJsonObject result{{QStringLiteral("queued"), queued}, {QStringLiteral("queue"), w->renderQueue()->status()}};
        if (!error.isEmpty())
            result[QStringLiteral("refused")] = error;
        return jsonResult(result);
    }
    if (action == QLatin1String("pack")) {
        deliver->refresh();
        auto text = [&](const char* key) { return has(key) ? std::optional<QString>(str(key)) : std::nullopt; };
        deliver->setPackText(text("title"), text("description"), text("tags"));
        if (has("thumbnail_time")) {
            const double t = num("thumbnail_time");
            if (!std::isfinite(t) || t < 0 || us(t) > project->timeline().duration())
                return errorResult(QStringLiteral("thumbnail_time: seconds inside the edit"));
            deliver->pickThumbnail(us(t));
        }
        QJsonObject result = deliver->report();
        if (a.value(QStringLiteral("save")).toBool()) {
            QString error;
            if (!deliver->savePack(&error))
                return errorResult(error);
            result[QStringLiteral("saved_in")] = result[QStringLiteral("folder")];
        }
        return jsonResult(result);
    }
    if (action == QLatin1String("history")) {
        deliver->refresh();
        return jsonResult({{QStringLiteral("renders"), deliver->historyJson()}});
    }
    return errorResult(QStringLiteral("action: check, outputs, render, pack or history"));
}

QJsonObject McpServer::ToolCall::tool_youtube_publish()
{
    const QString action = str("action");
    if (action == QLatin1String("chapters")) {
        QStringList warnings;
        const QString text = upload::youtubeChapters(project->timeline(), &warnings);
        return jsonResult({{QStringLiteral("chapters"), text}, {QStringLiteral("warnings"), QJsonArray::fromStringList(warnings)}});
    }
    if (action == QLatin1String("track_listing")) {
        const QString text = upload::trackListing(*project, integer("track", -1));
        if (text.isEmpty())
            return errorResult(QStringLiteral("no events on that track"));
        return jsonResult({{QStringLiteral("listing"), text}});
    }
    if (action == QLatin1String("thumbnail")) {
        const QString path = QFileInfo(str("path")).absoluteFilePath();
        if (str("path").isEmpty())
            return errorResult(QStringLiteral("give a path"));
        const TimeUs t = has("time") ? us(std::max(0.0, num("time"))) : w->m_timeline->cursorTime();
        Compositor renderer(true);
        const QImage image = upload::thumbnail(renderer.render(*project->snapshot(), t, QSize(ps.width, ps.height)), str("title"));
        bool saved = false;
        for (int quality = 92; quality >= 50 && !saved; quality -= 10)
            saved = image.save(path, nullptr, quality) && QFileInfo(path).size() <= 2 * 1024 * 1024;
        if (!saved)
            return errorResult(QStringLiteral("could not write %1 under 2 MB").arg(path));
        return jsonResult({{QStringLiteral("path"), path}, {QStringLiteral("size"), QStringLiteral("1280x720")}});
    }
    return errorResult(QStringLiteral("action: chapters, track_listing or thumbnail"));
}

QJsonObject McpServer::ToolCall::tool_render_queue()
{
    const QString action = str("action");
    RenderQueue* queue = w->renderQueue();
    if (action == QLatin1String("start")) {
        queue->start();
        return jsonResult(queue->status());
    }
    if (action == QLatin1String("stop")) {
        queue->stop();
        return jsonResult(queue->status());
    }
    if (action == QLatin1String("status"))
        return jsonResult(queue->status());
    if (action != QLatin1String("add"))
        return errorResult(QStringLiteral("action: add, start, stop or status"));
    if (str("path").isEmpty())
        return errorResult(QStringLiteral("give a path"));
    std::shared_ptr<const ProjectSnapshot> snap = project->snapshot();
    QString label = project->filePath().isEmpty() ? QStringLiteral("Untitled") : QFileInfo(project->filePath()).completeBaseName();
    QString projectFile = project->filePath();
    if (has("project_file")) {
        Project other;
        QString error;
        if (!other.load(str("project_file"), &error))
            return errorResult(error);
        snap = other.snapshot();
        label = QFileInfo(str("project_file")).completeBaseName();
        projectFile = QFileInfo(str("project_file")).absoluteFilePath();
    }
    const ExportPreset* chosen = matchPreset(has("preset") ? str("preset") : QStringLiteral("MP4 H.264"), str("path"));
    if (!chosen)
        return errorResult(QStringLiteral("no Render As template matches \"%1\"").arg(str("preset")));
    if (!chosen->extension.isEmpty() && QFileInfo(str("path")).suffix().compare(chosen->extension, Qt::CaseInsensitive) != 0)
        return errorResult(QStringLiteral("\"%1\" writes .%2 files: give a path ending in .%2").arg(chosen->name, chosen->extension));
    ExportSettings s = settingsFromPreset(*chosen, snap->settings);
    if (a.value(QStringLiteral("hardware_encoding")).toBool(true))
        s = withGpuEncoder(s);
    s.path = QFileInfo(str("path")).absoluteFilePath();
    s.rangeStart = us(std::max(0.0, num("from")));
    s.rangeEnd = has("to") ? us(num("to")) : snap->timeline.duration();
    if (s.rangeEnd <= s.rangeStart)
        return errorResult(QStringLiteral("nothing to render there"));
    QString error;
    if (!queue->add(label, snap, s, projectFile, &error))
        return errorResult(error);
    return jsonResult(queue->status());
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
        {QStringLiteral("edit_track"), {&ToolCall::tool_edit_track, false}},
        {QStringLiteral("freeze_frame"), {&ToolCall::tool_freeze_frame, false}},
        {QStringLiteral("new_project"), {&ToolCall::tool_new_project, false}},
        {QStringLiteral("open_project"), {&ToolCall::tool_open_project, false}},
        {QStringLiteral("delete_track"), {&ToolCall::tool_delete_track, false}},
        {QStringLiteral("scene_changes"), {&ToolCall::tool_scene_changes, false}},
        {QStringLiteral("match_colour"), {&ToolCall::tool_match_colour, false}},
        {QStringLiteral("node_effect"), {&ToolCall::tool_node_effect, false}},
        {QStringLiteral("envelope"), {&ToolCall::tool_envelope, false}},
        {QStringLiteral("group"), {&ToolCall::tool_group, false}},
        {QStringLiteral("duplicate_events"), {&ToolCall::tool_duplicate_events, false}},
        {QStringLiteral("paste_attributes"), {&ToolCall::tool_paste_attributes, false}},
        {QStringLiteral("replace_media"), {&ToolCall::tool_replace_media, false}},
        {QStringLiteral("separate_stems"), {&ToolCall::tool_separate_stems, false}},
        {QStringLiteral("stabilize"), {&ToolCall::tool_stabilize, false}},
        {QStringLiteral("beat_montage"), {&ToolCall::tool_beat_montage, false}},
        {QStringLiteral("media"), {&ToolCall::tool_media, false}},
        {QStringLiteral("track_motion"), {&ToolCall::tool_track_motion, false}},
        {QStringLiteral("presets"), {&ToolCall::tool_presets, false}},
        {QStringLiteral("normalize_audio"), {&ToolCall::tool_normalize_audio, false}},
        {QStringLiteral("youtube_clip"), {&ToolCall::tool_youtube_clip, false}},
        {QStringLiteral("cut_time"), {&ToolCall::tool_cut_time, false}},
        {QStringLiteral("auto_reframe"), {&ToolCall::tool_auto_reframe, false}},
        {QStringLiteral("youtube_publish"), {&ToolCall::tool_youtube_publish, false}},
        {QStringLiteral("deliver"), {&ToolCall::tool_deliver, false}},
        {QStringLiteral("render_queue"), {&ToolCall::tool_render_queue, false}},
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
        {QStringLiteral("remove_markers"), {&ToolCall::tool_remove_markers, false}},
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
        {QStringLiteral("generate_dialogue"), {&ToolCall::tool_generate_dialogue, false}},
        {QStringLiteral("list_sound_effects"), {&ToolCall::tool_list_sound_effects, false}},
        {QStringLiteral("user_guide"), {&ToolCall::tool_user_guide, false}},
        {QStringLiteral("multicam"), {&ToolCall::tool_multicam, false}},
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
    // An argument the tool doesn't take is refused, not ignored: a misspelt or invented one would otherwise do nothing and the call
    // something else than meant.
    static const QHash<QString, QJsonObject> takes = [] {
        QHash<QString, QJsonObject> h;
        for (const auto& t : toolList())
            h[t[QStringLiteral("name")].toString()] = t[QStringLiteral("inputSchema")][QStringLiteral("properties")].toObject();
        return h;
    }();
    const QJsonObject properties = takes.value(name);
    for (auto arg = a.begin(); arg != a.end(); ++arg)
        if (!properties.contains(arg.key()))
            return errorResult(QStringLiteral("%1 doesn't take \"%2\" (it takes: %3)")
                                   .arg(name, arg.key(), properties.isEmpty() ? QStringLiteral("nothing") : properties.keys().join(QStringLiteral(", "))));
    LiveView* live = w->m_live;
    if (it->live && !live)
        return errorResult(QStringLiteral("no Live tab"));
    ToolCall call{*this, name, a, w, w->m_project, w->m_project->settings(), live, live ? live->m_pads : nullptr};
    const QJsonObject result = (call.*(it->tool))();
    return call.refusal.isEmpty() ? result : errorResult(call.refusal);
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
