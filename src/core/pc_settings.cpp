#include "pc_settings.hpp"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSaveFile>

#include "utils/dpapi_crypt.hpp"

namespace mosaic {

namespace {

QJsonObject read_object(const QString& path) {
    QFile f(path);
    if (!f.open(QIODevice::ReadOnly)) {
        return {};
    }
    return QJsonDocument::fromJson(f.readAll()).object();
}

} // namespace

QString pc_settings_path(const QString& rootDir) { return rootDir + "/pc_settings.json"; }

QString load_pc_hf_token(const QString& path) {
    const QString stored = read_object(path)["hf_token"].toString();
    return stored.isEmpty() ? QString() : dpapi_decrypt(stored).trimmed();
}

bool save_pc_hf_token(const QString& path, const QString& token) {
    QJsonObject o = read_object(path);
    if (token.trimmed().isEmpty()) {
        o.remove("hf_token");
    } else {
        o["hf_token"] = dpapi_encrypt(token.trimmed());
    }
    QDir().mkpath(QFileInfo(path).absolutePath());
    QSaveFile f(path);
    if (!f.open(QIODevice::WriteOnly)) {
        return false;
    }
    f.write(QJsonDocument(o).toJson());
    return f.commit();
}

HfTokens adopt_profile_hf_token(const QString& pcToken, const QString& profileToken) {
    HfTokens t{pcToken.trimmed(), profileToken.trimmed(), false};
    if (t.profile.isEmpty()) {
        return t;
    }
    if (t.pc.isEmpty()) {
        t.pc        = t.profile;
        t.pcChanged = true;
    }
    if (t.profile == t.pc) {
        t.profile.clear();
    }
    return t;
}

} // namespace mosaic
