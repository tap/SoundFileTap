cmake_minimum_required(VERSION 3.16)
project(SoundFileTap VERSION 0.1.0 LANGUAGES C)

# FetchContent-friendly: no global state, no compiler flags leaking to parents
# (warnings are PRIVATE), options default off unless this is the top level.
if(CMAKE_SOURCE_DIR STREQUAL CMAKE_CURRENT_SOURCE_DIR)
    set(tap_soundfile_is_top_level ON)
else()
    set(tap_soundfile_is_top_level OFF)
endif()

option(TAP_SOUNDFILE_BUILD_TESTS "Build unit tests" ${tap_soundfile_is_top_level})
option(TAP_SOUNDFILE_BUILD_FUZZERS "Build libFuzzer fuzz targets (needs Clang)" OFF)
option(TAP_SOUNDFILE_NO_STDIO "Memory/callback IO only: no stdio, no tap_sf_open_file" OFF)
option(TAP_SOUNDFILE_INSTALL "Generate install rules and config package" ${tap_soundfile_is_top_level})

add_library(tap_soundfile src/soundfile.c)
add_library(tap::soundfile ALIAS tap_soundfile)

set_target_properties(tap_soundfile PROPERTIES C_STANDARD 11 C_STANDARD_REQUIRED ON C_EXTENSIONS OFF)
target_include_directories(tap_soundfile PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
    $<INSTALL_INTERFACE:include>)
# The vendored decoder is an implementation detail; consumers never see it.
target_include_directories(tap_soundfile PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/third_party)

if(TAP_SOUNDFILE_NO_STDIO)
    target_compile_definitions(tap_soundfile PUBLIC TAP_SOUNDFILE_NO_STDIO)
endif()

if(MSVC)
    target_compile_options(tap_soundfile PRIVATE /W4)
else()
    target_compile_options(tap_soundfile PRIVATE -Wall -Wextra -Wconversion -Wno-sign-conversion)
endif()

if(TAP_SOUNDFILE_INSTALL)
    include(GNUInstallDirs)
    include(CMakePackageConfigHelpers)
    install(TARGETS tap_soundfile EXPORT tap_soundfile-targets
        ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR}
        LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}
        RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
    install(DIRECTORY include/tap DESTINATION ${CMAKE_INSTALL_INCLUDEDIR})
    install(EXPORT tap_soundfile-targets NAMESPACE tap::
        DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/tap_soundfile)
    configure_package_config_file(cmake/tap_soundfile-config.cmake.in
        ${CMAKE_CURRENT_BINARY_DIR}/tap_soundfile-config.cmake
        INSTALL_DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/tap_soundfile)
    write_basic_package_version_file(
        ${CMAKE_CURRENT_BINARY_DIR}/tap_soundfile-config-version.cmake
        COMPATIBILITY SameMajorVersion)
    install(FILES
        ${CMAKE_CURRENT_BINARY_DIR}/tap_soundfile-config.cmake
        ${CMAKE_CURRENT_BINARY_DIR}/tap_soundfile-config-version.cmake
        DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/tap_soundfile)
endif()

if(TAP_SOUNDFILE_BUILD_TESTS)
    enable_language(CXX)
    enable_testing()
    add_subdirectory(tests)
endif()

if(TAP_SOUNDFILE_BUILD_FUZZERS)
    add_subdirectory(fuzz)
endif()
