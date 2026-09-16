cmake_minimum_required(VERSION 3.25)
project(LaptopMonitor VERSION 0.2.0 LANGUAGES C CXX)
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
include(CTest)
option(LM_BUILD_HOST "Build the Windows application, helper and benchmark" ON)
set(LM_DEFAULT_SIGNALING_URL "https://browser-monitor-signaling.danielruoqiao.workers.dev" CACHE STRING
    "Signaling endpoint compiled in as the default (changeable in Settings)")
set(LM_VIEWER_URL "https://danielw412.github.io/Laptop-as-a-second-monitor/" CACHE STRING
    "Receiver page shown to the user")
# nlohmann/json ships inside libdatachannel; fall back to a download for portable test-only builds.
set(LM_JSON_INCLUDE "${CMAKE_SOURCE_DIR}/.deps/libdatachannel/deps/json/include")
if(NOT EXISTS "${LM_JSON_INCLUDE}/nlohmann/json.hpp")
  include(FetchContent)
  FetchContent_Declare(nlohmann_json URL https://github.com/nlohmann/json/releases/download/v3.11.3/include.zip
                       URL_HASH SHA256=a22461d13119ac5c78f205d3df1db13403e58ce1bb1794edc9313677313f4a9d)
  FetchContent_MakeAvailable(nlohmann_json)
  set(LM_JSON_INCLUDE "${nlohmann_json_SOURCE_DIR}/include")
endif()

# Platform-independent logic: pairing codes, LaptopMon selection, lifecycle reducer, settings, hashing, logging.
add_library(lm-logic STATIC host/src/pairing.cpp host/src/display_identity.cpp host/src/app_state.cpp
            host/src/settings.cpp host/src/logging.cpp)
target_include_directories(lm-logic PUBLIC host/include "${LM_JSON_INCLUDE}")
target_compile_definitions(lm-logic PUBLIC "LM_DEFAULT_SIGNALING_URL=\"${LM_DEFAULT_SIGNALING_URL}\""
                           "LM_VIEWER_URL=\"${LM_VIEWER_URL}\"" "LM_VERSION=\"${PROJECT_VERSION}\"")
if(WIN32)
  target_compile_definitions(lm-logic PUBLIC UNICODE _UNICODE NOMINMAX WIN32_LEAN_AND_MEAN _WIN32_WINNT=0x0A00)
  target_link_libraries(lm-logic PUBLIC bcrypt crypt32)
endif()
if(MSVC)
  target_compile_options(lm-logic PUBLIC /W4 /EHsc /permissive- /utf-8)
endif()

add_executable(core-tests host/tests/core_tests.cpp)
target_include_directories(core-tests PRIVATE host/include)
add_test(NAME core-tests COMMAND core-tests)
add_executable(logic-tests host/tests/logic_tests.cpp)
target_link_libraries(logic-tests PRIVATE lm-logic)
add_test(NAME logic-tests COMMAND logic-tests)

if(LM_BUILD_HOST)
  if(NOT WIN32)
    message(FATAL_ERROR "The host requires Windows 11. Use -DLM_BUILD_HOST=OFF for portable tests.")
  endif()
  set(BUILD_SHARED_LIBS OFF CACHE BOOL "" FORCE)
  set(NO_EXAMPLES ON CACHE BOOL "" FORCE)
  set(NO_TESTS ON CACHE BOOL "" FORCE)
  set(LIBSRTP_TEST_APPS OFF CACHE BOOL "" FORCE)
  set(USE_MBEDTLS ON CACHE BOOL "" FORCE)
  set(CMAKE_POLICY_VERSION_MINIMUM 3.5)
  add_subdirectory(.deps/libdatachannel EXCLUDE_FROM_ALL)
  file(READ "${CMAKE_SOURCE_DIR}/shared/ice.json" LM_ICE_JSON)
  configure_file(host/include/ice_config.hpp.in generated/ice_config.hpp @ONLY)

  # The GPU pipeline and WebRTC transport, shared by the application and the benchmark. Unchanged video path:
  # capture texture -> D3D11 video processor NV12 -> hardware H.264 MFT -> RTP. No CPU readback.
  add_library(lm-pipeline STATIC host/src/capture.cpp host/src/converter.cpp host/src/encoder.cpp
              host/src/transport.cpp host/src/wgc.cpp host/src/pattern.cpp host/src/display_query.cpp
              host/src/display_scale.cpp
              host/src/pipeline.cpp)
  target_include_directories(lm-pipeline PUBLIC host/include "${CMAKE_BINARY_DIR}/generated")
  target_link_libraries(lm-pipeline PUBLIC lm-logic datachannel-static d3d11 dxgi mfplat mf mfuuid
                        wmcodecdspuuid ole32 oleaut32 user32 gdi32 shcore shell32 windowsapp evr)

  add_executable(laptop-monitor-bench host/bench/main.cpp)
  target_link_libraries(laptop-monitor-bench PRIVATE lm-pipeline)

  # Elevated display helper: owns the software device; started through the scheduled task registered by setup.
  # .manifest sources are merged into the linker-generated manifest by CMake.
  add_executable(LaptopMonitorDisplay WIN32 host/helper/main.cpp host/helper/helper.rc host/helper/helper.manifest)
  target_include_directories(LaptopMonitorDisplay PRIVATE host/include)
  target_compile_definitions(LaptopMonitorDisplay PRIVATE UNICODE _UNICODE NOMINMAX WIN32_LEAN_AND_MEAN
                             _WIN32_WINNT=0x0A00 "LM_VERSION=\"${PROJECT_VERSION}\"")
  target_compile_options(LaptopMonitorDisplay PRIVATE /W4 /EHsc /permissive- /utf-8)
  target_link_libraries(LaptopMonitorDisplay PRIVATE swdevice advapi32 user32 shell32)

  # The desktop application.
  add_executable(LaptopMonitor WIN32
                 host/app/main.cpp host/app/controller.cpp host/app/display_control.cpp host/app/setup.cpp
                 host/app/ui/main_window.cpp host/app/ui/renderer.cpp host/app/ui/tray.cpp
                 host/app/resources/LaptopMonitor.rc host/app/resources/app.manifest)
  target_include_directories(LaptopMonitor PRIVATE host/app)
  target_link_libraries(LaptopMonitor PRIVATE lm-pipeline d2d1 dwrite dwmapi uxtheme comctl32 taskschd
                        shlwapi advapi32 version)
  install(TARGETS LaptopMonitor LaptopMonitorDisplay laptop-monitor-bench RUNTIME DESTINATION bin)
endif()
