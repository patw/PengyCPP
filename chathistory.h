#pragma once
#include "themehelper.h"
#include <QWidget>
#include <QListWidget>
#include <QJsonObject>
#include <QLineEdit>
#include <QStackedWidget>
#include <atomic>
#include <memory>
#include <QPushButton>
#include <QLabel>
#include <QComboBox>
#include <QJsonArray>
#include <QTimer>

// Search results whose rich-text rows re-wrap when the sidebar is resized.
class SearchResultsList : public QListWidget {
public:
    using QListWidget::QListWidget;
    void relayoutRow(QListWidgetItem* item);

protected:
    void resizeEvent(QResizeEvent* event) override;
};

class ChatHistoryWidget : public QWidget {
    Q_OBJECT
public:
    explicit ChatHistoryWidget(QWidget* parent = nullptr);
    ~ChatHistoryWidget() override;
    void applyTheme(const Theme& theme, int scale = 100);

    void loadChats(const QJsonArray& chats);
    void addChat(const QString& id, const QString& title);
    void removeChat(const QString& id);
    void selectChatById(const QString& id);
    void updateChatTitle(const QString& id, const QString& title);
    void setModels(const QStringList& models, const QString& current);
    void updateQuickSettings(const QString& model, const QString& effort);
    void updateResponseRate(double tokensPerSecond);
    void updateTokenUsage(int prompt, int completion);
    void setThinking(bool thinking);
    void setToolRunning(bool running);
    void setRetrying(const QString& text);

    // Ctrl+K: put the cursor in the search box.
    void focusSearch();
    // Enter: search titles and message text on a worker thread.
    void runSearch();

#ifdef PENGY_UNIT_TEST
    int testChatCount() const { return m_chatList->count(); }
    QString testChatIdAt(int i) const { return m_chatList->item(i)->data(Qt::UserRole).toString(); }
    bool testModelHintHidden() const { return m_modelHint->isHidden(); }
    QLineEdit* testSearchInput() const { return m_searchInput; }
    int testListPage() const { return m_listStack->currentIndex(); }
    QString testSearchStatus() const { return m_searchStatus->text(); }
    QListWidget* testChatList() const { return m_chatList; }
    QListWidget* testResults() const { return m_resultsList; }
    int testSearchGeneration() const { return m_searchGeneration; }
    void testSearchEvent(int generation, const QJsonObject& e) { onSearchEvent(generation, e); }
    void testClickResult(int row) { onResultClicked(m_resultsList->item(row)); }
    QString testModelHintText() const { return m_modelHint->text(); }
#endif

signals:
    void chatSelected(const QString& id);
    void newChatRequested();
    void settingsRequested();
    void tasksRequested();
    void deleteRequested(const QString& id);
    void editRequested(const QString& id);
    // A search result was clicked: chat id, the hit messages' indexes in
    // chat["messages"] (empty for a title-only match), and the search words.
    void searchHitSelected(const QString& id, const QList<int>& sources, const QStringList& terms);
    void modelChanged(const QString& model);
    void effortChanged(const QString& effort);

private slots:
    void onItemClicked(QListWidgetItem* item);
    void blinkDot();
    void onModelCommit();

private:
    void     setupUi();
    QWidget* makeItemWidget(const QString& id, const QString& title);
    QString  itemTitle(QListWidgetItem* item) const;
    void     onSearchTextChanged();
    void     applyTitleFilter();
    void     cancelSearch();
    void     onSearchEvent(int generation, const QJsonObject& event);
    void     addResultRow(const QJsonObject& result);
    void     onResultClicked(QListWidgetItem* item);

    Theme m_theme;
    int m_scale = 100;

    QPushButton* m_newChatBtn;
    QPushButton* m_settingsBtn;
    QPushButton* m_tasksBtn;
    QLineEdit* m_searchInput;
    QAction* m_searchIconAction;
    QStackedWidget* m_listStack;
    QLabel* m_searchStatus;
    SearchResultsList* m_resultsList;
    // Bumped by every new search and every edit; events from an older
    // generation are dropped.  The worker polls its cancel flag between chats.
    int m_searchGeneration = 0;
    std::shared_ptr<std::atomic<bool>> m_searchCancel = std::make_shared<std::atomic<bool>>(false);
    QListWidget* m_chatList;
    QLabel* m_statusLabel;
    QLabel*      m_statusDot;
    QLabel*      m_statusText;
    QLabel*      m_modelLabel;
    QComboBox*   m_modelCombo;
    QLabel*      m_modelHint;
    QLabel*      m_effortLabel;
    QComboBox*   m_effortCombo;
    QLabel*      m_rateLabel;
    QLabel*      m_tokensLabel;
    QTimer*      m_blinkTimer;
    bool         m_dotPhase = true;
    QString      m_currentModel;
};
