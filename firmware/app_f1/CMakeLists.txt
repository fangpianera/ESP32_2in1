# The following lines of boilerplate have to be in your project's
# CMakeLists in this exact order for cmake to work correctly
cmake_minimum_required(VERSION 3.5)

include($ENV{IDF_PATH}/tools/cmake/project.cmake)

# -Wno-format: LVGL 的 printf 风格宏会产生大量格式串告警
# -Wno-error=old-style-declaration: IDF 5.5.6 的 esp_eap_client.c 里
#   `static void inline wpa2_set_state(...)` 在较新 GCC 下触发该告警，IDF 默认
#   -Werror 会把它升级成编译错误（与本项目代码无关，只能在这里降级）
add_compile_options(-Wno-format -Wno-error=old-style-declaration)
project(lvgl_demo_v9)
