cmake_minimum_required(VERSION 3.16)
project(magtag_host_tests C)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
enable_testing()

add_library(unity STATIC unity/unity.c)
target_include_directories(unity PUBLIC unity)

# Each suite is a single-TU test program: it #includes the source under
# test and its mocks directly, so no project sources are compiled here.
function(add_host_test name)
  add_executable(${name} ${name}/${name}.c)
  target_include_directories(${name} PRIVATE
      ${CMAKE_CURRENT_SOURCE_DIR}/mocks
      ${CMAKE_CURRENT_SOURCE_DIR}/../include)
  target_compile_definitions(${name} PRIVATE NATIVE)
  # Sanitizers give the negative-input tests (corrupt snapshots, malformed
  # NVS blobs) real teeth: an out-of-bounds read fails the suite instead of
  # silently returning garbage.
  target_compile_options(${name} PRIVATE
      -include ${CMAKE_CURRENT_SOURCE_DIR}/mocks/esp_compat.h
      -Wall -Wextra
      -fsanitize=address,undefined -fno-omit-frame-pointer)
  target_link_options(${name} PRIVATE -fsanitize=address,undefined)
  target_link_libraries(${name} unity)
  add_test(NAME ${name} COMMAND ${name})
endfunction()

# ---- display golden-render suite: real LVGL compiled for the host ----
file(GLOB_RECURSE LVGL_SRCS ${CMAKE_CURRENT_SOURCE_DIR}/../managed_components/lvgl__lvgl/src/*.c)
add_library(lvgl_host STATIC ${LVGL_SRCS})
target_include_directories(lvgl_host PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/../managed_components/lvgl__lvgl)
target_compile_definitions(lvgl_host PUBLIC
    LV_CONF_PATH="${CMAKE_CURRENT_SOURCE_DIR}/mocks/lv_conf_host.h")

add_executable(test_display_render test_display_render/test_display_render.c)
target_include_directories(test_display_render PRIVATE mocks ../include)
target_compile_definitions(test_display_render PRIVATE NATIVE
    GOLDEN_DIR="${CMAKE_CURRENT_SOURCE_DIR}/test_display_render/golden")
target_compile_options(test_display_render PRIVATE
    -include ${CMAKE_CURRENT_SOURCE_DIR}/mocks/esp_compat.h
    -Wall -Wextra
    -fsanitize=address,undefined -fno-omit-frame-pointer)
target_link_options(test_display_render PRIVATE -fsanitize=address,undefined)
target_link_libraries(test_display_render unity lvgl_host m)
add_test(NAME test_display_render COMMAND test_display_render)

add_host_test(test_timer)
add_host_test(test_schedule)
add_host_test(test_nvs_config)
add_host_test(test_display)
add_host_test(test_ssd1680)
add_host_test(test_battery)
add_host_test(test_quiet_hours)
add_host_test(test_sleep_plan)
add_host_test(test_button_latch)
add_host_test(test_wake_policy)
add_host_test(test_stats_json)
