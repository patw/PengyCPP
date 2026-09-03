#include "provider_messages.h"
#include "attachments.h"
#include <QJsonObject>
#include <QSet>

QJsonArray messagesForProvider(const QJsonArray& persisted, int keepTurns,
                               int maxDimension, double maxMb, int quality) {
    QVector<int> users;
    for (int i = 0; i < persisted.size(); ++i)
        if (persisted[i].toObject()["role"].toString() == "user") users.append(i);

    const int first = keepTurns == 0 ? 0 : qMax(0, users.size() - keepTurns);
    QSet<int> retained;
    for (int i = first; i < users.size(); ++i) retained.insert(users[i]);

    QJsonArray result;
    for (int i = 0; i < persisted.size(); ++i) {
        QJsonObject msg = persisted[i].toObject();
        const QJsonArray refs = msg["attachments"].toArray();
        msg.remove("attachments");

        if (msg["role"].toString() == "user" && retained.contains(i) && !refs.isEmpty()) {
            QJsonArray parts;
            for (const QJsonValue& ref : refs) {
                const QString url = attachmentImageDataUrl(ref.toObject(), maxDimension, maxMb, quality);
                if (!url.isEmpty())
                    parts.append(QJsonObject{{"type", "image_url"},
                                             {"image_url", QJsonObject{{"url", url}}}});
            }
            const QString text = msg["content"].toString();
            if (!text.isEmpty())
                parts.append(QJsonObject{{"type", "text"}, {"text", text}});
            if (!parts.isEmpty()) msg["content"] = parts;
        }
        result.append(msg);
    }
    return result;
}
