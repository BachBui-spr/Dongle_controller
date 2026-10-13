# SPDX-License-Identifier: Apache-2.0
#
# radxa-hci-iso: firmware Bluetooth HCI controller cho Nordic nRF52840 Dongle,
# giao tiep voi Radxa Dragon Q6A qua USB CDC-ACM (giao thuc H4), bat day du ISO (LE Audio).
# src/main.c lay nguyen ban tu zephyr/samples/bluetooth/hci_uart (Zephyr v4.4.2, Apache-2.0).

cmake_minimum_required(VERSION 3.20.0)

find_package(Zephyr REQUIRED HINTS $ENV{ZEPHYR_BASE})
project(radxa_hci_iso)

target_sources(app PRIVATE src/main.c)
