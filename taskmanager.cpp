#include "taskmanager.h"
#include <QFile>
#include <QDir>
#include <QFileInfo>
#include <QJsonDocument>
#include <QStandardPaths>
#include <QDateTime>
#include <QUuid>
#include <QRegularExpression>
#include <QSet>

static QString tasksFilePath() {
    QString base = qEnvironmentVariable("XDG_CONFIG_HOME");
    if (base.isEmpty()) base = QStandardPaths::writableLocation(QStandardPaths::GenericConfigLocation);
    return base + "/pengy/tasks.json";
}
static void backupCorruptFile(const QString& path) {
    QFileInfo fi(path);
    QFile::rename(path, fi.dir().filePath(fi.fileName() + ".corrupt-" + QString::number(QDateTime::currentSecsSinceEpoch())));
}
static QJsonObject normalizeTask(const QJsonObject& in) {
    QString now = QDateTime::currentDateTime().toString(Qt::ISODate);
    QJsonObject o;
    o["id"] = in["id"].toString().isEmpty() ? QUuid::createUuid().toString(QUuid::WithoutBraces) : in["id"].toString();
    o["title"] = in["title"].toString().isEmpty() ? "Untitled Task" : in["title"].toString();
    o["template"] = in["template"].toString();
    o["created_at"] = in["created_at"].toString().isEmpty() ? now : in["created_at"].toString();
    o["updated_at"] = in["updated_at"].toString().isEmpty() ? o["created_at"].toString() : in["updated_at"].toString();
    return o;
}
QJsonArray tasksLoad() {
    QString path = tasksFilePath();
    QFile f(path);
    if (!f.exists()) return {};
    if (f.open(QIODevice::ReadOnly)) {
        QJsonParseError err;
        auto doc = QJsonDocument::fromJson(f.readAll(), &err);
        f.close();
        if (err.error == QJsonParseError::NoError && doc.isArray()) {
            QJsonArray out;
            for (const auto& v : doc.array()) if (v.isObject()) out.append(normalizeTask(v.toObject()));
            return out;
        }
        backupCorruptFile(path);
    }
    return {};
}
bool tasksSave(const QJsonArray& tasks) {
    QString path = tasksFilePath();
    QFileInfo fi(path); QDir().mkpath(fi.dir().absolutePath());
    QJsonArray norm; for (const auto& v : tasks) if (v.isObject()) norm.append(normalizeTask(v.toObject()));
    QString tmp = path + ".tmp"; QFile f(tmp);
    if (!f.open(QIODevice::WriteOnly | QIODevice::Truncate)) return false;
    f.write(QJsonDocument(norm).toJson(QJsonDocument::Indented)); f.close();
    QFile::remove(path); return QFile::rename(tmp, path);
}
QJsonObject taskCreate(const QString& title, const QString& templ) {
    QString now = QDateTime::currentDateTime().toString(Qt::ISODate);
    QJsonObject task{{"id", QUuid::createUuid().toString(QUuid::WithoutBraces)}, {"title", title.trimmed().isEmpty()?"Untitled Task":title.trimmed()}, {"template", templ}, {"created_at", now}, {"updated_at", now}};
    QJsonArray tasks = tasksLoad(); tasks.append(task); tasksSave(tasks); return task;
}
QJsonObject taskUpdate(const QString& id, const QString& title, const QString& templ) {
    QJsonArray tasks = tasksLoad();
    for (int i=0;i<tasks.size();++i) { QJsonObject t=tasks[i].toObject(); if (t["id"].toString()==id) { t["title"] = title.trimmed().isEmpty()?"Untitled Task":title.trimmed(); t["template"] = templ; t["updated_at"] = QDateTime::currentDateTime().toString(Qt::ISODate); tasks[i]=t; tasksSave(tasks); return t; } }
    return {};
}
bool taskDelete(const QString& id) { QJsonArray out; for (const auto& v: tasksLoad()) if (v.toObject()["id"].toString()!=id) out.append(v); return tasksSave(out); }
QJsonObject taskGet(const QString& id) { for (const auto& v: tasksLoad()) if (v.toObject()["id"].toString()==id) return v.toObject(); return {}; }
QStringList extractPlaceholders(const QString& templ) {
    QStringList out; QSet<QString> seen; QRegularExpression re("%([^%\\r\\n]+)%");
    auto it = re.globalMatch(templ);
    while (it.hasNext()) { QString name = it.next().captured(1).trimmed(); if (!name.isEmpty() && !seen.contains(name)) { seen.insert(name); out << name; } }
    return out;
}
QString renderTaskTemplate(const QString& templ, const QMap<QString, QString>& values) {
    QString out; int last=0; QRegularExpression re("%([^%\\r\\n]+)%"); auto it = re.globalMatch(templ);
    while (it.hasNext()) { auto m=it.next(); out += templ.mid(last, m.capturedStart()-last); QString name=m.captured(1).trimmed(); out += values.contains(name) ? values[name] : m.captured(0); last = m.capturedEnd(); }
    out += templ.mid(last); return out;
}
