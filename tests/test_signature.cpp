#include "neotape/format.hpp"
#include "neotape/frame_builder.hpp"
#include "neotape/signature.hpp"

#include <catch2/catch_test_macros.hpp>

extern "C" {
#define b64_pton neotape_b64_pton
#define b64_ntop neotape_b64_ntop
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
const string regress_pubkey =
    string(NEOTAPE_SOURCE_DIR) + "/3rdparty/signify/regress/regresskey.pub";
const string regress_seckey =
    string(NEOTAPE_SOURCE_DIR) + "/3rdparty/signify/regress/regresskey.sec";

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
    REQUIRE(fd >= 0);
    close(fd);
    return path;
}

void write_text_file(const string &path, string_view text) {
    std::ofstream out(path, std::ios::binary);
    REQUIRE(out.good());
    out.write(text.data(), static_cast<std::streamsize>(text.size()));
    REQUIRE(out.good());
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
    REQUIRE(bcrypt_pbkdf(passphrase.data(), passphrase.size(), blob.data() + 8,
                         16, xor_key.data(), xor_key.size(), rounds) == 0);
    for (size_t i = 0; i < key.secret_key.size(); ++i) {
        blob[40 + i] = static_cast<uint8_t>(key.secret_key[i] ^ xor_key[i]);
    }
    char base64[512];
    REQUIRE(neotape_b64_ntop(blob.data(), blob.size(), base64,
                             sizeof(base64)) >= 0);
    return "untrusted comment: signify secret key\n" + string(base64) + "\n";
}

TEST_CASE("signature: load signify keys and sign verify", "[unit][signature]") {
    SignifyPublicKey const pubkey =
        neotape::load_signify_public_key(regress_pubkey);
    SignifySecretKey const seckey =
        neotape::load_signify_secret_key(regress_seckey);
    REQUIRE(pubkey.key_id == seckey.key_id);

    Hash const hash = sample_hash();
    neotape::SignatureBytes const signature =
        neotape::sign_frame_hash(seckey, hash);
    REQUIRE(neotape::signature_key_id(signature) == pubkey.key_id);
    REQUIRE(neotape::verify_frame_hash_signature(signature, hash, pubkey));
}

TEST_CASE("signature: load encrypted signify secret key", "[unit][signature]") {
    SignifySecretKey const plain =
        neotape::load_signify_secret_key(string(regress_seckey));
    string const path = make_temp_path();
    string const passphrase = "unit-test-passphrase";
    write_text_file(path, armor_secret_key(plain, passphrase, 64));

    SignifySecretKey const loaded =
        neotape::load_signify_secret_key(path, string(passphrase));
    REQUIRE(loaded.key_id == plain.key_id);
    REQUIRE(loaded.secret_key == plain.secret_key);

    REQUIRE_THROWS_AS(
        neotape::load_signify_secret_key(path, string("wrong-passphrase")),
        std::exception);

    std::remove(path.c_str());
}

TEST_CASE("signature: validate frame signature", "[unit][signature]") {
    SignifyPublicKey const pubkey =
        neotape::load_signify_public_key(regress_pubkey);
    SignifySecretKey const seckey =
        neotape::load_signify_secret_key(regress_seckey);
    Hash const hash = sample_hash();

    FrameHeader header;
    header.volume_block_size_kib = 4;
    header.archive_uuid = "00000000-0000-4000-8000-000000000123";
    header.archive_label = "sig-test";
    header.global_frame_seq_num = 9;
    header.frame_hash = hash;
    header.flags = neotape::frame_flag_signed;
    header.signature = neotape::sign_frame_hash(seckey, hash);

    auto validation = neotape::validate_frame_signature(header, {pubkey});
    REQUIRE(!validation.error.has_value());
    REQUIRE(validation.status == neotape::FrameSignatureStatus::verified);

    validation = neotape::validate_frame_signature(header, {});
    REQUIRE(!validation.error.has_value());
    REQUIRE(validation.status ==
            neotape::FrameSignatureStatus::signed_unverified);

    validation = neotape::validate_frame_signature(header, {}, true);
    REQUIRE(validation.error.has_value());
    REQUIRE(validation.status == neotape::FrameSignatureStatus::invalid);

    header.signature.fill(0);
    validation = neotape::validate_frame_signature(header, {});
    REQUIRE(validation.error.has_value());
    REQUIRE(validation.status == neotape::FrameSignatureStatus::invalid);

    header.flags = 0;
    header.signature.fill(0);
    validation = neotape::validate_frame_signature(header, {pubkey});
    REQUIRE(!validation.error.has_value());
    REQUIRE(validation.status == neotape::FrameSignatureStatus::unsigned_frame);

    header.signature[0] = 1;
    validation = neotape::validate_frame_signature(header, {pubkey});
    REQUIRE(validation.error.has_value());
    header.signature.fill(0);

    validation = neotape::validate_frame_signature(header, {pubkey}, true);
    REQUIRE(validation.error.has_value());
}

TEST_CASE("signature: auth nonce sign verify", "[unit][signature]") {
    SignifyPublicKey const pubkey =
        neotape::load_signify_public_key(regress_pubkey);
    SignifySecretKey const seckey =
        neotape::load_signify_secret_key(regress_seckey);

    neotape::AuthNonceBytes nonce{};
    for (size_t i = 0; i < nonce.size(); ++i) {
        nonce[i] = static_cast<uint8_t>(0x40U + i);
    }

    neotape::DetachedSignatureBytes const signature =
        neotape::sign_auth_nonce(seckey, nonce);
    REQUIRE(neotape::verify_auth_nonce_signature(signature, nonce, pubkey));

    nonce[0] ^= 0x5aU;
    REQUIRE(!neotape::verify_auth_nonce_signature(signature, nonce, pubkey));
}

TEST_CASE("signature: patch volume seq num finalizes deferred record",
          "[unit][signature]") {
    SignifyPublicKey const pubkey =
        neotape::load_signify_public_key(regress_pubkey);
    SignifySecretKey const seckey =
        neotape::load_signify_secret_key(regress_seckey);

    neotape::ContentFrameBuilder builder(
        4096, "00000000-0000-4000-8000-000000000123", "sig-test");
    builder.set_current_slice(0);

    std::array<std::byte, 32> payload{};
    for (size_t i = 0; i < payload.size(); ++i) {
        payload[i] = static_cast<std::byte>(i + 1);
    }

    auto flushed = builder.flush();
    REQUIRE(flushed.empty());

    auto frames = builder.feed(payload);
    REQUIRE(frames.empty());

    flushed = builder.flush();
    REQUIRE(flushed.size() == 1);
    auto &final_frame = flushed.front();

    FrameHeader const deferred = neotape::parse_fixed_header(
        reinterpret_cast<const uint8_t *>(final_frame.record.data()),
        final_frame.record.size());
    REQUIRE(deferred.volume_seq_num == 0);
    REQUIRE(std::all_of(deferred.frame_hash.begin(), deferred.frame_hash.end(),
                        [](uint8_t byte) { return byte == 0; }));
    REQUIRE(!neotape::has_frame_flag_signed(deferred.flags));

    neotape::patch_volume_seq_num(final_frame.record, 7, &seckey);

    FrameHeader const finalized = neotape::parse_fixed_header(
        reinterpret_cast<const uint8_t *>(final_frame.record.data()),
        final_frame.record.size());
    REQUIRE(finalized.volume_seq_num == 7);
    REQUIRE(neotape::has_frame_flag_signed(finalized.flags));
    REQUIRE(neotape::compute_frame_hash(
                reinterpret_cast<const uint8_t *>(final_frame.record.data()),
                final_frame.record.size()) == finalized.frame_hash);
    REQUIRE(neotape::verify_frame_hash_signature(finalized.signature,
                                                 finalized.frame_hash, pubkey));
}

} // namespace
