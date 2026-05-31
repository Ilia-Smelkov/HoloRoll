cmake_minimum_required(VERSION 3.24)
project(reaper_holoroll VERSION 0.12.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

option(REAPER_EXT_WARNINGS_AS_ERRORS "Treat warnings as errors" OFF)

set(REAPER_SDK_DIR_DEFAULT "${CMAKE_CURRENT_SOURCE_DIR}/third_party/reaper-sdk/sdk")
set(REAPER_SDK_DIR "$ENV{REAPER_SDK_DIR}")
if("${REAPER_SDK_DIR}" STREQUAL "" AND EXISTS "${REAPER_SDK_DIR_DEFAULT}/reaper_plugin.h")
  set(REAPER_SDK_DIR "${REAPER_SDK_DIR_DEFAULT}")
endif()

if(NOT EXISTS "${REAPER_SDK_DIR}/reaper_plugin.h")
  message(FATAL_ERROR
    "reaper_plugin.h was not found. Set REAPER_SDK_DIR or clone SDK to third_party/reaper-sdk/sdk.")
endif()

include(FetchContent)

FetchContent_Declare(
  imgui
  GIT_REPOSITORY https://github.com/ocornut/imgui.git
  GIT_TAG v1.91.9b
)
FetchContent_MakeAvailable(imgui)

# tinygltf: header-only glTF/GLB parser. Pulls in nlohmann/json automatically.
# We only need geometry + skinning + animations, so disable the image stack
# (no stb_image dependency, no decoded image data in memory).
FetchContent_Declare(
  tinygltf
  GIT_REPOSITORY https://github.com/syoyo/tinygltf.git
  GIT_TAG v2.9.3
)
FetchContent_MakeAvailable(tinygltf)

add_library(reaper_holoroll SHARED
  src/extension/entry.cpp
  src/extension/reaper_bridge.cpp
  src/extension/folder_picker.cpp
  src/extension/drop_target.cpp
  src/extension/socket_server.cpp
  src/core/mdd_data_manager.cpp
  src/core/obj_index_loader.cpp
  src/core/animation_library.cpp
  src/core/config_store.cpp
  src/core/folder_watcher.cpp
  src/core/glb_loader.cpp
  src/core/motion_events.cpp
  src/render/gl_viewport.cpp
  ${imgui_SOURCE_DIR}/imgui.cpp
  ${imgui_SOURCE_DIR}/imgui_draw.cpp
  ${imgui_SOURCE_DIR}/imgui_widgets.cpp
  ${imgui_SOURCE_DIR}/imgui_tables.cpp
  ${imgui_SOURCE_DIR}/backends/imgui_impl_win32.cpp
  ${imgui_SOURCE_DIR}/backends/imgui_impl_opengl3.cpp
)

target_include_directories(reaper_holoroll PRIVATE
  "${REAPER_SDK_DIR}"
  "${imgui_SOURCE_DIR}"
  "${imgui_SOURCE_DIR}/backends"
  "${tinygltf_SOURCE_DIR}"
  "${CMAKE_CURRENT_SOURCE_DIR}/src"
)

target_compile_definitions(reaper_holoroll PRIVATE
  WIN32_LEAN_AND_MEAN
  NOMINMAX
  REAPERAPI_WANT_EXTENSIONS
  # tinygltf knobs: we don't need image decoding for skinned meshes.
  TINYGLTF_NO_STB_IMAGE
  TINYGLTF_NO_STB_IMAGE_WRITE
  TINYGLTF_NO_EXTERNAL_IMAGE
)

target_link_libraries(reaper_holoroll PRIVATE
  opengl32
  user32
  gdi32
  comdlg32
  shell32
  ole32
  oleaut32
  # v0.12.0-alpha.11: TCP socket bridge.
  ws2_32
)

if(MSVC)
  target_compile_options(reaper_holoroll PRIVATE /W4 /permissive-)
  if(REAPER_EXT_WARNINGS_AS_ERRORS)
    target_compile_options(reaper_holoroll PRIVATE /WX)
  endif()
else()
  target_compile_options(reaper_holoroll PRIVATE -Wall -Wextra -Wpedantic)
endif()

set_target_properties(reaper_holoroll PROPERTIES
  OUTPUT_NAME "reaper_holoroll"
)
