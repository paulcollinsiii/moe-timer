# Lock-screen art. The committed main/sprite_data.c is generated from the
# public templates in tools/sprites/ and is what a plain clone compiles, with
# no Python dependency. When sprites.local/ (gitignored, at the repo root)
# holds any <slot>.png, the sprite data is instead generated into the build
# directory from the templates with those files merged over them
# (tools/gen_sprites.py --override-dir), and the committed copy is not
# compiled. Slots are fixed in name and size; the generator refuses a
# mismatch, so a bad override fails the build rather than the screen. See
# docs/developer_setup.md ("Lock-screen art").
set(SPRITE_DATA_SRCS "sprite_data.c")
if(NOT CMAKE_BUILD_EARLY_EXPANSION)
    set(SPRITE_OVERRIDE_DIR "${PROJECT_DIR}/sprites.local")
    # CONFIGURE_DEPENDS: adding or removing an override re-runs cmake, so the
    # choice between the two sources is never stale.
    # Files only, any letter case of the extension: charge.PNG must still
    # enter override mode so the generator runs and refuses it by name,
    # instead of the template shipping unnoticed.
    file(GLOB SPRITE_OVERRIDE_FILES LIST_DIRECTORIES false CONFIGURE_DEPENDS "${SPRITE_OVERRIDE_DIR}/*")
    set(SPRITE_OVERRIDE_PNGS "")
    foreach(f ${SPRITE_OVERRIDE_FILES})
        if(f MATCHES "\\.[pP][nN][gG]$")
            list(APPEND SPRITE_OVERRIDE_PNGS "${f}")
        endif()
    endforeach()
    if(SPRITE_OVERRIDE_PNGS)
        find_program(UV_EXECUTABLE uv HINTS $ENV{HOME}/.local/bin)
        if(NOT UV_EXECUTABLE)
            message(FATAL_ERROR
                "sprites.local/ holds override art but uv was not found. "
                "Install uv (https://docs.astral.sh/uv/) so tools/gen_sprites.py can "
                "build it in, or empty sprites.local/ to build the public templates.")
        endif()
        set(SPRITE_OVERRIDE_SLOTS "")
        foreach(png ${SPRITE_OVERRIDE_PNGS})
            # the generator's slot name: only the final .png is stripped
            # (NAME_WE would report charge.v2.png as "charge")
            get_filename_component(slot "${png}" NAME)
            string(REGEX REPLACE "\\.[pP][nN][gG]$" "" slot "${slot}")
            list(APPEND SPRITE_OVERRIDE_SLOTS "${slot}")
        endforeach()
        list(JOIN SPRITE_OVERRIDE_SLOTS ", " SPRITE_OVERRIDE_SLOTS_TEXT)
        message(STATUS "Lock-screen art: overriding slots from sprites.local/: ${SPRITE_OVERRIDE_SLOTS_TEXT}")

        file(GLOB SPRITE_TEMPLATE_PNGS CONFIGURE_DEPENDS "${PROJECT_DIR}/tools/sprites/*.png")
        set(SPRITE_DATA_GEN "${CMAKE_CURRENT_BINARY_DIR}/sprite_data.c")
        # --no-header: the build never writes include/sprite_data.h in the
        # source tree. The generator leaves an unchanged C alone; ninja's
        # custom-command rule restats, so that leaves the step clean (the
        # Makefile generator would re-run it every build: harmless, and the
        # project builds with ninja).
        add_custom_command(
            OUTPUT "${SPRITE_DATA_GEN}"
            COMMAND ${UV_EXECUTABLE} run "${PROJECT_DIR}/tools/gen_sprites.py"
                    --override-dir "${SPRITE_OVERRIDE_DIR}" --c-out "${SPRITE_DATA_GEN}" --no-header
            DEPENDS "${PROJECT_DIR}/tools/gen_sprites.py" ${SPRITE_TEMPLATE_PNGS} ${SPRITE_OVERRIDE_PNGS}
            COMMENT "Generating sprite data (overrides: ${SPRITE_OVERRIDE_SLOTS_TEXT})"
            VERBATIM)
        set(SPRITE_DATA_SRCS "")
    endif()
endif()

idf_component_register(
    SRCS
        "main.c"
        "alerts.c"
        "app_state.c"
        "timer.c"
        "timer_defs.c"
        "timer_persist.c"
        "wake_policy.c"
        "buttons_policy.c"
        "wake_flow.c"
        "status_led.c"
        "config_cache.c"
        "display.c"
        "display_screens.c"
        "display_layout.c"
        "ntp.c"
        "net_window.c"
        "net_apply.c"
        "wifi_session.c"
        "stats_json.c"
        "mqtt_ha.c"
        "ha_day_cmds.c"
        "mqtt_rx.c"
        "mqtt_topics.c"
        "config_apply.c"
        "config_validate.c"
        "ota_policy.c"
        "ota_flow.c"
        "ota_task.c"
        "ota_url.c"
        "ota_facts.c"
        "ota.c"
        "panic_diag.c"
        "ha_config.c"
        "cmd_apply.c"
        "device_id.c"
        "light.c"
        "nvs_config.c"
        "mqtt_form.c"
        "dns_reply.c"
        "setup_session.c"
        "setup_session_idf.c"
        "sleep_plan_idf.c"
        "setup_screens.c"
        "setup_trigger.c"
        "qr_render.c"
        "sprite.c"
        "sprite_pick.c"
        ${SPRITE_DATA_SRCS}
        "../lib/cJSON/cJSON.c"
        "../lib/qrcodegen/qrcodegen.c"
        "schedule.c"
        "buttons.c"
        "button_actions.c"
        "button_latch.c"
        "audio.c"
        "tones.c"
        "wav_header.c"
        "battery.c"
        "battery_soc.c"
        "battery_policy.c"
        "neopixel.c"
        "quiet_hours.c"
        "bedtime.c"
        "sleep_plan.c"
        "lock_gate.c"
        "chores.c"
        "chore_store.c"
        "hal_time.c"
        "hal_nvs.c"
    INCLUDE_DIRS
        "."
        "../include"
        "../lib/cJSON"
        "../lib/qrcodegen"
    REQUIRES
        nvs_flash
        esp_wifi
        esp_netif
        esp_event
        esp_timer
        esp_driver_gpio
        esp_driver_dac
        esp_driver_rmt
        esp_partition
        esp_adc
        log
        esp-tls
        lwip
        espressif__mqtt
        esp_app_format
        esp_http_client
        esp_https_ota
        app_update
        ssd1680
        esp_http_server
        protocomm
        espressif__network_provisioning
    # Root CA for the OTA endpoint, pinned into the image.
    #
    # TXTFILES, not BINFILES: esp_http_client_config_t::cert_pem takes a
    # NUL-TERMINATED PEM string, and TXTFILES is the variant that appends
    # the NUL. Embedding this as a binary blob yields a buffer that is not
    # a C string, and the failure is a TLS handshake error at runtime
    # rather than anything the build would catch.
    #
    # Referenced from ota.c (plan task 11) as:
    #     extern const uint8_t ota_ca_pem_start[] asm("_binary_ota_ca_pem_start");
    # The symbol is derived from the FILENAME, so renaming this file
    # renames the symbol and breaks the link -- which is the intended
    # behaviour, not a trap to work around.
    #
    # This is a self-signed ROOT (CN=bladerunner-homelab-system, CA:TRUE,
    # notAfter 2035-07-18), which is the only safe thing to pin: a leaf or
    # an intermediate rotates and takes the whole fleet's ability to
    # update with it, recoverable only with a serial cable. When this root
    # is eventually rotated, every device must be updated BEFORE the
    # server stops serving a chain to it -- the last OTA under the old CA
    # is the one that has to carry the new one.
    #
    # A certificate is public by construction: this holds no private key
    # and is committed deliberately, so a build from a clean checkout
    # produces an image that can actually reach the endpoint.
    EMBED_TXTFILES
        "certs/ota_ca.pem"
)

# Re-arm the diagnostic IDF deliberately disarms.
#
# IDF passes "-Wall -Werror -Wno-error=unused-function" to every TU
# (tools/cmake/build.cmake:183), which downgrades an unused static to a
# warning the build ignores. That matters here because a static with exactly
# one reference is this project's structural backstop against a dropped
# install: task 12 replaced the `.on_locate = run_locate_alarm` static
# initializer -- which could not be forgotten -- with a runtime
# alerts_set_extend_awake() call from app_main, which can be. Drop that call
# and extend_awake_failsafe becomes unused; with the diagnostic disarmed the
# build still exits 0 and the locate alarm silently runs without pushing the
# awake failsafe out.
#
# Scoped to OUR targets rather than set globally: a global re-arm would also
# apply to IDF's own components and the managed dependencies, which carry
# unused statics of their own and would need triage that has nothing to do
# with this project. target_compile_options lands after the global block, and
# GCC is last-flag-wins, so this wins for these sources only.
#
# Two-sided check when touching this: the build must pass as-is, AND deleting
# the alerts_set_extend_awake() install from app_main must make it FAIL. A
# re-arm that only satisfies the first half is not doing anything.
target_compile_options(${COMPONENT_LIB} PRIVATE -Werror=unused-function)

if(SPRITE_DATA_GEN)
    target_sources(${COMPONENT_LIB} PRIVATE "${SPRITE_DATA_GEN}")
endif()
