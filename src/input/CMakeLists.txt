# Xbox Input → XInput Compatibility Layer

add_library(xbox_input STATIC
    xinput_device.c
    keyboard.c          # the key table and the one key map (every host)
    mkb_input.c         # mouse+keyboard pad (RECOMP_MKB): Win32 raw input on
                        # the GDI build, the SDL window's events otherwise
)

target_include_directories(xbox_input PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})

if(WIN32)
    target_link_libraries(xbox_input PUBLIC platform)
    if(NOT XBOX_SDL_WINDOW)
        target_link_libraries(xbox_input PUBLIC xinput)
    endif()
endif()
if(NOT WIN32 OR XBOX_SDL_WINDOW)
    if(NOT TARGET SDL2::SDL2)
        find_package(SDL2 REQUIRED)
    endif()
    # The SDL window's key and focus events into that table, and the pads
    # through SDL's game controllers (xinput_device.c), on Windows too.
    target_sources(xbox_input PRIVATE keyboard_sdl.c)
    target_link_libraries(xbox_input PUBLIC platform SDL2::SDL2)
    if(WIN32)
        target_compile_definitions(xbox_input PRIVATE XBOX_SDL_WINDOW=1)
    endif()
endif()

if(MSVC)
    target_compile_options(xbox_input PRIVATE /W4 /O2)
else()
    target_compile_options(xbox_input PRIVATE -Wall -Wextra -O2)
endif()
