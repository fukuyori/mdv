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

// Returns true only for HTTPS URLs suitable for an image request. Other
// remote schemes and URLs containing embedded credentials are rejected.
bool allowsRemoteImage(const QUrl &url);

// The Content Security Policy applied to the preview document. Only scripts
// carrying `nonce` run; images may additionally come from HTTPS, while local
// images/media are further narrowed by PreviewRequestInterceptor. Everything
// else is denied.
QString contentSecurityPolicy(const QString &nonce);

} // namespace preview_policy
