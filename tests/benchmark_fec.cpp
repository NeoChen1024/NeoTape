#include "neotape/common.hpp"
#include "neotape/fec.hpp"

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <format>
#include <getopt.h>
#include <iostream>
#include <limits>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace {

using neotape::FecAvailableShards;
using neotape::FecRepairShards;
using neotape::FecShard;
using std::format;
using std::string;
using Clock = std::chrono::steady_clock;
using Seconds = std::chrono::duration<double>;

constexpr std::size_t default_shard_size = 1024 * 1024;
constexpr std::size_t default_iterations = 64;
constexpr double bytes_per_mib = 1024.0 * 1024.0;

struct Options {
    std::size_t shard_size = default_shard_size;
    std::size_t iterations = default_iterations;
};

[[noreturn]] void fail(const string &message) {
    std::cerr << format("benchmark_fec: {}\n", message);
    std::exit(2);
}

void usage(const char *program) {
    std::cerr << format(
        "usage: {} [-s|--shard-size <SIZE>] [-n|--iterations <N>] [-h]\n"
        "SIZE accepts K, M, G, or T binary suffixes (default 1M).\n",
        program);
}

Options parse_args(int argc, char **argv) {
    static const struct option long_options[] = {
        {"shard-size", required_argument, nullptr, 's'},
        {"iterations", required_argument, nullptr, 'n'},
        {"help", no_argument, nullptr, 'h'},
        {nullptr, 0, nullptr, 0},
    };

    Options options;
    int option = 0;
    while ((option = getopt_long(argc, argv, "s:n:h", long_options, nullptr)) !=
           -1) {
        switch (option) {
        case 's':
            options.shard_size = static_cast<std::size_t>(
                neotape::parse_size(optarg, "shard size"));
            break;
        case 'n': {
            char *end = nullptr;
            unsigned long long const value = std::strtoull(optarg, &end, 10);
            if (end == optarg || *end != '\0' || value == 0 ||
                value > std::numeric_limits<std::size_t>::max()) {
                fail("iterations must be a positive integer");
            }
            options.iterations = static_cast<std::size_t>(value);
            break;
        }
        case 'h':
            usage(argv[0]);
            std::exit(0);
        case '?':
            std::exit(2);
        }
    }
    if (optind != argc) {
        fail("unexpected positional arguments");
    }
    return options;
}

std::vector<FecShard> make_sources(std::size_t shard_size) {
    std::vector<FecShard> sources(neotape::fec_data_shards);
    for (std::size_t shard = 0; shard < sources.size(); ++shard) {
        sources[shard].resize(shard_size);
        for (std::size_t byte = 0; byte < shard_size; ++byte) {
            sources[shard][byte] =
                static_cast<std::byte>((shard * 37 + byte * 13) % 251);
        }
    }
    return sources;
}

neotape::Hash source_hash(const std::vector<FecShard> &sources) {
    std::vector<uint8_t> stream;
    stream.reserve(sources.size() * sources.front().size());
    for (const FecShard &shard : sources) {
        const auto *bytes = reinterpret_cast<const uint8_t *>(shard.data());
        stream.insert(stream.end(), bytes, bytes + shard.size());
    }
    return neotape::blake3_hash(stream.data(), stream.size());
}

FecAvailableShards make_recovery_input(const std::vector<FecShard> &sources,
                                       const FecRepairShards &repair) {
    FecAvailableShards available;
    for (std::size_t i = 0; i < sources.size(); ++i) {
        available[i] = sources[i];
    }
    for (std::size_t i = 0; i < repair.size(); ++i) {
        available[neotape::fec_data_shards + i] = repair[i];
    }
    for (std::size_t missing : {0U, 7U, 19U, 31U}) {
        available[missing].reset();
    }
    return available;
}

double throughput_mib(std::size_t source_bytes, std::size_t iterations,
                      Seconds elapsed) {
    double const processed =
        static_cast<double>(source_bytes) * static_cast<double>(iterations);
    return processed / bytes_per_mib / elapsed.count();
}

} // namespace

int main(int argc, char **argv) {
    try {
        Options const options = parse_args(argc, argv);
        std::vector<FecShard> const sources = make_sources(options.shard_size);
        std::size_t const source_bytes = sources.size() * options.shard_size;
        uint64_t const source_stream_size = source_bytes;
        neotape::Hash const expected_hash = source_hash(sources);

        FecRepairShards baseline_repair =
            neotape::encode_rs_32_4(sources, options.shard_size);
        auto warmup = make_recovery_input(sources, baseline_repair);
        auto recovered = neotape::recover_rs_32_4(
            std::move(warmup), neotape::fec_data_shards, source_stream_size,
            options.shard_size, expected_hash);
        if (recovered != sources) {
            throw std::runtime_error("warm-up correction produced bad data");
        }

        Seconds generator_elapsed{0};
        uint64_t checksum = 0;
        for (std::size_t iteration = 0; iteration < options.iterations;
             ++iteration) {
            auto const start = Clock::now();
            FecRepairShards repair =
                neotape::encode_rs_32_4(sources, options.shard_size);
            generator_elapsed += Clock::now() - start;
            checksum +=
                static_cast<uint8_t>(repair[iteration % repair.size()]
                                           [iteration % options.shard_size]);
        }

        Seconds correction_elapsed{0};
        for (std::size_t iteration = 0; iteration < options.iterations;
             ++iteration) {
            // Fixture preparation is outside the timed recovery API call.
            auto available = make_recovery_input(sources, baseline_repair);
            auto const start = Clock::now();
            recovered = neotape::recover_rs_32_4(
                std::move(available), neotape::fec_data_shards,
                source_stream_size, options.shard_size, expected_hash);
            correction_elapsed += Clock::now() - start;
            checksum +=
                static_cast<uint8_t>(recovered[iteration % recovered.size()]
                                              [iteration % options.shard_size]);
        }
        if (recovered != sources) {
            throw std::runtime_error("correction produced bad data");
        }

        double const source_mib =
            static_cast<double>(source_bytes) / bytes_per_mib;
        double const total_mib = source_mib * options.iterations;
        Seconds const combined_elapsed = generator_elapsed + correction_elapsed;
        std::cout << format(
            "FEC profile:       rs_32_4 (32 data + 4 repair)\n"
            "Shard size:        {:.2f} MiB\n"
            "Source per group:  {:.2f} MiB\n"
            "Iterations:        {}\n"
            "Payload per phase: {:.2f} MiB\n"
            "Generator:         {:.2f} MiB/s ({:.2f} MiB in "
            "{:.3f} s)\n"
            "Correction (4):    {:.2f} MiB/s ({:.2f} MiB in "
            "{:.3f} s)\n"
            "Generate+correct:  {:.2f} MiB/s ({:.2f} MiB in "
            "{:.3f} s)\n"
            "Checksum:          {}\n",
            static_cast<double>(options.shard_size) / bytes_per_mib, source_mib,
            options.iterations, total_mib,
            throughput_mib(source_bytes, options.iterations, generator_elapsed),
            total_mib, generator_elapsed.count(),
            throughput_mib(source_bytes, options.iterations,
                           correction_elapsed),
            total_mib, correction_elapsed.count(),
            throughput_mib(source_bytes, options.iterations, combined_elapsed),
            total_mib, combined_elapsed.count(), checksum);
        return 0;
    } catch (const std::exception &error) {
        std::cerr << format("benchmark_fec: {}\n", error.what());
        return 1;
    }
}
