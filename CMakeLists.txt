cmake_minimum_required(VERSION 3.16)

project(ebview
    VERSION 0.3.6.2
    DESCRIPTION "EPWING CD-ROM dictionary viewer (modernized build)"
    LANGUAGES C)

include(GNUInstallDirs)
include(CheckIncludeFile)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_EXTENSIONS ON)
if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE Release CACHE STRING "Build type" FORCE)
endif()

# The upstream code predates GTK's deprecation of GtkObject/GtkCombo.
# Keep the build warning-clean without hiding real errors.
add_compile_options(-Wall -Wno-deprecated-declarations)

find_package(PkgConfig REQUIRED)
pkg_check_modules(GTK2 REQUIRED IMPORTED_TARGET gtk+-2.0>=2.10 gthread-2.0)
find_package(Threads REQUIRED)
find_package(ZLIB REQUIRED)

# libeb (EB Library / "eb") has never shipped a .pc file.
find_path(EB_INCLUDE_DIR NAMES eb/eb.h)
find_library(EB_LIBRARY NAMES eb)
if(NOT EB_INCLUDE_DIR OR NOT EB_LIBRARY)
    message(FATAL_ERROR "EB Library development files not found (install libeb16-dev)")
endif()

find_library(INTL_LIBRARY NAMES intl)
find_program(MSGFMT_EXECUTABLE msgfmt)

set(PACKAGE "ebview")
set(PACKAGE_NAME "EBView")
set(PACKAGE_STRING "EBView ${PROJECT_VERSION}")
set(GETTEXT_PACKAGE "ebview")
set(LOCALEDIR "${CMAKE_INSTALL_FULL_LOCALEDIR}")
set(PACKAGEDIR "${CMAKE_INSTALL_FULL_DATADIR}/ebview")

foreach(_hdr unistd.h fcntl.h sys/ioctl.h sys/time.h iconv.h libintl.h dirent.h)
    string(TOUPPER "${_hdr}" _var)
    string(REGEX REPLACE "[/.]" "_" _var "HAVE_${_var}")
    check_include_file("${_hdr}" ${_var})
endforeach()

configure_file(config.h.in "${CMAKE_CURRENT_BINARY_DIR}/config.h" @ONLY)
file(MAKE_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/data")
configure_file(data/about.en.in "${CMAKE_CURRENT_BINARY_DIR}/data/about.en" @ONLY)
configure_file(data/about.jp.in "${CMAKE_CURRENT_BINARY_DIR}/data/about.jp" @ONLY)

# ebview-client.c is an unfinished standalone remote-client experiment from
# upstream; it was never part of the original Makefile.am target either.
file(GLOB EBVIEW_SOURCES CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/src/*.c")
list(REMOVE_ITEM EBVIEW_SOURCES "${CMAKE_CURRENT_SOURCE_DIR}/src/ebview-client.c")

add_executable(ebview ${EBVIEW_SOURCES})
target_include_directories(ebview PRIVATE
    "${CMAKE_CURRENT_BINARY_DIR}"
    "${CMAKE_CURRENT_SOURCE_DIR}/src"
    "${EB_INCLUDE_DIR}")
target_link_libraries(ebview PRIVATE
    PkgConfig::GTK2
    ${EB_LIBRARY}
    ZLIB::ZLIB
    Threads::Threads
    m)
if(INTL_LIBRARY)
    target_link_libraries(ebview PRIVATE ${INTL_LIBRARY})
endif()

# Stage a runtime data tree in the build dir so the app can be launched
# straight after compiling: EBVIEW_DATADIR=build/share/ebview ./build/ebview
set(EBVIEW_RUN_DATADIR "${CMAKE_CURRENT_BINARY_DIR}/share/ebview")
file(MAKE_DIRECTORY "${EBVIEW_RUN_DATADIR}/help")
file(COPY
    "${CMAKE_CURRENT_SOURCE_DIR}/data/endinglist.xml"
    "${CMAKE_CURRENT_SOURCE_DIR}/data/endinglist-ja.xml"
    "${CMAKE_CURRENT_SOURCE_DIR}/data/filter.xml"
    "${CMAKE_CURRENT_SOURCE_DIR}/data/searchengines.xml"
    "${CMAKE_CURRENT_SOURCE_DIR}/data/shortcut.xml"
    DESTINATION "${EBVIEW_RUN_DATADIR}")
file(COPY
    "${CMAKE_CURRENT_BINARY_DIR}/data/about.en"
    "${CMAKE_CURRENT_BINARY_DIR}/data/about.jp"
    DESTINATION "${EBVIEW_RUN_DATADIR}")
file(COPY "${CMAKE_CURRENT_SOURCE_DIR}/pixmaps/" DESTINATION "${EBVIEW_RUN_DATADIR}/pixmaps")
file(COPY "${CMAKE_CURRENT_SOURCE_DIR}/doc/en" DESTINATION "${EBVIEW_RUN_DATADIR}/help")
file(COPY "${CMAKE_CURRENT_SOURCE_DIR}/doc/ja" DESTINATION "${EBVIEW_RUN_DATADIR}/help")

# Translations
if(MSGFMT_EXECUTABLE)
    set(_mo "${CMAKE_CURRENT_BINARY_DIR}/locale/ja/LC_MESSAGES/${GETTEXT_PACKAGE}.mo")
    add_custom_command(OUTPUT "${_mo}"
        COMMAND ${CMAKE_COMMAND} -E make_directory
            "${CMAKE_CURRENT_BINARY_DIR}/locale/ja/LC_MESSAGES"
        COMMAND ${MSGFMT_EXECUTABLE} -o "${_mo}"
            "${CMAKE_CURRENT_SOURCE_DIR}/po/ja.po"
        DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/po/ja.po"
        COMMENT "Compiling Japanese translation")
    add_custom_target(translations ALL DEPENDS "${_mo}")
    install(FILES "${_mo}"
        DESTINATION "${CMAKE_INSTALL_LOCALEDIR}/ja/LC_MESSAGES")
endif()

install(TARGETS ebview RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
install(FILES
    "${CMAKE_CURRENT_BINARY_DIR}/data/about.en"
    "${CMAKE_CURRENT_BINARY_DIR}/data/about.jp"
    "${CMAKE_CURRENT_SOURCE_DIR}/data/endinglist.xml"
    "${CMAKE_CURRENT_SOURCE_DIR}/data/endinglist-ja.xml"
    "${CMAKE_CURRENT_SOURCE_DIR}/data/filter.xml"
    "${CMAKE_CURRENT_SOURCE_DIR}/data/searchengines.xml"
    "${CMAKE_CURRENT_SOURCE_DIR}/data/shortcut.xml"
    DESTINATION "${CMAKE_INSTALL_DATADIR}/ebview")
install(DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}/pixmaps/"
    DESTINATION "${CMAKE_INSTALL_DATADIR}/ebview/pixmaps")
install(DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}/doc/en" "${CMAKE_CURRENT_SOURCE_DIR}/doc/ja"
    DESTINATION "${CMAKE_INSTALL_DATADIR}/ebview/help")
install(FILES "${CMAKE_CURRENT_SOURCE_DIR}/desktop/ebview.desktop"
    DESTINATION "${CMAKE_INSTALL_DATADIR}/applications")
install(FILES "${CMAKE_CURRENT_SOURCE_DIR}/pixmaps/ebview.xpm"
    DESTINATION "${CMAKE_INSTALL_DATADIR}/pixmaps")
install(FILES "${CMAKE_CURRENT_SOURCE_DIR}/README.md"
    DESTINATION "${CMAKE_INSTALL_DOCDIR}" OPTIONAL)

message(STATUS "EBView ${PROJECT_VERSION}")
message(STATUS "  GTK         : ${GTK2_VERSION}")
message(STATUS "  EB library  : ${EB_LIBRARY}")
message(STATUS "  Data dir    : ${PACKAGEDIR}")
message(STATUS "  Run datadir : ${EBVIEW_RUN_DATADIR}")
