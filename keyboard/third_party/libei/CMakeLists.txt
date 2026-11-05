# Client-only build of the pinned upstream sources. Protocol snapshots are
# checked in so normal builds need neither network access nor Jinja2.
add_library(framekeyboard_ei STATIC
    src/brei-shared.c
    src/libei.c src/libei-button.c src/libei-callback.c src/libei-connection.c
    src/libei-device.c src/libei-event.c src/libei-fd.c src/libei-handshake.c
    src/libei-keyboard.c src/libei-log.c src/libei-ping.c src/libei-pingpong.c
    src/libei-pointer-absolute.c src/libei-pointer.c src/libei-region.c
    src/libei-scroll.c src/libei-seat.c src/libei-socket.c src/libei-text.c
    src/libei-touchscreen.c
    src/util-bits.c src/util-io.c src/util-list.c src/util-logger.c
    src/util-memfile.c src/util-memmap.c src/util-sources.c src/util-strings.c
    generated/ei-proto.c)
set_target_properties(framekeyboard_ei PROPERTIES
    C_STANDARD 11 C_STANDARD_REQUIRED YES C_EXTENSIONS YES
    C_VISIBILITY_PRESET hidden POSITION_INDEPENDENT_CODE YES)
target_include_directories(framekeyboard_ei PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/src"
    PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}" "${CMAKE_CURRENT_SOURCE_DIR}/generated")
target_link_libraries(framekeyboard_ei PRIVATE m)
# Upstream requires its protocol/state assertions in release builds too.
target_compile_options(framekeyboard_ei PRIVATE -UNDEBUG)
