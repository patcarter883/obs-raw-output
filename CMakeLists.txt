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

find_package(PkgConfig REQUIRED)
pkg_check_modules(OBS REQUIRED libobs)
pkg_check_modules(FRONTEND REQUIRED obs-frontend-api)
# Qt Widgets only; the dialog uses no moc-requiring features.
pkg_check_modules(QT6WIDGETS REQUIRED Qt6Widgets)

# The libobs/obs-frontend-api .pc files carry -Werror, so inheriting their flags
# would apply a DEPENDENCY's warning policy to our translation units - and in
# C++ it fires on OBS's own headers (pedantic anonymous structs in graphics/vec3.h
# and util/darray.h pulled in by obs.h). Keep our own flags below; drop theirs.
list(REMOVE_ITEM OBS_CFLAGS_OTHER "-Werror")
list(REMOVE_ITEM FRONTEND_CFLAGS_OTHER "-Werror")
# Same reason: the .pc files carry -std=gnu17 (their C standard). Forcing it onto
# our C++ TU only earns a "valid for C/ObjC but not for C++" warning; the two
# CMAKE_*_STANDARD settings above are the right place for ours.
list(FILTER OBS_CFLAGS_OTHER EXCLUDE REGEX "^-std=")
list(FILTER FRONTEND_CFLAGS_OTHER EXCLUDE REGEX "^-std=")

# The libobs .pc files carry no -I for the headers, which live under obs/.
find_path(OBS_INCLUDE_DIR obs-module.h
  PATHS /usr/include/obs /usr/local/include/obs
  REQUIRED)

add_library(open-broadcast-raw-output MODULE
  src/raw-output.c
  src/obc-settings-dialog.cpp)

# Marked SYSTEM: warnings from third-party headers are not our code's problem to
# silence, and keeping them out of our warning view makes our own -Wextra output
# readable.
target_include_directories(open-broadcast-raw-output SYSTEM PRIVATE
  ${OBS_INCLUDE_DIR} ${OBS_INCLUDE_DIRS} ${FRONTEND_INCLUDE_DIRS}
  ${QT6WIDGETS_INCLUDE_DIRS})

target_link_libraries(open-broadcast-raw-output PRIVATE
  ${OBS_LIBRARIES} ${FRONTEND_LIBRARIES} ${QT6WIDGETS_LIBRARIES})

target_compile_options(open-broadcast-raw-output PRIVATE
  ${OBS_CFLAGS_OTHER} ${FRONTEND_CFLAGS_OTHER} ${QT6WIDGETS_CFLAGS_OTHER}
  -Wall -Wextra -Wpedantic)

# OBS loads <plugins-dir>/<name>/bin/64bit/<name>.so, so the file must be named
# exactly after the plugin directory.
set_target_properties(open-broadcast-raw-output PROPERTIES
  PREFIX ""
  OUTPUT_NAME "open-broadcast-raw-output")

# OBS_MODULE_USE_DEFAULT_LOCALE expects a data/locale tree to exist.
set(OBS_PLUGIN_DIR "$ENV{HOME}/.config/obs-studio/plugins/open-broadcast-raw-output")
install(TARGETS open-broadcast-raw-output
  LIBRARY DESTINATION "${OBS_PLUGIN_DIR}/bin/64bit")
install(DIRECTORY data/
  DESTINATION "${OBS_PLUGIN_DIR}/data"
  OPTIONAL)
