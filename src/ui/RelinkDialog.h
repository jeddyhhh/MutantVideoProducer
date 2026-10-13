#pragma once

#include <QDialog>
#include <functional>
#include <set>

class Project;
class QLabel;
class QTreeWidget;

// "Missing Media": finds files the project uses that were moved, renamed or
// are on a drive that isn't connected. Locate one file and the others are
// looked for next to it (and where their old folder seems to have moved);
// or search a whole folder for all of them by name. Events come back online
// as soon as their file is found.
class RelinkDialog : public QDialog {
    Q_OBJECT
public:
    explicit RelinkDialog(Project* project, int focusMediaId = 0, QWidget* parent = nullptr);

    // Points media at `path`, then looks for the other missing files near it.
    // Returns how many items were relinked (0 with an error on failure).
    int relink(int mediaId, const QString& path, QString* error = nullptr);
    // Searches `folder` and its subfolders for the missing files by name.
    // Returns how many were relinked.
    int searchFolder(const QString& folder);
    int missingCount() const;

private:
    void refresh();
    void locateSelected();
    void searchDialog();
    // Relinks missing items to files found by `lookup` (file name -> path).
    int relinkFound(const std::function<QString(const QString& oldPath)>& lookup);

    Project* m_project;
    QTreeWidget* m_list;
    QLabel* m_summary;
    std::set<int> m_ids; // the items that were missing when the dialog opened
};
