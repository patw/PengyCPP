#pragma once
// Chat search internals (PengyCPP's native core), exposed for the web
// server, the CLI and the tests.  The GUI only needs chatsearch.h.
#include "chatsearch.h"
#include <QByteArray>
#include <QJsonArray>
#include <QJsonObject>
#include <QJsonValue>
#include <optional>

namespace ChatSearch {

inline constexpr int kMaxHits = 3;
inline constexpr int kSnippetChars = 80;

// The text of a message's content: a string, or the text parts of a list.
QString textOf(const QJsonValue& content);

// Cheap pre-check on a chat file's bytes before a JSON parse.  Only plain
// ASCII words without " or \ are checked: the Python edition writes non-ASCII
// as \uXXXX escapes and every edition escapes quotes and backslashes.
bool couldMatch(const QByteArray& raw, const QStringList& terms);

struct Snippet {
    QString text;
    QJsonArray spans;  // [[start, end), ...] in code points
};
// ~kSnippetChars of `text` around the first matching word, Markdown markers
// stripped and whitespace collapsed, plus the spans of every word inside it.
Snippet makeSnippet(const QString& text, const QStringList& terms);

// Hits [{index, role, snippet, spans}] for `chat`, or nullopt if some word
// appears nowhere in it.  Up to kMaxHits, preferring messages holding more
// of the words, then earlier ones; returned in message order.
std::optional<QJsonArray> matchChat(const QJsonObject& chat, const QStringList& terms);

}  // namespace ChatSearch
