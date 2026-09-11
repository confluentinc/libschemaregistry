# mpdecimal (libmpdec / libmpdec++) via FetchContent.
#
# mpdecimal does not ship a CMake build, so this configures one. It is the correctly-rounded
# arbitrary-precision decimal library that backs CPython's `decimal` module, and is used here
# as the backing type for the CEL Decimal functions (matching the other clients' 38-digit
# HALF_UP semantics exactly).
#
# Adapted from https://github.com/thefourthway/mpdec-cmake (BSD-2-Clause). Builds static
# libraries only; libmpdec is compiled with position-independent code. Assembly is disabled on
# Windows (portable C), so this works uniformly on Linux/macOS/Windows and on x86-64/arm64.

include(FetchContent)

FetchContent_Declare(
  mpdecimal
  URL https://www.bytereef.org/software/mpdecimal/releases/mpdecimal-4.0.1.tar.gz
  URL_HASH SHA256=96d33abb4bb0070c7be0fed4246cd38416188325f820468214471938545b1ac8
  DOWNLOAD_EXTRACT_TIMESTAMP TRUE
)

FetchContent_MakeAvailable(mpdecimal)

set(MPDEC_SOURCE_DIR ${mpdecimal_SOURCE_DIR})
set(MPDEC_BINARY_DIR ${mpdecimal_BINARY_DIR})

enable_language(C)
enable_language(CXX)

include(CheckIncludeFile)
include(CheckCSourceCompiles)
include(TestBigEndian)

check_include_file("stdint.h" HAVE_STDINT_H)
check_include_file("inttypes.h" HAVE_INTTYPES_H)
check_include_file("sys/types.h" HAVE_SYS_TYPES_H)
test_big_endian(WORDS_BIGENDIAN)

check_c_source_compiles("
#include <stdint.h>
int main() {
    __uint128_t x = 1;
    __int128_t y = 1;
    return 0;
}
" HAVE_UINT128_T)

check_c_source_compiles("
#include <stdint.h>
int main() {
    typedef unsigned __int128 uint128_t;
    typedef __int128 int128_t;
    uint128_t a = 1, b = 2;
    uint128_t c = a * b;
    return 0;
}
" HAVE_GCC_UINT128_T)

# The @MPD_HEADER_CONFIG@ placeholder must only set the SELECTOR macro (MPD_CONFIG_64 /
# MPD_CONFIG_32) plus endianness — mpdecimal.h.in's own `#if defined(MPD_CONFIG_64)` block
# defines the coefficient typedefs and MPD_*_MAX macros. (Injecting those typedefs here, as the
# upstream mpdec-cmake module does, redefines mpd_size_t and fails to compile on 4.0.1.)
if(CMAKE_SIZEOF_VOID_P EQUAL 8)
  set(MPD_HEADER_CONFIG "#define MPD_CONFIG_64 1")
else()
  set(MPD_HEADER_CONFIG "#define MPD_CONFIG_32 1")
endif()

if(WORDS_BIGENDIAN)
  string(APPEND MPD_HEADER_CONFIG "
#define MPD_BIGENDIAN 1
#define MPD_LITTLEENDIAN 0
")
else()
  string(APPEND MPD_HEADER_CONFIG "
#define MPD_BIGENDIAN 0
#define MPD_LITTLEENDIAN 1
")
endif()

set(MPDEC_GEN_DIR "${MPDEC_BINARY_DIR}/libmpdec")
file(MAKE_DIRECTORY "${MPDEC_GEN_DIR}")

# mpdecimal.h.in carries a bare @MPD_CONFIG@ token in addition to @MPD_HEADER_CONFIG@;
# upstream leaves it empty, so define it explicitly rather than rely on an unset variable.
set(MPD_CONFIG "")
file(READ "${MPDEC_SOURCE_DIR}/libmpdec/mpdecimal.h.in" MPDECIMAL_H_CONTENT)
string(REPLACE "@MPD_CONFIG@"        "${MPD_CONFIG}"        MPDECIMAL_H_CONTENT "${MPDECIMAL_H_CONTENT}")
string(REPLACE "@MPD_HEADER_CONFIG@" "${MPD_HEADER_CONFIG}" MPDECIMAL_H_CONTENT "${MPDECIMAL_H_CONTENT}")
file(WRITE "${MPDEC_GEN_DIR}/mpdecimal.h" "${MPDECIMAL_H_CONTENT}")

set(MPDEC_SOURCES
  ${MPDEC_SOURCE_DIR}/libmpdec/basearith.c
  ${MPDEC_SOURCE_DIR}/libmpdec/constants.c
  ${MPDEC_SOURCE_DIR}/libmpdec/context.c
  ${MPDEC_SOURCE_DIR}/libmpdec/convolute.c
  ${MPDEC_SOURCE_DIR}/libmpdec/crt.c
  ${MPDEC_SOURCE_DIR}/libmpdec/difradix2.c
  ${MPDEC_SOURCE_DIR}/libmpdec/fnt.c
  ${MPDEC_SOURCE_DIR}/libmpdec/fourstep.c
  ${MPDEC_SOURCE_DIR}/libmpdec/io.c
  ${MPDEC_SOURCE_DIR}/libmpdec/mpalloc.c
  ${MPDEC_SOURCE_DIR}/libmpdec/mpdecimal.c
  ${MPDEC_SOURCE_DIR}/libmpdec/numbertheory.c
  ${MPDEC_SOURCE_DIR}/libmpdec/sixstep.c
  ${MPDEC_SOURCE_DIR}/libmpdec/transpose.c
)

set(MPDEC_HEADERS
  ${MPDEC_GEN_DIR}/mpdecimal.h
  ${MPDEC_SOURCE_DIR}/libmpdec/basearith.h
  ${MPDEC_SOURCE_DIR}/libmpdec/bits.h
  ${MPDEC_SOURCE_DIR}/libmpdec/constants.h
  ${MPDEC_SOURCE_DIR}/libmpdec/convolute.h
  ${MPDEC_SOURCE_DIR}/libmpdec/crt.h
  ${MPDEC_SOURCE_DIR}/libmpdec/difradix2.h
  ${MPDEC_SOURCE_DIR}/libmpdec/fnt.h
  ${MPDEC_SOURCE_DIR}/libmpdec/fourstep.h
  ${MPDEC_SOURCE_DIR}/libmpdec/numbertheory.h
  ${MPDEC_SOURCE_DIR}/libmpdec/sixstep.h
  ${MPDEC_SOURCE_DIR}/libmpdec/transpose.h
  ${MPDEC_SOURCE_DIR}/libmpdec/typearith.h
  ${MPDEC_SOURCE_DIR}/libmpdec/umodarith.h
)

add_library(mpdec STATIC ${MPDEC_HEADERS} ${MPDEC_SOURCES})

# The source dir holds libmpdec's internal headers and must stay private: it ships an
# io.h that would otherwise precede the CRT's <io.h> for every consumer on Windows.
# Only the generated mpdecimal.h is public.
target_include_directories(mpdec
  PUBLIC "$<BUILD_INTERFACE:${MPDEC_GEN_DIR}>"
  PRIVATE "$<BUILD_INTERFACE:${MPDEC_SOURCE_DIR}/libmpdec>"
)

target_compile_definitions(mpdec PRIVATE ANSI=1)

if(CMAKE_SIZEOF_VOID_P EQUAL 8)
  target_compile_definitions(mpdec PRIVATE CONFIG_64=1)
else()
  target_compile_definitions(mpdec PRIVATE CONFIG_32=1)
endif()

if(HAVE_UINT128_T OR HAVE_GCC_UINT128_T)
  target_compile_definitions(mpdec PUBLIC HAVE_UINT128_T=1)
endif()

set_target_properties(mpdec PROPERTIES POSITION_INDEPENDENT_CODE ON)
target_compile_features(mpdec PRIVATE c_std_99)
# Suppress mpdecimal's own warnings (upstream code, not ours).
if(MSVC)
  target_compile_options(mpdec PRIVATE /w)
else()
  target_compile_options(mpdec PRIVATE -w)
endif()

add_library(mpdecxx STATIC "${MPDEC_SOURCE_DIR}/libmpdec++/decimal.cc")
target_link_libraries(mpdecxx PUBLIC mpdec)
target_compile_features(mpdecxx PUBLIC cxx_std_11)
set_target_properties(mpdecxx PROPERTIES POSITION_INDEPENDENT_CODE ON)

target_include_directories(mpdecxx
  PUBLIC
    "$<BUILD_INTERFACE:${MPDEC_GEN_DIR}>"
    "$<BUILD_INTERFACE:${MPDEC_SOURCE_DIR}/libmpdec++>"
  PRIVATE
    "$<BUILD_INTERFACE:${MPDEC_SOURCE_DIR}/libmpdec>"
)

if(MSVC)
  # decimal.hh picks dllimport whenever _DLL is defined, which MSVC sets for the dynamic CRT
  # (/MD) - so a *static* mpdecxx is still declared dllimport and every consumer emits __imp_
  # references that no import library provides. BUILD_LIBMPDECXX selects the dllexport branch
  # instead, which references the static library directly. PUBLIC because consumers include
  # decimal.hh and have to agree with how decimal.cc was compiled. The C header carries no
  # MSVC import/export logic, so libmpdec needs nothing here.
  target_compile_definitions(mpdecxx PUBLIC BUILD_LIBMPDECXX)
endif()

add_library(mpdec::mpdec ALIAS mpdec)
add_library(mpdecxx::mpdecxx ALIAS mpdecxx)
