#include "translation_validation.h"

#include <iostream>

namespace {

int failures = 0;

void expectAccepted(const QString &source, const QString &response, const char *name)
{
    const QString reason = translation_validation::rejectionReason(source, response);
    if (reason.isEmpty()) {
        return;
    }
    std::cerr << name << " unexpectedly rejected: " << reason.toStdString() << '\n';
    ++failures;
}

void expectRejected(const QString &source, const QString &response, const char *name)
{
    if (!translation_validation::rejectionReason(source, response).isEmpty()) {
        return;
    }
    std::cerr << name << " unexpectedly accepted\n";
    ++failures;
}

} // namespace

int main()
{
    expectAccepted(
        QStringLiteral("# Agent Development Guide"),
        QStringLiteral("# エージェント開発ガイド"),
        "translated heading");
    expectAccepted(
        QStringLiteral("- Build the project.\n- Run the tests."),
        QStringLiteral("- プロジェクトをビルドします。\n- テストを実行します。"),
        "translated list");
    expectAccepted(
        QStringLiteral("A file for guiding coding agents."),
        QStringLiteral("コーディングエージェントを導くためのファイルです。"),
        "translated paragraph");

    expectRejected(
        QStringLiteral("# Agent Development Guide"),
        QStringLiteral(
            "# エージェント開発ガイド\n\n"
            "This guide outlines the process of developing agents.\n\n"
            "## Prerequisites\n\n- Python 3.7+\n- pip\n\n"
            "## Installation\n\n```sh\npip install agent-framework\n```"),
        "hallucinated document");
    expectRejected(
        QStringLiteral("Translate this sentence."),
        QString(400, QLatin1Char('x')),
        "unexpected expansion");
    expectRejected(
        QStringLiteral("- One item"),
        QStringLiteral("- 項目1\n- 項目2"),
        "added list item");
    expectRejected(
        QStringLiteral("# Heading"),
        QStringLiteral("## 見出し"),
        "changed heading level");

    if (failures != 0) {
        return 1;
    }
    std::cout << "translation validation tests passed\n";
    return 0;
}
