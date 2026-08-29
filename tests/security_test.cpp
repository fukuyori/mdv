#include "md4c-html.h"

#include <cstdlib>
#include <iostream>
#include <string>

namespace {

std::string render(const std::string &markdown)
{
    std::string html;
    const int result = md_html(
        markdown.data(), MD_SIZE(markdown.size()),
        [](const MD_CHAR *text, MD_SIZE size, void *userdata) {
            static_cast<std::string *>(userdata)->append(text, size);
        },
        &html,
        MD_DIALECT_GITHUB | MD_FLAG_LATEXMATHSPANS | MD_FLAG_NOHTML,
        0);
    if (result != 0) {
        std::cerr << "md_html failed: " << result << '\n';
        std::exit(EXIT_FAILURE);
    }
    return html;
}

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
    const std::string rawHtml =
        "<img src=x onerror=\"document.body.dataset.pwned='yes'\">";
    const std::string escaped = render(rawHtml);
    require(escaped.find("<img") == std::string::npos,
        "raw HTML became an active image element");
    require(escaped.find("&lt;img") != std::string::npos,
        "raw HTML was not rendered as escaped text");

    std::string repeatedReferences = "[x]: /destination \"title\"\n\n";
    for (int i = 0; i < 20000; ++i) {
        repeatedReferences += "[x] ";
    }
    const std::string bounded = render(repeatedReferences);
    require(bounded.size() <= repeatedReferences.size() * 17,
        "link-reference expansion exceeded its output bound");

    return EXIT_SUCCESS;
}
