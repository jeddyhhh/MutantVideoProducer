#include "ui/DeliverView.h"

#include "core/Captions.h"
#include "core/Compositor.h"
#include "core/Exporter.h"
#include "core/MediaInfo.h"
#include "core/Profiles.h"
#include "core/Project.h"
#include "core/YouTubeUpload.h"
#include "ui/AudioFinishingWindow.h"
#include "ui/RenderQueue.h"
#include "ui/Theme.h"

#include <QApplication>
#include <QCheckBox>
#include <QClipboard>
#include <QComboBox>
#include <QCryptographicHash>
#include <QDateTime>
#include <QDesktopServices>
#include <QDir>
#include <QElapsedTimer>
#include <QFileDialog>
#include <QFileInfo>
#include <QFrame>
#include <QGridLayout>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLabel>
#include <QLineEdit>
#include <QMenu>
#include <QMouseEvent>
#include <QPainter>
#include <QPainterPath>
#include <QPlainTextEdit>
#include <QProgressBar>
#include <QPushButton>
#include <QRegularExpression>
#include <QSaveFile>
#include <QScrollArea>
#include <QSettings>
#include <QShowEvent>
#include <QStandardItemModel>
#include <QSplitter>
#include <QStorageInfo>
#include <QTimer>
#include <QToolButton>
#include <QToolTip>
#include <QTreeWidget>
#include <QUrl>
#include <QVBoxLayout>

#include <algorithm>
#include <cmath>

namespace {
constexpr int kHistoryLimit = 300;
constexpr int kDescriptionLimit = 5000; // YouTube's description and tag limits, in characters
constexpr int kTagLimit = 500;

const ExportPreset* presetNamed(const QString& name)
{
    for (const ExportPreset& p : exportPresets())
        if (p.name == name)
            return &p;
    return nullptr;
}

QString sizeText(double bytes)
{
    if (bytes >= 1024.0 * 1024 * 1024)
        return QStringLiteral("%1 GB").arg(bytes / (1024.0 * 1024 * 1024), 0, 'f', 1);
    if (bytes >= 1024.0 * 1024)
        return QStringLiteral("%1 MB").arg(bytes / (1024.0 * 1024), 0, 'f', bytes >= 100.0 * 1024 * 1024 ? 0 : 1);
    return QStringLiteral("%1 KB").arg(std::max(1.0, std::round(bytes / 1024)));
}

// "libx264" -> "H.264", for the output details.
QString codecName(const QString& encoder)
{
    static const std::vector<std::pair<const char*, const char*>> names = {
        {"264", "H.264"}, {"265", "HEVC"}, {"hevc", "HEVC"}, {"vp9", "VP9"}, {"vp8", "VP8"}, {"av1", "AV1"}, {"vvc", "VVC"}, {"266", "VVC"},
        {"prores", "ProRes"}, {"dnxhd", "DNxHR"}, {"cfhd", "CineForm"}, {"hap", "HAP"}, {"ffv1", "FFV1"}, {"utvideo", "UT Video"},
        {"mpeg2", "MPEG-2"}, {"mpeg1", "MPEG-1"}, {"mpeg4", "MPEG-4"}, {"h263", "H.263"}, {"flv", "Sorenson"}, {"wmv", "WMV"},
        {"theora", "Theora"}, {"cinepak", "Cinepak"}, {"mjpeg", "MJPEG"}, {"gif", "GIF"}, {"png", "PNG"}, {"qtrle", "Animation"},
        {"v210", "10-bit 4:2:2"}, {"roq", "RoQ"}, {"aac", "AAC"}, {"mp3", "MP3"}, {"opus", "Opus"}, {"vorbis", "Vorbis"},
        {"flac", "FLAC"}, {"alac", "ALAC"}, {"ac3", "AC-3"}, {"mp2", "MP2"}, {"amr", "AMR"}, {"wma", "WMA"}, {"pcm", "PCM"},
        {"adpcm", "ADPCM"}};
    for (const auto& [key, name] : names)
        if (encoder.contains(QLatin1String(key), Qt::CaseInsensitive))
            return QString::fromLatin1(name);
    return encoder;
}

// A coloured round badge for a check: a tick, a warning mark or a dot.
QPixmap badge(const QColor& colour, const QString& glyph, int size = 18)
{
    const qreal dpr = qApp->devicePixelRatio();
    QPixmap pm(int(size * dpr), int(size * dpr));
    pm.setDevicePixelRatio(dpr);
    pm.fill(Qt::transparent);
    QPainter p(&pm);
    p.setRenderHint(QPainter::Antialiasing);
    p.setPen(Qt::NoPen);
    p.setBrush(colour);
    p.drawEllipse(QRectF(1, 1, size - 2, size - 2));
    QFont f = p.font();
    f.setBold(true);
    f.setPixelSize(int(size * 0.62));
    p.setFont(f);
    p.setPen(colour.lightness() > 150 ? QColor(20, 20, 20) : QColor(255, 255, 255));
    p.drawText(QRectF(0, 0, size, size), Qt::AlignCenter, glyph);
    return pm;
}

// A panel with a heading: the tab is built from these.
QFrame* card(const QString& title, QVBoxLayout** body, QWidget* extra = nullptr)
{
    auto* frame = new QFrame;
    frame->setObjectName(QStringLiteral("deliverCard"));
    theme::style(frame, QStringLiteral("QFrame#deliverCard { background: @panel; border: 1px solid @line; border-radius: 8px; }"));
    auto* layout = new QVBoxLayout(frame);
    layout->setContentsMargins(14, 10, 14, 12);
    layout->setSpacing(8);
    auto* head = new QHBoxLayout;
    auto* label = new QLabel(title);
    theme::style(label, QStringLiteral("font-weight: bold; font-size: 11pt; color: @bright; background: transparent; border: none;"));
    head->addWidget(label);
    head->addStretch();
    if (extra)
        head->addWidget(extra);
    layout->addLayout(head);
    *body = layout;
    return frame;
}

QLabel* dimLabel(const QString& text = {})
{
    auto* label = new QLabel(text);
    theme::style(label, QStringLiteral("color: @dim; background: transparent; border: none;"));
    return label;
}

struct Segment {
    TimeUs start, end;
    QString name;
};

// The edit's chapters: the story chapters, or else the stretches between markers.
std::vector<Segment> segments(const Timeline& tl)
{
    std::vector<Segment> out;
    for (const auto& v : tl.chapters) {
        const QJsonObject c = v.toObject();
        const TimeUs start = c[QStringLiteral("start")].toString().toLongLong();
        const TimeUs end = start + c[QStringLiteral("narration")].toString().toLongLong() + c[QStringLiteral("listening")].toString().toLongLong();
        if (end > start)
            out.push_back({start, end, c[QStringLiteral("name")].toString()});
    }
    if (!out.empty() || tl.markers.empty())
        return out;
    const TimeUs duration = tl.duration();
    for (size_t i = 0; i < tl.markers.size(); ++i) {
        const Marker& m = tl.markers[i];
        const TimeUs end = m.isRegion() ? m.time + m.length : (i + 1 < tl.markers.size() ? tl.markers[i + 1].time : duration);
        if (end > m.time)
            out.push_back({m.time, end, m.name.isEmpty() ? QString::number(i + 1) : m.name});
    }
    return out;
}

// Stretches of a second or more with no picture on any (unmuted) picture track.
std::vector<std::pair<TimeUs, TimeUs>> pictureGaps(const Timeline& tl)
{
    std::vector<std::pair<TimeUs, TimeUs>> spans, gaps;
    for (const Track& t : tl.tracks)
        if (isVisualTrack(t.type) && !t.muted)
            for (const Clip& c : t.clips)
                if (!c.muted)
                    spans.emplace_back(c.start, c.end());
    std::sort(spans.begin(), spans.end());
    TimeUs covered = 0;
    for (const auto& [a, b] : spans) {
        if (a - covered >= kUsPerSecond)
            gaps.emplace_back(covered, a);
        covered = std::max(covered, b);
    }
    if (tl.duration() - covered >= kUsPerSecond)
        gaps.emplace_back(covered, tl.duration());
    return gaps;
}
} // namespace

// The edit at a glance: frames along it, the chapters under them, gaps in red, markers. Click a frame
// to make it the thumbnail; double-click to go there in the Editor.
class DeliverStrip : public QWidget {
public:
    std::function<void(TimeUs)> picked, goTo;
    TimeUs duration = 0, selected = -1;
    std::vector<Segment> chapters;
    std::vector<std::pair<TimeUs, TimeUs>> gaps;
    std::vector<TimeUs> markers;
    std::vector<std::pair<TimeUs, QImage>> frames; // time, picture (null until rendered)
    int hoverX = -1;

    DeliverStrip()
    {
        setObjectName(QStringLiteral("deliverStrip"));
        setMinimumHeight(118);
        setMouseTracking(true);
        setCursor(Qt::PointingHandCursor);
        setToolTip(tr("Click a frame to make it the thumbnail; double-click to go there in the Editor"));
    }
    QRectF area() const { return QRectF(rect()).adjusted(1, 1, -1, -1); }
    double xAt(TimeUs t) const { return area().left() + area().width() * (duration > 0 ? double(t) / duration : 0); }
    TimeUs timeAt(double x) const { return duration > 0 ? TimeUs(std::clamp((x - area().left()) / area().width(), 0.0, 1.0) * duration) : 0; }

protected:
    void paintEvent(QPaintEvent*) override
    {
        QPainter p(this);
        p.setRenderHint(QPainter::Antialiasing);
        const QRectF r = area();
        QPainterPath clip;
        clip.addRoundedRect(r, 6, 6);
        p.fillPath(clip, theme::c().deep);
        p.setClipPath(clip);
        const double barH = 22, frameTop = r.top(), frameH = r.height() - barH;
        if (duration <= 0) {
            p.setPen(theme::c().dim);
            p.drawText(r, Qt::AlignCenter, tr("The timeline is empty"));
            return;
        }
        // Frames, each in its slot (cropped to fill).
        const int n = int(frames.size());
        for (int i = 0; i < n; ++i) {
            const QRectF slot(r.left() + r.width() * i / n, frameTop, r.width() / n, frameH);
            const QImage& img = frames[size_t(i)].second;
            if (img.isNull()) {
                p.fillRect(slot.adjusted(1, 1, -1, -1), theme::c().raised);
                continue;
            }
            QSizeF s = img.size();
            s.scale(slot.size(), Qt::KeepAspectRatioByExpanding);
            const QRectF src((s.width() - slot.width()) / 2 * img.width() / s.width(), (s.height() - slot.height()) / 2 * img.height() / s.height(),
                             slot.width() * img.width() / s.width(), slot.height() * img.height() / s.height());
            p.drawImage(slot.adjusted(0.5, 0, -0.5, 0), img, src);
        }
        // Chapters along the bottom.
        const QRectF bar(r.left(), r.bottom() - barH, r.width(), barH);
        p.fillRect(bar, theme::c().panel);
        QFont small = font();
        small.setPointSizeF(small.pointSizeF() * 0.9);
        p.setFont(small);
        for (size_t i = 0; i < chapters.size(); ++i) {
            const QRectF seg(QPointF(xAt(chapters[i].start), bar.top() + 3), QPointF(xAt(chapters[i].end) - 1, bar.bottom() - 3));
            QColor c = theme::c().accent;
            if (i % 2)
                c = c.darker(140);
            p.setPen(Qt::NoPen);
            p.setBrush(c);
            p.drawRoundedRect(seg, 3, 3);
            p.setPen(theme::c().onAccent);
            p.drawText(seg.adjusted(5, 0, -3, 0), Qt::AlignVCenter | Qt::AlignLeft, p.fontMetrics().elidedText(chapters[i].name, Qt::ElideRight, int(seg.width() - 8)));
        }
        if (chapters.empty()) {
            p.setPen(theme::c().dim);
            p.drawText(bar.adjusted(8, 0, 0, 0), Qt::AlignVCenter | Qt::AlignLeft, tr("No chapters or markers"));
        }
        for (TimeUs m : markers) {
            p.setPen(QPen(theme::c().cue, 2));
            p.drawLine(QPointF(xAt(m), bar.top()), QPointF(xAt(m), bar.top() + 5));
        }
        // Gaps with no picture.
        for (const auto& [a, b] : gaps) {
            const QRectF g(QPointF(xAt(a), r.top()), QPointF(std::max(xAt(b), xAt(a) + 3), r.bottom()));
            QColor wash = theme::c().bad;
            wash.setAlpha(90);
            p.fillRect(g, wash);
            p.fillRect(QRectF(g.left(), r.top(), g.width(), 3), theme::c().bad);
        }
        // The chosen thumbnail frame.
        if (selected >= 0) {
            const double x = xAt(selected);
            p.setPen(QPen(theme::c().warn, 2));
            p.drawLine(QPointF(x, r.top()), QPointF(x, bar.top()));
            p.setBrush(theme::c().warn);
            p.setPen(Qt::NoPen);
            p.drawEllipse(QPointF(x, r.top() + 6), 4, 4);
        }
        // Hover: a line and the time.
        if (hoverX >= 0) {
            p.setPen(QPen(theme::c().bright, 1, Qt::DashLine));
            p.drawLine(QPointF(hoverX, r.top()), QPointF(hoverX, bar.top()));
            const QString text = formatDuration(timeAt(hoverX));
            QRectF label(0, r.top() + 4, p.fontMetrics().horizontalAdvance(text) + 10, 18);
            label.moveLeft(std::clamp(hoverX + 6.0, r.left() + 2, r.right() - label.width() - 2));
            p.setBrush(QColor(0, 0, 0, 170));
            p.setPen(Qt::NoPen);
            p.drawRoundedRect(label, 3, 3);
            p.setPen(Qt::white);
            p.drawText(label, Qt::AlignCenter, text);
        }
    }
    void mouseMoveEvent(QMouseEvent* e) override
    {
        hoverX = int(e->position().x());
        update();
    }
    void leaveEvent(QEvent*) override
    {
        hoverX = -1;
        update();
    }
    void mousePressEvent(QMouseEvent* e) override
    {
        if (e->button() != Qt::LeftButton || duration <= 0)
            return;
        selected = timeAt(e->position().x());
        update();
        if (picked)
            picked(selected);
    }
    void mouseDoubleClickEvent(QMouseEvent* e) override
    {
        if (goTo && duration > 0)
            goTo(timeAt(e->position().x()));
    }
};

// Integrated loudness on a -36..0 LUFS scale, with the target and its true-peak ceiling.
class LoudnessMeter : public QWidget {
public:
    std::optional<double> lufs, peak;
    double target = -16, ceiling = -1.5;
    LoudnessMeter()
    {
        setObjectName(QStringLiteral("deliverLoudness"));
        setMinimumHeight(48);
    }

protected:
    void paintEvent(QPaintEvent*) override
    {
        QPainter p(this);
        p.setRenderHint(QPainter::Antialiasing);
        const QRectF track(16, 4, width() - 32, 10);
        auto x = [&](double db) { return track.left() + track.width() * std::clamp((db + 36.0) / 36.0, 0.0, 1.0); };
        p.setPen(Qt::NoPen);
        p.setBrush(theme::c().deep);
        p.drawRoundedRect(track, 5, 5);
        if (lufs) {
            const bool close = std::abs(*lufs - target) <= 1.0, hot = peak && *peak > ceiling;
            p.setBrush(hot ? theme::c().bad : close ? theme::c().good : theme::c().warn);
            p.drawRoundedRect(QRectF(track.left(), track.top(), x(*lufs) - track.left(), track.height()), 5, 5);
        }
        p.setPen(QPen(theme::c().bright, 2));
        p.drawLine(QPointF(x(target), track.top() - 3), QPointF(x(target), track.bottom() + 3));
        QFont small = font();
        small.setPointSizeF(small.pointSizeF() * 0.85);
        p.setFont(small);
        p.setPen(theme::c().dim);
        for (int db : {-36, -24, -18, -12, -6, 0})
            p.drawText(QRectF(x(db) - 20, track.bottom() + 3, 40, 14), Qt::AlignCenter, QString::number(db));
        p.setPen(theme::c().text);
        const QString text = lufs ? tr("%1 LUFS · peak %2 dBTP").arg(*lufs, 0, 'f', 1).arg(peak.value_or(-120), 0, 'f', 1) : tr("not measured");
        p.drawText(QRectF(0, track.bottom() + 17, width() - 16, 14), Qt::AlignRight | Qt::AlignVCenter, text);
    }
};

struct DeliverView::OutputRow {
    QString templateName;
    QString path;
    QWidget* widget = nullptr;
    QCheckBox* tick = nullptr;
    QLabel *details = nullptr, *file = nullptr, *state = nullptr;
    QProgressBar* bar = nullptr;
};

DeliverView::DeliverView(Project* project, Hooks hooks, QWidget* parent) : QWidget(parent), m_project(project), m_hooks(std::move(hooks))
{
    setObjectName(QStringLiteral("deliverView"));
    m_renderClock = new QElapsedTimer;
    // In a scroll area: the tab's minimum size is the window's (a tab widget takes its largest page's),
    // so on a short screen it scrolls instead of making the window taller than the screen.
    auto* page = new QVBoxLayout(this);
    page->setContentsMargins(0, 0, 0, 0);
    auto* pageScroll = new QScrollArea;
    pageScroll->setObjectName(QStringLiteral("deliverScroll"));
    pageScroll->setWidgetResizable(true);
    pageScroll->setFrameShape(QFrame::NoFrame);
    auto* content = new QWidget;
    pageScroll->setWidget(content);
    page->addWidget(pageScroll);
    auto* outer = new QVBoxLayout(content);
    outer->setContentsMargins(10, 10, 10, 10);
    outer->setSpacing(10);

    // ---- Header: the project, how ready it is, Render ----
    auto* header = new QFrame;
    header->setObjectName(QStringLiteral("deliverHeader"));
    theme::style(header, QStringLiteral("QFrame#deliverHeader { background: @panel; border: 1px solid @line; border-radius: 8px; }"));
    auto* headerRow = new QHBoxLayout(header);
    headerRow->setContentsMargins(16, 10, 12, 10);
    auto* names = new QVBoxLayout;
    m_projectName = new QLabel;
    theme::style(m_projectName, QStringLiteral("font-size: 15pt; font-weight: bold; color: @bright; background: transparent; border: none;"));
    m_projectInfo = dimLabel();
    names->addWidget(m_projectName);
    names->addWidget(m_projectInfo);
    headerRow->addLayout(names);
    headerRow->addStretch();
    auto* status = new QVBoxLayout;
    m_readiness = new QLabel;
    m_readiness->setObjectName(QStringLiteral("deliverReadiness"));
    m_readiness->setAlignment(Qt::AlignCenter);
    m_renderStatus = dimLabel();
    m_renderStatus->setObjectName(QStringLiteral("deliverRenderStatus"));
    m_renderStatus->setAlignment(Qt::AlignRight);
    m_overall = new QProgressBar;
    m_overall->setObjectName(QStringLiteral("deliverOverall"));
    m_overall->setTextVisible(false);
    m_overall->setFixedHeight(6);
    m_overall->setMinimumWidth(220);
    m_overall->setVisible(false);
    status->addWidget(m_readiness, 0, Qt::AlignRight);
    status->addWidget(m_renderStatus);
    status->addWidget(m_overall);
    headerRow->addLayout(status);
    m_openWhenDone = new QPushButton(tr("Open Folder"));
    m_openWhenDone->setVisible(false);
    m_render = new QPushButton(tr("Render"));
    m_render->setObjectName(QStringLiteral("deliverRender"));
    m_render->setMinimumHeight(38);
    m_render->setMinimumWidth(130);
    theme::style(m_render, QStringLiteral("QPushButton { background: @accent; color: @onAccent; font-weight: bold; font-size: 11pt; border: none; "
                                          "border-radius: 6px; padding: 6px 18px; } QPushButton:hover { background: @checked; }"));
    headerRow->addSpacing(12);
    headerRow->addWidget(m_openWhenDone);
    headerRow->addWidget(m_render);
    outer->addWidget(header);
    connect(m_render, &QPushButton::clicked, this, [this] {
        QString error;
        if (renderOutputs(&error) == 0)
            m_renderStatus->setText(error.isEmpty() ? tr("Tick an output to render.") : error);
    });
    connect(m_openWhenDone, &QPushButton::clicked, this,
            [this] { QDesktopServices::openUrl(QUrl::fromLocalFile(QDir::fromNativeSeparators(m_folder->text()))); });

    // ---- The strip ----
    m_strip = new DeliverStrip;
    m_strip->picked = [this](TimeUs t) { pickThumbnail(t); };
    m_strip->goTo = [this](TimeUs t) {
        if (m_hooks.seek)
            m_hooks.seek(t);
    };
    outer->addWidget(m_strip);

    auto* split = new QSplitter(Qt::Horizontal);
    split->setChildrenCollapsible(false);
    auto* leftPane = new QWidget;
    auto* left = new QVBoxLayout(leftPane);
    left->setContentsMargins(0, 0, 0, 0);
    left->setSpacing(10);
    auto* rightPane = new QWidget;
    auto* right = new QVBoxLayout(rightPane);
    right->setContentsMargins(0, 0, 0, 0);
    right->setSpacing(10);
    split->addWidget(leftPane);
    split->addWidget(rightPane);
    outer->addWidget(split, 1);

    // ---- Checks ----
    auto* again = new QToolButton;
    again->setText(tr("Check Again"));
    again->setAutoRaise(true);
    QVBoxLayout* checks = nullptr;
    left->addWidget(card(tr("Checks"), &checks, again));
    m_checkList = new QVBoxLayout;
    m_checkList->setSpacing(2);
    checks->addLayout(m_checkList);
    auto* loudRow = new QHBoxLayout;
    auto* loudLabel = new QLabel(tr("Loudness"));
    theme::style(loudLabel, QStringLiteral("background: transparent; border: none;"));
    m_target = new QComboBox;
    m_target->setObjectName(QStringLiteral("deliverTarget"));
    for (const auto& t : finishing::targets())
        m_target->addItem(tr("%1 (%2 LUFS)").arg(t.name).arg(t.lufs, 0, 'f', 0));
    m_meter = new LoudnessMeter;
    auto* measure = new QPushButton(tr("Measure"));
    measure->setObjectName(QStringLiteral("deliverMeasure"));
    measure->setToolTip(tr("Plays through the final mix (track and master effects included) and measures it"));
    loudRow->addWidget(loudLabel);
    loudRow->addWidget(m_target);
    loudRow->addWidget(measure);
    checks->addLayout(loudRow);
    checks->addWidget(m_meter);
    connect(again, &QToolButton::clicked, this, [this] { refresh(); });
    connect(measure, &QPushButton::clicked, this, [this] {
        const auto snap = m_project->snapshot();
        if (snap->timeline.duration() <= 0)
            return;
        m_loudness = analyzeAudioMix(this, *snap);
        runChecks();
    });
    connect(m_target, &QComboBox::currentIndexChanged, this, [this](int i) {
        setChoice(QStringLiteral("target"), i);
        runChecks();
    });

    // ---- Outputs ----
    auto* packs = new QToolButton;
    packs->setText(tr("Add a Pack"));
    packs->setPopupMode(QToolButton::InstantPopup);
    packs->setAutoRaise(true);
    auto* packMenu = new QMenu(packs);
    for (const Pack& pk : DeliverView::packs())
        packMenu->addAction(pk.label, this, [this, id = pk.id] { addPack(id); });
    packs->setMenu(packMenu);
    QVBoxLayout* outputs = nullptr;
    left->addWidget(card(tr("Outputs"), &outputs, packs), 1);
    auto* scroll = new QScrollArea;
    scroll->setWidgetResizable(true);
    scroll->setFrameShape(QFrame::NoFrame);
    auto* listHost = new QWidget;
    listHost->setObjectName(QStringLiteral("deliverOutputs"));
    theme::style(listHost, QStringLiteral("QWidget#deliverOutputs { background: transparent; }"));
    m_outputList = new QVBoxLayout(listHost);
    m_outputList->setContentsMargins(0, 0, 0, 0);
    m_outputList->setSpacing(6);
    m_outputList->addStretch();
    scroll->setWidget(listHost);
    theme::style(scroll, QStringLiteral("QScrollArea { background: transparent; } QScrollArea > QWidget > QWidget { background: transparent; }"));
    outputs->addWidget(scroll, 1);
    auto* addRow = new QHBoxLayout;
    m_addTemplate = new QComboBox;
    m_addTemplate->setObjectName(QStringLiteral("deliverTemplate"));
    m_addTemplate->setMaxVisibleItems(24);
    QString category;
    for (const ExportPreset& p : exportPresets())
        if (presetAvailable(p)) {
            if (p.category != category) {
                category = p.category;
                m_addTemplate->addItem(QStringLiteral("— %1 —").arg(category));
                if (auto* model = qobject_cast<QStandardItemModel*>(m_addTemplate->model()))
                    model->item(m_addTemplate->count() - 1)->setEnabled(false);
            }
            m_addTemplate->addItem(p.name, p.name);
        }
    m_addTemplate->setCurrentIndex(1);
    auto* add = new QPushButton(tr("Add"));
    addRow->addWidget(m_addTemplate, 1);
    addRow->addWidget(add);
    outputs->addLayout(addRow);
    auto* folderRow = new QHBoxLayout;
    m_folder = new QLineEdit;
    m_folder->setObjectName(QStringLiteral("deliverFolder"));
    auto* browse = new QPushButton(tr("Browse..."));
    auto* folderLabel = new QLabel(tr("Folder"));
    theme::style(folderLabel, QStringLiteral("background: transparent; border: none;"));
    folderRow->addWidget(folderLabel);
    folderRow->addWidget(m_folder, 1);
    folderRow->addWidget(browse);
    outputs->addLayout(folderRow);
    m_totals = dimLabel();
    m_totals->setObjectName(QStringLiteral("deliverTotals"));
    outputs->addWidget(m_totals);
    connect(add, &QPushButton::clicked, this, [this] {
        const QString name = m_addTemplate->currentData().toString();
        QStringList list = DeliverView::outputs();
        if (!name.isEmpty() && !list.contains(name))
            setOutputs(list << name);
    });
    connect(browse, &QPushButton::clicked, this, [this] {
        const QString dir = QFileDialog::getExistingDirectory(this, tr("Render Into"), QDir::fromNativeSeparators(m_folder->text()));
        if (!dir.isEmpty()) {
            m_folder->setText(QDir::toNativeSeparators(dir));
            emit m_folder->editingFinished();
        }
    });
    connect(m_folder, &QLineEdit::editingFinished, this, [this] {
        setChoice(QStringLiteral("folder"), QDir::fromNativeSeparators(m_folder->text()));
        fillOutputs();
    });

    // ---- Publish pack ----
    QVBoxLayout* pack = nullptr;
    right->addWidget(card(tr("Publish Pack"), &pack), 3);
    auto* grid = new QGridLayout;
    grid->setHorizontalSpacing(12);
    m_thumbLabel = new QLabel(tr("Click a frame in the strip\nor use the cursor frame"));
    m_thumbLabel->setObjectName(QStringLiteral("deliverThumbnail"));
    m_thumbLabel->setFixedSize(240, 135);
    m_thumbLabel->setAlignment(Qt::AlignCenter);
    theme::style(m_thumbLabel, QStringLiteral("border: 1px dashed @line; border-radius: 4px; color: @dim; background: @deep;"));
    grid->addWidget(m_thumbLabel, 0, 0, 4, 1);
    auto* titleLabel = new QLabel(tr("Title"));
    theme::style(titleLabel, QStringLiteral("background: transparent; border: none;"));
    m_title = new QLineEdit;
    m_title->setObjectName(QStringLiteral("deliverTitle"));
    m_title->setPlaceholderText(tr("The video's title (drawn on the thumbnail)"));
    auto* fromCursor = new QPushButton(tr("Use Cursor Frame"));
    fromCursor->setObjectName(QStringLiteral("deliverThumbFromCursor"));
    auto* tagLabel = new QLabel(tr("Tags"));
    theme::style(tagLabel, QStringLiteral("background: transparent; border: none;"));
    m_tags = new QLineEdit;
    m_tags->setObjectName(QStringLiteral("deliverTags"));
    m_tags->setPlaceholderText(tr("noughties, windows xp, nostalgia"));
    m_tagCount = dimLabel();
    grid->addWidget(titleLabel, 0, 1);
    grid->addWidget(m_title, 1, 1, 1, 2);
    grid->addWidget(fromCursor, 0, 2, Qt::AlignRight);
    grid->addWidget(tagLabel, 2, 1);
    grid->addWidget(m_tagCount, 2, 2, Qt::AlignRight);
    grid->addWidget(m_tags, 3, 1, 1, 2);
    grid->setColumnStretch(1, 1);
    pack->addLayout(grid);
    auto* descRow = new QHBoxLayout;
    auto* descLabel = new QLabel(tr("Description"));
    theme::style(descLabel, QStringLiteral("background: transparent; border: none;"));
    descRow->addWidget(descLabel);
    m_descCount = dimLabel();
    descRow->addWidget(m_descCount, 1);
    auto* insert = new QToolButton;
    insert->setText(tr("Insert"));
    insert->setPopupMode(QToolButton::InstantPopup);
    auto* insertMenu = new QMenu(insert);
    for (const auto& [label, token] : std::vector<std::pair<QString, QString>>{{tr("Chapters (filled in when copied or saved)"), QStringLiteral("{chapters}")},
                                                                              {tr("Track listing of the chosen track"), QStringLiteral("{tracklist}")},
                                                                              {tr("Title"), QStringLiteral("{title}")},
                                                                              {tr("Length"), QStringLiteral("{length}")},
                                                                              {tr("Today's date"), QStringLiteral("{date}")}})
        insertMenu->addAction(label, this, [this, token] { m_description->insertPlainText(token); });
    insert->setMenu(insertMenu);
    m_listingTrack = new QComboBox;
    m_listingTrack->setToolTip(tr("The audio track {tracklist} lists, \"01. Name - 04:19\", for an album video"));
    descRow->addWidget(m_listingTrack);
    descRow->addWidget(insert);
    pack->addLayout(descRow);
    m_description = new QPlainTextEdit;
    m_description->setObjectName(QStringLiteral("deliverDescription"));
    m_description->setPlaceholderText(tr("What the video is about.\n\n{chapters}"));
    pack->addWidget(m_description, 1);
    auto* packButtons = new QHBoxLayout;
    auto* copy = new QPushButton(tr("Copy Description"));
    auto* save = new QPushButton(tr("Save Pack"));
    save->setObjectName(QStringLiteral("deliverSavePack"));
    save->setToolTip(tr("Writes the title, description and tags, the thumbnail (under 2 MB) and the captions (.srt) into the output folder"));
    m_packStatus = dimLabel();
    packButtons->addWidget(copy);
    packButtons->addWidget(save);
    packButtons->addWidget(m_packStatus, 1);
    pack->addLayout(packButtons);
    auto counts = [this] {
        const int d = int(expandedDescription().size()), t = int(m_tags->text().size());
        m_descCount->setText(tr("%1 / %2").arg(d).arg(kDescriptionLimit));
        theme::style(m_descCount, d > kDescriptionLimit ? QStringLiteral("color: @bad; background: transparent; border: none;")
                                                        : QStringLiteral("color: @dim; background: transparent; border: none;"));
        m_tagCount->setText(tr("%1 / %2").arg(t).arg(kTagLimit));
        theme::style(m_tagCount, t > kTagLimit ? QStringLiteral("color: @bad; background: transparent; border: none;")
                                               : QStringLiteral("color: @dim; background: transparent; border: none;"));
    };
    connect(m_title, &QLineEdit::textEdited, this, [this, counts] {
        setChoice(QStringLiteral("title"), m_title->text());
        if (!m_thumbFrame.isNull()) {
            m_thumb = upload::thumbnail(m_thumbFrame, m_title->text());
            m_thumbLabel->setPixmap(QPixmap::fromImage(m_thumb.scaled(m_thumbLabel->size(), Qt::KeepAspectRatio, Qt::SmoothTransformation)));
        }
        counts();
    });
    connect(m_tags, &QLineEdit::textEdited, this, [this, counts] {
        setChoice(QStringLiteral("tags"), m_tags->text());
        counts();
    });
    connect(m_description, &QPlainTextEdit::textChanged, this, [this, counts] {
        if (!m_filling)
            setChoice(QStringLiteral("description"), m_description->toPlainText());
        counts();
    });
    connect(fromCursor, &QPushButton::clicked, this, [this] { pickThumbnail(m_hooks.cursor ? m_hooks.cursor() : 0); });
    connect(copy, &QPushButton::clicked, this, [this] {
        QApplication::clipboard()->setText(expandedDescription());
        m_packStatus->setText(tr("Description copied."));
    });
    connect(save, &QPushButton::clicked, this, [this] {
        QString error;
        m_packStatus->setText(savePack(&error) ? tr("Saved in %1").arg(m_folder->text()) : error);
    });

    // ---- History ----
    auto* historyButtons = new QWidget;
    auto* hb = new QHBoxLayout(historyButtons);
    hb->setContentsMargins(0, 0, 0, 0);
    auto* openFile = new QToolButton;
    openFile->setText(tr("Play"));
    auto* openFolder = new QToolButton;
    openFolder->setText(tr("Show in Folder"));
    auto* renderAgain = new QToolButton;
    renderAgain->setText(tr("Render Again"));
    renderAgain->setObjectName(QStringLiteral("deliverRenderAgain"));
    for (auto* b : {openFile, openFolder, renderAgain}) {
        b->setAutoRaise(true);
        hb->addWidget(b);
    }
    QVBoxLayout* history = nullptr;
    right->addWidget(card(tr("History"), &history, historyButtons), 2);
    m_history = new QTreeWidget;
    m_history->setObjectName(QStringLiteral("deliverHistory"));
    m_history->setHeaderLabels({tr("When"), tr("File"), tr("Template"), tr("Length"), tr("Size")});
    m_history->setRootIsDecorated(false);
    m_history->setAlternatingRowColors(true);
    m_history->header()->setSectionResizeMode(1, QHeaderView::Stretch);
    history->addWidget(m_history);
    auto selected = [this] { return m_history->currentItem() ? m_history->currentItem()->data(1, Qt::UserRole).toString() : QString(); };
    connect(openFile, &QToolButton::clicked, this, [selected] {
        if (!selected().isEmpty())
            QDesktopServices::openUrl(QUrl::fromLocalFile(selected()));
    });
    connect(m_history, &QTreeWidget::itemDoubleClicked, this, [](QTreeWidgetItem* item) {
        QDesktopServices::openUrl(QUrl::fromLocalFile(item->data(1, Qt::UserRole).toString()));
    });
    connect(openFolder, &QToolButton::clicked, this, [this, selected] {
        const QString dir = selected().isEmpty() ? QDir::fromNativeSeparators(m_folder->text()) : QFileInfo(selected()).absolutePath();
        QDesktopServices::openUrl(QUrl::fromLocalFile(dir));
    });
    connect(renderAgain, &QToolButton::clicked, this, [this] {
        auto* item = m_history->currentItem();
        const ExportPreset* p = item ? presetNamed(item->text(2)) : nullptr;
        RenderQueue* queue = m_hooks.queue ? m_hooks.queue() : nullptr;
        if (!p || !queue) {
            m_renderStatus->setText(tr("Pick a render whose template is known."));
            return;
        }
        const auto snap = m_project->snapshot();
        ExportSettings s = withGpuEncoder(settingsFromPreset(*p, snap->settings));
        s.path = item->data(1, Qt::UserRole).toString();
        s.rangeEnd = limitFor(*p) > 0 ? std::min(snap->timeline.duration(), limitFor(*p)) : snap->timeline.duration();
        s.captions = timelineCaptions(snap->timeline);
        QString why;
        if (s.rangeEnd > 0 && queue->add(baseName(), snap, s, &why)) {
            m_pending[QDir::cleanPath(s.path)] = {p->name, double(s.rangeEnd) / kUsPerSecond};
            queue->start();
            m_poll->start();
            m_renderStatus->setText(tr("Rendering %1 again").arg(QFileInfo(s.path).fileName()));
        } else
            m_renderStatus->setText(why);
    });

    split->setStretchFactor(0, 1);
    split->setStretchFactor(1, 1);

    m_poll = new QTimer(this);
    m_poll->setInterval(400);
    connect(m_poll, &QTimer::timeout, this, &DeliverView::pollQueue);
    // Edits while the tab is open: check again a moment later.
    m_again = new QTimer(this);
    m_again->setSingleShot(true);
    m_again->setInterval(700);
    connect(m_again, &QTimer::timeout, this, [this] {
        runChecks();
        rebuildStrip();
        updateHeader();
    });
    connect(m_project, &Project::changed, this, [this] {
        if (isVisible())
            m_again->start();
    });
}

DeliverView::~DeliverView()
{
    delete m_renderClock;
}

QString DeliverView::baseName() const
{
    return m_project->filePath().isEmpty() ? tr("Untitled") : QFileInfo(m_project->filePath()).completeBaseName();
}

void DeliverView::setChoice(const QString& key, const QJsonValue& value)
{
    QJsonObject d = m_project->deliver();
    if (!d.contains(key) && (value == QJsonValue(QString()) || value == QJsonValue(QJsonArray()))) // (nothing chosen yet)
        return;
    d[key] = value;
    m_project->setDeliver(d);
}

QStringList DeliverView::storedList(const QString& key, const QStringList& fallback) const
{
    const QJsonValue v = m_project->deliver().value(key);
    return v.isArray() ? v.toVariant().toStringList() : fallback;
}

void DeliverView::showEvent(QShowEvent* e)
{
    QWidget::showEvent(e);
    refresh();
}

void DeliverView::refresh()
{
    // Projects from before 0.8.0 kept these in this computer's settings: they come into the project (and its file, on
    // the next save).
    if (m_project->deliver().isEmpty() && !m_project->filePath().isEmpty()) {
        QSettings settings;
        settings.beginGroup(QStringLiteral("deliver/")
                            + QString::fromLatin1(QCryptographicHash::hash(QDir::cleanPath(m_project->filePath()).toLower().toUtf8(),
                                                                           QCryptographicHash::Md5).toHex()));
        QJsonObject old;
        for (const QString& key : settings.childKeys())
            old[key] = key == QLatin1String("outputs") || key == QLatin1String("off") ? QJsonValue(QJsonArray::fromStringList(settings.value(key).toStringList()))
                       : key == QLatin1String("target") ? QJsonValue(settings.value(key).toInt())
                                                         : QJsonValue(settings.value(key).toString());
        m_project->setDeliver(old, false);
    }
    const QJsonObject stored = m_project->deliver();
    m_shownPath = m_project->filePath();
    QString folder = stored.value(QStringLiteral("folder")).toString();
    // (a folder on a drive this computer doesn't have: the project came from another one. A folder not made yet is
    // made by the render.)
    for (QString up = folder; !folder.isEmpty() && !QFileInfo(up).isDir();) {
        const QString parent = QFileInfo(up).absolutePath();
        if (parent == up)
            folder.clear();
        up = parent;
    }
    if (folder.isEmpty())
        folder = m_project->filePath().isEmpty() ? Profiles::instance().folder(QStringLiteral("Exports")) : QFileInfo(m_project->filePath()).absolutePath();
    if (folder.isEmpty())
        folder = QDir::homePath();
    m_folder->setText(QDir::toNativeSeparators(folder));
    m_title->setText(stored.value(QStringLiteral("title")).toString(baseName()));
    m_tags->setText(stored.value(QStringLiteral("tags")).toString());
    {
        const QSignalBlocker block(m_target);
        m_target->setCurrentIndex(std::clamp(stored.value(QStringLiteral("target")).toInt(0), 0, m_target->count() - 1));
    }
    m_filling = true;
    m_description->setPlainText(stored.value(QStringLiteral("description")).toString());
    m_filling = false;
    emit m_tags->textEdited(m_tags->text()); // the counters
    m_listingTrack->clear();
    const auto& tracks = m_project->timeline().tracks;
    for (int i = 0; i < int(tracks.size()); ++i)
        if (tracks[size_t(i)].type == TrackType::Audio && !tracks[size_t(i)].clips.empty())
            m_listingTrack->addItem(tracks[size_t(i)].name.isEmpty() ? tr("Audio %1").arg(i + 1) : tracks[size_t(i)].name, i);
    m_listingTrack->setVisible(m_listingTrack->count() > 0);
    fillOutputs();
    runChecks();
    fillHistory();
    rebuildStrip();
    updateHeader();
}

void DeliverView::updateHeader()
{
    const ProjectSettings& ps = m_project->settings();
    const TimeUs duration = m_project->timeline().duration();
    m_projectName->setText(m_title->text().trimmed().isEmpty() ? baseName() : m_title->text().trimmed());
    m_projectInfo->setText(tr("%1  ·  %2x%3  ·  %4 fps  ·  %n chapter(s)", nullptr, int(segments(m_project->timeline()).size()))
                               .arg(formatDuration(duration))
                               .arg(ps.width)
                               .arg(ps.height)
                               .arg(ps.fps.toDouble(), 0, 'g', 5));
    const bool ready = m_problems == 0 && duration > 0;
    m_readiness->setText(ready ? tr("Ready to deliver") : duration <= 0 ? tr("Nothing to deliver") : tr("%n to look at", nullptr, m_problems));
    theme::style(m_readiness, ready ? QStringLiteral("background: @good; color: @window; border-radius: 9px; padding: 2px 12px; font-weight: bold;")
                                    : QStringLiteral("background: @warn; color: @window; border-radius: 9px; padding: 2px 12px; font-weight: bold;"));
    int ticked = 0;
    for (const auto& row : m_rows)
        ticked += row->tick->isChecked();
    m_render->setText(ticked > 1 ? tr("Render %1").arg(ticked) : tr("Render"));
}

QStringList DeliverView::outputs() const
{
    QStringList names;
    for (const auto& row : m_rows)
        names << row->templateName;
    return names;
}

void DeliverView::setOutputs(const QStringList& templateNames)
{
    setChoice(QStringLiteral("outputs"), QJsonArray::fromStringList(templateNames));
    fillOutputs();
    runChecks();
    updateHeader();
}

QString DeliverView::outputPath(const ExportPreset& p) const
{
    // "05 Y2K - YouTube 1080p.mp4", "05 Y2K - MP3.mp3": the template's name up to its first " - " or "(",
    // or all of it ("05 Y2K - MP3 320 kbs.mp3") when another output's would be the same.
    static const QRegularExpression illegal(QStringLiteral("[\\\\/:*?\"<>|]"));
    auto shortLabel = [](const QString& name) { return name.section(QStringLiteral(" - "), 0, 0).section(QStringLiteral(" ("), 0, 0).trimmed(); };
    QString label = shortLabel(p.name);
    const QStringList listed = storedList(QStringLiteral("outputs"), {QStringLiteral("YouTube 1080p")});
    for (const QString& other : listed)
        if (other != p.name && shortLabel(other).compare(label, Qt::CaseInsensitive) == 0) {
            label = QString(p.name).replace(QStringLiteral(" - "), QStringLiteral(" "));
            label.remove(illegal);
            label.remove(QLatin1Char('(')).remove(QLatin1Char(')'));
            break;
        }
    label.replace(illegal, QStringLiteral(" "));
    const QString ext = p.extension.isEmpty() ? QStringLiteral("mp4") : p.extension;
    return QDir(QDir::fromNativeSeparators(m_folder->text())).filePath(baseName() + QStringLiteral(" - ") + label.simplified() + QLatin1Char('.') + ext);
}

double DeliverView::estimateBytes(const ExportPreset& p, int width, int height, double fps, TimeUs length, bool* exact)
{
    const double seconds = double(length) / kUsPerSecond;
    if (exact)
        *exact = true;
    if (p.targetSizeMB > 0)
        return std::min(p.targetSizeMB * 1024 * 1024, 1e18);
    double audio = 0;
    if (!p.audioEncoder.isEmpty()) {
        if (p.audioEncoder.startsWith(QLatin1String("pcm_"))) {
            const int bits = p.audioEncoder.contains(QLatin1String("24")) ? 24 : p.audioEncoder.contains(QLatin1String("32")) ? 32
                           : p.audioEncoder.contains(QLatin1String("u8")) ? 8 : 16;
            audio = double(p.sampleRate) * p.channels * bits;
        } else if (p.audioBitrate > 0)
            audio = double(p.audioBitrate);
        else {
            audio = double(p.sampleRate) * p.channels * 16 * 0.6; // lossless: about 60% of PCM
            if (exact)
                *exact = false;
        }
    }
    double video = 0;
    if (!p.videoEncoder.isEmpty()) {
        const double w = p.width > 0 ? p.width : width, h = p.height > 0 ? p.height : (p.width > 0 ? p.width * double(height) / width : height);
        const double rate = p.fps.valid() ? p.fps.toDouble() : fps;
        if (p.videoBitrate > 0)
            video = double(p.videoBitrate);
        else {
            // Quality-based: bits per pixel by codec family, a rough figure for ordinary footage.
            const QString e = p.videoEncoder;
            const double bpp = e.contains(QLatin1String("prores")) || e.contains(QLatin1String("dnxhd")) || e.contains(QLatin1String("cfhd")) ? 1.6
                             : e.contains(QLatin1String("ffv1")) || e.contains(QLatin1String("utvideo")) || e.contains(QLatin1String("v210"))
                                     || e.contains(QLatin1String("rle")) || e.contains(QLatin1String("png"))
                                 ? 6.0
                             : e.contains(QLatin1String("mjpeg")) || e.contains(QLatin1String("hap")) ? 0.9
                             : e.contains(QLatin1String("265")) || e.contains(QLatin1String("hevc")) || e.contains(QLatin1String("av1"))
                                     || e.contains(QLatin1String("vp9")) || e.contains(QLatin1String("vvc"))
                                 ? 0.05
                             : e.contains(QLatin1String("gif")) ? 0.35 : 0.09;
            video = w * h * rate * bpp;
            if (exact)
                *exact = false;
        }
    }
    return (audio + video) * seconds / 8;
}

void DeliverView::fillOutputs()
{
    const QStringList names = storedList(QStringLiteral("outputs"), {QStringLiteral("YouTube 1080p")});
    const QStringList off = storedList(QStringLiteral("off"));
    for (auto& row : m_rows)
        delete row->widget;
    m_rows.clear();
    const ProjectSettings& ps = m_project->settings();
    const TimeUs duration = m_project->timeline().duration();
    for (const QString& name : names) {
        const ExportPreset* p = presetNamed(name);
        if (!p)
            continue;
        auto row = std::make_unique<OutputRow>();
        row->templateName = name;
        row->path = outputPath(*p);
        row->widget = new QFrame;
        row->widget->setObjectName(QStringLiteral("deliverOutput"));
        theme::style(row->widget, QStringLiteral("QFrame#deliverOutput { background: @raised; border-radius: 6px; } QLabel { background: transparent; }"));
        auto* grid = new QGridLayout(row->widget);
        grid->setContentsMargins(10, 6, 6, 6);
        grid->setHorizontalSpacing(10);
        grid->setVerticalSpacing(2);
        row->tick = new QCheckBox;
        row->tick->setChecked(!off.contains(name));
        auto* title = new QLabel(name);
        theme::style(title, QStringLiteral("font-weight: bold; color: @bright;"));
        title->setToolTip(p->notes);
        const TimeUs length = limitFor(*p) > 0 ? std::min(duration, limitFor(*p)) : duration;
        bool exact = true;
        const double bytes = estimateBytes(*p, ps.width, ps.height, ps.fps.toDouble(), length, &exact);
        const int w = p->width > 0 ? p->width : ps.width, h = p->height > 0 ? p->height : (p->width > 0 ? p->width * ps.height / std::max(1, ps.width) : ps.height);
        QStringList detail;
        if (!p->videoEncoder.isEmpty())
            detail << QStringLiteral("%1x%2").arg(w).arg(h) << tr("%1 fps").arg(p->fps.valid() ? p->fps.toDouble() : ps.fps.toDouble(), 0, 'g', 5)
                   << codecName(p->videoEncoder);
        if (!p->audioEncoder.isEmpty())
            detail << codecName(p->audioEncoder);
        if (duration > 0)
            detail << (exact ? QString() : QStringLiteral("~")) + sizeText(bytes);
        if (limitFor(*p) > 0)
            detail << tr("first %1").arg(formatDuration(length));
        row->details = dimLabel(detail.join(QStringLiteral("  ·  ")));
        row->file = dimLabel(QFileInfo(row->path).fileName());
        row->file->setToolTip(QDir::toNativeSeparators(row->path));
        row->state = dimLabel();
        row->bar = new QProgressBar;
        row->bar->setTextVisible(false);
        row->bar->setFixedHeight(5);
        row->bar->setRange(0, 1000);
        row->bar->setVisible(false);
        auto* remove = new QToolButton;
        remove->setText(QStringLiteral("✕"));
        remove->setAutoRaise(true);
        remove->setToolTip(tr("Take this output out"));
        grid->addWidget(row->tick, 0, 0, 2, 1);
        grid->addWidget(title, 0, 1);
        grid->addWidget(row->state, 0, 2, Qt::AlignRight);
        grid->addWidget(remove, 0, 3);
        grid->addWidget(row->details, 1, 1, 1, 3);
        grid->addWidget(row->file, 2, 1, 1, 3);
        grid->addWidget(row->bar, 3, 1, 1, 3);
        grid->setColumnStretch(1, 1);
        connect(row->tick, &QCheckBox::toggled, this, [this] {
            QStringList offNow;
            for (const auto& r : m_rows)
                if (!r->tick->isChecked())
                    offNow << r->templateName;
            setChoice(QStringLiteral("off"), QJsonArray::fromStringList(offNow));
            runChecks();
            updateTotals();
            updateHeader();
        });
        connect(remove, &QToolButton::clicked, this, [this, name] {
            QStringList list = outputs();
            list.removeAll(name);
            QTimer::singleShot(0, this, [this, list] { setOutputs(list); }); // not while its button is still in use
        });
        m_outputList->insertWidget(m_outputList->count() - 1, row->widget);
        m_rows.push_back(std::move(row));
    }
    updateTotals();
}

void DeliverView::updateTotals()
{
    const ProjectSettings& ps = m_project->settings();
    const TimeUs duration = m_project->timeline().duration();
    double total = 0;
    bool exact = true;
    int ticked = 0;
    for (const auto& row : m_rows) {
        const ExportPreset* p = presetNamed(row->templateName);
        if (!p || !row->tick->isChecked())
            continue;
        ++ticked;
        bool e = true;
        total += estimateBytes(*p, ps.width, ps.height, ps.fps.toDouble(), limitFor(*p) > 0 ? std::min(duration, limitFor(*p)) : duration, &e);
        exact = exact && e;
    }
    QStorageInfo disk(QDir::fromNativeSeparators(m_folder->text()));
    if (!disk.isValid())
        disk = QStorageInfo(QFileInfo(QDir::fromNativeSeparators(m_folder->text())).absolutePath());
    const double free = disk.isValid() ? double(disk.bytesAvailable()) : -1;
    m_totalBytes = total;
    m_totalExact = exact;
    m_freeBytes = free;
    QString text = tr("%n output(s)", nullptr, ticked);
    if (ticked && duration > 0)
        text += tr("  ·  about %1").arg((exact ? QString() : QStringLiteral("~")) + sizeText(total));
    if (free >= 0)
        text += tr("  ·  %1 free").arg(sizeText(free));
    m_totals->setText(text);
    theme::style(m_totals, free >= 0 && total > free * 0.9 ? QStringLiteral("color: @bad; background: transparent; border: none;")
                                                         : QStringLiteral("color: @dim; background: transparent; border: none;"));
}

TimeUs DeliverView::limitFor(const ExportPreset& p) const
{
    const auto it = m_trim.find(p.name);
    return it == m_trim.end() ? 0 : it->second;
}

void DeliverView::runChecks()
{
    while (QLayoutItem* item = m_checkList->takeAt(0)) {
        delete item->widget();
        delete item;
    }
    const Timeline& tl = m_project->timeline();
    const TimeUs duration = tl.duration();
    m_problems = 0;
    enum Kind { Bad, Good, Note };
    auto row = [&](int kind, const QString& text, const QString& action = {}, std::function<void()> fix = {}) {
        auto* w = new QWidget;
        w->setObjectName(QStringLiteral("deliverCheck"));
        w->setProperty("ok", kind != Bad);
        auto* h = new QHBoxLayout(w);
        h->setContentsMargins(2, 3, 2, 3);
        auto* icon = new QLabel;
        icon->setPixmap(kind == Bad ? badge(theme::c().warn, QStringLiteral("!")) : kind == Good ? badge(theme::c().good, QStringLiteral("✓"))
                                                                                                 : badge(theme::c().raised, QStringLiteral("i")));
        icon->setFixedSize(20, 20);
        auto* label = new QLabel(text);
        label->setWordWrap(true);
        theme::style(label, kind == Note ? QStringLiteral("color: @dim; background: transparent;") : QStringLiteral("background: transparent;"));
        h->addWidget(icon);
        h->addWidget(label, 1);
        if (!action.isEmpty() && fix) {
            auto* button = new QPushButton(action);
            connect(button, &QPushButton::clicked, this, [this, fix] { QTimer::singleShot(0, this, fix); });
            h->addWidget(button);
        }
        m_checkList->addWidget(w);
        if (kind == Bad)
            ++m_problems;
        QJsonObject data{{QStringLiteral("status"), kind == Bad ? QStringLiteral("look") : kind == Good ? QStringLiteral("ok") : QStringLiteral("note")},
                         {QStringLiteral("text"), text}};
        if (!action.isEmpty())
            data[QStringLiteral("fix")] = action;
        m_checkData.append(data);
    };
    m_checkData = QJsonArray();
    m_meter->target = finishing::targets()[size_t(std::max(0, m_target->currentIndex()))].lufs;
    m_meter->ceiling = finishing::targets()[size_t(std::max(0, m_target->currentIndex()))].truePeak;
    m_meter->lufs.reset();
    m_meter->peak.reset();
    if (duration <= 0) {
        row(Bad, tr("The timeline is empty."));
        m_meter->update();
        updateHeader();
        return;
    }
    if (m_project->filePath().isEmpty())
        row(Bad, tr("The project isn't saved: file names and History follow its name."), tr("Save"), [this] {
            if (m_hooks.save && m_hooks.save())
                refresh();
        });

    // Offline media.
    int offline = 0;
    TimeUs firstOffline = -1;
    for (const Track& t : tl.tracks)
        for (const Clip& c : t.clips)
            if (!c.isGenerated() && !c.isSequence()) { // (a compound sequence's own events are its media)
                const auto media = m_project->mediaItem(c.mediaId);
                if (!media || !media->info.ok()) {
                    ++offline;
                    if (firstOffline < 0 || c.start < firstOffline)
                        firstOffline = c.start;
                }
            }
    if (offline)
        row(Bad, tr("%n event(s) use offline media.", nullptr, offline), tr("Go There"), [this, firstOffline] { m_hooks.seek(firstOffline); });
    else
        row(Good, tr("All media online."));

    const auto gaps = pictureGaps(tl);
    if (gaps.empty())
        row(Good, tr("Picture all the way through."));
    else
        row(Bad,
            tr("Nothing on screen %1–%2").arg(formatDuration(gaps.front().first), formatDuration(gaps.front().second))
                + (gaps.size() > 1 ? tr(" (and %n more)", nullptr, int(gaps.size() - 1)) : QString()),
            tr("Go There"), [this, at = gaps.front().first] { m_hooks.seek(at); });

    // Loudness, once measured.
    if (m_loudness && !m_loudness->cancelled) {
        if (!m_loudness->error.isEmpty())
            row(Bad, m_loudness->error);
        else {
            const finishing::Target target = finishing::targets()[size_t(std::max(0, m_target->currentIndex()))];
            m_meter->lufs = m_loudness->integrated;
            m_meter->peak = m_loudness->truePeak;
            const QStringList warnings = finishing::warnings(*m_loudness, target);
            if (warnings.isEmpty())
                row(Good, tr("Loudness on target."));
            else
                row(Bad, warnings.front(), tr("Fix..."), [this] {
                    if (m_hooks.audioFinishing)
                        m_hooks.audioFinishing();
                    m_loudness.reset();
                    runChecks();
                });
        }
    } else
        row(Note, tr("Loudness not measured yet."));
    m_meter->update();

    // YouTube chapters.
    QStringList chapterWarnings;
    const QString chapters = upload::youtubeChapters(tl, &chapterWarnings);
    if (chapters.isEmpty())
        row(Note, tr("No chapters (story chapters or markers make YouTube chapters)."));
    else if (chapterWarnings.isEmpty())
        row(Good, tr("%n YouTube chapter(s).", nullptr, int(chapters.count(QLatin1Char('\n')) + 1)));
    else
        for (const QString& w : chapterWarnings)
            row(Bad, w);

    // Templates with a platform length limit.
    for (const auto& r : m_rows) {
        const ExportPreset* p = presetNamed(r->templateName);
        if (!p || !r->tick->isChecked() || p->maxSeconds <= 0)
            continue;
        const TimeUs limit = TimeUs(p->maxSeconds) * kUsPerSecond;
        if (duration <= limit)
            continue;
        if (limitFor(*p) > 0)
            row(Good, tr("%1 renders the first %2.").arg(p->name, formatDuration(limitFor(*p))), tr("Whole Edit"), [this, name = p->name] {
                m_trim.erase(name);
                fillOutputs();
                runChecks();
            });
        else
            row(Bad, tr("%1 is %2 long; %3 accepts up to %4.").arg(p->name, formatDuration(duration), p->name.section(QLatin1Char(' '), 0, 0), formatDuration(limit)),
                tr("Trim"), [this, name = p->name, limit] {
                    m_trim[name] = limit;
                    fillOutputs();
                    runChecks();
                });
    }

    // Captions for the pack.
    const int captions = int(timelineCaptions(tl).size());
    row(captions ? Good : Note, captions ? tr("%n caption(s) go into the pack and subtitle tracks.", nullptr, captions) : tr("No captions."));
    updateHeader();
}

void DeliverView::rebuildStrip()
{
    const Timeline& tl = m_project->timeline();
    m_strip->duration = tl.duration();
    m_strip->chapters = segments(tl);
    m_strip->gaps = pictureGaps(tl);
    m_strip->markers.clear();
    for (const Marker& m : tl.markers)
        m_strip->markers.push_back(m.time);
    m_strip->selected = m_thumbTime;
    m_strip->frames.clear();
    const int count = std::clamp(m_strip->width() / 130, 4, 16);
    if (m_strip->duration > 0)
        for (int i = 0; i < count; ++i)
            m_strip->frames.push_back({TimeUs(double(m_strip->duration) * (i + 0.5) / count), QImage()});
    m_strip->update();
    // The frames, one at a time so the window keeps moving; an edit starts them again.
    const int generation = ++m_stripGeneration;
    auto next = std::make_shared<std::function<void(size_t)>>();
    *next = [this, generation, next](size_t i) {
        if (generation != m_stripGeneration || i >= m_strip->frames.size() || !isVisible())
            return;
        if (!m_stripRenderer)
            m_stripRenderer = std::make_unique<Compositor>();
        const ProjectSettings& ps = m_project->settings();
        const QSize size = QSize(ps.width, ps.height).scaled(240, 240, Qt::KeepAspectRatio);
        m_strip->frames[i].second = m_stripRenderer->render(*m_project->snapshot(), m_strip->frames[i].first, size);
        m_strip->update();
        QTimer::singleShot(15, this, [next, i] { (*next)(i + 1); });
    };
    QTimer::singleShot(30, this, [next] { (*next)(0); });
}

int DeliverView::renderOutputs(QString* error)
{
    if (m_project->filePath() != m_shownPath) // saved (or renamed) since: the file names follow the project's
        refresh();
    RenderQueue* queue = m_hooks.queue ? m_hooks.queue() : nullptr;
    const auto snap = m_project->snapshot();
    const TimeUs duration = snap->timeline.duration();
    if (!queue || duration <= 0) {
        if (error)
            *error = tr("The timeline is empty.");
        return 0;
    }
    QDir().mkpath(QDir::fromNativeSeparators(m_folder->text()));
    const auto captions = timelineCaptions(snap->timeline);
    int added = 0;
    for (const auto& row : m_rows) {
        const ExportPreset* p = presetNamed(row->templateName);
        if (!p || !row->tick->isChecked())
            continue;
        ExportSettings s = withGpuEncoder(settingsFromPreset(*p, snap->settings));
        s.path = row->path;
        s.rangeStart = 0;
        s.rangeEnd = limitFor(*p) > 0 ? std::min(duration, limitFor(*p)) : duration;
        s.captions = captions;
        QString why;
        if (queue->add(baseName(), snap, s, &why)) {
            ++added;
            m_pending[QDir::cleanPath(s.path)] = {p->name, double(s.rangeEnd - s.rangeStart) / kUsPerSecond};
            row->state->setText(tr("Waiting"));
            row->bar->setValue(0);
            row->bar->setVisible(true);
        } else {
            row->state->setText(why);
            if (error)
                *error = why;
        }
    }
    if (!m_queueConnected) {
        m_queueConnected = true;
        connect(queue, &RenderQueue::rendered, this, [this](const QString& path) {
            const QString clean = QDir::cleanPath(path);
            if (const auto it = m_pending.find(clean); it != m_pending.end()) {
                remember(m_project->filePath(), path, it->second.first, it->second.second);
                fillHistory();
            }
        });
    }
    if (added) {
        m_batch = added;
        m_renderClock->start();
        m_overall->setValue(0);
        m_overall->setVisible(true);
        m_openWhenDone->setVisible(false);
        m_renderStatus->setText(tr("Rendering %n output(s)...", nullptr, added));
        queue->start();
        m_poll->start();
    }
    return added;
}

void DeliverView::pollQueue()
{
    RenderQueue* queue = m_hooks.queue ? m_hooks.queue() : nullptr;
    if (!queue)
        return;
    const QJsonObject status = queue->status();
    const QJsonArray jobs = status[QStringLiteral("jobs")].toArray();
    const bool running = status[QStringLiteral("running")].toBool();
    bool active = false, stopped = false; // (a stopped queue's waiting jobs kept "Rendering", its time left growing, forever)
    int done = 0, failed = 0;
    double progress = 0;
    for (const auto& row : m_rows) {
        const QString path = QDir::cleanPath(row->path);
        for (auto it = jobs.end(); it != jobs.begin();) { // the latest job for the file (an earlier render's counted too)
            const QJsonObject job = (*--it).toObject();
            if (QDir::cleanPath(job[QStringLiteral("path")].toString()) != path)
                continue;
            const QString state = job[QStringLiteral("state")].toString();
            if (state == QLatin1String("rendering")) {
                const double f = job[QStringLiteral("progress")].toDouble();
                row->state->setText(tr("%1%").arg(int(f * 100)));
                row->bar->setValue(int(f * 1000));
                progress += f;
                active = true;
            } else if (state == QLatin1String("waiting")) {
                row->state->setText(running ? tr("Waiting") : tr("Stopped"));
                (running ? active : stopped) = true;
            } else if (state == QLatin1String("done")) {
                row->state->setText(tr("Done ✓"));
                row->bar->setValue(1000);
                ++done;
            } else {
                row->state->setText(job[QStringLiteral("error")].toString().isEmpty() ? state : job[QStringLiteral("error")].toString());
                row->state->setToolTip(job[QStringLiteral("error")].toString());
                ++failed;
            }
            break;
        }
    }
    const int batch = std::max(1, m_batch);
    const double fraction = std::clamp((done + failed + progress) / batch, 0.0, 1.0);
    m_overall->setValue(int(fraction * m_overall->maximum()));
    if (active) {
        const double elapsed = m_renderClock->elapsed() / 1000.0;
        QString text = tr("Rendering %1 of %2").arg(std::min(batch, done + failed + 1)).arg(batch);
        const double left = fraction > 0.03 ? elapsed * (1 - fraction) / fraction : 0;
        if (left >= 1)
            text += tr("  ·  about %1 left").arg(formatDuration(TimeUs(left) * kUsPerSecond));
        m_renderStatus->setText(text);
        return;
    }
    m_poll->stop();
    m_overall->setVisible(false);
    m_renderStatus->setText(stopped  ? tr("Stopped: %1 of %2 rendered").arg(done).arg(batch)
                            : failed ? tr("%1 rendered, %2 failed").arg(done).arg(failed)
                                   : tr("All %n rendered in %1", nullptr, done).arg(formatDuration(TimeUs(m_renderClock->elapsed()) * 1000)));
    m_openWhenDone->setVisible(done > 0);
    fillHistory();
}

void DeliverView::pickThumbnail(TimeUs t)
{
    const auto snap = m_project->snapshot();
    const ProjectSettings& ps = snap->settings;
    QApplication::setOverrideCursor(Qt::WaitCursor);
    Compositor renderer(true);
    m_thumbFrame = renderer.render(*snap, t, QSize(ps.width, ps.height));
    QApplication::restoreOverrideCursor();
    m_thumbTime = t;
    m_strip->selected = t;
    m_strip->update();
    m_thumb = upload::thumbnail(m_thumbFrame, m_title->text());
    m_thumbLabel->setPixmap(QPixmap::fromImage(m_thumb.scaled(m_thumbLabel->size(), Qt::KeepAspectRatio, Qt::SmoothTransformation)));
    m_thumbLabel->setToolTip(tr("From %1").arg(formatDuration(t)));
}

QString DeliverView::expandedDescription() const
{
    QString text = m_description->toPlainText();
    if (text.contains(QLatin1String("{chapters}")))
        text.replace(QStringLiteral("{chapters}"), upload::youtubeChapters(m_project->timeline()));
    if (text.contains(QLatin1String("{tracklist}"))) {
        const int track = m_listingTrack->currentData().isValid() ? m_listingTrack->currentData().toInt() : -1;
        text.replace(QStringLiteral("{tracklist}"), track >= 0 ? upload::trackListing(*m_project, track) : QString());
    }
    text.replace(QStringLiteral("{title}"), m_title->text().trimmed());
    text.replace(QStringLiteral("{length}"), formatDuration(m_project->timeline().duration()));
    text.replace(QStringLiteral("{date}"), QLocale().toString(QDate::currentDate(), QLocale::LongFormat));
    return text;
}

bool DeliverView::savePack(QString* error)
{
    const QDir dir(QDir::fromNativeSeparators(m_folder->text()));
    if (!dir.exists() && !QDir().mkpath(dir.path())) {
        if (error)
            *error = tr("Could not make %1.").arg(m_folder->text());
        return false;
    }
    auto write = [&](const QString& name, const QByteArray& bytes) {
        QSaveFile f(dir.filePath(name));
        const bool ok = f.open(QIODevice::WriteOnly) && f.write(bytes) == bytes.size() && f.commit();
        if (!ok && error)
            *error = tr("Could not write %1.").arg(QDir::toNativeSeparators(dir.filePath(name)));
        return ok;
    };
    const QString base = baseName();
    QString text = m_title->text().trimmed();
    const QString description = expandedDescription().trimmed();
    if (!description.isEmpty())
        text += (text.isEmpty() ? QString() : QStringLiteral("\n\n")) + description;
    if (!m_tags->text().trimmed().isEmpty())
        text += tr("\n\nTags: %1").arg(m_tags->text().trimmed());
    if (!write(base + tr(" - description.txt"), (text + QLatin1Char('\n')).toUtf8()))
        return false;
    const auto captions = timelineCaptions(m_project->timeline());
    if (!captions.empty() && !write(base + QStringLiteral(".srt"), writeSrt(captions).toUtf8()))
        return false;
    if (!m_thumb.isNull()) {
        const QString path = dir.filePath(base + tr(" - thumbnail.jpg"));
        bool saved = false;
        for (int quality = 92; quality >= 50 && !saved; quality -= 10) // YouTube takes thumbnails up to 2 MB
            saved = m_thumb.save(path, "JPG", quality) && QFileInfo(path).size() <= 2 * 1024 * 1024;
        if (!saved) {
            if (error)
                *error = tr("Could not write the thumbnail under 2 MB.");
            return false;
        }
    }
    return true;
}

void DeliverView::remember(const QString& projectPath, const QString& file, const QString& templateName, double seconds)
{
    QSettings settings;
    QJsonArray history = QJsonDocument::fromJson(settings.value(QStringLiteral("deliver/history")).toByteArray()).array();
    const QString clean = QDir::cleanPath(file);
    QString keep = templateName;
    for (int i = history.size() - 1; i >= 0; --i)
        if (QDir::cleanPath(history[i].toObject()[QStringLiteral("file")].toString()) == clean) {
            if (keep.isEmpty())
                keep = history[i].toObject()[QStringLiteral("template")].toString();
            history.removeAt(i);
        }
    history.append(QJsonObject{{QStringLiteral("project"), QDir::cleanPath(projectPath)},
                               {QStringLiteral("file"), clean},
                               {QStringLiteral("template"), keep},
                               {QStringLiteral("seconds"), seconds},
                               {QStringLiteral("when"), QDateTime::currentDateTime().toString(Qt::ISODate)}});
    while (history.size() > kHistoryLimit)
        history.removeFirst();
    settings.setValue(QStringLiteral("deliver/history"), QJsonDocument(history).toJson(QJsonDocument::Compact));
}

void DeliverView::fillHistory()
{
    m_history->clear();
    const QJsonArray history = QJsonDocument::fromJson(QSettings().value(QStringLiteral("deliver/history")).toByteArray()).array();
    const QString project = QDir::cleanPath(m_project->filePath());
    // Lengths read from the files, kept while the file is unchanged.
    static std::map<QString, std::pair<QDateTime, TimeUs>> lengths;
    for (int i = history.size() - 1; i >= 0; --i) {
        const QJsonObject h = history[i].toObject();
        if (h[QStringLiteral("project")].toString() != project)
            continue;
        const QString file = h[QStringLiteral("file")].toString();
        const QFileInfo info(file);
        const QDateTime when = QDateTime::fromString(h[QStringLiteral("when")].toString(), Qt::ISODate);
        QString length = tr("missing");
        QString warning;
        if (info.exists()) {
            auto& cached = lengths[file];
            if (cached.first != info.lastModified()) {
                const MediaInfo media = probeMedia(file);
                cached = {info.lastModified(), media.ok() ? std::max<TimeUs>(1, media.duration) : 0};
            }
            const double expected = h[QStringLiteral("seconds")].toDouble();
            length = cached.second > 0 ? formatDuration(cached.second) : tr("unreadable");
            if (cached.second <= 0)
                warning = tr("The file can't be read.");
            else if (expected > 0 && std::abs(double(cached.second) / kUsPerSecond - expected) > 1.5)
                warning = tr("Expected %1.").arg(formatDuration(TimeUs(expected * kUsPerSecond)));
        }
        auto* item = new QTreeWidgetItem(m_history, {QLocale().toString(when, QLocale::ShortFormat), info.fileName(), h[QStringLiteral("template")].toString(),
                                                      length, info.exists() ? sizeText(double(info.size())) : QString()});
        item->setData(1, Qt::UserRole, file);
        item->setToolTip(1, QDir::toNativeSeparators(file));
        if (!warning.isEmpty()) {
            item->setForeground(3, theme::c().warn);
            item->setToolTip(3, warning);
        }
        if (!info.exists())
            for (int c = 0; c < 5; ++c)
                item->setForeground(c, theme::c().faint);
    }
}

const std::vector<DeliverView::Pack>& DeliverView::packs()
{
    static const std::vector<Pack> list = {
        {QStringLiteral("youtube"), tr("YouTube: 1080p and Shorts"), {QStringLiteral("YouTube 1080p"), QStringLiteral("YouTube Shorts (1080x1920)")}},
        {QStringLiteral("social"), tr("Social: Reels, TikTok, square"),
         {QStringLiteral("Instagram Reels (1080x1920)"), QStringLiteral("TikTok (1080x1920)"), QStringLiteral("Instagram Feed - square (1080x1080)")}},
        {QStringLiteral("audio"), tr("Audio: MP3 and WAV"), {QStringLiteral("MP3 - 320 kb/s"), QStringLiteral("WAV - 24-bit PCM (studio)")}},
        {QStringLiteral("masters"), tr("Masters: ProRes HQ and lossless"),
         {QStringLiteral("MOV - ProRes 422 HQ + PCM (editing master)"), QStringLiteral("MKV - FFV1 lossless RGB + FLAC (archive master)")}},
        {QStringLiteral("share"), tr("Share: small and Discord"),
         {QStringLiteral("Small preview (480p, 1.5 Mb/s)"), QStringLiteral("Discord (free, 20 MB)")}}};
    return list;
}

bool DeliverView::addPack(const QString& id)
{
    for (const Pack& pk : DeliverView::packs())
        if (pk.id == id) {
            QStringList list = outputs();
            for (const QString& n : pk.templates)
                if (presetNamed(n) && !list.contains(n))
                    list << n;
            setOutputs(list);
            return true;
        }
    return false;
}

void DeliverView::setTicked(const QString& templateName, bool ticked)
{
    for (const auto& row : m_rows)
        if (row->templateName == templateName)
            row->tick->setChecked(ticked); // saves, checks again
}

void DeliverView::setTrim(const QString& templateName, TimeUs limit)
{
    if (limit > 0)
        m_trim[templateName] = limit;
    else
        m_trim.erase(templateName);
    fillOutputs();
    runChecks();
}

void DeliverView::setFolder(const QString& folder)
{
    m_folder->setText(QDir::toNativeSeparators(folder));
    emit m_folder->editingFinished();
}

void DeliverView::setPackText(const std::optional<QString>& title, const std::optional<QString>& description, const std::optional<QString>& tags)
{
    if (title) {
        m_title->setText(*title);
        emit m_title->textEdited(*title);
    }
    if (description)
        m_description->setPlainText(*description); // saved by textChanged
    if (tags) {
        m_tags->setText(*tags);
        emit m_tags->textEdited(*tags);
    }
    updateHeader();
}

void DeliverView::measureLoudness()
{
    const auto snap = m_project->snapshot();
    if (snap->timeline.duration() <= 0)
        return;
    m_loudness = analyzeAudioMix(this, *snap);
    runChecks();
}

QJsonObject DeliverView::report() const
{
    const TimeUs duration = m_project->timeline().duration();
    const ProjectSettings& ps = m_project->settings();
    QJsonArray outs;
    for (const auto& row : m_rows) {
        const ExportPreset* p = presetNamed(row->templateName);
        if (!p)
            continue;
        const TimeUs length = limitFor(*p) > 0 ? std::min(duration, limitFor(*p)) : duration;
        bool exact = true;
        QJsonObject o{{QStringLiteral("template"), row->templateName},
                      {QStringLiteral("ticked"), row->tick->isChecked()},
                      {QStringLiteral("file"), row->path},
                      {QStringLiteral("estimated_bytes"), std::round(estimateBytes(*p, ps.width, ps.height, ps.fps.toDouble(), length, &exact))},
                      {QStringLiteral("estimate_exact"), exact}};
        if (limitFor(*p) > 0)
            o[QStringLiteral("trimmed_to")] = double(limitFor(*p)) / kUsPerSecond;
        if (p->maxSeconds > 0)
            o[QStringLiteral("platform_limit")] = p->maxSeconds;
        if (!row->state->text().isEmpty())
            o[QStringLiteral("state")] = row->state->text();
        outs.append(o);
    }
    QJsonObject r{{QStringLiteral("ready"), m_problems == 0 && duration > 0},
                  {QStringLiteral("problems"), m_problems},
                  {QStringLiteral("checks"), m_checkData},
                  {QStringLiteral("seconds"), double(duration) / kUsPerSecond},
                  {QStringLiteral("folder"), QDir::fromNativeSeparators(m_folder->text())},
                  {QStringLiteral("outputs"), outs},
                  {QStringLiteral("total_estimated_bytes"), std::round(m_totalBytes)},
                  {QStringLiteral("title"), m_title->text()},
                  {QStringLiteral("description"), m_description->toPlainText()},
                  {QStringLiteral("description_filled_in"), expandedDescription()},
                  {QStringLiteral("tags"), m_tags->text()},
                  {QStringLiteral("loudness_target"), finishing::targets()[size_t(std::max(0, m_target->currentIndex()))].name}};
    if (m_freeBytes >= 0)
        r[QStringLiteral("free_bytes")] = m_freeBytes;
    if (m_loudness && !m_loudness->cancelled && m_loudness->error.isEmpty())
        r[QStringLiteral("loudness")] = QJsonObject{{QStringLiteral("integrated_lufs"), m_loudness->integrated},
                                                     {QStringLiteral("true_peak_dbtp"), m_loudness->truePeak},
                                                     {QStringLiteral("range_lu"), m_loudness->range}};
    if (m_thumbTime >= 0)
        r[QStringLiteral("thumbnail_time")] = double(m_thumbTime) / kUsPerSecond;
    return r;
}

QJsonArray DeliverView::historyJson() const
{
    QJsonArray out;
    for (int i = 0; i < m_history->topLevelItemCount(); ++i) {
        const auto* item = m_history->topLevelItem(i);
        QJsonObject o{{QStringLiteral("when"), item->text(0)},
                      {QStringLiteral("file"), item->data(1, Qt::UserRole).toString()},
                      {QStringLiteral("template"), item->text(2)},
                      {QStringLiteral("length"), item->text(3)},
                      {QStringLiteral("size"), item->text(4)}};
        if (!item->toolTip(3).isEmpty())
            o[QStringLiteral("warning")] = item->toolTip(3);
        out.append(o);
    }
    return out;
}
