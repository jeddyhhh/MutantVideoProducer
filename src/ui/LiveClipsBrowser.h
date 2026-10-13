#pragma once

#include <QFileSystemWatcher>
#include <QListWidget>
#include <QTimer>

// The Editor's "From Live" tab: every clip recorded on the Live tab (the
// Mutant Video Producer Live folder), newest first, with thumbnails. Drag clips onto the
// timeline, or double-click to add one at the cursor.
class LiveClipsBrowser : public QListWidget {
    Q_OBJECT
public:
    explicit LiveClipsBrowser(QWidget* parent = nullptr);

    // A file being recorded right now is left out until it is finished.
    void setRecordingPath(const QString& path);
    void refresh();

signals:
    void addToTimelineRequested(const QString& path);
    void addToMediaRequested(const QString& path);

protected:
    QMimeData* mimeData(const QList<QListWidgetItem*>& items) const override;
    QStringList mimeTypes() const override;
    void contextMenuEvent(QContextMenuEvent* e) override;
    void paintEvent(QPaintEvent* e) override;

private:
    void loadDetails(const QString& path); // duration, size and thumbnail in the background

    QFileSystemWatcher m_watcher;
    QTimer m_refreshTimer;
    QString m_recording;
};
