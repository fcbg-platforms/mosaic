#include "core/recording_access_control.hpp"

namespace mosaic {

QString legacy_shared_record_directory() { return "./recordings"; }

QString unassigned_record_directory() { return "./recordings/_unassigned"; }

QString default_record_directory_for(const QString& username) {
    return legacy_shared_record_directory() + "/" + username;
}

bool is_legacy_shared_record_directory(const QString& directory) {
    return directory == legacy_shared_record_directory();
}

QString resolve_migration_target(const QString& recordedBy, const QSet<QString>& knownUsernames) {
    if (!recordedBy.isEmpty() && knownUsernames.contains(recordedBy)) {
        return default_record_directory_for(recordedBy);
    }
    return unassigned_record_directory();
}

} // namespace mosaic
