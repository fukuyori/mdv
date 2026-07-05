#include <QGuiApplication>
#include <QImage>
#include <QPainter>
#include <QSvgRenderer>

#include <iostream>

int main(int argc, char *argv[])
{
    QGuiApplication app(argc, argv);

    if (argc != 4) {
        std::cerr << "Usage: mdv_icon_renderer <input.svg> <size> <output.png>\n";
        return 2;
    }

    const QString inputPath = QString::fromLocal8Bit(argv[1]);
    const int size = QString::fromLocal8Bit(argv[2]).toInt();
    const QString outputPath = QString::fromLocal8Bit(argv[3]);

    if (size <= 0) {
        std::cerr << "Invalid size\n";
        return 2;
    }

    QSvgRenderer renderer(inputPath);
    if (!renderer.isValid()) {
        std::cerr << "Invalid SVG: " << inputPath.toStdString() << "\n";
        return 1;
    }

    QImage image(size, size, QImage::Format_ARGB32_Premultiplied);
    image.fill(Qt::transparent);

    QPainter painter(&image);
    painter.setRenderHint(QPainter::Antialiasing);
    painter.setRenderHint(QPainter::SmoothPixmapTransform);
    renderer.render(&painter);
    painter.end();

    if (!image.save(outputPath, "PNG")) {
        std::cerr << "Could not write: " << outputPath.toStdString() << "\n";
        return 1;
    }

    return 0;
}
