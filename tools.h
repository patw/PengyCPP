#pragma once
#include <QString>
#include <QJsonObject>
#include <QJsonArray>
#include <QSet>
#include <QMutex>
#include <atomic>
#include <functional>

namespace Tools {

/// Blocking callback that prompts the user for a sudo password.
/// Returns the password, or an empty string if the user cancels.
using SudoPasswordFn = std::function<QString()>;

/// Per-run tool state: sudo provider, cached sudo password, and the set of
/// active subprocess groups.  Each concurrent run (e.g. one per GUI tab) gets
/// its own context so a sudo prompt is routed to the right run and pressing
/// Stop on one run kills only that run's subprocesses — never another tab's.
/// Callers that don't supply a context (CLI, Web) use the default context.
class ToolContext {
public:
    void           setSudoProvider(SudoPasswordFn fn);
    SudoPasswordFn sudoProvider();
    QString        cachedSudoPassword();
    void           setCachedSudoPassword(const QString& pw);
    void           clearSudo();

    void registerProcess(qint64 pid);
    void unregisterProcess(qint64 pid);
    void killAll();                 // kill every subprocess in this context

private:
    QMutex         m_mutex;
    SudoPasswordFn m_sudoProvider;
    QString        m_cachedSudoPassword;
    QSet<qint64>   m_procs;
};

const QJsonArray& toolDefinitions();
bool       isReadOnly(const QString& name);
void       setUserAgent(const QString& ua);
void       setTimeout(int secs);
void       setToolOutputMaxChars(int chars);
QString    execute(const QString& name, const QJsonObject& args,
                   std::atomic<bool>* cancel = nullptr,
                   ToolContext* ctx = nullptr);

// Operates on the default context (CLI/Web).  The tabbed GUI uses a per-run
// ToolContext instead.
void       killActiveProcesses();
void setSudoPasswordProvider(SudoPasswordFn fn);
void clearSudoPasswordProvider();

/// Rewrite every `sudo` in *command* to `sudo -A` (askpass). Exposed for tests.
QString rewriteSudoForAskpass(QString command);

} // namespace Tools
