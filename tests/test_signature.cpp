#include "neotape/format.hpp"
#include "neotape/frame_builder.hpp"
#include "neotape/signature.hpp"

extern "C" {
#define b64_pton __b64_pton
#define b64_ntop __b64_ntop
#include "signify/base64.h"
#include "signify/compat.h"
#include "signify/sha2.h"
#undef b64_ntop
#undef b64_pton
}

#include <algorithm>
#include <array>
#include <cstdio>
#include <cstdlib>
#include <exception>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <unistd.h>

namespace {

using neotape::FrameHeader;
using neotape::Hash;
using neotape::SignifyPublicKey;
using neotape::SignifySecretKey;
using std::array;
using std::string;
using std::string_view;

inline constexpr string_view regress_pubkey =
    "3rdparty/signify/regress/regresskey.pub";
inline constexpr string_view regress_seckey =
    "3rdparty/signify/regress/regresskey.sec";

[[noreturn]] void fail(const string &msg) {
    std::cerr << "test_signature: " << msg << "\n";
    std::exit(1);
}

void expect(bool ok, const string &msg) {
    if (!ok) {
        fail(msg);
    }
}

template <class Fn> void expect_throw(Fn fn, const string &msg) {
    try {
        fn();
    } catch (const std::exception &) {
        return;
    }
    fail(msg);
}

Hash sample_hash() {
    Hash hash{};
    for (size_t i = 0; i < hash.size(); ++i) {
        hash[i] = static_cast<uint8_t>(i * 7U);
    }
    return hash;
}

string make_temp_path() {
    char path[] = "/tmp/neotape-signature-XXXXXX";
    int const fd = mkstemp(path);
    if (fd < 0) {
        fail("mkstemp failed");
    }
    close(fd);
    return path;
}

void write_text_file(const string &path, string_view text) {
    std::ofstream out(path, std::ios::binary);
    if (!out) {
        fail("failed to open temp file");
    }
    out.write(text.data(), static_cast<std::streamsize>(text.size()));
    if (!out) {
        fail("failed to write temp file");
    }
}

string armor_secret_key(const SignifySecretKey &key, string_view passphrase,
                        uint32_t rounds) {
    array<uint8_t, 104> blob{};
    blob[0] = 'E';
    blob[1] = 'd';
    blob[2] = 'B';
    blob[3] = 'K';
    blob[4] = static_cast<uint8_t>((rounds >> 24) & 0xffU);
    blob[5] = static_cast<uint8_t>((rounds >> 16) & 0xffU);
    blob[6] = static_cast<uint8_t>((rounds >> 8) & 0xffU);
    blob[7] = static_cast<uint8_t>(rounds & 0xffU);
    for (size_t i = 0; i < 16; ++i) {
        blob[8 + i] = static_cast<uint8_t>(0xa0U + i);
    }

    array<uint8_t, SHA512_DIGEST_LENGTH> digest{};
    SHA2_CTX ctx;
    SHA512Init(&ctx);
    SHA512Update(&ctx, key.secret_key.data(), key.secret_key.size());
    SHA512Final(digest.data(), &ctx);
    std::copy_n(digest.begin(), 8, blob.begin() + 24);
    std::copy(key.key_id.begin(), key.key_id.end(), blob.begin() + 32);

    array<uint8_t, 64> xor_key{};
    if (bcrypt_pbkdf(passphrase.data(), passphrase.size(), blob.data() + 8, 16,
                     xor_key.data(), xor_key.size(), rounds) == -1) {
        fail("bcrypt_pbkdf failed");
    }
    for (size_t i = 0; i < key.secret_key.size(); ++i) {
        blob[40 + i] = static_cast<uint8_t>(key.secret_key[i] ^ xor_key[i]);
    }
    char base64[512];
    if (__b64_ntop(blob.data(), blob.size(), base64, sizeof(base64)) == -1) {
        fail("b64_ntop failed");
    }
    return "untrusted comment: signify secret key\n" + string(base64) + "\n";
}

void test_load_signify_keys_and_sign_verify() {
    SignifyPublicKey const pubkey =
        neotape::load_signify_public_key(string(regress_pubkey));
    SignifySecretKey const seckey =
        neotape::load_signify_secret_key(string(regress_seckey));
    expect(pubkey.key_id == seckey.key_id, "key ids should match");

    Hash const hash = sample_hash();
    neotape::SignatureBytes const signature =
        neotape::sign_frame_hash(seckey, hash);
    expect(neotape::signature_key_id(signature) == pubkey.key_id,
           "signature key id should match public key");
    expect(neotape::verify_frame_hash_signature(signature, hash, pubkey),
           "signature should verify");
}

void test_load_encrypted_signify_secret_key() {
    SignifySecretKey const plain =
        neotape::load_signify_secret_key(string(regress_seckey));
    string const path = make_temp_path();
    string const passphrase = "unit-test-passphrase";
    write_text_file(path, armor_secret_key(plain, passphrase, 64));

    SignifySecretKey const loaded =
        neotape::load_signify_secret_key(path, string(passphrase));
    expect(loaded.key_id == plain.key_id, "encrypted key id should round-trip");
    expect(loaded.secret_key == plain.secret_key,
           "encrypted secret key should decrypt");

    expect_throw(
        [&] {
            neotape::load_signify_secret_key(path, string("wrong-passphrase"));
        },
        "wrong passphrase should fail");

    std::remove(path.c_str());
}

void test_validate_frame_signature() {
    SignifyPublicKey const pubkey =
        neotape::load_signify_public_key(string(regress_pubkey));
    SignifySecretKey const seckey =
        neotape::load_signify_secret_key(string(regress_seckey));
    Hash const hash = sample_hash();

    FrameHeader header;
    header.volume_block_size_kib = 4;
    header.archive_uuid = "00000000-0000-4000-8000-000000000123";
    header.archive_label = "sig-test";
    header.global_frame_seq_num = 9;
    header.frame_hash = hash;
    header.flags = neotape::frame_flag_signed;
    header.signature = neotape::sign_frame_hash(seckey, hash);

    expect(!neotape::validate_frame_signature(header, {pubkey}).has_value(),
           "signed frame should validate");
    expect(neotape::validate_frame_signature(header, {}).has_value(),
           "signed frame without key should fail");

    header.flags = 0;
    header.signature.fill(0);
    expect(!neotape::validate_frame_signature(header, {pubkey}).has_value(),
           "unsigned frame should validate when signatures are optional");
    expect(neotape::validate_frame_signature(header, {pubkey}, true).has_value(),
           "unsigned frame should fail when signatures are required");
}

void test_auth_nonce_sign_verify() {
    SignifyPublicKey const pubkey =
        neotape::load_signify_public_key(string(regress_pubkey));
    SignifySecretKey const seckey =
        neotape::load_signify_secret_key(string(regress_seckey));

    neotape::AuthNonceBytes nonce{};
    for (size_t i = 0; i < nonce.size(); ++i) {
        nonce[i] = static_cast<uint8_t>(0x40U + i);
    }

    neotape::DetachedSignatureBytes const signature =
        neotape::sign_auth_nonce(seckey, nonce);
    expect(neotape::verify_auth_nonce_signature(signature, nonce, pubkey),
           "auth nonce signature should verify");

    nonce[0] ^= 0x5aU;
    expect(!neotape::verify_auth_nonce_signature(signature, nonce, pubkey),
           "auth nonce signature should fail for wrong nonce");
}

void test_patch_volume_seq_num_finalizes_deferred_record() {
    SignifyPublicKey const pubkey =
        neotape::load_signify_public_key(string(regress_pubkey));
    SignifySecretKey const seckey =
        neotape::load_signify_secret_key(string(regress_seckey));

    neotape::ContentFrameBuilder builder(
        4096, "00000000-0000-4000-8000-000000000123", "sig-test");
    builder.set_current_slice(0);

    std::array<std::byte, 32> payload{};
    for (size_t i = 0; i < payload.size(); ++i) {
        payload[i] = static_cast<std::byte>(i + 1);
    }

    auto maybe_frame = builder.flush();
    expect(!maybe_frame.has_value(),
           "flush without pending data should return no frame");

    auto frames = builder.feed(payload);
    expect(frames.empty(), "small payload should stay pending");

    maybe_frame = builder.flush();
    expect(maybe_frame.has_value(), "flush should produce final frame");

    FrameHeader const deferred = neotape::parse_fixed_header(
        reinterpret_cast<const uint8_t *>(maybe_frame->record.data()),
        maybe_frame->record.size());
    expect(deferred.volume_seq_num == 0,
           "deferred frame should keep placeholder volume seq");
    expect(std::all_of(deferred.frame_hash.begin(), deferred.frame_hash.end(),
                       [](uint8_t byte) { return byte == 0; }),
           "deferred frame hash should be zero until finalization");
    expect(!neotape::has_frame_flag_signed(deferred.flags),
           "deferred frame should not be marked signed");

    neotape::patch_volume_seq_num(maybe_frame->record, 7, &seckey);

    FrameHeader const finalized = neotape::parse_fixed_header(
        reinterpret_cast<const uint8_t *>(maybe_frame->record.data()),
        maybe_frame->record.size());
    expect(finalized.volume_seq_num == 7, "volume seq should be patched");
    expect(neotape::has_frame_flag_signed(finalized.flags),
           "finalized frame should be marked signed");
    expect(neotape::compute_frame_hash(
               reinterpret_cast<const uint8_t *>(maybe_frame->record.data()),
               maybe_frame->record.size()) == finalized.frame_hash,
           "finalized frame hash should match record bytes");
    expect(neotape::verify_frame_hash_signature(finalized.signature,
                                                finalized.frame_hash, pubkey),
           "finalized deferred frame signature should verify");
}

} // namespace

int main() {
    test_load_signify_keys_and_sign_verify();
    test_load_encrypted_signify_secret_key();
    test_validate_frame_signature();
    test_auth_nonce_sign_verify();
    test_patch_volume_seq_num_finalizes_deferred_record();
    return 0;
}
