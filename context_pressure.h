#pragma once
#include <QJsonArray>
#include <QJsonObject>
#include <QString>
#include <QStringList>
#include <optional>
#include <utility>

namespace ContextPressure {
QString identity(const QJsonObject &state);
std::optional<quint64> inputChars(const QJsonValue &messages,
                                  const QJsonValue &tools);
std::pair<quint64, quint64> budgets(quint64 window, int percent, quint64 output,
                                    bool inputOnly = false);
class Pressure {
public:
  quint64 window = 0;
  std::optional<std::pair<quint64, quint64>> sample;
  static Pressure load(const QString &key, const QJsonArray &messages,
                       const QString &view, quint64 window);
  void record(std::optional<quint64> chars, std::optional<quint64> tokens);
  std::optional<quint64> estimate(std::optional<quint64> chars) const;
  bool shouldCompact(std::optional<quint64> estimate, quint64 threshold,
                     quint64 hard) const;
  void compacted(std::optional<quint64> after, quint64 threshold);
  void setWindow(quint64 value);
  void save(const QString &key, const QJsonArray &messages,
            const QString &view);

private:
  std::optional<quint64> rearm;
  QString view;
  QStringList prefix;
};
} // namespace ContextPressure
