cmake_minimum_required(VERSION 3.26)
set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

if (NOT DEFINED CMAKE_BUILD_TYPE AND NOT DEFINED ENV{CMAKE_BUILD_TYPE})
    set(CMAKE_BUILD_TYPE "RelWithDebInfo" CACHE STRING "Build type")
endif ()

project(dusklight_hd_mods VERSION 2.1.0 LANGUAGES CXX)

# Pinned to the Dusklight release this mod is made for. cmake/FetchDusklight.cmake downloads its source
# (git tag v2.0.3 of https://github.com/TwilitRealm/dusklight) into ./dusklight on the first configure.
if (NOT DUSKLIGHT_VERSION)
    set(DUSKLIGHT_VERSION "v2.0.3")
endif ()
include("${CMAKE_CURRENT_SOURCE_DIR}/cmake/FetchDusklight.cmake")

# DUSK_GAME_EXE (optional, empty by default): the import library of your own Dusklight v2.0.3 install,
#   -DDUSK_GAME_EXE="<Dusklight folder>/sdk/windows-amd64.lib"
# Left empty, the Dusklight SDK downloads its version-independent link stub instead.
if (DUSK_GAME_EXE)
    message(STATUS "HD Features: linking against DUSK_GAME_EXE = ${DUSK_GAME_EXE}")
else ()
    message(STATUS "HD Features: DUSK_GAME_EXE is not set, so the Dusklight SDK uses its link stub. To link "
            "against your own install instead: -DDUSK_GAME_EXE=\"<Dusklight folder>/sdk/windows-amd64.lib\"")
endif ()

add_subdirectory("${DUSKLIGHT_DIR}/sdk" dusklight-sdk EXCLUDE_FROM_ALL)

# The mods to build, as folders under mods/. Since 2.0.0 there is one: HD Features (mods/hdfeatures,
# local.ricky.hdfeatures), which merged HD Stamps and Cave of Shadows.
if (NOT HD_MODS)
    set(HD_MODS hdfeatures)
endif ()
foreach (_mod IN LISTS HD_MODS)
    if (EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/mods/${_mod}/CMakeLists.txt")
        add_subdirectory(mods/${_mod})
    else ()
        message(FATAL_ERROR "HD_MODS: mods/${_mod}/CMakeLists.txt does not exist (the old 'stamps' and "
                "'caveofshadows' mods are now mods/hdfeatures)")
    endif ()
endforeach ()
