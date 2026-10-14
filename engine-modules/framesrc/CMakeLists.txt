cmake_minimum_required(VERSION 3.16)
project(framesrc LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

add_library(framesource STATIC
    src/FrameSource.cpp
    src/FrameSourceReader.cpp
    src/FrameSourceCapture.cpp
)
target_include_directories(framesource PUBLIC include)
find_package(OpenGL REQUIRED)
find_library(EGL_LIBRARY NAMES EGL REQUIRED)
target_link_libraries(framesource PUBLIC OpenGL::GL ${EGL_LIBRARY})

enable_testing()

add_executable(test_roundtrip tests/test_roundtrip.cpp)
target_link_libraries(test_roundtrip PRIVATE framesource)

add_executable(test_capture_gl tests/test_capture_gl.cpp tests/EglContext.cpp)
target_link_libraries(test_capture_gl PRIVATE framesource OpenGL::GL ${EGL_LIBRARY})

add_executable(test_perf tests/test_perf.cpp tests/EglContext.cpp)
target_link_libraries(test_perf PRIVATE framesource OpenGL::GL ${EGL_LIBRARY})

add_test(NAME roundtrip COMMAND test_roundtrip)
add_test(NAME capture_gl COMMAND test_capture_gl)
add_test(NAME perf COMMAND test_perf)
