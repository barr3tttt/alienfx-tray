# Run at build time, not configure time, by the alienfx_git_revision target: a configure-time
# lookup is baked in until something makes CMake re-run, so a rebuild after a commit or an
# edit would still report the old revision - the very staleness --version exists to expose.
#
# Inputs: GIT_EXECUTABLE (may be empty), SOURCE_DIR, OUTPUT.
# file(CONFIGURE) rewrites OUTPUT only when its contents change, so an unchanged revision
# does not recompile anything.

set(ALIENFX_TRAY_GIT_SHA "unknown")
if(GIT_EXECUTABLE)
  execute_process(
    COMMAND ${GIT_EXECUTABLE} rev-parse --short HEAD
    WORKING_DIRECTORY ${SOURCE_DIR}
    OUTPUT_VARIABLE sha
    OUTPUT_STRIP_TRAILING_WHITESPACE
    RESULT_VARIABLE sha_result
    ERROR_QUIET)
  # A build from a tarball or an exported tree has no revision to report; say so rather
  # than printing an empty string.
  if(sha_result EQUAL 0 AND sha)
    set(ALIENFX_TRAY_GIT_SHA "${sha}")
    # Two binaries built from different uncommitted edits of one commit are not the same
    # build, so a tracked-file change is marked.
    execute_process(
      COMMAND ${GIT_EXECUTABLE} status --porcelain --untracked-files=no
      WORKING_DIRECTORY ${SOURCE_DIR}
      OUTPUT_VARIABLE dirty
      OUTPUT_STRIP_TRAILING_WHITESPACE
      ERROR_QUIET)
    if(dirty)
      string(APPEND ALIENFX_TRAY_GIT_SHA "-dirty")
    endif()
  endif()
endif()

file(CONFIGURE OUTPUT "${OUTPUT}"
  CONTENT "#pragma once\n#define ALIENFX_TRAY_GIT_SHA \"@ALIENFX_TRAY_GIT_SHA@\"\n"
  @ONLY)
