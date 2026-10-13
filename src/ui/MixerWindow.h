#pragma once

#include <QWidget>
#include <map>
#include <vector>

class Project;
class QHBoxLayout;
class ChannelStrip;
class LevelMeter;

// Audio Mixer (View > Audio Mixer): a channel strip per audio track with a
// stereo meter, volume fader, pan, mute / solo, its FX and auto ducking,
// and the master output's meter. Edits go into the project (undoable) and
// show on the timeline's track headers too.
class MixerWindow : public QWidget {
    Q_OBJECT
public:
    explicit MixerWindow(Project* project, QWidget* parent = nullptr);

    // Levels heard since the last call (linear peaks), by track id; master L/R.
    void setPeaks(const std::map<int, std::pair<float, float>>& tracks, std::pair<float, float> master);
    int stripCount() const { return int(m_strips.size()); }
    ChannelStrip* strip(int i) const { return m_strips[size_t(i)]; }

signals:
    void trackEffectsRequested(int trackId);
    void masterEffectsRequested();

private:
    void rebuild();
    void refresh();
    QString structureKey() const;

    Project* m_project;
    QHBoxLayout* m_row;
    std::vector<ChannelStrip*> m_strips;
    LevelMeter* m_master;
    QString m_key;
};
