#include "neotape/fec.hpp"

#include <erasure_code.h>

#include <algorithm>
#include <array>
#include <climits>
#include <cstring>
#include <format>
#include <limits>
#include <stdexcept>

namespace neotape {

namespace {

using Matrix = std::array<uint8_t, fec_data_shards * fec_data_shards>;
using GeneratorMatrix = std::array<uint8_t, fec_total_shards * fec_data_shards>;

void put_u16(SidebandBytes &bytes, std::size_t offset, uint16_t value) {
    bytes[offset] = static_cast<uint8_t>(value);
    bytes[offset + 1] = static_cast<uint8_t>(value >> 8);
}

void put_u64(SidebandBytes &bytes, std::size_t offset, uint64_t value) {
    for (std::size_t i = 0; i < 8; ++i) {
        bytes[offset + i] = static_cast<uint8_t>(value >> (i * 8));
    }
}

uint16_t get_u16(const SidebandBytes &bytes, std::size_t offset) {
    return static_cast<uint16_t>(bytes[offset]) |
           static_cast<uint16_t>(bytes[offset + 1]) << 8;
}

uint64_t get_u64(const SidebandBytes &bytes, std::size_t offset) {
    uint64_t value = 0;
    for (std::size_t i = 0; i < 8; ++i) {
        value |= static_cast<uint64_t>(bytes[offset + i]) << (i * 8);
    }
    return value;
}

GeneratorMatrix generator_matrix() {
    GeneratorMatrix matrix{};
    gf_gen_rs_matrix(matrix.data(), static_cast<int>(fec_total_shards),
                     static_cast<int>(fec_data_shards));
    return matrix;
}

std::vector<uint8_t> select_decode_rows(const GeneratorMatrix &generator,
                                        const FecAvailableShards &available) {
    Matrix echelon{};
    std::array<bool, fec_data_shards> pivot_used{};
    std::vector<uint8_t> selected;

    for (std::size_t position = 0; position < fec_total_shards; ++position) {
        if (!available[position].has_value()) {
            continue;
        }

        std::array<uint8_t, fec_data_shards> row{};
        std::copy_n(generator.data() + position * fec_data_shards,
                    fec_data_shards, row.data());
        for (std::size_t pivot = 0; pivot < fec_data_shards; ++pivot) {
            if (row[pivot] == 0) {
                continue;
            }
            if (pivot_used[pivot]) {
                uint8_t const factor = row[pivot];
                for (std::size_t col = pivot; col < fec_data_shards; ++col) {
                    row[col] ^=
                        gf_mul(factor, echelon[pivot * fec_data_shards + col]);
                }
                continue;
            }

            uint8_t const inverse = gf_inv(row[pivot]);
            for (std::size_t col = pivot; col < fec_data_shards; ++col) {
                row[col] = gf_mul(row[col], inverse);
                echelon[pivot * fec_data_shards + col] = row[col];
            }
            pivot_used[pivot] = true;
            selected.push_back(static_cast<uint8_t>(position));
            break;
        }
        if (selected.size() == fec_data_shards) {
            return selected;
        }
    }
    throw std::runtime_error("not enough independent shards for FEC recovery");
}

Hash source_hash(const std::vector<FecShard> &shards, uint64_t stream_size) {
    if (stream_size > std::numeric_limits<std::size_t>::max()) {
        throw std::runtime_error("FEC source stream is too large");
    }
    std::vector<uint8_t> stream;
    stream.reserve(static_cast<std::size_t>(stream_size));
    uint64_t remaining = stream_size;
    for (const FecShard &shard : shards) {
        std::size_t const count = static_cast<std::size_t>(
            std::min<uint64_t>(remaining, shard.size()));
        const auto *begin = reinterpret_cast<const uint8_t *>(shard.data());
        stream.insert(stream.end(), begin, begin + count);
        remaining -= count;
        if (remaining == 0) {
            break;
        }
    }
    if (remaining != 0) {
        throw std::runtime_error("FEC source stream exceeds real shards");
    }
    return blake3_hash(stream.data(), stream.size());
}

} // namespace

SidebandBytes serialize_fec_descriptor(const FecDescriptor &descriptor) {
    SidebandBytes bytes{};
    bytes[0] = descriptor.fec_version;
    bytes[1] = descriptor.fec_profile;
    put_u16(bytes, 2, descriptor.fec_flags);
    put_u64(bytes, 4, descriptor.source_content_frame_start);
    put_u16(bytes, 12, descriptor.source_frame_count);
    put_u16(bytes, 14, descriptor.repair_index);
    put_u64(bytes, 16, descriptor.source_stream_size);
    std::copy(descriptor.fec_group_blake3.begin(),
              descriptor.fec_group_blake3.end(), bytes.begin() + 24);
    return bytes;
}

FecDescriptor parse_fec_descriptor(const SidebandBytes &bytes) {
    if (std::ranges::any_of(bytes.begin() + 56, bytes.end(),
                            [](uint8_t byte) { return byte != 0; })) {
        throw std::runtime_error("FEC descriptor reserved bytes must be zero");
    }
    FecDescriptor descriptor;
    descriptor.fec_version = bytes[0];
    descriptor.fec_profile = bytes[1];
    descriptor.fec_flags = get_u16(bytes, 2);
    descriptor.source_content_frame_start = get_u64(bytes, 4);
    descriptor.source_frame_count = get_u16(bytes, 12);
    descriptor.repair_index = get_u16(bytes, 14);
    descriptor.source_stream_size = get_u64(bytes, 16);
    std::copy(bytes.begin() + 24, bytes.begin() + 56,
              descriptor.fec_group_blake3.begin());
    return descriptor;
}

void validate_fec_descriptor(const FecDescriptor &descriptor,
                             uint32_t shard_size) {
    if (descriptor.fec_version != fec_descriptor_version) {
        throw std::runtime_error("unsupported FEC descriptor version");
    }
    if (descriptor.fec_profile != fec_profile_rs_32_4) {
        throw std::runtime_error("unsupported FEC profile");
    }
    if (descriptor.fec_flags != 0) {
        throw std::runtime_error("unsupported FEC descriptor flags");
    }
    if (descriptor.source_frame_count == 0 ||
        descriptor.source_frame_count > fec_data_shards) {
        throw std::runtime_error("FEC source_frame_count must be 1..32");
    }
    if (descriptor.repair_index >= fec_repair_shards) {
        throw std::runtime_error("FEC repair_index must be 0..3");
    }
    uint64_t const minimum =
        static_cast<uint64_t>(descriptor.source_frame_count - 1) * shard_size;
    uint64_t const maximum =
        static_cast<uint64_t>(descriptor.source_frame_count) * shard_size;
    if (descriptor.source_stream_size < minimum ||
        descriptor.source_stream_size > maximum) {
        throw std::runtime_error(
            "FEC source_stream_size is outside shard bounds");
    }
}

FecRepairShards encode_rs_32_4(std::span<const FecShard> source_shards,
                               std::size_t shard_size) {
    if (source_shards.empty() || source_shards.size() > fec_data_shards) {
        throw std::runtime_error("rs_32_4 requires 1..32 real source shards");
    }
    if (shard_size == 0 || shard_size > static_cast<std::size_t>(INT_MAX)) {
        throw std::runtime_error("invalid FEC shard size");
    }

    FecShard zero_shard(shard_size, std::byte{0});
    FecShard padded_final;
    std::array<unsigned char *, fec_data_shards> source_ptrs{};
    for (std::size_t i = 0; i < fec_data_shards; ++i) {
        if (i < source_shards.size() && source_shards[i].size() > shard_size) {
            throw std::runtime_error("FEC source shard exceeds shard size");
        }
        if (i >= source_shards.size()) {
            source_ptrs[i] =
                reinterpret_cast<unsigned char *>(zero_shard.data());
        } else if (source_shards[i].size() == shard_size) {
            source_ptrs[i] = const_cast<unsigned char *>(
                reinterpret_cast<const unsigned char *>(
                    source_shards[i].data()));
        } else {
            if (i + 1 != source_shards.size()) {
                throw std::runtime_error(
                    "only the final real FEC shard may be short");
            }
            padded_final.assign(shard_size, std::byte{0});
            std::copy(source_shards[i].begin(), source_shards[i].end(),
                      padded_final.begin());
            source_ptrs[i] =
                reinterpret_cast<unsigned char *>(padded_final.data());
        }
    }

    GeneratorMatrix const matrix = generator_matrix();
    std::array<unsigned char, 32 * fec_data_shards * fec_repair_shards>
        tables{};
    ec_init_tables(static_cast<int>(fec_data_shards),
                   static_cast<int>(fec_repair_shards),
                   const_cast<unsigned char *>(
                       matrix.data() + fec_data_shards * fec_data_shards),
                   tables.data());

    FecRepairShards repair;
    std::array<unsigned char *, fec_repair_shards> repair_ptrs{};
    for (std::size_t i = 0; i < fec_repair_shards; ++i) {
        repair[i].assign(shard_size, std::byte{0});
        repair_ptrs[i] = reinterpret_cast<unsigned char *>(repair[i].data());
    }
    ec_encode_data(static_cast<int>(shard_size),
                   static_cast<int>(fec_data_shards),
                   static_cast<int>(fec_repair_shards), tables.data(),
                   source_ptrs.data(), repair_ptrs.data());
    return repair;
}

std::vector<FecShard> recover_rs_32_4(FecAvailableShards available,
                                      uint16_t source_frame_count,
                                      uint64_t source_stream_size,
                                      std::size_t shard_size,
                                      const Hash &expected_hash) {
    FecDescriptor bounds;
    bounds.source_frame_count = source_frame_count;
    bounds.source_stream_size = source_stream_size;
    validate_fec_descriptor(bounds, static_cast<uint32_t>(shard_size));
    if (shard_size > static_cast<std::size_t>(INT_MAX)) {
        throw std::runtime_error("invalid FEC shard size");
    }

    for (std::size_t i = 0; i < fec_total_shards; ++i) {
        if (available[i].has_value() && available[i]->size() != shard_size) {
            throw std::runtime_error("available FEC shard has wrong size");
        }
    }
    for (std::size_t i = source_frame_count; i < fec_data_shards; ++i) {
        available[i] = FecShard(shard_size, std::byte{0});
    }

    GeneratorMatrix const generator = generator_matrix();
    std::vector<uint8_t> const selected =
        select_decode_rows(generator, available);
    Matrix basis{};
    Matrix inverse{};
    for (std::size_t row = 0; row < fec_data_shards; ++row) {
        std::copy_n(generator.data() + selected[row] * fec_data_shards,
                    fec_data_shards, basis.data() + row * fec_data_shards);
    }
    if (gf_invert_matrix(basis.data(), inverse.data(),
                         static_cast<int>(fec_data_shards)) < 0) {
        throw std::runtime_error("selected FEC decode basis is singular");
    }

    std::array<unsigned char *, fec_data_shards> basis_ptrs{};
    for (std::size_t i = 0; i < fec_data_shards; ++i) {
        basis_ptrs[i] =
            reinterpret_cast<unsigned char *>(available[selected[i]]->data());
    }

    std::vector<FecShard> result(source_frame_count);
    for (std::size_t data = 0; data < source_frame_count; ++data) {
        if (available[data].has_value()) {
            result[data] = *available[data];
            continue;
        }
        result[data].assign(shard_size, std::byte{0});
        std::array<unsigned char, 32 * fec_data_shards> tables{};
        ec_init_tables(static_cast<int>(fec_data_shards), 1,
                       inverse.data() + data * fec_data_shards, tables.data());
        unsigned char *output =
            reinterpret_cast<unsigned char *>(result[data].data());
        ec_encode_data(static_cast<int>(shard_size),
                       static_cast<int>(fec_data_shards), 1, tables.data(),
                       basis_ptrs.data(), &output);
    }

    if (source_hash(result, source_stream_size) != expected_hash) {
        throw std::runtime_error("reconstructed FEC group hash mismatch");
    }
    return result;
}

} // namespace neotape
