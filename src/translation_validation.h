#pragma once

#include <QString>

namespace translation_validation {

// Returns an empty string when the response is safe to display as a
// translation. Otherwise returns a diagnostic describing why it was rejected.
QString rejectionReason(const QString &source, const QString &response);

} // namespace translation_validation
