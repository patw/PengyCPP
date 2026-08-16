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
