idf_component_register(
    SRCS "ssd1680.c" "ssd1680_guard.c"
    INCLUDE_DIRS "include"
    REQUIRES esp_driver_spi esp_driver_gpio
)

# See the note in main/CMakeLists.txt: IDF disarms -Werror=unused-function
# globally, and this project treats a one-reference static as a structural
# backstop. Re-armed per-target so IDF's own components are unaffected.
target_compile_options(${COMPONENT_LIB} PRIVATE -Werror=unused-function)
