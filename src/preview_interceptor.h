#pragma once

#include <QString>
#include <QWebEngineUrlRequestInfo>
#include <QWebEngineUrlRequestInterceptor>

// Restricts what the preview page may fetch. The document itself arrives via
// setHtml, so every other request is a sub-resource referenced from untrusted
// Markdown. Images and media loaded from the document's own directory and
// HTTPS images are allowed; everything else (other local files, remote media,
// insecure schemes, fonts, scripts, frames, XHR) is blocked before it reaches
// the network layer. This backs up the CSP so that relative and HTTPS images
// work without granting broader file or network access.
class PreviewRequestInterceptor : public QWebEngineUrlRequestInterceptor {
public:
    using QWebEngineUrlRequestInterceptor::QWebEngineUrlRequestInterceptor;

    void setDocumentDirectory(const QString &dir)
    {
        documentDir_ = dir;
    }

    void interceptRequest(QWebEngineUrlRequestInfo &info) override;

private:
    QString documentDir_;
};
