cmake_minimum_required(VERSION 3.21)
project(alienfx-tray VERSION 0.1.0 LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR})

option(ALIENFX_TRAY_BUILD_TESTS "Build the test suite" ON)

include(GNUInstallDirs)
include(cmake/PinnedDependencies.cmake)

# The SDK submodule. ALIENFX_BUILD_CLI/EXAMPLE stay OFF: we link the library, we do not
# shell out to the CLI.
if(NOT EXISTS ${CMAKE_CURRENT_SOURCE_DIR}/third_party/alienfx-linux/CMakeLists.txt)
  message(FATAL_ERROR
    "third_party/alienfx-linux is empty. Run: git submodule update --init --recursive")
endif()
add_subdirectory(third_party/alienfx-linux EXCLUDE_FROM_ALL)

add_subdirectory(src)

if(ALIENFX_TRAY_BUILD_TESTS)
  enable_testing()
  add_subdirectory(tests)
endif()

install(FILES packaging/alienfx-tray.desktop
        DESTINATION ${CMAKE_INSTALL_DATADIR}/applications)
install(FILES packaging/alienfx-tray.svg
        DESTINATION ${CMAKE_INSTALL_DATADIR}/icons/hicolor/scalable/apps)
