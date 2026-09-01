#pragma once

#include <QByteArray>
#include <QString>

namespace mdv {

bool isCodexEventLog(const QString &path, const QByteArray &sample);

// True when the sampled head of a rollout log carries session metadata whose
// recorded working directory matches cwd.
bool codexSessionMatchesCwd(const QByteArray &sample, const QString &cwd);

// Most recently modified rollout log recorded for the working directory. An
// empty sessionsRoot uses ~/.codex/sessions. Returns an empty string when none
// exists.
QString latestCodexSessionForCwd(
    const QString &cwd, const QString &sessionsRoot = QString());

// Most recently modified valid rollout log under sessionsRoot. An empty root
// uses ~/.codex/sessions. Returns an empty string when no session exists.
QString latestCodexSession(const QString &sessionsRoot = QString());

QString renderCodexEventLog(
    const QString &jsonl,
    const QString &title,
    const QString &userLabel,
    const QString &assistantLabel,
    const QString &emptyLabel);

} // namespace mdv
