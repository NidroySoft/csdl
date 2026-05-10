cmake_minimum_required(VERSION 3.21)
include(GenerateExportHeader)

project(cs_native LANGUAGES CXX VERSION 2.4.0)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)
option(BUILD_TESTS "Build internal test executables" OFF)

# ── Platform-specific adjustments ──────────────────────────────────────
if(WIN32)
    set(CMAKE_SYSTEM_VERSION 10.0 CACHE TYPE INTERNAL FORCE)
elseif(APPLE)
    set(CMAKE_OSX_DEPLOYMENT_TARGET 11 CACHE TYPE INTERNAL FORCE)
endif()

# ── Dependencies (all platforms) ───────────────────────────────────────
find_package(LibtorrentRasterbar CONFIG REQUIRED)
find_package(magic_enum CONFIG REQUIRED)
find_package(Threads REQUIRED)

# ── RPATH for shared libraries ────────────────────────────────────────
if(APPLE)
    set(CMAKE_BUILD_WITH_INSTALL_RPATH TRUE)
    set(CMAKE_INSTALL_RPATH "@loader_path")
elseif(UNIX AND NOT APPLE)
    set(CMAKE_BUILD_WITH_INSTALL_RPATH TRUE)
    set(CMAKE_INSTALL_RPATH "$ORIGIN")
endif()

# ── Sources and headers (all .cpp and .h) ─────────────────────────────
set(COMMON_SOURCES
    native/src/settings.cpp
    native/src/library.cpp
    native/src/events.cpp
    native/src/streaming.cpp
)

set(COMMON_HEADERS
    native/include/library.h
    native/include/settings.h
    native/include/events.h
    native/include/streaming.h
    native/include/structs.h
    native/include/struct_align.h
    native/include/httplib.h
)

# ── Library target (DLL) ───────────────────────────────────────────────
add_library(csdl SHARED
    ${COMMON_SOURCES}
    ${COMMON_HEADERS}
)

target_include_directories(csdl
    PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/native/include>
        $<INSTALL_INTERFACE:include>
    PRIVATE
        ${CMAKE_BINARY_DIR}/include
)

generate_export_header(csdl
    EXPORT_FILE_NAME ${CMAKE_BINARY_DIR}/include/lib_export.h
    STATIC_DEFINE  CSDL_STATIC
)

set_target_properties(csdl PROPERTIES
    SOVERSION ${PROJECT_VERSION_MAJOR}
    VERSION   ${PROJECT_VERSION}
)

target_link_libraries(csdl PRIVATE
    LibtorrentRasterbar::torrent-rasterbar
    magic_enum::magic_enum
    Threads::Threads
)

# ── Executable de prueba ───────────────────────────────────────────────
if(BUILD_TESTS)
    add_executable(test_streaming
        native/test/test_streaming.cpp
        ${COMMON_SOURCES}
        ${COMMON_HEADERS}
    )
    target_compile_definitions(test_streaming PRIVATE CSDL_STATIC)
    target_include_directories(test_streaming PRIVATE
        ${CMAKE_BINARY_DIR}/include
        ${CMAKE_CURRENT_SOURCE_DIR}/native/include
    )
    target_link_libraries(test_streaming PRIVATE
        LibtorrentRasterbar::torrent-rasterbar
        magic_enum::magic_enum
        Threads::Threads
    )

    if(WIN32)
        target_link_libraries(test_streaming PRIVATE ${SYS_LIBS})
    endif()
endif()
# ── Windows system libraries ───────────────────────────────────────────
if(WIN32)
    set(SYS_LIBS ws2_32 crypt32 iphlpapi bcrypt advapi32 user32 shell32
                ole32 oleaut32 uuid version dbghelp winmm)
    target_link_libraries(csdl PRIVATE ${SYS_LIBS})
endif()

# ── Version resource (Windows only) ────────────────────────────────────
if(WIN32 AND EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/native/build/version.rc.in")
    configure_file(native/build/version.rc.in version.rc @ONLY)
    target_sources(csdl PRIVATE version.rc)
endif()