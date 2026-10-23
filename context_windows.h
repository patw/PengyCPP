#pragma once
#include <QJsonObject>
#include <QString>
#include <functional>
#include <optional>
namespace ContextRecovery {
struct Options;
}
namespace ContextWindows {
struct Window {
  quint64 tokens;
  bool inputOnly = false;
};
std::optional<Window> modelWindow(const QJsonObject &body,
                                  const QString &model);
std::optional<Window> errorWindow(const QString &message);
std::optional<Window> rememberError(const QString &base, const QString &model,
                                    const QString &message);
std::optional<Window> resolve(const QString &base, const QString &model,
                              const QString &key,
                              const ContextRecovery::Options &options,
                              const std::function<bool()> &cancelled);
} // namespace ContextWindows
