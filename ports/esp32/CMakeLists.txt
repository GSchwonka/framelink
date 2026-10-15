# ESP-IDF project for the framelink ESP32 demos and measurements.
# The application is chosen in menuconfig, or with one of the
# sdkconfig.<app> files: see README.md.
cmake_minimum_required(VERSION 3.16)
include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(framelink_esp32)
