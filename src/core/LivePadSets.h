#pragma once

// Saved sets of Live tab pads: whole 4x4 grids (effect chains, names, locks)
// that can be loaded into any project and switched between while performing.
// Each set is a JSON file in the app data folder ("pad-sets"; MVP_PADSETS_DIR
// overrides it for tests).

#include "core/Project.h"

#include <QString>
#include <QStringList>
#include <optional>

struct LivePadSet {
    QString name;
    LivePads pads;
};

namespace padsets {

QString directory();
QStringList names(); // sorted, case-insensitively
std::optional<LivePadSet> load(const QString& name);
// Saves (or overwrites, by name) a set.
bool save(const LivePadSet& set, QString* error = nullptr);
bool remove(const QString& name);

} // namespace padsets
