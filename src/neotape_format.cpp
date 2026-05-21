#include "neotape/format.hpp"

#include <blake3.h>
#include <crc32c/crc32c.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <cstring>
#include <ctime>
#include <format>
#include <random>
#include <stdexcept>

namespace neotape {

ParsedHeader parse_fixed_header(const uint8_t *data, std::size_t size) {
    check_common(data, size);

    ParsedHeader parsed;
    parsed.version = data[com_header_version];
    parsed.type = static_cast<HeaderType>(data[com_header_type]);
    parsed.stored_crc32c = detail::get_u32(data, hdr_crc32c);
    parsed.computed_crc32c = crc32c::Crc32c(data, hdr_crc32c);
    if (parsed.stored_crc32c != parsed.computed_crc32c)
        throw std::runtime_error("header CRC32C mismatch");

    switch (parsed.type) {
    case HeaderType::medium:
        parsed.medium = parse_medium(data);
        break;
    case HeaderType::volume:
        parsed.volume = parse_volume(data);
        break;
    case HeaderType::frame:
        parsed.frame = parse_frame(data);
        break;
    case HeaderType::archive_end:
        parsed.archive_end = parse_archive_end(data);
        break;
    default:
        throw std::runtime_error("unsupported header type");
    }
    return parsed;
}

std::string hash_hex(const Hash &hash) {
    std::string hex;
    for (uint8_t byte : hash)
        hex += std::format("{:02x}", static_cast<unsigned>(byte));
    return hex;
}

Hash blake3_hash(const uint8_t *data, std::size_t size) {
    Hash hash{};
    blake3_hasher hasher;
    blake3_hasher_init(&hasher);
    blake3_hasher_update(&hasher, data, size);
    blake3_hasher_finalize(&hasher, hash.data(), hash.size());
    return hash;
}

std::string utc_timestamp_now() {
    std::time_t now =
        std::chrono::system_clock::to_time_t(std::chrono::system_clock::now());
    std::tm utc{};
    if (gmtime_r(&now, &utc) == nullptr)
        throw std::runtime_error("gmtime_r failed");

    std::array<char, 20> buffer{};
    if (std::strftime(buffer.data(), buffer.size(), "%Y-%m-%dT%H:%M:%S",
                      &utc) != 19)
        throw std::runtime_error("strftime failed");
    return std::string(buffer.data(), 19);
}

std::string make_uuid_v4() {
    std::array<uint8_t, 16> bytes{};
    std::random_device rd;
    for (uint8_t &byte : bytes)
        byte = static_cast<uint8_t>(rd());
    bytes[6] = static_cast<uint8_t>((bytes[6] & 0x0fu) | 0x40u);
    bytes[8] = static_cast<uint8_t>((bytes[8] & 0x3fu) | 0x80u);
    return std::format("{:02x}{:02x}{:02x}{:02x}-{:02x}{:02x}-{:02x}{:02x}-"
                       "{:02x}{:02x}-{:02x}{:02x}{:02x}{:02x}{:02x}{:02x}",
                       bytes[0], bytes[1], bytes[2], bytes[3], bytes[4],
                       bytes[5], bytes[6], bytes[7], bytes[8], bytes[9],
                       bytes[10], bytes[11], bytes[12], bytes[13], bytes[14],
                       bytes[15]);
}

bool valid_block_size(uint32_t block_size) {
    return block_size >= min_block_size && block_size <= max_block_size;
}

} // namespace neotape
