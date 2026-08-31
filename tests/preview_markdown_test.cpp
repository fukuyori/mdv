#include "preview_markdown.h"

#include <iostream>

namespace {

int failures = 0;

void expectEqual(const QString &actual, const QString &expected, const char *name)
{
    if (actual == expected) {
        return;
    }
    std::cerr << name << " failed\nexpected:\n"
              << expected.toStdString() << "\nactual:\n"
              << actual.toStdString() << '\n';
    ++failures;
}

} // namespace

int main()
{
    const QString source = QStringLiteral(
        "# Title\n\n"
        "First paragraph.\n\n"
        "```cpp\nint main() {}\n```\n\n"
        "Second paragraph.");
    const QHash<QString, QString> translations{
        {QStringLiteral("# Title"), QStringLiteral("# 題名")},
        {QStringLiteral("First paragraph."), QStringLiteral("最初の段落。")},
        {QStringLiteral("Second paragraph."), QString()},
    };

    expectEqual(
        preview_markdown::composeExport(source, preview_markdown::Mode::Original,
            translations, QStringLiteral("(pending)"), QStringLiteral("(failed)")),
        source,
        "original");

    expectEqual(
        preview_markdown::composeExport(source, preview_markdown::Mode::Bilingual,
            translations, QStringLiteral("(pending)"), QStringLiteral("(failed)")),
        QStringLiteral(
            "# Title\n\n"
            "# 題名\n\n"
            "First paragraph.\n\n"
            "最初の段落。\n\n"
            "```cpp\nint main() {}\n```\n\n"
            "Second paragraph.\n\n"
            "(failed)"),
        "bilingual");

    expectEqual(
        preview_markdown::composeExport(source, preview_markdown::Mode::Translated,
            translations, QStringLiteral("(pending)"), QStringLiteral("(failed)")),
        QStringLiteral(
            "# 題名\n\n"
            "最初の段落。\n\n"
            "```cpp\nint main() {}\n```\n\n"
            "Second paragraph."),
        "translated");

    const QHash<QString, QString> incompleteTranslations{
        {QStringLiteral("# Title"), QStringLiteral("# 題名")},
    };
    expectEqual(
        preview_markdown::composeExport(source, preview_markdown::Mode::Bilingual,
            incompleteTranslations, QStringLiteral("(pending)"), QStringLiteral("(failed)")),
        QStringLiteral(
            "# Title\n\n"
            "# 題名\n\n"
            "First paragraph.\n\n"
            "(pending)\n\n"
            "```cpp\nint main() {}\n```\n\n"
            "Second paragraph.\n\n"
            "(pending)"),
        "pending bilingual");

    if (failures != 0) {
        return 1;
    }
    std::cout << "preview Markdown export tests passed\n";
    return 0;
}
