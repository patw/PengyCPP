#pragma once
#include "config.h"
#include <QMainWindow>
#include <QJsonObject>
#include <QJsonArray>
#include <QTimer>
#include <QPushButton>
#include <QTabWidget>
#include <QMap>

class ChatHistoryWidget;
class ChatView;
class ChatInputWidget;
class ChatWorker;

/// Per-tab state for a single chat.
struct TabSession {
    QJsonObject chat;
    ChatView*   chatView = nullptr;
    ChatWorker* worker   = nullptr;
    bool        yoloThisTurn  = false;
    bool        thinking      = false;
    bool        toolRunning   = false;
    int         promptTokens     = 0;
    int         completionTokens = 0;
};

class MainWindow : public QMainWindow {
    Q_OBJECT
public:
    explicit MainWindow(QWidget* parent = nullptr);
    void closeEvent(QCloseEvent* event) override;

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
    void pollToolConfirmation();

private:
    // ── UI setup ──────────────────────────────────────────────────
    void setupUi();
    void applyTheme();
    void loadChatList();

    // ── Tab management ────────────────────────────────────────────
    TabSession* addTab(const QJsonObject& chat, bool switchTo = true);
    void closeTab(int index);
    void onTabChanged(int index);
    TabSession* tabForChat(const QString& chatId);
    void saveOpenTabs();
    void updateTabTitle(TabSession* session);
    void loadIntoNewTab(const QString& chatId);

    // ── Message helpers ───────────────────────────────────────────
    void renderMessage(ChatView* view, const QJsonObject& msg);
    void processResponse(TabSession* session, const QJsonArray& apiMessages);
    void handleToolConfirm(TabSession* session, const QJsonObject& toolRequest);
    void handleFinalResponse(TabSession* session, const QJsonObject& response);
    void updateQuickSettingsFor(TabSession* session);

    // ── Worker lifecycle ──────────────────────────────────────────
    void abandonWorkerFor(TabSession* session);

    Config     m_config;
    QJsonArray m_chats;
    QString    m_activeChatId;

    ChatHistoryWidget* m_chatHistory;
    QTabWidget*        m_tabWidget;
    ChatInputWidget*   m_chatInput;
    QPushButton*       m_stopBtn;

    // Tab state
    QMap<QString, TabSession> m_openTabs;
    QMap<ChatWorker*, QString> m_workerToChat;

    QTimer*     m_confirmTimer = nullptr;
    bool        m_sudoDialogOpen = false;
};
