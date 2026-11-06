#include "ui/DeviceChainView.h"
#include "core/NodeGraph.h"

#include "ui/Help.h"
#include "core/AudioEffects.h"
#include "core/Presets.h"
#include "ui/EffectsBrowser.h"
#include "ui/Knob.h"
#include "ui/ParamDisplay.h"
#include "core/ColorMatch.h"
#include "core/RetroShaders.h"
#include <QTimer>
#include "ui/ModulationDialog.h"
#include "ui/PreviewWidget.h"
#include "core/EffectMask.h"
#include "core/MotionTracker.h"
#include "core/PanCrop.h"
#include "core/Stabilizer.h"
#include "core/LiveModulation.h"
#include "core/Vst3Host.h"
#include "ui/Vst3Window.h"
#include "ui/Theme.h"

#include <functional>
#include <QAbstractButton>
#include <QApplication>
#include <QColorDialog>
#include <QComboBox>
#include <QDialog>
#include <QDialogButtonBox>
#include <QDoubleSpinBox>
#include <QFormLayout>
#include <QCoreApplication>
#include <QDir>
#include <QUndoStack>
#include <QDrag>
#include <QDragEnterEvent>
#include <QFrame>
#include <QFileDialog>
#include <QFileInfo>
#include <QSettings>
#include <QGridLayout>
#include <QHBoxLayout>
#include <QInputDialog>
#include <QLabel>
#include <QLineEdit>
#include <QMenu>
#include <QPointer>
#include <QMessageBox>
#include <QMimeData>
#include <QMouseEvent>
#include <QPainter>
#include <QPushButton>
#include <QScrollArea>
#include <QScrollBar>
#include <QShortcut>
#include <QToolButton>
#include <QVBoxLayout>
#include <algorithm>
#include <cmath>
#include <QProgressDialog>
#include <atomic>
#include <chrono>
#include <numbers>
#include <thread>

namespace {

constexpr int kBodyHeight = 140;

QString pathToString(const ChainPath& p)
{
    QStringList parts;
    for (const auto& [e, c] : p.steps)
        parts << QStringLiteral("%1:%2").arg(e).arg(c);
    return parts.join(QLatin1Char(','));
}

ChainPath pathFromString(const QString& s)
{
    ChainPath p;
    for (const QString& part : s.split(QLatin1Char(','), Qt::SkipEmptyParts)) {
        const QStringList ec = part.split(QLatin1Char(':'));
        if (ec.size() == 2)
            p.steps.emplace_back(ec[0].toInt(), ec[1].toInt());
    }
    return p;
}

// Small square toggle button (power, solo, keyframe...).
QToolButton* smallButton(const QString& text, const QString& tip, bool checkable)
{
    auto* b = new QToolButton;
    b->setText(text);
    b->setToolTip(tip);
    b->setCheckable(checkable);
    b->setAutoRaise(true);
    b->setFixedSize(18, 18);
    b->setFocusPolicy(Qt::NoFocus);
    return b;
}

// A short choice as a row of buttons, one lit (Peak | RMS); longer ones stay menus.
bool segmented(const ParamDef& def)
{
    if (def.choices.size() < 2 || def.choices.size() > 4)
        return false;
    int chars = 0;
    for (const QString& c : def.choices)
        chars += int(c.size());
    return chars <= 26;
}

QString segmentStyle()
{
    return QStringLiteral("QToolButton { color: @text; border: 1px solid @deep; background: @hover; padding: 1px 5px; margin: 0; }"
                          "QToolButton:checked { color: @onAccent; background: @accent; }");
}

QString toggleStyle()
{
    return QStringLiteral("QToolButton { color: @text; border: 1px solid @deep; background: @hover; padding: 1px 4px; }"
                          "QToolButton:checked { color: @deep; background: @key; }");
}

// Keyframe marker: hollow diamond, filled when there is a key at the
// cursor, orange when the parameter is animated.
class KeyButton : public QAbstractButton {
public:
    KeyButton(QWidget* parent) : QAbstractButton(parent) { setFixedSize(14, 14); setCursor(Qt::PointingHandCursor); }
    void setState(bool here, bool animated)
    {
        m_here = here;
        m_animated = animated;
        update();
    }

protected:
    void paintEvent(QPaintEvent*) override
    {
        QPainter p(this);
        p.setRenderHint(QPainter::Antialiasing, true);
        const QColor c = m_animated ? theme::c().key : theme::c().dim;
        QPolygonF d;
        d << QPointF(7, 2) << QPointF(12, 7) << QPointF(7, 12) << QPointF(2, 7);
        p.setPen(QPen(underMouse() ? c.lighter(140) : c, 1.4));
        p.setBrush(m_here ? QBrush(c) : Qt::NoBrush);
        p.drawPolygon(d);
    }
    void enterEvent(QEnterEvent*) override { update(); }
    void leaveEvent(QEvent*) override { update(); }

private:
    bool m_here = false;
    bool m_animated = false;
};

} // namespace

// ---------------------------------------------------------------------------
// ParamControl: one parameter (knob, menu, toggle, colour or text) plus its
// keyframe button. Right-click for keyframes, typed values and reset.
// ---------------------------------------------------------------------------

class ParamControl : public QWidget {
public:
    ParamControl(DeviceChainView* view, DeviceChainView::ParamRef ref, const ParamDef& def, bool compact)
        : m_view(view), m_ref(std::move(ref)), m_def(def)
    {
        auto* l = new QVBoxLayout(this);
        l->setContentsMargins(0, 0, 0, 0);
        l->setSpacing(2);
        const QString id = def.id;
        auto label = [&] {
            if (compact)
                return;
            auto* lab = new QLabel(def.name);
            theme::style(lab, QStringLiteral("color: @text; font-size: 8pt;"));
            lab->setAlignment(Qt::AlignCenter);
            l->addWidget(lab);
        };
        switch (def.type) {
        case ParamType::Number:
            m_knob = new Knob(def, compact);
            l->addWidget(m_knob, 0, Qt::AlignHCenter);
            QObject::connect(m_knob, &Knob::valueEdited, this, [this, id](double v) { m_view->setParam(m_ref, id, v); });
            break;
        case ParamType::Choice:
            label();
            if (segmented(def)) {
                auto* row = new QHBoxLayout;
                row->setSpacing(0);
                for (int i = 0; i < int(def.choices.size()); ++i) {
                    auto* b = new QToolButton;
                    b->setText(def.choices[i]);
                    b->setCheckable(true);
                    b->setFocusPolicy(Qt::NoFocus);
                    theme::style(b, segmentStyle());
                    row->addWidget(b);
                    m_segments.push_back(b);
                    QObject::connect(b, &QToolButton::clicked, this, [this, id, i] {
                        m_view->setParam(m_ref, id, i);
                        refreshSegments(i); // (lit at once, as a menu shows its choice)
                    });
                }
                l->addLayout(row);
                break;
            }
            m_combo = new QComboBox;
            m_combo->addItems(def.choices);
            m_combo->setFocusPolicy(Qt::NoFocus);
            m_combo->setMaximumWidth(compact ? 96 : 130);
            l->addWidget(m_combo);
            QObject::connect(m_combo, qOverload<int>(&QComboBox::activated), this, [this, id](int i) { m_view->setParam(m_ref, id, i); });
            break;
        case ParamType::Bool:
            m_toggle = new QToolButton;
            m_toggle->setText(def.name);
            m_toggle->setCheckable(true);
            m_toggle->setFocusPolicy(Qt::NoFocus);
            theme::style(m_toggle, toggleStyle());
            l->addWidget(m_toggle);
            QObject::connect(m_toggle, &QToolButton::clicked, this, [this, id](bool on) { m_view->setParam(m_ref, id, on ? 1.0 : 0.0); });
            break;
        case ParamType::Color:
            label();
            m_color = new QPushButton;
            m_color->setFixedSize(48, 18);
            m_color->setFocusPolicy(Qt::NoFocus);
            l->addWidget(m_color, 0, Qt::AlignHCenter);
            QObject::connect(m_color, &QPushButton::clicked, this, [this, id] {
                const QColor c = QColorDialog::getColor(QColor(QRgb(m_color->property("rgb").toUInt() | 0xff000000u)), this, m_def.name);
                if (c.isValid())
                    m_view->setParam(m_ref, id, double(c.rgb() & 0xffffff));
            });
            break;
        case ParamType::Text:
        case ParamType::Font:
            label();
            m_text = new QLineEdit;
            m_text->setMinimumWidth(150);
            m_text->setToolTip(def.tooltip);
            QObject::connect(m_text, &QLineEdit::textEdited, this, [this, id](const QString& t) { m_view->setParamText(m_ref, id, t); });
            if (const EffectInstance* fx = m_view->effectAt(m_ref.path, m_ref.index); fx && fx->type == QLatin1String("a_ffmpeg")) {
                // A chain FFmpeg can't parse passes the sound through unchanged: say so.
                QObject::connect(m_text, &QLineEdit::textChanged, this, [this](const QString& t) {
                    const bool ok = audioFilterSpecValid(t);
                    theme::style(m_text, ok ? QString() : QStringLiteral("border: 1px solid @bad;"));
                    m_text->setToolTip(ok ? m_def.tooltip
                                          : QObject::tr("FFmpeg can't use this filter chain, so the sound passes through unchanged.\n\n") + m_def.tooltip);
                });
            }
            if (!def.fileFilter.isEmpty()) {
                // A file path: typed, or picked with Browse.
                auto* row = new QHBoxLayout;
                row->setSpacing(2);
                row->addWidget(m_text, 1);
                auto* browse = new QToolButton;
                browse->setText(QObject::tr("Browse..."));
                row->addWidget(browse);
                l->addLayout(row);
                QObject::connect(browse, &QToolButton::clicked, this, [this, id, filter = def.fileFilter, name = def.name] {
                    QSettings settings;
                    const QString start = m_text->text().isEmpty() ? settings.value(QStringLiteral("dirs/fxFiles")).toString() : m_text->text();
                    const QString path = QFileDialog::getOpenFileName(this, name, start, filter);
                    if (path.isEmpty())
                        return;
                    settings.setValue(QStringLiteral("dirs/fxFiles"), QFileInfo(path).absolutePath());
                    m_text->setText(path);
                    m_view->setParamText(m_ref, id, path);
                });
            } else {
                l->addWidget(m_text);
            }
            break;
        }
        l->addStretch();
        if (def.animatable() && !compact && m_view->m_target.kind != FxTarget::Kind::Live) { // live: no timeline to key on
            m_key = new KeyButton(this);
            m_key->setToolTip(QObject::tr("Add / remove a keyframe at the cursor (right-click the control for more)"));
            m_key->move(0, m_knob ? m_knob->sizeHint().height() - 42 : 0); // beside the knob, clear of its name
            m_key->raise();
            QObject::connect(m_key, &QAbstractButton::clicked, this, [this] { m_view->toggleKey(m_ref, m_def); });
        }
        setContextMenuPolicy(Qt::CustomContextMenu);
        QObject::connect(this, &QWidget::customContextMenuRequested, this, [this](const QPoint& pos) { showMenu(mapToGlobal(pos)); });
        if (m_knob) {
            m_knob->setContextMenuPolicy(Qt::CustomContextMenu);
            QObject::connect(m_knob, &QWidget::customContextMenuRequested, this,
                             [this](const QPoint& pos) { showMenu(m_knob->mapToGlobal(pos)); });
        }
        m_view->m_controls.push_back(this);
    }

    void refresh(TimeUs t, TimeUs tol)
    {
        const ParamMap* map = m_view->params(m_ref);
        if (!map)
            return;
        auto it = map->find(m_def.id);
        const ParamValue pv = it != map->end() ? it->second : ParamValue{m_def.def, m_def.defText, {}};
        const double v = pv.at(t, m_def.type);
        const bool animated = !pv.keys.empty();
        if (m_knob) {
            m_knob->setValue(v);
            m_knob->setAnimated(animated);
            m_knob->setModulated(pv.mod.has_value());
            m_knob->setToolTip(pv.mod ? QObject::tr("%1\nModulated: %2 (right-click to change)").arg(m_def.tooltip.isEmpty() ? m_def.name : m_def.tooltip,
                                                                                                    modulationSummary(*pv.mod))
                                      : m_def.tooltip);
        }
        if (m_combo) {
            QSignalBlocker block(m_combo);
            m_combo->setCurrentIndex(int(std::lround(v)));
        }
        refreshSegments(int(std::lround(v)));
        if (m_toggle)
            m_toggle->setChecked(v >= 0.5);
        if (m_color) {
            m_color->setProperty("rgb", uint32_t(v));
            theme::style(m_color, QStringLiteral("background-color: #%1; border: 1px solid @deep;").arg(uint32_t(v) & 0xffffff, 6, 16, QLatin1Char('0')));
        }
        if (m_text && !m_text->hasFocus())
            m_text->setText(pv.text);
        if (m_key)
            m_key->setState(pv.hasKeyAt(t, tol), animated);
    }

private:
    void refreshSegments(int on)
    {
        for (int i = 0; i < int(m_segments.size()); ++i)
            m_segments[size_t(i)]->setChecked(i == on);
    }
    void showMenu(const QPoint& global)
    {
        QMenu menu;
        const QString id = m_def.id;
        if (m_def.animatable()) {
            menu.addAction(QObject::tr("Add / Remove Keyframe at Cursor"), [this] { m_view->toggleKey(m_ref, m_def); });
            menu.addAction(QObject::tr("Previous Keyframe"), [this, id] { m_view->jumpKey(m_ref, id, -1); });
            menu.addAction(QObject::tr("Next Keyframe"), [this, id] { m_view->jumpKey(m_ref, id, 1); });
            QMenu* curve = menu.addMenu(QObject::tr("Keyframe Curve"));
            for (int i = 0; i < kKeyInterps; ++i)
                curve->addAction(keyInterpName(Keyframe::Interp(i)), [this, id, i] { m_view->setKeyInterp(m_ref, id, Keyframe::Interp(i)); });
            if (m_view->m_target.kind != FxTarget::Kind::Live)
                menu.addAction(QObject::tr("Edit in Keyframe Editor..."), [this, id] { emit m_view->keyframeEditorRequested(m_view->keyAddress(m_ref, id)); });
            menu.addSeparator();
        }
        if (m_knob)
            menu.addAction(QObject::tr("Enter Value..."), [this, id] {
                bool ok = false;
                const double v = QInputDialog::getDouble(this, m_def.name, m_def.name + (m_def.suffix.isEmpty() ? QString() : QStringLiteral(" (%1)").arg(m_def.suffix.trimmed())),
                                                         m_knob->value(), m_def.min, m_def.max, m_def.decimals, &ok);
                if (ok)
                    m_view->setParam(m_ref, id, v);
            });
        menu.addAction(QObject::tr("Reset to Default"), [this] { m_view->resetParam(m_ref, m_def); });
        if (m_view->m_target.kind != FxTarget::Kind::Live && m_def.type == ParamType::Number && m_def.animatable())
            menu.addAction(QObject::tr("Follow Audio..."), [this] { m_view->followAudio(m_ref, m_def); });
        if (m_view->m_target.kind == FxTarget::Kind::Live && m_def.type == ParamType::Number) {
            menu.addSeparator();
            const ParamMap* map = m_view->params(m_ref);
            std::optional<Modulation> current;
            if (map)
                if (auto it = map->find(id); it != map->end())
                    current = it->second.mod;
            menu.addAction(current ? QObject::tr("Modulation: %1...").arg(modulationSummary(*current)) : QObject::tr("Modulate..."), [this, id, current] {
                ModulationDialog dlg(m_def, current, this);
                if (dlg.exec() == QDialog::Accepted)
                    m_view->setModulation(m_ref, m_def, dlg.result());
            });
            if (current)
                menu.addAction(QObject::tr("Remove Modulation"), [this] { m_view->setModulation(m_ref, m_def, std::nullopt); });
        }
        menu.exec(global);
    }

    DeviceChainView* m_view;
    DeviceChainView::ParamRef m_ref;
    ParamDef m_def;
    Knob* m_knob = nullptr;
    QComboBox* m_combo = nullptr;
    std::vector<QToolButton*> m_segments;
    QToolButton* m_toggle = nullptr;
    QPushButton* m_color = nullptr;
    QLineEdit* m_text = nullptr;
    KeyButton* m_key = nullptr;
};

// ---------------------------------------------------------------------------
// DeviceTitle: the title bar; click selects, drag moves (Ctrl copies),
// double-click folds, right-click for the device menu.
// ---------------------------------------------------------------------------

class DeviceTitle : public QWidget {
public:
    DeviceTitle(DeviceChainView* view, ChainPath path, int index, const EffectInstance& e, const QString& name, bool selected)
        : m_view(view), m_path(std::move(path)), m_index(index), m_id(e.id), m_isRack(e.isRack()), m_rackChains(int(e.chains.size()))
    {
        setFixedHeight(22);
        setAutoFillBackground(true);
        QPalette pal = palette();
        pal.setColor(QPalette::Window, selected ? theme::c().key.darker(170) : theme::c().panel);
        setPalette(pal);
        auto* l = new QHBoxLayout(this);
        l->setContentsMargins(3, 0, 3, 0);
        l->setSpacing(4);
        auto* power = smallButton(QStringLiteral("⏻"), QObject::tr("Device on/off"), true);
        power->setChecked(e.enabled);
        theme::style(power, QStringLiteral("QToolButton { color: @faint; border-radius: 9px; background: @deep; }"
                                            "QToolButton:checked { color: @deep; background: @key; }"));
        l->addWidget(power);
        QObject::connect(power, &QToolButton::clicked, this, [this](bool on) { m_view->setDeviceEnabled(m_path, m_index, on); });
        auto* label = new QLabel(name);
        theme::style(label, QStringLiteral("color: @bright; font-weight: bold;"));
        label->setAttribute(Qt::WA_TransparentForMouseEvents);
        l->addWidget(label, 1);
        if (!view->isAudio() && !m_isRack && view->m_target.kind != FxTarget::Kind::AudioMaster) {
            auto* mask = smallButton(QStringLiteral("◐"), QObject::tr("Mask: limit this effect to a rectangle or an ellipse of the picture\n"
                                                                      "(drag it on the preview; Track Motion makes it follow something)"), true);
            const auto it = e.params.find(QStringLiteral("mask.shape"));
            mask->setChecked(it != e.params.end() && it->second.value >= 0.5);
            theme::style(mask, QStringLiteral("QToolButton { color: @faint; } QToolButton:checked { color: @info; }"));
            l->addWidget(mask);
            QObject::connect(mask, &QToolButton::clicked, this, [this] { m_view->toggleMask(m_path, m_index); });
        }
        if (e.type == QLatin1String("a_vst3")) {
            auto* plug = smallButton(QStringLiteral("🔌"), QObject::tr("The plugin: open its window, or pick an installed one"), false);
            l->addWidget(plug);
            QObject::connect(plug, &QToolButton::clicked, this, [this, plug] {
                QMenu menu;
                menu.addAction(QObject::tr("Open Plugin Window"), [this] { Vst3Window::open(m_view->m_project, m_id, m_view->window()); });
                menu.addSeparator();
                const QStringList installed = vst3InstalledPlugins();
                QMenu* pick = menu.addMenu(QObject::tr("Installed Plugins"));
                pick->setEnabled(!installed.isEmpty());
                for (const QString& path : installed)
                    pick->addAction(QFileInfo(path).completeBaseName(), [this, path] {
                        QString error;
                        const QStringList classes = vst3EffectClasses(path, &error);
                        if (classes.isEmpty()) {
                            QMessageBox::warning(m_view, QObject::tr("VST3 Plugin"), QObject::tr("No audio effect found: %1").arg(error));
                            return;
                        }
                        QString cls = classes.front();
                        if (classes.size() > 1) { // a shell of many plugins
                            bool ok = false;
                            cls = QInputDialog::getItem(m_view, QObject::tr("VST3 Plugin"), QObject::tr("Plugin"), classes, 0, false, &ok);
                            if (!ok)
                                return;
                        }
                        const DeviceChainView::ParamRef ref{m_path, m_index, -1};
                        m_view->editNow([&](Timeline& tl) {
                            if (ParamMap* map = m_view->params(tl, ref)) {
                                (*map)[QStringLiteral("plugin")].text = path;
                                (*map)[QStringLiteral("class")].text = cls;
                                (*map)[QStringLiteral("state")].text.clear(); // its defaults
                            }
                        }, QObject::tr("Choose Plugin"));
                    });
                menu.exec(plug->mapToGlobal(QPoint(0, plug->height())));
            });
        }
        auto* presets = smallButton(QStringLiteral("≡"), QObject::tr("Presets: load or save this device's settings"), false);
        theme::style(presets, QStringLiteral("QToolButton { color: @info; font-weight: bold; }"));
        l->addWidget(presets);
        QObject::connect(presets, &QToolButton::clicked, this, [this, presets] {
            QMenu menu;
            const EffectInstance* dev = m_view->effectAt(m_path, m_index);
            if (!dev)
                return;
            if (!dev->isRack()) {
                const std::vector<Preset> list = PresetLibrary::instance().forEffect(dev->type);
                if (list.empty())
                    menu.addAction(QObject::tr("No presets yet"))->setEnabled(false);
                for (const Preset& p : list) {
                    QAction* a = menu.addAction(p.user ? p.name + QObject::tr("  (user)") : p.name, [this, id = p.id] { m_view->loadPreset(m_path, m_index, id); });
                    a->setToolTip(p.description);
                }
                menu.addSeparator();
            }
            menu.addAction(dev->isRack() ? QObject::tr("Save Rack as Preset...") : QObject::tr("Save as Preset..."),
                           [this] { m_view->savePreset(m_path, m_index); });
            QMenu* del = nullptr;
            for (const Preset& p : PresetLibrary::instance().forEffect(dev->type))
                if (p.user) {
                    if (!del)
                        del = menu.addMenu(QObject::tr("Delete Preset"));
                    del->addAction(p.name, [id = p.id] { PresetLibrary::instance().remove(id); });
                }
            menu.setToolTipsVisible(true);
            menu.exec(presets->mapToGlobal(QPoint(0, presets->height())));
        });
        auto* fold = smallButton(m_view->m_folded.count(e.id) ? QStringLiteral("▸") : QStringLiteral("▾"), QObject::tr("Fold / unfold"), false);
        l->addWidget(fold);
        QObject::connect(fold, &QToolButton::clicked, this, [this] { m_view->toggleFold(m_id); });
        auto* close = smallButton(QStringLiteral("✕"), QObject::tr("Delete device"), false);
        l->addWidget(close);
        QObject::connect(close, &QToolButton::clicked, this, [this] { m_view->removeDevice(m_path, m_index); });
        setToolTip(QObject::tr("Drag to move it in front of or behind another device, into a rack, or onto a rack's chain list "
                               "(Ctrl+drag copies). Right-click for more."));
    }

protected:
    void mousePressEvent(QMouseEvent* e) override
    {
        if (e->button() == Qt::LeftButton) {
            m_pressPos = e->position().toPoint();
            m_pressed = true;
            m_view->setFocus();
        }
    }
    void mouseReleaseEvent(QMouseEvent* e) override
    {
        // Select on release: selecting rebuilds the view, which would end a drag.
        if (e->button() == Qt::LeftButton && m_pressed)
            m_view->selectDevice(m_id, e->modifiers() & Qt::ControlModifier);
        m_pressed = false;
    }
    void mouseMoveEvent(QMouseEvent* e) override
    {
        if (!m_pressed || !(e->buttons() & Qt::LeftButton)
            || (e->position().toPoint() - m_pressPos).manhattanLength() < QApplication::startDragDistance())
            return;
        m_pressed = false;
        auto* mime = new QMimeData;
        mime->setData(QString::fromLatin1(DeviceChainView::kDeviceMime), QStringLiteral("%1|%2").arg(pathToString(m_path)).arg(m_index).toUtf8());
        auto* drag = new QDrag(this);
        drag->setMimeData(mime);
        drag->setPixmap(parentWidget()->grab().scaledToHeight(60, Qt::SmoothTransformation));
        DeviceChainView* view = m_view;
        view->setDragging(true);
        drag->exec(Qt::MoveAction | Qt::CopyAction, Qt::MoveAction);
        view->setDragging(false); // may rebuild later: don't touch `this` after here
    }
    void mouseDoubleClickEvent(QMouseEvent*) override { m_view->toggleFold(m_id); }
    void contextMenuEvent(QContextMenuEvent* e) override
    {
        QMenu menu;
        menu.addAction(QObject::tr("Group into Rack\tCtrl+G"), [this] {
            if (!m_view->m_selected.count(m_id))
                m_view->selectDevice(m_id, false);
            m_view->groupSelection();
        });
        if (m_isRack) {
            QAction* a = menu.addAction(QObject::tr("Ungroup"), [this] { m_view->ungroup(m_path, m_index); });
            a->setEnabled(m_rackChains <= 1);
            if (m_rackChains > 1)
                a->setText(QObject::tr("Ungroup (only with one chain)"));
            menu.addAction(QObject::tr("Add Chain"), [this] { m_view->addChain(m_path, m_index); });
        }
        menu.addAction(QObject::tr("Duplicate"), [this] { m_view->duplicateDevice(m_path, m_index); });
        menu.addAction(m_isRack ? QObject::tr("Save Rack as Preset...") : QObject::tr("Save as Preset..."), [this] { m_view->savePreset(m_path, m_index); });
        if (m_view->m_selected.size() > 1 && m_view->m_selected.count(m_id))
            menu.addAction(QObject::tr("Save Selected Devices as Chain Preset..."), [this] { m_view->saveSelectionPreset(); });
        menu.addAction(m_view->m_folded.count(m_id) ? QObject::tr("Unfold") : QObject::tr("Fold"), [this] { m_view->toggleFold(m_id); });
        menu.addSeparator();
        menu.addAction(QObject::tr("Delete"), [this] { m_view->removeDevice(m_path, m_index); });
        menu.exec(e->globalPos());
    }

private:
    DeviceChainView* m_view;
    ChainPath m_path;
    int m_index;
    int m_id;
    bool m_isRack;
    int m_rackChains;
    QPoint m_pressPos;
    bool m_pressed = false;
};

// ---------------------------------------------------------------------------
// ChainStrip: devices in a row; drops land between them.
// ---------------------------------------------------------------------------

class ChainStrip : public QWidget {
public:
    ChainStrip(DeviceChainView* view, ChainPath path, const EffectChain& chain, bool nested);

    // Index the drop at x (strip coordinates) inserts at.
    int dropIndex(int x) const
    {
        int i = 0;
        for (QWidget* d : m_devices)
            if (d->geometry().center().x() < x)
                ++i;
        return i;
    }

protected:
    void dragEnterEvent(QDragEnterEvent* e) override
    {
        if (m_view->canDrop(e->mimeData()))
            e->acceptProposedAction();
    }
    void dragMoveEvent(QDragMoveEvent* e) override
    {
        if (!m_view->canDrop(e->mimeData()))
            return;
        e->acceptProposedAction();
        showIndicator(dropIndex(e->position().toPoint().x()));
    }
    void dragLeaveEvent(QDragLeaveEvent*) override { m_indicator->hide(); }
    void dropEvent(QDropEvent* e) override
    {
        m_indicator->hide();
        const bool copy = e->modifiers() & Qt::ControlModifier; // Ctrl+drag copies
        if (m_view->dropMime(e->mimeData(), m_path, dropIndex(e->position().toPoint().x()), copy)) {
            // Only a device dragged within the chain moves; effects and presets from the
            // browser are copies (a "move" would make the browser delete its item).
            const bool move = !copy && e->mimeData()->hasFormat(QString::fromLatin1(DeviceChainView::kDeviceMime));
            e->setDropAction(move ? Qt::MoveAction : Qt::CopyAction);
            e->accept();
        }
    }

private:
    void showIndicator(int index)
    {
        int x;
        if (m_devices.empty())
            x = 4;
        else if (index >= int(m_devices.size()))
            x = m_devices.back()->geometry().right() + 3;
        else
            x = m_devices[size_t(index)]->geometry().left() - 4;
        m_indicator->setGeometry(x - 1, 2, 3, height() - 4);
        m_indicator->show();
        m_indicator->raise();
    }

    DeviceChainView* m_view;
    ChainPath m_path;
    std::vector<QWidget*> m_devices;
    QFrame* m_indicator;
};

// ---------------------------------------------------------------------------
// RackChainList: a rack's chains (select, on/off, solo, level, blend);
// dropping an effect or device here makes a new parallel chain.
// ---------------------------------------------------------------------------

class RackChainList : public QWidget {
public:
    RackChainList(DeviceChainView* view, ChainPath path, int rackIndex, const EffectInstance& rack, int selected, bool audio)
        : m_view(view), m_path(std::move(path)), m_rackIndex(rackIndex)
    {
        setAcceptDrops(true);
        setFixedWidth(audio ? 230 : 300);
        auto* l = new QVBoxLayout(this);
        l->setContentsMargins(4, 4, 4, 4);
        l->setSpacing(2);
        const int rackId = rack.id;
        for (int i = 0; i < int(rack.chains.size()); ++i) {
            const RackChain& c = rack.chains[size_t(i)];
            auto* row = new QWidget;
            row->setFixedHeight(28);
            row->setAutoFillBackground(true);
            QPalette pal = row->palette();
            pal.setColor(QPalette::Window, i == selected ? theme::c().hover : theme::c().panel);
            row->setPalette(pal);
            auto* rl = new QHBoxLayout(row);
            rl->setContentsMargins(2, 1, 2, 1);
            rl->setSpacing(3);
            auto* power = smallButton(QStringLiteral("⏻"), QObject::tr("Chain on/off"), true);
            power->setChecked(c.enabled);
            theme::style(power, QStringLiteral("QToolButton { color: @faint; border-radius: 9px; background: @deep; }"
                                                "QToolButton:checked { color: @deep; background: @key; }"));
            auto* solo = smallButton(QStringLiteral("S"), QObject::tr("Solo this chain"), true);
            solo->setChecked(c.solo);
            theme::style(solo, QStringLiteral("QToolButton { color: @dim; background: @deep; }"
                                               "QToolButton:checked { color: @deep; background: @info; }"));
            auto* name = new QPushButton(c.name.isEmpty() ? QObject::tr("Chain %1").arg(i + 1) : c.name);
            name->setFlat(true);
            name->setFocusPolicy(Qt::NoFocus);
            theme::style(name, QStringLiteral("text-align: left; color: @text; padding: 0 4px;"));
            name->setToolTip(QObject::tr("Click to show this chain's devices; right-click to rename, duplicate or delete"));
            name->setContextMenuPolicy(Qt::CustomContextMenu);
            rl->addWidget(power);
            rl->addWidget(solo);
            rl->addWidget(name, 1);
            const std::vector<ParamDef>& defs = rackChainParams(audio);
            for (const ParamDef& d : defs)
                rl->addWidget(new ParamControl(m_view, DeviceChainView::ParamRef{m_path, m_rackIndex, i}, d, true));
            l->addWidget(row);
            QObject::connect(power, &QToolButton::clicked, this, [this, i](bool on) { m_view->setChainFlag(m_path, m_rackIndex, i, false, on); });
            QObject::connect(solo, &QToolButton::clicked, this, [this, i](bool on) { m_view->setChainFlag(m_path, m_rackIndex, i, true, on); });
            QObject::connect(name, &QPushButton::clicked, this, [this, rackId, i] { m_view->selectChain(rackId, i); });
            QObject::connect(name, &QWidget::customContextMenuRequested, this, [this, name, i](const QPoint& pos) {
                QMenu menu;
                menu.addAction(QObject::tr("Rename..."), [this, i] { m_view->renameChain(m_path, m_rackIndex, i); });
                menu.addAction(QObject::tr("Duplicate Chain"), [this, i] { m_view->duplicateChain(m_path, m_rackIndex, i); });
                menu.addAction(QObject::tr("Delete Chain"), [this, i] { m_view->removeChain(m_path, m_rackIndex, i); });
                menu.exec(name->mapToGlobal(pos));
            });
        }
        auto* hint = new QLabel(rack.chains.empty() ? QObject::tr("Drop effects here: each one starts a parallel chain")
                                                    : QObject::tr("Drop here for a new parallel chain"));
        hint->setWordWrap(true);
        hint->setAlignment(Qt::AlignCenter);
        theme::style(hint, QStringLiteral("color: @faint; font-size: 8pt; border: 1px dashed @line; padding: 3px;"));
        l->addWidget(hint);
        auto* add = new QPushButton(QObject::tr("+ Chain"));
        add->setFocusPolicy(Qt::NoFocus);
        add->setToolTip(QObject::tr("Add an empty parallel chain"));
        l->addWidget(add, 0, Qt::AlignLeft);
        l->addStretch();
        QObject::connect(add, &QPushButton::clicked, this, [this] { m_view->addChain(m_path, m_rackIndex); });
    }

protected:
    void dragEnterEvent(QDragEnterEvent* e) override
    {
        if (m_view->canDrop(e->mimeData()))
            e->acceptProposedAction();
    }
    void dragMoveEvent(QDragMoveEvent* e) override
    {
        if (m_view->canDrop(e->mimeData())) {
            e->acceptProposedAction();
            m_hover = true;
            update();
        }
    }
    void dragLeaveEvent(QDragLeaveEvent*) override
    {
        m_hover = false;
        update();
    }
    void dropEvent(QDropEvent* e) override
    {
        m_hover = false;
        update();
        const bool copy = e->modifiers() & Qt::ControlModifier; // Ctrl+drag copies
        if (m_view->dropAsNewChain(e->mimeData(), m_path, m_rackIndex, copy)) {
            const bool move = !copy && e->mimeData()->hasFormat(QString::fromLatin1(DeviceChainView::kDeviceMime));
            e->setDropAction(move ? Qt::MoveAction : Qt::CopyAction);
            e->accept();
        }
    }
    void paintEvent(QPaintEvent*) override
    {
        if (!m_hover)
            return;
        QPainter p(this);
        p.setPen(QPen(theme::c().key, 2));
        p.drawRect(rect().adjusted(1, 1, -1, -1));
    }

private:
    DeviceChainView* m_view;
    ChainPath m_path;
    int m_rackIndex;
    bool m_hover = false;
};

// ---------------------------------------------------------------------------
// DeviceWidget: title bar + controls (or, for a rack, its chain list, its
// Dry/Wet and the devices of the chain shown).
// ---------------------------------------------------------------------------

class DeviceWidget : public QFrame {
public:
    // Devices nested in a rack keep the standard height (the rack grows to fit them).
    DeviceWidget(DeviceChainView* view, const ChainPath& path, int index, const EffectInstance& e, bool nested)
        : m_view(view), m_effectId(e.id), m_user(e.type.startsWith(QLatin1String("user.")))
    {
        const int bodyHeight = nested ? kBodyHeight : view->m_bodyHeight;
        const bool selected = view->m_selected.count(e.id) > 0;
        const bool folded = view->m_folded.count(e.id) > 0;
        setFrameShape(QFrame::Box);
        setAutoFillBackground(true);
        QPalette pal = palette();
        pal.setColor(QPalette::Window, e.isRack() ? theme::c().window : theme::c().raised);
        pal.setColor(QPalette::WindowText, selected ? theme::c().key : theme::c().deep);
        setPalette(pal);
        setLineWidth(selected ? 2 : 1);
        auto* l = new QVBoxLayout(this);
        l->setContentsMargins(1, 1, 1, 1);
        l->setSpacing(0);

        const auto meta = e.isRack() ? std::nullopt : effectMeta(e.type);
        QString name = e.isRack() ? (view->isAudio() ? QObject::tr("Audio Effect Rack") : QObject::tr("Video Effect Rack"))
                                  : (meta ? meta->name : QObject::tr("Unknown (%1)").arg(e.type));
        if (e.type == QLatin1String("a_vst3")) { // the plugin's name
            auto text = [&](const char* k) { auto it = e.params.find(QString::fromLatin1(k)); return it == e.params.end() ? QString() : it->second.text; };
            const QString plugin = !text("class").isEmpty() ? text("class") : QFileInfo(text("plugin")).completeBaseName();
            if (!plugin.isEmpty())
                name = QObject::tr("VST3: %1").arg(plugin);
        }
        auto* title = new DeviceTitle(view, path, index, e, name, selected);
        if (meta)
            title->setToolTip(meta->description + QStringLiteral("\n\n") + title->toolTip());
        l->addWidget(title);
        if (folded) {
            setFixedWidth(std::max(140, title->sizeHint().width()));
            l->addStretch();
            return;
        }
        auto* body = new QWidget;
        // Racks grow to fit what they hold (nested racks); devices have a fixed height.
        if (e.isRack())
            body->setMinimumHeight(kBodyHeight);
        else
            body->setFixedHeight(bodyHeight);
        l->addWidget(body);
        if (!e.enabled)
            body->setEnabled(false);

        if (e.isRack()) {
            auto* bl = new QHBoxLayout(body);
            bl->setContentsMargins(4, 4, 4, 4);
            bl->setSpacing(6);
            int selectedChain = 0;
            if (auto it = view->m_rackChain.find(e.id); it != view->m_rackChain.end())
                selectedChain = it->second;
            selectedChain = std::clamp(selectedChain, 0, std::max(0, int(e.chains.size()) - 1));
            bl->addWidget(new RackChainList(view, path, index, e, selectedChain, view->isAudio()));
            auto* macro = new QVBoxLayout;
            for (const ParamDef& d : rackParams())
                macro->addWidget(new ParamControl(view, DeviceChainView::ParamRef{path, index, -1}, d, false));
            macro->addStretch();
            bl->addLayout(macro);
            if (!e.chains.empty()) {
                auto* inner = new ChainStrip(view, path.child(index, selectedChain), e.chains[size_t(selectedChain)].effects, true);
                inner->setAutoFillBackground(true);
                QPalette ip = inner->palette();
                ip.setColor(QPalette::Window, theme::c().panel);
                inner->setPalette(ip);
                inner->setMinimumWidth(200);
                bl->addWidget(inner, 1);
            }
            return;
        }
        auto* bodyRow = new QHBoxLayout(body);
        bodyRow->setContentsMargins(0, 0, 0, 0);
        bodyRow->setSpacing(0);
        if (meta && meta->params && !m_user) {
            // The settings as a picture (an EQ's curve, a compressor's transfer curve...), left of the controls.
            const DeviceChainView::ParamRef ref{path, index, -1};
            const std::vector<ParamDef> defs = *meta->params;
            auto get = [view, ref, defs](const QString& id) {
                const ParamMap* map = view->params(ref);
                for (const ParamDef& d : defs)
                    if (d.id == id) {
                        auto it = map ? map->find(id) : ParamMap::const_iterator();
                        return map && it != map->end() ? it->second.at(view->keyTime(), d.type) : d.def;
                    }
                return 0.0;
            };
            auto set = [view, ref](const QString& id, double v) { view->setParam(ref, id, v); };
            if (ParamDisplay* display = ParamDisplay::create(e.type, defs, get, set)) {
                bodyRow->addSpacing(4);
                bodyRow->addWidget(display);
                view->m_displays.push_back(display);
                if (!e.enabled)
                    display->setEnabled(false);
            }
        }
        auto* controls = new QWidget;
        bodyRow->addWidget(controls, 1);
        auto* grid = new QGridLayout(controls);
        grid->setContentsMargins(6, 4, 6, 4);
        grid->setHorizontalSpacing(6);
        grid->setVerticalSpacing(4);
        int row = 0, col = 0, used = 0;
        if (m_user) {
            // Made in the Effect Builder: its graph is changed there, not here.
            auto* warn = new QLabel(QObject::tr("\u26A0 Double-click this effect to change it in the Effect Builder"));
            warn->setObjectName(QStringLiteral("userEffectHint"));
            theme::style(warn, QStringLiteral("color: @warn; font-weight: bold;"));
            grid->addWidget(warn, row++, col, Qt::AlignTop | Qt::AlignHCenter);
            used += 46;
            setToolTip(QObject::tr("Double-click to change this effect in the Effect Builder (Tools)"));
        }
        const bool anyShown = meta && meta->params && std::any_of(meta->params->begin(), meta->params->end(), [](const ParamDef& d) { return !d.hidden; });
        if (!anyShown && m_user) {
            grid->setRowStretch(grid->rowCount(), 1);
            return;
        }
        if (!meta || !meta->params || meta->params->empty()) {
            grid->addWidget(new QLabel(QObject::tr("No settings")), 0, 0);
            return;
        }
        // Column-major: fill each column down to the body height, by the
        // controls' real heights (knobs are tall, menus and toggles short).
        const int avail = bodyHeight - 8;
        std::vector<ParamDef> defs = *meta->params;
        const auto shape = e.params.find(QStringLiteral("mask.shape"));
        const bool masked = !view->isAudio() && shape != e.params.end() && shape->second.value >= 0.5;
        if (masked) // the mask's settings after the effect's own
            defs.insert(defs.end(), effectMaskParams().begin(), effectMaskParams().end());
        for (const ParamDef& d : defs) {
            if (d.hidden)
                continue;
            const bool wide = d.type == ParamType::Text || d.type == ParamType::Font;
            auto* c = new ParamControl(view, DeviceChainView::ParamRef{path, index, -1}, d, false);
            const int need = c->sizeHint().height(); // (knobs with long names are taller)
            if (used > 0 && (used + need > avail || wide)) {
                row = 0;
                used = 0;
                ++col;
            }
            grid->addWidget(c, row, col, 1, wide ? 2 : 1, Qt::AlignTop | Qt::AlignHCenter);
            ++row;
            used += need + 4;
            if (wide) {
                col += 2;
                row = 0;
                used = 0;
            }
        }
        if (masked) {
            auto* track = new QPushButton(QObject::tr("Track Motion"));
            track->setToolTip(QObject::tr("Follow what is inside the mask from the cursor to the end of the event:\n"
                                          "keyframes the mask's position on every frame (Event FX of a video event)"));
            track->setEnabled(view->m_target.kind == FxTarget::Kind::Clip);
            QObject::connect(track, &QPushButton::clicked, view, [view, path, index] { view->trackMask(path, index); });
            grid->addWidget(track, row, col, Qt::AlignTop | Qt::AlignHCenter);
        }
        if (e.type == QLatin1String("corner_pin")) {
            auto* track = new QPushButton(QObject::tr("Track Corners"));
            track->setObjectName(QStringLiteral("trackCorners"));
            track->setToolTip(QObject::tr("Put the corners on the four corners of a screen or sign in the shot under this event (on a "
                                          "lower track), then follow them from the cursor on: keyframes on every frame (Event FX)"));
            track->setEnabled(view->m_target.kind == FxTarget::Kind::Clip);
            QObject::connect(track, &QPushButton::clicked, view, [view, path, index] { view->trackCorners(path, index); });
            grid->addWidget(track, row, col, Qt::AlignTop | Qt::AlignHCenter);
        }
        if (e.type == QLatin1String("white_balance")) {
            // Set from the event: Auto, or a click on something that should be grey or white.
            const bool onEvent = view->m_target.kind == FxTarget::Kind::Clip;
            auto* buttons = new QVBoxLayout;
            auto* autoButton = new QPushButton(QObject::tr("Auto"));
            autoButton->setObjectName(QStringLiteral("autoWhiteBalance"));
            autoButton->setToolTip(QObject::tr("Black and white points and the neutral from the event at the cursor (Event FX)"));
            autoButton->setEnabled(onEvent);
            QObject::connect(autoButton, &QPushButton::clicked, view, [view, path, index] { view->autoWhiteBalance(path, index); });
            auto* pick = new QPushButton(QObject::tr("Pick Neutral"));
            pick->setObjectName(QStringLiteral("pickNeutral"));
            pick->setToolTip(QObject::tr("Then click something on the preview that should be grey or white: it is made so (Event FX)"));
            pick->setEnabled(onEvent);
            QObject::connect(pick, &QPushButton::clicked, view, [view, path, index] { view->pickNeutral(path, index); });
            buttons->addWidget(autoButton);
            buttons->addWidget(pick);
            buttons->addStretch();
            grid->addLayout(buttons, 0, col + 1, -1, 1, Qt::AlignTop);
        }
        grid->setRowStretch(grid->rowCount(), 1);
    }

protected:
    void mouseDoubleClickEvent(QMouseEvent* e) override
    {
        if (m_user)
            emit m_view->userEffectRequested(m_effectId);
        else
            QFrame::mouseDoubleClickEvent(e);
    }

private:
    DeviceChainView* m_view;
    int m_effectId;
    bool m_user;
};

ChainStrip::ChainStrip(DeviceChainView* view, ChainPath path, const EffectChain& chain, bool nested)
    : m_view(view), m_path(std::move(path))
{
    setAcceptDrops(true);
    auto* l = new QHBoxLayout(this);
    l->setContentsMargins(6, 4, 24, 4);
    l->setSpacing(8);
    for (int i = 0; i < int(chain.size()); ++i) {
        auto* d = new DeviceWidget(view, m_path, i, chain[size_t(i)], nested);
        m_devices.push_back(d);
        l->addWidget(d, 0, Qt::AlignTop);
    }
    if (chain.empty()) {
        auto* hint = new QLabel(nested ? QObject::tr("Drop effects here")
                                       : (view->isAudio() ? QObject::tr("Drag audio effects here from the Audio FX tab (or use Add)")
                                                          : QObject::tr("Drag video effects here from the Video FX tab (or use Add)")));
        theme::style(hint, QStringLiteral("color: @faint; border: 1px dashed @line; padding: 12px;"));
        l->addWidget(hint, 0, Qt::AlignVCenter);
    }
    l->addStretch();
    m_indicator = new QFrame(this);
    theme::style(m_indicator, QStringLiteral("background: @key;"));
    m_indicator->hide();
}

// ---------------------------------------------------------------------------
// DeviceChainView
// ---------------------------------------------------------------------------

DeviceChainView::DeviceChainView(Project* project, std::function<TimeUs()> cursor, QWidget* parent)
    : QWidget(parent), m_project(project), m_cursor(std::move(cursor))
{
    setObjectName(QStringLiteral("deviceChainView"));
    setFocusPolicy(Qt::ClickFocus);
    auto* l = new QVBoxLayout(this);
    l->setContentsMargins(0, 0, 0, 0);
    l->setSpacing(0);
    auto* header = new QWidget;
    header->setObjectName(QStringLiteral("chainHeader"));
    header->setAttribute(Qt::WA_StyledBackground, true);
    theme::style(header, QStringLiteral("QWidget#chainHeader { background: @panel; }"));
    auto* hl = new QHBoxLayout(header);
    hl->setContentsMargins(6, 2, 6, 2);
    // Event / Track / Output tabs.
    auto scopeButton = [&](const QString& text, const QString& tip) {
        auto* b = new QToolButton;
        b->setText(text);
        b->setToolTip(tip);
        b->setCheckable(true);
        b->setAutoExclusive(false);
        b->setFocusPolicy(Qt::NoFocus);
        theme::style(b, QStringLiteral("QToolButton { padding: 2px 10px; border: 1px solid @deep; background: @raised; color: @text; }"
                                        "QToolButton:checked { background: @accent; color: @onAccent; }"
                                        "QToolButton:disabled { color: @faint; }"));
        hl->addWidget(b);
        return b;
    };
    m_scopeEvent = scopeButton(tr("Event"), tr("Effects on the selected event only"));
    m_scopeTrack = scopeButton(tr("Track"), tr("Track FX: effects on everything on the event's track"));
    m_scopeMain = scopeButton(tr("Output"), tr("Effects on the final picture (video output) or the final mix (audio master)"));
    hl->addSpacing(8);
    m_title = new QLabel;
    theme::style(m_title, QStringLiteral("font-weight: bold; color: @bright;"));
    hl->addWidget(m_title);
    m_addButton = new QToolButton;
    m_addButton->setObjectName(QStringLiteral("addDevice"));
    m_addButton->setText(tr("Add"));
    m_addButton->setPopupMode(QToolButton::InstantPopup);
    hl->addWidget(m_addButton);
    auto* saveChain = new QToolButton;
    saveChain->setText(tr("Save Chain"));
    saveChain->setToolTip(tr("Save every device in this chain as a chain preset"));
    connect(saveChain, &QToolButton::clicked, this, [this] {
        if (const EffectChain* r = root(m_project->timeline()); r && !r->empty())
            saveChainPreset(*r);
    });
    m_groupButton = new QToolButton;
    m_groupButton->setText(tr("Group"));
    m_groupButton->setToolTip(tr("Group the selected devices into a rack (Ctrl+G); Ctrl+click devices to select several"));
    hl->addWidget(m_groupButton);
    hl->addWidget(saveChain);
    m_clearLiveButton = new QToolButton;
    m_clearLiveButton->setObjectName(QStringLiteral("clearLiveFx"));
    m_clearLiveButton->setText(tr("Clear All"));
    m_clearLiveButton->setToolTip(tr("Remove every effect and rack from Live FX (always on). Undo with Ctrl+Z."));
    connect(m_clearLiveButton, &QToolButton::clicked, this, [this] {
        if (m_target.kind == FxTarget::Kind::Live && m_target.id == 0)
            editRoot([](EffectChain& chain, Timeline&) { chain.clear(); }, tr("Clear Live FX"));
    });
    hl->addWidget(m_clearLiveButton);
    auto* hint = new QLabel(tr("Drag effects in front of / behind a device or into a rack · drop on a rack's chain list for a "
                               "parallel chain · Ctrl+drag copies · ◇ keyframes"));
    theme::style(hint, QStringLiteral("color: @faint; font-size: 8pt;"));
    hint->setToolTip(hint->text());
    hint->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred); // clipped in narrow panels, not widening them
    hl->addWidget(hint, 1);
    l->addWidget(header);

    m_scroll = new QScrollArea;
    m_scroll->setWidgetResizable(true);
    m_scroll->setFrameShape(QFrame::NoFrame);
    m_scroll->setVerticalScrollBarPolicy(Qt::ScrollBarAsNeeded);
    l->addWidget(m_scroll, 1);
    setMinimumHeight(kBodyHeight + 70);

    auto buildMenu = [this](QMenu* menu, const auto& list, bool audio) -> std::map<QString, QMenu*> {
        std::map<QString, QMenu*> categories;
        for (const auto& e : list) {
            // "Parent/Child" categories become nested sub-menus.
            QMenu* sub = menu;
            QString path;
            for (const QString& part : QString(e.category).split(QLatin1Char('/'))) {
                path += (path.isEmpty() ? QString() : QStringLiteral("/")) + part;
                QMenu*& m = categories[path];
                if (!m)
                    m = sub->addMenu(QString(part).replace(QLatin1Char('&'), QStringLiteral("&&")));
                sub = m;
            }
            QAction* a = sub->addAction(QString(e.name).replace(QLatin1Char('&'), QStringLiteral("&&")));
            a->setToolTip(e.description);
            connect(a, &QAction::triggered, this, [this, id = e.id] { addEffect(id); });
        }
        menu->addSeparator();
        menu->addAction(audio ? tr("Audio Effect Rack (empty)") : tr("Video Effect Rack (empty)"), this, [this] {
            editRoot([this](EffectChain& root, Timeline& tl) {
                EffectInstance rack = makeRack(tl.allocId());
                rack.chains.push_back(makeRackChain(isAudio(), tr("Chain 1")));
                root.push_back(std::move(rack));
            }, tr("Add Rack"));
        });
        return categories;
    };
    m_videoMenu = new QMenu(this);
    const auto videoCategories = std::make_shared<const std::map<QString, QMenu*>>(buildMenu(m_videoMenu, videoEffects(), false));
    QMenu* userMenu = new QMenu(tr("User Created"), m_videoMenu);
    m_videoMenu->insertMenu(m_videoMenu->actions().value(m_videoMenu->actions().size() - 2), userMenu);
    // The Effect Builder's examples, put in their categories each time the menu opens (they can change): Node
    // Editor, or a category of the effects' own ("Broken VGS/Analogue Circuit Emu" joins the console effects).
    const auto exampleItems = std::make_shared<std::vector<QPointer<QObject>>>();
    connect(m_videoMenu, &QMenu::aboutToShow, this, [this, videoCategories, exampleItems, userMenu] {
        for (const QPointer<QObject>& o : *exampleItems)
            delete o.data();
        exampleItems->clear();
        std::map<QString, QMenu*> made;
        for (const EffectDef* e : nodes::userEffects()) {
            if (e->category == QLatin1String("User Created"))
                continue;
            QMenu* into = m_videoMenu;
            QString path;
            for (const QString& part : e->category.split(QLatin1Char('/'))) {
                path += (path.isEmpty() ? QString() : QStringLiteral("/")) + part;
                if (const auto it = videoCategories->find(path); it != videoCategories->end()) {
                    into = it->second;
                    continue;
                }
                QMenu*& m = made[path];
                if (!m) {
                    m = new QMenu(QString(part).replace(QLatin1Char('&'), QStringLiteral("&&")), into);
                    if (into == m_videoMenu)
                        m_videoMenu->insertMenu(userMenu->menuAction(), m); // (top level: before User Created)
                    else
                        into->addMenu(m);
                    exampleItems->push_back(m);
                }
                into = m;
            }
            QAction* a = into->addAction(QString(e->name).replace(QLatin1Char('&'), QStringLiteral("&&")));
            a->setToolTip(e->description);
            exampleItems->push_back(a);
            connect(a, &QAction::triggered, this, [this, id = e->id] { addEffect(id); });
        }
    });
    connect(userMenu, &QMenu::aboutToShow, this, [this, userMenu] {
        userMenu->clear();
        for (const EffectDef* e : nodes::userEffects()) {
            if (e->category != QLatin1String("User Created")) // (the examples are in their own menus)
                continue;
            QAction* a = userMenu->addAction(QString(e->name).replace(QLatin1Char('&'), QStringLiteral("&&")));
            a->setToolTip(e->description);
            connect(a, &QAction::triggered, this, [this, id = e->id] { addEffect(id); });
        }
        if (userMenu->isEmpty())
            userMenu->addAction(tr("(none yet: Tools > Effect Builder)"))->setEnabled(false);
    });
    // Screen shaders, by folder (read when opened: they come with Tools > Get Screen Shaders).
    QMenu* retroMenu = new QMenu(tr("Retro Shaders"), m_videoMenu);
    m_videoMenu->insertMenu(m_videoMenu->actions().value(m_videoMenu->actions().size() - 2), retroMenu);
    connect(retroMenu, &QMenu::aboutToShow, this, [this, retroMenu] {
        retroMenu->clear();
        std::map<QString, QMenu*> folders;
        for (const EffectDef* e : retro::effects()) {
            const QString sub = e->category.section(QLatin1Char('/'), 1);
            QMenu*& into = folders[sub];
            if (!into)
                into = sub.isEmpty() ? retroMenu : retroMenu->addMenu(QString(sub).replace(QLatin1Char('&'), QStringLiteral("&&")));
            QAction* a = into->addAction(QString(e->name).replace(QLatin1Char('&'), QStringLiteral("&&")));
            a->setToolTip(e->description);
            connect(a, &QAction::triggered, this, [this, id = e->id] { addEffect(id); });
        }
        if (retroMenu->isEmpty())
            retroMenu->addAction(tr("(none yet: Tools > Get Screen Shaders)"))->setEnabled(false);
    });
    m_audioMenu = new QMenu(this);
    buildMenu(m_audioMenu, audioEffects(), true);
    // Preset Chains at the top, as the FX browsers have them: read when opened, so a chain just saved is there.
    for (QMenu* menu : {m_videoMenu, m_audioMenu}) {
        const bool audio = menu == m_audioMenu;
        QMenu* chains = new QMenu(tr("Preset Chains"), menu);
        menu->insertMenu(menu->actions().value(0), chains);
        menu->insertSeparator(menu->actions().value(1));
        connect(chains, &QMenu::aboutToShow, this, [this, chains, audio] {
            chains->clear();
            std::map<QString, QMenu*> groups;
            for (const Preset& p : PresetLibrary::instance().chains(audio)) {
                QMenu*& group = groups[p.category.isEmpty() ? tr("Other") : p.category];
                if (!group)
                    group = chains->addMenu(QString(p.category.isEmpty() ? tr("Other") : p.category).replace(QLatin1Char('&'), QStringLiteral("&&")));
                QAction* a = group->addAction(QString(p.name).replace(QLatin1Char('&'), QStringLiteral("&&")));
                a->setToolTip(p.description);
                connect(a, &QAction::triggered, this, [this, id = p.id] { addPreset(id); });
            }
            if (chains->isEmpty())
                chains->addAction(tr("(none yet: Save Chain makes one)"))->setEnabled(false);
        });
    }
    connect(m_groupButton, &QToolButton::clicked, this, &DeviceChainView::groupSelection);
    connect(m_scopeEvent, &QToolButton::clicked, this, [this] {
        if (m_contextClip)
            setTarget({FxTarget::Kind::Clip, m_contextClip});
        updateScopeTabs();
    });
    connect(m_scopeTrack, &QToolButton::clicked, this, [this] {
        if (m_contextTrack)
            setTarget({FxTarget::Kind::Track, m_contextTrack});
        updateScopeTabs();
    });
    connect(m_scopeMain, &QToolButton::clicked, this, [this] {
        // The audio master when the context is audio, else the video output.
        bool audio = m_target.kind == FxTarget::Kind::AudioMaster;
        for (const auto& t : m_project->timeline().tracks)
            if (t.id == m_contextTrack)
                audio = t.type == TrackType::Audio;
        setTarget({audio ? FxTarget::Kind::AudioMaster : FxTarget::Kind::Output, 0});
        updateScopeTabs();
    });
    auto* group = new QShortcut(QKeySequence(QStringLiteral("Ctrl+G")), this, nullptr, nullptr, Qt::WidgetWithChildrenShortcut);
    connect(group, &QShortcut::activated, this, &DeviceChainView::groupSelection);

    m_commitTimer.setSingleShot(true);
    m_commitTimer.setInterval(700);
    connect(&m_commitTimer, &QTimer::timeout, this, &DeviceChainView::flushEdit);
    m_rebuildTimer.setSingleShot(true);
    m_rebuildTimer.setInterval(0);
    connect(&m_rebuildTimer, &QTimer::timeout, this, [this] {
        if (structureKey() != m_key)
            rebuild();
        else
            refreshValues();
    });
    connect(m_project, &Project::changed, this, [this] {
        if (m_applying)
            return;
        // Changed elsewhere (undo, timeline edit): drop any pending burst.
        m_commitTimer.stop();
        m_editBefore.reset();
        scheduleRebuild();
    });
    rebuild();
}

DeviceChainView::~DeviceChainView()
{
    if (m_preview)
        m_preview->setOverlay(nullptr);
}

// ---- Target access -----------------------------------------------------------------

bool DeviceChainView::isAudio() const
{
    const Timeline& tl = m_project->timeline();
    switch (m_target.kind) {
    case FxTarget::Kind::AudioMaster: return true;
    case FxTarget::Kind::Clip: {
        int ti = -1;
        return tl.findClip(m_target.id, &ti) && tl.tracks[size_t(ti)].type == TrackType::Audio;
    }
    case FxTarget::Kind::Track:
        for (const auto& t : tl.tracks)
            if (t.id == m_target.id)
                return t.type == TrackType::Audio;
        return false;
    default: return false;
    }
}

EffectChain* DeviceChainView::root(Timeline& tl) const
{
    switch (m_target.kind) {
    case FxTarget::Kind::Clip:
        if (Clip* c = tl.findClip(m_target.id))
            return &c->effects;
        return nullptr;
    case FxTarget::Kind::Track:
        for (auto& t : tl.tracks)
            if (t.id == m_target.id)
                return &t.effects;
        return nullptr;
    case FxTarget::Kind::Output: return &tl.outputEffects;
    case FxTarget::Kind::AudioMaster: return &tl.audioMasterEffects;
    case FxTarget::Kind::Live: // id 0: the Live FX chain; 1..16: a pad; 17..20: a layer's FX
        if (m_target.id == 0)
            return &tl.liveEffects;
        if (m_target.id >= 1 && m_target.id <= kLivePads)
            return &tl.livePads[size_t(m_target.id - 1)].effects;
        if (m_target.id > kLivePads && m_target.id <= kLivePads + kLiveLayers)
            return &tl.liveLayerEffects[size_t(m_target.id - kLivePads - 1)];
        return nullptr;
    default: return nullptr;
    }
}

const EffectChain* DeviceChainView::root(const Timeline& tl) const
{
    return root(const_cast<Timeline&>(tl));
}

bool DeviceChainView::targetExists() const
{
    return root(m_project->timeline()) != nullptr;
}

const EffectInstance* DeviceChainView::effectAt(const ChainPath& path, int index) const
{
    const EffectChain* r = root(m_project->timeline());
    const EffectChain* c = r ? resolveChain(*r, path) : nullptr;
    return c && index >= 0 && index < int(c->size()) ? &(*c)[size_t(index)] : nullptr;
}

ParamMap* DeviceChainView::params(Timeline& tl, const ParamRef& ref) const
{
    EffectChain* r = root(tl);
    EffectChain* c = r ? resolveChain(*r, ref.path) : nullptr;
    if (!c || ref.index < 0 || ref.index >= int(c->size()))
        return nullptr;
    EffectInstance& e = (*c)[size_t(ref.index)];
    if (ref.chain < 0)
        return &e.params;
    return ref.chain < int(e.chains.size()) ? &e.chains[size_t(ref.chain)].params : nullptr;
}

const ParamMap* DeviceChainView::params(const ParamRef& ref) const
{
    return params(const_cast<Timeline&>(m_project->timeline()), ref);
}

TimeUs DeviceChainView::keyTime() const
{
    const TimeUs cursor = m_cursor();
    if (m_target.kind == FxTarget::Kind::Clip)
        if (const Clip* c = m_project->timeline().findClip(m_target.id))
            return std::clamp<TimeUs>(cursor - c->start, 0, c->length);
    return cursor;
}

TimeUs DeviceChainView::keyToTimeline(TimeUs key) const
{
    if (m_target.kind == FxTarget::Kind::Clip)
        if (const Clip* c = m_project->timeline().findClip(m_target.id))
            return c->start + key;
    return key;
}

TimeUs DeviceChainView::keyTolerance() const
{
    return m_project->settings().fps.frameDuration() / 2;
}

// ---- Public API -----------------------------------------------------------------------

void DeviceChainView::setTarget(const FxTarget& target)
{
    const FxTarget t = target.isChain() ? target : FxTarget{};
    emit targetChanged(t);
    // Remember the event / track context for the scope tabs.
    const Timeline& tl = m_project->timeline();
    if (t.kind == FxTarget::Kind::Clip) {
        int ti = -1;
        if (tl.findClip(t.id, &ti)) {
            m_contextClip = t.id;
            m_contextTrack = tl.tracks[size_t(ti)].id;
        }
    } else if (t.kind == FxTarget::Kind::Track) {
        if (m_contextTrack != t.id)
            m_contextClip = 0; // the remembered event is on another track
        m_contextTrack = t.id;
    }
    if (t == m_target) {
        updateScopeTabs();
        return;
    }
    flushEdit();
    m_target = t;
    m_selected.clear();
    rebuild();
}

bool DeviceChainView::acceptsEffect(const QString& type) const
{
    const auto meta = effectMeta(type);
    return meta && targetExists() && meta->audio == isAudio();
}

void DeviceChainView::addEffect(const QString& type)
{
    const auto meta = effectMeta(type);
    if (!meta || !acceptsEffect(type))
        return;
    editRoot([&](EffectChain& r, Timeline& tl) { r.push_back(makeEffectOfType(type, tl.allocId())); }, tr("Add %1").arg(meta->name));
}

void DeviceChainView::onCursorMoved()
{
    refreshValues();
}

// ---- Editing ---------------------------------------------------------------------------

void DeviceChainView::edit(const std::function<void(Timeline&)>& fn, const QString& label)
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

void DeviceChainView::editNow(const std::function<void(Timeline&)>& fn, const QString& label)
{
    flushEdit();
    Timeline tl = m_project->timeline();
    fn(tl);
    if (tl == m_project->timeline())
        return;
    m_applying = true;
    m_project->applyEdit(tl, label);
    m_applying = false;
    scheduleRebuild();
}

void DeviceChainView::editRoot(const std::function<void(EffectChain&, Timeline&)>& fn, const QString& label)
{
    editNow([&](Timeline& tl) {
        if (EffectChain* r = root(tl))
            fn(*r, tl);
    }, label);
}

void DeviceChainView::flushEdit()
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

void DeviceChainView::setParam(const ParamRef& ref, const QString& id, double value, bool key)
{
    const TimeUs t = keyTime();
    const TimeUs tol = keyTolerance();
    edit([&](Timeline& tl) {
        ParamMap* map = params(tl, ref);
        if (!map)
            return;
        ParamValue& pv = (*map)[id];
        if (pv.keys.empty() && !key) {
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

// ---------------------------------------------------------------------------
// MaskOverlay: an effect's mask on the preview: drag inside to move it, near
// a corner to resize it (keyframed at the cursor when the mask is animated).
// ---------------------------------------------------------------------------

class MaskOverlay : public PreviewOverlay {
public:
    MaskOverlay(DeviceChainView* view, ChainPath path, int index) : m_view(view), m_path(std::move(path)), m_index(index) {}
    const ChainPath& path() const { return m_path; }
    int index() const { return m_index; }

    void paint(QPainter& p, const QRectF& picture) override
    {
        const EffectMask m = mask(picture);
        if (!m.active())
            return;
        p.save();
        p.translate(picture.topLeft());
        if (m.shape == 3) { // freehand: the outline and its points
            for (int pass = 0; pass < 2; ++pass) {
                p.setPen(QPen(pass ? QColor(0x40, 0xd0, 0xff) : QColor(0, 0, 0, 160), pass ? 1.5 : 3.5, pass ? Qt::DashLine : Qt::SolidLine));
                p.setBrush(Qt::NoBrush);
                p.drawPath(m.outline);
            }
            p.setPen(QPen(Qt::black, 1));
            p.setBrush(m.animate ? QColor(0xff, 0xb0, 0x40) : QColor(0x40, 0xd0, 0xff));
            for (const QPointF& c : m.points)
                p.drawEllipse(c, 5, 5);
            if (m.invert) {
                p.setPen(QColor(0x40, 0xd0, 0xff));
                p.drawText(m.outline.boundingRect(), Qt::AlignCenter, QObject::tr("outside"));
            }
            p.restore();
            return;
        }
        p.translate(m.cx, m.cy);
        p.rotate(std::atan2(m.sn, m.cs) * 180.0 / std::numbers::pi);
        const QRectF box(-m.hw, -m.hh, 2 * m.hw, 2 * m.hh);
        for (int pass = 0; pass < 2; ++pass) { // dark under light: visible on anything
            p.setPen(QPen(pass ? QColor(0x40, 0xd0, 0xff) : QColor(0, 0, 0, 160), pass ? 1.5 : 3.5, pass ? Qt::DashLine : Qt::SolidLine));
            p.setBrush(Qt::NoBrush);
            if (m.shape == 1)
                p.drawRect(box);
            else
                p.drawEllipse(box);
        }
        if (m.feather > 1) { // the soft edge, faintly
            p.setPen(QPen(QColor(0x40, 0xd0, 0xff, 90), 1, Qt::DotLine));
            const QRectF outer = box.adjusted(-m.feather / 2, -m.feather / 2, m.feather / 2, m.feather / 2);
            m.shape == 1 ? p.drawRect(outer) : p.drawEllipse(outer);
        }
        p.setPen(QPen(Qt::black, 1));
        p.setBrush(QColor(0x40, 0xd0, 0xff));
        for (const QPointF& c : {box.topLeft(), box.topRight(), box.bottomLeft(), box.bottomRight()})
            p.drawRect(QRectF(c - QPointF(4, 4), QSizeF(8, 8)));
        if (m.invert) {
            p.setPen(QColor(0x40, 0xd0, 0xff));
            p.drawText(box, Qt::AlignCenter, QObject::tr("outside"));
        }
        p.restore();
    }

    bool press(QPointF pos, const QRectF& picture, Qt::KeyboardModifiers mods) override
    {
        const EffectMask m = mask(picture);
        if (!m.active())
            return false;
        if (m.shape == 3) {
            const QPointF at = pos - picture.topLeft();
            if (mods & Qt::AltModifier) {
                // Alt+click: a point goes; on the outline, a new point there.
                const DeviceChainView::ParamRef ref{m_path, m_index, -1};
                const int n = int(m.points.size());
                for (int i = 0; i < n; ++i)
                    if (QLineF(at, m.points[size_t(i)]).length() < 9) {
                        m_view->editMaskPoints(ref, i, std::nullopt);
                        return true;
                    }
                int best = -1;
                double bestDist = 12;
                for (int i = 0; i < n; ++i) {
                    const QPointF a = m.points[size_t(i)], b = m.points[size_t((i + 1) % n)], ab = b - a;
                    const double t = std::clamp(QPointF::dotProduct(at - a, ab) / std::max(1e-9, QPointF::dotProduct(ab, ab)), 0.0, 1.0);
                    const double d = QLineF(at, a + ab * t).length();
                    if (d < bestDist) {
                        bestDist = d;
                        best = i;
                    }
                }
                if (best >= 0) {
                    const QPointF q = local(m, pos, picture);
                    m_view->editMaskPoints(ref, best, QPointF(q.x() / std::max(2.0 * m.hw, 1.0), q.y() / std::max(2.0 * m.hh, 1.0)));
                }
                return best >= 0;
            }
            m_point = -1;
            for (int i = 0; i < int(m.points.size()); ++i)
                if (QLineF(at, m.points[size_t(i)]).length() < 9)
                    m_point = i;
            m_mode = m_point >= 0 ? Mode::Point : (m.outline.contains(at) ? Mode::Move : Mode::None);
            m_start = pos;
            m_startCentre = QPointF(m.cx / picture.width(), m.cy / picture.height());
            return m_mode != Mode::None;
        }
        const QPointF q = local(m, pos, picture);
        m_mode = std::abs(std::abs(q.x()) - m.hw) < 9 && std::abs(std::abs(q.y()) - m.hh) < 9 ? Mode::Resize
                 : std::abs(q.x()) <= m.hw && std::abs(q.y()) <= m.hh                         ? Mode::Move
                                                                                              : Mode::None;
        if (m_mode == Mode::None)
            return false;
        m_start = pos;
        m_startCentre = QPointF(m.cx / picture.width(), m.cy / picture.height());
        return true;
    }

    void move(QPointF pos, const QRectF& picture) override
    {
        const DeviceChainView::ParamRef ref{m_path, m_index, -1};
        if (m_mode == Mode::Move) {
            const QPointF d = pos - m_start;
            m_view->setParam(ref, QStringLiteral("mask.x"), std::clamp(m_startCentre.x() + d.x() / picture.width(), 0.0, 1.0));
            m_view->setParam(ref, QStringLiteral("mask.y"), std::clamp(m_startCentre.y() + d.y() / picture.height(), 0.0, 1.0));
        } else if (m_mode == Mode::Point) {
            // Into the mask's own frame: -0.5..0.5 across its width / height.
            const EffectMask m = mask(picture);
            const QPointF q = local(m, pos, picture);
            m_view->setParam(ref, QStringLiteral("mask.p%1x").arg(m_point), std::clamp(q.x() / std::max(2.0 * m.hw, 1.0), -4.0, 4.0), m.animate);
            m_view->setParam(ref, QStringLiteral("mask.p%1y").arg(m_point), std::clamp(q.y() / std::max(2.0 * m.hh, 1.0), -4.0, 4.0), m.animate);
        } else if (m_mode == Mode::Resize) {
            const EffectMask m = mask(picture);
            const QPointF q = local(m, pos, picture);
            m_view->setParam(ref, QStringLiteral("mask.w"), std::clamp(2 * std::abs(q.x()) / picture.width(), 0.01, 1.5));
            m_view->setParam(ref, QStringLiteral("mask.h"), std::clamp(2 * std::abs(q.y()) / picture.height(), 0.01, 1.5));
        }
    }

    void release() override
    {
        m_mode = Mode::None;
        m_view->flushEdit();
    }

    Qt::CursorShape cursor(QPointF pos, const QRectF& picture) const override
    {
        const EffectMask m = mask(picture);
        if (!m.active())
            return Qt::ArrowCursor;
        if (m.shape == 3) {
            const QPointF at = pos - picture.topLeft();
            for (const QPointF& c : m.points)
                if (QLineF(at, c).length() < 9)
                    return Qt::CrossCursor;
            return m.outline.contains(at) ? Qt::OpenHandCursor : Qt::ArrowCursor;
        }
        const QPointF q = local(m, pos, picture);
        if (std::abs(std::abs(q.x()) - m.hw) < 9 && std::abs(std::abs(q.y()) - m.hh) < 9)
            return Qt::SizeAllCursor;
        return std::abs(q.x()) <= m.hw && std::abs(q.y()) <= m.hh ? Qt::OpenHandCursor : Qt::ArrowCursor;
    }

private:
    enum class Mode { None, Move, Resize, Point };
    EffectMask mask(const QRectF& picture) const
    {
        const ParamMap* map = m_view->params(DeviceChainView::ParamRef{m_path, m_index, -1});
        return map ? effectMask(*map, m_view->keyTime(), picture.size().toSize()) : EffectMask();
    }
    static QPointF local(const EffectMask& m, QPointF pos, const QRectF& picture)
    {
        const double dx = pos.x() - picture.left() - m.cx, dy = pos.y() - picture.top() - m.cy;
        return {dx * m.cs + dy * m.sn, -dx * m.sn + dy * m.cs};
    }

    DeviceChainView* m_view;
    ChainPath m_path;
    int m_index;
    Mode m_mode = Mode::None;
    int m_point = -1; // freehand point being dragged
    QPointF m_start, m_startCentre;
};

void DeviceChainView::setPreview(PreviewWidget* preview)
{
    m_preview = preview;
    updateMaskOverlay();
}

void DeviceChainView::toggleMask(const ChainPath& path, int index)
{
    const EffectInstance* e = effectAt(path, index);
    if (!e)
        return;
    const auto it = e->params.find(QStringLiteral("mask.shape"));
    const bool on = it != e->params.end() && it->second.value >= 0.5;
    m_maskEditing = e->id;
    editNow([&](Timeline& tl) {
        if (ParamMap* map = params(tl, ParamRef{path, index, -1})) {
            ParamValue& shape = (*map)[QStringLiteral("mask.shape")];
            shape.keys.clear();
            shape.value = on ? 0 : 2; // on: an ellipse in the middle (the other settings stay for next time)
        }
    }, on ? tr("Mask Off") : tr("Mask On"));
}

void DeviceChainView::updateMaskOverlay()
{
    if (!m_preview)
        return;
    // The mask of the device last selected or switched on, if it still has one
    // (and the picture shows this chain's canvas: not the Live tab or audio).
    std::optional<std::pair<ChainPath, int>> found;
    const EffectChain* r = root(m_project->timeline());
    if (r && m_maskEditing && !isAudio() && m_target.kind != FxTarget::Kind::Live) {
        std::function<void(const EffectChain&, const ChainPath&)> walk = [&](const EffectChain& c, const ChainPath& path) {
            for (int i = 0; i < int(c.size()) && !found; ++i) {
                const EffectInstance& e = c[size_t(i)];
                if (e.id == m_maskEditing) {
                    const auto it = e.params.find(QStringLiteral("mask.shape"));
                    if (it != e.params.end() && it->second.value >= 0.5)
                        found = std::make_pair(path, i);
                    return;
                }
                for (int k = 0; k < int(e.chains.size()); ++k)
                    walk(e.chains[size_t(k)].effects, path.child(i, k));
            }
        };
        walk(*r, {});
    }
    if (!found) {
        if (m_preview->overlay() == m_maskOverlay.get())
            m_preview->setOverlay(nullptr);
        m_maskOverlay.reset();
        return;
    }
    if (!m_maskOverlay || !(m_maskOverlay->path() == found->first) || m_maskOverlay->index() != found->second) {
        if (m_preview->overlay() == m_maskOverlay.get())
            m_preview->setOverlay(nullptr);
        m_maskOverlay = std::make_unique<MaskOverlay>(this, found->first, found->second);
    }
    m_preview->setOverlay(m_maskOverlay.get());
}

namespace {
// An event's canvas from its source display (as shown, rotation applied) at an event-local time: framing and
// stabilisation as rendered.
QTransform eventView(const Clip& clip, const MediaItem& media, const VideoStreamInfo& vs, QSizeF canvas, TimeUs local)
{
    QTransform v = panCropTransform(resolveParams(panCropParams(), clip.panCrop, local), QSizeF(vs.displaySize()), canvas);
    if (clip.stabilize > 0)
        v = stabilize::correction(media.path, vs, clip.sourceTimeAt(clip.start + local), clip.stabilize, clip.stabilizeZoom, clip.sourceIn,
                                  clip.sourceIn + clip.sourceSpan())
            * v;
    return v;
}

// Pick Neutral: a crosshair on the preview; a click there reports where (0..1 of the picture).
class PickOverlay : public PreviewOverlay {
public:
    explicit PickOverlay(std::function<void(QPointF)> picked) : m_picked(std::move(picked)) {}
    void paint(QPainter& p, const QRectF& picture) override
    {
        const QString hint = QObject::tr("Click something that should be grey or white (Pick Neutral again: cancel)");
        const QRectF box(picture.left() + 8, picture.top() + 8, p.fontMetrics().horizontalAdvance(hint) + 12, p.fontMetrics().height() + 6);
        p.fillRect(box, QColor(0, 0, 0, 170));
        p.setPen(Qt::white);
        p.drawText(box, Qt::AlignCenter, hint);
    }
    bool press(QPointF pos, const QRectF& picture, Qt::KeyboardModifiers) override
    {
        if (picture.contains(pos))
            m_picked(QPointF((pos.x() - picture.left()) / picture.width(), (pos.y() - picture.top()) / picture.height()));
        return false;
    }
    void move(QPointF, const QRectF&) override {}
    void release() override {}
    Qt::CursorShape cursor(QPointF, const QRectF&) const override { return Qt::CrossCursor; }

private:
    std::function<void(QPointF)> m_picked;
};
} // namespace

void DeviceChainView::autoWhiteBalance(const ChainPath& path, int index)
{
    const EffectInstance* e = effectAt(path, index);
    if (!e || m_target.kind != FxTarget::Kind::Clip)
        return;
    const int effectId = e->id;
    const QImage picture = colormatch::eventPicture(*m_project->snapshot(), m_target.id, m_cursor(), effectId);
    if (picture.isNull())
        return;
    editNow([&](Timeline& tl) {
        EffectChain* r = root(tl);
        if (EffectChain* c = r ? resolveChain(*r, path) : nullptr; c && index < int(c->size()) && (*c)[size_t(index)].id == effectId)
            colormatch::autoColour(picture, (*c)[size_t(index)]);
    }, tr("Auto Colour"));
}

void DeviceChainView::pickNeutral(const ChainPath& path, int index)
{
    if (!m_preview || m_target.kind != FxTarget::Kind::Clip)
        return;
    if (m_picker && m_preview->overlay() == m_picker.get()) { // (pressed again: cancel)
        m_preview->setOverlay(nullptr);
        m_picker.reset();
        updateMaskOverlay();
        return;
    }
    m_picker = std::make_unique<PickOverlay>([this, path, index](QPointF at) {
        setNeutralAt(path, index, at);
        QTimer::singleShot(0, this, [this] { // (not from inside its own click)
            if (m_preview && m_preview->overlay() == m_picker.get())
                m_preview->setOverlay(nullptr);
            m_picker.reset();
            updateMaskOverlay();
        });
    });
    m_preview->setOverlay(m_picker.get());
    m_preview->update();
}

void DeviceChainView::setNeutralAt(const ChainPath& path, int index, QPointF at)
{
    const EffectInstance* e = effectAt(path, index);
    if (!e || m_target.kind != FxTarget::Kind::Clip)
        return;
    const int effectId = e->id;
    const QImage picture = colormatch::eventPicture(*m_project->snapshot(), m_target.id, m_cursor(), effectId, 640)
                               .convertToFormat(QImage::Format_ARGB32);
    if (picture.isNull())
        return;
    // A few pixels round the click, averaged (noise, grain).
    const int cx = std::clamp(int(at.x() * picture.width()), 0, picture.width() - 1), cy = std::clamp(int(at.y() * picture.height()), 0, picture.height() - 1);
    double sum[3] = {}, n = 0;
    for (int y = std::max(0, cy - 2); y <= std::min(picture.height() - 1, cy + 2); ++y)
        for (int x = std::max(0, cx - 2); x <= std::min(picture.width() - 1, cx + 2); ++x) {
            const QRgb px = picture.pixel(x, y);
            sum[0] += qRed(px);
            sum[1] += qGreen(px);
            sum[2] += qBlue(px);
            ++n;
        }
    const QColor picked(int(sum[0] / n), int(sum[1] / n), int(sum[2] / n));
    editNow([&](Timeline& tl) {
        EffectChain* r = root(tl);
        if (EffectChain* c = r ? resolveChain(*r, path) : nullptr; c && index < int(c->size()) && (*c)[size_t(index)].id == effectId)
            colormatch::setNeutral((*c)[size_t(index)], picked);
    }, tr("Pick Neutral"));
}

QString DeviceChainView::trackCorners(const ChainPath& path, int index, bool quiet)
{
    auto say = [&](const QString& text) {
        if (!quiet)
            QMessageBox::information(this, tr("Track Corners"), text);
        return text;
    };
    // The shot under the pinned event at the cursor (the top video event on the tracks below it), each corner followed
    // on its source frames in a small box, mapped back onto the canvas as keyframes on the corners.
    const Timeline& tl = m_project->timeline();
    int pinTrack = -1;
    const Clip* pin = m_target.kind == FxTarget::Kind::Clip ? tl.findClip(m_target.id, &pinTrack) : nullptr;
    const EffectDef* def = findEffect(QStringLiteral("corner_pin"));
    if (!pin || !def)
        return say(tr("Track Corners follows the shot under an event: use it on the Event FX of the event to pin (on a track above the shot)."));
    const TimeUs now = std::clamp(m_cursor(), pin->start, pin->end() - 1);
    // (Two there at once, a crossfade or a cut a moment off: the one that goes on, not the one ending.)
    const Clip* shot = nullptr;
    for (int i = pinTrack + 1; i < int(tl.tracks.size()) && !shot; ++i)
        if (isVisualTrack(tl.tracks[size_t(i)].type))
            for (const Clip& c : tl.tracks[size_t(i)].clips)
                if (now >= c.start && now < c.end() && !c.isGenerated() && (!shot || c.end() > shot->end()))
                    shot = &c;
    const auto media = shot ? m_project->mediaItem(shot->mediaId) : nullptr;
    const VideoStreamInfo* vs = media ? media->info.videoStream(shot->streamIndex) : nullptr;
    if (!shot || !vs || media->info.isStill)
        return say(tr("There is no video under this event at the cursor to follow: put the event to pin on a track above the shot."));
    if (shot->reverse || shot->hasRamp())
        return say(tr("Track Corners works on shots playing forwards at a steady speed (no reverse or speed ramp)."));
    const ParamMap* map = params(ParamRef{path, index, -1});
    if (!map)
        return {};
    const QSizeF canvas(m_project->settings().width, m_project->settings().height);
    const QSizeF disp(vs->displaySize());
    auto view = [&](TimeUs shotLocal) { return eventView(*shot, *media, *vs, canvas, shotLocal); };
    const TimeUs from = now - pin->start;
    const Params pp = resolveParams(def->params, *map, from);
    bool invertible = false;
    const QTransform toSource = view(now - shot->start).inverted(&invertible);
    if (!invertible)
        return {};
    static const char* corners[4] = {"tl", "tr", "br", "bl"};
    const bool sideways = vs->rotation == 90 || vs->rotation == 270;
    const double side = disp.height() * 0.08; // (a box 8% of the picture's height round each corner)
    const QSizeF b = sideways ? QSizeF(side / disp.height(), side / disp.width()) : QSizeF(side / disp.width(), side / disp.height());
    QRectF boxes[4];
    for (int k = 0; k < 4; ++k) {
        const QPointF onCanvas(pp.num(QLatin1String(corners[k]) + QLatin1String("X")) * canvas.width(),
                               pp.num(QLatin1String(corners[k]) + QLatin1String("Y")) * canvas.height());
        const QPointF d = toSource.map(onCanvas);
        const QPointF c = tracker::displayToStored(QPointF(d.x() / disp.width(), d.y() / disp.height()), vs->rotation);
        boxes[k] = QRectF(c - QPointF(b.width() / 2, b.height() / 2), b);
    }
    const TimeUs end = std::min(pin->end(), shot->end()) - 1;
    const TimeUs srcFrom = shot->sourceTimeAt(now), srcTo = shot->sourceTimeAt(end);

    QProgressDialog progress(tr("Tracking the corners..."), tr("Stop"), 0, 1000, this);
    progress.setWindowModality(Qt::WindowModal);
    progress.setMinimumDuration(200);
    std::atomic<double> done{0};
    std::atomic<bool> stop{false}, finished{false};
    std::vector<tracker::Point> points[4];
    QString error;
    std::thread worker([&] {
        for (int k = 0; k < 4 && !stop; ++k)
            points[k] = tracker::track(media->path, shot->streamIndex, srcFrom, srcTo, boxes[k], [&](double p) {
                done = (k + p) / 4;
                return !stop.load();
            }, &error);
        finished = true;
    });
    while (!finished) {
        progress.setValue(int(done * 1000));
        QCoreApplication::processEvents(QEventLoop::AllEvents, 30);
        if (progress.wasCanceled())
            stop = true;
        std::this_thread::sleep_for(std::chrono::milliseconds(15));
    }
    worker.join();
    progress.close();
    const size_t frames = std::min({points[0].size(), points[1].size(), points[2].size(), points[3].size()});
    if (frames < 2) {
        QStringList counts;
        for (int k = 0; k < 4; ++k)
            counts << QString::number(points[k].size());
        return say(error.isEmpty() ? tr("Nothing to track there: put the corners on the corners of something in the shot (frames followed: %1).")
                                         .arg(counts.join(QStringLiteral(", ")))
                                   : error);
    }
    // Keyframes (every frame, until a corner is lost for good), replacing the ones in that span.
    std::vector<std::pair<TimeUs, std::array<QPointF, 4>>> keys;
    int lost = 0;
    for (size_t i = 0; i < frames; ++i) {
        const bool weak = std::any_of(std::begin(points), std::end(points), [&](const auto& pts) { return pts[i].match < 0.25f; });
        lost = weak ? lost + 1 : 0;
        if (lost >= 5)
            break;
        const TimeUs shotLocal = TimeUs(std::llround(double(points[0][i].srcT - shot->sourceIn) / shot->speed));
        const QTransform v = view(shotLocal);
        std::array<QPointF, 4> at;
        for (int k = 0; k < 4; ++k) {
            const QPointF d = tracker::storedToDisplay(points[k][i].centre, vs->rotation);
            const QPointF c = v.map(QPointF(d.x() * disp.width(), d.y() * disp.height()));
            at[size_t(k)] = QPointF(c.x() / canvas.width(), c.y() / canvas.height());
        }
        keys.emplace_back(shot->start + shotLocal - pin->start, at);
    }
    if (keys.empty())
        return say(tr("The corners were lost straight away: put them exactly on the corners and try again."));
    const TimeUs last = keys.back().first;
    editNow([&](Timeline& t) {
        ParamMap* m = params(t, ParamRef{path, index, -1});
        if (!m)
            return;
        for (int k = 0; k < 4; ++k)
            for (const bool x : {true, false}) {
                ParamValue& pv = (*m)[QLatin1String(corners[k]) + QLatin1String(x ? "X" : "Y")];
                std::erase_if(pv.keys, [&](const Keyframe& key) { return key.time >= from && key.time <= last; });
                for (const auto& [time, at] : keys)
                    pv.keys.push_back({time, std::clamp(x ? at[size_t(k)].x() : at[size_t(k)].y(), -0.5, 1.5), Keyframe::Interp::Linear});
                std::sort(pv.keys.begin(), pv.keys.end(), [](const Keyframe& a, const Keyframe& c) { return a.time < c.time; });
            }
    }, tr("Track Corners"));
    emit seekRequested(pin->start + last);
    return say(lost >= 5 ? tr("Followed the corners for %1 s, then lost one: move the corners back onto them there and track again.")
                               .arg(double(last - from) / 1e6, 0, 'f', 1)
                         : tr("Followed the corners for %1 s (%2 keyframes each).").arg(double(last - from) / 1e6, 0, 'f', 1).arg(keys.size()));
}

QString DeviceChainView::trackMask(const ChainPath& path, int index, bool quiet)
{
    auto say = [&](const QString& text) {
        if (!quiet)
            QMessageBox::information(this, tr("Track Motion"), text);
        return text;
    };
    // From the cursor to the end of the event, on its source frames: the mask's
    // box mapped from the canvas into the source, followed, and its centre mapped
    // back for keyframes on mask.x / mask.y.
    const Timeline& tl = m_project->timeline();
    const Clip* clip = m_target.kind == FxTarget::Kind::Clip ? tl.findClip(m_target.id) : nullptr;
    const auto media = clip && !clip->isGenerated() ? m_project->mediaItem(clip->mediaId) : nullptr;
    const VideoStreamInfo* vs = media ? media->info.videoStream(clip->streamIndex) : nullptr;
    if (!clip || !vs || media->info.isStill) {
        return say(tr("Track Motion follows the footage of a video event: use it on a video event's Event FX."));
    }
    if (clip->reverse || clip->hasRamp()) {
        return say(tr("Track Motion works on events playing forwards at a steady speed (no reverse or speed ramp)."));
    }
    const ParamMap* map = params(ParamRef{path, index, -1});
    if (!map)
        return {};
    const QSizeF canvas(m_project->settings().width, m_project->settings().height);
    const QSizeF disp(vs->displaySize());
    // Canvas <-> source display, at an event-local time (framing and stabilisation as rendered).
    auto view = [&](TimeUs local) { return eventView(*clip, *media, *vs, canvas, local); };
    const TimeUs from = std::clamp(m_cursor(), clip->start, clip->end() - 1) - clip->start;
    const Params mp = resolveParams(effectMaskParams(), *map, from);
    bool invertible = false;
    const QTransform toSource = view(from).inverted(&invertible);
    if (!invertible)
        return {};
    const QPointF centre = toSource.map(QPointF(mp.num(QStringLiteral("mask.x")) * canvas.width(), mp.num(QStringLiteral("mask.y")) * canvas.height()));
    const double scale = std::sqrt(std::abs(toSource.determinant()));
    const QSizeF boxDisp(mp.num(QStringLiteral("mask.w")) * canvas.width() * scale, mp.num(QStringLiteral("mask.h")) * canvas.height() * scale);
    const bool sideways = vs->rotation == 90 || vs->rotation == 270;
    const QPointF c = tracker::displayToStored(QPointF(centre.x() / disp.width(), centre.y() / disp.height()), vs->rotation);
    const QSizeF b = sideways ? QSizeF(boxDisp.height() / disp.height(), boxDisp.width() / disp.width())
                              : QSizeF(boxDisp.width() / disp.width(), boxDisp.height() / disp.height());
    const QRectF box(c - QPointF(b.width() / 2, b.height() / 2), b);
    const TimeUs srcFrom = clip->sourceTimeAt(clip->start + from), srcTo = clip->sourceTimeAt(clip->end() - 1);

    QProgressDialog progress(tr("Tracking..."), tr("Stop"), 0, 1000, this);
    progress.setWindowModality(Qt::WindowModal);
    progress.setMinimumDuration(200);
    std::atomic<double> done{0};
    std::atomic<bool> stop{false}, finished{false};
    std::vector<tracker::Point> points;
    QString error;
    std::thread worker([&] {
        points = tracker::track(media->path, clip->streamIndex, srcFrom, srcTo, box, [&](double p) {
            done = p;
            return !stop.load();
        }, &error);
        finished = true;
    });
    while (!finished) {
        progress.setValue(int(done * 1000));
        QCoreApplication::processEvents(QEventLoop::AllEvents, 30);
        if (progress.wasCanceled())
            stop = true;
        std::this_thread::sleep_for(std::chrono::milliseconds(15));
    }
    worker.join();
    progress.close();
    if (points.size() < 2) {
        return say(error.isEmpty() ? tr("Nothing to track there.") : error);
    }
    // Keyframes (every frame, until it is lost for good), replacing the ones in that span.
    std::vector<std::pair<TimeUs, QPointF>> keys;
    int lost = 0;
    for (const tracker::Point& pt : points) {
        lost = pt.match < 0.25f ? lost + 1 : 0;
        if (lost >= 5)
            break;
        const TimeUs local = TimeUs(std::llround(double(pt.srcT - clip->sourceIn) / clip->speed));
        const QPointF d = tracker::storedToDisplay(pt.centre, vs->rotation);
        const QPointF onCanvas = view(local).map(QPointF(d.x() * disp.width(), d.y() * disp.height()));
        keys.emplace_back(local, QPointF(onCanvas.x() / canvas.width(), onCanvas.y() / canvas.height()));
    }
    const TimeUs last = keys.back().first;
    editNow([&](Timeline& t) {
        ParamMap* m = params(t, ParamRef{path, index, -1});
        if (!m)
            return;
        for (const char* id : {"mask.x", "mask.y"}) {
            ParamValue& pv = (*m)[QString::fromLatin1(id)];
            std::erase_if(pv.keys, [&](const Keyframe& k) { return k.time >= from && k.time <= last; });
            const bool x = id[5] == 'x';
            for (const auto& [time, pos] : keys)
                pv.keys.push_back({time, std::clamp(x ? pos.x() : pos.y(), 0.0, 1.0), Keyframe::Interp::Linear});
            std::sort(pv.keys.begin(), pv.keys.end(), [](const Keyframe& a, const Keyframe& b) { return a.time < b.time; });
        }
    }, tr("Track Motion"));
    emit seekRequested(clip->start + last);
    return say(lost >= 5 ? tr("Followed it for %1 s, then lost it: track again from there with the mask moved onto it.").arg(double(last - from) / 1e6, 0, 'f', 1)
                                       : tr("Followed it for %1 s (%2 keyframes).").arg(double(last - from) / 1e6, 0, 'f', 1).arg(keys.size()));
}

void DeviceChainView::setModulation(const ParamRef& ref, const ParamDef& def, const std::optional<Modulation>& mod)
{
    editNow([&](Timeline& tl) {
        ParamMap* map = params(tl, ref);
        if (!map)
            return;
        auto it = map->find(def.id);
        if (it == map->end()) { // still at its default: store that with the modulation
            if (!mod)
                return;
            ParamValue pv;
            pv.value = def.def;
            pv.text = def.defText;
            it = map->emplace(def.id, pv).first;
        }
        it->second.mod = mod;
    }, mod ? tr("Modulate Effect Setting") : tr("Remove Modulation"));
    refreshValues();
}

void DeviceChainView::setParamText(const ParamRef& ref, const QString& id, const QString& text)
{
    edit([&](Timeline& tl) {
        if (ParamMap* map = params(tl, ref))
            (*map)[id].text = text;
    }, tr("Change Effect Setting"));
}

void DeviceChainView::toggleKey(const ParamRef& ref, const ParamDef& def)
{
    const TimeUs t = keyTime();
    const TimeUs tol = keyTolerance();
    editNow([&](Timeline& tl) {
        ParamMap* map = params(tl, ref);
        if (!map)
            return;
        auto it = map->find(def.id);
        ParamValue pv = it != map->end() ? it->second : ParamValue{def.def, {}, {}};
        const double current = pv.at(t, def.type);
        if (pv.hasKeyAt(t, tol)) {
            pv.removeKeyAt(t, tol);
            if (pv.keys.empty())
                pv.value = current; // back to a static value
        } else {
            pv.addKey(t, current);
        }
        (*map)[def.id] = pv;
    }, tr("Keyframe"));
    refreshValues();
}

void DeviceChainView::followAudio(const ParamRef& ref, const ParamDef& def)
{
    QDialog dlg(this);
    dlg.setWindowTitle(tr("Follow Audio: %1").arg(def.name));
    auto* form = new QFormLayout(&dlg);
    auto* band = new QComboBox;
    band->addItems({tr("Level (everything)"), tr("Bass (kicks, bass lines)"), tr("Mids (snares, voices, guitars)"), tr("Highs (hi-hats, cymbals)")});
    auto spin = [&](double v) {
        auto* s = new QDoubleSpinBox;
        s->setRange(def.min, def.max);
        s->setDecimals(def.decimals);
        s->setSuffix(def.suffix);
        s->setValue(v);
        return s;
    };
    const ParamMap* map = params(ref);
    const double now = map && map->count(def.id) ? map->at(def.id).at(keyTime(), def.type) : def.def;
    auto* quiet = spin(now);
    auto* loud = spin(std::abs(def.max - now) > std::abs(now - def.min) ? def.max : def.min);
    form->addRow(tr("Listen to"), band);
    form->addRow(tr("When quiet"), quiet);
    form->addRow(tr("When loudest"), loud);
    auto* note = new QLabel(tr("Keyframes follow the sound of the timeline over %1, replacing any there now.")
                                .arg(m_target.kind == FxTarget::Kind::Clip ? tr("the event") : tr("the whole project")));
    note->setWordWrap(true);
    form->addRow(note);
    auto* buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel);
    help::addButton(buttons, QStringLiteral("effects.md#follow-audio"));
    form->addRow(buttons);
    connect(buttons, &QDialogButtonBox::accepted, &dlg, &QDialog::accept);
    connect(buttons, &QDialogButtonBox::rejected, &dlg, &QDialog::reject);
    if (dlg.exec() != QDialog::Accepted)
        return;
    // Event FX keyframes are in event time, the others in timeline time.
    TimeUs from = 0, to = m_project->timeline().duration();
    if (m_target.kind == FxTarget::Kind::Clip)
        if (const Clip* c = m_project->timeline().findClip(m_target.id)) {
            from = c->start;
            to = c->end();
        }
    const TimeUs step = kUsPerSecond / 15;
    const std::vector<double> env = audioBandEnvelope(*m_project->snapshot(), from, to, step, BandFollower::Band(band->currentIndex()));
    const double q = quiet->value(), l = loud->value();
    editNow([&](Timeline& tl) {
        ParamMap* m = params(tl, ref);
        if (!m)
            return;
        ParamValue pv = m->count(def.id) ? m->at(def.id) : ParamValue{def.def, {}, {}};
        pv.keys.clear();
        for (size_t i = 0; i < env.size(); ++i)
            pv.addKey(TimeUs(i) * step + (m_target.kind == FxTarget::Kind::Clip ? 0 : from), q + (l - q) * env[i]);
        (*m)[def.id] = pv;
    }, tr("Follow Audio"));
    refreshValues();
}

void DeviceChainView::editMaskPoints(const ParamRef& ref, int index, std::optional<QPointF> insert)
{
    const TimeUs t = keyTime();
    editNow([&](Timeline& tl) {
        ParamMap* map = params(tl, ref);
        if (!map)
            return;
        const int n = std::clamp(int(std::lround(map->count(QStringLiteral("mask.points")) ? map->at(QStringLiteral("mask.points")).value : 6.0)), 3, 16);
        if ((insert && n >= 16) || (!insert && n <= 3))
            return;
        // Every point with its own value first (unset ones sit on a circle that changes with the count).
        std::vector<std::pair<ParamValue, ParamValue>> points;
        for (int i = 0; i < n; ++i) {
            auto value = [&](const QString& id, double def) {
                auto it = map->find(id);
                return it != map->end() ? it->second : ParamValue{def, {}, {}};
            };
            const QPointF d = maskPoint(*map, i, n, t);
            points.emplace_back(value(QStringLiteral("mask.p%1x").arg(i), d.x()), value(QStringLiteral("mask.p%1y").arg(i), d.y()));
        }
        if (insert)
            points.insert(points.begin() + index + 1, {ParamValue{insert->x(), {}, {}}, ParamValue{insert->y(), {}, {}}});
        else
            points.erase(points.begin() + index);
        for (int i = 0; i < 16; ++i) {
            map->erase(QStringLiteral("mask.p%1x").arg(i));
            map->erase(QStringLiteral("mask.p%1y").arg(i));
        }
        for (int i = 0; i < int(points.size()); ++i) {
            (*map)[QStringLiteral("mask.p%1x").arg(i)] = points[size_t(i)].first;
            (*map)[QStringLiteral("mask.p%1y").arg(i)] = points[size_t(i)].second;
        }
        ParamValue& count = (*map)[QStringLiteral("mask.points")];
        count.keys.clear();
        count.value = double(points.size());
    }, insert ? tr("Add Mask Point") : tr("Remove Mask Point"));
    refreshValues();
}

void DeviceChainView::setKeyInterp(const ParamRef& ref, const QString& id, Keyframe::Interp interp)
{
    const TimeUs t = keyTime();
    const TimeUs tol = keyTolerance();
    editNow([&](Timeline& tl) {
        if (ParamMap* map = params(tl, ref)) {
            ParamValue& pv = (*map)[id];
            keyops::Times at;
            for (const auto& k : pv.keys)
                if (std::llabs(k.time - t) <= tol)
                    at.insert(k.time);
            keyops::setInterp(pv, at, interp);
        }
    }, tr("Keyframe Curve"));
}

keyops::Address DeviceChainView::keyAddress(const ParamRef& ref, const QString& id) const
{
    keyops::Address a;
    using S = keyops::Address::Source;
    a.source = m_target.kind == FxTarget::Kind::Track    ? S::TrackFx
               : m_target.kind == FxTarget::Kind::Output ? S::OutputFx
               : m_target.kind == FxTarget::Kind::AudioMaster ? S::MasterFx
                                                              : S::ClipFx;
    a.owner = m_target.kind == FxTarget::Kind::Clip || m_target.kind == FxTarget::Kind::Track ? m_target.id : 0;
    if (const EffectInstance* e = effectAt(ref.path, ref.index))
        a.effect = e->id;
    a.chain = ref.chain;
    a.param = id;
    return a;
}

void DeviceChainView::resetParam(const ParamRef& ref, const ParamDef& def)
{
    editNow([&](Timeline& tl) {
        if (ParamMap* map = params(tl, ref))
            (*map)[def.id] = ParamValue{def.def, def.defText, {}};
    }, tr("Reset Effect Setting"));
    refreshValues();
}

void DeviceChainView::jumpKey(const ParamRef& ref, const QString& id, int direction)
{
    const ParamMap* map = params(ref);
    if (!map)
        return;
    auto it = map->find(id);
    if (it == map->end() || it->second.keys.empty())
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

// ---- Structure ------------------------------------------------------------------------------

bool DeviceChainView::canDrop(const QMimeData* mime) const
{
    if (!targetExists())
        return false;
    if (mime->hasFormat(QString::fromLatin1(DeviceChainView::kDeviceMime)))
        return true;
    if (mime->hasFormat(QString::fromLatin1(EffectsBrowser::kEffectMime)))
        return acceptsEffect(QString::fromUtf8(mime->data(QString::fromLatin1(EffectsBrowser::kEffectMime))));
    if (mime->hasFormat(QString::fromLatin1(EffectsBrowser::kPresetMime)))
        return acceptsPreset(QString::fromUtf8(mime->data(QString::fromLatin1(EffectsBrowser::kPresetMime))));
    return false;
}

bool DeviceChainView::acceptsPreset(const QString& presetId) const
{
    const auto p = PresetLibrary::instance().find(presetId);
    return p && targetExists() && p->audio == isAudio();
}

void DeviceChainView::addPreset(const QString& presetId)
{
    const auto p = PresetLibrary::instance().find(presetId);
    if (!p || !acceptsPreset(presetId))
        return;
    editRoot([&](EffectChain& r, Timeline& tl) {
        const EffectChain add = instantiatePreset(*p, [&] { return tl.allocId(); });
        r.insert(r.end(), add.begin(), add.end());
    }, tr("Add Preset %1").arg(p->name));
}

void DeviceChainView::loadPreset(const ChainPath& path, int index, const QString& presetId)
{
    const auto p = PresetLibrary::instance().find(presetId);
    if (!p)
        return;
    editRoot([&](EffectChain& r, Timeline& tl) {
        if (EffectChain* c = resolveChain(r, path); c && index < int(c->size()))
            applyPresetTo(*p, (*c)[size_t(index)], [&] { return tl.allocId(); });
    }, tr("Load Preset %1").arg(p->name));
    refreshValues();
}

void DeviceChainView::saveChainPreset(const EffectChain& devices)
{
    bool ok = false;
    const QString name = QInputDialog::getText(this, tr("Save Chain Preset"), tr("Preset name:"), QLineEdit::Normal, QString(), &ok).trimmed();
    if (!ok || name.isEmpty())
        return;
    PresetLibrary& lib = PresetLibrary::instance();
    if (lib.findUserByName(name, isAudio())
        && QMessageBox::question(this, tr("Save Preset"), tr("Replace the existing preset \"%1\"?").arg(name)) != QMessageBox::Yes)
        return;
    QString error;
    if (lib.save(name, isAudio(), QString(), devices, &error).isEmpty())
        QMessageBox::warning(this, tr("Save Preset"), error);
}

void DeviceChainView::savePreset(const ChainPath& path, int index)
{
    const EffectInstance* dev = effectAt(path, index);
    if (!dev)
        return;
    if (dev->isRack()) {
        saveChainPreset({*dev});
        return;
    }
    const auto meta = effectMeta(dev->type);
    bool ok = false;
    const QString name = QInputDialog::getText(this, tr("Save %1 Preset").arg(meta ? meta->name : dev->type), tr("Preset name:"), QLineEdit::Normal,
                                               QString(), &ok).trimmed();
    if (!ok || name.isEmpty())
        return;
    PresetLibrary& lib = PresetLibrary::instance();
    if (lib.findUserByName(name, isAudio())
        && QMessageBox::question(this, tr("Save Preset"), tr("Replace the existing preset \"%1\"?").arg(name)) != QMessageBox::Yes)
        return;
    QString error;
    if (lib.save(name, isAudio(), dev->type, {*dev}, &error).isEmpty())
        QMessageBox::warning(this, tr("Save Preset"), error);
}

void DeviceChainView::saveSelectionPreset()
{
    const EffectChain* r = root(m_project->timeline());
    if (!r)
        return;
    EffectChain picked;
    forEachEffect(*r, [&](const EffectInstance& e) {
        if (m_selected.count(e.id))
            picked.push_back(e);
    });
    if (!picked.empty())
        saveChainPreset(picked);
}

bool DeviceChainView::dropMime(const QMimeData* mime, const ChainPath& path, int index, bool copy)
{
    if (!canDrop(mime))
        return false;
    bool ok = false;
    if (mime->hasFormat(QString::fromLatin1(DeviceChainView::kDeviceMime))) {
        const QStringList parts = QString::fromUtf8(mime->data(QString::fromLatin1(kDeviceMime))).split(QLatin1Char('|'));
        if (parts.size() != 2)
            return false;
        const ChainPath from = pathFromString(parts[0]);
        const int fromIndex = parts[1].toInt();
        editRoot([&](EffectChain& r, Timeline& tl) {
            ok = moveEffect(r, from, fromIndex, path, index, copy, [&] { return tl.allocId(); });
        }, copy ? tr("Copy Effect") : tr("Move Effect"));
        return ok;
    }
    if (mime->hasFormat(QString::fromLatin1(EffectsBrowser::kPresetMime))) {
        const auto preset = PresetLibrary::instance().find(QString::fromUtf8(mime->data(QString::fromLatin1(EffectsBrowser::kPresetMime))));
        if (!preset)
            return false;
        editRoot([&](EffectChain& r, Timeline& tl) {
            if (EffectChain* c = resolveChain(r, path)) {
                const EffectChain add = instantiatePreset(*preset, [&] { return tl.allocId(); });
                c->insert(c->begin() + std::clamp(index, 0, int(c->size())), add.begin(), add.end());
                ok = true;
            }
        }, tr("Add Preset %1").arg(preset->name));
        return ok;
    }
    const QString type = QString::fromUtf8(mime->data(QString::fromLatin1(EffectsBrowser::kEffectMime)));
    const auto meta = effectMeta(type);
    editRoot([&](EffectChain& r, Timeline& tl) {
        if (EffectChain* c = resolveChain(r, path)) {
            c->insert(c->begin() + std::clamp(index, 0, int(c->size())), makeEffectOfType(type, tl.allocId()));
            ok = true;
        }
    }, tr("Add %1").arg(meta ? meta->name : type));
    return ok;
}

bool DeviceChainView::dropAsNewChain(const QMimeData* mime, const ChainPath& rackPath, int rackIndex, bool copy)
{
    if (!canDrop(mime))
        return false;
    const EffectInstance* rack = effectAt(rackPath, rackIndex);
    if (!rack || !rack->isRack())
        return false;
    const int newChain = int(rack->chains.size());
    const int rackId = rack->id;
    bool ok = false;
    editRoot([&](EffectChain& r, Timeline& tl) {
        EffectChain* c = resolveChain(r, rackPath);
        if (!c)
            return;
        EffectInstance& rk = (*c)[size_t(rackIndex)];
        rk.chains.push_back(makeRackChain(isAudio(), tr("Chain %1").arg(newChain + 1)));
        const ChainPath dest = rackPath.child(rackIndex, newChain);
        if (mime->hasFormat(QString::fromLatin1(DeviceChainView::kDeviceMime))) {
            const QStringList parts = QString::fromUtf8(mime->data(QString::fromLatin1(kDeviceMime))).split(QLatin1Char('|'));
            if (parts.size() == 2)
                ok = moveEffect(r, pathFromString(parts[0]), parts[1].toInt(), dest, 0, copy, [&] { return tl.allocId(); });
        } else if (mime->hasFormat(QString::fromLatin1(EffectsBrowser::kPresetMime))) {
            const auto preset = PresetLibrary::instance().find(QString::fromUtf8(mime->data(QString::fromLatin1(EffectsBrowser::kPresetMime))));
            if (EffectChain* d = resolveChain(r, dest); d && preset) {
                const EffectChain add = instantiatePreset(*preset, [&] { return tl.allocId(); });
                d->insert(d->end(), add.begin(), add.end());
                ok = true;
            }
        } else {
            const QString type = QString::fromUtf8(mime->data(QString::fromLatin1(EffectsBrowser::kEffectMime)));
            if (EffectChain* d = resolveChain(r, dest)) {
                d->push_back(makeEffectOfType(type, tl.allocId()));
                ok = true;
            }
        }
        if (!ok) // e.g. a rack dropped into itself: leave things as they were
            r = *root(m_project->timeline());
    }, tr("New Parallel Chain"));
    if (ok)
        selectChain(rackId, newChain);
    return ok;
}

void DeviceChainView::removeDevice(const ChainPath& path, int index)
{
    editRoot([&](EffectChain& r, Timeline&) {
        if (EffectChain* c = resolveChain(r, path); c && index < int(c->size()))
            c->erase(c->begin() + index);
    }, tr("Delete Effect"));
}

void DeviceChainView::duplicateDevice(const ChainPath& path, int index)
{
    editRoot([&](EffectChain& r, Timeline& tl) { moveEffect(r, path, index, path, index + 1, true, [&] { return tl.allocId(); }); },
             tr("Duplicate Effect"));
}

void DeviceChainView::setDeviceEnabled(const ChainPath& path, int index, bool on)
{
    editRoot([&](EffectChain& r, Timeline&) {
        if (EffectChain* c = resolveChain(r, path); c && index < int(c->size()))
            (*c)[size_t(index)].enabled = on;
    }, on ? tr("Enable Effect") : tr("Bypass Effect"));
}

void DeviceChainView::groupSelection()
{
    const EffectChain* r = root(m_project->timeline());
    if (!r || m_selected.empty())
        return;
    // Find the chain holding the selected devices (they must share one).
    std::optional<ChainPath> found;
    std::vector<int> indices;
    std::function<void(const EffectChain&, const ChainPath&)> walk = [&](const EffectChain& c, const ChainPath& p) {
        std::vector<int> here;
        for (int i = 0; i < int(c.size()); ++i) {
            if (m_selected.count(c[size_t(i)].id))
                here.push_back(i);
            for (int k = 0; k < int(c[size_t(i)].chains.size()); ++k)
                walk(c[size_t(i)].chains[size_t(k)].effects, p.child(i, k));
        }
        if (!here.empty() && !found) {
            found = p;
            indices = here;
        }
    };
    walk(*r, {});
    if (!found)
        return;
    editRoot([&](EffectChain& root, Timeline& tl) { groupIntoRack(root, *found, indices, tl.allocId(), isAudio()); },
             tr("Group into Rack"));
    m_selected.clear();
    scheduleRebuild();
}

void DeviceChainView::ungroup(const ChainPath& path, int index)
{
    editRoot([&](EffectChain& r, Timeline&) { ungroupRack(r, path, index); }, tr("Ungroup Rack"));
}

void DeviceChainView::addChain(const ChainPath& path, int rackIndex)
{
    const EffectInstance* rack = effectAt(path, rackIndex);
    if (!rack)
        return;
    const int rackId = rack->id, n = int(rack->chains.size());
    editRoot([&](EffectChain& r, Timeline&) {
        if (EffectChain* c = resolveChain(r, path))
            (*c)[size_t(rackIndex)].chains.push_back(makeRackChain(isAudio(), tr("Chain %1").arg(n + 1)));
    }, tr("Add Chain"));
    selectChain(rackId, n);
}

void DeviceChainView::removeChain(const ChainPath& path, int rackIndex, int chain)
{
    editRoot([&](EffectChain& r, Timeline&) {
        if (EffectChain* c = resolveChain(r, path)) {
            auto& chains = (*c)[size_t(rackIndex)].chains;
            if (chain < int(chains.size()))
                chains.erase(chains.begin() + chain);
        }
    }, tr("Delete Chain"));
}

void DeviceChainView::duplicateChain(const ChainPath& path, int rackIndex, int chain)
{
    editRoot([&](EffectChain& r, Timeline& tl) {
        if (EffectChain* c = resolveChain(r, path)) {
            auto& chains = (*c)[size_t(rackIndex)].chains;
            if (chain >= int(chains.size()))
                return;
            RackChain copy = chains[size_t(chain)];
            copy.name = tr("%1 copy").arg(copy.name);
            reassignEffectIds(copy.effects, [&] { return tl.allocId(); });
            chains.insert(chains.begin() + chain + 1, std::move(copy));
        }
    }, tr("Duplicate Chain"));
}

void DeviceChainView::renameChain(const ChainPath& path, int rackIndex, int chain)
{
    const EffectInstance* rack = effectAt(path, rackIndex);
    if (!rack || chain >= int(rack->chains.size()))
        return;
    bool ok = false;
    const QString name = QInputDialog::getText(this, tr("Rename Chain"), tr("Chain name:"), QLineEdit::Normal,
                                               rack->chains[size_t(chain)].name, &ok);
    if (!ok)
        return;
    editRoot([&](EffectChain& r, Timeline&) {
        if (EffectChain* c = resolveChain(r, path))
            (*c)[size_t(rackIndex)].chains[size_t(chain)].name = name;
    }, tr("Rename Chain"));
}

void DeviceChainView::setChainFlag(const ChainPath& path, int rackIndex, int chain, bool solo, bool on)
{
    editRoot([&](EffectChain& r, Timeline&) {
        if (EffectChain* c = resolveChain(r, path)) {
            auto& chains = (*c)[size_t(rackIndex)].chains;
            if (chain < int(chains.size()))
                (solo ? chains[size_t(chain)].solo : chains[size_t(chain)].enabled) = on;
        }
    }, solo ? tr("Solo Chain") : tr("Chain On/Off"));
}

void DeviceChainView::selectChain(int rackId, int chain)
{
    m_rackChain[rackId] = chain;
    scheduleRebuild();
}

void DeviceChainView::selectDevice(int effectId, bool toggle)
{
    m_maskEditing = effectId; // its mask (if it has one) goes on the picture
    if (toggle) {
        if (!m_selected.erase(effectId))
            m_selected.insert(effectId);
    } else {
        m_selected = {effectId};
    }
    scheduleRebuild();
}

void DeviceChainView::toggleFold(int effectId)
{
    if (!m_folded.erase(effectId))
        m_folded.insert(effectId);
    scheduleRebuild();
}

void DeviceChainView::setDragging(bool on)
{
    m_dragging = on;
    if (!on)
        scheduleRebuild();
}

// ---- Developer self-test ---------------------------------------------------------------------------

int DeviceChainView::runSelfTest(QStringList& log)
{
    int failures = 0;
    auto check = [&](bool ok, const QString& what) {
        log << QStringLiteral("%1  %2").arg(ok ? QStringLiteral("PASS") : QStringLiteral("FAIL"), what);
        if (!ok)
            ++failures;
    };
    auto settle = [] {
        for (int i = 0; i < 5; ++i)
            QCoreApplication::processEvents();
    };
    auto chainText = [this] {
        std::function<QString(const EffectChain&)> fmt = [&](const EffectChain& c) {
            QStringList parts;
            for (const auto& e : c) {
                if (!e.isRack()) {
                    parts << e.type;
                    continue;
                }
                QStringList chains;
                for (const auto& ch : e.chains)
                    chains << fmt(ch.effects);
                parts << QStringLiteral("rack(%1)").arg(chains.join(QLatin1Char('|')));
            }
            return parts.join(QLatin1Char(' '));
        };
        const EffectChain* r = root(m_project->timeline());
        return r ? fmt(*r) : QStringLiteral("<no target>");
    };
    auto strips = [this] {
        std::vector<ChainStrip*> out;
        for (QWidget* w : m_scroll->findChildren<QWidget*>())
            if (auto* s = dynamic_cast<ChainStrip*>(w))
                out.push_back(s);
        return out; // outer strip first (creation order)
    };
    auto send = [&](QWidget* target, QPoint pos, QMimeData* mime, Qt::KeyboardModifiers mods) {
        const Qt::DropActions actions = Qt::CopyAction | Qt::MoveAction;
        QDragEnterEvent enter(pos, actions, mime, Qt::LeftButton, mods);
        QApplication::sendEvent(target, &enter);
        QDragMoveEvent move(pos, actions, mime, Qt::LeftButton, mods);
        QApplication::sendEvent(target, &move);
        QDropEvent drop(QPointF(pos), actions, mime, Qt::LeftButton, mods);
        QApplication::sendEvent(target, &drop);
        settle();
        return drop.dropAction();
    };
    auto effectMime = [](const char* type) {
        auto* m = new QMimeData;
        m->setData(QString::fromLatin1(EffectsBrowser::kEffectMime), QByteArray(type));
        return m;
    };
    auto deviceMime = [](const ChainPath& p, int index) {
        auto* m = new QMimeData;
        m->setData(QString::fromLatin1(kDeviceMime), QStringLiteral("%1|%2").arg(pathToString(p)).arg(index).toUtf8());
        return m;
    };
    // A point in `s` just in front of device i (past the last one for a big i).
    auto gapBefore = [](ChainStrip* s, int i) {
        std::vector<QWidget*> devices;
        for (QWidget* w : s->findChildren<QWidget*>(Qt::FindDirectChildrenOnly))
            if (dynamic_cast<DeviceWidget*>(w))
                devices.push_back(w);
        std::sort(devices.begin(), devices.end(), [](QWidget* a, QWidget* b) { return a->x() < b->x(); });
        if (devices.empty())
            return QPoint(10, 20);
        if (i >= int(devices.size()))
            return QPoint(devices.back()->geometry().right() + 10, 20);
        return QPoint(devices[size_t(i)]->geometry().left() + 2, 20);
    };

    if (!targetExists()) {
        check(false, QStringLiteral("no target"));
        return failures;
    }
    editRoot([](EffectChain& r, Timeline&) { r.clear(); }, QStringLiteral("test reset"));
    settle();
    const int undoBase = m_project->undoStack()->index();

    std::unique_ptr<QMimeData> m(effectMime("invert"));
    const Qt::DropAction firstDrop = send(strips().front(), QPoint(20, 20), m.get(), Qt::NoModifier);
    // A "move" would make the effects browser delete the dragged item.
    check(firstDrop == Qt::CopyAction, QStringLiteral("an effect dropped from the browser is a copy, not a move (action %1)").arg(int(firstDrop)));
    m.reset(effectMime("sepia"));
    send(strips().front(), gapBefore(strips().front(), 0), m.get(), Qt::NoModifier);
    m.reset(effectMime("gaussian_blur"));
    send(strips().front(), gapBefore(strips().front(), 9), m.get(), Qt::NoModifier);
    check(chainText() == QStringLiteral("sepia invert gaussian_blur"),
          QStringLiteral("drop effects from the browser in front of / behind devices: %1").arg(chainText()));

    m.reset(deviceMime({}, 0));
    send(strips().front(), gapBefore(strips().front(), 9), m.get(), Qt::NoModifier);
    check(chainText() == QStringLiteral("invert gaussian_blur sepia"), QStringLiteral("drag a device behind the others: %1").arg(chainText()));
    m.reset(deviceMime({}, 0));
    send(strips().front(), gapBefore(strips().front(), 9), m.get(), Qt::ControlModifier);
    std::set<int> ids;
    int count = 0;
    forEachEffect(*root(m_project->timeline()), [&](const EffectInstance& e) {
        ids.insert(e.id);
        ++count;
    });
    check(chainText() == QStringLiteral("invert gaussian_blur sepia invert") && int(ids.size()) == count,
          QStringLiteral("Ctrl+drag copies a device (new id): %1").arg(chainText()));

    // Group the first two into a rack, then make a parallel chain by dropping on its chain list.
    const EffectChain r0 = *root(m_project->timeline());
    m_selected = {r0[0].id, r0[1].id};
    groupSelection();
    settle();
    check(chainText() == QStringLiteral("rack(invert gaussian_blur) sepia invert"), QStringLiteral("group into a rack: %1").arg(chainText()));
    RackChainList* list = nullptr;
    for (QWidget* w : m_scroll->findChildren<QWidget*>())
        if (auto* l = dynamic_cast<RackChainList*>(w))
            list = l;
    m.reset(effectMime("vignette"));
    if (list)
        send(list, QPoint(20, list->height() - 20), m.get(), Qt::NoModifier);
    check(chainText() == QStringLiteral("rack(invert gaussian_blur|vignette) sepia invert"),
          QStringLiteral("drop on the chain list adds a parallel chain: %1").arg(chainText()));
    // The new chain is shown; drag sepia into it, in front of vignette.
    auto all = strips();
    m.reset(deviceMime({}, 1));
    if (all.size() >= 2)
        send(all.back(), gapBefore(all.back(), 0), m.get(), Qt::NoModifier);
    check(chainText() == QStringLiteral("rack(invert gaussian_blur|sepia vignette) invert"),
          QStringLiteral("drag a device into a rack chain: %1").arg(chainText()));
    // ...and a device out of the rack to the end of the main chain.
    all = strips();
    m.reset(deviceMime(ChainPath{}.child(0, 1), 1));
    send(all.front(), gapBefore(all.front(), 9), m.get(), Qt::NoModifier);
    check(chainText() == QStringLiteral("rack(invert gaussian_blur|sepia) invert vignette"),
          QStringLiteral("drag a device out of a rack: %1").arg(chainText()));

    // A knob drag is one undo step and changes the value.
    flushEdit();
    const int before = m_project->undoStack()->index();
    Knob* mixKnob = nullptr;
    for (Knob* k : m_scroll->findChildren<Knob*>())
        if (k->def().id == QLatin1String("mix"))
            mixKnob = k;
    if (mixKnob) {
        const QPointF c = QRectF(mixKnob->rect()).center();
        QMouseEvent press(QEvent::MouseButtonPress, c, mixKnob->mapToGlobal(c), Qt::LeftButton, Qt::LeftButton, Qt::NoModifier);
        QApplication::sendEvent(mixKnob, &press);
        for (int dy = 10; dy <= 100; dy += 10) {
            const QPointF pt = c + QPointF(0, dy); // downwards = less
            QMouseEvent move(QEvent::MouseMove, pt, mixKnob->mapToGlobal(pt), Qt::NoButton, Qt::LeftButton, Qt::NoModifier);
            QApplication::sendEvent(mixKnob, &move);
        }
        QMouseEvent release(QEvent::MouseButtonRelease, c, mixKnob->mapToGlobal(c), Qt::LeftButton, Qt::NoButton, Qt::NoModifier);
        QApplication::sendEvent(mixKnob, &release);
    }
    flushEdit();
    const auto& rack = root(m_project->timeline())->front();
    const double mix = rack.params.count(QStringLiteral("mix")) ? rack.params.at(QStringLiteral("mix")).value : -1;
    check(mixKnob && std::abs(mix - 50) < 1.0 && m_project->undoStack()->index() == before + 1,
          QStringLiteral("dragging the Dry/Wet knob down 100 px: %1 % in one undo step").arg(mix));

    // Presets: a chain preset dropped into the panel, a device preset loaded into a device.
    {
        PresetLibrary& lib = PresetLibrary::instance();
        const QString oldDir = lib.directory();
        const QString dir = QDir::temp().filePath(QStringLiteral("mvp-ui-test-presets"));
        QDir(dir).removeRecursively();
        lib.setDirectory(dir);
        EffectInstance sepia = makeEffectOfType(QStringLiteral("sepia"), 1);
        sepia.params[QStringLiteral("amount")].value = 0.3;
        const QString single = lib.save(QStringLiteral("Light Sepia"), false, QStringLiteral("sepia"), {sepia});
        const QString chainId = lib.save(QStringLiteral("Two Looks"), false, QString(),
                                         {makeEffectOfType(QStringLiteral("posterize"), 2), makeEffectOfType(QStringLiteral("emboss"), 3)});
        settle();
        auto* mime = new QMimeData;
        mime->setData(QString::fromLatin1(EffectsBrowser::kPresetMime), chainId.toUtf8());
        std::unique_ptr<QMimeData> owned(mime);
        all = strips();
        send(all.front(), gapBefore(all.front(), 0), mime, Qt::NoModifier);
        check(chainText().startsWith(QStringLiteral("posterize emboss ")), QStringLiteral("a chain preset dropped in front of the first device: %1").arg(chainText()));
        // Load "Light Sepia" into a sepia device.
        const EffectChain* r = root(m_project->timeline());
        int sepiaIndex = -1;
        for (int i = 0; r && i < int(r->size()); ++i)
            if ((*r)[size_t(i)].type == QLatin1String("sepia"))
                sepiaIndex = i;
        if (sepiaIndex < 0) {
            editRoot([](EffectChain& rr, Timeline& tl) { rr.push_back(makeEffectOfType(QStringLiteral("sepia"), tl.allocId())); }, QStringLiteral("test"));
            sepiaIndex = int(root(m_project->timeline())->size()) - 1;
        }
        const int before = m_project->undoStack()->index();
        loadPreset({}, sepiaIndex, single);
        const EffectInstance& dev = (*root(m_project->timeline()))[size_t(sepiaIndex)];
        check(dev.params.count(QStringLiteral("amount")) && std::abs(dev.params.at(QStringLiteral("amount")).value - 0.3) < 1e-9
                  && m_project->undoStack()->index() == before + 1,
              QStringLiteral("loading a device preset sets its settings in one undo step"));
        lib.setDirectory(oldDir);
        QDir(dir).removeRecursively();
        settle();
    }

    // ---- Masks: the button switches one on, its settings and Track Motion show,
    // and it is drawn and dragged on the preview ----
    if (!isAudio()) {
        if (root(m_project->timeline())->empty())
            editRoot([](EffectChain& rr, Timeline& tl) { rr.push_back(makeEffectOfType(QStringLiteral("invert"), tl.allocId())); }, QStringLiteral("test"));
        settle();
        rebuild();
        const int id = (*root(m_project->timeline()))[0].id;
        toggleMask({}, 0);
        settle();
        rebuild();
        const EffectInstance& dev = (*root(m_project->timeline()))[0];
        const auto shape = dev.params.find(QStringLiteral("mask.shape"));
        bool trackButton = false;
        for (QPushButton* b : m_scroll->findChildren<QPushButton*>())
            trackButton |= b->text() == tr("Track Motion");
        const bool onPicture = m_preview && m_maskOverlay && m_preview->overlay() == m_maskOverlay.get();
        check(dev.id == id && shape != dev.params.end() && shape->second.value == 2 && trackButton && (onPicture || !m_preview),
              QStringLiteral("the mask button switches on an ellipse mask, shows its settings and Track Motion, and puts it on the preview"));
        if (onPicture) {
            m_preview->setImage(QImage(1280, 720, QImage::Format_RGB32)); // a picture to draw it on
            const QRectF pic = m_preview->pictureRect();
            const QPoint c = pic.center().toPoint();
            auto mouse = [&](QEvent::Type type, QPoint pos) {
                QMouseEvent ev(type, QPointF(pos), m_preview->mapToGlobal(QPointF(pos)), Qt::LeftButton,
                               type == QEvent::MouseButtonRelease ? Qt::NoButton : Qt::MouseButtons(Qt::LeftButton), Qt::NoModifier);
                QApplication::sendEvent(m_preview, &ev);
            };
            mouse(QEvent::MouseButtonPress, c);
            mouse(QEvent::MouseMove, c + QPoint(int(pic.width() / 10), 0));
            mouse(QEvent::MouseButtonRelease, c + QPoint(int(pic.width() / 10), 0));
            settle();
            const ParamMap& p = (*root(m_project->timeline()))[0].params;
            const double x = p.count(QStringLiteral("mask.x")) ? p.at(QStringLiteral("mask.x")).at(keyTime(), ParamType::Number) : 0.5;
            check(std::abs(x - 0.6) < 0.01, QStringLiteral("dragging the mask on the preview moves it (centre at %1 of the width)").arg(x, 0, 'f', 3));
            // Freehand: Alt+click the outline adds a point there, Alt+click a point removes it.
            editRoot([](EffectChain& rr, Timeline&) { rr[0].params[QStringLiteral("mask.shape")].value = 3; }, QStringLiteral("test"));
            settle();
            auto altClick = [&](QPointF pos) {
                QMouseEvent press(QEvent::MouseButtonPress, pos, m_preview->mapToGlobal(pos), Qt::LeftButton, Qt::LeftButton, Qt::AltModifier);
                QApplication::sendEvent(m_preview, &press);
                QMouseEvent release(QEvent::MouseButtonRelease, pos, m_preview->mapToGlobal(pos), Qt::LeftButton, Qt::NoButton, Qt::AltModifier);
                QApplication::sendEvent(m_preview, &release);
                settle();
            };
            auto points = [&] { return effectMask((*root(m_project->timeline()))[0].params, keyTime(), pic.size().toSize()).points; };
            const std::vector<QPointF> six = points();
            altClick(pic.topLeft() + (six[0] + six[1]) / 2); // the middle of the first side
            const std::vector<QPointF> seven = points();
            altClick(pic.topLeft() + seven[3]);
            const std::vector<QPointF> back = points();
            check(six.size() == 6 && seven.size() == 7 && QLineF(seven[1], (six[0] + six[1]) / 2).length() < 3 && seven[0] == six[0]
                      && back.size() == 6,
                  QStringLiteral("freehand mask: Alt+click the outline adds a point there (%1 -> %2 points), Alt+click a point removes it (%3)")
                      .arg(six.size()).arg(seven.size()).arg(back.size()));
        }
        toggleMask({}, 0);
        settle();
        rebuild();
        check(!m_preview || m_preview->overlay() == nullptr, QStringLiteral("the mask button switches it off again (and off the preview)"));
    }

    // Everything undoes back to an empty chain, and the view keeps up.
    while (m_project->undoStack()->index() > undoBase) {
        m_project->undoStack()->undo();
        settle();
    }
    check(chainText().isEmpty() && strips().size() == 1, QStringLiteral("undo back to an empty chain: [%1]").arg(chainText()));
    return failures;
}

// ---- UI ------------------------------------------------------------------------------------------

void DeviceChainView::updateScopeTabs()
{
    // The Live tab has one chain: no event / track / output scopes.
    const bool live = m_target.kind == FxTarget::Kind::Live;
    for (QToolButton* b : {m_scopeEvent, m_scopeTrack, m_scopeMain})
        b->setVisible(!live);
    if (live)
        return;
    const Timeline& tl = m_project->timeline();
    const bool clipExists = m_contextClip && tl.findClip(m_contextClip);
    const Track* track = nullptr;
    for (const auto& t : tl.tracks)
        if (t.id == m_contextTrack)
            track = &t;
    const bool audio = track ? track->type == TrackType::Audio : m_target.kind == FxTarget::Kind::AudioMaster;
    m_scopeEvent->setEnabled(clipExists);
    m_scopeTrack->setEnabled(track != nullptr);
    m_scopeTrack->setText(track ? (track->name.isEmpty() ? tr("Track") : tr("Track: %1").arg(track->name)) : tr("Track"));
    m_scopeMain->setText(audio ? tr("Master") : tr("Output"));
    m_scopeEvent->setChecked(m_target.kind == FxTarget::Kind::Clip);
    m_scopeTrack->setChecked(m_target.kind == FxTarget::Kind::Track);
    m_scopeMain->setChecked(m_target.kind == FxTarget::Kind::Output || m_target.kind == FxTarget::Kind::AudioMaster);
}

QString DeviceChainView::structureKey() const
{
    QString key = QStringLiteral("%1/%2|").arg(int(m_target.kind)).arg(m_target.id);
    const EffectChain* r = root(m_project->timeline());
    if (!r)
        return key + QStringLiteral("none");
    std::function<void(const EffectChain&)> walk = [&](const EffectChain& c) {
        key += QLatin1Char('[');
        for (const auto& e : c) {
            const auto shape = e.params.find(QStringLiteral("mask.shape"));
            key += QStringLiteral("%1:%2:%3:%4:%5:%6:%7").arg(e.id).arg(e.type).arg(e.enabled).arg(m_selected.count(e.id))
                       .arg(m_folded.count(e.id)).arg(m_rackChain.count(e.id) ? m_rackChain.at(e.id) : 0)
                       .arg(shape != e.params.end() && shape->second.value >= 0.5);
            for (const auto& ch : e.chains) {
                key += QStringLiteral("{%1:%2:%3").arg(ch.name).arg(ch.enabled).arg(ch.solo);
                walk(ch.effects);
                key += QLatin1Char('}');
            }
            key += QLatin1Char(',');
        }
        key += QLatin1Char(']');
    };
    walk(*r);
    return key + (isAudio() ? QStringLiteral("a") : QStringLiteral("v"));
}

int DeviceChainView::deviceBodyHeight() const
{
    // The scroll area less the horizontal scroll bar, the device title and margins;
    // as many rows of knobs as fit, so resizing rebuilds only when a row fits or goes.
    // (From the panel's own height: the scroll area isn't resized yet during a resize event.)
    const int header = m_title->parentWidget() ? m_title->parentWidget()->sizeHint().height() : 30;
    const int avail = height() - header - m_scroll->horizontalScrollBar()->sizeHint().height() - 22 - 14;
    constexpr int kKnobRow = 74; // a knob control and its spacing (see DeviceWidget)
    const int rows = std::clamp((avail - 8 + 4) / kKnobRow, 1, 6);
    return std::max(kBodyHeight, rows * kKnobRow + 12);
}

void DeviceChainView::resizeEvent(QResizeEvent* e)
{
    QWidget::resizeEvent(e);
    const int h = deviceBodyHeight();
    if (h != m_bodyHeight) {
        m_bodyHeight = h;
        m_key.clear(); // same devices, new layout
        scheduleRebuild();
    }
}

void DeviceChainView::scheduleRebuild()
{
    m_rebuildTimer.start();
}

void DeviceChainView::changeEvent(QEvent* e)
{
    QWidget::changeEvent(e);
    if (e->type() == QEvent::PaletteChange)
        rebuild();
}

void DeviceChainView::rebuild()
{
    if (m_dragging) // the drag source lives in the current widgets
        return;
    m_key = structureKey();
    m_controls.clear();
    m_displays.clear();
    const int scrollX = m_scroll->horizontalScrollBar()->value();
    if (QWidget* old = m_scroll->takeWidget())
        old->deleteLater();

    const Timeline& tl = m_project->timeline();
    const EffectChain* r = root(tl);
    QString title;
    switch (m_target.kind) {
    case FxTarget::Kind::Clip:
        if (const Clip* c = tl.findClip(m_target.id))
            title = (isAudio() ? tr("Audio Event FX: %1") : tr("Video Event FX: %1")).arg(c->isGenerated() ? generatorLabel(c->generator) : c->name);
        break;
    case FxTarget::Kind::Track:
        for (size_t i = 0; i < tl.tracks.size(); ++i)
            if (tl.tracks[i].id == m_target.id)
                title = (tl.tracks[i].type == TrackType::Image ? tr("Image Track FX: %1 (everything on the track)")
                        : tl.tracks[i].type == TrackType::Audio ? tr("Audio Track FX: %1 (everything on the track)")
                                                               : tr("Video Track FX: %1 (everything on the track)"))
                            .arg(tl.tracks[i].name.isEmpty() ? tr("Track %1").arg(i + 1) : tl.tracks[i].name);
        break;
    case FxTarget::Kind::Output: title = tr("Video Output FX (the final picture)"); break;
    case FxTarget::Kind::AudioMaster: title = tr("Audio Master FX (the final mix)"); break;
    case FxTarget::Kind::Live:
        if (m_target.id == 0) {
            title = tr("Live FX (always on)");
        } else if (m_target.id <= kLivePads) {
            const QString& name = tl.livePads[size_t(m_target.id - 1)].name;
            title = name.isEmpty() ? tr("Pad %1 FX").arg(m_target.id) : tr("Pad %1 FX: %2").arg(m_target.id).arg(name);
        } else {
            title = tr("Layer %1 FX (before it is laid over: e.g. a Chroma Keyer)").arg(m_target.id - kLivePads);
        }
        break;
    default: break;
    }
    if (!r)
        title = tr("Device Chain");
    updateScopeTabs();
    m_title->setText(title + QStringLiteral("   "));
    m_addButton->setEnabled(r != nullptr);
    m_addButton->setMenu(isAudio() ? m_audioMenu : m_videoMenu);
    m_groupButton->setEnabled(r && !m_selected.empty());
    m_clearLiveButton->setVisible(m_target.kind == FxTarget::Kind::Live && m_target.id == 0);
    m_clearLiveButton->setEnabled(r && !r->empty());

    if (!r) {
        updateMaskOverlay();
        auto* hint = new QLabel(tr("Select an event to see its effects here, or use a track's fx button, Output FX or Master FX."));
        hint->setAlignment(Qt::AlignCenter);
        theme::style(hint, QStringLiteral("color: @faint;"));
        m_scroll->setWidget(hint);
        return;
    }
    // Drop selections of devices that no longer exist.
    std::set<int> ids;
    forEachEffect(*r, [&](const EffectInstance& e) { ids.insert(e.id); });
    for (auto it = m_selected.begin(); it != m_selected.end();)
        it = ids.count(*it) ? std::next(it) : m_selected.erase(it);
    m_groupButton->setEnabled(!m_selected.empty());
    m_key = structureKey();

    auto* strip = new ChainStrip(this, {}, *r, false);
    m_scroll->setWidget(strip);
    m_scroll->horizontalScrollBar()->setValue(scrollX);
    refreshValues();
    updateMaskOverlay();
}

void DeviceChainView::refreshValues()
{
    const TimeUs t = keyTime();
    const TimeUs tol = keyTolerance();
    for (ParamControl* c : m_controls)
        c->refresh(t, tol);
    for (QWidget* d : m_displays)
        d->update();
    if (m_preview && m_maskOverlay && m_preview->overlay() == m_maskOverlay.get())
        m_preview->update();
}
