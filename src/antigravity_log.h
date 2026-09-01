#pragma once

#include <QByteArray>
#include <QString>

namespace mdv {

bool isAntigravitySessionLog(const QString &path, const QByteArray &sample);

// Most recently modified valid Antigravity transcript associated with cwd.
// An empty antigravityRoot uses ~/.gemini/antigravity-cli. Returns an empty
// string when the caches contain no matching conversation.
QString latestAntigravitySessionForCwd(
    const QString &cwd, const QString &antigravityRoot = QString());

// Most recently modified valid Antigravity transcript under brainRoot. An empty root
// uses ~/.gemini/antigravity-cli/brain. Returns an empty string when no session exists.
QString latestAntigravitySession(const QString &brainRoot = QString());

QString renderAntigravitySessionLog(
    const QString &jsonl,
    const QString &title,
    const QString &userLabel,
    const QString &assistantLabel,
    const QString &emptyLabel);

} // namespace mdv
