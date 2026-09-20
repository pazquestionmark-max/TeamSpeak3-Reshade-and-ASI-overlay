cmake_minimum_required(VERSION 3.20)
project(teamspeak-reshade-overlay VERSION 1.0.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

option(TSRO_BUILD_TESTS "Build the unit and integration test suite" ON)
option(TSRO_BUILD_PLUGIN "Build the TeamSpeak 3 plugin (Windows only)" OFF)
option(TSRO_BUILD_ADDON "Build the ReShade add-on (Windows only)" OFF)
option(TSRO_BUILD_ASI "Build the standalone .asi plugin (Windows only)" OFF)
option(TSRO_WARNINGS_AS_ERRORS "Treat compiler warnings as errors" OFF)

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
  set(CMAKE_BUILD_TYPE Release CACHE STRING "" FORCE)
endif()

# Static CRT for every target, set once here rather than per target. The plugin and the add-on
# need it -- a DLL that depends on a redistributable the user has not installed simply fails to
# load, with no diagnostic they can act on -- and a static library built against the dynamic CRT
# cannot be linked into them (LNK2038). Setting it in one place is what stops those drifting.
if(MSVC)
  set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
endif()

# These two live in the game process and the voice client; both are built for size and speed
# with no runtime the user must install separately.
# Scoped to C++ with a generator expression rather than set bare, so that a C source added to
# any target later is not handed /EHsc or /Zc:__cplusplus -- which mean nothing to a C compiler
# and earn a command-line warning that /WX would turn into a failed build.
if(MSVC)
  add_compile_options($<$<COMPILE_LANGUAGE:CXX>:/W4>
                      $<$<COMPILE_LANGUAGE:CXX>:/permissive->
                      $<$<COMPILE_LANGUAGE:CXX>:/Zc:__cplusplus>
                      /utf-8
                      $<$<COMPILE_LANGUAGE:CXX>:/EHsc>)
  if(TSRO_WARNINGS_AS_ERRORS)
    add_compile_options($<$<COMPILE_LANGUAGE:CXX>:/WX>)
  endif()
else()
  add_compile_options($<$<COMPILE_LANGUAGE:CXX>:-Wall>
                      $<$<COMPILE_LANGUAGE:CXX>:-Wextra>
                      $<$<COMPILE_LANGUAGE:CXX>:-Wpedantic>
                      $<$<COMPILE_LANGUAGE:CXX>:-Wshadow>
                      $<$<COMPILE_LANGUAGE:CXX>:-Wconversion>
                      $<$<COMPILE_LANGUAGE:CXX>:-Wsign-conversion>)
  if(TSRO_WARNINGS_AS_ERRORS)
    add_compile_options($<$<COMPILE_LANGUAGE:CXX>:-Werror>)
  endif()
endif()

add_subdirectory(shared)
add_subdirectory(tools/config-tool)

if(TSRO_BUILD_PLUGIN)
  add_subdirectory(teamspeak-plugin)
endif()

# The overlay proper -- renderer, settings window, icons, font engine and the host object that
# owns profiles and the IPC client -- knows nothing about ReShade or about hooking D3D. Both
# front ends compile exactly these sources and differ only in their entry point and their font
# texture sink, so the list lives here rather than being copied into two CMakeLists that drift.
set(TSRO_OVERLAY_SOURCES
  ${CMAKE_CURRENT_SOURCE_DIR}/reshade-integration/src/overlay_host.cpp
  ${CMAKE_CURRENT_SOURCE_DIR}/reshade-integration/src/font_engine.cpp
  ${CMAKE_CURRENT_SOURCE_DIR}/reshade-integration/src/renderer.cpp
  ${CMAKE_CURRENT_SOURCE_DIR}/reshade-integration/src/settings_ui.cpp
  ${CMAKE_CURRENT_SOURCE_DIR}/reshade-integration/src/icons.cpp
)
set(TSRO_OVERLAY_INCLUDE_DIR ${CMAKE_CURRENT_SOURCE_DIR}/reshade-integration/include)

if(TSRO_BUILD_ADDON)
  add_subdirectory(reshade-integration)
endif()

if(TSRO_BUILD_ASI)
  add_subdirectory(asi-integration)
endif()

if(TSRO_BUILD_TESTS)
  enable_testing()
  add_subdirectory(tests)
endif()
