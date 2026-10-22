#pragma once
#include "core/Project.h"
#include "core/Voice.h"
#include <QJsonObject>
#include <QObject>
#include <QProcess>
#include <atomic>
#include <thread>

// The GUI and MCP use the same asynchronous worker and import path.
class VoiceJob : public QObject {
    Q_OBJECT
public:
    explicit VoiceJob(Project* project, QObject* parent = nullptr);
    ~VoiceJob() override;
    static QString workerPath();
    bool start(const std::vector<Caption>& captions, const LocalVoice& voice, const QJsonObject& options, bool preview, QString* error);
    bool busy() const { return m_busy; }
    QJsonObject status() const;
    void cancel();
    const QString& outputFolder() const { return m_folder; }
signals:
    void progress(double value, const QString& message);
    void completed(bool ok, const QString& error);
private:
    void finish(const QString& error = {});
    void readOutput();
    Project* m_project;
    QProcess m_process;
    QByteArray m_pending;
    Timeline m_before;
    ProjectSettings m_settings;
    QString m_projectFile, m_folder, m_error, m_message, m_voiceName;
    bool m_busy = false, m_done = false, m_ok = false, m_cancelled = false, m_preview = false;
    double m_progress = 0;
    int m_trackId = 0, m_count = 0, m_warnings = 0;
    QJsonObject m_options;
    QJsonObject m_backends;
    QString m_voiceId;
    std::vector<int> m_createdIds;
    std::vector<Caption> m_wordCaptions;
    std::thread m_alignment;
    std::atomic<bool> m_alignmentCancel{false};
    bool m_alignmentRunning = false, m_alignmentReady = false;
    void* m_processTree = nullptr; // Windows job object: cancellation also stops RVC children.
};

// A script spoken by several characters: one VoiceJob per character, then every line placed in script order on the
// characters' own tracks (named after them, with their pitch), as one undo step.
struct DialogueVoice {
    QString name;
    LocalVoice voice;
    double rate = 1, semitones = 0;
};
class DialogueJob : public QObject {
    Q_OBJECT
public:
    DialogueJob(Project* project, VoiceJob* voice, QObject* parent = nullptr);
    // alignmentModel: a Whisper model; each line's words are then timed (exact mouth shapes).
    bool start(const QString& script, const std::vector<DialogueVoice>& cast, TimeUs start, TimeUs gap, QString* error,
               const QString& alignmentModel = {});
    bool busy() const { return m_busy; }
    QJsonObject status() const;
    void cancel();
signals:
    void progress(double value, const QString& message);
    void completed(bool ok, const QString& error);
private:
    void next();
    void finish(const QString& error);
    Project* m_project;
    VoiceJob* m_voice;
    std::vector<DialogueLine> m_lines;
    std::vector<DialogueVoice> m_cast;
    std::vector<DialogueCast> m_tracks;
    TimeUs m_start = 0, m_gap = 0;
    QString m_alignmentModel;
    int m_undoIndex = 0;
    size_t m_index = 0;
    bool m_busy = false, m_done = false, m_ok = false, m_cancelled = false;
    double m_progress = 0;
    QString m_error, m_message;
};
