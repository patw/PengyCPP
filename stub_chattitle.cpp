// Canned ChatTitle for the shared Edit Chat / Bulk Rename dialog tests, so
// they need neither a model endpoint nor the edition's core.  Chat IDs pick
// the outcome: "ok*" renamed, "same*" unchanged, "kept*" kept, "empty*"
// skipped, "fail*" failed.
#include "chattitle.h"

#include <QJsonArray>

namespace ChatTitle {

QJsonObject generate(const QJsonObject& chat) {
    if (chat["messages"].toArray().isEmpty())
        return {{"error", "This chat has no messages to generate a title from yet."}, {"unusable", true}};
    return {{"title", "Generated Title"}};
}

QJsonObject bulkStep(const QString& id, bool) {
    if (id.startsWith("ok")) return {{"status", "renamed"}, {"title", "Title " + id}};
    if (id.startsWith("same")) return {{"status", "unchanged"}, {"title", "Same"}};
    if (id.startsWith("kept")) return {{"status", "kept"}};
    if (id.startsWith("empty")) return {{"status", "skipped"}};
    return {{"status", "failed"}, {"error", "endpoint down"}};
}

QStringList bulkTargets(bool redo) { return redo ? QStringList{"a", "b", "c"} : QStringList{"a"}; }

}  // namespace ChatTitle
