#pragma once

#include <QColor>
#include <QString>
#include <vector>

class QAction;
class QApplication;
class QIcon;
class QWidget;

// The look: View > Theme picks one of the themes below. Everything drawn by
// Mutant Video Producer takes its colours from theme::c() when it paints, the palette and
// the application style sheet are made from it, and widget style sheets set
// with theme::style() are made again when the theme changes, so a switch
// takes effect at once. (Footage, thumbnails and effect previews are never tinted.)
namespace theme {

struct Colors {
    const char* id;   // saved in the settings ("neon")
    const char* name; // in the menu ("Neon Grid")
    bool dark;
    // Window chrome
    QColor window, panel, base, raised, hover, checked, deep, line;
    QColor text, bright, dim, faint;
    QColor accent, onAccent;
    // Timeline
    QColor timelineBg, trackBg, trackBgAlt, trackSelected, headerBg, headerSelected;
    QColor ruler, markerBar, corner, timecode, field;
    QColor videoClip, audioClip, clipSelected, clipText, cursor, playhead, region, waveform;
    // States: done / on, careful, wrong / recording, information, cue / AI, keyframes
    QColor good, warn, bad, info, cue, key;
    QColor ink; // fine lines and washes over the theme's surfaces: white on dark themes, black on light
    QColor imageClip = QColor(0xb96c25); // image tracks remain orange in every theme
    QColor captionClip = QColor(0xb39b1e);  // captions and text events: gold in every theme
    QColor graphicsClip = QColor(0x7a4fd8); // graphics overlays and waveforms: violet in every theme
};

// Classic, Neon Grid, Night City, Synthwave Sunset, Terminal, Arctic, Studio Light.
const std::vector<Colors>& themes();
// The theme in use.
const Colors& c();

// Applies the saved theme (or `id`, then saved): palette, style sheet, widget style sheets.
void apply(QApplication& app, const QString& id = {});

// A widget style sheet with theme colours: "color: @dim; border: 1px solid @line;"
// (any Colors name after an @). It is made again whenever the theme changes.
void style(QWidget* w, const QString& sheet);
// The same, resolved once (for painting code and one-off strings).
QString css(const QString& sheet);

// Transport-bar icons in the theme's colours.
enum class Icon { Loop, PlayFromStart, Play, Pause, Stop, GoToStart, GoToEnd, PrevFrame, NextFrame };
QIcon transportIcon(Icon icon);
// Gives an action that icon, drawn again when the theme changes.
void setIcon(QAction* a, Icon icon);

} // namespace theme
