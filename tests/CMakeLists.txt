find_package(Qt6 REQUIRED COMPONENTS Core Test)

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
