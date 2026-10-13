#pragma once

// Context help: the pages of the user guide (docs/, see MainWindow::showHelp)
// that panels and dialogs open with their Help button or Shift+F1.

#include <QString>

class QDialogButtonBox;
class QWidget;

namespace help {

// Opens a page of the guide, e.g. "effects.md#masks".
void open(const QString& page);
// The page Shift+F1 opens while `w` (or something inside it) has the focus.
void setPage(QWidget* w, const QString& page);
// The page of the focused widget: the nearest one set on it or a parent (else the contents).
QString pageFor(const QWidget* w);
// A Help button on a dialog's buttons, and Shift+F1 in the dialog, for its page.
void addButton(QDialogButtonBox* box, const QString& page);

} // namespace help
