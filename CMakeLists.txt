cmake_minimum_required(VERSION 3.21)

# The version, in one place.
#
# It is the folder the ModLoader installs into and the version it lists, so a
# stale value here is a mod that reports itself as something it is not - which
# is exactly what happened: this sat at 0.5.0 through four releases because it
# was a second place to remember. CI overrides it from the tag
# (-DTMX_VERSION=0.6.4), so a release can only ever announce the tag it was
# built from; this default is what a local build calls itself.
project(tmx_mod VERSION 0.8.1 LANGUAGES CXX)
# NOT a cache entry. A cached default is written once and then wins over the
# source forever: this sat at 0.6.15 while `project(VERSION)` said 0.7.0, so a
# local build installed itself into the wrong folder and the game kept loading
# an older DLL - which is a very expensive way to debug the *previous* version.
# `-DTMX_VERSION=...` from CI still defines it, and that is the only way it is
# ever anything other than the version above.
if(NOT DEFINED TMX_VERSION)
  set(TMX_VERSION "${PROJECT_VERSION}")
endif()
message(STATUS "100TMX version: ${TMX_VERSION}")

# TrackMania Forever is 32-bit. There is no 64-bit build to make, and a 64-bit
# DLL cannot be loaded into the game at all - configure with `-A Win32`.
if(CMAKE_SIZEOF_VOID_P EQUAL 8)
  message(FATAL_ERROR "TrackMania Forever is 32-bit: configure with -A Win32.")
endif()

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
cmake_policy(SET CMP0091 NEW)

# Statically linked runtime: a mod that needs a Visual C++ redistributable
# installed is a mod that fails for somebody on a fresh Windows, silently.
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")

include(FetchContent)
FetchContent_Declare(
  imgui
  GIT_REPOSITORY https://github.com/ocornut/imgui.git
  GIT_TAG v1.91.5          # pinned: an overlay that changes under you is a bug hunt
)
FetchContent_MakeAvailable(imgui)   # no CMakeLists of its own, so this only populates

# stb_image, for the map screenshots. Header-only, public domain, and the only
# thing standing between a JPEG from TMX and a D3D9 texture.
FetchContent_Declare(
  stb
  GIT_REPOSITORY https://github.com/nothings/stb.git
  GIT_TAG 2c980bb59875b0d32144a71867fbdebb2f77cd20  # pinned to a full sha: stb has no releases, and a short one cannot be checked out
)
FetchContent_MakeAvailable(stb)

add_library(tmx_mod SHARED
  src/bridge.cpp
  src/dllmain.cpp
  src/config.cpp
  src/game.cpp
  src/hook.cpp
  src/http.cpp
  src/log.cpp
  src/overlay.cpp
  src/scan.cpp
  src/textures.cpp
  src/worker.cpp

  ${imgui_SOURCE_DIR}/imgui.cpp
  ${imgui_SOURCE_DIR}/imgui_draw.cpp
  ${imgui_SOURCE_DIR}/imgui_tables.cpp
  ${imgui_SOURCE_DIR}/imgui_widgets.cpp
  ${imgui_SOURCE_DIR}/backends/imgui_impl_dx9.cpp
  ${imgui_SOURCE_DIR}/backends/imgui_impl_win32.cpp
)

target_include_directories(tmx_mod PRIVATE
  src
  ${imgui_SOURCE_DIR}
  ${stb_SOURCE_DIR}
)

target_compile_definitions(tmx_mod PRIVATE
  WIN32_LEAN_AND_MEAN
  NOMINMAX
  _CRT_SECURE_NO_WARNINGS
)

target_compile_options(tmx_mod PRIVATE /W4 /EHsc /permissive-)

# The libraries are declared next to the code that needs them, with
# `#pragma comment(lib, ...)`, except d3d9 which the hook also declares.
set_target_properties(tmx_mod PROPERTIES
  OUTPUT_NAME "100TMX"
  PREFIX ""
)

# Same binary, second extension: ASI loaders look for `.asi`, the TrackMania
# ModLoader takes the `.dll`.
add_custom_command(TARGET tmx_mod POST_BUILD
  COMMAND ${CMAKE_COMMAND} -E copy
          $<TARGET_FILE:tmx_mod>
          $<TARGET_FILE_DIR:tmx_mod>/100TMX.asi
  COMMENT "Copying 100TMX.dll to 100TMX.asi"
)

# ---------------------------------------------------------------------------
# The installer.
#
# One .exe that carries the DLL inside it and writes the three files the
# ModLoader's product database wants. The DLL is embedded as a resource, so the
# resource script has to be generated after the mod is built - `file(GENERATE)`
# rather than `configure_file`, because the path is only known per configuration.
# ---------------------------------------------------------------------------
file(GENERATE
  OUTPUT ${CMAKE_CURRENT_BINARY_DIR}/installer_$<CONFIG>.rc
  CONTENT "#include \"resource.h\"
101 RCDATA \"$<TARGET_FILE:tmx_mod>\"
"
)

add_executable(tmx_installer WIN32
  installer/main.cpp
  ${CMAKE_CURRENT_BINARY_DIR}/installer_$<CONFIG>.rc
)
add_dependencies(tmx_installer tmx_mod)

# Through a generated header, not a -D string: `TMX_VERSION=L"0.6.4"` loses its
# quotes between CMake, MSBuild and the compiler, and the installer went out
# registering a version folder called "0".
configure_file(installer/version.h.in "${CMAKE_CURRENT_BINARY_DIR}/tmx_version.h" @ONLY)
target_include_directories(tmx_installer PRIVATE installer "${CMAKE_CURRENT_BINARY_DIR}")
# The DLL logs its own version, so "installed" and "running" can be told apart
# when the ModLoader has several versions to choose from.
target_include_directories(tmx_mod PRIVATE "${CMAKE_CURRENT_BINARY_DIR}")
set_property(SOURCE ${CMAKE_CURRENT_BINARY_DIR}/installer_$<CONFIG>.rc
             PROPERTY INCLUDE_DIRECTORIES ${CMAKE_CURRENT_SOURCE_DIR}/installer)

target_compile_definitions(tmx_installer PRIVATE WIN32_LEAN_AND_MEAN NOMINMAX UNICODE _UNICODE)
target_compile_options(tmx_installer PRIVATE /W4 /EHsc)

set_target_properties(tmx_installer PROPERTIES
  OUTPUT_NAME "100TMX-Installer"
)
