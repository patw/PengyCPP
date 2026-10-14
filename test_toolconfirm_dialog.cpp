// Regression tests for the tool-confirmation dialog.
// Mirrors PengyR's gui/tests/test_toolconfirm_dialog.cpp.
//
// Two reported problems live here:
//   1. The dialog opened at 480x320 and was too small -- real tool arguments
//      were unreadable. It is now 840x480 (+75% wide, +50% tall).
//   2. "Don't ask again this session" did not stick: the approval flag was
//      cleared on every model round, so approving one bash call and letting the
//      model call bash again re-prompted. The flag now belongs to the chat tab.
#include <QApplication>
#include <QDialog>
#include <QJsonObject>
#include <QPlainTextEdit>
#include <QPushButton>
#include <iostream>

#include "toolconfirmdialog.h"

static void check(bool ok, const char* label) {
    if (!ok) {
        std::cerr << "FAIL: " << label << std::endl;
        std::exit(1);
    }
}

static QPushButton* buttonWithText(QDialog* dlg, const QString& text) {
    for (QPushButton* b : dlg->findChildren<QPushButton*>()) {
        if (b->text() == text) return b;
    }
    return nullptr;
}

int main(int argc, char** argv) {
    // No window is shown; offscreen keeps the test headless and avoids
    // stealing focus from a real desktop session.
    if (qEnvironmentVariableIsEmpty("QT_QPA_PLATFORM"))
        qputenv("QT_QPA_PLATFORM", "offscreen");
    QApplication app(argc, argv);

    const Theme theme = makeTheme("dark", "default");

    // The requested size: 75% wider and 50% taller than the original.
    check(kToolConfirmDialogWidth == 480 * 7 / 4, "width is 175% of the old 480px");
    check(kToolConfirmDialogHeight == 320 * 3 / 2, "height is 150% of the old 320px");

    // Geometry and argument rendering.
    {
        ToolConfirmResult result;
        QJsonObject args{{"command", "ls -la /Users/patw"}};
        QDialog* dlg = buildToolConfirmDialog(nullptr, "run_bash", args, theme, 100, &result);
        check(dlg != nullptr, "dialog built");

        check(dlg->width() == kToolConfirmDialogWidth, "dialog opens at the new width");
        check(dlg->height() == kToolConfirmDialogHeight, "dialog opens at the new height");
        // A maximum-width cap narrower than the dialog silently undoes resize().
        check(dlg->maximumWidth() >= kToolConfirmDialogWidth, "no maximum width cap");
        check(dlg->windowTitle() == "Confirm Tool: run_bash", "title names the tool");

        QPlainTextEdit* argsEdit = dlg->findChild<QPlainTextEdit*>();
        check(argsEdit != nullptr, "arguments pane present");
        check(argsEdit->isReadOnly(), "arguments pane is read-only");
        check(argsEdit->toPlainText().contains("ls -la /Users/patw"), "arguments are shown");

        QPushButton* exec = buttonWithText(dlg, "Execute");
        check(exec != nullptr, "Execute button present");
        exec->click();
        check(result.confirmed, "Execute confirms this tool");
        check(!result.approveSession, "Execute does not approve the session");
        delete dlg;
    }

    // The session approval button: label, tooltip, and what it reports back.
    {
        ToolConfirmResult result;
        QDialog* dlg = buildToolConfirmDialog(nullptr, "run_bash", QJsonObject{}, theme, 100, &result);
        QPushButton* approve = buttonWithText(dlg, QString::fromUtf8(kToolConfirmSessionApprovalText));
        check(approve != nullptr, "session approval button present");
        check(approve->toolTip().contains("chat tab"), "tooltip names the scope");
        approve->click();
        check(result.confirmed, "session approval runs this tool");
        check(result.approveSession, "session approval is reported to the caller");
        delete dlg;
    }

    // Declining must run nothing.
    {
        ToolConfirmResult result;
        QDialog* dlg = buildToolConfirmDialog(nullptr, "run_bash", QJsonObject{}, theme, 100, &result);
        QPushButton* decline = buttonWithText(dlg, "Decline");
        check(decline != nullptr, "Decline button present");
        decline->click();
        check(!result.confirmed, "Decline runs nothing");
        check(!result.approveSession, "Decline does not approve the session");
        delete dlg;
    }

    // A dialog closed without a choice (title-bar close, Escape) must not
    // report an approval -- the caller answers the waiting worker with it.
    {
        ToolConfirmResult result;
        QDialog* dlg = buildToolConfirmDialog(nullptr, "run_bash", QJsonObject{}, theme, 100, &result);
        dlg->reject();
        check(!result.confirmed, "dismissing without choosing runs nothing");
        delete dlg;
    }

    std::cout << "tool confirm dialog tests passed" << std::endl;
    return 0;
}
