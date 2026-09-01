#pragma once

#include <QByteArray>
#include <QString>

namespace mdv {

bool isClaudeSessionLog(const QString &path, const QByteArray &sample);

// True when a sampled Claude session contains a recorded working directory
// that identifies the same filesystem location as cwd.
bool claudeSessionMatchesCwd(const QByteArray &sample, const QString &cwd);

// Directory name used by Claude Code under ~/.claude/projects for a working
// directory (every character outside [A-Za-z0-9] becomes '-').
QString claudeProjectDirName(const QString &cwd);

// Most recently modified session log for the working directory. An empty
// projectsRoot uses ~/.claude/projects. Returns an empty string when none
// exists.
QString latestClaudeSessionForCwd(
    const QString &cwd, const QString &projectsRoot = QString());

QString renderClaudeSessionLog(
    const QString &jsonl,
    const QString &title,
    const QString &userLabel,
    const QString &assistantLabel,
    const QString &emptyLabel);

} // namespace mdv
