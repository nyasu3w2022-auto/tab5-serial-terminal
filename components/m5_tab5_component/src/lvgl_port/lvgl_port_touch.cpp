/*
 * SPDX-FileCopyrightText: 2024-2025
 * SPDX-License-Identifier: Apache-2.0
 *
 * LVGL Touch Port for ESP-IDF
 */

#include <string.h>
#include "esp_log.h"
#include "esp_err.h"
#include "esp_lcd_touch.h"

#include "lvgl_port.h"
#include "lvgl_port_touch.h"

static const char *TAG = "lvgl_touch";

/*******************************************************************************
 * Types definitions
 ******************************************************************************/
typedef struct {
    esp_lcd_touch_handle_t handle;
    lv_display_t *disp;
    lv_indev_t *indev;
    lv_display_rotation_t rotation;  // Independent rotation setting
    bool use_hw_rotation;            // Use independent rotation instead of display rotation
    struct {
        float x;
        float y;
    } scale;
} lvgl_touch_ctx_t;

/*******************************************************************************
 * Touch read callback
 ******************************************************************************/
static void touch_read_callback(lv_indev_t *indev, lv_indev_data_t *data)
{
    lvgl_touch_ctx_t *ctx = (lvgl_touch_ctx_t *)lv_indev_get_driver_data(indev);
    assert(ctx != NULL);
    assert(ctx->handle != NULL);

    uint16_t touch_x[1]        = {0};
    uint16_t touch_y[1]        = {0};
    uint16_t touch_strength[1] = {0};
    uint8_t touch_cnt           = 0;

    // Read touch data
    esp_lcd_touch_read_data(ctx->handle);

    // Get touch coordinates (first point only)
    bool touched = esp_lcd_touch_get_coordinates(ctx->handle, touch_x, touch_y, touch_strength, &touch_cnt, 1);

    if (touched && touch_cnt > 0) {
        // Get rotation to use
        lv_display_rotation_t rotation = LV_DISPLAY_ROTATION_0;
        int32_t hres = 0, vres = 0;

        if (ctx->use_hw_rotation) {
            rotation = ctx->rotation;
            if (ctx->disp) {
                hres = lv_display_get_horizontal_resolution(ctx->disp);
                vres = lv_display_get_vertical_resolution(ctx->disp);
            }
        } else if (ctx->disp) {
            rotation = lv_display_get_rotation(ctx->disp);
            hres     = lv_display_get_horizontal_resolution(ctx->disp);
            vres     = lv_display_get_vertical_resolution(ctx->disp);
        }

        // Apply scale factors
        int32_t x = (int32_t)(touch_x[0] * ctx->scale.x);
        int32_t y = (int32_t)(touch_y[0] * ctx->scale.y);

        // Transform coordinates based on rotation
        if (rotation != LV_DISPLAY_ROTATION_0 && hres > 0 && vres > 0) {
            int32_t tmp;
            int32_t phys_w = vres;
            int32_t phys_h = hres;

            switch (rotation) {
                case LV_DISPLAY_ROTATION_90:
                    tmp = x;
                    x   = phys_h - 1 - y;
                    y   = tmp;
                    break;
                case LV_DISPLAY_ROTATION_180:
                    x = hres - x - 1;
                    y = vres - y - 1;
                    break;
                case LV_DISPLAY_ROTATION_270:
                    tmp = x;
                    x   = y;
                    y   = phys_w - 1 - tmp;
                    break;
                default:
                    break;
            }
        }

        data->point.x = x;
        data->point.y = y;
        data->state   = LV_INDEV_STATE_PRESSED;

        ESP_LOGD(TAG, "Touch: raw=(%u,%u) -> pt=(%ld,%ld)", touch_x[0], touch_y[0], x, y);
    } else {
        data->state = LV_INDEV_STATE_RELEASED;
    }
}

/*******************************************************************************
 * Public API
 ******************************************************************************/
lv_indev_t *lvgl_port_add_touch(const lvgl_touch_cfg_t *touch_cfg)
{
    if (!touch_cfg || !touch_cfg->handle) {
        ESP_LOGE(TAG, "Invalid touch config");
        return NULL;
    }

    lvgl_port_lock(0);

    // Allocate touch context
    lvgl_touch_ctx_t *ctx = (lvgl_touch_ctx_t *)calloc(1, sizeof(lvgl_touch_ctx_t));
    if (!ctx) {
        ESP_LOGE(TAG, "Alloc touch context failed");
        lvgl_port_unlock();
        return NULL;
    }

    ctx->handle          = touch_cfg->handle;
    ctx->disp            = touch_cfg->disp;
    ctx->scale.x         = (touch_cfg->scale.x > 0) ? touch_cfg->scale.x : 1.0f;
    ctx->scale.y         = (touch_cfg->scale.y > 0) ? touch_cfg->scale.y : 1.0f;
    ctx->rotation        = LV_DISPLAY_ROTATION_0;
    ctx->use_hw_rotation = false;

    // Create LVGL input device
    lv_indev_t *indev = lv_indev_create();
    if (!indev) {
        ESP_LOGE(TAG, "Create indev failed");
        free(ctx);
        lvgl_port_unlock();
        return NULL;
    }

    lv_indev_set_type(indev, LV_INDEV_TYPE_POINTER);
    lv_indev_set_read_cb(indev, touch_read_callback);
    lv_indev_set_driver_data(indev, ctx);

    // Associate with display if provided
    if (ctx->disp) {
        lv_indev_set_display(indev, ctx->disp);
    }

    ctx->indev = indev;

    ESP_LOGI(TAG, "Touch input device created (scale: %.2f x %.2f)", ctx->scale.x, ctx->scale.y);

    lvgl_port_unlock();
    return indev;
}

esp_err_t lvgl_port_remove_touch(lv_indev_t *touch)
{
    if (!touch) {
        return ESP_ERR_INVALID_ARG;
    }

    lvgl_touch_ctx_t *ctx = (lvgl_touch_ctx_t *)lv_indev_get_driver_data(touch);

    lvgl_port_lock(0);
    lv_indev_delete(touch);
    lvgl_port_unlock();

    if (ctx) {
        free(ctx);
    }

    ESP_LOGI(TAG, "Touch input device removed");
    return ESP_OK;
}

esp_err_t lvgl_port_set_touch_rotation(lv_indev_t *touch, lv_display_rotation_t rotation)
{
    if (!touch) {
        return ESP_ERR_INVALID_ARG;
    }

    lvgl_touch_ctx_t *ctx = (lvgl_touch_ctx_t *)lv_indev_get_driver_data(touch);
    if (!ctx) {
        return ESP_ERR_INVALID_STATE;
    }

    lvgl_port_lock(0);
    ctx->rotation        = rotation;
    ctx->use_hw_rotation = true;  // Enable independent rotation
    lvgl_port_unlock();

    ESP_LOGI(TAG, "Touch rotation set to %d", (int)rotation);
    return ESP_OK;
}

lv_display_rotation_t lvgl_port_get_touch_rotation(lv_indev_t *touch)
{
    if (!touch) {
        return LV_DISPLAY_ROTATION_0;
    }

    lvgl_touch_ctx_t *ctx = (lvgl_touch_ctx_t *)lv_indev_get_driver_data(touch);
    if (!ctx) {
        return LV_DISPLAY_ROTATION_0;
    }

    return ctx->rotation;
}
