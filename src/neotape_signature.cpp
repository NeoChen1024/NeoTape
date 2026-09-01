#include "neotape/signature.hpp"

extern "C" {
#define b64_pton neotape_b64_pton
#define b64_ntop neotape_b64_ntop
#include "signify/base64.h"
#include "signify/compat.h"
#include "signify/crypto_api.h"
#include "signify/libbsd/bsd/readpassphrase.h"
#include "signify/sha2.h"
#undef b64_ntop
#undef b64_pton
}

#include <algorithm>
#include <array>
#include <cerrno>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <format>
#include <fstream>
#include <iterator>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace neotape {

using std::array;
using std::format;
using std::size_t;
using std::string;
using std::string_view;
using std::vector;

namespace {

inline constexpr string_view comment_header = "untrusted comment: ";
inline constexpr array<uint8_t, 14> frame_signature_domain = {
    'N', 'e', 'o', 'T', 'a', 'p', 'e', '-', 'f', 'r', 'a', 'm', 'e', '\0'};
inline constexpr array<uint8_t, 13> auth_signature_domain = {
    'N', 'e', 'o', 'T', 'a', 'p', 'e', '-', 'a', 'u', 't', 'h', '\0'};
inline constexpr string_view pkalg = "Ed";
inline constexpr string_view kdfalg = "BK";
inline constexpr size_t public_key_blob_size = 42;
inline constexpr size_t secret_key_blob_size = 104;
inline constexpr size_t salt_size = 16;
inline constexpr size_t checksum_size = 8;

struct ArmoredBlob {
    string comment;
    string base64;
};

[[noreturn]] void fail_parse(const string &path, string_view msg) {
    throw std::runtime_error(format("{}: {}", path, msg));
}

void zeroize(void *data, size_t size) {
    if (size != 0) {
        explicit_bzero(data, size);
    }
}

void zeroize_string(string &value) {
    zeroize(value.data(), value.size());
    value.clear();
}

string read_text_file(const string &path) {
    std::ifstream input(path, std::ios::binary);
    if (!input) {
        fail_parse(path, format("open failed: {}", std::strerror(errno)));
    }
    return string(std::istreambuf_iterator<char>(input),
                  std::istreambuf_iterator<char>());
}

ArmoredBlob parse_armored_blob(const string &path) {
    string const text = read_text_file(path);
    size_t const first_nl = text.find('\n');
    if (first_nl == string::npos || !text.starts_with(comment_header)) {
        fail_parse(path, "invalid signify comment header");
    }

    size_t const second_line_start = first_nl + 1;
    size_t second_nl = text.find('\n', second_line_start);
    if (second_nl == string::npos) {
        second_nl = text.size();
    }
    if (second_nl == second_line_start) {
        fail_parse(path, "missing base64 payload");
    }

    if (second_nl != text.size()) {
        string_view trailing(text.data() + second_nl + 1,
                             text.size() - second_nl - 1);
        if (trailing.find_first_not_of("\r\n") != string_view::npos) {
            fail_parse(path, "unexpected trailing data after base64 payload");
        }
    }

    ArmoredBlob blob;
    blob.comment =
        text.substr(comment_header.size(), first_nl - comment_header.size());
    blob.base64 = text.substr(second_line_start, second_nl - second_line_start);
    if (blob.base64.size() > 2048) {
        fail_parse(path, "base64 payload too large");
    }

    return blob;
}

template <size_t N>
array<uint8_t, N> decode_armored_payload(const string &path, string *comment) {
    ArmoredBlob const blob = parse_armored_blob(path);
    array<uint8_t, N> decoded{};
    int const rv =
        neotape_b64_pton(blob.base64.c_str(), decoded.data(), decoded.size());
    if (rv != static_cast<int>(N)) {
        fail_parse(path, "invalid base64 payload");
    }
    if (comment != nullptr) {
        *comment = blob.comment;
    }
    return decoded;
}

uint32_t read_be32(const uint8_t *data) {
    return static_cast<uint32_t>(data[0]) << 24 |
           static_cast<uint32_t>(data[1]) << 16 |
           static_cast<uint32_t>(data[2]) << 8 | static_cast<uint32_t>(data[3]);
}

bool raw_key_id_equals(std::span<const uint8_t> lhs, const KeyIdBytes &rhs) {
    return lhs.size() == rhs.size() &&
           std::equal(lhs.begin(), lhs.end(), rhs.begin());
}

vector<uint8_t> domain_separated_message(std::span<const uint8_t> domain,
                                         std::span<const uint8_t> payload) {
    vector<uint8_t> message;
    message.reserve(domain.size() + payload.size());
    message.insert(message.end(), domain.begin(), domain.end());
    message.insert(message.end(), payload.begin(), payload.end());
    return message;
}

vector<uint8_t> frame_signature_message(const Hash &hash) {
    return domain_separated_message(frame_signature_domain, hash);
}

vector<uint8_t> auth_signature_message(const AuthNonceBytes &nonce) {
    return domain_separated_message(auth_signature_domain, nonce);
}

DetachedSignatureBytes sign_detached_message(const SignifySecretKey &key,
                                             std::span<const uint8_t> message) {
    vector<uint8_t> sigbuf(message.size() + crypto_sign_ed25519_BYTES);
    unsigned long long siglen = 0;
    if (crypto_sign_ed25519(sigbuf.data(), &siglen, message.data(),
                            message.size(), key.secret_key.data()) != 0) {
        zeroize(sigbuf.data(), sigbuf.size());
        throw std::runtime_error("crypto_sign_ed25519 failed");
    }

    DetachedSignatureBytes signature{};
    std::copy_n(sigbuf.begin(), signature.size(), signature.begin());
    zeroize(sigbuf.data(), sigbuf.size());
    return signature;
}

bool verify_detached_message_signature(const DetachedSignatureBytes &signature,
                                       std::span<const uint8_t> message,
                                       const SignifyPublicKey &key) {
    vector<uint8_t> signed_message(message.size() + signature.size());
    vector<uint8_t> dummy(signed_message.size());
    unsigned long long dummylen = 0;
    std::copy(signature.begin(), signature.end(), signed_message.begin());
    std::copy(message.begin(), message.end(),
              signed_message.begin() +
                  static_cast<std::ptrdiff_t>(signature.size()));

    bool const ok = crypto_sign_ed25519_open(
                        dummy.data(), &dummylen, signed_message.data(),
                        signed_message.size(), key.public_key.data()) == 0;
    zeroize(signed_message.data(), signed_message.size());
    zeroize(dummy.data(), dummy.size());
    return ok;
}

void verify_algorithms(const string &path, string_view expected,
                       std::span<const uint8_t> actual, string_view label) {
    if (actual.size() != expected.size() ||
        !std::equal(actual.begin(), actual.end(), expected.begin(),
                    expected.end())) {
        fail_parse(path, format("unsupported {} algorithm", label));
    }
}

string key_id_hex_impl(std::span<const uint8_t> key_id) {
    string hex;
    hex.reserve(key_id.size() * 2);
    for (uint8_t byte : key_id) {
        hex += format("{:02x}", static_cast<unsigned>(byte));
    }
    return hex;
}

} // namespace

string read_signify_passphrase_file(const string &path) {
    string passphrase = read_text_file(path);
    if (!passphrase.empty() && passphrase.back() == '\n') {
        passphrase.pop_back();
        if (!passphrase.empty() && passphrase.back() == '\r') {
            passphrase.pop_back();
        }
    }
    if (passphrase.find('\0') != string::npos) {
        fail_parse(path, "passphrase file must not contain NUL bytes");
    }
    return passphrase;
}

string prompt_signify_passphrase(const string &path) {
    char prompt[1024];
    int const n = std::snprintf(prompt, sizeof(prompt),
                                "passphrase for %s: ", path.c_str());
    if (n < 0 || static_cast<size_t>(n) >= sizeof(prompt)) {
        throw std::runtime_error("passphrase prompt too long");
    }

    char pass[1024];
    if (readpassphrase(prompt, pass, sizeof(pass),
                       RPP_ECHO_OFF | RPP_REQUIRE_TTY) == nullptr) {
        throw std::runtime_error(
            format("read passphrase for {}: {}", path, std::strerror(errno)));
    }
    if (pass[0] == '\0') {
        zeroize(pass, sizeof(pass));
        throw std::runtime_error(format("empty passphrase for {}", path));
    }

    string out(pass);
    zeroize(pass, sizeof(pass));
    return out;
}

AuthNonceBytes random_auth_nonce() {
    AuthNonceBytes nonce{};
    randombytes(nonce.data(), nonce.size());
    return nonce;
}

SignifyPublicKey load_signify_public_key(const string &path) {
    string comment;
    array<uint8_t, public_key_blob_size> const blob =
        decode_armored_payload<public_key_blob_size>(path, &comment);

    verify_algorithms(path, pkalg, std::span(blob).subspan(0, 2), "public key");

    SignifyPublicKey key;
    std::copy_n(blob.begin() + 2, key.key_id.size(), key.key_id.begin());
    std::copy_n(blob.begin() + 10, key.public_key.size(),
                key.public_key.begin());
    key.comment = std::move(comment);
    key.source_path = path;
    return key;
}

SignifySecretKey load_signify_secret_key(const string &path,
                                         std::optional<string> passphrase) {
    string comment;
    array<uint8_t, secret_key_blob_size> blob =
        decode_armored_payload<secret_key_blob_size>(path, &comment);

    verify_algorithms(path, pkalg, std::span(blob).subspan(0, 2), "secret key");
    verify_algorithms(path, kdfalg, std::span(blob).subspan(2, 2),
                      "secret key KDF");

    uint32_t const rounds = read_be32(blob.data() + 4);
    SignifySecretKey key;
    std::copy_n(blob.begin() + 32, key.key_id.size(), key.key_id.begin());
    key.comment = std::move(comment);
    key.source_path = path;

    array<uint8_t, crypto_sign_ed25519_SECRETKEYBYTES> xor_key{};
    if (rounds != 0) {
        if (!passphrase.has_value()) {
            passphrase = prompt_signify_passphrase(path);
        }
        if (passphrase->empty()) {
            fail_parse(path, "empty passphrase for encrypted secret key");
        }
        if (bcrypt_pbkdf(passphrase->data(), passphrase->size(),
                         blob.data() + 8, salt_size, xor_key.data(),
                         xor_key.size(), rounds) == -1) {
            zeroize_string(*passphrase);
            throw std::runtime_error(format("{}: bcrypt_pbkdf failed", path));
        }
        zeroize_string(*passphrase);
    }
    if (passphrase.has_value()) {
        zeroize_string(*passphrase);
    }

    for (size_t i = 0; i < key.secret_key.size(); ++i) {
        key.secret_key[i] = blob[40 + i] ^ xor_key[i];
    }
    zeroize(xor_key.data(), xor_key.size());

    array<uint8_t, SHA512_DIGEST_LENGTH> digest{};
    SHA2_CTX ctx;
    SHA512Init(&ctx);
    SHA512Update(&ctx, key.secret_key.data(), key.secret_key.size());
    SHA512Final(digest.data(), &ctx);
    if (!std::equal(digest.begin(), digest.begin() + checksum_size,
                    blob.begin() + 24)) {
        zeroize(digest.data(), digest.size());
        zeroize(key.secret_key.data(), key.secret_key.size());
        fail_parse(path, "incorrect passphrase");
    }
    zeroize(digest.data(), digest.size());
    zeroize(blob.data(), blob.size());
    return key;
}

KeyIdBytes signature_key_id(const SignatureBytes &signature) {
    KeyIdBytes key_id{};
    std::copy_n(signature.begin(), key_id.size(), key_id.begin());
    return key_id;
}

std::string key_id_hex(std::span<const uint8_t> key_id) {
    return key_id_hex_impl(key_id);
}

SignatureBytes sign_frame_hash(const SignifySecretKey &key, const Hash &hash) {
    vector<uint8_t> message = frame_signature_message(hash);
    DetachedSignatureBytes const detached = sign_detached_message(key, message);
    SignatureBytes signature{};
    std::copy(key.key_id.begin(), key.key_id.end(), signature.begin());
    std::copy_n(detached.begin(), detached.size(),
                signature.begin() +
                    static_cast<std::ptrdiff_t>(key.key_id.size()));
    zeroize(message.data(), message.size());
    return signature;
}

bool verify_frame_hash_signature(const SignatureBytes &signature,
                                 const Hash &hash,
                                 const SignifyPublicKey &key) {
    if (!raw_key_id_equals(std::span(signature).subspan(0, key_id_size),
                           key.key_id)) {
        return false;
    }

    vector<uint8_t> message = frame_signature_message(hash);
    DetachedSignatureBytes detached{};
    std::copy_n(signature.begin() + static_cast<std::ptrdiff_t>(key_id_size),
                detached.size(), detached.begin());
    bool const ok = verify_detached_message_signature(detached, message, key);
    zeroize(message.data(), message.size());
    return ok;
}

DetachedSignatureBytes sign_auth_nonce(const SignifySecretKey &key,
                                       const AuthNonceBytes &nonce) {
    vector<uint8_t> message = auth_signature_message(nonce);
    DetachedSignatureBytes const signature =
        sign_detached_message(key, message);
    zeroize(message.data(), message.size());
    return signature;
}

bool verify_auth_nonce_signature(const DetachedSignatureBytes &signature,
                                 const AuthNonceBytes &nonce,
                                 const SignifyPublicKey &key) {
    vector<uint8_t> message = auth_signature_message(nonce);
    bool const ok = verify_detached_message_signature(signature, message, key);
    zeroize(message.data(), message.size());
    return ok;
}

FrameSignatureValidation
validate_frame_signature(const FrameHeader &header,
                         const std::vector<SignifyPublicKey> &keys,
                         bool require_signed) {
    bool const signature_present = std::ranges::any_of(
        header.signature, [](uint8_t byte) { return byte != 0; });
    if (!has_frame_flag_signed(header.flags)) {
        if (signature_present) {
            return {FrameSignatureStatus::invalid,
                    format("non-zero signature bytes without SIGNED flag at "
                           "global_seq={}",
                           header.global_frame_seq_num)};
        }
        if (require_signed) {
            return {FrameSignatureStatus::invalid,
                    format("unsigned frame at global_seq={}",
                           header.global_frame_seq_num)};
        }
        return {FrameSignatureStatus::unsigned_frame, std::nullopt};
    }
    if (!signature_present) {
        return {FrameSignatureStatus::invalid,
                format("SIGNED flag set but signature bytes are all zero at "
                       "global_seq={}",
                       header.global_frame_seq_num)};
    }

    KeyIdBytes const key_id = signature_key_id(header.signature);
    if (keys.empty()) {
        if (require_signed) {
            return {FrameSignatureStatus::invalid,
                    format("require-signed validation has no configured public "
                           "key for signed frame at global_seq={} (key_id={})",
                           header.global_frame_seq_num,
                           key_id_hex_impl(key_id))};
        }
        return {FrameSignatureStatus::signed_unverified, std::nullopt};
    }

    auto const it = std::find_if(
        keys.begin(), keys.end(),
        [&](const SignifyPublicKey &key) { return key.key_id == key_id; });
    if (it == keys.end()) {
        return {FrameSignatureStatus::invalid,
                format("no public key for key_id={} at global_seq={}",
                       key_id_hex_impl(key_id), header.global_frame_seq_num)};
    }
    if (!verify_frame_hash_signature(header.signature, header.frame_hash,
                                     *it)) {
        return {FrameSignatureStatus::invalid,
                format("signature verification failed for key_id={} at "
                       "global_seq={}",
                       key_id_hex_impl(key_id), header.global_frame_seq_num)};
    }
    return {FrameSignatureStatus::verified, std::nullopt};
}

} // namespace neotape
