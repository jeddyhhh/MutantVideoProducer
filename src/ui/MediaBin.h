#pragma once

#include <QListWidget>
#include <functional>
#include <map>

class Project;
class MediaCache;

// "Project Media" window: imported files, draggable onto the timeline,
// sorted into folders, rated (keys 0-5) and marked in / out, and filtered
// with the bar from createFilterBar().
class MediaBin : public QListWidget {
    Q_OBJECT
public:
    static constexpr const char* kMimeType = "application/x-mvp-media";

    MediaBin(Project* project, MediaCache* cache, QWidget* parent = nullptr);

    QList<int> selectedMediaIds() const;
    // Search, folder and rating filters (MainWindow puts it above the list).
    QWidget* createFilterBar();
    void setFilter(const QString& search, const QString& folder, int minRating);
    void setOriginFilter(const QString& origin);
    void setSharedProfile(bool enabled);
    // The Mark In / Out dialog for a file (a scrubber; I and O set the marks).
    void markInOut(int mediaId);

signals:
    void importRequested(const QStringList& paths);
    void addToTimelineRequested(int mediaId);
    void sharedToTimelineRequested(const QString& path);
    void relinkRequested(int mediaId);
    // Right-click > Create Proxy / Delete Proxy (the selected files).
    void proxyRequested(const QList<int>& mediaIds);
    void proxyDeleteRequested(const QList<int>& mediaIds);

protected:
    QMimeData* mimeData(const QList<QListWidgetItem*>& items) const override;
    QStringList mimeTypes() const override;
    void dragEnterEvent(QDragEnterEvent* e) override;
    void dragMoveEvent(QDragMoveEvent* e) override;
    void dropEvent(QDropEvent* e) override;
    void contextMenuEvent(QContextMenuEvent* e) override;
    void keyPressEvent(QKeyEvent* e) override;
    void paintEvent(QPaintEvent* e) override; // (a hint while it is empty)

public:
    // Redraws the icons (a proxy was made or deleted: they carry a "P").
    void refreshIcons();

private:
    void rebuild();
    void applyFilter();
    QStringList folders() const;
    // Changes the folder / rating of the selected files.
    void setDetails(const std::function<void(QString& folder, int& rating)>& change);
    // The icon of a profile library file (not in this project): a thumbnail saved on disk, or made on a
    // pool thread (many in parallel) and saved for next time. Null while it is being made.
    QIcon libraryIcon(const QString& path);

    Project* m_project;
    MediaCache* m_cache;
    QString m_search, m_folder; // m_folder empty: all
    QString m_origin;
    bool m_sharedProfile = false;
    std::map<QString, QIcon> m_library;
    int m_minRating = 0;
    class QComboBox* m_folderBox = nullptr;
};
