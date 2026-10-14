cmake_minimum_required(VERSION 3.25)
project(tetris LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE Debug CACHE STRING "Build type" FORCE)
endif()
set(CMAKE_CXX_FLAGS_RELEASE "-O2 -DNDEBUG")
if(NOT EMSCRIPTEN)
  add_compile_options(
    $<$<CONFIG:Debug>:-fsanitize=address,undefined>
    $<$<CONFIG:Debug>:-fno-omit-frame-pointer>)
  add_link_options($<$<CONFIG:Debug>:-fsanitize=address,undefined>)
endif()

include(FetchContent)
FetchContent_Declare(imgui
  URL https://github.com/ocornut/imgui/archive/refs/tags/v1.92.9b.tar.gz)

if(EMSCRIPTEN)
  FetchContent_MakeAvailable(imgui)
  # Emscripten's built-in SDL3 port stands in for the Homebrew package.
  add_library(sdl3_port INTERFACE)
  target_compile_options(sdl3_port INTERFACE -sUSE_SDL=3)
  target_link_options(sdl3_port INTERFACE -sUSE_SDL=3)
  add_library(SDL3::SDL3 ALIAS sdl3_port)
  set(IMGUI_RENDERER_BACKEND ${imgui_SOURCE_DIR}/backends/imgui_impl_opengl3.cpp)
  set(TETRIS_GFX src/app/gfx_webgl.cpp)
  set(TETRIS_NET src/net/ws_client_web.cpp)
  set(TETRIS_STORE src/app/settings_store_web.cpp)
else()
  FetchContent_Declare(doctest
    URL https://github.com/doctest/doctest/archive/refs/tags/v2.4.12.tar.gz)
  FetchContent_MakeAvailable(imgui doctest)
  find_package(SDL3 REQUIRED CONFIG)
  set(IMGUI_RENDERER_BACKEND ${imgui_SOURCE_DIR}/backends/imgui_impl_sdlgpu3.cpp)
  set(TETRIS_GFX src/app/gfx_sdlgpu.cpp)
  set(TETRIS_NET src/net/connection_none.cpp)
  set(TETRIS_STORE src/app/settings_store_desktop.cpp)
endif()

add_library(imgui STATIC
  ${imgui_SOURCE_DIR}/imgui.cpp
  ${imgui_SOURCE_DIR}/imgui_demo.cpp
  ${imgui_SOURCE_DIR}/imgui_draw.cpp
  ${imgui_SOURCE_DIR}/imgui_tables.cpp
  ${imgui_SOURCE_DIR}/imgui_widgets.cpp
  ${imgui_SOURCE_DIR}/backends/imgui_impl_sdl3.cpp
  ${IMGUI_RENDERER_BACKEND})
target_include_directories(imgui SYSTEM PUBLIC ${imgui_SOURCE_DIR} ${imgui_SOURCE_DIR}/backends)
target_link_libraries(imgui PUBLIC SDL3::SDL3)

set(TETRIS_WARNINGS -Wall -Wextra -Wpedantic)

add_library(tetris_core STATIC
  src/core/piece.cpp
  src/core/board.cpp
  src/core/srs.cpp
  src/core/bag.cpp
  src/core/game.cpp)
target_include_directories(tetris_core PUBLIC src)
target_compile_options(tetris_core PRIVATE ${TETRIS_WARNINGS})

add_library(tetris_audio STATIC
  src/audio/synth.cpp
  src/audio/mixer.cpp
  src/audio/music.cpp)
target_include_directories(tetris_audio PUBLIC src)
target_compile_options(tetris_audio PRIVATE ${TETRIS_WARNINGS})

add_library(tetris_input STATIC
  src/app/input.cpp)
target_link_libraries(tetris_input PUBLIC tetris_core)
target_compile_options(tetris_input PRIVATE ${TETRIS_WARNINGS})

add_library(tetris_duel STATIC
  src/net/protocol.cpp
  src/app/duel.cpp
  src/app/solo.cpp
  src/app/sound_cues.cpp)
target_include_directories(tetris_duel PUBLIC src)
target_link_libraries(tetris_duel PUBLIC tetris_core tetris_audio)
target_compile_options(tetris_duel PRIVATE ${TETRIS_WARNINGS})

add_library(tetris_settings STATIC
  src/app/keymap.cpp
  src/app/settings.cpp)
target_include_directories(tetris_settings PUBLIC src)
target_link_libraries(tetris_settings PUBLIC tetris_input tetris_duel SDL3::SDL3)
target_compile_options(tetris_settings PRIVATE ${TETRIS_WARNINGS})

add_executable(tetris
  src/app/main.cpp
  src/app/app.cpp
  src/app/fonts.cpp
  src/app/render.cpp
  src/app/audio_out.cpp
  ${TETRIS_GFX}
  ${TETRIS_NET}
  ${TETRIS_STORE})
target_include_directories(tetris PRIVATE src)
target_link_libraries(tetris PRIVATE tetris_duel tetris_audio tetris_input tetris_settings imgui SDL3::SDL3)
target_compile_options(tetris PRIVATE ${TETRIS_WARNINGS})

if(EMSCRIPTEN)
  # index.html so the Go server's "/" serves the game.
  set_target_properties(tetris PROPERTIES OUTPUT_NAME index SUFFIX ".html")
  target_link_options(tetris PRIVATE
    -sALLOW_MEMORY_GROWTH=1
    -lwebsocket.js
    --shell-file ${CMAKE_SOURCE_DIR}/web/shell.html)
  set_property(TARGET tetris APPEND PROPERTY LINK_DEPENDS ${CMAKE_SOURCE_DIR}/web/shell.html)
  return()  # tests run natively only
endif()

enable_testing()
add_executable(tetris_tests
  tests/main_test.cpp
  tests/piece_test.cpp
  tests/board_test.cpp
  tests/srs_test.cpp
  tests/bag_test.cpp
  tests/game_test.cpp
  tests/input_test.cpp
  tests/protocol_test.cpp
  tests/duel_test.cpp
  tests/solo_test.cpp
  tests/audio_test.cpp
  tests/sound_cues_test.cpp
  tests/keymap_test.cpp
  tests/settings_test.cpp)
target_include_directories(tetris_tests PRIVATE tests)
target_link_libraries(tetris_tests PRIVATE tetris_input tetris_duel tetris_audio tetris_settings doctest::doctest)
target_compile_options(tetris_tests PRIVATE ${TETRIS_WARNINGS})
add_test(NAME tetris_tests COMMAND tetris_tests)

add_executable(tetris_ui_tests
  tests/main_test.cpp
  tests/ui_test.cpp
  src/app/render.cpp)
target_include_directories(tetris_ui_tests PRIVATE src tests)
target_link_libraries(tetris_ui_tests PRIVATE tetris_input tetris_duel tetris_settings imgui doctest::doctest)
target_compile_options(tetris_ui_tests PRIVATE ${TETRIS_WARNINGS})
add_test(NAME tetris_ui_tests COMMAND tetris_ui_tests)
