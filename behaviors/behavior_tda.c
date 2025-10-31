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

/*
 * Tap Dance Advanced (TDA):
 * - выполняет следующее действие при каждом нажатии (мгновенно)
 * - сбрасывает счётчик, если с последнего нажатия прошло tapping-term-ms
 */

#define ZMK_BHV_TDA_MAX_HELD CONFIG_ZMK_BEHAVIOR_TAP_DANCE_MAX_HELD
#define ZMK_BHV_TDA_POSITION_FREE UINT32_MAX

struct behavior_tda_config {
    uint32_t tapping_term_ms;
    size_t behavior_count;
    struct zmk_behavior_binding *behaviors;
};

struct active_tda {
    uint32_t position;
#if IS_ENABLED(CONFIG_ZMK_SPLIT)
    uint8_t source;
#endif
    int counter;
    bool is_pressed;
    const struct behavior_tda_config *config;
    int64_t last_press_time;
    struct k_work_delayable reset_timer;
    bool timer_active;
};

static struct active_tda active_tdas[ZMK_BHV_TDA_MAX_HELD] = {};

static void clear_tda(struct active_tda *tda) {
    tda->position = ZMK_BHV_TDA_POSITION_FREE;
    tda->is_pressed = false;
    tda->counter = 0;
    tda->timer_active = false;
}

static struct active_tda *find_tda(uint32_t position) {
    for (int i = 0; i < ZMK_BHV_TDA_MAX_HELD; i++) {
        if (active_tdas[i].position == position)
            return &active_tdas[i];
    }
    return NULL;
}

static struct active_tda *new_tda(struct zmk_behavior_binding_event *event,
                                  const struct behavior_tda_config *config) {
    for (int i = 0; i < ZMK_BHV_TDA_MAX_HELD; i++) {
        struct active_tda *tda = &active_tdas[i];
        if (tda->position == ZMK_BHV_TDA_POSITION_FREE) {
            tda->position = event->position;
#if IS_ENABLED(CONFIG_ZMK_SPLIT)
            tda->source = event->source;
#endif
            tda->counter = 0;
            tda->is_pressed = false;
            tda->config = config;
            tda->timer_active = false;
            tda->last_press_time = 0;
            return tda;
        }
    }
    return NULL;
}

static void reset_tda_counter(struct active_tda *tda) {
    LOG_DBG("TDA[%d]: reset counter after timeout", tda->position);
    tda->counter = 0;
    tda->timer_active = false;
}

static void tda_reset_timer_handler(struct k_work *item) {
    struct k_work_delayable *d_work = k_work_delayable_from_work(item);
    struct active_tda *tda = CONTAINER_OF(d_work, struct active_tda, reset_timer);
    if (tda->position == ZMK_BHV_TDA_POSITION_FREE)
        return;
    reset_tda_counter(tda);
}

static void restart_reset_timer(struct active_tda *tda) {
    if (tda->config->tapping_term_ms == 0)
        return;

    if (tda->timer_active) {
        k_work_cancel_delayable(&tda->reset_timer);
    }

    k_work_schedule(&tda->reset_timer, K_MSEC(tda->config->tapping_term_ms));
    tda->timer_active = true;
}

static int on_tda_pressed(struct zmk_behavior_binding *binding,
                          struct zmk_behavior_binding_event event) {
    const struct device *dev = zmk_behavior_get_binding(binding->behavior_dev);
    const struct behavior_tda_config *cfg = dev->config;

    struct active_tda *tda = find_tda(event.position);
    if (!tda) {
        tda = new_tda(&event, cfg);
        if (!tda) {
            LOG_ERR("No free TDA slots");
            return ZMK_BEHAVIOR_OPAQUE;
        }
        k_work_init_delayable(&tda->reset_timer, tda_reset_timer_handler);
    }

    // Если давно не нажимали — сбрасываем
    if (cfg->tapping_term_ms > 0 &&
        (k_uptime_get() - tda->last_press_time > cfg->tapping_term_ms)) {
        tda->counter = 0;
    }

    tda->last_press_time = k_uptime_get();

    if (cfg->behavior_count == 0)
        return ZMK_BEHAVIOR_OPAQUE;

    // Переходим к следующему binding
    tda->counter++;
    if (tda->counter > cfg->behavior_count)
        tda->counter = 1;

    tda->is_pressed = true;
    restart_reset_timer(tda);

    struct zmk_behavior_binding act = cfg->behaviors[tda->counter - 1];
    LOG_DBG("TDA[%d]: pressed, binding %d/%d", event.position, tda->counter, cfg->behavior_count);

    return zmk_behavior_invoke_binding(&act, event, true);
}

static int on_tda_released(struct zmk_behavior_binding *binding,
                           struct zmk_behavior_binding_event event) {
    struct active_tda *tda = find_tda(event.position);
    if (!tda || !tda->is_pressed)
        return ZMK_BEHAVIOR_OPAQUE;

    tda->is_pressed = false;

    const struct behavior_tda_config *cfg = tda->config;
    if (cfg->behavior_count == 0)
        return ZMK_BEHAVIOR_OPAQUE;

    struct zmk_behavior_binding act = cfg->behaviors[tda->counter - 1];
    LOG_DBG("TDA[%d]: released binding %d", event.position, tda->counter);
    return zmk_behavior_invoke_binding(&act, event, false);
}

static const struct behavior_driver_api behavior_tda_driver_api = {
    .binding_pressed = on_tda_pressed,
    .binding_released = on_tda_released,
#if IS_ENABLED(CONFIG_ZMK_BEHAVIOR_METADATA)
    .get_parameter_metadata = zmk_behavior_get_empty_param_metadata,
#endif
};

static int behavior_tda_init(const struct device *dev) {
    static bool init_once = true;
    if (init_once) {
        for (int i = 0; i < ZMK_BHV_TDA_MAX_HELD; i++) {
            clear_tda(&active_tdas[i]);
            k_work_init_delayable(&active_tdas[i].reset_timer, tda_reset_timer_handler);
        }
        init_once = false;
    }
    return 0;
}

#define _TRANSFORM_ENTRY(idx, node) ZMK_KEYMAP_EXTRACT_BINDING(idx, node)
#define TRANSFORMED_BINDINGS(node) \
    { LISTIFY(DT_INST_PROP_LEN(node, bindings), _TRANSFORM_ENTRY, (, ), DT_DRV_INST(node)) }

#define KP_INST(n)                                                                                 \
    static struct zmk_behavior_binding                                                             \
        behavior_tda_config_##n##_bindings[DT_INST_PROP_LEN(n, bindings)] =                        \
            TRANSFORMED_BINDINGS(n);                                                               \
    static struct behavior_tda_config behavior_tda_config_##n = {                                  \
        .tapping_term_ms = DT_INST_PROP_OR(n, tapping_term_ms, 0),                                 \
        .behaviors = behavior_tda_config_##n##_bindings,                                           \
        .behavior_count = DT_INST_PROP_LEN(n, bindings)};                                          \
    BEHAVIOR_DT_INST_DEFINE(n, behavior_tda_init, NULL, NULL, &behavior_tda_config_##n,            \
                            POST_KERNEL, CONFIG_KERNEL_INIT_PRIORITY_DEFAULT,                      \
                            &behavior_tda_driver_api);

DT_INST_FOREACH_STATUS_OKAY(KP_INST)

#endif
