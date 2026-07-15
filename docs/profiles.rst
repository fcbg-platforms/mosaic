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
