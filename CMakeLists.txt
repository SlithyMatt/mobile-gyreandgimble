# Gyre & Gimble — Android, iOS and desktop (for testing)
#
#   Desktop:  cmake -S . -B build && cmake --build build && ./build/gyre
#   iOS:      cmake -S . -B build-ios -G Xcode -DCMAKE_SYSTEM_NAME=iOS (see README)
#   Android:  built by Gradle from android/ (see README)
#
# The game data is generated at build time from the x16 submodule by running
# the X16 repo's own tools (tools/export_data.py), so it needs Python 3.
cmake_minimum_required(VERSION 3.20)
if(CMAKE_SYSTEM_NAME STREQUAL "iOS" AND NOT CMAKE_OSX_DEPLOYMENT_TARGET)
  set(CMAKE_OSX_DEPLOYMENT_TARGET "13.0" CACHE STRING "Minimum iOS version")
endif()
project(GyreAndGimble VERSION 0.2.0 LANGUAGES C CXX)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_CXX_STANDARD 14)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

set(X16_DIR ${CMAKE_CURRENT_SOURCE_DIR}/x16)
if(NOT EXISTS ${X16_DIR}/tools/genrings.py)
  message(FATAL_ERROR "The x16 submodule is missing: git submodule update --init --recursive")
endif()

# --- game data, from the X16 repo -----------------------------------------
find_package(Python3 3.8 REQUIRED COMPONENTS Interpreter)
set(GG_DATA_DIR ${CMAKE_CURRENT_BINARY_DIR}/generated)
file(GLOB GG_X16_INPUTS CONFIGURE_DEPENDS
  ${X16_DIR}/tools/*.py
  ${X16_DIR}/assets/*.txt
  ${X16_DIR}/assets/music/*.txt
  ${X16_DIR}/levels/*.txt)
add_custom_command(
  OUTPUT ${GG_DATA_DIR}/gg_data.c ${GG_DATA_DIR}/gg_data.h
  COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/tools/export_data.py
          ${X16_DIR} ${GG_DATA_DIR} ${Python3_EXECUTABLE}
  DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/tools/export_data.py ${GG_X16_INPUTS}
  COMMENT "Building the game data from the x16 submodule"
  VERBATIM)
add_custom_target(gg_data DEPENDS ${GG_DATA_DIR}/gg_data.c ${GG_DATA_DIR}/gg_data.h)

# --- SDL2 -------------------------------------------------------------------
option(GG_SYSTEM_SDL "Use an installed SDL2 instead of third_party/SDL (desktop only)" OFF)
if(GG_SYSTEM_SDL)
  find_package(SDL2 REQUIRED CONFIG)
else()
  if(NOT EXISTS ${CMAKE_CURRENT_SOURCE_DIR}/third_party/SDL/CMakeLists.txt)
    message(FATAL_ERROR "The SDL submodule is missing: git submodule update --init --recursive")
  endif()
  set(SDL_TEST OFF CACHE BOOL "" FORCE)
  if(ANDROID)
    # SDLActivity loads libSDL2.so, then the game's libmain.so
    set(SDL_SHARED ON CACHE BOOL "" FORCE)
    set(SDL_STATIC OFF CACHE BOOL "" FORCE)
  else()
    set(SDL_SHARED OFF CACHE BOOL "" FORCE)
    set(SDL_STATIC ON CACHE BOOL "" FORCE)
  endif()
  add_subdirectory(third_party/SDL EXCLUDE_FROM_ALL)
endif()

# --- ymfm (YM2151) --------------------------------------------------------------
add_library(ymfm STATIC third_party/ymfm/src/ymfm_opm.cpp)
target_include_directories(ymfm PUBLIC third_party/ymfm/src)

# --- the game ---------------------------------------------------------------------
set(GG_SOURCES
  src/main.c
  src/game.c
  src/touch.c
  src/vera.c
  src/audio.c
  src/music.c
  src/ym.cpp
  ${GG_DATA_DIR}/gg_data.c)

if(ANDROID)
  add_library(main SHARED ${GG_SOURCES})
  set(GG_TARGET main)
elseif(IOS)
  set(GG_TARGET GyreAndGimble)
  add_executable(${GG_TARGET} MACOSX_BUNDLE ${GG_SOURCES} ios/LaunchScreen.storyboard ios/Assets.xcassets)
  set_source_files_properties(ios/LaunchScreen.storyboard ios/Assets.xcassets
    PROPERTIES MACOSX_PACKAGE_LOCATION Resources)
  set(GG_BUNDLE_ID "com.slithymatt.gyreandgimble" CACHE STRING "iOS bundle identifier")
  set(GG_TEAM_ID "" CACHE STRING "Apple development team ID, for code signing")
  set_target_properties(${GG_TARGET} PROPERTIES
    OUTPUT_NAME "GyreAndGimble"
    MACOSX_BUNDLE_INFO_PLIST ${CMAKE_CURRENT_SOURCE_DIR}/ios/Info.plist.in
    MACOSX_BUNDLE_GUI_IDENTIFIER ${GG_BUNDLE_ID}
    MACOSX_BUNDLE_BUNDLE_NAME "GyreAndGimble"
    MACOSX_BUNDLE_BUNDLE_VERSION ${PROJECT_VERSION}
    MACOSX_BUNDLE_SHORT_VERSION_STRING ${PROJECT_VERSION}
    XCODE_ATTRIBUTE_PRODUCT_BUNDLE_IDENTIFIER ${GG_BUNDLE_ID}
    XCODE_ATTRIBUTE_TARGETED_DEVICE_FAMILY "1,2"
    XCODE_ATTRIBUTE_IPHONEOS_DEPLOYMENT_TARGET ${CMAKE_OSX_DEPLOYMENT_TARGET}
    XCODE_ATTRIBUTE_ASSETCATALOG_COMPILER_APPICON_NAME "AppIcon"
    XCODE_ATTRIBUTE_DEVELOPMENT_TEAM "${GG_TEAM_ID}"
    XCODE_ATTRIBUTE_CODE_SIGN_STYLE "Automatic")
else()
  set(GG_TARGET gyre)
  add_executable(${GG_TARGET} ${GG_SOURCES})
endif()

add_dependencies(${GG_TARGET} gg_data)
target_include_directories(${GG_TARGET} PRIVATE src ${GG_DATA_DIR})
target_link_libraries(${GG_TARGET} PRIVATE ymfm)
if(TARGET SDL2::SDL2main)
  target_link_libraries(${GG_TARGET} PRIVATE SDL2::SDL2main)
endif()
if(ANDROID OR GG_SYSTEM_SDL)
  target_link_libraries(${GG_TARGET} PRIVATE SDL2::SDL2)
else()
  target_link_libraries(${GG_TARGET} PRIVATE SDL2::SDL2-static)
endif()
if(ANDROID)
  target_link_libraries(${GG_TARGET} PRIVATE log android)
endif()
if(MSVC)
  target_compile_options(${GG_TARGET} PRIVATE /W3)
else()
  target_compile_options(${GG_TARGET} PRIVATE -Wall -Wextra)
endif()
if(NOT MSVC AND NOT WIN32)
  target_link_libraries(${GG_TARGET} PRIVATE m)
endif()

# --- tests (desktop) ----------------------------------------------------------------
# joytest replays the X16 repo's scripted pad (x16/tools/joyscript.py) through
# every level to the ending, and solves level 1 by touch
if(NOT ANDROID AND NOT IOS)
  enable_testing()
  file(GLOB GG_LEVELS CONFIGURE_DEPENDS ${X16_DIR}/levels/*.txt)
  list(SORT GG_LEVELS)
  add_custom_command(
    OUTPUT ${GG_DATA_DIR}/joyscript.inc
    COMMAND ${Python3_EXECUTABLE} -B ${X16_DIR}/tools/joyscript.py ${GG_DATA_DIR}/joyscript.inc ${GG_LEVELS}
    DEPENDS ${X16_DIR}/tools/joyscript.py ${X16_DIR}/tools/level.py ${GG_LEVELS}
    WORKING_DIRECTORY ${X16_DIR}
    VERBATIM)
  add_custom_target(gg_joyscript DEPENDS ${GG_DATA_DIR}/joyscript.inc)
  add_executable(joytest EXCLUDE_FROM_ALL tests/joytest.c
    src/game.c src/touch.c src/vera.c src/audio.c src/music.c src/ym.cpp ${GG_DATA_DIR}/gg_data.c)
  add_dependencies(joytest gg_data gg_joyscript)
  target_include_directories(joytest PRIVATE src ${GG_DATA_DIR})
  target_link_libraries(joytest PRIVATE ymfm)
  if(NOT MSVC AND NOT WIN32)
    target_link_libraries(joytest PRIVATE m)
  endif()
  execute_process(
    COMMAND ${Python3_EXECUTABLE} -B ${X16_DIR}/tools/level.py keys ${X16_DIR}/levels/level001.txt
    OUTPUT_VARIABLE GG_LEVEL1_KEYS OUTPUT_STRIP_TRAILING_WHITESPACE)
  add_test(NAME joytest COMMAND joytest ${GG_DATA_DIR}/joyscript.inc ${GG_LEVEL1_KEYS})
endif()
