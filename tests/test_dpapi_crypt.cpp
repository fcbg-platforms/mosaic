#include <gtest/gtest.h>

#include "utils/dpapi_crypt.hpp"

namespace mosaic {

TEST(DpapiCrypt, RoundTripsANormalToken) {
    // Deliberately NOT a real-looking token value (see this repo's own git
    // history for why: an earlier version of this test used what was
    // actually a live token, tripping GitHub's secret-scanning push
    // protection) — any plain string exercises the same code path, since
    // dpapi_encrypt()/dpapi_decrypt() have no format-specific logic at all.
    const QString original = "not-a-real-token-just-test-fixture-text-12345";
    const QString stored   = dpapi_encrypt(original);
    EXPECT_EQ(dpapi_decrypt(stored), original);
}

TEST(DpapiCrypt, EncryptingProducesTheVersionedMarkerOnWindows) {
    // On non-Windows builds dpapi_encrypt() is a pass-through, so this
    // assertion only makes sense where DPAPI is actually available.
#if defined(Q_OS_WIN)
    const QString stored = dpapi_encrypt("some-token");
    EXPECT_TRUE(stored.startsWith("dpapi:v1:"));
    EXPECT_NE(stored, "some-token"); // must not be stored as plaintext
#else
    GTEST_SKIP() << "DPAPI marker only applies on Windows builds";
#endif
}

TEST(DpapiCrypt, EmptyStringRoundTripsToEmpty) {
    EXPECT_EQ(dpapi_encrypt(""), "");
    EXPECT_EQ(dpapi_decrypt(""), "");
}

TEST(DpapiCrypt, DecryptingAnUnmarkedStringReturnsItUnchanged) {
    // A pre-existing plaintext value from before this feature existed (or
    // any string that never went through dpapi_encrypt()) must load
    // exactly as-is -- this is the whole "no migration step needed"
    // guarantee: to_json() re-encrypts it automatically on the next save.
    const QString legacyPlaintext = "hf_alreadyPlaintextFromBeforeThisFeature";
    EXPECT_EQ(dpapi_decrypt(legacyPlaintext), legacyPlaintext);
}

TEST(DpapiCrypt, DecryptingGarbageWithTheMarkerFailsClosedNotWithWrongBytes) {
    // Has the right prefix but isn't a real DPAPI blob -- must not crash,
    // and must not return corrupted/wrong bytes silently; an empty string
    // is the documented, safe failure mode (see dpapi_decrypt()'s own doc
    // comment) so the token field visibly goes blank instead of showing
    // garbage that looks like it might be a real token.
    const QString result = dpapi_decrypt("dpapi:v1:not-a-real-encrypted-blob");
#if defined(Q_OS_WIN)
    EXPECT_EQ(result, "");
#else
    // Pass-through build: the marker check itself is what's exercised.
    EXPECT_EQ(result, "dpapi:v1:not-a-real-encrypted-blob");
#endif
}

} // namespace mosaic
