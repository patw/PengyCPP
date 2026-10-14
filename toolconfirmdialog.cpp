#include "toolconfirmdialog.h"

#include <QDialog>
#include <QHBoxLayout>
#include <QJsonDocument>
#include <QLabel>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QTextOption>
#include <QVBoxLayout>

namespace {

// A model can emit a multi-megabyte write_file body; rendering all of it makes
// the dialog unusable, so arguments are truncated with the real length noted.
constexpr int kMaxArgsLen = 4000;

QString buttonStyle(const QString& bg, const QString& fg, const QString& hover,
                    int paddingH) {
    return QString(
        "QPushButton { background-color:%1; color:%2; border:none; border-radius:6px;"
        " padding:8px %4px; font-weight:bold; }"
        "QPushButton:hover { background-color:%3; }").arg(bg, fg, hover).arg(paddingH);
}

}  // namespace

QDialog* buildToolConfirmDialog(QWidget* parent, const QString& toolName,
                                const QJsonObject& args, const Theme& theme,
                                int uiScale, ToolConfirmResult* result) {
    auto* dlg = new QDialog(parent);
    dlg->setWindowTitle("Confirm Tool: " + toolName);
    dlg->setModal(true);
    dlg->resize(kToolConfirmDialogWidth, kToolConfirmDialogHeight);
    dlg->setStyleSheet(appStyleSheet(theme, uiScale));

    auto* layout = new QVBoxLayout(dlg);

    auto* header = new QLabel(QString("Execute tool: <b>%1</b>").arg(toolName));
    header->setTextFormat(Qt::RichText);
    header->setStyleSheet(QString("color:%1; padding:8px;").arg(theme["fg"]));
    layout->addWidget(header);

    auto* argsLabel = new QLabel("Arguments:");
    argsLabel->setStyleSheet(QString("color:%1; padding:0 8px;").arg(theme["fg"]));
    layout->addWidget(argsLabel);

    QString argsText = QJsonDocument(args).toJson(QJsonDocument::Indented);
    if (argsText.length() > kMaxArgsLen) {
        argsText = argsText.left(kMaxArgsLen)
            + QString("\n... [truncated, %1 chars total]").arg(argsText.length());
    }
    auto* argsEdit = new QPlainTextEdit(argsText);
    argsEdit->setReadOnly(true);
    argsEdit->setLineWrapMode(QPlainTextEdit::WidgetWidth);
    argsEdit->setWordWrapMode(QTextOption::WrapAnywhere);
    argsEdit->setStyleSheet(QString("color:%1; padding:4px;").arg(theme["fg"]));
    layout->addWidget(argsEdit, 1);

    auto* btnLayout = new QHBoxLayout;
    auto* execBtn = new QPushButton("Execute");
    execBtn->setStyleSheet(buttonStyle(theme["primary"], theme["primary_fg"],
                                       theme["primary_hover"], 18));
    auto* approveBtn = new QPushButton(QString::fromUtf8(kToolConfirmSessionApprovalText));
    approveBtn->setToolTip("Approve all future tools in this chat tab until it is closed or the app exits. "
                           "Other tabs, questions, and sudo passwords still ask.");
    approveBtn->setStyleSheet(buttonStyle(theme["warning"], "white",
                                          theme["warning_hover"], 14));
    auto* cancelBtn = new QPushButton("Decline");
    cancelBtn->setStyleSheet(buttonStyle(theme["danger"], "white",
                                         theme["danger_hover"], 18));
    btnLayout->addWidget(execBtn);
    btnLayout->addWidget(approveBtn);
    btnLayout->addWidget(cancelBtn);
    layout->addLayout(btnLayout);

    QObject::connect(execBtn, &QPushButton::clicked, dlg, [dlg, result]() {
        if (result) {
            result->confirmed = true;
            result->approveSession = false;
        }
        dlg->accept();
    });
    QObject::connect(approveBtn, &QPushButton::clicked, dlg, [dlg, result]() {
        if (result) {
            result->confirmed = true;
            result->approveSession = true;
        }
        dlg->accept();
    });
    QObject::connect(cancelBtn, &QPushButton::clicked, dlg, [dlg, result]() {
        if (result) {
            result->confirmed = false;
            result->approveSession = false;
        }
        dlg->reject();
    });

    return dlg;
}
