//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
//
// File: src/tests/unit/test_windows_installer_update.cpp
// Purpose: Verify signed Windows update manifests without network access.
//
// Key invariants:
//   - Tests generate an ephemeral RSA-2048 key with Windows CNG.
//   - Tampering, wrong architecture, and cross-origin links are rejected.
//
// Ownership/Lifetime: All CNG handles are destroyed before test exit.
//
// Links: WindowsInstallerUpdate.cpp, WindowsInstallerVersion.cpp
//
//===----------------------------------------------------------------------===//

#include "tests/TestHarness.hpp"

#include "PkgHash.hpp"
#include "tools/windows_installer/WindowsInstallerUpdate.hpp"

#include <bcrypt.h>
#include <windows.h>

#include <cstdint>
#include <cstring>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace zanna::installer {

std::wstring utf8ToWide(std::string_view text) {
    if (text.empty())
        return {};
    const int required = MultiByteToWideChar(
        CP_UTF8, MB_ERR_INVALID_CHARS, text.data(), static_cast<int>(text.size()), nullptr, 0);
    if (required <= 0)
        throw std::runtime_error("invalid test UTF-8");
    std::wstring result(static_cast<std::size_t>(required), L'\0');
    if (MultiByteToWideChar(CP_UTF8,
                            MB_ERR_INVALID_CHARS,
                            text.data(),
                            static_cast<int>(text.size()),
                            result.data(),
                            required) != required) {
        throw std::runtime_error("cannot convert test UTF-8");
    }
    return result;
}

std::string wideToUtf8(std::wstring_view text) {
    if (text.empty())
        return {};
    const int required = WideCharToMultiByte(CP_UTF8,
                                             WC_ERR_INVALID_CHARS,
                                             text.data(),
                                             static_cast<int>(text.size()),
                                             nullptr,
                                             0,
                                             nullptr,
                                             nullptr);
    if (required <= 0)
        throw std::runtime_error("invalid test UTF-16");
    std::string result(static_cast<std::size_t>(required), '\0');
    if (WideCharToMultiByte(CP_UTF8,
                            WC_ERR_INVALID_CHARS,
                            text.data(),
                            static_cast<int>(text.size()),
                            result.data(),
                            required,
                            nullptr,
                            nullptr) != required) {
        throw std::runtime_error("cannot convert test UTF-16");
    }
    return result;
}

} // namespace zanna::installer

namespace {

class Algorithm {
  public:
    Algorithm() {
        if (BCryptOpenAlgorithmProvider(&value_, BCRYPT_RSA_ALGORITHM, nullptr, 0) != 0)
            throw std::runtime_error("cannot open RSA provider");
    }

    ~Algorithm() {
        if (value_)
            BCryptCloseAlgorithmProvider(value_, 0);
    }

    BCRYPT_ALG_HANDLE get() const {
        return value_;
    }

  private:
    BCRYPT_ALG_HANDLE value_{nullptr};
};

class Key {
  public:
    explicit Key(BCRYPT_ALG_HANDLE algorithm) {
        if (BCryptGenerateKeyPair(algorithm, &value_, 2048, 0) != 0 ||
            BCryptFinalizeKeyPair(value_, 0) != 0) {
            throw std::runtime_error("cannot generate RSA test key");
        }
    }

    ~Key() {
        if (value_)
            BCryptDestroyKey(value_);
    }

    BCRYPT_KEY_HANDLE get() const {
        return value_;
    }

  private:
    BCRYPT_KEY_HANDLE value_{nullptr};
};

std::string hex(const uint8_t *bytes, std::size_t size) {
    static constexpr char kDigits[] = "0123456789abcdef";
    std::string result(size * 2U, '0');
    for (std::size_t index = 0; index < size; ++index) {
        result[index * 2U] = kDigits[bytes[index] >> 4U];
        result[index * 2U + 1U] = kDigits[bytes[index] & 0x0fU];
    }
    return result;
}

std::vector<uint8_t> decodeHex(std::string_view value) {
    auto nibble = [](char ch) -> uint8_t {
        if (ch >= '0' && ch <= '9')
            return static_cast<uint8_t>(ch - '0');
        return static_cast<uint8_t>(ch - 'a' + 10);
    };
    std::vector<uint8_t> result(value.size() / 2U);
    for (std::size_t index = 0; index < result.size(); ++index)
        result[index] = static_cast<uint8_t>((nibble(value[index * 2U]) << 4U) |
                                             nibble(value[index * 2U + 1U]));
    return result;
}

struct TestSigner {
    Algorithm algorithm;
    Key key{algorithm.get()};
    std::string modulus;
    std::string exponent;

    TestSigner() {
        ULONG required = 0;
        if (BCryptExportKey(key.get(), nullptr, BCRYPT_RSAPUBLIC_BLOB, nullptr, 0, &required, 0) !=
            0)
            throw std::runtime_error("cannot size RSA public blob");
        std::vector<uint8_t> blob(required);
        if (BCryptExportKey(key.get(),
                            nullptr,
                            BCRYPT_RSAPUBLIC_BLOB,
                            blob.data(),
                            static_cast<ULONG>(blob.size()),
                            &required,
                            0) != 0 ||
            blob.size() < sizeof(BCRYPT_RSAKEY_BLOB)) {
            throw std::runtime_error("cannot export RSA public blob");
        }
        const auto *header = reinterpret_cast<const BCRYPT_RSAKEY_BLOB *>(blob.data());
        const uint8_t *publicExponent = blob.data() + sizeof(*header);
        const uint8_t *publicModulus = publicExponent + header->cbPublicExp;
        exponent = hex(publicExponent, header->cbPublicExp);
        modulus = hex(publicModulus, header->cbModulus);
    }

    std::string sign(std::string_view canonical) const {
        const std::string digestHex = zanna::pkg::sha256Hex(
            reinterpret_cast<const uint8_t *>(canonical.data()), canonical.size());
        std::vector<uint8_t> digest = decodeHex(digestHex);
        BCRYPT_PKCS1_PADDING_INFO padding{BCRYPT_SHA256_ALGORITHM};
        ULONG required = 0;
        if (BCryptSignHash(key.get(),
                           &padding,
                           digest.data(),
                           static_cast<ULONG>(digest.size()),
                           nullptr,
                           0,
                           &required,
                           BCRYPT_PAD_PKCS1) != 0) {
            throw std::runtime_error("cannot size RSA test signature");
        }
        std::vector<uint8_t> signature(required);
        if (BCryptSignHash(key.get(),
                           &padding,
                           digest.data(),
                           static_cast<ULONG>(digest.size()),
                           signature.data(),
                           static_cast<ULONG>(signature.size()),
                           &required,
                           BCRYPT_PAD_PKCS1) != 0) {
            throw std::runtime_error("cannot create RSA test signature");
        }
        signature.resize(required);
        return hex(signature.data(), signature.size());
    }
};

zanna::installer::HostPackage packageFor(const TestSigner &signer) {
    zanna::installer::HostPackage package;
    package.metadata.version = "1.2.3";
    package.metadata.channel = "stable";
    package.metadata.architecture = "x64";
    package.metadata.updateManifestUrl = "https://updates.example.test/zanna/windows.txt";
    package.metadata.updateRsaModulus = signer.modulus;
    package.metadata.updateRsaExponent = signer.exponent;
    return package;
}

std::string signedManifest(
    const TestSigner &signer,
    std::string_view version,
    std::string_view architecture = "x64",
    std::string_view downloadUrl = "https://updates.example.test/zanna/zanna-setup.exe",
    std::string_view sha256 = "0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef",
    std::string_view releaseNotesUrl = "https://updates.example.test/zanna/notes.html") {
    std::string canonical = "ZANNA-WINDOWS-UPDATE\t1\n"
                            "channel\tstable\n"
                            "architecture\t" +
                            std::string(architecture) + "\nversion\t" + std::string(version) +
                            "\ndownload-url\t" + std::string(downloadUrl) + "\nsha256\t" +
                            std::string(sha256) + "\nrelease-notes-url\t" +
                            std::string(releaseNotesUrl) + "\n";
    return canonical + "signature\t" + signer.sign(canonical) + "\n";
}

} // namespace

TEST(WindowsInstallerUpdate, AcceptsAuthenticSameOriginUpgrade) {
    const TestSigner signer;
    const auto result =
        zanna::installer::verifyUpdateManifest(packageFor(signer), signedManifest(signer, "1.3.0"));
    EXPECT_TRUE(result.status == zanna::installer::UpdateStatus::Available);
    EXPECT_EQ(result.availableVersion, "1.3.0");
    EXPECT_EQ(result.releaseNotesUrl, "https://updates.example.test/zanna/notes.html");
    const std::wstring json = zanna::installer::updateResultJson(result);
    EXPECT_TRUE(json.find(L"\"release_notes_url\": "
                          L"\"https://updates.example.test/zanna/notes.html\"") !=
                std::wstring::npos);
}

TEST(WindowsInstallerUpdate, DetectsCurrentVersion) {
    const TestSigner signer;
    const auto result =
        zanna::installer::verifyUpdateManifest(packageFor(signer), signedManifest(signer, "1.2.3"));
    EXPECT_TRUE(result.status == zanna::installer::UpdateStatus::Current);
}

TEST(WindowsInstallerUpdate, RejectsSignatureTampering) {
    const TestSigner signer;
    std::string manifest = signedManifest(signer, "1.3.0");
    manifest.replace(
        manifest.find("version\t1.3.0"), std::strlen("version\t1.3.0"), "version\t9.9.9");
    EXPECT_THROWS(zanna::installer::verifyUpdateManifest(packageFor(signer), manifest),
                  std::runtime_error);
}

TEST(WindowsInstallerUpdate, RequiresCanonicalLfTerminatedManifestBytes) {
    const TestSigner signer;
    const auto package = packageFor(signer);
    std::string manifest = signedManifest(signer, "1.3.0");
    manifest.pop_back();
    EXPECT_THROWS(zanna::installer::verifyUpdateManifest(package, manifest), std::runtime_error);

    manifest = signedManifest(signer, "1.3.0");
    std::string crlf;
    crlf.reserve(manifest.size() + 8U);
    for (const char ch : manifest) {
        if (ch == '\n')
            crlf.push_back('\r');
        crlf.push_back(ch);
    }
    EXPECT_THROWS(zanna::installer::verifyUpdateManifest(package, crlf), std::runtime_error);
}

TEST(WindowsInstallerUpdate, RejectsCrossOriginAndWrongArchitecture) {
    const TestSigner signer;
    EXPECT_THROWS(zanna::installer::verifyUpdateManifest(
                      packageFor(signer),
                      signedManifest(signer, "1.3.0", "x64", "https://evil.example/zanna.exe")),
                  std::runtime_error);
    EXPECT_THROWS(zanna::installer::verifyUpdateManifest(packageFor(signer),
                                                         signedManifest(signer, "1.3.0", "arm64")),
                  std::runtime_error);
}

TEST(WindowsInstallerUpdate, ComparesOriginsWithWindowsOrdinalCaseRules) {
    const TestSigner signer;
    auto package = packageFor(signer);
    package.metadata.updateManifestUrl = "https://UPDATES.EXAMPLE.TEST/zanna/windows.txt";
    const auto result =
        zanna::installer::verifyUpdateManifest(package, signedManifest(signer, "1.3.0"));
    EXPECT_TRUE(result.status == zanna::installer::UpdateStatus::Available);
}

TEST(WindowsInstallerUpdate, RejectsAmbiguousUrlCharacters) {
    const TestSigner signer;
    EXPECT_THROWS(zanna::installer::verifyUpdateManifest(
                      packageFor(signer),
                      signedManifest(
                          signer, "1.3.0", "x64", "https://updates.example.test/zanna\\setup.exe")),
                  std::runtime_error);
    EXPECT_THROWS(
        zanna::installer::verifyUpdateManifest(
            packageFor(signer),
            signedManifest(signer,
                           "1.3.0",
                           "x64",
                           "https://updates.example.test/zanna/setup.exe",
                           "0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef",
                           "https://updates.example.test/release notes.html")),
        std::runtime_error);
}

TEST(WindowsInstallerUpdate, RejectsPartialOrMalformedPinnedKeys) {
    const TestSigner signer;
    auto package = packageFor(signer);
    package.metadata.updateRsaExponent.clear();
    EXPECT_THROWS(zanna::installer::verifyUpdateManifest(package, signedManifest(signer, "1.3.0")),
                  std::runtime_error);

    package = packageFor(signer);
    package.metadata.updateRsaModulus = "01";
    EXPECT_THROWS(zanna::installer::verifyUpdateManifest(package, signedManifest(signer, "1.3.0")),
                  std::runtime_error);

    package = packageFor(signer);
    package.metadata.updateRsaExponent = "04";
    EXPECT_THROWS(zanna::installer::verifyUpdateManifest(package, signedManifest(signer, "1.3.0")),
                  std::runtime_error);
}

TEST(WindowsInstallerUpdate, RejectsPartialConfigurationBeforeNetworkAccess) {
    const TestSigner signer;
    auto package = packageFor(signer);
    package.metadata.updateRsaExponent.clear();
    EXPECT_THROWS(zanna::installer::checkForUpdates(package), std::runtime_error);
}

TEST(WindowsInstallerUpdate, BoundsDigestAndSignatureBeforeDecoding) {
    const TestSigner signer;
    EXPECT_THROWS(
        zanna::installer::verifyUpdateManifest(
            packageFor(signer),
            signedManifest(
                signer, "1.3.0", "x64", "https://updates.example.test/zanna/setup.exe", "00")),
        std::runtime_error);

    std::string manifest = signedManifest(signer, "1.3.0");
    const std::size_t signature = manifest.find("signature\t");
    manifest.replace(signature + std::strlen("signature\t"), signer.modulus.size(), "00");
    EXPECT_THROWS(zanna::installer::verifyUpdateManifest(packageFor(signer), manifest),
                  std::runtime_error);
}

TEST(WindowsInstallerUpdate, RejectsForgedStatusAndPresentationUrl) {
    const TestSigner signer;
    const auto package = packageFor(signer);
    auto result = zanna::installer::verifyUpdateManifest(package, signedManifest(signer, "1.3.0"));
    result.status = static_cast<zanna::installer::UpdateStatus>(99);
    EXPECT_THROWS(zanna::installer::updateResultJson(result), std::runtime_error);
    EXPECT_THROWS(zanna::installer::showUpdateResult(nullptr, package, result), std::runtime_error);

    result = zanna::installer::verifyUpdateManifest(package, signedManifest(signer, "1.3.0"));
    result.releaseNotesUrl = "https://evil.example/release-notes";
    EXPECT_THROWS(zanna::installer::showUpdateResult(nullptr, package, result), std::runtime_error);

    auto unconfigured = package;
    unconfigured.metadata.updateManifestUrl.clear();
    unconfigured.metadata.updateRsaModulus.clear();
    unconfigured.metadata.updateRsaExponent.clear();
    result = zanna::installer::verifyUpdateManifest(unconfigured, {});
    result.channel = "forged";
    EXPECT_THROWS(zanna::installer::showUpdateResult(nullptr, unconfigured, result),
                  std::runtime_error);
}

int main(int argc, char **argv) {
    zanna_test::init(&argc, argv);
    return zanna_test::run_all_tests();
}
