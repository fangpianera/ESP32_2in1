cmake_minimum_required(VERSION 3.16)

# 双程序启动选择器（app_sel / factory 分区）
# 与 EVA_Nav、F1_Dashboard 共用同一张分区表 partitions_dual.csv
include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(EVA_Selector)
