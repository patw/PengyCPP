#pragma once
#include <QJsonArray>
#include <QJsonObject>
#include <QString>
#include <functional>
#include <atomic>
#include <utility>

struct LlmParams {
    QString   baseUrl;
    QString   apiKey;
    QString   model;
    QJsonArray messages;
    QString   toolConfirmation; // "all" | "safe" | "none"
    QString   reasoningEffort;  // empty = provider default / omit
    bool      preserveReasoning = false;
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
    using ConfirmFn = std::function<std::pair<bool,bool>()>;

    // Blocks the calling thread until the conversation ends or is cancelled.
    void run(const LlmParams& params,
             EventFn   onEvent,
             ConfirmFn onConfirm,
             CancelFn  isCancelled);
};
