#include "ui/SetlistDialog.h"

#include "ui/Theme.h"

#include "ui/Help.h"
#include "core/LivePadSets.h"

#include <QCheckBox>
#include <QComboBox>
#include <QDialogButtonBox>
#include <QDoubleSpinBox>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QLabel>
#include <QPushButton>
#include <QTableWidget>
#include <QVBoxLayout>

namespace {
enum Column { kName, kPadSet, kBpm, kLock, kScene, kNotes, kColumns };
}

SetlistDialog::SetlistDialog(const std::vector<LiveSong>& songs, const std::function<LiveSong()>& current, QWidget* parent)
    : QDialog(parent), m_current(current)
{
    setWindowTitle(tr("Setlist"));
    resize(860, 420);
    auto* layout = new QVBoxLayout(this);
    auto* help = new QLabel(tr("The songs of a set, in order. Recalling one (the setlist arrows, Page Down / Page Up, MIDI) loads its\n"
                               "pad set, sets its tempo (locked, or listened for from there; 0 = find it by listening) and recalls its scene."));
    theme::style(help, QStringLiteral("color: @dim;"));
    layout->addWidget(help);
    m_table = new QTableWidget(0, kColumns);
    m_table->setObjectName(QStringLiteral("setlistTable"));
    m_table->setHorizontalHeaderLabels({tr("Song"), tr("Pad set"), tr("BPM"), tr("Lock"), tr("Scene"), tr("Notes")});
    m_table->horizontalHeader()->setSectionResizeMode(kNotes, QHeaderView::Stretch);
    m_table->setColumnWidth(kName, 200);
    m_table->setColumnWidth(kPadSet, 160);
    m_table->setColumnWidth(kBpm, 90);
    m_table->setColumnWidth(kLock, 50);
    m_table->setColumnWidth(kScene, 110);
    m_table->setSelectionBehavior(QAbstractItemView::SelectRows);
    m_table->setSelectionMode(QAbstractItemView::SingleSelection);
    m_table->verticalHeader()->setDefaultSectionSize(28);
    layout->addWidget(m_table, 1);

    auto* buttons = new QHBoxLayout;
    auto* add = new QPushButton(tr("Add"));
    auto* addCurrent = new QPushButton(tr("Add Current"));
    addCurrent->setToolTip(tr("A song from what is on now: the pad set, the tempo (and its lock) and the last scene recalled"));
    auto* remove = new QPushButton(tr("Remove"));
    auto* up = new QPushButton(tr("Up"));
    auto* down = new QPushButton(tr("Down"));
    for (QPushButton* b : {add, addCurrent, remove, up, down})
        buttons->addWidget(b);
    buttons->addStretch(1);
    layout->addLayout(buttons);
    auto* box = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel);
    help::addButton(box, QStringLiteral("live.md#the-setlist"));
    layout->addWidget(box);
    connect(box, &QDialogButtonBox::accepted, this, &QDialog::accept);
    connect(box, &QDialogButtonBox::rejected, this, &QDialog::reject);
    connect(add, &QPushButton::clicked, this, [this] {
        LiveSong s;
        s.name = tr("Song %1").arg(m_table->rowCount() + 1);
        addSong(s);
    });
    connect(addCurrent, &QPushButton::clicked, this, [this] {
        LiveSong s = m_current ? m_current() : LiveSong();
        if (s.name.isEmpty())
            s.name = tr("Song %1").arg(m_table->rowCount() + 1);
        addSong(s);
    });
    connect(remove, &QPushButton::clicked, this, [this] {
        const int row = m_table->currentRow();
        if (row >= 0)
            m_table->removeRow(row);
    });
    connect(up, &QPushButton::clicked, this, [this] { move(-1); });
    connect(down, &QPushButton::clicked, this, [this] { move(1); });
    for (const LiveSong& s : songs)
        addSong(s);
}

void SetlistDialog::addSong(const LiveSong& song)
{
    const int row = m_table->rowCount();
    m_table->insertRow(row);
    m_table->setItem(row, kName, new QTableWidgetItem(song.name));
    auto* sets = new QComboBox;
    sets->addItem(tr("(keep the pads)"), QString());
    for (const QString& n : padsets::names())
        sets->addItem(n, n);
    if (!song.padSet.isEmpty() && sets->findData(song.padSet) < 0)
        sets->addItem(tr("%1 (missing)").arg(song.padSet), song.padSet);
    sets->setCurrentIndex(std::max(0, sets->findData(song.padSet)));
    m_table->setCellWidget(row, kPadSet, sets);
    auto* bpm = new QDoubleSpinBox;
    bpm->setRange(0, 400);
    bpm->setDecimals(2);
    bpm->setSpecialValueText(tr("listen"));
    bpm->setValue(song.bpm);
    m_table->setCellWidget(row, kBpm, bpm);
    auto* lockHolder = new QWidget;
    auto* lockLayout = new QHBoxLayout(lockHolder);
    lockLayout->setContentsMargins(0, 0, 0, 0);
    lockLayout->setAlignment(Qt::AlignCenter);
    auto* lock = new QCheckBox;
    lock->setChecked(song.lockBpm);
    lock->setToolTip(tr("Lock the tempo (or start from it and keep listening)"));
    lockLayout->addWidget(lock);
    m_table->setCellWidget(row, kLock, lockHolder);
    auto* scene = new QComboBox;
    scene->addItem(tr("(none)"), -1);
    for (int i = 0; i < kLiveScenes; ++i)
        scene->addItem(tr("Scene %1").arg(i + 1), i);
    scene->setCurrentIndex(std::max(0, scene->findData(song.scene)));
    m_table->setCellWidget(row, kScene, scene);
    m_table->setItem(row, kNotes, new QTableWidgetItem(song.notes));
    m_table->setCurrentCell(row, kName);
}

std::vector<LiveSong> SetlistDialog::songs() const
{
    std::vector<LiveSong> out;
    for (int row = 0; row < m_table->rowCount(); ++row) {
        LiveSong s;
        s.name = m_table->item(row, kName) ? m_table->item(row, kName)->text().trimmed() : QString();
        if (auto* c = qobject_cast<QComboBox*>(m_table->cellWidget(row, kPadSet)))
            s.padSet = c->currentData().toString();
        if (auto* b = qobject_cast<QDoubleSpinBox*>(m_table->cellWidget(row, kBpm)))
            s.bpm = b->value();
        if (QWidget* w = m_table->cellWidget(row, kLock))
            if (auto* l = w->findChild<QCheckBox*>())
                s.lockBpm = l->isChecked();
        if (auto* c = qobject_cast<QComboBox*>(m_table->cellWidget(row, kScene)))
            s.scene = c->currentData().toInt();
        s.notes = m_table->item(row, kNotes) ? m_table->item(row, kNotes)->text().trimmed() : QString();
        out.push_back(s);
    }
    return out;
}

void SetlistDialog::move(int delta)
{
    const int row = m_table->currentRow(), to = row + delta;
    if (row < 0 || to < 0 || to >= m_table->rowCount())
        return;
    std::vector<LiveSong> list = songs();
    std::swap(list[size_t(row)], list[size_t(to)]);
    m_table->setRowCount(0);
    for (const LiveSong& s : list)
        addSong(s);
    m_table->setCurrentCell(to, kName);
}
