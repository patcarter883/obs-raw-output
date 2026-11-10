cmake_minimum_required(VERSION 3.28)
project(open-broadcast-raw-output C CXX)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
# The settings dialog is Qt, so the plugin needs a C++ TU alongside the C core.
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)

if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE Release)
endif()

# Two SDK shapes for one plugin. Unix uses the packaged pkg-config modules;
# Windows has no such thing, so it is given an obs-studio source tree for the
# headers and import libraries generated from the released obs.dll.
if(WIN32)
  if(NOT OBS_SOURCE_DIR)
    message(FATAL_ERROR
      "OBS_SOURCE_DIR must point at an obs-studio source tree (its libobs/ "
      "holds obs-module.h).")
  endif()
  if(NOT OBS_LIB_DIR)
    message(FATAL_ERROR
      "OBS_LIB_DIR must hold obs.lib and obs-frontend-api.lib, generated from "
      "the released obs.dll and obs-frontend-api.dll.")
  endif()

  find_package(Qt6 REQUIRED COMPONENTS Widgets)

  set(OBS_INCLUDE_DIRS
    "${OBS_SOURCE_DIR}/libobs"
    "${OBS_SOURCE_DIR}/UI/obs-frontend-api"
    "${CMAKE_CURRENT_SOURCE_DIR}/ci/windows")
  set(OBS_LINK_LIBS
    "${OBS_LIB_DIR}/obs.lib"
    "${OBS_LIB_DIR}/obs-frontend-api.lib"
    Qt6::Widgets
    ws2_32)
  set(OBS_CFLAGS "")
else()
  find_package(PkgConfig REQUIRED)
  pkg_check_modules(OBS REQUIRED libobs)
  pkg_check_modules(FRONTEND REQUIRED obs-frontend-api)
  # Qt Widgets only; the dialog uses no moc-requiring features.
  pkg_check_modules(QT6WIDGETS REQUIRED Qt6Widgets)

  # The libobs/obs-frontend-api .pc files carry -Werror, so inheriting their
  # flags would apply a DEPENDENCY's warning policy to our translation units -
  # and in C++ it fires on OBS's own headers (pedantic anonymous structs in
  # graphics/vec3.h and util/darray.h pulled in by obs.h). Keep our own flags
  # below; drop theirs.
  list(REMOVE_ITEM OBS_CFLAGS_OTHER "-Werror")
  list(REMOVE_ITEM FRONTEND_CFLAGS_OTHER "-Werror")
  # Same reason: the .pc files carry -std=gnu17 (their C standard). Forcing it
  # onto our C++ TU only earns a "valid for C/ObjC but not for C++" warning; the
  # two CMAKE_*_STANDARD settings above are the right place for ours.
  list(FILTER OBS_CFLAGS_OTHER EXCLUDE REGEX "^-std=")
  list(FILTER FRONTEND_CFLAGS_OTHER EXCLUDE REGEX "^-std=")

  # The libobs .pc files carry no -I for the headers, which live under obs/.
  find_path(OBS_INCLUDE_DIR obs-module.h
    PATHS /usr/include/obs /usr/local/include/obs
    REQUIRED)

  set(OBS_INCLUDE_DIRS
    ${OBS_INCLUDE_DIR} ${OBS_INCLUDE_DIRS} ${FRONTEND_INCLUDE_DIRS}
    ${QT6WIDGETS_INCLUDE_DIRS})
  set(OBS_LINK_LIBS
    ${OBS_LIBRARIES} ${FRONTEND_LIBRARIES} ${QT6WIDGETS_LIBRARIES})
  set(OBS_CFLAGS
    ${OBS_CFLAGS_OTHER} ${FRONTEND_CFLAGS_OTHER} ${QT6WIDGETS_CFLAGS_OTHER})
endif()

add_library(open-broadcast-raw-output MODULE
  src/raw-output.c
  src/obc-settings-dialog.cpp
  src/menu-action.cpp)

# Marked SYSTEM: warnings from third-party headers are not our code's problem to
# silence, and keeping them out of our warning view makes our own -Wextra output
# readable.
target_include_directories(open-broadcast-raw-output SYSTEM PRIVATE
  ${OBS_INCLUDE_DIRS})

target_link_libraries(open-broadcast-raw-output PRIVATE ${OBS_LINK_LIBS})

if(MSVC)
  target_compile_options(open-broadcast-raw-output PRIVATE /W3)
else()
  target_compile_options(open-broadcast-raw-output PRIVATE
    ${OBS_CFLAGS} -Wall -Wextra -Wpedantic)
endif()

# OBS loads <plugins-dir>/<name>/bin/64bit/<name>.<ext>, so the file must be
# named exactly after the plugin directory.
set_target_properties(open-broadcast-raw-output PROPERTIES
  PREFIX ""
  OUTPUT_NAME "open-broadcast-raw-output")

if(WIN32)
  # A Windows OBS scans %ProgramData%\obs-studio\plugins, so the package is the
  # plugin's own tree: obs-plugins/64bit + data.
  install(TARGETS open-broadcast-raw-output
    RUNTIME DESTINATION "obs-plugins/64bit")
  install(DIRECTORY data/
    DESTINATION "data"
    OPTIONAL)
else()
  # OBS_MODULE_USE_DEFAULT_LOCALE expects a data/locale tree to exist.
  set(OBS_PLUGIN_DIR "$ENV{HOME}/.config/obs-studio/plugins/open-broadcast-raw-output")
  install(TARGETS open-broadcast-raw-output
    LIBRARY DESTINATION "${OBS_PLUGIN_DIR}/bin/64bit")
  install(DIRECTORY data/
    DESTINATION "${OBS_PLUGIN_DIR}/data"
    OPTIONAL)
endif()
