#include "webserver.h"
#include "../config.h"
#include "../chatmanager.h"
#include "../tools.h"
#include <QTcpSocket>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonArray>
#include <QFile>
#include <QUrl>
#include <QTextStream>

WebServer::WebServer(quint16 port, QObject* parent)
    : QObject(parent), m_port(port),
      m_server(new QTcpServer(this))
{
    connect(m_server, &QTcpServer::newConnection, this, &WebServer::onNewConnection);
}

bool WebServer::start() {
    return m_server->listen(QHostAddress::LocalHost, m_port);
}

quint16 WebServer::port() const { return m_server->serverPort(); }

// ── Connection handling ──────────────────────────────────────────────

void WebServer::onNewConnection() {
    while (m_server->hasPendingConnections()) {
        auto* socket = m_server->nextPendingConnection();
        m_buffers[socket] = QByteArray();
        connect(socket, &QTcpSocket::readyRead,       this, &WebServer::onReadyRead);
        connect(socket, &QTcpSocket::disconnected,    this, &WebServer::onSocketDisconnected);
    }
}

void WebServer::onReadyRead() {
    auto* socket = qobject_cast<QTcpSocket*>(sender());
    if (!socket || m_sseSockets.contains(socket)) return;

    m_buffers[socket] += socket->readAll();
    QByteArray& buf = m_buffers[socket];

    int headerEnd = buf.indexOf("\r\n\r\n");
    if (headerEnd < 0) return;

    // Find content-length
    QByteArray headers = buf.left(headerEnd);
    int contentLength = 0;
    for (const QByteArray& line : headers.split('\n')) {
        if (line.toLower().trimmed().startsWith("content-length:"))
            contentLength = line.mid(line.indexOf(':') + 1).trimmed().toInt();
    }

    int total = headerEnd + 4 + contentLength;
    if (buf.size() < total) return;

    HttpRequest req = parseRequest(buf.left(total));
    buf.remove(0, total);
    handleRequest(req, socket);
}

void WebServer::onSocketDisconnected() {
    auto* socket = qobject_cast<QTcpSocket*>(sender());
    if (!socket) return;
    m_buffers.remove(socket);
    m_sseSockets.remove(socket);
    for (auto it = m_sse.begin(); it != m_sse.end(); ++it)
        it->removeAll(socket);
    socket->deleteLater();
}

// ── Routing ──────────────────────────────────────────────────────────

void WebServer::handleRequest(const HttpRequest& req, QTcpSocket* socket) {
    QString path = req.path;
    int q = path.indexOf('?');
    if (q >= 0) path = path.left(q);

    const QStringList parts = path.split('/', Qt::SkipEmptyParts);

    if (parts.isEmpty()) {
        routeRoot(socket);
    } else if (parts[0] == "settings") {
        routeSettings(req, socket);
    } else if (parts[0] == "chat") {
        if (parts.size() == 1) {
            routeRoot(socket);
        } else if (parts[1] == "new" && req.method == "POST") {
            routeChatNew(socket);
        } else if (parts.size() == 2) {
            routeChatView(parts[1], socket);
        } else if (parts.size() == 3) {
            const QString& id  = parts[1];
            const QString& act = parts[2];
            if      (act == "send"    && req.method == "POST") routeChatSend(id, req, socket);
            else if (act == "stream"  && req.method == "GET")  routeChatStream(id, socket);
            else if (act == "confirm" && req.method == "POST") routeChatConfirm(id, req, socket);
            else if (act == "sudo"    && req.method == "POST") routeChatSudo(id, req, socket);
            else if (act == "stop"    && req.method == "POST") routeChatStop(id, socket);
            else if (act == "delete"  && req.method == "POST") routeChatDelete(id, socket);
            else sendJson(socket, 404, {{"error","not found"}});
        } else {
            sendJson(socket, 404, {{"error","not found"}});
        }
    } else {
        sendJson(socket, 404, {{"error","not found"}});
    }
}

// ── Route handlers ───────────────────────────────────────────────────

void WebServer::routeRoot(QTcpSocket* socket) {
    QJsonArray chats = chatsLoad();
    if (!chats.isEmpty()) {
        sendRedirect(socket, "/chat/" + chats.first().toObject()["id"].toString());
    } else {
        QJsonObject chat = chatCreate("New Chat");
        sendRedirect(socket, "/chat/" + chat["id"].toString());
    }
}

void WebServer::routeChatNew(QTcpSocket* socket) {
    QJsonObject chat = chatCreate("New Chat");
    sendRedirect(socket, "/chat/" + chat["id"].toString());
}

void WebServer::routeChatView(const QString& chatId, QTcpSocket* socket) {
    QJsonObject chat = chatGet(chatId);
    if (chat.isEmpty()) {
        sendRedirect(socket, "/");
        return;
    }
    sendResponse(socket, 200, "text/html; charset=utf-8", renderChatPage(chatId));
}

void WebServer::routeChatSend(const QString& chatId,
                               const HttpRequest& req, QTcpSocket* socket) {
    QJsonObject body = bodyJson(req);
    QString content = body["content"].toString().trimmed();
    if (content.isEmpty()) {
        sendJson(socket, 400, {{"error","empty message"}});
        return;
    }

    QJsonObject chat = chatGet(chatId);
    if (chat.isEmpty()) {
        sendJson(socket, 404, {{"error","chat not found"}});
        return;
    }

    // Cancel any running worker for this chat
    if (auto* w = m_workers.value(chatId)) {
        w->cancel();
        m_workers.remove(chatId);
    }

    Config cfg = configLoad();
    Tools::setUserAgent(cfg.userAgent);
    Tools::setTimeout(cfg.toolTimeout);

    QJsonArray hist = chat["messages"].toArray();
    hist.append(QJsonObject{{"role","user"},{"content",content}});
    hist = cleanDanglingToolCalls(hist);
    if (cfg.contextKeepTurns > 0)
        hist = elideOldToolResults(hist, cfg.contextKeepTurns);

    QJsonArray sendMsgs;
    if (!cfg.systemMessage.isEmpty())
        sendMsgs.append(QJsonObject{
            {"role","system"},
            {"content", configRenderSystemMessage(cfg.systemMessage)}
        });
    for (const QJsonValue& v : hist) sendMsgs.append(v);

    m_pending[chatId] = content;

    auto* worker = new WebChatWorker(this);
    m_workers[chatId] = worker;

    // Push most events straight to SSE; enrich final_response with updated title
    connect(worker, &WebChatWorker::eventReady, this,
            [this, chatId](const QJsonObject& ev) {
        if (ev["type"].toString() == "final_response") {
            QString userInput = m_pending.value(chatId);
            QJsonObject curChat = chatGet(chatId);
            QString title = curChat["title"].toString();
            if (title == "New Chat" && !userInput.isEmpty())
                title = userInput.left(60).replace('\n', ' ');
            QJsonObject enriched = ev;
            enriched["chat_title"] = title;
            pushSse(chatId, enriched);
        } else {
            pushSse(chatId, ev);
        }
    });

    connect(worker, &WebChatWorker::sudoRequired, this,
            [this, chatId]() {
        pushSse(chatId, QJsonObject{{"type","sudo_request"}});
    });

    connect(worker, &WebChatWorker::finished, this,
            [this, chatId, worker](const QJsonArray& newMsgs) {
        QString userInput = m_pending.take(chatId);
        QJsonObject chat = chatGet(chatId);
        QJsonArray msgs = chat["messages"].toArray();
        msgs.append(QJsonObject{{"role","user"},{"content",userInput}});
        for (const QJsonValue& v : newMsgs) msgs.append(v);
        if (chat["title"].toString() == "New Chat" && !userInput.isEmpty())
            chat["title"] = userInput.left(60).replace('\n', ' ');
        chat["messages"] = msgs;
        chatSave(chat);
        m_workers.remove(chatId);
        worker->deleteLater();
    });

    worker->start(cfg.baseUrl, cfg.apiKey, cfg.model, sendMsgs, cfg.toolConfirmation, cfg.reasoningEffort, cfg.preserveReasoning);
    sendJson(socket, 200, {{"status","started"}});
}

void WebServer::routeChatStream(const QString& chatId, QTcpSocket* socket) {
    QByteArray headers =
        "HTTP/1.1 200 OK\r\n"
        "Content-Type: text/event-stream\r\n"
        "Cache-Control: no-cache\r\n"
        "Connection: keep-alive\r\n"
        "X-Accel-Buffering: no\r\n"
        "\r\n";
    socket->write(headers);
    socket->flush();

    m_sseSockets.insert(socket);
    m_sse[chatId].append(socket);

    // Flush any queued events (in case worker started before browser connected)
    for (const QJsonObject& ev : m_eventQueue.take(chatId)) {
        QByteArray data = "data: " +
            QJsonDocument(ev).toJson(QJsonDocument::Compact) + "\n\n";
        socket->write(data);
    }
    socket->flush();
}

void WebServer::routeChatConfirm(const QString& chatId,
                                  const HttpRequest& req, QTcpSocket* socket) {
    QJsonObject body = bodyJson(req);
    bool confirmed = body["confirmed"].toBool(true);
    bool yolo      = body["yolo_turn"].toBool(false);
    if (auto* w = m_workers.value(chatId))
        w->sendConfirmation(confirmed, yolo);
    sendJson(socket, 200, {{"status","ok"}});
}

void WebServer::routeChatSudo(const QString& chatId,
                               const HttpRequest& req, QTcpSocket* socket) {
    QJsonObject body = bodyJson(req);
    auto* w = m_workers.value(chatId);
    if (!w) { sendJson(socket, 200, {{"status","ok"}}); return; }
    if (body["cancel"].toBool())
        w->cancelSudo();
    else
        w->sendSudoPassword(body["password"].toString());
    sendJson(socket, 200, {{"status","ok"}});
}

void WebServer::routeChatStop(const QString& chatId, QTcpSocket* socket) {
    if (auto* w = m_workers.value(chatId)) {
        w->cancel();
        m_workers.remove(chatId);
        m_pending.remove(chatId);
    }
    sendJson(socket, 200, {{"status","stopped"}});
}

void WebServer::routeChatDelete(const QString& chatId, QTcpSocket* socket) {
    if (auto* w = m_workers.value(chatId)) {
        w->cancel();
        m_workers.remove(chatId);
    }
    chatDelete(chatId);
    sendRedirect(socket, "/");
}

void WebServer::routeSettings(const HttpRequest& req, QTcpSocket* socket) {
    if (req.method == "POST") {
        const QHash<QString, QString>& f = req.form;
        Config cfg = configLoad();
        if (f.contains("base_url"))          cfg.baseUrl          = f["base_url"];
        if (f.contains("api_key") && !f["api_key"].isEmpty())
                                             cfg.apiKey           = f["api_key"];
        if (f.contains("model"))             cfg.model            = f["model"];
        if (f.contains("system_message"))    cfg.systemMessage    = f["system_message"];
        if (f.contains("tool_confirmation")) cfg.toolConfirmation = f["tool_confirmation"];
        if (f.contains("reasoning_effort"))   cfg.reasoningEffort  = f["reasoning_effort"];
        cfg.preserveReasoning = f.contains("preserve_reasoning");
        if (f.contains("tool_timeout"))      cfg.toolTimeout      = f["tool_timeout"].toInt();
        if (f.contains("context_keep_turns"))cfg.contextKeepTurns = f["context_keep_turns"].toInt();
        configSave(cfg);
        sendRedirect(socket, "/settings");
    } else {
        sendResponse(socket, 200, "text/html; charset=utf-8", renderSettingsPage());
    }
}

// ── SSE push ─────────────────────────────────────────────────────────

void WebServer::pushSse(const QString& chatId, const QJsonObject& event) {
    QByteArray data = "data: " +
        QJsonDocument(event).toJson(QJsonDocument::Compact) + "\n\n";

    const auto& clients = m_sse.value(chatId);
    if (clients.isEmpty()) {
        m_eventQueue[chatId].append(event);
        return;
    }
    for (auto* sock : clients) {
        if (sock->isOpen()) {
            sock->write(data);
            sock->flush();
        }
    }
}

// ── Template rendering ────────────────────────────────────────────────

static QString safeJs(const QByteArray& json) {
    // Prevent </script> from closing the script tag
    return QString::fromUtf8(json).replace("</", "<\\/");
}

QByteArray WebServer::renderChatPage(const QString& chatId) {
    QFile f(":/web/templates/chat.html");
    if (!f.open(QIODevice::ReadOnly)) return "<h1>Template missing</h1>";
    QString html = QString::fromUtf8(f.readAll());

    Config cfg = configLoad();
    QJsonObject chat = chatGet(chatId);
    QJsonArray chats = chatsLoad();

    html.replace("{{CHAT_ID}}",          chatId);
    html.replace("{{CHAT_TITLE}}",       chat["title"].toString().toHtmlEscaped());
    html.replace("{{MODEL}}",            cfg.model.toHtmlEscaped());
    html.replace("{{TOOL_CONFIRMATION}}",cfg.toolConfirmation.toHtmlEscaped());
    html.replace("{{MESSAGES_JSON}}",
        safeJs(QJsonDocument(chat["messages"].toArray()).toJson(QJsonDocument::Compact)));
    html.replace("{{CHATS_JSON}}",
        safeJs(QJsonDocument(chats).toJson(QJsonDocument::Compact)));

    return html.toUtf8();
}

QByteArray WebServer::renderSettingsPage() {
    QFile f(":/web/templates/settings.html");
    if (!f.open(QIODevice::ReadOnly)) return "<h1>Template missing</h1>";
    QString html = QString::fromUtf8(f.readAll());

    Config cfg = configLoad();
    html.replace("{{BASE_URL}}",         cfg.baseUrl.toHtmlEscaped());
    html.replace("{{API_KEY_STATUS}}",   cfg.apiKey.isEmpty() ? "not set" : "set");
    html.replace("{{MODEL}}",            cfg.model.toHtmlEscaped());
    html.replace("{{SYSTEM_MESSAGE}}",   cfg.systemMessage.toHtmlEscaped());
    html.replace("{{TOOL_TIMEOUT}}",     QString::number(cfg.toolTimeout));
    html.replace("{{CONTEXT_KEEP_TURNS}}",QString::number(cfg.contextKeepTurns));
    html.replace("{{TC_NONE}}",  cfg.toolConfirmation == "none"  ? "selected" : "");
    html.replace("{{TC_SAFE}}",  cfg.toolConfirmation == "safe"  ? "selected" : "");
    html.replace("{{TC_ALL}}",   cfg.toolConfirmation == "all"   ? "selected" : "");
    html.replace("{{REASONING_DEFAULT}}", cfg.reasoningEffort.isEmpty() ? "selected" : "");
    html.replace("{{REASONING_NONE}}",    cfg.reasoningEffort == "none" ? "selected" : "");
    html.replace("{{REASONING_MINIMAL}}", cfg.reasoningEffort == "minimal" ? "selected" : "");
    html.replace("{{REASONING_LOW}}",     cfg.reasoningEffort == "low" ? "selected" : "");
    html.replace("{{REASONING_MEDIUM}}",  cfg.reasoningEffort == "medium" ? "selected" : "");
    html.replace("{{REASONING_HIGH}}",    cfg.reasoningEffort == "high" ? "selected" : "");
    html.replace("{{REASONING_XHIGH}}",   cfg.reasoningEffort == "xhigh" ? "selected" : "");
    html.replace("{{REASONING_MAX}}",     cfg.reasoningEffort == "max" ? "selected" : "");
    html.replace("{{PRESERVE_REASONING}}", cfg.preserveReasoning ? "checked" : "");

    return html.toUtf8();
}

// ── HTTP helpers ──────────────────────────────────────────────────────

void WebServer::sendResponse(QTcpSocket* socket, int status,
                              const QString& contentType, const QByteArray& body) {
    static const auto statusText = [](int s) -> const char* {
        switch (s) {
            case 200: return "OK";
            case 302: return "Found";
            case 400: return "Bad Request";
            case 404: return "Not Found";
            default:  return "Internal Server Error";
        }
    };
    QByteArray resp = QString(
        "HTTP/1.1 %1 %2\r\n"
        "Content-Type: %3\r\n"
        "Content-Length: %4\r\n"
        "Connection: close\r\n"
        "\r\n"
    ).arg(status).arg(statusText(status))
     .arg(contentType).arg(body.size())
     .toUtf8();
    socket->write(resp);
    socket->write(body);
    socket->flush();
    socket->disconnectFromHost();
}

void WebServer::sendJson(QTcpSocket* socket, int status, const QJsonObject& obj) {
    sendResponse(socket, status, "application/json",
                 QJsonDocument(obj).toJson(QJsonDocument::Compact));
}

void WebServer::sendRedirect(QTcpSocket* socket, const QString& location) {
    QByteArray resp = QString(
        "HTTP/1.1 302 Found\r\n"
        "Location: %1\r\n"
        "Content-Length: 0\r\n"
        "Connection: close\r\n"
        "\r\n"
    ).arg(location).toUtf8();
    socket->write(resp);
    socket->flush();
    socket->disconnectFromHost();
}

// ── Request parsing ───────────────────────────────────────────────────

HttpRequest WebServer::parseRequest(const QByteArray& data) {
    HttpRequest req;
    int headerEnd = data.indexOf("\r\n\r\n");
    QByteArray headerPart = headerEnd >= 0 ? data.left(headerEnd) : data;
    req.body = headerEnd >= 0 ? data.mid(headerEnd + 4) : QByteArray();

    const QList<QByteArray> lines = headerPart.split('\n');
    if (lines.isEmpty()) return req;

    const QList<QByteArray> rl = lines[0].trimmed().split(' ');
    if (rl.size() >= 2) {
        req.method = QString::fromUtf8(rl[0]).toUpper();
        req.path   = QString::fromUtf8(rl[1]);
    }

    for (int i = 1; i < lines.size(); i++) {
        const QByteArray line = lines[i].trimmed();
        int colon = line.indexOf(':');
        if (colon < 0) continue;
        req.headers[QString::fromUtf8(line.left(colon)).trimmed().toLower()] =
            QString::fromUtf8(line.mid(colon + 1)).trimmed();
    }

    const QString ct = req.headers.value("content-type");
    if (ct.startsWith("application/x-www-form-urlencoded"))
        req.form = parseForm(req.body);

    return req;
}

QHash<QString, QString> WebServer::parseForm(const QByteArray& body) {
    QHash<QString, QString> result;
    for (const QByteArray& pair : body.split('&')) {
        int eq = pair.indexOf('=');
        if (eq < 0) continue;
        result[urlDecode(pair.left(eq))] = urlDecode(pair.mid(eq + 1));
    }
    return result;
}

QString WebServer::urlDecode(const QByteArray& s) {
    QByteArray tmp = s;
    tmp.replace('+', ' ');
    return QUrl::fromPercentEncoding(tmp);
}

QJsonObject WebServer::bodyJson(const HttpRequest& req) {
    if (req.body.isEmpty()) return {};
    return QJsonDocument::fromJson(req.body).object();
}
