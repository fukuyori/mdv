#pragma once

#include <QString>

namespace preview_scroll {

inline QString script()
{
    return QStringLiteral(
        "function __mdvCaptureScrollAnchor(container) {"
        "  var maxY = Math.max(1, document.documentElement.scrollHeight - window.innerHeight);"
        "  var anchor = {index: -1, offset: 0, fraction: window.scrollY / maxY};"
        "  var children = container.children;"
        "  for (var i = 0; i < children.length; i++) {"
        "    var rect = children[i].getBoundingClientRect();"
        "    var top = rect.top + window.scrollY;"
        "    if (top + rect.height <= window.scrollY + 1) continue;"
        "    anchor.index = i;"
        "    anchor.offset = window.scrollY - top;"
        "    break;"
        "  }"
        "  return anchor;"
        "}"
        "function __mdvRestoreScrollAnchor(container, anchor, generation) {"
        "  if (!anchor || generation !== __mdvRenderGeneration) return;"
        "  var maxY = Math.max(0, document.documentElement.scrollHeight - window.innerHeight);"
        "  var y = anchor.fraction * maxY;"
        "  var children = container.children;"
        "  if (anchor.index >= 0 && children.length) {"
        "    var index = Math.min(anchor.index, children.length - 1);"
        "    y = children[index].getBoundingClientRect().top + window.scrollY + anchor.offset;"
        "  }"
        "  __mdvProgTs = Date.now();"
        "  window.scrollTo(0, Math.max(0, Math.min(y, maxY)));"
        "}");
}

} // namespace preview_scroll
