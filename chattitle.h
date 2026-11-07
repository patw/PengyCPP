#pragma once
// Chat titles for Edit Chat and Bulk Rename -- the GUI's view of the core.
//
// editchatdialog.{h,cpp} and bulkrename.{h,cpp} are shared verbatim between
// PengyR's GUI and PengyCPP; only this interface's implementation differs
// (chattitle_ffi.cpp calls the Rust core here, chattitle.cpp is native in
// PengyCPP).  Semantics match Python Pengy's pengy/core/chat_title.py.
//
// Every function that talks to the model blocks for the request: call it off
// the UI thread.
#include <QJsonObject>
#include <QString>
#include <QStringList>

namespace ChatTitle {

inline const QString kSourceModel = QStringLiteral("model");
inline const QString kSourceUser  = QStringLiteral("user");

// Generate a title for `chat` (the caller's live copy) with the saved default
// model.  Returns {"title": ...} or {"error": ..., "unusable": bool};
// `unusable` = nothing to name the chat from / an empty reply, not an
// endpoint failure.
QJsonObject generate(const QJsonObject& chat);

// Bulk Rename's per-chat step: load `chatId`, skip it unless its
// title_source wants renaming, generate a title and write it marked "model".
// Returns {"status": "renamed"|"unchanged"|"kept"|"skipped"|"failed",
// "title"?, "error"?}.
QJsonObject bulkStep(const QString& chatId, bool redo);

// IDs a Bulk Rename run would visit, newest first (chats with no messages
// and, unless `redo`, chats already titled are left out; "user" titles never
// are visited).
QStringList bulkTargets(bool redo);

}  // namespace ChatTitle
