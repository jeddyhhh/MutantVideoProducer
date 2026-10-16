#include "ui/SequenceEditor.h"
#include "core/Project.h"
#include "core/Story.h"
#include "core/TimelineOps.h"
#include "core/MediaInfo.h"
#include "core/Effects.h"
#include "engine/MediaCache.h"
#include "engine/PlaybackEngine.h"
#include "ui/TimelineWidget.h"
#include "ui/PreviewWidget.h"
#include "ui/DeviceChainView.h"
#include "ui/EffectEditor.h"
#include <QAction>
#include <QCheckBox>
#include <QComboBox>
#include <QDialog>
#include <QDialogButtonBox>
#include <QFileDialog>
#include <QHBoxLayout>
#include <QMessageBox>
#include <QPushButton>
#include <QSplitter>
#include <QTabWidget>
#include <QVBoxLayout>

bool editEmbeddedSequence(Project* project, int clipId, QWidget* parent)
{
    const auto* found = project->timeline().findClip(clipId);
    if (!found || !found->isSequence())
        return false;
    const Clip container = *found;
    const Timeline before = project->timeline();
    Project child;
    child.setSettings(project->settings());
    child.shareMediaPool(project->media());
    child.setTimelineLive(*container.sequence.value);
    MediaCache cache;
    PlaybackEngine engine;
    engine.setPreviewDivisor(2);
    engine.setSnapshot(child.snapshot());
    QDialog dialog(parent);
    dialog.setObjectName("sequenceEditor");
    dialog.setWindowTitle(QStringLiteral("Sequence: %1").arg(container.name));
    dialog.resize(1280, 900);
    auto* layout = new QVBoxLayout(&dialog);
    auto* toolbar = new QHBoxLayout;
    layout->addLayout(toolbar);
    auto* play = new QPushButton(QObject::tr("Play / Stop"));
    toolbar->addWidget(play);
    auto* media = new QComboBox;
    for (const auto& [id, m] : child.media())
        media->addItem(m->displayName(), id);
    toolbar->addWidget(media, 1);
    auto* insert = new QPushButton(QObject::tr("Insert media"));
    toolbar->addWidget(insert);
    auto* import = new QPushButton(QObject::tr("Import file..."));
    toolbar->addWidget(import);
    auto* split = new QSplitter(Qt::Vertical);
    layout->addWidget(split, 1);
    auto* preview = new PreviewWidget;
    split->addWidget(preview);
    auto* timeline = new TimelineWidget(&child, &cache);
    timeline->setObjectName("sequenceTimeline");
    split->addWidget(timeline);
    auto* tabs = new QTabWidget;
    split->addWidget(tabs);
    auto* devices = new DeviceChainView(&child, [&] { return timeline->cursorTime(); });
    devices->setPreview(preview);
    tabs->addTab(devices, QObject::tr("Effects"));
    auto* properties = new EffectEditor(&child, [&] { return timeline->cursorTime(); });
    properties->setEmbedded(true);
    properties->setPreview(preview);
    tabs->addTab(properties, QObject::tr("Properties"));
    split->setSizes({350, 330, 150});
    QObject::connect(&engine, &PlaybackEngine::frameReady, &dialog, [=](const QImage& image, TimeUs) { preview->setImage(image); });
    QObject::connect(&engine, &PlaybackEngine::positionChanged, &dialog, [=](TimeUs t) { timeline->setPlayhead(t); });
    QObject::connect(&child, &Project::changed, &dialog, [&] {
        engine.setSnapshot(child.snapshot());
        engine.requestFrame(timeline->cursorTime());
    });
    QObject::connect(timeline, &TimelineWidget::cursorMoved, &dialog, [&](TimeUs t) {
        if (engine.isPlaying()) {
            engine.stop();
            engine.play(t);
        } else
            engine.requestFrame(t);
        devices->onCursorMoved();
        properties->onCursorMoved();
    });
    QObject::connect(play, &QPushButton::clicked, &dialog, [&] {
        if (engine.isPlaying()) {
            engine.stop();
            timeline->setPlayhead({});
        } else
            engine.play(timeline->cursorTime());
    });
    QObject::connect(insert, &QPushButton::clicked, &dialog, [&] { timeline->insertMediaAtCursor(media->currentData().toInt()); });
    QObject::connect(import, &QPushButton::clicked, &dialog, [&] {
        const auto files = QFileDialog::getOpenFileNames(&dialog, QObject::tr("Import into Sequence"));
        for (const auto& path : files) {
            auto info = probeMedia(path);
            if (info.ok()) {
                int id = child.addMedia(info);
                media->addItem(child.mediaItem(id)->displayName(), id);
            }
        }
    });
    QObject::connect(timeline, &TimelineWidget::clipEffectsRequested, &dialog, [=](int id) {
        devices->setTarget({FxTarget::Kind::Clip, id});
        tabs->setCurrentWidget(devices);
    });
    QObject::connect(timeline, &TimelineWidget::trackEffectsRequested, &dialog, [=](int id) {
        devices->setTarget({FxTarget::Kind::Track, id});
        tabs->setCurrentWidget(devices);
    });
    QObject::connect(timeline, &TimelineWidget::generatorRequested, &dialog, [=](int id) {
        properties->setTarget({FxTarget::Kind::Generator, id});
        tabs->setCurrentWidget(properties);
    });
    QObject::connect(timeline, &TimelineWidget::panCropRequested, &dialog, [=](int id) {
        properties->setTarget({FxTarget::Kind::PanCrop, id});
        tabs->setCurrentWidget(properties);
    });
    QObject::connect(timeline, &TimelineWidget::transitionRequested, &dialog, [=](int id, bool in) {
        properties->setTarget({in ? FxTarget::Kind::TransitionIn : FxTarget::Kind::TransitionOut, id});
        tabs->setCurrentWidget(properties);
    });
    auto* effects = new QComboBox;
    for (const auto& e : videoEffects())
        effects->addItem(e.name, e.id);
    toolbar->addWidget(effects);
    auto* addFx = new QPushButton(QObject::tr("Add FX"));
    toolbar->addWidget(addFx);
    QObject::connect(addFx, &QPushButton::clicked, &dialog, [=] { timeline->addEffectToSelection(effects->currentData().toString()); });
    QObject::connect(timeline, &TimelineWidget::sequenceRequested, &dialog, [&](int id) {
        engine.stop();
        editEmbeddedSequence(&child, id, &dialog);
    });
    auto* nested = new QPushButton(QObject::tr("Open nested"));
    toolbar->addWidget(nested);
    QObject::connect(nested, &QPushButton::clicked, &dialog, [&] {
        engine.stop();
        editEmbeddedSequence(&child, timeline->firstSelectedClip(), &dialog);
    });
    auto command = [&](const QString& name, const QString& shortcut, std::function<void()> action) {
        auto* a = new QAction(name, &dialog);
        a->setShortcut(QKeySequence(shortcut));
        a->setShortcutContext(Qt::WidgetWithChildrenShortcut);
        dialog.addAction(a);
        QObject::connect(a, &QAction::triggered, &dialog, std::move(action));
    };
    command("Split", "S", [=] { timeline->splitAtCursor(); });
    command("Delete", "Delete", [=] { timeline->deleteSelection(false); });
    command("Copy", "Ctrl+C", [=] { timeline->copySelection(); });
    command("Paste", "Ctrl+V", [=] { timeline->paste(); });
    command("Undo", "Ctrl+Z", [&] { child.undoStack()->undo(); });
    command("Redo", "Ctrl+Y", [&] { child.undoStack()->redo(); });
    command("Select All", "Ctrl+A", [=] { timeline->selectAll(); });
    auto* ripple = new QCheckBox(QObject::tr("Move following material when the sequence duration changes"));
    ripple->setChecked(true);
    layout->addWidget(ripple);
    auto* buttons = new QDialogButtonBox(QDialogButtonBox::Save | QDialogButtonBox::Cancel);
    layout->addWidget(buttons);
    QObject::connect(buttons, &QDialogButtonBox::accepted, &dialog, &QDialog::accept);
    QObject::connect(buttons, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);
    engine.requestFrame(0);
    const bool accepted = dialog.exec() == QDialog::Accepted;
    engine.stop();
    if (!accepted)
        return false;
    if (project->timeline() != before) {
        QMessageBox::information(
            parent, QObject::tr("Sequence"),
            QObject::tr("The parent project changed while this sequence was open. Reopen the sequence to apply edits safely."));
        return false;
    }
    devices->setTarget({});
    properties->setTarget({});
    Timeline content = child.timeline();
    if (content.duration() <= 0) {
        QMessageBox::information(parent, QObject::tr("Sequence"), QObject::tr("A sequence must contain at least one clip."));
        return false;
    }
    // Import new files, remapping their media ids if the parent's pool has holes.
    std::map<int, int> mapping;
    for (const auto& [id, item] : child.media())
        mapping[id] = project->addMedia(item->info);
    std::function<void(Timeline&)> remap = [&](Timeline& t) {
        for (auto& track : t.tracks)
            for (auto& c : track.clips) {
                if (c.mediaId)
                    c.mediaId = mapping[c.mediaId];
                auto takes = c.narration["takes"].toArray();
                for (int i = 0; i < takes.size(); ++i) {
                    auto take = takes[i].toObject();
                    take["media"] = mapping[take["media"].toInt()];
                    takes[i] = take;
                }
                if (!takes.empty())
                    c.narration["takes"] = takes;
                if (c.isSequence()) {
                    Timeline inner = *c.sequence.value;
                    remap(inner);
                    c.sequence.value = std::make_shared<const Timeline>(std::move(inner));
                }
            }
    };
    remap(content);
    Timeline tl = before;
    QString error;
    if (!story::replaceCompound(tl, clipId, content, ripple->isChecked(), &error)) {
        QMessageBox::information(parent, QObject::tr("Sequence"), error);
        return false;
    }
    project->applyEdit(tl, QObject::tr("Edit Compound Sequence"));
    return true;
}
