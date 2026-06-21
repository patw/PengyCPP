#pragma once
#include "config.h"
#include <QMainWindow>
#include <QJsonObject>
#include <QJsonArray>
#include <QTimer>
#include <QPushButton>

class ChatHistoryWidget;
class ChatView;
class ChatInputWidget;
class ChatWorker;

class MainWindow : public QMainWindow {
    Q_OBJECT
public:
    explicit MainWindow(QWidget* parent = nullptr);

private slots:
    void createNewChat();
    void loadChat(const QString& chatId);
    void deleteChat(const QString& chatId);
    void sendMessage(const QString& text, const QStringList& images);
    void openSettings();
    void onWorkerEvent(const QString& eventJson);
    void onWorkerFinished();
    void onWorkerError(const QString& msg);
    void stopWorker();

private:
    void setupUi();
    void loadChatList();
    void processResponse(const QJsonArray& messages);
    void handleToolConfirm(const QJsonObject& toolRequest);

    Config     m_config;
    QJsonArray m_chats;
    QString    m_currentChatId;
    QJsonObject m_currentChat;
    bool        m_yoloThisTurn = false;

    ChatHistoryWidget* m_chatHistory;
    ChatView*          m_chatView;
    ChatInputWidget*   m_chatInput;
    QPushButton*       m_stopBtn;

    ChatWorker* m_worker = nullptr;
};
