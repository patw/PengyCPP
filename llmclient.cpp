#include "llmclient.h"
#include "tools.h"
#include <QJsonDocument>
#include <QNetworkAccessManager>
#include <QNetworkRequest>
#include <QNetworkReply>
#include <QEventLoop>
#include <QUrl>
#include <QStringList>
#include <QThread>
#include <random>
#include <chrono>

static const int    MAX_RETRIES       = 5;
static const double BASE_DELAY_SECS   = 1.0;
static const double MAX_DELAY_SECS    = 60.0;
static const double JITTER            = 0.25;
static const QList<int> RETRYABLE_STATUSES = {429, 529};

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

static double backoffDelay(int attempt, const QString& retryAfterHeader) {
    double base = MAX_DELAY_SECS;
    if (!retryAfterHeader.isEmpty()) {
        bool ok = false;
        double ra = retryAfterHeader.toDouble(&ok);
        if (ok && ra > 0.0) {
            base = qMin(ra, MAX_DELAY_SECS);
        }
    } else {
        base = qMin(BASE_DELAY_SECS * (1 << attempt), MAX_DELAY_SECS);
    }
    // ±JITTER jitter
    static std::mt19937 rng(std::chrono::steady_clock::now().time_since_epoch().count());
    std::uniform_real_distribution<double> dist(-JITTER, JITTER);
    double jitter = base * JITTER * dist(rng);
    return qMax(0.1, base + jitter);
}

static void interruptibleSleep(double seconds, const std::function<bool()>& isCancelled) {
    auto deadline = std::chrono::steady_clock::now()
        + std::chrono::milliseconds(static_cast<long long>(seconds * 1000.0));
    while (std::chrono::steady_clock::now() < deadline) {
        if (isCancelled()) return;
        auto remaining = std::chrono::duration_cast<std::chrono::milliseconds>(
            deadline - std::chrono::steady_clock::now());
        auto slice = qMin(remaining.count(), 500LL);
        if (slice <= 0) break;
        QThread::msleep(static_cast<unsigned long>(slice));
    }
}

static LlmResponse syncPost(const QUrl& url, const QByteArray& body,
                            const QString& apiKey) {
    QNetworkAccessManager mgr;
    QNetworkRequest req(url);
    req.setHeader(QNetworkRequest::ContentTypeHeader, "application/json");
    req.setRawHeader("Authorization", ("Bearer " + apiKey).toUtf8());
    req.setRawHeader("api-key",       apiKey.toUtf8());

    QNetworkReply* reply = mgr.post(req, body);
    QEventLoop loop;
    QObject::connect(reply, &QNetworkReply::finished, &loop, &QEventLoop::quit);
    loop.exec();

    LlmResponse resp;
    resp.httpStatus = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
    resp.body       = reply->readAll();

    // Capture Retry-After headers
    if (reply->hasRawHeader("retry-after-ms")) {
        resp.retryAfterHeader = QString::fromUtf8(reply->rawHeader("retry-after-ms"));
    } else if (reply->hasRawHeader("retry-after")) {
        resp.retryAfterHeader = QString::fromUtf8(reply->rawHeader("retry-after"));
    }

    if (reply->error() != QNetworkReply::NoError) {
        // HTTP error — try to surface the provider's actual error body,
        // falling back to Qt's errorString() if the body is unhelpful.
        QJsonDocument doc = QJsonDocument::fromJson(resp.body);
        if (!doc.isObject() || !doc.object().contains("error")) {
            QString detail = QString::fromUtf8(resp.body).trimmed();
            if (detail.isEmpty())
                detail = reply->errorString();
            resp.body = QJsonDocument(QJsonObject{
                {"error", QJsonObject{{"message", detail}}}
            }).toJson();
        }
    }
    reply->deleteLater();
    return resp;
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
        if (!params.reasoningEffort.isEmpty()) {
            payload["reasoning_effort"] = params.reasoningEffort;
        }

        // ── API call with 429 / 529 exponential backoff ──────────
        LlmResponse lastResp;
        bool gotSuccess = false;
        for (int attempt = 0; attempt <= MAX_RETRIES; ++attempt) {
            if (isCancelled()) return;

            lastResp = syncPost(url, QJsonDocument(payload).toJson(QJsonDocument::Compact),
                                params.apiKey);
            if (isCancelled()) return;

            QJsonObject body = QJsonDocument::fromJson(lastResp.body).object();

            int code = lastResp.httpStatus;
            if (code >= 200 && code < 300) {
                gotSuccess = true;
                break;
            }

            if (RETRYABLE_STATUSES.contains(code) && attempt < MAX_RETRIES) {
                double delay = backoffDelay(attempt, lastResp.retryAfterHeader);
                QString msg = body["error"].toObject()["message"].toString(
                    QString::fromUtf8(lastResp.body));
                onEvent(QJsonObject{
                    {"type",         "retrying"},
                    {"attempt",      attempt + 1},
                    {"max_attempts", MAX_RETRIES},
                    {"delay_secs",   qRound(delay * 10.0) / 10.0},
                    {"status_code",  code},
                    {"message",      msg},
                });
                interruptibleSleep(delay, isCancelled);
                if (isCancelled()) {
                    onEvent(QJsonObject{
                        {"type",    "final_response"},
                        {"content", "Request cancelled during backoff."},
                        {"usage",   accUsage},
                    });
                    return;
                }
                continue;
            }

            // Non-retryable or final attempt exhausted — fall through to error
            break;
        }

        if (!gotSuccess) {
            QJsonObject body = QJsonDocument::fromJson(lastResp.body).object();
            QString msg = body["error"].toObject()["message"].toString(
                QString::fromUtf8(lastResp.body));
            onEvent(QJsonObject{
                {"type",    "final_response"},
                {"content", QString("API error (HTTP %1): %2")
                    .arg(lastResp.httpStatus).arg(msg)},
                {"usage",   accUsage}
            });
            return;
        }

        QJsonObject body = QJsonDocument::fromJson(lastResp.body).object();

        // Check for API-level error (shouldn't happen in 2xx, but be safe)
        if (body.contains("error")) {
            QString msg = body["error"].toObject()["message"].toString(lastResp.body);
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
            if (params.preserveReasoning) {
                const QStringList reasoningKeys = {"reasoning_content", "reasoning", "reasoning_details"};
                for (const QString& key : reasoningKeys) {
                    if (msg.contains(key)) asstMsg[key] = msg[key];
                }
            }

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
        QJsonObject finalMsg;
        finalMsg["role"] = "assistant";
        finalMsg["content"] = content;
        if (params.preserveReasoning) {
            const QStringList reasoningKeys = {"reasoning_content", "reasoning", "reasoning_details"};
            for (const QString& key : reasoningKeys) {
                if (msg.contains(key)) finalMsg[key] = msg[key];
            }
        }
        onEvent(QJsonObject{
            {"type",    "final_response"},
            {"content", content},
            {"message", finalMsg},
            {"usage",   accUsage}
        });
        return;
    }
}
