#include "chatmanager.h"
#include <QFile>
#include <QDir>
#include <QFileInfo>
#include <QJsonDocument>
#include <QDateTime>
#include <QUuid>
#include <QSet>

/* Resolve the pengy config directory.
 * Uses $XDG_CONFIG_HOME if set, otherwise $HOME/.config — matching the
 * Python and Rust editions so all three share the same settings/chats/tasks. */
static QString pengyConfigDir() {
    QString base = qEnvironmentVariable("XDG_CONFIG_HOME");
    if (base.isEmpty())
        base = QDir::homePath() + "/.config";
    return base + "/pengy";
}

static QString chatsFilePath() {
    return pengyConfigDir() + "/chats.json";
}

static void backupCorruptFile(const QString& path) {
    QString ts = QString::number(QDateTime::currentSecsSinceEpoch());
    QFileInfo fi(path);
    QString backup = fi.dir().filePath(fi.baseName() + ".corrupt-" + ts);
    QFile::rename(path, backup);
}

QJsonArray chatsLoad() {
    QString path = chatsFilePath();
    QFile f(path);
    if (f.open(QIODevice::ReadOnly)) {
        QByteArray data = f.readAll();
        f.close();
        QJsonParseError err;
        QJsonDocument doc = QJsonDocument::fromJson(data, &err);
        if (err.error == QJsonParseError::NoError && doc.isArray())
            return doc.array();
        backupCorruptFile(path);
    }
    return QJsonArray();
}

bool chatsSave(const QJsonArray& chats) {
    QString path = chatsFilePath();
    QFileInfo fi(path);
    QDir().mkpath(fi.dir().absolutePath());

    QByteArray json = QJsonDocument(chats).toJson(QJsonDocument::Indented);
    QString tmp = path + ".tmp";
    QFile f(tmp);
    if (!f.open(QIODevice::WriteOnly | QIODevice::Truncate)) return false;
    f.write(json);
    f.close();
    QFile::remove(path);
    return QFile::rename(tmp, path);
}

QJsonObject chatCreate(const QString& title) {
    QJsonObject chat;
    chat["id"]         = QUuid::createUuid().toString(QUuid::WithoutBraces);
    chat["title"]      = title.isEmpty() ? "New Chat" : title;
    chat["messages"]   = QJsonArray();
    chat["created_at"] = QDateTime::currentDateTime().toString("yyyy-MM-ddTHH:mm:ss");

    QJsonArray chats = chatsLoad();
    chats.prepend(chat);
    chatsSave(chats);
    return chat;
}

bool chatDelete(const QString& id) {
    QJsonArray chats = chatsLoad();
    QJsonArray updated;
    for (const QJsonValue& v : chats) {
        if (v.toObject()["id"].toString() != id)
            updated.append(v);
    }
    return chatsSave(updated);
}

bool chatSave(const QJsonObject& chat) {
    QJsonArray chats = chatsLoad();
    QString id = chat["id"].toString();
    bool found = false;
    for (int i = 0; i < chats.size(); ++i) {
        if (chats[i].toObject()["id"].toString() == id) {
            chats[i] = chat;
            found = true;
            break;
        }
    }
    if (!found) chats.prepend(chat);
    return chatsSave(chats);
}

QJsonObject chatGet(const QString& id) {
    for (const QJsonValue& v : chatsLoad()) {
        QJsonObject c = v.toObject();
        if (c["id"].toString() == id)
            return c;
    }
    return QJsonObject();
}

QJsonArray cleanDanglingToolCalls(const QJsonArray& messages) {
    QJsonArray cleaned;
    QSet<QString> pendingIds;
    int i = 0;

    while (i < messages.size()) {
        QJsonObject msg = messages[i].toObject();
        ++i;

        if (msg["role"].toString() == "tool") {
            QString tcId = msg["tool_call_id"].toString();
            if (!tcId.isEmpty() && pendingIds.contains(tcId)) {
                pendingIds.remove(tcId);
                cleaned.append(msg);
            }
            // else: orphan tool message — drop it
            continue;
        }

        cleaned.append(msg);

        if (msg["role"].toString() == "assistant") {
            QJsonArray toolCalls = msg["tool_calls"].toArray();
            if (toolCalls.isEmpty()) continue;

            QSet<QString> tcIds;
            for (const QJsonValue& tc : toolCalls)
                tcIds.insert(tc.toObject()["id"].toString());
            pendingIds += tcIds;

            // Consume matching tool messages that follow
            while (i < messages.size() && messages[i].toObject()["role"].toString() == "tool") {
                QString tcId = messages[i].toObject()["tool_call_id"].toString();
                if (!tcId.isEmpty() && pendingIds.contains(tcId)) {
                    pendingIds.remove(tcId);
                    cleaned.append(messages[i]);
                    ++i;
                } else {
                    break;
                }
            }

            // Synthesize cancelled results for unresolved IDs
            QSet<QString> unsatisfied = tcIds & pendingIds;
            for (const QString& missingId : unsatisfied) {
                pendingIds.remove(missingId);
                QJsonObject synthetic;
                synthetic["role"]         = "tool";
                synthetic["tool_call_id"] = missingId;
                synthetic["content"]      = "Tool execution was cancelled by user.";
                cleaned.append(synthetic);
            }
        }
    }

    return cleaned;
}

QJsonArray elideOldToolResults(const QJsonArray& messages, int keepTurns) {
    if (keepTurns <= 0) return messages;

    // Collect indices of user messages (turn boundaries)
    QList<int> userIndices;
    for (int i = 0; i < messages.size(); ++i) {
        if (messages[i].toObject()["role"].toString() == "user")
            userIndices.append(i);
    }
    if (userIndices.isEmpty()) return messages;

    int numTurns = userIndices.size();
    QSet<int> recentIndices;
    for (int t = 0; t < numTurns; ++t) {
        int turnsFromEnd = numTurns - t;
        if (turnsFromEnd <= keepTurns) {
            int start = userIndices[t];
            int end   = (t + 1 < numTurns) ? userIndices[t + 1] : messages.size();
            for (int idx = start; idx < end; ++idx)
                recentIndices.insert(idx);
        }
    }

    QJsonArray result;
    for (int i = 0; i < messages.size(); ++i) {
        QJsonObject msg = messages[i].toObject();
        if (msg["role"].toString() == "tool" && !recentIndices.contains(i)) {
            msg["content"] = "[tool output from earlier turn elided]";
        }
        result.append(msg);
    }
    return result;
}
