cmake_minimum_required(VERSION 3.24)
project(framekeyboard VERSION 0.8.1 LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)
# Keep build-machine directories out of __FILE__, assertions and debug paths.
# RelWithDebInfo remains useful locally; packaging strips only its installed copy.
if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
    add_compile_options("-ffile-prefix-map=${CMAKE_CURRENT_SOURCE_DIR}=."
                        "-ffile-prefix-map=${CMAKE_CURRENT_BINARY_DIR}=./build")
    if(CMAKE_SYSROOT)
        add_compile_options("-ffile-prefix-map=${CMAKE_SYSROOT}=/")
    endif()
endif()
find_package(Python3 REQUIRED COMPONENTS Interpreter)
find_package(PkgConfig REQUIRED)
find_package(Threads REQUIRED)
pkg_check_modules(NATIVE REQUIRED IMPORTED_TARGET cairo pangocairo xkbcommon json-c)
pkg_check_modules(WAYLAND_CLIENT REQUIRED IMPORTED_TARGET wayland-client)
# SteamOS libei 1.4.1 crashes on an aborted negotiation. Keep the fixed client
# inside this app; never replace the runtime library used by Steam itself.
add_subdirectory(third_party/libei)
find_program(WAYLAND_SCANNER wayland-scanner REQUIRED)
set(IME_XML "${CMAKE_CURRENT_SOURCE_DIR}/third_party/gamescope/gamescope-input-method.xml")
set(IME_HEADER "${CMAKE_CURRENT_BINARY_DIR}/generated/gamescope-input-method-client.h")
set(IME_CODE "${CMAKE_CURRENT_BINARY_DIR}/generated/gamescope-input-method-protocol.c")
add_custom_command(OUTPUT "${IME_HEADER}" "${IME_CODE}"
    COMMAND "${CMAKE_COMMAND}" -E make_directory "${CMAKE_CURRENT_BINARY_DIR}/generated"
    COMMAND "${WAYLAND_SCANNER}" client-header "${IME_XML}" "${IME_HEADER}"
    COMMAND "${WAYLAND_SCANNER}" private-code "${IME_XML}" "${IME_CODE}"
    DEPENDS "${IME_XML}" VERBATIM)
pkg_check_modules(SDL REQUIRED IMPORTED_TARGET sdl2)
pkg_check_modules(VULKAN REQUIRED IMPORTED_TARGET vulkan)

file(GLOB LAYOUT_FILES CONFIGURE_DEPENDS layouts/*.json)
file(GLOB PROFILE_FILES CONFIGURE_DEPENDS layouts/*.json languages/*.json themes/*.json)
set(PROFILE_HEADER "${CMAKE_CURRENT_BINARY_DIR}/generated/bundled_profiles.hpp")
add_custom_command(OUTPUT "${PROFILE_HEADER}"
    COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/tools/validate_layout.py"
            ${LAYOUT_FILES}
    COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/tools/embed_defaults.py"
            "${CMAKE_CURRENT_SOURCE_DIR}" "${PROFILE_HEADER}"
    DEPENDS ${PROFILE_FILES} tools/embed_defaults.py tools/validate_layout.py
    VERBATIM)
add_library(keyboard_core src/vr_lifecycle.cpp src/vr_runtime.cpp src/config.cpp src/input.cpp src/output.cpp src/panel.cpp src/settings_ui.cpp src/settings_writer.cpp src/startup.cpp src/app.cpp src/instance.cpp src/placement.cpp src/placement_store.cpp src/grip.cpp src/ei_input.cpp src/japanese.cpp src/cjk.cpp src/text_input.cpp src/native_replacement.cpp src/updates.cpp "${IME_HEADER}" "${IME_CODE}" "${PROFILE_HEADER}")
target_include_directories(keyboard_core PUBLIC include PRIVATE "${CMAKE_CURRENT_BINARY_DIR}/generated")
target_link_libraries(keyboard_core PUBLIC PkgConfig::NATIVE framekeyboard_ei PkgConfig::WAYLAND_CLIENT ${CMAKE_DL_LIBS} Threads::Threads)
target_compile_options(keyboard_core PRIVATE -Wall -Wextra -Wpedantic -Wconversion)
# Frame already supplies these engines. Minimal host builds can still run the
# ordinary keyboard; unsupported composition profiles fail explicitly on Apply.
pkg_check_modules(GLIB REQUIRED IMPORTED_TARGET glib-2.0)
pkg_check_modules(PYZY QUIET IMPORTED_TARGET pyzy-1.0)
pkg_check_modules(HANGUL QUIET IMPORTED_TARGET libhangul)
foreach(engine PYZY HANGUL)
    if(${engine}_FOUND)
        if(engine STREQUAL "PYZY")
            set(module pinyin)
        else()
            set(module hangul)
        endif()
        add_library(cjk_${module} MODULE src/cjk_engine.cpp)
        target_link_libraries(cjk_${module} PRIVATE PkgConfig::GLIB PkgConfig::${engine})
        target_compile_definitions(cjk_${module} PRIVATE FRAMEKEYBOARD_HAVE_${engine}=1)
        target_compile_options(cjk_${module} PRIVATE -Wall -Wextra -Wpedantic -Wconversion)
        if(CMAKE_CROSSCOMPILING)
            target_link_options(cjk_${module} PRIVATE -fuse-ld=lld)
        endif()
        set_target_properties(cjk_${module} PROPERTIES
            PREFIX "" OUTPUT_NAME "framekeyboard-${module}"
            LIBRARY_OUTPUT_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/engines"
            CXX_VISIBILITY_PRESET hidden VISIBILITY_INLINES_HIDDEN YES)
        add_dependencies(keyboard_core cjk_${module})
        install(TARGETS cjk_${module} LIBRARY DESTINATION lib/framekeyboard)
    elseif(CMAKE_CROSSCOMPILING)
        message(FATAL_ERROR "Frame releases require ${engine} headers and libraries in the sysroot")
    else()
        message(STATUS "${engine} unavailable: this host build cannot compose that language")
    endif()
endforeach()

# Link the matching architecture from the sysroot; embed only the device runtime path.
if(CMAKE_CROSSCOMPILING)
    set(OPENVR_LIBRARY "${CMAKE_SYSROOT}/opt/steamvr/bin/linuxarm64/libopenvr_api.so" CACHE FILEPATH "OpenVR library")
    set(OPENVR_RUNTIME_PATH "/opt/steamvr/bin/linuxarm64")
else()
    find_library(OPENVR_LIBRARY openvr_api HINTS
        "$ENV{HOME}/.local/share/Steam/steamapps/common/SteamVR/bin/linux64"
        "$ENV{HOME}/.local/share/Steam/ubuntu12_64" REQUIRED)
    get_filename_component(OPENVR_RUNTIME_PATH "${OPENVR_LIBRARY}" DIRECTORY)
endif()
add_library(overlay_texture third_party/overlay_texture/vk_texture.cpp)
target_include_directories(overlay_texture PUBLIC third_party/overlay_texture third_party/openvr)
target_link_libraries(overlay_texture PUBLIC PkgConfig::VULKAN "${OPENVR_LIBRARY}")
add_executable(framekeyboard src/main.cpp src/preview.cpp src/vr.cpp)
add_executable(framekeyboard-native-guard src/native_restore_guard.cpp)
target_include_directories(framekeyboard-native-guard PRIVATE third_party/openvr)
target_link_libraries(framekeyboard-native-guard PRIVATE PkgConfig::NATIVE "${OPENVR_LIBRARY}")
set_target_properties(framekeyboard-native-guard PROPERTIES BUILD_WITH_INSTALL_RPATH TRUE INSTALL_RPATH "${OPENVR_RUNTIME_PATH}")
add_dependencies(framekeyboard framekeyboard-native-guard)
target_link_libraries(framekeyboard PRIVATE keyboard_core overlay_texture PkgConfig::SDL)
target_compile_options(framekeyboard PRIVATE -Wall -Wextra -Wpedantic -Wconversion)
target_compile_definitions(framekeyboard PRIVATE FRAMEKEYBOARD_VERSION="${PROJECT_VERSION}")
# Development binaries find these beside themselves; packages use share/framekeyboard.
# Do not embed an absolute source-tree path in the executable for this fallback.
file(MAKE_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/vr")
foreach(manifest actions.json frame-controller.json)
    configure_file("vr/${manifest}" "vr/${manifest}" COPYONLY)
endforeach()
set_target_properties(framekeyboard PROPERTIES BUILD_WITH_INSTALL_RPATH TRUE INSTALL_RPATH "${OPENVR_RUNTIME_PATH}")

file(MAKE_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/bridge")
foreach(bridge_file native_bridge.py steam_bridge.js)
    configure_file("bridge/${bridge_file}" "bridge/${bridge_file}" COPYONLY)
endforeach()
file(MAKE_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/update")
configure_file(update/update_helper.py update/update_helper.py COPYONLY)
include(CTest)
if(BUILD_TESTING)
    add_executable(vr-lifecycle-tests tests/vr_lifecycle_tests.cpp)
    target_link_libraries(vr-lifecycle-tests PRIVATE keyboard_core)
    target_compile_options(vr-lifecycle-tests PRIVATE -Wall -Wextra -Wpedantic)
    add_test(NAME vr-lifecycle COMMAND vr-lifecycle-tests)
    add_executable(ei-handshake-tests tests/ei_handshake_tests.cpp)
    target_link_libraries(ei-handshake-tests PRIVATE framekeyboard_ei)
    add_test(NAME ei-handshake COMMAND ei-handshake-tests)
    add_executable(output-tests tests/output_tests.cpp)
    target_link_libraries(output-tests PRIVATE keyboard_core)
    add_test(NAME ordered-output COMMAND output-tests)
    add_executable(startup-tests tests/startup_tests.cpp)
    target_link_libraries(startup-tests PRIVATE keyboard_core)
    add_test(NAME startup COMMAND startup-tests)
    find_program(SYSTEMD_ANALYZE systemd-analyze)
    if(SYSTEMD_ANALYZE AND NOT CMAKE_CROSSCOMPILING)
        add_test(NAME startup-unit COMMAND "${Python3_EXECUTABLE}"
            "${CMAKE_CURRENT_SOURCE_DIR}/tests/startup_unit_tests.py"
            "$<TARGET_FILE:startup-tests>" "${SYSTEMD_ANALYZE}")
    endif()
    add_executable(update-tests tests/update_tests.cpp)
    target_link_libraries(update-tests PRIVATE keyboard_core)
    add_test(NAME updates COMMAND update-tests)
    if(NOT CMAKE_CROSSCOMPILING)
        add_test(NAME update-helper COMMAND "${Python3_EXECUTABLE}"
            "${CMAKE_CURRENT_SOURCE_DIR}/tests/update_helper_tests.py"
            "${CMAKE_CURRENT_SOURCE_DIR}/update/update_helper.py")
    endif()
    add_executable(native-replacement-tests tests/native_replacement_tests.cpp)
    target_link_libraries(native-replacement-tests PRIVATE keyboard_core)
    add_test(NAME native-replacement COMMAND native-replacement-tests)
    if(NOT CMAKE_CROSSCOMPILING)
        add_test(NAME timing-ordering COMMAND "${Python3_EXECUTABLE}"
            "${CMAKE_CURRENT_SOURCE_DIR}/tests/timing_order_tests.py"
            "${CMAKE_CURRENT_SOURCE_DIR}")
        add_test(NAME native-helper-spawn COMMAND "${Python3_EXECUTABLE}"
            "${CMAKE_CURRENT_SOURCE_DIR}/tests/native_helper_spawn_tests.py"
            "$<TARGET_FILE:native-replacement-tests>"
            "${CMAKE_CURRENT_SOURCE_DIR}/bridge/native_bridge.py")
        add_test(NAME native-bridge-transport COMMAND "${Python3_EXECUTABLE}"
            "${CMAKE_CURRENT_SOURCE_DIR}/tests/native_bridge_transport_tests.py"
            "${CMAKE_CURRENT_SOURCE_DIR}/bridge/native_bridge.py")
        add_test(NAME replacement-measurement COMMAND "${Python3_EXECUTABLE}"
            "${CMAKE_CURRENT_SOURCE_DIR}/tests/replacement_measurement_tests.py"
            "${CMAKE_CURRENT_SOURCE_DIR}/scripts/measure-replacement.py")
        find_program(NODE_EXECUTABLE node)
        if(NODE_EXECUTABLE)
            add_test(NAME native-bridge COMMAND "${NODE_EXECUTABLE}"
                "${CMAKE_CURRENT_SOURCE_DIR}/tests/native_bridge_tests.js"
                "${CMAKE_CURRENT_SOURCE_DIR}/bridge/steam_bridge.js")
        endif()
    endif()
    # Opt-in timing probe: never sends input; --vr uses its own hidden overlay.
    add_executable(render-benchmark tests/render_benchmark.cpp)
    target_link_libraries(render-benchmark PRIVATE keyboard_core overlay_texture Threads::Threads)
    target_compile_options(render-benchmark PRIVATE -Wall -Wextra -Wpedantic)
    set_target_properties(render-benchmark PROPERTIES BUILD_WITH_INSTALL_RPATH TRUE INSTALL_RPATH "${OPENVR_RUNTIME_PATH}")
    add_executable(language-tests tests/language_tests.cpp)
    target_link_libraries(language-tests PRIVATE keyboard_core)
    target_compile_options(language-tests PRIVATE -Wall -Wextra -Wpedantic)
    add_test(NAME languages COMMAND language-tests --core)
    add_test(NAME korean-input COMMAND language-tests --korean)
    add_test(NAME chinese-input COMMAND language-tests --chinese)
    set_tests_properties(korean-input chinese-input PROPERTIES SKIP_RETURN_CODE 77)
    if(NOT CMAKE_CROSSCOMPILING)
        add_test(NAME optional-engines COMMAND "${Python3_EXECUTABLE}"
            "${CMAKE_CURRENT_SOURCE_DIR}/tests/engine_loading_tests.py" "$<TARGET_FILE:language-tests>")
    endif()
    add_executable(japanese-tests tests/japanese_tests.cpp)
    target_link_libraries(japanese-tests PRIVATE keyboard_core)
    add_test(NAME japanese-input COMMAND japanese-tests)
    add_executable(settings-tests tests/settings_tests.cpp)
    target_link_libraries(settings-tests PRIVATE keyboard_core)
    target_compile_options(settings-tests PRIVATE -Wall -Wextra -Wpedantic)
    add_test(NAME settings COMMAND settings-tests)
    add_executable(keyboard-tests tests/keyboard_tests.cpp)
    add_executable(render-tests tests/render_tests.cpp)
    target_link_libraries(render-tests PRIVATE keyboard_core)
    target_compile_options(render-tests PRIVATE -Wall -Wextra -Wpedantic)
    add_test(NAME rendering COMMAND render-tests)
    target_link_libraries(keyboard-tests PRIVATE keyboard_core)
    target_compile_options(keyboard-tests PRIVATE -Wall -Wextra -Wpedantic)
    add_test(NAME keyboard-core COMMAND keyboard-tests)
    add_executable(placement-instance-tests tests/placement_instance_tests.cpp)
    target_link_libraries(placement-instance-tests PRIVATE keyboard_core Threads::Threads)
    target_compile_options(placement-instance-tests PRIVATE -Wall -Wextra -Wpedantic)
    add_test(NAME placement-instance COMMAND placement-instance-tests)
    if(NOT CMAKE_CROSSCOMPILING)
        add_test(NAME installer COMMAND "${Python3_EXECUTABLE}"
            "${CMAKE_CURRENT_SOURCE_DIR}/tests/installer_tests.py"
            "${CMAKE_CURRENT_SOURCE_DIR}" "$<TARGET_FILE:framekeyboard>")
        pkg_check_modules(EIS QUIET IMPORTED_TARGET libeis-1.0>=1.6)
        if(EIS_FOUND)
            add_executable(ei-recovery-tests tests/ei_recovery_tests.cpp)
            target_link_libraries(ei-recovery-tests PRIVATE keyboard_core PkgConfig::EIS Threads::Threads)
            pkg_check_modules(WAYLAND_SERVER QUIET IMPORTED_TARGET wayland-server)
            if(WAYLAND_SERVER_FOUND)
                set(IME_SERVER_HEADER "${CMAKE_CURRENT_BINARY_DIR}/generated/gamescope-input-method-server.h")
                add_custom_command(OUTPUT "${IME_SERVER_HEADER}"
                    COMMAND "${WAYLAND_SCANNER}" server-header "${IME_XML}" "${IME_SERVER_HEADER}"
                    DEPENDS "${IME_XML}" VERBATIM)
                target_sources(ei-recovery-tests PRIVATE "${IME_SERVER_HEADER}")
                target_include_directories(ei-recovery-tests PRIVATE "${CMAKE_CURRENT_BINARY_DIR}/generated")
                target_link_libraries(ei-recovery-tests PRIVATE PkgConfig::WAYLAND_SERVER)
                target_compile_definitions(ei-recovery-tests PRIVATE FRAMEKEYBOARD_TEST_WAYLAND_SERVER=1)
            endif()
            add_test(NAME ei-recovery COMMAND ei-recovery-tests)
        endif()
    endif()
    # Opt-in only: maps a disposable window and verifies focus before each key.
    pkg_check_modules(X11 REQUIRED IMPORTED_TARGET x11)
    add_executable(unicode-target-probe tests/unicode_target_probe.cpp)
    target_link_libraries(unicode-target-probe PRIVATE keyboard_core PkgConfig::X11)
    add_executable(japanese-target-probe tests/japanese_target_probe.cpp)
    target_link_libraries(japanese-target-probe PRIVATE keyboard_core PkgConfig::X11)
    add_executable(ei-target-probe tests/ei_target_probe.cpp)
    target_link_libraries(ei-target-probe PRIVATE keyboard_core PkgConfig::X11)
    # Opt-in read-only validation against the installed Frame controller models.
    add_executable(stick-component-probe tests/stick_component_probe.cpp)
    target_link_libraries(stick-component-probe PRIVATE keyboard_core overlay_texture)
    set_target_properties(stick-component-probe PROPERTIES BUILD_WITH_INSTALL_RPATH TRUE INSTALL_RPATH "${OPENVR_RUNTIME_PATH}")
    add_executable(vr-panel-probe tests/vr_panel_probe.cpp)
    target_link_libraries(vr-panel-probe PRIVATE keyboard_core overlay_texture)
    set_target_properties(vr-panel-probe PROPERTIES BUILD_WITH_INSTALL_RPATH TRUE INSTALL_RPATH "${OPENVR_RUNTIME_PATH}")
    # Opt-in: sends physical haptic output, so it must not run in CTest.
    add_executable(haptics-probe tests/haptics_probe.cpp)
    target_link_libraries(haptics-probe PRIVATE keyboard_core overlay_texture)
    set_target_properties(haptics-probe PROPERTIES BUILD_WITH_INSTALL_RPATH TRUE INSTALL_RPATH "${OPENVR_RUNTIME_PATH}")
    # Intentionally excluded from CTest: requires an accessible kernel uinput device.
    add_executable(uinput-smoke-test tests/uinput_smoke.cpp)
    target_link_libraries(uinput-smoke-test PRIVATE keyboard_core)
endif()
install(TARGETS framekeyboard framekeyboard-native-guard RUNTIME DESTINATION bin)
install(DIRECTORY docs DESTINATION .)
install(DIRECTORY layouts languages themes config vr DESTINATION share/framekeyboard)
install(FILES bridge/native_bridge.py bridge/steam_bridge.js DESTINATION share/framekeyboard/bridge)
install(FILES update/update_helper.py DESTINATION share/framekeyboard/update)
install(FILES third_party/README.md DESTINATION third_party)
install(FILES third_party/openvr/LICENSE DESTINATION third_party/openvr)
install(FILES third_party/overlay_texture/LICENSE DESTINATION third_party/overlay_texture)
install(FILES third_party/libei/COPYING third_party/libei/README.md third_party/libei/provenance.json DESTINATION third_party/libei)

install(FILES third_party/gamescope/gamescope-input-method.xml DESTINATION third_party/gamescope)

install(FILES LICENSE README.md CHANGELOG.md CONTRIBUTING.md DESTINATION .)
