#pragma once
#include <QObject>
#include <QTimer>
#include <QMutex>
#include <QWaitCondition>
#include <QJsonArray>
#include <QJsonObject>
#include <QString>
#include <QStringList>
#include <atomic>
#include "../context_recovery.h"

class WebChatWorker : public QObject {
    Q_OBJECT
public:
    explicit WebChatWorker(QObject* parent = nullptr);

    void start(const QString& baseUrl, const QString& apiKey,
               const QString& model, const QJsonArray& messages,
               const QString& toolConfirmation, const QString& reasoningEffort = QString(),
               bool preserveReasoning = false, int llmTimeout = 300);
    ContextRecovery::Options recoveryOptions;
    void cancel();
    void sendConfirmation(bool confirmed, bool yoloTurn);
    // Answers to a pending ask_user_question.  An empty list is a cancel.
    void sendAnswers(const QStringList& answers);
    void sendSudoPassword(const QString& password);
    void cancelSudo();

    /// Attached SSE consumers, reported by the server on each broadcast.  The
    /// prompt lease keys on this: a confirm/question/sudo prompt waits
    /// indefinitely while someone is watching, and is bounded when nobody is.
    void setViewers(int viewers);

    /// Prompt lease for a run nobody is watching: announced each minute, and the
    /// run is cancelled with a notice after the lease.  A prompt never expires
    /// out from under an attached browser (v1.11.4 removed the silent expiry).
    void setPendingPrompt(bool pending);

signals:
    void eventReady(const QJsonObject& event);
    // *host* is the remote machine the command runs on; empty = local.
    void sudoRequired(const QString& host);
    // Emitted whenever a message is added to the turn, so the server can
    // persist mid-run: a crash must not take the turn's tool calls with it.
    void progress(const QJsonArray& newMessages);
    void finished(const QJsonArray& newMessages);

private:
    struct ConfirmState { int status = 0; bool yoloTurn = false; };

    /// Minutes a prompt may stay unanswered with no browser attached.
    static const int PROMPT_LEASE_MINUTES = 30;

    ConfirmState   m_confirmState;
    QMutex         m_mutex;
    QWaitCondition m_cond;
    std::atomic<bool> m_cancelled = false;

    mutable QMutex m_questionMutex;
    QWaitCondition m_questionCond;
    bool           m_questionPending = false;
    QStringList    m_questionAnswers;

    mutable QMutex m_sudoMutex;
    QWaitCondition m_sudoCond;
    bool           m_sudoPending = false;
    QString        m_sudoPassword;

    QString    m_baseUrl, m_apiKey, m_model, m_toolConfirmation, m_reasoningEffort;
    bool       m_preserveReasoning = false;
    QJsonArray m_messages;

    std::atomic<int>  m_viewers{0};
    std::atomic<bool> m_pendingPrompt{false};
    std::atomic<int>  m_promptIdleMinutes{0};
    QTimer* m_leaseTimer = nullptr;
};
