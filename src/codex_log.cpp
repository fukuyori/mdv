#include "codex_log.h"

#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QStringList>

namespace mdv {
namespace {

QJsonObject parseObject(const QByteArray &line)
{
    QJsonParseError error;
    const QJsonDocument document = QJsonDocument::fromJson(line, &error);
    if (error.error != QJsonParseError::NoError || !document.isObject()) {
        return {};
    }
    return document.object();
}

bool isConversationMessage(const QJsonObject &event)
{
    if (event.value(QStringLiteral("type")).toString() != QLatin1String("response_item")) {
        return false;
    }
    const QJsonObject payload = event.value(QStringLiteral("payload")).toObject();
    if (payload.value(QStringLiteral("type")).toString() != QLatin1String("message")) {
        return false;
    }
    const QString role = payload.value(QStringLiteral("role")).toString();
    return role == QLatin1String("user") || role == QLatin1String("assistant");
}

QString quotedMarkdown(const QString &text)
{
    QString normalized = text;
    normalized.replace(QLatin1String("\r\n"), QLatin1String("\n"));
    normalized.replace(QLatin1Char('\r'), QLatin1Char('\n'));

    QString result;
    const QStringList lines = normalized.split(QLatin1Char('\n'), Qt::KeepEmptyParts);
    for (const QString &line : lines) {
        result += line.isEmpty() ? QStringLiteral(">\n") : QStringLiteral("> ") + line + QLatin1Char('\n');
    }
    return result;
}

QString messageText(const QJsonObject &payload)
{
    QStringList parts;
    const QJsonArray content = payload.value(QStringLiteral("content")).toArray();
    for (const QJsonValue &value : content) {
        const QJsonObject item = value.toObject();
        const QString type = item.value(QStringLiteral("type")).toString();
        if (type != QLatin1String("input_text") && type != QLatin1String("output_text")) {
            continue;
        }
        const QString text = item.value(QStringLiteral("text")).toString();
        if (!text.isEmpty()) {
            parts.append(text);
        }
    }
    return parts.join(QLatin1String("\n\n"));
}

} // namespace

bool isCodexEventLog(const QString &path, const QByteArray &sample)
{
    const QFileInfo info(path);
    if (info.suffix().compare(QLatin1String("jsonl"), Qt::CaseInsensitive) != 0) {
        return false;
    }
    if (info.completeBaseName().startsWith(QLatin1String("rollout-"))) {
        return true;
    }

    qsizetype start = 0;
    int inspected = 0;
    while (start < sample.size() && inspected < 200) {
        qsizetype end = sample.indexOf('\n', start);
        if (end < 0) {
            end = sample.size();
        }
        const QJsonObject event = parseObject(sample.mid(start, end - start));
        if (event.value(QStringLiteral("type")).toString() == QLatin1String("session_meta")) {
            const QJsonObject payload = event.value(QStringLiteral("payload")).toObject();
            if (!payload.value(QStringLiteral("id")).toString().isEmpty()
                && !payload.value(QStringLiteral("cwd")).toString().isEmpty()) {
                return true;
            }
        }
        if (isConversationMessage(event)) {
            return true;
        }
        start = end + 1;
        ++inspected;
    }
    return false;
}

QString renderCodexEventLog(
    const QString &jsonl,
    const QString &title,
    const QString &userLabel,
    const QString &assistantLabel,
    const QString &emptyLabel)
{
    QString markdown = QStringLiteral("# ") + title + QLatin1String("\n\n");
    int messageCount = 0;
    qsizetype start = 0;
    while (start < jsonl.size()) {
        qsizetype end = jsonl.indexOf(QLatin1Char('\n'), start);
        if (end < 0) {
            end = jsonl.size();
        }

        const QByteArray line = QStringView(jsonl).mid(start, end - start).toUtf8();
        const QJsonObject event = parseObject(line);
        if (isConversationMessage(event)) {
            const QJsonObject payload = event.value(QStringLiteral("payload")).toObject();
            const QString text = messageText(payload);
            if (!text.isEmpty()) {
                const bool user = payload.value(QStringLiteral("role")).toString()
                    == QLatin1String("user");
                const QString timestamp = event.value(QStringLiteral("timestamp")).toString();
                markdown += QStringLiteral("## ") + (user ? userLabel : assistantLabel);
                if (!timestamp.isEmpty()) {
                    markdown += QStringLiteral(" — `") + timestamp.toHtmlEscaped()
                        + QLatin1Char('`');
                }
                markdown += QLatin1String("\n\n") + quotedMarkdown(text) + QLatin1Char('\n');
                ++messageCount;
            }
        }

        start = end + 1;
    }

    if (messageCount == 0) {
        markdown += QStringLiteral("> ") + emptyLabel + QLatin1Char('\n');
    }
    return markdown;
}

} // namespace mdv
