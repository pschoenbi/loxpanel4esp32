cmake_minimum_required(VERSION 3.16)

# Board-Profil für den Build. Anderes Board: idf.py -DLOXPANEL_BOARD=<profil> ...
if(NOT DEFINED LOXPANEL_BOARD)
    set(LOXPANEL_BOARD "crowpanel_p4_9in")
endif()
set(SDKCONFIG_DEFAULTS
    "sdkconfig.defaults"
    "components/boards/${LOXPANEL_BOARD}/sdkconfig.board"
)

include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(loxpanel_p4)
