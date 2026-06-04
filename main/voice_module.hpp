#pragma once

#include <cstdint>

#include "esp_err.h"

/**
 * @brief 亚博/CI1302 语音模块状态快照。
 */
struct VoiceModuleStatus {
    bool enabled;             ///< 编译配置是否启用语音模块。
    bool ready;               ///< UART 驱动和接收任务是否已启动。
    uint16_t last_code;       ///< 最近一次收到的 16 位命令码。
    uint32_t frame_count;     ///< 已收到的合法协议帧数量。
    uint32_t bad_frame_count; ///< 尾字节错误等异常帧数量。
    int64_t last_frame_ms;    ///< 最近一次合法帧时间，单位 ms。
};

/**
 * @brief 语音模块收到命令时的回调函数。
 *
 * @param code 16 位命令码，来自协议帧 AA 55 XX YY FB 中的 XXYY。
 * @param user_ctx 用户上下文指针。
 */
using VoiceModuleCallback = void (*)(uint16_t code, void *user_ctx);

/**
 * @brief 初始化 CI1302 UART 语音模块。
 *
 * 协议固定为 115200 8N1，帧格式 AA 55 XX YY FB。
 *
 * @param callback 收到合法帧后的回调，可为空。
 * @param user_ctx 回调上下文。
 * @return ESP_OK 表示初始化成功；未启用时返回 ESP_ERR_NOT_SUPPORTED。
 */
esp_err_t voice_module_init(VoiceModuleCallback callback, void *user_ctx);

/**
 * @brief 获取语音模块当前状态。
 *
 * @return 状态快照。
 */
VoiceModuleStatus voice_module_get_status();

/**
 * @brief 主动向语音模块发送一个 16 位命令码。
 *
 * @param code 16 位命令码。
 * @return ESP_OK 表示已写入 UART。
 */
esp_err_t voice_module_send_code(uint16_t code);
