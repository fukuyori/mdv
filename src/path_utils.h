#pragma once

#include <QDir>
#include <QFileInfo>
#include <QString>

namespace mdv {

inline QString normalizedPathForComparison(const QString &path)
{
    if (path.isEmpty()) {
        return {};
    }

    const QFileInfo info(path);
    QString normalized = info.canonicalFilePath();
    if (normalized.isEmpty()) {
        normalized = info.absoluteFilePath();
    }
    return QDir::cleanPath(QDir::fromNativeSeparators(normalized));
}

inline bool pathsReferToSameLocation(const QString &left, const QString &right)
{
    const QString normalizedLeft = normalizedPathForComparison(left);
    const QString normalizedRight = normalizedPathForComparison(right);
    if (normalizedLeft.isEmpty() || normalizedRight.isEmpty()) {
        return false;
    }
#ifdef Q_OS_WIN
    return normalizedLeft.compare(normalizedRight, Qt::CaseInsensitive) == 0;
#else
    return normalizedLeft == normalizedRight;
#endif
}

} // namespace mdv
