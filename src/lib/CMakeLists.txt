cmake_minimum_required(VERSION 3.17)
project(nimble_engine_client C)

set(CMAKE_C_STANDARD 99)
set(CMAKE_C_FLAGS "-std=c99")

if (CMAKE_BUILD_TYPE STREQUAL "Debug")
    set(isDebug TRUE)
else ()
    message("no debug")
    set(isDebug FALSE)
endif ()


if (isDebug)
    message("Debug build detected")
    add_compile_definitions(CONFIGURATION_DEBUG TYRAN_CONFIGURATION_DEBUG)
    set(CONFIGURATION_DEBUG 1)
endif ()


set(DEPS ../../deps/)

file(GLOB lib_src FOLLOW_SYMLINKS
        "*.c"
        )

add_library(nimble_engine_client STATIC
        ${lib_src}
        )

target_compile_options(nimble_engine_client PRIVATE -Wall -Wextra -Wshadow -Wstrict-aliasing -ansi -pedantic -Wno-declaration-after-statement -Wno-extra-semi-stmt -Wno-undef -Wno-unused-variable -Wno-unused-parameter -Wno-padded -Werror=implicit-function-declaration -Werror=incompatible-pointer-types -Werror=missing-prototypes -Werror=int-conversion -Werror=return-type -Werror=incompatible-function-pointer-types) # -Werror=incompatible-pointer-types-discards-qualifiers
if (COMPILER_CLANG)
    target_compile_options(nimble_engine_client PRIVATE -Wmost -Weverything -Werror=missing-variable-declarations)
endif ()

target_include_directories(nimble_engine_client PUBLIC ${DEPS}piot/clog/src/include)
target_include_directories(nimble_engine_client PUBLIC ${DEPS}piot/tiny-libc/src/include)
target_include_directories(nimble_engine_client PUBLIC ${DEPS}piot/transmute-c/src/include)
target_include_directories(nimble_engine_client PUBLIC ${DEPS}piot/nimble-steps-serialize-c/src/include)
target_include_directories(nimble_engine_client PUBLIC ${DEPS}piot/nimble-serialize-c/src/include)
target_include_directories(nimble_engine_client PUBLIC ${DEPS}piot/nimble-steps-c/src/include)
target_include_directories(nimble_engine_client PUBLIC ${DEPS}piot/discoid-c/src/include)
target_include_directories(nimble_engine_client PUBLIC ${DEPS}piot/nimble-client-c/src/include)
target_include_directories(nimble_engine_client PUBLIC ${DEPS}piot/blob-stream/src/include)
target_include_directories(nimble_engine_client PUBLIC ${DEPS}piot/bit-array/src/include)
target_include_directories(nimble_engine_client PUBLIC ${DEPS}piot/flood-c/src/include)
target_include_directories(nimble_engine_client PUBLIC ${DEPS}piot/monotonic-time-c/src/include)
target_include_directories(nimble_engine_client PUBLIC ${DEPS}piot/ordered-datagram-c/src/include)
target_include_directories(nimble_engine_client PUBLIC ${DEPS}piot/stats-c/src/include)
target_include_directories(nimble_engine_client PUBLIC ${DEPS}piot/udp-transport/src/include)
target_include_directories(nimble_engine_client PUBLIC ${DEPS}piot/rectify-c/src/include)
target_include_directories(nimble_engine_client PUBLIC ${DEPS}piot/assent-c/src/include)
target_include_directories(nimble_engine_client PUBLIC ${DEPS}piot/seer-c/src/include)
target_include_directories(nimble_engine_client PUBLIC ${DEPS}piot/lagometer-c/src/include)

target_include_directories(nimble_engine_client PUBLIC ../include)

