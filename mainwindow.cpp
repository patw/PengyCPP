#include "mainwindow.h"
#include "chathistory.h"
#include "chatview.h"
#include "chatinput.h"
#include "chatworker.h"
#include "settingsdialog.h"
#include "tasksdialog.h"
#include "themehelper.h"
#include "config.h"
#include "chatmanager.h"
#include "tools.h"

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
#include <QInputDialog>
#include <QLineEdit>

MainWindow::MainWindow(QWidget* parent) : QMainWindow(parent) {
    m_config = configLoad();
    setupUi();
    applyTheme();

    // Poll for sudo password requests from the worker thread
    m_confirmTimer = new QTimer(this);
    m_confirmTimer->setInterval(100);
    connect(m_confirmTimer, &QTimer::timeout, this, &MainWindow::pollToolConfirmation);

    Tools::setUserAgent(m_config.userAgent);
    Tools::setTimeout(m_config.toolTimeout);

    loadChatList();
    m_chatHistory->updateQuickSettings(m_config.model, m_config.toolConfirmation);

    if (m_chats.isEmpty())
        createNewChat();
    else
        loadChat(m_chats[0].toObject()["id"].toString());
}

void MainWindow::setupUi() {
    setWindowTitle("Pengy 🐧");
    resize(1100, 700);

    auto* central = new QWidget;
    setCentralWidget(central);
    auto* mainLayout = new QHBoxLayout(central);
    mainLayout->setSpacing(0);
    mainLayout->setContentsMargins(0, 0, 0, 0);

    m_chatHistory = new ChatHistoryWidget;
    connect(m_chatHistory, &ChatHistoryWidget::chatSelected,    this, &MainWindow::loadChat);
    connect(m_chatHistory, &ChatHistoryWidget::newChatRequested,this, &MainWindow::createNewChat);
    connect(m_chatHistory, &ChatHistoryWidget::settingsRequested,this,&MainWindow::openSettings);
    connect(m_chatHistory, &ChatHistoryWidget::tasksRequested,   this,&MainWindow::openTasks);
    connect(m_chatHistory, &ChatHistoryWidget::deleteRequested, this, &MainWindow::deleteChat);

    m_chatView  = new ChatView;
    m_chatInput = new ChatInputWidget;
    connect(m_chatInput, &ChatInputWidget::messageSent, this, &MainWindow::sendMessage);

    m_stopBtn = new QPushButton("⏹ Stop");
    m_stopBtn->setFixedHeight(scaledSize(32, m_config.uiScale));
    m_stopBtn->setStyleSheet(
        "QPushButton { background-color: #d20f39; color: white; border: none; "
        "border-radius: 8px; padding: 4px 14px; font-weight: bold; font-size: 11pt; }"
        "QPushButton:hover { background-color: #e64553; }");
    m_stopBtn->hide();
    connect(m_stopBtn, &QPushButton::clicked, this, &MainWindow::stopWorker);

    // Layout
    auto* inputRow = new QWidget;
    auto* inputLayout = new QHBoxLayout(inputRow);
    inputLayout->setContentsMargins(8, 4, 8, 4);
    inputLayout->addWidget(m_chatInput);
    inputLayout->addWidget(m_stopBtn);

    auto* leftSplitter = new QSplitter(Qt::Vertical);
    leftSplitter->addWidget(m_chatHistory);

    auto* rightPane = new QSplitter(Qt::Vertical);
    rightPane->addWidget(m_chatView);
    rightPane->addWidget(inputRow);
    rightPane->setStretchFactor(0, 1);

    auto* mainSplitter = new QSplitter(Qt::Horizontal);
    mainSplitter->addWidget(leftSplitter);
    mainSplitter->addWidget(rightPane);
    mainSplitter->setStretchFactor(0, 0);
    mainSplitter->setStretchFactor(1, 1);
    mainSplitter->setSizes({300, 800});
    mainLayout->addWidget(mainSplitter);
}

void MainWindow::applyTheme() {
    Theme theme = makeTheme(m_config.themeMode, m_config.themeAccent);
    qApp->setStyleSheet(appStyleSheet(theme, m_config.uiScale));
    if (m_chatView) m_chatView->applyTheme(theme, m_config.uiScale);
    if (m_chatInput) m_chatInput->applyTheme(theme, m_config.uiScale);
    if (m_chatHistory) m_chatHistory->applyTheme(theme, m_config.uiScale);
    if (m_stopBtn) {
        m_stopBtn->setFixedHeight(scaledSize(32, m_config.uiScale));
        m_stopBtn->setStyleSheet(QString(
            "QPushButton { background-color:%1; color:white; border:none; border-radius:8px; padding:4px 14px; font-weight:bold; font-size:11pt; }"
            "QPushButton:hover { background-color:%2; }").arg(theme["danger"], theme["danger_hover"]));
    }
}

void MainWindow::loadChatList() {
    m_chats = chatsLoad();
    m_chatHistory->loadChats(m_chats);
}

void MainWindow::createNewChat() {
    m_chats = chatsLoad();
    if (!m_chats.isEmpty()) {
        QJsonObject first = m_chats[0].toObject();
        if (first["title"].toString() == "New Chat" && first["messages"].toArray().isEmpty()) {
            m_currentChat   = first;
            m_currentChatId = first["id"].toString();
            m_chatHistory->selectChatById(m_currentChatId);
            m_chatView->clear();
            return;
        }
    }
    QJsonObject chat = chatCreate("New Chat");
    if (chat.isEmpty()) return;
    m_currentChat   = chat;
    m_currentChatId = chat["id"].toString();
    loadChatList();
    m_chatHistory->selectChatById(m_currentChatId);
    m_chatView->clear();
}

void MainWindow::deleteChat(const QString& chatId) {
    chatDelete(chatId);
    loadChatList();
    if (m_currentChatId == chatId) {
        if (!m_chats.isEmpty())
            loadChat(m_chats[0].toObject()["id"].toString());
        else
            createNewChat();
    }
}

void MainWindow::loadChat(const QString& chatId) {
    QJsonObject chat = chatGet(chatId);
    if (chat.isEmpty()) return;

    m_currentChat   = chat;
    m_currentChatId = chatId;
    m_chatHistory->selectChatById(chatId);
    m_chatView->clear();

    QJsonArray messages = chat["messages"].toArray();
    for (const QJsonValue& v : messages) {
        QJsonObject msg  = v.toObject();
        QString     role = msg["role"].toString();

        if (role == "user") {
            QString content = msg["content"].isString()
                ? msg["content"].toString()
                : QJsonDocument(msg["content"].toArray()).toJson(QJsonDocument::Compact);
            m_chatView->appendMessageText("user", content);

        } else if (role == "assistant") {
            QJsonArray toolCalls = msg["tool_calls"].toArray();
            if (!toolCalls.isEmpty()) {
                for (const QJsonValue& tc : toolCalls) {
                    QJsonObject tcObj = tc.toObject();
                    QJsonObject fn    = tcObj["function"].toObject();
                    QJsonObject argsObj = QJsonDocument::fromJson(
                        fn["arguments"].toString().toUtf8()).object();
                    QJsonObject req;
                    req["tool_call_id"] = tcObj["id"];
                    req["name"]         = fn["name"];
                    req["args"]         = argsObj;
                    m_chatView->appendMessage("tool_request", req);
                }
                if (!msg["content"].toString().isEmpty()) {
                    QJsonObject display;
                    display["role"] = "assistant";
                    display["content"] = msg["content"].toString();
                    if (msg.contains("reasoning_content"))
                        display["reasoning_content"] = msg["reasoning_content"];
                    else if (msg.contains("reasoning"))
                        display["reasoning_content"] = msg["reasoning"];
                    m_chatView->appendMessage("assistant", display);
                }
            } else if (!msg["content"].toString().isEmpty()) {
                QJsonObject display;
                display["role"] = "assistant";
                display["content"] = msg["content"].toString();
                if (msg.contains("reasoning_content"))
                    display["reasoning_content"] = msg["reasoning_content"];
                else if (msg.contains("reasoning"))
                    display["reasoning_content"] = msg["reasoning"];
                m_chatView->appendMessage("assistant", display);
            }
        } else if (role == "tool") {
            QJsonObject result;
            result["tool_call_id"] = msg["tool_call_id"];
            result["content"]      = msg["content"];
            result["declined"]     = false;
            m_chatView->appendMessage("tool_result", result);
        }
    }
}

void MainWindow::sendMessage(const QString& text, const QStringList& images) {
    if (m_currentChat.isEmpty()) return;

    m_yoloThisTurn = false;
    m_chatHistory->setThinking(true);
    m_chatHistory->updateTokenUsage(0, 0);

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
    QJsonArray messages = m_currentChat["messages"].toArray();
    messages.append(userMsg);
    m_currentChat["messages"] = messages;
    m_chatView->appendMessageText("user", displayContent);

    // Update chat title from first message
    if (m_currentChat["title"].toString() == "New Chat") {
        QString src = text.isEmpty()
            ? (images.isEmpty() ? "" : images[0].section('/', -1))
            : text;
        QString title = src.left(50);
        if (src.length() > 50) title += "...";
        m_currentChat["title"] = title;
        m_chatHistory->updateChatTitle(m_currentChatId, title);
    }

    chatSave(m_currentChat);
    loadChatList();
    m_stopBtn->show();

    // Build API message list
    QJsonArray apiMessages;

    // System prompt
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
        QJsonArray parts;
        for (const QString& imgPath : images) {
            QFile imgFile(imgPath);
            if (imgFile.open(QIODevice::ReadOnly)) {
                QByteArray imgData = imgFile.readAll();
                imgFile.close();
                QString b64 = QString::fromUtf8(imgData.toBase64());

                QMimeDatabase mimeDb;
                QString mimeStr = mimeDb.mimeTypeForFile(imgPath).name();
                if (mimeStr.isEmpty() || !mimeStr.startsWith("image/")) {
                    QString ext = imgPath.section('.', -1).toLower();
                    if (ext == "jpg" || ext == "jpeg") mimeStr = "image/jpeg";
                    else if (ext == "png")  mimeStr = "image/png";
                    else if (ext == "gif")  mimeStr = "image/gif";
                    else if (ext == "webp") mimeStr = "image/webp";
                    else                    mimeStr = "image/jpeg";
                }

                QJsonObject imgPart;
                imgPart["type"] = "image_url";
                imgPart["image_url"] = QJsonObject{
                    {"url", QString("data:%1;base64,%2").arg(mimeStr, b64)}
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

    processResponse(apiMessages);
}

void MainWindow::processResponse(const QJsonArray& apiMessages) {
    if (m_worker) {
        disconnect(m_worker, &ChatWorker::eventReceived, this, nullptr);
        disconnect(m_worker, &ChatWorker::errorOccurred, this, nullptr);
        m_worker->cancel();
        // Don't delete — inner thread still holds 'this'. The finished
        // signal (still connected) will trigger onWorkerFinished which
        // calls deleteLater once the thread has actually exited.
        m_worker = nullptr;
    }

    m_worker = new ChatWorker(this);
    connect(m_worker, &ChatWorker::eventReceived, this, &MainWindow::onWorkerEvent,
            Qt::QueuedConnection);
    connect(m_worker, &ChatWorker::finished,      this, &MainWindow::onWorkerFinished,
            Qt::QueuedConnection);
    connect(m_worker, &ChatWorker::errorOccurred, this, &MainWindow::onWorkerError,
            Qt::QueuedConnection);

    m_worker->start(m_config.baseUrl, m_config.apiKey, m_config.model,
                    apiMessages, m_config.toolConfirmation, m_config.reasoningEffort,
                    m_config.preserveReasoning);
    m_confirmTimer->start();
}

void MainWindow::onWorkerEvent(const QString& eventJson) {
    auto* s = qobject_cast<ChatWorker*>(sender());
    if (s && s != m_worker) return;

    QJsonObject event = QJsonDocument::fromJson(eventJson.toUtf8()).object();
    QString     type  = event["type"].toString();

    if (type == "final_response") {
        QString     content = event["content"].toString();
        QJsonObject usage   = event["usage"].toObject();

        if (!content.isEmpty()) {
            QJsonObject asstMsg = event["message"].toObject();
            if (asstMsg.isEmpty()) {
                asstMsg["role"]    = "assistant";
                asstMsg["content"] = content;
            }
            QJsonArray messages = m_currentChat["messages"].toArray();
            messages.append(asstMsg);
            m_currentChat["messages"] = messages;

            // Pass reasoning_content to chat view
            QJsonObject display;
            display["role"] = "assistant";
            display["content"] = content;
            if (asstMsg.contains("reasoning_content")) {
                display["reasoning_content"] = asstMsg["reasoning_content"];
            } else if (asstMsg.contains("reasoning")) {
                display["reasoning_content"] = asstMsg["reasoning"];
            }
            m_chatView->appendMessage("assistant", display);

            chatSave(m_currentChat);
            loadChatList();
        }

        m_chatHistory->setThinking(false);
        m_chatHistory->updateTokenUsage(
            usage["prompt_tokens"].toInt(),
            usage["completion_tokens"].toInt());

    } else if (type == "tool_request") {
        m_chatView->appendMessage("tool_request", event);
        m_chatHistory->setToolRunning(true);
        QString name = event["name"].toString();
        QString tc   = m_config.toolConfirmation;

        bool skipConfirm = (tc == "all") || m_yoloThisTurn ||
            (tc == "safe" && Tools::isReadOnly(name));

        if (skipConfirm) {
            m_worker->sendConfirmation(true, false);
        } else {
            handleToolConfirm(event);
        }

    } else if (type == "assistant_tool_calls") {
        m_yoloThisTurn = false;
        QJsonArray messages = m_currentChat["messages"].toArray();
        messages.append(event["message"]);
        m_currentChat["messages"] = messages;

    } else if (type == "tool_result") {
        m_chatView->appendMessage("tool_result", event);
        m_chatHistory->setToolRunning(false);
        QJsonObject toolMsg;
        toolMsg["role"]         = "tool";
        toolMsg["tool_call_id"] = event["tool_call_id"];
        toolMsg["content"]      = event["content"];
        QJsonArray messages = m_currentChat["messages"].toArray();
        messages.append(toolMsg);
        m_currentChat["messages"] = messages;
    }
}

void MainWindow::handleToolConfirm(const QJsonObject& req) {
    Theme theme = makeTheme(m_config.themeMode, m_config.themeAccent);
    QDialog dlg(this);
    dlg.setWindowTitle("Confirm Tool: " + req["name"].toString());
    dlg.setModal(true);
    dlg.resize(480, 320);
    dlg.setStyleSheet(appStyleSheet(theme, m_config.uiScale));

    auto* layout = new QVBoxLayout(&dlg);
    QString info = QString("Execute tool: <b>%1</b><br><br>Arguments:<br><pre>%2</pre>")
        .arg(req["name"].toString(),
             QJsonDocument(req["args"].toObject()).toJson(QJsonDocument::Indented));
    auto* label = new QLabel(info);
    label->setWordWrap(true);
    label->setTextFormat(Qt::RichText);
    label->setStyleSheet(QString("color:%1; padding:8px;").arg(theme["fg"]));
    layout->addWidget(label);

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

    connect(execBtn, &QPushButton::clicked, &dlg, [&]() {
        m_worker->sendConfirmation(true, false);
        dlg.accept();
    });
    connect(yesAllBtn, &QPushButton::clicked, &dlg, [&]() {
        m_yoloThisTurn = true;
        m_worker->sendConfirmation(true, true);
        dlg.accept();
    });
    connect(cancelBtn, &QPushButton::clicked, &dlg, [&]() {
        m_worker->sendConfirmation(false, false);
        dlg.reject();
    });

    dlg.exec();
}

void MainWindow::onWorkerFinished() {
    auto* w = qobject_cast<ChatWorker*>(sender());

    if (w && w != m_worker) {
        // Stale signal from an old cancelled worker whose inner thread
        // just finished — safe to delete now that the thread is done.
        w->deleteLater();
        return;
    }

    m_stopBtn->hide();
    m_chatHistory->setThinking(false);
    m_confirmTimer->stop();
    if (m_worker) {
        disconnect(m_worker, nullptr, this, nullptr);
        m_worker->deleteLater();
        m_worker = nullptr;
    }
}

void MainWindow::onWorkerError(const QString& msg) {
    auto* s = qobject_cast<ChatWorker*>(sender());
    if (s && s != m_worker) return;

    m_chatView->appendMessageText("assistant", "Error: " + msg);
    onWorkerFinished();
}

void MainWindow::pollToolConfirmation() {
    if (!m_worker || !m_worker->isSudoPending()) return;
    if (m_sudoDialogOpen) return;

    m_sudoDialogOpen = true;

    bool ok = false;
    QString password = QInputDialog::getText(
        this, "sudo Password", "Enter sudo password:",
        QLineEdit::Password, QString(), &ok);

    m_sudoDialogOpen = false;

    if (ok && !password.isEmpty()) {
        m_worker->sendSudoPassword(password);
    } else {
        m_worker->cancelSudo();
    }
}

void MainWindow::stopWorker() {
    if (m_worker) {
        disconnect(m_worker, &ChatWorker::eventReceived, this, nullptr);
        disconnect(m_worker, &ChatWorker::errorOccurred, this, nullptr);
        m_worker->cancel();
        // Keep worker alive — inner thread still references it.
        // onWorkerFinished will deleteLater when finished() fires.
        m_worker = nullptr;
    }
    m_stopBtn->hide();
    m_chatHistory->setThinking(false);
    m_confirmTimer->stop();
    m_chatView->appendMessageText("assistant", "⏹ *Stopped*");
}

void MainWindow::openSettings() {
    SettingsDialog dlg(m_config, this);
    if (dlg.exec() == QDialog::Accepted) {
        m_config = dlg.config();
        configSave(m_config);
        applyTheme();
        Tools::setUserAgent(m_config.userAgent);
        Tools::setTimeout(m_config.toolTimeout);
        loadChatList();
        if (!m_currentChatId.isEmpty()) m_chatHistory->selectChatById(m_currentChatId);
        m_chatHistory->updateQuickSettings(m_config.model, m_config.toolConfirmation);
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
