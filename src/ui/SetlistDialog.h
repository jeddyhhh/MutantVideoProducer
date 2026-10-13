#pragma once

#include "core/Project.h"

#include <QDialog>
#include <functional>
#include <vector>

class QTableWidget;

// Edits the Live tab's setlist: one row per song with its pad set, tempo
// (and whether it is locked), scene and notes; songs added, removed and
// moved up or down.
class SetlistDialog : public QDialog {
    Q_OBJECT
public:
    // `current` fills a new row from what is on now ("Add Current").
    SetlistDialog(const std::vector<LiveSong>& songs, const std::function<LiveSong()>& current, QWidget* parent = nullptr);

    std::vector<LiveSong> songs() const;
    QTableWidget* table() const { return m_table; }
    void addSong(const LiveSong& song);

private:
    void move(int delta);

    QTableWidget* m_table;
    std::function<LiveSong()> m_current;
};
