find_package(Qt6 REQUIRED COMPONENTS Core DBus Network Test Widgets)

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

# The editing model the captain asked for: manual edits preview live but never overwrite a
# saved scheme without an explicit Save, and selecting a scheme sets every zone every time.
# Drives a real LightingController against recording fakes, so the config file on disk and
# the wire commands are both asserted.
alienfx_add_test(test_editing_model alienfx_app)

# The panel half of the same model: the widgets are real, driven offscreen, and the unsaved
# strip, Save, Revert, close-to-revert and scheme-list activation are all exercised.
alienfx_add_test(test_control_panel alienfx_ui Qt6::Widgets)

# The hardware check for the editing model: drives a real LightingController through every
# scheme against the real controllers and lists every zone's commanded colour. Not a ctest
# test - it needs the laptop. tests/e2e/scheme-apply-all-zones.sh runs it.
qt_add_executable(scheme_apply_driver e2e/scheme_apply_driver.cpp)
target_link_libraries(scheme_apply_driver PRIVATE alienfx_app)
target_include_directories(scheme_apply_driver PRIVATE
  ${CMAKE_SOURCE_DIR}/src
  ${CMAKE_CURRENT_SOURCE_DIR})
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

# The headless retry poll: a domain that has settled is handed back at once rather than held
# claimed while the poll waits for the other controller.
alienfx_add_test(test_retry_poll alienfx_app)

# The exit codes the login unit and the resume hook read, and the rule that a mode which
# painted nothing never prints guidance as though it had. Holds the device lock throughout,
# so it stands down before opening anything and is safe to run on the real laptop.
alienfx_add_test(test_apply_modes alienfx_app)

# The frame a contended device lock used to swallow. It holds the lock from the test, so it
# spends a couple of real retry delays waiting for it to free.
alienfx_add_test(test_device_worker_lock alienfx_app)
set_tests_properties(test_device_worker_lock PROPERTIES TIMEOUT 120)

# The kernel-driver reattach reporting. tests/e2e/kernel-driver-reattach.sh drives the real
# binary and asserts the happy-path sysfs end state, so it cannot reach the error branch.
alienfx_add_test(test_usb_reattach alienfx_core)

# The guard on the device thread's containment: it wedges a worker inside a device call and
# stops it. A join anywhere on that path makes this test hang rather than fail, so it gets a
# timeout well under ctest's default.
alienfx_add_test(test_device_thread alienfx_app)
set_tests_properties(test_device_thread PROPERTIES TIMEOUT 60)
