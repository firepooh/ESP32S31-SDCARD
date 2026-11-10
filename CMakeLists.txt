# ESP32-S31-Korvo-1: microSD (exFAT) over USB 2.0 High-Speed MSC + throughput benchmark
cmake_minimum_required(VERSION 3.22)

include($ENV{IDF_PATH}/tools/cmake/project.cmake)
# Build only the components that main (and its dependencies) needs.
idf_build_set_property(MINIMAL_BUILD ON)
project(s31_sd_msc_bench)
