// ChatTitle backed by the native core (see chattitle.h and titlegen.h).
#include "chattitle.h"
#include "chatmanager.h"
#include "config.h"
#include "titlegen.h"

namespace ChatTitle {

QJsonObject generate(const QJsonObject& chat) {
    const TitleResult r = generateTitle(configLoad(), chat);
    if (r.ok()) return {{"title", r.title}};
    return {{"error", r.error}, {"unusable", r.unusable}};
}

QJsonObject bulkStep(const QString& chatId, bool redo) {
    QJsonObject chat = chatGet(chatId);
    if (chat.isEmpty()) return {{"status", "skipped"}};
    if (!titleWantsRename(chat["title_source"].toString(), redo)) return {{"status", "kept"}};
    const TitleResult r = generateTitle(configLoad(), chat);
    const QString oldTitle = chat["title"].toString();
    chat = QJsonObject();  // some chats are hundreds of MB; don't hold two at once
    if (!r.ok()) {
        if (r.unusable) return {{"status", "skipped"}};
        return {{"status", "failed"}, {"error", r.error}};
    }
    // Written (and marked) even when unchanged, so the next run skips it.
    if (!chatSetTitle(chatId, r.title, kTitleSourceModel)) return {{"status", "skipped"}};
    return {{"status", r.title == oldTitle ? "unchanged" : "renamed"}, {"title", r.title}};
}

QStringList bulkTargets(bool redo) { return bulkRenameTargets(redo); }

}  // namespace ChatTitle
