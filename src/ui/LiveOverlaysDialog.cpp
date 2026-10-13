#include "ui/LiveOverlaysDialog.h"

#include "ui/Theme.h"

#include "core/Project.h"

#include <QCheckBox>
#include <QColorDialog>
#include <QComboBox>
#include <QDoubleSpinBox>
#include <QFileDialog>
#include <QFileInfo>
#include <QFormLayout>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QPushButton>
#include <QSettings>
#include <QSlider>
#include <QToolButton>
#include <QVBoxLayout>
#include <cmath>

namespace {
QString kindName(LiveOverlay::Kind k)
{
    switch (k) {
    case LiveOverlay::Kind::Text: return LiveOverlaysDialog::tr("Text");
    case LiveOverlay::Kind::Image: return LiveOverlaysDialog::tr("Picture");
    case LiveOverlay::Kind::Bpm: return LiveOverlaysDialog::tr("Tempo (BPM)");
    case LiveOverlay::Kind::Clock: return LiveOverlaysDialog::tr("Clock");
    case LiveOverlay::Kind::Spectrum: return LiveOverlaysDialog::tr("Spectrum");
    case LiveOverlay::Kind::Waveform: return LiveOverlaysDialog::tr("Waveform");
    case LiveOverlay::Kind::Circle: return LiveOverlaysDialog::tr("Ring");
    }
    return {};
}

QString describe(const LiveOverlay& o)
{
    QString what = kindName(o.kind);
    if (o.kind == LiveOverlay::Kind::Text && !o.text.isEmpty())
        what += QStringLiteral(": ") + o.text.left(30);
    else if (o.kind == LiveOverlay::Kind::Image && !o.image.isEmpty())
        what += QStringLiteral(": ") + QFileInfo(o.image).fileName();
    return o.enabled ? what : LiveOverlaysDialog::tr("%1 (off)").arg(what);
}
} // namespace

LiveOverlaysDialog::LiveOverlaysDialog(Project* project, QWidget* parent) : QDialog(parent), m_project(project)
{
    setWindowTitle(tr("Live Overlays"));
    auto* layout = new QHBoxLayout(this);
    auto* left = new QVBoxLayout;
    m_list = new QListWidget;
    m_list->setMinimumWidth(220);
    left->addWidget(m_list, 1);
    // Two rows of "add" buttons: things to show, and the audio input visualised.
    auto* adds = new QHBoxLayout;
    auto* addsAudio = new QHBoxLayout;
    for (int k = 0; k <= int(LiveOverlay::Kind::Circle); ++k) {
        auto* b = new QPushButton(tr("+ %1").arg(kindName(LiveOverlay::Kind(k))));
        if (k >= int(LiveOverlay::Kind::Spectrum))
            b->setToolTip(tr("Moves with the Live tab's Audio input"));
        connect(b, &QPushButton::clicked, this, [this, k] { add(k); });
        (k >= int(LiveOverlay::Kind::Spectrum) ? addsAudio : adds)->addWidget(b);
    }
    left->addLayout(adds);
    left->addLayout(addsAudio);
    auto* moves = new QHBoxLayout;
    auto* up = new QPushButton(tr("Up"));
    auto* down = new QPushButton(tr("Down"));
    auto* remove = new QPushButton(tr("Remove"));
    moves->addWidget(up);
    moves->addWidget(down);
    moves->addStretch(1);
    moves->addWidget(remove);
    left->addLayout(moves);
    layout->addLayout(left, 1);

    m_editor = new QWidget;
    auto* form = new QFormLayout(m_editor);
    m_enabled = new QCheckBox(tr("Shown"));
    form->addRow(QString(), m_enabled);
    m_text = new QLineEdit;
    m_text->setPlaceholderText(tr("Text to show"));
    form->addRow(tr("Text"), m_text);
    m_image = new QLineEdit;
    m_image->setReadOnly(true);
    m_browse = new QToolButton;
    m_browse->setText(tr("Browse..."));
    auto* pic = new QHBoxLayout;
    pic->addWidget(m_image, 1);
    pic->addWidget(m_browse);
    form->addRow(tr("Picture"), pic);
    m_anchor = new QComboBox;
    m_anchor->addItems({tr("Top left"), tr("Top"), tr("Top right"), tr("Left"), tr("Centre"), tr("Right"), tr("Bottom left"), tr("Bottom"),
                        tr("Bottom right")});
    form->addRow(tr("Position"), m_anchor);
    m_size = new QDoubleSpinBox;
    m_size->setRange(1, 100);
    m_size->setDecimals(1);
    m_size->setSuffix(tr(" % of the height"));
    form->addRow(tr("Size"), m_size);
    m_width = new QDoubleSpinBox;
    m_width->setRange(5, 100);
    m_width->setDecimals(0);
    m_width->setSuffix(tr(" % of the width"));
    form->addRow(tr("Width"), m_width);
    m_color = new QPushButton;
    form->addRow(tr("Colour"), m_color);
    m_opacity = new QSlider(Qt::Horizontal);
    m_opacity->setRange(0, 100);
    form->addRow(tr("Opacity"), m_opacity);
    m_pulse = new QCheckBox(tr("Pulse on the beat"));
    m_pulse->setToolTip(tr("Grows a little on each beat of the beat clock"));
    form->addRow(QString(), m_pulse);
    auto* note = new QLabel(tr("Overlays are drawn over the live output and recorded with it (\"With effects\")."));
    note->setWordWrap(true);
    theme::style(note, QStringLiteral("color: @dim;"));
    form->addRow(note);
    layout->addWidget(m_editor, 1);

    connect(m_list, &QListWidget::currentRowChanged, this, &LiveOverlaysDialog::showSelected);
    connect(m_enabled, &QCheckBox::toggled, this, [this](bool on) { change([on](LiveOverlay& o) { o.enabled = on; }, tr("Show Overlay")); });
    connect(m_text, &QLineEdit::editingFinished, this, [this] {
        const QString t = m_text->text();
        change([t](LiveOverlay& o) { o.text = t; }, tr("Overlay Text"));
    });
    connect(m_browse, &QToolButton::clicked, this, [this] {
        QSettings settings;
        const QString path = QFileDialog::getOpenFileName(this, tr("Overlay Picture"), settings.value(QStringLiteral("dirs/overlay")).toString(),
                                                          tr("Pictures (*.png *.jpg *.jpeg *.bmp *.gif *.webp *.svg)"));
        if (path.isEmpty())
            return;
        settings.setValue(QStringLiteral("dirs/overlay"), QFileInfo(path).absolutePath());
        change([path](LiveOverlay& o) { o.image = path; }, tr("Overlay Picture"));
    });
    connect(m_anchor, qOverload<int>(&QComboBox::currentIndexChanged), this, [this](int a) { change([a](LiveOverlay& o) { o.anchor = a; }, tr("Overlay Position")); });
    connect(m_size, qOverload<double>(&QDoubleSpinBox::valueChanged), this, [this](double v) { change([v](LiveOverlay& o) { o.size = v; }, tr("Overlay Size")); });
    connect(m_width, qOverload<double>(&QDoubleSpinBox::valueChanged), this, [this](double v) { change([v](LiveOverlay& o) { o.width = v; }, tr("Overlay Width")); });
    connect(m_opacity, &QSlider::valueChanged, this, [this](int v) { change([v](LiveOverlay& o) { o.opacity = v / 100.0; }, tr("Overlay Opacity")); });
    connect(m_pulse, &QCheckBox::toggled, this, [this](bool on) { change([on](LiveOverlay& o) { o.pulse = on; }, tr("Overlay Pulse")); });
    connect(m_color, &QPushButton::clicked, this, [this] {
        const int row = m_list->currentRow();
        const auto& list = m_project->timeline().liveOverlays;
        if (row < 0 || row >= int(list.size()))
            return;
        const QColor c = QColorDialog::getColor(QColor(QRgb(list[size_t(row)].color)), this, tr("Overlay Colour"));
        if (c.isValid())
            change([rgb = int(c.rgb() & 0xffffff)](LiveOverlay& o) { o.color = rgb; }, tr("Overlay Colour"));
    });
    auto move = [this](int delta) {
        const int row = m_list->currentRow();
        Timeline tl = m_project->timeline();
        const int to = row + delta;
        if (row < 0 || to < 0 || to >= int(tl.liveOverlays.size()))
            return;
        std::swap(tl.liveOverlays[size_t(row)], tl.liveOverlays[size_t(to)]);
        m_project->applyEdit(tl, tr("Move Overlay"));
        m_list->setCurrentRow(to);
    };
    connect(up, &QPushButton::clicked, this, [move] { move(-1); });
    connect(down, &QPushButton::clicked, this, [move] { move(1); });
    connect(remove, &QPushButton::clicked, this, [this] {
        const int row = m_list->currentRow();
        Timeline tl = m_project->timeline();
        if (row < 0 || row >= int(tl.liveOverlays.size()))
            return;
        tl.liveOverlays.erase(tl.liveOverlays.begin() + row);
        m_project->applyEdit(tl, tr("Remove Overlay"));
    });
    connect(m_project, &Project::changed, this, &LiveOverlaysDialog::refresh);
    refresh();
    resize(720, 380);
}

void LiveOverlaysDialog::add(int kind)
{
    Timeline tl = m_project->timeline();
    LiveOverlay o;
    o.kind = LiveOverlay::Kind(kind);
    switch (o.kind) {
    case LiveOverlay::Kind::Text: o.text = tr("Hello"); o.anchor = 7; break;
    case LiveOverlay::Kind::Image: o.anchor = 2; o.size = 15; break;
    case LiveOverlay::Kind::Bpm: o.anchor = 0; o.size = 6; o.pulse = true; break;
    case LiveOverlay::Kind::Clock: o.anchor = 2; o.size = 6; break;
    case LiveOverlay::Kind::Spectrum: o.anchor = 7; o.size = 18; o.width = 70; o.opacity = 0.85; break;
    case LiveOverlay::Kind::Waveform: o.anchor = 4; o.size = 25; o.width = 80; break;
    case LiveOverlay::Kind::Circle: o.anchor = 4; o.size = 40; o.pulse = true; break;
    }
    tl.liveOverlays.push_back(o);
    m_project->applyEdit(tl, tr("Add Overlay"));
    m_list->setCurrentRow(int(tl.liveOverlays.size()) - 1);
    if (o.kind == LiveOverlay::Kind::Image)
        m_browse->click();
}

void LiveOverlaysDialog::change(const std::function<void(LiveOverlay&)>& fn, const QString& label)
{
    if (m_updating)
        return;
    const int row = m_list->currentRow();
    Timeline tl = m_project->timeline();
    if (row < 0 || row >= int(tl.liveOverlays.size()))
        return;
    fn(tl.liveOverlays[size_t(row)]);
    if (tl != m_project->timeline())
        m_project->applyEdit(tl, label);
}

void LiveOverlaysDialog::refresh()
{
    const auto& list = m_project->timeline().liveOverlays;
    const int row = m_list->currentRow();
    m_updating = true;
    m_list->clear();
    for (const LiveOverlay& o : list)
        m_list->addItem(describe(o));
    m_list->setCurrentRow(std::min(std::max(row, list.empty() ? -1 : 0), int(list.size()) - 1));
    m_updating = false;
    showSelected();
}

void LiveOverlaysDialog::showSelected()
{
    const auto& list = m_project->timeline().liveOverlays;
    const int row = m_list->currentRow();
    const bool valid = row >= 0 && row < int(list.size());
    m_editor->setEnabled(valid);
    if (!valid)
        return;
    const LiveOverlay& o = list[size_t(row)];
    m_updating = true;
    m_enabled->setChecked(o.enabled);
    m_text->setText(o.text);
    m_text->setEnabled(o.kind == LiveOverlay::Kind::Text);
    m_image->setText(o.image);
    m_image->setEnabled(o.kind == LiveOverlay::Kind::Image);
    m_browse->setEnabled(o.kind == LiveOverlay::Kind::Image);
    m_anchor->setCurrentIndex(o.anchor);
    m_size->setValue(o.size);
    m_width->setValue(o.width);
    m_width->setEnabled(o.kind == LiveOverlay::Kind::Spectrum || o.kind == LiveOverlay::Kind::Waveform);
    m_color->setEnabled(o.kind != LiveOverlay::Kind::Image);
    theme::style(m_color, QStringLiteral("background: #%1; min-width: 60px;").arg(o.color & 0xffffff, 6, 16, QLatin1Char('0')));
    m_opacity->setValue(int(std::lround(o.opacity * 100)));
    m_pulse->setChecked(o.pulse);
    m_updating = false;
}
