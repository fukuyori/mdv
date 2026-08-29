#pragma once

#include <QString>
#include <QUrl>

// Decides whether the preview page may load a local resource referenced by
// an untrusted Markdown document. Only regular files located under the
// document's directory are allowed, after resolving "..", "." and symbolic
// links, so a document cannot pull arbitrary files into the preview.
namespace preview_policy {

// documentDir is the directory of the document being previewed. Returns
// false for anything that is not a file: URL, for paths that escape the
// directory (including through symlinks), and for directories.
bool allowsLocalResource(const QUrl &url, const QString &documentDir);

} // namespace preview_policy
