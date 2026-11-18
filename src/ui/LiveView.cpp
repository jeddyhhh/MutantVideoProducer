#include "ui/LiveView.h"

#include "ui/Theme.h"

#include "core/BeatTracker.h"

#include "core/AudioDecoder.h"
#include "core/LivePadSets.h"
#include "core/LiveRecorder.h"
#include "core/Project.h"
#include "ui/DeviceChainView.h"
#include "core/MediaInfo.h"
#include "ui/AudioMeter.h"
#include "ui/EffectsBrowser.h"
#include "ui/LiveClipsBrowser.h"
#include "ui/BeatTuner.h"
#include "ui/LiveOverlaysDialog.h"
#include "ui/LivePadPanel.h"
#include "ui/MidiHub.h"
#include "core/Osc.h"
#include <QNetworkInterface>
#include <QUdpSocket>
#include "ui/PreviewWidget.h"
#include "ui/SetlistDialog.h"
#include "ui/StreamDialog.h"
#include "core/FFmpeg.h"
#include <QProcess>
#include "core/VideoShare.h"

#include <QCheckBox>
#include <QComboBox>
#include <QElapsedTimer>
#include <QEventLoop>
#include <QAbstractSpinBox>
#include <QApplication>
#include <QDir>
#include <QFileDialog>
#include <QInputDialog>
#include <QLineEdit>
#include <QPlainTextEdit>
#include <QSlider>
#include <QTextEdit>
#include <QFileInfo>
#include <QGuiApplication>
#include <QHBoxLayout>
#include <QCloseEvent>
#include <QContextMenuEvent>
#include <QKeyEvent>
#include <QMenu>
#include <QLabel>
#include <QPainter>
#include <QPushButton>
#include <QScreen>
#include <QSettings>
#include <QSpinBox>
#include <QSplitter>
#include <QStandardItemModel>
#include <QTimer>
#include <QToolButton>
#include <QVBoxLayout>
#include <QFormLayout>
#include <QWheelEvent>
#include <cmath>
#include <QScrollArea>
#include <QScrollBar>
#include <QTabWidget>

namespace {

// Full-screen output (double-click or Esc closes it).
class OutputWindow : public PreviewWidget {
public:
    using PreviewWidget::PreviewWidget;

protected:
    void keyPressEvent(QKeyEvent* e) override
    {
        if (e->key() == Qt::Key_Escape || e->key() == Qt::Key_F11)
            close();
    }
    void mouseDoubleClickEvent(QMouseEvent*) override { close(); }
};

// The picture in its own normal window, e.g. for OBS's Window Capture
// (its title stays "Mutant Video Producer Live Output"). Right-click for exact sizes and
// always-on-top; closing it puts the picture back in the Live tab.
class PopOutWindow : public PreviewWidget {
public:
    std::function<void()> onClosed;

    PopOutWindow()
    {
        setWindowFlag(Qt::Window);
        setWindowTitle(LiveView::tr("Mutant Video Producer Live Output"));
        setBackground(Qt::black);
        setAttribute(Qt::WA_DeleteOnClose, false);
    }

protected:
    void closeEvent(QCloseEvent* e) override
    {
        QSettings().setValue(QStringLiteral("live/popOutGeometry"), saveGeometry());
        PreviewWidget::closeEvent(e);
        if (onClosed)
            onClosed();
    }
    void keyPressEvent(QKeyEvent* e) override
    {
        if (e->key() == Qt::Key_F11)
            isFullScreen() ? showNormal() : showFullScreen();
        else if (e->key() == Qt::Key_Escape && isFullScreen())
            showNormal();
    }
    void mouseDoubleClickEvent(QMouseEvent*) override { isFullScreen() ? showNormal() : showFullScreen(); }
    void contextMenuEvent(QContextMenuEvent* e) override
    {
        QMenu menu(this);
        QMenu* sizes = menu.addMenu(LiveView::tr("Window Size"));
        if (!image().isNull())
            sizes->addAction(LiveView::tr("Picture Size (%1 x %2)").arg(image().width()).arg(image().height()), this,
                             [this, s = image().size()] { setSize(s); });
        for (QSize s : {QSize(1920, 1080), QSize(1280, 720), QSize(960, 540), QSize(640, 360)})
            sizes->addAction(QStringLiteral("%1 x %2").arg(s.width()).arg(s.height()), this, [this, s] { setSize(s); });
        QAction* top = menu.addAction(LiveView::tr("Always on Top"));
        top->setCheckable(true);
        top->setChecked(windowFlags() & Qt::WindowStaysOnTopHint);
        connect(top, &QAction::toggled, this, [this](bool on) {
            setWindowFlag(Qt::WindowStaysOnTopHint, on);
            show();
        });
        menu.addAction(isFullScreen() ? LiveView::tr("Exit Full Screen") : LiveView::tr("Full Screen"), this,
                       [this] { isFullScreen() ? showNormal() : showFullScreen(); });
        menu.addSeparator();
        menu.addAction(LiveView::tr("Back to the Live Tab"), this, [this] { close(); });
        menu.exec(e->globalPos());
    }

private:
    void setSize(QSize s)
    {
        showNormal();
        resize(s); // the inside of the window: what a window capture sees
    }
};

// The live picture; double-click opens the full-screen output.
class LiveDisplay : public PreviewWidget {
public:
    std::function<void()> onDoubleClick;
    // Pan and zoom of the output, in output pixels: a drag (its start point, then how far from it), a wheel turn at a
    // point, the menu.
    std::function<void(QPointF delta, bool start)> onPan;
    std::function<void(double steps, QPointF at)> onZoom;
    std::function<void(QPoint globalPos)> onMenu;
    std::function<bool(QPointF at)> onLayer; // a layer is at this point (output pixels): it can be dragged
    QString hint; // shown while there is no picture
    QString info; // shown in a corner while zoomed / panned
    using PreviewWidget::PreviewWidget;

protected:
    void paintEvent(QPaintEvent* e) override
    {
        PreviewWidget::paintEvent(e);
        QPainter p(this);
        if (!hint.isEmpty()) {
            p.setPen(QColor(0x9a, 0x9a, 0xa0));
            p.drawText(rect(), Qt::AlignCenter, hint);
        }
        if (!info.isEmpty() && !image().isNull()) {
            const QRect r = p.fontMetrics().boundingRect(info).adjusted(-6, -3, 6, 3);
            const QRect box(QPoint(8, height() - r.height() - 8), r.size());
            p.fillRect(box, QColor(0, 0, 0, 150));
            p.setPen(QColor(0xe0, 0xe0, 0xe6));
            p.drawText(box, Qt::AlignCenter, info);
        }
    }
    void mouseDoubleClickEvent(QMouseEvent*) override
    {
        m_dragging = false;
        if (onDoubleClick)
            onDoubleClick();
    }
    // Widget point -> output pixels.
    QPointF toImage(QPointF pos) const
    {
        const QRectF r = pictureRect();
        if (r.isEmpty() || image().isNull())
            return {};
        return QPointF((pos.x() - r.x()) * image().width() / r.width(), (pos.y() - r.y()) * image().height() / r.height());
    }
    void mousePressEvent(QMouseEvent* e) override
    {
        if (e->button() == Qt::LeftButton && !image().isNull() && onPan) {
            m_dragging = true;
            m_dragFrom = toImage(e->position());
            setCursor(onLayer && onLayer(m_dragFrom) ? Qt::SizeAllCursor : Qt::ClosedHandCursor);
            onPan(m_dragFrom, true);
            return;
        }
        PreviewWidget::mousePressEvent(e);
    }
    void mouseMoveEvent(QMouseEvent* e) override
    {
        if (m_dragging && onPan) {
            onPan(toImage(e->position()) - m_dragFrom, false);
            return;
        }
        setCursor(image().isNull()                                 ? Qt::ArrowCursor
                  : onLayer && onLayer(toImage(e->position())) ? Qt::SizeAllCursor
                                                               : Qt::OpenHandCursor);
        PreviewWidget::mouseMoveEvent(e);
    }
    void mouseReleaseEvent(QMouseEvent* e) override
    {
        if (m_dragging) {
            m_dragging = false;
            setCursor(Qt::OpenHandCursor);
            return;
        }
        PreviewWidget::mouseReleaseEvent(e);
    }
    void wheelEvent(QWheelEvent* e) override
    {
        if (image().isNull() || !onZoom) {
            PreviewWidget::wheelEvent(e);
            return;
        }
        onZoom(e->angleDelta().y() / 120.0, toImage(e->position()));
        e->accept();
    }
    void contextMenuEvent(QContextMenuEvent* e) override
    {
        if (onMenu && !image().isNull())
            onMenu(e->globalPos());
    }

private:
    bool m_dragging = false;
    QPointF m_dragFrom;
};

struct CameraFormat {
    const char* label;
    int w, h;
    double fps;
};
QString audioHelp()
{
    return LiveView::tr("Sound recorded with the clips: a microphone, line-in or capture card,\n"
                        "or \"Desktop audio\": whatever the computer plays through an output.");
}

const CameraFormat kCameraFormats[] = {{"Camera default", 0, 0, 0},      {"640x480", 640, 480, 0},
                                       {"1280x720", 1280, 720, 0},      {"1280x720 @ 60", 1280, 720, 60},
                                       {"1920x1080", 1920, 1080, 0},    {"1920x1080 @ 60", 1920, 1080, 60},
                                       {"3840x2160", 3840, 2160, 0}};

} // namespace

// A setup section in the sidebar: a title that folds its settings away
// (remembered).
class LiveSection : public QWidget {
public:
    LiveSection(const QString& title, const QString& key, QLayout* body)
    {
        setObjectName(QStringLiteral("liveSection_") + key);
        auto* v = new QVBoxLayout(this);
        v->setContentsMargins(0, 0, 0, 0);
        v->setSpacing(0);
        auto* header = new QToolButton;
        header->setText(title);
        header->setCheckable(true);
        header->setToolButtonStyle(Qt::ToolButtonTextBesideIcon);
        header->setAutoRaise(true);
        header->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
        theme::style(header, QStringLiteral("QToolButton { font-weight: bold; text-align: left; padding: 3px; border-bottom: 1px solid @line; }"));
        auto* content = new QWidget;
        content->setLayout(body);
        v->addWidget(header);
        v->addWidget(content);
        const QString setting = QStringLiteral("live/section/") + key;
        auto apply = [header, content](bool open) {
            header->setArrowType(open ? Qt::DownArrow : Qt::RightArrow);
            content->setVisible(open);
        };
        const bool open = QSettings().value(setting, true).toBool();
        header->setChecked(open);
        apply(open);
        connect(header, &QToolButton::toggled, this, [apply, setting](bool on) {
            apply(on);
            QSettings().setValue(setting, on);
        });
    }
};

QString LiveView::layoutName(int layout)
{
    switch (layout) {
    case 1:
        return tr("Stage: the picture across the top, pads and effects below");
    case 2:
        return tr("Compact: a big picture, pads / setup / effects in tabs");
    default:
        return tr("Sidebar: pads left, setup right");
    }
}

void LiveView::saveLayoutState() const
{
    QSettings settings;
    for (const auto& [name, splitter] : m_frameSplitters)
        if (splitter)
            settings.setValue(QStringLiteral("live/layout%1/%2").arg(m_layout).arg(name), splitter->saveState());
    if (m_frameTabs)
        settings.setValue(QStringLiteral("live/layout%1/tab").arg(m_layout), m_frameTabs->currentIndex());
}

void LiveView::setLayoutMode(int layout)
{
    layout = std::clamp(layout, 0, kLayoutCount - 1);
    if (m_frame) {
        if (layout == m_layout)
            return;
        saveLayoutState();
    }
    // Take the pieces out of the old frame (they live on; the frame goes).
    if (m_padsScroll)
        m_padsScroll->takeWidget();
    for (QWidget* w : {static_cast<QWidget*>(m_pads), m_center, m_cuePanel, static_cast<QWidget*>(m_sidebar), static_cast<QWidget*>(m_bottomSplit)}) {
        w->hide();
        w->setParent(this);
    }
    delete m_frame;
    m_frameSplitters.clear();
    m_frameTabs = nullptr;
    m_padsScroll = nullptr;
    m_layout = layout;
    m_frame = new QWidget;
    m_frame->setObjectName(QStringLiteral("liveFrame"));
    auto* fl = new QVBoxLayout(m_frame);
    fl->setContentsMargins(0, 0, 0, 0);
    auto splitter = [this](Qt::Orientation o, const QString& name) {
        auto* sp = new QSplitter(o);
        sp->setChildrenCollapsible(false);
        m_frameSplitters.push_back({name, sp});
        return sp;
    };
    auto padsScrolled = [this] { // pads in a scroll area, for frames where they may be short of room
        m_padsScroll = new QScrollArea;
        m_padsScroll->setWidget(m_pads);
        m_padsScroll->setWidgetResizable(true);
        m_padsScroll->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
        m_padsScroll->setFrameShape(QFrame::NoFrame);
        m_padsScroll->setMinimumWidth(m_pads->minimumSizeHint().width() + 18);
        return m_padsScroll.data();
    };
    auto sideColumn = [this] { // the cue over the setup
        auto* col = new QWidget;
        auto* v = new QVBoxLayout(col);
        v->setContentsMargins(0, 0, 0, 0);
        v->setSpacing(4);
        v->addWidget(m_cuePanel);
        v->addWidget(m_sidebar, 1);
        return col;
    };
    QSplitter* root = nullptr;
    m_bottomSplit->setOrientation(layout == 2 ? Qt::Vertical : Qt::Horizontal); // a tab is narrow: the list over the chain
    if (layout == 1) {
        // Stage: the picture as large as can be, the setup beside it; pads and effects in a deck below.
        root = splitter(Qt::Vertical, QStringLiteral("root"));
        auto* top = splitter(Qt::Horizontal, QStringLiteral("top"));
        top->addWidget(m_center);
        top->addWidget(sideColumn());
        top->setStretchFactor(0, 1);
        top->setStretchFactor(1, 0);
        auto* deck = splitter(Qt::Horizontal, QStringLiteral("deck"));
        deck->addWidget(padsScrolled());
        deck->addWidget(m_bottomSplit);
        deck->setStretchFactor(0, 0);
        deck->setStretchFactor(1, 1);
        root->addWidget(top);
        root->addWidget(deck);
        root->setStretchFactor(0, 3);
        root->setStretchFactor(1, 2);
        top->setSizes({1200, 300});
        deck->setSizes({340, 1200});
    } else if (layout == 2) {
        // Compact: the picture, and everything else one tab away (laptops, small screens).
        root = splitter(Qt::Horizontal, QStringLiteral("root"));
        m_frameTabs = new QTabWidget;
        m_frameTabs->setObjectName(QStringLiteral("liveTabs"));
        auto* perform = new QWidget;
        auto* pv = new QVBoxLayout(perform);
        pv->setContentsMargins(0, 0, 0, 0);
        pv->addWidget(m_cuePanel);
        pv->addWidget(padsScrolled(), 1);
        m_frameTabs->addTab(perform, tr("Perform"));
        m_frameTabs->addTab(m_sidebar, tr("Setup"));
        m_frameTabs->addTab(m_bottomSplit, tr("Effects"));
        root->addWidget(m_center);
        root->addWidget(m_frameTabs);
        root->setStretchFactor(0, 1);
        root->setStretchFactor(1, 0);
        root->setSizes({1100, 420});
    } else {
        // Sidebar: pads left, the picture with scenes and the setlist, the setup right; effects below.
        root = splitter(Qt::Vertical, QStringLiteral("root"));
        auto* top = splitter(Qt::Horizontal, QStringLiteral("top"));
        top->addWidget(padsScrolled());
        top->addWidget(m_center);
        top->addWidget(sideColumn());
        top->setStretchFactor(0, 0);
        top->setStretchFactor(1, 1);
        top->setStretchFactor(2, 0);
        top->setSizes({300, 1000, 300});
        root->addWidget(top);
        root->addWidget(m_bottomSplit);
        root->setStretchFactor(0, 3);
        root->setStretchFactor(1, 1);
    }
    fl->addWidget(root);
    m_mainLayout->addWidget(m_frame, 1);
    for (QWidget* w : {static_cast<QWidget*>(m_pads), m_center, static_cast<QWidget*>(m_sidebar), static_cast<QWidget*>(m_bottomSplit)})
        w->show();
    m_cuePanel->setVisible(m_cueKind != LiveEngine::CueNone);
    QSettings settings;
    for (const auto& [name, sp] : m_frameSplitters) {
        const QByteArray state = settings.value(QStringLiteral("live/layout%1/%2").arg(layout).arg(name)).toByteArray();
        if (!state.isEmpty())
            sp->restoreState(state);
    }
    if (m_frameTabs)
        m_frameTabs->setCurrentIndex(std::clamp(settings.value(QStringLiteral("live/layout%1/tab").arg(layout), 0).toInt(), 0, 2));
    settings.setValue(QStringLiteral("live/layout"), layout);
    emit layoutChanged(layout);
}

LiveView::LiveView(Project* project, QWidget* parent) : QWidget(parent), m_project(project), m_engine(new LiveEngine(this))
{
    auto* layout = new QVBoxLayout(this);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(0);

    // ---- Controls (placed by the chosen layout below) ----
    m_source = new QComboBox;
    m_source->setObjectName(QStringLiteral("liveSource"));
    m_source->setSizeAdjustPolicy(QComboBox::AdjustToMinimumContentsLengthWithIcon);
    m_source->setMinimumContentsLength(13);
    m_source->setToolTip(tr("Cameras and capture cards, whole screens, or a single window"));
    auto* refresh = new QToolButton;
    refresh->setText(QStringLiteral("⟳"));
    refresh->setToolTip(tr("Look for sources again (new windows, plugged-in cameras)"));
    m_nextFile = new QToolButton;
    m_nextFile->setText(QStringLiteral("⏭"));
    m_nextFile->setToolTip(tr("Next video in the playlist"));
    m_format = new QComboBox;
    for (const CameraFormat& f : kCameraFormats)
        m_format->addItem(QString::fromLatin1(f.label));
    m_format->setToolTip(tr("Format to ask the camera for"));
    m_audio = new QComboBox;
    m_audio->setSizeAdjustPolicy(QComboBox::AdjustToMinimumContentsLengthWithIcon);
    m_audio->setMinimumContentsLength(11);
    m_audio->setToolTip(audioHelp());
    m_audioMeter = new AudioMeter;
    m_audioMeter->setFixedSize(64, 22);
    m_audioMeter->setToolTip(tr("Input level"));
    m_size = new QComboBox;
    m_size->addItem(tr("Source size"), QSize()); // the only size that follows the source's shape
    // Every other size is a fixed output, whatever the source's shape (a window, a 4:3 camera): Framing fits it in.
    for (const auto& [label, size] : std::initializer_list<std::pair<const char*, QSize>>{
             {"1080p", {1920, 1080}}, {"720p", {1280, 720}}, {"540p", {960, 540}}, {"360p", {640, 360}}}) {
        m_size->addItem(QString::fromLatin1(label), size);
        m_size->setItemData(m_size->count() - 1, true, Qt::UserRole + 1); // a fixed output shape
    }
    // Phones: a fixed portrait (or square) output.
    m_size->insertSeparator(m_size->count());
    struct Canvas {
        const char* label;
        QSize size;
    };
    for (const Canvas& c : {Canvas{"Portrait 1080x1920 (9:16)", {1080, 1920}}, Canvas{"Portrait 720x1280 (9:16)", {720, 1280}},
                            Canvas{"Portrait 540x960 (9:16)", {540, 960}}, Canvas{"Portrait 1080x1350 (4:5)", {1080, 1350}},
                            Canvas{"Square 1080x1080 (1:1)", {1080, 1080}}}) {
        m_size->addItem(QString::fromLatin1(c.label), c.size);
        m_size->setItemData(m_size->count() - 1, true, Qt::UserRole + 1); // a fixed output shape
    }
    m_size->setToolTip(tr("The output's size, which the effects work at (smaller is faster). It stays that size and shape\n"
                          "whatever the source is; Framing says how a picture of another shape goes in.\n"
                          "Source size follows the source instead. Portrait and square sizes are for phones (TikTok, Reels,\n"
                          "Shorts, Stories)."));
    m_size->setSizeAdjustPolicy(QComboBox::AdjustToMinimumContentsLengthWithIcon); // shares its row with Framing
    m_size->setMinimumContentsLength(6);
    m_framing = new QComboBox;
    m_framing->setObjectName(QStringLiteral("liveFraming"));
    m_framing->setSizeAdjustPolicy(QComboBox::AdjustToMinimumContentsLengthWithIcon);
    m_framing->setMinimumContentsLength(6);
    m_framing->addItem(tr("Crop to fill"), int(LiveFraming::CropFill));
    m_framing->addItem(tr("Fit (black bars)"), int(LiveFraming::Fit));
    m_framing->addItem(tr("Fit on blurred background"), int(LiveFraming::FitBlur));
    m_framing->setToolTip(tr("How a picture of another shape goes into the output:\n"
                             "cropped to fill it (the middle), fitted with black bars, or fitted over a blurred copy of itself"));
    m_fps = new QComboBox;
    for (int f : {24, 25, 30, 50, 60})
        m_fps->addItem(QStringLiteral("%1 fps").arg(f), f);
    m_fps->setToolTip(tr("Frames per second the effects are rendered at"));
    m_mirror = new QCheckBox(tr("Mirror"));
    m_mirror->setToolTip(tr("Flip the picture left to right (like a mirror, for cameras pointing at you)"));
    m_start = new QPushButton;
    m_start->setObjectName(QStringLiteral("liveStart"));
    m_start->setMinimumWidth(90);
    m_rec = new QPushButton(tr("●  Rec"));
    m_rec->setObjectName(QStringLiteral("liveRec"));
    m_rec->setMinimumWidth(80);
    m_rec->setToolTip(tr("Record a clip of the live output; it appears in the Editor's From Live tab"));
    m_screenshot = new QPushButton(tr("Screenshot"));
    m_screenshot->setObjectName(QStringLiteral("liveScreenshot"));
    m_screenshot->setToolTip(tr("Save the picture on the big display as a PNG; it appears in the Editor's From Live tab, "
                                "next to the recordings. Right-click: Learn MIDI"));
    m_recMode = new QComboBox;
    m_recMode->addItem(tr("With effects"));
    m_recMode->addItem(tr("Clean picture"));
    m_recMode->setToolTip(tr("Record what you see, or the picture before the Live FX (to add effects later in the editor)"));
    m_recTime = new QLabel;
    m_recTime->setMinimumWidth(70);
    theme::style(m_recTime, QStringLiteral("color: @bad; font-weight: bold;"));
    auto* full = new QPushButton(tr("Full Screen"));
    full->setToolTip(tr("Show the live output full screen (double-click the picture; Esc closes it)"));
    m_popOutButton = new QPushButton(tr("Pop Out"));
    m_popOutButton->setCheckable(true);
    m_popOutButton->setToolTip(tr("Move the picture into its own window, \"Mutant Video Producer Live Output\" (e.g. to capture it in OBS\n"
                                  "with a Window Capture). Right-click that window for exact sizes; close it to bring the picture back."));
    m_status = new QLabel;
    theme::style(m_status, QStringLiteral("color: @dim;"));
    m_status->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred); // long messages don't widen the window

    // Layers, replay, MIDI, scenes, overlays.
    m_layerPick = new QComboBox;
    m_layerPick->setObjectName(QStringLiteral("liveLayerPick"));
    for (int i = 0; i < kLiveLayers; ++i)
        m_layerPick->addItem(tr("Layer %1").arg(i + 1));
    m_layerPick->setToolTip(tr("The layer the settings below are for. Layers are laid over in order: layer 1 lowest"));
    m_layer = new QComboBox;
    m_layer->setObjectName(QStringLiteral("liveLayer"));
    m_layer->setSizeAdjustPolicy(QComboBox::AdjustToMinimumContentsLengthWithIcon);
    m_layer->setMinimumContentsLength(12);
    m_layer->setToolTip(tr("A source laid over the first: a camera over a screen, a video loop over a camera...\n"
                           "Give it its own effects (a Chroma Keyer for a green screen) in the pads' Edit list > Layer FX."));
    for (int k = 0; k < LiveEngine::kMaxInputs; ++k) {
        auto* c = new QComboBox;
        c->setObjectName(QStringLiteral("liveInput%1").arg(k + 2));
        c->setSizeAdjustPolicy(QComboBox::AdjustToMinimumContentsLengthWithIcon);
        c->setMinimumContentsLength(12);
        c->setToolTip(tr("Another source kept open for the switcher under the picture (video files chosen for Source or Layer are listed too)"));
        m_inputs[size_t(k)] = c;
    }
    m_switcher = new QWidget;
    m_switcher->setObjectName(QStringLiteral("liveSwitcher"));
    {
        auto* h = new QHBoxLayout(m_switcher);
        h->setContentsMargins(4, 4, 4, 0);
        h->setSpacing(4);
        for (int k = 0; k <= LiveEngine::kMaxInputs; ++k) {
            auto* t = new QToolButton;
            t->setObjectName(QStringLiteral("liveInputTile%1").arg(k + 1));
            t->setToolButtonStyle(Qt::ToolButtonTextUnderIcon);
            t->setIconSize(QSize(128, 72));
            t->setFocusPolicy(Qt::NoFocus);
            t->setToolTip(tr("Click: preview this input (green). Double-click: put it on air (red) at once. Right-click: Learn MIDI"));
            t->installEventFilter(this);
            t->setContextMenuPolicy(Qt::CustomContextMenu);
            connect(t, &QToolButton::clicked, this, [this, k] { setPreviewInput(k); });
            connect(t, &QWidget::customContextMenuRequested, this, [this, k, t](const QPoint& pos) {
                QMenu menu;
                menu.addAction(tr("Learn MIDI (cut to this input)..."), [this, k] { learnMidi(QStringLiteral("input:%1").arg(k), tr("Input %1").arg(k + 1)); });
                menu.exec(t->mapToGlobal(pos));
            });
            m_inputTiles[size_t(k)] = t;
            h->addWidget(t);
        }
        h->addStretch(1);
        auto* cut = new QPushButton(tr("Cut"));
        cut->setObjectName(QStringLiteral("liveCut"));
        cut->setToolTip(tr("Put the preview (green) on air at once; the input that was on air goes to preview (Enter)"));
        auto* autoTake = new QPushButton(tr("Auto"));
        autoTake->setObjectName(QStringLiteral("liveAuto"));
        autoTake->setToolTip(tr("Put the preview on air with the pads' Transition and length (Shift+Enter)"));
        connect(cut, &QPushButton::clicked, this, [this] { cutTo(m_preview, false); });
        connect(autoTake, &QPushButton::clicked, this, [this] { cutTo(m_preview, true); });
        auto* buttons = new QVBoxLayout;
        buttons->addWidget(cut);
        buttons->addWidget(autoTake);
        h->addLayout(buttons);
        m_switcher->hide();
    }
    m_nextLayerFile = new QToolButton;
    m_nextLayerFile->setText(QStringLiteral("⏭"));
    m_nextLayerFile->setToolTip(tr("Next video in the layer's playlist"));
    m_layerNoFx = new QCheckBox(tr("No Effects"));
    m_layerNoFx->setToolTip(tr("Lay this layer over after the Live FX and the pads, so they don't change it (its own Layer FX still do)"));
    m_layerMode = new QComboBox;
    m_layerMode->addItems({tr("Picture in picture"), tr("Full frame")});
    m_layerMode->setToolTip(tr("A small picture in a corner, or over the whole picture (blended)"));
    m_layerCorner = new QComboBox;
    m_layerCorner->addItems({tr("Top left"), tr("Top right"), tr("Bottom left"), tr("Bottom right"), tr("Centre"), tr("Where dragged")});
    m_layerCorner->setToolTip(tr("Where the picture in picture goes. Or drag it on the Live picture"));
    m_layerSize = new QSlider(Qt::Horizontal);
    m_layerSize->setRange(5, 100);
    m_layerSize->setMinimumWidth(90);
    m_layerSize->setToolTip(tr("Width of the picture in picture"));
    m_layerSizeLabel = new QLabel;
    m_layerSizeLabel->setMinimumWidth(QFontMetrics(m_layerSizeLabel->font()).horizontalAdvance(QStringLiteral("100%")));
    m_layerBlend = new QComboBox;
    m_layerBlend->addItems(blendModeNames());
    m_layerBlend->setToolTip(tr("How the layer mixes with the picture under it"));
    m_layerOpacity = new QSlider(Qt::Horizontal);
    m_layerOpacity->setRange(0, 100);
    m_layerOpacity->setMinimumWidth(90);
    m_layerOpacity->setToolTip(tr("Layer opacity"));
    m_replay = new QCheckBox(tr("Replay buffer"));
    m_replay->setToolTip(tr("Keep the last seconds of the output in memory while capturing, so you can save what just happened"));
    m_replayLength = new QComboBox;
    for (int s : {15, 30, 60, 120})
        m_replayLength->addItem(tr("%1 s").arg(s), s);
    m_saveReplay = new QPushButton(tr("Save Replay"));
    m_saveReplay->setToolTip(tr("Save the replay buffer as a clip (it appears in the Editor's From Live tab). Right-click: Learn MIDI"));
    m_velocity = new QCheckBox(tr("Velocity sets pad strength"));
    m_velocity->setToolTip(tr("Hit a MIDI pad softly and less of its chain shows, hard and all of it (times the pad's Mix).\n"
                              "Off: every hit plays the pad in full. Keys and the mouse always do."));
    m_velocity->setChecked(QSettings().value(QStringLiteral("live/midiVelocity"), true).toBool());
    connect(m_velocity, &QCheckBox::toggled, this, [](bool on) { QSettings().setValue(QStringLiteral("live/midiVelocity"), on); });
    m_oscBox = new QCheckBox(tr("Listen on port"));
    m_oscBox->setObjectName(QStringLiteral("liveOsc"));
    m_oscBox->setToolTip(tr("OSC from a phone or tablet app (TouchOSC, Lemur...) on the same network: point it at this computer's\n"
                            "address and port. Right-click a pad or button > Learn MIDI and send a control to map it, or send\n"
                            "/mvp/pad/1..16, /mvp/scene/1..8, /mvp/padmix 0..1, /mvp/blackout, /mvp/tap, /mvp/rec..."));
    m_oscBox->setChecked(QSettings().value(QStringLiteral("live/osc"), false).toBool());
    m_oscPort = new QSpinBox;
    m_oscPort->setRange(1024, 65535);
    m_oscPort->setValue(QSettings().value(QStringLiteral("live/oscPort"), 9000).toInt());
    m_oscStatus = new QLabel;
    m_oscStatus->setWordWrap(true);
    m_oscStatus->setTextInteractionFlags(Qt::TextSelectableByMouse);
    theme::style(m_oscStatus, QStringLiteral("color: @dim;"));
    m_midi = new QComboBox;
    m_midi->setSizeAdjustPolicy(QComboBox::AdjustToMinimumContentsLengthWithIcon);
    m_midi->setMinimumContentsLength(12);
    m_midi->setToolTip(tr("A MIDI controller or an Xbox controller: right-click a pad (or Tap, Set 1, Rec, Save Replay) > Learn MIDI to map\n"
                          "it to a key or button; knobs, faders, triggers and sticks can move effect settings (right-click a Live FX knob >\n"
                          "Modulate > MIDI)"));
    for (int i = 0; i < kLiveScenes; ++i) {
        auto* b = new QToolButton;
        b->setText(QString::number(i + 1));
        b->setFixedWidth(24);
        b->setContextMenuPolicy(Qt::CustomContextMenu);
        connect(b, &QToolButton::clicked, this, [this, i] {
            if (QApplication::keyboardModifiers() & Qt::ShiftModifier)
                cueScene(i); // a look in the Cue window first
            else
                recallScene(i);
        });
        connect(b, &QWidget::customContextMenuRequested, this, [this, i, b](const QPoint& pos) {
            const LiveScene& s = m_project->timeline().liveScenes[size_t(i)];
            QMenu menu(this);
            menu.addAction(tr("Cue (preview it, not live; Shift+click)"), this, [this, i] { cueScene(i); })->setEnabled(s.used);
            menu.addAction(s.used ? tr("Save the Current Look Here (replace)") : tr("Save the Current Look Here"), this, [this, i] { saveScene(i); });
            QAction* rename = menu.addAction(tr("Rename..."), this, [this, i] {
                bool ok = false;
                const QString name = QInputDialog::getText(this, tr("Rename Scene"), tr("Name of scene %1:").arg(i + 1), QLineEdit::Normal,
                                                           m_project->timeline().liveScenes[size_t(i)].name, &ok);
                if (!ok)
                    return;
                Timeline tl = m_project->timeline();
                tl.liveScenes[size_t(i)].name = name.trimmed();
                m_project->applyEdit(tl, tr("Rename Scene"));
            });
            rename->setEnabled(s.used);
            QAction* clear = menu.addAction(tr("Clear"), this, [this, i] {
                Timeline tl = m_project->timeline();
                tl.liveScenes[size_t(i)] = LiveScene();
                m_project->applyEdit(tl, tr("Clear Scene"));
            });
            clear->setEnabled(s.used);
            menu.addSeparator();
            menu.addAction(tr("Learn MIDI..."), this, [this, i] { learnMidi(QStringLiteral("scene:%1").arg(i), tr("scene %1").arg(i + 1)); });
            menu.exec(b->mapToGlobal(pos));
        });
        m_sceneButtons[size_t(i)] = b;
    }
    auto* overlays = new QPushButton(tr("Overlays..."));
    overlays->setToolTip(tr("Text, a logo, the tempo or the time over the output"));

    // The setlist, sending the output to other apps, going live.
    m_songPrev = new QToolButton;
    m_songPrev->setText(QStringLiteral("◀"));
    m_songPrev->setToolTip(tr("Previous song (Page Up). Right-click: Learn MIDI"));
    m_song = new QComboBox;
    m_song->setObjectName(QStringLiteral("liveSong"));
    m_song->setSizeAdjustPolicy(QComboBox::AdjustToMinimumContentsLengthWithIcon);
    m_song->setMinimumContentsLength(14);
    m_song->setToolTip(tr("The song playing: choosing one loads its pad set, sets its tempo and recalls its scene"));
    m_songNext = new QToolButton;
    m_songNext->setText(QStringLiteral("▶"));
    m_songNext->setToolTip(tr("Next song (Page Down). Right-click: Learn MIDI"));
    auto* editSetlistButton = new QPushButton(tr("Edit..."));
    editSetlistButton->setToolTip(tr("Edit the setlist: the songs of the set, each one's pad set, tempo (locked or listened for) and scene"));
    m_songNotes = new QLabel;
    m_songNotes->setObjectName(QStringLiteral("liveSongNotes"));
    theme::style(m_songNotes, QStringLiteral("color: @warn; padding-left: 8px;"));
    m_spoutBox = new QCheckBox(tr("Spout"));
    m_spoutBox->setObjectName(QStringLiteral("liveSpout"));
    m_spoutBox->setContextMenuPolicy(Qt::CustomContextMenu);
    m_spoutBox->setToolTip(tr("Share the output (with effects and overlays) as a Spout sender, for OBS (Spout2 plugin),\n"
                              "Resolume, TouchDesigner, MadMapper, SpoutCam... Right-click: the sender's name"));
    m_cameraBox = new QCheckBox(tr("Virtual camera"));
    m_cameraBox->setObjectName(QStringLiteral("liveVirtualCamera"));
    m_cameraBox->setToolTip(tr("Show the output through the \"OBS Virtual Camera\" webcam, so Zoom, Discord, Teams or a\n"
                               "browser can pick it as a camera (OBS doesn't need to be running; its own virtual camera must be off)"));
    if (!SpoutSender::available()) {
        m_spoutBox->setEnabled(false);
        m_spoutBox->setToolTip(tr("Spout needs Windows with Direct3D 11"));
    }
    if (!VirtualCamera::installed()) {
        m_cameraBox->setEnabled(false);
        m_cameraBox->setToolTip(tr("Install OBS Studio to get its virtual camera (Mutant Video Producer feeds it pictures without OBS running)"));
    }
    m_shareStatus = new QLabel;
    m_shareStatus->setObjectName(QStringLiteral("liveShareStatus"));
    theme::style(m_shareStatus, QStringLiteral("color: @dim;"));
    m_goLive = new QPushButton(tr("Go Live"));
    m_goLive->setObjectName(QStringLiteral("liveGoLive"));
    m_goLive->setMinimumWidth(110);
    m_goLive->setToolTip(tr("Stream the output and the Audio input to Twitch, YouTube, Facebook or your own server\n"
                            "(Stream Settings...); press again to end the stream"));
    auto* streamSettings = new QToolButton;
    streamSettings->setText(QStringLiteral("⚙"));
    streamSettings->setToolTip(tr("Stream Settings: the service, the stream key and the bitrate"));
    m_streamStatus = new QLabel;
    m_streamStatus->setObjectName(QStringLiteral("liveStreamStatus"));
    theme::style(m_streamStatus, QStringLiteral("color: @dim;"));
    connect(m_goLive, &QPushButton::clicked, this, &LiveView::toggleStream);
    connect(streamSettings, &QToolButton::clicked, this, [this] { StreamDialog(this).exec(); });

    // ---- Picture over the effects section ----
    auto* display = new LiveDisplay;
    display->onDoubleClick = [this] { showFullScreenOutput(); };
    // Drag a layer to move it; elsewhere drag to pan, scroll to zoom: the source in the output (framing it for a phone, say).
    display->onLayer = [this](QPointF at) { return layerAt(at) >= 0; };
    display->onPan = [this](QPointF delta, bool start) {
        if (start) {
            m_panStart = m_panZoom;
            m_dragLayer = layerAt(delta); // (at the start: where the drag began)
            if (m_dragLayer >= 0) {
                m_dragLayerStart = m_engine->layerRects()[size_t(m_dragLayer)].center();
                m_layerIndex = m_dragLayer; // its settings show
                showLayer();
            }
            return;
        }
        if (m_dragLayer >= 0) {
            const QSize out = m_display->image().size();
            if (out.isEmpty())
                return;
            LiveLayerSettings& s = m_layerSets[size_t(m_dragLayer)];
            s.corner = 5;
            s.x = std::clamp(m_dragLayerStart.x() + delta.x() / out.width(), 0.0, 1.0);
            s.y = std::clamp(m_dragLayerStart.y() + delta.y() / out.height(), 0.0, 1.0);
            m_engine->setLayerSettings(m_dragLayer, s);
            showLayer();
            saveSettings();
            return;
        }
        QSizeF canvas;
        LiveFraming framing;
        panZoomFrame(&canvas, &framing);
        const QRectF r = sourceRectInCanvas(QSizeF(m_sourceSize), canvas, framing, m_panStart);
        if (r.isEmpty())
            return;
        LivePanZoom pz = m_panStart;
        pz.center -= QPointF(delta.x() / r.width(), delta.y() / r.height());
        setPanZoom(pz);
    };
    display->onZoom = [this](double steps, QPointF at) { zoomBy(std::pow(1.15, steps), at); };
    display->onMenu = [this](const QPoint& globalPos) {
        QMenu menu(this);
        menu.addAction(tr("Reset Pan and Zoom"), this, [this] { setPanZoom({}); })->setEnabled(!m_panZoom.isDefault());
        menu.addSeparator();
        for (double z : {1.0, 1.5, 2.0, 3.0, 4.0})
            menu.addAction(tr("Zoom %1%").arg(int(z * 100)), this, [this, z] {
                LivePanZoom pz = m_panZoom;
                pz.zoom = z;
                setPanZoom(pz);
            });
        menu.addSeparator();
        menu.addAction(tr("Full Screen"), this, &LiveView::showFullScreenOutput);
        menu.exec(globalPos);
    };
    display->hint = tr("Choose a camera, capture card, screen or window (Setup > Input) and press Start.\n"
                       "Add effects from the Video FX list: they apply to the live picture.");
    m_display = display;
    m_display->setMinimumSize(320, 180);
    m_browser = new EffectsBrowser(EffectsBrowser::Kind::Effects);
    m_browser->setObjectName(QStringLiteral("liveFxBrowser"));
    m_browser->setToolTip(tr("Drag effects and presets into the Live FX chain, or double-click them"));
    m_devices = new DeviceChainView(m_project, [] { return TimeUs(0); });
    m_devices->setObjectName(QStringLiteral("liveDevices"));
    m_devices->setTarget({FxTarget::Kind::Live, 0});
    m_bottomSplit = new QSplitter(Qt::Horizontal);
    m_bottomSplit->addWidget(m_browser);
    m_bottomSplit->addWidget(m_devices);
    m_bottomSplit->setStretchFactor(0, 0);
    m_bottomSplit->setStretchFactor(1, 1);
    m_bottomSplit->setSizes({280, 1200});
    m_pads = new LivePadPanel(m_project);
    // The cue: a pad or scene previewed before it goes live.
    m_cuePanel = new QWidget;
    m_cuePanel->setObjectName(QStringLiteral("liveCue"));
    {
        auto* cv = new QVBoxLayout(m_cuePanel);
        cv->setContentsMargins(6, 4, 6, 4);
        auto* title = new QLabel(tr("CUE  (not live)"));
        theme::style(title, QStringLiteral("color: @cue; font-weight: bold;"));
        cv->addWidget(title);
        m_cueView = new PreviewWidget;
        m_cueView->setMinimumSize(240, 135);
        cv->addWidget(m_cueView, 1);
        m_cueLabel = new QLabel;
        m_cueLabel->setWordWrap(true);
        theme::style(m_cueLabel, QStringLiteral("color: @text;"));
        cv->addWidget(m_cueLabel);
        auto* buttons = new QHBoxLayout;
        auto* take = new QPushButton(tr("Take"));
        take->setObjectName(QStringLiteral("liveCueTake"));
        take->setToolTip(tr("Put it live (on the beat or bar if Launch says so). Right-click: Learn MIDI"));
        theme::style(take, QStringLiteral("QPushButton { background: @cue; color: @window; font-weight: bold; }"));
        take->setContextMenuPolicy(Qt::CustomContextMenu);
        auto* close = new QPushButton(tr("Close"));
        buttons->addWidget(take, 1);
        buttons->addWidget(close);
        cv->addLayout(buttons);
        connect(take, &QPushButton::clicked, this, &LiveView::takeCue);
        connect(close, &QPushButton::clicked, this, &LiveView::clearCue);
        connect(take, &QWidget::customContextMenuRequested, this, [this, take](const QPoint& pos) {
            QMenu menu(this);
            menu.addAction(tr("Learn MIDI..."), this, [this] { learnMidi(QStringLiteral("take"), tr("Take (cue)")); });
            menu.exec(take->mapToGlobal(pos));
        });
    }
    m_cuePanel->setVisible(false);

    // ---- The pieces the layouts arrange ----
    // Show bar: what is pressed during a set.
    m_showBar = new QWidget;
    m_showBar->setObjectName(QStringLiteral("liveShowBar"));
    {
        auto* h = new QHBoxLayout(m_showBar);
        h->setContentsMargins(8, 4, 8, 4);
        h->addWidget(m_start);
        h->addWidget(m_rec);
        h->addWidget(m_recTime);
        h->addWidget(m_screenshot);
        h->addSpacing(6);
        h->addWidget(m_goLive);
        h->addWidget(m_streamStatus);
        h->addSpacing(6);
        h->addWidget(m_saveReplay);
        h->addSpacing(14);
        h->addWidget(full);
        h->addWidget(m_popOutButton);
        h->addSpacing(14);
        h->addWidget(m_status, 1);
        auto* layoutButton = new QToolButton;
        layoutButton->setObjectName(QStringLiteral("liveLayoutButton"));
        layoutButton->setText(tr("Layout"));
        layoutButton->setPopupMode(QToolButton::InstantPopup);
        layoutButton->setToolTip(tr("How the Live tab is laid out (also Options > Live Tab Layout)"));
        auto* menu = new QMenu(layoutButton);
        for (int i = 0; i < kLayoutCount; ++i)
            menu->addAction(layoutName(i), this, [this, i] { setLayoutMode(i); });
        layoutButton->setMenu(menu);
        h->addWidget(layoutButton);
    }
    // Under the picture: scenes and the setlist, pressed during a set too.
    m_strip = new QWidget;
    m_strip->setObjectName(QStringLiteral("liveStrip"));
    {
        auto* h = new QHBoxLayout(m_strip);
        h->setContentsMargins(4, 4, 4, 2);
        h->addWidget(new QLabel(tr("Scenes")));
        for (QToolButton* b : m_sceneButtons)
            h->addWidget(b);
        h->addSpacing(16);
        h->addWidget(new QLabel(tr("Setlist")));
        h->addWidget(m_songPrev);
        h->addWidget(m_song, 1);
        h->addWidget(m_songNext);
        h->addWidget(editSetlistButton);
        m_songNotes->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred);
        h->addWidget(m_songNotes, 1);
    }
    m_center = new QWidget;
    m_center->setObjectName(QStringLiteral("liveCenter"));
    {
        auto* v = new QVBoxLayout(m_center);
        v->setContentsMargins(0, 0, 0, 0);
        v->setSpacing(0);
        v->addWidget(m_display, 1);
        v->addWidget(m_switcher);
        v->addWidget(m_strip);
    }
    // The setup: chosen before a set, in sections that fold away.
    auto* sections = new QWidget;
    auto* sv = new QVBoxLayout(sections);
    sv->setContentsMargins(4, 4, 4, 4);
    sv->setSpacing(2);
    auto form = [] {
        auto* f = new QFormLayout;
        f->setContentsMargins(6, 2, 2, 8);
        f->setHorizontalSpacing(8);
        f->setVerticalSpacing(4);
        f->setFieldGrowthPolicy(QFormLayout::AllNonFixedFieldsGrow);
        return f;
    };
    auto row = [](std::initializer_list<QWidget*> widgets, int stretchFirst = 1) {
        auto* w = new QWidget;
        auto* h = new QHBoxLayout(w);
        h->setContentsMargins(0, 0, 0, 0);
        h->setSpacing(4);
        bool first = true;
        for (QWidget* x : widgets) {
            h->addWidget(x, first ? stretchFirst : 0);
            first = false;
        }
        return w;
    };
    {
        auto* f = form();
        f->addRow(tr("Source"), row({m_source, refresh, m_nextFile}));
        m_formatRow = row({new QLabel(tr("Camera")), m_format}, 0);
        static_cast<QHBoxLayout*>(m_formatRow->layout())->setStretch(1, 1);
        f->addRow(m_formatRow);
        f->addRow(tr("Audio"), row({m_audio, m_audioMeter}));
        // Framing beside the size (every size but Source size has one), so the panel doesn't grow a row.
        m_framingRow = m_framing;
        auto* sizeRow = row({m_size, m_framing}, 0);
        static_cast<QHBoxLayout*>(sizeRow->layout())->setStretch(0, 1);
        static_cast<QHBoxLayout*>(sizeRow->layout())->setStretch(1, 1);
        f->addRow(tr("Size"), sizeRow);
        f->addRow(tr("Rate"), row({m_fps, m_mirror}));
        m_panZoomLabel = new QLabel;
        m_panZoomLabel->setObjectName(QStringLiteral("livePanZoom"));
        auto* resetFrame = new QToolButton;
        resetFrame->setText(tr("Reset"));
        resetFrame->setToolTip(tr("Back to the whole source, as framed"));
        connect(resetFrame, &QToolButton::clicked, this, [this] { setPanZoom({}); });
        auto* frameRow = row({m_panZoomLabel, resetFrame});
        frameRow->setToolTip(tr("Pan and zoom the source in the output: drag the picture to move it, scroll on it to zoom\n"
                                "(up to 8x), right-click it for set zooms. With a portrait size it picks the part of the source\n"
                                "that fills the frame. Recordings, streams and everything sent out get it too."));
        m_panZoomLabel->setToolTip(frameRow->toolTip());
        f->addRow(tr("Frame"), frameRow);
        sv->addWidget(new LiveSection(tr("Input"), QStringLiteral("input"), f));
    }
    {
        auto* f = form();
        f->addRow(tr("Layer"), m_layerPick);
        f->addRow(tr("Source"), row({m_layer, m_nextLayerFile}));
        f->addRow(tr("Mode"), m_layerMode);
        f->addRow(tr("Where"), m_layerCorner);
        f->addRow(tr("Size"), row({m_layerSize, m_layerSizeLabel}));
        f->addRow(tr("Blend"), m_layerBlend);
        f->addRow(tr("Opacity"), m_layerOpacity);
        f->addRow(QString(), m_layerNoFx);
        sv->addWidget(new LiveSection(tr("Layer"), QStringLiteral("layer"), f));
    }
    {
        auto* f = form();
        for (int k = 0; k < LiveEngine::kMaxInputs; ++k)
            f->addRow(tr("Input %1").arg(k + 2), m_inputs[size_t(k)]);
        auto* hint = new QLabel(tr("Input 1 is the Source. With more inputs, a switcher appears under the picture: "
                                   "click one to preview it, then Cut or Auto."));
        hint->setWordWrap(true);
        theme::style(hint, QStringLiteral("color: @dim;"));
        f->addRow(hint);
        sv->addWidget(new LiveSection(tr("Switcher"), QStringLiteral("switcher"), f));
    }
    {
        auto* f = form();
        f->addRow(tr("Record"), m_recMode);
        f->addRow(tr("Replay"), row({m_replay, m_replayLength}, 0));
        f->addRow(QString(), overlays);
        sv->addWidget(new LiveSection(tr("Output"), QStringLiteral("output"), f));
    }
    {
        auto* f = form();
        f->addRow(row({m_spoutBox, m_cameraBox}, 0));
        m_shareStatus->setWordWrap(true);
        f->addRow(m_shareStatus);
        streamSettings->setText(tr("Stream Settings..."));
        streamSettings->setToolButtonStyle(Qt::ToolButtonTextOnly);
        f->addRow(tr("Stream"), streamSettings);
        sv->addWidget(new LiveSection(tr("Send"), QStringLiteral("send"), f));
    }
    {
        auto* f = form();
        f->addRow(tr("Device"), m_midi);
        f->addRow(m_velocity);
        auto* hint = new QLabel(tr("Right-click a pad, scene or button > Learn MIDI to map it."));
        hint->setWordWrap(true);
        theme::style(hint, QStringLiteral("color: @dim;"));
        f->addRow(hint);
        auto* oscRow = new QHBoxLayout;
        oscRow->addWidget(m_oscBox);
        oscRow->addWidget(m_oscPort);
        oscRow->addStretch(1);
        f->addRow(tr("OSC"), oscRow);
        f->addRow(m_oscStatus);
        sv->addWidget(new LiveSection(tr("MIDI"), QStringLiteral("midi"), f));
    }
    sv->addStretch(1);
    m_sidebar = new QScrollArea;
    m_sidebar->setObjectName(QStringLiteral("liveSetup"));
    m_sidebar->setWidget(sections);
    m_sidebar->setWidgetResizable(true);
    m_sidebar->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    m_sidebar->setFrameShape(QFrame::NoFrame);
    // As wide as the sections need with the scroll bar beside them (not over them), again whenever they change (a section
    // opened, the theme's fonts...).
    sections->installEventFilter(this);
    fitSidebar();
    layout->addWidget(m_showBar);
    m_mainLayout = layout;

    // ---- Settings ----
    QSettings settings;
    m_size->setCurrentIndex(std::clamp(settings.value(QStringLiteral("live/size"), 2).toInt(), 0, m_size->count() - 1));
    if (m_size->currentData().isNull() && m_size->currentIndex() > 0) // (the separator)
        m_size->setCurrentIndex(2);
    m_framing->setCurrentIndex(std::clamp(settings.value(QStringLiteral("live/framing"), 0).toInt(), 0, m_framing->count() - 1));
    m_fps->setCurrentIndex(std::max(0, m_fps->findData(settings.value(QStringLiteral("live/fps"), 30).toInt())));
    m_format->setCurrentIndex(std::clamp(settings.value(QStringLiteral("live/format"), 0).toInt(), 0, m_format->count() - 1));
    m_mirror->setChecked(settings.value(QStringLiteral("live/mirror"), false).toBool());
    m_recMode->setCurrentIndex(settings.value(QStringLiteral("live/recordClean"), false).toBool() ? 1 : 0);
    m_bottomSplit->restoreState(settings.value(QStringLiteral("live/bottomSplit")).toByteArray());
    setLayoutMode(std::clamp(settings.value(QStringLiteral("live/layout"), 0).toInt(), 0, kLayoutCount - 1));
    {
        LivePanZoom pz;
        pz.center = QPointF(settings.value(QStringLiteral("live/panX"), 0.5).toDouble(), settings.value(QStringLiteral("live/panY"), 0.5).toDouble());
        pz.zoom = settings.value(QStringLiteral("live/zoom"), 1.0).toDouble();
        m_panZoom = pz; // checked against the framing once the source's size is known
        m_engine->setPanZoom(pz);
    }
    for (int i = 0; i < kLiveLayers; ++i) { // ("live/layer", "live/layerMode"..., "live/layer2", "live/layer2Mode"...)
        auto value = [&](const char* what, const QVariant& def) {
            return settings.value(liveLayerKey(QStringLiteral("live/layer"), i) + QLatin1String(what), def);
        };
        LiveLayerSettings& s = m_layerSets[size_t(i)];
        m_layerNames[size_t(i)] = value("", QString()).toString();
        s.mode = LiveLayerSettings::Mode(std::clamp(value("Mode", 0).toInt(), 0, 1));
        s.corner = std::clamp(value("Corner", 3).toInt(), 0, 5);
        s.x = std::clamp(value("X", 0.5).toDouble(), 0.0, 1.0);
        s.y = std::clamp(value("Y", 0.5).toDouble(), 0.0, 1.0);
        s.size = std::clamp(value("Size", 0.25).toDouble(), 0.05, 1.0);
        s.blend = std::clamp(value("Blend", 0).toInt(), 0, m_layerBlend->count() - 1);
        s.opacity = std::clamp(value("Opacity", 100).toInt(), 0, 100) / 100.0;
        s.noEffects = value("NoEffects", false).toBool();
    }
    showLayer();
    m_replayLength->setCurrentIndex(std::max(0, m_replayLength->findData(settings.value(QStringLiteral("live/replaySeconds"), 30).toInt())));
    m_spoutBox->setChecked(m_spoutBox->isEnabled() && settings.value(QStringLiteral("live/spout"), false).toBool());
    m_cameraBox->setChecked(m_cameraBox->isEnabled() && settings.value(QStringLiteral("live/virtualCamera"), false).toBool());
    for (const QString& kv : settings.value(QStringLiteral("live/midiMap")).toStringList())
        if (const int eq = kv.indexOf(QLatin1Char('=')); eq > 0)
            m_midiMap[kv.left(eq)] = kv.mid(eq + 1);

    // ---- Wiring ----
    connect(refresh, &QToolButton::clicked, this, [this] {
        refreshSources();
        refreshAudioSources();
        refreshMidi();
    });
    connect(m_audio, qOverload<int>(&QComboBox::currentIndexChanged), this, [this] {
        updateControls();
        if (m_engine->isRunning())
            m_engine->setAudioSource(audioSource());
        saveSettings();
    });
    connect(m_engine, &LiveEngine::audioStatus, this, [this](const QString& description, const QString& error) {
        m_audioState = !error.isEmpty() ? tr("no audio: %1").arg(error) : description.isEmpty() ? QString() : tr("audio %1").arg(description);
        m_audio->setToolTip(m_audioState.isEmpty() ? audioHelp() : m_audioState + QStringLiteral("\n\n") + audioHelp());
        m_audioMeter->setPeaks(0, 0);
        if (!error.isEmpty())
            m_status->setText(tr("Audio input: %1").arg(error));
    });
    connect(m_engine, &LiveEngine::audioLevel, m_audioMeter, &AudioMeter::setPeaks);
    connect(m_start, &QPushButton::clicked, this, &LiveView::startStop);
    connect(m_rec, &QPushButton::clicked, this, &LiveView::toggleRecording);
    connect(m_recMode, qOverload<int>(&QComboBox::currentIndexChanged), this, [this] { saveSettings(); });
    connect(m_engine, &LiveEngine::recordingStarted, this, [this](const QString&, const QString& encoder) {
        m_recTime->setText(QStringLiteral("REC 0:00"));
        m_recTime->setToolTip(tr("Encoding with %1").arg(encoder));
        updateControls();
    });
    connect(m_engine, &LiveEngine::recordingProgress, this, [this](double secs, qint64 dropped) {
        const int s = int(secs);
        m_recTime->setText(QStringLiteral("REC %1:%2").arg(s / 60).arg(s % 60, 2, 10, QLatin1Char('0')));
        m_recTime->setToolTip(dropped ? tr("%1 frame(s) dropped: the encoder could not keep up").arg(dropped) : QString());
    });
    connect(m_engine, &LiveEngine::recordingFinished, this, [this](const QString& path, bool ok, const QString& error, double secs) {
        m_recTime->clear();
        if (ok)
            m_lastRecording = path;
        if (ok)
            m_status->setText(tr("Recorded %1 (%2 s): it is in the Editor's From Live tab").arg(QFileInfo(path).fileName()).arg(secs, 0, 'f', 1));
        else
            m_status->setText(tr("Recording failed: %1").arg(error));
        updateControls();
    });
    connect(full, &QPushButton::clicked, this, &LiveView::showFullScreenOutput);
    connect(m_popOutButton, &QPushButton::clicked, this, &LiveView::togglePopOut);
    connect(m_pads, &LivePadPanel::padPressed, this, &LiveView::triggerPad);
    connect(m_pads, &LivePadPanel::padCued, this, &LiveView::cuePad);
    connect(m_engine, &LiveEngine::cueFrameReady, this, [this](const QImage& img) {
        if (m_cueKind == 0)
            return;
        ++m_cueFrames;
        m_cueView->setImage(img);
    });
    connect(m_pads, &LivePadPanel::editRequested, this, &LiveView::editChain);
    connect(m_pads, &LivePadPanel::message, m_status, &QLabel::setText);
    connect(m_pads, &LivePadPanel::setLoaded, this, [this](const QString& name) {
        if (m_activePad >= 0)
            triggerPad(m_activePad); // the pads changed under it: switch it off (with the transition)
        updateDeviceLock();
        m_status->setText(name.isEmpty() ? tr("New empty pad set") : tr("Loaded the pad set \"%1\"").arg(name));
    });
    connect(m_project, &Project::changed, this, &LiveView::updateDeviceLock);
    connect(m_engine, &LiveEngine::beat, m_pads, &LivePadPanel::audioBeat);
    m_pads->setBeatTracker(m_engine->beatTracker());
    connect(m_engine, &LiveEngine::started, m_pads, [this] { m_pads->setRunning(true); });
    connect(m_engine, &LiveEngine::stopped, m_pads, [this] { m_pads->setRunning(false); });
    connect(m_engine, &LiveEngine::failed, m_pads, [this] { m_pads->setRunning(false); });
    connect(m_pads, &LivePadPanel::dropped, this, [this](int pad, const QString& mime, const QString& id) {
        if (m_pads->isLocked(pad)) {
            m_status->setText(tr("Pad %1 is locked (right-click it > Unlock Pad to change it)").arg(pad + 1));
            return;
        }
        editChain(pad + 1);
        if (mime == QLatin1String(EffectsBrowser::kPresetMime)) {
            if (m_devices->acceptsPreset(id))
                m_devices->addPreset(id);
        } else {
            m_devices->addEffect(id);
        }
    });
    connect(m_source, qOverload<int>(&QComboBox::currentIndexChanged), this, [this] {
        if (m_source->currentData().toInt() == -2) {
            chooseVideoFiles(m_source);
            return;
        }
        updateControls();
        updateSwitcher(); // (input 1's name)
        setPanZoom({}); // another picture: framed afresh
        if (m_engine->isRunning())
            m_engine->start(options()); // switch source live
    });
    connect(m_format, qOverload<int>(&QComboBox::currentIndexChanged), this, [this] {
        if (m_engine->isRunning() && options().source.kind == LiveSource::Kind::Device)
            m_engine->start(options());
        saveSettings();
    });
    auto applySize = [this] {
        const bool canvas = m_size->currentData(Qt::UserRole + 1).toBool();
        m_framingRow->setVisible(canvas);
        setPanZoom(m_panZoom); // the pan's limits follow the output's shape
        m_engine->setCanvas(canvas ? m_size->currentData().toSize() : QSize(), LiveFraming(m_framing->currentData().toInt()));
        if (!canvas)
            m_engine->setMaxSize(m_size->currentData().toSize());
        saveSettings();
    };
    connect(m_size, qOverload<int>(&QComboBox::currentIndexChanged), this, applySize);
    connect(m_framing, qOverload<int>(&QComboBox::currentIndexChanged), this, applySize);
    m_framingRow->setVisible(m_size->currentData(Qt::UserRole + 1).toBool());
    connect(m_fps, qOverload<int>(&QComboBox::currentIndexChanged), this, [this] {
        m_engine->setFps(m_fps->currentData().toDouble());
        saveSettings();
    });
    connect(m_mirror, &QCheckBox::toggled, this, [this](bool on) {
        m_engine->setMirror(on);
        saveSettings();
    });
    connect(m_browser, &EffectsBrowser::activated, m_devices, [this](const QString& id) {
        if (editingLocked())
            m_status->setText(tr("Pad %1 is locked").arg(m_pads->editing()));
        else
            m_devices->addEffect(id);
    });
    connect(m_browser, &EffectsBrowser::presetActivated, m_devices, [this](const QString& id) {
        if (editingLocked())
            m_status->setText(tr("Pad %1 is locked").arg(m_pads->editing()));
        else if (m_devices->acceptsPreset(id))
            m_devices->addPreset(id);
    });
    connect(m_project, &Project::changed, this, [this] { m_engine->setSnapshot(m_project->snapshot()); });
    m_engine->setSnapshot(m_project->snapshot());

    // The beat clock (modulation, overlays) follows the pads' clock.
    connect(m_pads, &LivePadPanel::beatTicked, m_engine, &LiveEngine::noteBeat);
    connect(m_pads, &LivePadPanel::tempoChanged, m_engine, &LiveEngine::setTempo);
    m_engine->setTempo(m_pads->bpm());
    connect(m_pads, &LivePadPanel::padHeld, this, &LiveView::holdPad);
    connect(m_pads, &LivePadPanel::padTouched, this, [this](int pad) { m_engine->setPadVelocity(pad, 1.0); });
    connect(m_pads, &LivePadPanel::padsMixChanged, m_engine, &LiveEngine::setPadsMix);
    connect(m_pads, &LivePadPanel::blackoutChanged, m_engine, &LiveEngine::setBlackout);
    connect(m_oscBox, &QCheckBox::toggled, this, &LiveView::updateOsc);
    connect(m_oscPort, &QSpinBox::editingFinished, this, &LiveView::updateOsc);
    updateOsc();
    connect(m_pads, &LivePadPanel::midiLearnRequested, this, &LiveView::learnMidi);
    // Files: the next one in the playlist.
    connect(m_nextFile, &QToolButton::clicked, this, [this] { m_engine->nextFile(-1); });
    connect(m_nextLayerFile, &QToolButton::clicked, this, [this] { m_engine->nextFile(m_layerIndex); });
    // The layers: the controls show one at a time.
    connect(m_layerPick, qOverload<int>(&QComboBox::currentIndexChanged), this, [this](int i) {
        m_layerIndex = std::clamp(i, 0, kLiveLayers - 1);
        showLayer();
    });
    auto layerChanged = [this] {
        LiveLayerSettings& s = m_layerSets[size_t(m_layerIndex)];
        s.mode = LiveLayerSettings::Mode(m_layerMode->currentIndex());
        s.corner = m_layerCorner->currentIndex();
        s.size = m_layerSize->value() / 100.0;
        s.blend = m_layerBlend->currentIndex();
        s.opacity = m_layerOpacity->value() / 100.0;
        s.noEffects = m_layerNoFx->isChecked();
        m_engine->setLayerSettings(m_layerIndex, s);
        updateLayerControls();
        saveSettings();
    };
    connect(m_layer, qOverload<int>(&QComboBox::activated), this, [this] {
        if (m_layer->currentData().toInt() == -2) {
            chooseVideoFiles(m_layer);
            return;
        }
        const int i = m_layer->currentData().toInt();
        m_layerNames[size_t(m_layerIndex)] = i >= 0 && i < int(m_sources.size()) ? m_sources[size_t(i)].name : QString();
        if (m_engine->isRunning())
            m_engine->setLayer(m_layerIndex, layerSource(m_layerIndex));
        updateLayerControls();
        saveSettings();
    });
    connect(m_layerMode, qOverload<int>(&QComboBox::currentIndexChanged), this, layerChanged);
    connect(m_layerCorner, qOverload<int>(&QComboBox::currentIndexChanged), this, layerChanged);
    connect(m_layerSize, &QSlider::valueChanged, this, layerChanged);
    connect(m_layerBlend, qOverload<int>(&QComboBox::currentIndexChanged), this, layerChanged);
    connect(m_layerOpacity, &QSlider::valueChanged, this, layerChanged);
    connect(m_layerNoFx, &QCheckBox::toggled, this, layerChanged);
    for (QComboBox* c : m_inputs)
        connect(c, qOverload<int>(&QComboBox::activated), this, [this] {
            if (m_engine->isRunning())
                m_engine->setInputs(inputSources());
            updateSwitcher();
            saveSettings();
        });
    connect(m_engine, &LiveEngine::inputFrames, this, [this](const QList<QImage>& pictures) {
        for (int k = 0; k < int(pictures.size()) && k <= LiveEngine::kMaxInputs; ++k)
            m_inputTiles[size_t(k)]->setIcon(pictures[k].isNull() ? QIcon() : QIcon(QPixmap::fromImage(pictures[k])));
    });
    connect(m_engine, &LiveEngine::layerStatus, this, [this](int layer, const QString& description, const QString& error) {
        if (!error.isEmpty())
            m_status->setText(tr("Layer %1: %2").arg(layer + 1).arg(error));
        else if (!description.isEmpty())
            m_status->setText(tr("Layer %1: %2").arg(layer + 1).arg(description));
    });
    // The replay buffer.
    auto replayChanged = [this] {
        m_engine->setReplay(m_replay->isChecked(), m_replayLength->currentData().toDouble(), m_recMode->currentIndex() == 0);
        saveSettings();
    };
    connect(m_replay, &QCheckBox::toggled, this, replayChanged);
    connect(m_replayLength, qOverload<int>(&QComboBox::currentIndexChanged), this, replayChanged);
    connect(m_recMode, qOverload<int>(&QComboBox::currentIndexChanged), this, replayChanged);
    connect(m_saveReplay, &QPushButton::clicked, this, &LiveView::saveReplay);
    connect(m_screenshot, &QPushButton::clicked, this, &LiveView::saveScreenshot);
    struct Learnable {
        QPushButton* button;
        QString action, label;
    };
    for (const Learnable& l : {Learnable{m_saveReplay, QStringLiteral("replay"), tr("Save Replay")}, Learnable{m_rec, QStringLiteral("rec"), tr("Rec")},
                               Learnable{m_screenshot, QStringLiteral("screenshot"), tr("Screenshot")}}) {
        QPushButton* button = l.button;
        const QString action = l.action, label = l.label;
        button->setContextMenuPolicy(Qt::CustomContextMenu);
        connect(button, &QWidget::customContextMenuRequested, this, [this, button, action, label](const QPoint& pos) {
            QMenu menu(this);
            menu.addAction(tr("Learn MIDI..."), this, [this, action, label] { learnMidi(action, label); });
            menu.exec(button->mapToGlobal(pos));
        });
    }
    connect(m_engine, &LiveEngine::replayStatus, this, [this](bool running, const QString& encoder, const QString& error) {
        m_saveReplay->setToolTip(running ? tr("Save the last %1 s as a clip (encoding with %2)").arg(m_replayLength->currentData().toInt()).arg(encoder)
                                         : tr("Save the replay buffer as a clip (it appears in the Editor's From Live tab)"));
        if (!error.isEmpty())
            m_status->setText(tr("Replay buffer: %1").arg(error));
        updateControls();
    });
    connect(m_engine, &LiveEngine::replaySaved, this, [this](const QString& path, bool ok, const QString& error, double secs) {
        m_status->setText(ok ? tr("Saved the replay %1 (%2 s): it is in the Editor's From Live tab").arg(QFileInfo(path).fileName()).arg(secs, 0, 'f', 1)
                             : tr("Could not save the replay: %1").arg(error));
    });
    // MIDI.
    connect(m_midi, qOverload<int>(&QComboBox::activated), this, [this] {
        const QString name = m_midi->currentData().toString();
        QString error;
        if (name.isEmpty())
            MidiHub::instance()->close();
        else if (!MidiHub::instance()->open(name, &error))
            m_status->setText(tr("MIDI: %1").arg(error));
        else
            m_status->setText(tr("MIDI: listening to %1").arg(name));
        QSettings().setValue(QStringLiteral("live/midi"), name);
    });
    connect(MidiHub::instance(), &MidiHub::message, this, &LiveView::midiMessage);
    connect(m_project, &Project::changed, this, &LiveView::refreshScenes);
    // Setlist.
    connect(m_project, &Project::changed, this, &LiveView::refreshSetlist);
    connect(m_songPrev, &QToolButton::clicked, this, [this] { stepSong(-1); });
    connect(m_songNext, &QToolButton::clicked, this, [this] { stepSong(1); });
    connect(m_song, qOverload<int>(&QComboBox::activated), this, [this](int item) {
        const int song = m_song->itemData(item).toInt();
        if (song >= 0)
            recallSong(song);
    });
    connect(editSetlistButton, &QPushButton::clicked, this, &LiveView::editSetlist);
    struct SongButton {
        QToolButton* button;
        QString action, label;
    };
    for (const SongButton& sb : {SongButton{m_songPrev, QStringLiteral("song:prev"), tr("previous song")},
                                 SongButton{m_songNext, QStringLiteral("song:next"), tr("next song")}}) {
        QToolButton* button = sb.button;
        const QString action = sb.action, label = sb.label;
        button->setContextMenuPolicy(Qt::CustomContextMenu);
        connect(button, &QWidget::customContextMenuRequested, this, [this, button, action, label](const QPoint& pos) {
            QMenu menu(this);
            menu.addAction(tr("Learn MIDI..."), this, [this, action, label] { learnMidi(action, label); });
            menu.exec(button->mapToGlobal(pos));
        });
    }
    // Sending the output to other apps.
    connect(m_spoutBox, &QCheckBox::toggled, this, [this](bool on) {
        QSettings().setValue(QStringLiteral("live/spout"), on);
        updateSharing();
    });
    connect(m_cameraBox, &QCheckBox::toggled, this, [this](bool on) {
        QSettings().setValue(QStringLiteral("live/virtualCamera"), on);
        updateSharing();
    });
    connect(m_spoutBox, &QWidget::customContextMenuRequested, this, [this](const QPoint& pos) {
        QMenu menu(this);
        menu.addAction(tr("Sender Name..."), this, [this] {
            bool ok = false;
            const QString name = QInputDialog::getText(this, tr("Spout Sender"), tr("Name other apps see this output by:"), QLineEdit::Normal,
                                                       QSettings().value(QStringLiteral("live/spoutName"), QStringLiteral("Mutant Video Producer")).toString(), &ok)
                                     .trimmed();
            if (ok && !name.isEmpty()) {
                QSettings().setValue(QStringLiteral("live/spoutName"), name);
                updateSharing();
            }
        });
        menu.exec(m_spoutBox->mapToGlobal(pos));
    });
    auto styleGoLive = [this](int state) { // 0 off, 1 connecting, 2 live
        m_goLive->setText(state == 2 ? tr("■  End Stream") : state == 1 ? tr("Connecting...") : tr("Go Live"));
        theme::style(m_goLive, state == 2   ? QStringLiteral("QPushButton { background: @bad; color: @window; font-weight: bold; }")
                                : state == 1 ? QStringLiteral("QPushButton { background: @warn; color: @window; }")
                                             : QString());
    };
    connect(m_engine, &LiveEngine::streamStatus, this,
            [this, styleGoLive](bool connecting, bool live, const QString& encoder, const QString& error) {
                m_streamLive = live;
                styleGoLive(live ? 2 : connecting ? 1 : 0);
                if (live) {
                    theme::style(m_streamStatus, QStringLiteral("color: @bad; font-weight: bold;"));
                    m_streamStatus->setText(tr("LIVE on %1").arg(StreamDialog::serviceName()));
                    m_streamStatus->setToolTip(tr("Encoding with %1 at %2 kb/s").arg(encoder).arg(StreamDialog::bitrateKbps()));
                } else if (connecting) {
                    theme::style(m_streamStatus, QStringLiteral("color: @warn;"));
                    m_streamStatus->setText(tr("connecting to %1...").arg(StreamDialog::serviceName()));
                } else {
                    theme::style(m_streamStatus, error.isEmpty() ? QStringLiteral("color: @dim;") : QStringLiteral("color: @bad;"));
                    m_streamStatus->setText(error.isEmpty() ? tr("stream ended") : tr("stream: %1").arg(error));
                    m_streamStatus->setToolTip(error);
                }
            });
    connect(m_engine, &LiveEngine::streamProgress, this, [this](double seconds, qint64 dropped) {
        if (!m_streamLive)
            return;
        const int s = int(seconds);
        m_streamStatus->setText(tr("LIVE on %1  %2:%3:%4%5")
                                    .arg(StreamDialog::serviceName())
                                    .arg(s / 3600)
                                    .arg(s / 60 % 60, 2, 10, QLatin1Char('0'))
                                    .arg(s % 60, 2, 10, QLatin1Char('0'))
                                    .arg(dropped > 0 ? tr("  (%1 pictures dropped: lower the bitrate or size)").arg(dropped) : QString()));
    });
    connect(m_engine, &LiveEngine::sharingStatus, this,
            [this](const QString& spoutName, const QString& spoutError, QSize cameraSize, const QString& cameraError) {
                m_spoutSender = spoutName;
                m_cameraSize = cameraSize;
                QStringList parts;
                if (!spoutName.isEmpty())
                    parts << tr("Spout \"%1\"").arg(spoutName);
                if (cameraSize.isValid())
                    parts << tr("camera %1x%2").arg(cameraSize.width()).arg(cameraSize.height());
                if (!spoutError.isEmpty()) {
                    parts << tr("Spout: %1").arg(spoutError);
                    QSignalBlocker block(m_spoutBox);
                    m_spoutBox->setChecked(false);
                }
                if (!cameraError.isEmpty())
                    parts << tr("camera: %1").arg(cameraError);
                m_shareStatus->setText(parts.join(QStringLiteral("  ·  ")));
                theme::style(m_shareStatus, spoutError.isEmpty() && cameraError.isEmpty() ? QStringLiteral("color: @good;")
                                                                                          : QStringLiteral("color: @bad;"));
                if (parts.isEmpty())
                    updateSharing(); // the hint while not capturing
            });
    // Overlays.
    connect(overlays, &QPushButton::clicked, this, [this] {
        if (!m_overlays)
            m_overlays = new LiveOverlaysDialog(m_project, this);
        m_overlays->show();
        m_overlays->raise();
        m_overlays->activateWindow();
    });
    qApp->installEventFilter(this);

    connect(m_engine, &LiveEngine::frameReady, this, [this, display](const QImage& img) {
        ++m_frames;
        if (m_popOut && m_popOut->isVisible()) {
            m_popOut->setImage(img);
        } else {
            display->hint.clear();
            m_display->setImage(img);
        }
        if (m_fullScreen && m_fullScreen->isVisible())
            m_fullScreen->setImage(img);
    });
    connect(m_engine, &LiveEngine::started, this, [this](const QString& how) {
        m_status->setText(tr("Opened %1").arg(how));
        m_status->setToolTip(how);
        updateControls();
    });
    connect(m_engine, &LiveEngine::failed, this, [this](const QString& error) {
        m_status->setText(tr("Could not capture: %1").arg(error));
        m_status->setToolTip(error);
        updateControls();
    });
    connect(m_engine, &LiveEngine::stopped, this, [this] {
        m_status->setText(tr("Stopped"));
        m_audioState.clear();
        m_audioMeter->setPeaks(0, 0);
        updateControls();
    });
    connect(m_engine, &LiveEngine::statsChanged, this,
            [this](double captureFps, double renderFps, double ms, const QString& renderer, QSize src) {
                if (src != m_sourceSize) {
                    m_sourceSize = src;
                    setPanZoom(m_panZoom); // the limits of the pan, now the source's shape is known
                }
                m_status->setText(tr("Source %1x%2 at %3 fps  ·  effects %4 fps, %5 ms/frame (%6)")
                                      .arg(src.width())
                                      .arg(src.height())
                                      .arg(captureFps, 0, 'f', 0)
                                      .arg(renderFps, 0, 'f', 0)
                                      .arg(ms, 0, 'f', 1)
                                      .arg(renderer)
                                  + (m_audioState.isEmpty() ? QString() : QStringLiteral("  ·  ") + m_audioState));
            });
    refreshSources();
    refreshAudioSources();
    refreshMidi();
    refreshScenes();
    refreshSetlist();
    updateLayerControls();
    updateControls();
    updateSharing();
}

void LiveView::refreshSetlist()
{
    const std::vector<LiveSong>& songs = m_project->timeline().liveSetlist;
    if (m_currentSong >= int(songs.size()))
        m_currentSong = -1;
    QSignalBlocker block(m_song);
    m_song->clear();
    if (songs.empty())
        m_song->addItem(tr("(no songs: Edit Setlist...)"), -1);
    for (int i = 0; i < int(songs.size()); ++i) {
        const LiveSong& s = songs[size_t(i)];
        QString text = QStringLiteral("%1. %2").arg(i + 1).arg(s.name.isEmpty() ? tr("Song %1").arg(i + 1) : s.name);
        if (s.bpm > 0)
            text += QStringLiteral("  ·  %1 BPM").arg(s.bpm, 0, 'f', s.bpm == std::round(s.bpm) ? 0 : 2);
        m_song->addItem(text, i);
    }
    m_song->setCurrentIndex(songs.empty() ? 0 : m_currentSong);
    m_songPrev->setEnabled(!songs.empty());
    m_songNext->setEnabled(!songs.empty());
    m_songNotes->setText(m_currentSong >= 0 ? songs[size_t(m_currentSong)].notes : QString());
}

void LiveView::recallSong(int index)
{
    const std::vector<LiveSong> songs = m_project->timeline().liveSetlist;
    if (index < 0 || index >= int(songs.size())) {
        m_status->setText(songs.empty() ? tr("The setlist is empty: Edit Setlist... to add songs") : tr("There is no song %1").arg(index + 1));
        return;
    }
    const LiveSong s = songs[size_t(index)];
    m_currentSong = index;
    QStringList problems;
    // Its pad set, as saved (unless it is on already, untouched).
    if (!s.padSet.isEmpty() && (m_pads->currentSet() != s.padSet || m_pads->setModified()) && !m_pads->loadSet(s.padSet))
        problems << tr("the pad set \"%1\" is missing").arg(s.padSet);
    m_pads->setSongTempo(s.bpm, s.lockBpm);
    if (s.scene >= 0)
        recallScene(s.scene);
    refreshSetlist();
    const QString name = s.name.isEmpty() ? tr("Song %1").arg(index + 1) : s.name;
    m_status->setText(tr("Song %1: %2").arg(index + 1).arg(name)
                      + (s.bpm > 0 ? tr(", %1 BPM%2").arg(s.bpm, 0, 'f', 2).arg(s.lockBpm ? tr(" locked") : QString()) : tr(", finding the tempo"))
                      + (problems.isEmpty() ? QString() : tr(" (%1)").arg(problems.join(QStringLiteral(", ")))));
}

void LiveView::stepSong(int delta)
{
    const int n = int(m_project->timeline().liveSetlist.size());
    if (n == 0) {
        recallSong(0); // says the setlist is empty
        return;
    }
    const int next = m_currentSong < 0 ? (delta > 0 ? 0 : n - 1) : std::clamp(m_currentSong + delta, 0, n - 1);
    if (next == m_currentSong) {
        m_status->setText(delta > 0 ? tr("That was the last song") : tr("This is the first song"));
        return;
    }
    recallSong(next);
}

void LiveView::editSetlist()
{
    SetlistDialog dlg(m_project->timeline().liveSetlist, [this] {
        LiveSong s;
        s.name = m_pads->currentSet();
        s.padSet = m_pads->currentSet();
        s.bpm = m_pads->bpm();
        s.lockBpm = m_pads->lockBpmButton()->isChecked();
        s.scene = m_activeScene;
        return s;
    }, this);
    if (dlg.exec() != QDialog::Accepted)
        return;
    Timeline tl = m_project->timeline();
    tl.liveSetlist = dlg.songs();
    if (tl != m_project->timeline())
        m_project->applyEdit(tl, tr("Edit Setlist"));
}

void LiveView::updateSharing()
{
    const QString name = QSettings().value(QStringLiteral("live/spoutName"), QStringLiteral("Mutant Video Producer")).toString();
    m_engine->setSharing(m_spoutBox->isChecked(), name, m_cameraBox->isChecked());
    if (!m_engine->isRunning()) {
        theme::style(m_shareStatus, QStringLiteral("color: @dim;"));
        m_shareStatus->setText(m_spoutBox->isChecked() || m_cameraBox->isChecked() ? tr("(sends while capturing)") : QString());
    }
}

void LiveView::refreshScenes()
{
    const LiveScenes& scenes = m_project->timeline().liveScenes;
    for (int i = 0; i < kLiveScenes; ++i) {
        QToolButton* b = m_sceneButtons[size_t(i)];
        const LiveScene& s = scenes[size_t(i)];
        const QString name = s.name.isEmpty() ? tr("Scene %1").arg(i + 1) : s.name;
        b->setToolTip(s.used ? tr("%1 (F%2): click to recall the look; right-click to save over it, rename or learn MIDI").arg(name).arg(i + 1)
                             : tr("Scene %1 (F%1): empty. Right-click > Save the Current Look Here: the Live FX, the active pad,\n"
                                  "the layer and its FX, and the overlays").arg(i + 1));
        const bool cued = m_cueKind == LiveEngine::CueScene && m_cueIndex == i;
        theme::style(b, cued && s.used                  ? QStringLiteral("QToolButton { background: @raised; color: @text; border: 2px solid @cue; }")
                         : i == m_activeScene && s.used ? QStringLiteral("QToolButton { background: @accent; color: @onAccent; font-weight: bold; }")
                         : s.used                     ? QStringLiteral("QToolButton { background: @raised; color: @text; }")
                                                      : QStringLiteral("QToolButton { color: @faint; }"));
    }
}

void LiveView::saveScene(int scene)
{
    if (scene < 0 || scene >= kLiveScenes)
        return;
    Timeline tl = m_project->timeline();
    LiveScene& s = tl.liveScenes[size_t(scene)];
    const QString name = s.name;
    s = LiveScene();
    s.used = true;
    s.name = name;
    s.liveEffects = tl.liveEffects;
    s.pad = m_activePad;
    for (int i = 0; i < kLiveLayers; ++i)
        s.layers[size_t(i)] = {layerSource(i).name, m_layerSets[size_t(i)], tl.liveLayerEffects[size_t(i)]};
    s.overlays = tl.liveOverlays;
    m_activeScene = scene;
    m_project->applyEdit(tl, tr("Save Scene %1").arg(scene + 1));
    m_status->setText(tr("Saved the current look as scene %1").arg(scene + 1));
}

void LiveView::recallScene(int scene)
{
    if (scene < 0 || scene >= kLiveScenes)
        return;
    const LiveScene s = m_project->timeline().liveScenes[size_t(scene)];
    if (!s.used) {
        m_status->setText(tr("Scene %1 is empty: right-click it > Save the Current Look Here").arg(scene + 1));
        return;
    }
    m_activeScene = scene;
    Timeline tl = m_project->timeline();
    tl.liveEffects = s.liveEffects;
    for (int i = 0; i < kLiveLayers; ++i)
        tl.liveLayerEffects[size_t(i)] = s.layers[size_t(i)].effects;
    tl.liveOverlays = s.overlays;
    if (tl != m_project->timeline())
        m_project->applyEdit(tl, tr("Recall Scene %1").arg(scene + 1));
    if (s.pad != m_activePad) { // with the pad transition
        const int before = m_activePad;
        m_activePad = s.pad;
        m_pads->setActivePad(m_activePad);
        m_engine->setPad(m_activePad, m_pads->transition(), m_pads->transitionSeconds());
        followPad(before);
    }
    // The layers: their sources (if they are still there) and how they are laid over.
    for (int i = 0; i < kLiveLayers; ++i) {
        const LiveLayer& l = s.layers[size_t(i)];
        m_layerNames[size_t(i)] = l.source;
        m_layerSets[size_t(i)] = l.settings;
        if (!l.source.isEmpty() && layerSource(i).name.isEmpty())
            m_status->setText(tr("Scene %1: layer %2's source \"%3\" isn't available").arg(scene + 1).arg(i + 1).arg(l.source));
        m_engine->setLayerSettings(i, l.settings);
        if (m_engine->isRunning())
            m_engine->setLayer(i, layerSource(i));
    }
    showLayer();
    refreshScenes();
    saveSettings();
    const QString name = s.name.isEmpty() ? tr("scene %1").arg(scene + 1) : s.name;
    if (m_status->text().isEmpty() || !m_status->text().startsWith(tr("Scene %1:").arg(scene + 1)))
        m_status->setText(tr("Recalled %1").arg(name));
}

LiveView::~LiveView()
{
    qApp->removeEventFilter(this);
    saveSettings();
    m_engine->stop();
    delete m_fullScreen;
    if (m_popOut && m_popOut->isVisible())
        QSettings().setValue(QStringLiteral("live/popOutGeometry"), m_popOut->saveGeometry());
    delete m_popOut;
}

void LiveView::saveSettings() const
{
    QSettings settings;
    settings.setValue(QStringLiteral("live/size"), m_size->currentIndex());
    settings.setValue(QStringLiteral("live/framing"), m_framing->currentIndex());
    settings.setValue(QStringLiteral("live/fps"), m_fps->currentData().toInt());
    settings.setValue(QStringLiteral("live/format"), m_format->currentIndex());
    settings.setValue(QStringLiteral("live/mirror"), m_mirror->isChecked());
    settings.setValue(QStringLiteral("live/recordClean"), m_recMode->currentIndex() == 1);
    saveLayoutState();
    settings.setValue(QStringLiteral("live/bottomSplit"), m_bottomSplit->saveState());
    const int i = m_source->currentData().toInt();
    if (i >= 0 && i < int(m_sources.size()))
        settings.setValue(QStringLiteral("live/source"), m_sources[size_t(i)].name);
    if (m_audio->count() > 0)
        settings.setValue(QStringLiteral("live/audio"), audioSource().name);
    if (m_inputs[0]->count() > 0) {
        QStringList inputs;
        for (const LiveSource& s : inputSources())
            inputs << s.name;
        settings.setValue(QStringLiteral("live/inputs"), inputs);
    }
    for (int i = 0; i < kLiveLayers; ++i) {
        const QString key = liveLayerKey(QStringLiteral("live/layer"), i);
        const LiveLayerSettings& s = m_layerSets[size_t(i)];
        settings.setValue(key, m_layerNames[size_t(i)]);
        settings.setValue(key + QLatin1String("Mode"), int(s.mode));
        settings.setValue(key + QLatin1String("Corner"), s.corner);
        settings.setValue(key + QLatin1String("X"), s.x);
        settings.setValue(key + QLatin1String("Y"), s.y);
        settings.setValue(key + QLatin1String("Size"), s.size);
        settings.setValue(key + QLatin1String("Blend"), s.blend);
        settings.setValue(key + QLatin1String("Opacity"), int(std::lround(s.opacity * 100)));
        settings.setValue(key + QLatin1String("NoEffects"), s.noEffects);
    }
    settings.setValue(QStringLiteral("live/replaySeconds"), m_replayLength->currentData().toInt());
}

void LiveView::refreshAudioSources()
{
    const QString current = m_audio->count() > 0 ? audioSource().name : QSettings().value(QStringLiteral("live/audio")).toString();
    QSignalBlocker block(m_audio);
    m_audio->clear();
    m_audioSources = listLiveAudioSources();
    m_audio->addItem(tr("No audio"), -1);
    int select = 0;
    for (size_t i = 0; i < m_audioSources.size(); ++i) {
        m_audio->addItem(m_audioSources[i].name, int(i));
        if (m_audioSources[i].name == current)
            select = m_audio->count() - 1;
    }
    m_audio->setCurrentIndex(select);
    if (m_engine->isRunning())
        m_engine->setAudioSource(audioSource());
    updateControls();
}

LiveAudioSource LiveView::audioSource() const
{
    const int i = m_audio->currentData().toInt();
    return i >= 0 && i < int(m_audioSources.size()) ? m_audioSources[size_t(i)] : LiveAudioSource();
}

void LiveView::refreshSources()
{
    // (Choosing video files: what was chosen before, unless the dialog chose new ones.)
    const QString current = m_source->currentIndex() >= 0 && m_source->currentData().toInt() >= 0
                                ? m_source->currentText()
                                : QSettings().value(QStringLiteral("live/source")).toString();
    QSignalBlocker block(m_source);
    m_source->clear();
    m_sources = listLiveSources();
    m_sources.push_back(editorLiveSource());
    // Video files chosen before (newest first), as playlists.
    for (const QVariant& v : QSettings().value(QStringLiteral("live/playlists")).toList()) {
        const QStringList paths = v.toStringList();
        if (paths.isEmpty())
            continue;
        LiveSource s;
        s.kind = LiveSource::Kind::File;
        s.id = paths.join(QLatin1Char('\n'));
        s.name = paths.size() == 1 ? tr("Video: %1").arg(QFileInfo(paths.front()).fileName())
                                   : tr("Videos: %1 + %2 more").arg(QFileInfo(paths.front()).fileName()).arg(paths.size() - 1);
        m_sources.push_back(s);
    }
    auto* model = qobject_cast<QStandardItemModel*>(m_source->model());
    auto header = [&](const QString& text) {
        m_source->addItem(text, -1);
        if (model) {
            QStandardItem* item = model->item(m_source->count() - 1);
            item->setEnabled(false);
            QFont f = item->font();
            f.setBold(true);
            item->setFont(f);
        }
    };
    const struct {
        LiveSource::Kind kind;
        QString title;
    } groups[] = {{LiveSource::Kind::Device, tr("Cameras & capture devices")}, {LiveSource::Kind::Screen, tr("Screens")},
                  {LiveSource::Kind::Window, tr("Windows")}, {LiveSource::Kind::File, tr("Video files (looped)")},
                  {LiveSource::Kind::Editor, tr("Editor (the timeline playing, looped)")}};
    int select = -1, firstScreen = -1;
    for (const auto& g : groups) {
        header(g.title);
        if (g.kind == LiveSource::Kind::File)
            m_source->addItem(tr("   Choose video files..."), -2);
        bool any = g.kind == LiveSource::Kind::File;
        for (size_t i = 0; i < m_sources.size(); ++i) {
            if (m_sources[i].kind != g.kind)
                continue;
            any = true;
            m_source->addItem(QStringLiteral("   ") + m_sources[i].name, int(i));
            if (m_sources[i].name == current.trimmed())
                select = m_source->count() - 1;
            if (g.kind == LiveSource::Kind::Screen && firstScreen < 0)
                firstScreen = m_source->count() - 1;
        }
        if (!any) {
            header(tr("   (none found)"));
        }
    }
    if (select < 0)
        select = firstScreen;
    if (select >= 0)
        m_source->setCurrentIndex(select);
    // A layer: none, or any of the same sources.
    {
        QSignalBlocker blockLayer(m_layer);
        m_layer->clear();
        m_layer->addItem(tr("No layer"), -3);
        for (size_t i = 0; i < m_sources.size(); ++i)
            m_layer->addItem(m_sources[i].name, int(i));
        m_layer->addItem(tr("Choose video files..."), -2);
    }
    showLayer();
    // The switcher's inputs: off, or any of the same sources.
    {
        const QStringList saved = QSettings().value(QStringLiteral("live/inputs")).toStringList();
        for (int k = 0; k < LiveEngine::kMaxInputs; ++k) {
            QComboBox* c = m_inputs[size_t(k)];
            const QString current = c->count() > 0 ? (c->currentData().toInt() >= 0 ? m_sources[size_t(c->currentData().toInt())].name : QString())
                                                   : saved.value(k);
            QSignalBlocker block(c);
            c->clear();
            c->addItem(tr("Off"), -3);
            for (size_t i = 0; i < m_sources.size(); ++i) {
                c->addItem(m_sources[i].name, int(i));
                if (!current.isEmpty() && m_sources[i].name == current)
                    c->setCurrentIndex(c->count() - 1);
            }
        }
    }
    updateSwitcher();
    updateLayerControls();
    updateControls();
}

void LiveView::addPlaylist(const QStringList& paths)
{
    QSettings settings;
    QVariantList lists = settings.value(QStringLiteral("live/playlists")).toList();
    for (int i = int(lists.size()) - 1; i >= 0; --i)
        if (lists[i].toStringList() == paths)
            lists.removeAt(i);
    lists.prepend(paths);
    while (lists.size() > 6)
        lists.removeLast();
    settings.setValue(QStringLiteral("live/playlists"), lists);
}

void LiveView::chooseVideoFiles(QComboBox* combo)
{
    QSettings settings;
    const QStringList paths = QFileDialog::getOpenFileNames(this, tr("Video Files (played in a loop, one after the other)"),
                                                            settings.value(QStringLiteral("dirs/liveVideos")).toString(),
                                                            tr("Videos (*.mp4 *.mov *.mkv *.webm *.avi *.m4v *.mpg *.mpeg *.wmv *.flv *.gif);;All files (*)"));
    if (!paths.isEmpty()) {
        settings.setValue(QStringLiteral("dirs/liveVideos"), QFileInfo(paths.front()).absolutePath());
        addPlaylist(paths);
        const QString name = paths.size() == 1 ? tr("Video: %1").arg(QFileInfo(paths.front()).fileName())
                                               : tr("Videos: %1 + %2 more").arg(QFileInfo(paths.front()).fileName()).arg(paths.size() - 1);
        if (combo == m_layer)
            m_layerNames[size_t(m_layerIndex)] = name;
        else
            settings.setValue(QStringLiteral("live/source"), name);
    }
    // Rebuild the lists (the new playlist selected, or the previous choice back).
    refreshSources();
    if (!paths.isEmpty() && m_engine->isRunning()) {
        if (combo == m_source)
            m_engine->start(options());
        else
            m_engine->setLayer(m_layerIndex, layerSource(m_layerIndex));
    }
    saveSettings();
}

std::vector<LiveSource> LiveView::inputSources() const
{
    std::vector<LiveSource> list;
    for (QComboBox* c : m_inputs) {
        const int i = c->currentData().toInt();
        list.push_back(i >= 0 && i < int(m_sources.size()) ? m_sources[size_t(i)] : LiveSource());
    }
    return list;
}

void LiveView::updateSwitcher()
{
    const std::vector<LiveSource> inputs = inputSources();
    const bool any = std::any_of(inputs.begin(), inputs.end(), [](const LiveSource& s) { return !s.name.isEmpty(); });
    m_switcher->setVisible(any);
    for (int k = 0; k <= LiveEngine::kMaxInputs; ++k) {
        QToolButton* t = m_inputTiles[size_t(k)];
        const QString name = k == 0 ? m_source->currentText().trimmed() : inputs[size_t(k - 1)].name;
        t->setVisible(k == 0 || !name.isEmpty());
        t->setText(QStringLiteral("%1  %2").arg(k + 1).arg(QFontMetrics(t->font()).elidedText(name, Qt::ElideRight, 110)));
        const QString border = k == m_program ? QStringLiteral("@bad") : (k == m_preview ? QStringLiteral("@good") : QStringLiteral("@line"));
        theme::style(t, QStringLiteral("QToolButton { border: 3px solid %1; border-radius: 3px; padding: 1px; }").arg(border));
    }
}

void LiveView::setPreviewInput(int input)
{
    m_preview = std::clamp(input, 0, LiveEngine::kMaxInputs);
    updateSwitcher();
}

void LiveView::cutTo(int input, bool withTransition)
{
    input = std::clamp(input, 0, LiveEngine::kMaxInputs);
    if (input == m_program)
        return;
    m_engine->setProgram(input, withTransition ? m_pads->transition() : TransitionSpec(), withTransition ? m_pads->transitionSeconds() : 0.0);
    m_preview = m_program; // what was on air waits in preview
    m_program = input;
    updateSwitcher();
}

int LiveView::layerAt(QPointF at) const
{
    const QSize out = m_display->image().size();
    if (out.isEmpty())
        return -1;
    const QPointF p(at.x() / out.width(), at.y() / out.height());
    const std::array<QRectF, kLiveLayers> rects = m_engine->layerRects();
    for (int i = kLiveLayers - 1; i >= 0; --i) // (the topmost first)
        if (rects[size_t(i)].contains(p))
            return i;
    return -1;
}

LiveSource LiveView::layerSource(int layer) const
{
    const QString& name = m_layerNames[size_t(std::clamp(layer, 0, kLiveLayers - 1))];
    if (!name.isEmpty())
        for (const LiveSource& s : m_sources)
            if (s.name == name)
                return s;
    return {};
}

void LiveView::showLayer()
{
    const LiveLayerSettings& s = m_layerSets[size_t(m_layerIndex)];
    {
        QSignalBlocker b1(m_layer), b2(m_layerMode), b3(m_layerCorner), b4(m_layerSize), b5(m_layerBlend), b6(m_layerOpacity), b7(m_layerNoFx),
            b8(m_layerPick);
        m_layerPick->setCurrentIndex(m_layerIndex);
        const int source = m_layer->findText(layerSource(m_layerIndex).name);
        m_layer->setCurrentIndex(layerSource(m_layerIndex).name.isEmpty() || source < 0 ? 0 : source);
        m_layerMode->setCurrentIndex(int(s.mode));
        m_layerCorner->setCurrentIndex(s.corner);
        m_layerSize->setValue(int(std::lround(s.size * 100)));
        m_layerBlend->setCurrentIndex(s.blend);
        m_layerOpacity->setValue(int(std::lround(s.opacity * 100)));
        m_layerNoFx->setChecked(s.noEffects);
    }
    updateLayerControls();
}

void LiveView::updateLayerControls()
{
    const LiveSource layer = layerSource(m_layerIndex);
    const bool on = !layer.name.isEmpty();
    const bool pip = m_layerMode->currentIndex() == 0;
    for (QWidget* w : {static_cast<QWidget*>(m_layerMode), static_cast<QWidget*>(m_layerBlend), static_cast<QWidget*>(m_layerOpacity),
                       static_cast<QWidget*>(m_layerNoFx)})
        w->setEnabled(on);
    m_layerCorner->setEnabled(on && pip);
    m_layerSize->setEnabled(on && pip);
    m_layerSizeLabel->setText(QStringLiteral("%1%").arg(m_layerSize->value()));
    m_nextLayerFile->setVisible(layer.kind == LiveSource::Kind::File && on);
    for (int i = 0; i < kLiveLayers; ++i) { // ("Layer 2: Camera")
        const QString name = layerSource(i).name;
        m_layerPick->setItemText(i, name.isEmpty() ? tr("Layer %1").arg(i + 1) : tr("Layer %1: %2").arg(i + 1).arg(name));
    }
}

void LiveView::refreshMidi()
{
    const QString current = MidiHub::instance()->current().isEmpty() ? QSettings().value(QStringLiteral("live/midi")).toString()
                                                                    : MidiHub::instance()->current();
    QSignalBlocker block(m_midi);
    m_midi->clear();
    m_midi->addItem(tr("No MIDI"), QString());
    for (const QString& d : MidiHub::instance()->devices())
        m_midi->addItem(d, d);
    const int i = m_midi->findData(current);
    m_midi->setCurrentIndex(std::max(0, i));
    if (i > 0 && MidiHub::instance()->current() != current) {
        QString error;
        if (!MidiHub::instance()->open(current, &error))
            m_status->setText(tr("MIDI: %1").arg(error));
    }
}

void LiveView::saveMidiMap() const
{
    QStringList list;
    for (auto it = m_midiMap.begin(); it != m_midiMap.end(); ++it)
        list << it.key() + QLatin1Char('=') + it.value();
    QSettings().setValue(QStringLiteral("live/midiMap"), list);
}

void LiveView::learnMidi(const QString& action, const QString& label)
{
    const bool osc = m_osc != nullptr;
    if (MidiHub::instance()->current().isEmpty() && !osc) {
        m_status->setText(tr("Choose your MIDI controller in the MIDI list (or switch OSC on) first, then Learn MIDI again"));
        return;
    }
    m_midiLearn = action;
    m_midiLearnLabel = label;
    const QString from = MidiHub::instance()->current().isEmpty() ? tr("your OSC app")
                       : osc ? tr("%1 or your OSC app").arg(MidiHub::instance()->current())
                             : MidiHub::instance()->current();
    m_status->setText(action == QLatin1String("padmix") ? tr("Move a knob or fader on %1 for %2...").arg(from, label)
                                                        : tr("Press a pad or key (or move a button) on %1 for %2...").arg(from, label));
}

void LiveView::midiMessage(const MidiMessage& m)
{
    if (m.status >= 0xF0) { // MIDI clock: beats (and where the bar is) for Auto: MIDI clock
        if (const auto beat = m_midiClock.feed(m))
            m_pads->clockBeat(beat->bpm, beat->barBeat);
        return;
    }
    if (m.isController())
        m_engine->setMidiController(m.channel(), m.data1, m.data2); // modulation reads every controller
    const bool note = m.isNoteOn() || m.isNoteOff();
    if (!note && !m.isController())
        return;
    const QString key = QStringLiteral("%1:%2:%3").arg(note ? QStringLiteral("n") : QStringLiteral("c")).arg(m.channel()).arg(m.data1);
    if (!m_midiLearn.isEmpty() && (m.isNoteOn() || (m.isController() && m.data2 > 0))) {
        for (auto it = m_midiMap.begin(); it != m_midiMap.end();) // one control per action
            it = it.value() == m_midiLearn ? m_midiMap.erase(it) : std::next(it);
        m_midiMap[key] = m_midiLearn;
        saveMidiMap();
        const QString pad = isGamePad(MidiHub::instance()->current()) ? gamePadControlName(m) : QString();
        m_status->setText(tr("%1 now plays %2").arg(!pad.isEmpty() ? tr("The %1").arg(pad)
                                                    : note         ? tr("MIDI note %1 (channel %2)").arg(m.data1).arg(m.channel() + 1)
                                                                   : tr("MIDI controller %1 (channel %2)").arg(m.data1).arg(m.channel() + 1),
                                                    m_midiLearnLabel));
        m_midiLearn.clear();
        if (m.isController())
            m_ccDown.insert(key);
        return;
    }
    const auto it = m_midiMap.find(key);
    if (it == m_midiMap.end())
        return;
    if (it.value() == QLatin1String("padmix")) { // a fader: the whole range
        if (m.isController())
            m_pads->setPadsMix(m.data2 / 127.0);
        return;
    }
    if (note) {
        runAction(it.value(), m.isNoteOn(), m_velocity->isChecked() && m.isNoteOn() ? m.data2 / 127.0 : 1.0);
    } else { // a button sending a controller: down at 64 and over
        const bool down = m.data2 >= 64;
        if (down == m_ccDown.contains(key))
            return;
        if (down)
            m_ccDown.insert(key);
        else
            m_ccDown.remove(key);
        runAction(it.value(), down);
    }
}

void LiveView::runAction(const QString& action, bool down, double velocity)
{
    if (action.startsWith(QLatin1String("pad:"))) {
        const int pad = action.mid(4).toInt();
        if (pad < 0 || pad >= kLivePads)
            return;
        if (down)
            m_engine->setPadVelocity(pad, velocity);
        if (m_pads->isMomentary(pad))
            holdPad(pad, down);
        else if (down)
            m_pads->pressPad(pad); // at once, or on the next beat / bar (Launch)
        return;
    }
    if (action.startsWith(QLatin1String("scene:"))) {
        if (down)
            recallScene(action.mid(6).toInt());
        return;
    }
    if (action.startsWith(QLatin1String("input:"))) { // input:N = cut to input N (0 = the Source)
        if (down)
            cutTo(action.mid(6).toInt(), false);
        return;
    }
    if (action == QLatin1String("cut") || action == QLatin1String("auto")) {
        if (down)
            cutTo(m_preview, action == QLatin1String("auto"));
        return;
    }
    if (action == QLatin1String("take")) {
        if (down)
            takeCue();
        return;
    }
    if (action.startsWith(QLatin1String("cue:"))) { // cue:N = pad N (0-based)
        if (down)
            cuePad(action.mid(4).toInt());
        return;
    }
    if (action.startsWith(QLatin1String("song:"))) {
        if (!down)
            return;
        const QString which = action.mid(5);
        if (which == QLatin1String("next"))
            stepSong(1);
        else if (which == QLatin1String("prev"))
            stepSong(-1);
        else
            recallSong(which.toInt());
        return;
    }
    if (!down)
        return;
    if (action == QLatin1String("tap"))
        m_pads->tap();
    else if (action == QLatin1String("setone"))
        m_pads->setDownbeat();
    else if (action == QLatin1String("findtempo"))
        m_pads->findTempoAgain();
    else if (action == QLatin1String("lockbpm"))
        m_pads->lockBpmButton()->toggle();
    else if (action == QLatin1String("rec"))
        toggleRecording();
    else if (action == QLatin1String("replay"))
        saveReplay();
    else if (action == QLatin1String("screenshot"))
        saveScreenshot();
    else if (action == QLatin1String("blackout"))
        m_pads->blackoutButton()->toggle();
}

void LiveView::updateOsc()
{
    QSettings().setValue(QStringLiteral("live/osc"), m_oscBox->isChecked());
    QSettings().setValue(QStringLiteral("live/oscPort"), m_oscPort->value());
    const int port = m_oscPort->value();
    if (m_osc && (!m_oscBox->isChecked() || m_osc->localPort() != port)) {
        delete m_osc;
        m_osc = nullptr;
    }
    if (!m_oscBox->isChecked()) {
        m_oscStatus->setText(QString());
        return;
    }
    if (m_osc)
        return;
    m_osc = new QUdpSocket(this);
    if (!m_osc->bind(QHostAddress::AnyIPv4, quint16(port))) {
        m_oscStatus->setText(tr("Could not listen on port %1: %2").arg(port).arg(m_osc->errorString()));
        delete m_osc;
        m_osc = nullptr;
        return;
    }
    connect(m_osc, &QUdpSocket::readyRead, this, [this] {
        while (m_osc && m_osc->hasPendingDatagrams()) {
            QByteArray data(int(m_osc->pendingDatagramSize()), '\0');
            m_osc->readDatagram(data.data(), data.size());
            for (const OscMessage& m : parseOsc(data))
                oscMessage(m);
        }
    });
    QStringList addresses;
    for (const QHostAddress& a : QNetworkInterface::allAddresses())
        if (a.protocol() == QAbstractSocket::IPv4Protocol && !a.isLoopback() && !a.isLinkLocal())
            addresses << a.toString();
    m_oscStatus->setText(addresses.isEmpty() ? tr("Listening on port %1").arg(port)
                                             : tr("Send to %1, port %2").arg(addresses.join(tr(" or ")), QString::number(port)));
}

void LiveView::oscMessage(const OscMessage& m)
{
    const double value = m.arg(0, 1.0); // (no argument: a press)
    const QString key = QStringLiteral("o:") + m.address;
    if (!m_midiLearn.isEmpty() && value > 0 && !m.address.startsWith(QLatin1String("/mvp/"))) {
        for (auto it = m_midiMap.begin(); it != m_midiMap.end();) // one control per action
            it = it.value() == m_midiLearn ? m_midiMap.erase(it) : std::next(it);
        m_midiMap[key] = m_midiLearn;
        saveMidiMap();
        m_status->setText(tr("OSC %1 now plays %2").arg(m.address, m_midiLearnLabel));
        m_midiLearn.clear();
        return;
    }
    QString action = m_midiMap.value(key);
    if (action.isEmpty() && m.address.startsWith(QLatin1String("/mvp/"))) {
        // /mvp/pad/3, /mvp/scene/2, /mvp/song/next, /mvp/tap...: pads, scenes and songs count from 1, inputs from 0 (the Source).
        const QStringList parts = m.address.mid(5).split(QLatin1Char('/'), Qt::SkipEmptyParts);
        if (parts.size() == 2) {
            bool ok = false;
            const int n = parts[1].toInt(&ok);
            if (parts[0] == QLatin1String("song") && !ok)
                action = QStringLiteral("song:") + parts[1];
            else if (ok)
                action = parts[0] + QLatin1Char(':') + QString::number(parts[0] == QLatin1String("input") ? n : n - 1);
        } else if (parts.size() == 1) {
            action = parts[0];
        }
    }
    if (action.isEmpty())
        return;
    if (action == QLatin1String("padmix")) {
        m_pads->setPadsMix(value);
        return;
    }
    // Buttons send 1 when pressed and 0 when let go; a pressure-sensitive pad may send less than 1.
    runAction(action, value > 0, m_velocity->isChecked() && value > 0 && value < 1 ? value : 1.0);
}

void LiveView::holdPad(int pad, bool down)
{
    const auto it = std::find(m_held.begin(), m_held.end(), pad);
    if (down && it == m_held.end())
        m_held.push_back(pad);
    else if (!down && it != m_held.end())
        m_held.erase(it);
    else
        return;
    m_engine->setHeldPads(m_held);
    m_pads->setHeldPads(m_held);
}

void LiveView::saveReplay()
{
    if (!m_engine->replayRunning()) {
        m_status->setText(m_engine->isRunning() ? tr("Switch the Replay buffer on first: it keeps the last seconds from then on")
                                                : tr("The replay buffer fills while capturing: press Start (and tick Replay buffer)"));
        return;
    }
    m_engine->saveReplay(newLiveRecordingPath(QStringLiteral("Replay")));
    m_status->setText(tr("Saving the replay..."));
}

void LiveView::saveScreenshot()
{
    // What the big display shows: the output (in the popped-out window when it is out).
    const QImage shown = m_popOut && m_popOut->isVisible() ? m_popOut->image() : m_display->image();
    if (shown.isNull()) {
        m_status->setText(tr("Nothing to capture yet: press Start"));
        return;
    }
    const QString path = newLiveRecordingPath(QStringLiteral("Screenshot"), QStringLiteral("png"));
    if (!shown.convertToFormat(QImage::Format_RGB888).save(path)) {
        m_status->setText(tr("Could not save %1").arg(QDir::toNativeSeparators(path)));
        return;
    }
    m_status->setText(tr("Saved %1 (%2 x %3)").arg(QFileInfo(path).fileName()).arg(shown.width()).arg(shown.height()));
    m_status->setToolTip(QDir::toNativeSeparators(path));
    m_lastScreenshot = path;
}

void LiveView::fitSidebar()
{
    const int bar = m_sidebar->verticalScrollBar()->sizeHint().width();
    m_sidebar->setMinimumWidth(m_sidebar->widget()->minimumSizeHint().width() + bar + 2 * m_sidebar->frameWidth() + 2);
}

bool LiveView::eventFilter(QObject* watched, QEvent* event)
{
    const QEvent::Type type = event->type();
    if (m_sidebar && watched == m_sidebar->widget() && (type == QEvent::LayoutRequest || type == QEvent::Show || type == QEvent::StyleChange))
        fitSidebar();
    if (type == QEvent::MouseButtonDblClick) // a switcher input: on air at once
        for (int k = 0; k <= LiveEngine::kMaxInputs; ++k)
            if (watched == m_inputTiles[size_t(k)]) {
                cutTo(k, false);
                return true;
            }
    if (type != QEvent::KeyPress && type != QEvent::KeyRelease && type != QEvent::ShortcutOverride)
        return QWidget::eventFilter(watched, event);
    auto* k = static_cast<QKeyEvent*>(event);
    // Only on the Live tab (or its output windows), not while typing, not with Ctrl / Alt.
    const QWidget* active = QApplication::activeWindow();
    const bool live = (m_active && active == window()) || (m_popOut && active == m_popOut) || (m_fullScreen && active == m_fullScreen);
    if (!live || (k->modifiers() & (Qt::ControlModifier | Qt::AltModifier | Qt::MetaModifier)))
        return false;
    QWidget* focus = QApplication::focusWidget();
    if (qobject_cast<QLineEdit*>(focus) || qobject_cast<QAbstractSpinBox*>(focus) || qobject_cast<QTextEdit*>(focus)
        || qobject_cast<QPlainTextEdit*>(focus) || (focus && focus->window() != active))
        return false;
    static const int keys[16] = {Qt::Key_1, Qt::Key_2, Qt::Key_3, Qt::Key_4, Qt::Key_Q, Qt::Key_W, Qt::Key_E, Qt::Key_R,
                                 Qt::Key_A, Qt::Key_S, Qt::Key_D, Qt::Key_F, Qt::Key_Z, Qt::Key_X, Qt::Key_C, Qt::Key_V};
    QString action;
    if (const int* hit = std::find(std::begin(keys), std::end(keys), k->key()); hit != std::end(keys))
        action = QStringLiteral("pad:%1").arg(int(hit - std::begin(keys)));
    else if (k->key() >= Qt::Key_F1 && k->key() < Qt::Key_F1 + kLiveScenes && !(k->modifiers() & Qt::ShiftModifier)) // (Shift+F1: help)
        action = QStringLiteral("scene:%1").arg(k->key() - Qt::Key_F1);
    else if (k->key() == Qt::Key_Backspace)
        action = QStringLiteral("blackout");
    else if (k->key() == Qt::Key_PageDown)
        action = QStringLiteral("song:next");
    else if (k->key() == Qt::Key_PageUp)
        action = QStringLiteral("song:prev");
    else if ((k->key() == Qt::Key_Return || k->key() == Qt::Key_Enter) && m_switcher->isVisible())
        action = (k->modifiers() & Qt::ShiftModifier) ? QStringLiteral("auto") : QStringLiteral("cut");
    else
        return false;
    if (type == QEvent::ShortcutOverride) { // the pad, not a menu shortcut (R is Insert Region in the editor)
        event->accept();
        return true;
    }
    if (!k->isAutoRepeat())
        runAction(action, type == QEvent::KeyPress);
    return true;
}

bool LiveView::usesEditor() const
{
    if (!m_engine->isRunning())
        return false;
    const LiveEngine::Options o = options();
    auto editor = [](const LiveSource& s) { return s.kind == LiveSource::Kind::Editor; };
    return editor(o.source) || std::any_of(o.layers.begin(), o.layers.end(), editor) || std::any_of(o.inputs.begin(), o.inputs.end(), editor);
}

LiveEngine::Options LiveView::options() const
{
    LiveEngine::Options o;
    const int i = m_source->currentData().toInt();
    if (i >= 0 && i < int(m_sources.size()))
        o.source = m_sources[size_t(i)];
    const CameraFormat& f = kCameraFormats[std::clamp(m_format->currentIndex(), 0, int(std::size(kCameraFormats)) - 1)];
    o.format.size = QSize(f.w, f.h);
    o.format.fps = f.fps;
    if (m_size->currentData(Qt::UserRole + 1).toBool()) {
        o.canvas = m_size->currentData().toSize();
        o.framing = LiveFraming(m_framing->currentData().toInt());
    } else {
        o.maxSize = m_size->currentData().toSize();
    }
    o.fps = m_fps->currentData().toDouble();
    o.mirror = m_mirror->isChecked();
    o.audio = audioSource();
    o.pad = m_activePad;
    for (int i = 0; i < kLiveLayers; ++i)
        o.layers[size_t(i)] = layerSource(i);
    o.layerSettings = m_layerSets;
    o.inputs = inputSources();
    o.program = m_program;
    return o;
}

void LiveView::startStop()
{
    if (m_engine->isRunning()) {
        m_engine->stop();
        m_status->setText(tr("Stopped"));
    } else {
        const int i = m_source->currentData().toInt();
        if (i < 0 || i >= int(m_sources.size()))
            return;
        m_status->setText(tr("Opening %1...").arg(m_sources[size_t(i)].name));
        m_engine->setPaused(!m_active);
        m_engine->start(options());
        saveSettings();
    }
    updateControls();
}

void LiveView::toggleRecording()
{
    if (m_engine->isRecording()) {
        m_engine->stopRecording();
        return;
    }
    if (!m_engine->isRunning()) {
        startStop();
        if (!m_engine->isRunning())
            return;
    }
    m_engine->startRecording(newLiveRecordingPath(), m_recMode->currentIndex() == 0);
    updateControls();
}

void LiveView::panZoomFrame(QSizeF* canvas, LiveFraming* framing) const
{
    if (m_size->currentData(Qt::UserRole + 1).toBool()) {
        *canvas = QSizeF(m_size->currentData().toSize());
        *framing = LiveFraming(m_framing->currentData().toInt());
    } else { // the output is the source's shape: the zoomed-in part fills it
        *canvas = QSizeF(m_display->image().isNull() ? m_sourceSize : m_display->image().size());
        *framing = LiveFraming::CropFill;
    }
}

void LiveView::setPanZoom(LivePanZoom pz)
{
    pz.zoom = std::clamp(pz.zoom, 1.0, kLiveMaxZoom);
    QSizeF canvas;
    LiveFraming framing;
    panZoomFrame(&canvas, &framing);
    if (m_sourceSize.isValid() && !canvas.isEmpty()) { // as far as the framing allows
        QPointF used;
        sourceRectInCanvas(QSizeF(m_sourceSize), canvas, framing, pz, &used);
        pz.center = QPointF(std::clamp(used.x(), 0.0, 1.0), std::clamp(used.y(), 0.0, 1.0));
    }
    m_panZoom = pz;
    m_engine->setPanZoom(pz);
    const bool moved = !pz.isDefault();
    const bool panned = std::abs(pz.center.x() - 0.5) + std::abs(pz.center.y() - 0.5) > 1e-3;
    const int percent = int(std::lround(pz.zoom * 100));
    m_panZoomLabel->setText(!moved ? tr("100% (drag / scroll the picture)") : panned ? tr("%1%, moved").arg(percent) : tr("%1%").arg(percent));
    static_cast<LiveDisplay*>(m_display)->info = moved ? tr("Zoom %1%  ·  drag to move  ·  right-click to reset").arg(int(std::lround(pz.zoom * 100))) : QString();
    m_display->update();
    QSettings s;
    s.setValue(QStringLiteral("live/panX"), pz.center.x());
    s.setValue(QStringLiteral("live/panY"), pz.center.y());
    s.setValue(QStringLiteral("live/zoom"), pz.zoom);
}

void LiveView::zoomBy(double factor, QPointF at)
{
    QSizeF canvas;
    LiveFraming framing;
    panZoomFrame(&canvas, &framing);
    const QRectF r = sourceRectInCanvas(QSizeF(m_sourceSize), canvas, framing, m_panZoom);
    LivePanZoom pz = m_panZoom;
    pz.zoom = std::clamp(m_panZoom.zoom * factor, 1.0, kLiveMaxZoom);
    if (!r.isEmpty() && !canvas.isEmpty()) {
        // The point under the cursor stays put.
        const QPointF s((at.x() - r.x()) / r.width(), (at.y() - r.y()) / r.height());
        const QSizeF size = r.size() * (pz.zoom / m_panZoom.zoom);
        const QPointF topLeft(at.x() - s.x() * size.width(), at.y() - s.y() * size.height());
        pz.center = QPointF((canvas.width() / 2 - topLeft.x()) / size.width(), (canvas.height() / 2 - topLeft.y()) / size.height());
    }
    setPanZoom(pz);
}

void LiveView::cuePad(int pad)
{
    if (pad < 0 || pad >= kLivePads)
        return;
    if (m_cueKind == LiveEngine::CuePad && m_cueIndex == pad) {
        clearCue();
        return;
    }
    m_cueKind = LiveEngine::CuePad;
    m_cueIndex = pad;
    m_engine->setCue(m_cueKind, pad);
    m_pads->setCuedPad(pad);
    const LivePad& p = m_project->timeline().livePads[size_t(pad)];
    m_cueLabel->setText(tr("Pad %1%2 over the Live FX%3")
                            .arg(pad + 1)
                            .arg(p.name.isEmpty() ? QString() : QStringLiteral(": ") + p.name)
                            .arg(m_engine->isRunning() ? QString() : tr(" (shows while capturing)")));
    m_cueView->setImage(QImage());
    m_cuePanel->setVisible(true);
    refreshScenes();
}

void LiveView::cueScene(int scene)
{
    if (scene < 0 || scene >= kLiveScenes)
        return;
    const LiveScene& s = m_project->timeline().liveScenes[size_t(scene)];
    if (!s.used) {
        m_status->setText(tr("Scene %1 is empty: nothing to cue").arg(scene + 1));
        return;
    }
    if (m_cueKind == LiveEngine::CueScene && m_cueIndex == scene) {
        clearCue();
        return;
    }
    m_cueKind = LiveEngine::CueScene;
    m_cueIndex = scene;
    m_engine->setCue(m_cueKind, scene);
    m_pads->setCuedPad(-1);
    m_cueLabel->setText(tr("Scene %1%2 (its Live FX, pad and overlays; the layer as it is now)%3")
                            .arg(scene + 1)
                            .arg(s.name.isEmpty() ? QString() : QStringLiteral(": ") + s.name)
                            .arg(m_engine->isRunning() ? QString() : tr(" - shows while capturing")));
    m_cueView->setImage(QImage());
    m_cuePanel->setVisible(true);
    refreshScenes();
}

void LiveView::takeCue()
{
    const int kind = m_cueKind, index = m_cueIndex;
    clearCue();
    if (kind == LiveEngine::CuePad) {
        if (index != m_activePad) // pressing the live pad would switch it off
            m_pads->pressPad(index);
    } else if (kind == LiveEngine::CueScene) {
        recallScene(index);
    }
}

void LiveView::clearCue()
{
    m_cueKind = LiveEngine::CueNone;
    m_cueIndex = -1;
    m_engine->setCue(LiveEngine::CueNone, -1);
    m_pads->setCuedPad(-1);
    m_cuePanel->setVisible(false);
    refreshScenes();
}

void LiveView::toggleStream()
{
    if (m_engine->isStreaming()) {
        m_engine->stopStream();
        return;
    }
    QString url = StreamDialog::streamUrl();
    if (url.isEmpty()) {
        StreamDialog dlg(this);
        if (dlg.exec() != QDialog::Accepted)
            return;
        url = StreamDialog::streamUrl();
        if (url.isEmpty()) {
            m_streamStatus->setText(tr("stream: set the server and the stream key first"));
            return;
        }
    }
    if (!m_engine->isRunning()) {
        startStop();
        if (!m_engine->isRunning())
            return;
    }
    m_engine->startStream(url, StreamDialog::bitrateKbps());
    m_goLive->setText(tr("Connecting..."));
}

void LiveView::updateControls()
{
    const int i = m_source->currentData().toInt();
    const bool valid = i >= 0 && i < int(m_sources.size());
    m_formatRow->setVisible(valid && m_sources[size_t(i)].kind == LiveSource::Kind::Device);
    m_start->setEnabled(valid || m_engine->isRunning());
    m_start->setText(m_engine->isRunning() ? tr("■  Stop") : tr("▶  Start"));
    const bool rec = m_engine->isRecording();
    m_rec->setText(rec ? tr("■  Stop Rec") : tr("●  Rec"));
    theme::style(m_rec, rec ? QStringLiteral("QPushButton { background: @bad; color: @window; font-weight: bold; }")
                             : QStringLiteral("QPushButton { color: @bad; }"));
    m_rec->setEnabled(valid || m_engine->isRunning());
    m_audioMeter->setVisible(!audioSource().isNull());
    m_nextFile->setVisible(valid && m_sources[size_t(i)].kind == LiveSource::Kind::File);
    m_saveReplay->setEnabled(m_replay->isChecked());
}

void LiveView::setActive(bool active)
{
    m_active = active;
    updatePaused();
    if (active && m_sources.empty())
        refreshSources();
}

void LiveView::updatePaused()
{
    // Keep rendering while the picture is visible somewhere: this tab, the
    // full-screen output or the pop-out window (OBS may be capturing it).
    const bool shown = m_active || (m_fullScreen && m_fullScreen->isVisible()) || (m_popOut && m_popOut->isVisible());
    m_engine->setPaused(!shown);
}

void LiveView::triggerPad(int pad)
{
    const int before = m_activePad;
    m_activePad = m_activePad == pad ? -1 : pad; // pressing the active pad switches it off
    m_pads->setActivePad(m_activePad);
    m_engine->setPad(m_activePad, m_pads->transition(), m_pads->transitionSeconds());
    if (!m_pads->autoStepping())
        followPad(before);
}

void LiveView::followPad(int before)
{
    if (!m_pads->followsActivePad() || m_activePad == before)
        return;
    if (m_activePad >= 0)
        editChain(m_activePad + 1); // the pad switched on, ready to tweak
    else if (m_pads->editing() == before + 1)
        editChain(0); // its pad went off: back to the Live FX
}

void LiveView::editChain(int target)
{
    m_pads->setEditing(target);
    m_devices->setTarget({FxTarget::Kind::Live, target});
    updateDeviceLock();
}

bool LiveView::editingLocked() const
{
    return m_pads->editing() > 0 && m_pads->isLocked(m_pads->editing() - 1);
}

void LiveView::updateDeviceLock()
{
    // A locked pad's chain is shown but cannot be changed.
    const bool locked = editingLocked();
    m_devices->setEnabled(!locked);
    m_devices->setToolTip(locked ? tr("This pad is locked: right-click it > Unlock Pad to change its effects") : QString());
}

void LiveView::togglePopOut()
{
    auto* display = static_cast<LiveDisplay*>(m_display);
    if (m_popOut && m_popOut->isVisible()) {
        m_popOut->close(); // onClosed puts the picture back
        return;
    }
    if (!m_popOut) {
        auto* w = new PopOutWindow;
        w->onClosed = [this, display] {
            m_popOutButton->setChecked(false);
            display->hint = m_engine->isRunning() ? QString()
                                                  : tr("Choose a camera, capture card, screen or window (Setup > Input) and press Start.\n"
                                                       "Add effects from the Video FX list: they apply to the live picture.");
            m_display->setImage(m_popOut->image());
            updatePaused();
        };
        m_popOut = w;
        const QByteArray geometry = QSettings().value(QStringLiteral("live/popOutGeometry")).toByteArray();
        if (geometry.isEmpty() || !m_popOut->restoreGeometry(geometry))
            m_popOut->resize(1280, 720);
    }
    m_popOut->setImage(m_display->image());
    display->hint = tr("The picture is in its own window, \"Mutant Video Producer Live Output\".\nPress Pop Out again (or close that window) to bring it back.");
    m_display->setImage(QImage());
    m_popOutButton->setChecked(true);
    m_popOut->show();
    m_popOut->raise();
    m_popOut->activateWindow();
    updatePaused();
}

void LiveView::showFullScreenOutput()
{
    if (!m_fullScreen) {
        m_fullScreen = new OutputWindow;
        m_fullScreen->setWindowTitle(tr("Mutant Video Producer Live Output"));
        m_fullScreen->setAttribute(Qt::WA_DeleteOnClose, false);
    }
    // Prefer a screen other than the editor's (a projector / second monitor).
    QScreen* target = screen();
    for (QScreen* s : QGuiApplication::screens())
        if (s != screen()) {
            target = s;
            break;
        }
    m_fullScreen->setScreen(target);
    m_fullScreen->setGeometry(target->geometry());
    m_fullScreen->showFullScreen();
    m_fullScreen->raise();
}
