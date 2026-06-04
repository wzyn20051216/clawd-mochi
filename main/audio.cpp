#include "audio.hpp"

#include "esp_log.h"
#include "sdkconfig.h"

#if CONFIG_MOCHI_AUDIO_ENABLE
#include <algorithm>
#include <cmath>
#include <cstdint>

#include "driver/i2s_std.h"
#include "esp_check.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#endif

namespace {

constexpr const char *kTag = "mochi_audio";

#if CONFIG_MOCHI_AUDIO_ENABLE
constexpr uint32_t kSampleRate = 16000;
constexpr float kPi = 3.14159265358979323846f;

i2s_chan_handle_t g_tx = nullptr;
SemaphoreHandle_t g_audio_mutex = nullptr;

int16_t clamp_i16(float value)
{
    const float clipped = std::clamp(value, -32768.0f, 32767.0f);
    return static_cast<int16_t>(clipped);
}
#endif

} // namespace

esp_err_t audio_init()
{
#if CONFIG_MOCHI_AUDIO_ENABLE
    if (g_tx != nullptr) {
        return ESP_OK;
    }

    g_audio_mutex = xSemaphoreCreateMutex();
    ESP_RETURN_ON_FALSE(g_audio_mutex != nullptr, ESP_ERR_NO_MEM, kTag, "audio mutex alloc failed");

    i2s_chan_config_t chan_cfg = I2S_CHANNEL_DEFAULT_CONFIG(I2S_NUM_AUTO, I2S_ROLE_MASTER);
    chan_cfg.dma_desc_num = 6;
    chan_cfg.dma_frame_num = 256;
    ESP_RETURN_ON_ERROR(i2s_new_channel(&chan_cfg, &g_tx, nullptr), kTag, "i2s tx channel failed");

    i2s_std_config_t std_cfg = {
        .clk_cfg = I2S_STD_CLK_DEFAULT_CONFIG(kSampleRate),
        .slot_cfg = I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_16BIT, I2S_SLOT_MODE_MONO),
        .gpio_cfg = {
            .mclk = I2S_GPIO_UNUSED,
            .bclk = static_cast<gpio_num_t>(CONFIG_MOCHI_AUDIO_PIN_BCLK),
            .ws = static_cast<gpio_num_t>(CONFIG_MOCHI_AUDIO_PIN_WS),
            .dout = static_cast<gpio_num_t>(CONFIG_MOCHI_AUDIO_PIN_DOUT),
            .din = I2S_GPIO_UNUSED,
            .invert_flags = {
                .mclk_inv = false,
                .bclk_inv = false,
                .ws_inv = false,
            },
        },
    };
    ESP_RETURN_ON_ERROR(i2s_channel_init_std_mode(g_tx, &std_cfg), kTag, "i2s std init failed");
    ESP_RETURN_ON_ERROR(i2s_channel_enable(g_tx), kTag, "i2s enable failed");
    ESP_LOGI(kTag, "audio ready: 16kHz pcm16 mono, bclk=%d ws=%d dout=%d",
             CONFIG_MOCHI_AUDIO_PIN_BCLK, CONFIG_MOCHI_AUDIO_PIN_WS, CONFIG_MOCHI_AUDIO_PIN_DOUT);
#else
    ESP_LOGI(kTag, "audio disabled");
#endif
    return ESP_OK;
}

bool audio_is_ready()
{
#if CONFIG_MOCHI_AUDIO_ENABLE
    return g_tx != nullptr;
#else
    return false;
#endif
}

esp_err_t audio_play_pcm16(const uint8_t *data, size_t size)
{
#if CONFIG_MOCHI_AUDIO_ENABLE
    ESP_RETURN_ON_FALSE(g_tx != nullptr, ESP_ERR_INVALID_STATE, kTag, "audio not initialized");
    ESP_RETURN_ON_FALSE(data != nullptr, ESP_ERR_INVALID_ARG, kTag, "pcm data is null");
    ESP_RETURN_ON_FALSE((size % sizeof(int16_t)) == 0, ESP_ERR_INVALID_SIZE, kTag, "pcm size must be even");

    if (size == 0) {
        return ESP_OK;
    }

    xSemaphoreTake(g_audio_mutex, portMAX_DELAY);
    size_t offset = 0;
    while (offset < size) {
        size_t written = 0;
        const size_t chunk = std::min<size_t>(size - offset, 2048);
        const esp_err_t err = i2s_channel_write(g_tx, data + offset, chunk, &written, 1000);
        if (err != ESP_OK) {
            xSemaphoreGive(g_audio_mutex);
            return err;
        }
        offset += written;
    }
    xSemaphoreGive(g_audio_mutex);
    return ESP_OK;
#else
    (void)data;
    (void)size;
    return ESP_ERR_INVALID_STATE;
#endif
}

esp_err_t audio_play_test_tone()
{
#if CONFIG_MOCHI_AUDIO_ENABLE
    constexpr size_t kFrames = kSampleRate / 2;
    constexpr size_t kChunkFrames = 256;
    int16_t samples[kChunkFrames] = {};

    ESP_RETURN_ON_FALSE(g_tx != nullptr, ESP_ERR_INVALID_STATE, kTag, "audio not initialized");

    xSemaphoreTake(g_audio_mutex, portMAX_DELAY);
    for (size_t base = 0; base < kFrames; base += kChunkFrames) {
        const size_t frames = std::min(kChunkFrames, kFrames - base);
        for (size_t i = 0; i < frames; ++i) {
            const float t = static_cast<float>(base + i) / static_cast<float>(kSampleRate);
            const float envelope = std::min(1.0f, std::min(t * 20.0f, (0.5f - t) * 20.0f));
            samples[i] = clamp_i16(std::sin(2.0f * kPi * 880.0f * t) * envelope * 9000.0f);
        }
        size_t written = 0;
        const esp_err_t err = i2s_channel_write(g_tx, samples, frames * sizeof(int16_t), &written, 1000);
        if (err != ESP_OK) {
            xSemaphoreGive(g_audio_mutex);
            return err;
        }
    }
    xSemaphoreGive(g_audio_mutex);
    return ESP_OK;
#else
    return ESP_ERR_INVALID_STATE;
#endif
}
