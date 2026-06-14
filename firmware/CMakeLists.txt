cmake_minimum_required(VERSION 3.16)

set(PROJECT_VER "1.0.1")
set(PROJECT_VER_NUMBER 1)

include($ENV{IDF_PATH}/tools/cmake/project.cmake)

idf_build_set_property(CXX_COMPILE_OPTIONS "-std=gnu++17;-Os;-DCHIP_HAVE_CONFIG_H;-Wno-overloaded-virtual" APPEND)
idf_build_set_property(C_COMPILE_OPTIONS "-Os" APPEND)
idf_build_set_property(COMPILE_OPTIONS "-Wno-format-nonliteral;-Wno-format-security" APPEND)

project(t1n_smart_lock)