#include "chatinput.h"
#include "themehelper.h"
#include "iconhelper.h"
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
#include <QFile>
#include <QStandardPaths>
#include <QTextCursor>
#include <QDir>
#include <QDragEnterEvent>
#include <QDragMoveEvent>
#include <QDragLeaveEvent>
#include <QStyle>
#include <QDropEvent>
#include <QFileInfo>
#include <QUrl>

namespace {

/// Files a drag actually carries, filtered to ones that still exist.
QStringList localFilePaths(const QMimeData* mime) {
    QStringList paths;
    if (!mime || !mime->hasUrls()) return paths;
    for (const QUrl& url : mime->urls()) {
        if (!url.isLocalFile()) continue;
        const QString path = url.toLocalFile();
        if (QFileInfo(path).isFile()) paths.append(path);
    }
    return paths;
}

/// Save an image carried by a paste or a drag to a temp file; empty when the
/// payload carries none.  Browsers, chat clients and screenshot tools hand
/// images over as raw image data with no file URL, so a URL-only drop handler
/// ignores them silently; some sources supply both forms, so callers try
/// localFilePaths() first and fall back to this.
QString imageFromMime(const QMimeData* source) {
    if (!source || !source->hasImage()) return QString();
    // macOS commonly supplies an NSImage as a QPixmap QVariant, whereas
    // Linux normally supplies a QImage.  Asking QVariant directly for a
    // QImage loses the former, yielding an empty temporary PNG.
    const QVariant imageData = source->imageData();
    QImage image;
    if (imageData.canConvert<QPixmap>())
        image = imageData.value<QPixmap>().toImage();
    if (image.isNull() && imageData.canConvert<QImage>())
        image = imageData.value<QImage>();
    if (image.isNull()) return QString();
    QString tmpDir = QStandardPaths::writableLocation(QStandardPaths::TempLocation);
    QTemporaryFile tmpFile(tmpDir + "/pengy_clip_XXXXXX.png");
    tmpFile.setAutoRemove(false);
    if (!tmpFile.open()) return QString();
    const QString path = tmpFile.fileName();
    tmpFile.close();
    if (!image.save(path, "PNG")) {
        // A failed save used to leave the empty file created above behind.
        QFile::remove(path);
        return QString();
    }
    return path;
}

/// Bytes-to-tokens is not a constant across tokenizers, so the conversion is
/// the pessimistic side: measured on the fleet engine at 4.13 bytes/token for
/// prose and 3.97 for source, and denser tokenizers exist.  Budget math uses
/// tokens * 7 / 8, which is exactly 3.5 bytes/token * a 25% share of context.
constexpr qulonglong kUnknownContextTokens = 256000;
constexpr qint64 kMaxFileBytes = 1024 * 1024;
constexpr qint64 kMinEmbedBytes = 32 * 1024;
constexpr qint64 kMaxEmbedBytes = 1024 * 1024;

} // namespace

int ChatInputWidget::embedBudgetForContext(qulonglong contextTokens) {
    const qulonglong tokens = contextTokens ? contextTokens : kUnknownContextTokens;
    const qint64 budget = qint64(tokens) * 7 / 8;
    return int(qBound(kMinEmbedBytes, budget, kMaxEmbedBytes));
}

bool ChatInputWidget::oversizedForPrompt(qulonglong sizeBytes) {
    return qint64(sizeBytes) > kMaxFileBytes;
}

void ChatInputWidget::setEmbedBudgetBytes(int bytes) {
    m_embedBudgetBytes = int(qBound(kMinEmbedBytes, qint64(bytes), kMaxEmbedBytes));
}

// ── InputEdit (subclassed QTextEdit with image paste) ──────────────

InputEdit::InputEdit(QWidget* parent) : QTextEdit(parent) {
    auto font = QFontDatabase::systemFont(QFontDatabase::FixedFont);
    font.setPointSize(10);
    setFont(font);
    setPlaceholderText("Type a message... (Enter to send, Shift+Enter for new line)");
    setAcceptDrops(true);
    applyTheme(makeTheme("system", "default"), 100);
    installEventFilter(this);
}

void InputEdit::applyTheme(const Theme& theme, int scale) {
    auto font = scaledChatFont(scale);
    setFont(font);
    document()->setDefaultFont(font);
    m_theme = theme;
    m_scale = scale;
    m_minH = scaledSize(40, scale);
    m_maxH = scaledSize(200, scale);
    setStyleSheet(QString(R"(
QTextEdit { background-color:%1; color:%2; border:1px solid %3; border-radius:8px; padding:6px 10px; }
QTextEdit:focus { border-color:%4; }
QTextEdit[fileDragActive="true"] { border:2px dashed %4; background-color:%5; }
)" ).arg(theme["input_bg"], theme["input_fg"], theme["border"], theme["focus"], theme["selection"]));
    connect(this, &QTextEdit::textChanged, this, &InputEdit::autoSize);
    autoSize();
}

void InputEdit::autoSize() {
    int lo = m_minH > 0 ? m_minH : scaledSize(40, m_scale);
    int hi = m_maxH > 0 ? m_maxH : scaledSize(200, m_scale);
    document()->setTextWidth(viewport()->width());
    int margins = int(document()->documentMargin() * 2) + contentsMargins().top() + contentsMargins().bottom() + 4;
    int wanted = int(document()->size().height()) + margins;
    setFixedHeight(qMax(lo, qMin(hi, wanted)));
    setVerticalScrollBarPolicy(wanted > hi ? Qt::ScrollBarAsNeeded : Qt::ScrollBarAlwaysOff);
}

void InputEdit::resizeEvent(QResizeEvent* event) {
    QTextEdit::resizeEvent(event);
    autoSize();
}

void InputEdit::insertFromMimeData(const QMimeData* source) {
    const QString path = imageFromMime(source);
    if (!path.isEmpty()) {
        emit imagePasted(path);
        return;
    }
    // Fall back to normal text paste
    QTextEdit::insertFromMimeData(source);
}

void InputEdit::setFileDragging(bool active) {
    if (m_fileDragging == active) return;
    m_fileDragging = active;
    emit fileDragActive(active);
}

void InputEdit::dragEnterEvent(QDragEnterEvent* event) {
    if (ChatInputWidget::canAcceptDrag(event->mimeData())) {
        setFileDragging(true);
        event->acceptProposedAction();
    } else {
        setFileDragging(false);
        event->ignore();
    }
}

void InputEdit::dragMoveEvent(QDragMoveEvent* event) {
    if (ChatInputWidget::canAcceptDrag(event->mimeData())) {
        setFileDragging(true);
        event->acceptProposedAction();
    } else {
        setFileDragging(false);
        event->ignore();
    }
}

void InputEdit::dragLeaveEvent(QDragLeaveEvent* event) {
    setFileDragging(false);
    event->accept();
}

void InputEdit::dropEvent(QDropEvent* event) {
    setFileDragging(false);
    const QStringList paths = localFilePaths(event->mimeData());
    if (!paths.isEmpty()) {
        emit filesDropped(paths);
        event->acceptProposedAction();
        return;
    }
    // A picture dragged out of a browser, a chat client or a screenshot tool
    // arrives as image data with no file URL: the paste path already knows how
    // to save it, so the drag goes through the same door.
    const QString image = imageFromMime(event->mimeData());
    if (!image.isEmpty()) {
        emit imagePasted(image);
        event->acceptProposedAction();
        return;
    }
    event->ignore();
}

bool InputEdit::eventFilter(QObject* obj, QEvent* event) {
    if (obj == this && event->type() == QEvent::KeyPress) {
        auto* ke = static_cast<QKeyEvent*>(event);
        if (ke->key() == Qt::Key_Return && !(ke->modifiers() & Qt::ShiftModifier)) {
            emit submitPressed();
            return true;
        }
        // Up/Down on an *empty* composer step through the messages queued while a
        // turn runs, so a thought typed mid-turn can be edited instead of being
        // sent verbatim when the turn ends.  Only an empty composer is
        // intercepted: with text in it, Up/Down must keep moving the cursor.
        if ((ke->key() == Qt::Key_Up || ke->key() == Qt::Key_Down)
            && !(ke->modifiers() & ~Qt::KeypadModifier)
            && toPlainText().isEmpty()) {
            emit queueRecall(ke->key() == Qt::Key_Up ? -1 : 1);
            return true;
        }
    }
    return QTextEdit::eventFilter(obj, event);
}

// ── ChatInputWidget ────────────────────────────────────────────────

ChatInputWidget::ChatInputWidget(QWidget* parent) : QWidget(parent) {
    m_theme = makeTheme("system", "default");
    auto* layout = new QVBoxLayout(this);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(2);

    // Queue strip — the messages typed while a turn was running. Above the chips
    // row so it reads as "what happens next", directly over the composer. Hidden
    // until something is waiting.
    m_queueLabel = new QLabel;
    m_queueLabel->setObjectName("queueLabel");
    m_queueLabel->setWordWrap(true);
    m_queueLabel->hide();
    layout->addWidget(m_queueLabel);

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

    m_attachBtn = new QPushButton;
    m_attachBtn->setFixedSize(36, 36);
    m_attachBtn->setToolTip("Attach a file (text or image)");
    m_attachBtn->setAccessibleName("Attach a file");
    // Styled by applyTheme().
    connect(m_attachBtn, &QPushButton::clicked, this, &ChatInputWidget::pickFile);
    rowLayout->addWidget(m_attachBtn);

    m_edit = new InputEdit;
    connect(m_edit, &InputEdit::submitPressed, this, &ChatInputWidget::onSubmit);
    connect(m_edit, &InputEdit::queueRecall, this, &ChatInputWidget::queueRecallRequested);
    connect(m_edit, &InputEdit::imagePasted, this, &ChatInputWidget::onImagePasted);
    connect(m_edit, &InputEdit::filesDropped, this, &ChatInputWidget::attachFiles);
    connect(m_edit, &InputEdit::fileDragActive, this, &ChatInputWidget::showDropCue);
    rowLayout->addWidget(m_edit);

    layout->addWidget(inputRow);

    m_dropHint = new QLabel("Drop files to attach");
    m_dropHint->setAlignment(Qt::AlignCenter);
    m_dropHint->setAccessibleName("Drop files to attach");
    m_dropHint->hide();
    layout->addWidget(m_dropHint);
}

bool ChatInputWidget::canAcceptDrag(const QMimeData* mime) {
    return !localFilePaths(mime).isEmpty() || (mime && mime->hasImage());
}

bool ChatInputWidget::handleDrop(const QMimeData* mime) {
    const QStringList paths = localFilePaths(mime);
    if (!paths.isEmpty()) {
        attachFiles(paths);
        return true;
    }
    const QString image = imageFromMime(mime);
    if (!image.isEmpty()) {
        onImagePasted(image);
        return true;
    }
    return false;
}

void ChatInputWidget::showDropCue(bool active) {
    m_dropHint->setVisible(active);
    m_edit->setProperty("fileDragActive", active);
    m_edit->style()->unpolish(m_edit);
    m_edit->style()->polish(m_edit);
    m_edit->update();
}

void ChatInputWidget::setQueueState(const QStringList& previews, bool held, bool running) {
    // The placeholder is the only always-visible statement of what Enter does, so
    // it tracks the turn rather than the queue: it must be right even when nothing
    // is queued yet.
    if (m_turnRunning != running) {
        m_turnRunning = running;
        m_edit->setPlaceholderText(running
            ? QStringLiteral("Pengy is working — type your next message and press Enter to queue it")
            : QStringLiteral("Type a message... (Enter to send, Shift+Enter for new line)"));
    }

    if (previews.isEmpty()) {
        m_queueLabel->clear();
        m_queueLabel->hide();
        return;
    }

    QString hint;
    if (held)
        hint = QStringLiteral("held after Stop — Enter to send");
    else if (running)
        hint = QStringLiteral("sends when this turn ends — ↑ edit newest · ↓ edit oldest");
    else
        hint = QStringLiteral("Enter to send — ↑ edit newest · ↓ edit oldest");

    QStringList lines;
    lines << QStringLiteral("⏳ Queued (%1) · %2").arg(previews.size()).arg(hint.toHtmlEscaped());
    for (int i = 0; i < previews.size(); ++i)
        lines << QStringLiteral("%1. %2").arg(i + 1).arg(previews.at(i).toHtmlEscaped());
    m_queueLabel->setText(lines.join(QStringLiteral("<br>")));
    m_queueLabel->show();
}

void ChatInputWidget::setComposerText(const QString& text) {
    m_edit->setPlainText(text);
    m_edit->moveCursor(QTextCursor::End);
    m_edit->setFocus();
}

bool ChatInputWidget::composerEmpty() const {
    return m_edit->toPlainText().isEmpty() && m_attachments.isEmpty();
}

void ChatInputWidget::applyTheme(const Theme& theme, int scale) {
    m_theme = theme;
    m_scale = scale;
    if (m_edit) m_edit->applyTheme(theme, scale);
    if (m_queueLabel) {
        m_queueLabel->setStyleSheet(QString(
            "color:%1; background:%2; border:1px solid %3; border-radius:6px; padding:3px 6px;")
            .arg(theme["fg"], theme["selection"], theme["border"]));
    }
    m_dropHint->setStyleSheet(QString(
        "color:%1; background:%2; border:1px dashed %1; border-radius:6px; padding:3px;")
        .arg(theme["focus"], theme["selection"]));
    if (m_attachBtn) {
        int sz = scaledSize(36, scale);
        m_attachBtn->setFixedSize(sz, sz);
        applyPengyIcon(m_attachBtn, "attach", theme, scaledSize(18, scale));
        m_attachBtn->setStyleSheet(QString(R"(
QPushButton { background:transparent; color:%1; border:1px solid %2; border-radius:6px; }
QPushButton:hover { background:%3; }
)" ).arg(theme["fg"], theme["border"], theme["hover"]));
    }
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
    if (!path.isEmpty()) attachFiles({path});
}

void ChatInputWidget::attachFiles(const QStringList& paths) {
    QStringList unsupported;
    QStringList oversized;
    for (const QString& path : paths) {
        if (!QFileInfo(path).isFile() || (!isTextFile(path) && !isImageFile(path))) {
            unsupported.append(QFileInfo(path).fileName());
        } else if (isTextFile(path) && oversizedForPrompt(QFileInfo(path).size())) {
            oversized.append(QString("%1 (%2 MB)")
                                 .arg(QFileInfo(path).fileName())
                                 .arg(QFileInfo(path).size() / (1024.0 * 1024.0), 0, 'f', 1));
        } else if (!m_attachments.contains(path)) {
            m_attachments.append(path);
            addChip(path);
        }
    }
    if (!oversized.isEmpty()) {
        QMessageBox::warning(this, "File Too Large",
            QString("Not attached, over the %1 KB limit:\n%2")
                .arg(kMaxFileBytes / 1024).arg(oversized.join("\n")));
    }
    if (!unsupported.isEmpty()) {
        QMessageBox::warning(this, "Cannot Attach File",
            "Unsupported file(s): " + unsupported.join(", ") + "\n"
            "Supported: text files and images (JPEG, PNG, GIF, WebP).");
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
    chip->setStyleSheet(QString("background:%1; border:1px solid %2; border-radius:4px;").arg(m_theme["selection"], m_theme["border"]));
    auto* chipLayout = new QHBoxLayout(chip);
    chipLayout->setContentsMargins(5, 2, 3, 2);
    chipLayout->setSpacing(3);

    QString fname = path.section('/', -1);
    auto* iconLabel = new QLabel;
    iconLabel->setPixmap(renderPengyIcon(isImageFile(path) ? "image" : "file", m_theme["fg"], 14));
    chipLayout->addWidget(iconLabel);
    auto* label = new QLabel(fname);
    label->setStyleSheet(QString("font-size:11px; color:%1; border:none; background:transparent;").arg(m_theme["fg"]));
    chipLayout->addWidget(label);

    auto* removeBtn = new QPushButton;
    removeBtn->setFixedSize(16, 16);
    removeBtn->setAccessibleName("Remove " + fname);
    applyPengyIcon(removeBtn, "close", m_theme, 10, "muted", "danger");
    removeBtn->setStyleSheet(QString(
        "QPushButton { background:transparent; border:none; border-radius:3px; }"
        "QPushButton:hover { background:%2; }").arg(m_theme["muted"], m_theme["danger"]));
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
    // Detach from the parent before the deferred delete: deleteLater() alone
    // leaves the chip in m_chipsRow's child list, so a later teardown can walk a
    // freed QObject (QObject::~QObject -> setParent_helper) and crash before
    // DeferredDelete is delivered (matches the Python edition).
    chip->setParent(nullptr);
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
            // takeAt() removes the layout item, not the child: detach from
            // m_chipsRow too, or the deferred delete leaves a dangling child
            // that a later widget teardown crashes on (see removeChip).
            item->widget()->setParent(nullptr);
            item->widget()->deleteLater();
        }
        delete item;
    }
    m_chipsRow->hide();
}

void ChatInputWidget::onSubmit() {
    QString text = m_edit->toPlainText().trimmed();
    if (text.isEmpty() && m_attachments.isEmpty()) {
        // Enter on an empty composer is not a no-op any more: it releases the
        // oldest queued message (MainWindow decides; a Stop parks the queue, and
        // this is how it gets going again). Nothing is sent from here.
        emit emptySubmit();
        return;
    }

    QStringList parts;
    QStringList images;

    for (const QString& path : m_attachments) {
        if (isImageFile(path)) {
            images.append(path);
        } else {
            QFile f(path);
            if (f.open(QIODevice::ReadOnly | QIODevice::Text)) {
                QByteArray raw = f.readAll();
                f.close();
                const qint64 total = raw.size();
                const bool capped = total > m_embedBudgetBytes;
                if (capped) raw.truncate(m_embedBudgetBytes);
                QString content = QString::fromUtf8(raw);
                const QString note = capped
                    ? QString("\n[... %1 of %2 bytes embedded; the rest is truncated, "
                              "not omitted silently]")
                          .arg(m_embedBudgetBytes).arg(total)
                    : QString();
                QString fname = path.section('/', -1);
                parts.append(QString("[File: %1]\n```\n%2%3\n```").arg(fname, content, note));
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
