cmake_minimum_required(VERSION 3.20)
project(GodsHand VERSION 1.5.0 LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
  set(CMAKE_BUILD_TYPE Release CACHE STRING "" FORCE)
endif()

option(GH_BUILD_GAME "Build the game executable (needs raylib)" ON)
if(ANDROID)
  # Android (NDK toolchain): the game is libgodshand.so inside an APK, see android/.
  option(GH_BUILD_TESTS "Build the headless test executable" OFF)
else()
  option(GH_BUILD_TESTS "Build the headless test executable" ON)
endif()

find_package(Threads REQUIRED)

# Static MSVC runtime: the game runs without installing the VC++ redistributable.
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")

include(FetchContent)

# The complete source package ships every library in third_party/: use those
# copies instead of downloading (the game then builds without internet).
foreach(dep IN ITEMS lua miniz pl_mpeg raylib)
  string(TOUPPER ${dep} dep_upper)
  if(NOT FETCHCONTENT_SOURCE_DIR_${dep_upper} AND IS_DIRECTORY ${CMAKE_SOURCE_DIR}/third_party/${dep})
    set(FETCHCONTENT_SOURCE_DIR_${dep_upper} ${CMAKE_SOURCE_DIR}/third_party/${dep})
    message(STATUS "Using third_party/${dep}")
  endif()
endforeach()

# ---- Lua 5.4 (mod scripts) -----------------------------------------------------
# Compiled as C++ so script errors unwind with exceptions instead of longjmp.
# Set FETCHCONTENT_SOURCE_DIR_LUA to use a local checkout.
FetchContent_Declare(lua
  GIT_REPOSITORY https://github.com/lua/lua.git
  GIT_TAG v5.4.7
  GIT_SHALLOW TRUE)
FetchContent_MakeAvailable(lua)  # no CMakeLists.txt upstream: this only downloads
set(GH_LUA_SOURCES)
foreach(f lapi lauxlib lbaselib lcode lcorolib lctype ldebug ldo ldump lfunc lgc llex lmathlib lmem
          lobject lopcodes lparser lstate lstring lstrlib ltable ltablib ltm lundump lutf8lib lvm lzio)
  list(APPEND GH_LUA_SOURCES ${lua_SOURCE_DIR}/${f}.c)
endforeach()
set_source_files_properties(${GH_LUA_SOURCES} PROPERTIES LANGUAGE CXX)
add_library(gh_lua STATIC ${GH_LUA_SOURCES})
target_include_directories(gh_lua PUBLIC ${lua_SOURCE_DIR})
if(MSVC)
  target_compile_options(gh_lua PRIVATE /TP /W0)
  target_compile_definitions(gh_lua PRIVATE _CRT_SECURE_NO_WARNINGS)
else()
  target_compile_options(gh_lua PRIVATE -w)
endif()

# ---- miniz (installing mods from .zip) ------------------------------------------
# Set FETCHCONTENT_SOURCE_DIR_MINIZ to use a local checkout.
FetchContent_Declare(miniz
  GIT_REPOSITORY https://github.com/richgel999/miniz.git
  GIT_TAG 3.0.2
  GIT_SHALLOW TRUE
  SOURCE_SUBDIR do-not-add)  # we compile the four sources ourselves
FetchContent_MakeAvailable(miniz)
add_library(gh_miniz STATIC
  ${miniz_SOURCE_DIR}/miniz.c ${miniz_SOURCE_DIR}/miniz_tdef.c
  ${miniz_SOURCE_DIR}/miniz_tinfl.c ${miniz_SOURCE_DIR}/miniz_zip.c)
target_include_directories(gh_miniz PUBLIC ${miniz_SOURCE_DIR} ${CMAKE_SOURCE_DIR}/cmake)
if(MSVC)
  target_compile_definitions(gh_miniz PRIVATE _CRT_SECURE_NO_WARNINGS)
else()
  target_compile_options(gh_miniz PRIVATE -w)
endif()

# ---- pl_mpeg (MPEG-1 video for cutscenes) -----------------------------------------
# Set FETCHCONTENT_SOURCE_DIR_PL_MPEG to use a local checkout.
FetchContent_Declare(pl_mpeg
  GIT_REPOSITORY https://github.com/phoboslab/pl_mpeg.git
  GIT_TAG c871f2be022ece7ef4f64230b4fb8e1fb9eb6023
  SOURCE_SUBDIR do-not-add)  # single header, included by src/video.cpp
FetchContent_MakeAvailable(pl_mpeg)

# ---- engine core: pure C++, no window/audio dependency ----------------------
add_library(gh_core STATIC
  src/json.cpp
  src/chart.cpp
  src/chart_stream.cpp
  src/basegame.cpp
  src/shader_glsl.cpp
  src/effects.cpp
  src/hscript.cpp
  src/hscript_bridge.cpp
  src/session.cpp
  src/synth.cpp
  src/composer.cpp
  src/psych.cpp
  src/scene.cpp
  src/song_scan.cpp
  src/flp_import.cpp
  src/lua_host.cpp
  src/replay.cpp
  src/mod_install.cpp
  src/autochart.cpp
)
target_include_directories(gh_core PUBLIC src)
target_link_libraries(gh_core PUBLIC Threads::Threads gh_lua gh_miniz)
if(MSVC)
  target_compile_options(gh_core PRIVATE /W3 /utf-8)
else()
  target_compile_options(gh_core PRIVATE -Wall -Wextra -Wno-unused-parameter)
endif()

if(GH_BUILD_TESTS)
  add_executable(gh_tests tests/tests.cpp tests/psych_tests.cpp tests/scan_tests.cpp)
  target_link_libraries(gh_tests PRIVATE gh_core)
  enable_testing()
  add_test(NAME core COMMAND gh_tests)
endif()

# ---- the game ----------------------------------------------------------------
if(GH_BUILD_GAME)
  # Fonts (SIL OFL) ship in assets/fonts; copies of the source without them
  # fetch the exact same files here. The game falls back to a built-in font.
  set(GH_FONT_COMMIT 5bb28733cec313b63fbd415b574b5ae35c8aa8b8)
  set(GH_FONTS
    "CinzelDecorative-Bold.ttf|ofl/cinzeldecorative/CinzelDecorative-Bold.ttf|e854e68a388aa50d742a4415c1ae5c17a617ef7956c95a70021d0a4a44f20518"
    "Rajdhani-Bold.ttf|ofl/rajdhani/Rajdhani-Bold.ttf|691470dd3286a14e9677940d0bf75796179841ba5215cbda1a2c8910a3226afd")
  foreach(entry IN LISTS GH_FONTS)
    string(REPLACE "|" ";" parts "${entry}")
    list(GET parts 0 font_name)
    list(GET parts 1 font_path)
    list(GET parts 2 font_hash)
    set(font_file ${CMAKE_SOURCE_DIR}/assets/fonts/${font_name})
    if(NOT EXISTS ${font_file})
      message(STATUS "Downloading font ${font_name}")
      file(DOWNLOAD https://raw.githubusercontent.com/google/fonts/${GH_FONT_COMMIT}/${font_path} ${font_file}
        EXPECTED_HASH SHA256=${font_hash} STATUS font_status)
      list(GET font_status 0 font_code)
      if(NOT font_code EQUAL 0)
        file(REMOVE ${font_file})
        message(WARNING "Could not download ${font_name}; the game will use its fallback font.")
      endif()
    endif()
  endforeach()

  set(BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
  set(BUILD_GAMES OFF CACHE BOOL "" FORCE)
  set(CUSTOMIZE_BUILD OFF CACHE BOOL "" FORCE)
  if(ANDROID)
    set(PLATFORM "Android" CACHE STRING "" FORCE)  # raylib's NativeActivity + OpenGL ES 2 backend
  endif()
  # Pinned raylib release. Set FETCHCONTENT_SOURCE_DIR_RAYLIB to use a local copy.
  FetchContent_Declare(raylib
    GIT_REPOSITORY https://github.com/raysan5/raylib.git
    GIT_TAG 5.5
    GIT_SHALLOW TRUE)
  FetchContent_MakeAvailable(raylib)
  # src/audio_decode.cpp calls the decoders raylib compiles in (stb_vorbis,
  # dr_mp3, dr_wav): their headers are in raylib's external/ folder.
  if(EXISTS ${raylib_SOURCE_DIR}/src/external/dr_mp3.h)
    set(GH_RAYLIB_EXTERNAL ${raylib_SOURCE_DIR}/src/external)
  else()
    set(GH_RAYLIB_EXTERNAL ${raylib_SOURCE_DIR}/external)
  endif()
  set_source_files_properties(src/audio_decode.cpp PROPERTIES INCLUDE_DIRECTORIES ${GH_RAYLIB_EXTERNAL})
  # The main loop polls input ~1000 times per second and only renders at the
  # frame limit, so raylib must not swap/poll/sleep inside EndDrawing().
  target_compile_definitions(raylib PRIVATE SUPPORT_CUSTOM_FRAME_CONTROL=1)
  if(ANDROID)
    # Android: make raylib follow the window when it changes size (see
    # cmake/raylib_android_resize.c). Applied once, in raylib's own source.
    set(gh_rcore ${raylib_SOURCE_DIR}/src/platforms/rcore_android.c)
    file(READ ${gh_rcore} gh_src)
    if(NOT gh_src MATCHES "GhFollowWindowSize")
      file(READ ${CMAKE_SOURCE_DIR}/cmake/raylib_android_resize.c gh_fn)
      set(gh_def "static void AndroidCommandCallback(struct android_app *app, int32_t cmd)\n{")
      string(REPLACE "${gh_def}" "${gh_fn}${gh_def}" gh_src "${gh_src}")
      string(REPLACE "        case APP_CMD_CONFIG_CHANGED:"
        "        case APP_CMD_WINDOW_RESIZED:\n        case APP_CMD_CONTENT_RECT_CHANGED:\n        case APP_CMD_CONFIG_CHANGED:"
        gh_src "${gh_src}")
      string(REPLACE "            // Check screen orientation here!" "            GhFollowWindowSize(app);" gh_src "${gh_src}")
      string(REPLACE "platform.contextRebindRequired = false;"
        "platform.contextRebindRequired = false;\n                    GhFollowWindowSize(app);" gh_src "${gh_src}")
      string(REGEX MATCHALL "GhFollowWindowSize\\(app\\)" gh_calls "${gh_src}")
      list(LENGTH gh_calls gh_count)
      if(NOT gh_count EQUAL 2 OR NOT gh_src MATCHES "case APP_CMD_WINDOW_RESIZED")
        message(FATAL_ERROR "Could not patch ${gh_rcore} (raylib 5.5 expected)")
      endif()
      file(WRITE ${gh_rcore} "${gh_src}")
      message(STATUS "Patched raylib's rcore_android.c to follow window size changes")
    endif()
  endif()

  set(GH_GAME_SOURCES
    src/main.cpp
    src/app.cpp
    src/audio.cpp
    src/audio_decode.cpp
    src/input.cpp
    src/settings.cpp
    src/library.cpp
    src/gfx.cpp
    src/characters.cpp
    src/stage.cpp
    src/states_menu.cpp
    src/states_options.cpp
    src/state_play.cpp
    src/play_psych.cpp
    src/scene_draw.cpp
    src/video.cpp
    src/state_results.cpp
    src/state_editor.cpp
    src/state_modpacks.cpp
    src/process.cpp
    src/platform.cpp
    src/folder_picker.cpp
    src/psych_ui.cpp
    src/touch_ui.cpp
  )
  if(ANDROID)
    # NativeActivity loads libgodshand.so; raylib's android_main() calls main().
    add_library(godshand SHARED ${GH_GAME_SOURCES})
    target_include_directories(godshand PRIVATE ${ANDROID_NDK}/sources/android/native_app_glue)
    target_link_libraries(godshand PRIVATE gh_core raylib android log EGL GLESv2 OpenSLES m)
    target_link_options(godshand PRIVATE -u ANativeActivity_onCreate -Wl,--no-undefined -Wl,-z,max-page-size=16384)
  else()
    add_executable(godshand WIN32 ${GH_GAME_SOURCES})
  endif()
  if(WIN32)
    # UTF-8 as the process code page (Windows 10 1903+): files and folders
    # named in any script (mods with Chinese, Japanese... names) open.
    if(MSVC)
      target_sources(godshand PRIVATE src/windows/godshand.manifest)
    else()
      target_sources(godshand PRIVATE src/windows/godshand.rc)
    endif()
  endif()
  target_link_libraries(godshand PRIVATE gh_core raylib)
  target_compile_definitions(godshand PRIVATE GH_VERSION="${PROJECT_VERSION}")
  target_include_directories(godshand PRIVATE ${pl_mpeg_SOURCE_DIR})
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
  if(WIN32)
    target_link_libraries(godshand PRIVATE shell32 ole32)  # the folder dialog
  endif()

  # Runtime layout next to the executable: assets/, songs/, mods/
  if(NOT ANDROID)
    add_custom_command(TARGET godshand POST_BUILD
      COMMAND ${CMAKE_COMMAND} -E copy_directory ${CMAKE_SOURCE_DIR}/assets $<TARGET_FILE_DIR:godshand>/assets
      COMMAND ${CMAKE_COMMAND} -E copy_directory ${CMAKE_SOURCE_DIR}/songs $<TARGET_FILE_DIR:godshand>/songs
      COMMAND ${CMAKE_COMMAND} -E make_directory $<TARGET_FILE_DIR:godshand>/mods)

    install(TARGETS godshand RUNTIME DESTINATION .)
    install(DIRECTORY assets songs DESTINATION .)
    install(FILES README.md LICENSE DESTINATION .)
  endif()
endif()

# Streamed song audio is tested with raylib's decoders when raylib is built.
if(GH_BUILD_TESTS AND TARGET raylib)
  target_sources(gh_tests PRIVATE tests/audio_tests.cpp src/audio_decode.cpp)
  target_link_libraries(gh_tests PRIVATE raylib)
  target_compile_definitions(gh_tests PRIVATE GH_AUDIO_TESTS=1)
endif()
