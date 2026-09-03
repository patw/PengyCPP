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
