
.. _program_listing_file_src_core_recording_access_control.hpp:

Program Listing for File recording_access_control.hpp
=====================================================

|exhale_lsh| :ref:`Return to documentation for file <file_src_core_recording_access_control.hpp>` (``src\core\recording_access_control.hpp``)

.. |exhale_lsh| unicode:: U+021B0 .. UPWARDS ARROW WITH TIP LEFTWARDS

.. code-block:: cpp

   #pragma once
   #include <QSet>
   #include <QString>
   
   namespace mosaic {
   
   // Pure helpers backing per-user recording access control (item 27) — pulled
   // out of Application::initialize()'s anonymous namespace into their own
   // module specifically so this logic is directly unit-testable without a
   // real filesystem/ProfileManager, matching this project's established
   // pattern for isolating pure decision logic (e.g. rms_quality.hpp,
   // speaker_palette.hpp). The actual I/O (AppSettings::load(), QDir::rename())
   // stays in application.cpp; only the pure "what should the answer be, given
   // these plain inputs" pieces live here.
   
   // The legacy, pre-item-27 default every profile used to share. Named (not
   // just a literal) so every place that checks "was this ever customized
   // away from the default" can never silently drift from the seeding
   // convention below.
   [[nodiscard]] QString legacy_shared_record_directory();
   
   // Session folders whose recorded_by doesn't match any known profile (or is
   // empty) land here during the one-time flat-folder migration — kept
   // distinct from any one profile's own directory so nothing found during
   // migration silently becomes invisible to every admin.
   [[nodiscard]] QString unassigned_record_directory();
   
   // The per-user recording directory convention every profile's own
   // record.directory is seeded/migrated to, and that an admin's aggregate
   // directory resolution falls back to for a profile with no settings.json
   // yet. Kept as one function so every call site can never disagree.
   [[nodiscard]] QString default_record_directory_for(const QString& username);
   
   // True only for an exact, unmodified match against
   // legacy_shared_record_directory() — the migration/seeding logic in
   // Application::initialize() must only touch a profile's record.directory
   // when it's genuinely still the untouched shared default, never a
   // directory a user (or admin) has already customized to something else.
   [[nodiscard]] bool is_legacy_shared_record_directory(const QString& directory);
   
   // Where a migrated flat-folder session should land: that user's own
   // per-user directory if recordedBy names a known profile, else the shared
   // "_unassigned" fallback (covers both an empty recordedBy and one that
   // doesn't match any currently-known profile).
   [[nodiscard]] QString resolve_migration_target(const QString& recordedBy,
                                                   const QSet<QString>& knownUsernames);
   
   } // namespace mosaic
