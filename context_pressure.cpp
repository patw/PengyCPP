#include "context_pressure.h"
#include <QCryptographicHash>
#include <QHash>
#include <QJsonDocument>
#include <QMutex>
#include <QMutexLocker>
#include <limits>

namespace ContextPressure {
static QMutex mutex;
static QHash<QString, Pressure> samples;
static QString digest(const QByteArray &b) {
  return QString::fromLatin1(
      QCryptographicHash::hash(b, QCryptographicHash::Sha256).toHex());
}
QString identity(const QJsonObject &s) {
  return digest(QJsonDocument(s).toJson(QJsonDocument::Compact));
}
static QStringList hashes(const QJsonArray &messages) {
  QStringList out;
  for (const auto &v : messages)
    out << identity(v.toObject());
  return out;
}
static std::optional<quint64> count(const QJsonValue &v) {
  if (v.isString())
    return quint64(v.toString().toUcs4().size());
  quint64 n = 0;
  if (v.isArray()) {
    for (const auto &child : v.toArray()) {
      auto c = count(child);
      if (!c)
        return {};
      n += *c;
    }
    return n;
  }
  if (v.isObject()) {
    auto o = v.toObject();
    if (o.contains("image_url") || o.contains("input_audio") ||
        o.contains("file"))
      return {};
    for (auto it = o.begin(); it != o.end(); ++it) {
      auto c = count(it.value());
      if (!c)
        return {};
      n += it.key().toUcs4().size() + *c;
    }
    return n;
  }
  if (v.isNull() || v.isUndefined())
    return 0;
  auto bytes = QJsonDocument(QJsonArray{v}).toJson(QJsonDocument::Compact);
  return quint64(bytes.size() - 2);
}
std::optional<quint64> inputChars(const QJsonValue &m, const QJsonValue &t) {
  auto a = count(m), b = count(t);
  if (!a || !b)
    return {};
  return *a + *b;
}
std::pair<quint64, quint64> budgets(quint64 w, int p, quint64 out, bool input) {
  quint64 hard = input ? w : (w > out ? w - out : 0);
  return {qMin(w / 100 * quint64(p) + (w % 100) * quint64(p) / 100, hard),
          hard};
}
Pressure Pressure::load(const QString &key, const QJsonArray &m,
                        const QString &v, quint64 w) {
  QMutexLocker lock(&mutex);
  if (!key.isEmpty() && samples.contains(key)) {
    auto p = samples.value(key);
    auto ids = hashes(m);
    if (p.window == w && p.view == v && ids.mid(0, p.prefix.size()) == p.prefix)
      return p;
  }
  Pressure p;
  p.window = w;
  return p;
}
void Pressure::record(std::optional<quint64> c, std::optional<quint64> t) {
  if (c && t && *c > 0 && *t > 0)
    sample = std::make_pair(*c, *t);
}
std::optional<quint64> Pressure::estimate(std::optional<quint64> c) const {
  if (!sample || !c)
    return {};
  auto [chars, tokens] = *sample;
  if (!chars)
    return {};
  auto max = std::numeric_limits<quint64>::max();
  if (tokens && *c > max / tokens)
    return max;
  quint64 product = *c * tokens;
  return product / chars + (product % chars != 0);
}
bool Pressure::shouldCompact(std::optional<quint64> n, quint64 at,
                             quint64 hard) const {
  return window && n && *n >= at && (*n >= hard || !rearm || *n >= *rearm);
}
void Pressure::compacted(std::optional<quint64> after, quint64 at) {
  rearm = after && *after < at
              ? std::optional<quint64>(at + window / 20 + (window % 20 != 0))
              : std::nullopt;
}
void Pressure::setWindow(quint64 w) {
  if (window != w) {
    window = w;
    rearm.reset();
  }
}
void Pressure::save(const QString &key, const QJsonArray &m, const QString &v) {
  view = v;
  prefix = hashes(m);
  if (!key.isEmpty()) {
    QMutexLocker lock(&mutex);
    if (samples.size() >= 256 && !samples.contains(key))
      samples.clear();
    samples.insert(key, *this);
  }
}
} // namespace ContextPressure
