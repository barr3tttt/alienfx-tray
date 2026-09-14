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
