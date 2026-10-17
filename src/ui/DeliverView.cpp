#include "ui/DeliverView.h"

#include "core/Captions.h"
#include "core/Compositor.h"
#include "core/Exporter.h"
#include "core/Profiles.h"
#include "core/Project.h"
#include "core/YouTubeUpload.h"
#include "ui/AudioFinishingWindow.h"
#include "ui/RenderQueue.h"
#include "ui/Theme.h"

#include <QApplication>
#include <QClipboard>
#include <QComboBox>
#include <QCryptographicHash>
#include <QDateTime>
#include <QDesktopServices>
#include <QDir>
#include <QFileDialog>
#include <QFileInfo>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLabel>
#include <QLineEdit>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QRegularExpression>
#include <QSaveFile>
#include <QSettings>
#include <QShowEvent>
#include <QTimer>
#include <QTreeWidget>
#include <QUrl>
#include <QVBoxLayout>

#include <algorithm>

namespace {
const ExportPreset* presetNamed(const QString& name)
{
    for (const ExportPreset& p : exportPresets())
        if (p.name == name)
            return &p;
    return nullptr;
}

QString sizeText(qint64 bytes)
{
    return bytes >= 1024 * 1024 ? QStringLiteral("%1 MB").arg(double(bytes) / (1024 * 1024), 0, 'f', 1)
                                : QStringLiteral("%1 KB").arg(std::max<qint64>(1, bytes / 1024));
}

constexpr int kHistoryLimit = 300;
} // namespace

DeliverView::DeliverView(Project* project, Hooks hooks, QWidget* parent) : QWidget(parent), m_project(project), m_hooks(std::move(hooks))
{
    setObjectName(QStringLiteral("deliverView"));
    auto* columns = new QHBoxLayout(this);
    auto* left = new QVBoxLayout;
    auto* right = new QVBoxLayout;
    columns->addLayout(left, 1);
    columns->addLayout(right, 1);

    // ---- Checks ----
    auto* checksBox = new QGroupBox(tr("Checks"));
    auto* checksLayout = new QVBoxLayout(checksBox);
    m_checks = new QTreeWidget;
    m_checks->setObjectName(QStringLiteral("deliverChecks"));
    m_checks->setHeaderHidden(true);
    m_checks->setColumnCount(2);
    m_checks->setRootIsDecorated(false);
    m_checks->header()->setStretchLastSection(false);
    m_checks->header()->setSectionResizeMode(0, QHeaderView::Stretch);
    m_checks->header()->setSectionResizeMode(1, QHeaderView::ResizeToContents);
    checksLayout->addWidget(m_checks);
    auto* checkRow = new QHBoxLayout;
    auto* again = new QPushButton(tr("Check Again"));
    auto* measure = new QPushButton(tr("Measure Loudness"));
    measure->setObjectName(QStringLiteral("deliverMeasure"));
    checkRow->addWidget(again);
    checkRow->addWidget(measure);
    checkRow->addStretch();
    checksLayout->addLayout(checkRow);
    left->addWidget(checksBox, 1);
    connect(again, &QPushButton::clicked, this, [this] { refresh(); });
    connect(measure, &QPushButton::clicked, this, [this] {
        const auto snap = m_project->snapshot();
        if (snap->timeline.duration() <= 0)
            return;
        m_loudness = analyzeAudioMix(this, *snap);
        runChecks();
    });

    // ---- Outputs ----
    auto* outBox = new QGroupBox(tr("Outputs"));
    auto* outLayout = new QVBoxLayout(outBox);
    m_outputs = new QTreeWidget;
    m_outputs->setObjectName(QStringLiteral("deliverOutputs"));
    m_outputs->setHeaderLabels({tr("Template"), tr("File"), tr("State")});
    m_outputs->setRootIsDecorated(false);
    m_outputs->header()->setSectionResizeMode(0, QHeaderView::ResizeToContents);
    m_outputs->header()->setSectionResizeMode(1, QHeaderView::Stretch);
    outLayout->addWidget(m_outputs);
    auto* addRow = new QHBoxLayout;
    m_addTemplate = new QComboBox;
    m_addTemplate->setObjectName(QStringLiteral("deliverTemplate"));
    for (const ExportPreset& p : exportPresets())
        if (presetAvailable(p))
            m_addTemplate->addItem(p.category.isEmpty() ? p.name : p.category + QStringLiteral(" / ") + p.name, p.name);
    auto* add = new QPushButton(tr("Add"));
    auto* remove = new QPushButton(tr("Remove"));
    addRow->addWidget(m_addTemplate, 1);
    addRow->addWidget(add);
    addRow->addWidget(remove);
    outLayout->addLayout(addRow);
    auto* folderRow = new QHBoxLayout;
    m_folder = new QLineEdit;
    m_folder->setObjectName(QStringLiteral("deliverFolder"));
    auto* browse = new QPushButton(tr("Browse..."));
    m_render = new QPushButton(tr("Render"));
    m_render->setObjectName(QStringLiteral("deliverRender"));
    folderRow->addWidget(new QLabel(tr("Folder")));
    folderRow->addWidget(m_folder, 1);
    folderRow->addWidget(browse);
    folderRow->addWidget(m_render);
    outLayout->addLayout(folderRow);
    left->addWidget(outBox, 1);
    connect(add, &QPushButton::clicked, this, [this] {
        QStringList names = outputs();
        const QString name = m_addTemplate->currentData().toString();
        if (!names.contains(name))
            setOutputs(names << name);
    });
    connect(remove, &QPushButton::clicked, this, [this] {
        if (auto* item = m_outputs->currentItem()) {
            QStringList names = outputs();
            names.removeAll(item->data(0, Qt::UserRole).toString());
            setOutputs(names);
        }
    });
    connect(m_outputs, &QTreeWidget::itemChanged, this, [this](QTreeWidgetItem*, int column) {
        if (column == 0) {
            QSettings().setValue(settingsKey(QStringLiteral("off")), [this] {
                QStringList off;
                for (int i = 0; i < m_outputs->topLevelItemCount(); ++i)
                    if (m_outputs->topLevelItem(i)->checkState(0) != Qt::Checked)
                        off << m_outputs->topLevelItem(i)->data(0, Qt::UserRole).toString();
                return off;
            }());
            runChecks();
        }
    });
    connect(browse, &QPushButton::clicked, this, [this] {
        const QString dir = QFileDialog::getExistingDirectory(this, tr("Render Into"), m_folder->text());
        if (!dir.isEmpty()) {
            m_folder->setText(QDir::toNativeSeparators(dir));
            QSettings().setValue(settingsKey(QStringLiteral("folder")), dir);
            fillOutputs();
        }
    });
    connect(m_folder, &QLineEdit::editingFinished, this, [this] {
        QSettings().setValue(settingsKey(QStringLiteral("folder")), QDir::fromNativeSeparators(m_folder->text()));
        fillOutputs();
    });
    connect(m_render, &QPushButton::clicked, this, [this] {
        QString error;
        if (renderOutputs(&error) == 0 && !error.isEmpty())
            m_outputs->setToolTip(error);
    });

    // ---- Publish pack ----
    auto* packBox = new QGroupBox(tr("Publish Pack"));
    auto* pack = new QVBoxLayout(packBox);
    m_title = new QLineEdit;
    m_title->setObjectName(QStringLiteral("deliverTitle"));
    pack->addWidget(new QLabel(tr("Title")));
    pack->addWidget(m_title);
    auto* thumbRow = new QHBoxLayout;
    m_thumbLabel = new QLabel(tr("No thumbnail yet"));
    m_thumbLabel->setObjectName(QStringLiteral("deliverThumbnail"));
    m_thumbLabel->setFixedSize(256, 144);
    m_thumbLabel->setAlignment(Qt::AlignCenter);
    theme::style(m_thumbLabel, QStringLiteral("border: 1px solid @line; color: @dim;"));
    auto* thumbButtons = new QVBoxLayout;
    auto* fromCursor = new QPushButton(tr("Use Cursor Frame"));
    fromCursor->setObjectName(QStringLiteral("deliverThumbFromCursor"));
    thumbButtons->addWidget(fromCursor);
    thumbButtons->addStretch();
    thumbRow->addWidget(m_thumbLabel);
    thumbRow->addLayout(thumbButtons);
    thumbRow->addStretch();
    pack->addWidget(new QLabel(tr("Thumbnail (the title is drawn on it)")));
    pack->addLayout(thumbRow);
    auto* descRow = new QHBoxLayout;
    descRow->addWidget(new QLabel(tr("Description")), 1);
    auto* fromChapters = new QPushButton(tr("Chapters"));
    fromChapters->setToolTip(tr("Add the YouTube chapters from the story chapters or markers"));
    m_listingTrack = new QComboBox;
    m_listingTrack->setToolTip(tr("An audio track to list, \"01. Name - 04:19\", for an album video"));
    auto* listing = new QPushButton(tr("Track Listing"));
    descRow->addWidget(fromChapters);
    descRow->addWidget(m_listingTrack);
    descRow->addWidget(listing);
    pack->addLayout(descRow);
    m_description = new QPlainTextEdit;
    m_description->setObjectName(QStringLiteral("deliverDescription"));
    pack->addWidget(m_description, 1);
    auto* packButtons = new QHBoxLayout;
    auto* copy = new QPushButton(tr("Copy Description"));
    auto* save = new QPushButton(tr("Save Pack"));
    save->setObjectName(QStringLiteral("deliverSavePack"));
    save->setToolTip(tr("Writes the description, the thumbnail (under 2 MB) and the captions (.srt) into the output folder"));
    auto* packStatus = new QLabel;
    theme::style(packStatus, QStringLiteral("color: @dim;"));
    packButtons->addWidget(copy);
    packButtons->addWidget(save);
    packButtons->addWidget(packStatus, 1);
    pack->addLayout(packButtons);
    right->addWidget(packBox, 3);
    connect(m_title, &QLineEdit::textEdited, this, [this] {
        QSettings().setValue(settingsKey(QStringLiteral("title")), m_title->text());
        if (!m_thumbFrame.isNull()) {
            m_thumb = upload::thumbnail(m_thumbFrame, m_title->text());
            m_thumbLabel->setPixmap(QPixmap::fromImage(m_thumb.scaled(m_thumbLabel->size(), Qt::KeepAspectRatio, Qt::SmoothTransformation)));
        }
    });
    connect(m_description, &QPlainTextEdit::textChanged, this, [this] {
        if (!m_filling)
            QSettings().setValue(settingsKey(QStringLiteral("description")), m_description->toPlainText());
    });
    connect(fromCursor, &QPushButton::clicked, this, [this] { pickThumbnail(); });
    connect(fromChapters, &QPushButton::clicked, this, [this] {
        const QString chapters = upload::youtubeChapters(m_project->timeline());
        if (!chapters.isEmpty())
            m_description->appendPlainText((m_description->toPlainText().isEmpty() ? QString() : QStringLiteral("\n")) + chapters);
    });
    connect(listing, &QPushButton::clicked, this, [this] {
        const int track = m_listingTrack->currentData().toInt();
        const QString text = track >= 0 ? upload::trackListing(*m_project, track) : QString();
        if (!text.isEmpty())
            m_description->appendPlainText((m_description->toPlainText().isEmpty() ? QString() : QStringLiteral("\n")) + text);
    });
    connect(copy, &QPushButton::clicked, this, [this, packStatus] {
        QApplication::clipboard()->setText(m_description->toPlainText());
        packStatus->setText(tr("Description copied."));
    });
    connect(save, &QPushButton::clicked, this, [this, packStatus] {
        QString error;
        packStatus->setText(savePack(&error) ? tr("Saved in %1").arg(m_folder->text()) : error);
    });

    // ---- History ----
    auto* historyBox = new QGroupBox(tr("History"));
    auto* historyLayout = new QVBoxLayout(historyBox);
    m_history = new QTreeWidget;
    m_history->setObjectName(QStringLiteral("deliverHistory"));
    m_history->setHeaderLabels({tr("When"), tr("File"), tr("Template"), tr("Size")});
    m_history->setRootIsDecorated(false);
    m_history->header()->setSectionResizeMode(1, QHeaderView::Stretch);
    historyLayout->addWidget(m_history);
    auto* historyButtons = new QHBoxLayout;
    auto* openFile = new QPushButton(tr("Open File"));
    auto* openFolder = new QPushButton(tr("Open Folder"));
    historyButtons->addWidget(openFile);
    historyButtons->addWidget(openFolder);
    historyButtons->addStretch();
    historyLayout->addLayout(historyButtons);
    right->addWidget(historyBox, 2);
    auto selectedFile = [this] { return m_history->currentItem() ? m_history->currentItem()->data(1, Qt::UserRole).toString() : QString(); };
    connect(openFile, &QPushButton::clicked, this, [selectedFile] {
        if (!selectedFile().isEmpty())
            QDesktopServices::openUrl(QUrl::fromLocalFile(selectedFile()));
    });
    connect(openFolder, &QPushButton::clicked, this, [this, selectedFile] {
        const QString dir = selectedFile().isEmpty() ? QDir::fromNativeSeparators(m_folder->text()) : QFileInfo(selectedFile()).absolutePath();
        QDesktopServices::openUrl(QUrl::fromLocalFile(dir));
    });

    m_poll = new QTimer(this);
    m_poll->setInterval(500);
    connect(m_poll, &QTimer::timeout, this, &DeliverView::pollQueue);
    // Edits while the tab is open: check again a moment later.
    auto* again2 = new QTimer(this);
    again2->setSingleShot(true);
    again2->setInterval(600);
    connect(again2, &QTimer::timeout, this, [this] { runChecks(); });
    connect(m_project, &Project::changed, this, [this, again2] {
        if (isVisible())
            again2->start();
    });
}

QString DeliverView::baseName() const
{
    return m_project->filePath().isEmpty() ? tr("Untitled") : QFileInfo(m_project->filePath()).completeBaseName();
}

QString DeliverView::settingsKey(const QString& what) const
{
    const QString file = m_project->filePath();
    const QString id = file.isEmpty() ? QStringLiteral("untitled")
                                      : QString::fromLatin1(QCryptographicHash::hash(QDir::cleanPath(file).toLower().toUtf8(),
                                                                                     QCryptographicHash::Md5).toHex());
    return QStringLiteral("deliver/%1/%2").arg(id, what);
}

void DeliverView::showEvent(QShowEvent* e)
{
    QWidget::showEvent(e);
    refresh();
}

void DeliverView::refresh()
{
    QSettings settings;
    QString folder = settings.value(settingsKey(QStringLiteral("folder"))).toString();
    if (folder.isEmpty())
        folder = m_project->filePath().isEmpty() ? Profiles::instance().folder(QStringLiteral("Exports")) : QFileInfo(m_project->filePath()).absolutePath();
    if (folder.isEmpty())
        folder = QDir::homePath();
    m_folder->setText(QDir::toNativeSeparators(folder));
    m_title->setText(settings.value(settingsKey(QStringLiteral("title")), baseName()).toString());
    m_filling = true;
    m_description->setPlainText(settings.value(settingsKey(QStringLiteral("description"))).toString());
    m_filling = false;
    m_listingTrack->clear();
    const auto& tracks = m_project->timeline().tracks;
    for (int i = 0; i < int(tracks.size()); ++i)
        if (tracks[size_t(i)].type == TrackType::Audio && !tracks[size_t(i)].clips.empty())
            m_listingTrack->addItem(tracks[size_t(i)].name.isEmpty() ? tr("Audio %1").arg(i + 1) : tracks[size_t(i)].name, i);
    fillOutputs();
    runChecks();
    fillHistory();
}

QStringList DeliverView::outputs() const
{
    QStringList names;
    for (int i = 0; i < m_outputs->topLevelItemCount(); ++i)
        names << m_outputs->topLevelItem(i)->data(0, Qt::UserRole).toString();
    return names;
}

void DeliverView::setOutputs(const QStringList& templateNames)
{
    QSettings().setValue(settingsKey(QStringLiteral("outputs")), templateNames);
    fillOutputs();
    runChecks();
}

QString DeliverView::outputPath(const ExportPreset& p) const
{
    // "05 Y2K - YouTube 1080p.mp4", "05 Y2K - MP3.mp3": the template's name up to its first " - " or "(".
    QString label = p.name.section(QStringLiteral(" - "), 0, 0).section(QStringLiteral(" ("), 0, 0).trimmed();
    label.replace(QRegularExpression(QStringLiteral("[\\\\/:*?\"<>|]")), QStringLiteral(" "));
    const QString ext = p.extension.isEmpty() ? QStringLiteral("mp4") : p.extension;
    return QDir(QDir::fromNativeSeparators(m_folder->text())).filePath(baseName() + QStringLiteral(" - ") + label.simplified() + QLatin1Char('.') + ext);
}

void DeliverView::fillOutputs()
{
    QSettings settings;
    const QStringList names = settings.value(settingsKey(QStringLiteral("outputs")), QStringList{QStringLiteral("YouTube 1080p")}).toStringList();
    const QStringList off = settings.value(settingsKey(QStringLiteral("off"))).toStringList();
    const QSignalBlocker block(m_outputs);
    m_outputs->clear();
    for (const QString& name : names) {
        const ExportPreset* p = presetNamed(name);
        if (!p)
            continue;
        const QString path = outputPath(*p);
        auto* item = new QTreeWidgetItem(m_outputs, {name, QFileInfo(path).fileName(), QString()});
        item->setData(0, Qt::UserRole, name);
        item->setData(1, Qt::UserRole, path);
        item->setToolTip(1, QDir::toNativeSeparators(path));
        item->setFlags(item->flags() | Qt::ItemIsUserCheckable);
        item->setCheckState(0, off.contains(name) ? Qt::Unchecked : Qt::Checked);
        item->setToolTip(0, p->notes);
    }
    int ticked = 0;
    for (int i = 0; i < m_outputs->topLevelItemCount(); ++i)
        ticked += m_outputs->topLevelItem(i)->checkState(0) == Qt::Checked;
    m_render->setText(ticked > 1 ? tr("Render %1").arg(ticked) : tr("Render"));
}

TimeUs DeliverView::limitFor(const ExportPreset& p) const
{
    const auto it = m_trim.find(p.name);
    return it == m_trim.end() ? 0 : it->second;
}

void DeliverView::runChecks()
{
    m_checks->clear();
    const Timeline& tl = m_project->timeline();
    const TimeUs duration = tl.duration();
    int toLookAt = 0;
    enum { Note = 2 };
    auto row = [&](int ok, const QString& text, const QString& action = {}, std::function<void()> fix = {}) {
        auto* item = new QTreeWidgetItem(m_checks, {(ok == Note ? QStringLiteral("•  ") : ok ? QStringLiteral("✓  ") : QStringLiteral("⚠  ")) + text});
        item->setForeground(0, ok == Note ? theme::c().dim : ok ? theme::c().text : theme::c().warn);
        item->setData(0, Qt::UserRole, ok);
        if (!ok)
            ++toLookAt;
        if (!action.isEmpty() && fix) {
            auto* button = new QPushButton(action);
            connect(button, &QPushButton::clicked, this, [fix] { fix(); });
            m_checks->setItemWidget(item, 1, button);
        }
    };
    if (duration <= 0) {
        row(false, tr("The timeline is empty."));
        return;
    }
    if (m_project->filePath().isEmpty())
        row(false, tr("The project isn't saved: file names and History follow its name."), tr("Save"), [this] {
            if (m_hooks.save && m_hooks.save())
                refresh();
        });

    // Offline media.
    int offline = 0;
    TimeUs firstOffline = -1;
    for (const Track& t : tl.tracks)
        for (const Clip& c : t.clips)
            if (!c.isGenerated()) {
                const auto media = m_project->mediaItem(c.mediaId);
                if (!media || !media->info.ok()) {
                    ++offline;
                    if (firstOffline < 0 || c.start < firstOffline)
                        firstOffline = c.start;
                }
            }
    if (offline)
        row(false, tr("%n event(s) use offline media.", nullptr, offline), tr("Go There"), [this, firstOffline] { m_hooks.seek(firstOffline); });
    else
        row(true, tr("No offline media."));

    // Nothing on screen: gaps of a second or more with no picture.
    std::vector<std::pair<TimeUs, TimeUs>> spans;
    for (const Track& t : tl.tracks)
        if (isVisualTrack(t.type) && !t.muted)
            for (const Clip& c : t.clips)
                if (!c.muted)
                    spans.emplace_back(c.start, c.end());
    std::sort(spans.begin(), spans.end());
    std::vector<std::pair<TimeUs, TimeUs>> gaps;
    TimeUs covered = 0;
    for (const auto& [a, b] : spans) {
        if (a - covered >= kUsPerSecond)
            gaps.emplace_back(covered, a);
        covered = std::max(covered, b);
    }
    if (duration - covered >= kUsPerSecond)
        gaps.emplace_back(covered, duration);
    if (gaps.empty())
        row(true, tr("Picture all the way through."));
    else
        row(false,
            tr("Nothing on screen %1–%2").arg(formatDuration(gaps.front().first), formatDuration(gaps.front().second))
                + (gaps.size() > 1 ? tr(" (and %n more)", nullptr, int(gaps.size() - 1)) : QString()),
            tr("Go There"), [this, at = gaps.front().first] { m_hooks.seek(at); });

    // Loudness, once measured.
    if (!m_loudness)
        row(Note, tr("Loudness not measured yet (Measure Loudness)."));
    else if (!m_loudness->error.isEmpty())
        row(false, m_loudness->error);
    else if (!m_loudness->cancelled) {
        const finishing::Target target = finishing::targets().front();
        const QStringList warnings = finishing::warnings(*m_loudness, target);
        const QString measured = tr("Loudness %1 LUFS, true peak %2 dBTP").arg(m_loudness->integrated, 0, 'f', 1).arg(m_loudness->truePeak, 0, 'f', 1);
        if (warnings.isEmpty())
            row(true, measured + QStringLiteral(" (") + target.name + QLatin1Char(')'));
        else
            row(false, measured + QStringLiteral(": ") + warnings.front(), tr("Audio Finishing..."), [this] {
                if (m_hooks.audioFinishing)
                    m_hooks.audioFinishing();
                m_loudness.reset();
                runChecks();
            });
    }

    // YouTube chapters.
    QStringList chapterWarnings;
    const QString chapters = upload::youtubeChapters(tl, &chapterWarnings);
    if (chapters.isEmpty())
        row(Note, tr("No chapters (story chapters or markers make YouTube chapters)."));
    else if (chapterWarnings.isEmpty())
        row(true, tr("%n YouTube chapter(s).", nullptr, int(chapters.count(QLatin1Char('\n')) + 1)));
    else
        for (const QString& w : chapterWarnings)
            row(false, w);

    // Templates with a platform length limit.
    for (int i = 0; i < m_outputs->topLevelItemCount(); ++i) {
        auto* item = m_outputs->topLevelItem(i);
        const ExportPreset* p = presetNamed(item->data(0, Qt::UserRole).toString());
        if (!p || item->checkState(0) != Qt::Checked || p->maxSeconds <= 0)
            continue;
        const TimeUs limit = TimeUs(p->maxSeconds) * kUsPerSecond;
        if (duration <= limit)
            continue;
        if (limitFor(*p) > 0)
            row(true, tr("%1 renders the first %2.").arg(p->name, formatDuration(limitFor(*p))));
        else
            row(false, tr("%1 is %2 long; %3 accepts up to %4.").arg(p->name, formatDuration(duration), p->name.section(QLatin1Char(' '), 0, 0), formatDuration(limit)),
                tr("Trim"), [this, name = p->name, limit] {
                    m_trim[name] = limit;
                    runChecks();
                });
    }

    // Captions for the pack.
    const int captions = int(timelineCaptions(tl).size());
    row(captions ? 1 : int(Note), captions ? tr("%n caption(s) go into the pack and subtitle tracks.", nullptr, captions) : tr("No captions."));
    m_checks->setToolTip(toLookAt ? tr("%n to look at", nullptr, toLookAt) : tr("All clear"));
}

int DeliverView::renderOutputs(QString* error)
{
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
    for (int i = 0; i < m_outputs->topLevelItemCount(); ++i) {
        auto* item = m_outputs->topLevelItem(i);
        const ExportPreset* p = presetNamed(item->data(0, Qt::UserRole).toString());
        if (!p || item->checkState(0) != Qt::Checked)
            continue;
        ExportSettings s = withGpuEncoder(settingsFromPreset(*p, snap->settings));
        s.path = outputPath(*p);
        s.rangeStart = 0;
        s.rangeEnd = limitFor(*p) > 0 ? std::min(duration, limitFor(*p)) : duration;
        s.captions = captions;
        QString why;
        if (queue->add(baseName(), snap, s, &why)) {
            ++added;
            m_pending[QDir::cleanPath(s.path)] = p->name;
            item->setText(2, tr("Waiting"));
        } else {
            item->setText(2, why);
            if (error)
                *error = why;
        }
    }
    if (!m_queueConnected) {
        m_queueConnected = true;
        connect(queue, &RenderQueue::rendered, this, [this](const QString& path) {
            const QString clean = QDir::cleanPath(path);
            if (m_pending.count(clean)) {
                remember(m_project->filePath(), path, m_pending[clean]);
                fillHistory();
            }
        });
    }
    if (added) {
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
    const QJsonArray jobs = queue->status()[QStringLiteral("jobs")].toArray();
    bool active = false;
    for (int i = 0; i < m_outputs->topLevelItemCount(); ++i) {
        auto* item = m_outputs->topLevelItem(i);
        const QString path = QDir::cleanPath(item->data(1, Qt::UserRole).toString());
        for (const auto& v : jobs) {
            const QJsonObject job = v.toObject();
            if (QDir::cleanPath(job[QStringLiteral("path")].toString()) != path)
                continue;
            const QString state = job[QStringLiteral("state")].toString();
            if (state == QLatin1String("rendering")) {
                item->setText(2, tr("Rendering %1%").arg(int(job[QStringLiteral("progress")].toDouble() * 100)));
                active = true;
            } else if (state == QLatin1String("waiting")) {
                item->setText(2, tr("Waiting"));
                active = true;
            } else if (state == QLatin1String("done"))
                item->setText(2, tr("Done"));
            else
                item->setText(2, job[QStringLiteral("error")].toString().isEmpty() ? state : job[QStringLiteral("error")].toString());
        }
    }
    if (!active)
        m_poll->stop();
}

void DeliverView::pickThumbnail()
{
    const auto snap = m_project->snapshot();
    const ProjectSettings& ps = snap->settings;
    QApplication::setOverrideCursor(Qt::WaitCursor);
    Compositor renderer(true);
    m_thumbFrame = renderer.render(*snap, m_hooks.cursor ? m_hooks.cursor() : 0, QSize(ps.width, ps.height));
    QApplication::restoreOverrideCursor();
    m_thumb = upload::thumbnail(m_thumbFrame, m_title->text());
    m_thumbLabel->setPixmap(QPixmap::fromImage(m_thumb.scaled(m_thumbLabel->size(), Qt::KeepAspectRatio, Qt::SmoothTransformation)));
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
    if (!m_description->toPlainText().trimmed().isEmpty())
        text += (text.isEmpty() ? QString() : QStringLiteral("\n\n")) + m_description->toPlainText().trimmed();
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

void DeliverView::remember(const QString& projectPath, const QString& file, const QString& templateName)
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
                               {QStringLiteral("bytes"), double(QFileInfo(file).size())},
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
    for (int i = history.size() - 1; i >= 0; --i) {
        const QJsonObject h = history[i].toObject();
        if (h[QStringLiteral("project")].toString() != project)
            continue;
        const QString file = h[QStringLiteral("file")].toString();
        const QDateTime when = QDateTime::fromString(h[QStringLiteral("when")].toString(), Qt::ISODate);
        auto* item = new QTreeWidgetItem(m_history, {QLocale().toString(when, QLocale::ShortFormat), QFileInfo(file).fileName(),
                                                      h[QStringLiteral("template")].toString(),
                                                      QFileInfo::exists(file) ? sizeText(QFileInfo(file).size()) : tr("deleted")});
        item->setData(1, Qt::UserRole, file);
        item->setToolTip(1, QDir::toNativeSeparators(file));
    }
}
