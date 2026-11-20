# Phase 1 bench tool: passive sniffer for the M66003 VFD bus (RX-V540RDS).
# Build with ESP-IDF v5.x or v6.x:  idf.py set-target esp32 && idf.py build flash
cmake_minimum_required(VERSION 3.16)
include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(m66003_sniffer)
