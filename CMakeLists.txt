cmake_minimum_required(VERSION 3.21)
project(tmx_mod LANGUAGES CXX)

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
  src/dllmain.cpp
  src/config.cpp
  src/game.cpp
  src/hook.cpp
  src/http.cpp
  src/log.cpp
  src/overlay.cpp
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

target_include_directories(tmx_installer PRIVATE installer)
set_property(SOURCE ${CMAKE_CURRENT_BINARY_DIR}/installer_$<CONFIG>.rc
             PROPERTY INCLUDE_DIRECTORIES ${CMAKE_CURRENT_SOURCE_DIR}/installer)

target_compile_definitions(tmx_installer PRIVATE WIN32_LEAN_AND_MEAN NOMINMAX UNICODE _UNICODE)
target_compile_options(tmx_installer PRIVATE /W4 /EHsc)

set_target_properties(tmx_installer PROPERTIES
  OUTPUT_NAME "100TMX-Installer"
)
