#include "preview_policy.h"

#include <QDir>
#include <QFileInfo>

namespace preview_policy {

namespace {

Qt::CaseSensitivity pathCaseSensitivity()
{
#if defined(Q_OS_WIN) || defined(Q_OS_MACOS)
    return Qt::CaseInsensitive;
#else
    return Qt::CaseSensitive;
#endif
}

// True when `path` is `dir` itself or lies below it. Both must already be
// clean absolute paths without a trailing separator.
bool isUnder(const QString &path, const QString &dir)
{
    if (dir.isEmpty()) {
        return false;
    }
    if (path.compare(dir, pathCaseSensitivity()) == 0) {
        return true;
    }
    const QString prefix = dir.endsWith(QLatin1Char('/')) ? dir : dir + QLatin1Char('/');
    return path.startsWith(prefix, pathCaseSensitivity());
}

QString cleanAbsolute(const QString &path)
{
    if (path.isEmpty()) {
        return QString();
    }
    QString cleaned = QDir::cleanPath(QDir(path).absolutePath());
    while (cleaned.size() > 1 && cleaned.endsWith(QLatin1Char('/'))) {
        cleaned.chop(1);
    }
    return cleaned;
}

} // namespace

bool allowsLocalResource(const QUrl &url, const QString &documentDir)
{
    if (!url.isValid() || !url.isLocalFile()) {
        return false;
    }
    const QString host = url.host();
    if (!host.isEmpty() && host.compare(QStringLiteral("localhost"), Qt::CaseInsensitive) != 0) {
        return false;
    }

    const QFileInfo dirInfo(documentDir);
    if (documentDir.isEmpty() || !dirInfo.isDir()) {
        return false;
    }
    // Resolve the document directory itself so that a document opened
    // through a symlinked directory still maps onto its real location.
    const QString canonicalDir = cleanAbsolute(dirInfo.canonicalFilePath());
    if (canonicalDir.isEmpty()) {
        return false;
    }

    const QString requested = cleanAbsolute(url.toLocalFile());
    if (requested.isEmpty()) {
        return false;
    }
    // The lexical path must stay under the directory: this rejects "../"
    // escapes even when the target does not exist yet.
    if (!isUnder(requested, cleanAbsolute(dirInfo.absoluteFilePath()))
        && !isUnder(requested, canonicalDir)) {
        return false;
    }

    const QFileInfo requestedInfo(requested);
    if (!requestedInfo.exists()) {
        // Nothing to disclose. Let WebEngine report the missing file.
        return true;
    }
    if (!requestedInfo.isFile()) {
        return false;
    }
    // Symlinks (of the file or any parent component) must also resolve
    // inside the directory, otherwise a link planted next to the document
    // could expose files elsewhere on the system.
    const QString canonical = cleanAbsolute(requestedInfo.canonicalFilePath());
    if (canonical.isEmpty()) {
        return false;
    }
    return isUnder(canonical, canonicalDir);
}

} // namespace preview_policy
