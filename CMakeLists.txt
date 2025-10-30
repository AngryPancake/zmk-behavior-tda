# SPDX-License-Identifier: MIT
zephyr_library()

zephyr_library_sources_ifdef(CONFIG_ZMK_BEHAVIOR_TDA
    behaviors/behavior_tda.c
)
