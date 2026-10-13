# SPDX-License-Identifier: Apache-2.0
#
# radxa-hci-iso, bien the "lean" (xem prj.conf): cung src/main.c voi ban "iso" (nguyen ban hci_uart, Zephyr v4.4.2).

cmake_minimum_required(VERSION 3.20.0)

find_package(Zephyr REQUIRED HINTS $ENV{ZEPHYR_BASE})
project(radxa_hci_lean)

target_sources(app PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/../firmware/src/main.c)
# Dia chi public co dinh: BlueZ 5.72 mo ISO socket voi kieu dia chi LE Public (TEST_REPORT F57)
target_sources(app PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/../firmware/src/public_addr.c)
