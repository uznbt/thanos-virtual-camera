cmake_minimum_required(VERSION 3.20)
project(thanos_virtual_camera LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

option(ONNXRUNTIME_DIR "Path to extracted ONNX Runtime SDK" "${CMAKE_CURRENT_SOURCE_DIR}/../vendor/onnxruntime")

if(NOT EXISTS "${ONNXRUNTIME_DIR}/include" OR NOT EXISTS "${ONNXRUNTIME_DIR}/lib")
  message(FATAL_ERROR "ONNX Runtime SDK not found at ${ONNXRUNTIME_DIR}. Run scripts/fetch-native-deps.mjs.")
endif()

set(ONNXRUNTIME_INCLUDE "${ONNXRUNTIME_DIR}/include")
if(WIN32)
  set(ONNXRUNTIME_LIB "${ONNXRUNTIME_DIR}/lib/onnxruntime.lib")
  set(ONNXRUNTIME_RUNTIME "${ONNXRUNTIME_DIR}/lib/onnxruntime.dll")
else()
  set(ONNXRUNTIME_LIB "${ONNXRUNTIME_DIR}/lib/libonnxruntime.so")
  set(ONNXRUNTIME_RUNTIME "${ONNXRUNTIME_DIR}/lib/libonnxruntime.so")
endif()

if(NOT EXISTS "${ONNXRUNTIME_LIB}")
  message(FATAL_ERROR "ONNX Runtime library not found: ${ONNXRUNTIME_LIB}")
endif()

if(WIN32)
  add_executable(native-camera src/main.cpp src/windows_camera.cpp)
else()
  add_executable(native-camera src/main.cpp src/v4l2_camera.cpp)
endif()

target_compile_definitions(native-camera PRIVATE THANOS_CAMERA_NATIVE=1)
if(WIN32)
  target_compile_definitions(native-camera PRIVATE WIN32_LEAN_AND_MEAN NOMINMAX)
  target_link_libraries(native-camera PRIVATE mfplat mfreadwrite mfstdk mfuuid ole32 oleaut32 strmiids winhttp ws2_32)
  find_library(DSHOW_LIB msdmo.lib)
else()
  target_link_libraries(native-camera PRIVATE pthread dl)
endif()

target_include_directories(native-camera PRIVATE "${ONNXRUNTIME_INCLUDE}")
target_link_libraries(native-camera PRIVATE "${ONNXRUNTIME_LIB}")

if(WIN32)
  # The DLL must live beside the executable in the packaged app.
  add_custom_command(TARGET native-camera POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_if_different "${ONNXRUNTIME_RUNTIME}" $<TARGET_FILE_DIR:native-camera>)
else()
  set_target_properties(native-camera PROPERTIES INSTALL_RPATH "$ORIGIN/onnxruntime" BUILD_RPATH "${ONNXRUNTIME_DIR}/lib")
  add_custom_command(TARGET native-camera POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E make_directory $<TARGET_FILE_DIR:native-camera>/onnxruntime
    COMMAND ${CMAKE_COMMAND} -E copy_if_different "${ONNXRUNTIME_DIR}/lib/libonnxruntime.so" $<TARGET_FILE_DIR:native-camera>/onnxruntime/
    COMMAND ${CMAKE_COMMAND} -E copy_if_different "${ONNXRUNTIME_DIR}/lib/libonnxruntime_providers_shared.so" $<TARGET_FILE_DIR:native-camera>/onnxruntime/)
endif()

install(TARGETS native-camera RUNTIME DESTINATION .)
if(WIN32)
  install(FILES "${ONNXRUNTIME_RUNTIME}" DESTINATION .)
else()
  install(DIRECTORY "${ONNXRUNTIME_DIR}/lib/" DESTINATION onnxruntime
          FILES_MATCHING PATTERN "libonnxruntime*.so*")
endif()
