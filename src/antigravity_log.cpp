#include "antigravity_log.h"

#include <QDateTime>
#include <QDir>
#include <QDirIterator>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QStringList>

#include <algorithm>
#include <utility>
#include <vector>

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

bool isConversationStep(const QJsonObject &entry)
{
    const QString type = entry.value(QStringLiteral("type")).toString();
    const QString source = entry.value(QStringLiteral("source")).toString();

    // User input step
    if (type == QLatin1String("USER_INPUT") || source == QLatin1String("USER_EXPLICIT") || source == QLatin1String("USER")) {
        return true;
    }
    // Assistant / Planner response step
    if (type == QLatin1String("PLANNER_RESPONSE") || source == QLatin1String("MODEL")) {
        return true;
    }
    return false;
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

QString extractTextFromContent(const QJsonValue &contentVal)
{
    if (contentVal.isString()) {
        return contentVal.toString();
    }
    if (contentVal.isObject()) {
        const QJsonObject obj = contentVal.toObject();
        if (obj.contains(QStringLiteral("text"))) {
            return obj.value(QStringLiteral("text")).toString();
        }
        if (obj.contains(QStringLiteral("formatted"))) {
            return obj.value(QStringLiteral("formatted")).toString();
        }
    }
    if (contentVal.isArray()) {
        QStringList parts;
        const QJsonArray arr = contentVal.toArray();
        for (const QJsonValue &v : arr) {
            const QString sub = extractTextFromContent(v);
            if (!sub.isEmpty()) {
                parts.append(sub);
            }
        }
        return parts.join(QLatin1String("\n\n"));
    }
    return {};
}

QString stepMessageText(const QJsonObject &entry)
{
    const QJsonValue content = entry.value(QStringLiteral("content"));
    return extractTextFromContent(content);
}

bool isUserRole(const QJsonObject &entry)
{
    const QString type = entry.value(QStringLiteral("type")).toString();
    const QString source = entry.value(QStringLiteral("source")).toString();
    if (type == QLatin1String("USER_INPUT") || source == QLatin1String("USER_EXPLICIT") || source == QLatin1String("USER")) {
        return true;
    }
    return false;
}

} // namespace

bool isAntigravitySessionLog(const QString &path, const QByteArray &sample)
{
    const QFileInfo info(path);
    if (info.suffix().compare(QLatin1String("jsonl"), Qt::CaseInsensitive) != 0) {
        return false;
    }

    const QString fileName = info.fileName().toLower();
    const bool isTranscriptFile = (fileName == QLatin1String("transcript.jsonl") || fileName == QLatin1String("transcript_full.jsonl"));

    qsizetype start = 0;
    int inspected = 0;
    while (start < sample.size() && inspected < 200) {
        qsizetype end = sample.indexOf('\n', start);
        if (end < 0) {
            end = sample.size();
        }
        const QJsonObject entry = parseObject(sample.mid(start, end - start));
        if (!entry.isEmpty()) {
            if (entry.contains(QStringLiteral("step_index"))) {
                return true;
            }
            if (isConversationStep(entry)) {
                return true;
            }
            const QString type = entry.value(QStringLiteral("type")).toString();
            const QString source = entry.value(QStringLiteral("source")).toString();
            if (type == QLatin1String("STEP_UPDATE") || type == QLatin1String("SUBAGENT_RESPONSE")
                || source == QLatin1String("SYSTEM")) {
                return true;
            }
        }
        start = end + 1;
        ++inspected;
    }

    if (isTranscriptFile && sample.trimmed().startsWith('{')) {
        return true;
    }

    return false;
}

QString latestAntigravitySession(const QString &brainRoot)
{
    const QString root = brainRoot.isEmpty()
        ? QDir::home().filePath(QStringLiteral(".gemini/antigravity-cli/brain"))
        : brainRoot;

    std::vector<std::pair<QDateTime, QString>> candidates;
    QDirIterator it(
        root, QStringList() << QStringLiteral("transcript.jsonl") << QStringLiteral("transcript_full.jsonl"),
        QDir::Files | QDir::Hidden | QDir::NoDotAndDotDot,
        QDirIterator::Subdirectories);
    while (it.hasNext()) {
        it.next();
        const QFileInfo info = it.fileInfo();
        candidates.emplace_back(info.lastModified(), info.absoluteFilePath());
    }
    std::sort(candidates.begin(), candidates.end(),
        [](const auto &a, const auto &b) {
            if (a.first != b.first) {
                return a.first > b.first;
            }
            return a.second > b.second;
        });

    for (const auto &[modified, path] : candidates) {
        Q_UNUSED(modified);
        QFile file(path);
        if (!file.open(QIODevice::ReadOnly)) {
            continue;
        }
        const QByteArray sample = file.read(4096);
        if (isAntigravitySessionLog(path, sample)) {
            return path;
        }
    }
    return {};
}

QString renderAntigravitySessionLog(
    const QString &jsonl,
    const QString &title,
    const QString &userLabel,
    const QString &assistantLabel,
    const QString &emptyLabel)
{
    QString markdown = QStringLiteral("# ") + title + QLatin1String("\n\n");
    int messageCount = 0;

    QString pendingRole;
    QString pendingTimestamp;
    QStringList pendingParts;

    const auto flush = [&]() {
        if (pendingParts.isEmpty()) {
            pendingRole.clear();
            return;
        }
        const bool user = (pendingRole == QLatin1String("user"));
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
        if (isConversationStep(entry)) {
            const bool user = isUserRole(entry);
            const QString role = user ? QStringLiteral("user") : QStringLiteral("assistant");
            if (role != pendingRole) {
                flush();
                pendingRole = role;
            }
            const QString text = stepMessageText(entry);
            if (!text.isEmpty()) {
                if (pendingTimestamp.isEmpty()) {
                    if (entry.contains(QStringLiteral("timestamp"))) {
                        pendingTimestamp = entry.value(QStringLiteral("timestamp")).toString();
                    } else if (entry.contains(QStringLiteral("created_at"))) {
                        pendingTimestamp = entry.value(QStringLiteral("created_at")).toString();
                    } else if (entry.contains(QStringLiteral("time"))) {
                        pendingTimestamp = entry.value(QStringLiteral("time")).toString();
                    }
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
