#pragma once
#include <QJsonObject>
#include <QString>
#include "themehelper.h"

class QDialog;
class QWidget;

// Geometry for the modal tool-confirmation dialog.
//
// It used to open at 480x320, which was too small to read: a run_bash command
// or a JSON argument body wrapped into a few cramped lines with the buttons
// crowding the bottom edge. Width is +75% (480 -> 840) and height is +50%
// (320 -> 480) of the original.
inline constexpr int kToolConfirmDialogWidth  = 840;  // 480 * 1.75
inline constexpr int kToolConfirmDialogHeight = 480;  // 320 * 1.50

// Label of the "approve every later tool in this chat tab" button. Shared with
// the regression test so the promise the label makes and the behaviour it
// triggers cannot drift apart.
inline constexpr const char* kToolConfirmSessionApprovalText =
    "Execute & Don't Ask Again\nThis Session";

// What the user chose in the confirmation dialog.
struct ToolConfirmResult {
    // Run the tool that was asked about.
    bool confirmed = false;
    // Also run every later tool in this chat tab until it is closed.
    bool approveSession = false;
};

// Builds the modal tool-confirmation dialog without showing it.
//
// Split out of MainWindow::handleToolConfirm so the dialog's geometry, labels
// and button behaviour can be tested without a running agent, a worker, or a
// modal exec(). The caller owns the returned dialog and is expected to exec()
// and then delete it.
QDialog* buildToolConfirmDialog(QWidget* parent, const QString& toolName,
                                const QJsonObject& args, const Theme& theme,
                                int uiScale, ToolConfirmResult* result);
