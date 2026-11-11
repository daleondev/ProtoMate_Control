if(NOT STM32_CORE STREQUAL "CM7")
    message(FATAL_ERROR "GIGA Wi-Fi belongs to M7")
endif()
set(NX_USER_FILE ${CMAKE_CURRENT_SOURCE_DIR}/nx_user.h CACHE FILEPATH "" FORCE)
set(NXD_ENABLE_FILE_SERVERS OFF CACHE BOOL "" FORCE)
set(NXD_ENABLE_BSD OFF CACHE BOOL "" FORCE)
add_subdirectory(${PROJECT_SOURCE_DIR}/external/netxduo ${PROJECT_BINARY_DIR}/netxduo EXCLUDE_FROM_ALL)

set(sdk ${PROJECT_SOURCE_DIR}/external/infineon-stm32/Middlewares/Third_Party)
set(whd ${sdk}/wifi-host-driver/COMPONENT_WIFI5)
set(pal ${sdk}/pal)
find_package(Python3 REQUIRED COMPONENTS Interpreter)
set(sdio_source ${pal}/targets/TARGET_STM32/Src/stm32_cyhal_sdio.c)
set(sdio_adapted ${CMAKE_CURRENT_BINARY_DIR}/stm32_cyhal_sdio.c)
execute_process(COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/prepare_sdio.py
    ${sdio_source} ${sdio_adapted} COMMAND_ERROR_IS_FATAL ANY)
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS
    ${sdio_source} ${CMAKE_CURRENT_SOURCE_DIR}/prepare_sdio.py)
file(GLOB whd_sources CONFIGURE_DEPENDS ${whd}/src/*.c)
add_library(platform_wifi_vendor STATIC
    ${whd_sources}
    ${whd}/src/bus_protocols/whd_bus.c
    ${whd}/src/bus_protocols/whd_bus_common.c
    ${whd}/src/bus_protocols/whd_bus_sdio_protocol.c
    ${whd}/resources/resource_imp/whd_resources.c
    ${whd}/resources/firmware/COMPONENT_4343W/4343WA1_bin.c
    ${whd}/resources/clm/COMPONENT_4343W/COMPONENT_MURATA-1DX/4343WA1_clm_blob.c
    ${sdk}/whd-bsp-integration/cybsp_wifi.c
    ${sdk}/whd-bsp-integration/COMPONENT_NETXDUO/cy_network_buffer_netxduo.c
    ${sdk}/netxduo-network-interface-integration/source/cy_netxduo.c
    ${sdk}/wifi-connection-manager/source/cy_wcm.c
    ${sdk}/connectivity-utilities/cy_log/cy_log.c
    ${sdk}/connectivity-utilities/cy_string/cy_string_utils.c
    ${sdk}/connectivity-utilities/linked_list/cy_linked_list.c
    ${sdk}/connectivity-utilities/network/cy_nw_helper_common.c
    ${sdk}/connectivity-utilities/network/COMPONENT_NETXDUO/cy_nw_helper.c
    ${pal}/common/Src/cyhal_system.c
    ${sdio_adapted}
    ${pal}/targets/TARGET_STM32/Src/stm32_cyhal_gpio.c
    ${pal}/targets/TARGET_STM32/Src/stm32_cyhal_trng.c
    ${pal}/abstraction-rtos/source/COMPONENT_THREADX/cyabs_rtos_threadx.c
    ${pal}/abstraction-rtos/source/cy_worker_thread.c
    ${PROJECT_SOURCE_DIR}/external/stm32h7xx-hal-driver/Src/stm32h7xx_hal_sd.c
    ${PROJECT_SOURCE_DIR}/external/stm32h7xx-hal-driver/Src/stm32h7xx_ll_sdmmc.c)
target_include_directories(platform_wifi_vendor PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/giga
    ${pal}/hal/include ${pal}/core-lib/include ${pal}/common/Inc
    ${pal}/targets/TARGET_STM32/Inc
    ${pal}/abstraction-rtos/include ${pal}/abstraction-rtos/include/COMPONENT_THREADX
    ${whd}/inc ${whd}/src ${whd}/src/include ${whd}/src/bus_protocols
    ${whd}/resources/resource_imp
    ${whd}/resources/firmware/COMPONENT_4343W
    ${whd}/resources/clm/COMPONENT_4343W/COMPONENT_MURATA-1DX
    ${whd}/resources/nvram/COMPONENT_4343W/COMPONENT_MURATA-1DX
    ${sdk}/whd-bsp-integration ${sdk}/whd-bsp-integration/COMPONENT_NETXDUO
    ${sdk}/netxduo-network-interface-integration/include
    ${sdk}/wifi-connection-manager/include ${sdk}/wifi-connection-manager/source
    ${sdk}/wifi-connection-manager/source/COMPONENT_4343W
    ${sdk}/connectivity-utilities ${sdk}/connectivity-utilities/cy_log
    ${sdk}/connectivity-utilities/cy_string ${sdk}/connectivity-utilities/linked_list
    ${sdk}/connectivity-utilities/network
    $<TARGET_PROPERTY:platform,INTERFACE_INCLUDE_DIRECTORIES>)
target_compile_definitions(platform_wifi_vendor PUBLIC
    $<TARGET_PROPERTY:platform,INTERFACE_COMPILE_DEFINITIONS>
    COMPONENT_WIFI5 COMPONENT_4343W COMPONENT_MURATA_1DX COMPONENT_WIFI_INTERFACE_SDIO
    COMPONENT_THREADX COMPONENT_NETXDUO NETXDUO_ENABLED THREADX_ENABLED
    CY_RTOS_AWARE CY_USING_HAL HAL_SD_MODULE_ENABLED WHD_DYNAMIC_NVRAM
    CY_NETWORK_DISABLE_DHCP_SERVER
    CY_WIFI_HOST_WAKE_SW_FORCE=0 CY_WIFI_COUNTRY=WHD_COUNTRY_GERMANY
    TX_PACKET_POOL_SIZE=12 RX_PACKET_POOL_SIZE=12 CY_WIFI_THREAD_STACK_SIZE=6144)
set_property(TARGET platform_wifi_vendor PROPERTY COMPILE_OPTIONS -Os -ffunction-sections -fdata-sections)
target_link_libraries(platform_wifi_vendor PUBLIC netxduo threadx)
add_library(platform_network STATIC giga/Board.c Wifi.c)
add_library(platform::network ALIAS platform_network)
target_include_directories(platform_network PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(platform_network PUBLIC platform_wifi_vendor)
target_compile_options(platform_network PRIVATE -Wall -Wextra -Wpedantic)
