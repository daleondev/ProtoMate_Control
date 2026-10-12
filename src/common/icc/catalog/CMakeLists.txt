add_library(application_icc_catalog INTERFACE)
add_library(application::icc::catalog ALIAS application_icc_catalog)

target_sources(application_icc_catalog
    INTERFACE
        ${CMAKE_CURRENT_SOURCE_DIR}/topics.hpp
        ${CMAKE_CURRENT_SOURCE_DIR}/services.hpp
        ${CMAKE_CURRENT_SOURCE_DIR}/actions.hpp
)
