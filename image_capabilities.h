#pragma once
#include "config.h"
#include <QCryptographicHash>
#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QMutex>
#include <QMutexLocker>
#include <QSaveFile>
#include <QSet>

// Shared cache schema/key with Rust and Python; one file per endpoint/model.
// Files never contain credentials or image data. Delete an entry to retry
// after the route's capabilities change. A failed save is remembered in-process.
namespace ImageCapabilities {
inline QMutex mutex;
inline QSet<QString> unsaved;
inline QString pathFor(const QString& baseUrl, const QString& model) {
    QString endpoint = baseUrl.trimmed();
    while (endpoint.endsWith('/')) endpoint.chop(1);
    const QByteArray key = QJsonDocument(QJsonArray{endpoint, model}).toJson(QJsonDocument::Compact);
    return pengyConfigDirPath() + "/image_rejections/" +
        QString::fromLatin1(QCryptographicHash::hash(key, QCryptographicHash::Sha256).toHex()) + ".json";
}
inline QString endpointFor(QString baseUrl) {
    baseUrl = baseUrl.trimmed();
    while (baseUrl.endsWith('/')) baseUrl.chop(1);
    return baseUrl;
}
inline bool imagesRejected(const QString& baseUrl, const QString& model) {
    QMutexLocker lock(&mutex);
    const QString path = pathFor(baseUrl, model);
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) return unsaved.contains(path);
    const auto doc = QJsonDocument::fromJson(file.readAll());
    if (!doc.isObject()) return unsaved.contains(path);
    const auto data = doc.object();
    return data["v"].toInt() == 1 && data["endpoint"].toString() == endpointFor(baseUrl)
        && data["model"].toString() == model;
}
inline void rememberImageRejection(const QString& baseUrl, const QString& model) {
    QMutexLocker lock(&mutex);
    const QString path = pathFor(baseUrl, model);
    QDir().mkpath(pengyConfigDirPath() + "/image_rejections");
    QSaveFile file(path);
    const QByteArray bytes = QJsonDocument(QJsonObject{{"v",1},{"endpoint",endpointFor(baseUrl)},
        {"model",model},{"rejected_at",QDateTime::currentSecsSinceEpoch()}}).toJson(QJsonDocument::Compact);
    if (file.open(QIODevice::WriteOnly) && file.write(bytes) == bytes.size() && file.commit())
        unsaved.remove(path);
    else unsaved.insert(path);
}
}
