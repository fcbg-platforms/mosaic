#pragma once
#include <QString>

namespace mosaic {

// ── Settings shared by every profile on this PC ────────────────────────────
//
// Most settings belong to a profile. The Hugging Face token (speaker
// diarization) is one per PC: entered once, every profile uses it, and a
// profile can still keep its own. It lives in pc_settings.json beside
// profiles.json, DPAPI-encrypted like the profile copy, so it decrypts only
// under the Windows account that saved it (every MOSAIC profile on a PC runs
// under the same account).

/// pc_settings.json inside `rootDir` (ProfileManager::root_dir()).
[[nodiscard]] QString pc_settings_path(const QString& rootDir);

/// The PC's Hugging Face token; empty when none is saved or it cannot be
/// decrypted here.
[[nodiscard]] QString load_pc_hf_token(const QString& path);

/// Saves the PC's Hugging Face token (empty removes it), keeping any other
/// keys in the file. Returns false when the file cannot be written.
bool save_pc_hf_token(const QString& path, const QString& token);

/// A profile's token and the PC's, after moving the profile's up to the PC.
struct HfTokens {
    QString pc;
    QString profile; ///< non-empty only when this profile keeps its own
    bool pcChanged = false;
};

/// Profiles used to keep the token to themselves. The first profile to log
/// in with one gives it to the PC; a profile whose token matches the PC's
/// drops its copy; a profile with a different one keeps it as its own.
[[nodiscard]] HfTokens adopt_profile_hf_token(const QString& pcToken, const QString& profileToken);

} // namespace mosaic
