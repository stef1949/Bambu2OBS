cmake_minimum_required(VERSION 3.28...3.30)

project(bambu-printer-overlay VERSION 0.3.0 LANGUAGES C)

option(BAMBU_OVERLAY_BUILD_PLUGIN "Build the libobs plugin module" ON)
option(BAMBU_OVERLAY_BUILD_TESTS "Build OBS-independent tests" ON)
option(
  BAMBU_OVERLAY_BOOTSTRAP_OBS
  "Download the pinned official OBS build dependencies when libobs is unavailable"
  ON
)

function(bambu_apply_warnings target)
  if(MSVC)
    # C4127: Winsock FD_SET expands to a constant condition.
    target_compile_options(${target} PRIVATE /W4 /WX /wd4127)
    target_compile_definitions(${target} PRIVATE _CRT_SECURE_NO_WARNINGS _WINSOCK_DEPRECATED_NO_WARNINGS)
  else()
    target_compile_options(${target} PRIVATE -Wall -Wextra -Wpedantic -Werror)
  endif()
endfunction()

# Portable core: protocol, state, layout, and compositing. No OBS, no platform API.
add_library(
  bambu-overlay-core
  STATIC
  src/bambu-format.c
  src/bambu-json.c
  src/bambu-log.c
  src/bambu-maps.c
  src/bambu-mqtt-codec.c
  src/bambu-preview-select.c
  src/bambu-report.c
  src/bambu-simulation.c
  src/bambu-util.c
  src/bambu-zip.c
  src/overlay-canvas.c
  src/overlay-render.c
  src/printer-snapshot.c
)
target_include_directories(bambu-overlay-core PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/src")
target_compile_features(bambu-overlay-core PUBLIC c_std_11)
set_target_properties(bambu-overlay-core PROPERTIES POSITION_INDEPENDENT_CODE ON)
bambu_apply_warnings(bambu-overlay-core)
if(NOT WIN32)
  target_link_libraries(bambu-overlay-core PUBLIC m)
endif()

# Windows services: TLS, FTPS, HTTP, credentials, imaging, text, the printer link.
# These use only components of Windows, which is why the plugin ships as one DLL.
if(WIN32)
  add_library(
    bambu-overlay-win
    STATIC
    src/bambu-credentials-win.c
    src/bambu-ftps-win.c
    src/bambu-http-win.c
    src/bambu-image-win.c
    src/bambu-profiles.c
    src/bambu-tls-win.c
    src/printer-service.c
    src/text-gdi-win.c
  )
  target_link_libraries(
    bambu-overlay-win
    PUBLIC
      bambu-overlay-core
      advapi32
      bcrypt
      crypt32
      gdi32
      ole32
      secur32
      windowscodecs
      winhttp
      ws2_32
  )
  bambu_apply_warnings(bambu-overlay-win)
endif()

if(BAMBU_OVERLAY_BUILD_PLUGIN)
  # Only the settings dialog is C++, because Qt is.
  enable_language(CXX)

  if(WIN32 AND BAMBU_OVERLAY_BOOTSTRAP_OBS)
    set(dependencies_dir "${CMAKE_CURRENT_SOURCE_DIR}/.deps")
    file(READ "${CMAKE_CURRENT_SOURCE_DIR}/buildspec.json" bambu_buildspec)
    string(JSON bambu_prebuilt_version GET ${bambu_buildspec} dependencies prebuilt version)
    string(JSON bambu_obs_version GET ${bambu_buildspec} dependencies obs-studio version)
    string(JSON bambu_qt_version GET ${bambu_buildspec} dependencies qt6 version)
    set(obs_runtime_dependency_dir "${dependencies_dir}/obs-deps-${bambu_prebuilt_version}-x64/bin")
    set(obs_license_path "${dependencies_dir}/obs-studio-${bambu_obs_version}/COPYING")
    set(obs_qt_dir "${dependencies_dir}/obs-deps-qt6-${bambu_qt_version}-x64")
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
    set(qt6_filename "windows-deps-qt6-VERSION-ARCH-REVISION.zip")
    set(qt6_destination "obs-deps-qt6-VERSION-ARCH")
    set(obs-studio_filename "VERSION.zip")
    set(obs-studio_destination "obs-studio-VERSION")
    set(dependencies_list prebuilt qt6 obs-studio)
    _check_dependencies()
    set(obs_runtime_dependency_dir "${dependencies_dir}/obs-deps-${bambu_prebuilt_version}-${arch}/bin")
  endif()

  find_package(libobs REQUIRED)
  find_package(obs-frontend-api REQUIRED)
  find_package(Qt6 REQUIRED COMPONENTS Core Gui Widgets)

  add_library(
    bambu-printer-overlay
    MODULE
    src/bambu-overlay-source.c
    src/bambu-settings-qt.cpp
    src/plugin-main.c
  )
  target_compile_features(bambu-printer-overlay PRIVATE c_std_11 cxx_std_17)
  target_link_libraries(
    bambu-printer-overlay
    PRIVATE OBS::libobs OBS::obs-frontend-api Qt6::Core Qt6::Gui Qt6::Widgets bambu-overlay-win shell32 user32
  )
  target_compile_definitions(bambu-printer-overlay PRIVATE BAMBU_OVERLAY_VERSION="${PROJECT_VERSION}")
  set_target_properties(bambu-printer-overlay PROPERTIES PREFIX "" OUTPUT_NAME "bambu-printer-overlay")
  bambu_apply_warnings(bambu-printer-overlay)

  if(MSVC)
    configure_file(windows/resource.rc.in "${CMAKE_CURRENT_BINARY_DIR}/bambu-printer-overlay.rc" @ONLY)
    target_sources(bambu-printer-overlay PRIVATE "${CMAKE_CURRENT_BINARY_DIR}/bambu-printer-overlay.rc")
  endif()

  # OBS 33 layout. bin/64bit is flagged Legacy: https://obsproject.com/kb/legacy-plugin-locations
  install(
    TARGETS bambu-printer-overlay
    RUNTIME DESTINATION "bambu-printer-overlay"
    LIBRARY DESTINATION "bambu-printer-overlay"
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

  function(bambu_add_test name)
    add_executable(${name} ${ARGN})
    target_include_directories(${name} PRIVATE tests)
    bambu_apply_warnings(${name})
  endfunction()

  bambu_add_test(bambu-overlay-core-tests tests/test-printer-snapshot.c)
  target_link_libraries(bambu-overlay-core-tests PRIVATE bambu-overlay-core)
  add_test(NAME bambu-overlay-core-tests COMMAND bambu-overlay-core-tests)

  bambu_add_test(bambu-overlay-protocol-tests tests/test-protocol.c)
  target_link_libraries(bambu-overlay-protocol-tests PRIVATE bambu-overlay-core)
  add_test(NAME bambu-overlay-protocol-tests COMMAND bambu-overlay-protocol-tests)

  bambu_add_test(bambu-overlay-render-tests tests/test-render.c)
  target_link_libraries(bambu-overlay-render-tests PRIVATE bambu-overlay-core)
  add_test(NAME bambu-overlay-render-tests COMMAND bambu-overlay-render-tests)

  if(WIN32)
    bambu_add_test(bambu-overlay-gallery tests/render-gallery.c)
    target_link_libraries(bambu-overlay-gallery PRIVATE bambu-overlay-win)

    bambu_add_test(bambu-overlay-service-tests tests/test-service.c)
    target_link_libraries(bambu-overlay-service-tests PRIVATE bambu-overlay-win)

    # Drives the real TLS, MQTT, and FTPS client code against a local fake printer.
    find_package(Python3 COMPONENTS Interpreter)
    find_program(BAMBU_OPENSSL_PROGRAM openssl)
    if(Python3_Interpreter_FOUND AND BAMBU_OPENSSL_PROGRAM)
      add_test(
        NAME bambu-overlay-service-tests
        COMMAND
          "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/tests/fake_printer.py"
          --openssl "${BAMBU_OPENSSL_PROGRAM}"
          --client "$<TARGET_FILE:bambu-overlay-service-tests>"
      )
      set_tests_properties(bambu-overlay-service-tests PROPERTIES TIMEOUT 180)
    endif()
  endif()

  if(TARGET bambu-printer-overlay)
    # Runs the real plugin inside real libobs. BAMBU_OVERLAY_TEST_OBS_DIR may point
    # at an installed OBS, to check compatibility with that exact build.
    set(BAMBU_OVERLAY_TEST_OBS_DIR "" CACHE PATH "Optional OBS install to run the load test against")
    add_executable(bambu-overlay-plugin-load-tests tests/test-plugin-load.c)
    target_compile_features(bambu-overlay-plugin-load-tests PRIVATE c_std_11)
    target_include_directories(bambu-overlay-plugin-load-tests PRIVATE tests)
    target_compile_definitions(bambu-overlay-plugin-load-tests PRIVATE _CRT_SECURE_NO_WARNINGS)
    target_link_libraries(bambu-overlay-plugin-load-tests PRIVATE OBS::libobs psapi)
    add_dependencies(bambu-overlay-plugin-load-tests bambu-printer-overlay)

    if(BAMBU_OVERLAY_TEST_OBS_DIR)
      set(bambu_test_runtime "${BAMBU_OVERLAY_TEST_OBS_DIR}/bin/64bit")
      set(bambu_test_libobs_data "${BAMBU_OVERLAY_TEST_OBS_DIR}/data/libobs/")
      set(bambu_test_qt_plugins "${BAMBU_OVERLAY_TEST_OBS_DIR}/bin/64bit")
    elseif(dependencies_dir)
      set(bambu_test_runtime "${dependencies_dir}/bin/64bit;${obs_runtime_dependency_dir};${obs_qt_dir}/bin")
      set(bambu_test_libobs_data "${dependencies_dir}/obs-studio-${bambu_obs_version}/libobs/data/")
      set(bambu_test_qt_plugins "${obs_qt_dir}/plugins")
      set(bambu_test_qt_bin "${obs_qt_dir}/bin")
    endif()

    # The real dialog, with the plugin's source file but no running OBS.
    add_executable(bambu-overlay-dialog-tests tests/test-settings-dialog.cpp src/bambu-settings-qt.cpp)
    target_compile_features(bambu-overlay-dialog-tests PRIVATE cxx_std_17)
    target_include_directories(bambu-overlay-dialog-tests PRIVATE tests)
    target_compile_definitions(bambu-overlay-dialog-tests PRIVATE BAMBU_OVERLAY_VERSION="${PROJECT_VERSION}")
    target_link_libraries(
      bambu-overlay-dialog-tests
      PRIVATE OBS::libobs OBS::obs-frontend-api Qt6::Core Qt6::Gui Qt6::Widgets bambu-overlay-win shell32 user32
    )
    bambu_apply_warnings(bambu-overlay-dialog-tests)

    if(bambu_test_runtime)
      add_test(NAME bambu-overlay-dialog-tests COMMAND bambu-overlay-dialog-tests)
      set(bambu_dialog_tests bambu-overlay-dialog-tests)
      # The certificate trust flow needs a real TLS server.
      if(Python3_Interpreter_FOUND AND BAMBU_OPENSSL_PROGRAM)
        add_test(
          NAME bambu-overlay-dialog-trust-tests
          COMMAND
            "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/tests/fake_printer.py"
            --openssl "${BAMBU_OPENSSL_PROGRAM}"
            --client "$<TARGET_FILE:bambu-overlay-dialog-tests>"
        )
        set_tests_properties(bambu-overlay-dialog-trust-tests PROPERTIES TIMEOUT 120)
        list(APPEND bambu_dialog_tests bambu-overlay-dialog-trust-tests)
      endif()
      add_test(
        NAME bambu-overlay-plugin-load-tests
        COMMAND
          bambu-overlay-plugin-load-tests
          "$<TARGET_FILE:bambu-printer-overlay>"
          "${CMAKE_CURRENT_SOURCE_DIR}/data"
          "${bambu_test_libobs_data}"
          "${CMAKE_CURRENT_BINARY_DIR}/test-obs-config"
      )
      set(bambu_test_path "")
      foreach(entry IN LISTS bambu_test_runtime)
        list(APPEND bambu_test_path "PATH=path_list_prepend:${entry}")
      endforeach()
      list(APPEND bambu_test_path "BAMBU_TEST_QT_BIN=set:${bambu_test_qt_bin}")
      set_tests_properties(
        bambu-overlay-plugin-load-tests
        PROPERTIES ENVIRONMENT_MODIFICATION "${bambu_test_path}"
      )
      set_tests_properties(
        ${bambu_dialog_tests}
        PROPERTIES
          ENVIRONMENT_MODIFICATION
            "${bambu_test_path};QT_PLUGIN_PATH=set:${bambu_test_qt_plugins};BAMBU_DIALOG_LOCALE=set:${CMAKE_CURRENT_SOURCE_DIR}/data/locale/en-US.ini;BAMBU_DIALOG_TEMP=set:${CMAKE_CURRENT_BINARY_DIR}"
      )
    endif()
  endif()
endif()
