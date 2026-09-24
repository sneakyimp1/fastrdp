#pragma once

#include <QString>

namespace fastrdp::secrets {

// Thin synchronous wrappers over QtKeychain (Secret Service / KWallet).
// They spin a local event loop, so call them only from the GUI thread.

bool available();
// Returns true and fills value if an entry exists.
bool read(const QString& key, QString& value, QString* error = nullptr);
bool write(const QString& key, const QString& value, QString* error = nullptr);
void remove(const QString& key);

} // namespace fastrdp::secrets
