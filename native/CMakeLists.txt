cmake_minimum_required(VERSION 3.28...3.30)

project(bambu-printer-overlay VERSION 0.1.0 LANGUAGES C)

option(BAMBU_OVERLAY_BUILD_PLUGIN "Build the libobs plugin module" ON)
option(BAMBU_OVERLAY_BUILD_TESTS "Build OBS-independent core tests" ON)

add_library(bambu-overlay-core STATIC src/printer-snapshot.c)
target_include_directories(bambu-overlay-core PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/src")
target_compile_features(bambu-overlay-core PUBLIC c_std_11)

if(MSVC)
  target_compile_options(bambu-overlay-core PRIVATE /W4 /WX)
else()
  target_compile_options(bambu-overlay-core PRIVATE -Wall -Wextra -Wpedantic -Werror)
endif()

if(BAMBU_OVERLAY_BUILD_PLUGIN)
  find_package(libobs REQUIRED)

  add_library(bambu-printer-overlay MODULE
    src/plugin-main.c
    src/bambu-overlay-source.c
  )
  target_compile_features(bambu-printer-overlay PRIVATE c_std_11)
  target_link_libraries(bambu-printer-overlay PRIVATE OBS::libobs bambu-overlay-core)
  set_target_properties(bambu-printer-overlay PROPERTIES PREFIX "" OUTPUT_NAME "bambu-printer-overlay")

  if(MSVC)
    target_compile_options(bambu-printer-overlay PRIVATE /W4 /WX)
  else()
    target_compile_options(bambu-printer-overlay PRIVATE -Wall -Wextra -Wpedantic -Werror)
  endif()

  install(
    TARGETS bambu-printer-overlay
    RUNTIME DESTINATION "bambu-printer-overlay/bin/64bit"
    LIBRARY DESTINATION "bambu-printer-overlay/bin/64bit"
  )
  install(DIRECTORY data/ DESTINATION "bambu-printer-overlay/data")
endif()

if(BAMBU_OVERLAY_BUILD_TESTS)
  include(CTest)
  add_executable(bambu-overlay-core-tests tests/test-printer-snapshot.c)
  target_link_libraries(bambu-overlay-core-tests PRIVATE bambu-overlay-core)
  add_test(NAME bambu-overlay-core-tests COMMAND bambu-overlay-core-tests)
endif()
