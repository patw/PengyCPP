#include "context_windows.h"
#include "config.h"
#include "context_recovery.h"
#include <QCryptographicHash>
#include <QDateTime>
#include <QDir>
#include <QElapsedTimer>
#include <QEventLoop>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QRegularExpression>
#include <QSaveFile>
#include <QTimer>
#include <cmath>
#include <limits>

namespace ContextWindows {
static QString normalized(QString base) {
  base = base.trimmed();
  while (base.endsWith('/'))
    base.chop(1);
  return base;
}
static std::optional<quint64> number(const QJsonValue &v) {
  double d = v.toDouble(-1);
  if (!v.isDouble() || !std::isfinite(d) || d <= 0 || std::floor(d) != d ||
      d >= double(std::numeric_limits<quint64>::max()))
    return {};
  return quint64(d);
}
static QString path(const QString &base, const QString &model) {
  auto key = QJsonDocument(QJsonArray{normalized(base), model})
                 .toJson(QJsonDocument::Compact);
  return pengyConfigDirPath() + "/context_windows/" +
         QString::fromLatin1(
             QCryptographicHash::hash(key, QCryptographicHash::Sha256)
                 .toHex()) +
         ".json";
}
static QJsonObject serialize(Window w) {
  return {{"tokens", qint64(w.tokens)}, {"input_only", w.inputOnly}};
}
static void remember(const QString &base, const QString &model,
                     std::optional<Window> w, const QString &source) {
  auto file = path(base, model);
  QDir().mkpath(QFileInfo(file).path());
  QSaveFile f(file);
  if (!f.open(QIODevice::WriteOnly))
    return;
  f.setPermissions(QFileDevice::ReadOwner | QFileDevice::WriteOwner);
  QJsonObject o{
      {"v", 1},
      {"endpoint", normalized(base)},
      {"model", model},
      {"window", w ? QJsonValue(serialize(*w)) : QJsonValue(QJsonValue::Null)},
      {"source", source},
      {"expires", QDateTime::currentSecsSinceEpoch() + (w ? 86400 : 300)}};
  if (f.write(QJsonDocument(o).toJson(QJsonDocument::Compact)) < 0)
    return;
  f.commit();
}
static std::optional<Window> fields(const QJsonObject &o) {
  for (auto key : {"context_window", "context_length"})
    if (auto n = number(o[key]))
      return Window{*n, false};
  if (auto n = number(o["meta"].toObject()["n_ctx"]))
    return Window{*n, false};
  if (auto n = number(o["max_input_tokens"]))
    return Window{*n, true};
  return {};
}
std::optional<Window> modelWindow(const QJsonObject &b, const QString &model) {
  for (auto v : b["data"].toArray())
    if (v.toObject()["id"] == model)
      return fields(v.toObject());
  return {};
}
std::optional<Window> errorWindow(const QString &s) {
  static QRegularExpression re(
      "(?:maximum|max) context (?:length|window|size)(?: is| "
      "of|:|=)?\\s*([0-9][0-9,]*)",
      QRegularExpression::CaseInsensitiveOption);
  static QRegularExpression strata("prompt \\([0-9,]+ tokens\\) leaves no room "
                                   "to answer in the context \\(([0-9,]+)\\)",
                                   QRegularExpression::CaseInsensitiveOption);
  auto m = re.match(s);
  if (!m.hasMatch())
    m = strata.match(s);
  if (!m.hasMatch())
    return {};
  bool ok = false;
  auto n = m.captured(1).remove(',').toULongLong(&ok);
  return ok && n ? std::optional<Window>(Window{n, false}) : std::nullopt;
}
std::optional<Window> rememberError(const QString &b, const QString &m,
                                    const QString &s) {
  auto w = errorWindow(s);
  if (w)
    remember(b, m, w, "context_error");
  return w;
}
std::optional<Window> resolve(const QString &base, const QString &model,
                              const QString &key,
                              const ContextRecovery::Options &opts,
                              const std::function<bool()> &cancelled) {
  const QString mode=opts.contextWindowModes[model].toString();
  if(mode=="off")return {};
  if(mode!="auto") {
  if (opts.contextWindows.contains(model)) {
    auto n = number(opts.contextWindows[model]);
    return n ? std::optional<Window>(Window{*n, false}) : std::nullopt;
  }
  if (opts.contextWindow)
    return Window{opts.contextWindow, false};
  }
  if (!opts.enabled || (!opts.windowAuto && mode!="auto"))
    return {};
  QFile f(path(base, model));
  if (f.open(QIODevice::ReadOnly)) {
    auto o = QJsonDocument::fromJson(f.readAll()).object();
    if (o["v"] == 1 && o["endpoint"] == normalized(base) &&
        o["model"] == model &&
        o["expires"].toDouble() > QDateTime::currentSecsSinceEpoch()) {
      if (o["window"].isNull())
        return {};
      auto w = o["window"].toObject();
      if (auto n = number(w["tokens"]))
        return Window{*n, w["input_only"].toBool()};
    }
  }
  auto b = normalized(base), sibling = b;
  if (sibling.endsWith("/v1"))
    sibling.chop(3);
  QElapsedTimer deadline;
  deadline.start();
  QNetworkAccessManager nam;
  bool single = false;
  for (auto source : {"models", "props", "health"}) {
    int remaining = 2000 - int(deadline.elapsed());
    if (remaining <= 0 || cancelled())
      break;
    QUrl url(QString(source) == "models" ? b + "/models"
                                         : sibling + "/" + source);
    QNetworkRequest req(url);
    req.setTransferTimeout(qMin(700, remaining));
    if (!key.isEmpty())
      req.setRawHeader("Authorization", ("Bearer " + key).toUtf8());
    QEventLoop loop;
    auto reply = nam.get(req);
    QByteArray data;
    bool oversized = false;
    QObject::connect(reply, &QNetworkReply::readyRead, &loop, [&] {
      auto chunk = reply->readAll();
      if (data.size() + chunk.size() > 1024 * 1024) {
        oversized = true;
        reply->abort();
      } else
        data += chunk;
    });
    QObject::connect(reply, &QNetworkReply::finished, &loop, &QEventLoop::quit);
    QTimer timer;
    timer.setInterval(20);
    QObject::connect(&timer, &QTimer::timeout, &loop, [&] {
      if (cancelled() || deadline.elapsed() >= 2000)
        reply->abort();
    });
    timer.start();
    loop.exec();
    timer.stop();
    auto tail = reply->readAll();
    if (data.size() + tail.size() > 1024 * 1024)
      oversized = true;
    else
      data += tail;
    bool success = !oversized && reply->error() == QNetworkReply::NoError;
    reply->deleteLater();
    if (!success)
      continue;
    auto body = QJsonDocument::fromJson(data).object();
    std::optional<Window> w;
    if (QString(source) == "models") {
      auto a = body["data"].toArray();
      single = a.size() == 1 && a[0].toObject()["id"] == model;
      w = modelWindow(body, model);
    } else if (single || body["model"] == model ||
               body["model_alias"] == model) {
      auto n =
          QString(source) == "props"
              ? number(body["default_generation_settings"].toObject()["n_ctx"])
              : number(body["max_context"]);
      if (n)
        w = Window{*n, false};
    }
    if (w) {
      remember(base, model, w, source);
      return w;
    }
  }
  if (!cancelled())
    remember(base, model, {}, "unknown");
  return {};
}
} // namespace ContextWindows
