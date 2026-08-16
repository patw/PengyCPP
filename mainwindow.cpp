#include "mainwindow.h"
#include "chathistory.h"
#include "chatview.h"
#include "chatinput.h"
#include "chatworker.h"
#include "settingsdialog.h"
#include "tasksdialog.h"
#include "themehelper.h"
#include "iconhelper.h"
#include <QTabBar>
#include <QToolButton>
#include "config.h"
#include "chatmanager.h"
#include "tools.h"
#include "image_utils.h"

#include <QSplitter>
#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QPushButton>
#include <QJsonDocument>
#include <QJsonArray>
#include <QJsonObject>
#include <QMessageBox>
#include <QFile>
#include <QMimeDatabase>
#include <QMimeType>
#include <QLabel>
#include <QDialog>
#include <QPlainTextEdit>
#include <QTextOption>
#include <QInputDialog>
#include <QLineEdit>
#include <QCloseEvent>
#include <QScrollArea>
#include <QGroupBox>
#include <QButtonGroup>
#include <QRadioButton>

MainWindow::MainWindow(QWidget* parent) : QMainWindow(parent) {
    m_config = configLoad();
    m_runtimeUiScale = m_config.uiScale;
    setupUi();
    applyTheme();

    Tools::setUserAgent(m_config.userAgent);
    Tools::setTimeout(m_config.toolTimeout);
    Tools::setToolOutputMaxChars(m_config.toolOutputMaxChars);
    Tools::setDownloadMaxMb(m_config.downloadMaxMb);
    Tools::setImageLimits(m_config.imageMaxDimension, m_config.imageMaxMb, m_config.imageQuality);

    // Poll for sudo password requests from any tab's worker
    m_confirmTimer = new QTimer(this);
    m_confirmTimer->setInterval(100);
    connect(m_confirmTimer, &QTimer::timeout, this, &MainWindow::pollToolConfirmation);

    loadChatList();
    refreshModelCombo();

    // Restore open tabs from config, or create initial chat
    QStringList openIds = m_config.openTabs;

    if (!openIds.isEmpty()) {
        for (const QString& cid : openIds) {
            QJsonObject chat = chatGet(cid);
            if (!chat.isEmpty())
                addTab(chat, (cid == openIds.last()));
        }
    }

    if (m_openTabs.isEmpty()) {
        if (m_chats.isEmpty())
            createNewChat();
        else
            loadIntoNewTab(m_chats[0].toObject()["id"].toString());
    }
}

// ── UI setup ──────────────────────────────────────────────────────

void MainWindow::setupUi() {
    setWindowTitle("Pengy 🐧");
    resize(1100, 700);

    auto* central = new QWidget;
    setCentralWidget(central);
    auto* mainLayout = new QHBoxLayout(central);
    mainLayout->setSpacing(0);
    mainLayout->setContentsMargins(0, 0, 0, 0);

    // Left sidebar
    auto* leftSplitter = new QSplitter(Qt::Vertical);
    m_chatHistory = new ChatHistoryWidget;
    connect(m_chatHistory, &ChatHistoryWidget::chatSelected,     this, &MainWindow::loadChat);
    connect(m_chatHistory, &ChatHistoryWidget::newChatRequested, this, &MainWindow::createNewChat);
    connect(m_chatHistory, &ChatHistoryWidget::settingsRequested,this, &MainWindow::openSettings);
    connect(m_chatHistory, &ChatHistoryWidget::tasksRequested,   this, &MainWindow::openTasks);
    connect(m_chatHistory, &ChatHistoryWidget::deleteRequested,  this, &MainWindow::deleteChat);
    connect(m_chatHistory, &ChatHistoryWidget::modelChanged,     this, &MainWindow::onModelChanged);
    leftSplitter->addWidget(m_chatHistory);

    // Right pane: tab widget + input row
    auto* rightSplitter = new QSplitter(Qt::Vertical);

    m_tabWidget = new QTabWidget;
    m_tabWidget->setTabsClosable(false);
    m_tabWidget->tabBar()->setExpanding(false);
    m_tabWidget->setMovable(true);
    m_tabWidget->setUsesScrollButtons(true);
    connect(m_tabWidget, &QTabWidget::tabCloseRequested, this, &MainWindow::closeTab);
    connect(m_tabWidget, &QTabWidget::currentChanged,    this, &MainWindow::onTabChanged);
    rightSplitter->addWidget(m_tabWidget);

    // Input row
    auto* inputRow = new QWidget;
    auto* inputLayout = new QHBoxLayout(inputRow);
    inputLayout->setContentsMargins(8, 4, 8, 4);
    m_chatInput = new ChatInputWidget;
    connect(m_chatInput, &ChatInputWidget::messageSent, this, &MainWindow::sendMessage);
    inputLayout->addWidget(m_chatInput);

    m_stopBtn = new QPushButton("Stop");
    m_stopBtn->setFixedHeight(scaledSize(32, m_runtimeUiScale));
    applyPengyIcon(m_stopBtn, "stop", makeTheme(m_config.themeMode, m_config.themeAccent), 16, "primary_fg", "primary_fg");
    m_stopBtn->setStyleSheet(
        "QPushButton { background-color: #d20f39; color: white; border: none; "
        "border-radius: 8px; padding: 4px 14px; font-weight: bold; font-size: 11pt; }"
        "QPushButton:hover { background-color: #e64553; }");
    m_stopBtn->hide();
    connect(m_stopBtn, &QPushButton::clicked, this, &MainWindow::stopWorker);
    inputLayout->addWidget(m_stopBtn);

    rightSplitter->addWidget(inputRow);
    rightSplitter->setStretchFactor(0, 1);

    // Main splitter
    auto* mainSplitter = new QSplitter(Qt::Horizontal);
    mainSplitter->addWidget(leftSplitter);
    mainSplitter->addWidget(rightSplitter);
    mainSplitter->setStretchFactor(0, 0);
    mainSplitter->setStretchFactor(1, 1);
    mainSplitter->setSizes({300, 800});
    mainLayout->addWidget(mainSplitter);
}

// ── Theme ─────────────────────────────────────────────────────────

void MainWindow::applyTheme() {
    Theme theme = makeTheme(m_config.themeMode, m_config.themeAccent);
    qApp->setFont(scaledSystemFont(m_runtimeUiScale));
    qApp->setStyleSheet(appStyleSheet(theme, m_runtimeUiScale));
    if (m_chatInput) m_chatInput->applyTheme(theme, m_runtimeUiScale);
    if (m_chatHistory) m_chatHistory->applyTheme(theme, m_runtimeUiScale);
    if (m_stopBtn) {
        m_stopBtn->setFixedHeight(scaledSize(32, m_runtimeUiScale));
        applyPengyIcon(m_stopBtn, "stop", theme, 16, "primary_fg", "primary_fg");
        m_stopBtn->setStyleSheet(QString(
            "QPushButton { background-color:%1; color:white; border:none; border-radius:8px; padding:4px 14px; font-weight:bold; font-size:11pt; }"
            "QPushButton:hover { background-color:%2; }").arg(theme["danger"], theme["danger_hover"]));
    }
    // Re-theme all open tab chat views
    for (auto& session : m_openTabs) {
        if (session.chatView)
            session.chatView->applyTheme(theme, m_runtimeUiScale);
    }
    for (int i = 0; i < m_tabWidget->count(); ++i) {
        if (auto* button = qobject_cast<QToolButton*>(m_tabWidget->tabBar()->tabButton(i, QTabBar::RightSide))) {
            button->setFixedSize(scaledSize(22, m_runtimeUiScale), scaledSize(22, m_runtimeUiScale));
            applyPengyIcon(button, "close", theme, scaledSize(13, m_runtimeUiScale), "muted", "danger");
        }
    }
}

void MainWindow::loadChatList() {
    m_chats = chatsLoadIndex();
    m_chatHistory->loadChats(m_chats);
}

void MainWindow::refreshModelCombo() {
    QStringList models = modelCacheForBaseUrl(m_config.baseUrl);

    TabSession* session = tabForChat(m_activeChatId);
    QString current = session ? modelForSession(session) : m_config.model;
    m_chatHistory->setModels(models, current);
}

QString MainWindow::modelForSession(TabSession* session) const {
    if (session) {
        QString overrideModel = session->chat["model"].toString();
        if (!overrideModel.isEmpty())
            return overrideModel;
    }
    return m_config.model;
}

void MainWindow::onModelChanged(const QString& model) {
    QString m = model.trimmed();
    TabSession* session = tabForChat(m_activeChatId);
    if (!session || m.isEmpty())
        return;
    if (session->chat["model"].toString() == m)
        return;
    session->chat["model"] = m;
    chatSave(session->chat);
    updateQuickSettingsFor(session);
}

// ── Tab management ────────────────────────────────────────────────

TabSession* MainWindow::addTab(const QJsonObject& chat, bool switchTo) {
    auto* chatView = new ChatView;

    TabSession session;
    session.chat     = chat;
    session.chatView = chatView;

    // Apply theme FIRST so renderNow() uses the correct colours
    Theme theme = makeTheme(m_config.themeMode, m_config.themeAccent);
    chatView->applyTheme(theme, m_runtimeUiScale);

    // Render existing messages
    QJsonArray messages = chat["messages"].toArray();
    for (const QJsonValue& v : messages)
        renderMessage(chatView, v.toObject());
    chatView->renderNow();

    QString chatId = chat["id"].toString();
    m_openTabs[chatId] = session;

    QString title = chat["title"].toString("New Chat").left(30);
    int idx = m_tabWidget->addTab(chatView, title);
    installTabCloseButton(idx);

    if (switchTo)
        m_tabWidget->setCurrentIndex(idx);

    saveOpenTabs();
    return &m_openTabs[chatId];
}

void MainWindow::installTabCloseButton(int index) {
    auto* button = new QToolButton(m_tabWidget->tabBar());
    button->setAutoRaise(true);
    button->setCursor(Qt::ArrowCursor);
    button->setToolTip("Close tab");
    button->setAccessibleName("Close tab");
    button->setFixedSize(scaledSize(22, m_runtimeUiScale), scaledSize(22, m_runtimeUiScale));
    Theme theme = makeTheme(m_config.themeMode, m_config.themeAccent);
    applyPengyIcon(button, "close", theme, scaledSize(13, m_runtimeUiScale), "muted", "danger");
    button->setStyleSheet(QString("QToolButton { background:transparent; border:none; border-radius:5px; padding:3px; } QToolButton:hover { background:%1; }").arg(theme["hover"]));
    connect(button, &QToolButton::clicked, this, [this, button]() {
        QTabBar* bar = m_tabWidget->tabBar();
        for (int i = 0; i < m_tabWidget->count(); ++i)
            if (bar->tabButton(i, QTabBar::RightSide) == button) { closeTab(i); return; }
    });
    m_tabWidget->tabBar()->setTabButton(index, QTabBar::RightSide, button);
}

void MainWindow::closeTab(int index) {
    if (index < 0) return;
    QWidget* w = m_tabWidget->widget(index);
    QString chatId;
    for (auto it = m_openTabs.begin(); it != m_openTabs.end(); ++it) {
        if (it->chatView == w) {
            chatId = it.key();
            break;
        }
    }

    if (chatId.isEmpty()) {
        m_tabWidget->removeTab(index);
        return;
    }

    TabSession& session = m_openTabs[chatId];
    abandonWorkerFor(&session);

    // Save or delete the chat
    bool isEmptyNew = (session.chat["title"].toString() == "New Chat"
                       && session.chat["messages"].toArray().isEmpty());
    if (isEmptyNew)
        chatDelete(chatId);
    else
        chatSave(session.chat);

    m_tabWidget->removeTab(index);
    m_openTabs.remove(chatId);

    saveOpenTabs();

    // Always give a fresh tab if the user closes the last one
    if (m_tabWidget->count() == 0)
        createNewChat();
}

void MainWindow::onTabChanged(int index) {
    if (index < 0) return;
    QWidget* w = m_tabWidget->widget(index);
    for (auto it = m_openTabs.begin(); it != m_openTabs.end(); ++it) {
        if (it->chatView == w) {
            m_activeChatId = it.key();
            m_chatHistory->selectChatById(it.key());
            updateQuickSettingsFor(&it.value());
            m_stopBtn->setVisible(it->thinking);
            return;
        }
    }
}

TabSession* MainWindow::tabForChat(const QString& chatId) {
    auto it = m_openTabs.find(chatId);
    return (it != m_openTabs.end()) ? &it.value() : nullptr;
}

void MainWindow::saveOpenTabs() {
    QStringList ids;
    for (const auto& key : m_openTabs.keys())
        ids.append(key);
    m_config.openTabs = ids;
    configSave(m_config);
}

void MainWindow::updateTabTitle(TabSession* session) {
    QString base = session->chat["title"].toString("New Chat").left(30);
    QString prefix = session->thinking ? "● " : "";

    for (int i = 0; i < m_tabWidget->count(); ++i) {
        if (m_tabWidget->widget(i) == session->chatView) {
            m_tabWidget->setTabText(i, prefix + base);
            return;
        }
    }
}

void MainWindow::loadIntoNewTab(const QString& chatId) {
    // If there's a single empty "New Chat" tab, replace it
    if (m_openTabs.size() == 1) {
        QString onlyId = m_openTabs.firstKey();
        TabSession& onlySession = m_openTabs.first();
        if (onlySession.chat["title"].toString() == "New Chat"
            && onlySession.chat["messages"].toArray().isEmpty()) {
            chatDelete(onlySession.chat["id"].toString());
            for (int i = 0; i < m_tabWidget->count(); ++i) {
                if (m_tabWidget->widget(i) == onlySession.chatView) {
                    m_tabWidget->removeTab(i);
                    break;
                }
            }
            m_openTabs.remove(onlyId);
        }
    }

    QJsonObject chat = chatGet(chatId);
    if (chat.isEmpty()) return;

    addTab(chat, true);
    m_activeChatId = chatId;
    m_chatHistory->selectChatById(chatId);

    TabSession* session = tabForChat(chatId);
    if (session) updateQuickSettingsFor(session);
}

// ── Message rendering ─────────────────────────────────────────────

// The ChatView display object for an assistant message, carrying whichever
// reasoning field the provider used.
QJsonObject MainWindow::assistantDisplayMessage(const QJsonObject& msg) {
    QJsonObject display;
    display["role"]    = "assistant";
    display["content"] = msg["content"].toString();
    if (msg.contains("reasoning_content"))
        display["reasoning_content"] = msg["reasoning_content"];
    else if (msg.contains("reasoning"))
        display["reasoning_content"] = msg["reasoning"];
    return display;
}

void MainWindow::renderMessage(ChatView* view, const QJsonObject& msg) {
    QString role = msg["role"].toString();

    if (role == "user") {
        QString content = msg["content"].isString()
            ? msg["content"].toString()
            : QJsonDocument(msg["content"].toArray()).toJson(QJsonDocument::Compact);
        view->appendMessageText("user", content, false);

    } else if (role == "assistant") {
        // Text first, tool cards after: the model wrote its narration *before*
        // deciding on the tool calls in the same message, and that is the order
        // the live run renders it in.
        if (!msg["content"].toString().isEmpty())
            view->appendMessage("assistant", assistantDisplayMessage(msg), false);

        for (const QJsonValue& tc : msg["tool_calls"].toArray()) {
            QJsonObject tcObj = tc.toObject();
            QJsonObject fn    = tcObj["function"].toObject();
            QJsonObject argsObj = QJsonDocument::fromJson(
                fn["arguments"].toString().toUtf8()).object();
            QJsonObject req;
            req["tool_call_id"] = tcObj["id"];
            req["name"]         = fn["name"];
            req["args"]         = argsObj;
            view->appendMessage("tool_request", req, false);
        }
    } else if (role == "tool") {
        QJsonObject result;
        result["tool_call_id"] = msg["tool_call_id"];
        result["content"]      = msg["content"];
        result["declined"]     = false;
        view->appendMessage("tool_result", result, false);
    }
}

// ── Chat lifecycle ────────────────────────────────────────────────

void MainWindow::createNewChat() {
    // If any open tab is an empty "New Chat", just switch to it
    for (auto it = m_openTabs.begin(); it != m_openTabs.end(); ++it) {
        if (it->chat["title"].toString() == "New Chat"
            && it->chat["messages"].toArray().isEmpty()) {
            for (int i = 0; i < m_tabWidget->count(); ++i) {
                if (m_tabWidget->widget(i) == it->chatView) {
                    m_tabWidget->setCurrentIndex(i);
                    return;
                }
            }
        }
    }

    QJsonObject chat = chatCreate("New Chat");
    if (chat.isEmpty()) return;

    loadChatList();
    addTab(chat, true);
    m_activeChatId = chat["id"].toString();
    m_chatHistory->selectChatById(m_activeChatId);

    TabSession* session = tabForChat(m_activeChatId);
    if (session) updateQuickSettingsFor(session);
}

void MainWindow::deleteChat(const QString& chatId) {
    QJsonObject chat = chatGet(chatId);
    QString title = chat["title"].toString("this chat");
    QMessageBox::StandardButton reply = QMessageBox::question(
        this, "Delete Chat",
        QString("Delete \"%1\"?\n\nThis cannot be undone.").arg(title),
        QMessageBox::Yes | QMessageBox::Cancel,
        QMessageBox::Cancel);
    if (reply != QMessageBox::Yes) return;

    // Close tab if open
    TabSession* session = tabForChat(chatId);
    if (session) {
        abandonWorkerFor(session);
        for (int i = 0; i < m_tabWidget->count(); ++i) {
            if (m_tabWidget->widget(i) == session->chatView) {
                m_tabWidget->removeTab(i);
                break;
            }
        }
        m_openTabs.remove(chatId);
    }

    chatDelete(chatId);
    loadChatList();

    if (m_tabWidget->count() == 0)
        createNewChat();

    saveOpenTabs();
}

void MainWindow::loadChat(const QString& chatId) {
    // User clicked a chat in the sidebar
    TabSession* existing = tabForChat(chatId);
    if (existing) {
        // Already open — switch to its tab
        for (int i = 0; i < m_tabWidget->count(); ++i) {
            if (m_tabWidget->widget(i) == existing->chatView) {
                m_tabWidget->setCurrentIndex(i);
                return;
            }
        }
    } else {
        loadIntoNewTab(chatId);
    }
}

void MainWindow::openSettings() {
    SettingsDialog dlg(m_config, this);
    if (dlg.exec() == QDialog::Accepted) {
        m_config = dlg.config();
        configSave(m_config);
        applyTheme();
        Tools::setUserAgent(m_config.userAgent);
        Tools::setTimeout(m_config.toolTimeout);
        Tools::setToolOutputMaxChars(m_config.toolOutputMaxChars);
        Tools::setDownloadMaxMb(m_config.downloadMaxMb);
        Tools::setImageLimits(m_config.imageMaxDimension, m_config.imageMaxMb, m_config.imageQuality);
        loadChatList();
        refreshModelCombo();
        if (!m_activeChatId.isEmpty())
            m_chatHistory->selectChatById(m_activeChatId);
        TabSession* session = tabForChat(m_activeChatId);
        if (session) updateQuickSettingsFor(session);
    }
}

void MainWindow::openTasks() {
    Theme theme = makeTheme(m_config.themeMode, m_config.themeAccent);
    TasksDialog dlg(theme, this);
    connect(&dlg, &TasksDialog::taskPlayed, this, [this](const QString& prompt) {
        sendMessage(prompt, QStringList());
    });
    dlg.exec();
}

// ── Sending messages ──────────────────────────────────────────────

void MainWindow::sendMessage(const QString& text, const QStringList& images) {
    TabSession* session = tabForChat(m_activeChatId);
    if (!session || session->chat.isEmpty()) return;

    session->yoloThisTurn = false;

    // Build display string with image placeholders
    QStringList placeholders;
    for (const QString& img : images)
        placeholders.append(QString("[Image: %1]").arg(img.section('/', -1)));
    if (!text.isEmpty()) placeholders.append(text);
    QString displayContent = placeholders.join("\n");

    // Append user message to persistent history
    QJsonObject userMsg;
    userMsg["role"]    = "user";
    userMsg["content"] = displayContent;
    QJsonArray messages = session->chat["messages"].toArray();
    messages.append(userMsg);
    session->chat["messages"] = messages;
    session->chatView->appendMessageText("user", displayContent);

    // Update chat title from first message
    if (session->chat["title"].toString() == "New Chat") {
        QString src = text.isEmpty()
            ? (images.isEmpty() ? "" : images[0].section('/', -1))
            : text;
        QString title = src.left(50);
        if (src.length() > 50) title += "...";
        session->chat["title"] = title;
        m_chatHistory->updateChatTitle(session->chat["id"].toString(), title);
        updateTabTitle(session);  // tab label was "New Chat"
    }

    chatSave(session->chat);

    session->thinking = true;
    updateTabTitle(session);
    updateQuickSettingsFor(session);
    m_stopBtn->show();

    // Build API message list
    QJsonArray apiMessages;

    if (!m_config.systemMessage.isEmpty()) {
        QJsonObject sysObj;
        sysObj["role"]    = "system";
        sysObj["content"] = configRenderSystemMessage(m_config.systemMessage);
        apiMessages.append(sysObj);
    }

    // Prior messages (all but last), cleaned + elided
    QJsonArray prior;
    for (int i = 0; i < messages.size() - 1; ++i)
        prior.append(messages[i]);
    QJsonArray cleaned = cleanDanglingToolCalls(prior);
    cleaned = elideOldToolResults(cleaned, m_config.contextKeepTurns);
    for (const QJsonValue& v : cleaned)
        apiMessages.append(v);

    // Current user message (with real image data if any)
    if (!images.isEmpty()) {
        int maxDim = m_config.imageMaxDimension;
        double maxMb = m_config.imageMaxMb;
        int quality = m_config.imageQuality;

        QJsonArray parts;
        for (const QString& imgPath : images) {
            ImageResult ir = imagePreprocess(imgPath, maxDim, maxMb, quality);
            if (ir.ok) {
                QJsonObject imgPart;
                imgPart["type"] = "image_url";
                imgPart["image_url"] = QJsonObject{
                    {"url", QString("data:%1;base64,%2")
                        .arg(ir.mime, QString::fromUtf8(ir.bytes_base64))}
                };
                parts.append(imgPart);
            }
        }
        if (!text.isEmpty())
            parts.append(QJsonObject{{"type", "text"}, {"text", text}});

        QJsonObject multiMsg;
        multiMsg["role"]    = "user";
        multiMsg["content"] = parts;
        apiMessages.append(multiMsg);
    } else {
        QJsonObject textMsg;
        textMsg["role"]    = "user";
        textMsg["content"] = displayContent;
        apiMessages.append(textMsg);
    }

    processResponse(session, apiMessages);
}

void MainWindow::processResponse(TabSession* session, const QJsonArray& apiMessages) {
    abandonWorkerFor(session);

    QString toolConfirmation = m_config.toolConfirmation;
    auto* worker = new ChatWorker(this);
    m_workerToChat[worker] = session->chat["id"].toString();

    connect(worker, &ChatWorker::eventReceived, this, &MainWindow::onWorkerEvent,
            Qt::QueuedConnection);
    connect(worker, &ChatWorker::finished,      this, &MainWindow::onWorkerFinished,
            Qt::QueuedConnection);
    connect(worker, &ChatWorker::errorOccurred, this, &MainWindow::onWorkerError,
            Qt::QueuedConnection);

    worker->start(m_config.baseUrl, m_config.apiKey, modelForSession(session),
                  apiMessages, toolConfirmation, m_config.reasoningEffort,
                  m_config.preserveReasoning);

    session->worker = worker;
    m_confirmTimer->start();
}

// ── Worker signal handlers ────────────────────────────────────────

void MainWindow::onWorkerEvent(const QString& eventJson) {
    auto* worker = qobject_cast<ChatWorker*>(sender());
    if (!worker) return;

    QString chatId = m_workerToChat.value(worker);
    if (chatId.isEmpty()) return;

    TabSession* session = tabForChat(chatId);
    if (!session) return;

    QJsonObject event = QJsonDocument::fromJson(eventJson.toUtf8()).object();
    QString     type  = event["type"].toString();

    if (type == "final_response") {
        handleFinalResponse(session, event);

    } else if (type == "tool_request") {
        session->thinking    = true;
        session->toolRunning = true;
        updateTabTitle(session);
        // Refresh the status dot *before* unblocking the worker below —
        // setToolRunning() forces an immediate repaint so the orange state
        // is actually visible for auto-approved tools.
        if (session == tabForChat(m_activeChatId))
            updateQuickSettingsFor(session);
        session->chatView->appendMessage("tool_request", event);

        QString name = event["name"].toString();
        QString tc   = m_config.toolConfirmation;
        bool skipConfirm = (tc == "all") || session->yoloThisTurn ||
            (tc == "safe" && Tools::isReadOnly(name));

        if (skipConfirm) {
            worker->sendConfirmation(true, false);
        } else {
            handleToolConfirm(session, event);
        }

    } else if (type == "assistant_tool_calls") {
        session->yoloThisTurn = false;
        // The narration the model wrote alongside its tool calls is persisted
        // and shows on reload, so it has to render live too -- before the tool
        // cards, which is the order it was written in.
        const QJsonObject assistantMsg = event["message"].toObject();
        if (!assistantMsg["content"].toString().trimmed().isEmpty())
            session->chatView->appendMessage("assistant",
                                             assistantDisplayMessage(assistantMsg));
        QJsonArray messages = session->chat["messages"].toArray();
        messages.append(event["message"]);
        session->chat["messages"] = messages;
        chatSave(session->chat);

    } else if (type == "question_request") {
        session->thinking    = true;
        updateTabTitle(session);
        session->chatView->appendMessage("tool_request", event);
        handleQuestionRequest(session, event);

    } else if (type == "question_result") {
        session->thinking    = true;
        updateTabTitle(session);
        session->chatView->appendMessage("tool_result", event);
        // The LLM loop already has this on its own message list; persist it
        // too, or the assistant tool_calls message above is left dangling.
        QJsonObject answerMsg;
        answerMsg["role"]         = "tool";
        answerMsg["tool_call_id"] = event["tool_call_id"];
        answerMsg["content"]      = event["content"];
        QJsonArray messages = session->chat["messages"].toArray();
        messages.append(answerMsg);
        session->chat["messages"] = messages;
        chatSave(session->chat);

    } else if (type == "tool_result") {
        session->toolRunning = false;
        session->thinking    = true;  // still thinking after tool result
        updateTabTitle(session);
        if (session == tabForChat(m_activeChatId))
            updateQuickSettingsFor(session);
        session->chatView->appendMessage("tool_result", event);
        QJsonObject toolMsg;
        toolMsg["role"]         = "tool";
        toolMsg["tool_call_id"] = event["tool_call_id"];
        toolMsg["content"]      = event["content"];
        QJsonArray messages = session->chat["messages"].toArray();
        messages.append(toolMsg);
        session->chat["messages"] = messages;
        chatSave(session->chat);
    }
}

void MainWindow::handleFinalResponse(TabSession* session, const QJsonObject& response) {
    QString content = response["content"].toString();

    if (!content.isEmpty()) {
        QJsonObject asstMsg = response["message"].toObject();
        if (asstMsg.isEmpty()) {
            asstMsg["role"]    = "assistant";
            asstMsg["content"] = content;
        }
        QJsonArray messages = session->chat["messages"].toArray();
        messages.append(asstMsg);
        session->chat["messages"] = messages;

        QJsonObject display;
        display["role"] = "assistant";
        display["content"] = content;
        if (asstMsg.contains("reasoning_content")) {
            display["reasoning_content"] = asstMsg["reasoning_content"];
        } else if (asstMsg.contains("reasoning")) {
            display["reasoning_content"] = asstMsg["reasoning"];
        }
        session->chatView->appendMessage("assistant", display);
        chatSave(session->chat);
    }

    QJsonObject usage = response["usage"].toObject();
    session->promptTokens     = usage["prompt_tokens"].toInt();
    session->completionTokens = usage["completion_tokens"].toInt();

    if (session == tabForChat(m_activeChatId))
        updateQuickSettingsFor(session);
}

void MainWindow::handleToolConfirm(TabSession* session, const QJsonObject& req) {
    Theme theme = makeTheme(m_config.themeMode, m_config.themeAccent);
    QDialog dlg(this);
    dlg.setWindowTitle("Confirm Tool: " + req["name"].toString());
    dlg.setModal(true);
    dlg.resize(480, 320);
    dlg.setMaximumWidth(600);
    dlg.setStyleSheet(appStyleSheet(theme, m_config.uiScale));

    auto* layout = new QVBoxLayout(&dlg);
    auto* header = new QLabel(QString("Execute tool: <b>%1</b>").arg(req["name"].toString()));
    header->setTextFormat(Qt::RichText);
    header->setStyleSheet(QString("color:%1; padding:8px;").arg(theme["fg"]));
    layout->addWidget(header);

    auto* argsLabel = new QLabel("Arguments:");
    argsLabel->setStyleSheet(QString("color:%1; padding:0 8px;").arg(theme["fg"]));
    layout->addWidget(argsLabel);

    QString argsText = QJsonDocument(req["args"].toObject()).toJson(QJsonDocument::Indented);
    static const int kMaxArgsLen = 4000;
    if (argsText.length() > kMaxArgsLen) {
        argsText = argsText.left(kMaxArgsLen) + QString("\n... [truncated, %1 chars total]").arg(argsText.length());
    }
    auto* argsEdit = new QPlainTextEdit(argsText);
    argsEdit->setReadOnly(true);
    argsEdit->setLineWrapMode(QPlainTextEdit::WidgetWidth);
    argsEdit->setWordWrapMode(QTextOption::WrapAnywhere);
    argsEdit->setStyleSheet(QString("color:%1; padding:4px;").arg(theme["fg"]));
    layout->addWidget(argsEdit, 1);

    auto* btnLayout = new QHBoxLayout;

    auto* execBtn = new QPushButton("Execute");
    execBtn->setStyleSheet(QString(
        "QPushButton { background-color:%1; color:%2; border:none; border-radius:6px; padding:8px 18px; font-weight:bold; }"
        "QPushButton:hover { background-color:%3; }").arg(theme["primary"], theme["primary_fg"], theme["primary_hover"]));

    auto* yesAllBtn = new QPushButton("Yes to All\nThis Turn");
    yesAllBtn->setStyleSheet(QString(
        "QPushButton { background-color:%1; color:white; border:none; border-radius:6px; padding:8px 14px; font-weight:bold; }"
        "QPushButton:hover { background-color:%2; }").arg(theme["warning"], theme["warning_hover"]));

    auto* cancelBtn = new QPushButton("Decline");
    cancelBtn->setStyleSheet(QString(
        "QPushButton { background-color:%1; color:white; border:none; border-radius:6px; padding:8px 18px; font-weight:bold; }"
        "QPushButton:hover { background-color:%2; }").arg(theme["danger"], theme["danger_hover"]));

    btnLayout->addWidget(execBtn);
    btnLayout->addWidget(yesAllBtn);
    btnLayout->addWidget(cancelBtn);
    layout->addLayout(btnLayout);

    ChatWorker* worker = session->worker;
    bool responded = false;
    connect(execBtn, &QPushButton::clicked, &dlg, [&]() {
        responded = true;
        if (worker) worker->sendConfirmation(true, false);
        dlg.accept();
    });
    connect(yesAllBtn, &QPushButton::clicked, &dlg, [&]() {
        responded = true;
        session->yoloThisTurn = true;
        if (worker) worker->sendConfirmation(true, true);
        dlg.accept();
    });
    connect(cancelBtn, &QPushButton::clicked, &dlg, [&]() {
        responded = true;
        if (worker) worker->sendConfirmation(false, false);
        dlg.reject();
    });

    dlg.exec();

    if (!responded && worker)
        worker->sendConfirmation(false, false);
}

void MainWindow::onWorkerError(const QString& msg) {
    auto* worker = qobject_cast<ChatWorker*>(sender());
    if (!worker) return;

    QString chatId = m_workerToChat.value(worker);
    TabSession* session = tabForChat(chatId);
    if (!session) return;

    session->chatView->appendMessageText("assistant", "Error: " + msg);
    // The run died mid-turn: the last assistant message may hold tool_calls
    // with no result behind them, which 400s on the next request.
    if (!session->chat.isEmpty()) {
        QJsonArray msgs = session->chat["messages"].toArray();
        session->chat["messages"] = cleanDanglingToolCalls(msgs);
        chatSave(session->chat);
    }
    session->thinking    = false;
    session->toolRunning = false;
    updateTabTitle(session);

    if (session == tabForChat(m_activeChatId)) {
        m_stopBtn->hide();
        updateQuickSettingsFor(session);
    }
}

void MainWindow::onWorkerFinished() {
    auto* worker = qobject_cast<ChatWorker*>(sender());
    if (!worker) return;

    QString chatId = m_workerToChat.take(worker);
    TabSession* session = tabForChat(chatId);
    if (!session) {
        worker->deleteLater();
        return;
    }

    session->worker       = nullptr;
    session->thinking     = false;
    session->toolRunning  = false;
    updateTabTitle(session);

    if (session == tabForChat(m_activeChatId)) {
        m_stopBtn->hide();
        m_confirmTimer->stop();
        updateQuickSettingsFor(session);
    }

    disconnect(worker, nullptr, this, nullptr);
    worker->deleteLater();
}

// ── Worker lifecycle ──────────────────────────────────────────────

void MainWindow::abandonWorkerFor(TabSession* session) {
    if (!session->worker) return;

    ChatWorker* worker = session->worker;
    m_workerToChat.remove(worker);
    worker->cancel();

    // Stop listening for UI updates, but keep the worker alive and tracked so
    // closeEvent can wait for its thread before we (its parent) are destroyed.
    // Its thread still runs to completion; reap + delete it when it finishes.
    disconnect(worker, nullptr, this, nullptr);
    m_abandonedWorkers.append(worker);
    connect(worker, &ChatWorker::finished, this, [this, worker] {
        m_abandonedWorkers.removeOne(worker);
        worker->deleteLater();
    }, Qt::QueuedConnection);

    session->worker = nullptr;
}

void MainWindow::stopWorker() {
    TabSession* session = tabForChat(m_activeChatId);
    if (!session) return;

    abandonWorkerFor(session);

    m_stopBtn->hide();
    m_confirmTimer->stop();
    session->thinking    = false;
    session->toolRunning = false;
    updateTabTitle(session);
    updateQuickSettingsFor(session);

    if (!session->chat.isEmpty()) {
        QJsonArray msgs = session->chat["messages"].toArray();
        session->chat["messages"] = cleanDanglingToolCalls(msgs);
        session->chatView->appendMessageText("assistant", "⏹ *Stopped*");
        chatSave(session->chat);
    }
}

void MainWindow::pollToolConfirmation() {
    // Check active tab's worker for pending sudo
    TabSession* session = tabForChat(m_activeChatId);
    if (!session || !session->worker || m_sudoDialogOpen) return;
    if (!session->worker->isSudoPending()) return;

    m_sudoDialogOpen = true;

    bool ok = false;
    QString password = QInputDialog::getText(
        this, "sudo Password", "Enter sudo password:",
        QLineEdit::Password, QString(), &ok);

    m_sudoDialogOpen = false;

    if (ok && !password.isEmpty()) {
        session->worker->sendSudoPassword(password);
    } else {
        session->worker->cancelSudo();
    }
}

// ── Quick settings panel ──────────────────────────────────────────

void MainWindow::updateQuickSettingsFor(TabSession* session) {
    m_chatHistory->updateQuickSettings(modelForSession(session), m_config.toolConfirmation);

    if (session->promptTokens || session->completionTokens)
        m_chatHistory->updateTokenUsage(session->promptTokens, session->completionTokens);

    // Drive the status dot + label from this tab's state
    if (session->toolRunning)
        m_chatHistory->setToolRunning(true);
    else if (session->thinking)
        m_chatHistory->setThinking(true);
    else
        m_chatHistory->setThinking(false);
}

// ── Clean shutdown ────────────────────────────────────────────────


void MainWindow::handleQuestionRequest(TabSession* session, const QJsonObject& event) {
    ChatWorker* worker = session->worker;
    if (!worker) return;

    QJsonArray questions = event["questions"].toArray();
    if (questions.isEmpty()) {
        worker->sendQuestionAnswers(QStringList());
        return;
    }

    // Show a simple dialog
    Theme theme = makeTheme(m_config.themeMode, m_config.themeAccent);
    QDialog dlg(this);
    dlg.setWindowTitle("Pengy — Questions");
    dlg.setModal(true);
    dlg.setMinimumWidth(450);
    dlg.setMaximumWidth(750);
    dlg.setStyleSheet(appStyleSheet(theme, m_config.uiScale));

    QVBoxLayout* layout = new QVBoxLayout(&dlg);
    QLabel* header = new QLabel("The assistant needs your input:");
    header->setStyleSheet(QString("color:%1; font-weight:bold; padding:4px;").arg(theme["fg"]));
    layout->addWidget(header);

    QVector<QButtonGroup*> groups;
    QScrollArea* scroll = new QScrollArea;
    scroll->setWidgetResizable(true);
    QWidget* scrollW = new QWidget;
    QVBoxLayout* scrollL = new QVBoxLayout(scrollW);
    scrollL->setSizeConstraint(QLayout::SetMinAndMaxSize);

    for (int qi = 0; qi < questions.size(); ++qi) {
        QJsonObject q = questions[qi].toObject();
        QGroupBox* gb = new QGroupBox(q["header"].toString());
        gb->setStyleSheet(QString("QGroupBox { color:%1; font-weight:bold; border:1px solid %2; border-radius:6px; margin-top:8px; padding:12px 8px 8px 8px; } QGroupBox::title { subcontrol-origin:margin; left:10px; padding:0 4px; }").arg(theme["primary"], theme["border_soft"]));
        QVBoxLayout* gl = new QVBoxLayout(gb);
        QLabel* qLabel = new QLabel(q["question"].toString());
        qLabel->setWordWrap(true);
        gl->addWidget(qLabel);
        QButtonGroup* bg = new QButtonGroup(&dlg);
        groups.append(bg);
        QJsonArray opts = q["options"].toArray();
        for (int oi = 0; oi < opts.size(); ++oi) {
            QJsonObject opt = opts[oi].toObject();
            QRadioButton* rb = new QRadioButton(QString("%1  —  %2").arg(opt["label"].toString(), opt["description"].toString()));
            bg->addButton(rb, oi);
            gl->addWidget(rb);
            if (oi == 0) rb->setChecked(true);
        }
        scrollL->addWidget(gb);
    }
    scrollL->addStretch();
    scroll->setWidget(scrollW);
    layout->addWidget(scroll, 1);

    QHBoxLayout* btnL = new QHBoxLayout;
    QPushButton* submit = new QPushButton("Submit Answers");
    submit->setStyleSheet(QString("QPushButton{background-color:%1;color:%2;border:none;border-radius:6px;padding:8px 24px;font-weight:bold;}").arg(theme["primary"], theme["primary_fg"]));
    QPushButton* cancel = new QPushButton("Cancel");
    cancel->setStyleSheet(QString("QPushButton{background-color:%1;color:white;border:none;border-radius:6px;padding:8px 24px;font-weight:bold;}").arg(theme["danger"]));
    btnL->addWidget(submit);
    btnL->addWidget(cancel);
    layout->addLayout(btnL);

    // Size dialog to fit content, with reasonable limits
    scrollW->adjustSize();
    int idealW = qMin(scrollW->sizeHint().width() + 40, 750);
    int idealH = qMin(scrollW->sizeHint().height() + 140, 650);
    dlg.resize(qMax(idealW, 480), qMax(idealH, 280));

    connect(submit, &QPushButton::clicked, &dlg, &QDialog::accept);
    connect(cancel, &QPushButton::clicked, &dlg, &QDialog::reject);

    if (dlg.exec() == QDialog::Accepted) {
        QStringList answers;
        for (auto* bg : groups) {
            QAbstractButton* checked = bg->checkedButton();
            if (checked) {
                QString text = checked->text();
                int dash = text.indexOf("  —  ");
                answers.append(dash > 0 ? text.left(dash) : text);
            } else {
                answers.append("");
            }
        }
        worker->sendQuestionAnswers(answers);
    } else {
        worker->sendQuestionAnswers(QStringList());
    }
}

void MainWindow::closeEvent(QCloseEvent* event) {
    saveOpenTabs();
    for (auto& session : m_openTabs) {
        if (!session.chat.isEmpty())
            chatSave(session.chat);
    }

    // Cancel every live worker (open tabs + already-abandoned ones) and wait
    // for its thread to stop.  Workers are parented to this window, so letting
    // one run past our destruction would use-after-free `this` in its thread.
    QVector<ChatWorker*> workers;
    for (auto& session : m_openTabs) {
        if (session.worker) workers.append(session.worker);
    }
    workers += m_abandonedWorkers;

    for (ChatWorker* w : workers) {
        if (w) w->cancel();
    }
    for (ChatWorker* w : workers) {
        if (w) w->wait(3000);
    }

    QMainWindow::closeEvent(event);
}
