#pragma once

#include "core/Effects.h"

#include <QWidget>
#include <functional>

// A device's picture of its settings, beside its controls in the device chain: an EQ's or a filter's curve, a compressor's,
// gate's or limiter's transfer curve, a delay's echoes, a reverb's tail, a chorus's sweep, the stereo field, a colour effect's
// curves or hue ring, an effect's centre on the picture. Most are edited by dragging on them (the handles), as the knobs are.
class ParamDisplay : public QWidget {
public:
    using Get = std::function<double(const QString& id)>;          // a setting's value now (at the cursor)
    using Set = std::function<void(const QString& id, double v)>; // a change made on the picture

    // The picture for an effect type with these settings; null when it has none.
    static ParamDisplay* create(const QString& effectType, const std::vector<ParamDef>& defs, Get get, Set set);

protected:
    ParamDisplay(const std::vector<ParamDef>& defs, Get get, Set set);
    const ParamDef* def(const QString& id) const;
    double value(const QString& id) const { return m_get(id); }
    void setValue(const QString& id, double v) const; // clamped to the setting's range, rounded to its steps
    Params params() const;                       // every number now
    void paintFrame(class QPainter& p, const QRectF& r) const; // the plot's background and border
    QRectF plot() const { return QRectF(rect()).adjusted(3, 3, -3, -3); }

    std::vector<ParamDef> m_defs;

private:
    Get m_get;
    Set m_set;
};
