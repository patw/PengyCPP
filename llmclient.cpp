#include "llmclient.h"
#include "config.h"
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
static const int MAX_CONTEXT_RETRIES = 4;
static const int CONTEXT_PREVIEW = 1500;
static const QString CONTEXT_STUB = QStringLiteral("[tool output omitted from provider request to fit context; original remains in chat history]");

// ── Failed turns ────────────────────────────────────────────────

/// Phrases OpenAI-compatible endpoints use when a request cannot be
/// authenticated.  Matched case-insensitively in addition to the status code,
/// because several compatible servers answer 400 with "api key is required"
/// rather than a 401 -- the same reason the Python edition matches text as
/// well as exception types.
static const QStringList& credentialPhrases() {
    static const QStringList phrases = {
        "missing credentials",
        "no api key",
        "api key is required",
        "api_key is required",
        "api key must be set",
        "api_key client option must be set",
        "invalid api key",
        "invalid_api_key",
        "incorrect api key",
        "invalid authentication",
        "authentication failed",
        "unauthorized",
        "credentials not found",
        "you didn't provide an api key",
    };
    return phrases;
}

bool looksLikeCredentialProblem(int httpStatus, const QString& detail) {
    if (httpStatus == 401 || httpStatus == 403)
        return true;
    const QString text = detail.toLower();
    for (const QString& phrase : credentialPhrases()) {
        if (text.contains(phrase))
            return true;
    }
    return false;
}

QString credentialHelp(const QString& baseUrl) {
    const QString settings = pengyConfigDirPath() + "/settings.json";
    return QString(
               "No API credentials are configured for %1.\n"
               "\n"
               "Configure Pengy (the CLI, Web UI and GUI all share %2):\n"
               "    pengy-cli /apikey <your-key>    set the API key\n"
               "    pengy-cli /baseurl <url>        change the endpoint (a local Ollama/vLLM, the default, needs no key)\n"
               "    pengy-cli /model <name>         choose a model\n"
               "    pengy-cli /config               review the current settings\n"
               "  Or run pengy-web and open Settings (http://127.0.0.1:5000/settings).\n"
               "\n"
               "Note: Pengy reads credentials from its own settings file. OPENAI_API_KEY\n"
               "and similar environment variables are NOT used, whatever the API error says.")
        .arg(baseUrl, settings);
}

bool isLocalEndpoint(const QString& baseUrl) {
    const QString host = QUrl(baseUrl).host().toLower();
    return host == "localhost" || host == "::1" || host == "0.0.0.0" || host.startsWith("127.");
}

QString noModelHelp(const QString& baseUrl) {
    return QString(
               "No model is selected for %1.\n"
               "\n"
               "Pengy's default endpoint is a local server, which has no model of its own:\n"
               "    pengy-cli /models               list the models this endpoint offers\n"
               "    pengy-cli /model <name>         select one\n"
               "    ollama pull <name>              (Ollama) download one first, if the list is empty\n"
               "  Or open Settings in the GUI / Web UI and use Fetch Models.")
        .arg(baseUrl);
}

QString unreachableHelp(const QString& baseUrl, const QString& detail) {
    const QString suffix = detail.isEmpty() ? QString() : " (" + detail + ")";
    if (isLocalEndpoint(baseUrl)) {
        return QString(
                   "Nothing answered at %1%2.\n"
                   "\n"
                   "Is your local model server running?\n"
                   "    ollama serve                    (Ollama) start the server, then: ollama pull <name>\n"
                   "    pengy-cli /models               list the models it offers\n"
                   "    pengy-cli /baseurl <url>        point Pengy at a different endpoint\n"
                   "    pengy-cli /config               review the current settings")
            .arg(baseUrl, suffix);
    }
    return QString("Could not reach %1%2. Check the endpoint with pengy-cli /baseurl <url>.")
        .arg(baseUrl, suffix);
}

/// Emit a failed turn instead of a final response.
///
/// Credential failures are replaced by credentialHelp(); everything else keeps
/// the endpoint's detail but still travels as an error, so no frontend can
/// mistake it for something the model said.
static void emitTurnError(const LlmClient::EventFn& onEvent,
                          int httpStatus,
                          const QString& detail,
                          const QString& baseUrl) {
    const bool credential = looksLikeCredentialProblem(httpStatus, detail);
    onEvent(QJsonObject{
        {"type",    "error"},
        {"kind",    credential ? "credentials" : "error"},
        {"message", credential ? credentialHelp(baseUrl) : detail},
    });
}

/// Emit a turn that could not even be attempted (see noModelHelp).
static void emitConfigError(const LlmClient::EventFn& onEvent, const QString& message) {
    onEvent(QJsonObject{
        {"type",    "error"},
        {"kind",    "config"},
        {"message", message},
    });
}

// ── Graceful image-stripping helpers ────────────────────────────

static bool hasImageUrlParts(const QJsonArray& messages) {
    for (const QJsonValue& mv : messages) {
        QJsonObject msg = mv.toObject();
        QJsonValue cv = msg["content"];
        if (cv.isArray()) {
            for (const QJsonValue& pv : cv.toArray()) {
                if (pv.toObject()["type"].toString() == "image_url")
                    return true;
            }
        }
    }
    return false;
}

static void stripImageUrlParts(QJsonArray& messages) {
    for (int i = 0; i < messages.size(); ++i) {
        QJsonObject msg = messages[i].toObject();
        QJsonValue cv = msg["content"];
        if (!cv.isArray()) continue;
        QJsonArray old = cv.toArray();
        QJsonArray kept;
        for (const QJsonValue& pv : old) {
            if (pv.toObject()["type"].toString() != "image_url")
                kept.append(pv);
        }
        if (kept.size() == 1 && kept[0].toObject()["type"].toString() == "text") {
            msg["content"] = kept[0].toObject()["text"];
        } else if (kept.isEmpty()) {
            msg["content"] = QStringLiteral("[Empty \xe2\x80\x94 image content was removed]");
        } else {
            msg["content"] = kept;
        }
        messages[i] = msg;
    }
}

static const QStringList IMAGE_ERROR_KEYWORDS = {
    "image", "multimodal", "vision", "not support", "unsupported"
};

static bool isImageInputError(int statusCode, const QString& errorMsg) {
    if (statusCode != 400) return false;
    QString lower = errorMsg.toLower();
    for (const QString& kw : IMAGE_ERROR_KEYWORDS) {
        if (lower.contains(kw)) return true;
    }
    return false;
}

static bool isContextLimitError(int status, const QJsonObject& body, const QString& detail) {
    if (status != 400 && status != 413 && status != 422) return false;
    const QJsonValue error = body.contains("error") ? body["error"] : QJsonValue(body);
    const QJsonObject fields = error.toObject();
    static const QStringList codes = {
        "context_length_exceeded", "context_window_exceeded", "prompt_too_long",
        "input_too_long", "max_context_length_exceeded", "token_limit_exceeded",
    };
    for (const QString& code : {fields["code"].toString(), fields["type"].toString(), body["code"].toString()}) {
        if (codes.contains(code.toLower())) return true;
    }
    static const QStringList phrases = {
        "context length", "context window", "context limit", "maximum context",
        "prompt too long", "input too long", "too many tokens", "token limit exceeded",
        "exceeds the model's context", "exceeds the model context",
        "exceeds the context", "context size", "context_length_exceeded",
        "exceeds the maximum allowed number of tokens", "maximum number of tokens",
    };
    const QString text = (fields["message"].toString().isEmpty()
        ? (error.isString() ? error.toString() : detail)
        : fields["message"].toString()).toLower();
    for (const QString& phrase : phrases) {
        if (text.contains(phrase)) return true;
    }
    return false;
}

// Return the number of QChars removed, or zero if no safe reduction remains.
// Tool-call IDs, assistant tool_calls and the original transcript stay intact.
static int compactToolResults(QJsonArray& messages, int stage) {
    int newest = -1;
    for (int i = 0; i < messages.size(); ++i) {
        if (messages[i].toObject()["role"].toString() == "tool") newest = i;
    }
    const auto eligible = [&](int i) {
        const QJsonObject msg = messages[i].toObject();
        if (msg["role"].toString() != "tool" || !msg["content"].isString()) return false;
        const QString text = msg["content"].toString();
        return !text.startsWith(CONTEXT_STUB)
            && !text.startsWith("Tool execution was declined")
            && !text.startsWith("User cancelled")
            && text.size() >= (stage == 1 ? 2 * CONTEXT_PREVIEW + 200 : 256);
    };
    bool olderEligible = false;
    for (int i = 0; i < messages.size(); ++i) {
        if (i != newest && eligible(i)) { olderEligible = true; break; }
    }
    int saved = 0;
    for (int i = 0; i < messages.size(); ++i) {
        if (!eligible(i) || (olderEligible && i == newest)) continue;
        QJsonObject msg = messages[i].toObject();
        const QString text = msg["content"].toString();
        const QString replacement = stage == 1
            ? text.left(CONTEXT_PREVIEW)
                + QString("\n\n[... %1 characters omitted from provider request; original remains in chat history ...]\n\n")
                    .arg(text.size() - 2 * CONTEXT_PREVIEW)
                + text.right(CONTEXT_PREVIEW)
            : CONTEXT_STUB;
        const int reduction = text.size() - replacement.size();
        if (reduction > 0) {
            msg["content"] = replacement;
            messages[i] = msg;
            saved += reduction;
        }
    }
    return saved;
}

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
                            const QString& apiKey, int timeoutMs) {
    QNetworkAccessManager mgr;
    QNetworkRequest req(url);
    req.setHeader(QNetworkRequest::ContentTypeHeader, "application/json");
    req.setRawHeader("Authorization", ("Bearer " + apiKey).toUtf8());
    req.setRawHeader("api-key",       apiKey.toUtf8());
    req.setTransferTimeout(timeoutMs);

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
                    CancelFn  isCancelled,
                    QuestionFn onQuestion) {

    // Checked here rather than in each frontend so the CLI, GUI and Web UI cannot
    // disagree -- and because an empty model name would otherwise be sent to the
    // endpoint, whose complaint about it is not an instruction.  No request is
    // made, so nothing is charged or logged anywhere.
    if (params.model.trimmed().isEmpty()) {
        emitConfigError(onEvent, noModelHelp(params.baseUrl));
        return;
    }

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

        // Compact a private request copy only. Events and persisted history
        // continue to use the original current messages.
        QJsonArray requestMessages = current;
        int contextRetries = 0;
        QJsonObject payload{
            {"model",       params.model},
            {"messages",    requestMessages},
            {"tools",       Tools::toolDefinitions()},
            {"tool_choice", "auto"},
        };
        if (!params.reasoningEffort.isEmpty()) {
            payload["reasoning_effort"] = params.reasoningEffort;
        }

        // ── API call with 429 / 529 exponential backoff ──────────
        LlmResponse lastResp;
        bool gotSuccess = false;
        bool imagesStripped = false;
        int rateRetries = 0;
        for (;;) {
            if (isCancelled()) return;

            lastResp = syncPost(url, QJsonDocument(payload).toJson(QJsonDocument::Compact),
                                params.apiKey, params.llmTimeout * 1000);
            if (isCancelled()) return;

            QJsonObject body = QJsonDocument::fromJson(lastResp.body).object();

            int code = lastResp.httpStatus;
            if (code >= 200 && code < 300) {
                gotSuccess = true;
                break;
            }

            // ── Graceful handling: model doesn't support images ──
            {
                QString msg = body["error"].toObject()["message"].toString(
                    QString::fromUtf8(lastResp.body));
                if (isImageInputError(code, msg) && !isContextLimitError(code, body, msg)
                        && hasImageUrlParts(current)) {
                    stripImageUrlParts(current);
                    current.append(QJsonObject{
                        {"role",    "user"},
                        {"content", QStringLiteral(
                            "[This AI model does not support image/vision inputs, "
                            "so the image could not be attached. "
                            "The file metadata was returned above.]")},
                    });
                    imagesStripped = true;
                    break;  // exit retry loop, outer loop will restart
                }
            }

            const QString detail = body["error"].toObject()["message"].toString(
                QString::fromUtf8(lastResp.body));
            if (isContextLimitError(code, body, detail)) {
                if (contextRetries < MAX_CONTEXT_RETRIES) {
                    const int saved = compactToolResults(requestMessages, contextRetries == 0 ? 1 : 2);
                    if (saved > 0) {
                        payload["messages"] = requestMessages;
                        ++contextRetries;
                        onEvent(QJsonObject{
                            {"type", "context_compacted"},
                            {"attempt", contextRetries},
                            {"max_attempts", MAX_CONTEXT_RETRIES},
                            {"chars_removed", saved},
                        });
                        continue;
                    }
                }
                emitTurnError(onEvent, code,
                    QString("Model context limit reached; could not fit this request after %1 tool-output reductions. The full tool outputs remain in chat history. Try a shorter request or a larger-context model.")
                        .arg(contextRetries), params.baseUrl);
                return;
            }

            if (RETRYABLE_STATUSES.contains(code) && rateRetries < MAX_RETRIES) {
                double delay = backoffDelay(rateRetries, lastResp.retryAfterHeader);
                ++rateRetries;
                QString msg = body["error"].toObject()["message"].toString(
                    QString::fromUtf8(lastResp.body));
                onEvent(QJsonObject{
                    {"type",         "retrying"},
                    {"attempt",      rateRetries},
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

        if (imagesStripped) {
            continue;  // restart outer loop with images removed
        }

        if (!gotSuccess) {
            QJsonObject body = QJsonDocument::fromJson(lastResp.body).object();
            QString msg = body["error"].toObject()["message"].toString(
                QString::fromUtf8(lastResp.body));
            if (lastResp.httpStatus <= 0) {
                // The request never reached the endpoint -- syncPost() wrote the
                // transport failure into an error body for us.  With a local
                // default (and no key) this is the likeliest first-run failure, so
                // name the URL and the server to start instead of relaying Qt's
                // text.  A credential-looking transport failure stays classified
                // as one, because a proxy or tunnel can fail while still talking
                // about credentials.
                if (looksLikeCredentialProblem(0, msg))
                    emitTurnError(onEvent, 0, "API error: " + msg, params.baseUrl);
                else
                    onEvent(QJsonObject{
                        {"type",    "error"},
                        {"kind",    "error"},
                        {"message", unreachableHelp(params.baseUrl, msg)},
                    });
                return;
            }
            emitTurnError(
                onEvent,
                lastResp.httpStatus,
                QString("API error (HTTP %1): %2").arg(lastResp.httpStatus).arg(msg),
                params.baseUrl);
            return;
        }

        QJsonObject body = QJsonDocument::fromJson(lastResp.body).object();

        // Check for API-level error (shouldn't happen in 2xx, but be safe)
        if (body.contains("error")) {
            QString msg = body["error"].toObject()["message"].toString(lastResp.body);
            emitTurnError(onEvent, lastResp.httpStatus, "API error: " + msg, params.baseUrl);
            return;
        }

        QJsonArray choices = body["choices"].toArray();
        if (choices.isEmpty()) {
            emitTurnError(onEvent, lastResp.httpStatus,
                          "No choices in API response.", params.baseUrl);
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

                // ask_user_question is handled at the harness level
                if (name == "ask_user_question") {
                    QJsonArray questions = argsObj["questions"].toArray();
                    onEvent(QJsonObject{
                        {"type",         "question_request"},
                        {"name",         name},
                        {"args",         argsObj},
                        {"tool_call_id", tcId},
                        {"questions",    questions}
                    });
                    QStringList answers = onQuestion(questions);
                    QJsonObject toolMsg;
                    toolMsg["role"]         = "tool";
                    toolMsg["tool_call_id"] = tcId;
                    if (!answers.isEmpty()) {
                        // Format answers as tool result
                        QStringList answerLines;
                        for (int i = 0; i < questions.size(); ++i) {
                            QJsonObject q = questions[i].toObject();
                            QString header = q["header"].toString();
                            QString answer = i < answers.size() ? answers[i] : "(no answer)";
                            QJsonArray opts = q["options"].toArray();
                            QString detail;
                            for (const QJsonValue& ov : opts) {
                                QJsonObject opt = ov.toObject();
                                if (opt["label"].toString() == answer)
                                    detail = " — " + opt["description"].toString();
                            }
                            answerLines.append(QString("**%1**: %2%3").arg(header, answer, detail));
                        }
                        toolMsg["content"] = answerLines.join("\n");
                        onEvent(QJsonObject{
                            {"type",         "question_result"},
                            {"tool_call_id", tcId},
                            {"name",         name},
                            {"content",      answerLines.join("\n")}
                        });
                    } else {
                        toolMsg["content"] = "User cancelled the question.";
                        onEvent(QJsonObject{
                            {"type",         "tool_result"},
                            {"tool_call_id", tcId},
                            {"name",         name},
                            {"args",         argsObj},
                            {"content",      "User cancelled the question."},
                            {"declined",     true}
                        });
                    }
                    current.append(toolMsg);
                    continue;
                }

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
                    result = Tools::execute(name, argsObj, nullptr, params.toolContext);
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

            // read_image parks its picture on the tool context because a
            // role:"tool" message only accepts string content on
            // OpenAI-compatible APIs.  Attach anything queued as a follow-up
            // user message — after the loop, so every tool_call keeps its
            // matching tool message immediately behind the assistant one.
            {
                QJsonArray pendingImages = Tools::takePendingImages(params.toolContext);
                if (!pendingImages.isEmpty()) {
                    QJsonArray parts;
                    for (const QJsonValue& v : pendingImages) {
                        const QJsonObject img = v.toObject();
                        parts.append(QJsonObject{
                            {"type", "text"},
                            {"text", "Image loaded by read_image: " +
                                     img["path"].toString()},
                        });
                        parts.append(QJsonObject{
                            {"type", "image_url"},
                            {"image_url", QJsonObject{
                                {"url", "data:" + img["mime"].toString() +
                                        ";base64," + img["b64"].toString()},
                            }},
                        });
                    }
                    current.append(QJsonObject{
                        {"role",    "user"},
                        {"content", parts},
                    });
                }
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
