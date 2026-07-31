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
