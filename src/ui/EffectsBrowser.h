#pragma once

#include <QSet>
#include <QTreeWidget>

// Lists video FX (grouped by category), audio FX, transitions or generators;
// items can be dragged onto timeline events/tracks or the device chain, or
// double-clicked to apply to the selection. The FX lists also show presets:
// "Preset Chains" at the top and each effect's presets under it.
class EffectsBrowser : public QTreeWidget {
    Q_OBJECT
public:
    enum class Kind { Effects, Transitions, Generators, AudioEffects };
    static constexpr const char* kEffectMime = "application/x-mvp-effect";
    static constexpr const char* kTransitionMime = "application/x-mvp-transition";
    static constexpr const char* kGeneratorMime = "application/x-mvp-generator";
    static constexpr const char* kPresetMime = "application/x-mvp-preset";

    explicit EffectsBrowser(Kind kind, QWidget* parent = nullptr);

signals:
    void activated(const QString& id);
    void presetActivated(const QString& presetId);

protected:
    QMimeData* mimeData(const QList<QTreeWidgetItem*>& items) const override;
    QStringList mimeTypes() const override;
    void contextMenuEvent(QContextMenuEvent* e) override;
    // Items are only ever copied out: a drop target accepting a "move" must not remove them.
    void startDrag(Qt::DropActions supportedActions) override;

private:
    void populate();
    Kind m_kind;
    QSet<QString> m_open; // (the folders open before the list was made again)
};
