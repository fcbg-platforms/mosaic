#pragma once
#include <QString>

namespace mosaic {

/// @brief Encrypts @p plaintext for storage in a JSON settings file, using
/// Windows DPAPI (CryptProtectData, current-user scope) on Windows builds.
///
/// Returns a `"dpapi:v1:<base64>"` marker string on success. Returns
/// @p plaintext unchanged (with a logged warning) if DPAPI itself fails, and
/// on non-Windows builds (a plain pass-through, since DPAPI is Windows-only)
/// — never silently drops the value, just leaves it unprotected rather than
/// lose it. An empty string returns an empty string (nothing to protect).
///
/// @see dpapi_decrypt
[[nodiscard]] QString dpapi_encrypt(const QString& plaintext);

/// @brief Reverses dpapi_encrypt().
///
/// A string that doesn't start with the `"dpapi:v1:"` marker is assumed to
/// be a pre-existing plaintext value from before this feature existed (or a
/// non-Windows build) and is returned unchanged — no migration step is
/// needed, since the very next save re-encrypts it via dpapi_encrypt().
///
/// If the marker IS present but decryption fails (e.g. the settings file
/// was copied to a different machine or user account, or the blob is
/// corrupted), returns an empty string with a logged warning — surfacing
/// the problem by making the field visibly empty rather than returning
/// silently wrong bytes.
[[nodiscard]] QString dpapi_decrypt(const QString& stored);

} // namespace mosaic
