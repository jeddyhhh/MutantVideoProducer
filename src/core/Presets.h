#pragma once

#include "core/Effects.h"

#include <QObject>
#include <functional>
#include <optional>
#include <vector>

// Effect presets: settings for one device (shown under that effect in the
// FX browsers and in the device's preset menu) or whole chains of devices,
// racks included (shown under "Preset Chains"). Factory presets are built
// in; user presets are JSON files in presetDirectory().
struct Preset {
    QString id;          // "f:<slug>" factory, "u:<file name>" user
    QString name;
    QString category;    // chain presets: browser group ("Voice", "Space"...)
    QString description;
    bool audio = true;
    bool user = false;
    QString effectType;  // single-device presets: the device's effect type; empty for chains
    EffectChain chain;   // ids are placeholders: use instantiatePreset()

    bool isChain() const { return effectType.isEmpty(); }
};

const std::vector<Preset>& factoryPresets();

// The preset's devices with fresh ids, ready to insert.
EffectChain instantiatePreset(const Preset& preset, const std::function<int()>& allocId);
// Single-device preset applied to an existing device: its parameters (and
// rack chains) replace the device's; the id and on/off state stay.
bool applyPresetTo(const Preset& preset, EffectInstance& device, const std::function<int()>& allocId);

// Factory + user presets; emits changed() when user presets are saved or removed.
class PresetLibrary : public QObject {
    Q_OBJECT
public:
    static PresetLibrary& instance();

    QString directory() const { return m_dir; }
    void setDirectory(const QString& dir); // (tests) default: the app data folder

    std::vector<Preset> all(bool audio) const;
    std::vector<Preset> forEffect(const QString& effectType) const;
    std::vector<Preset> chains(bool audio) const;
    std::optional<Preset> find(const QString& id) const;
    std::optional<Preset> findUserByName(const QString& name, bool audio) const;

    // Saves (or overwrites, by name) a user preset; returns its id or empty on error.
    QString save(const QString& name, bool audio, const QString& effectType, const EffectChain& chain, QString* error = nullptr);
    bool remove(const QString& id);
    void reload();

signals:
    void changed();

private:
    PresetLibrary();
    QString m_dir;
    std::vector<Preset> m_user;
};
