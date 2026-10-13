# SPDX-License-Identifier: Apache-2.0
#
# radxa-hci-iso, bien the "baseline" (xem prj.conf): cung src/main.c voi ban "iso" (nguyen ban hci_uart, Zephyr v4.4.2).

cmake_minimum_required(VERSION 3.20.0)

find_package(Zephyr REQUIRED HINTS $ENV{ZEPHYR_BASE})
project(radxa_hci_baseline)

target_sources(app PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/../firmware/src/main.c)
