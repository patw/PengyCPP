#include "llmclient.h"
#include "config.h"
#include "tools.h"
#include <QJsonDocument>
#include <QNetworkAccessManager>
#include <QNetworkRequest>
#include <QNetworkReply>
#include <QEventLoop>
#include <QUrl>
#include <QLocale>
#include <QStringList>
#include <QThread>
#include <QSet>
#include <QMutex>
#include <QMutexLocker>
#include <random>
#include <chrono>
#include <cmath>
#include <stdexcept>

static const int    MAX_RETRIES       = 5;
static const double BASE_DELAY_SECS   = 1.0;
static const double MAX_DELAY_SECS    = 60.0;
static const double JITTER            = 0.25;
/// Sent back for a tool call that needs confirmation when the frontend has
/// nobody to ask (a CLI run with no TTY on stdin -- cron, scripts, CI).
/// "Declined by user" is a lie there and reads as a refusal, which made
/// unattended runs stop with nothing done.
/// Identical to `NO_USER_CONFIRM` in the Python and Rust editions.
static const QString NO_USER_CONFIRM = QStringLiteral(
    "No user is available to confirm this tool (non-interactive run). Do not treat this as "
    "a refusal. If running this tool is clearly the right next step, say so in your reply "
    "and continue without it; if it was optional, carry on with the rest of the task. "
    "Never claim the tool ran.");

static const QList<int> RETRYABLE_STATUSES = {429, 529};

// ── Provider / transport timeouts ────────────────────────────────
// A timeout usually means "the endpoint was busy", not "the request was wrong",
// so it gets a retry budget of its own.  The budget is deliberately smaller than
// MAX_RETRIES: this request is non-streaming, so llmTimeout is total wall clock
// per attempt, and five retries at the 300 s default would hang a turn for
// ~25 minutes before the user saw the timeout message.  Cancel is polled during
// every backoff sleep.
static const int MAX_TIMEOUT_RETRIES = 3;
// 408 Request Timeout, and 504 Gateway Timeout -- a proxy answering for an
// upstream that ran out of time, which is the same failure seen from the client.
static const QList<int> TIMEOUT_STATUSES = {408, 504};

// ── Transient server errors, and routes that drop mid-task ───────
// 502/503 say "the proxy or upstream is not available right now" -- a restart,
// a rolling deploy, a worker crash.  That is the same busy-endpoint failure a
// timeout is, and a ten-second blip used to kill a multi-hour task.  500 is
// often *deterministic* (the request trips a provider bug), so it buys one
// retry rather than three.
static const int MAX_TRANSIENT_RETRIES = 3;
static const int MAX_INTERNAL_RETRIES = 1;
static const QList<int> TRANSIENT_STATUSES = {502, 503};
// A connection failure *after* this endpoint has answered is a dropped route
// (server restart, OOM kill, sleep/wake, a VM's address changing), not a
// misconfigured endpoint.  First contact keeps failing fast: that is the case
// the "is your server running?" help is written for.
static const int MAX_UNREACHABLE_RETRIES = 3;
// A 200 that carries no usable answer (no choices at all) is the shape a proxy
// answers when it loses its upstream mid-generation, so it gets one retry --
// the same reasoning as a 500.
static const int MAX_EMPTY_RETRIES = 1;

/// A 200 that carries no choices at all.
static QString noChoicesHelp() {
    return QStringLiteral(
        "The endpoint answered with no choices in the response, so there is no answer to "
        "deliver. Pengy retried once. This is the endpoint, not your request: the usual "
        "causes are a proxy that lost its upstream mid-generation, an engine that died "
        "while answering, or a model name the endpoint does not serve. "
        "Nothing was added to the chat.");
}

/// A 200 whose message has neither content nor tool calls.  Names the causes that
/// are actually observed: the reasoning-budget case is documented in the field
/// (BotTalk ff4f1c1f) -- a reasoning model whose whole output allowance goes to
/// reasoning tokens answers with empty content and finish_reason=stop.
static QString emptyReplyHelp(const QJsonObject& message, const QString& finishReason,
                              const QJsonObject& body) {
    const QString reasoning = (message.contains("reasoning_content")
        ? message["reasoning_content"].toString()
        : message["reasoning"].toString()).trimmed();
    const int completion = body["usage"].toObject()["completion_tokens"].toInt(-1);
    QString help = QString(
        "The model returned an empty reply%1: no content and no tool calls, so the turn "
        "has no answer. Nothing was added to the chat. Usual causes: a content filter "
        "that wiped the answer; a proxy that lost its upstream mid-generation; a "
        "reasoning model that spent the whole output allowance on reasoning tokens.")
        .arg(completion >= 0 ? QString(" (completion %1 tokens)").arg(completion) : QString());
    if (!reasoning.isEmpty())
        help += QStringLiteral(" This reply carried reasoning text and no answer, which "
                               "points at the reasoning budget: raise the output allowance, "
                               "lower the reasoning effort, or choose a non-reasoning model.");
    help += QString(" The provider reported finish_reason=%1. Pengy did not retry: an empty "
                    "answer is the endpoint's shape, not a blip.")
                .arg(finishReason.isEmpty() ? QStringLiteral("unset") : finishReason);
    return help;
}

namespace {
// Endpoints that have answered at least one request in this process, keyed by
// base URL so a second chat cannot inherit another's confidence.
QSet<QString> g_answeredEndpoints;
QMutex g_answeredMutex;
}

static bool endpointHasAnswered(const QString& baseUrl) {
    const QMutexLocker lk(&g_answeredMutex);
    return g_answeredEndpoints.contains(baseUrl);
}

static void markEndpointAnswered(const QString& baseUrl) {
    const QMutexLocker lk(&g_answeredMutex);
    g_answeredEndpoints.insert(baseUrl);
}

/// Whether to re-send a request that answered with `code`, and how many times.
/// 400/401/404 and friends are the request's fault -- no amount of waiting
/// makes the explanatory message arrive faster, so they must not be queued
/// behind a backoff.
static bool transientRetry(int code, int& budget, QString& reason) {
    if (TIMEOUT_STATUSES.contains(code)) { budget = MAX_TIMEOUT_RETRIES; reason = "timeout"; return true; }
    if (TRANSIENT_STATUSES.contains(code)) { budget = MAX_TRANSIENT_RETRIES; reason = "server"; return true; }
    if (code == 500) { budget = MAX_INTERNAL_RETRIES; reason = "server"; return true; }
    return false;
}
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

static QString errorDetail(const QJsonObject& body, const QByteArray& raw) {
    const QJsonValue error = body["error"];
    const QString message = error.toObject()["message"].toString();
    if (!message.isEmpty()) return message;
    if (error.isString()) return error.toString();
    if (body["message"].isString()) return body["message"].toString();
    return QString::fromUtf8(raw);
}

static const QString LEGACY_TEXT_ONLY_ERROR =
    "Only text content parts are supported by this upstream format";

static bool isImageInputError(int statusCode, const QJsonObject& body, const QString& errorMsg) {
    if (statusCode != 400) return false;
    const QJsonObject error = body["error"].toObject();
    // Structured adapter errors are authoritative. Do not strip images to hide
    // malformed requests, unsupported audio, image roles/detail, or options.
    if (error["source"].toString() == "openai-proxy")
        return error["code"].toString() == "unsupported_content_type"
            && error["content_type"].toString() == "image_url";
    QString lower = errorMsg.toLower();
    while (lower.endsWith('.')) lower.chop(1);
    if (lower == LEGACY_TEXT_ONLY_ERROR.toLower()) return true;
    static const QStringList phrases = {
        "does not support image", "doesn't support image", "do not support image",
        "does not support vision", "does not support multimodal",
        "image inputs are not supported", "image input is not supported",
        "images are not supported", "image_url is not supported",
        "unsupported image input", "unsupported vision input",
    };
    for (const QString& phrase : phrases)
        if (lower.contains(phrase)) return true;
    return (lower.contains("text-only") || lower.contains("only text"))
        && (lower.contains("image") || lower.contains("vision") || lower.contains("multimodal"));
}

static QString imageRejectionNotice(const QJsonObject& body, QString detail) {
    while (detail.endsWith('.')) detail.chop(1);
    if (body["error"].toObject()["source"].toString() == "openai-proxy"
            || detail.compare(LEGACY_TEXT_ONLY_ERROR, Qt::CaseInsensitive) == 0)
        return "[The proxy adapter cannot translate image inputs for this route. Images were omitted from this request; only file metadata is available. This is not evidence that the underlying model lacks vision. Do not claim to have inspected the images.]";
    return "[The API endpoint rejected image/vision inputs as unsupported. Images were omitted from this request; only file metadata is available. Do not claim to have inspected the images.]";
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
        "leaves no room to answer in the context",
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

std::optional<double> responseTokensPerSecond(const QJsonObject& body, double seconds) {
    const QJsonValue output = body["usage"].toObject()["completion_tokens"];
    if (!output.isDouble() || output.toDouble() < 0 || !std::isfinite(seconds) || seconds <= 0)
        return std::nullopt;
    const double rate = output.toDouble() / seconds;
    return std::isfinite(rate) ? std::optional<double>(rate) : std::nullopt;
}

static LlmResponse syncPost(const QUrl& url, const QByteArray& body,
                            const QString& apiKey, int timeoutMs) {
    QNetworkAccessManager mgr;
    QNetworkRequest req(url);
    req.setHeader(QNetworkRequest::ContentTypeHeader, "application/json");
    req.setRawHeader("Authorization", ("Bearer " + apiKey).toUtf8());
    req.setRawHeader("api-key",       apiKey.toUtf8());
    req.setTransferTimeout(timeoutMs);

    const auto started = std::chrono::steady_clock::now();
    QNetworkReply* reply = mgr.post(req, body);
    QEventLoop loop;
    QObject::connect(reply, &QNetworkReply::finished, &loop, &QEventLoop::quit);
    loop.exec();

    LlmResponse resp;
    resp.httpStatus = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
    // Qt's transfer deadline (the configured LLM timeout) firing is the one
    // transport failure worth re-sending on the timeout budget; connection
    // refused is only worth re-sending once this endpoint has answered.
    const auto qtError = reply->error();
    resp.transportTimeout = (qtError == QNetworkReply::TimeoutError
                             || qtError == QNetworkReply::ProxyTimeoutError);
    resp.transportFailure = (qtError != QNetworkReply::NoError);
    resp.body       = reply->readAll();
    resp.requestSeconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - started).count();

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

static bool isProxyReasoning(const QJsonValue& details) {
    return details.isObject() && details.toObject()["format"].toString()
        == "openai-proxy/reasoning-v1";
}

static QJsonArray withoutCrossModelProxyState(const QJsonArray& messages, const QString& model) {
    QJsonArray result;
    for (const QJsonValue& value : messages) {
        QJsonObject message = value.toObject();
        const QJsonValue details = message["reasoning_details"];
        if (isProxyReasoning(details) && details.toObject()["proxy_model"].toString() != model)
            message.remove("reasoning_details");
        result.append(message);
    }
    return result;
}

static void preserveReasoningFields(QJsonObject& target, const QJsonObject& source, bool preserve) {
    const QStringList keys = {"reasoning_content", "reasoning", "reasoning_details"};
    for (const QString& key : keys) {
        // Tagged proxy state is required for continuation, not a UI preference.
        if (source.contains(key) && (preserve ||
                (key == "reasoning_details" && isProxyReasoning(source[key]))))
            target[key] = source[key];
    }
}

// A length stop this short is context starvation, not a plausible output cap;
// larger (or unreported) completions keep the safe truncation failure.
static constexpr qint64 kShortLengthCompletionTokens = 1024;

static qint64 completionTokens(const QJsonObject& usage) {
    const QJsonValue value = usage["completion_tokens"];
    return value.isDouble() && value.toDouble() >= 0 ? qint64(value.toDouble()) : -1;
}

// Whether a length stop is worth a context-reduction retry.
//
// An empty answer always qualifies. A partial answer or truncated tool call
// qualifies only when the provider reports a completion too short to be an
// output cap -- typically a lead-in like "Redoing it properly:" cut off just
// before its tool call because the window was nearly full.
static bool lengthSuggestsContextPressure(const QString& content, bool hasToolCalls,
                                          const QJsonObject& usage, int outputLimit) {
    if (!hasToolCalls && content.trimmed().isEmpty()) return true;
    qint64 limit = kShortLengthCompletionTokens;
    if (outputLimit > 0) limit = qMin(limit, qint64(outputLimit));
    const qint64 completion = completionTokens(usage);
    return completion >= 0 && completion < limit;
}

// Length means generation exhaustion, not necessarily a full context window.
static QString generationLimitMessage(const QString& content, bool hasToolCalls,
                                      const QJsonObject& usage, int recoveryAttempts) {
    const QString detail = hasToolCalls
        ? "Generation limit reached during tool calls; no tools from this response were executed."
        : content.trimmed().isEmpty()
            ? "Generation limit reached before an answer was produced."
            : "Generation limit reached; the answer is incomplete.";
    // Grouped digits match Python's {:,} in the shared message.
    const QLocale grouped(QLocale::English, QLocale::UnitedStates);
    const qint64 completion = completionTokens(usage);
    QString counts;
    if (usage["prompt_tokens"].isDouble() && completion >= 0)
        counts = QString(" (prompt %1 tokens, completion %2 tokens)")
            .arg(grouped.toString(qint64(usage["prompt_tokens"].toDouble())), grouped.toString(completion));
    const QString retried = recoveryAttempts > 0
        ? QString("Pengy retried after %1 context reduction(s) without success.").arg(recoveryAttempts)
        : QString("Pengy did not retry automatically.");
    QString message = detail + " The provider reported finish_reason=length" + counts + ". "
        "This can mean an output-token cap or insufficient remaining context. "
        "Try a shorter conversation, a larger output allowance, or a reasoning budget "
        "that leaves room for an answer. " + retried;
    if (!content.trimmed().isEmpty())
        message += "\n\nPartial response (incomplete, not saved as an answer):\n" + content;
    return message;
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
    bool imageInputRejected = false;
    QString imageRejection;

    ContextRecovery::Recovery recovery(params.messages, params.baseUrl, params.model, params.recovery);
    if (params.recovery.outputParameter != "max_tokens" && params.recovery.outputParameter != "max_completion_tokens") {
        emitConfigError(onEvent, "output_token_parameter must be max_tokens or max_completion_tokens"); return;
    }
    auto recover = [&]() -> bool {
        ContextRecovery::Plan plan;
        if (!recovery.plan(current, plan)) return false;
        QStringList summaries;
        for (const QString& chunk : plan.chunks) {
            if (isCancelled()) return false;
            ++recovery.summaryCalls;
            QJsonObject payload{{"model", params.model}, {"messages", QJsonArray{
                QJsonObject{{"role", "system"}, {"content", ContextRecovery::SummaryPrompt}},
                QJsonObject{{"role", "user"}, {"content", chunk}}}}};
            payload[params.recovery.outputParameter] = 2048;
            // The summary is part of this turn, so every transient failure shape
            // gets the same budget here as on the turn request.
            int summaryRetries = 0;
            LlmResponse resp;
            for (;;) {
                resp = syncPost(url, QJsonDocument(payload).toJson(QJsonDocument::Compact), params.apiKey, params.llmTimeout * 1000);
                if (isCancelled()) return false;
                if (resp.httpStatus >= 200 && resp.httpStatus < 300) markEndpointAnswered(params.baseUrl);
                int budget = 0;
                QString reason;
                if (!transientRetry(resp.httpStatus, budget, reason)) {
                    if (resp.transportTimeout) budget = MAX_TIMEOUT_RETRIES;
                    else if (resp.transportFailure && endpointHasAnswered(params.baseUrl))
                        budget = MAX_UNREACHABLE_RETRIES;
                }
                if (budget == 0 || summaryRetries >= budget) break;
                ++summaryRetries;
                interruptibleSleep(backoffDelay(summaryRetries - 1, resp.retryAfterHeader), isCancelled);
                if (isCancelled()) return false;
            }
            auto body = QJsonDocument::fromJson(resp.body).object();
            if (body.contains("usage")) addUsage(accUsage, body["usage"].toObject());
            auto choices = body["choices"].toArray();
            auto choice = choices.isEmpty() ? QJsonObject{} : choices[0].toObject();
            auto message = choice["message"].toObject();
            if (resp.httpStatus < 200 || resp.httpStatus >= 300 || choice["finish_reason"] != "stop" || message["content"].toString().trimmed().isEmpty() || !message["tool_calls"].toArray().isEmpty()) {
                emitTurnError(onEvent, 0, "Context summary was incomplete or failed; original history retained.", params.baseUrl);
                throw std::runtime_error("context summary failed");
            }
            summaries << message["content"].toString();
        }
        QString error;
        auto event = recovery.commit(plan, summaries, current, error);
        if (!error.isEmpty()) { emitTurnError(onEvent, 0, error, params.baseUrl); throw std::runtime_error("context checkpoint failed"); }
        if (event.isEmpty()) return false;
        onEvent(event); return true;
    };
    for (;;) {
        if (isCancelled()) return;

        // Compact a private request copy only. Events and persisted history
        // continue to use the original current messages.
        QJsonArray requestMessages = withoutCrossModelProxyState(recovery.apply(current), params.model);
        if (imageInputRejected) {
            stripImageUrlParts(requestMessages);
            requestMessages.append(QJsonObject{{"role", "user"}, {"content", imageRejection}});
        }
        int contextRetries = 0;
        QJsonObject payload{
            {"model",       params.model},
            {"messages",    requestMessages},
            {"tools",       Tools::toolDefinitions()},
            {"tool_choice", "auto"},
        };
        if (params.recovery.outputLimit > 0) payload[params.recovery.outputParameter] = params.recovery.outputLimit;
        if (!params.reasoningEffort.isEmpty()) {
            payload["reasoning_effort"] = params.reasoningEffort;
        }

        // ── API call with 429 / 529 exponential backoff ──────────
        LlmResponse lastResp;
        bool gotSuccess = false;
        int rateRetries = 0;
        int transientRetries = 0;
        // The provider answer that started this retry chain.  Reported when a
        // retry itself fails with nothing more informative, so the retries never
        // change what the user is told about the failure they care about.
        int chainStatus = 0;
        QString chainDetail;
        bool chainTimedOut = false;
        for (;;) {
            if (isCancelled()) return;

            lastResp = syncPost(url, QJsonDocument(payload).toJson(QJsonDocument::Compact),
                                params.apiKey, params.llmTimeout * 1000);
            if (isCancelled()) return;

            QJsonObject body = QJsonDocument::fromJson(lastResp.body).object();

            int code = lastResp.httpStatus;
            if (code >= 200 && code < 300) {
                // The endpoint is reachable and speaks our protocol.  From here
                // on, a connection failure means it went away mid-work.
                markEndpointAnswered(params.baseUrl);
                if (body["choices"].toArray().isEmpty()) {
                    // A 200 with no choices is the shape a proxy answers when it
                    // loses its upstream mid-generation: one retry (the 500
                    // reasoning -- ride out a blip, do not pay three full-cost
                    // attempts for a deterministic answer), then say what happened.
                    if (transientRetries < MAX_EMPTY_RETRIES) {
                        const double delay = backoffDelay(transientRetries, lastResp.retryAfterHeader);
                        ++transientRetries;
                        onEvent(QJsonObject{
                            {"type",         "retrying"},
                            {"reason",       "empty"},
                            {"attempt",      transientRetries},
                            {"max_attempts", MAX_EMPTY_RETRIES},
                            {"delay_secs",   qRound(delay * 10.0) / 10.0},
                            {"status_code",  0},
                            {"message",      QStringLiteral("The endpoint answered with no choices")},
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
                    onEvent(QJsonObject{
                        {"type",    "error"},
                        {"kind",    "empty"},
                        {"message", noChoicesHelp()},
                    });
                    return;
                }
                gotSuccess = true;
                break;
            }

            const QString detail = errorDetail(body, lastResp.body);
            // Retry the outgoing copy only. Preserve stored history, prior
            // context reductions, and opaque reasoning/tool continuation state.
            if (isImageInputError(code, body, detail) && !isContextLimitError(code, body, detail)
                    && hasImageUrlParts(requestMessages)) {
                imageInputRejected = true;
                imageRejection = imageRejectionNotice(body, detail);
                stripImageUrlParts(requestMessages);
                requestMessages.append(QJsonObject{
                    {"role", "user"}, {"content", imageRejection},
                });
                payload["messages"] = requestMessages;
                continue; // bounded: this request no longer contains image parts
            }
            if (isContextLimitError(code, body, detail)) {
                if (params.recovery.enabled) {
                    try {
                        if (recover()) {
                            requestMessages = withoutCrossModelProxyState(recovery.apply(current), params.model);
                            if (imageInputRejected) { stripImageUrlParts(requestMessages); requestMessages.append(QJsonObject{{"role","user"},{"content",imageRejection}}); }
                            payload["messages"] = requestMessages; continue;
                        }
                    } catch (const std::runtime_error&) { return; }
                    emitTurnError(onEvent, code, "Model context limit reached; could not fit the protected task after bounded recovery. Full history retained; try a shorter request or a new chat.", params.baseUrl);
                    return;
                }
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

            // A provider that answers "I ran out of time" (408, or 504 from a
            // proxy), and our own transfer deadline firing with no answer at
            // all, are both the busy-endpoint failure: re-send it.  Once the
            // budget is spent the ordinary timeout message is reported below,
            // unchanged.
            // Transient failures -- the endpoint is busy or gone, not the request
            // being wrong: 408/504 (out of time), 502/503 (proxy/upstream gone),
            // 500 (one attempt, since a 500 is often the request's own fault),
            // our own transfer deadline, and -- once this endpoint has answered
            // in this session -- a connection that dropped mid-task.  Once the
            // budget is spent the ordinary message is reported below, unchanged.
            int budget = 0;
            QString reason;
            const bool byStatus = transientRetry(code, budget, reason);
            const bool byTransport = !byStatus && lastResp.transportFailure;
            if (byTransport) {
                if (lastResp.transportTimeout) {
                    budget = MAX_TIMEOUT_RETRIES;
                    reason = "timeout";
                } else if (endpointHasAnswered(params.baseUrl)) {
                    // Refused / DNS / TLS after this endpoint answered: a restart
                    // or a dropped route, not a bad URL.  These failures are
                    // instant, so the retries cost seconds.  First contact still
                    // fails fast, because that is the case the "is your server
                    // running?" help is for.
                    budget = MAX_UNREACHABLE_RETRIES;
                    reason = "unreachable";
                }
            }
            if ((byStatus || byTransport) && transientRetries < budget) {
                if (byStatus && chainStatus == 0) { chainStatus = code; chainDetail = detail; }
                if (byTransport && lastResp.transportTimeout) chainTimedOut = true;
                double delay = backoffDelay(transientRetries, lastResp.retryAfterHeader);
                ++transientRetries;
                onEvent(QJsonObject{
                    {"type",         "retrying"},
                    {"reason",       reason},
                    {"attempt",      transientRetries},
                    {"max_attempts", budget},
                    {"delay_secs",   qRound(delay * 10.0) / 10.0},
                    // A transport failure has no HTTP status to report.
                    {"status_code",  byTransport ? 0 : code},
                    {"message",      detail},
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

            if (RETRYABLE_STATUSES.contains(code) && rateRetries < MAX_RETRIES) {
                double delay = backoffDelay(rateRetries, lastResp.retryAfterHeader);
                ++rateRetries;
                QString msg = errorDetail(body, lastResp.body);
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

        if (!gotSuccess) {
            QJsonObject body = QJsonDocument::fromJson(lastResp.body).object();
            QString msg = errorDetail(body, lastResp.body);
            if (lastResp.httpStatus <= 0 && chainStatus > 0) {
                // We were retrying a provider answer (408/500/502/503) and the
                // retry itself failed with nothing more informative.  Report the
                // answer that started the chain: a transport failure carries no
                // provider message, and the retries must not change what the
                // user is told about the failure they care about.
                emitTurnError(onEvent, chainStatus,
                              QString("API error (HTTP %1): %2").arg(chainStatus).arg(chainDetail),
                              params.baseUrl);
                return;
            }
            if (lastResp.httpStatus <= 0 && chainTimedOut && !lastResp.transportTimeout) {
                // We were retrying a timeout; the retry hit something else.
                // Report the timeout -- that is the failure the retries were for.
                onEvent(QJsonObject{
                    {"type",    "error"},
                    {"kind",    "error"},
                    {"message", unreachableHelp(params.baseUrl, "timed out")},
                });
                return;
            }
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
            // Reached only when the request loop is bypassed; the loop itself
            // retries this shape and reports noChoicesHelp() when it gives up.
            onEvent(QJsonObject{
                {"type",    "error"},
                {"kind",    "empty"},
                {"message", noChoicesHelp()},
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

        // Fail before emitting/persisting assistant messages or executing tools.
        // A length-truncated tool sequence is unsafe even if its JSON parses.
        if (choice["finish_reason"].toString() == "length") {
            const QJsonObject usage = body["usage"].toObject();
            // The truncated reply is discarded unexecuted; a retry regenerates
            // it against a smaller provider view.
            if (lengthSuggestsContextPressure(content, !toolCalls.isEmpty(), usage,
                                              params.recovery.outputLimit)) {
                try { if (recover()) continue; } catch (const std::runtime_error&) { return; }
            }
            onEvent(QJsonObject{
                {"type", "error"},
                {"kind", "truncated"},
                {"message", generationLimitMessage(content, !toolCalls.isEmpty(), usage, recovery.attempts)},
            });
            return;
        }

        if (!toolCalls.isEmpty()) {
            // Build assistant message for history
            QJsonObject asstMsg;
            asstMsg["role"]       = "assistant";
            asstMsg["content"]    = content;
            asstMsg["tool_calls"] = toolCalls;
            preserveReasoningFields(asstMsg, msg, params.preserveReasoning);

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
                // Malformed JSON is kept (not replaced by {}) so the tool
                // layer can tell the model its call didn't parse.
                QJsonObject argsObj = Tools::parseRawArguments(argsStr);

                // ask_user_question is handled at the harness level
                if (name == "ask_user_question") {
                    QJsonObject checked;
                    const QString argError = Tools::validateQuestionArguments(argsObj, checked);
                    if (!argError.isEmpty()) {
                        current.append(QJsonObject{
                            {"role",         "tool"},
                            {"tool_call_id", tcId},
                            {"content",      argError}
                        });
                        onEvent(QJsonObject{
                            {"type",         "tool_result"},
                            {"tool_call_id", tcId},
                            {"name",         name},
                            {"args",         argsObj},
                            {"content",      argError},
                            {"declined",     false}
                        });
                        continue;
                    }
                    argsObj = checked;
                    QJsonArray questions = argsObj["questions"].toArray();
                    onEvent(QJsonObject{
                        {"type",         "question_request"},
                        {"name",         name},
                        {"args",         argsObj},
                        {"tool_call_id", tcId},
                        {"questions",    questions},
                        // Running turn usage so the UI can tick the token count
                        // before the turn ends (see final_response).
                        {"usage",        accUsage}
                    });
                    QStringList answers = onQuestion(questions);
                    QJsonObject toolMsg;
                    toolMsg["role"]         = "tool";
                    toolMsg["tool_call_id"] = tcId;
                    if (!answers.isEmpty() && answers != noUserAvailable()) {
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
                        // A headless run (no one to ask) is not a refusal:
                        // tell the model to decide and say what it assumed.
                        const QString unanswered = answers == noUserAvailable()
                            ? QString::fromUtf8(kNoUserAnswer)
                            : QStringLiteral("User cancelled the question.");
                        toolMsg["content"] = unanswered;
                        onEvent(QJsonObject{
                            {"type",         "tool_result"},
                            {"tool_call_id", tcId},
                            {"name",         name},
                            {"args",         argsObj},
                            {"content",      unanswered},
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
                    {"tool_call_id", tcId},
                    // Running turn usage so the UI can tick the token count
                    // before the turn ends (see final_response).
                    {"usage",        accUsage}
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
                    // Headless run: "declined by user" is a lie there and reads
                    // as a refusal, which stopped unattended runs cold.
                    result   = params.noUserAvailable ? NO_USER_CONFIRM
                                                      : QStringLiteral("Tool execution was declined by user.");
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

        // No tool calls — final text response.  An empty one is not an answer:
        // it used to end the turn as a completed reply, so the chat stored a
        // blank assistant message that the next turn reads back as something the
        // model said.  Report it as the provider anomaly it is.
        if (content.trimmed().isEmpty()) {
            onEvent(QJsonObject{
                {"type",    "error"},
                {"kind",    "empty"},
                {"message", emptyReplyHelp(msg, choice["finish_reason"].toString(), body)},
            });
            return;
        }
        QJsonObject finalMsg;
        finalMsg["role"] = "assistant";
        finalMsg["content"] = content;
        preserveReasoningFields(finalMsg, msg, params.preserveReasoning);
        QJsonObject event{
            {"type",    "final_response"},
            {"content", content},
            {"message", finalMsg},
            {"usage",   accUsage}
        };
        // Final response output only; never accumulated turn usage.
        if (const auto rate = responseTokensPerSecond(body, lastResp.requestSeconds))
            event["tokens_per_second"] = *rate;
        onEvent(event);
        return;
    }
}
