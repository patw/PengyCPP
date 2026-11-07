#include "bulkrename.h"
#include "chattitle.h"

#include <QApplication>
#include <QCheckBox>
#include <QHBoxLayout>
#include <QLabel>
#include <QMessageBox>
#include <QPointer>
#include <QProgressBar>
#include <QPushButton>
#include <QThread>
#include <QVBoxLayout>

QString bulkRenameSummaryText(const BulkRenameSummary& s) {
    QStringList parts;
    parts << QString("Renamed %1 chat%2.").arg(s.renamed).arg(s.renamed == 1 ? "" : "s");
    if (s.unchanged) parts << QString("%1 already had that title.").arg(s.unchanged);
    if (s.kept) parts << QString("%1 already titled, left alone.").arg(s.kept);
    if (s.skipped) parts << QString("%1 skipped (no messages).").arg(s.skipped);
    if (s.failed) parts << QString("%1 failed.").arg(s.failed);
    if (s.aborted)
        parts << QString("Stopped after %1 failures in a row.").arg(kBulkRenameMaxConsecutiveFailures);
    else if (s.cancelled)
        parts << "Stopped by you; chats already renamed keep their new titles. "
                 "Run it again to continue where it left off.";
    if (!s.lastError.isEmpty()) parts << "Last error: " + s.lastError;
    return parts.join(' ');
}

std::optional<bool> confirmBulkRename(QWidget* parent, const QString& model,
                                      const QString& baseUrl) {
    if (model.trimmed().isEmpty()) {
        QMessageBox::warning(parent, "Bulk Rename Chats",
                             "No default model is configured. Set one in Settings → LLM first.");
        return std::nullopt;
    }
    const int pending = ChatTitle::bulkTargets(false).size();
    const int redoTotal = ChatTitle::bulkTargets(true).size();
    if (!redoTotal) {
        QMessageBox::information(parent, "Bulk Rename Chats", "There are no chats to rename.");
        return std::nullopt;
    }
    const QString status = pending
        ? QString("%1 chat%2 still %3 automatic title.")
              .arg(pending).arg(pending == 1 ? "" : "s").arg(pending == 1 ? "has its" : "have their")
        : QString("Every chat already has a model-generated or hand-picked title. "
                  "Tick the box below to generate new ones anyway.");
    QMessageBox box(QMessageBox::Warning, "Bulk Rename Chats",
        QString("This will use the currently configured default model (%1 at %2) to generate "
                "new titles, replacing the current ones. This cannot be undone.\n\n%3\n\n"
                "Titles you set yourself in Pengy are left alone, but chats renamed by hand "
                "before this version can't be told apart from automatic titles and will be "
                "renamed too.\n\n"
                "It makes one request per chat, so a long history can take a while. You can "
                "stop at any time and run it again later to continue where it left off.\n\n"
                "Continue?").arg(model, baseUrl, status),
        QMessageBox::Yes | QMessageBox::Cancel, parent);
    box.setDefaultButton(QMessageBox::Cancel);
    auto* redo = new QCheckBox(
        QString("Rename all chats again, including model-titled ones (%1)").arg(redoTotal));
    box.setCheckBox(redo);
    if (box.exec() != QMessageBox::Yes) return std::nullopt;
    if (!pending && !redo->isChecked()) return std::nullopt;
    return redo->isChecked();
}

BulkRenameDialog::BulkRenameDialog(const QStringList& chatIds, bool redo,
                                   std::function<void(const QString&, const QString&)> onRenamed,
                                   QWidget* parent)
    : QDialog(parent), m_chatIds(chatIds), m_redo(redo), m_onRenamed(std::move(onRenamed)) {
    setWindowTitle("Bulk Rename Chats");
    setModal(true);
    setMinimumWidth(480);

    auto* layout = new QVBoxLayout(this);
    m_statusLabel = new QLabel(QString("Renaming 0 of %1 chats…").arg(chatIds.size()));
    m_statusLabel->setWordWrap(true);
    layout->addWidget(m_statusLabel);
    m_bar = new QProgressBar;
    m_bar->setRange(0, qMax(1, int(chatIds.size())));
    m_bar->setValue(0);
    layout->addWidget(m_bar);
    m_latestLabel = new QLabel;
    m_latestLabel->setWordWrap(true);
    layout->addWidget(m_latestLabel);

    auto* row = new QHBoxLayout;
    row->addStretch(1);
    m_button = new QPushButton("Stop");
    connect(m_button, &QPushButton::clicked, this, [this]() {
        if (m_running) stop(); else accept();
    });
    row->addWidget(m_button);
    layout->addLayout(row);
}

BulkRenameDialog::~BulkRenameDialog() {
    // The thread checks this between chats and never touches us once we're gone.
    m_cancel->store(true);
}

void BulkRenameDialog::start() {
    m_running = true;
    QPointer<BulkRenameDialog> guard(this);
    const QStringList ids = m_chatIds;
    const bool redo = m_redo;
    auto cancel = m_cancel;
    QThread* thread = QThread::create([guard, ids, redo, cancel]() {
        int consecutive = 0;
        bool cancelled = false, aborted = false;
        for (int i = 0; i < ids.size(); ++i) {
            if (cancel->load()) { cancelled = true; break; }
            const QJsonObject result = ChatTitle::bulkStep(ids[i], redo);
            const QString status = result["status"].toString();
            consecutive = status == "failed" ? consecutive + 1
                        : (status == "renamed" || status == "unchanged") ? 0 : consecutive;
            QMetaObject::invokeMethod(qApp, [guard, i, result]() {
                if (guard) guard->onStep(i + 1, result);
            }, Qt::QueuedConnection);
            if (consecutive >= kBulkRenameMaxConsecutiveFailures) { aborted = true; break; }
        }
        QMetaObject::invokeMethod(qApp, [guard, cancelled, aborted]() {
            if (!guard) return;
            guard->m_summary.cancelled = cancelled;
            guard->m_summary.aborted = aborted;
            guard->onFinished();
        }, Qt::QueuedConnection);
    });
    connect(thread, &QThread::finished, thread, &QObject::deleteLater);
    thread->start();
}

void BulkRenameDialog::onStep(int done, const QJsonObject& result) {
    const QString status = result["status"].toString();
    const QString title = result["title"].toString();
    if (status == "renamed") ++m_summary.renamed;
    else if (status == "unchanged") ++m_summary.unchanged;
    else if (status == "kept") ++m_summary.kept;
    else if (status == "failed") { ++m_summary.failed; m_summary.lastError = result["error"].toString(); }
    else ++m_summary.skipped;
    if ((status == "renamed" || status == "unchanged") && m_onRenamed)
        m_onRenamed(m_chatIds.value(done - 1), title);

    m_bar->setValue(done);
    if (m_running && !m_cancel->load())
        m_statusLabel->setText(QString("Renaming %1 of %2 chats…").arg(done).arg(m_chatIds.size()));
    if (status == "renamed") m_latestLabel->setText("Latest: " + title);
}

void BulkRenameDialog::onFinished() {
    m_running = false;
    m_statusLabel->setText(bulkRenameSummaryText(m_summary));
    m_latestLabel->clear();
    m_button->setText("Close");
    m_button->setEnabled(true);
}

void BulkRenameDialog::stop() {
    m_cancel->store(true);
    m_button->setEnabled(false);
    m_statusLabel->setText("Stopping after the current chat…");
}

void BulkRenameDialog::reject() {
    // Esc / window close: stop the run, but stay open until it has stopped so
    // the summary is shown and no title is written after we're gone.
    if (m_running) stop();
    else QDialog::reject();
}
