#pragma once
#include "config.h"
#include <QMainWindow>
#include <QJsonObject>
#include <QJsonArray>
#include <QTimer>
#include "messagequeue.h"
#include <QPushButton>
#include <QTabWidget>
#include <QMap>
#include <QVector>
#include <QDragEnterEvent>
#include <QDragMoveEvent>
#include <QDragLeaveEvent>
#include <QDropEvent>

class ChatHistoryWidget;
class ChatView;
class ChatInputWidget;
class ChatWorker;
class QDialog;

/// Per-tab state for a single chat.
struct TabSession {
    QJsonObject chat;
    ChatView*   chatView = nullptr;
    ChatWorker* worker   = nullptr;
    bool        toolsApprovedForSession = false; // Memory-only; never saved to chat/config.
    bool        thinking      = false;
    bool        toolRunning   = false;
    int         promptTokens     = 0;
    int         completionTokens = 0;
    /// Messages typed while this tab's turn was running. Memory only: a message
    /// that was never sent must not appear in chats.json as if it had been.
    MessageQueue queue;
};

class MainWindow : public QMainWindow {
    Q_OBJECT
public:
    explicit MainWindow(QWidget* parent = nullptr);
    void closeEvent(QCloseEvent* event) override;

protected:
    // Window-wide drop target.  Qt hands a drag to the deepest widget that
    // accepts it, so the composer keeps priority and these are the fallback for
    // the rest of the window: the transcript, the sidebar, the tab bar, the
    // splitter gutters.  Every decision here is the composer's own rule.
    void dragEnterEvent(QDragEnterEvent* event) override;
    void dragMoveEvent(QDragMoveEvent* event) override;
    void dragLeaveEvent(QDragLeaveEvent* event) override;
    void dropEvent(QDropEvent* event) override;

private slots:
    void createNewChat();
    void loadChat(const QString& chatId);
    void deleteChat(const QString& chatId);
    void sendMessage(const QString& text, const QStringList& images);
    void openSettings();
    void openTasks();
    void onWorkerEvent(const QString& eventJson);
    void onWorkerFinished();
    void onWorkerError(const QString& msg);
    void stopWorker();
    void redactLast();
    void pollToolConfirmation();
    void onModelChanged(const QString& model);
    void onEffortChanged(const QString& effort);
    void editChat(const QString& chatId);
    void onSearchHitSelected(const QString& chatId, const QList<int>& sources, const QStringList& terms);
    void bulkRenameChats(QWidget* parent);

private:
    // ── UI setup ──────────────────────────────────────────────────
    void setupUi();
    void applyTheme();
    void loadChatList();
    void refreshModelCombo();
    QString modelForSession(TabSession* session) const;

    // ── Tab management ────────────────────────────────────────────
    TabSession* addTab(const QJsonObject& chat, bool switchTo = true);
    void closeTab(int index);
    void installTabCloseButton(int index);
    void onTabChanged(int index);
    TabSession* tabForChat(const QString& chatId);
    void saveOpenTabs();
    void updateTabTitle(TabSession* session);
    void loadIntoNewTab(const QString& chatId);

    // ── Message helpers ───────────────────────────────────────────
    // Renders chat["messages"][index]; the index tags user/assistant
    // messages so a search hit can find them (ChatView::revealHits).
    void renderMessage(ChatView* view, const QJsonObject& msg, int index);
    static QJsonObject assistantDisplayMessage(const QJsonObject& msg);
    void processResponse(TabSession* session, const QJsonArray& apiMessages);
    // ── Message queue (typed while a turn runs) ───────────────────
    // sendMessage() queues instead of cancelling an in-flight turn; startTurn()
    // is the old sendMessage body and always begins a turn.
    void startTurn(TabSession* session, const QString& text, const QStringList& images);
    void enqueueForTurn(TabSession* session, const QString& text, const QStringList& images);
    void refreshQueueUi(TabSession* session);
    void releaseQueued(TabSession* session);
    void recallQueued(TabSession* session, int direction);
    bool sessionBusy(TabSession* session) const;
    void handleToolConfirm(TabSession* session, const QJsonObject& toolRequest);
    void handleFinalResponse(TabSession* session, const QJsonObject& response);
    void showTurnError(TabSession* session, const QString& text);
    void handleQuestionRequest(TabSession* session, const QJsonObject& event);
    void updateQuickSettingsFor(TabSession* session);
    void applyBulkTitle(const QString& chatId, const QString& title);

    // ── Worker lifecycle ──────────────────────────────────────────
    void abandonWorkerFor(TabSession* session);

    int m_runtimeUiScale = 100;

    // Cached model list for the configured endpoint, kept so modelForSession can
    // fall back to a real model name when none is configured.
    QStringList m_cachedModels;

    Config     m_config;
    QJsonArray m_chats;
    QString    m_activeChatId;

    ChatHistoryWidget* m_chatHistory;
    QTabWidget*        m_tabWidget;
    ChatInputWidget*   m_chatInput;
    QPushButton*       m_stopBtn;
    QPushButton*       m_redactBtn;

    // Tab state
    QMap<QString, TabSession> m_openTabs;
    QMap<ChatWorker*, QString> m_workerToChat;
    // Workers whose tab moved on (Stop / new message) but whose thread may
    // still be running.  Kept so closeEvent can wait for them before the
    // MainWindow (their parent) is destroyed.
    QVector<ChatWorker*> m_abandonedWorkers;

    QTimer*     m_confirmTimer = nullptr;
    bool        m_sudoDialogOpen = false;
    QDialog*    m_sudoDialog = nullptr;
    ChatWorker* m_sudoDialogWorker = nullptr;
    QDialog*    m_questionDialog = nullptr;
    ChatWorker* m_questionDialogWorker = nullptr;
};
