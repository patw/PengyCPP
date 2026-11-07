#pragma once
// Chat search -- the GUI's view of the core.
//
// Shared verbatim between PengyR's GUI and PengyCPP (like chattitle.h); only
// run()'s implementation differs (chatsearch_ffi.cpp calls the Rust core,
// chatsearch.cpp is native in PengyCPP).  Rules match Python Pengy's
// pengy/core/chat_search.py and its spec.md ("Chat Search").
#include <QJsonObject>
#include <QList>
#include <QPair>
#include <QString>
#include <QStringList>
#include <functional>

namespace ChatSearch {

inline constexpr int kMaxResults = 100;

// Lowercased search words; a "quoted phrase" is one word.  Same tokenising as
// Python's  "([^"]*)"|(\S+) : a quote only opens a phrase when a closing quote
// follows, otherwise it is part of an ordinary word.  Used by the sidebar's
// title filter, so it lives here rather than behind run().
inline QStringList parseQuery(const QString& query) {
    QStringList terms;
    const int n = query.size();
    int i = 0;
    auto wordEnd = [&](int from) {
        while (from < n && !query.at(from).isSpace()) ++from;
        return from;
    };
    while (i < n) {
        if (query.at(i).isSpace()) { ++i; continue; }
        QString raw;
        int close = query.at(i) == QLatin1Char('"') ? query.indexOf(QLatin1Char('"'), i + 1) : -1;
        if (close >= 0) {
            raw = query.mid(i + 1, close - i - 1);
            i = close + 1;
        } else {
            int end = wordEnd(i);
            raw = query.mid(i, end - i);
            i = end;
        }
        QString term = raw.simplified().toLower();
        if (!term.isEmpty() && !terms.contains(term)) terms << term;
    }
    return terms;
}

// Snippet spans are [start, end) offsets in Unicode code points (Python str
// indexing).  QString is UTF-16, so convert before slicing.
inline int codePointToUtf16(const QString& s, int codePoint) {
    int units = 0;
    for (int cp = 0; cp < codePoint && units < s.size(); ++cp)
        units += (s.at(units).isHighSurrogate() && units + 1 < s.size()) ? 2 : 1;
    return units;
}

// Search every stored chat.  Blocking: call off the UI thread.  `onEvent` is
// called on the calling thread with each event, exactly as the Python edition
// yields them:
//   {"type":"result", id, title, created_at, title_match, hits:[{index, role, snippet, spans}]}
//   {"type":"progress", scanned, total}      after each chat file
//   {"type":"done", results, truncated, cancelled}   last
// Returning false from `onEvent` cancels the search before the next chat file.
void run(const QString& query, int limit,
         const std::function<bool(const QJsonObject&)>& onEvent);

}  // namespace ChatSearch
