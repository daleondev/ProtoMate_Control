target_sources(${PROJECT_NAME} PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/${application_role}.cpp)
target_link_libraries(${PROJECT_NAME} PRIVATE proto::common)
target_compile_options(${PROJECT_NAME} PRIVATE $<$<CONFIG:Debug>:-Og>)
