#pragma once
#include "core/Exporter.h"
#include <QDialog>
#include <QElapsedTimer>
#include <QJsonObject>
#include <atomic>
#include <memory>
#include <mutex>
class QLabel;
class QProgressBar;
class QPlainTextEdit;
class QPushButton;
struct RenderMonitor {
    mutable std::mutex mutex;
    ExportStatus status;
    QStringList activity;
    std::atomic<bool> cancel{false}, done{false};
    void update(const ExportStatus& value);
    ExportStatus snapshot() const;
    QJsonObject json() const;
};
class RenderStatusDialog : public QDialog {
public:
    RenderStatusDialog(const ExportSettings& settings, const ProjectSnapshot& project,
                       std::shared_ptr<RenderMonitor> monitor, QWidget* parent = nullptr);
    void reject() override;
private:
    void refresh();
    ExportSettings m_settings;
    std::shared_ptr<RenderMonitor> m_monitor;
    QLabel *m_stage, *m_summary;
    QProgressBar* m_progress;
    QPlainTextEdit *m_metrics, *m_log, *m_active;
    QPushButton *m_cancel, *m_close, *m_folder;
    int m_logLines = 0;
    QElapsedTimer m_clock;
};
