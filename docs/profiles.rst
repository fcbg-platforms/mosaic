Research-group profiles
=======================

.. contents:: On this page
   :local:
   :depth: 2

Overview
--------

MOSAIC's profile system is a **local settings switcher**, not a network
authentication service.  Its purpose is to let multiple research groups share
one workstation (or one installation) without overwriting each other's camera
configurations, recording directories, or calibration data.

Each profile gets:

- An isolated ``settings.json`` (cameras, audio, triggers, recording paths).
- A separate ``mosaic.log``.
- A distinct colour and initials avatar in the login dialog.
- Its username embedded in every ``session_meta.json`` as ``recorded_by``.

.. note::

   The password protects against *accidental* profile switching, not
   adversarial access.  Do not rely on MOSAIC profiles to protect sensitive
   participant data — apply OS-level access controls for that.

Storage layout
--------------

.. code-block:: text

   ~/.config/CSRU/mosaic/
     profiles.json                       ← auth manifest
     profiles/
       cognitive_lab/
         settings.json
         mosaic.log
       social_neuro/
         settings.json
         mosaic.log

``profiles.json`` format:

.. code-block:: json

   [
     {
       "username":     "cognitive_lab",
       "display_name": "Cognitive Science Lab",
       "initials":     "CL",
       "accent":       "#5566dd",
       "salt":         "a3f2...(64 hex chars)...",
       "password_hash":"b8c1...(64 hex chars)...",
       "last_login":   "2026-06-04T14:32:05Z",
       "created":      "2026-01-15T09:00:00Z"
     }
   ]

Passwords are stored as **PBKDF2-HMAC-SHA256** with a 32-byte random salt
and 100 000 iterations.  No plain-text password is ever written to disk.

Recording access control (admin vs. per-user)
--------------------------------------------------

Beyond isolated *settings*, each profile also gets its own isolated
**recordings** folder: ``recordings/<username>/`` (resolved relative to
wherever the app is running from), seeded automatically the first time a
brand-new profile logs in. This is a real filesystem boundary, not just a
display filter — see :doc:`recording`'s session-layout section for the
full folder tree.

.. list-table::
   :widths: 25 75
   :header-rows: 1

   * - Role
     - What it sees / can do
   * - **Regular user**
       (``Role::User``)
     - Session Browser and Analysis tab show only that profile's own
       ``recordings/<username>/`` sessions. The Record Settings
       recording-directory field is **read-only** — shown, but not
       editable — precisely so it can't be pointed at another profile's
       folder.
   * - **Admin**
       (``Role::Admin``)
     - Session Browser and Analysis tab show an **aggregated** view across
       *every* known profile's ``recordings/<username>/`` folder, plus a
       shared ``recordings/_unassigned/`` bucket, each session labeled with
       an ``@username`` badge. The recording-directory field stays fully
       editable, same as before this feature existed.

.. note::

   **One-time migration.** The first time an **admin** profile logs in
   after this feature was added, MOSAIC scans the legacy flat
   ``recordings/`` root (the shared location every profile used before
   per-user folders existed) for loose session folders and moves each one
   into ``recordings/<recorded_by>/`` — matched by that session's own
   ``recorded_by`` field in ``session_meta.json`` — or into
   ``recordings/_unassigned/`` if ``recorded_by`` is empty or doesn't
   match any known profile. This runs silently at startup and is
   naturally idempotent (nothing is left loose in the flat root after the
   first successful run for a later run to find). Sessions recorded
   before this feature shipped are moved forward, never deleted.

Creating a profile
------------------

1. Launch MOSAIC.
2. In the login dialog, click the **＋ New profile** chip.
3. Enter:

   - **Username** — 3–32 characters, lowercase ``[a-z0-9_]``.
   - **Group name** — displayed in the avatar chip and in ``session_meta.json``.
   - **Password** (optional) — leave blank for an open profile.

4. Click **Create profile**.  MOSAIC logs you in immediately and creates the
   profile directory.

Switching profiles
------------------

Use **File → Switch profile** (``Ctrl+Shift+P``) at any time.  If a recording
is active it is stopped cleanly before the switch.  The application exits with
code 42 and ``main()`` re-shows the login dialog; the new session starts with
fresh ``Application`` and ``MainWindow`` objects loaded from the new profile's
settings.

.. tip::

   The current profile is always shown in the bottom-right corner of the
   status bar as a ``👤 @username`` chip, and in the window title bar as
   ``MOSAIC — @username``.

API usage
---------

.. code-block:: cpp

   mosaic::ProfileManager mgr;
   mgr.load();   // reads ~/.config/CSRU/mosaic/profiles.json

   // Register a new group
   using Res = mosaic::ProfileManager::RegisterResult;
   if (mgr.register_profile("social_neuro", "Social Neuroscience Lab", "pass1234")
           == Res::Ok) {
       // Profile directory and settings.json are created automatically
   }

   // Verify at login
   if (mgr.verify("social_neuro", "pass1234")) {
       mgr.touch("social_neuro");   // updates last_login
       QString path = mosaic::ProfileManager::settings_path("social_neuro");
       auto settings = mosaic::AppSettings::load(path).value_or(mosaic::AppSettings{});
   }

See :cpp:class:`mosaic::ProfileManager` for the full API.
