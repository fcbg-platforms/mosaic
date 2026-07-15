
.. _program_listing_file_src_auth_profile_manager.hpp:

Program Listing for File profile_manager.hpp
============================================

|exhale_lsh| :ref:`Return to documentation for file <file_src_auth_profile_manager.hpp>` (``src/auth/profile_manager.hpp``)

.. |exhale_lsh| unicode:: U+021B0 .. UPWARDS ARROW WITH TIP LEFTWARDS

.. code-block:: cpp

   #pragma once
   #include "auth/profile.hpp"
   #include <QObject>
   #include <memory>
   #include <vector>
   
   namespace mosaic {
   
   /// @brief Manages the on-disk profile manifest and per-profile settings directories.
   ///
   /// @par Storage layout
   /// @code
   /// ~/.config/CSRU/mosaic/
   ///   profiles.json          ← auth manifest (hashed credentials)
   ///   profiles/
   ///     <username>/
   ///       settings.json      ← per-group AppSettings
   ///       mosaic.log
   /// @endcode
   ///
   /// @par Password security
   /// PBKDF2-HMAC-SHA256, 100 000 iterations, 32-byte output key, 32-byte random
   /// salt per profile.  This protects against accidental profile collisions on a
   /// shared workstation; it is **not** designed to protect against an attacker
   /// with file-system access.
   ///
   /// @par Example
   /// @code{.cpp}
   /// ProfileManager mgr;
   /// mgr.load();
   ///
   /// using Res = ProfileManager::RegisterResult;
   /// if (mgr.register_profile("cognitive_lab", "Cognitive Science Lab", "secret")
   ///         == Res::Ok) {
   ///     qDebug() << "profile created at" << ProfileManager::profile_dir("cognitive_lab");
   /// }
   ///
   /// if (mgr.verify("cognitive_lab", "secret")) {
   ///     mgr.touch("cognitive_lab");
   ///     auto path = ProfileManager::settings_path("cognitive_lab");
   /// }
   /// @endcode
   ///
   /// @see Profile, AppSettings
   class ProfileManager : public QObject {
       Q_OBJECT
   public:
       explicit ProfileManager(QObject* parent = nullptr);
       ~ProfileManager() override;
   
       /// @brief Loads the profiles manifest from disk.
       ///
       /// Creates an empty manifest if the file does not exist yet.
       /// Safe to call multiple times — each call replaces the in-memory list.
       void load();
   
       // ── Queries ────────────────────────────────────────────────────────────
   
       /// @returns A copy of all currently loaded profiles.
       [[nodiscard]] std::vector<Profile> profiles() const;
   
       /// @param username  Username to look up (case-sensitive).
       /// @returns         @c true if a profile with this username exists.
       [[nodiscard]] bool has_profile(const QString& username) const;
   
       /// @param username  Username to look up.
       /// @returns         Pointer to the matching Profile, or @c nullptr.
       ///
       /// @note The returned pointer is invalidated the next time the profile
       ///       list is modified (register, delete).  Copy if you need to keep it.
       [[nodiscard]] const Profile* find(const QString& username) const;
   
       // ── Registration ───────────────────────────────────────────────────────
   
       /// @brief Possible outcomes of register_profile().
       enum class RegisterResult {
           Ok,               ///< Profile was created successfully.
           UsernameTaken,    ///< A profile with that username already exists.
           UsernameInvalid,  ///< Username does not match ``[a-z0-9_]{3,32}``.
           PasswordTooShort, ///< Non-empty password is shorter than 4 characters.
       };
   
       /// @brief Registers a new research-group profile.
       ///
       /// Creates the profile directory, hashes the password (if non-empty),
       /// appends the profile to the manifest, and saves it to disk.
       ///
       /// @param username     Unique login key — ``[a-z0-9_]{3,32}``.
       /// @param displayName  Human-readable group name (any Unicode string).
       /// @param password     Plain-text password.  Pass an empty string for
       ///                     a password-free profile.
       /// @returns            RegisterResult::Ok on success, or an error code.
       RegisterResult register_profile(const QString& username,
                                        const QString& displayName,
                                        const QString& password);
   
       // ── Authentication ─────────────────────────────────────────────────────
   
       /// @brief Verifies a password against the stored hash.
       ///
       /// @param username  Profile to authenticate.
       /// @param password  Plain-text password entered by the user.
       /// @returns         @c true if the password matches, or if the profile
       ///                  has no password set.  @c false if the profile does
       ///                  not exist or the password is wrong.
       [[nodiscard]] bool verify(const QString& username, const QString& password) const;
   
       /// @brief Updates the @c lastLogin timestamp and saves the manifest.
       ///
       /// Call this immediately after a successful login so the login dialog
       /// can display accurate "last seen" information.
       ///
       /// @param username  The profile that just logged in.
       void touch(const QString& username);
   
       // ── Profile paths ──────────────────────────────────────────────────────
   
       /// @returns The root config directory: ``~/.config/CSRU/mosaic``.
       [[nodiscard]] static QString root_dir();
   
       /// @param username  Profile username.
       /// @returns         The profile-specific directory:
       ///                  ``~/.config/CSRU/mosaic/profiles/<username>``.
       [[nodiscard]] static QString profile_dir(const QString& username);
   
       /// @param username  Profile username.
       /// @returns         Full path to the profile's settings file:
       ///                  ``<profile_dir>/settings.json``.
       [[nodiscard]] static QString settings_path(const QString& username);
   
       /// @param username  Profile username.
       /// @returns         Full path to the profile's log file:
       ///                  ``<profile_dir>/mosaic.log``.
       [[nodiscard]] static QString log_path(const QString& username);
   
       // ── Mutation ───────────────────────────────────────────────────────────
   
       /// @brief Permanently deletes a profile from the manifest.
       ///
       /// @note This does **not** delete the profile directory or its settings.
       ///       The data remains on disk so it can be manually recovered.
       ///
       /// @param username  Profile to delete.
       /// @returns         @c true if the profile was found and removed.
       bool delete_profile(const QString& username);
   
       /// @brief Renames a profile's display name without changing its username.
       ///
       /// @param username    Profile to update.
       /// @param newDisplay  New human-readable name.
       /// @returns           @c true if the profile was found and updated.
       bool rename_display(const QString& username, const QString& newDisplay);
   
       /// @brief Returns the hex accent colour for a given position in the palette.
       ///
       /// Colours are assigned round-robin so consecutive profiles get distinct
       /// colours.
       ///
       /// @param profileIndex  Zero-based index (wraps around the 8-colour palette).
       /// @returns             A hex RGB string like ``"#5566dd"``.
       [[nodiscard]] static QString next_accent_colour(int profileIndex);
   
   signals:
       /// Emitted after any change to the profile list (register, delete, rename).
       void profiles_changed();
   
   private:
       [[nodiscard]] bool save() const;
       [[nodiscard]] static QString hash_password(const QString& password,
                                                   const QByteArray& salt);
       [[nodiscard]] static QString generate_initials(const QString& displayName);
   
       struct Impl;
       std::unique_ptr<Impl> d;
   };
   
   } // namespace mosaic
