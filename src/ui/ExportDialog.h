#pragma once

#include "core/Exporter.h"

#include <QDialog>

class QCheckBox;
class QDoubleSpinBox;
class QLabel;
class QComboBox;
class QGroupBox;
class QLineEdit;
class QRadioButton;
class QSpinBox;

// "Render As" dialog: pick a template or any muxer/encoder combination the
// FFmpeg build supports.
class ExportDialog : public QDialog {
    Q_OBJECT
public:
    ExportDialog(const ProjectSettings& project, TimeUs projectDuration, bool hasRegion, TimeUs regionStart,
                 TimeUs regionEnd, const QString& suggestedPath, QWidget* parent = nullptr);

    ~ExportDialog() override;

    ExportSettings settings() const;
    // The project's captions: offered as a subtitle track (MP4, MOV, MKV, WebM).
    void setCaptions(const std::vector<Caption>& captions);

private:
    void applyPreset(int index);
    void fillTemplates(); // grouped by category, filtered by the search text
    void updateNotes();
    TimeUs renderLength() const;
    void onMuxerChanged();
    void fixExtension(const QString& ext);
    void browse();
    void validateAndAccept();

    ProjectSettings m_project;
    TimeUs m_duration;
    TimeUs m_regionStart, m_regionEnd;

    QComboBox* m_preset;
    QLineEdit* m_search;
    QLabel* m_notes;
    QCheckBox* m_useGpu;
    QCheckBox* m_subtitles;
    std::vector<Caption> m_captions;
    QComboBox* m_pixFmt;
    QComboBox* m_framing;
    QDoubleSpinBox* m_targetSize;
    QLineEdit* m_muxerOptions;
    double m_gopSeconds = 0;
    int m_presetIndex = -1;
    QLineEdit* m_path;
    QComboBox* m_muxer;
    QGroupBox* m_videoBox;
    QComboBox* m_videoCodec;
    QSpinBox* m_width;
    QSpinBox* m_height;
    QComboBox* m_fps;
    QSpinBox* m_quality;
    QSpinBox* m_videoBitrate;
    QLineEdit* m_encoderPreset;
    QLineEdit* m_videoOptions;
    QGroupBox* m_audioBox;
    QComboBox* m_audioCodec;
    QComboBox* m_sampleRate;
    QComboBox* m_channels;
    QSpinBox* m_audioBitrate;
    QRadioButton* m_rangeAll;
    QRadioButton* m_rangeRegion;
};

// Parses "29.97", "30000/1001", "25" etc.
FrameRate parseFrameRate(const QString& text, FrameRate fallback);
QString frameRateText(const FrameRate& fps);
