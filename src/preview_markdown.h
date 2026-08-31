#pragma once

#include <QHash>
#include <QList>
#include <QString>

namespace preview_markdown {

struct Block {
    QString text;
    int position = 0;
};

enum class Mode {
    Original,
    Bilingual,
    Translated,
};

QList<Block> splitBlocks(const QString &markdown);
bool isTranslatable(const QString &block);

QString composeExport(
    const QString &source,
    Mode mode,
    const QHash<QString, QString> &translations,
    const QString &pendingMarker,
    const QString &failedMarker);

} // namespace preview_markdown
