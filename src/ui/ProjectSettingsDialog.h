#pragma once

#include "core/Project.h"

#include <QDialog>

class QComboBox;
class QSpinBox;

// File > Properties: output frame size, frame rate and audio sample rate.
class ProjectSettingsDialog : public QDialog {
    Q_OBJECT
public:
    ProjectSettingsDialog(const ProjectSettings& current, const MediaMap& media, QWidget* parent = nullptr);
    ProjectSettings settings() const;

private:
    void load(const ProjectSettings& s);

    ProjectSettings m_base;
    QSpinBox* m_width;
    QSpinBox* m_height;
    QComboBox* m_fps;
    QComboBox* m_sampleRate;
};

// Project settings matching a video stream (frame rate snapped to standard rates).
ProjectSettings settingsForMedia(const VideoStreamInfo& v, const ProjectSettings& base);
