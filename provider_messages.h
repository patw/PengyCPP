#pragma once
#include <QJsonArray>

// Convert persisted chat messages into provider-safe messages. Local-only
// attachment refs are always removed; image bytes are included only for the
// configured number of most-recent user turns (0 means all turns).
QJsonArray messagesForProvider(const QJsonArray& persistedMessages,
                              int attachmentKeepTurns,
                              int maxDimension,
                              double maxMb,
                              int quality);

// Replace tool-call arguments that are not valid JSON with a stable handle
// (character count, SHA-256, 160-character preview), keeping each call's
// id/type/name. Called by messagesForProvider: a stored, unparseable arguments
// string makes a strict engine reject the whole request, so it must never be
// replayed. The persisted transcript is not modified.
QJsonArray repairUnparseableArguments(const QJsonArray& messages);
