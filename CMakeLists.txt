cmake_minimum_required(VERSION 3.21)
include(GenerateExportHeader)

project(cs_native LANGUAGES CXX VERSION 2.0.1)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)

if(${CMAKE_SYSTEM_NAME} MATCHES "Windows")
    set(CMAKE_SYSTEM_VERSION 10.0 CACHE TYPE INTERNAL FORCE)
elseif(${CMAKE_SYSTEM_NAME} MATCHES "Darwin")
    set(CMAKE_OSX_DEPLOYMENT_TARGET 11 CACHE TYPE INTERNAL FORCE)
endif()

find_package(LibtorrentRasterbar CONFIG REQUIRED)
find_package(magic_enum CONFIG REQUIRED)

if(${CMAKE_SYSTEM_NAME} MATCHES "Darwin")
    set(CMAKE_BUILD_WITH_INSTALL_RPATH TRUE)
    set(CMAKE_INSTALL_RPATH "@loader_path")
elseif(${CMAKE_SYSTEM_NAME} MATCHES "Linux")
    set(CMAKE_BUILD_WITH_INSTALL_RPATH TRUE)
    set(CMAKE_INSTALL_RPATH "$ORIGIN")
endif()

# ── Librería principal (DLL) ──────────────────────────────────────────
add_library(csdl SHARED
    native/src/settings.cpp
    native/src/library.cpp
    native/src/events.cpp
    native/src/streaming.cpp
)
target_compile_definitions(csdl PRIVATE CSDL_BUILD_DLL)

target_include_directories(csdl PRIVATE
    ${CMAKE_BINARY_DIR}/include
    ${CMAKE_CURRENT_SOURCE_DIR}/native/include
)

generate_export_header(csdl EXPORT_FILE_NAME ${CMAKE_BINARY_DIR}/include/lib_export.h)

set_target_properties(csdl PROPERTIES
    SOVERSION ${PROJECT_VERSION_MAJOR}
    VERSION ${PROJECT_VERSION}
)

target_link_libraries(csdl PRIVATE
    LibtorrentRasterbar::torrent-rasterbar
    magic_enum::magic_enum
)

# ── Ejecutable de prueba nativo (consola) ─────────────────────────────
add_executable(test_streaming
    native/test/test_streaming.cpp
    native/src/streaming.cpp
    native/src/library.cpp
    native/src/events.cpp
    native/src/settings.cpp
)
target_compile_definitions(test_streaming PRIVATE CSDL_STATIC)

target_include_directories(test_streaming PRIVATE
    ${CMAKE_BINARY_DIR}/include
    ${CMAKE_CURRENT_SOURCE_DIR}/native/include
)

target_link_libraries(test_streaming PRIVATE
    LibtorrentRasterbar::torrent-rasterbar
    magic_enum::magic_enum
)

# ── Dependencias de sistema (Windows) ──────────────────────────────────
if(WIN32)
    set(SYS_LIBS ws2_32 crypt32 iphlpapi bcrypt advapi32 user32 shell32
                ole32 oleaut32 uuid version dbghelp winmm)

    target_link_libraries(csdl PRIVATE ${SYS_LIBS})
    target_link_libraries(test_streaming PRIVATE ${SYS_LIBS})
endif()

# ── Recurso de versión (solo para la librería) ────────────────────────
if(WIN32 AND EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/native/build/version.rc.in")
    configure_file(native/build/version.rc.in version.rc @ONLY)
    target_sources(csdl PRIVATE version.rc)
endif()