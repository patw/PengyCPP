#include "config.h"
#include <QFile>
#include <QDir>
#include <QJsonDocument>
#include <QDateTime>
#include <QSysInfo>

/* Resolve the pengy config directory.
 * Uses $XDG_CONFIG_HOME if set, otherwise $HOME/.config — matching the
 * Python and Rust editions so all three share the same settings/chats/tasks.
 * Can be overridden via setConfigDir(). */
static QString& configDirOverride() {
    static QString override;
    return override;
}

void setConfigDir(const QString& path) {
    configDirOverride() = path;
}

QString pengyConfigDirPath() {
    QString& override = configDirOverride();
    if (!override.isEmpty())
        return override;
    QString base = qEnvironmentVariable("XDG_CONFIG_HOME");
    if (base.isEmpty())
        base = QDir::homePath() + "/.config";
    return base + "/pengy";
}

static QString configFilePath() {
    return pengyConfigDirPath() + "/settings.json";
}

static void backupCorruptFile(const QString& path) {
    QString ts = QString::number(QDateTime::currentSecsSinceEpoch());
    QFileInfo fi(path);
    QString backup = fi.dir().filePath(fi.baseName() + ".corrupt-" + ts);
    QFile::rename(path, backup);
}

QJsonObject Config::toJson() const {
    QJsonObject o;
    o["base_url"]           = baseUrl;
    o["api_key"]            = apiKey;
    o["model"]              = model;
    o["system_message"]     = systemMessage;
    o["tool_confirmation"]  = toolConfirmation;
    o["reasoning_effort"]   = reasoningEffort;
    o["preserve_reasoning"] = preserveReasoning;
    o["context_keep_turns"] = contextKeepTurns;
    o["ui_scale"]           = uiScale;
    o["theme_mode"]         = themeMode;
    o["theme_accent"]       = themeAccent;
    o["user_agent"]         = userAgent;
    o["tool_timeout"]       = toolTimeout;
    o["image_max_dimension"] = imageMaxDimension;
    o["image_max_mb"]        = imageMaxMb;
    o["image_quality"]       = imageQuality;
    return o;
}

Config Config::fromJson(const QJsonObject& o) {
    Config c;
    if (o.contains("base_url"))           c.baseUrl           = o["base_url"].toString(c.baseUrl);
    if (o.contains("api_key"))            c.apiKey            = o["api_key"].toString();
    if (o.contains("model"))              c.model             = o["model"].toString(c.model);
    if (o.contains("system_message"))     c.systemMessage     = o["system_message"].toString(c.systemMessage);
    if (o.contains("tool_confirmation"))  c.toolConfirmation  = o["tool_confirmation"].toString(c.toolConfirmation);
    if (o.contains("reasoning_effort"))   c.reasoningEffort   = o["reasoning_effort"].toString(c.reasoningEffort);
    if (o.contains("preserve_reasoning")) c.preserveReasoning = o["preserve_reasoning"].toBool(false);
    if (o.contains("context_keep_turns")) c.contextKeepTurns  = o["context_keep_turns"].toInt(0);
    if (o.contains("ui_scale"))           c.uiScale           = o["ui_scale"].toInt(100);
    if (o.contains("theme_mode"))         c.themeMode         = o["theme_mode"].toString(c.themeMode);
    if (o.contains("theme_accent"))       c.themeAccent       = o["theme_accent"].toString(c.themeAccent);
    if (o.contains("user_agent"))         c.userAgent         = o["user_agent"].toString(c.userAgent);
    if (o.contains("tool_timeout"))       c.toolTimeout       = o["tool_timeout"].toInt(60);
    if (o.contains("image_max_dimension")) c.imageMaxDimension = o["image_max_dimension"].toInt(4096);
    if (o.contains("image_max_mb"))        c.imageMaxMb        = o["image_max_mb"].toDouble(4.5);
    if (o.contains("image_quality"))       c.imageQuality      = o["image_quality"].toInt(85);
    return c;
}

Config configLoad() {
    QString path = configFilePath();
    QFile f(path);
    if (f.open(QIODevice::ReadOnly)) {
        QByteArray data = f.readAll();
        f.close();
        QJsonParseError err;
        QJsonDocument doc = QJsonDocument::fromJson(data, &err);
        if (err.error == QJsonParseError::NoError && doc.isObject()) {
            return Config::fromJson(doc.object());
        }
        backupCorruptFile(path);
    }
    Config def;
    configSave(def);
    return def;
}

bool configSave(const Config& cfg) {
    QString path = configFilePath();
    QFileInfo fi(path);
    QDir().mkpath(fi.dir().absolutePath());

    QByteArray json = QJsonDocument(cfg.toJson()).toJson(QJsonDocument::Indented);
    QString tmp = path + ".tmp";
    QFile f(tmp);
    if (!f.open(QIODevice::WriteOnly | QIODevice::Truncate)) return false;
    f.write(json);
    f.close();
    QFile::remove(path);
    return QFile::rename(tmp, path);
}

QString configRenderSystemMessage(const QString& tmpl) {
    QString date     = QDate::currentDate().toString("MMMM dd, yyyy");
    QString username = qEnvironmentVariable("USER", qEnvironmentVariable("USERNAME", "unknown"));
    QString hostname;
    {
        QFile f("/etc/hostname");
        if (f.open(QIODevice::ReadOnly))
            hostname = QString::fromUtf8(f.readAll()).trimmed();
        if (hostname.isEmpty())
            hostname = "unknown";
    }
    QString osinfo = QSysInfo::productType() + " " + QSysInfo::currentCpuArchitecture();

    QString result = tmpl;
    result.replace("{date}",     date);
    result.replace("{username}", username);
    result.replace("{hostname}", hostname);
    result.replace("{osinfo}",   osinfo);
    return result;
}
