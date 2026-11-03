#pragma once

// Starter-skills detection, installation and system-message wiring.
//
// Shared, header-only, Qt-only (no widgets): the Extras tab in Settings uses it
// directly, and the unit test drives the same functions against a temp folder.
// The behaviour is deliberately identical to the Python edition's
// pengy/core/skills_install.py -- the three editions share one config file, so
// they must agree on where skills live, what "installed" means and what the
// pointer sentence says.
//
// Contract (mirrors Python):
//   * root resolution: skills_dir from config -> the folder the system message
//     names (only if it exists) -> ~/skills
//   * "installed" = the folder exists AND contains skill_index.md
//   * install copies the bundled tree from the Qt resource system and never
//     overwrites an existing file
//   * the pointer is appended once; a message that already mentions
//     skill_index.md (any path spelling) is left alone

#include <QDir>
#include <QDirIterator>
#include <QFile>
#include <QFileInfo>
#include <QRegularExpression>
#include <QString>
#include <QStringList>

namespace Skills {

//: Qt resource root holding the bundled starter skills.
inline const QString& resourceRoot() {
    static const QString root = QStringLiteral(":/skills");
    return root;
}
inline const QString& indexName() {
    static const QString name = QStringLiteral("skill_index.md");
    return name;
}
inline const QString& readmeName() {
    static const QString name = QStringLiteral("README.md");
    return name;
}

struct Status {
    QString     root;
    bool        exists   = false;
    bool        hasIndex = false;
    QStringList skills;

    //: Usable as a skills folder: present *and* indexed. A folder without an
    //: index is a dead end -- nothing tells the model to read it.
    bool installed() const { return exists && hasIndex; }
};

struct InstallReport {
    int created = 0;
    int skipped = 0;
    int failed  = 0;

    bool changed() const { return created > 0; }
};

inline QString defaultRoot() { return QDir::homePath() + QStringLiteral("/skills"); }

//: Expand ~ and $HOME, anchor a relative value at home (never the CWD).
inline QString expandPath(const QString& raw) {
    QString p = raw.trimmed();
    if (p.isEmpty()) return p;
    if (p == QLatin1String("~")) return QDir::homePath();
    if (p.startsWith(QLatin1String("~/"))) p = QDir::homePath() + p.mid(1);
    else if (p.startsWith(QLatin1String("$HOME"))) p = QDir::homePath() + p.mid(5);
    if (!QDir::isAbsolutePath(p)) p = QDir::homePath() + QLatin1Char('/') + p;
    return QDir::cleanPath(p);
}

//: The skill index a system message points at, or an empty string.
inline QString referencedIndex(const QString& message) {
    static const QRegularExpression re(
        QStringLiteral("(?:~|\\$HOME|/)[^\\s'\"`()<>\\[\\]]*skill_index\\.md"));
    const QRegularExpressionMatch m = re.match(message);
    if (!m.hasMatch()) return QString();
    return expandPath(m.captured(0));
}

//: skills_dir -> folder named by the system message (if it exists) -> ~/skills.
inline QString resolveRoot(const QString& configured, const QString& systemMessage) {
    const QString raw = configured.trimmed();
    if (!raw.isEmpty()) return expandPath(raw);
    const QString referenced = referencedIndex(systemMessage);
    if (!referenced.isEmpty()) {
        const QString parent = QFileInfo(referenced).absolutePath();
        if (QDir(parent).exists()) return parent;
    }
    return defaultRoot();
}

//: Abbreviate a path under $HOME to ~/... for prose and prompts.
inline QString displayPath(const QString& path) {
    const QString home  = QDir::homePath();
    const QString clean = QDir::cleanPath(path);
    if (clean == home) return QStringLiteral("~");
    if (clean.startsWith(home + QLatin1Char('/'))) return QStringLiteral("~/") + clean.mid(home.size() + 1);
    return clean;
}

//: Inspect a folder (read-only).
inline Status scan(const QString& root) {
    Status s;
    s.root = QDir::cleanPath(root);
    QDir dir(s.root);
    s.exists = dir.exists();
    if (!s.exists) return s;
    s.hasIndex = QFileInfo(s.root + QLatin1Char('/') + indexName()).isFile();
    const QFileInfoList entries =
        dir.entryInfoList(QDir::Dirs | QDir::NoDotAndDotDot, QDir::Name);
    for (const QFileInfo& fi : entries) {
        if (fi.fileName().startsWith(QLatin1Char('.'))) continue;
        const QString marker =
            fi.absoluteFilePath() + QLatin1Char('/') + fi.fileName() + QStringLiteral("_skill.md");
        if (QFileInfo(marker).isFile()) s.skills << fi.fileName();
    }
    return s;
}

//: Copy the bundled starter tree into *root*. Existing files are never touched.
inline InstallReport install(const QString& root) {
    InstallReport report;
    QDir().mkpath(root);
    const QString prefix = resourceRoot() + QLatin1Char('/');
    QDirIterator it(resourceRoot(), QDir::Files, QDirIterator::Subdirectories);
    while (it.hasNext()) {
        const QString src = it.next();
        const QString rel = src.mid(prefix.size());
        if (rel.isEmpty()) continue;
        const QString dst = QDir::cleanPath(root + QLatin1Char('/') + rel);
        const QFileInfo dstInfo(dst);
        if (dstInfo.exists()) {          // a file the user may have edited
            report.skipped++;
            continue;
        }
        QDir().mkpath(dstInfo.absolutePath());
        if (!QFile::copy(src, dst)) {
            report.failed++;
            continue;
        }
        report.created++;
        // Resource files carry no usable mode -- and Qt copies the resource's
        // read-only flag through -- so set the mode explicitly. 0644 for data,
        // 0755 for helper scripts, which the skills document as ./script.py.
        const bool isScript = dst.endsWith(QLatin1String(".py")) ||
                              dst.endsWith(QLatin1String(".sh"));
        QFile::setPermissions(dst,
            QFileDevice::ReadOwner | QFileDevice::WriteOwner |
            QFileDevice::ReadGroup | QFileDevice::ReadOther |
            (isScript ? (QFileDevice::ExeOwner | QFileDevice::ExeGroup | QFileDevice::ExeOther)
                      : QFileDevice::Permissions()));
    }
    return report;
}

inline QString pointer(const QString& indexFile) {
    return QStringLiteral(
               "WARNING: Always look at %1 before running web search, bash, running code "
               "or fetch_url! Skills should be used over tools!")
        .arg(displayPath(indexFile));
}

//: True when a message already points at a skill index. Matched on the bare
//: filename so a hand-written pointer (any folder spelling) is never doubled.
inline bool referencesSkills(const QString& message) {
    return message.contains(indexName());
}

//: Append the pointer once; a message that already mentions the index is
//: returned unchanged, so pressing the button twice cannot stack sentences.
inline QString withReference(const QString& message, const QString& indexFile) {
    if (referencesSkills(message)) return message;
    const QString trimmed = message.trimmed();
    return trimmed.isEmpty() ? pointer(indexFile) : trimmed + QLatin1Char(' ') + pointer(indexFile);
}

}  // namespace Skills
