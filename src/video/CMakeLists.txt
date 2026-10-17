# Shared FMV player: Media Foundation decode onto a D3D8 texture.
add_library(xbox_video STATIC
    video_player.c
    video_pump.c
    fb_present.c
)
target_include_directories(xbox_video PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${CMAKE_CURRENT_SOURCE_DIR}/..
)
# xbox_input: the windows record keys in its key table (keyboard.c).
target_link_libraries(xbox_video PUBLIC xbox_d3d8 xbox_input)
if(WIN32)
    target_link_libraries(xbox_video PUBLIC mfplat mfreadwrite mfuuid mf ole32)
endif()
if(NOT WIN32 OR XBOX_SDL_WINDOW)
    # The framebuffer window through SDL2 (fb_present_sdl.c): on POSIX on the
    # process main thread, which then also pumps the events the game
    # controllers are read from; on Windows (XBOX_SDL_WINDOW) on a thread of
    # its own, as the GDI window's.
    if(NOT TARGET SDL2::SDL2)
        find_package(SDL2 REQUIRED)
    endif()
    target_sources(xbox_video PRIVATE fb_present_sdl.c)
    target_link_libraries(xbox_video PUBLIC SDL2::SDL2)
    if(WIN32)
        target_compile_definitions(xbox_video PRIVATE XBOX_SDL_WINDOW=1)
    endif()
endif()
