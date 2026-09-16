#include "translation_validation.h"

#include <QRegularExpression>
#include <QStringList>

namespace translation_validation {
namespace {

QStringList markdownStructure(const QString &markdown)
{
    static const QRegularExpression fence(QStringLiteral(R"(^\s*(`{3,}|~{3,}))"));
    static const QRegularExpression heading(QStringLiteral(R"(^\s{0,3}(#{1,6})(?:\s+|$))"));
    static const QRegularExpression unorderedItem(QStringLiteral(R"(^\s*[-+*]\s+)"));
    static const QRegularExpression orderedItem(QStringLiteral(R"(^\s*\d+[.)]\s+)"));
    static const QRegularExpression quote(QStringLiteral(R"(^\s*(>\s*)+)"));
    static const QRegularExpression thematicBreak(
        QStringLiteral(R"(^\s{0,3}(?:(?:\*\s*){3,}|(?:-\s*){3,}|(?:_\s*){3,})$)"));
    static const QRegularExpression tableSeparator(
        QStringLiteral(R"(^\s*\|?\s*:?-{3,}:?\s*(?:\|\s*:?-{3,}:?\s*)+\|?\s*$)"));

    QStringList structure;
    const QStringList lines = markdown.split(QLatin1Char('\n'));
    for (const QString &line : lines) {
        QRegularExpressionMatch match = fence.match(line);
        if (match.hasMatch()) {
            const QString marker = match.captured(1);
            structure.append(QStringLiteral("fence:%1:%2").arg(marker.front()).arg(marker.size()));
            continue;
        }

        match = heading.match(line);
        if (match.hasMatch()) {
            structure.append(QStringLiteral("heading:%1").arg(match.capturedLength(1)));
            continue;
        }
        if (unorderedItem.match(line).hasMatch()) {
            structure.append(QStringLiteral("unordered-item"));
            continue;
        }
        if (orderedItem.match(line).hasMatch()) {
            structure.append(QStringLiteral("ordered-item"));
            continue;
        }
        if (quote.match(line).hasMatch()) {
            structure.append(QStringLiteral("quote"));
            continue;
        }
        if (thematicBreak.match(line).hasMatch()) {
            structure.append(QStringLiteral("thematic-break"));
            continue;
        }
        if (tableSeparator.match(line).hasMatch()) {
            structure.append(QStringLiteral("table-separator"));
        }
    }
    return structure;
}

} // namespace

QString rejectionReason(const QString &source, const QString &response)
{
    const QString trimmedResponse = response.trimmed();
    if (trimmedResponse.isEmpty()) {
        return QStringLiteral("empty response");
    }

    // Translations can expand, but a many-times-larger response to a short
    // fragment is normally an unrelated document or model commentary. Keep a
    // generous fixed allowance for very short fragments and a proportional
    // allowance for ordinary paragraphs.
    const qsizetype maximumLength = qMax<qsizetype>(256, source.trimmed().size() * 6 + 128);
    if (trimmedResponse.size() > maximumLength) {
        return QStringLiteral("translation response expanded unexpectedly");
    }

    // The translator is instructed to preserve Markdown. Reject added or
    // removed headings, list items, quotes, fences, and other block markers;
    // otherwise a hallucinated document can replace the intended preview.
    if (markdownStructure(source) != markdownStructure(trimmedResponse)) {
        return QStringLiteral("translation response changed Markdown structure");
    }

    return {};
}

} // namespace translation_validation
