#pragma once

#include "neotape/format.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <vector>

namespace neotape {

inline constexpr uint8_t fec_descriptor_version = 1;
inline constexpr uint8_t fec_profile_rs_32_4 = 1;
inline constexpr std::size_t fec_data_shards = 32;
inline constexpr std::size_t fec_repair_shards = 4;
inline constexpr std::size_t fec_total_shards =
    fec_data_shards + fec_repair_shards;

struct FecDescriptor {
    uint8_t fec_version = fec_descriptor_version;
    uint8_t fec_profile = fec_profile_rs_32_4;
    uint16_t fec_flags = 0;
    uint64_t source_content_frame_start = 0;
    uint16_t source_frame_count = 0;
    uint16_t repair_index = 0;
    uint64_t source_stream_size = 0;
    Hash fec_group_blake3{};
};

SidebandBytes serialize_fec_descriptor(const FecDescriptor &descriptor);
FecDescriptor parse_fec_descriptor(const SidebandBytes &bytes);
void validate_fec_descriptor(const FecDescriptor &descriptor,
                             uint32_t shard_size);

using FecShard = std::vector<std::byte>;
using FecRepairShards = std::array<FecShard, fec_repair_shards>;
using FecAvailableShards =
    std::array<std::optional<FecShard>, fec_total_shards>;

// Encode 1..32 real data shards. Short shards and unused data positions are
// zero-filled to shard_size exactly as specified by rs_32_4.
FecRepairShards encode_rs_32_4(std::span<const FecShard> source_shards,
                               std::size_t shard_size);

// Recover all real data shards after treating absent/corrupt frames as
// unavailable. Synthetic positions source_frame_count..31 are always known
// zero shards. The reconstructed concatenation is accepted only when its
// group hash matches expected_hash.
std::vector<FecShard> recover_rs_32_4(FecAvailableShards available,
                                      uint16_t source_frame_count,
                                      uint64_t source_stream_size,
                                      std::size_t shard_size,
                                      const Hash &expected_hash);

} // namespace neotape
