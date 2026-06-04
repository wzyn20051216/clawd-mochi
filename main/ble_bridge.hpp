#pragma once

#include <stdint.h>

#include "esp_err.h"

/**
 * @brief BLE 桌宠事件。
 *
 * 电脑端桥接器通过 BLE GATT 写入一条短 JSON，例如：
 * {"mood":"thinking","text":"Build"}。
 */
struct BlePetEvent {
    char mood[32]; ///< 表情状态，和 /pet?mood=... 保持一致。
    char text[96]; ///< LCD 底部短文本，固件侧会再次过滤为 ASCII。
};

/**
 * @brief BLE 桌宠事件回调。
 *
 * 回调运行在 NimBLE 主机任务上下文中，调用方必须快速返回。
 */
using BlePetEventCallback = void (*)(const BlePetEvent &event, void *user_ctx);

/**
 * @brief 启动 Clawd Mochi BLE GATT 桌宠桥接服务。
 *
 * 服务名称：Clawd Mochi
 * Service UUID：6d6f6368-692d-7065-742d-627269646765
 * Write UUID：6d6f6368-692d-7065-742d-737461747573
 *
 * @param callback 收到桌宠状态事件后的回调，可为 nullptr。
 * @param user_ctx 透传给 callback 的用户指针。
 * @return ESP_OK 表示 BLE 已启动；其他值表示初始化失败。
 */
esp_err_t ble_bridge_init(BlePetEventCallback callback, void *user_ctx);

/**
 * @brief BLE 桥接是否已经启动。
 *
 * @return true 表示 BLE GATT 服务已初始化。
 */
bool ble_bridge_is_ready();
