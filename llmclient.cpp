#include "llmclient.h"
#include "tools.h"
#include <QJsonDocument>
#include <QNetworkAccessManager>
#include <QNetworkRequest>
#include <QNetworkReply>
#include <QEventLoop>
#include <QUrl>

static QJsonObject usage0() {
    return QJsonObject{
        {"prompt_tokens",     0},
        {"completion_tokens", 0},
        {"total_tokens",      0}
    };
}

static void addUsage(QJsonObject& acc, const QJsonObject& delta) {
    acc["prompt_tokens"]     = acc["prompt_tokens"].toInt()     + delta["prompt_tokens"].toInt();
    acc["completion_tokens"] = acc["completion_tokens"].toInt() + delta["completion_tokens"].toInt();
    acc["total_tokens"]      = acc["total_tokens"].toInt()      + delta["total_tokens"].toInt();
}

static QByteArray syncPost(const QUrl& url, const QByteArray& body,
                            const QString& apiKey) {
    QNetworkAccessManager mgr;
    QNetworkRequest req(url);
    req.setHeader(QNetworkRequest::ContentTypeHeader, "application/json");
    req.setRawHeader("Authorization", ("Bearer " + apiKey).toUtf8());
    req.setRawHeader("api-key",       apiKey.toUtf8());
    // No hard timeout — LLM calls can be slow

    QNetworkReply* reply = mgr.post(req, body);
    QEventLoop loop;
    QObject::connect(reply, &QNetworkReply::finished, &loop, &QEventLoop::quit);
    loop.exec();

    QByteArray data;
    if (reply->error() == QNetworkReply::NoError) {
        data = reply->readAll();
    } else {
        data = QJsonDocument(QJsonObject{
            {"error", QJsonObject{{"message", reply->errorString()}}}
        }).toJson();
    }
    reply->deleteLater();
    return data;
}

void LlmClient::run(const LlmParams& params,
                    EventFn   onEvent,
                    ConfirmFn onConfirm,
                    CancelFn  isCancelled) {

    enum class TcMode { All, Safe, None };
    TcMode tcMode = TcMode::None;
    if (params.toolConfirmation == "all")  tcMode = TcMode::All;
    if (params.toolConfirmation == "safe") tcMode = TcMode::Safe;

    QString baseUrl = params.baseUrl;
    while (baseUrl.endsWith('/')) baseUrl.chop(1);
    QUrl url(baseUrl + "/chat/completions");

    QJsonArray current = params.messages;
    QJsonObject accUsage = usage0();
    bool yoloThisTurn = false;

    for (;;) {
        if (isCancelled()) return;

        QJsonObject payload{
            {"model",       params.model},
            {"messages",    current},
            {"tools",       Tools::toolDefinitions()},
            {"tool_choice", "auto"},
        };

        QByteArray respData = syncPost(url, QJsonDocument(payload).toJson(QJsonDocument::Compact),
                                       params.apiKey);
        if (isCancelled()) return;

        QJsonObject body = QJsonDocument::fromJson(respData).object();

        // Check for API-level error
        if (body.contains("error")) {
            QString msg = body["error"].toObject()["message"].toString(respData);
            onEvent(QJsonObject{
                {"type",    "final_response"},
                {"content", "API error: " + msg},
                {"usage",   accUsage}
            });
            return;
        }

        QJsonArray choices = body["choices"].toArray();
        if (choices.isEmpty()) {
            onEvent(QJsonObject{
                {"type",    "final_response"},
                {"content", "No choices in API response."},
                {"usage",   accUsage}
            });
            return;
        }

        // Accumulate usage
        if (body.contains("usage")) {
            addUsage(accUsage, body["usage"].toObject());
        }

        QJsonObject choice = choices[0].toObject();
        QJsonObject msg    = choice["message"].toObject();
        QString     content = msg["content"].toString();
        QJsonArray  toolCalls = msg["tool_calls"].toArray();

        if (!toolCalls.isEmpty()) {
            // Build assistant message for history
            QJsonObject asstMsg;
            asstMsg["role"]       = "assistant";
            asstMsg["content"]    = content;
            asstMsg["tool_calls"] = toolCalls;

            onEvent(QJsonObject{
                {"type",    "assistant_tool_calls"},
                {"message", asstMsg}
            });
            current.append(asstMsg);

            yoloThisTurn = false;

            for (const QJsonValue& tcv : toolCalls) {
                if (isCancelled()) return;

                QJsonObject tc    = tcv.toObject();
                QString     tcId  = tc["id"].toString();
                QJsonObject fn    = tc["function"].toObject();
                QString     name  = fn["name"].toString();
                QString     argsStr = fn["arguments"].toString();
                QJsonObject argsObj = QJsonDocument::fromJson(argsStr.toUtf8()).object();

                bool skipConfirm =
                    tcMode == TcMode::All ||
                    (tcMode == TcMode::Safe && Tools::isReadOnly(name)) ||
                    yoloThisTurn;

                onEvent(QJsonObject{
                    {"type",         "tool_request"},
                    {"name",         name},
                    {"args",         argsObj},
                    {"tool_call_id", tcId}
                });

                bool confirmed = true;
                bool yolo      = false;

                if (!skipConfirm) {
                    auto [c, y] = onConfirm();
                    confirmed = c;
                    yolo      = y;
                    if (yolo) yoloThisTurn = true;
                }

                QString result;
                bool    declined = false;

                if (confirmed) {
                    result = Tools::execute(name, argsObj);
                } else {
                    result   = "Tool execution was declined by user.";
                    declined = true;
                }

                if (isCancelled()) return;

                QJsonObject toolMsg;
                toolMsg["role"]         = "tool";
                toolMsg["tool_call_id"] = tcId;
                toolMsg["content"]      = result;
                current.append(toolMsg);

                onEvent(QJsonObject{
                    {"type",         "tool_result"},
                    {"tool_call_id", tcId},
                    {"name",         name},
                    {"args",         argsObj},
                    {"content",      result},
                    {"declined",     declined}
                });
            }
            // Loop: send tool results back to LLM
            continue;
        }

        // No tool calls — final text response
        onEvent(QJsonObject{
            {"type",    "final_response"},
            {"content", content},
            {"usage",   accUsage}
        });
        return;
    }
}
