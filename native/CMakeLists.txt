cmake_minimum_required(VERSION 3.28...3.30)

project(bambu-printer-overlay VERSION 0.2.0 LANGUAGES C)

option(BAMBU_OVERLAY_BUILD_PLUGIN "Build the libobs plugin module" ON)
option(BAMBU_OVERLAY_BUILD_TESTS "Build OBS-independent core tests" ON)
option(
  BAMBU_OVERLAY_BOOTSTRAP_OBS
  "Download the pinned official OBS build dependencies when libobs is unavailable"
  ON
)

add_library(bambu-overlay-core STATIC src/printer-snapshot.c)
target_include_directories(bambu-overlay-core PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/src")
target_compile_features(bambu-overlay-core PUBLIC c_std_11)
set_target_properties(bambu-overlay-core PROPERTIES POSITION_INDEPENDENT_CODE ON)

if(MSVC)
  target_compile_options(bambu-overlay-core PRIVATE /W4 /WX)
else()
  target_compile_options(bambu-overlay-core PRIVATE -Wall -Wextra -Wpedantic -Werror)
endif()

if(BAMBU_OVERLAY_BUILD_PLUGIN)
  if(WIN32 AND BAMBU_OVERLAY_BOOTSTRAP_OBS)
    set(dependencies_dir "${CMAKE_CURRENT_SOURCE_DIR}/.deps")
    file(READ "${CMAKE_CURRENT_SOURCE_DIR}/buildspec.json" bambu_buildspec)
    string(JSON bambu_prebuilt_version GET ${bambu_buildspec} dependencies prebuilt version)
    string(JSON bambu_obs_version GET ${bambu_buildspec} dependencies obs-studio version)
    set(obs_runtime_dependency_dir "${dependencies_dir}/obs-deps-${bambu_prebuilt_version}-x64/bin")
    set(obs_license_path "${dependencies_dir}/obs-studio-${bambu_obs_version}/COPYING")
  endif()

  if(WIN32 AND BAMBU_OVERLAY_BOOTSTRAP_OBS AND NOT libobs_DIR)
    include(FetchContent)
    FetchContent_Declare(
      obs_plugin_template
      GIT_REPOSITORY https://github.com/obsproject/obs-plugintemplate.git
      GIT_TAG 3e7d7ac3b5342cd7d9b88890b9c70b472d1520fc
      GIT_SHALLOW FALSE
      SOURCE_SUBDIR _dependency_only
    )
    FetchContent_MakeAvailable(obs_plugin_template)

    list(
      APPEND
      CMAKE_MODULE_PATH
      "${obs_plugin_template_SOURCE_DIR}/cmake/common"
      "${obs_plugin_template_SOURCE_DIR}/cmake/windows"
    )
    set(OS_WINDOWS TRUE)
    include(buildspec_common)

    set(arch ${CMAKE_VS_PLATFORM_NAME})
    if(NOT arch)
      set(arch x64)
    endif()
    set(platform windows-${arch})
    set(prebuilt_filename "windows-deps-VERSION-ARCH-REVISION.zip")
    set(prebuilt_destination "obs-deps-VERSION-ARCH")
    set(obs-studio_filename "VERSION.zip")
    set(obs-studio_destination "obs-studio-VERSION")
    set(dependencies_list prebuilt obs-studio)
    _check_dependencies()
    set(obs_runtime_dependency_dir "${dependencies_dir}/obs-deps-${bambu_prebuilt_version}-${arch}/bin")
  endif()

  find_package(libobs REQUIRED)

  add_library(bambu-printer-overlay MODULE
    src/plugin-main.c
    src/bambu-overlay-source.c
  )
  target_compile_features(bambu-printer-overlay PRIVATE c_std_11)
  target_link_libraries(bambu-printer-overlay PRIVATE OBS::libobs bambu-overlay-core)
  target_compile_definitions(
    bambu-printer-overlay
    PRIVATE BAMBU_OVERLAY_VERSION="${PROJECT_VERSION}"
  )
  set_target_properties(bambu-printer-overlay PROPERTIES PREFIX "" OUTPUT_NAME "bambu-printer-overlay")

  if(MSVC)
    target_compile_options(bambu-printer-overlay PRIVATE /W4 /WX)
  else()
    target_compile_options(bambu-printer-overlay PRIVATE -Wall -Wextra -Wpedantic -Werror)
  endif()

  if(MSVC)
    configure_file(windows/resource.rc.in "${CMAKE_CURRENT_BINARY_DIR}/bambu-printer-overlay.rc" @ONLY)
    target_sources(bambu-printer-overlay PRIVATE "${CMAKE_CURRENT_BINARY_DIR}/bambu-printer-overlay.rc")
  endif()

  install(
    TARGETS bambu-printer-overlay
    RUNTIME DESTINATION "bambu-printer-overlay/bin/64bit"
    LIBRARY DESTINATION "bambu-printer-overlay/bin/64bit"
  )
  install(DIRECTORY data/ DESTINATION "bambu-printer-overlay/data")
  install(FILES README.md DESTINATION "bambu-printer-overlay/data")
  install(FILES "${CMAKE_CURRENT_SOURCE_DIR}/../LICENSE" DESTINATION "bambu-printer-overlay/data/licenses")
  install(
    FILES "${CMAKE_CURRENT_SOURCE_DIR}/../THIRD_PARTY_NOTICES.md"
    DESTINATION "bambu-printer-overlay/data/licenses"
  )
  if(EXISTS "${obs_license_path}")
    install(
      FILES "${obs_license_path}"
      DESTINATION "bambu-printer-overlay/data/licenses"
      RENAME "GPL-2.0-or-later.txt"
    )
  elseif(WIN32 AND BAMBU_OVERLAY_BOOTSTRAP_OBS)
    message(FATAL_ERROR "The pinned OBS licence file is missing: ${obs_license_path}")
  endif()
endif()

if(BAMBU_OVERLAY_BUILD_TESTS)
  include(CTest)
  add_executable(bambu-overlay-core-tests tests/test-printer-snapshot.c)
  target_link_libraries(bambu-overlay-core-tests PRIVATE bambu-overlay-core)
  add_test(NAME bambu-overlay-core-tests COMMAND bambu-overlay-core-tests)

  if(TARGET bambu-printer-overlay)
    add_executable(bambu-overlay-plugin-load-tests tests/test-plugin-load.c)
    target_compile_features(bambu-overlay-plugin-load-tests PRIVATE c_std_11)
    target_link_libraries(bambu-overlay-plugin-load-tests PRIVATE OBS::libobs)
    add_dependencies(bambu-overlay-plugin-load-tests bambu-printer-overlay)
    add_test(
      NAME bambu-overlay-plugin-load-tests
      COMMAND
        bambu-overlay-plugin-load-tests
        "$<TARGET_FILE:bambu-printer-overlay>"
        "${CMAKE_CURRENT_SOURCE_DIR}/data"
    )
    if(WIN32 AND dependencies_dir)
      set_tests_properties(
        bambu-overlay-plugin-load-tests
        PROPERTIES
          ENVIRONMENT_MODIFICATION
            "PATH=path_list_prepend:${dependencies_dir}/bin/64bit;PATH=path_list_prepend:${obs_runtime_dependency_dir}"
      )
    endif()
  endif()
endif()
