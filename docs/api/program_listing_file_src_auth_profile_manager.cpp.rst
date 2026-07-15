
.. _program_listing_file_src_auth_profile_manager.cpp:

Program Listing for File profile_manager.cpp
============================================

|exhale_lsh| :ref:`Return to documentation for file <file_src_auth_profile_manager.cpp>` (``src/auth/profile_manager.cpp``)

.. |exhale_lsh| unicode:: U+021B0 .. UPWARDS ARROW WITH TIP LEFTWARDS

.. code-block:: cpp

   #include "auth/profile_manager.hpp"
   #include "utils/logger.hpp"
   #include <QCryptographicHash>
   #include <QDateTime>
   #include <QDir>
   #include <QFile>
   #include <QJsonArray>
   #include <QJsonDocument>
   #include <QJsonObject>
   #include <QPasswordDigestor>
   #include <QRandomGenerator>
   #include <QStandardPaths>
   #include <QRegularExpression>
   
   namespace mosaic {
   
   static constexpr int k_pbkdf2_iterations = 100'000;
   static constexpr int k_key_bytes         = 32;
   static constexpr int k_salt_bytes        = 32;
   
   // Accent palette — vivid enough to show clearly on the dark (#09091a) background.
   // White initials need at least ~40% lightness; these are tuned for that.
   static constexpr std::array k_accents = {
       "#5566dd",  // indigo-blue
       "#dd5566",  // rose-red
       "#33bb88",  // teal-green
       "#dd8833",  // amber
       "#33aacc",  // cyan
       "#cc44aa",  // magenta
       "#88aa33",  // olive-green
       "#6644cc",  // violet
   };
   
   struct ProfileManager::Impl {
       std::vector<Profile> profiles;
   };
   
   ProfileManager::ProfileManager(QObject* parent)
       : QObject(parent), d(std::make_unique<Impl>()) {}
   
   ProfileManager::~ProfileManager() = default;
   
   // ── Paths ──────────────────────────────────────────────────────────────────
   
   QString ProfileManager::root_dir() {
       return QStandardPaths::writableLocation(QStandardPaths::AppConfigLocation);
   }
   
   QString ProfileManager::profile_dir(const QString& username) {
       return root_dir() + "/profiles/" + username;
   }
   
   QString ProfileManager::settings_path(const QString& username) {
       return profile_dir(username) + "/settings.json";
   }
   
   QString ProfileManager::log_path(const QString& username) {
       return profile_dir(username) + "/mosaic.log";
   }
   
   // ── Persistence ────────────────────────────────────────────────────────────
   
   void ProfileManager::load() {
       d->profiles.clear();
   
       const QString path = root_dir() + "/profiles.json";
       QFile file(path);
       if (!file.open(QIODevice::ReadOnly | QIODevice::Text)) {
           log_info("[ProfileManager] No profiles manifest — starting fresh.");
           return;
       }
   
       QJsonParseError err;
       const auto doc = QJsonDocument::fromJson(file.readAll(), &err);
       if (err.error != QJsonParseError::NoError) {
           log_error(QString("[ProfileManager] Parse error in %1: %2").arg(path, err.errorString()));
           return;
       }
   
       for (const auto& val : doc.array()) {
           const QJsonObject obj = val.toObject();
           Profile prof;
           prof.username     = obj["username"].toString();
           prof.displayName  = obj["display_name"].toString();
           prof.initials     = obj["initials"].toString();
           prof.accentColour = obj["accent"].toString();
           prof.salt         = obj["salt"].toString();
           prof.passwordHash = obj["password_hash"].toString();
           prof.lastLogin    = QDateTime::fromString(obj["last_login"].toString(), Qt::ISODate);
           prof.created      = QDateTime::fromString(obj["created"].toString(),    Qt::ISODate);
           d->profiles.push_back(std::move(prof));
       }
   
       log_info(QString("[ProfileManager] Loaded %1 profile(s).").arg(d->profiles.size()));
   }
   
   bool ProfileManager::save() const {
       const QString dir  = root_dir();
       const QString path = dir + "/profiles.json";
       QDir().mkpath(dir);
   
       QJsonArray arr;
       for (const auto& prof : d->profiles) {
           arr.append(QJsonObject{
               {"username",      prof.username},
               {"display_name",  prof.displayName},
               {"initials",      prof.initials},
               {"accent",        prof.accentColour},
               {"salt",          prof.salt},
               {"password_hash", prof.passwordHash},
               {"last_login",    prof.lastLogin.toString(Qt::ISODate)},
               {"created",       prof.created.toString(Qt::ISODate)},
           });
       }
   
       QFile file(path);
       if (!file.open(QIODevice::WriteOnly | QIODevice::Text)) {
           log_error(QString("[ProfileManager] Cannot write profiles.json: %1").arg(file.errorString()));
           return false;
       }
       file.write(QJsonDocument(arr).toJson(QJsonDocument::Indented));
       return true;
   }
   
   // ── Queries ────────────────────────────────────────────────────────────────
   
   std::vector<Profile> ProfileManager::profiles() const { return d->profiles; }
   
   bool ProfileManager::has_profile(const QString& username) const {
       return find(username) != nullptr;
   }
   
   const Profile* ProfileManager::find(const QString& username) const {
       for (const auto& prof : d->profiles) {
           if (prof.username == username) { return &prof; }
       }
       return nullptr;
   }
   
   // ── Password helpers ───────────────────────────────────────────────────────
   
   QString ProfileManager::hash_password(const QString& password, const QByteArray& salt) {
       const QByteArray key = QPasswordDigestor::deriveKeyPbkdf2(
           QCryptographicHash::Sha256,
           password.toUtf8(),
           salt,
           k_pbkdf2_iterations,
           k_key_bytes);
       return QString::fromLatin1(key.toHex());
   }
   
   QString ProfileManager::generate_initials(const QString& displayName) {
       const QStringList parts = displayName.trimmed().split(' ', Qt::SkipEmptyParts);
       if (parts.isEmpty()) { return "??"; }
       if (parts.size() == 1) {
           return parts[0].left(2).toUpper();
       }
       return QString(parts.first()[0]).toUpper() + QString(parts.last()[0]).toUpper();
   }
   
   QString ProfileManager::next_accent_colour(int profileIndex) {
       return k_accents[static_cast<size_t>(profileIndex) % k_accents.size()];
   }
   
   // ── Registration ───────────────────────────────────────────────────────────
   
   ProfileManager::RegisterResult
   ProfileManager::register_profile(const QString& username,
                                     const QString& displayName,
                                     const QString& password) {
       // Validate username: 3–32 lowercase alphanumeric + underscore.
       static const QRegularExpression re("^[a-z0-9_]{3,32}$");
       if (!re.match(username).hasMatch()) {
           return RegisterResult::UsernameInvalid;
       }
   
       if (has_profile(username)) {
           return RegisterResult::UsernameTaken;
       }
   
       if (!password.isEmpty() && password.length() < 4) {
           return RegisterResult::PasswordTooShort;
       }
   
       // Generate salt and hash.
       QByteArray salt(k_salt_bytes, '\0');
       QRandomGenerator::global()->fillRange(
           reinterpret_cast<quint32*>(salt.data()),
           static_cast<qsizetype>(k_salt_bytes / sizeof(quint32)));
   
       Profile prof;
       prof.username     = username;
       prof.displayName  = displayName.trimmed().isEmpty() ? username : displayName.trimmed();
       prof.initials     = generate_initials(prof.displayName);
       prof.accentColour = next_accent_colour(static_cast<int>(d->profiles.size()));
       prof.salt         = QString::fromLatin1(salt.toHex());
       prof.passwordHash = password.isEmpty() ? QString{} : hash_password(password, salt);
       prof.created      = QDateTime::currentDateTimeUtc();
       prof.lastLogin    = prof.created;
   
       // Create the profile directory.
       QDir().mkpath(profile_dir(username));
   
       d->profiles.push_back(std::move(prof));
       [[maybe_unused]] const bool saved = save();
       emit profiles_changed();
       log_info(QString("[ProfileManager] Registered profile '%1'.").arg(username));
       return RegisterResult::Ok;
   }
   
   // ── Authentication ─────────────────────────────────────────────────────────
   
   bool ProfileManager::verify(const QString& username, const QString& password) const {
       const Profile* prof = find(username);
       if (!prof) { return false; }
       if (!prof->has_password()) { return true; }           // no password set — always pass
       const QByteArray salt = QByteArray::fromHex(prof->salt.toLatin1());
       return hash_password(password, salt) == prof->passwordHash;
   }
   
   void ProfileManager::touch(const QString& username) {
       for (auto& prof : d->profiles) {
           if (prof.username == username) {
               prof.lastLogin = QDateTime::currentDateTimeUtc();
               break;
           }
       }
       [[maybe_unused]] const bool saved = save();
   }
   
   // ── Mutation ───────────────────────────────────────────────────────────────
   
   bool ProfileManager::delete_profile(const QString& username) {
       const auto it = std::find_if(d->profiles.begin(), d->profiles.end(),
                                     [&](const Profile& prof) { return prof.username == username; });
       if (it == d->profiles.end()) { return false; }
       d->profiles.erase(it);
       [[maybe_unused]] const bool saved = save();
       emit profiles_changed();
       return true;
   }
   
   bool ProfileManager::rename_display(const QString& username, const QString& newDisplay) {
       for (auto& prof : d->profiles) {
           if (prof.username == username) {
               prof.displayName = newDisplay;
               prof.initials    = generate_initials(newDisplay);
               [[maybe_unused]] const bool saved = save();
               emit profiles_changed();
               return true;
           }
       }
       return false;
   }
   
   } // namespace mosaic
