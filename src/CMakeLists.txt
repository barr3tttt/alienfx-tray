# --------------------------------------------------------------------------------------
# alienfx_core - no Qt, no USB, no SDK. Pure lighting model, fully testable on any machine.
# --------------------------------------------------------------------------------------
add_library(alienfx_core STATIC
  core/color.cpp
  core/device_interface.cpp
  core/lighting_engine.cpp
  core/scheme.cpp
  core/config.cpp)

target_include_directories(alienfx_core PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(alienfx_core PUBLIC nlohmann_json::nlohmann_json)

# --------------------------------------------------------------------------------------
# alienfx_device - the only place AlienFX_SDK.h is included. It does `using namespace std;`
# at file scope, so it is confined to two translation units behind the core interfaces.
# --------------------------------------------------------------------------------------
add_library(alienfx_device STATIC
  device/usb_context.cpp
  device/sdk_devices.cpp
  device/device_factory.cpp)

target_link_libraries(alienfx_device PUBLIC alienfx_core PRIVATE AlienFX_SDK)

add_subdirectory(app)
