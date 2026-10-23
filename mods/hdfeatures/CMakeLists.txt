# HD Features (local.ricky.hdfeatures): Twilight Princess HD's 50 stamps and the Cave of Shadows as
# one Dusklight mod. Built from the repository root (see BUILDING.md):
#   cmake -S . -B build-hdfeatures -G Ninja -DCMAKE_BUILD_TYPE=Release
#   cmake --build build-hdfeatures            -> build-hdfeatures/mods/hd_features.dusk
#
# Since 2.1.0 the package holds no game files (plan research/dropin/plan.md 2.1, Correction #12): only
# mod.json, lib/windows-amd64/mod.dll and package/res/ (the neutral icon and banner drawn by
# tools/dist/base_art.py, and THIRD-PARTY-NOTICES.txt). The mod makes the game files on the player's PC from
# their own games, with the converter library in convert/ (hdf_convert), and stores them in its data folder
# (src/hdfiles). The developer CLI hdf-convert builds with -DHDF_BUILD_CLI=ON; it is never packaged.

set(HDFEATURES_MOD_ID "local.ricky.hdfeatures")
set(HDFEATURES_VERSION "2.1.0")

configure_file(mod.json.in "${CMAKE_CURRENT_BINARY_DIR}/mod.json" @ONLY NEWLINE_STYLE LF)

# The converter: vendored zlib and zstd are C. Enabled here, the highest folder common to hd_features and
# convert/ (the root project only enables C++).
enable_language(C)
add_subdirectory(convert)

# The package's res/ folder: exactly these three files (Correction #12). Anything else there would be
# shipped to every player, so configure stops.
set(_hdf_package_res "${CMAKE_CURRENT_SOURCE_DIR}/package/res")
set(_hdf_package_expected THIRD-PARTY-NOTICES.txt banner.png icon.png)
file(GLOB_RECURSE _hdf_package_files LIST_DIRECTORIES false RELATIVE "${_hdf_package_res}" "${_hdf_package_res}/*")
list(SORT _hdf_package_files)
if (NOT _hdf_package_files STREQUAL _hdf_package_expected)
    list(JOIN _hdf_package_files ", " _hdf_package_text)
    message(FATAL_ERROR "hd_features: package/res must hold exactly THIRD-PARTY-NOTICES.txt, banner.png and "
            "icon.png (it holds: ${_hdf_package_text})")
endif ()
foreach (_dir IN ITEMS res overlay)
    if (EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/${_dir}")
        message(WARNING "hd_features: mods/hdfeatures/${_dir}/ is no longer packaged (2.1.0 makes the game files "
                "in the game). Move it out of the source tree (cache/legacy-generated/).")
    endif ()
endforeach ()

# src/hdfiles: the stored game files (R1: files, cache, overlays) and their setup (R2). cache.cpp uses only the
# converter library and the standard library (no game headers, no SDK), so it builds without the mod's forced
# global.h, in its own small library.
add_library(hd_features_cache STATIC src/hdfiles/cache.cpp src/hdfiles/cache.hpp)
target_include_directories(hd_features_cache PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/src")
target_link_libraries(hd_features_cache PUBLIC hdf_convert)
target_compile_features(hd_features_cache PUBLIC cxx_std_23)
if (MSVC)
    target_compile_options(hd_features_cache PRIVATE /W4 /permissive- /utf-8)
    target_compile_definitions(hd_features_cache PRIVATE NOMINMAX WIN32_LEAN_AND_MEAN UNICODE _UNICODE)
endif ()
file(GLOB _hdfiles_sources CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/src/hdfiles/*.cpp")
list(REMOVE_ITEM _hdfiles_sources "${CMAKE_CURRENT_SOURCE_DIR}/src/hdfiles/cache.cpp")

add_mod(hd_features
        FEATURES game fmt
        SOURCES
        # the mod: entry points, feature switches (mod.cpp), Mods panel / menu tab / window (panel.cpp)
        src/mod.cpp
        src/panel.cpp
        # the stored game files (src/hdfiles: files.cpp, overlays.cpp, and R2's setup files)
        ${_hdfiles_sources}
        # Stamps (namespace hdstamps)
        src/stamps/feature.cpp
        src/stamps/items.cpp
        src/stamps/state.cpp
        src/stamps/ui.cpp
        src/stamps/book.cpp
        src/stamps/book_assets.cpp
        src/stamps/book_host.cpp
        src/stamps/collect_slot.cpp
        src/stamps/filesel.cpp
        src/stamps/stamp_book.cpp
        # Cave of Shadows (namespace cave)
        src/cave/cave.cpp
        src/cave/hooks.cpp
        src/cave/ui.cpp
        src/cave/amiibo.cpp
        src/cave/confirm.cpp   # HD's pre-entry "Cave of Shadows" screen (Save and begin / Cancel)
        src/cave/record.cpp    # HD's "Cave of Shadows Record" screen after an exit portal
        src/cave/hdtext.cpp
        src/cave/hdparse.cpp
        MOD_JSON "${CMAKE_CURRENT_BINARY_DIR}/mod.json"
        RES_DIR package/res    # res/icon.png, res/banner.png, res/THIRD-PARTY-NOTICES.txt; no OVERLAY_DIR
)

# src/ for "hdfeatures.hpp", "stamps/...", "cave/..." and "hdfiles/..."; a feature's own headers are found
# next to the including file first (both features have an hdtext.hpp and a ui.cpp, in different folders and
# namespaces).
target_include_directories(hd_features PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/src")
target_compile_definitions(hd_features PRIVATE HDFEATURES_MOD_ID="${HDFEATURES_MOD_ID}")
target_link_libraries(hd_features PRIVATE hd_features_cache hdf_convert)
