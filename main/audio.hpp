#pragma once

#include <cstddef>
#include <cstdint>

#include "esp_err.h"

/**
 * @brief 初始化 I2S 扬声器输出。
 *
 * 默认面向 MAX98357A 一类 I2S 数字功放，音频格式固定为
 * 16 kHz / 16-bit / mono PCM，便于电脑端 TTS 桥接器生成数据。
 *
 * @return ESP_OK 表示初始化成功；未启用音频时也返回 ESP_OK。
 */
esp_err_t audio_init();

/**
 * @brief 播放一段 16-bit little-endian mono PCM 数据。
 *
 * @param data PCM 字节缓冲区。
 * @param size 缓冲区字节数，应为偶数。
 * @return ESP_OK 表示已写入 I2S；未启用音频时返回 ESP_ERR_INVALID_STATE。
 */
esp_err_t audio_play_pcm16(const uint8_t *data, size_t size);

/**
 * @brief 播放一段短测试音，用于确认 I2S 功放和喇叭接线。
 *
 * @return ESP_OK 表示播放请求已完成。
 */
esp_err_t audio_play_test_tone();

/**
 * @brief 查询音频输出是否已启用并初始化。
 *
 * @return true 表示可以播放音频。
 */
bool audio_is_ready();
