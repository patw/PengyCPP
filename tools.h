#pragma once
#include <QString>
#include <QJsonObject>
#include <QJsonArray>
#include <atomic>

namespace Tools {

QJsonArray toolDefinitions();
bool       isReadOnly(const QString& name);
void       setUserAgent(const QString& ua);
void       setTimeout(int secs);
QString    execute(const QString& name, const QJsonObject& args,
                   std::atomic<bool>* cancel = nullptr);

} // namespace Tools
