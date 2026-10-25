cmake_minimum_required(VERSION 3.24)
project(anydemake C)

if(NOT WIN32)
  message(FATAL_ERROR "AnyDemake is a Windows DirectX proxy")
endif()

set(CMAKE_C_STANDARD 11)
add_compile_definitions(UNICODE _UNICODE _CRT_SECURE_NO_WARNINGS)
if(MSVC)
  # C4295: the config magic is deliberately not NUL-terminated.
  add_compile_options(/W4 /wd4201 /wd4295)
  # The proxy runs inside games: no dependency on the VC runtime DLLs.
  set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
endif()

add_library(anydemake_proxy SHARED proxy/proxy.c proxy/d3d11.def)
set_target_properties(anydemake_proxy PROPERTIES OUTPUT_NAME d3d11 RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/template)
target_link_libraries(anydemake_proxy PRIVATE dxguid)

file(GENERATE OUTPUT ${CMAKE_BINARY_DIR}/gen/$<CONFIG>/proxy.rc
     CONTENT "#include \"resource.h\"\nIDR_PROXY_DLL RCDATA \"$<TARGET_FILE:anydemake_proxy>\"\n")
add_executable(anydemake WIN32 tool/main.c tool/tool-core.c ${CMAKE_BINARY_DIR}/gen/$<CONFIG>/proxy.rc)
target_include_directories(anydemake PRIVATE tool)
target_link_libraries(anydemake PRIVATE comctl32 shell32 ole32)
target_link_options(anydemake PRIVATE /ENTRY:wmainCRTStartup)
add_dependencies(anydemake anydemake_proxy)
set_source_files_properties(${CMAKE_BINARY_DIR}/gen/$<CONFIG>/proxy.rc PROPERTIES OBJECT_DEPENDS $<TARGET_FILE:anydemake_proxy>)

include(CTest)
if(BUILD_TESTING)
  add_executable(d3d11-probe tests/d3d11-probe.c)
  target_link_libraries(d3d11-probe PRIVATE dxguid)
  target_link_options(d3d11-probe PRIVATE /ENTRY:wmainCRTStartup)

  # name | anydemake arguments | probe arguments
  function(demake_case name tool_args probe_args)
    add_test(NAME ${name} COMMAND ${CMAKE_COMMAND}
      -DTOOL=$<TARGET_FILE:anydemake> -DPROBE=$<TARGET_FILE:d3d11-probe>
      -DDIR=${CMAKE_BINARY_DIR}/test-games/${name}
      "-DTOOL_ARGS=${tool_args}" "-DPROBE_ARGS=${probe_args}"
      -P ${CMAKE_SOURCE_DIR}/tests/run-case.cmake)
  endfunction()

  demake_case(passthrough_texture "--preset;off" "texture;256;256")
  demake_case(passthrough_present "--preset;off" "present;0;0")
  demake_case(mip_skip_2 "--mip-skip;2" "texture;64;256")
  demake_case(mip_skip_default "--mip-skip;1;--mip-skip-default" "texture;128;128")
  demake_case(mip_skip_min_size "--mip-skip;4;--min-size;64" "texture;64;256")
  demake_case(sampler_point "--sampler;point" "sampler;0")
  demake_case(sampler_bilinear "--sampler;bilinear" "sampler;0x14")
  demake_case(pixelate_4 "--pixelate;4" "present;4;0")
  demake_case(pixelate_3_bits_2 "--pixelate;3;--color-bits;2" "present;3;2")
  demake_case(preset_ps1 "--preset;ps1" "texture;32;256")
  add_test(NAME refuse_foreign_dll COMMAND ${CMAKE_COMMAND} -DTOOL=$<TARGET_FILE:anydemake>
    -DDIR=${CMAKE_BINARY_DIR}/test-games/foreign -P ${CMAKE_SOURCE_DIR}/tests/foreign-dll.cmake)
endif()
