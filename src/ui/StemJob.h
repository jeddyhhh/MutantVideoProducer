#pragma once

// Separate Stems: runs the Stem Separation engine (Demucs, an AI component) on an audio event in the background, then
// puts the stems on tracks under it (core/StemSeparation.h) as one undo step.

#include "core/Project.h"

#include <QJsonArray>
#include <QJsonObject>
#include <QObject>
#include <QProcess>

class StemJob : public QObject {
    Q_OBJECT
public:
    explicit StemJob(Project* project, QObject* parent = nullptr);
    ~StemJob() override;
    // stems: 2 (voice and background) or 4 (vocals, drums, bass, other); best: the fine-tuned model (about 4x slower).
    bool start(int clipId, int stems, bool best, QString* error);
    bool busy() const { return m_busy; }
    void cancel();
    // For AI control: busy, progress, message, done, ok, error, the new events.
    QJsonObject status() const;
signals:
    void progress(double value, const QString& message);
    void completed(bool ok, const QString& error);

private:
    void readOutput();
    void finish(const QString& error);
    Project* m_project;
    QProcess m_process;
    QByteArray m_pending;
    QString m_folder, m_input, m_message, m_error;
    QJsonObject m_stems;
    QJsonArray m_made;
    int m_clipId = 0, m_count = 2;
    double m_progress = 0;
    bool m_busy = false, m_done = false, m_ok = false, m_cancelled = false;
};
