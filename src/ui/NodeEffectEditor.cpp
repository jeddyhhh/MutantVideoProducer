#include "ui/NodeEffectEditor.h"
#include "core/FrameConverter.h"
#include "core/GpuCompositor.h"
#include "core/Generators.h"
#include "core/MediaInfo.h"
#include "core/VideoDecoder.h"
#include "ui/Help.h"
#include "ui/Theme.h"

#include <QApplication>
#include <QCheckBox>
#include <QCloseEvent>
#include <QColorDialog>
#include <QComboBox>
#include <QDialogButtonBox>
#include <QDoubleSpinBox>
#include <QDrag>
#include <QElapsedTimer>
#include <QFileDialog>
#include <QFileInfo>
#include <QFontComboBox>
#include <QFontMetricsF>
#include <QFormLayout>
#include <QGraphicsPathItem>
#include <QGraphicsScene>
#include <QGraphicsSceneMouseEvent>
#include <QGraphicsView>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QJsonDocument>
#include <QLabel>
#include <QLineEdit>
#include <QMenu>
#include <QMessageBox>
#include <QMimeData>
#include <QOffscreenSurface>
#include <QPainter>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QScrollArea>
#include <QScrollBar>
#include <QShortcut>
#include <QSlider>
#include <QSplitter>
#include <QTimer>
#include <QTreeWidget>
#include <QVBoxLayout>
#include <QWheelEvent>
#include <QContextMenuEvent>
#include <QDir>
#include <QDragEnterEvent>
#include <QDragMoveEvent>
#include <QDropEvent>
#include <QKeyEvent>
#include <QMouseEvent>
#include <QRegularExpression>
#include <set>
#include <cmath>

namespace nodeui {
namespace {
constexpr double kWidth = 184, kHeader = 26, kRow = 22, kThumbW = 164, kThumbH = 92, kPort = 7;
const char* kMime = "application/x-mvp-node";
} // namespace

// A node: its name, input ports down the left, the output port on the right, and its picture.
class NodeItem : public QGraphicsItem {
public:
    NodeItem(int id, const nodes::NodeDef* def, const QString& type) : m_id(id), m_def(def), m_type(type)
    {
        setFlags(ItemIsMovable | ItemIsSelectable | ItemSendsGeometryChanges);
        setZValue(1);
    }
    int id() const { return m_id; }
    int inputs() const { return m_def ? int(m_def->inputs.size()) : 0; }
    bool hasOutput() const { return m_def && m_def->output; }
    double thumbTop() const { return kHeader + std::max(1, inputs()) * kRow + 4; }
    QRectF boundingRect() const override { return QRectF(-kPort, 0, kWidth + 2 * kPort, thumbTop() + kThumbH + 10); }
    QPointF inputPort(int i) const { return QPointF(0, kHeader + kRow * (i + 0.5)); }
    QPointF outputPort() const { return QPointF(kWidth, kHeader + kRow * 0.5); }
    void setThumb(const QImage& img)
    {
        m_thumb = img.isNull() ? QImage() : img.scaled(int(kThumbW), int(kThumbH), Qt::KeepAspectRatio, Qt::FastTransformation);
        update();
    }
    // Where it ran in the last preview: 1 the GPU, 0 the CPU, -1 not known (shown as a badge on the header).
    void setRunsOn(int where)
    {
        if (where == m_runsOn)
            return;
        m_runsOn = where;
        setToolTip(where == 1   ? QObject::tr("Runs on the GPU (the graphics card)")
                   : where == 0 ? QObject::tr("Runs on the CPU (its picture goes to and from the graphics card when the rest run there)")
                                : QString());
        update();
    }
    std::function<void(int, QPointF)> moved;

    void paint(QPainter* p, const QStyleOptionGraphicsItem*, QWidget*) override
    {
        const auto& c = theme::c();
        const QRectF body(0, 0, kWidth, thumbTop() + kThumbH + 10);
        p->setRenderHint(QPainter::Antialiasing);
        p->setPen(QPen(isSelected() ? c.accent : c.line, isSelected() ? 2.0 : 1.0));
        p->setBrush(c.raised);
        p->drawRoundedRect(body, 6, 6);
        // The header, coloured by kind: source and output, the nodes of their own, effects, sources.
        const QColor head = (m_type == QLatin1String("source") || m_type == QLatin1String("output") ? QColor(0x2f7d4f)
                             : m_type.startsWith(QLatin1String("fx."))                                 ? QColor(0x3a5f9e)
                             : m_type.startsWith(QLatin1String("gen."))                                ? QColor(0x6a46b8)
                                                                                                       : QColor(0xa35a1c));
        QPainterPath top;
        top.addRoundedRect(QRectF(0, 0, kWidth, kHeader), 6, 6);
        top.addRect(QRectF(0, kHeader / 2, kWidth, kHeader / 2));
        p->fillPath(top.simplified(), head);
        p->setPen(Qt::white);
        QFont f = p->font();
        f.setBold(true);
        p->setFont(f);
        double titleW = kWidth - 16;
        if (m_runsOn >= 0) { // the badge, top right: GPU green, CPU blue
            QFont bf = f;
            bf.setPointSizeF(std::max(6.0, f.pointSizeF() - 2));
            const QString label = m_runsOn ? QStringLiteral("GPU") : QStringLiteral("CPU");
            const double bw = QFontMetricsF(bf).horizontalAdvance(label) + 8, bh = kHeader - 10;
            const QRectF badge(kWidth - 6 - bw, (kHeader - bh) / 2, bw, bh);
            p->setPen(QPen(QColor(255, 255, 255, 140), 1));
            p->setBrush(m_runsOn ? QColor(0x2e9e4f) : QColor(0x2f6fd6));
            p->drawRoundedRect(badge, 3, 3);
            p->setFont(bf);
            p->setPen(Qt::white);
            p->drawText(badge, Qt::AlignCenter, label);
            p->setFont(f);
            titleW = badge.left() - 4 - 8;
        }
        p->drawText(QRectF(8, 0, titleW, kHeader), Qt::AlignVCenter | Qt::AlignLeft,
                    p->fontMetrics().elidedText(m_def ? m_def->name : m_type, Qt::ElideRight, int(titleW)));
        f.setBold(false);
        p->setFont(f);
        p->setPen(c.text);
        for (int i = 0; i < inputs(); ++i) {
            p->drawText(QRectF(12, kHeader + kRow * i, kWidth - 24, kRow), Qt::AlignVCenter | Qt::AlignLeft, m_def->inputs[i]);
            p->setBrush(c.bright);
            p->drawEllipse(inputPort(i), kPort * 0.75, kPort * 0.75);
        }
        if (hasOutput()) {
            p->drawText(QRectF(12, kHeader, kWidth - 24, kRow), Qt::AlignVCenter | Qt::AlignRight, QStringLiteral("Out"));
            p->setBrush(c.accent);
            p->drawEllipse(outputPort(), kPort * 0.75, kPort * 0.75);
        }
        const QRectF thumb(10, thumbTop(), kThumbW, kThumbH);
        p->fillRect(thumb, QBrush(QColor(40, 40, 46), Qt::SolidPattern));
        // (a checkerboard under see-through parts)
        for (int y = 0; y < int(kThumbH); y += 8)
            for (int x = 0; x < int(kThumbW); x += 8)
                if ((x / 8 + y / 8) % 2)
                    p->fillRect(QRectF(thumb.left() + x, thumb.top() + y, 8, 8), QColor(56, 56, 64));
        if (!m_thumb.isNull())
            p->drawImage(QPointF(thumb.left() + (kThumbW - m_thumb.width()) / 2, thumb.top() + (kThumbH - m_thumb.height()) / 2), m_thumb);
    }

protected:
    QVariant itemChange(GraphicsItemChange change, const QVariant& value) override
    {
        if (change == ItemPositionHasChanged && moved)
            moved(m_id, pos());
        return QGraphicsItem::itemChange(change, value);
    }

private:
    int m_id;
    const nodes::NodeDef* m_def;
    QString m_type;
    QImage m_thumb;
    int m_runsOn = -1;
};

// A wire from an output to an input.
class WireItem : public QGraphicsPathItem {
public:
    explicit WireItem(const nodes::Link& l) : link(l)
    {
        setFlags(ItemIsSelectable);
        setZValue(0);
    }
    nodes::Link link;
    static QPainterPath curve(QPointF a, QPointF b)
    {
        QPainterPath path(a);
        const double dx = std::max(40.0, std::abs(b.x() - a.x()) * 0.5);
        path.cubicTo(a + QPointF(dx, 0), b - QPointF(dx, 0), b);
        return path;
    }
    void paint(QPainter* p, const QStyleOptionGraphicsItem*, QWidget*) override
    {
        p->setRenderHint(QPainter::Antialiasing);
        p->setPen(QPen(isSelected() ? theme::c().accent : theme::c().bright, isSelected() ? 3.0 : 2.0));
        p->drawPath(path());
    }
    QPainterPath shape() const override
    {
        QPainterPathStroker s;
        s.setWidth(10);
        return s.createStroke(path());
    }
};

// The graph: zoom with the wheel, pan with the middle button (or Space + drag), drag from an output to an input to
// wire them, drop nodes from the list, Delete removes what's selected, right-click to add a node.
class GraphView : public QGraphicsView {
public:
    explicit GraphView(QGraphicsScene* scene) : QGraphicsView(scene)
    {
        setRenderHint(QPainter::Antialiasing);
        setDragMode(RubberBandDrag);
        setTransformationAnchor(AnchorUnderMouse);
        setAcceptDrops(true);
        setBackgroundBrush(theme::c().deep);
        setViewportUpdateMode(FullViewportUpdate);
    }
    std::function<NodeItem*(int)> item;
    std::function<const std::vector<nodes::Link>&()> links;
    std::function<void(int from, int to, int port)> connectPorts;
    std::function<void(int to, int port)> disconnect;
    std::function<void(const QString& type, QPointF at)> addNode;
    std::function<void()> deleteSelected;
    std::function<void(QPoint global, QPointF at)> menu;
    std::function<void()> movedDone;

protected:
    struct PortHit {
        NodeItem* node = nullptr;
        int port = -1; // -1: the output
    };
    PortHit portAt(QPointF scenePos) const
    {
        for (QGraphicsItem* gi : scene()->items(QRectF(scenePos - QPointF(12, 12), QSizeF(24, 24)))) {
            auto* n = dynamic_cast<NodeItem*>(gi);
            if (!n)
                continue;
            if (n->hasOutput() && QLineF(n->mapToScene(n->outputPort()), scenePos).length() <= kPort + 4)
                return {n, -1};
            for (int i = 0; i < n->inputs(); ++i)
                if (QLineF(n->mapToScene(n->inputPort(i)), scenePos).length() <= kPort + 4)
                    return {n, i};
        }
        return {};
    }
    void mousePressEvent(QMouseEvent* e) override
    {
        const QPointF at = mapToScene(e->position().toPoint());
        if (e->button() == Qt::MiddleButton || (e->button() == Qt::LeftButton && m_space)) {
            m_panning = true;
            m_panFrom = e->position().toPoint();
            viewport()->setCursor(Qt::ClosedHandCursor);
            return;
        }
        if (e->button() == Qt::LeftButton) {
            const PortHit hit = portAt(at);
            if (hit.node) {
                if (hit.port < 0) {
                    m_dragFrom = hit.node->id();
                    m_dragAnchor = hit.node->mapToScene(hit.node->outputPort());
                } else if (const nodes::Link* l = findLink(hit.node->id(), hit.port)) {
                    // Pulling a wire off an input: carry it from its output again.
                    m_dragFrom = l->from;
                    m_dragAnchor = item(l->from)->mapToScene(item(l->from)->outputPort());
                    disconnect(hit.node->id(), hit.port);
                } else {
                    m_dragToNode = hit.node->id();
                    m_dragToPort = hit.port;
                    m_dragAnchor = hit.node->mapToScene(hit.node->inputPort(hit.port));
                }
                m_temp = scene()->addPath(QPainterPath(), QPen(theme::c().accent, 2, Qt::DashLine));
                updateTemp(at);
                return;
            }
        }
        QGraphicsView::mousePressEvent(e);
    }
    void mouseMoveEvent(QMouseEvent* e) override
    {
        if (m_panning) {
            const QPoint d = e->position().toPoint() - m_panFrom;
            m_panFrom = e->position().toPoint();
            horizontalScrollBar()->setValue(horizontalScrollBar()->value() - d.x());
            verticalScrollBar()->setValue(verticalScrollBar()->value() - d.y());
            return;
        }
        if (m_temp) {
            updateTemp(mapToScene(e->position().toPoint()));
            return;
        }
        QGraphicsView::mouseMoveEvent(e);
    }
    void mouseReleaseEvent(QMouseEvent* e) override
    {
        if (m_panning) {
            m_panning = false;
            viewport()->unsetCursor();
            return;
        }
        if (m_temp) {
            const PortHit hit = portAt(mapToScene(e->position().toPoint()));
            delete m_temp;
            m_temp = nullptr;
            if (m_dragFrom && hit.node && hit.port >= 0)
                connectPorts(m_dragFrom, hit.node->id(), hit.port);
            else if (m_dragToNode && hit.node && hit.port < 0)
                connectPorts(hit.node->id(), m_dragToNode, m_dragToPort);
            m_dragFrom = m_dragToNode = 0;
            m_dragToPort = -1;
            return;
        }
        const bool wasDragging = !scene()->selectedItems().isEmpty();
        QGraphicsView::mouseReleaseEvent(e);
        if (wasDragging && movedDone)
            movedDone();
    }
    void wheelEvent(QWheelEvent* e) override
    {
        const double f = std::pow(1.0015, e->angleDelta().y());
        const double now = transform().m11();
        if ((now < 0.15 && f < 1) || (now > 3 && f > 1))
            return;
        scale(f, f);
    }
    void keyPressEvent(QKeyEvent* e) override
    {
        if (e->key() == Qt::Key_Delete || e->key() == Qt::Key_Backspace) {
            deleteSelected();
            return;
        }
        if (e->key() == Qt::Key_Space && !e->isAutoRepeat())
            m_space = true;
        QGraphicsView::keyPressEvent(e);
    }
    void keyReleaseEvent(QKeyEvent* e) override
    {
        if (e->key() == Qt::Key_Space && !e->isAutoRepeat())
            m_space = false;
        QGraphicsView::keyReleaseEvent(e);
    }
    void contextMenuEvent(QContextMenuEvent* e) override { menu(e->globalPos(), mapToScene(e->pos())); }
    void dragEnterEvent(QDragEnterEvent* e) override { e->setAccepted(e->mimeData()->hasFormat(QString::fromLatin1(kMime))); }
    void dragMoveEvent(QDragMoveEvent* e) override { e->setAccepted(e->mimeData()->hasFormat(QString::fromLatin1(kMime))); }
    void dropEvent(QDropEvent* e) override
    {
        const QString type = QString::fromUtf8(e->mimeData()->data(QString::fromLatin1(kMime)));
        addNode(type, mapToScene(e->position().toPoint()) - QPointF(kWidth / 2, kHeader / 2));
        e->acceptProposedAction();
    }

private:
    const nodes::Link* findLink(int to, int port) const
    {
        for (const nodes::Link& l : links())
            if (l.to == to && l.port == port)
                return &l;
        return nullptr;
    }
    void updateTemp(QPointF at)
    {
        m_temp->setPath(m_dragFrom ? WireItem::curve(m_dragAnchor, at) : WireItem::curve(at, m_dragAnchor));
    }
    QGraphicsPathItem* m_temp = nullptr;
    int m_dragFrom = 0, m_dragToNode = 0, m_dragToPort = -1;
    QPointF m_dragAnchor;
    bool m_panning = false, m_space = false;
    QPoint m_panFrom;
};

// The node list: drag a node out of it onto the graph.
class Library : public QTreeWidget {
public:
    using QTreeWidget::QTreeWidget;

protected:
    QStringList mimeTypes() const override { return {QString::fromLatin1(kMime)}; }
    QMimeData* mimeData(const QList<QTreeWidgetItem*>& items) const override
    {
        if (items.isEmpty() || items.front()->data(0, Qt::UserRole).toString().isEmpty())
            return nullptr;
        auto* m = new QMimeData;
        m->setData(QString::fromLatin1(kMime), items.front()->data(0, Qt::UserRole).toString().toUtf8());
        return m;
    }
};
} // namespace nodeui

using nodeui::GraphView;
using nodeui::NodeItem;
using nodeui::WireItem;

NodeEffectEditor::NodeEffectEditor(const std::vector<std::pair<QString, QString>>& videos, const QString& example, int projectWidth, QWidget* parent)
    : QDialog(parent), m_videos(videos), m_converter(std::make_unique<FrameConverter>()), m_projectWidth(std::max(16, projectWidth))
{
    setWindowTitle(tr("Effect Builder"));
    setWindowFlag(Qt::WindowMaximizeButtonHint);
    resize(1560, 880);
    auto* layout = new QVBoxLayout(this);

    // The file, the name, the example.
    auto* bar = new QHBoxLayout;
    auto button = [&](const QString& text, const QString& tip, auto fn) {
        auto* b = new QPushButton(text);
        b->setToolTip(tip);
        connect(b, &QPushButton::clicked, this, fn);
        bar->addWidget(b);
        return b;
    };
    button(tr("New"), tr("Start again from a Source wired to the Output"), [this] {
        if (!maybeSave())
            return;
        m_path.clear();
        setGraph(nodes::starter());
        m_modified = false;
    });
    button(tr("Open..."), tr("Open an effect made here before, to change it"), [this] {
        if (!maybeSave())
            return;
        const QString path = QFileDialog::getOpenFileName(this, tr("Open Effect"), nodes::userEffectsFolder(), tr("Effects (*.mvpfx)"));
        if (!path.isEmpty())
            openFile(path);
    });
    button(tr("Save As..."), tr("Save under another file name"), [this] { save(true); });
    bar->addSpacing(16);
    bar->addWidget(new QLabel(tr("Name:")));
    m_name = new QLineEdit;
    m_name->setMaximumWidth(260);
    m_name->setToolTip(tr("The effect's name in the Effects tab (User Created)"));
    connect(m_name, &QLineEdit::textEdited, this, [this](const QString& text) {
        changed(QStringLiteral("name"));
        m_graph.name = text;
    });
    bar->addWidget(m_name);
    bar->addSpacing(16);
    bar->addWidget(new QLabel(tr("Example:")));
    m_example = new QComboBox;
    m_example->setMinimumWidth(220);
    for (const auto& [name, path] : m_videos)
        m_example->addItem(name, path);
    // Patterns to try it on (the color bars are the empty entry, the default).
    m_example->addItem(tr("Pattern: Color bars"), QString());
    for (const char* id : {"bars_smpte", "gradient", "vignette_gradient", "checkerboard", "plasma_neon", "plasma_fire", "plasma_ocean",
                           "aurora", "starfield", "hyperspace", "retro_grid", "digital_rain", "bokeh_warm", "countdown"})
        if (const GeneratorPreset* p = findGeneratorPreset(QLatin1String(id)))
            m_example->addItem(tr("Pattern: %1").arg(p->name), QStringLiteral("pattern:") + p->id);
    m_example->addItem(tr("Choose a File..."), QStringLiteral("?"));
    connect(m_example, qOverload<int>(&QComboBox::activated), this, [this](int i) {
        QString path = m_example->itemData(i).toString();
        if (path == QLatin1String("?")) {
            path = QFileDialog::getOpenFileName(this, tr("Example Video"), QString(), tr("Video (*.mp4 *.mov *.mkv *.webm *.avi *.m4v *.mxf *.ts)"));
            if (path.isEmpty())
                return;
            m_example->insertItem(0, QFileInfo(path).fileName(), path);
            m_example->setCurrentIndex(0);
        }
        setExample(path);
    });
    bar->addWidget(m_example);
    m_play = new QPushButton(tr("Pause"));
    connect(m_play, &QPushButton::clicked, this, [this] {
        m_playing = !m_playing;
        m_play->setText(m_playing ? tr("Pause") : tr("Play"));
    });
    bar->addWidget(m_play);
    m_scrub = new QSlider(Qt::Horizontal);
    m_scrub->setRange(0, 1000);
    m_scrub->setToolTip(tr("Where in the example"));
    connect(m_scrub, &QSlider::sliderMoved, this, [this](int v) { m_time = TimeUs(double(m_duration) * v / 1000.0); });
    bar->addWidget(m_scrub, 1);
    layout->addLayout(bar);

    auto* split = new QSplitter(Qt::Horizontal);
    // Left: the example, and the nodes.
    auto* left = new QWidget;
    auto* ll = new QVBoxLayout(left);
    ll->setContentsMargins(0, 0, 0, 0);
    ll->addWidget(new QLabel(tr("<b>Example</b>")));
    m_sourceView = new QLabel;
    m_sourceView->setMinimumSize(320, 180);
    m_sourceView->setAlignment(Qt::AlignCenter);
    m_sourceView->setStyleSheet(QStringLiteral("background: black;"));
    ll->addWidget(m_sourceView);
    ll->addWidget(new QLabel(tr("<b>Nodes</b> (drag one onto the graph, or double-click)")));
    m_filter = new QLineEdit;
    m_filter->setPlaceholderText(tr("Search %1 nodes...").arg(nodes::nodeDefs().size()));
    m_filter->setClearButtonEnabled(true);
    connect(m_filter, &QLineEdit::textChanged, this, &NodeEffectEditor::rebuildLibrary);
    ll->addWidget(m_filter);
    auto* library = new nodeui::Library;
    m_library = library;
    m_library->setHeaderHidden(true);
    m_library->setDragEnabled(true);
    m_library->setDragDropMode(QAbstractItemView::DragOnly);
    connect(m_library, &QTreeWidget::itemDoubleClicked, this, [this](QTreeWidgetItem* it) {
        const QString type = it->data(0, Qt::UserRole).toString();
        if (!type.isEmpty())
            addNode(type, m_view->mapToScene(m_view->viewport()->rect().center()) - QPointF(nodeui::kWidth / 2, 40));
    });
    ll->addWidget(m_library, 1);
    split->addWidget(left);

    // Middle: the graph.
    m_scene = new QGraphicsScene(this);
    m_scene->setSceneRect(-4000, -4000, 8000, 8000);
    m_view = new GraphView(m_scene);
    m_view->item = [this](int id) { return m_items.count(id) ? m_items[id] : nullptr; };
    m_view->links = [this]() -> const std::vector<nodes::Link>& { return m_graph.links; };
    m_view->connectPorts = [this](int from, int to, int port) { connectNodes(from, to, port); };
    m_view->disconnect = [this](int to, int port) {
        changed();
        std::erase_if(m_graph.links, [&](const nodes::Link& l) { return l.to == to && l.port == port; });
        edited();
    };
    m_view->addNode = [this](const QString& type, QPointF at) { addNode(type, at); };
    m_view->deleteSelected = [this] {
        std::vector<int> gone;
        std::vector<nodes::Link> cut;
        for (QGraphicsItem* gi : m_scene->selectedItems()) {
            if (auto* n = dynamic_cast<NodeItem*>(gi)) {
                const nodes::Node* node = m_graph.find(n->id());
                if (node && node->type != QLatin1String("source") && node->type != QLatin1String("output")) // (those two stay)
                    gone.push_back(n->id());
            } else if (auto* w = dynamic_cast<WireItem*>(gi)) {
                cut.push_back(w->link);
            }
        }
        if (gone.empty() && cut.empty())
            return;
        changed();
        for (int id : gone)
            m_graph.removeNode(id);
        for (const nodes::Link& l : cut)
            std::erase(m_graph.links, l);
        edited();
    };
    m_view->movedDone = [this] { m_lastKey.clear(); };
    m_view->menu = [this](QPoint global, QPointF at) {
        QMenu menu(this);
        // On a node: duplicate or delete it; anywhere: add one (by category).
        NodeItem* on = nullptr;
        for (QGraphicsItem* gi : m_scene->items(at))
            if ((on = dynamic_cast<NodeItem*>(gi)))
                break;
        if (on) {
            const int id = on->id();
            const nodes::Node* n = m_graph.find(id);
            if (n && n->type != QLatin1String("source") && n->type != QLatin1String("output")) {
                menu.addAction(tr("Duplicate"), this, [this, id] {
                    const nodes::Node* src = m_graph.find(id);
                    if (!src)
                        return;
                    const nodes::Node copy = *src;
                    changed();
                    const int added = m_graph.addNode(copy.type, copy.pos + QPointF(30, 30));
                    if (nodes::Node* d = m_graph.find(added))
                        d->params = copy.params;
                    edited();
                });
                menu.addAction(tr("Reset Settings"), this, [this, id] {
                    nodes::Node* n2 = m_graph.find(id);
                    const nodes::NodeDef* def = n2 ? nodes::findNode(n2->type) : nullptr;
                    if (!def)
                        return;
                    changed();
                    n2->params = defaultParams(def->params);
                    edited();
                });
                menu.addAction(tr("Delete"), this, [this, id] {
                    changed();
                    m_graph.removeNode(id);
                    edited();
                });
                menu.addSeparator();
            }
        }
        QMenu* add = menu.addMenu(tr("Add Node"));
        std::map<QString, QMenu*> cats;
        for (const nodes::NodeDef& d : nodes::nodeDefs()) {
            if (d.id == QLatin1String("source") || d.id == QLatin1String("output"))
                continue;
            QMenu* sub = add;
            QString path;
            for (const QString& part : d.category.split(QLatin1Char('/'))) {
                path += QLatin1Char('/') + part;
                QMenu*& m = cats[path];
                if (!m)
                    m = sub->addMenu(QString(part).replace(QLatin1Char('&'), QStringLiteral("&&")));
                sub = m;
            }
            sub->addAction(QString(d.name).replace(QLatin1Char('&'), QStringLiteral("&&")), this, [this, type = d.id, at] { addNode(type, at); });
        }
        menu.exec(global);
    };
    connect(m_scene, &QGraphicsScene::selectionChanged, this, &NodeEffectEditor::selectionChanged);
    split->addWidget(m_view);

    // Right: the result, and the chosen node's settings.
    auto* right = new QWidget;
    auto* rl = new QVBoxLayout(right);
    rl->setContentsMargins(0, 0, 0, 0);
    rl->addWidget(new QLabel(tr("<b>Live result</b>")));
    m_resultView = new QLabel;
    m_resultView->setMinimumSize(320, 180);
    m_resultView->setAlignment(Qt::AlignCenter);
    m_resultView->setStyleSheet(QStringLiteral("background: black;"));
    rl->addWidget(m_resultView);
    m_status = new QLabel;
    m_status->setWordWrap(true);
    theme::style(m_status, QStringLiteral("color: @dim;"));
    rl->addWidget(m_status);
    auto* scroll = new QScrollArea;
    scroll->setWidgetResizable(true);
    scroll->setFrameShape(QFrame::NoFrame);
    m_paramsHost = new QWidget;
    m_paramsLayout = new QVBoxLayout(m_paramsHost);
    m_paramsLayout->setAlignment(Qt::AlignTop);
    scroll->setWidget(m_paramsHost);
    rl->addWidget(scroll, 1);
    rl->addWidget(new QLabel(tr("Description (shown in the Effects tab):")));
    m_description = new QLineEdit;
    connect(m_description, &QLineEdit::textEdited, this, [this](const QString& text) {
        changed(QStringLiteral("description"));
        m_graph.description = text;
    });
    rl->addWidget(m_description);
    split->addWidget(right);
    split->setStretchFactor(0, 0);
    split->setStretchFactor(1, 1);
    split->setStretchFactor(2, 0);
    split->setSizes({360, 820, 380});
    layout->addWidget(split, 1);

    auto* buttons = new QDialogButtonBox;
    auto* saveButton = buttons->addButton(tr("Save as User Created Effect"), QDialogButtonBox::AcceptRole);
    saveButton->setDefault(true);
    saveButton->setToolTip(tr("Save it to a file and add it to the Effects tab (User Created), ready for any event, track or adjustment layer"));
    buttons->addButton(QDialogButtonBox::Close);
    help::addButton(buttons, QStringLiteral("effects.md#effect-builder"));
    connect(saveButton, &QPushButton::clicked, this, [this] { save(false); });
    connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::close);
    auto* hint = new QLabel(tr("Drag from a node's output (right) to an input (left) to wire them; pull a wire off an input to unplug it. "
                               "Wheel: zoom. Middle button or Space + drag: pan. Delete removes. Ctrl+Z / Ctrl+Y: undo / redo."));
    hint->setWordWrap(true);
    theme::style(hint, QStringLiteral("color: @dim;"));
    auto* bottom = new QHBoxLayout;
    bottom->addWidget(hint, 1);
    bottom->addWidget(buttons);
    layout->addLayout(bottom);

    new QShortcut(QKeySequence::Undo, this, [this] { undo(); });
    new QShortcut(QKeySequence::Redo, this, [this] { redo(); });
    new QShortcut(QKeySequence(QStringLiteral("Ctrl+Y")), this, [this] { redo(); });
    new QShortcut(QKeySequence::Save, this, [this] { save(false); });

    rebuildLibrary({});
    setGraph(nodes::starter());
    m_modified = false;
    const int at = m_example->findData(example);
    m_example->setCurrentIndex(at >= 0 ? at : m_example->findData(QString()));
    setExample(m_example->currentData().toString());
    auto* timer = new QTimer(this);
    timer->setSingleShot(true);
    connect(timer, &QTimer::timeout, this, [this, timer] {
        QElapsedTimer took;
        took.start();
        tick();
        timer->start(int(std::clamp<qint64>(33 - took.elapsed(), 10, 33)));
    });
    timer->start(0);
}

NodeEffectEditor::~NodeEffectEditor()
{
    // (The scene goes with the children, after this: deleting a selected node then says the selection changed, which
    // must not reach this editor, already gone.)
    m_scene->disconnect(this);
}

// ---- the example ----
void NodeEffectEditor::setExample(const QString& path)
{
    m_decoder.reset();
    m_time = 0;
    m_duration = 20 * kUsPerSecond;
    m_live = path == QLatin1String("live");
    const bool pattern = path.startsWith(QLatin1String("pattern:"));
    m_pattern = pattern ? path.mid(8) : QStringLiteral("bars_full");
    if (!path.isEmpty() && !m_live && !pattern) {
        const MediaInfo info = probeMedia(path);
        auto dec = std::make_unique<VideoDecoder>();
        if (info.ok() && !info.video.empty() && dec->open(path, info.video.front().index)) {
            m_decoder = std::move(dec);
            m_duration = std::max<TimeUs>(kUsPerSecond / 10, info.duration);
        }
    }
}

void NodeEffectEditor::setLiveSource(std::function<QImage()> frame)
{
    m_liveFrame = std::move(frame);
    m_example->insertItem(0, tr("Live (the Big Live picture)"), QStringLiteral("live"));
    m_example->setCurrentIndex(0);
    setExample(QStringLiteral("live"));
}

QImage NodeEffectEditor::patternFrame()
{
    QImage img;
    if (const GeneratorPreset* p = findGeneratorPreset(m_pattern))
        img = renderGenerator(makeGenerator(*p), QSize(512, 288), 512.0 / m_projectWidth, m_time, m_duration, FrameRate{30, 1});
    if (img.isNull()) {
        img = QImage(512, 288, QImage::Format_ARGB32_Premultiplied);
        img.fill(Qt::gray);
    }
    return img.convertToFormat(QImage::Format_ARGB32_Premultiplied);
}

QImage NodeEffectEditor::exampleFrame()
{
    if (m_live) {
        const QImage f = m_liveFrame ? m_liveFrame() : QImage();
        if (f.isNull())
            return patternFrame();
        return f.scaled(QSize(512, 288), Qt::KeepAspectRatio, Qt::SmoothTransformation).convertToFormat(QImage::Format_ARGB32_Premultiplied);
    }
    if (!m_decoder)
        return patternFrame();
    const AVFrame* f = m_decoder->frameAt(m_time);
    if (!f)
        return {};
    // At most 512 x 288, the example's shape.
    const double s = std::min(512.0 / std::max(1, f->width), 288.0 / std::max(1, f->height));
    const QSize size(std::max(16, int(f->width * s)) & ~1, std::max(16, int(f->height * s)) & ~1);
    return m_converter->convert(f, size, false).convertToFormat(QImage::Format_ARGB32_Premultiplied);
}

void NodeEffectEditor::tick()
{
    if (!isVisible())
        return;
    step();
    if (m_playing) {
        m_time += kUsPerSecond / 30;
        if (m_time >= m_duration)
            m_time = 0;
    }
}

void NodeEffectEditor::step()
{
    const QImage frame = exampleFrame();
    if (frame.isNull())
        return;
    QElapsedTimer took;
    took.start();
    RenderContext ctx;
    ctx.time = m_time;
    ctx.timelineTime = m_time;
    ctx.scale = double(frame.width()) / m_projectWidth;
    ctx.fps = 30;
    ctx.contentRect = QRectF(frame.rect());
    if (!m_renderer) {
        m_gpuSurface = createGpuSurface();
        m_renderer = createFrameRenderer(false, m_gpuSurface.get());
        m_gpu = dynamic_cast<GpuCompositor*>(m_renderer.get());
    }
    if (m_gpu && nodes::problem(m_graph).isEmpty()) {
        m_eval.outputs.clear();
        m_eval.error.clear();
        m_runsOn.clear();
        m_result = m_gpu->renderGraph(m_graph, frame, ctx, &m_eval.outputs, QSize(int(nodeui::kThumbW), int(nodeui::kThumbH)), &m_runsOn);
    } else {
        m_result = m_eval.run(m_graph, frame, ctx, true);
        m_runsOn.clear();
        for (const nodes::Node& n : m_graph.nodes)
            m_runsOn[n.id] = false; // (no graphics card, or the graph can't run as it is: all on the CPU)
    }
    const qint64 ms = took.elapsed();
    auto show = [](QLabel* label, const QImage& img) {
        QPixmap pm = QPixmap::fromImage(img.scaled(label->size(), Qt::KeepAspectRatio, Qt::SmoothTransformation));
        label->setPixmap(pm);
    };
    show(m_sourceView, frame);
    show(m_resultView, m_result);
    for (auto& [id, item] : m_items) {
        const auto it = m_eval.outputs.find(id);
        item->setThumb(it == m_eval.outputs.end() ? QImage() : it->second);
        const auto on = m_runsOn.find(id);
        item->setRunsOn(on == m_runsOn.end() ? -1 : on->second ? 1 : 0);
    }
    if (!m_scrub->isSliderDown() && m_duration > 0)
        m_scrub->setValue(int(1000.0 * double(m_time) / double(m_duration)));
    m_status->setText(m_eval.error.isEmpty() ? tr("%n node(s), %1 ms a frame at %2 x %3", nullptr, int(m_graph.nodes.size()))
                                                   .arg(ms)
                                                   .arg(frame.width())
                                                   .arg(frame.height())
                                             : m_eval.error);
}

QString NodeEffectEditor::status() const { return m_status->text(); }

// ---- the graph ----
void NodeEffectEditor::setGraph(const nodes::Graph& g)
{
    m_graph = g;
    m_undo.clear();
    m_redo.clear();
    m_lastKey.clear();
    m_name->setText(m_graph.name);
    m_description->setText(m_graph.description);
    rebuildScene();
    showParams(0);
    QTimer::singleShot(0, this, [this] { frameGraph(); });
}

void NodeEffectEditor::frameGraph()
{
    // The whole graph in view (no closer than 1:1).
    const QRectF r = m_scene->itemsBoundingRect().adjusted(-60, -60, 60, 60);
    m_view->resetTransform();
    if (r.width() > m_view->viewport()->width() || r.height() > m_view->viewport()->height())
        m_view->fitInView(r, Qt::KeepAspectRatio);
    m_view->centerOn(r.center());
}

int NodeEffectEditor::addNode(const QString& type, QPointF pos)
{
    changed();
    const int id = m_graph.addNode(type, pos);
    edited();
    if (NodeItem* it = m_items.count(id) ? m_items[id] : nullptr) {
        m_scene->clearSelection();
        it->setSelected(true);
    }
    return id;
}

bool NodeEffectEditor::connectNodes(int from, int to, int port)
{
    nodes::Graph g = m_graph;
    if (!g.connect(from, to, port)) {
        m_status->setText(tr("That would wire the node back into itself."));
        return false;
    }
    changed();
    m_graph = std::move(g);
    edited();
    return true;
}

void NodeEffectEditor::setParam(int node, const QString& id, double value)
{
    nodes::Node* n = m_graph.find(node);
    if (!n)
        return;
    changed(QStringLiteral("%1/%2").arg(node).arg(id));
    ParamValue& v = n->params[id];
    v.keys.clear();
    v.value = value;
    m_modified = true;
}

void NodeEffectEditor::setExposed(int node, const QString& param, bool on)
{
    const nodes::Node* n = m_graph.find(node);
    const nodes::NodeDef* def = n ? nodes::findNode(n->type) : nullptr;
    if (!def)
        return;
    changed();
    std::erase_if(m_graph.exposed, [&](const nodes::Exposed& x) { return x.node == node && x.param == param; });
    if (on) {
        const auto pd = std::find_if(def->params.begin(), def->params.end(), [&](const ParamDef& p) { return p.id == param; });
        const QString name = pd == def->params.end() ? param : pd->name;
        // (the node's name too when two nodes would give the same name)
        const bool clash = std::any_of(m_graph.exposed.begin(), m_graph.exposed.end(), [&](const nodes::Exposed& x) { return x.name == name; });
        m_graph.exposed.push_back({QStringLiteral("n%1_%2").arg(node).arg(param), clash ? def->name + QStringLiteral(": ") + name : name, node, param});
    }
    m_modified = true;
}

void NodeEffectEditor::changed(const QString& key)
{
    if (key.isEmpty() || key != m_lastKey) {
        m_undo.push_back(nodes::toJson(m_graph));
        if (m_undo.size() > 200)
            m_undo.erase(m_undo.begin());
        m_redo.clear();
    }
    m_lastKey = key;
    m_modified = true;
}

void NodeEffectEditor::edited()
{
    rebuildScene();
    if (!m_graph.find(m_paramsNode))
        showParams(0);
}

void NodeEffectEditor::undo()
{
    if (m_undo.empty())
        return;
    m_redo.push_back(nodes::toJson(m_graph));
    const nodes::Graph g = nodes::fromJson(m_undo.back());
    m_undo.pop_back();
    m_graph = g;
    m_lastKey.clear();
    m_name->setText(m_graph.name);
    m_description->setText(m_graph.description);
    rebuildScene();
    showParams(m_graph.find(m_paramsNode) ? m_paramsNode : 0);
}

void NodeEffectEditor::redo()
{
    if (m_redo.empty())
        return;
    m_undo.push_back(nodes::toJson(m_graph));
    m_graph = nodes::fromJson(m_redo.back());
    m_redo.pop_back();
    m_lastKey.clear();
    m_name->setText(m_graph.name);
    m_description->setText(m_graph.description);
    rebuildScene();
    showParams(m_graph.find(m_paramsNode) ? m_paramsNode : 0);
}

void NodeEffectEditor::rebuildScene()
{
    m_rebuilding = true;
    std::set<int> selected;
    for (QGraphicsItem* gi : m_scene->selectedItems())
        if (auto* n = dynamic_cast<NodeItem*>(gi))
            selected.insert(n->id());
    m_scene->clear();
    m_items.clear();
    for (const nodes::Node& n : m_graph.nodes) {
        auto* item = new NodeItem(n.id, nodes::findNode(n.type), n.type);
        item->setPos(n.pos);
        m_scene->addItem(item);
        item->setSelected(selected.count(n.id) > 0);
        if (const auto it = m_eval.outputs.find(n.id); it != m_eval.outputs.end())
            item->setThumb(it->second);
        m_items[n.id] = item;
    }
    std::vector<WireItem*> wires;
    for (const nodes::Link& l : m_graph.links) {
        if (!m_items.count(l.from) || !m_items.count(l.to))
            continue;
        auto* w = new WireItem(l);
        w->setPath(WireItem::curve(m_items[l.from]->mapToScene(m_items[l.from]->outputPort()), m_items[l.to]->mapToScene(m_items[l.to]->inputPort(l.port))));
        m_scene->addItem(w);
        wires.push_back(w);
    }
    // Moving a node: its place in the graph, and its wires follow.
    for (auto& [id, item] : m_items)
        item->moved = [this, wires](int node, QPointF pos) {
            if (m_rebuilding)
                return;
            if (nodes::Node* n = m_graph.find(node)) {
                changed(QStringLiteral("move/%1").arg(node));
                n->pos = pos;
            }
            for (WireItem* w : wires)
                if (w->link.from == node || w->link.to == node)
                    w->setPath(WireItem::curve(m_items[w->link.from]->mapToScene(m_items[w->link.from]->outputPort()),
                                               m_items[w->link.to]->mapToScene(m_items[w->link.to]->inputPort(w->link.port))));
        };
    m_rebuilding = false;
}

void NodeEffectEditor::rebuildLibrary(const QString& filter)
{
    m_library->clear();
    std::map<QString, QTreeWidgetItem*> cats;
    for (const nodes::NodeDef& d : nodes::nodeDefs()) {
        if (d.id == QLatin1String("source") || d.id == QLatin1String("output"))
            continue;
        if (!filter.isEmpty() && !d.name.contains(filter, Qt::CaseInsensitive) && !d.category.contains(filter, Qt::CaseInsensitive)
            && !d.description.contains(filter, Qt::CaseInsensitive))
            continue;
        QTreeWidgetItem* parent = nullptr;
        QString path;
        for (const QString& part : d.category.split(QLatin1Char('/'))) {
            path += QLatin1Char('/') + part;
            QTreeWidgetItem*& c = cats[path];
            if (!c) {
                c = parent ? new QTreeWidgetItem(parent, {part}) : new QTreeWidgetItem(m_library, {part});
                c->setFlags(Qt::ItemIsEnabled);
                // (the nodes of their own open; with a search, everything)
                c->setExpanded(!filter.isEmpty() || (!d.id.startsWith(QLatin1String("fx.")) && !d.id.startsWith(QLatin1String("gen."))));
            }
            parent = c;
        }
        auto* item = new QTreeWidgetItem(parent, {d.name});
        item->setData(0, Qt::UserRole, d.id);
        item->setToolTip(0, d.description);
    }
}

void NodeEffectEditor::selectionChanged()
{
    if (m_rebuilding)
        return;
    int only = 0;
    for (QGraphicsItem* gi : m_scene->selectedItems())
        if (auto* n = dynamic_cast<NodeItem*>(gi)) {
            if (only) {
                only = -1;
                break;
            }
            only = n->id();
        }
    if (only != m_paramsNode)
        showParams(std::max(0, only));
}

// The chosen node's settings, each with "Show on the effect".
void NodeEffectEditor::showParams(int nodeId)
{
    m_paramsNode = nodeId;
    while (QLayoutItem* it = m_paramsLayout->takeAt(0)) {
        delete it->widget();
        delete it;
    }
    const nodes::Node* n = m_graph.find(nodeId);
    const nodes::NodeDef* def = n ? nodes::findNode(n->type) : nullptr;
    if (!def) {
        auto* none = new QLabel(tr("Click a node to change its settings.\n\nTick \"Show on effect\" next to a setting to make it a "
                                   "setting of the saved effect, which can be changed and keyframed on each event."));
        none->setWordWrap(true);
        theme::style(none, QStringLiteral("color: @dim;"));
        m_paramsLayout->addWidget(none);
        return;
    }
    auto* title = new QLabel(QStringLiteral("<b>%1</b>").arg(def->name.toHtmlEscaped()));
    m_paramsLayout->addWidget(title);
    auto* about = new QLabel(def->description);
    about->setWordWrap(true);
    theme::style(about, QStringLiteral("color: @dim;"));
    m_paramsLayout->addWidget(about);
    auto* form = new QWidget;
    auto* fl = new QFormLayout(form);
    fl->setContentsMargins(0, 6, 0, 0);
    const ParamMap params = n->params;
    for (const ParamDef& pd : def->params) {
        if (pd.hidden)
            continue;
        const auto it = params.find(pd.id);
        const double value = it != params.end() ? it->second.value : pd.def;
        const QString text = it != params.end() ? it->second.text : pd.defText;
        QWidget* control = nullptr;
        const QString pid = pd.id;
        switch (pd.type) {
        case ParamType::Number: {
            auto* row = new QWidget;
            auto* rl = new QHBoxLayout(row);
            rl->setContentsMargins(0, 0, 0, 0);
            auto* slider = new QSlider(Qt::Horizontal);
            slider->setRange(0, 1000);
            auto* spin = new QDoubleSpinBox;
            spin->setRange(pd.min, pd.max);
            spin->setDecimals(pd.decimals);
            spin->setSingleStep(std::pow(10.0, -pd.decimals) * (pd.decimals ? 10 : 1));
            spin->setSuffix(pd.suffix);
            spin->setValue(value);
            auto toSlider = [pd](double v) { return int(1000.0 * (v - pd.min) / std::max(1e-9, pd.max - pd.min) + 0.5); };
            slider->setValue(toSlider(value));
            connect(slider, &QSlider::valueChanged, this, [this, spin, pd](int s) {
                const double v = pd.min + (pd.max - pd.min) * s / 1000.0;
                QSignalBlocker block(spin);
                spin->setValue(v);
                setParam(m_paramsNode, pd.id, spin->value());
            });
            connect(spin, &QDoubleSpinBox::valueChanged, this, [this, slider, toSlider, pid](double v) {
                QSignalBlocker block(slider);
                slider->setValue(toSlider(v));
                setParam(m_paramsNode, pid, v);
            });
            rl->addWidget(slider, 1);
            rl->addWidget(spin);
            control = row;
            break;
        }
        case ParamType::Choice: {
            auto* combo = new QComboBox;
            combo->addItems(pd.choices);
            combo->setCurrentIndex(int(value + 0.5));
            connect(combo, qOverload<int>(&QComboBox::currentIndexChanged), this, [this, pid](int i) { setParam(m_paramsNode, pid, i); });
            control = combo;
            break;
        }
        case ParamType::Bool: {
            auto* box = new QCheckBox;
            box->setChecked(value >= 0.5);
            connect(box, &QCheckBox::toggled, this, [this, pid](bool on) { setParam(m_paramsNode, pid, on ? 1 : 0); });
            control = box;
            break;
        }
        case ParamType::Color: {
            auto* b = new QPushButton;
            auto paint = [b](uint32_t rgb) { b->setStyleSheet(QStringLiteral("background: %1; min-height: 20px;").arg(QColor(QRgb(rgb)).name())); };
            paint(uint32_t(value));
            connect(b, &QPushButton::clicked, this, [this, b, paint, pid] {
                const nodes::Node* now = m_graph.find(m_paramsNode);
                const auto v = now ? now->params.find(pid) : ParamMap::const_iterator{};
                const QColor start = now && v != now->params.end() ? QColor(QRgb(uint32_t(v->second.value))) : Qt::white;
                const QColor c = QColorDialog::getColor(start, this);
                if (!c.isValid())
                    return;
                paint(c.rgb() & 0xffffff);
                setParam(m_paramsNode, pid, double(c.rgb() & 0xffffff));
            });
            control = b;
            break;
        }
        case ParamType::Text:
        case ParamType::Font: {
            auto setText = [this, pid](const QString& t) {
                if (nodes::Node* now = m_graph.find(m_paramsNode)) {
                    changed(QStringLiteral("%1/%2").arg(m_paramsNode).arg(pid));
                    now->params[pid].text = t;
                }
            };
            if (pd.type == ParamType::Font) {
                auto* f = new QFontComboBox;
                f->setCurrentFont(QFont(text));
                connect(f, &QFontComboBox::currentFontChanged, this, [setText](const QFont& font) { setText(font.family()); });
                control = f;
            } else if (pd.multiline) {
                auto* e = new QPlainTextEdit(text);
                e->setMaximumHeight(70);
                connect(e, &QPlainTextEdit::textChanged, this, [e, setText] { setText(e->toPlainText()); });
                control = e;
            } else {
                auto* e = new QLineEdit(text);
                connect(e, &QLineEdit::textEdited, this, setText);
                control = e;
            }
            break;
        }
        }
        if (!control)
            continue;
        control->setToolTip(pd.tooltip);
        auto* cell = new QWidget;
        auto* cl = new QVBoxLayout(cell);
        cl->setContentsMargins(0, 0, 0, 0);
        cl->setSpacing(1);
        cl->addWidget(control);
        if (pd.type != ParamType::Text && pd.type != ParamType::Font) {
            auto* show = new QCheckBox(tr("Show on effect"));
            show->setToolTip(tr("Make this a setting of the saved effect: it can then be changed, and keyframed, on each event"));
            show->setChecked(std::any_of(m_graph.exposed.begin(), m_graph.exposed.end(),
                                         [&](const nodes::Exposed& x) { return x.node == nodeId && x.param == pid; }));
            theme::style(show, QStringLiteral("color: @dim;"));
            connect(show, &QCheckBox::toggled, this, [this, pid](bool on) { setExposed(m_paramsNode, pid, on); });
            cl->addWidget(show);
        }
        fl->addRow(pd.name + QLatin1Char(':'), cell);
    }
    m_paramsLayout->addWidget(form);
}

// ---- files ----
bool NodeEffectEditor::openFile(const QString& path)
{
    nodes::Graph g;
    QString error;
    if (!nodes::load(path, &g, &error)) {
        QMessageBox::warning(this, tr("Open Effect"), tr("Couldn't open %1: %2").arg(QFileInfo(path).fileName(), error));
        return false;
    }
    // (an example, from the program's folder, is a starting point: Save asks where, in the user's)
    m_path = QFileInfo(path).absolutePath() == QFileInfo(nodes::examplesFolder()).absoluteFilePath() ? QString() : path;
    setGraph(g);
    m_modified = false;
    return true;
}

bool NodeEffectEditor::saveTo(const QString& path, QString* error)
{
    if (m_graph.name.trimmed().isEmpty())
        m_graph.name = QFileInfo(path).completeBaseName();
    if (!nodes::save(m_graph, path, error))
        return false;
    m_path = path;
    m_modified = false;
    const EffectDef* def = nodes::registerEffect(m_graph);
    emit saved(def->id);
    return true;
}

bool NodeEffectEditor::save(bool as)
{
    QString path = m_path;
    if (as || path.isEmpty()) {
        QDir().mkpath(nodes::userEffectsFolder());
        const QString suggested = QDir(nodes::userEffectsFolder())
                                      .filePath(QString(m_graph.name.trimmed().isEmpty() ? tr("My Effect") : m_graph.name.trimmed())
                                                    .replace(QRegularExpression(QStringLiteral("[\\\\/:*?\"<>|]")), QStringLiteral(" "))
                                                + QStringLiteral(".mvpfx"));
        path = QFileDialog::getSaveFileName(this, tr("Save Effect"), suggested, tr("Effects (*.mvpfx)"));
        if (path.isEmpty())
            return false;
    }
    QString error;
    if (!saveTo(path, &error)) {
        QMessageBox::warning(this, tr("Save Effect"), tr("Couldn't save: %1").arg(error));
        return false;
    }
    m_status->setText(tr("Saved. \"%1\" is in the Effects tab under User Created.").arg(m_graph.name));
    return true;
}

bool NodeEffectEditor::maybeSave()
{
    if (!m_modified)
        return true;
    const auto answer = QMessageBox::question(this, tr("Effect Builder"), tr("Save the changes to \"%1\"?").arg(m_graph.name),
                                              QMessageBox::Save | QMessageBox::Discard | QMessageBox::Cancel);
    if (answer == QMessageBox::Cancel)
        return false;
    return answer == QMessageBox::Discard || save(false);
}

void NodeEffectEditor::closeEvent(QCloseEvent* e)
{
    if (maybeSave())
        e->accept();
    else
        e->ignore();
}
