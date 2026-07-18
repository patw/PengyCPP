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
    quint16 port() const { return m_server->serverPort(); }

private slots:
    void onNewConnection();
    void onReadyRead();
    void onSocketDisconnected();

private:
    QString     m_host;
    quint16     m_port;
    QTcpServer* m_server;

    QHash<QTcpSocket*, QByteArray>        m_buffers;
    QSet<QTcpSocket*>                     m_sseSockets;
    QHash<QString, QList<QTcpSocket*>>    m_sse;
    QHash<QString, QList<QJsonObject>>    m_eventQueue;
    QHash<QString, WebChatWorker*>        m_workers;
    QHash<QString, QString>               m_pending;

    void handleRequest(const HttpRequest& req, QTcpSocket* socket);

    void routeRoot(QTcpSocket* socket);
    void routeChatNew(QTcpSocket* socket);
    void routeChatView(const QString& chatId, QTcpSocket* socket);
    void routeChatSend(const QString& chatId, const HttpRequest& req, QTcpSocket* socket);
    void routeChatStream(const QString& chatId, QTcpSocket* socket);
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

    QByteArray renderChatPage(const QString& chatId);
    QByteArray renderSettingsPage();

    static HttpRequest parseRequest(const QByteArray& data);
    static QHash<QString, QString> parseForm(const QByteArray& body);
    static QString urlDecode(const QByteArray& s);
    static QJsonObject bodyJson(const HttpRequest& req);
};
