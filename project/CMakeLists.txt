cmake_minimum_required(VERSION 3.20)
project(GH6Recomp CXX)
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded")

set(RECOMP_ROOT ${CMAKE_CURRENT_SOURCE_DIR}/..)
file(GLOB PPC_SOURCES ${RECOMP_ROOT}/ppc/ppc_recomp.*.cpp ${RECOMP_ROOT}/ppc/ppc_func_mapping.cpp)

# Codigo PPC traducido (enorme): libreria aparte con flags propias
add_library(GH6RecompLib STATIC ${PPC_SOURCES})
target_include_directories(GH6RecompLib PUBLIC ${RECOMP_ROOT}/ppc ${RECOMP_ROOT}/XenonRecomp/thirdparty/simde)
# Optimizacion del codigo traducido del juego (sesion 24): GH6_PPC_OPT=/O1 (compila antes) o /O2 (mas rapido)
set(GH6_PPC_OPT "/O2" CACHE STRING "Optimizacion del codigo PPC traducido")   # /O2: 35-40 % menos de tirones de carga (medido)
target_compile_options(GH6RecompLib PRIVATE ${GH6_PPC_OPT} -fno-strict-aliasing -msse4.1 -Wno-everything -gline-tables-only /Z7 "/FI${RECOMP_ROOT}/compat/ppc_indirect.h")

add_executable(GH6 ${RECOMP_ROOT}/runtime/main.cpp ${RECOMP_ROOT}/runtime/kernel.cpp ${RECOMP_ROOT}/runtime/import_stubs.cpp
    ${RECOMP_ROOT}/runtime/threads.cpp ${RECOMP_ROOT}/runtime/fs.cpp ${RECOMP_ROOT}/runtime/gpu_stub.cpp ${RECOMP_ROOT}/runtime/diag.cpp
    ${RECOMP_ROOT}/runtime/gpu_cp.cpp ${RECOMP_ROOT}/runtime/gpu_window.cpp ${RECOMP_ROOT}/runtime/hooks.cpp ${RECOMP_ROOT}/runtime/atomics.cpp ${RECOMP_ROOT}/runtime/xam.cpp ${RECOMP_ROOT}/runtime/net.cpp ${RECOMP_ROOT}/runtime/audio_wasapi.cpp ${RECOMP_ROOT}/runtime/mic_capture.cpp ${RECOMP_ROOT}/runtime/launcher.cpp ${RECOMP_ROOT}/runtime/launcher_ui.cpp ${RECOMP_ROOT}/runtime/updater.cpp ${RECOMP_ROOT}/runtime/achievements.cpp ${RECOMP_ROOT}/runtime/dualsense.cpp ${RECOMP_ROOT}/runtime/gpu_d3d12.cpp
    ${RECOMP_ROOT}/runtime/gpu_shader.cpp ${RECOMP_ROOT}/runtime/gpu_shadertest.cpp ${RECOMP_ROOT}/runtime/xma.cpp)
target_include_directories(GH6 PRIVATE ${RECOMP_ROOT}/runtime)
target_compile_options(GH6 PRIVATE /O2 -fno-strict-aliasing -msse4.1 -Wno-unused-parameter)
target_compile_definitions(GH6 PRIVATE NOMINMAX WIN32_LEAN_AND_MEAN)
# Icono y version: gh6.res, compilado con llvm-rc desde gh6.rc (tools/build_game.bat)
target_link_libraries(GH6 PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/gh6.res)
target_link_libraries(GH6 PRIVATE GH6RecompLib user32 gdi32 d3d12 dxgi d3dcompiler winmm ole32 xinput bcrypt comdlg32 hid setupapi comctl32 dwmapi uxtheme winhttp ws2_32 oleaut32 ${RECOMP_ROOT}/toolchain/ffmpeg/lib/avcodec.lib ${RECOMP_ROOT}/toolchain/ffmpeg/lib/avutil.lib)
target_include_directories(GH6 PRIVATE ${RECOMP_ROOT}/toolchain/ffmpeg/include)
add_custom_command(TARGET GH6 POST_BUILD COMMAND ${CMAKE_COMMAND} -E copy_if_different ${RECOMP_ROOT}/toolchain/ffmpeg/bin/avcodec-62.dll ${RECOMP_ROOT}/toolchain/ffmpeg/bin/avutil-60.dll ${RECOMP_ROOT}/toolchain/ffmpeg/bin/swresample-6.dll $<TARGET_FILE_DIR:GH6>)
target_compile_options(GH6 PRIVATE /Z7)
target_link_options(GH6 PRIVATE /DEBUG /SUBSYSTEM:WINDOWS /ENTRY:mainCRTStartup /MANIFEST:EMBED)   # sin ventana de consola (sesion 23)
