cmake_minimum_required(VERSION 3.16)
project(magtag_host_tests C)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
enable_testing()

add_library(unity STATIC unity/unity.c)
target_include_directories(unity PUBLIC unity)

# Each suite is a single-TU test program: it #includes the source under
# test and its mocks directly, so no project sources are compiled here.
# Optional keyword arguments for suites that need more than the defaults:
#   EXTRA_SRCS      additional sources (e.g. vendored cJSON)
#   EXTRA_INCLUDES  additional include directories
#   EXTRA_DEFS      additional compile definitions
#   EXTRA_LIBS      additional link libraries
function(add_host_test name)
  cmake_parse_arguments(HT "" "" "EXTRA_SRCS;EXTRA_INCLUDES;EXTRA_DEFS;EXTRA_LIBS" ${ARGN})
  add_executable(${name} ${name}/${name}.c ${HT_EXTRA_SRCS})
  target_include_directories(${name} PRIVATE
      ${CMAKE_CURRENT_SOURCE_DIR}/mocks
      ${CMAKE_CURRENT_SOURCE_DIR}/../include
      ${HT_EXTRA_INCLUDES})
  target_compile_definitions(${name} PRIVATE NATIVE ${HT_EXTRA_DEFS})
  # Sanitizers give the negative-input tests (corrupt snapshots, malformed
  # NVS blobs) real teeth: an out-of-bounds read fails the suite instead of
  # silently returning garbage.
  target_compile_options(${name} PRIVATE
      -include ${CMAKE_CURRENT_SOURCE_DIR}/mocks/esp_compat.h
      -Wall -Wextra
      -fsanitize=address,undefined -fno-omit-frame-pointer)
  target_link_options(${name} PRIVATE -fsanitize=address,undefined)
  target_link_libraries(${name} unity ${HT_EXTRA_LIBS})
  add_test(NAME ${name} COMMAND ${name})
endfunction()

# ---- display golden-render suite: real LVGL compiled for the host ----
file(GLOB_RECURSE LVGL_SRCS CONFIGURE_DEPENDS
    ${CMAKE_CURRENT_SOURCE_DIR}/../managed_components/lvgl__lvgl/src/*.c)
add_library(lvgl_host STATIC ${LVGL_SRCS})
target_include_directories(lvgl_host PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/../managed_components/lvgl__lvgl)
target_compile_definitions(lvgl_host PUBLIC
    LV_CONF_PATH="${CMAKE_CURRENT_SOURCE_DIR}/mocks/lv_conf_host.h")

add_host_test(test_display_render
    EXTRA_DEFS GOLDEN_DIR="${CMAKE_CURRENT_SOURCE_DIR}/test_display_render/golden"
    EXTRA_LIBS lvgl_host m)

add_host_test(test_timer)
add_host_test(test_button_actions)
add_host_test(test_schedule)
add_host_test(test_nvs_config)
add_host_test(test_display)
add_host_test(test_ssd1680)
add_host_test(test_battery)
add_host_test(test_quiet_hours)
add_host_test(test_chores)
add_host_test(test_chore_store)
add_host_test(test_bedtime)
add_host_test(test_sleep_plan)
add_host_test(test_lock_gate)
add_host_test(test_buttons_policy)
add_host_test(test_button_latch)
add_host_test(test_wake_policy)
# Deliberately NOT THE SHIPPING FIGURE, which is 400 as of M2-HW-FIX and is
# both the Kconfig default and status_led.h's host fallback. Written that way
# round on purpose: this comment said "NOT 250" at three points and the raise
# to 400 left all three behind, which is the same one-obligation-many-sites
# drift the override itself exists to catch. The 170 below is the only figure
# here that means anything, and it means "neither of the other two".
#
# The ack hold and the chore-ack coalescing window are the same menuconfig
# figure (CONFIG_MAGTAG_STATUS_LED_ACK_HOLD_MS) precisely so a board can sweep
# it, and every case that measures either one is written against
# STATUS_LED_ACK_HOLD_MS rather than a number. Building the suite at a value
# that is not the default is what makes that real: a literal left at either
# call site -- Button B's start or the coalescing window -- compiles fine and
# passes every relative assertion while the two figures have silently diverged.
# With this, it fails. The value itself means nothing beyond "not the default,
# and crosses no second boundary in the mock clock's sub-second carry".
#
# NOTE this override is also why status_led.h's #else fallback reaches no host
# binary: defining the CONFIG symbol here takes the #ifdef arm. That is what
# made the header's old "MUST AGREE" claim vacuous, and why the agreement is
# now held by scripts/check-ack-hold-default.py instead of by a sentence.
add_host_test(test_wake_flow
    EXTRA_DEFS CONFIG_MAGTAG_STATUS_LED_ACK_HOLD_MS=170)
add_host_test(test_status_led)
# alerts.c reads two alarm-length Kconfig ints straight out of sdkconfig.h,
# which the host build has no equivalent of. Supplied here rather than as
# #ifndef fallbacks inside alerts.c, so a renamed or dropped Kconfig symbol
# still breaks the firmware build loudly instead of silently compiling
# against a default. The values below merely happened to match
# Kconfig.projbuild's defaults when written and nothing keeps them in step
# — that is deliberate and harmless, because no test asserts on either
# value (the locate pattern's poll cap is a literal); they exist only to
# make the two pattern initialisers compile.
add_host_test(test_alerts
    EXTRA_DEFS CONFIG_MAGTAG_EXPIRY_ALARM_CYCLES=5 CONFIG_MAGTAG_BREAK_ALARM_CYCLES=3)
add_host_test(test_config_cache)
add_host_test(test_timer_persist)
add_host_test(test_time_util)
add_host_test(test_mock_hal_time)
add_host_test(test_net_apply)
add_host_test(test_app_state)
add_host_test(test_stats_json)
# The panic breadcrumb. Only the PURE half of panic_diag.c compiles under
# NATIVE (the RTC/NVS/FreeRTOS half is behind the fence), which is the
# half that carries every rule: the phase table, the slot map, the label,
# the magic+checksum guard that stops an uninitialised cold-boot record
# being published as evidence, and the record state machine.
add_host_test(test_panic_diag)
add_host_test(test_mqtt_topics)
add_host_test(test_mqtt_rx)
add_host_test(test_config_validate)
# The one decision inside the OTA driver that needs no radio: whether a
# Location header may be followed. It compiles config_validate.c beside
# ota_url.c so the https rule under test is the real one, not a restatement.
add_host_test(test_ota_url)
add_host_test(test_ha_config EXTRA_LIBS m) # m: tones.c sine LUT
add_host_test(test_tones EXTRA_LIBS m)
add_host_test(test_wav_header)

# ---- suites that parse JSON compile the vendored cJSON for the host ----
add_host_test(test_config_apply
    EXTRA_SRCS ${CMAKE_CURRENT_SOURCE_DIR}/../lib/cJSON/cJSON.c
    EXTRA_INCLUDES ${CMAKE_CURRENT_SOURCE_DIR}/../lib/cJSON
    EXTRA_LIBS m)
add_host_test(test_cmd_apply
    EXTRA_SRCS ${CMAKE_CURRENT_SOURCE_DIR}/../lib/cJSON/cJSON.c
    EXTRA_INCLUDES ${CMAKE_CURRENT_SOURCE_DIR}/../lib/cJSON
    EXTRA_LIBS m)
# BUG-8. The ONLY suite that host-compiles main/timer_defs.c, which is what
# lets timer_defs_install() be tested rather than transcribed (the entry's
# repro had to copy it). It carries cJSON because the regression needs the
# real config_apply() on the other side of the boot.
add_host_test(test_timer_defs
    EXTRA_SRCS ${CMAKE_CURRENT_SOURCE_DIR}/../lib/cJSON/cJSON.c
    EXTRA_INCLUDES ${CMAKE_CURRENT_SOURCE_DIR}/../lib/cJSON
    EXTRA_LIBS m)
add_host_test(test_ota_policy
    EXTRA_SRCS ${CMAKE_CURRENT_SOURCE_DIR}/../lib/cJSON/cJSON.c
    EXTRA_INCLUDES ${CMAKE_CURRENT_SOURCE_DIR}/../lib/cJSON
    EXTRA_LIBS m)
# The decisions that were buried in the OTA driver: esp-tls vs socket
# classification, image-vs-transport errors, the fallback-fact rule and
# manifest completeness. It carries ota_policy.c (and therefore cJSON)
# because the fallback rule's guarantee is stated in terms of
# ota_policy_reason, and asserting it against the real classifier rather
# than a restatement is the whole point.
add_host_test(test_ota_facts
    EXTRA_SRCS ${CMAKE_CURRENT_SOURCE_DIR}/../lib/cJSON/cJSON.c
    EXTRA_INCLUDES ${CMAKE_CURRENT_SOURCE_DIR}/../lib/cJSON
    EXTRA_LIBS m)
# The flow suite carries cJSON for the same reason: it runs the REAL
# ota_policy under the sequencing rather than a mock of it, so the
# decisions driving the order are the ones the firmware will make.
add_host_test(test_ota_flow
    EXTRA_SRCS ${CMAKE_CURRENT_SOURCE_DIR}/../lib/cJSON/cJSON.c
    EXTRA_INCLUDES ${CMAKE_CURRENT_SOURCE_DIR}/../lib/cJSON
    EXTRA_LIBS m)

# ---- the one suite that is not C, and why ----
#
# tools/check_slot_size.py is the build-size guard wired into the project
# root CMakeLists.txt. It is a build-system tool, not firmware: add_host_test
# cannot build it and Unity cannot call it. The alternative -- exercising the
# guard only by building the firmware -- can never reach its failure branch,
# because tripping it for real would mean growing the image by tens of KB.
# So it is registered directly with add_test() and driven by unittest, which
# ships with the Python the ESP-IDF toolchain already requires.
#
# It stays in the same ctest sweep as the C suites on purpose: a guard whose
# tests live somewhere separate is a guard whose tests stop being run.
find_package(Python3 REQUIRED COMPONENTS Interpreter)
add_test(NAME test_check_slot_size
         COMMAND ${Python3_EXECUTABLE}
                 ${CMAKE_CURRENT_SOURCE_DIR}/test_check_slot_size/test_check_slot_size.py)

# scripts/check-log-args.py is the HAZ-1 gate: no function call inside an
# ESP_LOGx argument list, because ESP_LOGx wraps its arguments in a
# compile-time level guard and a call left there stops executing once the
# level is compiled out. Same reasoning as the guard above for why it is
# Python and why it is registered here: it is a pre-commit tool, not
# firmware, and a gate whose tests live somewhere separate is a gate whose
# tests stop being run. It shipped with two silent holes that review found
# and no test could have missed -- which is why it has one now.
add_test(NAME test_check_log_args
         COMMAND ${Python3_EXECUTABLE}
                 ${CMAKE_CURRENT_SOURCE_DIR}/test_check_log_args/test_check_log_args.py)

# scripts/check-status-led-wrapper.py is the chore-gate gate: exactly one raw
# status_led_show_timer_state() call in wake_flow.c, because that function
# writes the pixel the chore checklist puts its gate on and the file funnels
# all twelve of its status paints through one wrapper to keep them apart.
# Python and registered here for the same two reasons as the two above. It
# exists because review reverted each of those twelve sites in turn and TEN
# survived the entire host suite -- so the thing this file is really testing
# is a gate that was added after a near miss, which is exactly the kind that
# needs its own tests.
add_test(NAME test_check_status_led_wrapper
         COMMAND ${Python3_EXECUTABLE}
                 ${CMAKE_CURRENT_SOURCE_DIR}/test_check_status_led_wrapper/test_check_status_led_wrapper.py)

# scripts/check-ack-hold-default.py holds STATUS_LED_ACK_HOLD_MS's Kconfig
# default equal to the host fallback in status_led.h's #else arm. Python and
# registered here for the same two reasons as the three above. It exists
# because status_led.h asserted that agreement as a MUST while nothing held
# it -- and review showed the assertion was worse than unenforced: the only
# host suite that expands the macro is test_wake_flow, built with the CONFIG
# symbol defined (see the =170 override above), so the #else literal reaches
# NO host binary and mutating it to 250 or 0 failed nothing. The two gates
# before this one each shipped with a hole review had to find, so this one's
# suite spends most of its cases on the ways it could pass while blind.
add_test(NAME test_check_ack_hold_default
         COMMAND ${Python3_EXECUTABLE}
                 ${CMAKE_CURRENT_SOURCE_DIR}/test_check_ack_hold_default/test_check_ack_hold_default.py)
