#pragma once
// Edit Chat dialog: rename, generate a title with the model, or export as
// Markdown.  Shared verbatim between PengyR's GUI and PengyCPP (see
// chattitle.h); mirrors Python Pengy's pengy/ui/edit_chat_dialog.py.
#include "themehelper.h"

#include <QDialog>
#include <QJsonObject>

class QLabel;
class QLineEdit;
class QPushButton;

// User and assistant turns of `chat` as a Markdown document.
QString chatMarkdown(const QJsonObject& chat);

class EditChatDialog : public QDialog {
    Q_OBJECT
public:
    EditChatDialog(const QJsonObject& chat, const QString& model, const Theme& theme,
                   QWidget* parent = nullptr);

    QString title() const;
    // "model" for an unedited generated title, "user" for a typed one.
    // Saving the title the chat already had keeps its current source (empty
    // = automatic), so opening the dialog and pressing Save doesn't mark an
    // automatic title as hand-picked and hide it from Bulk Rename.
    QString titleSource() const;

#ifdef PENGY_UNIT_TEST
    void testGenerated(const QString& title) { onGenerated(title); }
    void testSetTitle(const QString& title);
#endif

private:
    void generate();
    void onGenerated(const QString& title);
    void onFailed(const QString& error);
    void resetGenerateButton();
    void showStatus(const QString& text);
    void exportMarkdown();

    QJsonObject m_chat;
    Theme m_theme;
    QString m_generated;
    QLineEdit* m_titleInput;
    QPushButton* m_generateBtn;
    QLabel* m_statusLabel;
};
