#pragma once
#include <QObject>
#include <QMutex>
#include <QWaitCondition>
#include <QJsonArray>
#include <QString>
#include <atomic>
#include <limits.h>

#include "tools.h"

class QThread;

class ChatWorker : public QObject {
    Q_OBJECT
public:
    explicit ChatWorker(QObject* parent = nullptr);

    void start(const QString& baseUrl, const QString& apiKey,
               const QString& model, const QJsonArray& messages,
               const QString& toolConfirmation, const QString& reasoningEffort,
               bool preserveReasoning);

    void cancel();
    void sendConfirmation(bool confirmed, bool yoloTurn);

    bool isSudoPending() const;
    void sendSudoPassword(const QString& password);
    void cancelSudo();

    // Thread lifetime — used by closeEvent to wait for a run to stop before
    // the worker (parented to MainWindow) is destroyed, avoiding a UAF.
    bool isRunning() const;
    bool wait(unsigned long ms = ULONG_MAX);

signals:
    void eventReceived(const QString& eventJson);
    void finished();
    void errorOccurred(const QString& message);

private:
    struct ConfirmState {
        int  status   = 0;  // 0=idle, 2=confirmed, 3=declined
        bool yoloTurn = false;
    };

    ConfirmState    m_confirmState;
    QMutex          m_mutex;
    QWaitCondition  m_cond;
    std::atomic<bool> m_cancelled = false;

    // ── Sudo state ────────────────────────────────────────────────
    mutable QMutex  m_sudoMutex;
    QWaitCondition  m_sudoCond;
    bool            m_sudoPending   = false;
    QString         m_sudoPassword;

    QString    m_baseUrl, m_apiKey, m_model, m_toolConfirmation, m_reasoningEffort;
    bool       m_preserveReasoning = false;
    QJsonArray m_messages;

    QThread*   m_thread = nullptr;

    // Per-run tool state so concurrent tabs don't share a sudo provider
    // or kill each other's subprocesses.
    Tools::ToolContext m_toolContext;
};
