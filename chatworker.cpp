#include "chatworker.h"
#include "llmclient.h"
#include <QThread>
#include <QJsonDocument>

ChatWorker::ChatWorker(QObject* parent) : QObject(parent) {}

void ChatWorker::start(const QString& baseUrl, const QString& apiKey,
                       const QString& model, const QJsonArray& messages,
                       const QString& toolConfirmation) {
    m_baseUrl           = baseUrl;
    m_apiKey            = apiKey;
    m_model             = model;
    m_messages          = messages;
    m_toolConfirmation  = toolConfirmation;
    m_cancelled         = false;

    {
        QMutexLocker lock(&m_mutex);
        m_confirmState = ConfirmState{};
    }

    auto* thread = QThread::create([this] {
        LlmParams params{m_baseUrl, m_apiKey, m_model, m_messages, m_toolConfirmation};

        LlmClient::EventFn onEvent = [this](const QJsonObject& ev) {
            if (m_cancelled) return;
            emit eventReceived(QJsonDocument(ev).toJson(QJsonDocument::Compact));
        };

        LlmClient::CancelFn isCancelled = [this]() -> bool {
            return m_cancelled;
        };

        LlmClient::ConfirmFn onConfirm = [this]() -> std::pair<bool,bool> {
            QMutexLocker lock(&m_mutex);
            // Wait until status is set by the main thread
            while (m_confirmState.status == 0 && !m_cancelled) {
                m_cond.wait(&m_mutex);
            }
            if (m_cancelled) return {false, false};
            bool confirmed = (m_confirmState.status == 2);
            bool yolo      = m_confirmState.yoloTurn;
            m_confirmState.status = 0;
            return {confirmed, yolo};
        };

        LlmClient client;
        client.run(params, onEvent, onConfirm, isCancelled);

        if (!m_cancelled)
            emit finished();
        else
            emit finished();
    });

    connect(thread, &QThread::finished, thread, &QObject::deleteLater);
    thread->start();
}

void ChatWorker::cancel() {
    m_cancelled = true;
    QMutexLocker lock(&m_mutex);
    m_confirmState.status = 3;  // declined
    m_cond.wakeAll();
}

void ChatWorker::sendConfirmation(bool confirmed, bool yoloTurn) {
    QMutexLocker lock(&m_mutex);
    m_confirmState.status   = confirmed ? 2 : 3;
    m_confirmState.yoloTurn = yoloTurn;
    m_cond.wakeAll();
}
