# Standalone .asi plugin (Component A, second front end).
#
# The same overlay as the ReShade add-on, built against its own copy of Dear ImGui instead of
# ReShade's function table. That is the whole difference in the build: the shared sources are
# compiled here WITHOUT TSRO_HOST_RESHADE, so the ImGui calls in them bind to the real library
# rather than to ReShade's inline forwarders.

# MinHook is C, and this is the only part of the project that is. The top-level project()
# declares CXX alone, so without this CMake has no compiler for those files and fails at
# generate time with "can not determine linker language for target". Enabled here rather than
# at the top so a build that does not want the .asi does not need a C compiler at all.
enable_language(C)

set(TSRO_IMGUI_DIR "${CMAKE_SOURCE_DIR}/third_party/imgui"
    CACHE PATH "Path to a checkout of ocornut/imgui")
set(TSRO_MINHOOK_DIR "${CMAKE_SOURCE_DIR}/third_party/minhook"
    CACHE PATH "Path to the vendored MinHook sources")

if(NOT EXISTS "${TSRO_IMGUI_DIR}/imgui.h")
  message(FATAL_ERROR
    "Dear ImGui sources not found under '${TSRO_IMGUI_DIR}'.\n"
    "Run scripts/fetch-deps.ps1 (or fetch-deps.sh), or pass -DTSRO_IMGUI_DIR=<path>.")
endif()
if(NOT EXISTS "${TSRO_MINHOOK_DIR}/include/MinHook.h")
  message(FATAL_ERROR "MinHook not found under '${TSRO_MINHOOK_DIR}'.")
endif()

# MinHook is C. Only the 64-bit half of its length-disassembler is vendored, which is all an
# .asi needs: every ASI loader worth supporting -- FiveM's among them -- is 64-bit.
add_library(tsro_minhook STATIC
  ${TSRO_MINHOOK_DIR}/src/buffer.c
  ${TSRO_MINHOOK_DIR}/src/hook.c
  ${TSRO_MINHOOK_DIR}/src/trampoline.c
  ${TSRO_MINHOOK_DIR}/src/hde/hde64.c
)
target_include_directories(tsro_minhook PUBLIC ${TSRO_MINHOOK_DIR}/include)
# Third-party C: its warnings are not ours to fix, and /WX would fail the build on them.
if(MSVC)
  target_compile_options(tsro_minhook PRIVATE /W0)
else()
  target_compile_options(tsro_minhook PRIVATE -w)
endif()

# Dear ImGui plus the two backends this host needs. Compiled as its own library so the
# project's -Wconversion/-WX settings, which ImGui does not build clean under, stop at the
# boundary instead of being switched off for the overlay's own code.
add_library(tsro_imgui STATIC
  ${TSRO_IMGUI_DIR}/imgui.cpp
  ${TSRO_IMGUI_DIR}/imgui_draw.cpp
  ${TSRO_IMGUI_DIR}/imgui_tables.cpp
  ${TSRO_IMGUI_DIR}/imgui_widgets.cpp
  ${TSRO_IMGUI_DIR}/backends/imgui_impl_dx11.cpp
  ${TSRO_IMGUI_DIR}/backends/imgui_impl_win32.cpp
)
target_include_directories(tsro_imgui PUBLIC ${TSRO_IMGUI_DIR})
target_compile_definitions(tsro_imgui PUBLIC
  IMGUI_DISABLE_OBSOLETE_FUNCTIONS
  IMGUI_DISABLE_DEMO_WINDOWS
  # imgui_impl_dx11 compiles its shaders at load time through d3dcompiler_47.dll, which ships
  # with Windows. Nothing extra has to be installed for this.
  WIN32_LEAN_AND_MEAN
  NOMINMAX
)
# gdi32 for imgui_impl_win32's DPI path (GetDeviceCaps), dwmapi for its composition query.
target_link_libraries(tsro_imgui PUBLIC d3d11 dxgi d3dcompiler dwmapi gdi32)
if(MSVC)
  target_compile_options(tsro_imgui PRIVATE /W0)
else()
  target_compile_options(tsro_imgui PRIVATE -w)
endif()

# FiveM's loader refuses an .asi that does not declare the game build it is running on, and the
# refusal happens before DllMain, so the plugin cannot report it -- it just never loads. The
# declaration is a resource, so the .rc is not optional decoration here, it is load-bearing.
# Generated so its version block tracks project(VERSION) on its own.
set(TSRO_ASI_RC "")
if(WIN32)
  enable_language(RC)
  configure_file(TeamSpeakOverlay.rc.in
                 "${CMAKE_CURRENT_BINARY_DIR}/TeamSpeakOverlay.rc" @ONLY)
  set(TSRO_ASI_RC "${CMAKE_CURRENT_BINARY_DIR}/TeamSpeakOverlay.rc")
endif()

add_library(tsro_asi SHARED
  src/asi_main.cpp
  src/d3d11_overlay.cpp
  ${TSRO_ASI_RC}
  ${TSRO_OVERLAY_SOURCES}
)

target_include_directories(tsro_asi
  PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/include
  PRIVATE ${TSRO_OVERLAY_INCLUDE_DIR}
)

# Note what is NOT defined here: TSRO_HOST_RESHADE. That one absence is what makes the shared
# renderer, settings UI, icons and font engine link against this target's real ImGui.
target_compile_definitions(tsro_asi PRIVATE
  WIN32_LEAN_AND_MEAN
  NOMINMAX
)

target_link_libraries(tsro_asi PRIVATE tsro_shared tsro_imgui tsro_minhook)

# ASI loaders -- FiveM's included -- load plugins by extension from a plugins folder.
set_target_properties(tsro_asi PROPERTIES
  OUTPUT_NAME "TeamSpeakOverlay"
  PREFIX ""
  SUFFIX ".asi"
)

install(TARGETS tsro_asi RUNTIME DESTINATION asi LIBRARY DESTINATION asi)
