if(EMSCRIPTEN)
    # The browser client: elemental_client.{js,wasm,data}, loaded by the page in web/
    add_executable(elemental_client client_main.cpp draw.cpp components.cpp level.cpp)
    target_link_libraries(elemental_client PRIVATE engine)
    target_link_options(elemental_client PRIVATE
        "--preload-file=${CMAKE_CURRENT_SOURCE_DIR}/assets@/assets"
        "--exclude-file=*.DS_Store" "--exclude-file=*background2.png"
        -sALLOW_MEMORY_GROWTH=1 -sINITIAL_MEMORY=67108864
        -sEXPORTED_RUNTIME_METHODS=callMain -sINVOKE_RUN=0)
    set_target_properties(elemental_client PROPERTIES RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/web")
    # The page around the game, so build/web is the whole site
    add_custom_command(TARGET elemental_client POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_directory "${PROJECT_SOURCE_DIR}/web" "${CMAKE_BINARY_DIR}/web"
        COMMAND ${CMAKE_COMMAND} -E make_directory "${CMAKE_BINARY_DIR}/web/img"
        COMMAND ${CMAKE_COMMAND} -E copy "${CMAKE_CURRENT_SOURCE_DIR}/assets/background.jpeg"
                "${CMAKE_CURRENT_SOURCE_DIR}/assets/player_fire.png" "${CMAKE_CURRENT_SOURCE_DIR}/assets/player_water.png"
                "${CMAKE_CURRENT_SOURCE_DIR}/assets/player_earth.png" "${CMAKE_CURRENT_SOURCE_DIR}/assets/player_air.png"
                "${CMAKE_BINARY_DIR}/web/img")
    return()
endif()

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
