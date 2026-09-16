#pragma once
#include <QJsonArray>
#include <QJsonObject>
#include <QString>
#include <functional>
#include <atomic>
#include <utility>

namespace Tools { class ToolContext; }

struct LlmParams {
    QString   baseUrl;
    QString   apiKey;
    QString   model;
    QJsonArray messages;
    QString   toolConfirmation; // "all" | "safe" | "none"
    QString   reasoningEffort;  // empty = provider default / omit
    bool      preserveReasoning = false;
    int       llmTimeout        = 300;
    Tools::ToolContext* toolContext = nullptr;  // per-run sudo/subprocess scope
};

struct LlmResponse {
    int         httpStatus = 200;
    QByteArray  body;
    QString     retryAfterHeader;  // "retry-after" or "retry-after-ms" value
};

// ── Failed turns ─────────────────────────────────────────────────────
//
// A request that fails is reported as an "error" event, never as a
// "final_response".  Every frontend treats a final_response's content as the
// model's answer: it is drawn inside the assistant's own block and appended to
// the chat history.  So an endpoint error used to become a permanent assistant
// turn in the chat file -- read back by /show, /export, the GUI and the Web UI
// as something the model had said.  An "error" event carries:
//   {type: "error", kind: "credentials"|"error", message: <user-facing text>}

/// Does this failed request look like a credentials problem?
///
/// The status code leads (401/403 is unambiguous), then the endpoint's own
/// wording, because several compatible servers answer 400 with "api key is
/// required" instead of a 401.
bool looksLikeCredentialProblem(int httpStatus, const QString& detail);

/// The instructions a user actually needs when credentials are missing.
///
/// The endpoint's own text is not actionable here: OpenAI answers a fresh
/// install with "provide your API key in an Authorization header using Bearer
/// auth", and the Python SDK's client-side error told users to set
/// OPENAI_API_KEY -- an environment variable no edition of Pengy reads.
/// Wording is shared with the Python and Rust editions.
QString credentialHelp(const QString& baseUrl);

class LlmClient {
public:
    using EventFn   = std::function<void(const QJsonObject&)>;
    using CancelFn  = std::function<bool()>;
    // Returns {confirmed, yoloTurn}
    using ConfirmFn  = std::function<std::pair<bool,bool>()>;
    using QuestionFn = std::function<QStringList(const QJsonArray&)>;

    // Blocks the calling thread until the conversation ends or is cancelled.
    //
    // *onQuestion* is required, not defaulted: ask_user_question always pauses
    // for the user, so a frontend that omits it would call a null std::function
    // (std::bad_function_call → terminate) the first time the model asks
    // anything.  A frontend with no way to ask returns an empty list, which the
    // harness reports to the model as a cancelled question.
    void run(const LlmParams& params,
             EventFn   onEvent,
             ConfirmFn onConfirm,
             CancelFn  isCancelled,
             QuestionFn onQuestion);
};
