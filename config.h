#pragma once
#include <QString>
#include <QJsonObject>

struct Config {
    // A local server, not a hosted API: Pengy's audience runs Ollama or llama.cpp
    // on their own machine, so the default endpoint is Ollama's OpenAI-compatible
    // port, which needs no API key.
    QString baseUrl          = "http://127.0.0.1:11434/v1";
    QString apiKey;
    // Deliberately empty: a local server ships no model of its own (a fresh
    // `ollama list` is empty), so naming one would be a lie that fails on the
    // user's first message.  An empty model produces Pengy's own "pick a model"
    // instructions instead -- see noModelHelp() in llmclient.h.
    QString model            = "";
    QString systemMessage    =
        "You are a helpful assistant named Pengy. "
        "The current date is {date} and the user is {username} on host {hostname} which is {osinfo}.";
    QString toolConfirmation = "none";
    QString reasoningEffort;
    bool preserveReasoning   = false;
    int  contextKeepTurns    = 0;
    int  attachmentContextKeepTurns = 4;
    int  uiScale             = 100;
    QString themeMode        = "system"; // "system" | "light" | "dark"
    QString themeAccent      = "default"; // default | blue | teal | green | orange | red | pink | purple
    QString userAgent        = "PengyAgent/1.0";
    int  llmTimeout          = 300;
    int  toolTimeout         = 300;
    int  toolOutputMaxChars  = 50000;
    int  downloadMaxMb       = 100;
    int  imageMaxDimension   = 4096;
    double imageMaxMb        = 4.5;
    int  imageQuality        = 85;

    QStringList openTabs;   // persisted list of open chat IDs

    QJsonObject toJson() const;
    static Config fromJson(const QJsonObject& obj);
};

Config  configLoad();
bool    configSave(const Config& cfg);
QString configRenderSystemMessage(const QString& tmpl);
void    setConfigDir(const QString& path);
QString pengyConfigDirPath();

// ── Persistent model-list cache (shared with the Python/Rust editions) ──────
// Stored in <config>/models_cache.json, keyed by base URL. Returns an empty
// list when there is no cache or the endpoint does not match.
QStringList modelCacheForBaseUrl(const QString& baseUrl);
bool        modelCacheSave(const QString& baseUrl, const QStringList& models);
