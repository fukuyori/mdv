#include "codex_log.h"

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
    require(file.open(QIODevice::WriteOnly), "could not create a test session log");
    require(file.write(data) == data.size(), "could not write a test session log");
    require(file.flush(), "could not flush a test session log");
    require(file.setFileTime(modified, QFileDevice::FileModificationTime),
        "could not set a test session timestamp");
}

} // namespace

int main()
{
    const QByteArray session = R"json({"timestamp":"2026-08-29T00:00:00.000Z","type":"session_meta","payload":{"id":"session-id","cwd":"/tmp"}})json";
    require(mdv::isCodexEventLog("renamed.jsonl", session),
        "Codex session metadata was not detected");
    require(mdv::isCodexEventLog("rollout-example.jsonl", {}),
        "Codex rollout filename was not detected");
    require(!mdv::isCodexEventLog("events.jsonl", R"json({"type":"ordinary"})json"),
        "ordinary JSONL was misidentified as a Codex log");
    require(!mdv::isCodexEventLog("rollout-example.md", session),
        "a non-JSONL file was misidentified as a Codex log");

    const QString jsonl = QString::fromUtf8(
        R"json({"timestamp":"2026-08-29T01:00:00.000Z","type":"response_item","payload":{"type":"message","role":"user","content":[{"type":"input_text","text":"First line\n\n- item"}]}})json"
        "\n"
        R"json({"timestamp":"2026-08-29T01:00:01.000Z","type":"event_msg","payload":{"type":"user_message","message":"duplicate"}})json"
        "\n"
        R"json({"timestamp":"2026-08-29T01:00:02.000Z","type":"response_item","payload":{"type":"message","role":"developer","content":[{"type":"input_text","text":"secret instructions"}]}})json"
        "\n"
        R"json({"timestamp":"2026-08-29T01:00:03.000Z","type":"response_item","payload":{"type":"custom_tool_call_output","output":"secret command output"}})json"
        "\n"
        R"json({"timestamp":"2026-08-29T01:00:04.000Z","type":"response_item","payload":{"type":"message","role":"assistant","content":[{"type":"output_text","text":"Answer"}]}})json"
        "\n{incomplete");

    const QString markdown = mdv::renderCodexEventLog(
        jsonl, QStringLiteral("Conversation"), QStringLiteral("User"),
        QStringLiteral("Assistant"), QStringLiteral("No messages"));
    require(markdown.contains(QLatin1String("# Conversation")),
        "conversation title is missing");
    require(markdown.contains(QStringLiteral("## User — `2026-08-29T01:00:00.000Z`")),
        "user heading or timestamp is missing");
    require(markdown.contains(QLatin1String("> First line\n>\n> - item")),
        "multiline user Markdown was not quoted correctly");
    require(markdown.contains(QStringLiteral("## Assistant — `2026-08-29T01:00:04.000Z`")),
        "assistant heading or timestamp is missing");
    require(markdown.contains(QLatin1String("> Answer")),
        "assistant text is missing");
    require(!markdown.contains(QLatin1String("duplicate")),
        "duplicate event message was rendered");
    require(!markdown.contains(QLatin1String("secret instructions")),
        "developer instructions were rendered");
    require(!markdown.contains(QLatin1String("secret command output")),
        "tool output was rendered");

    const QByteArray metaLine =
        R"json({"type":"session_meta","payload":{"id":"session-id","cwd":"/home/fuk/project"}})json";
    require(mdv::codexSessionMatchesCwd(metaLine, QStringLiteral("/home/fuk/project")),
        "matching session cwd was not detected");
    require(!mdv::codexSessionMatchesCwd(metaLine, QStringLiteral("/home/fuk/other")),
        "non-matching session cwd was accepted");
    require(!mdv::codexSessionMatchesCwd(R"json({"type":"ordinary"})json", QStringLiteral("/home/fuk/project")),
        "a log without session metadata was accepted");

    QTemporaryDir sessions;
    require(sessions.isValid(), "could not create a temporary sessions directory");
    QDir root(sessions.path());
    require(root.mkpath(QStringLiteral("2026/08/28")),
        "could not create an older session directory");
    require(root.mkpath(QStringLiteral("2026/08/29")),
        "could not create a newer session directory");
    const QString older = root.filePath(QStringLiteral("2026/08/28/rollout-older.jsonl"));
    const QString newest = root.filePath(QStringLiteral("2026/08/29/rollout-newest.jsonl"));
    const QString invalid = root.filePath(QStringLiteral("2026/08/29/rollout-invalid.jsonl"));
    const QDateTime now = QDateTime::currentDateTimeUtc();
    const QByteArray longMetaLine = QByteArrayLiteral(
        "{\"type\":\"session_meta\",\"payload\":{\"id\":\"long-session\","
        "\"cwd\":\"/home/fuk/project\",\"base_instructions\":\"")
        + QByteArray(20 * 1024, 'x') + QByteArrayLiteral("\"}}\n");
    require(mdv::codexSessionMatchesCwd(longMetaLine, QStringLiteral("/home/fuk/project")),
        "long session metadata was not parsed");
    writeLog(older, metaLine, now.addSecs(-20));
    writeLog(newest, longMetaLine, now.addSecs(-10));
    writeLog(invalid, QByteArrayLiteral("{incomplete"), now);
    QFile persistedNewest(newest);
    require(persistedNewest.open(QIODevice::ReadOnly), "could not reopen the long session log");
    require(mdv::codexSessionMatchesCwd(
                persistedNewest.readAll(), QStringLiteral("/home/fuk/project")),
        "persisted long session metadata was not parsed");
    const QString selected = mdv::latestCodexSession(sessions.path());
    require(selected == newest, "the newest valid Codex session was not selected");

    QTemporaryDir emptySessions;
    require(emptySessions.isValid(), "could not create an empty sessions directory");
    require(mdv::latestCodexSession(emptySessions.path()).isEmpty(),
        "an empty sessions directory returned a session");

    const QString empty = mdv::renderCodexEventLog(
        QStringLiteral("{broken"), QStringLiteral("Conversation"), QStringLiteral("User"),
        QStringLiteral("Assistant"), QStringLiteral("No messages"));
    require(empty.contains(QLatin1String("> No messages")),
        "empty-log marker is missing");

    return EXIT_SUCCESS;
}
