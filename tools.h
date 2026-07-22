#pragma once
#include <QString>
#include <QJsonObject>
#include <QJsonArray>
#include <atomic>
#include <functional>

namespace Tools {

const QJsonArray& toolDefinitions();
bool       isReadOnly(const QString& name);
void       setUserAgent(const QString& ua);
void       setTimeout(int secs);
QString    execute(const QString& name, const QJsonObject& args,
                   std::atomic<bool>* cancel = nullptr);

void       killActiveProcesses();

/// Blocking callback that prompts the user for a sudo password.
/// Returns the password, or an empty string if the user cancels.
using SudoPasswordFn = std::function<QString()>;
void setSudoPasswordProvider(SudoPasswordFn fn);
void clearSudoPasswordProvider();

} // namespace Tools
