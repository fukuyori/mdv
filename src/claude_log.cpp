#include "claude_log.h"

#include <QDir>
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

bool isConversationEntry(const QJsonObject &entry)
{
    const QString type = entry.value(QStringLiteral("type")).toString();
    if (type != QLatin1String("user") && type != QLatin1String("assistant")) {
        return false;
    }
    if (entry.value(QStringLiteral("isMeta")).toBool()
        || entry.value(QStringLiteral("isSidechain")).toBool()) {
        return false;
    }
    const QJsonObject message = entry.value(QStringLiteral("message")).toObject();
    return message.value(QStringLiteral("role")).toString() == type;
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

// Slash commands and their captured output are stored as user messages wrapped
// in command tags; they are bookkeeping, not conversation.
bool isCommandWrapper(const QString &text)
{
    const QString trimmed = text.trimmed();
    return trimmed.startsWith(QLatin1String("<command-name>"))
        || trimmed.startsWith(QLatin1String("<local-command-stdout>"))
        || trimmed.startsWith(QLatin1String("<local-command-stderr>"));
}

QString messageText(const QJsonObject &message)
{
    const QJsonValue content = message.value(QStringLiteral("content"));
    if (content.isString()) {
        const QString text = content.toString();
        return isCommandWrapper(text) ? QString() : text;
    }

    QStringList parts;
    const QJsonArray blocks = content.toArray();
    for (const QJsonValue &value : blocks) {
        const QJsonObject block = value.toObject();
        if (block.value(QStringLiteral("type")).toString() != QLatin1String("text")) {
            continue;
        }
        const QString text = block.value(QStringLiteral("text")).toString();
        if (!text.isEmpty() && !isCommandWrapper(text)) {
            parts.append(text);
        }
    }
    return parts.join(QLatin1String("\n\n"));
}

} // namespace

bool isClaudeSessionLog(const QString &path, const QByteArray &sample)
{
    const QFileInfo info(path);
    if (info.suffix().compare(QLatin1String("jsonl"), Qt::CaseInsensitive) != 0) {
        return false;
    }

    qsizetype start = 0;
    int inspected = 0;
    while (start < sample.size() && inspected < 200) {
        qsizetype end = sample.indexOf('\n', start);
        if (end < 0) {
            end = sample.size();
        }
        const QJsonObject entry = parseObject(sample.mid(start, end - start));
        const bool hasSessionId = !entry.value(QStringLiteral("sessionId")).toString().isEmpty();
        if (isConversationEntry(entry)
            && (hasSessionId || !entry.value(QStringLiteral("uuid")).toString().isEmpty())) {
            return true;
        }
        if (hasSessionId && !entry.value(QStringLiteral("type")).toString().isEmpty()) {
            return true;
        }
        start = end + 1;
        ++inspected;
    }
    return false;
}

QString claudeProjectDirName(const QString &cwd)
{
    QString name = cwd;
    for (QChar &ch : name) {
        const char16_t unit = ch.unicode();
        const bool alnum = (unit >= u'0' && unit <= u'9')
            || (unit >= u'A' && unit <= u'Z')
            || (unit >= u'a' && unit <= u'z');
        if (!alnum) {
            ch = QLatin1Char('-');
        }
    }
    return name;
}

QString latestClaudeSessionForCwd(const QString &cwd)
{
    const QDir projectDir(
        QDir::home().filePath(QStringLiteral(".claude/projects/") + claudeProjectDirName(cwd)));
    const QFileInfoList sessions = projectDir.entryInfoList(
        QStringList() << QStringLiteral("*.jsonl"), QDir::Files, QDir::Time);
    return sessions.isEmpty() ? QString() : sessions.first().absoluteFilePath();
}

QString renderClaudeSessionLog(
    const QString &jsonl,
    const QString &title,
    const QString &userLabel,
    const QString &assistantLabel,
    const QString &emptyLabel)
{
    QString markdown = QStringLiteral("# ") + title + QLatin1String("\n\n");
    int messageCount = 0;

    // One conversational turn is stored as several consecutive entries with the
    // same role (one per API message or content block); merge them into a
    // single section under the first entry's timestamp.
    QString pendingRole;
    QString pendingTimestamp;
    QStringList pendingParts;

    const auto flush = [&]() {
        if (pendingParts.isEmpty()) {
            pendingRole.clear();
            return;
        }
        const bool user = pendingRole == QLatin1String("user");
        markdown += QStringLiteral("## ") + (user ? userLabel : assistantLabel);
        if (!pendingTimestamp.isEmpty()) {
            markdown += QStringLiteral(" — `") + pendingTimestamp.toHtmlEscaped()
                + QLatin1Char('`');
        }
        markdown += QLatin1String("\n\n")
            + quotedMarkdown(pendingParts.join(QLatin1String("\n\n"))) + QLatin1Char('\n');
        ++messageCount;
        pendingRole.clear();
        pendingTimestamp.clear();
        pendingParts.clear();
    };

    qsizetype start = 0;
    while (start < jsonl.size()) {
        qsizetype end = jsonl.indexOf(QLatin1Char('\n'), start);
        if (end < 0) {
            end = jsonl.size();
        }

        const QByteArray line = QStringView(jsonl).mid(start, end - start).toUtf8();
        const QJsonObject entry = parseObject(line);
        if (isConversationEntry(entry)) {
            const QString role = entry.value(QStringLiteral("type")).toString();
            if (role != pendingRole) {
                flush();
                pendingRole = role;
            }
            const QString text = messageText(entry.value(QStringLiteral("message")).toObject());
            if (!text.isEmpty()) {
                if (pendingTimestamp.isEmpty()) {
                    pendingTimestamp = entry.value(QStringLiteral("timestamp")).toString();
                }
                pendingParts.append(text);
            }
        }

        start = end + 1;
    }
    flush();

    if (messageCount == 0) {
        markdown += QStringLiteral("> ") + emptyLabel + QLatin1Char('\n');
    }
    return markdown;
}

} // namespace mdv
