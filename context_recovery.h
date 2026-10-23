#pragma once
#include <QJsonArray>
#include <QJsonObject>
#include <QString>
#include <functional>

struct Config;
namespace ContextRecovery {
extern const QString SummaryPrompt;
struct Options {
    bool enabled = true;
    int keepTurns = 3;
    QString chatId;
    int outputLimit = 0;
    QString outputParameter = "max_tokens";
    quint64 contextWindow = 0;
    QJsonObject contextWindows;
    QJsonObject contextWindowModes;
    bool windowAuto = false;
    int compactAtPercent = 75;
    QString thinkingOff;
    QJsonObject thinkingOffModels;
    static Options configured(const Config& config, const QString& chatId);
};
struct Plan {
    QJsonObject state;
    QStringList chunks;
    QString strategy;
    int turns = 0;
    int before = 0;
    int sourceLength = 0;
};
QString text(const QJsonObject &message);
QString fingerprint(const QJsonObject &message);
bool synthetic(const QJsonObject &message);
class Recovery {
  public:
    Recovery(const QJsonArray &messages, const QString &endpoint, const QString &model,
             const Options &options);
    QJsonArray apply(const QJsonArray &messages) const;
    QJsonObject state() const { return m_state; }
    bool plan(const QJsonArray &messages, Plan &result) const;
    QJsonObject commit(Plan plan, const QStringList &summaries, const QJsonArray &messages,
                       QString &error);
    Options options;
    int attempts = 0;
    int summaryCalls = 0;

  private:
    bool summaryPlan(const QJsonArray &messages, Plan &result) const;
    QJsonObject m_state;
    QString m_path;
};
} // namespace ContextRecovery
