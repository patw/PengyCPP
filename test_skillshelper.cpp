// Tests for skillshelper.h — the starter-skills logic behind Settings → Extras.
//
// These run headless (offscreen Qt) and drive the same functions the dialog
// uses: root resolution, "installed" detection, a non-destructive install from
// the Qt resource bundle, and the append-once system-message pointer. The
// expectations mirror the Python edition's tests/test_skills_install.py, because
// all three editions read the same config file.

#include <QtTest>
#include <QTemporaryDir>
#include <QDir>
#include <QFile>
#include <QFileInfo>

#include "skillshelper.h"

class TestSkillsHelper : public QObject {
    Q_OBJECT

private slots:
    void defaultRootIsHomeSkills() {
        QCOMPARE(Skills::defaultRoot(), QDir::homePath() + "/skills");
    }

    void expandPathAnchorsRelativeValuesAtHome() {
        QCOMPARE(Skills::expandPath("~/x"), QDir::homePath() + "/x");
        QCOMPARE(Skills::expandPath("skills"), QDir::homePath() + "/skills");
        QCOMPARE(Skills::expandPath("$HOME/x"), QDir::homePath() + "/x");
    }

    void resolvePrefersConfigThenMessageThenDefault() {
        QTemporaryDir tmp;
        QVERIFY(tmp.isValid());
        const QString explicitDir = tmp.filePath("explicit");
        QVERIFY(QDir().mkpath(explicitDir));
        QCOMPARE(Skills::resolveRoot(explicitDir, "Read /nope/skill_index.md"), explicitDir);

        // No skills_dir: the folder the system message names is used, but only
        // when it really exists -- a stale pointer must not send an install to
        // nowhere.
        const QString named = tmp.filePath("synced");
        QVERIFY(QDir().mkpath(named));
        const QString message = QString("You are Pengy. Read %1/skill_index.md first.").arg(named);
        QCOMPARE(Skills::resolveRoot(QString(), message), named);

        const QString stale = tmp.filePath("gone/skill_index.md");
        QCOMPARE(Skills::resolveRoot(QString(), QString("Read %1 first.").arg(stale)),
                 Skills::defaultRoot());
        QCOMPARE(Skills::resolveRoot(QString(), QString()), Skills::defaultRoot());
    }

    void displayPathAbbreviatesHome() {
        QCOMPARE(Skills::displayPath(QDir::homePath() + "/skills"), QString("~/skills"));
        QCOMPARE(Skills::displayPath("/opt/x"), QString("/opt/x"));
    }

    void scanNeedsAnIndexToCountAsInstalled() {
        QTemporaryDir tmp;
        const QString root = tmp.filePath("skills");
        QVERIFY(QDir().mkpath(root + "/weather"));
        QFile marker(root + "/weather/weather_skill.md");
        QVERIFY(marker.open(QIODevice::WriteOnly));
        marker.write("# Weather\n");
        marker.close();

        Skills::Status s = Skills::scan(root);
        QVERIFY(s.exists);
        QVERIFY(!s.hasIndex);
        QVERIFY(!s.installed());
        QCOMPARE(s.skills, QStringList{"weather"});

        QFile index(root + "/skill_index.md");
        QVERIFY(index.open(QIODevice::WriteOnly));
        index.write("# Skills Index\n");
        index.close();
        QVERIFY(Skills::scan(root).installed());

        QVERIFY(!Skills::scan(tmp.filePath("absent")).exists);
    }

    void installWritesTheBundleAndNeverClobbers() {
        QTemporaryDir tmp;
        const QString root = tmp.filePath("skills");

        const Skills::InstallReport first = Skills::install(root);
        QVERIFY(first.created > 0);
        QCOMPARE(first.skipped, 0);
        QCOMPARE(first.failed, 0);

        QVERIFY(QFileInfo(root + "/skill_index.md").isFile());
        QVERIFY(QFileInfo(root + "/memory/memory.py").isFile());
        QVERIFY(QFileInfo(root + "/user_profile/profile.md").isFile());

        // A script the skill documents as ./memory.py must arrive executable.
        const QFileDevice::Permissions perms =
            QFileInfo(root + "/memory/memory.py").permissions();
        QVERIFY(perms & QFileDevice::ExeOwner);

        // Second run: nothing created, everything skipped.
        const Skills::InstallReport second = Skills::install(root);
        QCOMPARE(second.created, 0);
        QCOMPARE(second.failed, 0);
        QCOMPARE(second.skipped, first.created);
        QVERIFY(!second.changed());

        // A file the user edited is left exactly as it is.
        QFile mine(root + "/skill_index.md");
        QVERIFY(mine.open(QIODevice::WriteOnly | QIODevice::Truncate));
        mine.write("# my index\n");
        mine.close();
        QVERIFY(Skills::install(root).created == 0);
        QVERIFY(mine.open(QIODevice::ReadOnly));
        QCOMPARE(QString::fromUtf8(mine.readAll()), QString("# my index\n"));
    }

    void scanFindsEveryBundledSkill() {
        QTemporaryDir tmp;
        const QString root = tmp.filePath("skills");
        Skills::install(root);
        const Skills::Status s = Skills::scan(root);
        QVERIFY(s.installed());
        // The bundle and its index must agree, or "Install Skills" reports
        // success on a folder it only half understands.
        for (const QString& name : {"botskills", "memory", "pengy_bio", "sysinfo", "user_profile"})
            QVERIFY2(s.skills.contains(name), qPrintable(name));
    }

    void pointerIsAppendedOnce() {
        QTemporaryDir tmp;
        const QString index = tmp.filePath("skills/skill_index.md");
        const QString base = "You are Pengy. The current date is {date}.";

        const QString once = Skills::withReference(base, index);
        QVERIFY(once.startsWith(base));
        QVERIFY(once.contains("skill_index.md"));
        QVERIFY(once.contains("Skills should be used over tools"));
        // Pressing the button again cannot stack the sentence.
        QCOMPARE(Skills::withReference(once, index), once);
        // An empty message gets the pointer alone.
        QVERIFY(Skills::withReference(QString(), index).startsWith("WARNING:"));
        // A hand-written pointer in another spelling is respected.
        const QString custom = "Read ~/Personal/skills/skill_index.md before bash.";
        QCOMPARE(Skills::withReference(custom, index), custom);
    }

    void profileNeedsInterviewFollowsTheTodoMarker() {
        QTemporaryDir tmp;
        const QString root = tmp.filePath("skills");
        Skills::install(root);
        QVERIFY(Skills::profileNeedsInterview(Skills::scan(root)));

        const QString profile = root + "/user_profile/profile.md";
        QFile f(profile);
        QVERIFY(f.open(QIODevice::ReadOnly));
        QString text = QString::fromUtf8(f.readAll());
        f.close();
        text.replace(Skills::profileTodo(), "Prefers terse answers.");
        QVERIFY(f.open(QIODevice::WriteOnly | QIODevice::Truncate));
        f.write(text.toUtf8());
        f.close();

        QVERIFY(!Skills::profileNeedsInterview(Skills::scan(root)));
    }

    void interviewPromptNamesTheSkillFiles() {
        QTemporaryDir tmp;
        const QString root = tmp.filePath("skills");
        Skills::install(root);
        const QString prompt = Skills::profileInterviewPrompt(Skills::scan(root));
        QVERIFY(prompt.contains("user_profile/user_profile_skill.md"));
        QVERIFY(prompt.contains("user_profile/profile.md"));
        QVERIFY(prompt.contains("one question at a time"));
    }
};

QTEST_MAIN(TestSkillsHelper)
#include "test_skillshelper.moc"
