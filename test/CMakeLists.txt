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
add_host_test(test_bedtime)
add_host_test(test_sleep_plan)
add_host_test(test_button_latch)
add_host_test(test_wake_policy)
add_host_test(test_net_apply)
add_host_test(test_app_state)
add_host_test(test_stats_json)
add_host_test(test_mqtt_topics)
add_host_test(test_mqtt_rx)
add_host_test(test_config_validate)
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
