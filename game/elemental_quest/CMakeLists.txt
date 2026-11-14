add_library(elemental_common STATIC components.cpp level.cpp scene_host.cpp server_game.cpp)
target_link_libraries(elemental_common PUBLIC engine)

add_executable(elemental_server server_main.cpp)
add_executable(elemental_client client_main.cpp draw.cpp)
set(GAME_TARGETS elemental_server elemental_client)
if(ELEMENTAL_TOOLS)
    add_executable(elemental_bench bench_main.cpp)
    add_executable(elemental_check check_main.cpp)
    list(APPEND GAME_TARGETS elemental_bench elemental_check)
endif()
foreach(TARGET ${GAME_TARGETS})
    target_link_libraries(${TARGET} PRIVATE elemental_common)
    set_target_properties(${TARGET} PROPERTIES RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}")
endforeach()

add_custom_command(TARGET elemental_client POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_directory
            "${CMAKE_CURRENT_SOURCE_DIR}/assets" "$<TARGET_FILE_DIR:elemental_client>/assets")
