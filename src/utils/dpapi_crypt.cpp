#include "utils/dpapi_crypt.hpp"

#include <QByteArray>

#include "utils/logger.hpp"

#if defined(Q_OS_WIN)
#define WIN32_LEAN_AND_MEAN
// clang-format off
// windows.h must be included before wincrypt.h — wincrypt.h relies on types
// windows.h defines and doesn't pull them in itself. clang-format's
// alphabetical include sort would otherwise put wincrypt.h first (same
// class of bug already hit once for dbghelp.h/windows.h in main.cpp).
#include <windows.h>
#include <wincrypt.h>
// clang-format on
#endif

namespace mosaic {

namespace {
const QLatin1String k_dpapi_marker("dpapi:v1:");
}

#if defined(Q_OS_WIN)

QString dpapi_encrypt(const QString& plaintext) {
    if (plaintext.isEmpty()) {
        return {};
    }

    const QByteArray utf8 = plaintext.toUtf8();
    DATA_BLOB in{};
    in.cbData = static_cast<DWORD>(utf8.size());
    in.pbData = reinterpret_cast<BYTE*>(const_cast<char*>(utf8.constData()));

    DATA_BLOB out{};
    const BOOL ok = CryptProtectData(&in, L"MOSAIC analysis token", nullptr, nullptr, nullptr,
                                     CRYPTPROTECT_UI_FORBIDDEN, &out);
    if (!ok) {
        log_warning("[dpapi_crypt] CryptProtectData failed — storing token unprotected.");
        return plaintext;
    }

    const QByteArray cipher(reinterpret_cast<const char*>(out.pbData),
                            static_cast<int>(out.cbData));
    LocalFree(out.pbData);

    return k_dpapi_marker + QString::fromLatin1(cipher.toBase64());
}

QString dpapi_decrypt(const QString& stored) {
    if (!stored.startsWith(k_dpapi_marker)) {
        return stored; // legacy plaintext (or empty) — returned unchanged
    }

    const QByteArray cipher = QByteArray::fromBase64(stored.mid(k_dpapi_marker.size()).toLatin1());
    if (cipher.isEmpty()) {
        log_warning(
            "[dpapi_crypt] Stored token has the dpapi: marker but decodes to "
            "empty ciphertext — treating as missing.");
        return {};
    }

    DATA_BLOB in{};
    in.cbData = static_cast<DWORD>(cipher.size());
    in.pbData = reinterpret_cast<BYTE*>(const_cast<char*>(cipher.constData()));

    DATA_BLOB out{};
    const BOOL ok = CryptUnprotectData(&in, nullptr, nullptr, nullptr, nullptr,
                                       CRYPTPROTECT_UI_FORBIDDEN, &out);
    if (!ok) {
        log_warning(
            "[dpapi_crypt] CryptUnprotectData failed (settings.json moved to a "
            "different machine/user account, or the blob is corrupted) — token "
            "cleared; re-enter it in the Diarization plugin's token field.");
        return {};
    }

    const QString plaintext =
        QString::fromUtf8(reinterpret_cast<const char*>(out.pbData), static_cast<int>(out.cbData));
    LocalFree(out.pbData);
    return plaintext;
}

#else // !Q_OS_WIN — DPAPI is Windows-only; pass through unprotected.

QString dpapi_encrypt(const QString& plaintext) { return plaintext; }
QString dpapi_decrypt(const QString& stored) { return stored; }

#endif

} // namespace mosaic
