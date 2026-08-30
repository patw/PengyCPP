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
#include <QPair>
#include <algorithm>

// ---------------------------------------------------------------------------
// storage layout
// ---------------------------------------------------------------------------
// Chats live one per file in <config>/chats/<id>.json.
//
// The previous layout was a single <config>/chats.json array, so every save
// rewrote the whole corpus. Per-chat files make saving and opening proportional
// to the chat you touched instead of to everything you have ever said.
//
// <config>/chats/index.json caches the sidebar summary (id, title, created_at,
// message count, preview) so listing chats is one small read instead of one per
// chat. It is a *cache*, never the source of truth: if it is missing, stale,
// corrupt, or loses a race between two frontends, it is rebuilt by scanning the
// directory. The per-chat files are authoritative.
//
// The legacy chats.json is still read, so a machine that switches between the
// Python, Rust and C++ editions doesn't appear to lose history. It is never
// written and never deleted.

static const char* kIndexFile   = "index.json";
static const int   kIndexVersion = 1;
static const int   kPreviewChars = 200;

// Serialises index read-modify-write within this process. Across processes the
// id-set check in ensureCurrent() repairs whatever a lost race dropped.
static QMutex g_indexMutex;

// The pre-split single-file store. Read-only; never written or removed.
static QString legacyFilePath() {
    return pengyConfigDirPath() + "/chats.json";
}

static QString chatsDirPath() {
    return pengyConfigDirPath() + "/chats";
}

static QString chatFilePath(const QString& id) {
    return chatsDirPath() + "/" + id + ".json";
}

static QString indexFilePath() {
    return chatsDirPath() + "/" + QString::fromLatin1(kIndexFile);
}

static void backupCorruptFile(const QString& path) {
    QString ts = QString::number(QDateTime::currentSecsSinceEpoch());
    QFileInfo fi(path);
    QString backup = fi.dir().filePath(fi.fileName() + ".corrupt-" + ts);
    QFile::rename(path, backup);
}

// (lastModified ms, size); mtime -1 means the file doesn't exist.
static QPair<qint64, qint64> statKey(const QString& path) {
    QFileInfo fi(path);
    if (!fi.exists()) return {-1, -1};
    return {fi.lastModified().toMSecsSinceEpoch(), fi.size()};
}

// Write JSON atomically (temp file + rename).
static bool atomicWrite(const QString& path, const QJsonDocument& doc) {
    QFileInfo fi(path);
    QDir().mkpath(fi.dir().absolutePath());
    QString tmp = path + ".tmp";
    QFile f(tmp);
    if (!f.open(QIODevice::WriteOnly | QIODevice::Truncate)) return false;
    f.write(doc.toJson(QJsonDocument::Indented));
    f.close();
    QFile::remove(path);
    if (!QFile::rename(tmp, path)) {
        QFile::remove(tmp);
        return false;
    }
    return true;
}

// Read and parse a JSON file, moving it aside if it is corrupt.
// Returns an undefined document when the file is missing or unreadable.
static QJsonDocument readJson(const QString& path) {
    QFile f(path);
    if (!f.open(QIODevice::ReadOnly)) return QJsonDocument();
    QByteArray data = f.readAll();
    f.close();
    QJsonParseError err;
    QJsonDocument doc = QJsonDocument::fromJson(data, &err);
    if (err.error != QJsonParseError::NoError) {
        backupCorruptFile(path);
        return QJsonDocument();
    }
    return doc;
}

// ---------------------------------------------------------------------------
// summaries & index
// ---------------------------------------------------------------------------

// First user message, truncated -- what /list and the sidebar show.
static QString previewOf(const QJsonObject& chat) {
    const QJsonArray msgs = chat["messages"].toArray();
    for (const QJsonValue& v : msgs) {
        QJsonObject m = v.toObject();
        if (m["role"].toString() != "user") continue;
        QJsonValue content = m["content"];
        QString text;
        if (content.isString()) {
            text = content.toString();
        } else if (content.isArray()) {
            // Multipart (image) content: use the first text part.
            for (const QJsonValue& pv : content.toArray()) {
                QJsonObject part = pv.toObject();
                if (part["type"].toString() == "text") {
                    text = part["text"].toString();
                    break;
                }
            }
        }
        return text.left(kPreviewChars);
    }
    return QString();
}

static QJsonObject summarize(const QJsonObject& chat) {
    QJsonObject e;
    e["id"]         = chat["id"].toString();
    e["title"]      = chat["title"].toString("Untitled");
    e["created_at"] = chat["created_at"].toString();
    e["msg_count"]  = chat["messages"].toArray().size();
    e["preview"]    = previewOf(chat);
    return e;
}

// Newest first. created_at is unique in practice; id breaks ties.
static bool newestFirst(const QJsonValue& a, const QJsonValue& b) {
    QJsonObject oa = a.toObject(), ob = b.toObject();
    QString ca = oa["created_at"].toString(), cb = ob["created_at"].toString();
    if (ca != cb) return ca > cb;
    return oa["id"].toString() > ob["id"].toString();
}

static QJsonArray sortedNewestFirst(QJsonArray arr) {
    QList<QJsonValue> items;
    items.reserve(arr.size());
    for (const QJsonValue& v : arr) items.append(v);
    std::sort(items.begin(), items.end(), newestFirst);
    QJsonArray out;
    for (const QJsonValue& v : items) out.append(v);
    return out;
}

// ids of the per-chat files, from one directory read.
static QSet<QString> chatIdsOnDisk() {
    QSet<QString> ids;
    QDir dir(chatsDirPath());
    if (!dir.exists()) return ids;
    const QStringList names = dir.entryList(QStringList() << "*.json", QDir::Files);
    for (const QString& name : names) {
        if (name == QString::fromLatin1(kIndexFile)) continue;
        ids.insert(name.left(name.size() - 5));  // strip ".json"
    }
    return ids;
}

// Read every per-chat file. The fallback when the index can't be trusted.
static QJsonArray scanChats() {
    QJsonArray chats;
    const QSet<QString> ids = chatIdsOnDisk();
    for (const QString& id : ids) {
        QJsonDocument doc = readJson(chatFilePath(id));
        if (doc.isObject() && !doc.object()["id"].toString().isEmpty())
            chats.append(doc.object());
    }
    return sortedNewestFirst(chats);
}

// Returns an empty object when the index is missing, corrupt or the wrong version.
static QJsonObject readIndex() {
    QJsonDocument doc = readJson(indexFilePath());
    if (!doc.isObject()) return QJsonObject();
    QJsonObject o = doc.object();
    if (o["version"].toInt() != kIndexVersion) return QJsonObject();
    return o;
}

static void writeIndex(const QJsonArray& entries, const QPair<qint64, qint64>& legacySeen) {
    QJsonObject o;
    o["version"] = kIndexVersion;
    QJsonArray seen;
    seen.append(static_cast<double>(legacySeen.first));
    seen.append(static_cast<double>(legacySeen.second));
    o["legacy_seen"] = seen;
    o["chats"] = sortedNewestFirst(entries);
    atomicWrite(indexFilePath(), QJsonDocument(o));
}

static QPair<qint64, qint64> indexLegacySeen(const QJsonObject& idx) {
    QJsonArray seen = idx["legacy_seen"].toArray();
    if (seen.size() != 2) return {-1, -1};
    return {static_cast<qint64>(seen[0].toDouble()),
            static_cast<qint64>(seen[1].toDouble())};
}

// Regenerate the index from the authoritative per-chat files.
static QJsonArray rebuildIndex(const QPair<qint64, qint64>& legacySeen) {
    QJsonArray entries;
    for (const QJsonValue& v : scanChats()) entries.append(summarize(v.toObject()));
    entries = sortedNewestFirst(entries);
    writeIndex(entries, legacySeen);
    return entries;
}

// Copy chats.json entries that have no per-chat file yet.
// Existing per-chat files always win -- this only ever adds.
static void importLegacy() {
    QJsonDocument doc = readJson(legacyFilePath());
    if (!doc.isArray()) return;
    const QSet<QString> have = chatIdsOnDisk();
    for (const QJsonValue& v : doc.array()) {
        QJsonObject chat = v.toObject();
        QString id = chat["id"].toString();
        if (id.isEmpty() || have.contains(id)) continue;
        atomicWrite(chatFilePath(id), QJsonDocument(chat));
    }
}

// Bring the index in line with disk, then return its entries.
//
// Steady state is one directory read plus one small parse. The expensive paths
// (importing chats.json, rescanning every chat) run only when the cheap checks
// say something actually changed.
static QJsonArray ensureCurrent() {
    QMutexLocker lock(&g_indexMutex);
    QDir().mkpath(chatsDirPath());

    QJsonObject idx = readIndex();
    QPair<qint64, qint64> legacyNow = statKey(legacyFilePath());

    // chats.json appeared or was rewritten -- most likely by the Python or Rust
    // edition on a machine that runs more than one. Re-import so its chats
    // become visible here.
    if (legacyNow.first != -1 && (idx.isEmpty() || indexLegacySeen(idx) != legacyNow)) {
        importLegacy();
        return rebuildIndex(legacyNow);
    }

    if (idx.isEmpty() || !idx.contains("chats"))
        return rebuildIndex(legacyNow);

    // The index is a cache: if it disagrees with the directory (a frontend
    // crashed mid-write, or two raced on index.json), rebuild from files.
    QJsonArray entries = idx["chats"].toArray();
    QSet<QString> indexed;
    for (const QJsonValue& v : entries) indexed.insert(v.toObject()["id"].toString());
    if (indexed != chatIdsOnDisk())
        return rebuildIndex(legacyNow);

    return entries;
}

// Insert or replace summaries without rescanning everything.
static void updateIndexEntries(const QJsonArray& chats) {
    if (chats.isEmpty()) return;
    QMutexLocker lock(&g_indexMutex);

    QJsonObject idx = readIndex();
    QPair<qint64, qint64> legacySeen =
        idx.isEmpty() ? statKey(legacyFilePath()) : indexLegacySeen(idx);

    QJsonArray entries;
    if (idx.isEmpty() || !idx.contains("chats")) {
        for (const QJsonValue& v : scanChats()) entries.append(summarize(v.toObject()));
    } else {
        QSet<QString> ids;
        for (const QJsonValue& v : chats) ids.insert(v.toObject()["id"].toString());
        for (const QJsonValue& v : idx["chats"].toArray()) {
            if (!ids.contains(v.toObject()["id"].toString()))
                entries.append(v);
        }
        for (const QJsonValue& v : chats) entries.append(summarize(v.toObject()));
    }
    writeIndex(entries, legacySeen);
}

static void dropIndexEntry(const QString& id) {
    QMutexLocker lock(&g_indexMutex);
    QJsonObject idx = readIndex();
    if (idx.isEmpty() || !idx.contains("chats")) return;
    QJsonArray entries;
    for (const QJsonValue& v : idx["chats"].toArray()) {
        if (v.toObject()["id"].toString() != id)
            entries.append(v);
    }
    writeIndex(entries, indexLegacySeen(idx));
}

// ---------------------------------------------------------------------------
// public API
// ---------------------------------------------------------------------------

QJsonArray chatsLoadIndex() {
    return ensureCurrent();
}

QJsonArray chatsLoad() {
    ensureCurrent();
    return scanChats();
}

void chatsInvalidateCache() {
    // No in-memory cache any more: reads go straight to the per-chat files,
    // and index.json is validated against the directory on every load. Kept so
    // callers (and the other editions' API surface) don't have to change.
}

bool chatsSave(const QJsonArray& chats) {
    // Additive on purpose: it writes and updates, but never deletes. The old
    // whole-array rewrite made "save this list" and "delete everything else"
    // the same operation. Use chatDelete() to remove a chat.
    QDir().mkpath(chatsDirPath());
    QJsonArray written;
    for (const QJsonValue& v : chats) {
        QJsonObject chat = v.toObject();
        QString id = chat["id"].toString();
        if (id.isEmpty()) continue;
        if (!atomicWrite(chatFilePath(id), QJsonDocument(chat))) return false;
        written.append(chat);
    }
    updateIndexEntries(written);
    return true;
}

QJsonObject chatCreate(const QString& title) {
    QJsonObject chat;
    chat["id"]         = QUuid::createUuid().toString(QUuid::WithoutBraces);
    chat["title"]      = title.isEmpty() ? "New Chat" : title;
    chat["messages"]   = QJsonArray();
    chat["created_at"] = QDateTime::currentDateTime().toString("yyyy-MM-ddTHH:mm:ss");

    ensureCurrent();
    QDir().mkpath(chatsDirPath());
    atomicWrite(chatFilePath(chat["id"].toString()), QJsonDocument(chat));
    QJsonArray one;
    one.append(chat);
    updateIndexEntries(one);
    return chat;
}

bool chatDelete(const QString& id) {
    ensureCurrent();
    QFile::remove(chatFilePath(id));
    dropIndexEntry(id);
    return true;
}

bool chatSave(const QJsonObject& chat) {
    QString id = chat["id"].toString();
    if (id.isEmpty()) return false;
    QDir().mkpath(chatsDirPath());
    if (!atomicWrite(chatFilePath(id), QJsonDocument(chat))) return false;
    QJsonArray one;
    one.append(chat);
    updateIndexEntries(one);
    return true;
}

QJsonObject chatGet(const QString& id) {
    QJsonDocument doc = readJson(chatFilePath(id));
    if (doc.isObject()) return doc.object();
    // Not split out yet (first run after upgrade, or written by another
    // edition): fall back to the legacy store.
    ensureCurrent();
    doc = readJson(chatFilePath(id));
    return doc.isObject() ? doc.object() : QJsonObject();
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

// Drop the last message and repair any dangling tool_calls it leaves behind.
//
// One "redact" call pops exactly one raw message off the end -- a tool
// result, an assistant tool_calls request, or a final response.
//
// Popping a tool result can't just fall through to cleanDanglingToolCalls():
// that function *synthesizes* a "cancelled" placeholder for any tool_calls
// entry left unsatisfied, so a naive pop-then-repair would regenerate an
// equivalent stub every time and redaction could never advance past it.
// Instead the popped tool_call_id is struck directly from its assistant
// message's tool_calls array; if that empties the array (and the assistant
// left no other text), the now-empty assistant message is dropped too, so
// redacting a single-tool-call round removes it in one call, not two.
//
// Safe to call repeatedly down to an empty array.
QJsonArray messagesRedactLast(const QJsonArray& messages) {
    if (messages.isEmpty()) return QJsonArray();

    QJsonArray working = messages;
    QJsonObject last = working.last().toObject();
    working.removeLast();

    if (last["role"].toString() == "tool") {
        QString lastId = last["tool_call_id"].toString();
        if (!lastId.isEmpty()) {
            // Find the assistant message that declared this tool_call_id --
            // not necessarily the immediately preceding message, since a
            // multi-tool round has several tool results trailing one
            // assistant message.
            for (int i = working.size() - 1; i >= 0; --i) {
                QJsonObject candidate = working[i].toObject();
                if (candidate["role"].toString() != "assistant") continue;

                QJsonArray toolCalls = candidate["tool_calls"].toArray();
                bool owns = false;
                QJsonArray remaining;
                for (const QJsonValue& tc : toolCalls) {
                    if (tc.toObject()["id"].toString() == lastId) {
                        owns = true;
                    } else {
                        remaining.append(tc);
                    }
                }
                if (owns) {
                    if (!remaining.isEmpty()) {
                        candidate["tool_calls"] = remaining;
                        working[i] = candidate;
                    } else if (!candidate["content"].toString().isEmpty()) {
                        candidate.remove("tool_calls");
                        working[i] = candidate;
                    } else {
                        working.removeAt(i);
                    }
                }
                break;
            }
        }
    }

    return cleanDanglingToolCalls(working);
}

// Accumulate one turn's token usage into the chat's running total.
//
// LlmClient's "final_response" event reports usage for that turn only
// (reset every call), so without this each frontend only ever showed the
// *last* turn's numbers -- no signal for how much context a long-running
// chat has actually burned through. Stored on chat["usage"] (not
// session/tab-only state) so the running total persists across reloads and
// is visible from any frontend, not just the process that made the call.
// Returns the updated chat (with chat["usage"] set); the caller is
// responsible for persisting it via chatSave()/chatSaveProgress().
QJsonObject chatAddUsage(QJsonObject chat, const QJsonObject& usage) {
    QJsonObject totals = chat["usage"].toObject();
    totals["prompt_tokens"]     = totals["prompt_tokens"].toInt() + usage["prompt_tokens"].toInt();
    totals["completion_tokens"] = totals["completion_tokens"].toInt() + usage["completion_tokens"].toInt();
    totals["total_tokens"]      = totals["total_tokens"].toInt() + usage["total_tokens"].toInt();
    chat["usage"] = totals;
    return chat;
}
