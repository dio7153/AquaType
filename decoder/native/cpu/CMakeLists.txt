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
# one full decoder per instruction set; ISA is avx2 or sse
function(aq_decoder_lib name isa)
  add_library(${name} SHARED
    ${NATIVE}/edit.cpp ${NATIVE}/enumerate.cpp ${NATIVE}/v2_session.cpp ${NATIVE}/aquadecoder.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/v2_model_cpu.cpp ${CMAKE_CURRENT_SOURCE_DIR}/cpu_ops.cpp)
  target_include_directories(${name} PUBLIC ${NATIVE})
  target_compile_definitions(${name} PRIVATE AQ_CPU _CRT_SECURE_NO_WARNINGS DECODER_NATIVE_VERSION="${AQ_VERSION}-cpu+${AQ_DATE}")
  target_link_libraries(${name} PRIVATE OpenMP::OpenMP_CXX)
  if(MSVC)
    set_property(TARGET ${name} PROPERTY MSVC_RUNTIME_LIBRARY "MultiThreaded")   # static CRT: only vcomp140 ships beside it
    target_compile_options(${name} PRIVATE /O2 /fp:precise /utf-8 /EHsc /W3 /wd4244 /wd4267 /wd4305)
    if(isa STREQUAL "avx2")
      target_compile_options(${name} PRIVATE /arch:AVX2)
    endif()                                   # sse: MSVC's x64 default (SSE2) for the compiler's own code; the kernels use SSE4.1
  elseif(isa STREQUAL "avx2")
    target_compile_options(${name} PRIVATE -O3 -mavx2 -mfma -mf16c)
  else()
    target_compile_options(${name} PRIVATE -O3 -msse4.1)
  endif()
endfunction()

if(WIN32)
  # aquadecoder.dll checks the CPU and loads aquadecoder_avx2.dll or aquadecoder_sse.dll (aq_dispatch.cpp)
  aq_decoder_lib(aquadecoder_avx2 avx2)
  aq_decoder_lib(aquadecoder_sse sse)
  add_library(aquadecoder SHARED aq_dispatch.cpp)
  target_include_directories(aquadecoder PUBLIC ${NATIVE})
  add_dependencies(aquadecoder aquadecoder_avx2 aquadecoder_sse)
  if(MSVC)
    set_property(TARGET aquadecoder PROPERTY MSVC_RUNTIME_LIBRARY "MultiThreaded")
    target_compile_definitions(aquadecoder PRIVATE _CRT_SECURE_NO_WARNINGS)
    target_compile_options(aquadecoder PRIVATE /O2 /utf-8 /EHsc /W3)
  endif()
  # The variants need OpenMP's DLL, which has no static form: vcomp140.dll ships beside the decoder (app-local, from the
  # toolset's x64 desktop redist; that build depends on KERNEL32 only - the onecore one would also want vcruntime140).
  set(_redist "$ENV{VCToolsRedistDir}")
  file(GLOB AQ_RUNTIME_DLLS "${_redist}x64/Microsoft.VC*.OpenMP/vcomp140.dll")
  if(NOT AQ_RUNTIME_DLLS)
    message(FATAL_ERROR "vcomp140.dll not found under VCToolsRedistDir='${_redist}' (configure from a vcvars64 shell)")
  endif()
  set(AQ_RUNTIME_DLLS ${AQ_RUNTIME_DLLS} CACHE INTERNAL "runtime DLLs shipped beside aquadecoder")
  # the variants and the runtime sit next to the dispatcher, wherever it is copied
  add_custom_command(TARGET aquadecoder POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_if_different $<TARGET_FILE:aquadecoder_avx2> $<TARGET_FILE:aquadecoder_sse> ${AQ_RUNTIME_DLLS} $<TARGET_FILE_DIR:aquadecoder>)
else()
  aq_decoder_lib(aquadecoder avx2)
endif()

add_executable(aq_check aq_check.cpp)
target_link_libraries(aq_check PRIVATE aquadecoder)
if(MSVC)
  set_property(TARGET aq_check PROPERTY MSVC_RUNTIME_LIBRARY "MultiThreaded")   # runs from a folder with no VC runtime installed
  target_compile_options(aq_check PRIVATE /O2 /utf-8 /EHsc)
endif()

add_executable(bench_gemm bench_gemm.cpp cpu_ops.cpp)
target_link_libraries(bench_gemm PRIVATE OpenMP::OpenMP_CXX)
if(MSVC)
  target_compile_options(bench_gemm PRIVATE /O2 /arch:AVX2 /utf-8 /EHsc)
endif()
