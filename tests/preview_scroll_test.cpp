#include "preview_scroll_script.h"

#include <QApplication>
#include <QEventLoop>
#include <QTimer>
#include <QWebEnginePage>
#include <QWebEngineView>

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

    QWebEngineView view;
    view.resize(800, 300);
    view.show();
    QWebEnginePage &page = *view.page();

    QEventLoop loadLoop;
    bool loaded = false;
    QObject::connect(&page, &QWebEnginePage::loadFinished, [&](bool ok) {
        loaded = ok;
        loadLoop.quit();
    });
    QTimer::singleShot(10000, &loadLoop, &QEventLoop::quit);
    const QString html = QStringLiteral(
        "<!DOCTYPE html><html><body style=\"margin:0\">"
        "<div id=\"content\">"
        "<div style=\"height:400px\">first</div>"
        "<div style=\"height:400px\">second</div>"
        "<div style=\"height:400px\">third</div>"
        "</div><script>var __mdvProgTs=0;var __mdvRenderGeneration=1;")
        + preview_scroll::script()
        + QStringLiteral("</script></body></html>");
    page.setHtml(html);
    loadLoop.exec();
    require(loaded, "page did not finish loading");

    const int restoredY = runJs(page, QStringLiteral(
        "window.scrollTo(0,450);"
        "var c=document.getElementById('content');"
        "var anchor=__mdvCaptureScrollAnchor(c);"
        "c.innerHTML='<div style=\"height:800px\">first translated</div>'"
        "+'<div style=\"height:400px\">second</div>'"
        "+'<div style=\"height:400px\">third</div>';"
        "__mdvRestoreScrollAnchor(c,anchor,1);"
        "window.scrollY;")).toInt();

    require(restoredY >= 845 && restoredY <= 855,
        "content replacement did not preserve the visible block and offset");

    std::cout << "preview scroll preservation test passed\n";
    return EXIT_SUCCESS;
}
