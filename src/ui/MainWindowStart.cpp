#include "core/FileNames.h"
#include "core/Profiles.h"
#include <QRegularExpression>
// Starting out: the welcome window, File > New's shapes and frame rates,
// Help > System Check (what this computer can do, and how to fix what it
// can't), and the check for a newer release on GitHub.

#include "ui/MainWindow.h"

#include "ui/Theme.h"

#include "core/Captions.h"
#include "core/Hardware.h"
#include "core/Segmentation.h"
#include "core/NodeGraph.h"
#include "core/RetroShaders.h"
#include <QEventLoop>
#include <QProgressDialog>
#include <QTemporaryFile>
#include "core/VideoShare.h"
#include "core/YouTube.h"
#include "core/Vst3Host.h"
#include "ui/CrashReport.h"
#include "ui/ExportDialog.h"
#include "ui/Help.h"
#include "ui/LiveView.h"
#include "ui/ProjectSettingsDialog.h"
#include "ui/ShortcutsDialog.h"

#include <QApplication>
#include <QTimer>
#include <QCheckBox>
#include <QComboBox>
#include <QCoreApplication>
#include <QDate>
#include <QDesktopServices>
#include <QDialog>
#include <QDialogButtonBox>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QFormLayout>
#include <QGridLayout>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QJsonDocument>
#include <QKeyEvent>
#include <QLineEdit>
#include <QMenuBar>
#include <QJsonObject>
#include <QLabel>
#include <QListWidget>
#include <QMenu>
#include <QMessageBox>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QPushButton>
#include <QSettings>
#include <QStandardPaths>
#include <QUrlQuery>
#include <QStatusBar>
#include <QTabWidget>
#include <QToolButton>
#include <QTreeWidget>
#include <QVBoxLayout>
#include <QVersionNumber>

namespace {

// File > New: the shape of the video (0x0: the first video added decides).
struct Shape {
    const char* name;
    const char* note;
    int width, height;
};
const Shape kShapes[] = {
    {"Match the first video I add", "Its frame size and frame rate", 0, 0},
    {"Landscape 1080p (1920 x 1080)", "YouTube and most video", 1920, 1080},
    {"Landscape 4K (3840 x 2160)", "4K YouTube, TVs", 3840, 2160},
    {"Landscape 720p (1280 x 720)", "Smaller files, quicker to edit", 1280, 720},
    {"Vertical 9:16 (1080 x 1920)", "TikTok, Reels, Shorts, Stories", 1080, 1920},
    {"Square 1:1 (1080 x 1080)", "Instagram and Facebook feeds", 1080, 1080},
    {"Portrait 4:5 (1080 x 1350)", "Instagram feed posts", 1080, 1350},
};

// The search box of Find a Command: the arrow keys move through the list below it.
class CommandSearch : public QLineEdit {
public:
    QTreeWidget* list = nullptr;

protected:
    void keyPressEvent(QKeyEvent* e) override
    {
        if (list && (e->key() == Qt::Key_Up || e->key() == Qt::Key_Down || e->key() == Qt::Key_PageUp || e->key() == Qt::Key_PageDown))
            QApplication::sendEvent(list, e);
        else
            QLineEdit::keyPressEvent(e);
    }
};

QString stateMark(MainWindow::CheckItem::State s)
{
    return s == MainWindow::CheckItem::Ready ? QStringLiteral("✓") : s == MainWindow::CheckItem::Note ? QStringLiteral("!") : QStringLiteral("✗");
}

} // namespace

// ---------------------------------------------------------------------------
// File > New
// ---------------------------------------------------------------------------

void MainWindow::newProjectDialog()
{
    if (!maybeSave())
        return;
    QDialog dlg(this);
    dlg.setAttribute(Qt::WA_DontShowOnScreen, testAttribute(Qt::WA_DontShowOnScreen));
    dlg.setObjectName(QStringLiteral("newProjectDialog"));
    dlg.setWindowTitle(tr("New Project"));
    auto* layout = new QVBoxLayout(&dlg);
    layout->addWidget(new QLabel(tr("What shape is the video?")));
    auto* shapes = new QListWidget;
    shapes->setObjectName(QStringLiteral("newProjectShapes"));
    for (const Shape& s : kShapes)
        shapes->addItem(tr(s.name) + QStringLiteral("\n    ") + tr(s.note));
    shapes->setSpacing(3);
    shapes->setMinimumSize(420, 330);
    layout->addWidget(shapes);
    auto* form = new QFormLayout;
    auto* projectName = new QLineEdit(tr("Untitled"), &dlg);
    projectName->setObjectName(QStringLiteral("newProjectName"));
    if (!Profiles::instance().active().id.isEmpty()) form->addRow(tr("Project name"), projectName); else projectName->hide();
    auto* fps = new QComboBox;
    fps->setObjectName(QStringLiteral("newProjectFps"));
    for (const auto& [text, label] : {std::pair{"30", "30 fps"}, {"24", "24 fps (film)"}, {"25", "25 fps (Europe, PAL)"}, {"60", "60 fps (smooth motion, games)"},
                                      {"50", "50 fps"}, {"29.970", "29.97 fps (NTSC)"}, {"23.976", "23.976 fps"}, {"59.940", "59.94 fps"}})
        fps->addItem(tr(label), QString::fromLatin1(text));
    form->addRow(tr("Frame rate"), fps);
    layout->addLayout(form);
    auto* note = new QLabel(tr("File > Properties changes them later."));
    theme::style(note, QStringLiteral("color: @dim;"));
    layout->addWidget(note);
    auto* buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel);
    buttons->button(QDialogButtonBox::Ok)->setText(tr("Create"));
    QPushButton* custom = buttons->addButton(tr("Custom..."), QDialogButtonBox::ActionRole);
    help::addButton(buttons, QStringLiteral("getting-started.md#a-first-edit"));
    layout->addWidget(buttons);

    QSettings settings;
    shapes->setCurrentRow(std::clamp(settings.value(QStringLiteral("newProject/shape"), 0).toInt(), 0, int(std::size(kShapes)) - 1));
    fps->setCurrentIndex(std::max(0, fps->findData(settings.value(QStringLiteral("newProject/fps"), QStringLiteral("30")))));
    auto sync = [&] { fps->setEnabled(shapes->currentRow() > 0); };
    sync();
    connect(shapes, &QListWidget::currentRowChanged, &dlg, sync);
    connect(shapes, &QListWidget::itemDoubleClicked, &dlg, &QDialog::accept);
    connect(buttons, &QDialogButtonBox::accepted, &dlg, &QDialog::accept);
    connect(buttons, &QDialogButtonBox::rejected, &dlg, &QDialog::reject);
    std::optional<ProjectSettings> chosen;
    connect(custom, &QPushButton::clicked, &dlg, [&] {
        ProjectSettingsDialog props(ProjectSettings{}, {}, &dlg);
        if (props.exec() == QDialog::Accepted) {
            chosen = props.settings();
            dlg.accept();
        }
    });
    if (dlg.exec() != QDialog::Accepted)
        return;
    settings.setValue(QStringLiteral("newProject/shape"), shapes->currentRow());
    settings.setValue(QStringLiteral("newProject/fps"), fps->currentData());
    if (const Shape& s = kShapes[std::max(0, shapes->currentRow())]; !chosen && s.width > 0) {
        ProjectSettings p;
        p.width = s.width;
        p.height = s.height;
        p.fps = parseFrameRate(fps->currentData().toString(), p.fps);
        chosen = p;
    }
    newProject(chosen);
    const QString home = Profiles::instance().folder(QStringLiteral("Projects"));
    if (!home.isEmpty()) {
        const QString name = safeFileName(projectName->text(), tr("Untitled"));
        QString path = QDir(home).filePath(name + QStringLiteral(".mvp"));
        for (int n = 2; QFileInfo::exists(path); ++n) path = QDir(home).filePath(name + QStringLiteral(" %1.mvp").arg(n));
        QString error;
        if (m_project->save(path, &error)) addRecent(path);
        else QMessageBox::warning(this, tr("New Project"), tr("Could not save the new project: %1").arg(error));
    }
    updateTitle();
    updatePreviewInfo();
}

// ---------------------------------------------------------------------------
// Help > System Check
// ---------------------------------------------------------------------------

std::vector<MainWindow::CheckItem> MainWindow::systemCheck()
{
    using S = CheckItem::State;
    std::vector<CheckItem> items;
    const QString driver = tr("Update the graphics driver (from NVIDIA, AMD or Intel).");

    if (!hw::settings().gpuCompositing)
        items.push_back({tr("Preview and renders"), tr("On the CPU: GPU Acceleration is off"), tr("Turn on Options > GPU Acceleration."), S::Note});
    else if (m_rendererName.startsWith(QLatin1String("GPU")))
        items.push_back({tr("Preview and renders"), m_rendererName, {}, S::Ready});
    else
        items.push_back({tr("Preview and renders"), tr("On the CPU (slower): OpenGL 3.3 isn't available"), driver, S::Problem});

    const QString decode = hw::decodeDeviceName();
    if (!hw::settings().hardwareDecoding)
        items.push_back({tr("Video decoding"), tr("On the CPU: Hardware Video Decoding is off"), tr("Turn on Options > Hardware Video Decoding."), S::Note});
    else if (decode.isEmpty())
        items.push_back({tr("Video decoding"), tr("On the CPU (heavy 4K and HEVC files play slower)"), driver + QLatin1Char(' ') + tr("Proxies help too (Tools menu)."), S::Note});
    else
        items.push_back({tr("Video decoding"), tr("On the graphics card (%1)").arg(decode), {}, S::Ready});

    QStringList encoders;
    for (const auto& [codec, name] : {std::pair{"h264_nvenc", "NVIDIA NVENC"}, {"h264_amf", "AMD AMF"}, {"h264_qsv", "Intel Quick Sync"}})
        if (hw::encoderUsable(QString::fromLatin1(codec)))
            encoders << QString::fromLatin1(name);
    if (encoders.isEmpty())
        items.push_back({tr("Fast rendering (GPU encoders)"), tr("None: renders use the CPU encoders"),
                         tr("An NVIDIA, AMD or Intel graphics chip with an up-to-date driver renders H.264 / HEVC / AV1 many times faster."), S::Note});
    else
        items.push_back({tr("Fast rendering (GPU encoders)"), encoders.join(QStringLiteral(", ")), {}, S::Ready});

    QString error;
    if (!segmentationAvailable(SegmentModel::Fast, &error))
        items.push_back({tr("Remove Background (AI)"), tr("Not available: %1").arg(error), tr("Reinstall Mutant Video Producer (the models folder or ONNX Runtime is missing)."), S::Problem});
    else if (segmentationDevice().contains(QLatin1String("GPU")))
        items.push_back({tr("Remove Background (AI)"), tr("On the graphics card (%1)").arg(segmentationDevice()), {}, S::Ready});
    else
        items.push_back({tr("Remove Background (AI)"), tr("On the CPU (slower)"), tr("A DirectX 12 graphics card and an up-to-date driver run it on the GPU."), S::Note});

    if (!whisperAvailable()) {
        items.push_back({tr("Auto Captions"), tr("Not in this build of Mutant Video Producer"), tr("Use a release from GitHub: they include it."), S::Problem});
    } else {
        QStringList models;
        for (const char* f : {"ggml-tiny.bin", "ggml-base.bin", "ggml-small.bin", "ggml-small.en-tdrz.bin"})
            if (QFileInfo::exists(whisperModelPath(QString::fromLatin1(f))))
                models << QString::fromLatin1(f).remove(QStringLiteral("ggml-")).remove(QStringLiteral(".bin"));
        const QString where = whisperOnGpu() ? tr("On the graphics card (Vulkan)") : tr("On the CPU (slower)");
        const QString have = models.isEmpty() ? tr("no speech model yet: one is downloaded the first time (78-488 MB)")
                                              : tr("models: %1").arg(models.join(QStringLiteral(", ")));
        items.push_back({tr("Auto Captions"), where + QStringLiteral("; ") + have, whisperOnGpu() ? QString() : driver, whisperOnGpu() ? S::Ready : S::Note});
    }

    if (!vst3Available())
        items.push_back({tr("VST3 plugins"), tr("Not in this build of Mutant Video Producer"), {}, S::Note});
    else if (const int n = int(vst3InstalledPlugins().size()); n == 0)
        items.push_back({tr("VST3 plugins"), tr("None installed"), tr("Install VST3 effects to C:\\Program Files\\Common Files\\VST3 to use them."), S::Note});
    else
        items.push_back({tr("VST3 plugins"), tr("%n installed", nullptr, n), {}, S::Ready});

    if (ffmpegFolder().isEmpty())
        items.push_back({tr("Create Clip from YouTube"), tr("ffmpeg is missing (it joins the picture and sound)"),
                         tr("Reinstall Mutant Video Producer, or install FFmpeg (on Linux: sudo apt install ffmpeg)."), S::Problem});
    else
        items.push_back({tr("Create Clip from YouTube"), QFileInfo::exists(ytdlpPath()) ? tr("Ready (yt-dlp is here)") : tr("Ready: yt-dlp (18 MB) is downloaded the first time"),
                         {}, S::Ready});

    items.push_back({tr("Live: Spout"), SpoutSender::available() ? tr("Ready") : tr("Not available (needs Direct3D 11)"),
                     SpoutSender::available() ? QString() : driver, SpoutSender::available() ? S::Ready : S::Note});
    items.push_back({tr("Live: virtual camera"), VirtualCamera::installed() ? tr("Ready (OBS Virtual Camera)") : tr("Not installed"),
                     VirtualCamera::installed() ? QString() : tr("Install OBS Studio (obsproject.com) for the OBS Virtual Camera."),
                     VirtualCamera::installed() ? S::Ready : S::Note});
    return items;
}

void MainWindow::showSystemCheck()
{
    QApplication::setOverrideCursor(Qt::WaitCursor);
    const std::vector<CheckItem> items = systemCheck();
    QApplication::restoreOverrideCursor();
    QDialog dlg(this);
    dlg.setAttribute(Qt::WA_DontShowOnScreen, testAttribute(Qt::WA_DontShowOnScreen));
    dlg.setWindowTitle(tr("System Check"));
    dlg.resize(900, 420);
    auto* layout = new QVBoxLayout(&dlg);
    auto* tree = new QTreeWidget;
    tree->setObjectName(QStringLiteral("systemCheck"));
    tree->setHeaderLabels({QString(), tr("Feature"), tr("On this computer"), tr("To fix")});
    tree->setRootIsDecorated(false);
    tree->setWordWrap(true);
    for (const CheckItem& c : items) {
        auto* it = new QTreeWidgetItem(tree, {stateMark(c.state), c.what, c.status, c.fix});
        it->setForeground(0, c.state == CheckItem::Ready ? theme::c().good : c.state == CheckItem::Note ? theme::c().warn : theme::c().bad);
        for (int col = 1; col < 4; ++col)
            it->setToolTip(col, it->text(col));
    }
    tree->header()->setSectionResizeMode(0, QHeaderView::ResizeToContents);
    tree->header()->setSectionResizeMode(1, QHeaderView::ResizeToContents);
    tree->setColumnWidth(2, 300);
    layout->addWidget(tree);
    auto* version = new QLabel(tr("Mutant Video Producer %1").arg(QApplication::applicationVersion()));
    theme::style(version, QStringLiteral("color: @dim;"));
    layout->addWidget(version);
    auto* buttons = new QDialogButtonBox(QDialogButtonBox::Close);
    help::addButton(buttons, QStringLiteral("options.md#performance"));
    connect(buttons, &QDialogButtonBox::rejected, &dlg, &QDialog::reject);
    layout->addWidget(buttons);
    dlg.exec();
}

// ---------------------------------------------------------------------------
// Help > Find a Command
// ---------------------------------------------------------------------------

void MainWindow::findCommand()
{
    // Each command with the menu it is in ("Edit > Ripple Delete").
    std::map<QAction*, QString> where;
    std::function<void(QMenu*, const QString&)> walk = [&](QMenu* menu, const QString& path) {
        for (QAction* a : menu->actions())
            if (a->menu())
                walk(a->menu(), path + QStringLiteral(" > ") + QString(a->text()).remove(QLatin1Char('&')));
            else if (!a->isSeparator())
                where.emplace(a, path);
    };
    for (QAction* top : menuBar()->actions())
        if (top->menu())
            walk(top->menu(), QString(top->text()).remove(QLatin1Char('&')));

    QDialog dlg(this);
    dlg.setAttribute(Qt::WA_DontShowOnScreen, testAttribute(Qt::WA_DontShowOnScreen));
    dlg.setObjectName(QStringLiteral("findCommand"));
    dlg.setWindowTitle(tr("Find a Command"));
    dlg.resize(620, 420);
    auto* layout = new QVBoxLayout(&dlg);
    auto* search = new CommandSearch;
    search->setPlaceholderText(tr("Type part of a command: split, captions, proxy, theme..."));
    auto* list = new QTreeWidget;
    list->setHeaderLabels({tr("Command"), tr("Menu"), tr("Key")});
    list->setRootIsDecorated(false);
    list->setUniformRowHeights(true);
    search->list = list;
    layout->addWidget(search);
    layout->addWidget(list);
    for (QAction* a : ShortcutsDialog::commands(this)) {
        QString name = a->text().section(QLatin1Char('\t'), 0, 0).remove(QLatin1Char('&'));
        if (name.isEmpty() || !a->isVisible())
            continue;
        auto* it = new QTreeWidgetItem(list, {name, where.count(a) ? where[a] : tr("Window"), a->shortcut().toString(QKeySequence::NativeText)});
        it->setData(0, Qt::UserRole, QVariant::fromValue(static_cast<QObject*>(a)));
        it->setDisabled(!a->isEnabled());
    }
    list->sortItems(0, Qt::AscendingOrder);
    list->header()->setSectionResizeMode(0, QHeaderView::Stretch);
    list->header()->setSectionResizeMode(1, QHeaderView::ResizeToContents);
    list->header()->setSectionResizeMode(2, QHeaderView::ResizeToContents);
    auto filter = [list](const QString& text) {
        const QStringList words = text.split(QLatin1Char(' '), Qt::SkipEmptyParts);
        QTreeWidgetItem* first = nullptr;
        for (int i = 0; i < list->topLevelItemCount(); ++i) {
            QTreeWidgetItem* it = list->topLevelItem(i);
            const QString hay = it->text(0) + QLatin1Char(' ') + it->text(1);
            const bool show = std::all_of(words.begin(), words.end(), [&](const QString& w) { return hay.contains(w, Qt::CaseInsensitive); });
            it->setHidden(!show);
            if (show && !first && !it->isDisabled())
                first = it;
        }
        list->setCurrentItem(first);
    };
    filter({});
    QAction* chosen = nullptr;
    auto run = [&](QTreeWidgetItem* it) {
        if (it && !it->isDisabled()) {
            chosen = qobject_cast<QAction*>(it->data(0, Qt::UserRole).value<QObject*>());
            dlg.accept();
        }
    };
    connect(search, &QLineEdit::textChanged, &dlg, filter);
    connect(search, &QLineEdit::returnPressed, &dlg, [&] { run(list->currentItem()); });
    connect(list, &QTreeWidget::itemActivated, &dlg, [&](QTreeWidgetItem* it) { run(it); });
    if (dlg.exec() == QDialog::Accepted && chosen)
        chosen->trigger(); // (once the window is closed)
}

// ---------------------------------------------------------------------------
// Files from another start, and crash reports
// ---------------------------------------------------------------------------

void MainWindow::openFromElsewhere(const QStringList& paths)
{
    if (isMinimized())
        showNormal();
    raise();
    activateWindow();
    // A file double-clicked while a window of ours is open (a render's progress, Director's Dice...), a menu or a drag is
    // opened once that's done: the open one holds the project, and puts back what it had on closing.
    const auto busy = [this] {
        return QApplication::activeModalWidget() || QApplication::activePopupWidget() || QGuiApplication::mouseButtons() != Qt::NoButton
               || m_editorTransaction;
    };
    const auto open = [this, paths] {
        QString project;
        QStringList media;
        for (const QString& p : paths)
            if (QFileInfo(p).suffix().compare(QLatin1String("mvp"), Qt::CaseInsensitive) == 0)
                project = p;
            else if (QFileInfo::exists(p))
                media << p;
        if (!project.isEmpty() && maybeSave())
            openProject(project);
        if (!media.isEmpty())
            importFiles(media);
    };
    if (!busy()) {
        open();
        return;
    }
    auto* wait = new QTimer(this);
    wait->setInterval(250);
    connect(wait, &QTimer::timeout, this, [wait, busy, open] {
        if (busy())
            return;
        wait->stop();
        wait->deleteLater();
        open();
    });
    wait->start();
}

void MainWindow::offerCrashReport()
{
    const QStringList reports = crash::newReports();
    if (reports.isEmpty())
        return;
    crash::markSeen();
    QMessageBox box(QMessageBox::Warning, tr("Mutant Video Producer closed unexpectedly"),
                    tr("Mutant Video Producer closed unexpectedly last time, and saved a report of what happened. Sending it helps get the "
                       "problem fixed.\n\n%1")
                        .arg(crash::summary(reports.front())),
                    QMessageBox::Close, this);
    QPushButton* report = box.addButton(tr("Report on GitHub..."), QMessageBox::AcceptRole);
    QPushButton* show = box.addButton(tr("Show the Report"), QMessageBox::ActionRole);
    box.exec();
    if (box.clickedButton() == show || box.clickedButton() == report)
        QDesktopServices::openUrl(QUrl::fromLocalFile(crash::folder()));
    if (box.clickedButton() != report)
        return;
    // A new issue, filled in: the report's text (the browser shows it before anything is sent).
    QFile f(reports.front());
    f.open(QIODevice::ReadOnly);
    const QString text = QString::fromUtf8(f.readAll()).left(4000);
    QUrlQuery q;
    q.addQueryItem(QStringLiteral("title"), QStringLiteral("Crash: %1").arg(crash::summary(reports.front()).left(120)));
    q.addQueryItem(QStringLiteral("body"), QStringLiteral("**What I was doing:**\n\n\n**Crash report** (%1):\n```\n%2\n```\n"
                                                         "The same folder has a `.dmp` file: drag it here too if you can.")
                                               .arg(QFileInfo(reports.front()).fileName(), text));
    QUrl url(QStringLiteral("https://github.com/jeddyhhh/MutantVideoProducer/issues/new"));
    url.setQuery(q);
    QDesktopServices::openUrl(url);
}

// ---------------------------------------------------------------------------
// Updates
// ---------------------------------------------------------------------------

bool MainWindow::isNewerVersion(const QString& latest, const QString& current)
{
    auto parse = [](QString v) { return QVersionNumber::fromString(v.startsWith(QLatin1Char('v')) ? v.mid(1) : v); };
    const QVersionNumber a = parse(latest), b = parse(current);
    return !a.isNull() && !b.isNull() && a > b;
}

void MainWindow::getScreenShaders()
{
    const QUrl url(QStringLiteral("https://github.com/libretro/glsl-shaders/archive/refs/heads/master.zip"));
    const bool again = retro::installed();
    if (QMessageBox::question(this, tr("Get Screen Shaders"),
                              tr("%1 libretro's GLSL screen shaders, the ones RetroArch uses (CRT screens, scanlines, handheld LCDs, NTSC "
                                 "and VHS looks, pixel-art upscalers...), from GitHub (about 30 MB):\n%2\n\nEach shader is under its own "
                                 "licence (in its file). They go into %3 and show as effects under Retro Shaders (the GPU renderer runs "
                                 "them).")
                                  .arg(again ? tr("Download again") : tr("Download"), url.toString(), QDir::toNativeSeparators(retro::folder())))
        != QMessageBox::Yes)
        return;
    QProgressDialog progress(tr("Downloading the screen shaders..."), tr("Cancel"), 0, 1000, this);
    progress.setWindowModality(Qt::WindowModal);
    progress.setMinimumDuration(0);
    QNetworkAccessManager net;
    QNetworkRequest request(url);
    request.setHeader(QNetworkRequest::UserAgentHeader, QStringLiteral("MutantVideoProducer/%1").arg(QApplication::applicationVersion()));
    QNetworkReply* reply = net.get(request);
    QTemporaryFile zip(QDir::temp().filePath(QStringLiteral("mvp-shaders-XXXXXX.zip")));
    zip.open();
    connect(reply, &QNetworkReply::readyRead, &zip, [&] { zip.write(reply->readAll()); });
    connect(reply, &QNetworkReply::downloadProgress, &progress, [&](qint64 got, qint64 total) {
        progress.setValue(total > 0 ? int(got * 900 / total) : std::min<int>(900, int(got / 40000)));
        progress.setLabelText(tr("Downloading the screen shaders... %1 MB").arg(double(got) / 1e6, 0, 'f', 1));
    });
    QEventLoop loop;
    connect(reply, &QNetworkReply::finished, &loop, &QEventLoop::quit);
    connect(&progress, &QProgressDialog::canceled, reply, &QNetworkReply::abort);
    loop.exec();
    zip.write(reply->readAll());
    zip.flush();
    const bool ok = reply->error() == QNetworkReply::NoError;
    const QString netError = reply->errorString();
    reply->deleteLater();
    if (!ok) {
        if (!progress.wasCanceled())
            QMessageBox::warning(this, tr("Get Screen Shaders"), tr("The download failed: %1").arg(netError));
        return;
    }
    progress.setLabelText(tr("Unpacking the screen shaders..."));
    QString error;
    const bool installed = retro::installZip(zip.fileName(), &error, [&](double f) {
        progress.setValue(900 + int(f * 100));
        QCoreApplication::processEvents();
        return !progress.wasCanceled();
    });
    progress.setValue(1000);
    if (!installed) {
        if (!progress.wasCanceled())
            QMessageBox::warning(this, tr("Get Screen Shaders"), tr("They couldn't be unpacked: %1").arg(error));
        return;
    }
    emit nodes::Notifier::instance().changed(); // (the effect lists, again)
    statusBar()->showMessage(tr("%n screen shader(s) ready: Video FX > Retro Shaders", nullptr, int(retro::effects().size())), 8000);
}

void MainWindow::checkForUpdates(bool manual)
{
    auto* net = new QNetworkAccessManager(this);
    QNetworkRequest request(QUrl(QStringLiteral("https://api.github.com/repos/jeddyhhh/MutantVideoProducer/releases/latest")));
    request.setHeader(QNetworkRequest::UserAgentHeader, QStringLiteral("MutantVideoProducer/%1").arg(QApplication::applicationVersion()));
    request.setRawHeader("Accept", "application/vnd.github+json");
    QNetworkReply* reply = net->get(request);
    connect(reply, &QNetworkReply::finished, this, [this, net, reply, manual] {
        reply->deleteLater();
        net->deleteLater();
        const QJsonObject release = QJsonDocument::fromJson(reply->readAll()).object();
        const QString tag = release[QStringLiteral("tag_name")].toString();
        if (reply->error() != QNetworkReply::NoError || tag.isEmpty()) {
            if (manual)
                QMessageBox::warning(this, tr("Check for Updates"), tr("Could not reach GitHub: %1").arg(reply->errorString()));
            return;
        }
        QSettings().setValue(QStringLiteral("updates/lastCheck"), QDate::currentDate());
        if (!isNewerVersion(tag, QApplication::applicationVersion())) {
            if (manual)
                QMessageBox::information(this, tr("Check for Updates"), tr("You have the latest version (%1).").arg(QApplication::applicationVersion()));
            return;
        }
        m_updateUrl = release[QStringLiteral("html_url")].toString();
        if (!m_updateButton) {
            m_updateButton = new QToolButton;
            m_updateButton->setObjectName(QStringLiteral("updateAvailable"));
            m_updateButton->setAutoRaise(true);
            theme::style(m_updateButton, QStringLiteral("color: @warn;"));
            connect(m_updateButton, &QToolButton::clicked, this, [this] { QDesktopServices::openUrl(QUrl(m_updateUrl)); });
            statusBar()->addPermanentWidget(m_updateButton);
        }
        m_updateButton->setText(tr("Mutant Video Producer %1 is out: download").arg(tag.startsWith(QLatin1Char('v')) ? tag.mid(1) : tag));
        m_updateButton->setToolTip(m_updateUrl);
        if (manual
            && QMessageBox::question(this, tr("Check for Updates"),
                                     tr("Mutant Video Producer %1 is out (you have %2). Open its download page?").arg(tag, QApplication::applicationVersion()))
                   == QMessageBox::Yes)
            QDesktopServices::openUrl(QUrl(m_updateUrl));
    });
}

// ---------------------------------------------------------------------------
// The welcome window
// ---------------------------------------------------------------------------

void MainWindow::showWelcome()
{
    QDialog dlg(this);
    dlg.setAttribute(Qt::WA_DontShowOnScreen, testAttribute(Qt::WA_DontShowOnScreen));
    dlg.setObjectName(QStringLiteral("welcome"));
    dlg.setWindowTitle(tr("Welcome to Mutant Video Producer"));
    std::function<void()> then; // what to do once the window has closed
    auto choose = [&](std::function<void()> f) {
        then = std::move(f);
        dlg.accept();
    };
    auto* layout = new QVBoxLayout(&dlg);
    auto* title = new QLabel(theme::css(tr("<span style='font-size:20pt; font-weight:600;'>Welcome to Mutant Video Producer</span>"
                                           "&nbsp;&nbsp;<span style='color:@dim;'>%1</span>"))
                                 .arg(QApplication::applicationVersion()));
    layout->addWidget(title);
    auto* intro = new QLabel(tr("A video editor (the <b>Editor</b> tab) and a live effects workspace for cameras, screens and "
                                "videos (the <b>Live</b> tab)."));
    intro->setWordWrap(true);
    layout->addWidget(intro);

    auto* columns = new QGridLayout;
    auto heading = [](const QString& text) {
        auto* l = new QLabel(QStringLiteral("<b>%1</b>").arg(text));
        l->setContentsMargins(0, 10, 0, 2);
        return l;
    };
    auto button = [](const QString& text, const QString& tip = {}) {
        auto* b = new QPushButton(text);
        b->setToolTip(tip);
        b->setMinimumHeight(30);
        return b;
    };

    // Start
    auto* start = new QVBoxLayout;
    start->addWidget(heading(tr("Start")));
    QPushButton* newButton = button(tr("New Project..."), tr("Choose the shape (landscape, vertical, square) and frame rate"));
    QPushButton* openButton = button(tr("Open Project..."));
    auto* exampleButton = new QToolButton;
    exampleButton->setText(tr("Open an Example Project"));
    exampleButton->setPopupMode(QToolButton::InstantPopup);
    exampleButton->setToolButtonStyle(Qt::ToolButtonTextOnly);
    exampleButton->setMinimumHeight(30);
    exampleButton->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
    auto* examples = new QMenu(exampleButton);
    for (const QFileInfo& f : QDir(QDir(QCoreApplication::applicationDirPath()).filePath(QStringLiteral("examples")))
                                  .entryInfoList({QStringLiteral("*.mvp")}, QDir::Files, QDir::Name)) {
        QString name = f.completeBaseName().replace(QLatin1Char('-'), QLatin1Char(' '));
        name[0] = name[0].toUpper();
        examples->addAction(name, &dlg, [&, base = f.completeBaseName()] { choose([this, base] { openExample(base); }); });
    }
    exampleButton->setMenu(examples);
    exampleButton->setEnabled(!examples->isEmpty());
    start->addWidget(newButton);
    start->addWidget(openButton);
    start->addWidget(exampleButton);
    start->addWidget(heading(tr("Recent projects")));
    auto* recent = new QListWidget;
    recent->setObjectName(QStringLiteral("welcomeRecent"));
    for (const QString& path : QSettings().value(Profiles::instance().settingsKey(QStringLiteral("recent/projects"))).toStringList())
        if (QFileInfo(path).suffix().compare(QStringLiteral("mvp"), Qt::CaseInsensitive) == 0 && QFileInfo::exists(path)) {
            auto* it = new QListWidgetItem(QFileInfo(path).completeBaseName(), recent);
            it->setToolTip(QDir::toNativeSeparators(path));
            it->setData(Qt::UserRole, path);
        }
    if (recent->count() == 0)
        recent->addItem(tr("(none yet)"));
    recent->setMinimumHeight(140);
    start->addWidget(recent, 1);

    // Learn
    auto* learn = new QVBoxLayout;
    learn->addWidget(heading(tr("Learn")));
    QPushButton* firstEdit = button(tr("Tutorial: Your First Edit"), tr("Step by step, with an example project"));
    QPushButton* tutorials = button(tr("All Tutorials"), tr("Sixteen short lessons covering the whole program"));
    QPushButton* guide = button(tr("User Guide"), tr("Every feature and how to use it"));
    QPushButton* live = button(tr("Try the Live Tab"), tr("Effects on a camera, a screen or video files in real time"));
    QPushButton* keys = button(tr("Keyboard Shortcuts"));
    for (QPushButton* b : {firstEdit, tutorials, guide, live, keys})
        learn->addWidget(b);
    auto* tip = new QLabel(tr("Tip: Shift+F1 opens the help for the part of the window you are in, and every dialog has a "
                              "Help button."));
    tip->setWordWrap(true);
    theme::style(tip, QStringLiteral("color: @dim;"));
    learn->addWidget(tip);
    learn->addStretch();

    columns->addLayout(start, 0, 0);
    columns->addLayout(learn, 0, 1);
    columns->setColumnStretch(0, 1);
    columns->setColumnStretch(1, 1);
    columns->setHorizontalSpacing(24);
    layout->addLayout(columns);

    // This computer
    auto* checkRow = new QHBoxLayout;
    auto* checkLabel = new QLabel(tr("Checking what this computer can do..."));
    checkLabel->setObjectName(QStringLiteral("welcomeCheck"));
    checkLabel->setWordWrap(true);
    QPushButton* checkButton = button(tr("System Check..."));
    checkRow->addWidget(checkLabel, 1);
    checkRow->addWidget(checkButton);
    layout->addLayout(checkRow);
    if (!m_updateUrl.isEmpty()) {
        auto* update = new QLabel(QStringLiteral("<a href='%1'>%2</a>").arg(m_updateUrl, m_updateButton ? m_updateButton->text() : tr("A newer Mutant Video Producer is out")));
        update->setOpenExternalLinks(true);
        layout->addWidget(update);
    }

    auto* bottom = new QHBoxLayout;
    auto* showAtStart = new QCheckBox(tr("Show this window when Mutant Video Producer starts"));
    showAtStart->setChecked(QSettings().value(QStringLiteral("startup/welcome"), true).toBool());
    connect(showAtStart, &QCheckBox::toggled, &dlg, [](bool on) { QSettings().setValue(QStringLiteral("startup/welcome"), on); });
    auto* close = new QPushButton(tr("Close"));
    bottom->addWidget(showAtStart);
    bottom->addStretch();
    bottom->addWidget(close);
    layout->addLayout(bottom);

    connect(close, &QPushButton::clicked, &dlg, &QDialog::reject);
    connect(newButton, &QPushButton::clicked, &dlg, [&] { choose([this] { newProjectDialog(); }); });
    connect(openButton, &QPushButton::clicked, &dlg, [&] { choose([this] { openProjectDialog(); }); });
    connect(recent, &QListWidget::itemActivated, &dlg, [&](QListWidgetItem* it) {
        if (const QString path = it->data(Qt::UserRole).toString(); !path.isEmpty())
            choose([this, path] {
                if (maybeSave())
                    openProject(path);
            });
    });
    connect(firstEdit, &QPushButton::clicked, &dlg, [&] { choose([this] { showHelp(QStringLiteral("tutorials/01-first-edit.md")); }); });
    connect(tutorials, &QPushButton::clicked, &dlg, [&] { choose([this] { showHelp(QStringLiteral("tutorials/README.md")); }); });
    connect(guide, &QPushButton::clicked, &dlg, [&] { choose([this] { showHelp(QStringLiteral("README.md")); }); });
    connect(keys, &QPushButton::clicked, &dlg, [&] { choose([this] { showHelp(QStringLiteral("shortcuts.md")); }); });
    connect(live, &QPushButton::clicked, &dlg, [&] {
        choose([this] {
            if (m_modes)
                m_modes->setCurrentWidget(m_live);
            showHelp(QStringLiteral("tutorials/14-performing-live.md"));
        });
    });
    connect(checkButton, &QPushButton::clicked, &dlg, [this] { showSystemCheck(); });

    // The check takes a moment (it loads the AI models): once the window is up.
    QTimer::singleShot(150, &dlg, [this, checkLabel] {
        QApplication::setOverrideCursor(Qt::WaitCursor);
        const std::vector<CheckItem> items = systemCheck();
        QApplication::restoreOverrideCursor();
        QStringList problems, notes;
        for (const CheckItem& c : items)
            if (c.state == CheckItem::Problem)
                problems << c.what;
            else if (c.state == CheckItem::Note)
                notes << c.what;
        if (problems.isEmpty() && notes.isEmpty())
            checkLabel->setText(tr("✓ This computer is ready for everything Mutant Video Producer does."));
        else if (problems.isEmpty())
            checkLabel->setText(tr("✓ Ready. Could be faster or more complete: %1.").arg(notes.join(QStringLiteral(", "))));
        else
            checkLabel->setText(tr("✗ Needs a look: %1. Open the System Check for how to fix it.").arg(problems.join(QStringLiteral(", "))));
    });

    dlg.resize(760, 560);
    dlg.exec();
    if (then)
        then();
}
