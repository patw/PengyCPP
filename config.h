#pragma once
#include <QString>
#include <QJsonObject>

struct Config {
    QString baseUrl          = "https://api.openai.com/v1";
    QString apiKey;
    QString model            = "gpt-4o";
    QString systemMessage    =
        "You are a helpful assistant named Pengy. "
        "The current date is {date} and the user is {username} on host {hostname} which is {osinfo}.";
    QString toolConfirmation = "none";
    QString reasoningEffort;
    bool preserveReasoning   = false;
    int  contextKeepTurns    = 0;
    int  uiScale             = 100;
    QString userAgent        = "PengyAgent/1.0";
    int  toolTimeout         = 60;

    QJsonObject toJson() const;
    static Config fromJson(const QJsonObject& obj);
};

Config  configLoad();
bool    configSave(const Config& cfg);
QString configRenderSystemMessage(const QString& tmpl);
