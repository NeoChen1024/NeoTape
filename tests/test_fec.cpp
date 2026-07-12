#include "neotape/fec.hpp"
#include "neotape/frame_builder.hpp"

#include <algorithm>
#include <cstdlib>
#include <exception>
#include <iostream>
#include <string>
#include <vector>

namespace {

using neotape::FecShard;
using std::string;

[[noreturn]] void fail(const string &message) {
    std::cerr << "test_fec: " << message << "\n";
    std::exit(1);
}

void expect(bool condition, const string &message) {
    if (!condition) {
        fail(message);
    }
}

template <typename Function>
void expect_throw(Function &&function, const string &message) {
    try {
        function();
    } catch (const std::exception &) {
        return;
    }
    fail(message);
}

std::vector<FecShard> make_sources(std::size_t count, std::size_t shard_size) {
    std::vector<FecShard> shards(count);
    for (std::size_t shard = 0; shard < count; ++shard) {
        shards[shard].resize(shard_size);
        for (std::size_t byte = 0; byte < shard_size; ++byte) {
            shards[shard][byte] =
                static_cast<std::byte>((shard * 37 + byte * 13) % 251);
        }
    }
    return shards;
}

uint8_t gf_multiply(uint8_t left, uint8_t right) {
    uint8_t result = 0;
    for (int bit = 0; bit < 8; ++bit) {
        if ((right & 1U) != 0) {
            result ^= left;
        }
        bool const high = (left & 0x80U) != 0;
        left = static_cast<uint8_t>(left << 1);
        if (high) {
            left ^= 0x1dU;
        }
        right = static_cast<uint8_t>(right >> 1);
    }
    return result;
}

uint8_t gf_power(uint8_t value, unsigned exponent) {
    uint8_t result = 1;
    while (exponent-- != 0) {
        result = gf_multiply(result, value);
    }
    return result;
}

neotape::Hash source_hash(const std::vector<FecShard> &shards,
                          uint64_t source_stream_size) {
    std::vector<uint8_t> stream;
    uint64_t remaining = source_stream_size;
    for (const FecShard &shard : shards) {
        std::size_t const count = static_cast<std::size_t>(
            std::min<uint64_t>(remaining, shard.size()));
        const auto *bytes = reinterpret_cast<const uint8_t *>(shard.data());
        stream.insert(stream.end(), bytes, bytes + count);
        remaining -= count;
        if (remaining == 0) {
            break;
        }
    }
    return neotape::blake3_hash(stream.data(), stream.size());
}

void test_descriptor_round_trip() {
    neotape::FecDescriptor descriptor;
    descriptor.source_content_frame_start = 81;
    descriptor.source_frame_count = 17;
    descriptor.repair_index = 3;
    descriptor.source_stream_size = 16 * 4096 + 123;
    descriptor.fec_group_blake3[0] = 0xa5;
    descriptor.fec_group_blake3[31] = 0x5a;

    neotape::SidebandBytes const bytes =
        neotape::serialize_fec_descriptor(descriptor);
    expect(bytes[0] == 1 && bytes[1] == 1, "bad descriptor prefix");
    expect(bytes[56] == 0 && bytes[127] == 0,
           "descriptor reserved bytes must be zero");

    neotape::FecDescriptor const parsed = neotape::parse_fec_descriptor(bytes);
    expect(parsed.source_content_frame_start == 81,
           "source frame start did not round trip");
    expect(parsed.source_frame_count == 17 && parsed.repair_index == 3,
           "shard counts did not round trip");
    expect(parsed.source_stream_size == descriptor.source_stream_size,
           "source stream size did not round trip");
    expect(parsed.fec_group_blake3 == descriptor.fec_group_blake3,
           "group hash did not round trip");
    neotape::validate_fec_descriptor(parsed, 4096);

    neotape::SidebandBytes corrupt = bytes;
    corrupt[127] = 1;
    expect_throw([&] { neotape::parse_fec_descriptor(corrupt); },
                 "nonzero descriptor reserved byte should fail");
}

void test_encode_coefficients() {
    constexpr std::size_t shard_size = 4096;
    std::vector<FecShard> sources = make_sources(2, shard_size);
    neotape::FecRepairShards const repair =
        neotape::encode_rs_32_4(sources, shard_size);

    // Repair row 0 consists entirely of coefficient 1.
    for (std::size_t byte = 0; byte < shard_size; ++byte) {
        expect(repair[0][byte] == (sources[0][byte] ^ sources[1][byte]),
               "repair row zero does not match XOR coefficients");
    }
    expect(repair[0].size() == shard_size && repair[3].size() == shard_size,
           "repair shard size mismatch");

    for (std::size_t row = 0; row < neotape::fec_repair_shards; ++row) {
        for (std::size_t byte : {0U, 17U, 1023U, 4095U}) {
            uint8_t expected = 0;
            for (std::size_t shard = 0; shard < sources.size(); ++shard) {
                uint8_t const coefficient =
                    gf_power(0x02, static_cast<unsigned>(row * shard));
                expected ^= gf_multiply(
                    coefficient, static_cast<uint8_t>(sources[shard][byte]));
            }
            expect(static_cast<uint8_t>(repair[row][byte]) == expected,
                   "repair bytes do not match normative coefficients");
        }
    }
}

void test_recover_four_missing_real_shards() {
    constexpr std::size_t shard_size = 4096;
    constexpr uint16_t source_count = 17;
    uint64_t const stream_size = 16 * shard_size + 777;
    std::vector<FecShard> sources = make_sources(source_count, shard_size);
    neotape::FecRepairShards repair =
        neotape::encode_rs_32_4(sources, shard_size);

    neotape::FecAvailableShards available;
    for (std::size_t i = 0; i < source_count; ++i) {
        available[i] = sources[i];
    }
    for (std::size_t i = 0; i < neotape::fec_repair_shards; ++i) {
        available[neotape::fec_data_shards + i] = std::move(repair[i]);
    }
    available[0].reset();
    available[3].reset();
    available[9].reset();
    available[16].reset();

    std::vector<FecShard> const recovered = neotape::recover_rs_32_4(
        std::move(available), source_count, stream_size, shard_size,
        source_hash(sources, stream_size));
    expect(recovered == sources,
           "four missing source shards were not recovered");
}

void test_recovery_rejects_wrong_commitment() {
    constexpr std::size_t shard_size = 4096;
    std::vector<FecShard> sources = make_sources(1, shard_size);
    neotape::FecRepairShards repair =
        neotape::encode_rs_32_4(sources, shard_size);
    neotape::FecAvailableShards available;
    for (std::size_t i = 0; i < neotape::fec_repair_shards; ++i) {
        available[neotape::fec_data_shards + i] = std::move(repair[i]);
    }
    neotape::Hash wrong_hash{};
    expect_throw(
        [&] {
            neotape::recover_rs_32_4(std::move(available), 1, shard_size,
                                     shard_size, wrong_hash);
        },
        "recovery must reject a wrong group commitment");
}

void test_fec_frame_builder_layout() {
    constexpr std::size_t block_size = 4096;
    constexpr std::size_t capacity = block_size - neotape::fixed_header_size;
    std::vector<std::byte> payload(capacity * 33 + 123);
    for (std::size_t i = 0; i < payload.size(); ++i) {
        payload[i] = static_cast<std::byte>(i % 251);
    }

    neotape::ContentFrameBuilder builder(
        block_size, "00000000-0000-4000-8000-000000000123", "fec-test", true);
    builder.set_current_slice(0);
    std::vector<neotape::BuiltFrame> frames = builder.feed(payload);
    expect(frames.size() == 36,
           "first full 32C+4F group should be emitted together");
    auto tail = builder.flush();
    frames.insert(frames.end(), std::make_move_iterator(tail.begin()),
                  std::make_move_iterator(tail.end()));
    expect(frames.size() == 42, "expected 32C+4F then 2C+4F");

    for (std::size_t i = 0; i < frames.size(); ++i) {
        neotape::FrameHeader const header = neotape::parse_fixed_header(
            reinterpret_cast<const uint8_t *>(frames[i].record.data()),
            frames[i].record.size());
        expect(header.global_frame_seq_num == i,
               "FEC layout global sequence is not gapless");
        bool const first_repair = i >= 32 && i < 36;
        bool const final_repair = i >= 38;
        if (first_repair || final_repair) {
            expect(header.channel_type == neotape::ChannelType::CH_FEC,
                   "repair position is not ch_fec");
            expect(neotape::has_frame_flag_sideband(header.flags),
                   "ch_fec frame is missing SIDEBAND");
            neotape::FecDescriptor const descriptor =
                neotape::parse_fec_descriptor(header.sideband_data);
            neotape::validate_fec_descriptor(descriptor, capacity);
            expect(descriptor.source_frame_count == (first_repair ? 32 : 2),
                   "FEC descriptor source count mismatch");
            expect(descriptor.repair_index == (first_repair ? i - 32 : i - 38),
                   "FEC repair index mismatch");
            expect(header.frame_payload_size == capacity,
                   "repair payload must fill the record");
        } else {
            expect(header.channel_type == neotape::ChannelType::CH_CONTENT,
                   "content position is not ch_content");
            expect(neotape::has_frame_flag_fec_protected(header.flags),
                   "protected content is missing FEC_PROTECTED");
        }
    }

    neotape::FrameHeader const final_content = neotape::parse_fixed_header(
        reinterpret_cast<const uint8_t *>(frames[37].record.data()),
        frames[37].record.size());
    neotape::FrameHeader const final_fec = neotape::parse_fixed_header(
        reinterpret_cast<const uint8_t *>(frames.back().record.data()),
        frames.back().record.size());
    expect(neotape::has_frame_flag_end(final_content.flags),
           "final content channel frame must carry END");
    expect(neotape::has_frame_flag_end(final_fec.flags),
           "final FEC channel frame must carry END");
}

} // namespace

int main() {
    test_descriptor_round_trip();
    test_encode_coefficients();
    test_recover_four_missing_real_shards();
    test_recovery_rejects_wrong_commitment();
    test_fec_frame_builder_layout();
    std::cout << "test_fec: ok\n";
}
