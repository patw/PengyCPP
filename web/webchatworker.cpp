#include "webchatworker.h"
#include "../llmclient.h"
#include "../tools.h"
#include <QThread>

WebChatWorker::WebChatWorker(QObject* parent) : QObject(parent) {}

void WebChatWorker::start(const QString& baseUrl, const QString& apiKey,
                           const QString& model, const QJsonArray& messages,
                           const QString& toolConfirmation, const QString& reasoningEffort,
                           bool preserveReasoning) {
    m_baseUrl          = baseUrl;
    m_apiKey           = apiKey;
    m_model            = model;
    m_messages         = messages;
    m_toolConfirmation = toolConfirmation;
    m_reasoningEffort = reasoningEffort;
    m_preserveReasoning = preserveReasoning;
    m_cancelled        = false;

    {
        QMutexLocker lk(&m_mutex);
        m_confirmState = ConfirmState{};
    }
    {
        QMutexLocker lk(&m_sudoMutex);
        m_sudoPending = false;
        m_sudoPassword.clear();
    }

    Tools::setSudoPasswordProvider([this]() -> QString {
        {
            QMutexLocker lk(&m_sudoMutex);
            m_sudoPending = true;
        }
        emit sudoRequired();
        QMutexLocker lk(&m_sudoMutex);
        while (m_sudoPending && !m_cancelled)
            m_sudoCond.wait(&m_sudoMutex);
        return m_cancelled ? QString() : m_sudoPassword;
    });

    auto* thread = QThread::create([this] {
        QJsonArray accMsgs;
        bool yoloThisTurn = false;

        LlmClient::EventFn onEvent = [this, &accMsgs, &yoloThisTurn](const QJsonObject& ev) {
            const QString type = ev["type"].toString();

            if (type == "assistant_tool_calls") {
                yoloThisTurn = false;
                accMsgs.append(ev["message"].toObject());
                emit eventReady(ev);

            } else if (type == "tool_request") {
                bool autoApproved =
                    m_toolConfirmation == "all" ||
                    (m_toolConfirmation == "safe" && Tools::isReadOnly(ev["name"].toString())) ||
                    yoloThisTurn;
                QJsonObject enriched = ev;
                enriched["auto_approved"] = autoApproved;
                emit eventReady(enriched);

            } else if (type == "tool_result") {
                accMsgs.append(QJsonObject{
                    {"role",         "tool"},
                    {"tool_call_id", ev["tool_call_id"].toString()},
                    {"content",      ev["content"].toString()}
                });
                emit eventReady(ev);

            } else if (type == "final_response") {
                const QString content = ev["content"].toString();
                QJsonObject msg = ev["message"].toObject();
                if (msg.isEmpty() && !content.isEmpty()) {
                    msg = QJsonObject{{"role","assistant"},{"content",content}};
                }
                if (!msg.isEmpty()) accMsgs.append(msg);
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

        LlmClient client;
        client.run(LlmParams{m_baseUrl, m_apiKey, m_model, m_messages, m_toolConfirmation, m_reasoningEffort, m_preserveReasoning},
                   onEvent, onConfirm, isCancelled);

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
        QMutexLocker lk(&m_sudoMutex);
        m_sudoPending = false;
        m_sudoCond.wakeAll();
    }
}

void WebChatWorker::sendConfirmation(bool confirmed, bool yoloTurn) {
    QMutexLocker lk(&m_mutex);
    m_confirmState.status   = confirmed ? 2 : 3;
    m_confirmState.yoloTurn = yoloTurn;
    m_cond.wakeAll();
}

void WebChatWorker::sendSudoPassword(const QString& password) {
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
