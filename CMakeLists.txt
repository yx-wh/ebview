cmake_minimum_required(VERSION 3.16)

project(ebview
    VERSION 0.4.0
    DESCRIPTION "EPWING CD-ROM dictionary viewer (modernized build)"
    LANGUAGES C)

# Needed to compile the .rc that embeds the application icon into the
# executable.  Unix ports get the icon from the installed desktop files.
if(WIN32)
    enable_language(RC)
endif()

# Export build/compile_commands.json so clangd/IDE can resolve the
# generated config.h and the GTK/GLib include paths.
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

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
# AppStream metadata: the release version tracks PROJECT_VERSION so the
# software centre never shows a stale version.
configure_file(data/ebview.metainfo.xml.in
    "${CMAKE_CURRENT_BINARY_DIR}/data/ebview.metainfo.xml" @ONLY)

# ebview-client.c is an unfinished standalone remote-client experiment from
# upstream; it was never part of the original Makefile.am target either.
file(GLOB EBVIEW_SOURCES CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/src/*.c")
list(REMOVE_ITEM EBVIEW_SOURCES "${CMAKE_CURRENT_SOURCE_DIR}/src/ebview-client.c")

# WIN32 => link as a GUI-subsystem executable (-mwindows on MinGW).  EBView is
# a GUI app, not a console tool; the console-subsystem build allocated a console
# window on launch (the "black box" flash) and, on Windows 7, the console stole
# foreground focus from the main window so it appeared frozen.  A GUI-subsystem
# binary has no console at all and focus goes straight to the main window.
add_executable(ebview WIN32 ${EBVIEW_SOURCES})
target_include_directories(ebview PRIVATE
    "${CMAKE_CURRENT_BINARY_DIR}"
    "${CMAKE_CURRENT_SOURCE_DIR}/src"
    "${EB_INCLUDE_DIR}")

# libeb is configured with autoconf's AC_SYS_LARGEFILE, which puts
# _FILE_OFFSET_BITS=64 into its config.h.  That macro widens off_t to 64 bits,
# and EB_Text_Context stores an off_t (EB_Text_Context::location), so it
# changes the *binary layout* of every EB_Book the library touches:
#
#     off_t 32-bit -> sizeof(EB_Text_Context) = 336, sizeof(EB_Book) = 3544
#     off_t 64-bit -> sizeof(EB_Text_Context) = 344, sizeof(EB_Book) = 3552
#
# eb/defs.h is compiled into the client, so a client built without the same
# macro allocates an 8-byte-short EB_Book; eb_initialize_book() then writes
# past the end of it and the process dies later with STATUS_HEAP_CORRUPTION
# (0xC0000374) in an unrelated place.  The upstream autotools build compiled
# both sides with AC_SYS_LARGEFILE, so restoring the macro restores the ABI.
#
# This must stay in sync with whatever libeb itself was built with.
target_compile_definitions(ebview PRIVATE _FILE_OFFSET_BITS=64)

target_link_libraries(ebview PRIVATE
    PkgConfig::GTK2
    ${EB_LIBRARY}
    ZLIB::ZLIB
    Threads::Threads
    m)
if(INTL_LIBRARY)
    target_link_libraries(ebview PRIVATE ${INTL_LIBRARY})
endif()

# Embed the application icon and a VERSIONINFO block in the executable.
# Without this the .ico sits in the tree unused, Explorer/Alt-Tab show the
# generic program icon, and the Details tab has no version to report.
if(WIN32)
    set(EBVIEW_ICON_FILE "${CMAKE_CURRENT_SOURCE_DIR}/pixmaps/ebview.ico")

    # VERSIONINFO wants four comma separated 16-bit numbers, but
    # PROJECT_VERSION is a dot string and PROJECT_VERSION_TWEAK is empty when
    # the version has only three components (0.4.0), so build it here.
    set(EBVIEW_VERSION_TWEAK "${PROJECT_VERSION_TWEAK}")
    if(EBVIEW_VERSION_TWEAK STREQUAL "")
        set(EBVIEW_VERSION_TWEAK 0)
    endif()
    set(EBVIEW_VERSION_COMMA
        "${PROJECT_VERSION_MAJOR},${PROJECT_VERSION_MINOR},${PROJECT_VERSION_PATCH},${EBVIEW_VERSION_TWEAK}")

    configure_file("${CMAKE_CURRENT_SOURCE_DIR}/src/ebview.rc.in"
        "${CMAKE_CURRENT_BINARY_DIR}/ebview.rc" @ONLY)
    target_sources(ebview PRIVATE "${CMAKE_CURRENT_BINARY_DIR}/ebview.rc")
endif()

# Windows-specific system/compatibility libraries:
#   winmm  - MCI multimedia (external.c: mciSendCommand)
#   iconv  - GNU libiconv (jcode.c: libiconv_open / libiconv / libiconv_close)
#   gnurx  - POSIX regex (reg.c: regcomp / regexec / regfree)
if(WIN32)
    target_link_libraries(ebview PRIVATE winmm iconv gnurx)
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
    "${CMAKE_CURRENT_SOURCE_DIR}/data/gtkrc"
    DESTINATION "${EBVIEW_RUN_DATADIR}")
file(COPY
    "${CMAKE_CURRENT_BINARY_DIR}/data/about.en"
    "${CMAKE_CURRENT_BINARY_DIR}/data/about.jp"
    DESTINATION "${EBVIEW_RUN_DATADIR}")
file(COPY "${CMAKE_CURRENT_SOURCE_DIR}/pixmaps/" DESTINATION "${EBVIEW_RUN_DATADIR}/pixmaps")
file(COPY "${CMAKE_CURRENT_SOURCE_DIR}/doc/en" DESTINATION "${EBVIEW_RUN_DATADIR}/help")
file(COPY "${CMAKE_CURRENT_SOURCE_DIR}/doc/ja" DESTINATION "${EBVIEW_RUN_DATADIR}/help")
file(COPY "${CMAKE_CURRENT_SOURCE_DIR}/doc/zh_CN" DESTINATION "${EBVIEW_RUN_DATADIR}/help")
file(COPY "${CMAKE_CURRENT_SOURCE_DIR}/doc/zh_TW" DESTINATION "${EBVIEW_RUN_DATADIR}/help")

# Translations
if(MSGFMT_EXECUTABLE)
    set(EBVIEW_LINGUAS ja zh_CN zh_TW)
    set(_mos "")
    foreach(_lang ${EBVIEW_LINGUAS})
        set(_mo "${CMAKE_CURRENT_BINARY_DIR}/locale/${_lang}/LC_MESSAGES/${GETTEXT_PACKAGE}.mo")
        add_custom_command(OUTPUT "${_mo}"
            COMMAND ${CMAKE_COMMAND} -E make_directory
                "${CMAKE_CURRENT_BINARY_DIR}/locale/${_lang}/LC_MESSAGES"
            COMMAND ${MSGFMT_EXECUTABLE} -o "${_mo}"
                "${CMAKE_CURRENT_SOURCE_DIR}/po/${_lang}.po"
            DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/po/${_lang}.po"
            COMMENT "Compiling ${_lang} translation")
        list(APPEND _mos "${_mo}")
        install(FILES "${_mo}"
            DESTINATION "${CMAKE_INSTALL_LOCALEDIR}/${_lang}/LC_MESSAGES")
    endforeach()
    add_custom_target(translations ALL DEPENDS ${_mos})
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
    "${CMAKE_CURRENT_SOURCE_DIR}/data/gtkrc"
    DESTINATION "${CMAKE_INSTALL_DATADIR}/ebview")
install(DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}/pixmaps/"
    DESTINATION "${CMAKE_INSTALL_DATADIR}/ebview/pixmaps")
install(DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}/doc/en" "${CMAKE_CURRENT_SOURCE_DIR}/doc/ja"
    "${CMAKE_CURRENT_SOURCE_DIR}/doc/zh_CN" "${CMAKE_CURRENT_SOURCE_DIR}/doc/zh_TW"
    DESTINATION "${CMAKE_INSTALL_DATADIR}/ebview/help")

# The hicolor theme is where desktop shells look up Icon=ebview, and having
# every size is what keeps the icon sharp on HiDPI screens.  /usr/share/pixmaps
# remains as a fallback for environments without icon theme support.
install(DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}/pixmaps/hicolor/"
    DESTINATION "${CMAKE_INSTALL_DATADIR}/icons/hicolor")
install(FILES "${CMAKE_CURRENT_SOURCE_DIR}/pixmaps/ebview.svg"
    DESTINATION "${CMAKE_INSTALL_DATADIR}/icons/hicolor/scalable/apps")
install(FILES "${CMAKE_CURRENT_SOURCE_DIR}/desktop/ebview.desktop"
    DESTINATION "${CMAKE_INSTALL_DATADIR}/applications")
install(FILES "${CMAKE_CURRENT_BINARY_DIR}/data/ebview.metainfo.xml"
    DESTINATION "${CMAKE_INSTALL_DATADIR}/metainfo")
install(FILES "${CMAKE_CURRENT_SOURCE_DIR}/pixmaps/ebview.xpm"
    DESTINATION "${CMAKE_INSTALL_DATADIR}/pixmaps")
install(FILES "${CMAKE_CURRENT_SOURCE_DIR}/README.md"
    DESTINATION "${CMAKE_INSTALL_DOCDIR}" OPTIONAL)

message(STATUS "EBView ${PROJECT_VERSION}")
message(STATUS "  GTK         : ${GTK2_VERSION}")
message(STATUS "  EB library  : ${EB_LIBRARY}")
message(STATUS "  Data dir    : ${PACKAGEDIR}")
message(STATUS "  Run datadir : ${EBVIEW_RUN_DATADIR}")

# clangd looks for compile_commands.json in the source tree itself, not in
# build/. Mirror the generated database at the project root so IDE
# diagnostics resolve config.h and the GTK/GLib headers out of the box.
set(_db_link "${CMAKE_SOURCE_DIR}/compile_commands.json")
if(IS_SYMLINK "${_db_link}" OR EXISTS "${_db_link}")
    file(REMOVE "${_db_link}")
endif()
file(CREATE_LINK "${CMAKE_BINARY_DIR}/compile_commands.json" "${_db_link}"
     SYMBOLIC RESULT _db_link_result)
