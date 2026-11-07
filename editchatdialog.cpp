#include "editchatdialog.h"
#include "chattitle.h"
#include "iconhelper.h"

#include <QApplication>
#include <QDialogButtonBox>
#include <QDir>
#include <QFile>
#include <QFileDialog>
#include <QHBoxLayout>
#include <QJsonArray>
#include <QLabel>
#include <QLineEdit>
#include <QMessageBox>
#include <QPointer>
#include <QPushButton>
#include <QRegularExpression>
#include <QTextStream>
#include <QThread>
#include <QVBoxLayout>

namespace {

QString textOf(const QJsonValue& content) {
    if (content.isString()) return content.toString();
    QStringList parts;
    for (const QJsonValue& p : content.toArray()) {
        QJsonObject part = p.toObject();
        if (part["type"].toString() == "text") parts << part["text"].toString();
    }
    return parts.join(' ');
}

}  // namespace

QString chatMarkdown(const QJsonObject& chat) {
    QString md = "# " + chat["title"].toString("Chat") + "\n\n";
    for (const QJsonValue& v : chat["messages"].toArray()) {
        QJsonObject msg = v.toObject();
        const QString role = msg["role"].toString();
        const QString content = textOf(msg["content"]);
        if (role == "user") {
            md += "**You**\n\n" + content + "\n\n---\n\n";
        } else if (role == "assistant" && !content.isEmpty()) {
            md += "**Assistant**\n\n" + content + "\n\n---\n\n";
        }
    }
    return md;
}

EditChatDialog::EditChatDialog(const QJsonObject& chat, const QString& model,
                               const Theme& theme, QWidget* parent)
    : QDialog(parent), m_chat(chat), m_theme(theme) {
    setWindowTitle("Edit Chat");
    setMinimumWidth(460);

    auto* layout = new QVBoxLayout(this);
    layout->setContentsMargins(12, 12, 12, 12);
    layout->setSpacing(8);

    layout->addWidget(new QLabel("Title"));
    auto* titleRow = new QHBoxLayout;
    m_titleInput = new QLineEdit(chat["title"].toString());
    m_titleInput->setPlaceholderText("Chat title…");
    m_titleInput->selectAll();
    titleRow->addWidget(m_titleInput, 1);

    m_generateBtn = new QPushButton("Generate");
    m_generateBtn->setToolTip(QString("Generate a title with %1")
                                  .arg(model.isEmpty() ? "the configured model" : model));
    applyPengyIcon(m_generateBtn, "refresh", m_theme, 14);
    connect(m_generateBtn, &QPushButton::clicked, this, &EditChatDialog::generate);
    titleRow->addWidget(m_generateBtn);
    layout->addLayout(titleRow);

    m_statusLabel = new QLabel;
    m_statusLabel->setWordWrap(true);
    m_statusLabel->setStyleSheet(QString("color: %1;").arg(m_theme["muted"]));
    m_statusLabel->hide();
    layout->addWidget(m_statusLabel);

    layout->addSpacing(4);
    auto* bottom = new QHBoxLayout;
    auto* exportBtn = new QPushButton("Export as Markdown…");
    applyPengyIcon(exportBtn, "save", m_theme, 14);
    connect(exportBtn, &QPushButton::clicked, this, &EditChatDialog::exportMarkdown);
    bottom->addWidget(exportBtn);
    bottom->addStretch(1);

    auto* buttons = new QDialogButtonBox(QDialogButtonBox::Save | QDialogButtonBox::Cancel);
    connect(buttons, &QDialogButtonBox::accepted, this, [this]() {
        if (!title().isEmpty()) accept();
    });
    connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);
    buttons->button(QDialogButtonBox::Save)->setDefault(true);
    bottom->addWidget(buttons);
    layout->addLayout(bottom);

    setStyleSheet(QString("QDialog { background-color: %1; color: %2; } QLabel { color: %2; }")
                      .arg(m_theme["bg"], m_theme["fg"]));
}

QString EditChatDialog::title() const { return m_titleInput->text().trimmed(); }

QString EditChatDialog::titleSource() const {
    if (!m_generated.isEmpty() && title() == m_generated) return ChatTitle::kSourceModel;
    if (title() == m_chat["title"].toString()) return m_chat["title_source"].toString();
    return ChatTitle::kSourceUser;
}

#ifdef PENGY_UNIT_TEST
void EditChatDialog::testSetTitle(const QString& title) { m_titleInput->setText(title); }
#endif

void EditChatDialog::generate() {
    m_generateBtn->setEnabled(false);
    m_generateBtn->setText("Generating…");
    showStatus(QString());
    // The request can take as long as llm_timeout; run it off the UI thread
    // and drop the reply if the dialog is gone by then.
    QPointer<EditChatDialog> guard(this);
    const QJsonObject chat = m_chat;
    QThread* thread = QThread::create([guard, chat]() {
        const QJsonObject result = ChatTitle::generate(chat);
        QMetaObject::invokeMethod(qApp, [guard, result]() {
            if (!guard) return;
            if (result.contains("title"))
                guard->onGenerated(result["title"].toString());
            else
                guard->onFailed(result["error"].toString());
        }, Qt::QueuedConnection);
    });
    connect(thread, &QThread::finished, thread, &QObject::deleteLater);
    thread->start();
}

void EditChatDialog::onGenerated(const QString& title) {
    resetGenerateButton();
    m_generated = title;
    m_titleInput->setText(title);
    m_titleInput->setFocus();
    showStatus(QString());
}

void EditChatDialog::onFailed(const QString& error) {
    resetGenerateButton();
    showStatus("Couldn't generate a title: " + error);
}

void EditChatDialog::resetGenerateButton() {
    m_generateBtn->setEnabled(true);
    m_generateBtn->setText("Generate");
}

void EditChatDialog::showStatus(const QString& text) {
    m_statusLabel->setText(text);
    m_statusLabel->setVisible(!text.isEmpty());
}

void EditChatDialog::exportMarkdown() {
    // Export what is in the field, so a just-edited title names the file too.
    QJsonObject chat = m_chat;
    if (!title().isEmpty()) chat["title"] = title();
    QString safe = chat["title"].toString();
    safe.replace(QRegularExpression(R"([^\w\s-])"), "");
    safe = safe.trimmed().replace(QRegularExpression(R"(\s+)"), "_");
    if (safe.isEmpty()) safe = "chat";

    const QString path = QFileDialog::getSaveFileName(
        this, "Export Chat as Markdown", QDir::homePath() + "/" + safe + ".md",
        "Markdown (*.md);;All Files (*)");
    if (path.isEmpty()) return;
    QFile f(path);
    if (!f.open(QIODevice::WriteOnly | QIODevice::Text)) {
        QMessageBox::warning(this, "Export Failed", f.errorString());
        return;
    }
    QTextStream(&f) << chatMarkdown(chat);
    showStatus("Exported to " + path);
}
