#pragma once
#include <QDateTime>
#include <QString>

namespace mosaic {

/// @brief A single research-group profile stored in the profiles manifest.
///
/// Passwords are **never** stored in plain text.  Only the PBKDF2-SHA256
/// hash and the random salt used to compute it are persisted.
///
/// Profiles are loaded from and saved to
/// ``~/.config/CSRU/mosaic/profiles.json`` by ProfileManager.
///
/// @see ProfileManager
struct Profile {
    /// @brief Privilege level for this profile.
    enum class Role : uint8_t {
        User  = 0, ///< Regular research-group profile.
        Admin = 1, ///< Full administrator — can manage all profiles and global settings.
    };

    /// Unique login key — lowercase alphanumeric + underscore, 3–32 chars.
    QString username;

    /// Human-readable group name shown in the login dialog and session metadata.
    /// Example: ``"Cognitive Science Lab"``.
    QString displayName;

    /// One or two uppercase letters derived from displayName, shown inside
    /// the avatar circle.  Example: ``"CL"`` for *Cognitive Lab*.
    QString initials;

    /// Hex RGB colour for the avatar background.
    /// Assigned round-robin from a fixed palette on registration.
    /// Example: ``"#5566dd"``.
    QString accentColour;

    /// Hex-encoded random 32-byte salt used in the password hash.
    /// Empty string means no password was set.
    QString salt;

    /// Hex-encoded PBKDF2-HMAC-SHA256 output (100 000 iterations, 32-byte key).
    /// Empty string means no password was set — any login attempt succeeds.
    QString passwordHash;

    /// UTC timestamp of the most recent successful login.
    QDateTime lastLogin;

    /// UTC timestamp when this profile was first created.
    QDateTime created;

    /// Privilege level — only Admin profiles open the admin panel after login.
    Role role = Role::User;

    /// Optional lab / university / department name (shown in admin panel and status bar).
    QString institution;

    /// @returns @c true if this is the built-in unauthenticated guest session.
    [[nodiscard]] bool is_guest() const noexcept { return username == "guest"; }

    /// @returns @c true if a password hash is stored for this profile.
    ///          Profiles without a password always pass authentication.
    [[nodiscard]] bool has_password() const noexcept { return !passwordHash.isEmpty(); }

    /// @returns @c true if this profile has administrator privileges.
    [[nodiscard]] bool is_admin() const noexcept { return role == Role::Admin; }
};

} // namespace mosaic
