cmake_minimum_required(VERSION 3.16)
project(adsbtocot VERSION 1.1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# Sources are UTF-8; tell MSVC so wide literals with “…”, em dashes, etc.
# compile to the right code points instead of mojibake.
if(MSVC)
    add_compile_options(/utf-8)
endif()

add_compile_definitions(ADSBTOCOT_VERSION="${PROJECT_VERSION}")

add_library(adsb_core STATIC src/adsb_core.cpp)
target_include_directories(adsb_core PUBLIC src)
if(WIN32)
    target_link_libraries(adsb_core PUBLIC ws2_32)
endif()

add_executable(adsbtocot src/main.cpp)
target_link_libraries(adsbtocot PRIVATE adsb_core)

if(WIN32)
    add_executable(adsbtocot_gui WIN32 src/gui_main.cpp)
    target_link_libraries(adsbtocot_gui PRIVATE adsb_core comctl32)
endif()

# ---------------------------------------------------------------------------
# Packaging: `cpack` (or `cmake --build . --target package`) produces a
# versioned zip with both executables and the docs.
# ---------------------------------------------------------------------------
install(TARGETS adsbtocot DESTINATION .)
if(WIN32)
    install(TARGETS adsbtocot_gui DESTINATION .)
endif()
install(FILES README.md DESTINATION .)

set(CPACK_PACKAGE_NAME "adsbtocot")
set(CPACK_PACKAGE_VERSION "${PROJECT_VERSION}")
set(CPACK_PACKAGE_VENDOR "rabidfox")
set(CPACK_PACKAGE_DESCRIPTION_SUMMARY
    "ADS-B JSON feed to Cursor-on-Target bridge with a Windows UI")
set(CPACK_GENERATOR "ZIP")
set(CPACK_PACKAGE_FILE_NAME "adsbtocot-${PROJECT_VERSION}-win64")
include(CPack)
