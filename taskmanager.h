#pragma once
#include <QJsonArray>
#include <QJsonObject>
#include <QString>
#include <QStringList>
#include <QMap>

QJsonArray tasksLoad();
bool       tasksSave(const QJsonArray& tasks);
QJsonObject taskCreate(const QString& title, const QString& templ);
QJsonObject taskUpdate(const QString& id, const QString& title, const QString& templ);
bool       taskDelete(const QString& id);
QJsonObject taskGet(const QString& id);
QStringList extractPlaceholders(const QString& templ);
QString renderTaskTemplate(const QString& templ, const QMap<QString, QString>& values);
