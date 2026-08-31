#include "preview_markdown.h"

#include <QStringList>

namespace preview_markdown {

QList<Block> splitBlocks(const QString &markdown)
{
    QList<Block> blocks;
    QStringList current;
    int currentStart = 0;
    int lineStart = 0;
    bool inFence = false;
    bool inDisplayMath = false;

    const QStringList lines = markdown.split(QLatin1Char('\n'));
    for (const QString &line : lines) {
        const QString trimmed = line.trimmed();
        if (!inFence && !inDisplayMath && trimmed.isEmpty()) {
            if (!current.isEmpty()) {
                blocks.append({current.join(QLatin1Char('\n')), currentStart});
                current.clear();
            }
        } else {
            if (current.isEmpty()) {
                currentStart = lineStart;
            }
            current.append(line);
            if (!inDisplayMath
                && (trimmed.startsWith(QLatin1String("```"))
                    || trimmed.startsWith(QLatin1String("~~~")))) {
                inFence = !inFence;
            }
            if (!inFence && trimmed == QLatin1String("$$")) {
                inDisplayMath = !inDisplayMath;
            }
        }
        lineStart += static_cast<int>(line.size()) + 1;
    }
    if (!current.isEmpty()) {
        blocks.append({current.join(QLatin1Char('\n')), currentStart});
    }

    return blocks;
}

bool isTranslatable(const QString &block)
{
    const QString trimmed = block.trimmed();
    if (trimmed.startsWith(QLatin1String("```")) || trimmed.startsWith(QLatin1String("~~~"))) {
        return false;
    }
    if (trimmed.size() >= 4 && trimmed.startsWith(QLatin1String("$$"))
        && trimmed.endsWith(QLatin1String("$$"))) {
        return false;
    }
    for (const QChar c : trimmed) {
        if (c.isLetter()) {
            return true;
        }
    }
    return false;
}

QString composeExport(
    const QString &source,
    Mode mode,
    const QHash<QString, QString> &translations,
    const QString &pendingMarker,
    const QString &failedMarker)
{
    if (mode == Mode::Original) {
        return source;
    }

    const QList<Block> blocks = splitBlocks(source);
    QStringList parts;
    parts.reserve(mode == Mode::Bilingual ? blocks.size() * 2 : blocks.size());

    for (const Block &block : blocks) {
        if (mode == Mode::Bilingual) {
            parts.append(block.text);
        }
        if (!isTranslatable(block.text)) {
            if (mode == Mode::Translated) {
                parts.append(block.text);
            }
            continue;
        }

        const auto translation = translations.constFind(block.text);
        if (translation == translations.cend()) {
            parts.append(mode == Mode::Translated ? block.text : pendingMarker);
        } else if (translation->isEmpty()) {
            if (mode == Mode::Translated) {
                parts.append(block.text);
            } else {
                parts.append(failedMarker);
            }
        } else {
            parts.append(*translation);
        }
    }

    return parts.join(QLatin1String("\n\n"));
}

} // namespace preview_markdown
