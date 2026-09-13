# The vendored AlienFX SDK declares libusb, hidapi, loguru and nlohmann/json with
# FetchContent pointing at `master`/`main`. Unpinned upstreams mean the build breaks on
# somebody else's Tuesday, and a tray app that controls a laptop keyboard is not a place
# for surprise dependency churn.
#
# FetchContent uses the FIRST declaration it sees for a given name, so declaring the same
# four names here - before add_subdirectory() reaches the SDK - pins them without patching
# the submodule. The names must match the SDK's spelling exactly.
#
# The commits below are the ones verified to build and drive both controllers on an
# Alienware m15 R7 AMD.

include(FetchContent)

# Allow an offline or distro build to substitute local sources:
#   cmake -DFETCHCONTENT_SOURCE_DIR_HIDAPI=/path/to/hidapi ...

FetchContent_Declare(
  libusb
  GIT_REPOSITORY https://github.com/libusb/libusb-cmake.git
  GIT_TAG bea6567b63796ab27c1c33257d230284fb2d8316
  GIT_SHALLOW FALSE)

FetchContent_Declare(
  hidapi
  GIT_REPOSITORY https://github.com/libusb/hidapi.git
  GIT_TAG 852cc68b8e0e4c7eba0815ef992951cd8b75c55b
  GIT_SHALLOW FALSE)

FetchContent_Declare(
  LoguruGitRepo
  GIT_REPOSITORY https://github.com/emilk/loguru.git
  GIT_TAG v2.2.0
  GIT_SHALLOW TRUE)

FetchContent_Declare(
  json
  GIT_REPOSITORY https://github.com/nlohmann/json.git
  GIT_TAG v3.12.0
  GIT_SHALLOW TRUE)
