cmake_minimum_required(VERSION 3.22)

# Prefer an Xcode-bundled macOS SDK when the command-line-tools SDK is incomplete.
# Must be set before project() so the compiler probe uses the right sysroot.
if(APPLE AND NOT CMAKE_OSX_SYSROOT AND NOT DEFINED ENV{SDKROOT})
  execute_process(COMMAND bash -c "ls -d /Applications/Xcode*.app/Contents/Developer/Platforms/MacOSX.platform/Developer/SDKs/MacOSX*.sdk 2>/dev/null | sort -V | tail -1"
                  OUTPUT_VARIABLE SP_XCODE_SDK OUTPUT_STRIP_TRAILING_WHITESPACE)
  if(EXISTS "${SP_XCODE_SDK}")
    set(CMAKE_OSX_SYSROOT "${SP_XCODE_SDK}" CACHE STRING "" FORCE)
    message(STATUS "Using macOS SDK: ${SP_XCODE_SDK}")
  endif()
endif()

project(SnakePuppyReferencePlayer VERSION 0.1.0 LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_OSX_DEPLOYMENT_TARGET "13.0" CACHE STRING "" FORCE)

option(SP_BUILD_UNIVERSAL "Build universal (arm64 + x86_64) binary" OFF)
option(SP_LTO "Enable link-time optimisation (slow; release packaging only)" OFF)
if(SP_BUILD_UNIVERSAL)
  set(CMAKE_OSX_ARCHITECTURES "arm64;x86_64" CACHE STRING "" FORCE)
else()
  set(CMAKE_OSX_ARCHITECTURES "arm64" CACHE STRING "" FORCE)
endif()

add_subdirectory(JUCE)

#== Shared JUCE definitions ===================================================
set(SP_JUCE_DEFINITIONS
    JUCE_WEB_BROWSER=0
    JUCE_USE_CURL=0
    JUCE_DISPLAY_SPLASH_SCREEN=0
    JUCE_USE_MP3AUDIOFORMAT=1
    JUCE_USE_FLAC=1
    JUCE_USE_OGGVORBIS=1
    JUCE_STRICT_REFCOUNTEDPOINTER=1
    JUCE_MODAL_LOOPS_PERMITTED=1
    JUCE_REPORT_APP_USAGE=0)

#== Source lists ==============================================================
file(GLOB_RECURSE SP_CORE_SOURCES CONFIGURE_DEPENDS
     "${CMAKE_SOURCE_DIR}/Source/Core/*.cpp"
     "${CMAKE_SOURCE_DIR}/Source/DSP/*.cpp"
     "${CMAKE_SOURCE_DIR}/Source/Audio/*.cpp"
     "${CMAKE_SOURCE_DIR}/Source/Library/*.cpp"
     "${CMAKE_SOURCE_DIR}/Source/Playlist/*.cpp"
     "${CMAKE_SOURCE_DIR}/Source/Profile/*.cpp"
     "${CMAKE_SOURCE_DIR}/Source/Network/*.cpp")

file(GLOB_RECURSE SP_UI_SOURCES CONFIGURE_DEPENDS
     "${CMAKE_SOURCE_DIR}/Source/UI/*.cpp"
     "${CMAKE_SOURCE_DIR}/Source/UI/*.mm")

file(GLOB SP_TEST_SOURCES CONFIGURE_DEPENDS
     "${CMAKE_SOURCE_DIR}/Tests/*.cpp")

#== Application ===============================================================
juce_add_gui_app(SnakePuppyReferencePlayer
    PRODUCT_NAME "Snake Puppy Reference Player"
    BUNDLE_ID "com.snakepuppy.referenceplayer"
    VERSION "0.1.0"
    COMPANY_NAME "Snake Puppy Audio"
    ICON_BIG "${CMAKE_SOURCE_DIR}/assets/icon_1024.png"
    COPY_PLUGIN_ARTIFACTS FALSE)

juce_generate_juce_header(SnakePuppyReferencePlayer)

target_sources(SnakePuppyReferencePlayer PRIVATE
    "${CMAKE_SOURCE_DIR}/Source/Main.cpp"
    ${SP_CORE_SOURCES}
    ${SP_UI_SOURCES})

target_include_directories(SnakePuppyReferencePlayer PRIVATE "${CMAKE_SOURCE_DIR}/Source")

target_compile_definitions(SnakePuppyReferencePlayer PRIVATE
    ${SP_JUCE_DEFINITIONS}
    SP_SOURCE_DIR="${CMAKE_SOURCE_DIR}"
    JUCE_APPLICATION_NAME_STRING="$<TARGET_PROPERTY:SnakePuppyReferencePlayer,JUCE_PRODUCT_NAME>"
    JUCE_APPLICATION_VERSION_STRING="$<TARGET_PROPERTY:SnakePuppyReferencePlayer,JUCE_VERSION>")

target_link_libraries(SnakePuppyReferencePlayer PRIVATE
    juce::juce_audio_utils
    juce::juce_dsp
    juce::juce_audio_devices
    juce::juce_audio_formats
    juce::juce_gui_extra
    juce::juce_cryptography
    juce::juce_recommended_config_flags
    juce::juce_recommended_warning_flags
    "-lsqlite3")

if(SP_LTO)
  target_link_libraries(SnakePuppyReferencePlayer PRIVATE juce::juce_recommended_lto_flags)
endif()

#== Headless test harness =====================================================
juce_add_console_app(SnakePuppyTests PRODUCT_NAME "Snake Puppy Tests")
juce_generate_juce_header(SnakePuppyTests)

target_sources(SnakePuppyTests PRIVATE
    ${SP_CORE_SOURCES}
    ${SP_TEST_SOURCES})

target_include_directories(SnakePuppyTests PRIVATE
    "${CMAKE_SOURCE_DIR}/Source"
    "${CMAKE_SOURCE_DIR}/Tests")

target_compile_definitions(SnakePuppyTests PRIVATE
    ${SP_JUCE_DEFINITIONS}
    SP_SOURCE_DIR="${CMAKE_SOURCE_DIR}")

target_link_libraries(SnakePuppyTests PRIVATE
    juce::juce_audio_utils
    juce::juce_dsp
    juce::juce_audio_devices
    juce::juce_audio_formats
    juce::juce_gui_extra
    juce::juce_cryptography
    juce::juce_recommended_config_flags
    juce::juce_recommended_warning_flags
    "-lsqlite3")

#== Convenience: copy app to dist/ ===========================================
add_custom_target(SP_BuildApp ALL DEPENDS SnakePuppyReferencePlayer)
add_custom_target(SP_RunTests ALL DEPENDS SnakePuppyTests)
