// MainWindow: cutting time out of the edit (Remove Silences, Edit by Transcript) and putting time in (Insert Time).
#include "ui/MainWindow.h"
#include "core/Captions.h"
#include "core/Cuts.h"
#include <QInputDialog>
#include "ui/Help.h"
#include "ui/TimelineWidget.h"

#include <QApplication>
#include <QDialog>
#include <QDialogButtonBox>
#include <QDoubleSpinBox>
#include <QFileInfo>
#include <QFormLayout>
#include <QHBoxLayout>
#include <QKeyEvent>
#include <QLabel>
#include <QMessageBox>
#include <QProgressDialog>
#include <QPushButton>
#include <QSettings>
#include <QShortcut>
#include <QStatusBar>
#include <QTextBlock>
#include <QTextCursor>
#include <QTextEdit>
#include <QVBoxLayout>

namespace {
// The part of the edit to work on: the time selection, or else all of it.
std::pair<TimeUs, TimeUs> workRange(const TimelineWidget* timeline, const Timeline& tl)
{
    if (timeline->hasRegion())
        return {timeline->regionStart(), timeline->regionEnd()};
    return {0, tl.duration()};
}

QString seconds(TimeUs t)
{
    return QString::number(double(t) / kUsPerSecond, 'f', 1);
}
} // namespace

void MainWindow::removeSilences()
{
    const auto [from, to] = workRange(m_timeline, m_project->timeline());
    if (to <= from) {
        statusBar()->showMessage(tr("Nothing to listen to: the timeline is empty"), 4000);
        return;
    }
    QSettings settings;
    QDialog dlg(this);
    dlg.setWindowTitle(tr("Remove Silences"));
    auto* form = new QFormLayout(&dlg);
    form->addRow(new QLabel(m_timeline->hasRegion() ? tr("Listens to the time selection's mix and cuts its quiet stretches out of every unlocked track.")
                                                    : tr("Listens to the whole mix and cuts its quiet stretches out of every unlocked track.\n"
                                                         "Make a time selection first to work on part of it.")));
    auto* threshold = new QDoubleSpinBox;
    threshold->setRange(-70, -10);
    threshold->setSuffix(tr(" dB"));
    threshold->setValue(settings.value(QStringLiteral("cuts/threshold"), -40.0).toDouble());
    threshold->setToolTip(tr("Quieter than this counts as silence. Raise it for a noisy room."));
    auto* minimum = new QDoubleSpinBox;
    minimum->setRange(0.2, 10);
    minimum->setSingleStep(0.1);
    minimum->setSuffix(tr(" s"));
    minimum->setValue(settings.value(QStringLiteral("cuts/minimum"), 0.7).toDouble());
    minimum->setToolTip(tr("Shorter pauses stay: they are part of speaking."));
    auto* keep = new QDoubleSpinBox;
    keep->setRange(0, 1);
    keep->setSingleStep(0.05);
    keep->setSuffix(tr(" s"));
    keep->setValue(settings.value(QStringLiteral("cuts/keep"), 0.15).toDouble());
    keep->setToolTip(tr("Left either side of each cut, so words don't start or end abruptly."));
    form->addRow(tr("Silent below:"), threshold);
    form->addRow(tr("At least:"), minimum);
    form->addRow(tr("Keep either side:"), keep);
    auto* buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel);
    help::addButton(buttons, QStringLiteral("editing.md#remove-silences-and-edit-by-transcript"));
    form->addRow(buttons);
    connect(buttons, &QDialogButtonBox::accepted, &dlg, &QDialog::accept);
    connect(buttons, &QDialogButtonBox::rejected, &dlg, &QDialog::reject);
    if (dlg.exec() != QDialog::Accepted)
        return;
    settings.setValue(QStringLiteral("cuts/threshold"), threshold->value());
    settings.setValue(QStringLiteral("cuts/minimum"), minimum->value());
    settings.setValue(QStringLiteral("cuts/keep"), keep->value());
    stopPlayback(false);
    QApplication::setOverrideCursor(Qt::WaitCursor);
    const std::vector<float> audio = timelineSpeechAudio(*m_project->snapshot(), from, to);
    const auto ranges = cuts::silences(audio, 16000, from, threshold->value(), TimeUs(minimum->value() * kUsPerSecond),
                                       TimeUs(keep->value() * kUsPerSecond));
    QApplication::restoreOverrideCursor();
    if (ranges.empty()) {
        statusBar()->showMessage(tr("No silences that long and that quiet"), 5000);
        return;
    }
    Timeline tl = m_project->timeline();
    const TimeUs removed = cuts::removeRanges(tl, ranges, m_project->settings().fps);
    m_project->applyEdit(tl, tr("Remove Silences"));
    m_timeline->setRegion(0, 0);
    statusBar()->showMessage(tr("Removed %n silence(s), %1 s in all (Ctrl+Z puts them back)", nullptr, int(ranges.size())).arg(seconds(removed)), 6000);
}

void MainWindow::editByTranscript()
{
    const auto [from, to] = workRange(m_timeline, m_project->timeline());
    if (to <= from) {
        statusBar()->showMessage(tr("Nothing to listen to: the timeline is empty"), 4000);
        return;
    }
    QSettings settings;
    const QString modelFile = settings.value(QStringLiteral("captions/model"), QStringLiteral("ggml-base.bin")).toString();
    const QString model = whisperModelPath(modelFile);
    if (!whisperAvailable() || !QFileInfo::exists(model)) {
        QMessageBox::information(this, tr("Edit by Transcript"),
                                 tr("This needs a speech model. Download one in Tools > Captions > Auto Captions first (it is used here too)."));
        return;
    }
    stopPlayback(false);
    TranscribeOptions options;
    options.language = settings.value(QStringLiteral("captions/language"), QStringLiteral("auto")).toString();
    options.wordTimestamps = true;
    QProgressDialog progress(tr("Listening..."), tr("Cancel"), 0, 1000, this);
    progress.setWindowModality(Qt::WindowModal);
    progress.setMinimumDuration(0);
    std::vector<Caption> words;
    QString error;
    if (!transcribeTimeline(model, options, from, to, &progress, words, &error)) {
        progress.close();
        if (!progress.wasCanceled())
            QMessageBox::warning(this, tr("Edit by Transcript"), error);
        return;
    }
    progress.close();
    for (Caption& w : words) {
        w.start += from;
        w.end += from;
        w.text = w.text.simplified();
    }
    std::erase_if(words, [](const Caption& w) { return w.text.isEmpty(); });

    // The transcript: select words and press Delete to strike them; Backspace on struck ones keeps them.
    QDialog dlg(this);
    dlg.setWindowTitle(tr("Edit by Transcript"));
    dlg.resize(760, 520);
    auto* layout = new QVBoxLayout(&dlg);
    layout->addWidget(new QLabel(tr("Select words and press Delete (or Cut Words) to take them out of the edit; "
                                    "select struck words and press Backspace (or Keep Words) to keep them.")));
    auto* text = new QTextEdit;
    text->setReadOnly(true);
    text->setTextInteractionFlags(Qt::TextSelectableByMouse | Qt::TextSelectableByKeyboard);
    QFont font = text->font();
    font.setPointSizeF(font.pointSizeF() * 1.25);
    text->setFont(font);
    std::vector<std::pair<int, int>> spans; // each word's characters in the text
    QString plain;
    for (const Caption& w : words) {
        if (!plain.isEmpty())
            plain += QLatin1Char(' ');
        spans.emplace_back(int(plain.size()), int(plain.size() + w.text.size()));
        plain += w.text;
    }
    text->setPlainText(plain);
    layout->addWidget(text, 1);
    auto* summary = new QLabel;
    layout->addWidget(summary);
    std::vector<bool> removed(words.size(), false);
    auto refresh = [&] {
        TimeUs total = 0;
        int count = 0;
        for (const cuts::Range& r : cuts::wordCuts(words, removed))
            total += r.to - r.from;
        for (bool r : removed)
            count += r;
        summary->setText(tr("%n word(s) struck: %1 s will be cut.", nullptr, count).arg(seconds(total)));
    };
    auto mark = [&](bool cut) {
        const QTextCursor sel = text->textCursor();
        const int a = std::min(sel.selectionStart(), sel.selectionEnd()), b = std::max(sel.selectionStart(), sel.selectionEnd());
        for (size_t i = 0; i < spans.size(); ++i) {
            if (spans[i].second <= a || spans[i].first >= std::max(b, a + 1))
                continue;
            removed[i] = cut;
            QTextCursor c(text->document());
            c.setPosition(spans[i].first);
            c.setPosition(spans[i].second, QTextCursor::KeepAnchor);
            QTextCharFormat f;
            f.setFontStrikeOut(cut);
            f.setForeground(cut ? QColor(220, 80, 70) : text->palette().text().color());
            c.mergeCharFormat(f);
        }
        refresh();
    };
    auto* row = new QHBoxLayout;
    auto* cutButton = new QPushButton(tr("Cut Words"));
    auto* keepButton = new QPushButton(tr("Keep Words"));
    connect(cutButton, &QPushButton::clicked, &dlg, [&] { mark(true); });
    connect(keepButton, &QPushButton::clicked, &dlg, [&] { mark(false); });
    new QShortcut(QKeySequence::Delete, text, [&] { mark(true); }, Qt::WidgetShortcut);
    new QShortcut(QKeySequence(Qt::Key_Backspace), text, [&] { mark(false); }, Qt::WidgetShortcut);
    row->addWidget(cutButton);
    row->addWidget(keepButton);
    row->addStretch(1);
    auto* buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel);
    buttons->button(QDialogButtonBox::Ok)->setText(tr("Cut from the Edit"));
    help::addButton(buttons, QStringLiteral("editing.md#remove-silences-and-edit-by-transcript"));
    row->addWidget(buttons);
    layout->addLayout(row);
    connect(buttons, &QDialogButtonBox::accepted, &dlg, &QDialog::accept);
    connect(buttons, &QDialogButtonBox::rejected, &dlg, &QDialog::reject);
    refresh();
    if (dlg.exec() != QDialog::Accepted)
        return;
    const auto ranges = cuts::wordCuts(words, removed);
    if (ranges.empty())
        return;
    Timeline tl = m_project->timeline();
    const TimeUs cut = cuts::removeRanges(tl, ranges, m_project->settings().fps);
    m_project->applyEdit(tl, tr("Edit by Transcript"));
    m_timeline->setRegion(0, 0);
    statusBar()->showMessage(tr("Cut %1 s (Ctrl+Z puts it back)").arg(seconds(cut)), 6000);
}

void MainWindow::insertTime()
{
    stopPlayback(false);
    bool ok = false;
    const double seconds = QInputDialog::getDouble(this, tr("Insert Time"),
                                                   tr("Seconds of empty time to make at the cursor, on every unlocked track\n"
                                                      "(what follows, its markers and its story chapters move later):"),
                                                   5.0, 0.04, 3600.0, 2, &ok);
    if (!ok)
        return;
    Timeline tl = m_project->timeline();
    const TimeUs added = cuts::insertTime(tl, m_timeline->cursorTime(), TimeUs(seconds * kUsPerSecond), m_project->settings().fps);
    if (added <= 0)
        return;
    m_project->applyEdit(tl, tr("Insert Time"));
    statusBar()->showMessage(tr("%1 of empty time made at the cursor.").arg(formatDuration(added)), 5000);
}
