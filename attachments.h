#pragma once
#include <QJsonObject>
#include <QString>
#include <QByteArray>

// Durable attachment schema/storage v1. Chat JSON stores AttachmentRef objects;
// immutable source bytes and JPEG derivatives stay under <config>/attachments.
QJsonObject attachmentImportImage(const QString& sourcePath, const QString& displayName,
                                  int maxDimension = 4096, double maxMb = 4.5, int quality = 85);
QString attachmentObjectPath(const QString& id);
QString attachmentDerivativePath(const QString& id, const QString& name);
bool attachmentEnsureImageDerivatives(const QString& id, int maxDimension = 4096,
                                      double maxMb = 4.5, int quality = 85);
QString attachmentImageDataUrl(const QJsonObject& ref, int maxDimension = 4096,
                               double maxMb = 4.5, int quality = 85);
bool attachmentIdIsValid(const QString& id);
QString attachmentLabel(const QJsonObject& ref);QJsonObject attachmentStorageReport(const QJsonArray& chats);

