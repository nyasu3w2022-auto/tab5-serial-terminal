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
static constexpr int BELL_AMP_SETTLE_MS = 10;
// esp_codec_dev maps 0..100 to approximately -50..0 dB.  A 30 percent
// setting combined with the former low-amplitude waveform was roughly -50 dB
// at the speaker and could not serve as an audible terminal Bell.
static constexpr int BELL_OUTPUT_VOLUME = 70;
static constexpr int BELL_FRAMES = BELL_SAMPLE_RATE * BELL_DURATION_MS / 1000;
static constexpr int BELL_FADE_FRAMES = BELL_SAMPLE_RATE * BELL_FADE_MS / 1000;
// Tab5's official BSP routes the ES8388 speaker through I2S1 in mono mode.
// Keep this identical to the board route rather than merely relying on GPIO
// matrix routing on I2S0.
static constexpr i2s_port_t BELL_I2S_PORT = I2S_NUM_1;
static constexpr int BELL_CHANNELS = 1;
static constexpr int BELL_PCM_SAMPLES = BELL_FRAMES * BELL_CHANNELS;
static constexpr TickType_t BELL_MIN_INTERVAL = pdMS_TO_TICKS(250);

// One complete 2 kHz cycle at 48 kHz. Peak amplitude remains below full scale
// so the raised ES8388 output volume is audible without clipping this tone.
static const int16_t BELL_WAVE[] = {
     0,  3106,  6000,  8486, 10392, 11592,
 12000, 11592, 10392,  8486,  6000,  3106,
     0, -3106, -6000, -8486, -10392, -11592,
-12000, -11592, -10392, -8486, -6000, -3106,
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
    i2s_chan_config_t channel_cfg = I2S_CHANNEL_DEFAULT_CONFIG(BELL_I2S_PORT, I2S_ROLE_MASTER);
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
        I2S_DATA_BIT_WIDTH_16BIT, I2S_SLOT_MODE_MONO);
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

    // esp_codec_dev's I2S adapter deliberately disables the supplied channel
    // before it reconfigures the runtime sample format in esp_codec_dev_open().
    // The channel must therefore have entered ENABLED state first.  This is
    // also the ordering used by the official Tab5 BSP audio initializer.
    err = i2s_channel_enable(s_i2s_tx);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "I2S TX channel enable failed: %s", esp_err_to_name(err));
        bell_release_failed_audio();
        return false;
    }

    const i2s_chan_handle_t tx_handle = s_i2s_tx;
    audio_codec_i2s_cfg_t codec_i2s_cfg = {};
    codec_i2s_cfg.port = BELL_I2S_PORT;
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
        esp_codec_dev_set_out_vol(s_playback, BELL_OUTPUT_VOLUME) != ESP_CODEC_DEV_OK ||
        esp_codec_dev_set_out_mute(s_playback, true) != ESP_CODEC_DEV_OK) {
        ESP_LOGE(TAG, "ES8388 playback open or configuration failed");
        bell_release_failed_audio();
        return false;
    }

    bell_make_pcm();
    s_audio_ready = true;
    ESP_LOGI(TAG, "ES8388 Bell audio ready: I2S%d mono, %d Hz, %d ms, volume=%d",
             (int)BELL_I2S_PORT, BELL_SAMPLE_RATE, BELL_DURATION_MS, BELL_OUTPUT_VOLUME);
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

    // Give the IO-expander-controlled power amplifier a conservative settle
    // time before unmuting. This follows the official BSP's enable-before-
    // codec order while still turning SPK_EN off after each short Bell.
    vTaskDelay(pdMS_TO_TICKS(BELL_AMP_SETTLE_MS));
    const int unmute_result = esp_codec_dev_set_out_mute(s_playback, false);
    const int write_result = (unmute_result == ESP_CODEC_DEV_OK)
                             ? esp_codec_dev_write(s_playback, s_bell_pcm, sizeof(s_bell_pcm))
                             : ESP_CODEC_DEV_WRONG_STATE;
    if (unmute_result != ESP_CODEC_DEV_OK || write_result != ESP_CODEC_DEV_OK) {
        ESP_LOGW(TAG, "ES8388 Bell playback failed: unmute=%d write=%d", unmute_result, write_result);
        s_audio_failed = true;
    } else {
        ESP_LOGI(TAG, "Bell playback started: I2S%d mono, %d bytes", (int)BELL_I2S_PORT,
                 (int)sizeof(s_bell_pcm));
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
