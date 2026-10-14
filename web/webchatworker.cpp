#include "webchatworker.h"
#include "../llmclient.h"
#include "../tools.h"
#include <QThread>

WebChatWorker::WebChatWorker(QObject* parent) : QObject(parent) {}

void WebChatWorker::start(const QString& baseUrl, const QString& apiKey,
                           const QString& model, const QJsonArray& messages,
                           const QString& toolConfirmation, const QString& reasoningEffort,
                           bool preserveReasoning, int llmTimeout) {
    m_baseUrl          = baseUrl;
    m_apiKey           = apiKey;
    m_model            = model;
    m_messages         = messages;
    m_toolConfirmation = toolConfirmation;
    m_reasoningEffort = reasoningEffort;
    m_preserveReasoning = preserveReasoning;
    m_cancelled        = false;
    m_promptIdleMinutes.store(0);
    m_pendingPrompt.store(false);

    // Prompt lease: announced each minute while a prompt is unanswered and no
    // browser is attached, and the run is stopped with a notice after the lease.
    // While a browser is attached the prompt waits indefinitely -- v1.11.4
    // removed the silent expiry precisely so it cannot expire under a user who
    // stepped away.
    if (!m_leaseTimer) {
        m_leaseTimer = new QTimer(this);
        m_leaseTimer->setInterval(60 * 1000);
        connect(m_leaseTimer, &QTimer::timeout, this, [this] {
            if (m_cancelled || !m_pendingPrompt.load()) { m_promptIdleMinutes.store(0); return; }
            if (m_viewers.load() > 0) { m_promptIdleMinutes.store(0); return; }
            const int idle = m_promptIdleMinutes.fetch_add(1) + 1;
            const bool expiring = idle >= PROMPT_LEASE_MINUTES;
            const QString message = expiring
                ? QString("No browser has been attached to this chat for %1 minutes, so the "
                          "waiting run is being stopped. Open the chat to answer prompts, "
                          "then send the message again.").arg(PROMPT_LEASE_MINUTES)
                : QStringLiteral("Waiting for you to answer in this chat — no browser is "
                                 "currently attached. Open the chat to continue, or press Stop.");
            emit eventReady(QJsonObject{{"type", "notice"}, {"message", message}});
            if (expiring) cancel();
        });
    }
    m_leaseTimer->start();

    {
        QMutexLocker lk(&m_mutex);
        m_confirmState = ConfirmState{};
    }
    {
        QMutexLocker lk(&m_sudoMutex);
        m_sudoPending = false;
        m_sudoPassword.clear();
    }

    Tools::setSudoPasswordProvider([this](const QString& host) -> QString {
        {
            QMutexLocker lk(&m_sudoMutex);
            m_sudoPending = true;
        }
        m_pendingPrompt.store(true);
        emit sudoRequired(host);
        QMutexLocker lk(&m_sudoMutex);
        while (m_sudoPending && !m_cancelled)
            m_sudoCond.wait(&m_sudoMutex);
        return m_cancelled ? QString() : m_sudoPassword;
    });

    auto* thread = QThread::create([this, llmTimeout] {
        QJsonArray accMsgs;
        bool yoloThisTurn = false;

        LlmClient::EventFn onEvent = [this, &accMsgs, &yoloThisTurn](const QJsonObject& ev) {
            const QString type = ev["type"].toString();

            if (type == "assistant_tool_calls") {
                yoloThisTurn = false;
                accMsgs.append(ev["message"].toObject());
                emit progress(accMsgs);
                emit eventReady(ev);

            } else if (type == "tool_request") {
                bool autoApproved =
                    m_toolConfirmation == "all" ||
                    (m_toolConfirmation == "safe" && Tools::isReadOnly(ev["name"].toString())) ||
                    yoloThisTurn;
                QJsonObject enriched = ev;
                enriched["auto_approved"] = autoApproved;
                // A tool request that needs an answer starts the prompt lease;
                // an auto-approved one does not.
                if (!autoApproved) m_pendingPrompt.store(true);
                emit eventReady(enriched);

            } else if (type == "question_request") {
                // Arm the answer wait before the browser can see the question.
                {
                    QMutexLocker lk(&m_questionMutex);
                    m_questionAnswers.clear();
                    m_questionPending = true;
                }
                m_pendingPrompt.store(true);
                emit eventReady(ev);

            } else if (type == "tool_result" || type == "question_result") {
                // question_result is a tool message too: without it the
                // assistant tool_calls message above is left dangling.
                accMsgs.append(QJsonObject{
                    {"role",         "tool"},
                    {"tool_call_id", ev["tool_call_id"].toString()},
                    {"content",      ev["content"].toString()}
                });
                emit progress(accMsgs);
                emit eventReady(ev);

            } else if (type == "final_response") {
                const QString content = ev["content"].toString();
                QJsonObject msg = ev["message"].toObject();
                if (msg.isEmpty() && !content.isEmpty()) {
                    msg = QJsonObject{{"role","assistant"},{"content",content}};
                }
                if (!msg.isEmpty()) {
                    accMsgs.append(msg);
                    emit progress(accMsgs);
                }
                emit eventReady(ev);

            } else {
                emit eventReady(ev);
            }
        };

        LlmClient::ConfirmFn onConfirm = [this, &yoloThisTurn]() -> std::pair<bool,bool> {
            QMutexLocker lk(&m_mutex);
            while (m_confirmState.status == 0 && !m_cancelled)
                m_cond.wait(&m_mutex);
            if (m_cancelled) return {false, false};
            bool confirmed = (m_confirmState.status == 2);
            bool yolo      = m_confirmState.yoloTurn;
            m_confirmState.status = 0;
            if (yolo) yoloThisTurn = true;
            return {confirmed, yolo};
        };

        LlmClient::CancelFn isCancelled = [this]() -> bool { return m_cancelled; };

        // ask_user_question always pauses for the user.  The wait is armed in
        // onEvent, before question_request goes out over SSE, so an answer that
        // beats us here clears the flag instead of being lost.
        LlmClient::QuestionFn onQuestion = [this](const QJsonArray&) -> QStringList {
            QMutexLocker lk(&m_questionMutex);
            while (m_questionPending && !m_cancelled)
                m_questionCond.wait(&m_questionMutex);
            return m_cancelled ? QStringList() : m_questionAnswers;
        };

        LlmClient client;
        LlmParams params{m_baseUrl, m_apiKey, m_model, m_messages, m_toolConfirmation, m_reasoningEffort, m_preserveReasoning, llmTimeout};
        params.recovery = recoveryOptions;
        client.run(params,
                   onEvent, onConfirm, isCancelled, onQuestion);

        Tools::clearSudoPasswordProvider();
        emit finished(accMsgs);
    });

    connect(thread, &QThread::finished, thread, &QObject::deleteLater);
    thread->start();
}

void WebChatWorker::cancel() {
    m_cancelled = true;
    Tools::killActiveProcesses();
    {
        QMutexLocker lk(&m_mutex);
        m_confirmState.status = 3;
        m_cond.wakeAll();
    }
    {
        QMutexLocker lk(&m_questionMutex);
        m_questionAnswers.clear();
        m_questionPending = false;
        m_questionCond.wakeAll();
    }
    {
        QMutexLocker lk(&m_sudoMutex);
        m_sudoPending = false;
        m_sudoCond.wakeAll();
    }
}

void WebChatWorker::sendConfirmation(bool confirmed, bool yoloTurn) {
    m_pendingPrompt.store(false);
    QMutexLocker lk(&m_mutex);
    m_confirmState.status   = confirmed ? 2 : 3;
    m_confirmState.yoloTurn = yoloTurn;
    m_cond.wakeAll();
}

void WebChatWorker::setViewers(int viewers) {
    m_viewers.store(viewers);
}

void WebChatWorker::setPendingPrompt(bool pending) {
    m_pendingPrompt.store(pending);
    if (!pending) m_promptIdleMinutes.store(0);
}

void WebChatWorker::sendAnswers(const QStringList& answers) {
    m_pendingPrompt.store(false);
    QMutexLocker lk(&m_questionMutex);
    m_questionAnswers = answers;
    m_questionPending = false;
    m_questionCond.wakeAll();
}

void WebChatWorker::sendSudoPassword(const QString& password) {
    m_pendingPrompt.store(false);
    QMutexLocker lk(&m_sudoMutex);
    m_sudoPassword = password;
    m_sudoPending  = false;
    m_sudoCond.wakeAll();
}

void WebChatWorker::cancelSudo() {
    QMutexLocker lk(&m_sudoMutex);
    m_sudoPassword.clear();
    m_sudoPending = false;
    m_sudoCond.wakeAll();
}
