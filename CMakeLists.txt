cmake_minimum_required(VERSION 3.16)
project(A-OH VERSION 1.0.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

if(APPLE)
    enable_language(OBJCXX)
    set(CMAKE_OBJCXX_STANDARD 17)
endif()

add_library(aoh_embedded OBJECT src/audio/embedded_sounds.cpp)
target_include_directories(aoh_embedded PRIVATE src)

set(AOH_COMMON_SOURCES
    src/common/config.cpp
    src/audio/audio_engine.cpp
    src/daemon/daemon.cpp
    src/watch/watcher.cpp
    src/main.cpp
    $<TARGET_OBJECTS:aoh_embedded>
)

if(WIN32)
    list(APPEND AOH_COMMON_SOURCES
        src/audio/audio_win32.cpp
        src/platform/platform_win32.cpp
    )
elseif(APPLE)
    list(APPEND AOH_COMMON_SOURCES
        src/audio/audio_macos.mm
        src/platform/platform_macos.cpp
    )
else()
    list(APPEND AOH_COMMON_SOURCES
        src/audio/audio_linux.cpp
        src/platform/platform_linux.cpp
    )
endif()

add_executable(a-oh ${AOH_COMMON_SOURCES})
target_include_directories(a-oh PRIVATE src)

if(WIN32)
    target_compile_definitions(a-oh PRIVATE UNICODE _UNICODE NOMINMAX WIN32_LEAN_AND_MEAN)
    target_link_libraries(a-oh PRIVATE
        winmm mfplat mfreadwrite mfuuid ole32 oleaut32
        ws2_32 comdlg32 shell32 advapi32 user32 gdi32
    )
    if(MINGW)
        target_link_options(a-oh PRIVATE -municode -static -static-libgcc -static-libstdc++)
    endif()

    # Windowless tray/autostart launcher executable (no console flash)
    add_executable(a-oh-tray WIN32 ${AOH_COMMON_SOURCES})
    target_include_directories(a-oh-tray PRIVATE src)
    target_compile_definitions(a-oh-tray PRIVATE UNICODE _UNICODE NOMINMAX WIN32_LEAN_AND_MEAN AOH_GUI_APP)
    target_link_libraries(a-oh-tray PRIVATE
        winmm mfplat mfreadwrite mfuuid ole32 oleaut32
        ws2_32 comdlg32 shell32 advapi32 user32 gdi32
    )
    if(MINGW)
        target_link_options(a-oh-tray PRIVATE -municode -mwindows -static -static-libgcc -static-libstdc++)
    endif()
elseif(APPLE)
    target_link_libraries(a-oh PRIVATE
        "-framework Foundation"
        "-framework AppKit"
        pthread
    )
else()
    target_link_libraries(a-oh PRIVATE pthread)
endif()
