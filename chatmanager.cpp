#include "chatmanager.h"
#include "config.h"
#include <QFile>
#include <QDir>
#include <QFileInfo>
#include <QJsonDocument>
#include <QDateTime>
#include <QUuid>
#include <QSet>
#include <QMutex>
#include <QMutexLocker>

static QString chatsFilePath() {
    return pengyConfigDirPath() + "/chats.json";
}

static void backupCorruptFile(const QString& path) {
    QString ts = QString::number(QDateTime::currentSecsSinceEpoch());
    QFileInfo fi(path);
    QString backup = fi.dir().filePath(fi.baseName() + ".corrupt-" + ts);
    QFile::rename(path, backup);
}

// ---------------------------------------------------------------------------
// in-memory cache
// ---------------------------------------------------------------------------
// chats.json is a single (potentially large) file that was fully re-parsed on
// every read: startup loads it, then chatGet() re-loads it; chatSave() loads
// it again before writing. We cache the parsed array keyed by the file's
// (lastModified, size). Any external writer (the CLI, or the Python/Rust
// editions sharing ~/.config/pengy/) bumps mtime and invalidates us.
static QMutex     g_cacheMutex;
static bool       g_cacheValid = false;
static qint64     g_cacheMTime = -1;
static qint64     g_cacheSize  = -1;
static QJsonArray g_cacheChats;

static void cacheStatLocked(const QString& path, qint64* mtime, qint64* size) {
    QFileInfo fi(path);
    if (fi.exists()) {
        *mtime = fi.lastModified().toMSecsSinceEpoch();
        *size  = fi.size();
    } else {
        *mtime = -1;
        *size  = -1;
    }
}

void chatsInvalidateCache() {
    QMutexLocker lock(&g_cacheMutex);
    g_cacheValid = false;
    g_cacheChats = QJsonArray();
}

QJsonArray chatsLoad() {
    QString path = chatsFilePath();

    QMutexLocker lock(&g_cacheMutex);
    qint64 mtime, size;
    cacheStatLocked(path, &mtime, &size);
    if (g_cacheValid && mtime == g_cacheMTime && size == g_cacheSize && mtime != -1)
        return g_cacheChats;   // QJsonArray is implicitly shared (copy-on-write)

    QFile f(path);
    if (f.open(QIODevice::ReadOnly)) {
        QByteArray data = f.readAll();
        f.close();
        QJsonParseError err;
        QJsonDocument doc = QJsonDocument::fromJson(data, &err);
        if (err.error == QJsonParseError::NoError && doc.isArray()) {
            g_cacheChats = doc.array();
            cacheStatLocked(path, &g_cacheMTime, &g_cacheSize);
            g_cacheValid = true;
            return g_cacheChats;
        }
        backupCorruptFile(path);
    }
    g_cacheChats = QJsonArray();
    g_cacheValid = false;
    return g_cacheChats;
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
    if (!QFile::rename(tmp, path)) {
        chatsInvalidateCache();
        return false;
    }
    // Prime the cache with what we just wrote so the next chatsLoad() (e.g. the
    // load->mutate->save cycle in chatSave) skips a re-parse.
    {
        QMutexLocker lock(&g_cacheMutex);
        g_cacheChats = chats;
        cacheStatLocked(path, &g_cacheMTime, &g_cacheSize);
        g_cacheValid = (g_cacheMTime != -1);
    }
    return true;
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
