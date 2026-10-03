cmake_minimum_required(VERSION 3.28)
project(open-broadcast-raw-output C)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)

if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE Release)
endif()

find_package(PkgConfig REQUIRED)
pkg_check_modules(OBS REQUIRED libobs)
pkg_check_modules(FRONTEND REQUIRED obs-frontend-api)

# The libobs .pc files carry no -I for the headers, which live under obs/.
find_path(OBS_INCLUDE_DIR obs-module.h
  PATHS /usr/include/obs /usr/local/include/obs
  REQUIRED)

add_library(open-broadcast-raw-output MODULE src/raw-output.c)

target_include_directories(open-broadcast-raw-output PRIVATE
  ${OBS_INCLUDE_DIR} ${OBS_INCLUDE_DIRS} ${FRONTEND_INCLUDE_DIRS})

target_link_libraries(open-broadcast-raw-output PRIVATE
  ${OBS_LIBRARIES} ${FRONTEND_LIBRARIES})

target_compile_options(open-broadcast-raw-output PRIVATE
  ${OBS_CFLAGS_OTHER} ${FRONTEND_CFLAGS_OTHER}
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
