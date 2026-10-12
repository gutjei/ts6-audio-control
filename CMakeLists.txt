cmake_minimum_required(VERSION 3.24)
project(TeamSpeakAudioControl LANGUAGES C CXX)
if(NOT WIN32 OR NOT MSVC OR NOT CMAKE_SIZEOF_VOID_P EQUAL 8)
    message(FATAL_ERROR "Windows x64 and MSVC are required")
endif()
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_INSTALL_BINDIR ".")
set(CMAKE_INSTALL_LIBDIR ".")
find_package(Qt6 6.10.2 EXACT REQUIRED COMPONENTS Widgets)
qt_standard_project_setup()
add_compile_options(/utf-8)
add_compile_definitions(_WIN32_WINNT=0x0A00 NTDDI_VERSION=0x0A00000A)
add_library(minhook STATIC native/vendor/minhook/src/buffer.c native/vendor/minhook/src/hook.c
    native/vendor/minhook/src/trampoline.c native/vendor/minhook/src/hde/hde64.c)
add_library(audio_mixer SHARED native/router.cpp native/agent.cpp)
target_link_libraries(audio_mixer PRIVATE minhook ole32 mmdevapi uuid advapi32 avrt)
add_executable(audio_clock native/clock.cpp)
foreach(target minhook audio_mixer audio_clock)
    set_target_properties(${target} PROPERTIES AUTOMOC OFF AUTOUIC OFF
        MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
endforeach()
qt_add_executable(TeamSpeakAudioControl WIN32 app/main.cpp app/window.cpp app/window.h
    app/backend.cpp app/backend.h app/settings.cpp app/runtime.cpp app/diagnostics.cpp
    native/transport.cpp native/audio_inventory.cpp)
target_link_libraries(TeamSpeakAudioControl PRIVATE Qt6::Widgets ole32 mmdevapi uuid advapi32)
target_compile_options(TeamSpeakAudioControl PRIVATE /W4)
add_dependencies(TeamSpeakAudioControl audio_mixer audio_clock)
qt_add_resources(TeamSpeakAudioControl app_resources PREFIX "/" FILES
    app/style.qss app/check.xpm native/vendor/minhook/LICENSE.txt)
install(TARGETS TeamSpeakAudioControl audio_mixer audio_clock RUNTIME DESTINATION . LIBRARY DESTINATION .)
install(FILES README.md README.ru.md DESTINATION .)
install(FILES native/README.md DESTINATION native)
install(FILES native/vendor/minhook/LICENSE.txt DESTINATION licenses/minhook)
install(DIRECTORY licenses/qt DESTINATION licenses)
set(CMAKE_INSTALL_SYSTEM_RUNTIME_DESTINATION ".")
include(InstallRequiredSystemLibraries)
qt_generate_deploy_app_script(TARGET TeamSpeakAudioControl OUTPUT_SCRIPT deploy_script
    NO_UNSUPPORTED_PLATFORM_ERROR NO_COMPILER_RUNTIME NO_TRANSLATIONS
    DEPLOY_TOOL_OPTIONS --no-system-d3d-compiler --no-system-dxc-compiler --no-opengl-sw
        --skip-plugin-types generic,networkinformation,tls,imageformats)
install(SCRIPT ${deploy_script})
