# The decoder on the CPU backend (AQ_CPU): aquadecoder (the same C API as libaquadecoder.dylib) and aq_check.
#   cmake -S decoder/native/cpu -B decoder/native/cpu/build -G Ninja -DCMAKE_BUILD_TYPE=Release
#   cmake --build decoder/native/cpu/build
cmake_minimum_required(VERSION 3.20)
project(aquadecoder_cpu CXX)
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_WINDOWS_EXPORT_ALL_SYMBOLS ON)

find_package(OpenMP REQUIRED)
find_package(Git QUIET)
set(AQ_VERSION "unknown")
if(GIT_FOUND)
  execute_process(COMMAND ${GIT_EXECUTABLE} -C ${CMAKE_CURRENT_SOURCE_DIR} rev-parse --short HEAD OUTPUT_VARIABLE AQ_VERSION OUTPUT_STRIP_TRAILING_WHITESPACE ERROR_QUIET)
endif()
string(TIMESTAMP AQ_DATE "%Y%m%d" UTC)

set(NATIVE ${CMAKE_CURRENT_SOURCE_DIR}/..)
add_library(aquadecoder SHARED
  ${NATIVE}/edit.cpp ${NATIVE}/enumerate.cpp ${NATIVE}/v2_session.cpp ${NATIVE}/aquadecoder.cpp
  v2_model_cpu.cpp cpu_ops.cpp)
target_include_directories(aquadecoder PUBLIC ${NATIVE})
target_compile_definitions(aquadecoder PRIVATE AQ_CPU _CRT_SECURE_NO_WARNINGS DECODER_NATIVE_VERSION="${AQ_VERSION}-cpu+${AQ_DATE}")
target_link_libraries(aquadecoder PRIVATE OpenMP::OpenMP_CXX)
if(MSVC)
  target_compile_options(aquadecoder PRIVATE /O2 /arch:AVX2 /fp:precise /utf-8 /EHsc /W3 /wd4244 /wd4267 /wd4305)
else()
  target_compile_options(aquadecoder PRIVATE -O3 -mavx2 -mfma -mf16c)
endif()

add_executable(aq_check aq_check.cpp)
target_link_libraries(aq_check PRIVATE aquadecoder)
if(MSVC)
  target_compile_options(aq_check PRIVATE /O2 /utf-8 /EHsc)
endif()

add_executable(bench_gemm bench_gemm.cpp cpu_ops.cpp)
target_link_libraries(bench_gemm PRIVATE OpenMP::OpenMP_CXX)
if(MSVC)
  target_compile_options(bench_gemm PRIVATE /O2 /arch:AVX2 /utf-8 /EHsc)
endif()
