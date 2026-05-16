/* Generated for NeoTape build (not using CMake). */
#ifndef CRC32C_CRC32C_CONFIG_H_
#define CRC32C_CRC32C_CONFIG_H_

#define BYTE_ORDER_BIG_ENDIAN 0
#define HAVE_BUILTIN_PREFETCH 1

#if defined(__x86_64__) || defined(_M_X64)
#define HAVE_MM_PREFETCH 1
#define HAVE_SSE42 1
#else
#define HAVE_MM_PREFETCH 0
#define HAVE_SSE42 0
#endif

#if defined(__aarch64__) || defined(_M_ARM64)
#define HAVE_ARM64_CRC32C 1
#else
#define HAVE_ARM64_CRC32C 0
#endif

#if defined(__linux__) && defined(__aarch64__)
#define HAVE_STRONG_GETAUXVAL 1
#else
#define HAVE_STRONG_GETAUXVAL 0
#endif

#define HAVE_WEAK_GETAUXVAL 0
#define CRC32C_TESTS_BUILT_WITH_GLOG 0

#endif /* CRC32C_CRC32C_CONFIG_H_ */
