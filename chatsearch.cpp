// Chat search, native core (see chatsearch.h / chatsearchcore.h).  Port of
// Python Pengy's pengy/core/chat_search.py; the rules are pinned in its
// spec.md ("Chat Search") and must stay identical.  Positions are worked out
// in code points (QString::toUcs4) because snippet spans are code points.
#include "chatsearchcore.h"
#include "chatmanager.h"

#include <QJsonDocument>
#include <QRegularExpression>
#include <QSet>
#include <algorithm>

namespace ChatSearch {

namespace {

constexpr int kLeadChars = 30;

const QRegularExpression& markupRe() {
    // Markdown markers that only add noise to a one-line snippet.
    static const QRegularExpression re(QStringLiteral("```[\\w+-]*|\\*\\*|__|`"),
                                       QRegularExpression::UseUnicodePropertiesOption);
    return re;
}

const QRegularExpression& wsRe() {
    static const QRegularExpression re(QStringLiteral("\\s+"),
                                       QRegularExpression::UseUnicodePropertiesOption);
    return re;
}

using Chars = QList<uint>;

int findChars(const Chars& hay, const Chars& needle, int from) {
    if (needle.isEmpty() || needle.size() > hay.size()) return -1;
    for (int i = from; i + needle.size() <= hay.size(); ++i) {
        if (std::equal(needle.begin(), needle.end(), hay.begin() + i)) return i;
    }
    return -1;
}

QString fromChars(const Chars& chars, int start, int end) {
    return QString::fromUcs4(reinterpret_cast<const char32_t*>(chars.constData() + start), end - start);
}

bool plainAscii(const QString& term) {
    for (QChar c : term) {
        if (c.unicode() > 127 || c == QLatin1Char('"') || c == QLatin1Char('\\')) return false;
    }
    return true;
}

}  // namespace

QString textOf(const QJsonValue& content) {
    if (content.isString()) return content.toString();
    if (!content.isArray()) return {};
    QStringList parts;
    for (const QJsonValue& part : content.toArray()) {
        if (part.isObject() && part.toObject().value("type").toString() == QLatin1String("text"))
            parts << part.toObject().value("text").toString();
    }
    return parts.join(QLatin1Char(' '));
}

bool couldMatch(const QByteArray& raw, const QStringList& terms) {
    const QByteArray lowered = raw.toLower();  // ASCII-only, like the words it checks
    for (const QString& term : terms) {
        if (plainAscii(term) && !lowered.contains(term.toLatin1())) return false;
    }
    return true;
}

Snippet makeSnippet(const QString& text, const QStringList& terms) {
    QString flatText = text;
    flatText.remove(markupRe());
    flatText = flatText.replace(wsRe(), QStringLiteral(" ")).trimmed();
    const Chars flat = flatText.toUcs4();
    const Chars lowered = flatText.toLower().toUcs4();
    QList<Chars> termChars;
    for (const QString& t : terms) termChars << t.toUcs4();

    // Lowercasing can change a string's length (e.g. "İ"); positions are then
    // unreliable, so fall back to the start of the text.
    int first = -1;
    if (lowered.size() == flat.size()) {
        for (const Chars& t : termChars) {
            int p = findChars(lowered, t, 0);
            if (p >= 0 && (first < 0 || p < first)) first = p;
        }
    }
    if (first < 0) first = 0;
    int start = std::max(0, first - kLeadChars);
    const int end = std::min(int(flat.size()), start + kSnippetChars);
    start = std::max(0, std::min(start, end - kSnippetChars));

    const QString prefix = start > 0 ? QStringLiteral("…") : QString();
    const QString suffix = end < flat.size() ? QStringLiteral("…") : QString();
    const QString body = fromChars(flat, start, end);
    const Chars low = body.toLower().toUcs4();
    QList<QPair<int, int>> spans;
    if (low.size() == end - start) {
        const int offset = prefix.isEmpty() ? 0 : 1;
        for (const Chars& t : termChars) {
            for (int i = findChars(low, t, 0); i >= 0; i = findChars(low, t, i + t.size()))
                spans.append({i + offset, i + offset + int(t.size())});
        }
    }
    std::sort(spans.begin(), spans.end());
    Snippet out{prefix + body + suffix, {}};
    for (const auto& [s, e] : spans) out.spans.append(QJsonArray{s, e});
    return out;
}

std::optional<QJsonArray> matchChat(const QJsonObject& chat, const QStringList& terms) {
    const QString title = chat.value("title").toString().toLower();
    QSet<QString> remaining;
    for (const QString& t : terms) {
        if (!title.contains(t)) remaining.insert(t);
    }
    struct Candidate { int present; int index; QString role; QString text; };
    QList<Candidate> candidates;
    const QJsonArray messages = chat.value("messages").toArray();
    for (int index = 0; index < messages.size(); ++index) {
        const QJsonObject msg = messages.at(index).toObject();
        const QString role = msg.value("role").toString();
        if (role != QLatin1String("user") && role != QLatin1String("assistant")) continue;
        const QString text = textOf(msg.value("content"));
        if (text.isEmpty()) continue;
        const QString lowered = text.toLower();
        int present = 0;
        for (const QString& t : terms) {
            if (lowered.contains(t)) { ++present; remaining.remove(t); }
        }
        if (present) candidates.append({present, index, role, text});
    }
    if (!remaining.isEmpty()) return std::nullopt;
    std::sort(candidates.begin(), candidates.end(), [](const Candidate& a, const Candidate& b) {
        return a.present != b.present ? a.present > b.present : a.index < b.index;
    });
    if (candidates.size() > kMaxHits) candidates.resize(kMaxHits);
    std::sort(candidates.begin(), candidates.end(),
              [](const Candidate& a, const Candidate& b) { return a.index < b.index; });
    QJsonArray hits;
    for (const Candidate& c : candidates) {
        const Snippet snippet = makeSnippet(c.text, terms);
        hits.append(QJsonObject{{"index", c.index}, {"role", c.role},
                                {"snippet", snippet.text}, {"spans", snippet.spans}});
    }
    return hits;
}

void run(const QString& query, int limit, const std::function<bool(const QJsonObject&)>& onEvent) {
    const QStringList terms = parseQuery(query);
    if (limit <= 0) limit = kMaxResults;
    auto done = [](int results, bool truncated, bool cancelled) {
        return QJsonObject{{"type", "done"}, {"results", results},
                           {"truncated", truncated}, {"cancelled", cancelled}};
    };
    if (terms.isEmpty()) {
        onEvent(done(0, false, false));
        return;
    }
    const QJsonArray index = chatsLoadIndex();
    int results = 0;
    bool keepGoing = true;

    QSet<QString> titleIds;
    for (const QJsonValue& v : index) {
        const QJsonObject entry = v.toObject();
        const QString title = entry.value("title").toString();
        const QString lowered = title.toLower();
        if (!std::all_of(terms.begin(), terms.end(), [&](const QString& t) { return lowered.contains(t); }))
            continue;
        if (results >= limit) {
            onEvent(done(results, true, false));
            return;
        }
        titleIds.insert(entry.value("id").toString());
        ++results;
        keepGoing = onEvent(QJsonObject{{"type", "result"}, {"id", entry.value("id")}, {"title", title},
                                        {"created_at", entry.value("created_at").toString()},
                                        {"title_match", true}, {"hits", QJsonArray()}}) && keepGoing;
    }

    QList<QJsonObject> candidates;
    for (const QJsonValue& v : index) {
        const QJsonObject entry = v.toObject();
        if (!titleIds.contains(entry.value("id").toString()) && entry.value("msg_count").toInt(1))
            candidates << entry;
    }
    for (int n = 0; n < candidates.size(); ++n) {
        if (!keepGoing) {
            onEvent(done(results, false, true));
            return;
        }
        const QString id = candidates[n].value("id").toString();
        const QByteArray raw = chatReadRaw(id);
        if (!raw.isEmpty() && couldMatch(raw, terms)) {
            const QJsonDocument doc = QJsonDocument::fromJson(raw);
            const QJsonObject chat = doc.object();
            if (doc.isObject()) {
                if (auto hits = matchChat(chat, terms)) {
                    if (results >= limit) {
                        onEvent(done(results, true, false));
                        return;
                    }
                    ++results;
                    keepGoing = onEvent(QJsonObject{
                        {"type", "result"}, {"id", id}, {"title", chat.value("title").toString()},
                        {"created_at", chat.value("created_at").toString()},
                        {"title_match", false}, {"hits", *hits}}) && keepGoing;
                }
            }
        }
        keepGoing = onEvent(QJsonObject{{"type", "progress"}, {"scanned", n + 1},
                                        {"total", int(candidates.size())}}) && keepGoing;
    }
    onEvent(done(results, false, false));
}

}  // namespace ChatSearch
