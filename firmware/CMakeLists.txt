cmake_minimum_required(VERSION 3.16)

# core/ = everything that doesnt touch hardware (the sky, the demo, fonts, drawing),
# the PC preview in tools/preview builds the same files
# NOTE: flash is 40 MHz in sdkconfig.defaults, at 80 MHz the CYD hangs
set(EXTRA_COMPONENT_DIRS "${CMAKE_CURRENT_LIST_DIR}/../core")

# which screen, see core/config.h. BUILD.BAT passes it, 1 (the 320x240 CYD) if nobody does
if(NOT DEFINED RADAR_DISPLAY)
    set(RADAR_DISPLAY 1)
endif()

# a board whose settings differ from the rest on its chip: its own file goes after
# sdkconfig.defaults and sdkconfig.defaults.<chip> and wins over them. read only when a build
# folder's sdkconfig is made
if(RADAR_DISPLAY EQUAL 7 OR RADAR_DISPLAY EQUAL 9)   # the S3 boards with an LCD
    set(SDKCONFIG_DEFAULTS "${CMAKE_CURRENT_LIST_DIR}/sdkconfig.defaults;${CMAKE_CURRENT_LIST_DIR}/sdkconfig.defaults.s3lcd")
elseif(RADAR_DISPLAY EQUAL 10 OR RADAR_DISPLAY EQUAL 11 OR RADAR_DISPLAY EQUAL 13 OR RADAR_DISPLAY EQUAL 14 OR
       RADAR_DISPLAY EQUAL 15 OR RADAR_DISPLAY EQUAL 16)   # and those with an RGB panel (15, 16: the Waveshare 7/7B)
    set(SDKCONFIG_DEFAULTS "${CMAKE_CURRENT_LIST_DIR}/sdkconfig.defaults;${CMAKE_CURRENT_LIST_DIR}/sdkconfig.defaults.s3lcd;${CMAKE_CURRENT_LIST_DIR}/sdkconfig.defaults.s3rgb")
    if(RADAR_DISPLAY EQUAL 15 OR RADAR_DISPLAY EQUAL 16)
        list(APPEND SDKCONFIG_DEFAULTS "${CMAKE_CURRENT_LIST_DIR}/sdkconfig.defaults.wave7")
    endif()
endif()

include($ENV{IDF_PATH}/tools/cmake/project.cmake)

idf_build_set_property(COMPILE_DEFINITIONS "RADAR_DISPLAY=${RADAR_DISPLAY}" APPEND)

# the version is the first "## " line of CHANGES.md: the site shows that file as the history and
# publishes the same number in version.json, which the board compares its own with
set(CHANGES "${CMAKE_CURRENT_LIST_DIR}/../CHANGES.md")
file(STRINGS "${CHANGES}" FLIGHT_VERSION REGEX "^## [0-9]" LIMIT_COUNT 1)
string(REGEX REPLACE "^## ([0-9.]+).*" "\\1" FLIGHT_VERSION "${FLIGHT_VERSION}")
if(NOT FLIGHT_VERSION MATCHES "^[0-9]+(\\.[0-9]+)*$")
    message(FATAL_ERROR "no version in ${CHANGES}: its first \"## \" line should be one, like ## 0.3")
endif()
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${CHANGES}")   # a new version: configure again
idf_build_set_property(COMPILE_DEFINITIONS "FLIGHT_VERSION=\"${FLIGHT_VERSION}\"" APPEND)
project(owlos-flight-radar)
