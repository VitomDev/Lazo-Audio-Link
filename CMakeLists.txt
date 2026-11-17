# Lazo.exe, its core library and the tests. Use build.ps1, which also fetches
# the dependencies and builds the installer.
#   -DLAZO_ZXING_SOURCE=<zxing-cpp source folder>
cmake_minimum_required(VERSION 3.21)
project(Lazo CXX C)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
# Static CRT: no Visual C++ redistributable needed.
set(CMAKE_MSVC_RUNTIME_LIBRARY MultiThreaded)

# ZXing-C++ with the QR code writer only.
set(BUILD_SHARED_LIBS OFF CACHE BOOL "" FORCE)
set(ZXING_READERS OFF CACHE BOOL "" FORCE)
set(ZXING_WRITERS OLD CACHE STRING "" FORCE)
set(ZXING_C_API OFF CACHE BOOL "" FORCE)
set(ZXING_EXAMPLES OFF CACHE BOOL "" FORCE)
set(ZXING_DEPENDENCIES LOCAL CACHE STRING "" FORCE)
foreach(format 1D AZTEC DATAMATRIX MAXICODE PDF417)
    set(ZXING_ENABLE_${format} OFF CACHE BOOL "" FORCE)
endforeach()
set(ZXING_ENABLE_QRCODE ON CACHE BOOL "" FORCE)
add_subdirectory(${LAZO_ZXING_SOURCE} zxing EXCLUDE_FROM_ALL)

# Lazo's own code is held to stricter flags than the library.
add_compile_definitions(UNICODE _UNICODE NOMINMAX WIN32_LEAN_AND_MEAN _WIN32_WINNT=0x0A00 WINVER=0x0A00)
add_compile_options(/W4 /WX /permissive- /sdl /utf-8 /guard:cf /external:anglebrackets /external:W0)
add_link_options(/guard:cf /CETCOMPAT /OPT:REF /OPT:ICF)

add_library(lazocore STATIC
    src/core/audio.cpp
    src/core/http.cpp
    src/core/mdns.cpp
    src/core/network.cpp
    src/core/paths.cpp
    src/core/pcm.cpp
    src/core/qr.cpp
    src/core/server.cpp
    src/core/settings.cpp
    src/core/text.cpp
    src/core/web_page.cpp
    src/core/websocket.cpp
)
target_include_directories(lazocore PUBLIC src)
# ZXing's internal headers (the QR writer) are included as system headers.
target_include_directories(lazocore SYSTEM PRIVATE ${LAZO_ZXING_SOURCE}/core/src)
target_link_libraries(lazocore PUBLIC ws2_32 iphlpapi bcrypt ole32 user32 advapi32 mmdevapi version PRIVATE ZXing::ZXing)

add_executable(Lazo WIN32
    src/app/main.cpp
    src/app/main_window.cpp
    src/app/settings_dialog.cpp
    src/app/theme.cpp
    src/app/tray_app.cpp
    src/app/Lazo.rc
)
target_link_libraries(Lazo PRIVATE lazocore gdi32 comctl32 dwmapi uxtheme shell32)
# Rebuild the resources when the icon, manifest or version changes.
set_source_files_properties(src/app/Lazo.rc PROPERTIES OBJECT_DEPENDS
    "${CMAKE_SOURCE_DIR}/src/version.h;${CMAKE_SOURCE_DIR}/src/app/Lazo.manifest;${CMAKE_SOURCE_DIR}/assets/lazo.ico;${CMAKE_SOURCE_DIR}/assets/lazo.png")
# The manifest is embedded through Lazo.rc.
target_link_options(Lazo PRIVATE /MANIFEST:NO)

add_executable(LazoTests tests/tests.cpp)
target_link_libraries(LazoTests PRIVATE lazocore)
