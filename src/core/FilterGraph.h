#pragma once

#include "core/Effects.h"

#include <QImage>
#include <QString>

struct AVFilterGraph;
struct AVFilterContext;

// Runs an FFmpeg filter graph description (e.g. "hqdn3d=4:3:6:4,eq=gamma=1.2")
// over BGRA frames. Temporal filters see a continuous stream while playback
// is sequential; the graph is rebuilt after jumps.
class FilterGraphState : public EffectState {
public:
    ~FilterGraphState() override;

    // Returns the filtered image (same size as `in`), or `in` unchanged when
    // the filter produced no output yet. `error` is set if the graph is invalid.
    QImage process(const QImage& in, const QString& spec, TimeUs t, double fps, QString* error);

private:
    bool build(QSize size, const QString& spec, double fps, QString* error);
    void reset();

    AVFilterGraph* m_graph = nullptr;
    AVFilterContext* m_src = nullptr;
    AVFilterContext* m_sink = nullptr;
    QString m_spec;
    QSize m_size;
    TimeUs m_lastT = -1;
    QString m_error;
};
