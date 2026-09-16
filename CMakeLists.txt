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

add_library(tmx_mod SHARED
  src/dllmain.cpp
  src/config.cpp
  src/game.cpp
  src/hook.cpp
  src/http.cpp
  src/overlay.cpp
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
