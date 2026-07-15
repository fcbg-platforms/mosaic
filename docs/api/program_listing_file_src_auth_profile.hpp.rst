
.. _program_listing_file_src_auth_profile.hpp:

Program Listing for File profile.hpp
====================================

|exhale_lsh| :ref:`Return to documentation for file <file_src_auth_profile.hpp>` (``src/auth/profile.hpp``)

.. |exhale_lsh| unicode:: U+021B0 .. UPWARDS ARROW WITH TIP LEFTWARDS

.. code-block:: cpp

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
       /// Unique login key — lowercase alphanumeric + underscore, 3–32 chars.
       QString  username;
   
       /// Human-readable group name shown in the login dialog and session metadata.
       /// Example: ``"Cognitive Science Lab"``.
       QString  displayName;
   
       /// One or two uppercase letters derived from displayName, shown inside
       /// the avatar circle.  Example: ``"CL"`` for *Cognitive Lab*.
       QString  initials;
   
       /// Hex RGB colour for the avatar background.
       /// Assigned round-robin from a fixed palette on registration.
       /// Example: ``"#5566dd"``.
       QString  accentColour;
   
       /// Hex-encoded random 32-byte salt used in the password hash.
       /// Empty string means no password was set.
       QString  salt;
   
       /// Hex-encoded PBKDF2-HMAC-SHA256 output (100 000 iterations, 32-byte key).
       /// Empty string means no password was set — any login attempt succeeds.
       QString  passwordHash;
   
       /// UTC timestamp of the most recent successful login.
       QDateTime lastLogin;
   
       /// UTC timestamp when this profile was first created.
       QDateTime created;
   
       /// @returns @c true if this is the built-in unauthenticated guest session.
       [[nodiscard]] bool is_guest()     const noexcept { return username == "guest"; }
   
       /// @returns @c true if a password hash is stored for this profile.
       ///          Profiles without a password always pass authentication.
       [[nodiscard]] bool has_password() const noexcept { return !passwordHash.isEmpty(); }
   };
   
   } // namespace mosaic
