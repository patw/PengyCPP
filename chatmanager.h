#pragma once
#include <QJsonObject>
#include <QJsonArray>
#include <QString>

// All data is represented as QJsonObject/QJsonArray to match the existing
// serialization format and minimize conversion boilerplate.

QJsonArray  chatsLoad();
// Drop the in-memory chats cache (forces a re-read on next chatsLoad).
void        chatsInvalidateCache();
bool        chatsSave(const QJsonArray& chats);
QJsonObject chatCreate(const QString& title);
bool        chatDelete(const QString& id);
bool        chatSave(const QJsonObject& chat);
QJsonObject chatGet(const QString& id);
QJsonArray  cleanDanglingToolCalls(const QJsonArray& messages);
QJsonArray  elideOldToolResults(const QJsonArray& messages, int keepTurns);
