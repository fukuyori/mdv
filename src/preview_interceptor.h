#pragma once

#include <QString>
#include <QWebEngineUrlRequestInfo>
#include <QWebEngineUrlRequestInterceptor>

// Restricts what the preview page may fetch. The document itself arrives via
// setHtml, so every other request is a sub-resource referenced from untrusted
// Markdown. Only images and media loaded from the document's own directory
// are allowed; everything else (other local files, other schemes, fonts,
// scripts, frames, XHR) is blocked before it reaches the network layer.
// This backs up the CSP so that relative images keep working without
// granting the page general file: access.
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
