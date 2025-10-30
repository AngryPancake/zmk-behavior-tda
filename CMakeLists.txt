# SPDX-License-Identifier: MIT
# zephyr_library()
# zephyr_include_directories(${ZEPHYR_BASE}/../zmk/include)
# zephyr_library_sources(behaviors/behavior_tda.c)

if ((NOT CONFIG_ZMK_SPLIT) OR CONFIG_ZMK_SPLIT_ROLE_CENTRAL)
  target_sources_ifdef(CONFIG_ZMK_BEHAVIOR_TDA app PRIVATE behaviors/behavior_tda.c)
endif()