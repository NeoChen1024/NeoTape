set(_signify_dir "${PROJECT_SOURCE_DIR}/3rdparty/signify")

add_library(neotape_signify STATIC
    "${_signify_dir}/base64.c"
    "${_signify_dir}/bcrypt_pbkdf.c"
    "${_signify_dir}/blowfish.c"
    "${_signify_dir}/crypto_api.c"
    "${_signify_dir}/explicit_bzero.c"
    "${_signify_dir}/fe25519.c"
    "${_signify_dir}/libbsd/readpassphrase.c"
    "${_signify_dir}/mod_ed25519.c"
    "${_signify_dir}/mod_ge25519.c"
    "${_signify_dir}/sc25519.c"
    "${_signify_dir}/sha2.c"
    "${_signify_dir}/timingsafe_bcmp.c"
)
add_library(NeoTape::Signify ALIAS neotape_signify)
target_include_directories(neotape_signify PUBLIC "${_signify_dir}")
target_include_directories(neotape_signify SYSTEM PRIVATE "${_signify_dir}/libbsd/bsd")
target_compile_definitions(neotape_signify PRIVATE
    LIBBSD_OVERLAY
    BUNDLED_BZERO
    typeof=__typeof__
    b64_ntop=neotape_b64_ntop
    b64_pton=neotape_b64_pton
    __b64_ntop=neotape_b64_ntop
    __b64_pton=neotape_b64_pton
)
target_compile_options(neotape_signify PRIVATE
    "-include${_signify_dir}/compat.h"
)

unset(_signify_dir)
