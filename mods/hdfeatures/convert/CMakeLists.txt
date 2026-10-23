# hdf_convert: the HD Features converter (plan research/dropin/plan.md, sections 2.2, 3 and 4).
#
# A pure C++23 static library with no SDK and no game headers. It is NOT built with the mod's flags: add_mod's
# FEATURES game force-includes the game's global.h (M_PI as a float, ROUND/SQUARE/DEBUG macros). Builds:
#   - on its own (no Dusklight checkout needed), for the developer CLI and the tests:
#       cmake -S mods/hdfeatures/convert -B build-conv -G Ninja -DCMAKE_BUILD_TYPE=Release
#       cmake --build build-conv
#   - through add_subdirectory(convert) from mods/hdfeatures/CMakeLists.txt (wave 3, R1), which then links
#     hdf_convert into mod.dll. The root project must then enable C as well (project(... LANGUAGES C CXX)).
#
# Targets:
#   hdf_third_party  zlib 1.3.2 + zstd 1.5.7 decoder (third_party/, vendored, /W0)
#   hdf_convert      the library (/fp:precise, /W4; never /fp:fast or /arch:AVX2)
#   hdf-convert      developer CLI + unit tests (HDF_BUILD_CLI, default ON when standalone). Never packaged.
#
# Module sources are globbed per folder (src/<module>/*.cpp, cli/oracle/*.cpp, tests/*.cpp) so parallel modules
# never edit this file. A module that lands its real code deletes its src/<module>/*_stub.cpp.

if (CMAKE_SOURCE_DIR STREQUAL CMAKE_CURRENT_SOURCE_DIR)
    cmake_minimum_required(VERSION 3.26)
    if (NOT DEFINED CMAKE_BUILD_TYPE AND NOT DEFINED ENV{CMAKE_BUILD_TYPE})
        set(CMAKE_BUILD_TYPE "Release" CACHE STRING "Build type")
    endif ()
    project(hdf_convert LANGUAGES C CXX)
    set(CMAKE_CXX_STANDARD 23)
    set(CMAKE_CXX_STANDARD_REQUIRED ON)
    set(CMAKE_EXPORT_COMPILE_COMMANDS ON)
    set(_hdf_standalone ON)
    enable_testing()
else ()
    set(_hdf_standalone OFF)
    get_property(_hdf_languages GLOBAL PROPERTY ENABLED_LANGUAGES)
    if (NOT "C" IN_LIST _hdf_languages)
        message(FATAL_ERROR "hdf_convert: the root project must enable C too (project(... LANGUAGES C CXX)) for the "
                "vendored zlib and zstd")
    endif ()
endif ()

option(HDF_BUILD_CLI "Build hdf-convert (developer converter CLI and tests; never packaged)" ${_hdf_standalone})
option(HDF_WERROR "Treat warnings in hdf_convert and hdf-convert as errors" OFF)

get_filename_component(HDF_PROJECT_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/../../.." ABSOLUTE)
set(_tp "${HDF_PROJECT_ROOT}/third_party")
if (NOT EXISTS "${_tp}/zlib/zlib.h" OR NOT EXISTS "${_tp}/zstd/zstddeclib.c")
    message(FATAL_ERROR "hdf_convert: vendored libraries not found in ${_tp} (see third_party/README.md)")
endif ()

# ------------------------------------------------------------------------------------------ third party
add_library(hdf_third_party STATIC
        "${_tp}/zlib/adler32.c"
        "${_tp}/zlib/compress.c"
        "${_tp}/zlib/crc32.c"
        "${_tp}/zlib/deflate.c"
        "${_tp}/zlib/inffast.c"
        "${_tp}/zlib/inflate.c"
        "${_tp}/zlib/inftrees.c"
        "${_tp}/zlib/trees.c"
        "${_tp}/zlib/uncompr.c"
        "${_tp}/zlib/zutil.c"
        "${_tp}/zstd/zstddeclib.c")
target_include_directories(hdf_third_party SYSTEM PUBLIC "${_tp}/zlib" "${_tp}/zstd")
target_compile_definitions(hdf_third_party PRIVATE ZSTD_DISABLE_ASM ZSTD_LEGACY_SUPPORT=0)
set_target_properties(hdf_third_party PROPERTIES POSITION_INDEPENDENT_CODE ON C_STANDARD 11)
if (MSVC)
    target_compile_options(hdf_third_party PRIVATE /W0 /utf-8)
else ()
    target_compile_options(hdf_third_party PRIVATE -w)
endif ()

# ------------------------------------------------------------------------------------------ library
file(GLOB _hdf_sources CONFIGURE_DEPENDS
        "${CMAKE_CURRENT_SOURCE_DIR}/src/*/*.cpp")
file(GLOB _hdf_headers CONFIGURE_DEPENDS
        "${CMAKE_CURRENT_SOURCE_DIR}/include/hdf/*.hpp"
        "${CMAKE_CURRENT_SOURCE_DIR}/data/*.inc")
add_library(hdf_convert STATIC ${_hdf_sources} ${_hdf_headers})
target_include_directories(hdf_convert
        PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/include"
        PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/data" "${CMAKE_CURRENT_SOURCE_DIR}/src")
target_compile_features(hdf_convert PUBLIC cxx_std_23)
target_link_libraries(hdf_convert PUBLIC hdf_third_party)
set_target_properties(hdf_convert PROPERTIES POSITION_INDEPENDENT_CODE ON)
if (WIN32)
    target_link_libraries(hdf_convert PUBLIC bcrypt)  # SHA-256 through Windows CNG (BCryptHash)
    target_compile_definitions(hdf_convert PRIVATE NOMINMAX WIN32_LEAN_AND_MEAN UNICODE _UNICODE)
endif ()

set(_hdf_warn_msvc /W4 /permissive- /Zc:__cplusplus /utf-8 /fp:precise)
set(_hdf_warn_gnu -Wall -Wextra -ffp-contract=off -fno-fast-math)
if (HDF_WERROR)
    list(APPEND _hdf_warn_msvc /WX)
    list(APPEND _hdf_warn_gnu -Werror)
endif ()
if (MSVC)
    target_compile_options(hdf_convert PRIVATE ${_hdf_warn_msvc})
else ()
    target_compile_options(hdf_convert PRIVATE ${_hdf_warn_gnu})
endif ()

# ------------------------------------------------------------------------------------------ CLI + tests
if (HDF_BUILD_CLI)
    configure_file("${CMAKE_CURRENT_SOURCE_DIR}/cli/hdf_cli_config.hpp.in"
            "${CMAKE_CURRENT_BINARY_DIR}/hdf_cli_config.hpp" @ONLY NEWLINE_STYLE LF)
    file(GLOB _hdf_cli_sources CONFIGURE_DEPENDS
            "${CMAKE_CURRENT_SOURCE_DIR}/cli/*.cpp"
            "${CMAKE_CURRENT_SOURCE_DIR}/cli/oracle/*.cpp"
            "${CMAKE_CURRENT_SOURCE_DIR}/tests/*.cpp")
    add_executable(hdf-convert ${_hdf_cli_sources}
            "${CMAKE_CURRENT_SOURCE_DIR}/cli/cli.hpp"
            "${CMAKE_CURRENT_SOURCE_DIR}/tests/test.hpp")
    target_include_directories(hdf-convert PRIVATE "${CMAKE_CURRENT_BINARY_DIR}" "${CMAKE_CURRENT_SOURCE_DIR}/cli"
            "${CMAKE_CURRENT_SOURCE_DIR}/tests")
    target_link_libraries(hdf-convert PRIVATE hdf_convert)
    if (MSVC)
        target_compile_options(hdf-convert PRIVATE ${_hdf_warn_msvc})
        target_compile_definitions(hdf-convert PRIVATE NOMINMAX WIN32_LEAN_AND_MEAN UNICODE _UNICODE)
    else ()
        target_compile_options(hdf-convert PRIVATE ${_hdf_warn_gnu})
    endif ()

    # CI-safe: synthetic data only.
    add_test(NAME hdf_unit COMMAND hdf-convert self-test)

    # Local only (needs your own games and the oracle; outputs stay in the build folder).
    set(HDF_TEST_HD "" CACHE PATH "TP HD content/title folder for the local oracle tests")
    set(HDF_TEST_HD_WUA "" CACHE FILEPATH "TP HD .wua for the local oracle tests (optional)")
    set(HDF_TEST_GC "" CACHE PATH "Extracted GZ2E01 'files' folder for the local oracle tests")
    set(HDF_TEST_ORACLE "" CACHE FILEPATH "The oracle .dusk (cache/oracle/hd_features-oracle-2.0.0.dusk)")
    if (HDF_TEST_HD AND HDF_TEST_GC AND HDF_TEST_ORACLE)
        set(_out "${CMAKE_CURRENT_BINARY_DIR}/hdf-oracle-out")
        add_test(NAME hdf_oracle
                COMMAND hdf-convert make --hd "${HDF_TEST_HD}" --gc "${HDF_TEST_GC}" --out "${_out}" --time)
        # --skip-missing: exit 3 (skipped) while make has not made anything yet (wave-1 stubs)
        add_test(NAME hdf_oracle_compare
                COMMAND hdf-convert compare --out "${_out}" --oracle "${HDF_TEST_ORACLE}" --skip-missing)
        set_tests_properties(hdf_oracle_compare PROPERTIES DEPENDS hdf_oracle)
        if (HDF_TEST_HD_WUA)
            add_test(NAME hdf_oracle_wua
                    COMMAND hdf-convert make --hd "${HDF_TEST_HD_WUA}" --gc "${HDF_TEST_GC}" --gc-mode memory
                    --out "${CMAKE_CURRENT_BINARY_DIR}/hdf-oracle-out-wua")
        endif ()
        add_test(NAME hdf_determinism
                COMMAND hdf-convert test gen --hd "${HDF_TEST_HD}" --gc "${HDF_TEST_GC}" --oracle "${HDF_TEST_ORACLE}"
                --work "${CMAKE_CURRENT_BINARY_DIR}/hdf-determinism" determinism)
        add_test(NAME hdf_cancel
                COMMAND hdf-convert test gen --hd "${HDF_TEST_HD}" --gc "${HDF_TEST_GC}" --oracle "${HDF_TEST_ORACLE}"
                --work "${CMAKE_CURRENT_BINARY_DIR}/hdf-cancel" cancel)
        # C2: tables, check-inputs on the folder (and the .wua), compare oracle vs itself, seed-cache.
        set(_hdf_base_wua "")
        if (HDF_TEST_HD_WUA)
            set(_hdf_base_wua --wua "${HDF_TEST_HD_WUA}")
        endif ()
        add_test(NAME hdf_base
                COMMAND hdf-convert test base --hd "${HDF_TEST_HD}" --gc "${HDF_TEST_GC}" --oracle "${HDF_TEST_ORACLE}"
                ${_hdf_base_wua} --work "${CMAKE_CURRENT_BINARY_DIR}/hdf-base")
        # C1: RARC/JSON/PNG against the oracle + the golden list (fix-wave1 L7). Make the list first:
        #   python -I -B tools/dev/golden.py io --hd <..> --gc <..> --out <build dir>/golden/io.tsv
        # Without it, part (c) is skipped and the test reports exit 3 (skipped).
        add_test(NAME hdf_io
                COMMAND hdf-convert test io --hd "${HDF_TEST_HD}" --gc "${HDF_TEST_GC}" --oracle "${HDF_TEST_ORACLE}"
                --golden "${CMAKE_CURRENT_BINARY_DIR}/golden/io.tsv")
        # The wave-1 stubs exit 3 ("todo"): report those as skipped, not failed.
        set_tests_properties(hdf_base hdf_io hdf_oracle hdf_oracle_compare hdf_determinism hdf_cancel
                PROPERTIES SKIP_RETURN_CODE 3)
        if (HDF_TEST_HD_WUA)
            set_tests_properties(hdf_oracle_wua PROPERTIES SKIP_RETURN_CODE 3)
        endif ()
    endif ()
endif ()
