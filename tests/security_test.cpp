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

    const std::string remoteImageMarkdown =
        "![](https://substackcdn.com/image/fetch/$s_!XYEq!,w_1456,c_limit,f_webp,q_auto:good,"
        "fl_progressive:steep/https%3A%2F%2Fsubstack-post-media.s3.amazonaws.com%2Fpublic%2F"
        "images%2F9f11eeb1-99f1-4d2b-881f-61da8ced7b9f_1314x912.png)";
    const std::string remoteImageHtml = render(remoteImageMarkdown);
    require(remoteImageHtml.find("<img src=\"https://substackcdn.com/image/fetch/") != std::string::npos,
        "HTTPS Markdown image was not rendered as an image element");

    std::string repeatedReferences = "[x]: /destination \"title\"\n\n";
    for (int i = 0; i < 20000; ++i) {
        repeatedReferences += "[x] ";
    }
    const std::string bounded = render(repeatedReferences);
    require(bounded.size() <= repeatedReferences.size() * 17,
        "link-reference expansion exceeded its output bound");

    return EXIT_SUCCESS;
}
