find_package(Qt6 REQUIRED COMPONENTS Core Network Test)

qt_standard_project_setup()

# Every test is guiless: they run in CI and over SSH with no display.
function(alienfx_add_test name)
  qt_add_executable(${name} ${name}.cpp)
  target_link_libraries(${name} PRIVATE ${ARGN} Qt6::Test)
  target_include_directories(${name} PRIVATE
    ${CMAKE_SOURCE_DIR}/src
    ${CMAKE_CURRENT_SOURCE_DIR})
  add_test(NAME ${name} COMMAND ${name})
endfunction()

# The guard around the command that leaves the keyboard dark until a reboot. If this one
# ever fails, stop and read tests/test_effect_guard.cpp before changing anything.
alienfx_add_test(test_effect_guard alienfx_core)

alienfx_add_test(test_lighting_engine alienfx_core)
alienfx_add_test(test_config alienfx_core)
alienfx_add_test(test_startup_integration alienfx_app)
alienfx_add_test(test_cli_options alienfx_app)

# What --probe-external paints. The mode is how the external light IDs are mapped to physical
# fixtures at all, so a property leaking in from the active scheme yields a wrong answer the
# user then writes down as fact.
alienfx_add_test(test_probe_mode alienfx_app)

# That a wedged device thread reaches the UI rather than hiding behind a stale Busy. It
# drives a real LightingController against a device layer that genuinely hangs, so it needs
# room for a couple of real watchdog periods but must never be allowed to hang for good.
alienfx_add_test(test_watchdog_escalation alienfx_app)
set_tests_properties(test_watchdog_escalation PROPERTIES TIMEOUT 120)

# The kernel-driver reattach reporting. tests/e2e/kernel-driver-reattach.sh drives the real
# binary and asserts the happy-path sysfs end state, so it cannot reach the error branch.
alienfx_add_test(test_usb_reattach alienfx_core)

# The guard on the device thread's containment: it wedges a worker inside a device call and
# stops it. A join anywhere on that path makes this test hang rather than fail, so it gets a
# timeout well under ctest's default.
alienfx_add_test(test_device_thread alienfx_app)
set_tests_properties(test_device_thread PROPERTIES TIMEOUT 60)
