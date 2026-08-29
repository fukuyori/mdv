// Loads a hostile document into a real, offscreen WebEngine page configured
// exactly like the preview (same settings, CSP, and request interceptor)
// and verifies that nothing escapes: no script executes, only the image
// inside the document directory loads, and no request reaches a local
// HTTP server that the document tries to contact in several ways.

#include "preview_interceptor.h"
#include "preview_policy.h"

#include <QApplication>
#include <QDir>
#include <QEventLoop>
#include <QFile>
#include <QTcpServer>
#include <QTcpSocket>
#include <QTemporaryDir>
#include <QTimer>
#include <QWebEnginePage>
#include <QWebEngineSettings>

#include <cstdlib>
#include <iostream>

namespace {

void require(bool condition, const char *message)
{
    if (!condition) {
        std::cerr << "FAIL: " << message << '\n';
        std::exit(EXIT_FAILURE);
    }
}

bool writeFile(const QString &path, const QByteArray &data)
{
    QFile file(path);
    return file.open(QIODevice::WriteOnly) && file.write(data) == data.size();
}

// Smallest valid 1x1 PNG.
QByteArray onePixelPng()
{
    return QByteArray::fromBase64(
        "iVBORw0KGgoAAAANSUhEUgAAAAEAAAABCAYAAAAfFcSJAAAADUlEQVR42mNk+M9QDwADhgGAWjR9awAAAABJRU5ErkJggg==");
}

QVariant runJs(QWebEnginePage &page, const QString &script)
{
    QVariant result;
    QEventLoop loop;
    page.runJavaScript(script, [&](const QVariant &value) {
        result = value;
        loop.quit();
    });
    QTimer::singleShot(10000, &loop, &QEventLoop::quit);
    loop.exec();
    return result;
}

} // namespace

int main(int argc, char **argv)
{
    QApplication app(argc, argv);

    // Anything the page manages to contact lands here.
    QTcpServer trap;
    require(trap.listen(QHostAddress::LocalHost, 0), "trap server listen");
    int trapHits = 0;
    QObject::connect(&trap, &QTcpServer::newConnection, [&] {
        while (QTcpSocket *socket = trap.nextPendingConnection()) {
            ++trapHits;
            socket->write("HTTP/1.1 200 OK\r\nContent-Length: 0\r\n\r\n");
            socket->disconnectFromHost();
            socket->deleteLater();
        }
    });
    const QString trapUrl = QStringLiteral("http://127.0.0.1:%1/").arg(trap.serverPort());

    QTemporaryDir root;
    require(root.isValid(), "temporary directory");
    const QString docDir = root.filePath("doc");
    const QString outside = root.filePath("outside");
    require(QDir().mkpath(docDir + "/assets") && QDir().mkpath(outside), "mkpath");
    require(writeFile(docDir + "/assets/ok.png", onePixelPng()), "write ok.png");
    require(writeFile(outside + "/secret.png", onePixelPng()), "write secret.png");
    require(writeFile(outside + "/secret.txt", "top secret"), "write secret.txt");
#ifdef Q_OS_UNIX
    require(QFile::link(outside + "/secret.png", docDir + "/link.png"), "symlink");
#endif

    const QString nonce = QStringLiteral("test-nonce-0123456789");
    const QString html = QStringLiteral(
        "<!DOCTYPE html><html><head><meta charset=\"utf-8\">"
        "<meta http-equiv=\"Content-Security-Policy\" content=\"%1\"></head><body>"
        // Hostile content as it would appear if raw HTML were ever let through.
        "<script>window.__pwned = 'inline';</script>"
        "<img id=\"evt\" src=\"x\" onerror=\"window.__pwned = 'handler'\">"
        "<img id=\"ok\" src=\"assets/ok.png\">"
        "<img id=\"esc\" src=\"../outside/secret.png\">"
        "<img id=\"abs\" src=\"%3\">"
        "<img id=\"sys\" src=\"file:///etc/hostname\">"
        "<img id=\"link\" src=\"link.png\">"
        "<img id=\"remote\" src=\"%2track.png\">"
        "<iframe id=\"frame\" src=\"%2frame\"></iframe>"
        "<object data=\"%2object\"></object>"
        "<link rel=\"stylesheet\" href=\"%2style.css\">"
        "<form id=\"form\" action=\"%2form\" method=\"post\"></form>"
        // Our own (nonce'd) script, trying the things page JS could try.
        "<script nonce=\"%4\">"
        "window.__ownScript = true;"
        "window.__fetchDone = false; window.__fetchOk = false;"
        "fetch('%2fetch').then(function(){window.__fetchOk = true;}).catch(function(){})"
        ".finally(function(){window.__fetchDone = true;});"
        "try { var x = new XMLHttpRequest(); x.open('GET', 'file://%5/secret.txt', false); x.send();"
        "  window.__xhrText = x.responseText; } catch (e) { window.__xhrText = ''; }"
        "try { new WebSocket('ws://127.0.0.1:%6/ws'); } catch (e) {}"
        "try { navigator.sendBeacon('%2beacon', 'x'); } catch (e) {}"
        "</script>"
        "</body></html>")
        .arg(preview_policy::contentSecurityPolicy(nonce).toHtmlEscaped())
        .arg(trapUrl)
        .arg(QUrl::fromLocalFile(outside + "/secret.png").toString())
        .arg(nonce)
        .arg(outside)
        .arg(trap.serverPort());

    QWebEnginePage page;
    page.settings()->setAttribute(QWebEngineSettings::LocalContentCanAccessRemoteUrls, false);
    page.settings()->setAttribute(QWebEngineSettings::JavascriptCanOpenWindows, false);
    page.settings()->setAttribute(QWebEngineSettings::JavascriptCanAccessClipboard, false);
    page.settings()->setAttribute(QWebEngineSettings::LocalStorageEnabled, false);
    auto *interceptor = new PreviewRequestInterceptor(&page);
    interceptor->setDocumentDirectory(docDir);
    page.setUrlRequestInterceptor(interceptor);

    QEventLoop loadLoop;
    bool loaded = false;
    QObject::connect(&page, &QWebEnginePage::loadFinished, [&](bool ok) {
        loaded = ok;
        loadLoop.quit();
    });
    QTimer::singleShot(30000, &loadLoop, &QEventLoop::quit);
    page.setHtml(html, QUrl::fromLocalFile(docDir + "/"));
    loadLoop.exec();
    require(loaded, "page did not finish loading");

    // Let image loads, the fetch promise, and any sneaky connections settle.
    QEventLoop settle;
    QTimer::singleShot(1500, &settle, &QEventLoop::quit);
    settle.exec();

    const auto width = [&](const char *id) {
        return runJs(page, QStringLiteral("document.getElementById('%1').naturalWidth").arg(id)).toInt();
    };
    const auto js = [&](const char *expr) { return runJs(page, QString::fromLatin1(expr)); };

    require(js("window.__ownScript").toBool(), "nonce'd script must run (test harness sanity)");
    require(js("window.__fetchDone").toBool(), "fetch must have settled");
    require(js("typeof window.__pwned").toString() == "undefined",
        "inline script or event handler executed");
    require(width("ok") == 1, "image inside the document directory must load");
    require(width("esc") == 0, "'..' escape image must not load");
    require(width("abs") == 0, "absolute path outside the directory must not load");
    require(width("sys") == 0, "/etc/hostname must not load");
#ifdef Q_OS_UNIX
    require(width("link") == 0, "symlink pointing outside must not load");
#endif
    require(width("remote") == 0, "remote image must not load");
    require(!js("window.__fetchOk").toBool(), "fetch must be blocked");
    require(js("window.__xhrText").toString().isEmpty(), "XHR must not read a local file");
    require(js("document.getElementById('frame').contentDocument === null "
               "|| document.getElementById('frame').contentDocument.body.innerHTML === ''").toBool(),
        "iframe must not load");
    require(trapHits == 0, "the page reached the local HTTP server");

    std::cout << "preview WebEngine security test passed\n";
    return EXIT_SUCCESS;
}
