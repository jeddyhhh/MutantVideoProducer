#include "ui/KeyframeEditor.h"

#include "core/Generators.h"
#include "ui/Theme.h"

#include <QApplication>
#include <QClipboard>
#include <QContextMenuEvent>
#include <QDoubleSpinBox>
#include <QHBoxLayout>
#include <QInputDialog>
#include <QJsonDocument>
#include <QLabel>
#include <QMenu>
#include <QMimeData>
#include <QMouseEvent>
#include <QPainter>
#include <QPainterPath>
#include <QToolButton>
#include <QToolTip>
#include <QUndoStack>
#include <QVBoxLayout>
#include <QWheelEvent>
#include <algorithm>
#include <cmath>
#include <limits>

using Interp = Keyframe::Interp;

namespace {

constexpr int kListW = 250;  // setting names, values and ◀ ◇ ▶
constexpr int kRulerH = 26;
constexpr int kRowH = 22;
constexpr const char* kMime = "application/x-mvp-keyframes";

// Distinct curve colours, in graph order.
const QColor kCurveColors[] = {QColor(0xff6b6b), QColor(0x4dabf7), QColor(0x69db7c), QColor(0xffd43b),
                               QColor(0xda77f2), QColor(0x38d9a9), QColor(0xff922b), QColor(0xa5d8ff)};

QString shortTimecode(TimeUs t, const FrameRate& fps)
{
    QString s = formatTimecode(std::max<TimeUs>(0, t), fps);
    return s.startsWith(QLatin1String("00:")) ? s.mid(3) : s;
}

} // namespace

// The painted part: forwards everything to the editor.
class KeyframeCanvas : public QWidget {
public:
    explicit KeyframeCanvas(KeyframeEditor* editor) : QWidget(editor), m_editor(editor)
    {
        setObjectName(QStringLiteral("keyframeCanvas"));
        setFocusPolicy(Qt::StrongFocus);
        setMouseTracking(true);
        setMinimumHeight(120);
        setAttribute(Qt::WA_OpaquePaintEvent);
    }

protected:
    void paintEvent(QPaintEvent*) override
    {
        QPainter p(this);
        p.setRenderHint(QPainter::Antialiasing);
        m_editor->paint(p);
    }
    void mousePressEvent(QMouseEvent* e) override { m_editor->mousePress(e); }
    void mouseMoveEvent(QMouseEvent* e) override { m_editor->mouseMove(e); }
    void mouseReleaseEvent(QMouseEvent* e) override { m_editor->mouseRelease(e); }
    void mouseDoubleClickEvent(QMouseEvent* e) override { m_editor->mouseDouble(e); }
    void wheelEvent(QWheelEvent* e) override { m_editor->wheel(e); }
    void keyPressEvent(QKeyEvent* e) override { m_editor->keyPress(e); }
    void contextMenuEvent(QContextMenuEvent* e) override { m_editor->contextMenu(e->pos(), e->globalPos()); }
    void resizeEvent(QResizeEvent*) override
    {
        if (!m_editor->m_viewSet)
            m_editor->fit();
    }

private:
    KeyframeEditor* m_editor;
};

// ---------------------------------------------------------------------------
// Construction and target
// ---------------------------------------------------------------------------

KeyframeEditor::KeyframeEditor(Project* project, std::function<TimeUs()> cursor, QWidget* parent)
    : QWidget(parent), m_project(project), m_cursor(std::move(cursor))
{
    setObjectName(QStringLiteral("keyframeEditor"));
    auto* top = new QHBoxLayout;
    top->setContentsMargins(4, 2, 4, 2);
    top->setSpacing(2);
    auto button = [&](const QString& text, const QString& tip, bool checkable = false) {
        auto* b = new QToolButton;
        b->setText(text);
        b->setToolTip(tip);
        b->setCheckable(checkable);
        b->setAutoRaise(true);
        top->addWidget(b);
        return b;
    };
    m_dopeButton = button(tr("Dope Sheet"), tr("Keyframes as marks, one row per setting (G switches)"), true);
    m_graphButton = button(tr("Graph"), tr("The curves, with values and Bezier handles to drag (G switches)"), true);
    m_dopeButton->setChecked(true);
    m_graphButton->setObjectName(QStringLiteral("keyGraphMode"));
    connect(m_dopeButton, &QToolButton::clicked, this, [this] { m_graph = false; m_dopeButton->setChecked(true); m_graphButton->setChecked(false); m_canvas->update(); });
    connect(m_graphButton, &QToolButton::clicked, this, [this] { m_graph = true; m_graphButton->setChecked(true); m_dopeButton->setChecked(false); fit(); });
    m_animatedButton = button(tr("Animated Only"), tr("Only settings that have keyframes (off: every setting, to add keyframes to)"), true);
    m_animatedButton->setChecked(true);
    connect(m_animatedButton, &QToolButton::toggled, this, [this](bool on) { m_animatedOnly = on; rebuildRows(); m_canvas->update(); });
    top->addSpacing(8);
    for (int i = 0; i < kKeyInterps; ++i) {
        const auto interp = Interp(i);
        QToolButton* b = button(QString(), tr("%1 (%2): the curve from the picked keyframes to the next").arg(keyInterpName(interp)).arg(i + 1));
        b->setObjectName(QStringLiteral("keyCurve_") + keyInterpId(interp));
        connect(b, &QToolButton::clicked, this, [this, interp] { setInterp(interp); });
        m_curveButtons.push_back(b);
    }
    m_smoothButton = button(tr("Smooth"), tr("Smooth handles: Bezier curves through the picked keyframes, flat at peaks (no overshoot)"));
    connect(m_smoothButton, &QToolButton::clicked, this, [this] { smoothHandles(); });
    top->addSpacing(8);
    top->addWidget(new QLabel(tr("Time")));
    m_timeSpin = new QDoubleSpinBox;
    m_timeSpin->setObjectName(QStringLiteral("keyTime"));
    m_timeSpin->setDecimals(3);
    m_timeSpin->setRange(0, 1e6);
    m_timeSpin->setSuffix(QStringLiteral(" s"));
    m_timeSpin->setKeyboardTracking(false);
    m_timeSpin->setToolTip(tr("Timeline time of the picked keyframe"));
    top->addWidget(m_timeSpin);
    connect(m_timeSpin, &QDoubleSpinBox::editingFinished, this, [this] { setPickedTime(m_timeSpin->value()); });
    top->addWidget(new QLabel(tr("Value")));
    m_valueSpin = new QDoubleSpinBox;
    m_valueSpin->setObjectName(QStringLiteral("keyValue"));
    m_valueSpin->setKeyboardTracking(false);
    m_valueSpin->setRange(-1e9, 1e9);
    m_valueSpin->setToolTip(tr("Value of the picked keyframes"));
    top->addWidget(m_valueSpin);
    connect(m_valueSpin, &QDoubleSpinBox::editingFinished, this, [this] { setPickedValue(m_valueSpin->value()); });
    top->addSpacing(8);
    m_snapButton = button(tr("Snap"), tr("Moved keyframes snap to the cursor, other keyframes and the event's ends (always to frames)"), true);
    m_snapButton->setChecked(true);
    connect(m_snapButton, &QToolButton::toggled, this, [this](bool on) { m_snap = on; });
    QToolButton* fitButton = button(tr("Fit"), tr("Show all the keyframes (F)"));
    connect(fitButton, &QToolButton::clicked, this, [this] { fit(); });
    top->addStretch(1);
    m_title = new QLabel;
    theme::style(m_title, QStringLiteral("color: @dim;"));
    top->addWidget(m_title);

    m_canvas = new KeyframeCanvas(this);
    auto* layout = new QVBoxLayout(this);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(0);
    layout->addLayout(top);
    layout->addWidget(m_canvas, 1);
    updateIcons();

    connect(m_project, &Project::changed, this, [this] {
        if (!m_applying)
            refresh();
        else
            m_canvas->update();
    });
    refresh();
}

KeyframeEditor::~KeyframeEditor() = default;

void KeyframeEditor::setTarget(const FxTarget& target)
{
    using K = FxTarget::Kind;
    FxTarget t = target;
    if (t.kind == K::TransitionIn || t.kind == K::TransitionOut || t.kind == K::Generator || t.kind == K::PanCrop)
        t.kind = K::Clip; // all of an event's settings show together
    if (t.kind != K::Clip && t.kind != K::Track && t.kind != K::Output && t.kind != K::AudioMaster)
        t = {};
    if (t == m_target)
        return;
    m_target = t;
    m_picked.clear();
    m_focus.clear();
    m_shown.clear();
    m_scroll = 0;
    m_viewSet = false;
    refresh();
    fit();
}

void KeyframeEditor::focusSetting(const keyops::Address& address)
{
    using S = keyops::Address::Source;
    switch (address.source) {
    case S::TrackFx: setTarget({FxTarget::Kind::Track, address.owner}); break;
    case S::OutputFx: setTarget({FxTarget::Kind::Output, 0}); break;
    case S::MasterFx: setTarget({FxTarget::Kind::AudioMaster, 0}); break;
    default: setTarget({FxTarget::Kind::Clip, address.owner}); break;
    }
    const QString key = address.key();
    m_shown.insert(key);
    m_focus = {key};
    refresh();
    const int ch = channelIndex(key);
    if (ch < 0)
        return;
    m_collapsed.erase(m_channels[size_t(ch)].group);
    rebuildRows();
    m_picked.clear();
    if (const ParamValue* p = values(ch))
        for (const Keyframe& k : p->keys)
            m_picked[key].insert(k.time);
    for (size_t r = 0; r < m_rows.size(); ++r)
        if (m_rows[r].channel == ch) {
            const int y = int(r) * kRowH;
            const int visible = m_canvas->height() - kRulerH;
            if (y < m_scroll || y + kRowH > m_scroll + visible)
                m_scroll = std::max(0, y - visible / 3);
        }
    updateFields();
    m_canvas->update();
    m_canvas->setFocus();
}

void KeyframeEditor::onCursorMoved()
{
    m_canvas->update();
}

void KeyframeEditor::changeEvent(QEvent* e)
{
    QWidget::changeEvent(e);
    if (e->type() == QEvent::PaletteChange || e->type() == QEvent::StyleChange)
        updateIcons();
}

// ---------------------------------------------------------------------------
// Model
// ---------------------------------------------------------------------------

void KeyframeEditor::refresh()
{
    const Timeline& tl = m_project->timeline();
    using K = FxTarget::Kind;
    m_channels.clear();
    QString title;
    switch (m_target.kind) {
    case K::Clip:
        if (const Clip* c = tl.findClip(m_target.id)) {
            m_channels = keyops::channels(tl, keyops::Scope::Clip, m_target.id);
            QString name = c->name;
            if (name.isEmpty() && c->isGenerated())
                name = generatorLabel(c->generator);
            if (name.isEmpty())
                if (const auto m = m_project->mediaItem(c->mediaId))
                    name = m->displayName();
            title = tr("Event: %1").arg(name);
        }
        break;
    case K::Track:
        for (const auto& t : tl.tracks)
            if (t.id == m_target.id) {
                m_channels = keyops::channels(tl, keyops::Scope::Track, t.id);
                title = tr("Track FX: %1").arg(t.name);
            }
        break;
    case K::Output:
        m_channels = keyops::channels(tl, keyops::Scope::Output, 0);
        title = tr("Video Output FX");
        break;
    case K::AudioMaster:
        m_channels = keyops::channels(tl, keyops::Scope::AudioMaster, 0);
        title = tr("Audio Master FX");
        break;
    default: break;
    }
    m_title->setText(title);
    rebuildRows();
    prunePicked();
    updateFields();
    m_canvas->update();
}

void KeyframeEditor::rebuildRows()
{
    m_rows.clear();
    QString group;
    for (size_t i = 0; i < m_channels.size(); ++i) {
        const keyops::Channel& ch = m_channels[i];
        const ParamValue* p = values(int(i));
        const bool animated = p && !p->keys.empty();
        if (m_animatedOnly && !animated && !m_shown.count(ch.address.key()))
            continue;
        if (ch.group != group) {
            group = ch.group;
            m_rows.push_back({true, group, -1});
        }
        if (!m_collapsed.count(group))
            m_rows.push_back({false, group, int(i)});
    }
}

const keyops::Channel* KeyframeEditor::channel(int index) const
{
    return index >= 0 && index < int(m_channels.size()) ? &m_channels[size_t(index)] : nullptr;
}

int KeyframeEditor::channelIndex(const QString& key) const
{
    for (size_t i = 0; i < m_channels.size(); ++i)
        if (m_channels[i].address.key() == key)
            return int(i);
    return -1;
}

const ParamValue* KeyframeEditor::values(int index) const
{
    const keyops::Channel* ch = channel(index);
    return ch ? keyops::find(m_project->timeline(), *ch) : nullptr;
}

std::vector<int> KeyframeEditor::graphChannels() const
{
    std::vector<int> out;
    for (const Row& r : m_rows) {
        if (r.header || m_channels[size_t(r.channel)].def.type != ParamType::Number)
            continue;
        const QString key = m_channels[size_t(r.channel)].address.key();
        const ParamValue* p = values(r.channel);
        if (m_focus.empty() ? (p && !p->keys.empty()) : m_focus.count(key) > 0)
            out.push_back(r.channel);
    }
    return out;
}

std::pair<TimeUs, TimeUs> KeyframeEditor::span() const
{
    const Timeline& tl = m_project->timeline();
    if (m_target.kind == FxTarget::Kind::Clip)
        if (const Clip* c = tl.findClip(m_target.id))
            return {c->start, c->start + c->length};
    return {0, std::max<TimeUs>(tl.duration(), 10 * kUsPerSecond)};
}

TimeUs KeyframeEditor::snapFrame(TimeUs t) const
{
    return m_project->settings().fps.snap(t);
}

TimeUs KeyframeEditor::channelSnap(const keyops::Channel& ch, TimeUs key) const
{
    const Timeline& tl = m_project->timeline();
    return keyops::fromTimeline(tl, ch, snapFrame(keyops::toTimeline(tl, ch, key)));
}

double KeyframeEditor::normalize(const keyops::Channel& ch, double v) const
{
    const double range = ch.def.max - ch.def.min;
    return range > 0 ? (v - ch.def.min) / range : 0.5;
}

double KeyframeEditor::denormalize(const keyops::Channel& ch, double n) const
{
    return ch.def.min + n * (ch.def.max - ch.def.min);
}

QString KeyframeEditor::formatValue(const keyops::Channel& ch, double v) const
{
    if (ch.def.type == ParamType::Color)
        return QStringLiteral("#%1").arg(uint32_t(v) & 0xffffff, 6, 16, QLatin1Char('0'));
    return QString::number(v, 'f', std::clamp(ch.def.decimals, 0, 6)) + ch.def.suffix;
}

QColor KeyframeEditor::channelColor(int index) const
{
    const std::vector<int> g = graphChannels();
    const auto it = std::find(g.begin(), g.end(), index);
    if (it == g.end())
        return theme::c().dim;
    return kCurveColors[size_t(it - g.begin()) % std::size(kCurveColors)];
}

int KeyframeEditor::pickedCount() const
{
    int n = 0;
    for (const auto& [key, times] : m_picked)
        n += int(times.size());
    return n;
}

void KeyframeEditor::prunePicked()
{
    for (auto it = m_picked.begin(); it != m_picked.end();) {
        const ParamValue* p = values(channelIndex(it->first));
        Times kept;
        if (p)
            for (const Keyframe& k : p->keys)
                if (it->second.count(k.time))
                    kept.insert(k.time);
        if (kept.empty())
            it = m_picked.erase(it);
        else
            it->second = std::move(kept), ++it;
    }
}

// ---------------------------------------------------------------------------
// Edits
// ---------------------------------------------------------------------------

void KeyframeEditor::change(const QString& label, const std::function<void(Timeline&)>& fn)
{
    Timeline tl = m_project->timeline();
    fn(tl);
    if (tl == m_project->timeline())
        return;
    m_applying = true;
    m_project->applyEdit(tl, label);
    m_applying = false;
    refresh();
}

void KeyframeEditor::forPicked(Timeline& tl, const std::function<void(const keyops::Channel&, ParamValue&, const Times&)>& fn)
{
    for (const auto& [key, times] : m_picked) {
        const keyops::Channel* ch = channel(channelIndex(key));
        if (ParamValue* p = ch ? keyops::find(tl, *ch) : nullptr)
            fn(*ch, *p, times);
    }
}

void KeyframeEditor::setInterp(Interp interp)
{
    change(tr("Keyframe Curve: %1").arg(keyInterpName(interp)), [&](Timeline& tl) {
        forPicked(tl, [&](const keyops::Channel&, ParamValue& p, const Times& t) { keyops::setInterp(p, t, interp); });
    });
}

void KeyframeEditor::smoothHandles()
{
    change(tr("Smooth Keyframe Handles"), [&](Timeline& tl) {
        forPicked(tl, [&](const keyops::Channel&, ParamValue& p, const Times& t) { keyops::autoTangents(p, t); });
    });
}

void KeyframeEditor::deletePicked()
{
    const TimeUs cursor = m_cursor();
    change(tr("Delete Keyframes"), [&](Timeline& tl) {
        forPicked(tl, [&](const keyops::Channel& ch, ParamValue& p, const Times& t) {
            keyops::deleteKeys(p, t, keyops::fromTimeline(tl, ch, cursor), ch.def.type);
        });
    });
    m_picked.clear();
    updateFields();
}

void KeyframeEditor::reversePicked()
{
    std::map<QString, Times> next;
    change(tr("Reverse Keyframes"), [&](Timeline& tl) {
        forPicked(tl, [&](const keyops::Channel& ch, ParamValue& p, const Times& t) { next[ch.address.key()] = keyops::reverseKeys(p, t); });
    });
    if (!next.empty())
        m_picked = next;
    m_canvas->update();
}

void KeyframeEditor::nudgeTime(int frames)
{
    const TimeUs step = m_project->settings().fps.frameDuration() * frames;
    const Timeline& now = m_project->timeline();
    TimeUs delta = step;
    for (const auto& [key, times] : m_picked) // keep every keyframe inside its range
        if (const keyops::Channel* ch = channel(channelIndex(key)); ch && !times.empty()) {
            const auto [lo, hi] = keyops::keyRange(now, *ch);
            delta = std::clamp(delta, lo - *times.begin(), hi - *times.rbegin());
        }
    if (!delta)
        return;
    std::map<QString, Times> next;
    change(tr("Nudge Keyframes"), [&](Timeline& tl) {
        forPicked(tl, [&](const keyops::Channel& ch, ParamValue& p, const Times& t) { next[ch.address.key()] = keyops::moveKeys(p, t, delta); });
    });
    m_picked = next;
    updateFields();
    m_canvas->update();
}

void KeyframeEditor::nudgeValue(double fraction)
{
    change(tr("Nudge Keyframe Values"), [&](Timeline& tl) {
        forPicked(tl, [&](const keyops::Channel& ch, ParamValue& p, const Times& t) {
            if (ch.def.type != ParamType::Number)
                return;
            for (Keyframe& k : p.keys)
                if (t.count(k.time))
                    k.value = std::clamp(k.value + fraction * (ch.def.max - ch.def.min), ch.def.min, ch.def.max);
        });
    });
}

void KeyframeEditor::setPickedTime(double seconds)
{
    if (pickedCount() != 1)
        return;
    const auto [key, times] = *m_picked.begin();
    const keyops::Channel* ch = channel(channelIndex(key));
    if (!ch)
        return;
    const Timeline& now = m_project->timeline();
    const auto [lo, hi] = keyops::keyRange(now, *ch);
    const TimeUs target = std::clamp(keyops::fromTimeline(now, *ch, snapFrame(TimeUs(std::llround(seconds * kUsPerSecond)))), lo, hi);
    const TimeUs delta = target - *times.begin();
    if (!delta)
        return;
    Times next;
    change(tr("Move Keyframe"), [&](Timeline& tl) {
        if (ParamValue* p = keyops::find(tl, *ch))
            next = keyops::moveKeys(*p, times, delta);
    });
    m_picked = {{key, next}};
    updateFields();
    m_canvas->update();
}

void KeyframeEditor::setPickedValue(double value)
{
    change(tr("Keyframe Value"), [&](Timeline& tl) {
        forPicked(tl, [&](const keyops::Channel& ch, ParamValue& p, const Times& t) {
            if (ch.def.type == ParamType::Number)
                keyops::setValue(p, t, value, ch.def);
        });
    });
}

void KeyframeEditor::toggleKeyAtCursor(int index)
{
    const keyops::Channel* ch = channel(index);
    if (!ch)
        return;
    const Timeline& now = m_project->timeline();
    const auto [lo, hi] = keyops::keyRange(now, *ch);
    const TimeUs t = std::clamp(keyops::fromTimeline(now, *ch, snapFrame(m_cursor())), lo, hi);
    const TimeUs tol = m_project->settings().fps.frameDuration() / 2;
    const ParamValue* p = values(index);
    const bool has = p && p->hasKeyAt(t, tol);
    const keyops::Channel c = *ch;
    m_shown.insert(c.address.key());
    change(has ? tr("Remove Keyframe") : tr("Add Keyframe"), [&](Timeline& tl) {
        ParamValue* v = keyops::find(tl, c, true);
        if (!v)
            return;
        if (has) {
            Times at;
            for (const Keyframe& k : v->keys)
                if (std::llabs(k.time - t) <= tol)
                    at.insert(k.time);
            keyops::deleteKeys(*v, at, t, c.def.type);
        } else {
            keyops::insertKey(*v, t, c.def.type);
        }
    });
}

void KeyframeEditor::addKeyAt(int index, TimeUs timelineTime)
{
    const keyops::Channel* ch = channel(index);
    if (!ch)
        return;
    const keyops::Channel c = *ch;
    const Timeline& now = m_project->timeline();
    const auto [lo, hi] = keyops::keyRange(now, c);
    const TimeUs t = std::clamp(keyops::fromTimeline(now, c, snapFrame(timelineTime)), lo, hi);
    m_shown.insert(c.address.key());
    change(tr("Add Keyframe"), [&](Timeline& tl) {
        if (ParamValue* v = keyops::find(tl, c, true))
            keyops::insertKey(*v, t, c.def.type);
    });
    m_picked = {{c.address.key(), {t}}};
    updateFields();
    m_canvas->update();
}

void KeyframeEditor::jumpKey(int index, int direction)
{
    const keyops::Channel* ch = channel(index);
    const ParamValue* p = values(index);
    if (!ch || !p)
        return;
    const Timeline& tl = m_project->timeline();
    const TimeUs now = keyops::fromTimeline(tl, *ch, m_cursor());
    const TimeUs tol = m_project->settings().fps.frameDuration() / 2;
    std::optional<TimeUs> best;
    for (const Keyframe& k : p->keys)
        if (direction < 0 ? k.time < now - tol : k.time > now + tol)
            if (!best || (direction < 0 ? k.time > *best : k.time < *best))
                best = k.time;
    if (best)
        emit seekRequested(keyops::toTimeline(tl, *ch, *best));
}

void KeyframeEditor::copyPicked(bool cut)
{
    std::vector<std::pair<keyops::Channel, Times>> list;
    for (const auto& [key, times] : m_picked)
        if (const keyops::Channel* ch = channel(channelIndex(key)))
            list.emplace_back(*ch, times);
    if (list.empty())
        return;
    auto* mime = new QMimeData;
    const QByteArray json = QJsonDocument(keyops::copyKeys(m_project->timeline(), list)).toJson(QJsonDocument::Compact);
    mime->setData(QString::fromLatin1(kMime), json);
    mime->setText(QString::fromUtf8(json));
    QApplication::clipboard()->setMimeData(mime);
    if (cut)
        deletePicked();
}

void KeyframeEditor::paste(TimeUs at, int fallbackChannel)
{
    const QMimeData* mime = QApplication::clipboard()->mimeData();
    if (!mime || !mime->hasFormat(QString::fromLatin1(kMime)))
        return;
    const QJsonObject copied = QJsonDocument::fromJson(mime->data(QString::fromLatin1(kMime))).object();
    const keyops::Channel* fallback = channel(fallbackChannel);
    if (!fallback && m_focus.size() == 1)
        fallback = channel(channelIndex(*m_focus.begin()));
    std::optional<keyops::Channel> fb;
    if (fallback)
        fb = *fallback;
    const std::vector<keyops::Channel> into = m_channels;
    int pasted = 0;
    change(tr("Paste Keyframes"), [&](Timeline& tl) { pasted = keyops::pasteKeys(tl, copied, into, fb ? &*fb : nullptr, snapFrame(at)); });
    for (const auto& item : copied[QStringLiteral("channels")].toArray()) // show what was pasted into
        m_shown.insert(item.toObject()[QStringLiteral("address")].toString());
    if (pasted)
        rebuildRows();
    m_canvas->update();
}

void KeyframeEditor::selectAll()
{
    m_picked.clear();
    for (const Row& r : m_rows)
        if (!r.header)
            if (const ParamValue* p = values(r.channel))
                for (const Keyframe& k : p->keys)
                    m_picked[m_channels[size_t(r.channel)].address.key()].insert(k.time);
    updateFields();
    m_canvas->update();
}

void KeyframeEditor::fit()
{
    const Timeline& tl = m_project->timeline();
    auto [lo, hi] = span();
    bool any = false;
    TimeUs kLo = std::numeric_limits<TimeUs>::max(), kHi = std::numeric_limits<TimeUs>::min();
    for (const Row& r : m_rows)
        if (!r.header)
            if (const ParamValue* p = values(r.channel); p && !p->keys.empty()) {
                any = true;
                kLo = std::min(kLo, keyops::toTimeline(tl, m_channels[size_t(r.channel)], p->keys.front().time));
                kHi = std::max(kHi, keyops::toTimeline(tl, m_channels[size_t(r.channel)], p->keys.back().time));
            }
    if (m_target.kind != FxTarget::Kind::Clip && any) // tracks and the output: around their keyframes
        lo = kLo, hi = std::max(kHi, kLo + kUsPerSecond);
    const TimeUs margin = std::max<TimeUs>((hi - lo) / 20, m_project->settings().fps.frameDuration() * 2);
    const int width = std::max(100, m_canvas->width() - kListW);
    m_t0 = lo - margin;
    m_usPerPx = std::max(50.0, double(hi - lo + 2 * margin) / width);
    if (m_graph) {
        double vLo = 0, vHi = 1;
        bool first = true;
        for (int index : graphChannels()) {
            const keyops::Channel& ch = m_channels[size_t(index)];
            const ParamValue* p = values(index);
            if (!p || p->keys.empty())
                continue;
            const TimeUs a = p->keys.front().time, b = p->keys.back().time;
            auto include = [&](double n) { vLo = first ? n : std::min(vLo, n), vHi = first ? n : std::max(vHi, n), first = false; };
            for (int s = 0; s <= 200; ++s)
                include(normalize(ch, p->at(a + (b - a) * s / 200, ch.def.type)));
            for (size_t i = 0; i + 1 < p->keys.size(); ++i) // Bezier handles stay in view
                if (p->keys[i].interp == Interp::Bezier)
                    include(normalize(ch, p->keys[i].value + p->keys[i].y1)), include(normalize(ch, p->keys[i + 1].value + p->keys[i].y2));
        }
        if (vHi - vLo < 0.05)
            vLo -= 0.05, vHi += 0.05;
        const double m = (vHi - vLo) * 0.08;
        m_v0 = vLo - m, m_v1 = vHi + m;
    }
    m_viewSet = m_canvas->width() > kListW + 50;
    m_canvas->update();
}

void KeyframeEditor::updateFields()
{
    const Timeline& tl = m_project->timeline();
    const int n = pickedCount();
    QSignalBlocker b1(m_timeSpin), b2(m_valueSpin);
    m_timeSpin->setEnabled(n == 1);
    std::optional<double> value;
    const keyops::Channel* only = nullptr;
    bool oneChannel = true;
    for (const auto& [key, times] : m_picked) {
        const keyops::Channel* ch = channel(channelIndex(key));
        const ParamValue* p = ch ? keyops::find(tl, *ch) : nullptr;
        if (!p || ch->def.type != ParamType::Number)
            continue;
        oneChannel &= !only || only == ch;
        only = ch;
        for (const Keyframe& k : p->keys)
            if (times.count(k.time)) {
                if (!value)
                    value = k.value;
                if (n == 1)
                    m_timeSpin->setValue(double(keyops::toTimeline(tl, *ch, k.time)) / kUsPerSecond);
            }
    }
    m_valueSpin->setEnabled(value.has_value());
    if (only && oneChannel) {
        m_valueSpin->setRange(only->def.min, only->def.max);
        m_valueSpin->setDecimals(std::clamp(only->def.decimals, 0, 6));
        m_valueSpin->setSuffix(only->def.suffix);
    } else {
        m_valueSpin->setRange(-1e9, 1e9);
        m_valueSpin->setDecimals(3);
        m_valueSpin->setSuffix(QString());
    }
    if (value)
        m_valueSpin->setValue(*value);
    // The curve buttons show the first picked keyframe's curve.
    std::optional<Interp> current;
    for (const auto& [key, times] : m_picked)
        if (const ParamValue* p = values(channelIndex(key)); p && !current)
            for (const Keyframe& k : p->keys)
                if (times.count(k.time) && !current)
                    current = k.interp;
    for (int i = 0; i < kKeyInterps; ++i) {
        m_curveButtons[size_t(i)]->setCheckable(true);
        m_curveButtons[size_t(i)]->setChecked(current && int(*current) == i);
        m_curveButtons[size_t(i)]->setEnabled(n > 0);
    }
    m_smoothButton->setEnabled(n > 0);
}

// ---------------------------------------------------------------------------
// Geometry
// ---------------------------------------------------------------------------

QRect KeyframeEditor::lanes() const
{
    return QRect(kListW, kRulerH, std::max(1, m_canvas->width() - kListW), std::max(1, m_canvas->height() - kRulerH));
}

int KeyframeEditor::rowTop(int row) const
{
    return kRulerH + row * kRowH - m_scroll;
}

int KeyframeEditor::rowAt(int y) const
{
    if (y < kRulerH)
        return -1;
    const int r = (y - kRulerH + m_scroll) / kRowH;
    return r >= 0 && r < int(m_rows.size()) ? r : -1;
}

double KeyframeEditor::timeToX(TimeUs t) const
{
    return kListW + double(t - m_t0) / m_usPerPx;
}

TimeUs KeyframeEditor::xToTime(double x) const
{
    return m_t0 + TimeUs(std::llround((x - kListW) * m_usPerPx));
}

double KeyframeEditor::valueToY(double n) const
{
    const QRect r = lanes();
    return r.top() + 8 + (1.0 - (n - m_v0) / std::max(1e-9, m_v1 - m_v0)) * (r.height() - 16);
}

double KeyframeEditor::yToValue(double y) const
{
    const QRect r = lanes();
    return m_v0 + (1.0 - (y - r.top() - 8) / std::max(1.0, double(r.height() - 16))) * (m_v1 - m_v0);
}

QPointF KeyframeEditor::keyPoint(int index, const Keyframe& k) const
{
    const keyops::Channel& ch = m_channels[size_t(index)];
    return {timeToX(keyops::toTimeline(m_project->timeline(), ch, k.time)), valueToY(normalize(ch, k.value))};
}

std::pair<QPointF, QPointF> KeyframeEditor::handlePoints(int index, size_t i) const
{
    const keyops::Channel& ch = m_channels[size_t(index)];
    const ParamValue* p = values(index);
    const Keyframe& a = p->keys[i];
    const Keyframe& b = p->keys[i + 1];
    const Timeline& tl = m_project->timeline();
    const double dt = double(b.time - a.time);
    const QPointF out(timeToX(keyops::toTimeline(tl, ch, a.time + TimeUs(a.x1 * dt))), valueToY(normalize(ch, a.value + a.y1)));
    const QPointF in(timeToX(keyops::toTimeline(tl, ch, a.time + TimeUs(a.x2 * dt))), valueToY(normalize(ch, b.value + a.y2)));
    return {out, in};
}

bool KeyframeEditor::scaleBar(double& x0, double& x1) const
{
    if (m_graph || pickedCount() < 2)
        return false;
    const Timeline& tl = m_project->timeline();
    TimeUs lo = std::numeric_limits<TimeUs>::max(), hi = std::numeric_limits<TimeUs>::min();
    for (const auto& [key, times] : m_picked)
        if (const keyops::Channel* ch = channel(channelIndex(key)); ch && !times.empty()) {
            lo = std::min(lo, keyops::toTimeline(tl, *ch, *times.begin()));
            hi = std::max(hi, keyops::toTimeline(tl, *ch, *times.rbegin()));
        }
    if (hi <= lo)
        return false;
    x0 = timeToX(lo), x1 = timeToX(hi);
    return true;
}

KeyframeEditor::Hit KeyframeEditor::hitTest(const QPoint& pos) const
{
    Hit h;
    const Timeline& tl = m_project->timeline();
    double sx0 = 0, sx1 = 0;
    if (pos.y() < kRulerH) {
        if (scaleBar(sx0, sx1) && pos.y() >= kRulerH - 11) {
            if (std::abs(pos.x() - sx0) <= 5)
                h.kind = Hit::ScaleLeft;
            else if (std::abs(pos.x() - sx1) <= 5)
                h.kind = Hit::ScaleRight;
        }
        return h;
    }
    const int row = rowAt(pos.y());
    if (pos.x() < kListW) {
        if (row < 0)
            return h;
        h.row = row;
        const Row& r = m_rows[size_t(row)];
        if (r.header) {
            h.kind = Hit::Header;
            return h;
        }
        h.channel = r.channel;
        const int x = pos.x();
        h.kind = x >= kListW - 54 && x < kListW - 38 ? Hit::ListPrev
                 : x >= kListW - 38 && x < kListW - 20 ? Hit::ListKey
                 : x >= kListW - 20                     ? Hit::ListNext
                                                        : Hit::ListName;
        return h;
    }
    if (m_graph) {
        // Handles of picked keyframes first (they sit over curves), then keyframes.
        double best = 7.5;
        for (int index : graphChannels()) {
            const ParamValue* p = values(index);
            const QString key = m_channels[size_t(index)].address.key();
            const auto picked = m_picked.find(key);
            if (!p || picked == m_picked.end())
                continue;
            for (size_t i = 0; i < p->keys.size(); ++i) {
                const bool pickedHere = picked->second.count(p->keys[i].time) > 0;
                if (i + 1 < p->keys.size() && p->keys[i].interp == Interp::Bezier) {
                    const auto [out, in] = handlePoints(index, i);
                    if (pickedHere && QLineF(out, pos).length() < best)
                        best = QLineF(out, pos).length(), h = {Hit::HandleOut, row, index, p->keys[i].time};
                    if (picked->second.count(p->keys[i + 1].time) && QLineF(in, pos).length() < best)
                        best = QLineF(in, pos).length(), h = {Hit::HandleIn, row, index, p->keys[i + 1].time};
                }
            }
        }
        if (h.kind != Hit::None)
            return h;
        best = 8;
        for (int index : graphChannels())
            if (const ParamValue* p = values(index))
                for (const Keyframe& k : p->keys)
                    if (const double d = QLineF(keyPoint(index, k), pos).length(); d < best)
                        best = d, h = {Hit::Key, row, index, k.time};
        return h;
    }
    if (row < 0)
        return h;
    const Row& r = m_rows[size_t(row)];
    double best = 7;
    if (r.header) {
        for (const keyops::Channel& ch : m_channels)
            if (ch.group == r.group)
                if (const ParamValue* p = keyops::find(tl, ch))
                    for (const Keyframe& k : p->keys) {
                        const TimeUs t = keyops::toTimeline(tl, ch, k.time);
                        if (const double d = std::abs(timeToX(t) - pos.x()); d < best)
                            best = d, h = {Hit::Summary, row, -1, t};
                    }
        return h;
    }
    if (const ParamValue* p = values(r.channel))
        for (const Keyframe& k : p->keys)
            if (const double d = std::abs(timeToX(keyops::toTimeline(tl, m_channels[size_t(r.channel)], k.time)) - pos.x()); d < best)
                best = d, h = {Hit::Key, row, r.channel, k.time};
    return h;
}

// ---------------------------------------------------------------------------
// Painting
// ---------------------------------------------------------------------------

void KeyframeEditor::paint(QPainter& p)
{
    const auto& c = theme::c();
    p.fillRect(m_canvas->rect(), c.timelineBg);
    if (m_target.kind == FxTarget::Kind::None || m_channels.empty() || m_rows.empty()) {
        p.setPen(c.dim);
        const QString text = m_target.kind == FxTarget::Kind::None ? tr("Select an event or a track to see its keyframes.")
                             : m_channels.empty()                  ? tr("Nothing here can have keyframes: add effects in the Effects tab.")
                                                                   : tr("No keyframes yet. Turn off Animated Only to see every setting, then "
                                                                        "click ◇ or double-click a row to add one.");
        p.drawText(m_canvas->rect().adjusted(16, 0, -16, 0), Qt::AlignCenter | Qt::TextWordWrap, text);
        return;
    }
    // Outside the event: dimmed.
    const QRect lr = lanes();
    const auto [lo, hi] = span();
    p.fillRect(lr, c.trackBg);
    if (m_target.kind == FxTarget::Kind::Clip) {
        QColor shade = c.timelineBg;
        shade.setAlpha(200);
        p.fillRect(QRectF(lr.left(), lr.top(), std::max(0.0, timeToX(lo) - lr.left()), lr.height()), shade);
        p.fillRect(QRectF(timeToX(hi), lr.top(), std::max(0.0, lr.right() - timeToX(hi)), lr.height()), shade);
    }
    p.save();
    p.setClipRect(lr);
    if (m_graph)
        paintGraph(p);
    else
        paintDope(p);
    p.restore();
    paintList(p);
    paintRuler(p);
    // The cursor.
    const double cx = timeToX(m_cursor());
    if (cx >= kListW) {
        p.setPen(QPen(c.playhead, 1));
        p.drawLine(QPointF(cx, 0), QPointF(cx, m_canvas->height()));
    }
    if (m_drag.kind == Drag::Marquee) {
        QColor fill = c.accent;
        fill.setAlpha(40);
        p.setPen(QPen(c.accent, 1, Qt::DashLine));
        p.setBrush(fill);
        p.drawRect(QRect(m_drag.start, m_drag.last).normalized());
    }
}

void KeyframeEditor::paintRuler(QPainter& p)
{
    const auto& c = theme::c();
    const QRect r(kListW, 0, m_canvas->width() - kListW, kRulerH);
    p.fillRect(QRect(0, 0, m_canvas->width(), kRulerH), c.ruler);
    p.setPen(c.line);
    p.drawLine(0, kRulerH - 1, m_canvas->width(), kRulerH - 1);
    // Tick spacing: whole frames, then seconds, at least 70 px apart.
    const FrameRate fps = m_project->settings().fps;
    const TimeUs frame = fps.frameDuration();
    TimeUs step = frame;
    for (TimeUs s : {frame, 2 * frame, 5 * frame, 10 * frame, kUsPerSecond, 2 * kUsPerSecond, 5 * kUsPerSecond, 10 * kUsPerSecond,
                     15 * kUsPerSecond, 30 * kUsPerSecond, 60 * kUsPerSecond, 120 * kUsPerSecond, 300 * kUsPerSecond, 600 * kUsPerSecond}) {
        step = s;
        if (double(s) / m_usPerPx >= 70)
            break;
    }
    p.save();
    p.setClipRect(r);
    const TimeUs first = (xToTime(kListW) / step) * step;
    QFont f = p.font();
    f.setPointSizeF(f.pointSizeF() * 0.85);
    p.setFont(f);
    for (TimeUs t = first; timeToX(t) < m_canvas->width(); t += step) {
        const double x = timeToX(t);
        p.setPen(c.dim);
        p.drawLine(QPointF(x, kRulerH - 8), QPointF(x, kRulerH - 1));
        const double half = timeToX(t + step / 2);
        p.drawLine(QPointF(half, kRulerH - 4), QPointF(half, kRulerH - 1));
        p.setPen(c.text);
        p.drawText(QPointF(x + 3, 12), shortTimecode(t, fps));
    }
    // The picked keyframes' span: drag its ends to stretch them.
    double x0 = 0, x1 = 0;
    if (scaleBar(x0, x1)) {
        QColor bar = c.accent;
        bar.setAlpha(110);
        p.fillRect(QRectF(x0, kRulerH - 8, x1 - x0, 5), bar);
        p.setBrush(c.accent);
        p.setPen(Qt::NoPen);
        p.drawRect(QRectF(x0 - 3, kRulerH - 11, 6, 10));
        p.drawRect(QRectF(x1 - 3, kRulerH - 11, 6, 10));
    }
    p.restore();
    p.fillRect(QRect(0, 0, kListW, kRulerH), c.headerBg);
    p.setPen(c.dim);
    p.drawText(QRect(8, 0, kListW - 16, kRulerH), Qt::AlignVCenter | Qt::AlignLeft,
               m_graph ? tr("Graph · %1").arg(shortTimecode(m_cursor(), fps)) : tr("Dope Sheet · %1").arg(shortTimecode(m_cursor(), fps)));
}

void KeyframeEditor::paintList(QPainter& p)
{
    const auto& c = theme::c();
    const Timeline& tl = m_project->timeline();
    p.fillRect(QRect(0, kRulerH, kListW, m_canvas->height() - kRulerH), c.headerBg);
    p.save();
    p.setClipRect(QRect(0, kRulerH, kListW, m_canvas->height() - kRulerH));
    const TimeUs cursor = m_cursor();
    const TimeUs tol = m_project->settings().fps.frameDuration() / 2;
    for (size_t i = 0; i < m_rows.size(); ++i) {
        const int y = rowTop(int(i));
        if (y + kRowH < kRulerH || y > m_canvas->height())
            continue;
        const Row& r = m_rows[i];
        const QRect row(0, y, kListW, kRowH);
        if (r.header) {
            p.fillRect(row, c.raised);
            p.setPen(c.bright);
            QFont f = p.font();
            f.setBold(true);
            p.save();
            p.setFont(f);
            p.drawText(row.adjusted(6, 0, -4, 0), Qt::AlignVCenter | Qt::AlignLeft,
                       (m_collapsed.count(r.group) ? QStringLiteral("▸ ") : QStringLiteral("▾ ")) + r.group);
            p.restore();
            continue;
        }
        const keyops::Channel& ch = m_channels[size_t(r.channel)];
        const QString key = ch.address.key();
        const ParamValue* pv = keyops::find(tl, ch);
        if (m_focus.count(key))
            p.fillRect(row, c.headerSelected);
        if (m_graph && ch.def.type == ParamType::Number) {
            p.setPen(Qt::NoPen);
            p.setBrush(channelColor(r.channel));
            p.drawRoundedRect(QRectF(8, y + kRowH / 2 - 4, 8, 8), 2, 2);
        }
        const bool animated = pv && !pv->keys.empty();
        const double now = pv ? pv->at(keyops::fromTimeline(tl, ch, cursor), ch.def.type) : ch.def.def;
        p.setPen(animated ? c.text : c.dim);
        p.drawText(QRect(22, y, kListW - 150, kRowH), Qt::AlignVCenter | Qt::AlignLeft,
                   p.fontMetrics().elidedText(ch.def.name, Qt::ElideRight, kListW - 150));
        if (ch.def.type == ParamType::Color) {
            p.setPen(c.deep);
            p.setBrush(QColor::fromRgb(QRgb(uint32_t(now) | 0xff000000u)));
            p.drawRect(QRectF(kListW - 100, y + 5, 40, kRowH - 10));
        } else {
            p.setPen(animated ? c.key : c.dim);
            p.drawText(QRect(kListW - 128, y, 72, kRowH), Qt::AlignVCenter | Qt::AlignRight, formatValue(ch, now));
        }
        const bool here = pv && pv->hasKeyAt(keyops::fromTimeline(tl, ch, cursor), tol);
        p.setPen(c.dim);
        p.drawText(QRect(kListW - 54, y, 16, kRowH), Qt::AlignCenter, QStringLiteral("◀"));
        p.drawText(QRect(kListW - 20, y, 16, kRowH), Qt::AlignCenter, QStringLiteral("▶"));
        p.setPen(here ? c.key : c.dim);
        p.drawText(QRect(kListW - 38, y, 18, kRowH), Qt::AlignCenter, here ? QStringLiteral("◆") : QStringLiteral("◇"));
    }
    p.restore();
    p.setPen(c.line);
    p.drawLine(kListW - 1, kRulerH, kListW - 1, m_canvas->height());
}

void KeyframeEditor::drawKey(QPainter& p, QPointF at, Interp interp, bool picked, double s) const
{
    const auto& c = theme::c();
    p.setPen(QPen(picked ? c.bright : c.deep, 1));
    p.setBrush(picked ? c.accent : c.key);
    switch (interp) {
    case Interp::Hold: p.drawRect(QRectF(at.x() - s * 0.75, at.y() - s * 0.75, s * 1.5, s * 1.5)); break;
    case Interp::Smooth:
    case Interp::EaseIn:
    case Interp::EaseOut:
    case Interp::EaseInOut: p.drawEllipse(at, s * 0.85, s * 0.85); break;
    case Interp::Bounce:
    case Interp::Elastic: p.drawPolygon(QPolygonF{at + QPointF(0, -s), at + QPointF(s, s * 0.8), at + QPointF(-s, s * 0.8)}); break;
    case Interp::Bezier:
        p.drawPolygon(QPolygonF{at + QPointF(0, -s), at + QPointF(s, 0), at + QPointF(0, s), at + QPointF(-s, 0)});
        p.setBrush(picked ? c.bright : c.deep);
        p.drawEllipse(at, s * 0.3, s * 0.3);
        break;
    default: p.drawPolygon(QPolygonF{at + QPointF(0, -s), at + QPointF(s, 0), at + QPointF(0, s), at + QPointF(-s, 0)}); break;
    }
}

void KeyframeEditor::paintDope(QPainter& p)
{
    const auto& c = theme::c();
    const Timeline& tl = m_project->timeline();
    const int w = m_canvas->width();
    for (size_t i = 0; i < m_rows.size(); ++i) {
        const int y = rowTop(int(i));
        if (y + kRowH < kRulerH || y > m_canvas->height())
            continue;
        const Row& r = m_rows[i];
        p.fillRect(QRect(kListW, y, w - kListW, kRowH), r.header ? c.raised : (i % 2 ? c.trackBgAlt : c.trackBg));
        const double cy = y + kRowH / 2.0;
        if (r.header) {
            // One mark per time any of the group's settings has a keyframe.
            std::map<TimeUs, bool> marks; // -> any picked
            for (size_t ci = 0; ci < m_channels.size(); ++ci) {
                const keyops::Channel& ch = m_channels[ci];
                if (ch.group != r.group)
                    continue;
                const auto picked = m_picked.find(ch.address.key());
                if (const ParamValue* pv = keyops::find(tl, ch))
                    for (const Keyframe& k : pv->keys)
                        marks[keyops::toTimeline(tl, ch, k.time)] |= picked != m_picked.end() && picked->second.count(k.time);
            }
            for (const auto& [t, picked] : marks)
                drawKey(p, QPointF(timeToX(t), cy), Interp::Linear, picked, 4.5);
            continue;
        }
        const keyops::Channel& ch = m_channels[size_t(r.channel)];
        const ParamValue* pv = keyops::find(tl, ch);
        if (!pv)
            continue;
        const auto picked = m_picked.find(ch.address.key());
        // Where the value changes: a line to the next keyframe (dashed: it holds, then jumps).
        for (size_t k = 0; k + 1 < pv->keys.size(); ++k) {
            const Keyframe &a = pv->keys[k], &b = pv->keys[k + 1];
            if (a.value == b.value && a.interp != Interp::Bezier)
                continue;
            QColor line = c.key;
            line.setAlpha(120);
            p.setPen(QPen(line, 2, a.interp == Interp::Hold ? Qt::DashLine : Qt::SolidLine));
            p.drawLine(QPointF(timeToX(keyops::toTimeline(tl, ch, a.time)), cy), QPointF(timeToX(keyops::toTimeline(tl, ch, b.time)), cy));
        }
        for (const Keyframe& k : pv->keys)
            drawKey(p, QPointF(timeToX(keyops::toTimeline(tl, ch, k.time)), cy), k.interp,
                    picked != m_picked.end() && picked->second.count(k.time), 5.5);
    }
}

void KeyframeEditor::paintGraph(QPainter& p)
{
    const auto& c = theme::c();
    const Timeline& tl = m_project->timeline();
    const QRect lr = lanes();
    const std::vector<int> shown = graphChannels();
    // Value grid: the setting's own numbers with one curve, else percentages of each range.
    QFont f = p.font();
    f.setPointSizeF(f.pointSizeF() * 0.85);
    p.setFont(f);
    double step = 0.25;
    for (double s : {0.01, 0.02, 0.05, 0.1, 0.25, 0.5, 1.0}) {
        step = s;
        if (s / (m_v1 - m_v0) * (lr.height() - 16) >= 28)
            break;
    }
    for (double n = std::floor(m_v0 / step) * step; n <= m_v1; n += step) {
        const double y = valueToY(n);
        QColor grid = c.line;
        if (std::abs(n) < 1e-9 || std::abs(n - 1) < 1e-9)
            grid = c.dim; // the ends of the range
        p.setPen(QPen(grid, 1, Qt::DotLine));
        p.drawLine(QPointF(lr.left(), y), QPointF(lr.right(), y));
        p.setPen(c.dim);
        const QString label = shown.size() == 1 ? formatValue(m_channels[size_t(shown.front())], denormalize(m_channels[size_t(shown.front())], n))
                                                : QStringLiteral("%1%").arg(n * 100, 0, 'f', 0);
        p.drawText(QPointF(lr.left() + 4, y - 2), label);
    }
    for (int index : shown) {
        const keyops::Channel& ch = m_channels[size_t(index)];
        const ParamValue* pv = keyops::find(tl, ch);
        const QColor color = channelColor(index);
        // The curve, from the setting itself (what renders), across the visible span.
        QPainterPath path;
        const auto [lo, hi] = keyops::keyRange(tl, ch);
        bool started = false;
        for (int x = lr.left(); x <= lr.right() + 2; x += 2) {
            const TimeUs key = keyops::fromTimeline(tl, ch, xToTime(x));
            if (key < lo || key > hi)
                continue;
            const double v = pv ? pv->at(key, ch.def.type) : ch.def.def;
            const QPointF pt(x, valueToY(normalize(ch, std::clamp(v, ch.def.min, ch.def.max))));
            started ? path.lineTo(pt) : path.moveTo(pt);
            started = true;
        }
        p.setBrush(Qt::NoBrush);
        p.setPen(QPen(color, m_focus.count(ch.address.key()) ? 2.2 : 1.6));
        p.drawPath(path);
        if (!pv)
            continue;
        const auto picked = m_picked.find(ch.address.key());
        // Bezier handles of the picked keyframes.
        for (size_t i = 0; i + 1 < pv->keys.size(); ++i) {
            if (pv->keys[i].interp != Interp::Bezier || picked == m_picked.end())
                continue;
            const auto [out, in] = handlePoints(index, i);
            p.setPen(QPen(c.bright, 1));
            p.setBrush(c.bright);
            if (picked->second.count(pv->keys[i].time)) {
                p.drawLine(keyPoint(index, pv->keys[i]), out);
                p.drawEllipse(out, 3.5, 3.5);
            }
            if (picked->second.count(pv->keys[i + 1].time)) {
                p.drawLine(keyPoint(index, pv->keys[i + 1]), in);
                p.drawEllipse(in, 3.5, 3.5);
            }
        }
        for (const Keyframe& k : pv->keys)
            drawKey(p, keyPoint(index, k), k.interp, picked != m_picked.end() && picked->second.count(k.time), 5);
    }
    if (shown.empty()) {
        p.setPen(c.dim);
        p.drawText(lr, Qt::AlignCenter, tr("No curves: click a setting's name to show it, or add keyframes."));
    }
}

QIcon KeyframeEditor::curveIcon(Interp interp) const
{
    QPixmap pm(20, 20);
    pm.fill(Qt::transparent);
    QPainter p(&pm);
    p.setRenderHint(QPainter::Antialiasing);
    Keyframe a, b;
    a.interp = interp;
    b.time = 1, b.value = 1;
    if (interp == Interp::Bezier)
        a.x1 = 0.85, a.y1 = 0, a.x2 = 0.15, a.y2 = 0.3; // a custom shape: eased with a bump
    QPainterPath path;
    for (int s = 0; s <= 32; ++s) {
        const double f = s / 32.0, v = keyframeValue(a, b, f);
        const QPointF pt(3 + f * 14, 16 - (v + 0.2) / 1.5 * 13);
        s ? path.lineTo(pt) : path.moveTo(pt);
    }
    p.setPen(QPen(theme::c().bright, 1.6));
    p.drawPath(path);
    return QIcon(pm);
}

void KeyframeEditor::updateIcons()
{
    for (int i = 0; i < int(m_curveButtons.size()); ++i)
        m_curveButtons[size_t(i)]->setIcon(curveIcon(Interp(i)));
}

// ---------------------------------------------------------------------------
// Mouse and keys
// ---------------------------------------------------------------------------

void KeyframeEditor::mousePress(QMouseEvent* e)
{
    m_canvas->setFocus();
    const QPoint pos = e->position().toPoint();
    const Qt::KeyboardModifiers mods = e->modifiers();
    m_drag = {};
    m_drag.start = m_drag.last = pos;
    if (e->button() == Qt::MiddleButton) {
        m_drag.kind = Drag::Pan;
        m_drag.t0 = m_t0, m_drag.v0 = m_v0, m_drag.v1 = m_v1, m_drag.scroll = m_scroll;
        return;
    }
    const Hit h = hitTest(pos);
    const Timeline& tl = m_project->timeline();
    auto pick = [&](const QString& key, TimeUs t, bool exclusive) {
        if (exclusive)
            m_picked.clear();
        m_picked[key].insert(t);
    };
    if (e->button() == Qt::RightButton) {
        // The menu works on what was clicked: pick it unless it is already picked.
        if (h.kind == Hit::Key) {
            const QString key = m_channels[size_t(h.channel)].address.key();
            if (!(m_picked.count(key) && m_picked[key].count(h.time)))
                pick(key, h.time, true);
        }
        updateFields();
        m_canvas->update();
        return;
    }
    if (e->button() != Qt::LeftButton)
        return;
    switch (h.kind) {
    case Hit::Header:
        if (m_collapsed.count(m_rows[size_t(h.row)].group))
            m_collapsed.erase(m_rows[size_t(h.row)].group);
        else
            m_collapsed.insert(m_rows[size_t(h.row)].group);
        rebuildRows();
        m_canvas->update();
        return;
    case Hit::ListPrev: jumpKey(h.channel, -1); return;
    case Hit::ListNext: jumpKey(h.channel, 1); return;
    case Hit::ListKey: toggleKeyAtCursor(h.channel); return;
    case Hit::ListName: {
        const QString key = m_channels[size_t(h.channel)].address.key();
        if (mods & Qt::ControlModifier)
            m_focus.count(key) ? (void)m_focus.erase(key) : (void)m_focus.insert(key);
        else
            m_focus = {key};
        if (!m_graph) { // the Dope Sheet: picks the setting's keyframes too
            if (!(mods & (Qt::ShiftModifier | Qt::ControlModifier)))
                m_picked.clear();
            if (const ParamValue* p = values(h.channel))
                for (const Keyframe& k : p->keys)
                    m_picked[key].insert(k.time);
        }
        updateFields();
        m_canvas->update();
        return;
    }
    case Hit::ScaleLeft:
    case Hit::ScaleRight: {
        double x0 = 0, x1 = 0;
        scaleBar(x0, x1);
        m_drag.kind = Drag::Scale;
        m_drag.before = tl;
        m_drag.picked = m_picked;
        // (exact times: from the keyframes, not the pixels)
        TimeUs kLo = std::numeric_limits<TimeUs>::max(), kHi = std::numeric_limits<TimeUs>::min();
        for (const auto& [key, times] : m_picked)
            if (const keyops::Channel* ch = channel(channelIndex(key)); ch && !times.empty()) {
                kLo = std::min(kLo, keyops::toTimeline(tl, *ch, *times.begin()));
                kHi = std::max(kHi, keyops::toTimeline(tl, *ch, *times.rbegin()));
            }
        m_drag.pivot = h.kind == Hit::ScaleRight ? kLo : kHi;
        m_drag.end = h.kind == Hit::ScaleRight ? kHi : kLo;
        return;
    }
    case Hit::Key:
    case Hit::Summary: {
        std::vector<std::pair<QString, TimeUs>> hits; // what this click is on
        if (h.kind == Hit::Key) {
            hits.emplace_back(m_channels[size_t(h.channel)].address.key(), h.time);
        } else {
            for (const keyops::Channel& ch : m_channels)
                if (ch.group == m_rows[size_t(h.row)].group)
                    if (const ParamValue* p = keyops::find(tl, ch))
                        for (const Keyframe& k : p->keys)
                            if (keyops::toTimeline(tl, ch, k.time) == h.time)
                                hits.emplace_back(ch.address.key(), k.time);
        }
        const bool allPicked = std::all_of(hits.begin(), hits.end(), [&](const auto& kt) {
            const auto it = m_picked.find(kt.first);
            return it != m_picked.end() && it->second.count(kt.second);
        });
        if (mods & Qt::ControlModifier) {
            for (const auto& [key, t] : hits)
                allPicked ? (void)m_picked[key].erase(t) : (void)m_picked[key].insert(t);
            prunePicked();
            updateFields();
            m_canvas->update();
            if (allPicked)
                return;
        } else if (mods & Qt::ShiftModifier || allPicked) {
            for (const auto& [key, t] : hits)
                m_picked[key].insert(t);
        } else {
            m_picked.clear();
            for (const auto& [key, t] : hits)
                m_picked[key].insert(t);
        }
        m_drag.kind = Drag::Move;
        m_drag.before = tl;
        m_drag.picked = m_picked;
        m_drag.duplicate = mods & Qt::AltModifier;
        if (h.kind == Hit::Key) {
            const keyops::Channel& ch = m_channels[size_t(h.channel)];
            m_drag.grab = keyops::toTimeline(tl, ch, h.time);
            if (const ParamValue* p = values(h.channel))
                for (const Keyframe& k : p->keys)
                    if (k.time == h.time)
                        m_drag.grabValue = normalize(ch, k.value);
        } else {
            m_drag.grab = h.time;
        }
        updateFields();
        m_canvas->update();
        return;
    }
    case Hit::HandleOut:
    case Hit::HandleIn:
        m_drag.kind = Drag::Handle;
        m_drag.before = tl;
        m_drag.channel = h.channel;
        m_drag.key = h.time;
        m_drag.out = h.kind == Hit::HandleOut;
        m_drag.aligned = !(mods & Qt::AltModifier);
        return;
    default: break;
    }
    if (pos.y() < kRulerH && pos.x() >= kListW) {
        m_drag.kind = Drag::Seek;
        emit seekRequested(std::max<TimeUs>(0, snapFrame(xToTime(pos.x()))));
        return;
    }
    if (pos.x() >= kListW) {
        m_drag.kind = Drag::Marquee;
        m_drag.additive = mods & (Qt::ShiftModifier | Qt::ControlModifier);
        if (!m_drag.additive) {
            m_picked.clear();
            updateFields();
        }
        m_canvas->update();
    }
}

void KeyframeEditor::mouseMove(QMouseEvent* e)
{
    const QPoint pos = e->position().toPoint();
    if (m_drag.kind == Drag::None) {
        // Hover: what a keyframe is.
        const Hit h = hitTest(pos);
        if (h.kind == Hit::Key) {
            const keyops::Channel& ch = m_channels[size_t(h.channel)];
            if (const ParamValue* p = values(h.channel))
                for (const Keyframe& k : p->keys)
                    if (k.time == h.time)
                        QToolTip::showText(e->globalPosition().toPoint(),
                                           QStringLiteral("%1\n%2 · %3 · %4")
                                               .arg(ch.def.name, shortTimecode(keyops::toTimeline(m_project->timeline(), ch, k.time), m_project->settings().fps),
                                                    formatValue(ch, k.value), keyInterpName(k.interp)),
                                           m_canvas);
        } else {
            QToolTip::hideText();
        }
        m_canvas->setCursor(h.kind == Hit::ScaleLeft || h.kind == Hit::ScaleRight ? Qt::SizeHorCursor
                            : h.kind == Hit::Key || h.kind == Hit::Summary || h.kind == Hit::HandleIn || h.kind == Hit::HandleOut
                                ? Qt::PointingHandCursor
                                : Qt::ArrowCursor);
        return;
    }
    m_drag.last = pos;
    applyDrag(pos, e->modifiers());
}

void KeyframeEditor::mouseRelease(QMouseEvent* e)
{
    if (m_drag.kind == Drag::Marquee)
        selectInRect(QRect(m_drag.start, e->position().toPoint()).normalized(), m_drag.additive);
    finishDrag();
}

void KeyframeEditor::mouseDouble(QMouseEvent* e)
{
    const QPoint pos = e->position().toPoint();
    const Hit h = hitTest(pos);
    m_drag = {};
    if (h.kind == Hit::Key) {
        emit seekRequested(keyops::toTimeline(m_project->timeline(), m_channels[size_t(h.channel)], h.time));
        return;
    }
    if (pos.x() < kListW || pos.y() < kRulerH)
        return;
    if (!m_graph) {
        const int row = rowAt(pos.y());
        if (row >= 0 && !m_rows[size_t(row)].header)
            addKeyAt(m_rows[size_t(row)].channel, xToTime(pos.x()));
        return;
    }
    // The graph: on the curve nearest the click (or the one setting shown).
    const TimeUs t = xToTime(pos.x());
    int best = -1;
    double bestD = 14;
    const std::vector<int> shown = graphChannels();
    for (int index : shown) {
        const keyops::Channel& ch = m_channels[size_t(index)];
        const ParamValue* p = values(index);
        const double v = p ? p->at(keyops::fromTimeline(m_project->timeline(), ch, t), ch.def.type) : ch.def.def;
        if (const double d = std::abs(valueToY(normalize(ch, v)) - pos.y()); d < bestD)
            bestD = d, best = index;
    }
    if (best < 0 && shown.size() == 1)
        best = shown.front();
    if (best >= 0)
        addKeyAt(best, t);
}

void KeyframeEditor::wheel(QWheelEvent* e)
{
    const QPointF pos = e->position();
    // (Alt turns the wheel sideways on some systems: either direction counts.)
    const int delta = e->angleDelta().y() ? e->angleDelta().y() : e->angleDelta().x();
    const double steps = delta / 120.0;
    if (e->modifiers() & Qt::ControlModifier) {
        const TimeUs at = xToTime(std::max<double>(kListW, pos.x()));
        m_usPerPx = std::clamp(m_usPerPx * std::pow(1.25, -steps), 20.0, 5e6);
        m_t0 = at - TimeUs((std::max<double>(kListW, pos.x()) - kListW) * m_usPerPx);
    } else if (e->modifiers() & Qt::ShiftModifier) {
        m_t0 -= TimeUs(steps * 80 * m_usPerPx);
    } else if (m_graph && pos.x() >= kListW) {
        if (e->modifiers() & Qt::AltModifier) {
            const double at = yToValue(pos.y()), k = std::pow(1.25, -steps);
            m_v0 = at + (m_v0 - at) * k, m_v1 = at + (m_v1 - at) * k;
        } else {
            const double shift = steps * 0.1 * (m_v1 - m_v0);
            m_v0 += shift, m_v1 += shift;
        }
    } else {
        const int max = std::max(0, int(m_rows.size()) * kRowH - (m_canvas->height() - kRulerH) + kRowH);
        m_scroll = std::clamp(m_scroll - int(steps * kRowH * 2), 0, max);
    }
    m_viewSet = true;
    m_canvas->update();
    e->accept();
}

void KeyframeEditor::keyPress(QKeyEvent* e)
{
    const bool shift = e->modifiers() & Qt::ShiftModifier;
    if (e->matches(QKeySequence::Copy))
        return copyPicked(false);
    if (e->matches(QKeySequence::Cut))
        return copyPicked(true);
    if (e->matches(QKeySequence::Paste))
        return paste(m_cursor(), -1);
    if (e->matches(QKeySequence::SelectAll))
        return selectAll();
    switch (e->key()) {
    case Qt::Key_Delete:
    case Qt::Key_Backspace: deletePicked(); return;
    case Qt::Key_Left: nudgeTime(shift ? -10 : -1); return;
    case Qt::Key_Right: nudgeTime(shift ? 10 : 1); return;
    case Qt::Key_Up:
        if (m_graph)
            nudgeValue(shift ? 0.1 : 0.01);
        return;
    case Qt::Key_Down:
        if (m_graph)
            nudgeValue(shift ? -0.1 : -0.01);
        return;
    case Qt::Key_F: fit(); return;
    case Qt::Key_G: (m_graph ? m_dopeButton : m_graphButton)->click(); return;
    case Qt::Key_Escape:
        m_picked.clear();
        updateFields();
        m_canvas->update();
        return;
    case Qt::Key_K: { // keyframes at the cursor on the settings worked on
        std::set<int> targets;
        for (const auto& [key, times] : m_picked)
            targets.insert(channelIndex(key));
        for (const QString& key : m_focus)
            targets.insert(channelIndex(key));
        for (int index : targets)
            if (index >= 0)
                addKeyAt(index, m_cursor());
        return;
    }
    default: break;
    }
    if (e->key() >= Qt::Key_1 && e->key() <= Qt::Key_9 && !(e->modifiers() & Qt::ControlModifier)) {
        setInterp(Interp(e->key() - Qt::Key_1));
        return;
    }
    e->ignore();
}

void KeyframeEditor::contextMenu(const QPoint& pos, const QPoint& global)
{
    const Hit h = hitTest(pos);
    const int row = rowAt(pos.y());
    const int rowChannel = row >= 0 && !m_rows[size_t(row)].header ? m_rows[size_t(row)].channel : (h.channel >= 0 ? h.channel : -1);
    const keyops::Channel* ch = channel(rowChannel);
    QMenu menu;
    const int n = pickedCount();
    if (n > 0) {
        QMenu* curves = menu.addMenu(tr("Curve"));
        std::optional<Interp> current;
        for (const auto& [key, times] : m_picked)
            if (const ParamValue* p = values(channelIndex(key)); p && !current)
                for (const Keyframe& k : p->keys)
                    if (times.count(k.time) && !current)
                        current = k.interp;
        for (int i = 0; i < kKeyInterps; ++i) {
            QAction* a = curves->addAction(curveIcon(Interp(i)), keyInterpName(Interp(i)), this, [this, i] { setInterp(Interp(i)); });
            a->setShortcut(QKeySequence(Qt::Key_1 + i));
            a->setCheckable(true);
            a->setChecked(current && int(*current) == i);
        }
        menu.addAction(tr("Smooth Handles"), this, [this] { smoothHandles(); });
        menu.addSeparator();
        menu.addAction(tr("Copy"), this, [this] { copyPicked(false); })->setShortcut(QKeySequence::Copy);
        menu.addAction(tr("Cut"), this, [this] { copyPicked(true); })->setShortcut(QKeySequence::Cut);
    }
    if (QApplication::clipboard()->mimeData() && QApplication::clipboard()->mimeData()->hasFormat(QString::fromLatin1(kMime))) {
        menu.addAction(tr("Paste at Cursor"), this, [this, rowChannel] { paste(m_cursor(), rowChannel); })->setShortcut(QKeySequence::Paste);
        if (pos.x() >= kListW)
            menu.addAction(tr("Paste Here"), this, [this, rowChannel, t = xToTime(pos.x())] { paste(t, rowChannel); });
    }
    if (n > 0) {
        menu.addAction(tr("Delete"), this, [this] { deletePicked(); })->setShortcut(QKeySequence::Delete);
        if (n >= 2)
            menu.addAction(tr("Reverse"), this, [this] { reversePicked(); });
        bool numbers = false;
        for (const auto& [key, times] : m_picked)
            if (const keyops::Channel* c = channel(channelIndex(key)))
                numbers |= c->def.type == ParamType::Number;
        if (numbers)
            menu.addAction(tr("Set Value..."), this, [this] {
                bool ok = false;
                const double v = QInputDialog::getDouble(this, tr("Keyframe Value"), tr("Value of the picked keyframes:"), m_valueSpin->value(),
                                                         m_valueSpin->minimum(), m_valueSpin->maximum(), m_valueSpin->decimals(), &ok);
                if (ok)
                    setPickedValue(v);
            });
        if (h.kind == Hit::Key)
            menu.addAction(tr("Go to Keyframe"), this, [this, h] {
                if (const keyops::Channel* c = channel(h.channel))
                    emit seekRequested(keyops::toTimeline(m_project->timeline(), *c, h.time));
            });
    }
    if (ch) {
        menu.addSeparator();
        const QString name = ch->def.name;
        const int index = rowChannel;
        menu.addAction(tr("Add Keyframe at Cursor"), this, [this, index] { addKeyAt(index, m_cursor()); });
        if (pos.x() >= kListW && !m_graph)
            menu.addAction(tr("Add Keyframe Here"), this, [this, index, t = xToTime(pos.x())] { addKeyAt(index, t); });
        // (while the menu is open an edit, the AI's, can rebuild the channels: the index is checked again when picked)
        if (const ParamValue* p = values(index); p && !p->keys.empty()) {
            menu.addAction(tr("Pick All Keyframes of %1").arg(name), this, [this, index] {
                const ParamValue* p = values(index);
                if (!p)
                    return;
                m_picked.clear();
                for (const Keyframe& k : p->keys)
                    m_picked[m_channels[size_t(index)].address.key()].insert(k.time);
                updateFields();
                m_canvas->update();
            });
            menu.addAction(tr("Remove All Keyframes of %1").arg(name), this, [this, index] {
                const ParamValue* p = values(index);
                if (!p)
                    return;
                Times all;
                for (const Keyframe& k : p->keys)
                    all.insert(k.time);
                m_picked = {{m_channels[size_t(index)].address.key(), all}};
                deletePicked();
            });
        }
    }
    menu.addSeparator();
    menu.addAction(tr("Pick All"), this, [this] { selectAll(); })->setShortcut(QKeySequence::SelectAll);
    menu.addAction(tr("Fit"), this, [this] { fit(); })->setShortcut(QKeySequence(Qt::Key_F));
    menu.addAction(m_graph ? tr("Dope Sheet") : tr("Graph"), this, [this] { (m_graph ? m_dopeButton : m_graphButton)->click(); })->setShortcut(QKeySequence(Qt::Key_G));
    menu.exec(global);
}

void KeyframeEditor::applyDrag(const QPoint& pos, Qt::KeyboardModifiers mods)
{
    const QPoint d = pos - m_drag.start;
    switch (m_drag.kind) {
    case Drag::Seek: emit seekRequested(std::max<TimeUs>(0, snapFrame(xToTime(pos.x())))); return;
    case Drag::Marquee: m_canvas->update(); return;
    case Drag::Pan:
        m_t0 = m_drag.t0 - TimeUs(d.x() * m_usPerPx);
        if (m_graph) {
            const double dv = d.y() / std::max(1.0, double(lanes().height() - 16)) * (m_drag.v1 - m_drag.v0);
            m_v0 = m_drag.v0 + dv, m_v1 = m_drag.v1 + dv;
        } else {
            const int max = std::max(0, int(m_rows.size()) * kRowH - (m_canvas->height() - kRulerH) + kRowH);
            m_scroll = std::clamp(m_drag.scroll - d.y(), 0, max);
        }
        m_viewSet = true;
        m_canvas->update();
        return;
    default: break;
    }
    if (!m_drag.before)
        return;
    Timeline tl = *m_drag.before;
    const Timeline& before = *m_drag.before;
    std::map<QString, Times> next;
    if (m_drag.kind == Drag::Move) {
        if (std::abs(d.x()) + std::abs(d.y()) < 3 && !m_drag.changed)
            return; // a click, not a drag yet
        if (m_graph && (mods & Qt::ShiftModifier)) {
            if (!m_drag.axis && std::abs(d.x()) + std::abs(d.y()) > 4)
                m_drag.axis = std::abs(d.x()) >= std::abs(d.y()) ? 1 : 2;
        } else {
            m_drag.axis = 0;
        }
        // Time: whole frames, then the cursor, other keyframes or the event's ends nearby.
        TimeUs delta = 0;
        if (m_drag.axis != 2) {
            TimeUs target = snapFrame(m_drag.grab + TimeUs(d.x() * m_usPerPx));
            if (m_snap) {
                std::vector<TimeUs> candidates{m_cursor(), span().first, span().second};
                for (const Row& r : m_rows)
                    if (!r.header) {
                        const keyops::Channel& ch = m_channels[size_t(r.channel)];
                        const auto picked = m_drag.picked.find(ch.address.key());
                        if (const ParamValue* p = keyops::find(before, ch))
                            for (const Keyframe& k : p->keys)
                                if (picked == m_drag.picked.end() || !picked->second.count(k.time))
                                    candidates.push_back(keyops::toTimeline(before, ch, k.time));
                    }
                double bestPx = 6;
                for (TimeUs c : candidates)
                    if (const double px = std::abs(timeToX(c) - timeToX(target)); px < bestPx)
                        bestPx = px, target = c;
            }
            delta = target - m_drag.grab;
            for (const auto& [key, times] : m_drag.picked) // inside every setting's range
                if (const keyops::Channel* ch = channel(channelIndex(key)); ch && !times.empty()) {
                    const auto [lo, hi] = keyops::keyRange(before, *ch);
                    delta = std::clamp(delta, lo - *times.begin(), hi - *times.rbegin());
                }
        }
        const double dn = m_graph && m_drag.axis != 1 ? -d.y() / std::max(1.0, double(lanes().height() - 16)) * (m_v1 - m_v0) : 0.0;
        for (const auto& [key, times] : m_drag.picked) {
            const keyops::Channel* ch = channel(channelIndex(key));
            ParamValue* p = ch ? keyops::find(tl, *ch) : nullptr;
            if (!p)
                continue;
            std::map<TimeUs, double> original; // values, by new time
            for (const Keyframe& k : p->keys)
                if (times.count(k.time))
                    original[k.time + delta] = k.value;
            Times moved;
            if (m_drag.duplicate) {
                std::vector<Keyframe> copies;
                for (const Keyframe& k : p->keys)
                    if (times.count(k.time))
                        copies.push_back(k), copies.back().time += delta;
                for (const Keyframe& c : copies) {
                    auto it = std::find_if(p->keys.begin(), p->keys.end(), [&](const Keyframe& k) { return k.time == c.time; });
                    if (it != p->keys.end())
                        *it = c;
                    else
                        p->keys.insert(std::upper_bound(p->keys.begin(), p->keys.end(), c.time, [](TimeUs t, const Keyframe& k) { return t < k.time; }), c);
                    moved.insert(c.time);
                }
            } else {
                moved = keyops::moveKeys(*p, times, delta);
            }
            if (dn != 0 && ch->def.type == ParamType::Number)
                for (Keyframe& k : p->keys)
                    if (moved.count(k.time) && original.count(k.time))
                        k.value = std::clamp(denormalize(*ch, normalize(*ch, original[k.time]) + dn), ch->def.min, ch->def.max);
            next[key] = moved;
        }
        m_drag.changed = true;
    } else if (m_drag.kind == Drag::Handle) {
        const keyops::Channel* ch = channel(m_drag.channel);
        ParamValue* p = ch ? keyops::find(tl, *ch) : nullptr;
        if (!p)
            return;
        auto it = std::find_if(p->keys.begin(), p->keys.end(), [&](const Keyframe& k) { return k.time == m_drag.key; });
        if (it == p->keys.end())
            return;
        const size_t i = size_t(it - p->keys.begin());
        const TimeUs t = keyops::fromTimeline(tl, *ch, xToTime(pos.x()));
        const double v = denormalize(*ch, yToValue(pos.y()));
        // Pixel position of a handle, back into a keyframe's numbers.
        auto toHandle = [&](const QPointF& px, const Keyframe& a, const Keyframe& b, double& x, double& y, double base) {
            const TimeUs ht = keyops::fromTimeline(tl, *ch, xToTime(px.x()));
            x = std::clamp(double(ht - a.time) / double(std::max<TimeUs>(1, b.time - a.time)), 0.0, 1.0);
            y = denormalize(*ch, yToValue(px.y())) - base;
        };
        auto pixel = [&](TimeUs key, double value) { return QPointF(timeToX(keyops::toTimeline(tl, *ch, key)), valueToY(normalize(*ch, value))); };
        if (m_drag.out && i + 1 < p->keys.size()) {
            Keyframe& a = p->keys[i];
            const Keyframe& b = p->keys[i + 1];
            a.x1 = std::clamp(double(t - a.time) / double(std::max<TimeUs>(1, b.time - a.time)), 0.0, 1.0);
            a.y1 = v - a.value;
            if (m_drag.aligned && i > 0 && p->keys[i - 1].interp == Interp::Bezier) { // keep the other side in line
                Keyframe& prev = p->keys[i - 1];
                const QPointF kp = pixel(a.time, a.value);
                const QPointF outPx = pixel(a.time + TimeUs(a.x1 * double(b.time - a.time)), a.value + a.y1);
                const QPointF inPx = pixel(prev.time + TimeUs(prev.x2 * double(a.time - prev.time)), a.value + prev.y2);
                const QPointF dir = kp - outPx;
                const double len = std::hypot(dir.x(), dir.y()), other = QLineF(kp, inPx).length();
                if (len > 1)
                    toHandle(kp + dir / len * other, prev, a, prev.x2, prev.y2, a.value);
            }
        } else if (!m_drag.out && i > 0) {
            Keyframe& prev = p->keys[i - 1];
            Keyframe& k = p->keys[i];
            prev.x2 = std::clamp(double(t - prev.time) / double(std::max<TimeUs>(1, k.time - prev.time)), 0.0, 1.0);
            prev.y2 = v - k.value;
            if (m_drag.aligned && i + 1 < p->keys.size() && k.interp == Interp::Bezier) {
                const Keyframe& b = p->keys[i + 1];
                const QPointF kp = pixel(k.time, k.value);
                const QPointF inPx = pixel(prev.time + TimeUs(prev.x2 * double(k.time - prev.time)), k.value + prev.y2);
                const QPointF outPx = pixel(k.time + TimeUs(k.x1 * double(b.time - k.time)), k.value + k.y1);
                const QPointF dir = kp - inPx;
                const double len = std::hypot(dir.x(), dir.y()), other = QLineF(kp, outPx).length();
                if (len > 1)
                    toHandle(kp + dir / len * other, k, b, k.x1, k.y1, k.value);
            }
        }
        next = m_picked;
        m_drag.changed = true;
    } else if (m_drag.kind == Drag::Scale) {
        if (m_drag.end == m_drag.pivot)
            return;
        const TimeUs target = snapFrame(xToTime(pos.x()));
        double factor = double(target - m_drag.pivot) / double(m_drag.end - m_drag.pivot);
        factor = std::max(factor, 0.02);
        // No keyframe past its setting's range.
        for (const auto& [key, times] : m_drag.picked)
            if (const keyops::Channel* ch = channel(channelIndex(key)))
                for (TimeUs t : times) {
                    const auto [lo, hi] = keyops::keyRange(before, *ch);
                    const double off = double(keyops::toTimeline(before, *ch, t) - m_drag.pivot);
                    const double room = double((off > 0 ? keyops::toTimeline(before, *ch, hi) : keyops::toTimeline(before, *ch, lo)) - m_drag.pivot);
                    if (std::abs(off) > 0 && room / off > 0)
                        factor = std::min(factor, room / off);
                }
        for (const auto& [key, times] : m_drag.picked) {
            const keyops::Channel* ch = channel(channelIndex(key));
            ParamValue* p = ch ? keyops::find(tl, *ch) : nullptr;
            if (p)
                next[key] = keyops::scaleKeys(*p, times, keyops::fromTimeline(before, *ch, m_drag.pivot), factor,
                                              [&](TimeUs k) { return channelSnap(*ch, k); });
        }
        m_drag.changed = true;
    }
    m_picked = next;
    m_applying = true;
    m_project->setTimelineLive(tl);
    m_applying = false;
    updateFields();
    m_canvas->update();
}

void KeyframeEditor::finishDrag()
{
    const Drag d = std::move(m_drag);
    m_drag = {};
    if (d.before && d.changed && !(*d.before == m_project->timeline())) {
        const QString label = d.kind == Drag::Handle  ? tr("Bezier Handle")
                              : d.kind == Drag::Scale ? tr("Stretch Keyframes")
                              : d.duplicate           ? tr("Copy Keyframes")
                                                      : tr("Move Keyframes");
        m_applying = true;
        m_project->commitEdit(*d.before, label);
        m_applying = false;
    }
    refresh();
}

void KeyframeEditor::selectInRect(const QRect& r, bool additive)
{
    if (!additive)
        m_picked.clear();
    const Timeline& tl = m_project->timeline();
    if (m_graph) {
        for (int index : graphChannels())
            if (const ParamValue* p = values(index))
                for (const Keyframe& k : p->keys)
                    if (r.contains(keyPoint(index, k).toPoint()))
                        m_picked[m_channels[size_t(index)].address.key()].insert(k.time);
    } else {
        for (size_t i = 0; i < m_rows.size(); ++i) {
            const double cy = rowTop(int(i)) + kRowH / 2.0;
            if (cy < r.top() || cy > r.bottom())
                continue;
            for (size_t ci = 0; ci < m_channels.size(); ++ci) {
                const keyops::Channel& ch = m_channels[ci];
                // A group's row picks all of its settings' keyframes in the box's span.
                if (m_rows[i].header ? ch.group != m_rows[i].group : int(ci) != m_rows[i].channel)
                    continue;
                if (const ParamValue* p = keyops::find(tl, ch))
                    for (const Keyframe& k : p->keys)
                        if (const double x = timeToX(keyops::toTimeline(tl, ch, k.time)); x >= r.left() && x <= r.right())
                            m_picked[ch.address.key()].insert(k.time);
            }
        }
    }
    updateFields();
    m_canvas->update();
}

// ---------------------------------------------------------------------------
// Self-test
// ---------------------------------------------------------------------------

int KeyframeEditor::runSelfTest(QStringList& log)
{
    int failures = 0;
    auto check = [&](bool ok, const QString& what) {
        log << QStringLiteral("%1  keyframe editor: %2").arg(ok ? QStringLiteral("PASS") : QStringLiteral("FAIL"), what);
        failures += !ok;
    };
    // An event with a Gaussian Blur whose first number setting has two keyframes.
    int clipId = 0;
    for (const Track& t : m_project->timeline().tracks)
        if (t.type == TrackType::Video && !t.clips.empty() && !clipId)
            clipId = t.clips.front().id;
    if (!clipId) {
        check(false, "the project has a video event to test on");
        return failures;
    }
    const FrameRate fps = m_project->settings().fps;
    const TimeUs frame = fps.frameDuration();
    int effectId = 0;
    QString param;
    {
        Timeline tl = m_project->timeline();
        Clip* c = tl.findClip(clipId);
        EffectInstance blur = makeEffectOfType(QStringLiteral("gaussian_blur"), tl.allocId());
        effectId = blur.id;
        for (const ParamDef& d : *effectMeta(blur.type)->params)
            if (d.type == ParamType::Number && param.isEmpty())
                param = d.id;
        const ParamDef* def = nullptr;
        for (const ParamDef& d : *effectMeta(blur.type)->params)
            if (d.id == param)
                def = &d;
        auto& pv = blur.params[param];
        Keyframe a, b;
        a.time = fps.snap(c->length / 4), a.value = def->min;
        b.time = fps.snap(c->length * 3 / 4), b.value = def->min + (def->max - def->min) / 2;
        pv.keys = {a, b};
        c->effects.push_back(blur);
        m_project->applyEdit(tl, QStringLiteral("Keyframe editor test"));
    }
    keyops::Address address;
    address.source = keyops::Address::Source::ClipFx;
    address.owner = clipId;
    address.effect = effectId;
    address.param = param;
    const QString key = address.key();
    m_canvas->resize(std::max(m_canvas->width(), 1000), std::max(m_canvas->height(), 320));
    setTarget({FxTarget::Kind::Clip, clipId});
    m_snapButton->setChecked(false);
    fit();
    const int index = channelIndex(key);
    check(index >= 0 && std::any_of(m_rows.begin(), m_rows.end(), [&](const Row& r) { return r.channel == index; }),
          "an event's keyframed effect setting has a row");
    auto keys = [&] { const ParamValue* p = values(channelIndex(key)); return p ? p->keys : std::vector<Keyframe>{}; };
    auto rowY = [&] {
        for (size_t r = 0; r < m_rows.size(); ++r)
            if (m_rows[r].channel == channelIndex(key))
                return rowTop(int(r)) + kRowH / 2;
        return -1;
    };
    auto send = [&](QEvent::Type type, QPoint at, Qt::MouseButton button, Qt::KeyboardModifiers mods = {}) {
        QMouseEvent e(type, QPointF(at), m_canvas->mapToGlobal(QPointF(at)), button, type == QEvent::MouseButtonRelease ? Qt::NoButton : Qt::MouseButtons(button), mods);
        QApplication::sendEvent(m_canvas, &e);
    };
    auto keyX = [&](size_t i) { return int(std::lround(timeToX(keyops::toTimeline(m_project->timeline(), m_channels[size_t(channelIndex(key))], keys()[i].time)))); };
    auto press = [&](int k, Qt::KeyboardModifiers mods = {}) {
        QKeyEvent e(QEvent::KeyPress, k, mods);
        QApplication::sendEvent(m_canvas, &e);
    };
    QUndoStack* undo = m_project->undoStack();

    // Dope Sheet: click the second keyframe and drag it ten frames later.
    const TimeUs secondBefore = keys().at(1).time;
    const QPoint k2(keyX(1), rowY());
    send(QEvent::MouseButtonPress, k2, Qt::LeftButton);
    check(m_picked.count(key) && m_picked[key].count(secondBefore), "clicking a keyframe picks it");
    const int tenFrames = int(std::lround(10 * frame / m_usPerPx));
    send(QEvent::MouseMove, k2 + QPoint(tenFrames, 0), Qt::LeftButton);
    send(QEvent::MouseButtonRelease, k2 + QPoint(tenFrames, 0), Qt::LeftButton);
    const TimeUs moved = keys().at(1).time - secondBefore;
    check(std::llabs(moved - 10 * frame) <= frame, QStringLiteral("dragging moves it by whole frames (%1 frames)").arg(double(moved) / frame, 0, 'f', 1));
    undo->undo();
    check(keys().at(1).time == secondBefore, "one undo puts it back");
    undo->redo();

    // Curves from the keyboard and the toolbar.
    m_picked = {{key, {keys().at(0).time}}};
    press(Qt::Key_5);
    check(keys().at(0).interp == Interp::EaseOut, "key 5 gives the picked keyframe an Ease Out curve");
    findChild<QToolButton*>(QStringLiteral("keyCurve_bezier"))->click();
    check(keys().at(0).interp == Interp::Bezier && keys().at(0).y1 > 0, "the Bezier button keeps the curve's shape in handles");

    // Graph: drag the first keyframe up, then its handle.
    m_graphButton->click();
    fit();
    const QPoint k1 = keyPoint(channelIndex(key), keys().at(0)).toPoint();
    const double valueBefore = keys().at(0).value;
    send(QEvent::MouseButtonPress, k1, Qt::LeftButton);
    send(QEvent::MouseMove, k1 + QPoint(0, -30), Qt::LeftButton, Qt::ShiftModifier);
    send(QEvent::MouseMove, k1 + QPoint(1, -40), Qt::LeftButton, Qt::ShiftModifier);
    send(QEvent::MouseButtonRelease, k1 + QPoint(1, -40), Qt::LeftButton, Qt::ShiftModifier);
    check(keys().at(0).value > valueBefore,
          QStringLiteral("dragging a keyframe up in the graph raises its value (%1 to %2)").arg(valueBefore).arg(keys().at(0).value));
    fit();
    const QPoint handle = handlePoints(channelIndex(key), 0).first.toPoint();
    const Keyframe beforeHandle = keys().at(0);
    send(QEvent::MouseButtonPress, handle, Qt::LeftButton);
    send(QEvent::MouseMove, handle + QPoint(20, -25), Qt::LeftButton);
    send(QEvent::MouseButtonRelease, handle + QPoint(20, -25), Qt::LeftButton);
    check(keys().at(0).x1 > beforeHandle.x1 && keys().at(0).y1 > beforeHandle.y1, "dragging a Bezier handle reshapes the curve");
    m_dopeButton->click();
    fit();

    // Box-pick both, copy, paste at the cursor (two more), stretch, delete.
    send(QEvent::MouseButtonPress, QPoint(kListW + 2, rowY() - kRowH / 2 + 1), Qt::LeftButton);
    send(QEvent::MouseMove, QPoint(m_canvas->width() - 2, rowY() + kRowH / 2 - 1), Qt::LeftButton);
    send(QEvent::MouseButtonRelease, QPoint(m_canvas->width() - 2, rowY() + kRowH / 2 - 1), Qt::LeftButton);
    check(pickedCount() == 2, "a box picks the keyframes inside it");
    const TimeUs gap = keys().at(1).time - keys().at(0).time;
    double x0 = 0, x1 = 0;
    scaleBar(x0, x1);
    const int extra = int(std::lround(double(gap) / 2 / m_usPerPx));
    send(QEvent::MouseButtonPress, QPoint(int(std::lround(x1)), kRulerH - 6), Qt::LeftButton);
    send(QEvent::MouseMove, QPoint(int(std::lround(x1)) - extra, kRulerH - 6), Qt::LeftButton);
    send(QEvent::MouseButtonRelease, QPoint(int(std::lround(x1)) - extra, kRulerH - 6), Qt::LeftButton);
    const TimeUs gapAfter = keys().at(1).time - keys().at(0).time;
    check(std::llabs(gapAfter - gap / 2) <= 2 * frame, QStringLiteral("dragging the bar's end squeezes the keyframes (%1 to %2 frames apart)").arg(gap / frame).arg(gapAfter / frame));
    press(Qt::Key_C, Qt::ControlModifier);
    const Clip* c = m_project->timeline().findClip(clipId);
    emit seekRequested(c->start); // (the test's cursor may not follow; paste at an explicit time)
    paste(c->start + c->length - gapAfter - 2 * frame, -1);
    check(keys().size() == 4, QStringLiteral("copy and paste adds the keyframes again (%1 now)").arg(keys().size()));
    m_picked = {{key, {keys().back().time}}};
    press(Qt::Key_Delete);
    check(keys().size() == 3, "Delete removes the picked keyframe");
    // A setting with no keyframes, shown from a control's menu.
    keyops::Address pan;
    pan.source = keyops::Address::Source::PanCrop;
    pan.owner = clipId;
    pan.param = QStringLiteral("zoom");
    focusSetting(pan);
    const int panIndex = channelIndex(pan.key());
    check(panIndex >= 0 && std::any_of(m_rows.begin(), m_rows.end(), [&](const Row& r) { return r.channel == panIndex; }),
          "a setting without keyframes can be shown to add them to");
    if (panIndex >= 0) {
        addKeyAt(panIndex, c->start + c->length / 2);
        const ParamValue* zp = values(panIndex);
        check(zp && zp->keys.size() == 1, "adding a keyframe to a Pan/Crop setting not stored yet");
    }
    undo->undo();
    m_snapButton->setChecked(true);
    return failures;
}
