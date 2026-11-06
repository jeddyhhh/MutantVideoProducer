#pragma once

// User-made video effects (Tools > Effect Builder): a graph of nodes, each a picture operation with inputs, an output
// and settings, wired from a Source node (the picture the effect is on) to an Output node. Every built-in video
// effect is a node, every background or text generator is a source node, and there are nodes of their own to blend,
// mask, displace, key, split and combine channels, threshold and delay.
//
// A saved graph (a .mvpfx file in userEffectsFolder()) becomes a video effect in the "User Created" category
// ("user.<name>"). The settings chosen to show ("exposed") are its settings, keyframes and all; the graph itself rides
// along in a hidden setting, so a project that uses it renders the same on another computer.

#include "core/Effects.h"
#include <QImage>
#include <QJsonObject>
#include <QObject>
#include <QPointF>
#include <functional>
#include <map>
#include <memory>
#include <vector>

namespace composite {
struct Signal;
}

namespace nodes {

// The composite video signals the Composite Signal nodes made this frame, by node id: they pass the signal itself
// between them this way (the picture each gives back is only a view of it).
using SignalBus = std::map<int, std::shared_ptr<const composite::Signal>>;

struct Node {
    int id = 0;
    QString type;            // NodeDef::id
    ParamMap params;
    QPointF pos;             // where it sits in the editor
};

// A wire from a node's output to input `port` of node `to`.
struct Link {
    int from = 0;
    int to = 0;
    int port = 0;
    bool operator==(const Link&) const = default;
};

// A node setting shown as a setting of the saved effect.
struct Exposed {
    QString id;              // the effect's setting id
    QString name;
    int node = 0;
    QString param;
};

struct Graph {
    QString name;
    QString description;
    std::vector<Node> nodes;
    std::vector<Link> links;
    std::vector<Exposed> exposed;
    int nextId = 1;

    Node* find(int id);
    const Node* find(int id) const;
    // The link into input `port` of node `to`, or null.
    const Link* input(int to, int port) const;
    int addNode(const QString& type, QPointF pos);  // with its default settings; returns its id (0: unknown type)
    void removeNode(int id);                         // with its links and exposed settings
    // Wires from -> to's input `port` (replacing what was wired there). False if that would make a loop.
    bool connect(int from, int to, int port);
};

// A new graph: Source wired straight to Output.
Graph starter();

QJsonObject toJson(const Graph& g);
Graph fromJson(const QJsonObject& o);

struct NodeContext {
    QSize size;              // the picture's size
    RenderContext render;    // time, scale (pixel sizes and generators follow it), fps
    SignalBus* bus = nullptr;    // this frame's signals (null: none kept)
    int node = 0;            // the node being run
    std::vector<int> inputs; // the node wired into each of its inputs (0: none)
    bool pictureWanted = true; // false: only Composite Signal nodes read its picture, and no thumbnails are drawn
};

// Whether node `id`'s picture is looked at (thumbnails, or a node that isn't a Composite Signal one wired from it):
// a signal node's picture is only a view of its signal, not worth drawing for the signal nodes after it.
bool pictureWanted(const Graph& g, int id, bool thumbnails);

struct NodeDef {
    QString id;
    QString name;
    QString category;
    QString description;
    QStringList inputs;      // their names; a source node has none
    bool output = true;      // false only for the Output node
    std::vector<ParamDef> params;
    // The node's picture from its inputs (null where nothing is wired: a transparent picture) and settings.
    std::function<QImage(const std::vector<const QImage*>& in, const Params&, const ParamMap& raw, const NodeContext&,
                         std::unique_ptr<EffectState>& state)>
        eval;
};

const std::vector<NodeDef>& nodeDefs();
const NodeDef* findNode(const QString& id);

// Runs graphs. Keeps each node's running state (delays, effects that remember the frames before) between frames.
class Evaluator {
public:
    // The Output node's picture for `source`. With keepOutputs, each node's picture is kept in `outputs` (the editor's
    // thumbnails). `error` says what's wrong (a node type this build doesn't have, no Output); the source comes back then.
    QImage run(const Graph& g, const QImage& source, const RenderContext& ctx, bool keepOutputs = false);
    std::map<int, QImage> outputs;
    QString error;

private:
    struct State {
        QString type;
        std::unique_ptr<EffectState> state;
    };
    std::map<int, State> m_states;
};

// What keeps a graph from drawing (no Output node, a node this version doesn't have); empty when nothing.
QString problem(const Graph& g);

// The graph a user effect's settings hold (its hidden "graph" setting, parsed again only when it changed: `text` keeps
// what `graph` was parsed from), with the effect's shown settings put into their nodes. For the renderers.
void graphFor(const Params& p, QString& text, Graph& graph);

// ---- Saved effects ----
QString userEffectsFolder();
// The example effects that come with the program (the effects folder next to it), under User Created/Examples.
QString examplesFolder();
bool save(const Graph& g, const QString& path, QString* error);
bool load(const QString& path, Graph* g, QString* error);
QString effectId(const QString& name); // "user.<name as an id>"
// Registers (or updates) the graph as a video effect; returns it.
const EffectDef* registerEffect(const Graph& g);
// The examples', then the user's own (in userEffectsFolder()), at startup. One the user saved under an example's
// name takes its place.
void loadUserEffects();
// The registered user effects (the latest of each).
std::vector<const EffectDef*> userEffects();
const EffectDef* findUserEffect(const QString& id);
// A project's instance of a user effect this computer doesn't have: registered from the graph it carries.
void registerFromInstance(const EffectInstance& e);

// Says when user effects are added or changed (the effect lists refresh).
class Notifier : public QObject {
    Q_OBJECT
public:
    static Notifier& instance();
signals:
    void changed();
};

} // namespace nodes
