#pragma once
#include "themehelper.h"
#include <QWidget>
#include <QListWidget>
#include <QPushButton>
#include <QLabel>
#include <QComboBox>
#include <QJsonArray>
#include <QTimer>

class ChatHistoryWidget : public QWidget {
    Q_OBJECT
public:
    explicit ChatHistoryWidget(QWidget* parent = nullptr);
    void applyTheme(const Theme& theme, int scale = 100);

    void loadChats(const QJsonArray& chats);
    void addChat(const QString& id, const QString& title);
    void removeChat(const QString& id);
    void selectChatById(const QString& id);
    void updateChatTitle(const QString& id, const QString& title);
    void setModels(const QStringList& models, const QString& current);
    void updateQuickSettings(const QString& model, const QString& confirm);
    void updateTokenUsage(int prompt, int completion);
    void setThinking(bool thinking);
    void setToolRunning(bool running);
    void setRetrying(const QString& text);

#ifdef PENGY_UNIT_TEST
    int testChatCount() const { return m_chatList->count(); }
    QString testChatIdAt(int i) const { return m_chatList->item(i)->data(Qt::UserRole).toString(); }
    bool testModelHintHidden() const { return m_modelHint->isHidden(); }
    QString testModelHintText() const { return m_modelHint->text(); }
#endif

signals:
    void chatSelected(const QString& id);
    void newChatRequested();
    void settingsRequested();
    void tasksRequested();
    void deleteRequested(const QString& id);
    void modelChanged(const QString& model);

private slots:
    void onItemClicked(QListWidgetItem* item);
    void blinkDot();
    void onModelCommit();

private:
    void     setupUi();
    QWidget* makeItemWidget(const QString& id, const QString& title);
    void     saveChatMarkdown(const QString& id);

    Theme m_theme;
    int m_scale = 100;

    QPushButton* m_newChatBtn;
    QPushButton* m_settingsBtn;
    QPushButton* m_tasksBtn;
    QListWidget* m_chatList;
    QLabel* m_statusLabel;
    QLabel*      m_statusDot;
    QLabel*      m_statusText;
    QLabel*      m_modelLabel;
    QComboBox*   m_modelCombo;
    QLabel*      m_modelHint;
    QLabel*      m_confirmLabel;
    QLabel*      m_tokensLabel;
    QTimer*      m_blinkTimer;
    bool         m_dotPhase = true;
    QString      m_currentModel;
};
