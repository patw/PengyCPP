#pragma once
#include <QObject>
#include <QMutex>
#include <QWaitCondition>
#include <QJsonArray>
#include <QString>

class ChatWorker : public QObject {
    Q_OBJECT
public:
    explicit ChatWorker(QObject* parent = nullptr);

    void start(const QString& baseUrl, const QString& apiKey,
               const QString& model, const QJsonArray& messages,
               const QString& toolConfirmation);

    void cancel();
    void sendConfirmation(bool confirmed, bool yoloTurn);

    bool isSudoPending() const;
    void sendSudoPassword(const QString& password);
    void cancelSudo();

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
    bool            m_cancelled = false;

    // ── Sudo state ────────────────────────────────────────────────
    mutable QMutex  m_sudoMutex;
    QWaitCondition  m_sudoCond;
    bool            m_sudoPending   = false;
    QString         m_sudoPassword;

    QString    m_baseUrl, m_apiKey, m_model, m_toolConfirmation;
    QJsonArray m_messages;
};
