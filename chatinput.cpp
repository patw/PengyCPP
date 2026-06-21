#include "chatinput.h"
#include <QHBoxLayout>
#include <QVBoxLayout>
#include <QFileDialog>
#include <QKeyEvent>
#include <QMessageBox>
#include <QFontDatabase>
#include <QLabel>
#include <QImage>
#include <QPixmap>
#include <QTemporaryFile>
#include <QStandardPaths>
#include <QDir>

// ── InputEdit (subclassed QTextEdit with image paste) ──────────────

InputEdit::InputEdit(QWidget* parent) : QTextEdit(parent) {
    auto font = QFontDatabase::systemFont(QFontDatabase::FixedFont);
    font.setPointSize(10);
    setFont(font);
    setPlaceholderText("Type a message... (Enter to send, Shift+Enter for new line)");
    setMaximumHeight(60);
    setMinimumHeight(40);
    setStyleSheet(
        "QTextEdit { background: #fff; color: #1e1e2e; border: 1px solid #ccc; "
        "border-radius: 8px; padding: 6px 10px; }"
        "QTextEdit:focus { border-color: #89b4fa; }");
    installEventFilter(this);
}

void InputEdit::insertFromMimeData(const QMimeData* source) {
    // Check for image first
    if (source->hasImage()) {
        QImage image = source->imageData().value<QImage>();
        if (!image.isNull()) {
            // Save to temp file
            QString tmpDir = QStandardPaths::writableLocation(QStandardPaths::TempLocation);
            QTemporaryFile tmpFile(tmpDir + "/pengy_clip_XXXXXX.png");
            tmpFile.setAutoRemove(false);
            if (tmpFile.open()) {
                QString path = tmpFile.fileName();
                tmpFile.close();
                if (image.save(path, "PNG")) {
                    emit imagePasted(path);
                    return;
                }
            }
        }
    }
    // Fall back to normal text paste
    QTextEdit::insertFromMimeData(source);
}

bool InputEdit::eventFilter(QObject* obj, QEvent* event) {
    if (obj == this && event->type() == QEvent::KeyPress) {
        auto* ke = static_cast<QKeyEvent*>(event);
        if (ke->key() == Qt::Key_Return && !(ke->modifiers() & Qt::ShiftModifier)) {
            emit submitPressed();
            return true;
        }
    }
    return QTextEdit::eventFilter(obj, event);
}

// ── ChatInputWidget ────────────────────────────────────────────────

ChatInputWidget::ChatInputWidget(QWidget* parent) : QWidget(parent) {
    auto* layout = new QVBoxLayout(this);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(2);

    // File chips row — hidden until something is attached
    m_chipsRow = new QWidget;
    auto* chipsLayout = new QHBoxLayout(m_chipsRow);
    chipsLayout->setContentsMargins(2, 0, 2, 0);
    chipsLayout->setSpacing(4);
    chipsLayout->addStretch();
    m_chipsRow->hide();
    layout->addWidget(m_chipsRow);

    // Input row: attach button + text edit
    auto* inputRow = new QWidget;
    auto* rowLayout = new QHBoxLayout(inputRow);
    rowLayout->setContentsMargins(0, 0, 0, 0);
    rowLayout->setSpacing(4);

    m_attachBtn = new QPushButton("📎");
    m_attachBtn->setFixedSize(32, 32);
    m_attachBtn->setToolTip("Attach a file (text or image)");
    m_attachBtn->setStyleSheet(
        "QPushButton { background: transparent; border: 1px solid #ccc; border-radius: 6px; font-size: 16px; }"
        "QPushButton:hover { background: #f0f0f0; }");
    connect(m_attachBtn, &QPushButton::clicked, this, &ChatInputWidget::pickFile);
    rowLayout->addWidget(m_attachBtn);

    m_edit = new InputEdit;
    connect(m_edit, &InputEdit::submitPressed, this, &ChatInputWidget::onSubmit);
    connect(m_edit, &InputEdit::imagePasted, this, &ChatInputWidget::onImagePasted);
    rowLayout->addWidget(m_edit);

    layout->addWidget(inputRow);
}

bool ChatInputWidget::isImageFile(const QString& path) const {
    // Check extension first
    static QStringList imageExts = {".jpg", ".jpeg", ".png", ".gif", ".webp"};
    for (const QString& ext : imageExts) {
        if (path.endsWith(ext, Qt::CaseInsensitive))
            return true;
    }
    // Fall back to MIME database
    QMimeType mime = m_mimeDb.mimeTypeForFile(path);
    return mime.name().startsWith("image/");
}

bool ChatInputWidget::isTextFile(const QString& path) const {
    // Check extension first
    static QStringList textExts = {
        ".txt", ".md", ".markdown", ".rst", ".json", ".xml", ".html", ".htm",
        ".css", ".js", ".ts", ".py", ".rb", ".go", ".rs", ".c", ".cpp", ".h",
        ".java", ".kt", ".swift", ".sh", ".bash", ".zsh", ".fish", ".ps1",
        ".yaml", ".yml", ".toml", ".ini", ".cfg", ".conf", ".config",
        ".env", ".csv", ".tsv", ".sql", ".graphql", ".proto", ".tf",
        ".log", ".diff", ".patch"
    };
    for (const QString& ext : textExts) {
        if (path.endsWith(ext, Qt::CaseInsensitive))
            return true;
    }
    // Fall back to MIME database
    QMimeType mime = m_mimeDb.mimeTypeForFile(path);
    if (mime.name().startsWith("text/"))
        return true;
    // Try to decode as UTF-8
    QFile f(path);
    if (f.open(QIODevice::ReadOnly)) {
        QByteArray head = f.read(8192);
        f.close();
        // Check if it's valid UTF-8
        QString decoded = QString::fromUtf8(head);
        if (!decoded.isEmpty() || head.isEmpty())
            return true;
    }
    return false;
}

void ChatInputWidget::pickFile() {
    QString path = QFileDialog::getOpenFileName(this, "Attach File");
    if (path.isEmpty()) return;

    if (!isTextFile(path) && !isImageFile(path)) {
        QMessageBox::warning(
            this, "Cannot Attach File",
            QString("\"%1\" is not a supported file type.\n"
                    "Supported: text files and images (JPEG, PNG, GIF, WebP).")
                .arg(path.section('/', -1)));
        return;
    }
    if (!m_attachments.contains(path)) {
        m_attachments.append(path);
        addChip(path);
    }
}

void ChatInputWidget::onImagePasted(const QString& path) {
    if (!m_attachments.contains(path)) {
        m_attachments.append(path);
        addChip(path);
    }
}

void ChatInputWidget::addChip(const QString& path) {
    auto* chip = new QWidget;
    chip->setStyleSheet("background:#e8f0fe; border:1px solid #c0d0f0; border-radius:4px;");
    auto* chipLayout = new QHBoxLayout(chip);
    chipLayout->setContentsMargins(5, 2, 3, 2);
    chipLayout->setSpacing(3);

    QString icon = isImageFile(path) ? "🖼" : "📄";
    QString fname = path.section('/', -1);
    auto* label = new QLabel(QString("%1 %2").arg(icon, fname));
    label->setStyleSheet("font-size:11px; color:#333; border:none; background:transparent;");
    chipLayout->addWidget(label);

    auto* removeBtn = new QPushButton("✕");
    removeBtn->setFixedSize(14, 14);
    removeBtn->setStyleSheet(
        "QPushButton { background: transparent; border: none; color: #888; font-size: 9px; }"
        "QPushButton:hover { color: #c00; }");
    QString pathCopy = path;
    connect(removeBtn, &QPushButton::clicked, this, [this, pathCopy, chip]() {
        removeChip(pathCopy, chip);
    });
    chipLayout->addWidget(removeBtn);

    // Insert before the trailing stretch
    auto* cl = qobject_cast<QHBoxLayout*>(m_chipsRow->layout());
    cl->insertWidget(cl->count() - 1, chip);
    m_chipsRow->show();
}

void ChatInputWidget::removeChip(const QString& path, QWidget* chip) {
    m_attachments.removeAll(path);
    chip->deleteLater();
    if (m_attachments.isEmpty()) {
        m_chipsRow->hide();
    }
}

void ChatInputWidget::clearChips() {
    auto* cl = m_chipsRow->layout();
    while (cl->count() > 1) {
        QLayoutItem* item = cl->takeAt(0);
        if (item->widget()) {
            item->widget()->deleteLater();
        }
        delete item;
    }
    m_chipsRow->hide();
}

void ChatInputWidget::onSubmit() {
    QString text = m_edit->toPlainText().trimmed();
    if (text.isEmpty() && m_attachments.isEmpty()) return;

    QStringList parts;
    QStringList images;

    for (const QString& path : m_attachments) {
        if (isImageFile(path)) {
            images.append(path);
        } else {
            QFile f(path);
            if (f.open(QIODevice::ReadOnly | QIODevice::Text)) {
                QString content = QString::fromUtf8(f.readAll());
                f.close();
                QString fname = path.section('/', -1);
                parts.append(QString("[File: %1]\n```\n%2\n```").arg(fname, content));
            } else {
                QString fname = path.section('/', -1);
                parts.append(QString("[File: %1 — error reading file]").arg(fname));
            }
        }
    }

    if (!text.isEmpty()) {
        parts.append(text);
    }

    m_edit->clear();
    m_attachments.clear();
    clearChips();

    emit messageSent(parts.join("\n\n"), images);
}
