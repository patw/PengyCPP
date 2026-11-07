#pragma once
// Bulk Rename Chats (Settings → UI): give every stored chat a model-generated
// title.  Shared verbatim between PengyR's GUI and PengyCPP (see chattitle.h);
// mirrors Python Pengy's pengy/ui/bulk_rename.py.
//
// One request per chat, so it runs on a worker thread behind a progress
// dialog and can be stopped between chats.  Each renamed chat is marked
// title_source "model" as it is written and only unmarked chats are queued,
// so a run that was stopped, crashed or closed resumes on the next one.
// "user" titles are never touched.
#include <QDialog>
#include <QJsonObject>
#include <QStringList>

#include <atomic>
#include <functional>
#include <memory>
#include <optional>

class QLabel;
class QProgressBar;
class QPushButton;

// Stop after this many API failures in a row: the endpoint is down, or the
// model is wrong, and every remaining chat would fail the same way.
constexpr int kBulkRenameMaxConsecutiveFailures = 5;

struct BulkRenameSummary {
    int renamed = 0, unchanged = 0, skipped = 0, kept = 0, failed = 0;
    bool cancelled = false, aborted = false;
    QString lastError;
};

QString bulkRenameSummaryText(const BulkRenameSummary& summary);

// Warn and ask.  nullopt = don't run; otherwise whether to also redo chats
// the model already titled.
std::optional<bool> confirmBulkRename(QWidget* parent, const QString& model,
                                      const QString& baseUrl);

class BulkRenameDialog : public QDialog {
    Q_OBJECT
public:
    // `onRenamed` runs on the UI thread for every chat whose title was
    // written, so the owner can update the sidebar and any open tab.
    BulkRenameDialog(const QStringList& chatIds, bool redo,
                     std::function<void(const QString&, const QString&)> onRenamed,
                     QWidget* parent = nullptr);
    ~BulkRenameDialog() override;

    void start();
    bool running() const { return m_running; }
    const BulkRenameSummary& summary() const { return m_summary; }

protected:
    void reject() override;

private:
    void onStep(int done, const QJsonObject& result);
    void onFinished();
    void stop();

    QStringList m_chatIds;
    bool m_redo;
    std::function<void(const QString&, const QString&)> m_onRenamed;
    std::shared_ptr<std::atomic<bool>> m_cancel = std::make_shared<std::atomic<bool>>(false);
    bool m_running = false;
    int m_consecutiveFailures = 0;
    BulkRenameSummary m_summary;

    QLabel* m_statusLabel;
    QProgressBar* m_bar;
    QLabel* m_latestLabel;
    QPushButton* m_button;
};
