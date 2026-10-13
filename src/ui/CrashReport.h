#pragma once

// When Mutant Video Producer crashes, a report is written to the crashes folder: when and
// where it happened (the module and offset, the exception), the version, the
// last lines Mutant Video Producer logged, and on Windows a minidump for a debugger. The
// next start offers it (see MainWindow::startup), to report on GitHub.

#include <QString>
#include <QStringList>

namespace crash {

// Early in main(), once the application's name is set: the handlers and the log hook.
void install();
// A report on a thread that stopped answering (where it is stuck), as for a
// crash; returns its text. Windows only (elsewhere empty).
QString reportHang(unsigned long threadId);
// Where reports go (the app data folder's "crashes").
QString folder();
// Reports (.txt) written since the last markSeen(), newest first.
QStringList newReports();
void markSeen();
// The report's first lines, for a GitHub issue ("Mutant Video Producer 0.1.5 crashed: access violation in ...").
QString summary(const QString& report);

} // namespace crash
