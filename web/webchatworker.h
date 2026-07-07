#pragma once
#include <QObject>
#include <QMutex>
#include <QWaitCondition>
#include <QJsonArray>
#include <QJsonObject>
#include <QString>

class WebChatWorker : public QObject {
    Q_OBJECT
public:
    explicit WebChatWorker(QObject* parent = nullptr);

    void start(const QString& baseUrl, const QString& apiKey,
               const QString& model, const QJsonArray& messages,
               const QString& toolConfirmation, const QString& reasoningEffort = QString(),
               bool preserveReasoning = false);
    void cancel();
    void sendConfirmation(bool confirmed, bool yoloTurn);
    void sendSudoPassword(const QString& password);
    void cancelSudo();

signals:
    void eventReady(const QJsonObject& event);
    void sudoRequired();
    void finished(const QJsonArray& newMessages);

private:
    struct ConfirmState { int status = 0; bool yoloTurn = false; };

    ConfirmState   m_confirmState;
    QMutex         m_mutex;
    QWaitCondition m_cond;
    bool           m_cancelled = false;

    mutable QMutex m_sudoMutex;
    QWaitCondition m_sudoCond;
    bool           m_sudoPending = false;
    QString        m_sudoPassword;

    QString    m_baseUrl, m_apiKey, m_model, m_toolConfirmation, m_reasoningEffort;
    bool       m_preserveReasoning = false;
    QJsonArray m_messages;
};
