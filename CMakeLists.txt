# SPDX-License-Identifier: MIT
zephyr_library()
zephyr_include_directories(${ZEPHYR_BASE}/../zmk/include)
zephyr_library_sources(behaviors/behavior_tda.c)
