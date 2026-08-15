#pragma once
#include <QObject>
#include <QTcpServer>
#include <QTcpSocket>
#include <QHash>
#include <QSet>
#include <QList>
#include <QJsonObject>
#include <QJsonArray>
#include <QByteArray>
#include <QString>
#include <QStringList>
#include "webchatworker.h"

struct HttpRequest {
    QString method, path;
    QHash<QString, QString> headers;
    QByteArray body;
    QHash<QString, QString> form;
};

class WebServer : public QObject {
    Q_OBJECT
public:
    explicit WebServer(const QString& host, quint16 port, QObject* parent = nullptr);
    bool start();

    /// Hostnames this server may legitimately be reached as, from
    /// --trusted-host. Needed only for a reverse proxy on a loopback bind.
    void setTrustedHosts(const QStringList& hosts);
    quint16 port() const { return m_server->serverPort(); }

#ifdef PENGY_UNIT_TEST
    // Test-only hooks keep replay policy independently verifiable without a
    // live LLM worker or timing-sensitive socket choreography.
    void testPushSse(const QString& chatId, const QJsonObject& event) { pushSse(chatId, event); }
    QByteArray testReplay(const QString& chatId, int after) const;
    void testMarkCompleted(const QString& chatId) { m_workerDone[chatId] = true; }
    void testCleanupCompleted(const QString& chatId);
#endif

private slots:
    void onNewConnection();
    void onReadyRead();
    void onSocketDisconnected();

private:
    QString       m_host;
    quint16       m_port;
    QSet<QString> m_trustedHosts;
    QTcpServer*   m_server;

    QHash<QTcpSocket*, QByteArray>        m_buffers;
    QSet<QTcpSocket*>                     m_sseSockets;
    QSet<QTcpSocket*>                     m_sseRetrySent;   // tracks sockets that got retry:1000
    QHash<QString, QList<QTcpSocket*>>    m_sse;
    QHash<QString, QList<QJsonObject>>    m_eventQueue;     // append-only event log per chat
    QHash<QString, int>                   m_eventBase;      // ID represented by log index zero
    QHash<QString, bool>                  m_workerDone;     // true once a worker has finished
    QHash<QString, qint64>                m_completedAt;    // completion time for bounded replay retention
    QHash<QString, WebChatWorker*>        m_workers;
    QHash<QString, QString>               m_pending;
    QHash<QString, int>                   m_persistedCount; // turn messages already written

    // Append whatever part of the running turn is not on disk yet and save.
    // Re-reads the chat each call, so a rename landing mid-run survives.
    void persistTurnProgress(const QString& chatId, const QJsonArray& turnMsgs,
                             bool repairDangling);

    // Returns false (and has already replied 403) if the request must be
    // rejected as cross-origin or rebound-DNS.
    bool checkRequestOrigin(const HttpRequest& req, QTcpSocket* socket);

    void handleRequest(const HttpRequest& req, QTcpSocket* socket);

    void routeRoot(QTcpSocket* socket);
    void routeChatNew(QTcpSocket* socket);
    void routeChatView(const QString& chatId, QTcpSocket* socket);
    void routeChatSend(const QString& chatId, const HttpRequest& req, QTcpSocket* socket);
    void routeChatStream(const QString& chatId, const HttpRequest& req, QTcpSocket* socket);
    void routeChatConfirm(const QString& chatId, const HttpRequest& req, QTcpSocket* socket);
    void routeChatSudo(const QString& chatId, const HttpRequest& req, QTcpSocket* socket);
    void routeChatStop(const QString& chatId, QTcpSocket* socket);
    void routeChatDelete(const QString& chatId, QTcpSocket* socket);
    void routeChatExport(const QString& chatId, QTcpSocket* socket);
    void routeChatRename(const QString& chatId, const HttpRequest& req, QTcpSocket* socket);
    void routeChatCommand(const QString& chatId, const HttpRequest& req, QTcpSocket* socket);
    void routeModels(QTcpSocket* socket);
    void routeFile(const HttpRequest& req, QTcpSocket* socket);
    void routeSettings(const HttpRequest& req, QTcpSocket* socket);

    void sendResponse(QTcpSocket* socket, int status,
                      const QString& contentType, const QByteArray& body);
    void sendJson(QTcpSocket* socket, int status, const QJsonObject& obj);
    void sendRedirect(QTcpSocket* socket, const QString& location);
    void pushSse(const QString& chatId, const QJsonObject& event);
    static QByteArray formatSseEvent(int id, const QJsonObject& event);
    void scheduleCompletedLogCleanup(const QString& chatId, qint64 completedAt);
    QByteArray replayEvents(const QString& chatId, int after, bool hasCursor) const;

    QByteArray renderChatPage(const QString& chatId);
    QByteArray renderSettingsPage();

    static HttpRequest parseRequest(const QByteArray& data);
    static QHash<QString, QString> parseForm(const QByteArray& body);
    static QString urlDecode(const QByteArray& s);
    static QJsonObject bodyJson(const HttpRequest& req);
};
