#pragma once

#include <QByteArray>
#include <QString>

namespace mdv {

bool isCodexEventLog(const QString &path, const QByteArray &sample);

QString renderCodexEventLog(
    const QString &jsonl,
    const QString &title,
    const QString &userLabel,
    const QString &assistantLabel,
    const QString &emptyLabel);

} // namespace mdv
