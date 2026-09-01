#include "claude_log.h"

#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QJsonDocument>
#include <QJsonObject>
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

void writeSession(const QString &path, const QString &cwd, const QDateTime &modified)
{
    QJsonObject entry;
    entry.insert(QStringLiteral("type"), QStringLiteral("user"));
    entry.insert(QStringLiteral("cwd"), cwd);
    entry.insert(QStringLiteral("sessionId"), QStringLiteral("session-id"));
    QJsonObject message;
    message.insert(QStringLiteral("role"), QStringLiteral("user"));
    message.insert(QStringLiteral("content"), QStringLiteral("Hello"));
    entry.insert(QStringLiteral("message"), message);

    QFile file(path);
    require(file.open(QIODevice::WriteOnly), "could not create a Claude session log");
    const QByteArray data = QJsonDocument(entry).toJson(QJsonDocument::Compact) + '\n';
    require(file.write(data) == data.size(), "could not write a Claude session log");
    require(file.flush(), "could not flush a Claude session log");
    require(file.setFileTime(modified, QFileDevice::FileModificationTime),
        "could not set a Claude session timestamp");
}

} // namespace

int main()
{
    const QByteArray preamble =
        R"json({"type":"mode","mode":"normal","sessionId":"11111111-2222-3333-4444-555555555555"})json";
    require(mdv::isClaudeSessionLog("session.jsonl", preamble),
        "Claude session preamble was not detected");

    const QByteArray excerpt =
        R"json({"type":"user","message":{"role":"user","content":"Hello"},"uuid":"u-1","timestamp":"2026-08-29T01:00:00.000Z"})json";
    require(mdv::isClaudeSessionLog("session.jsonl", excerpt),
        "Claude conversation entry was not detected");
    require(!mdv::isClaudeSessionLog("events.jsonl", R"json({"type":"ordinary"})json"),
        "ordinary JSONL was misidentified as a Claude session");
    require(!mdv::isClaudeSessionLog("session.md", excerpt),
        "a non-JSONL file was misidentified as a Claude session");
    const QByteArray codexLine =
        R"json({"timestamp":"2026-08-29T00:00:00.000Z","type":"session_meta","payload":{"id":"session-id","cwd":"/tmp"}})json";
    require(!mdv::isClaudeSessionLog("rollout-example.jsonl", codexLine),
        "a Codex log was misidentified as a Claude session");

    const QString jsonl = QString::fromUtf8(
        R"json({"type":"user","message":{"role":"user","content":"First line\n\n- item"},"uuid":"u-1","sessionId":"s","timestamp":"2026-08-29T01:00:00.000Z"})json"
        "\n"
        R"json({"type":"user","isMeta":true,"message":{"role":"user","content":"meta caveat"},"uuid":"u-2","sessionId":"s","timestamp":"2026-08-29T01:00:00.500Z"})json"
        "\n"
        R"json({"type":"assistant","message":{"role":"assistant","content":[{"type":"thinking","thinking":"secret thinking"},{"type":"text","text":"Looking into it."}]},"uuid":"a-1","sessionId":"s","timestamp":"2026-08-29T01:00:01.000Z"})json"
        "\n"
        R"json({"type":"assistant","message":{"role":"assistant","content":[{"type":"tool_use","id":"t1","name":"Bash","input":{"command":"secret command"}}]},"uuid":"a-2","sessionId":"s","timestamp":"2026-08-29T01:00:02.000Z"})json"
        "\n"
        R"json({"type":"user","message":{"role":"user","content":[{"type":"tool_result","tool_use_id":"t1","content":"secret tool output"}]},"uuid":"u-3","sessionId":"s","timestamp":"2026-08-29T01:00:03.000Z"})json"
        "\n"
        R"json({"type":"assistant","message":{"role":"assistant","content":[{"type":"text","text":"Answer"}]},"uuid":"a-3","sessionId":"s","timestamp":"2026-08-29T01:00:04.000Z"})json"
        "\n"
        R"json({"type":"user","isSidechain":true,"message":{"role":"user","content":"sidechain prompt"},"uuid":"u-4","sessionId":"s","timestamp":"2026-08-29T01:00:05.000Z"})json"
        "\n"
        R"json({"type":"user","message":{"role":"user","content":"<command-name>/clear</command-name>"},"uuid":"u-5","sessionId":"s","timestamp":"2026-08-29T01:00:06.000Z"})json"
        "\n{incomplete");

    const QString markdown = mdv::renderClaudeSessionLog(
        jsonl, QStringLiteral("Conversation"), QStringLiteral("User"),
        QStringLiteral("Assistant"), QStringLiteral("No messages"));
    require(markdown.contains(QLatin1String("# Conversation")),
        "conversation title is missing");
    require(markdown.contains(QStringLiteral("## User — `2026-08-29T01:00:00.000Z`")),
        "user heading or timestamp is missing");
    require(markdown.contains(QLatin1String("> First line\n>\n> - item")),
        "multiline user Markdown was not quoted correctly");
    require(markdown.contains(QStringLiteral("## Assistant — `2026-08-29T01:00:01.000Z`")),
        "assistant heading or timestamp is missing");
    require(markdown.contains(QLatin1String("> Looking into it.")),
        "assistant text is missing");
    require(markdown.contains(QLatin1String("> Answer")),
        "assistant answer is missing");
    require(!markdown.contains(QLatin1String("meta caveat")),
        "meta message was rendered");
    require(!markdown.contains(QLatin1String("secret thinking")),
        "thinking block was rendered");
    require(!markdown.contains(QLatin1String("secret command")),
        "tool input was rendered");
    require(!markdown.contains(QLatin1String("secret tool output")),
        "tool output was rendered");
    require(!markdown.contains(QLatin1String("sidechain prompt")),
        "sidechain message was rendered");
    require(!markdown.contains(QLatin1String("/clear")),
        "command wrapper was rendered");

    require(mdv::claudeProjectDirName(QStringLiteral("/home/fuk/source/cpp/mdv"))
            == QLatin1String("-home-fuk-source-cpp-mdv"),
        "project directory name was not encoded correctly");
    require(mdv::claudeProjectDirName(QStringLiteral("/tmp/my_app.v2"))
            == QLatin1String("-tmp-my-app-v2"),
        "underscore and dot were not encoded as dashes");

    QTemporaryDir projectsRoot;
    QTemporaryDir workspaceRoot;
    require(projectsRoot.isValid() && workspaceRoot.isValid(),
        "could not create temporary Claude directories");
    QDir workspace(workspaceRoot.path());
    require(workspace.mkpath(QStringLiteral("project"))
            && workspace.mkpath(QStringLiteral("other")),
        "could not create temporary Claude workspaces");
    const QString cwd = workspace.filePath(QStringLiteral("project"));
    const QString foreignCwd = workspace.filePath(QStringLiteral("other"));
    const QString projectDir = QDir(projectsRoot.path()).filePath(
        mdv::claudeProjectDirName(cwd));
    require(QDir().mkpath(projectDir), "could not create a Claude project directory");
    const QString matching = QDir(projectDir).filePath(QStringLiteral("matching.jsonl"));
    const QString foreign = QDir(projectDir).filePath(QStringLiteral("foreign.jsonl"));
    const QDateTime now = QDateTime::currentDateTimeUtc();
    writeSession(matching, cwd, now.addSecs(-10));
    writeSession(foreign, foreignCwd, now.addSecs(-5));
    require(mdv::latestClaudeSessionForCwd(cwd, projectsRoot.path()) == matching,
        "a newer Claude session from another working directory was selected");

    QFile matchingFile(matching);
    require(matchingFile.open(QIODevice::ReadOnly),
        "could not reopen the matching Claude session");
    require(mdv::claudeSessionMatchesCwd(matchingFile.readAll(), cwd),
        "the matching Claude session cwd was not detected");

    const QString empty = mdv::renderClaudeSessionLog(
        QStringLiteral("{broken"), QStringLiteral("Conversation"), QStringLiteral("User"),
        QStringLiteral("Assistant"), QStringLiteral("No messages"));
    require(empty.contains(QLatin1String("> No messages")),
        "empty-log marker is missing");

    return EXIT_SUCCESS;
}
