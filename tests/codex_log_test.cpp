#include "codex_log.h"

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

    const QString empty = mdv::renderCodexEventLog(
        QStringLiteral("{broken"), QStringLiteral("Conversation"), QStringLiteral("User"),
        QStringLiteral("Assistant"), QStringLiteral("No messages"));
    require(empty.contains(QLatin1String("> No messages")),
        "empty-log marker is missing");

    return EXIT_SUCCESS;
}
