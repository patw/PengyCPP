#pragma once
#include <QJsonObject>
#include <QJsonArray>
#include <QString>

// All data is represented as QJsonObject/QJsonArray to match the existing
// serialization format and minimize conversion boilerplate.

// Sidebar summaries only: id, title, created_at, msg_count, preview.
// Reads one small index file instead of every chat -- prefer this over
// chatsLoad() wherever message bodies aren't actually needed.
QJsonArray  chatsLoadIndex();
// Every chat in full, newest first. Reads every chat file.
QJsonArray  chatsLoad();
// No-op: there is no in-memory cache any more. Kept for API compatibility.
void        chatsInvalidateCache();
// Additive: writes and updates the given chats, never deletes others.
bool        chatsSave(const QJsonArray& chats);
QJsonObject chatCreate(const QString& title);
bool        chatDelete(const QString& id);
bool        chatSave(const QJsonObject& chat);
QJsonObject chatGet(const QString& id);
QJsonArray  cleanDanglingToolCalls(const QJsonArray& messages);
QJsonArray  elideOldToolResults(const QJsonArray& messages, int keepTurns);
// Context-pruning "undo": pops exactly one raw message off the end,
// repairing any dangling tool_calls it leaves behind. Safe to call
// repeatedly down to an empty array.
QJsonArray  messagesRedactLast(const QJsonArray& messages);
// Accumulates one turn's token usage into chat["usage"] (persisted running
// total, not session-only state) and returns the updated chat. Caller must
// still persist it (chatSave/chatSaveProgress).
QJsonObject chatAddUsage(QJsonObject chat, const QJsonObject& usage);
