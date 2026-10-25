cmake_minimum_required(VERSION 3.24)
project(potatify C)

if(NOT WIN32)
  message(FATAL_ERROR "Potatify is a Windows DirectX proxy")
endif()

set(CMAKE_C_STANDARD 11)
add_compile_definitions(UNICODE _UNICODE _CRT_SECURE_NO_WARNINGS)
if(MSVC)
  # C4295: the config magic is deliberately not NUL-terminated.
  add_compile_options($<$<COMPILE_LANGUAGE:C>:/W4> $<$<COMPILE_LANGUAGE:C>:/wd4201> $<$<COMPILE_LANGUAGE:C>:/wd4295>)
  # The proxy runs inside games: no dependency on the VC runtime DLLs.
  set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
endif()

enable_language(ASM_MASM)

# One proxy per API: name, implemented exports, extra libraries.
function(demake_proxy api own libs)
  # MASM does not create object subfolders, so generated sources sit in the build root.
  set(gen ${CMAKE_BINARY_DIR})
  set(outputs ${gen}/${api}-forward.def ${gen}/${api}-forward.asm ${gen}/${api}-forward.c)
  add_custom_command(OUTPUT ${outputs}
    COMMAND ${CMAKE_COMMAND} -DEXPORTS=${CMAKE_SOURCE_DIR}/proxy/exports-${api}.txt "-DOWN=${own}"
            -DLIB=${api} -DOUT_DIR=${gen} -P ${CMAKE_SOURCE_DIR}/cmake/gen-forwarders.cmake
    DEPENDS proxy/exports-${api}.txt cmake/gen-forwarders.cmake)
  add_library(proxy_${api} SHARED proxy/common.c proxy/${api}-proxy.c ${outputs})
  set_target_properties(proxy_${api} PROPERTIES OUTPUT_NAME ${api}
    RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/template/${api})
  target_link_libraries(proxy_${api} PRIVATE dxguid ${libs})
endfunction()

demake_proxy(d3d11 "D3D11CreateDevice;D3D11CreateDeviceAndSwapChain;D3D11On12CreateDevice" "")
demake_proxy(d3d12 "D3D12CreateDevice" "dxgi")

file(GENERATE OUTPUT ${CMAKE_BINARY_DIR}/gen/$<CONFIG>/proxy.rc
     CONTENT "#include \"resource.h\"
IDR_PROXY_D3D11 RCDATA \"$<TARGET_FILE:proxy_d3d11>\"
IDR_PROXY_D3D12 RCDATA \"$<TARGET_FILE:proxy_d3d12>\"
")
add_executable(potatify WIN32 tool/main.c tool/tool-core.c tool/unreal.c ${CMAKE_BINARY_DIR}/gen/$<CONFIG>/proxy.rc)
target_include_directories(potatify PRIVATE tool)
target_link_libraries(potatify PRIVATE comctl32 shell32 ole32)
target_link_options(potatify PRIVATE /ENTRY:wmainCRTStartup)
add_dependencies(potatify proxy_d3d11 proxy_d3d12)
set_source_files_properties(${CMAKE_BINARY_DIR}/gen/$<CONFIG>/proxy.rc PROPERTIES
  OBJECT_DEPENDS "$<TARGET_FILE:proxy_d3d11>;$<TARGET_FILE:proxy_d3d12>")

include(CTest)
if(BUILD_TESTING)
  add_executable(d3d11-probe tests/d3d11-probe.c)
  target_link_libraries(d3d11-probe PRIVATE dxguid)
  target_link_options(d3d11-probe PRIVATE /ENTRY:wmainCRTStartup)
  add_executable(d3d12-probe tests/d3d12-probe.c)
  target_link_libraries(d3d12-probe PRIVATE dxguid dxgi)
  target_link_options(d3d12-probe PRIVATE /ENTRY:wmainCRTStartup)

  # name | potatify arguments | probe arguments
  function(demake_case name tool_args probe_args)
    set(api d3d11)
    if(name MATCHES "^d3d12_")
      set(api d3d12)
    endif()
    add_test(NAME ${name} COMMAND ${CMAKE_COMMAND}
      -DTOOL=$<TARGET_FILE:potatify> -DPROBE=$<TARGET_FILE:${api}-probe> -DDLL=${api}.dll
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
  demake_case(render_scale_50 "--render-scale;50" "scale;50")
  demake_case(render_scale_75 "--render-scale;75" "scale;75")
  demake_case(render_scale_off "--preset;off" "scale;100")
  demake_case(d3d12_passthrough_sampler "--preset;off" "sampler;0.7;0.7")
  demake_case(d3d12_sampler_point "--sampler;point" "sampler;1;1")
  demake_case(d3d12_sampler_bilinear "--sampler;bilinear" "sampler;0.7;0.7")
  demake_case(d3d12_passthrough_present "--preset;off" "present;0;0")
  demake_case(d3d12_pixelate_4 "--pixelate;4" "present;4;0")
  demake_case(d3d12_pixelate_3_bits_2 "--pixelate;3;--color-bits;2" "present;3;2")
  demake_case(d3d12_api_only "--api;d3d12;--sampler;point" "sampler;1;1")
  add_test(NAME refuse_foreign_dll COMMAND ${CMAKE_COMMAND} -DTOOL=$<TARGET_FILE:potatify>
    -DDIR=${CMAKE_BINARY_DIR}/test-games/foreign -P ${CMAKE_SOURCE_DIR}/tests/foreign-dll.cmake)
  add_test(NAME stats_log COMMAND ${CMAKE_COMMAND} -DTOOL=$<TARGET_FILE:potatify>
    -DPROBE=$<TARGET_FILE:d3d11-probe> -DDIR=${CMAKE_BINARY_DIR}/test-games/stats -P ${CMAKE_SOURCE_DIR}/tests/stats-log.cmake)
  add_test(NAME unreal_engine_ini COMMAND ${CMAKE_COMMAND} -DTOOL=$<TARGET_FILE:potatify>
    -DDIR=${CMAKE_BINARY_DIR}/test-games/unreal -P ${CMAKE_SOURCE_DIR}/tests/unreal.cmake)
endif()
