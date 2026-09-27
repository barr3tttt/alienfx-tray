find_package(Qt6 REQUIRED COMPONENTS Core Gui Network Widgets Svg)

qt_standard_project_setup()

# Everything that needs no display, in one place. The headless `--apply` modes live here
# because the resume hook runs them without a session, and the device thread lives here
# because the test suite has to be able to drive it without a GUI or hardware.
add_library(alienfx_app STATIC
  apply_mode.cpp
  cli_options.cpp
  device_thread.cpp
  device_worker.cpp
  lighting_controller.cpp
  metatypes.cpp
  process_lock.cpp
  signal_handler.cpp
  single_instance.cpp
  startup_integration.cpp)

target_link_libraries(alienfx_app PUBLIC
  alienfx_device
  Qt6::Core
  # QLocalServer/QLocalSocket, for the single-instance guard. Two tray processes fighting
  # over one USB interface is exactly the failure that leaves a keyboard claimed.
  Qt6::Network)

target_include_directories(alienfx_app PUBLIC ${CMAKE_SOURCE_DIR}/src)

# The GUI and the headless modes share one binary. `--apply` never constructs a QApplication,
# so it works from the resume hook, which has no display and no session bus.
qt_add_executable(alienfx-tray
  ../main.cpp
  ../ui/color_picker.cpp
  ../ui/control_panel.cpp
  ../ui/tray_icon.cpp)

# Single-source the version string from the project() declaration, and bake in the git
# revision so --version can answer "what am I running" - a stale install was once
# indistinguishable from a current one, and the difference mattered. The revision is looked
# up on every build rather than at configure time; see cmake/GitRevision.cmake.
find_package(Git QUIET)
set(ALIENFX_GIT_REVISION_HEADER "${CMAKE_CURRENT_BINARY_DIR}/generated/git_revision.h")
add_custom_target(alienfx_git_revision
  COMMAND ${CMAKE_COMMAND}
    -DGIT_EXECUTABLE=${GIT_EXECUTABLE}
    -DSOURCE_DIR=${CMAKE_SOURCE_DIR}
    -DOUTPUT=${ALIENFX_GIT_REVISION_HEADER}
    -P ${CMAKE_SOURCE_DIR}/cmake/GitRevision.cmake
  BYPRODUCTS ${ALIENFX_GIT_REVISION_HEADER}
  COMMENT "Recording the git revision"
  VERBATIM)
add_dependencies(alienfx-tray alienfx_git_revision)
target_include_directories(alienfx-tray PRIVATE ${CMAKE_CURRENT_BINARY_DIR}/generated)
target_compile_definitions(alienfx-tray PRIVATE
  ALIENFX_TRAY_VERSION="${PROJECT_VERSION}")

qt_add_resources(alienfx-tray "app_resources"
  PREFIX "/icons"
  BASE "${CMAKE_SOURCE_DIR}/packaging"
  FILES "${CMAKE_SOURCE_DIR}/packaging/alienfx-tray.svg")

target_link_libraries(alienfx-tray PRIVATE
  alienfx_app
  Qt6::Widgets
  # Qt6::Svg is not used directly - it provides the image plugin that renders the tray icon.
  Qt6::Svg)

install(TARGETS alienfx-tray RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
