/*
 * Tap Dance Advanced (TDA) for ZMK
 * Based on original tap_dance.c by ZMK Contributors
 * Modified to trigger the first action immediately.
 *
 * SPDX-License-Identifier: MIT
 */

#define DT_DRV_COMPAT zmk_behavior_tda

#include <zephyr/device.h>
#include <drivers/behavior.h>
#include <zephyr/logging/log.h>
#include <zmk/behavior.h>
#include <zmk/keymap.h>
#include <zmk/matrix.h>
#include <zmk/event_manager.h>
#include <zmk/events/position_state_changed.h>
#include <zmk/events/keycode_state_changed.h>
#include <zmk/hid.h>

LOG_MODULE_DECLARE(zmk, CONFIG_ZMK_LOG_LEVEL);

#if DT_HAS_COMPAT_STATUS_OKAY(DT_DRV_COMPAT)

#define ZMK_BHV_TDA_MAX_HELD CONFIG_ZMK_BEHAVIOR_TDA_MAX_HELD
#define ZMK_BHV_TDA_POSITION_FREE UINT32_MAX

struct behavior_tda_config {
    uint32_t tapping_term_ms;
    size_t behavior_count;
    struct zmk_behavior_binding *behaviors;
};

struct active_tda {
    int counter;
    uint32_t position;
#if IS_ENABLED(CONFIG_ZMK_SPLIT)
    uint8_t source;
#endif
    bool is_pressed;
    bool timer_cancelled;
    bool tap_dance_decided;
    int64_t release_at;
    const struct behavior_tda_config *config;
    struct k_work_delayable release_timer;
};

static struct active_tda active_tdas[ZMK_BHV_TDA_MAX_HELD];

static void clear_tda(struct active_tda *tda) {
    tda->position = ZMK_BHV_TDA_POSITION_FREE;
    tda->counter = 0;
    tda->is_pressed = false;
    tda->timer_cancelled = false;
    tda->tap_dance_decided = false;
}

static struct active_tda *find_tda(uint32_t position) {
    for (int i = 0; i < ZMK_BHV_TDA_MAX_HELD; i++) {
        if (active_tdas[i].position == position && !active_tdas[i].timer_cancelled)
            return &active_tdas[i];
    }
    return NULL;
}

static int new_tda(struct zmk_behavior_binding_event *event,
                   const struct behavior_tda_config *config,
                   struct active_tda **tda) {
    for (int i = 0; i < ZMK_BHV_TDA_MAX_HELD; i++) {
        struct active_tda *slot = &active_tdas[i];
        if (slot->position == ZMK_BHV_TDA_POSITION_FREE) {
            slot->position = event->position;
#if IS_ENABLED(CONFIG_ZMK_SPLIT)
            slot->source = event->source;
#endif
            slot->config = config;
            slot->release_at = 0;
            slot->is_pressed = true;
            slot->timer_cancelled = false;
            slot->tap_dance_decided = false;
            slot->counter = 0;
            *tda = slot;
            return 0;
        }
    }
    return -ENOMEM;
}

static void reset_timer(struct active_tda *tda, struct zmk_behavior_binding_event event) {
    tda->release_at = event.timestamp + tda->config->tapping_term_ms;
    int32_t ms_left = tda->release_at - k_uptime_get();
    if (ms_left > 0) {
        k_work_schedule(&tda->release_timer, K_MSEC(ms_left));
    }
}

static inline int press_tda_behavior(struct active_tda *tda, int64_t timestamp) {
    tda->tap_dance_decided = true;
    struct zmk_behavior_binding binding = tda->config->behaviors[tda->counter - 1];
    struct zmk_behavior_binding_event event = {
        .position = tda->position,
        .timestamp = timestamp,
#if IS_ENABLED(CONFIG_ZMK_SPLIT)
        .source = tda->source,
#endif
    };
    return zmk_behavior_invoke_binding(&binding, event, true);
}

static inline int release_tda_behavior(struct active_tda *tda, int64_t timestamp) {
    struct zmk_behavior_binding binding = tda->config->behaviors[tda->counter - 1];
    struct zmk_behavior_binding_event event = {
        .position = tda->position,
        .timestamp = timestamp,
#if IS_ENABLED(CONFIG_ZMK_SPLIT)
        .source = tda->source,
#endif
    };
    return zmk_behavior_invoke_binding(&binding, event, false);
}

static void tda_timer_handler(struct k_work *item) {
    struct k_work_delayable *d_work = k_work_delayable_from_work(item);
    struct active_tda *tda = CONTAINER_OF(d_work, struct active_tda, release_timer);

    if (tda->position == ZMK_BHV_TDA_POSITION_FREE || tda->timer_cancelled)
        return;

    // Таймер истёк: просто завершаем "танец"
    if (!tda->is_pressed) {
        release_tda_behavior(tda, tda->release_at);
        clear_tda(tda);
    }
}

static int on_tda_pressed(struct zmk_behavior_binding *binding,
                          struct zmk_behavior_binding_event event) {
    const struct device *dev = zmk_behavior_get_binding(binding->behavior_dev);
    const struct behavior_tda_config *cfg = dev->config;
    struct active_tda *tda = find_tda(event.position);

    if (tda == NULL) {
        if (new_tda(&event, cfg, &tda) != 0)
            return ZMK_BEHAVIOR_OPAQUE;
    }

    tda->is_pressed = true;
    tda->counter++;

    if (tda->counter > cfg->behavior_count)
        tda->counter = cfg->behavior_count;

    if (tda->counter > 1) {
        // Отпустить предыдущее поведение и запустить следующее
        release_tda_behavior(tda, event.timestamp);
    }

    press_tda_behavior(tda, event.timestamp);
    reset_timer(tda, event);
    return ZMK_BEHAVIOR_OPAQUE;
}

static int on_tda_released(struct zmk_behavior_binding *binding,
                           struct zmk_behavior_binding_event event) {
    struct active_tda *tda = find_tda(event.position);
    if (!tda)
        return ZMK_BEHAVIOR_OPAQUE;

    tda->is_pressed = false;
    release_tda_behavior(tda, event.timestamp);
    clear_tda(tda);
    return ZMK_BEHAVIOR_OPAQUE;
}

static const struct behavior_driver_api behavior_tda_driver_api = {
    .binding_pressed = on_tda_pressed,
    .binding_released = on_tda_released,
};

static int behavior_tda_init(const struct device *dev) {
    static bool first_run = true;
    if (first_run) {
        for (int i = 0; i < ZMK_BHV_TDA_MAX_HELD; i++) {
            k_work_init_delayable(&active_tdas[i].release_timer, tda_timer_handler);
            clear_tda(&active_tdas[i]);
        }
        first_run = false;
    }
    return 0;
}

#define _TRANSFORM_ENTRY(idx, node) ZMK_KEYMAP_EXTRACT_BINDING(idx, node)
#define TRANSFORMED_BINDINGS(node) \
    { LISTIFY(DT_INST_PROP_LEN(node, bindings), _TRANSFORM_ENTRY, (, ), DT_DRV_INST(node)) }

#define KP_INST(n) \
    static struct zmk_behavior_binding behavior_tda_config_##n##_bindings[DT_INST_PROP_LEN(n, bindings)] = \
        TRANSFORMED_BINDINGS(n); \
    static struct behavior_tda_config behavior_tda_config_##n = { \
        .tapping_term_ms = DT_INST_PROP(n, tapping_term_ms), \
        .behaviors = behavior_tda_config_##n##_bindings, \
        .behavior_count = DT_INST_PROP_LEN(n, bindings) }; \
    BEHAVIOR_DT_INST_DEFINE(n, behavior_tda_init, NULL, NULL, \
                            &behavior_tda_config_##n, POST_KERNEL, \
                            CONFIG_KERNEL_INIT_PRIORITY_DEFAULT, &behavior_tda_driver_api);

DT_INST_FOREACH_STATUS_OKAY(KP_INST)

#endif
