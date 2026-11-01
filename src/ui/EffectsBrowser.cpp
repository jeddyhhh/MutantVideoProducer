#include "ui/EffectsBrowser.h"
#include "core/RetroShaders.h"

#include "ui/Theme.h"

#include <QMessageBox>
#include <QMenu>
#include <QContextMenuEvent>
#include "core/Presets.h"

#include "core/AudioEffects.h"
#include "core/Effects.h"
#include "core/Generators.h"
#include "core/NodeGraph.h"

#include <QHeaderView>
#include <QMimeData>
#include <functional>
#include <map>

namespace {
constexpr int kIdRole = Qt::UserRole;
constexpr int kIsPresetRole = Qt::UserRole + 1;
constexpr int kUserRole = Qt::UserRole + 2;
constexpr int kNestedRole = Qt::UserRole + 3;

QTreeWidgetItem* presetItem(QTreeWidgetItem* parent, const Preset& p)
{
    auto* item = new QTreeWidgetItem(parent, {p.user ? p.name + QStringLiteral("  (user)") : p.name});
    item->setData(0, kIdRole, p.id);
    item->setData(0, kIsPresetRole, true);
    item->setData(0, kUserRole, p.user);
    item->setToolTip(0, p.description.isEmpty() ? QObject::tr("Preset: drag onto an event, track or device chain") : p.description);
    item->setForeground(0, theme::c().info);
    return item;
}
} // namespace

EffectsBrowser::EffectsBrowser(Kind kind, QWidget* parent) : QTreeWidget(parent), m_kind(kind)
{
    setHeaderHidden(true);
    setColumnCount(1);
    setDragEnabled(true);
    setDragDropMode(QAbstractItemView::DragOnly);
    setSelectionMode(QAbstractItemView::SingleSelection);
    setRootIsDecorated(kind != Kind::Transitions);
    populate();
    if (kind == Kind::Effects)
        connect(&nodes::Notifier::instance(), &nodes::Notifier::changed, this, &EffectsBrowser::populate);
    if (kind == Kind::Effects || kind == Kind::AudioEffects)
        connect(&PresetLibrary::instance(), &PresetLibrary::changed, this, &EffectsBrowser::populate);

    connect(this, &QTreeWidget::itemDoubleClicked, this, [this](QTreeWidgetItem* item) {
        const QString id = item->data(0, kIdRole).toString();
        if (id.isEmpty())
            return;
        if (item->data(0, kIsPresetRole).toBool())
            emit presetActivated(id);
        else
            emit activated(id);
    });
}

void EffectsBrowser::populate()
{
    clear();
    auto groupItem = [this](std::map<QString, QTreeWidgetItem*>& groups, const QString& name, QTreeWidgetItem* parent = nullptr) {
        QTreeWidgetItem*& group = groups[name];
        if (!group) {
            group = parent ? new QTreeWidgetItem(parent, {name}) : new QTreeWidgetItem(this, {name});
            group->setFlags(Qt::ItemIsEnabled);
            QFont f = group->font(0);
            f.setBold(true);
            group->setFont(0, f);
        }
        return group;
    };
    if (m_kind == Kind::AudioEffects || m_kind == Kind::Effects) {
        const bool audio = m_kind == Kind::AudioEffects;
        const PresetLibrary& lib = PresetLibrary::instance();
        // Preset chains first, grouped by category.
        const std::vector<Preset> chains = lib.chains(audio);
        if (!chains.empty()) {
            std::map<QString, QTreeWidgetItem*> top, sub;
            QTreeWidgetItem* root = groupItem(top, tr("Preset Chains"));
            root->setToolTip(0, tr("Ready-made chains of effects: drag onto an event, a track or the device chain"));
            for (const Preset& p : chains)
                presetItem(groupItem(sub, p.category.isEmpty() ? tr("Other") : p.category, root), p);
            root->setExpanded(false);
        }
        std::map<QString, QTreeWidgetItem*> groups;
        // "Parent/Child" categories become nested folders; sub-folders start closed.
        std::function<QTreeWidgetItem*(const QString&)> categoryItem = [&](const QString& path) -> QTreeWidgetItem* {
            const int slash = path.lastIndexOf(QLatin1Char('/'));
            if (slash < 0)
                return groupItem(groups, path);
            const bool fresh = !groups.count(path);
            QTreeWidgetItem* parent = categoryItem(path.left(slash));
            QTreeWidgetItem* item = groupItem(groups, path, parent);
            if (fresh) {
                item->setText(0, path.mid(slash + 1));
                item->setData(0, kNestedRole, true);
            }
            return item;
        };
        auto addEffect = [&](const QString& id, const QString& name, const QString& category, const QString& description) {
            auto* item = new QTreeWidgetItem(categoryItem(category), {name});
            item->setData(0, kIdRole, id);
            item->setToolTip(0, description);
            for (const Preset& p : lib.forEffect(id))
                presetItem(item, p);
            return item;
        };
        if (audio)
            for (const AudioEffectDef& e : audioEffects())
                addEffect(e.id, e.name, e.category, e.description);
        else
            for (const EffectDef& e : videoEffects())
                addEffect(e.id, e.name, e.category, e.description);
        if (!audio)
            for (const EffectDef* e : nodes::userEffects()) // (made in Tools > Effect Builder)
                addEffect(e->id, e->name, e->category, e->description);
        if (!audio)
            for (const EffectDef* e : retro::effects()) // (screen shaders: Tools > Get Screen Shaders)
                addEffect(e->id, e->name, e->category, e->description);
        for (auto& [name, g] : groups)
            g->setExpanded(!g->data(0, kNestedRole).toBool()); // categories open, sub-folders and presets folded
    } else if (m_kind == Kind::Generators) {
        std::map<QString, QTreeWidgetItem*> groups;
        for (const GeneratorPreset& p : generatorPresets()) {
            auto* item = new QTreeWidgetItem(groupItem(groups, p.category), {p.name});
            item->setData(0, kIdRole, p.id);
            if (const GeneratorDef* def = findGenerator(p.generator))
                item->setToolTip(0, def->description);
        }
        expandAll();
    } else {
        for (const TransitionDef& t : transitions()) {
            auto* item = new QTreeWidgetItem(this, {t.name});
            item->setData(0, kIdRole, t.id);
            item->setToolTip(0, t.description);
        }
    }
}

void EffectsBrowser::contextMenuEvent(QContextMenuEvent* e)
{
    QTreeWidgetItem* item = itemAt(e->pos());
    if (!item || !item->data(0, kIsPresetRole).toBool() || !item->data(0, kUserRole).toBool())
        return;
    const QString id = item->data(0, kIdRole).toString();
    QMenu menu;
    menu.addAction(tr("Delete Preset"), this, [this, id] {
        if (QMessageBox::question(this, tr("Delete Preset"), tr("Delete this preset?")) == QMessageBox::Yes)
            PresetLibrary::instance().remove(id);
    });
    menu.exec(e->globalPos());
}

static const char* mimeFor(EffectsBrowser::Kind kind)
{
    switch (kind) {
    case EffectsBrowser::Kind::Effects:
    case EffectsBrowser::Kind::AudioEffects: return EffectsBrowser::kEffectMime;
    case EffectsBrowser::Kind::Transitions: return EffectsBrowser::kTransitionMime;
    case EffectsBrowser::Kind::Generators: return EffectsBrowser::kGeneratorMime;
    }
    return EffectsBrowser::kEffectMime;
}

void EffectsBrowser::startDrag(Qt::DropActions)
{
    QTreeWidget::startDrag(Qt::CopyAction);
}

QStringList EffectsBrowser::mimeTypes() const
{
    return {QString::fromLatin1(mimeFor(m_kind)), QString::fromLatin1(kPresetMime)};
}

QMimeData* EffectsBrowser::mimeData(const QList<QTreeWidgetItem*>& items) const
{
    if (items.isEmpty())
        return nullptr;
    const QString id = items.front()->data(0, kIdRole).toString();
    if (id.isEmpty())
        return nullptr;
    auto* mime = new QMimeData;
    mime->setData(QString::fromLatin1(items.front()->data(0, kIsPresetRole).toBool() ? kPresetMime : mimeFor(m_kind)), id.toUtf8());
    return mime;
}
