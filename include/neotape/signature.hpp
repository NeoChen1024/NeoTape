#pragma once

#include "neotape/format.hpp"

#include <array>
#include <optional>
#include <span>
#include <string>
#include <vector>

namespace neotape {

inline constexpr std::size_t key_id_size = 8;
inline constexpr std::size_t auth_nonce_size = 32;
using KeyIdBytes = std::array<uint8_t, key_id_size>;
using AuthNonceBytes = std::array<uint8_t, auth_nonce_size>;
using PublicKeyBytes = std::array<uint8_t, 32>;
using SecretKeyBytes = std::array<uint8_t, 64>;
using DetachedSignatureBytes = std::array<uint8_t, 64>;

struct SignifyPublicKey {
    KeyIdBytes key_id{};
    PublicKeyBytes public_key{};
    std::string comment;
    std::string source_path;
};

struct SignifySecretKey {
    KeyIdBytes key_id{};
    SecretKeyBytes secret_key{};
    std::string comment;
    std::string source_path;
};

enum class FrameSignatureStatus {
    unsigned_frame,
    signed_unverified,
    verified,
    invalid,
};

// `error` is populated exactly when status is invalid. A
// signed_unverified result is successful integrity-only validation and does
// not claim authenticity.
struct FrameSignatureValidation {
    FrameSignatureStatus status = FrameSignatureStatus::unsigned_frame;
    std::optional<std::string> error;
};

SignifyPublicKey load_signify_public_key(const std::string &path);
SignifySecretKey
load_signify_secret_key(const std::string &path,
                        std::optional<std::string> passphrase = std::nullopt);
std::string read_signify_passphrase_file(const std::string &path);
std::string prompt_signify_passphrase(const std::string &path);
AuthNonceBytes random_auth_nonce();

KeyIdBytes signature_key_id(const SignatureBytes &signature);
std::string key_id_hex(std::span<const uint8_t> key_id);
SignatureBytes sign_frame_hash(const SignifySecretKey &key, const Hash &hash);
bool verify_frame_hash_signature(const SignatureBytes &signature,
                                 const Hash &hash, const SignifyPublicKey &key);
DetachedSignatureBytes sign_auth_nonce(const SignifySecretKey &key,
                                       const AuthNonceBytes &nonce);
bool verify_auth_nonce_signature(const DetachedSignatureBytes &signature,
                                 const AuthNonceBytes &nonce,
                                 const SignifyPublicKey &key);
FrameSignatureValidation
validate_frame_signature(const FrameHeader &header,
                         const std::vector<SignifyPublicKey> &keys,
                         bool require_signed = false);

} // namespace neotape
