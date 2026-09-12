cmake_minimum_required(VERSION 3.16)

set(PROJECT_VER "1.1.4-dev")
include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(esp32_s3_poe_eth_8di_8ro_bacnet VERSION ${PROJECT_VER})

# Match the selected NTP server's source/port and echoed request timestamp.
# This configures the pinned lwIP implementation without editing the submodule.
idf_component_get_property(lwip_component_lib lwip COMPONENT_LIB)
target_compile_definitions(${lwip_component_lib} PRIVATE SNTP_CHECK_RESPONSE=2)
