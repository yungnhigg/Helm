#pragma once
// Windows DPAPI wrappers for secrets at rest. Encryption is bound to this
// Windows user account: the blob decrypts only for the same user on the same
// machine, which is exactly the scope an API key in a per-user config file
// should have. No key management, no external dependencies.
#include <string>

namespace lar {

// Plaintext -> base64(DPAPI blob). Empty on failure or empty input.
std::string dpapi_protect_to_base64(const std::string& plain);

// base64(DPAPI blob) -> plaintext. Empty on failure (wrong user, corrupt
// blob, or empty input) - callers treat that as "not configured".
std::string dpapi_unprotect_from_base64(const std::string& base64);

} // namespace lar
