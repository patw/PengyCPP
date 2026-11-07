#pragma once
// Generate a short chat title with the configured model.
//
// Port of Python Pengy's pengy/core/chat_title.py; prompt, limits and cleanup
// rules are identical across editions.  Used by the GUI's Edit Chat dialog and
// Bulk Rename (through chattitle.cpp), the web Edit Chat modal and /title (web
// + CLI).  One plain, tool-free completion: the conversation's text (user and
// assistant turns only, truncated) goes in, a single line comes out.
#include "config.h"

#include <QJsonObject>
#include <QString>
#include <QStringList>

extern const char* const kTitlePrompt;
constexpr int kTitleSourceMaxChars = 6000;
constexpr int kTitleMaxChars = 80;

struct TitleResult {
    QString title;
    QString error;
    // Nothing to name the chat from, or an empty reply -- not the endpoint's
    // fault, so Bulk Rename skips the chat instead of counting a failure.
    bool unusable = false;
    bool ok() const { return error.isEmpty(); }
};

// The conversation text a title is drawn from, or "" if there is none.  Tool
// calls and results are skipped; the text is cut from the end.
QString titleSourceText(const QJsonObject& chat);

// Reduce a model reply to one title line (think blocks, quotes, prefixes removed).
QString cleanTitle(const QString& raw);

// Ask the configured model for a title for `chat`.  Blocks for the request.
TitleResult generateTitle(const Config& config, const QJsonObject& chat);

// Whether a chat whose title came from `source` is renamed by Bulk Rename:
// "user" titles never are; "model" ones only when redoing everything.
bool titleWantsRename(const QString& source, bool redo);

// IDs of the chats a Bulk Rename run would rename, newest first (chats with no
// messages are left out).  Built from the index, which can lag the chat files,
// so the caller re-checks each chat's own title_source before a request.
QStringList bulkRenameTargets(bool redo);
