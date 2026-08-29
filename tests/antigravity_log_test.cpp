#include "antigravity_log.h"

#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QTemporaryDir>

#include <cstdlib>
#include <iostream>

namespace {

void require(bool condition, const char *message)
{
    if (!condition) {
        std::cerr << message << '\n';
        std::exit(EXIT_FAILURE);
    }
}

void writeLog(const QString &path, const QByteArray &data, const QDateTime &modified)
{
    QFile file(path);
    require(file.open(QIODevice::WriteOnly), "could not create a test transcript log");
    require(file.write(data) == data.size(), "could not write a test transcript log");
    require(file.flush(), "could not flush a test transcript log");
    require(file.setFileTime(modified, QFileDevice::FileModificationTime),
        "could not set a test transcript timestamp");
}

} // namespace

int main()
{
    const QByteArray transcriptSample =
        R"json({"step_index":0,"source":"USER_EXPLICIT","type":"USER_INPUT","status":"DONE","content":"Hello Antigravity","timestamp":"2026-08-29T10:00:00.000Z"})json";
    require(mdv::isAntigravitySessionLog("transcript.jsonl", transcriptSample),
        "Antigravity transcript entry was not detected");

    require(!mdv::isAntigravitySessionLog("events.jsonl", R"json({"type":"ordinary"})json"),
        "ordinary JSONL was misidentified as an Antigravity log");
    require(!mdv::isAntigravitySessionLog("transcript.md", transcriptSample),
        "a non-JSONL file was misidentified as an Antigravity log");

    const QString jsonl = QString::fromUtf8(
        R"json({"step_index":0,"source":"USER_EXPLICIT","type":"USER_INPUT","status":"DONE","content":"First line\n\n- item","timestamp":"2026-08-29T10:00:00.000Z"})json"
        "\n"
        R"json({"step_index":1,"source":"MODEL","type":"PLANNER_RESPONSE","status":"DONE","content":"Thinking and responding","timestamp":"2026-08-29T10:00:01.000Z"})json"
        "\n"
        R"json({"step_index":2,"source":"SYSTEM","type":"STEP_UPDATE","status":"DONE","content":"internal update"})json"
        "\n"
        R"json({"step_index":3,"source":"MODEL","type":"PLANNER_RESPONSE","status":"DONE","content":"Final answer","timestamp":"2026-08-29T10:00:02.000Z"})json"
        "\n{incomplete");

    const QString markdown = mdv::renderAntigravitySessionLog(
        jsonl, QStringLiteral("Antigravity Log"), QStringLiteral("User"),
        QStringLiteral("Assistant"), QStringLiteral("No messages"));

    require(markdown.contains(QLatin1String("# Antigravity Log")),
        "conversation title is missing");
    require(markdown.contains(QStringLiteral("## User — `2026-08-29T10:00:00.000Z`")),
        "user heading or timestamp is missing");
    require(markdown.contains(QLatin1String("> First line\n>\n> - item")),
        "multiline user Markdown was not quoted correctly");
    require(markdown.contains(QStringLiteral("## Assistant — `2026-08-29T10:00:01.000Z`")),
        "assistant heading or timestamp is missing");
    require(markdown.contains(QLatin1String("> Thinking and responding")),
        "assistant text is missing");
    require(markdown.contains(QLatin1String("> Final answer")),
        "assistant final answer is missing");
    require(!markdown.contains(QLatin1String("internal update")),
        "internal update step was rendered");

    QTemporaryDir brainDir;
    require(brainDir.isValid(), "could not create a temporary brain directory");
    QDir root(brainDir.path());
    require(root.mkpath(QStringLiteral("conv1/.system_generated/logs")),
        "could not create conv1 directory");
    require(root.mkpath(QStringLiteral("conv2/.system_generated/logs")),
        "could not create conv2 directory");

    const QString older = root.filePath(QStringLiteral("conv1/.system_generated/logs/transcript.jsonl"));
    const QString newest = root.filePath(QStringLiteral("conv2/.system_generated/logs/transcript.jsonl"));
    const QDateTime now = QDateTime::currentDateTimeUtc();

    writeLog(older, transcriptSample, now.addSecs(-20));
    writeLog(newest, transcriptSample, now.addSecs(-10));

    const QString selected = mdv::latestAntigravitySession(brainDir.path());
    require(selected == newest, "the newest valid Antigravity session was not selected");

    const QString empty = mdv::renderAntigravitySessionLog(
        QStringLiteral("{broken"), QStringLiteral("Conversation"), QStringLiteral("User"),
        QStringLiteral("Assistant"), QStringLiteral("No messages"));
    require(empty.contains(QLatin1String("> No messages")),
        "empty-log marker is missing");

    return EXIT_SUCCESS;
}
