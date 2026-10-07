/*
 * bell_notification.cpp — Deferred ES8388 speaker Bell notification.
 *
 * SPDX-License-Identifier: MIT
 */

#include "bell_notification.h"

#include <stdint.h>
#include <string.h>

#include <esp_err.h>
#include <esp_log.h>
#include <freertos/FreeRTOS.h>
#include <freertos/queue.h>
#include <freertos/task.h>
#include <driver/i2c_master.h>
#include <driver/i2s_std.h>

#include "m5tab5_pinmap.h"

extern "C" {
#include "esp_codec_dev.h"
#include "esp_codec_dev_defaults.h"
#include "es8388_codec.h"
}

namespace {

static const char *TAG = "bell_audio";

// A short, modestly amplified 2 kHz sine-like tone. The wave is generated
// locally so no audio asset occupies flash or SPIFFS space.
static constexpr int BELL_SAMPLE_RATE = 48000;
static constexpr int BELL_DURATION_MS = 80;
static constexpr int BELL_FADE_MS = 5;
static constexpr int BELL_FRAMES = BELL_SAMPLE_RATE * BELL_DURATION_MS / 1000;
static constexpr int BELL_FADE_FRAMES = BELL_SAMPLE_RATE * BELL_FADE_MS / 1000;
static constexpr int BELL_CHANNELS = 2;
static constexpr int BELL_PCM_SAMPLES = BELL_FRAMES * BELL_CHANNELS;
static constexpr TickType_t BELL_MIN_INTERVAL = pdMS_TO_TICKS(250);

// One complete 2 kHz cycle at 48 kHz. Peak amplitude is intentionally below
// full scale because the ES8388 output volume is also set to 30 percent.
static const int16_t BELL_WAVE[] = {
     0,  1553,  3000,  4243,  5196,  5796,
  6000,  5796,  5196,  4243,  3000,  1553,
     0, -1553, -3000, -4243, -5196, -5796,
 -6000, -5796, -5196, -4243, -3000, -1553,
};
static constexpr int BELL_WAVE_SAMPLES = sizeof(BELL_WAVE) / sizeof(BELL_WAVE[0]);

static QueueHandle_t s_bell_queue = nullptr;
static TaskHandle_t s_bell_task = nullptr;
static m5::tab5::m5tab5_component *s_board = nullptr;
static i2s_chan_handle_t s_i2s_tx = nullptr;
static esp_codec_dev_handle_t s_playback = nullptr;
static bool s_audio_ready = false;
static bool s_audio_failed = false;
static int16_t s_bell_pcm[BELL_PCM_SAMPLES] = {};
static bool s_bell_pcm_ready = false;

static void bell_make_pcm(void)
{
    if (s_bell_pcm_ready) return;

    for (int frame = 0; frame < BELL_FRAMES; ++frame) {
        int gain = 1000;
        if (frame < BELL_FADE_FRAMES) {
            gain = frame * 1000 / BELL_FADE_FRAMES;
        } else if (frame >= BELL_FRAMES - BELL_FADE_FRAMES) {
            gain = (BELL_FRAMES - 1 - frame) * 1000 / BELL_FADE_FRAMES;
        }
        if (gain < 0) gain = 0;

        const int16_t sample = (int16_t)((int32_t)BELL_WAVE[frame % BELL_WAVE_SAMPLES] * gain / 1000);
        s_bell_pcm[frame * BELL_CHANNELS] = sample;
        s_bell_pcm[frame * BELL_CHANNELS + 1] = sample;
    }
    s_bell_pcm_ready = true;
}

static void bell_release_failed_audio(void)
{
    if (s_board != nullptr) {
        (void)s_board->speaker_enable(false);
    }
    s_audio_failed = true;
}

static bool bell_audio_prepare(void)
{
    if (s_audio_failed || s_board == nullptr) return false;
    if (s_audio_ready) return true;

    // I2S is isolated from the terminal transport. The I2S channel stays
    // disabled until esp_codec_dev_open() establishes the fixed playback
    // format below.
    i2s_chan_config_t channel_cfg = I2S_CHANNEL_DEFAULT_CONFIG(I2S_NUM_0, I2S_ROLE_MASTER);
    channel_cfg.auto_clear = true;
    esp_err_t err = i2s_new_channel(&channel_cfg, &s_i2s_tx, nullptr);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "I2S TX channel creation failed: %s", esp_err_to_name(err));
        bell_release_failed_audio();
        return false;
    }

    i2s_std_config_t i2s_cfg = {};
    i2s_cfg.clk_cfg = I2S_STD_CLK_DEFAULT_CONFIG(BELL_SAMPLE_RATE);
    i2s_cfg.slot_cfg = I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(
        I2S_DATA_BIT_WIDTH_16BIT, I2S_SLOT_MODE_STEREO);
    i2s_cfg.gpio_cfg.mclk = m5::tab5::M5TAB5_PIN_I2S_MCLK;
    i2s_cfg.gpio_cfg.bclk = m5::tab5::M5TAB5_PIN_I2S_SCLK;
    i2s_cfg.gpio_cfg.ws = m5::tab5::M5TAB5_PIN_I2S_LCLK;
    i2s_cfg.gpio_cfg.dout = m5::tab5::M5TAB5_PIN_I2S_DOUT;
    i2s_cfg.gpio_cfg.din = I2S_GPIO_UNUSED;
    i2s_cfg.gpio_cfg.invert_flags.mclk_inv = false;
    i2s_cfg.gpio_cfg.invert_flags.bclk_inv = false;
    i2s_cfg.gpio_cfg.invert_flags.ws_inv = false;
    err = i2s_channel_init_std_mode(s_i2s_tx, &i2s_cfg);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "I2S standard-mode init failed: %s", esp_err_to_name(err));
        bell_release_failed_audio();
        return false;
    }

    const i2s_chan_handle_t tx_handle = s_i2s_tx;
    audio_codec_i2s_cfg_t codec_i2s_cfg = {};
    codec_i2s_cfg.port = I2S_NUM_0;
    codec_i2s_cfg.tx_handle = tx_handle;
    const audio_codec_data_if_t *data_if = audio_codec_new_i2s_data(&codec_i2s_cfg);
    if (data_if == nullptr) {
        ESP_LOGE(TAG, "ES8388 I2S data interface creation failed");
        bell_release_failed_audio();
        return false;
    }

    i2c_master_bus_handle_t i2c_bus = static_cast<i2c_master_bus_handle_t>(s_board->sys_i2c_master_bus());
    if (i2c_bus == nullptr) {
        ESP_LOGE(TAG, "Tab5 SYS I2C handle unavailable for ES8388");
        bell_release_failed_audio();
        return false;
    }
    audio_codec_i2c_cfg_t codec_i2c_cfg = {};
    codec_i2c_cfg.port = m5::tab5::M5TAB5_I2C_PORT_SYS;
    codec_i2c_cfg.addr = ES8388_CODEC_DEFAULT_ADDR;
    codec_i2c_cfg.bus_handle = i2c_bus;
    const audio_codec_ctrl_if_t *ctrl_if = audio_codec_new_i2c_ctrl(&codec_i2c_cfg);
    if (ctrl_if == nullptr) {
        ESP_LOGE(TAG, "ES8388 I2C control interface creation failed");
        bell_release_failed_audio();
        return false;
    }

    esp_codec_dev_hw_gain_t gain = {};
    gain.pa_voltage = 5.0f;
    gain.codec_dac_voltage = 3.3f;
    es8388_codec_cfg_t es8388_cfg = {};
    es8388_cfg.ctrl_if = ctrl_if;
    es8388_cfg.gpio_if = audio_codec_new_gpio();
    es8388_cfg.codec_mode = ESP_CODEC_DEV_WORK_MODE_DAC;
    // SPK_EN is an I/O-expander bit, not a direct GPIO. speaker_enable()
    // controls it around every beep, so the codec driver must not attempt
    // to operate a nonexistent GPIO itself.
    es8388_cfg.pa_pin = -1;
    es8388_cfg.pa_reverted = false;
    es8388_cfg.master_mode = false;
    es8388_cfg.hw_gain = gain;
    const audio_codec_if_t *codec_if = es8388_codec_new(&es8388_cfg);
    if (codec_if == nullptr) {
        ESP_LOGE(TAG, "ES8388 codec interface creation failed");
        bell_release_failed_audio();
        return false;
    }

    esp_codec_dev_cfg_t playback_cfg = {};
    playback_cfg.dev_type = ESP_CODEC_DEV_TYPE_OUT;
    playback_cfg.codec_if = codec_if;
    playback_cfg.data_if = data_if;
    s_playback = esp_codec_dev_new(&playback_cfg);
    if (s_playback == nullptr) {
        ESP_LOGE(TAG, "ES8388 playback device creation failed");
        bell_release_failed_audio();
        return false;
    }

    esp_codec_dev_sample_info_t sample_info = {};
    sample_info.sample_rate = BELL_SAMPLE_RATE;
    sample_info.channel = BELL_CHANNELS;
    sample_info.bits_per_sample = 16;
    if (esp_codec_dev_open(s_playback, &sample_info) != ESP_CODEC_DEV_OK ||
        esp_codec_dev_set_out_vol(s_playback, 30) != ESP_CODEC_DEV_OK ||
        esp_codec_dev_set_out_mute(s_playback, true) != ESP_CODEC_DEV_OK) {
        ESP_LOGE(TAG, "ES8388 playback open or configuration failed");
        bell_release_failed_audio();
        return false;
    }

    bell_make_pcm();
    s_audio_ready = true;
    ESP_LOGI(TAG, "ES8388 Bell audio ready: %d Hz, %d ms", BELL_SAMPLE_RATE, BELL_DURATION_MS);
    return true;
}

static void bell_audio_play_once(void)
{
    if (!bell_audio_prepare()) return;

    if (s_board->speaker_enable(true) != ESP_OK) {
        ESP_LOGW(TAG, "Could not enable Tab5 speaker amplifier");
        s_audio_failed = true;
        return;
    }

    // Let the amplifier settle before unmuting; this removes the startup click
    // on the short notification waveform.
    vTaskDelay(pdMS_TO_TICKS(2));
    if (esp_codec_dev_set_out_mute(s_playback, false) != ESP_CODEC_DEV_OK ||
        esp_codec_dev_write(s_playback, s_bell_pcm, sizeof(s_bell_pcm)) != ESP_CODEC_DEV_OK) {
        ESP_LOGW(TAG, "ES8388 Bell PCM write failed");
        s_audio_failed = true;
    }

    // I2S writes may finish when DMA has accepted the final block. Keep the
    // amplifier alive through the whole waveform, then mute before power-off.
    vTaskDelay(pdMS_TO_TICKS(BELL_DURATION_MS + 12));
    (void)esp_codec_dev_set_out_mute(s_playback, true);
    (void)s_board->speaker_enable(false);
}

static void bell_audio_task(void *arg)
{
    (void)arg;
    uint8_t request = 0;
    TickType_t last_beep = 0;

    while (true) {
        if (xQueueReceive(s_bell_queue, &request, portMAX_DELAY) != pdTRUE) continue;

        const TickType_t now = xTaskGetTickCount();
        if (last_beep != 0 && (TickType_t)(now - last_beep) < BELL_MIN_INTERVAL) {
            vTaskDelay(BELL_MIN_INTERVAL - (now - last_beep));
        }
        bell_audio_play_once();
        last_beep = xTaskGetTickCount();
    }
}

}  // namespace

bool bell_notification_init(m5::tab5::m5tab5_component &board)
{
    if (s_bell_task != nullptr) return true;

    s_board = &board;
    s_bell_queue = xQueueCreate(1, sizeof(uint8_t));
    if (s_bell_queue == nullptr) {
        ESP_LOGE(TAG, "Bell audio queue allocation failed");
        s_board = nullptr;
        return false;
    }

    BaseType_t task_ok = xTaskCreate(bell_audio_task, "bell_audio", 6144,
                                     nullptr, 4, &s_bell_task);
    if (task_ok != pdPASS) {
        ESP_LOGE(TAG, "Bell audio task creation failed");
        vQueueDelete(s_bell_queue);
        s_bell_queue = nullptr;
        s_board = nullptr;
        return false;
    }
    return true;
}

void bell_notification_request_sound(void)
{
    if (s_bell_queue == nullptr) return;

    const uint8_t request = 1;
    // A full queue is intentional: a single outstanding audible notification
    // already represents all BELL bytes received during the current beep.
    (void)xQueueSend(s_bell_queue, &request, 0);
}
