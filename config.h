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
    QString themeMode        = "system"; // "system" | "light" | "dark"
    QString themeAccent      = "default"; // default | blue | teal | green | orange | red | pink | purple
    QString userAgent        = "PengyAgent/1.0";
    int  llmTimeout          = 300;
    int  toolTimeout         = 300;
    int  imageMaxDimension   = 4096;
    double imageMaxMb        = 4.5;
    int  imageQuality        = 85;

    QJsonObject toJson() const;
    static Config fromJson(const QJsonObject& obj);
};

Config  configLoad();
bool    configSave(const Config& cfg);
QString configRenderSystemMessage(const QString& tmpl);
void    setConfigDir(const QString& path);
QString pengyConfigDirPath();
