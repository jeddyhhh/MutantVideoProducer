#include "ui/EffectEditor.h"

#include "ui/Theme.h"
#include "ui/GraphicsEditor.h"

#include "core/FrameConverter.h"
#include "core/PanCrop.h"
#include "core/TimelineOps.h"
#include "core/VideoDecoder.h"
#include "ui/PanCropView.h"
#include "ui/PreviewWidget.h"
#include "core/MediaInfo.h"
#include <QShowEvent>
#include <numbers>

#include <QCheckBox>
#include <QColorDialog>
#include <QComboBox>
#include <QDoubleSpinBox>
#include <QFontComboBox>
#include <QPlainTextEdit>
#include <QFormLayout>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QMenu>
#include <QPainter>
#include <QPushButton>
#include <QScrollArea>
#include <QSlider>
#include <QToolButton>
#include <QVBoxLayout>
#include <algorithm>
#include <cmath>
#include <map>

namespace {
constexpr int kSliderSteps = 1000;

// Slider position <-> value; logarithmic for zoom-like parameters.
double sliderToValue(const ParamDef& d, int pos)
{
    const double f = double(pos) / kSliderSteps;
    if (d.logScale && d.min > 0)
        return d.min * std::pow(d.max / d.min, f);
    return d.min + (d.max - d.min) * f;
}

int valueToSlider(const ParamDef& d, double v)
{
    double f;
    if (d.logScale && d.min > 0)
        f = std::log(std::max(v, d.min) / d.min) / std::log(d.max / d.min);
    else
        f = (v - d.min) / std::max(1e-9, d.max - d.min);
    return int(std::lround(std::clamp(f, 0.0, 1.0) * kSliderSteps));
}

QString colorStyle(uint32_t rgb)
{
    return QStringLiteral("background-color: #%1; min-width: 60px; border: 1px solid #222;").arg(rgb & 0xffffff, 6, 16, QLatin1Char('0'));
}
} // namespace

// Handles on the preview for what the editor shows: a position to drag
// (a title's place, an iris's or gradient's centre), an angle knob (wipes,
// the clock's start, a gradient), a size on the wheel; for Pan/Crop, the
// picture itself is dragged and the wheel zooms.
class PropertyOverlay : public PreviewOverlay {
public:
    explicit PropertyOverlay(EffectEditor* editor) : m_e(editor) {}
    QString xId, yId, angleId, sizeId;
    bool panCrop = false;

    bool any() const { return !xId.isEmpty() || !angleId.isEmpty() || !sizeId.isEmpty() || panCrop; }

    QPointF position(const QRectF& pic) const
    {
        if (xId.isEmpty())
            return pic.center();
        return QPointF(pic.x() + m_e->paramNow(xId) * pic.width(), pic.y() + m_e->paramNow(yId) * pic.height());
    }
    // Unit vector of the angle on screen.
    QPointF direction() const
    {
        const double a = m_e->paramNow(angleId) * std::numbers::pi / 180.0;
        if (angleId == QLatin1String("start")) // the clock: 0 at 12 o'clock, clockwise
            return QPointF(std::sin(a), -std::cos(a));
        return QPointF(std::cos(a), std::sin(a));
    }
    QPointF knob(const QRectF& pic) const
    {
        return position(pic) + direction() * (std::min(pic.width(), pic.height()) * 0.18);
    }

    void paint(QPainter& p, const QRectF& pic) override
    {
        p.save();
        p.setRenderHint(QPainter::Antialiasing);
        auto ring = [&](QPointF c, double r) {
            p.setBrush(Qt::NoBrush);
            p.setPen(QPen(QColor(0, 0, 0, 160), 4));
            p.drawEllipse(c, r, r);
            p.setPen(QPen(QColor(255, 210, 60), 2));
            p.drawEllipse(c, r, r);
        };
        if (!xId.isEmpty() && !panCrop) {
            const QPointF c = position(pic);
            ring(c, 9);
            p.drawLine(c - QPointF(14, 0), c - QPointF(5, 0));
            p.drawLine(c + QPointF(5, 0), c + QPointF(14, 0));
            p.drawLine(c - QPointF(0, 14), c - QPointF(0, 5));
            p.drawLine(c + QPointF(0, 5), c + QPointF(0, 14));
        }
        if (!angleId.isEmpty()) {
            const QPointF from = position(pic), to = knob(pic);
            p.setPen(QPen(QColor(0, 0, 0, 160), 4));
            p.drawLine(from, to);
            p.setPen(QPen(QColor(255, 210, 60), 2, Qt::DashLine));
            p.drawLine(from, to);
            p.setBrush(QColor(255, 210, 60));
            p.setPen(QPen(QColor(0, 0, 0, 160), 1.5));
            p.drawEllipse(to, 6, 6);
        }
        QStringList hint;
        if (panCrop)
            hint << QObject::tr("drag to pan") << QObject::tr("scroll to zoom");
        else if (!xId.isEmpty())
            hint << QObject::tr("drag to move");
        if (!angleId.isEmpty())
            hint << QObject::tr("drag the dot to turn");
        if (!sizeId.isEmpty() && !panCrop)
            hint << QObject::tr("scroll to resize");
        if (!hint.isEmpty()) {
            const QString text = hint.join(QStringLiteral("  ·  "));
            QFont f = p.font();
            f.setPointSizeF(f.pointSizeF() * 0.9);
            p.setFont(f);
            QRectF box = QRectF(p.fontMetrics().boundingRect(text)).adjusted(-6, -3, 6, 3);
            box.moveBottomLeft(QPointF(pic.left() + 8, pic.bottom() - 8));
            p.setPen(Qt::NoPen);
            p.setBrush(QColor(0, 0, 0, 150));
            p.drawRoundedRect(box, 3, 3);
            p.setPen(QColor(255, 225, 120));
            p.drawText(box, Qt::AlignCenter, text);
        }
        p.restore();
    }
    bool press(QPointF pos, const QRectF& pic, Qt::KeyboardModifiers) override
    {
        m_from = pos;
        m_drag = None;
        if (!angleId.isEmpty() && QLineF(pos, knob(pic)).length() < 14)
            m_drag = Angle;
        else if (pic.contains(pos) && (panCrop || !xId.isEmpty()))
            m_drag = Move;
        if (m_drag == None)
            return false;
        m_startX = xId.isEmpty() ? 0 : m_e->paramNow(xId);
        m_startY = yId.isEmpty() ? 0 : m_e->paramNow(yId);
        return true;
    }
    void move(QPointF pos, const QRectF& pic) override
    {
        if (m_drag == Angle) {
            const QPointF d = pos - position(pic);
            double deg = angleId == QLatin1String("start") ? std::atan2(d.x(), -d.y()) : std::atan2(d.y(), d.x());
            deg *= 180.0 / std::numbers::pi;
            set(angleId, deg);
        } else if (m_drag == Move) {
            const QPointF d((pos.x() - m_from.x()) / pic.width(), (pos.y() - m_from.y()) / pic.height());
            if (panCrop) {
                // The picture follows the mouse: the centre of what shows moves the other way, by the framing's scale.
                const QSizeF scale = m_e->panCropScale();
                set(QStringLiteral("centerX"), m_startX - d.x() / std::max(1e-6, scale.width()));
                set(QStringLiteral("centerY"), m_startY - d.y() / std::max(1e-6, scale.height()));
            } else {
                set(xId, m_startX + d.x());
                set(yId, m_startY + d.y());
            }
        }
    }
    void release() override
    {
        m_drag = None;
        m_e->flushEdit(); // one undo step per drag
    }
    Qt::CursorShape cursor(QPointF pos, const QRectF& pic) const override
    {
        if (!angleId.isEmpty() && QLineF(pos, knob(pic)).length() < 14)
            return Qt::PointingHandCursor;
        if (pic.contains(pos) && panCrop)
            return Qt::OpenHandCursor;
        if (pic.contains(pos) && !xId.isEmpty())
            return Qt::SizeAllCursor;
        return Qt::ArrowCursor;
    }
    bool wheel(QPointF, const QRectF&, double steps) override
    {
        if (sizeId.isEmpty() || steps == 0)
            return false;
        set(sizeId, m_e->paramNow(sizeId) * std::pow(1.1, steps));
        m_e->m_commitTimer.start(); // a run of notches is one undo step
        return true;
    }

private:
    void set(const QString& id, double v)
    {
        if (const ParamDef* d = m_e->paramDef(id))
            m_e->setParam(id, std::clamp(v, d->min, d->max));
        if (m_e->m_preview)
            m_e->m_preview->update();
    }
    enum Drag { None, Move, Angle };
    EffectEditor* m_e;
    Drag m_drag = None;
    QPointF m_from;
    double m_startX = 0, m_startY = 0;
};

void EffectEditor::updateOverlay()
{
    if (!m_overlay)
        m_overlay = std::make_unique<PropertyOverlay>(this);
    PropertyOverlay& o = *m_overlay;
    o.xId.clear();
    o.yId.clear();
    o.angleId.clear();
    o.sizeId.clear();
    o.panCrop = isPanCrop() && targetExists();
    if (targetExists()) {
        auto has = [&](const char* id) { return paramDef(QString::fromLatin1(id)) != nullptr; };
        if (o.panCrop) {
            o.xId = QStringLiteral("centerX");
            o.yId = QStringLiteral("centerY");
            o.sizeId = QStringLiteral("zoom");
        } else {
            if (has("x") && has("y")) {
                o.xId = QStringLiteral("x");
                o.yId = QStringLiteral("y");
            } else if (has("centerX") && has("centerY")) {
                o.xId = QStringLiteral("centerX");
                o.yId = QStringLiteral("centerY");
            }
            if (has("angle"))
                o.angleId = QStringLiteral("angle");
            else if (has("start") && isTransition())
                o.angleId = QStringLiteral("start");
            if (has("size") && isGenerator())
                o.sizeId = QStringLiteral("size");
        }
    }
    if (!m_preview)
        return;
    PreviewOverlay* mine = m_overlay.get();
    if (o.any() && isVisible())
        m_preview->setOverlay(mine);
    else if (m_preview->overlay() == mine)
        m_preview->setOverlay(nullptr);
}

QStringList EffectEditor::handles() const
{
    if (!m_overlay || !m_preview || m_preview->overlay() != m_overlay.get())
        return {};
    return {m_overlay->xId, m_overlay->yId, m_overlay->angleId, m_overlay->sizeId};
}

const ParamDef* EffectEditor::paramDef(const QString& id) const
{
    const std::vector<ParamDef>* defs = currentDefs();
    if (!defs)
        return nullptr;
    for (const ParamDef& d : *defs)
        if (d.id == id)
            return &d;
    return nullptr;
}

double EffectEditor::paramNow(const QString& id) const
{
    const ParamDef* d = paramDef(id);
    if (!d)
        return 0;
    if (const ParamMap* params = currentParams()) {
        const auto it = params->find(id);
        if (it != params->end())
            return it->second.at(keyTime(), d->type);
    }
    return d->def;
}

QSizeF EffectEditor::panCropScale() const
{
    // How much of the output one whole source width / height covers (Pan/Crop drags).
    const Clip* c = m_project->timeline().findClip(m_target.id);
    const ProjectSettings& ps = m_project->settings();
    QSizeF src(ps.width, ps.height);
    if (c && !c->isGenerated())
        if (auto m = m_project->mediaItem(c->mediaId))
            if (const VideoStreamInfo* vs = m->info.videoStream(c->streamIndex))
                src = QSizeF(vs->displaySize());
    const int mode = int(paramNow(QStringLiteral("mode")));
    double sx = ps.width / std::max(1.0, src.width()), sy = ps.height / std::max(1.0, src.height());
    if (mode == 0)
        sx = sy = std::min(sx, sy);
    else if (mode == 1)
        sx = sy = std::max(sx, sy);
    const double zoom = std::max(0.001, paramNow(QStringLiteral("zoom")));
    return QSizeF(sx * zoom * src.width() / ps.width, sy * zoom * src.height() / ps.height);
}

EffectEditor::EffectEditor(Project* project, std::function<TimeUs()> cursor, QWidget* parent)
    : QDialog(parent, Qt::Tool), m_project(project), m_cursor(std::move(cursor))
{
    setWindowTitle(tr("Properties"));
    resize(900, 480);
    auto* layout = new QVBoxLayout(this);

    m_title = new QLabel;
    QFont tf = m_title->font();
    tf.setBold(true);
    tf.setPointSizeF(tf.pointSizeF() * 1.15);
    m_title->setFont(tf);
    layout->addWidget(m_title);

    // What it is on the left (type, description, the pan/crop view), its settings on the right.
    auto* body = new QHBoxLayout;
    body->setContentsMargins(0, 0, 0, 0);
    layout->addLayout(body, 1);
    auto* left = new QWidget;
    auto* rl = new QVBoxLayout(left);
    rl->setContentsMargins(0, 0, 0, 0);
    left->setMinimumWidth(260);
    body->addWidget(left, 2);
    auto* right = new QWidget;
    auto* params = new QVBoxLayout(right);
    params->setContentsMargins(0, 0, 0, 0);
    body->addWidget(right, 3);
    m_transitionType = new QComboBox;
    m_transitionType->addItem(tr("None (hard cut): remove the transition"), QStringLiteral("__none__"));
    m_transitionType->addItem(tr("Crossfade (default)"), QString());
    for (const TransitionDef& t : transitions())
        if (t.id != QLatin1String("dissolve"))
            m_transitionType->addItem(t.name, t.id);
    m_removeTransition = new QPushButton(tr("Remove Transition"));
    m_removeTransition->setToolTip(tr("No transition here: the event starts / ends with a hard cut. "
                                      "Where it overlaps its neighbour, the earlier event is cut where this one starts."));
    {
        auto* row = new QHBoxLayout;
        row->addWidget(m_transitionType, 1);
        row->addWidget(m_removeTransition);
        rl->addLayout(row);
    }
    connect(m_removeTransition, &QPushButton::clicked, this, [this] {
        const bool in = m_target.kind == FxTarget::Kind::TransitionIn;
        const int clip = m_target.id;
        editNow([&](Timeline& tl) { ops::removeTransition(tl, clip, in); }, tr("Remove Transition"));
        if (m_embedded) { // nothing left to edit
            setTarget({});
            emit closed();
        } else {
            hide();
        }
    });
    m_description = new QLabel;
    m_description->setWordWrap(true);
    theme::style(m_description, QStringLiteral("color: @dim;"));
    rl->addWidget(m_description);
    m_panCropView = new PanCropView;
    m_panCropView->setMinimumHeight(150);
    rl->addWidget(m_panCropView, 2);
    m_resetButton = new QPushButton(tr("Reset to default framing"));
    rl->addWidget(m_resetButton);
    rl->addStretch(0);
    connect(m_resetButton, &QPushButton::clicked, this, [this] {
        editNow([&](Timeline& tl) {
            if (Clip* c = tl.findClip(m_target.id))
                c->panCrop.clear();
        }, tr("Reset Pan/Crop"));
        refreshValues();
    });
    connect(m_panCropView, &PanCropView::centerChanged, this, [this](double x, double y) {
        setParam(QStringLiteral("centerX"), x);
        setParam(QStringLiteral("centerY"), y);
    });
    connect(m_panCropView, &PanCropView::zoomChanged, this, [this](double z) { setParam(QStringLiteral("zoom"), z); });
    m_scroll = new QScrollArea;
    m_scroll->setWidgetResizable(true);
    m_scroll->setFrameShape(QFrame::NoFrame);
    m_scroll->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    params->addWidget(m_scroll, 1);
    auto* hint = new QLabel(tr("◆ adds/removes a keyframe at the cursor (right-click for the curve); "
                               "◀ ▶ jump between keyframes. Once a parameter has keyframes, changing it "
                               "sets a keyframe at the cursor. On the preview: drag to move, scroll to resize."));
    hint->setWordWrap(true);
    theme::style(hint, QStringLiteral("color: @faint; font-size: 8pt;"));
    params->addWidget(hint);

    m_commitTimer.setSingleShot(true);
    m_commitTimer.setInterval(700);
    connect(&m_commitTimer, &QTimer::timeout, this, &EffectEditor::flushEdit);

    connect(m_transitionType, qOverload<int>(&QComboBox::currentIndexChanged), this, [this] {
        if (m_updatingUi)
            return;
        const QString type = m_transitionType->currentData().toString();
        if (type == QLatin1String("__none__")) {
            m_removeTransition->click();
            return;
        }
        editNow([&](Timeline& tl) {
            if (TransitionSpec* t = transition(tl)) {
                const TransitionDef* def = findTransition(type);
                *t = def ? makeTransition(*def) : TransitionSpec{};
            }
        }, tr("Change Transition"));
        rebuildParams();
    });

    connect(m_project, &Project::changed, this, [this] {
        if (m_applying || !isVisible())
            return;
        // Changed elsewhere (undo, timeline edit): drop any pending burst.
        m_commitTimer.stop();
        m_editBefore.reset();
        rebuild();
    });
    rebuild();
}

EffectEditor::~EffectEditor() = default;

// ---------------------------------------------------------------------------
// Target access
// ---------------------------------------------------------------------------

bool EffectEditor::isTransition() const
{
    return m_target.kind == FxTarget::Kind::TransitionIn || m_target.kind == FxTarget::Kind::TransitionOut;
}

TransitionSpec* EffectEditor::transition(Timeline& tl) const
{
    if (!isTransition())
        return nullptr;
    Clip* c = tl.findClip(m_target.id);
    if (!c)
        return nullptr;
    return m_target.kind == FxTarget::Kind::TransitionIn ? &c->transitionIn : &c->transitionOut;
}

const TransitionSpec* EffectEditor::transition(const Timeline& tl) const
{
    return const_cast<EffectEditor*>(this)->transition(const_cast<Timeline&>(tl));
}

bool EffectEditor::targetExists() const
{
    const Timeline& tl = m_project->timeline();
    if (isGenerator()) {
        const Clip* c = tl.findClip(m_target.id);
        return c && c->isGenerated();
    }
    if (isPanCrop()) {
        int ti = -1;
        const Clip* c = tl.findClip(m_target.id, &ti);
        return c && isVisualTrack(tl.tracks[ti].type);
    }
    return isTransition() && transition(tl) != nullptr;
}

const std::vector<ParamDef>* EffectEditor::currentDefs() const
{
    const Timeline& tl = m_project->timeline();
    if (isPanCrop())
        return &panCropParams();
    if (isGenerator()) {
        const Clip* c = tl.findClip(m_target.id);
        const GeneratorDef* def = c ? findGenerator(c->generator.type) : nullptr;
        return def ? &def->params : nullptr;
    }
    if (isTransition()) {
        const TransitionSpec* t = transition(tl);
        const TransitionDef* def = t ? findTransition(t->type) : nullptr;
        return def ? &def->params : nullptr;
    }
    return nullptr;
}

const ParamMap* EffectEditor::currentParams() const
{
    return currentParams(const_cast<Timeline&>(m_project->timeline()));
}

ParamMap* EffectEditor::currentParams(Timeline& tl) const
{
    if (isGenerator()) {
        Clip* c = tl.findClip(m_target.id);
        return c ? &c->generator.params : nullptr;
    }
    if (isPanCrop()) {
        Clip* c = tl.findClip(m_target.id);
        return c ? &c->panCrop : nullptr;
    }
    TransitionSpec* t = transition(tl);
    return t ? &t->params : nullptr;
}

TimeUs EffectEditor::keyTime() const
{
    const TimeUs cursor = m_cursor();
    if (isEventLocal())
        if (const Clip* c = m_project->timeline().findClip(m_target.id))
            return std::clamp<TimeUs>(cursor - c->start, 0, c->length);
    return cursor;
}

TimeUs EffectEditor::keyToTimeline(TimeUs key) const
{
    if (isEventLocal())
        if (const Clip* c = m_project->timeline().findClip(m_target.id))
            return c->start + key;
    return key;
}

TimeUs EffectEditor::keyTolerance() const
{
    return m_project->settings().fps.frameDuration() / 2;
}

// ---------------------------------------------------------------------------
// Public API
// ---------------------------------------------------------------------------

void EffectEditor::setTarget(const FxTarget& target)
{
    flushEdit();
    m_target = target.isChain() ? FxTarget{} : target;
    rebuild();
    if (m_embedded) {
        if (m_target.kind != FxTarget::Kind::None)
            emit opened();
        return;
    }
    show();
    raise();
    activateWindow();
}

void EffectEditor::setEmbedded(bool embedded)
{
    m_embedded = embedded;
    if (embedded)
        setWindowFlags(Qt::Widget);
}

void EffectEditor::reject()
{
    if (!m_embedded) // (Esc in the panel must not hide it)
        QDialog::reject();
}

void EffectEditor::showEvent(QShowEvent* e)
{
    QDialog::showEvent(e);
    refreshValues();
    updateOverlay();
}

void EffectEditor::setPreview(PreviewWidget* preview)
{
    m_preview = preview;
    updateOverlay();
}

void EffectEditor::onCursorMoved()
{
    if (isVisible())
        refreshValues();
}

void EffectEditor::hideEvent(QHideEvent* e)
{
    flushEdit();
    if (m_preview && m_overlay && m_preview->overlay() == m_overlay.get())
        m_preview->setOverlay(nullptr);
    QDialog::hideEvent(e);
}

// ---------------------------------------------------------------------------
// Editing
// ---------------------------------------------------------------------------

void EffectEditor::edit(const std::function<void(Timeline&)>& fn, const QString& label)
{
    if (!m_editBefore) {
        m_editBefore = m_project->timeline();
        m_editLabel = label;
    }
    Timeline tl = m_project->timeline();
    fn(tl);
    m_applying = true;
    m_project->setTimelineLive(tl);
    m_applying = false;
    m_commitTimer.start();
}

void EffectEditor::editNow(const std::function<void(Timeline&)>& fn, const QString& label)
{
    flushEdit();
    Timeline tl = m_project->timeline();
    fn(tl);
    m_applying = true;
    m_project->applyEdit(tl, label);
    m_applying = false;
}

void EffectEditor::flushEdit()
{
    m_commitTimer.stop();
    if (!m_editBefore)
        return;
    const Timeline before = std::move(*m_editBefore);
    m_editBefore.reset();
    m_applying = true;
    m_project->commitEdit(before, m_editLabel);
    m_applying = false;
}

void EffectEditor::setParam(const QString& id, double value)
{
    const TimeUs t = keyTime();
    const TimeUs tol = keyTolerance();
    edit([&](Timeline& tl) {
        ParamMap* params = currentParams(tl);
        if (!params)
            return;
        ParamValue& pv = (*params)[id];
        if (pv.keys.empty()) {
            pv.value = value;
            return;
        }
        // Animated: update the keyframe at the cursor, or create one.
        for (auto& k : pv.keys)
            if (std::llabs(k.time - t) <= tol) {
                k.value = value;
                return;
            }
        pv.addKey(t, value);
    }, tr("Change Effect Setting"));
    refreshValues();
}

void EffectEditor::setParamText(const QString& id, const QString& text)
{
    edit([&](Timeline& tl) {
        if (ParamMap* params = currentParams(tl))
            (*params)[id].text = text;
    }, isGenerator() ? tr("Edit Text") : tr("Change Effect Setting"));
}

void EffectEditor::toggleKey(const QString& id)
{
    const TimeUs t = keyTime();
    const TimeUs tol = keyTolerance();
    const std::vector<ParamDef>* defs = currentDefs();
    if (!defs)
        return;

    auto def = std::find_if(defs->begin(), defs->end(), [&](const ParamDef& d) { return d.id == id; });
    if (def == defs->end())
        return;
    const ParamType type = def->type;
    const double fallback = def->def;
    editNow([&](Timeline& tl) {
        ParamMap* params = currentParams(tl);
        if (!params)
            return;
        auto it = params->find(id);
        ParamValue pv = it != params->end() ? it->second : ParamValue{fallback, {}, {}};
        const double current = pv.at(t, type);
        if (pv.hasKeyAt(t, tol)) {
            pv.removeKeyAt(t, tol);
            if (pv.keys.empty())
                pv.value = current; // back to a static value
        } else {
            pv.addKey(t, current);
        }
        (*params)[id] = pv;
    }, tr("Keyframe"));
    refreshValues();
}

void EffectEditor::setKeyInterp(const QString& id, Keyframe::Interp interp)
{
    const TimeUs t = keyTime();
    const TimeUs tol = keyTolerance();
    editNow([&](Timeline& tl) {
        if (ParamMap* params = currentParams(tl)) {
            ParamValue& pv = (*params)[id];
            keyops::Times at;
            for (const auto& k : pv.keys)
                if (std::llabs(k.time - t) <= tol)
                    at.insert(k.time);
            keyops::setInterp(pv, at, interp);
        }
    }, tr("Keyframe Curve"));
}

void EffectEditor::jumpKey(const QString& id, int direction)
{
    const ParamMap* params = currentParams();
    if (!params)
        return;
    auto it = params->find(id);
    if (it == params->end() || it->second.keys.empty())
        return;
    const TimeUs t = keyTime();
    const TimeUs tol = keyTolerance();
    std::optional<TimeUs> target;
    for (const auto& k : it->second.keys) {
        if (direction > 0 && k.time > t + tol && (!target || k.time < *target))
            target = k.time;
        if (direction < 0 && k.time < t - tol && (!target || k.time > *target))
            target = k.time;
    }
    if (target)
        emit seekRequested(keyToTimeline(*target));
}

// ---------------------------------------------------------------------------
// UI
// ---------------------------------------------------------------------------

void EffectEditor::rebuild()
{
    m_updatingUi = true;
    const Timeline& tl = m_project->timeline();
    QString title;
    switch (m_target.kind) {
    case FxTarget::Kind::TransitionIn:
    case FxTarget::Kind::TransitionOut:
        if (const Clip* c = tl.findClip(m_target.id)) {
            const bool in = m_target.kind == FxTarget::Kind::TransitionIn;
            title = (in ? tr("Transition into %1") : tr("Transition out of %1")).arg(c->name);
        }
        break;
    case FxTarget::Kind::Generator:
        if (const Clip* c = tl.findClip(m_target.id); c && c->isGenerated())
            if (const GeneratorDef* def = findGenerator(c->generator.type))
                title = tr("Generated Media: %1").arg(def->name);
        break;
    case FxTarget::Kind::PanCrop:
        if (const Clip* c = tl.findClip(m_target.id))
            title = tr("Event Pan/Crop: %1").arg(c->isGenerated() ? generatorLabel(c->generator) : c->name);
        break;
    default: break;
    }
    const bool exists = targetExists();
    if (!exists)
        title = tr("Nothing to edit.");
    m_title->setText(title);

    const bool trans = isTransition();
    m_panCropView->setVisible(isPanCrop() && exists);
    m_resetButton->setVisible(isPanCrop() && exists);
    m_transitionType->setVisible(trans && exists);
    m_removeTransition->setVisible(trans && exists);

    if (trans && exists) {
        const TransitionSpec* t = transition(tl);
        const int idx = m_transitionType->findData(t->isPlainFade() ? QString() : t->type);
        m_transitionType->setCurrentIndex(std::max(0, idx));
    }
    m_updatingUi = false;
    rebuildParams();
    updateOverlay();
}

void EffectEditor::rebuildParams()
{
    m_rows.clear();
    m_form = new QWidget;
    auto* form = new QFormLayout(m_form);
    form->setFieldGrowthPolicy(QFormLayout::AllNonFixedFieldsGrow);
    m_scroll->setWidget(m_form); // deletes the previous form

    const std::vector<ParamDef>* defs = targetExists() ? currentDefs() : nullptr;
    QString desc;
    if (isTransition()) {
        const TransitionSpec* t = transition(m_project->timeline());
        const TransitionDef* def = t ? findTransition(t->isPlainFade() ? QStringLiteral("dissolve") : t->type) : nullptr;
        if (def)
            desc = def->description;
        if (const Clip* c = m_project->timeline().findClip(m_target.id)) {
            const TimeUs fade = m_target.kind == FxTarget::Kind::TransitionIn ? c->fadeIn : c->fadeOut;
            desc += tr("\nLength comes from the event's fade (%1) or its overlap with the neighbouring event; "
                       "drag the fade handle on the timeline to change it. Remove Transition (or the \u00D7 on its label "
                       "on the timeline) takes it away.").arg(formatDuration(fade));
        }
    } else if (isPanCrop()) {
        desc = tr("Frames the event in the output: drag the rectangle to pan, drag a corner or scroll to zoom. "
                  "Keyframe zoom and center for a Ken Burns move.");
    } else if (isGenerator()) {
        if (const Clip* c = m_project->timeline().findClip(m_target.id))
            if (const GeneratorDef* def = findGenerator(c->generator.type))
                desc = def->description;
    }
    m_description->setText(desc);
    if (!defs)
        return;

    if(isGenerator())if(const Clip* c=m_project->timeline().findClip(m_target.id)) {
        const int id=c->id;
        if(c->generator.type=="graphics_overlay") {auto* button=new QPushButton(tr("Compose Graphics…"));button->setObjectName("composeGraphicsButton");form->addRow(button);connect(button,&QPushButton::clicked,this,[this,id]{composeGraphics(this,m_project,id);});}
        if(c->generator.type=="audio_waveform") {auto* button=new QPushButton(tr("Choose Audio Source / Rebuild Waveform…"));button->setObjectName("waveformSourceButton");form->addRow(button);connect(button,&QPushButton::clicked,this,[this,id]{chooseWaveformAudio(this,m_project,id);});}
    }

    for (const ParamDef& d : *defs) {
        Row row;
        row.id = d.id;
        row.type = d.type;
        row.min = d.min;
        row.max = d.max;
        auto* line = new QWidget;
        auto* hl = new QHBoxLayout(line);
        hl->setContentsMargins(0, 0, 0, 0);
        const QString id = d.id;
        switch (d.type) {
        case ParamType::Number: {
            row.slider = new QSlider(Qt::Horizontal);
            row.slider->setRange(0, kSliderSteps);
            row.slider->setMinimumWidth(80);
            row.spin = new QDoubleSpinBox;
            row.spin->setRange(d.min, d.max);
            row.spin->setDecimals(d.decimals);
            row.spin->setSingleStep(std::pow(10.0, -d.decimals) * (d.decimals == 0 ? 1 : 10));
            row.spin->setSuffix(d.suffix);
            row.spin->setKeyboardTracking(false);
            row.spin->setFixedWidth(96);
            hl->addWidget(row.slider, 1);
            hl->addWidget(row.spin);
            QDoubleSpinBox* spin = row.spin;
            QSlider* slider = row.slider;
            connect(row.slider, &QSlider::valueChanged, this, [this, id, d, spin](int v) {
                if (m_updatingUi)
                    return;
                const double value = sliderToValue(d, v);
                m_updatingUi = true;
                spin->setValue(value);
                m_updatingUi = false;
                setParam(id, value);
            });
            connect(row.spin, qOverload<double>(&QDoubleSpinBox::valueChanged), this, [this, id, d, slider](double v) {
                if (m_updatingUi)
                    return;
                m_updatingUi = true;
                slider->setValue(valueToSlider(d, v));
                m_updatingUi = false;
                setParam(id, v);
            });
            break;
        }
        case ParamType::Choice:
            row.combo = new QComboBox;
            row.combo->addItems(d.choices);
            hl->addWidget(row.combo, 1);
            connect(row.combo, qOverload<int>(&QComboBox::currentIndexChanged), this, [this, id](int i) {
                if (!m_updatingUi)
                    setParam(id, i);
            });
            break;
        case ParamType::Bool:
            row.check = new QCheckBox;
            hl->addWidget(row.check, 1);
            connect(row.check, &QCheckBox::toggled, this, [this, id](bool on) {
                if (!m_updatingUi)
                    setParam(id, on ? 1.0 : 0.0);
            });
            break;
        case ParamType::Color: {
            row.colorButton = new QPushButton;
            hl->addWidget(row.colorButton);
            hl->addStretch();
            QPushButton* btn = row.colorButton;
            connect(row.colorButton, &QPushButton::clicked, this, [this, id, btn] {
                const QColor initial(btn->property("rgb").toUInt() | 0xff000000u);
                const QColor c = QColorDialog::getColor(initial, this, tr("Choose Color"));
                if (c.isValid())
                    setParam(id, double(c.rgb() & 0xffffff));
            });
            break;
        }
        case ParamType::Text: {
            if (d.multiline) {
                row.multiText = new QPlainTextEdit;
                row.multiText->setToolTip(d.tooltip);
                row.multiText->setFixedHeight(90);
                row.multiText->setTabChangesFocus(true);
                hl->addWidget(row.multiText, 1);
                QPlainTextEdit* edit = row.multiText;
                connect(row.multiText, &QPlainTextEdit::textChanged, this, [this, id, edit] {
                    if (!m_updatingUi)
                        setParamText(id, edit->toPlainText());
                });
            } else {
                row.text = new QLineEdit;
                row.text->setToolTip(d.tooltip);
                hl->addWidget(row.text, 1);
                connect(row.text, &QLineEdit::textEdited, this, [this, id](const QString& t) {
                    if (!m_updatingUi)
                        setParamText(id, t);
                });
            }
            break;
        }
        case ParamType::Font: {
            row.font = new QFontComboBox;
            hl->addWidget(row.font, 1);
            connect(row.font, &QFontComboBox::currentFontChanged, this, [this, id](const QFont& f) {
                if (!m_updatingUi)
                    setParamText(id, f.family());
            });
            break;
        }
        }
        if (d.animatable() && !isTransition() && m_target.kind != FxTarget::Kind::None) {
            row.prevKey = new QToolButton;
            row.prevKey->setText(QStringLiteral("◀"));
            row.prevKey->setToolTip(tr("Previous keyframe"));
            row.keyButton = new QToolButton;
            row.keyButton->setToolTip(tr("Add / remove keyframe at the cursor (right-click: curve)"));
            row.keyButton->setContextMenuPolicy(Qt::CustomContextMenu);
            row.nextKey = new QToolButton;
            row.nextKey->setText(QStringLiteral("▶"));
            row.nextKey->setToolTip(tr("Next keyframe"));
            for (QToolButton* b : {row.prevKey, row.keyButton, row.nextKey})
                b->setFixedWidth(22);
            hl->addWidget(row.prevKey);
            hl->addWidget(row.keyButton);
            hl->addWidget(row.nextKey);
            connect(row.keyButton, &QToolButton::clicked, this, [this, id] { toggleKey(id); });
            connect(row.prevKey, &QToolButton::clicked, this, [this, id] { jumpKey(id, -1); });
            connect(row.nextKey, &QToolButton::clicked, this, [this, id] { jumpKey(id, 1); });
            QToolButton* kb = row.keyButton;
            connect(row.keyButton, &QToolButton::customContextMenuRequested, this, [this, id, kb](const QPoint& pos) {
                QMenu menu;
                for (int i = 0; i < kKeyInterps; ++i)
                    menu.addAction(keyInterpName(Keyframe::Interp(i)), this, [this, id, i] { setKeyInterp(id, Keyframe::Interp(i)); });
                menu.addSeparator();
                menu.addAction(tr("Edit in Keyframe Editor..."), this, [this, id] {
                    keyops::Address a;
                    a.source = isPanCrop() ? keyops::Address::Source::PanCrop : keyops::Address::Source::Generator;
                    a.owner = m_target.id;
                    a.param = id;
                    emit keyframeEditorRequested(a);
                });
                menu.exec(kb->mapToGlobal(pos));
            });
        }
        form->addRow(d.name, line);
        m_rows.push_back(row);
    }
    refreshValues();
}

void EffectEditor::updatePanCropView()
{
    const Clip* c = m_project->timeline().findClip(m_target.id);
    if (!c)
        return;
    const ProjectSettings& ps = m_project->settings();
    const TimeUs local = keyTime();
    m_panCropView->setFraming(resolveParams(panCropParams(), c->panCrop, local), QSizeF(ps.width, ps.height));
    if (c->isGenerated()) {
        const QSize size(480, std::max(1, 480 * ps.height / std::max(1, ps.width)));
        QImage img(size, QImage::Format_ARGB32_Premultiplied);
        img.fill(QColor(0x2a, 0x22, 0x3a));
        QPainter(&img).drawImage(0, 0, renderGenerator(c->generator, size, 480.0 / std::max(1, ps.width), local, c->length, ps.fps));
        m_panCropView->setPicture(img, QSizeF(ps.width, ps.height));
        return;
    }
    auto media = m_project->mediaItem(c->mediaId);
    const VideoStreamInfo* vs = media ? media->info.videoStream(c->streamIndex) : nullptr;
    if (!vs)
        return;
    if (!m_pcDecoder || m_pcPath != media->path || m_pcStream != c->streamIndex) {
        m_pcDecoder = std::make_unique<VideoDecoder>();
        m_pcConverter = std::make_unique<FrameConverter>();
        m_pcPath = media->path;
        m_pcStream = c->streamIndex;
        if (!m_pcDecoder->open(media->path, c->streamIndex))
            return;
    }
    const AVFrame* frame = m_pcDecoder->frameAt(media->info.isStill ? 0 : c->sourceTimeAt(c->start + local));
    if (!frame)
        return;
    const QSize disp = vs->displaySize();
    const bool sideways = vs->rotation == 90 || vs->rotation == 270;
    const QSize upright = sideways ? QSize(disp.height(), disp.width()) : disp;
    const double k = std::min(1.0, 640.0 / std::max(upright.width(), upright.height()));
    QImage img = m_pcConverter->convert(frame, QSize(std::max(1, int(upright.width() * k)), std::max(1, int(upright.height() * k))), false);
    if (vs->rotation)
        img = img.transformed(QTransform().rotate(vs->rotation));
    m_panCropView->setPicture(img, QSizeF(disp));
}

void EffectEditor::refreshValues()
{
    if (isPanCrop() && targetExists())
        updatePanCropView();
    const std::vector<ParamDef>* defs = targetExists() ? currentDefs() : nullptr;
    const ParamMap* params = defs ? currentParams() : nullptr;
    if (!defs || !params || defs->size() != m_rows.size())
        return;
    m_updatingUi = true;
    const TimeUs t = keyTime();
    const TimeUs tol = keyTolerance();
    for (size_t i = 0; i < m_rows.size(); ++i) {
        const ParamDef& d = (*defs)[i];
        Row& row = m_rows[i];
        auto it = params->find(d.id);
        const ParamValue pv = it != params->end() ? it->second : ParamValue{d.def, d.defText, {}};
        const double v = pv.at(t, d.type);
        if (row.slider) {
            row.slider->setValue(valueToSlider(d, v));
            row.spin->setValue(v);
        }
        if (row.combo)
            row.combo->setCurrentIndex(int(std::lround(v)));
        if (row.check)
            row.check->setChecked(v >= 0.5);
        if (row.colorButton) {
            row.colorButton->setProperty("rgb", uint32_t(v));
            row.colorButton->setStyleSheet(colorStyle(uint32_t(v)));
        }
        if (row.text && !row.text->hasFocus())
            row.text->setText(pv.text);
        if (row.multiText && !row.multiText->hasFocus() && row.multiText->toPlainText() != pv.text)
            row.multiText->setPlainText(pv.text);
        if (row.font && row.font->currentFont().family() != pv.text)
            row.font->setCurrentFont(QFont(pv.text));
        if (row.keyButton) {
            const bool animated = !pv.keys.empty();
            const bool here = pv.hasKeyAt(t, tol);
            row.keyButton->setText(here ? QStringLiteral("◆") : QStringLiteral("◇"));
            theme::style(row.keyButton, animated ? QStringLiteral("color: @key;") : QString());
            row.prevKey->setEnabled(animated);
            row.nextKey->setEnabled(animated);
        }
    }
    m_updatingUi = false;
}
