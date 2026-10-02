#include "preview_interceptor.h"

#include "preview_policy.h"

void PreviewRequestInterceptor::interceptRequest(QWebEngineUrlRequestInfo &info)
{
    if (info.resourceType() == QWebEngineUrlRequestInfo::ResourceTypeMainFrame) {
        return;
    }
    const QUrl url = info.requestUrl();
    if (url.scheme() == QLatin1String("data")) {
        return;
    }
    const auto type = info.resourceType();
    if (type == QWebEngineUrlRequestInfo::ResourceTypeImage
        && preview_policy::allowsRemoteImage(url)) {
        return;
    }
    if ((type == QWebEngineUrlRequestInfo::ResourceTypeImage
            || type == QWebEngineUrlRequestInfo::ResourceTypeMedia)
        && preview_policy::allowsLocalResource(url, documentDir_)) {
        return;
    }
    info.block(true);
}
