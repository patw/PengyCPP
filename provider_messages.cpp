#include "provider_messages.h"
#include "attachments.h"
#include <QCryptographicHash>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSet>

namespace {

// A call whose arguments never parsed cannot be replayed to a strict engine.
//
// A length-capped or malformed generation (the model stopped mid-object) is
// stored verbatim: the transcript stays honest and running it already failed
// cleanly with "not valid JSON". Replaying that raw string is what hurts -- an
// engine that parses every call's arguments (Basalt/Strata, llama.cpp's tool
// parser) answers the whole request 400 "Expecting ',' delimiter: line 1
// column N" at the offset where the stored string ran out, so one bad entry in
// an old chat breaks every later turn on that route while the same chat keeps
// working on a lenient hosted API.
//
// `chars` counts UTF-16 code units here (the Rust and Python editions count
// code points); `sha256` is over the UTF-8 bytes in every edition.
QString repairedArgumentsHandle(const QString& raw)
{
    const QJsonObject handle{
        {"_repaired", true},
        {"note", QStringLiteral(
            "The original arguments for this call were not valid JSON (a truncated "
            "or malformed generation) and the call was never run. Detail kept: "
            "character count, sha256 and a short preview.")},
        {"chars", raw.size()},
        {"sha256", QString::fromLatin1(
            QCryptographicHash::hash(raw.toUtf8(), QCryptographicHash::Sha256)
                .toHex().left(16))},
        {"preview", raw.left(160)},
    };
    return QString::fromUtf8(QJsonDocument(handle).toJson(QJsonDocument::Compact));
}

}  // namespace

QJsonArray repairUnparseableArguments(const QJsonArray& messages)
{
    QJsonArray out;
    for (const QJsonValue& value : messages) {
        const QJsonObject message = value.toObject();
        const QJsonArray calls = message["tool_calls"].toArray();
        if (calls.isEmpty()) {
            out.append(value);
            continue;
        }
        QJsonArray rebuilt;
        bool changed = false;
        for (const QJsonValue& callValue : calls) {
            QJsonObject call = callValue.toObject();
            QJsonObject function = call["function"].toObject();
            const QString raw = function["arguments"].toString();
            if (raw.trimmed().isEmpty()) {
                rebuilt.append(call);
                continue;
            }
            QJsonParseError parseError{};
            QJsonDocument::fromJson(raw.toUtf8(), &parseError);
            if (parseError.error == QJsonParseError::NoError) {
                rebuilt.append(call);
                continue;
            }
            // id/type/name are kept: the assistant -> tool-result pairing and the
            // prompt prefix cache stay valid.
            function["arguments"] = repairedArgumentsHandle(raw);
            call["function"] = function;
            rebuilt.append(call);
            changed = true;
        }
        if (!changed) {
            out.append(value);
            continue;
        }
        QJsonObject repaired = message;
        repaired["tool_calls"] = rebuilt;
        out.append(repaired);
    }
    return out;
}

QJsonArray messagesForProvider(const QJsonArray& persisted, int keepTurns,
                               int maxDimension, double maxMb, int quality) {
    QVector<int> users;
    for (int i = 0; i < persisted.size(); ++i)
        if (persisted[i].toObject()["role"].toString() == "user") users.append(i);

    const int first = keepTurns == 0 ? 0 : qMax(0, users.size() - keepTurns);
    QSet<int> retained;
    for (int i = first; i < users.size(); ++i) retained.insert(users[i]);

    QJsonArray result;
    for (int i = 0; i < persisted.size(); ++i) {
        QJsonObject msg = persisted[i].toObject();
        const QJsonArray refs = msg["attachments"].toArray();
        msg.remove("attachments");

        if (msg["role"].toString() == "user" && retained.contains(i) && !refs.isEmpty()) {
            QJsonArray parts;
            for (const QJsonValue& ref : refs) {
                const QString url = attachmentImageDataUrl(ref.toObject(), maxDimension, maxMb, quality);
                if (!url.isEmpty())
                    parts.append(QJsonObject{{"type", "image_url"},
                                             {"image_url", QJsonObject{{"url", url}}}});
            }
            const QString text = msg["content"].toString();
            if (!text.isEmpty())
                parts.append(QJsonObject{{"type", "text"}, {"text", text}});
            if (!parts.isEmpty()) msg["content"] = parts;
        }
        result.append(msg);
    }
    return repairUnparseableArguments(result);
}
