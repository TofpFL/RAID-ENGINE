cmake_minimum_required(VERSION 3.20)
project(GodsHand VERSION 1.0.0 LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
  set(CMAKE_BUILD_TYPE Release CACHE STRING "" FORCE)
endif()

option(GH_BUILD_GAME "Build the game executable (needs raylib)" ON)
option(GH_BUILD_TESTS "Build the headless test executable" ON)

find_package(Threads REQUIRED)

# Static MSVC runtime: the game runs without installing the VC++ redistributable.
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")

# ---- engine core: pure C++, no window/audio dependency ----------------------
add_library(gh_core STATIC
  src/json.cpp
  src/chart.cpp
  src/session.cpp
  src/synth.cpp
  src/composer.cpp
)
target_include_directories(gh_core PUBLIC src)
target_link_libraries(gh_core PUBLIC Threads::Threads)
if(MSVC)
  target_compile_options(gh_core PRIVATE /W3 /utf-8)
else()
  target_compile_options(gh_core PRIVATE -Wall -Wextra -Wno-unused-parameter)
endif()

if(GH_BUILD_TESTS)
  add_executable(gh_tests tests/tests.cpp)
  target_link_libraries(gh_tests PRIVATE gh_core)
  enable_testing()
  add_test(NAME core COMMAND gh_tests)
endif()

# ---- the game ----------------------------------------------------------------
if(GH_BUILD_GAME)
  include(FetchContent)
  set(BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
  set(BUILD_GAMES OFF CACHE BOOL "" FORCE)
  set(CUSTOMIZE_BUILD OFF CACHE BOOL "" FORCE)
  # Pinned raylib release. Set FETCHCONTENT_SOURCE_DIR_RAYLIB to use a local copy.
  FetchContent_Declare(raylib
    GIT_REPOSITORY https://github.com/raysan5/raylib.git
    GIT_TAG 5.5
    GIT_SHALLOW TRUE)
  FetchContent_MakeAvailable(raylib)
  # The main loop polls input ~1000 times per second and only renders at the
  # frame limit, so raylib must not swap/poll/sleep inside EndDrawing().
  target_compile_definitions(raylib PRIVATE SUPPORT_CUSTOM_FRAME_CONTROL=1)

  add_executable(godshand WIN32
    src/main.cpp
    src/app.cpp
    src/audio.cpp
    src/input.cpp
    src/settings.cpp
    src/library.cpp
    src/gfx.cpp
    src/characters.cpp
    src/stage.cpp
    src/states_menu.cpp
    src/states_options.cpp
    src/state_play.cpp
    src/state_results.cpp
  )
  target_link_libraries(godshand PRIVATE gh_core raylib)
  if(MSVC)
    target_compile_options(godshand PRIVATE /W3 /utf-8)
    target_link_options(godshand PRIVATE /ENTRY:mainCRTStartup)
  else()
    target_compile_options(godshand PRIVATE -Wall -Wextra -Wno-unused-parameter -Wno-missing-field-initializers)
  endif()
  if(MINGW)
    target_link_options(godshand PRIVATE -static -static-libgcc -static-libstdc++)
  endif()
  if(APPLE)
    target_link_libraries(godshand PRIVATE "-framework IOKit" "-framework Cocoa" "-framework OpenGL")
  endif()

  # Runtime layout next to the executable: assets/, songs/, mods/
  add_custom_command(TARGET godshand POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_directory ${CMAKE_SOURCE_DIR}/assets $<TARGET_FILE_DIR:godshand>/assets
    COMMAND ${CMAKE_COMMAND} -E copy_directory ${CMAKE_SOURCE_DIR}/songs $<TARGET_FILE_DIR:godshand>/songs
    COMMAND ${CMAKE_COMMAND} -E make_directory $<TARGET_FILE_DIR:godshand>/mods)

  install(TARGETS godshand RUNTIME DESTINATION .)
  install(DIRECTORY assets songs DESTINATION .)
  install(FILES README.md LICENSE DESTINATION .)
endif()
