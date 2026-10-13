#pragma once

#include <QString>

class MainWindow;

// --monkey-test: random menu commands, keys, clicks, drags and wheel turns
// on the editor (off-screen), with the project checked as it goes. Dialogs
// it opens are answered at random or closed. Each step is written to `logPath`
// as it happens, so after a crash the last line says what did it. Exits the
// application when done (the exit code is the number of failures). Call it
// before anything can open a dialog: from then on none shows on the screen.
// liveTab: the Live tab instead of the editor (with MVP_NO_DEVICES: video
// files and a test beat, never a camera, microphone, screen or window).
void startMonkeyTest(MainWindow& window, const QString& logPath, int steps, unsigned seed, bool liveTab = false);
