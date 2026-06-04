#include "ble_bridge.hpp"

#include "sdkconfig.h"

#if CONFIG_BT_ENABLED && CONFIG_BT_NIMBLE_ENABLED

#include <algorithm>
#include <atomic>
#include <cctype>
#include <cstring>
#include <string>

#include "esp_check.h"
#include "esp_log.h"
#include "host/ble_gap.h"
#include "host/ble_hs.h"
#include "host/ble_hs_id.h"
#include "host/ble_hs_mbuf.h"
#include "host/ble_store.h"
#include "host/ble_uuid.h"
#include "host/util/util.h"
#include "nimble/nimble_port.h"
#include "nimble/nimble_port_freertos.h"
#include "services/gap/ble_svc_gap.h"
#include "services/gatt/ble_svc_gatt.h"

extern "C" void ble_store_config_init(void);

namespace {

constexpr const char *kTag = "mochi_ble";
constexpr const char *kDeviceName = "Clawd Mochi";
constexpr size_t kMaxPayloadLen = 192;

/**
 * @brief UUID 字节序按 BLE little-endian 填写。
 *
 * Service UUID：6d6f6368-692d-7065-742d-627269646765
 * Write UUID：6d6f6368-692d-7065-742d-737461747573
 */
const ble_uuid128_t kServiceUuid = BLE_UUID128_INIT(
    0x65, 0x67, 0x64, 0x69, 0x72, 0x62, 0x2d, 0x74,
    0x65, 0x70, 0x2d, 0x69, 0x68, 0x63, 0x6f, 0x6d);
const ble_uuid128_t kWriteUuid = BLE_UUID128_INIT(
    0x73, 0x75, 0x74, 0x61, 0x74, 0x73, 0x2d, 0x74,
    0x65, 0x70, 0x2d, 0x69, 0x68, 0x63, 0x6f, 0x6d);

uint8_t g_own_addr_type = 0;
std::atomic_bool g_ready{false};
BlePetEventCallback g_callback = nullptr;
void *g_user_ctx = nullptr;
ble_npl_event g_adv_event;

void start_advertising();

/**
 * @brief 在 NimBLE 事件队列里异步恢复广播。
 *
 * GAP 回调里直接 stop/start 广播在部分时序下不可靠，因此断连或连接失败后
 * 只投递事件，等当前 GAP 事件处理完成后再启动下一轮广播。
 */
void schedule_advertising()
{
    ble_npl_eventq *eventq = nimble_port_get_dflt_eventq();
    if (eventq == nullptr) {
        ESP_LOGW(kTag, "default eventq not ready, skip advertising schedule");
        return;
    }
    if (!ble_npl_event_is_queued(&g_adv_event)) {
        ble_npl_eventq_put(eventq, &g_adv_event);
    }
}

void advertising_event_cb(ble_npl_event *)
{
    start_advertising();
}

/**
 * @brief 将字段值裁剪到固定 char 数组，并保证以 0 结尾。
 */
template <size_t N>
void copy_field(char (&dst)[N], const std::string &value)
{
    static_assert(N > 0);
    const size_t len = std::min(N - 1, value.size());
    std::memcpy(dst, value.data(), len);
    dst[len] = '\0';
}

/**
 * @brief 只保留可显示 ASCII，避免 BLE 侧输入把 LCD 字体打乱。
 */
std::string ascii_clean(std::string text, size_t max_len)
{
    std::string out;
    out.reserve(std::min(text.size(), max_len));
    for (unsigned char ch : text) {
        if (out.size() >= max_len) {
            break;
        }
        if (ch >= 32 && ch <= 126) {
            out.push_back(static_cast<char>(ch));
        } else if (!out.empty() && out.back() != ' ') {
            out.push_back(' ');
        }
    }
    return out;
}

/**
 * @brief 跳过 JSON 字段冒号后的空白并定位字符串起始引号。
 */
bool find_json_string(const std::string &json, const char *key, size_t *pos)
{
    const std::string pattern = std::string("\"") + key + "\"";
    size_t at = json.find(pattern);
    if (at == std::string::npos) {
        return false;
    }
    at = json.find(':', at + pattern.size());
    if (at == std::string::npos) {
        return false;
    }
    ++at;
    while (at < json.size() && std::isspace(static_cast<unsigned char>(json[at]))) {
        ++at;
    }
    if (at >= json.size() || json[at] != '"') {
        return false;
    }
    *pos = at + 1;
    return true;
}

/**
 * @brief 读取一个短 JSON 字符串字段，支持常见转义。
 */
std::string json_string_field(const std::string &json, const char *key, size_t max_len)
{
    size_t pos = 0;
    if (!find_json_string(json, key, &pos)) {
        return {};
    }
    std::string out;
    out.reserve(max_len);
    bool escaped = false;
    for (; pos < json.size() && out.size() < max_len; ++pos) {
        const char ch = json[pos];
        if (escaped) {
            switch (ch) {
            case 'n':
                out.push_back(' ');
                break;
            case 'r':
            case 't':
                out.push_back(' ');
                break;
            case '"':
            case '\\':
            case '/':
                out.push_back(ch);
                break;
            default:
                out.push_back(ch);
                break;
            }
            escaped = false;
            continue;
        }
        if (ch == '\\') {
            escaped = true;
            continue;
        }
        if (ch == '"') {
            break;
        }
        out.push_back(ch);
    }
    return out;
}

/**
 * @brief 解析 BLE 写入载荷，兼容 JSON 和 mood|text 两种格式。
 */
bool parse_pet_payload(const char *payload, size_t len, BlePetEvent *event)
{
    if (payload == nullptr || event == nullptr || len == 0) {
        return false;
    }
    std::string raw(payload, len);
    while (!raw.empty() && (raw.back() == '\0' || raw.back() == '\r' || raw.back() == '\n')) {
        raw.pop_back();
    }

    std::string mood;
    std::string text;
    if (raw.size() >= 2 && raw.front() == '!') {
        switch (raw[1]) {
        case 'H':
            mood = "happy";
            break;
        case 'T':
            mood = "thinking";
            break;
        case 'E':
            mood = "error";
            break;
        case 'S':
            mood = "surprise";
            break;
        case 'P':
            mood = "sleepy";
            break;
        case 'L':
            mood = "love";
            break;
        case 'W':
            mood = "wink";
            break;
        case 'K':
            mood = "look";
            break;
        case 'N':
        default:
            mood = "normal";
            break;
        }
        text = raw.substr(2);
    } else if (!raw.empty() && raw.front() == '{') {
        mood = json_string_field(raw, "mood", sizeof(event->mood) - 1);
        text = json_string_field(raw, "text", sizeof(event->text) - 1);
    } else {
        const size_t split = raw.find('|');
        if (split == std::string::npos) {
            mood = raw;
        } else {
            mood = raw.substr(0, split);
            text = raw.substr(split + 1);
        }
    }

    mood = ascii_clean(mood.empty() ? "normal" : mood, sizeof(event->mood) - 1);
    text = ascii_clean(text, sizeof(event->text) - 1);
    if (mood.empty()) {
        mood = "normal";
    }
    copy_field(event->mood, mood);
    copy_field(event->text, text);
    return true;
}

/**
 * @brief BLE 广播，包含完整设备名和自定义 Service UUID。
 */
void start_advertising()
{
    ble_gap_adv_params adv_params = {};
    ble_hs_adv_fields fields = {};

    fields.flags = BLE_HS_ADV_F_DISC_GEN | BLE_HS_ADV_F_BREDR_UNSUP;
    fields.tx_pwr_lvl_is_present = 1;
    fields.tx_pwr_lvl = BLE_HS_ADV_TX_PWR_LVL_AUTO;
    fields.name = reinterpret_cast<const uint8_t *>(kDeviceName);
    fields.name_len = std::strlen(kDeviceName);
    fields.name_is_complete = 1;

    const int adv_rc = ble_gap_adv_set_fields(&fields);
    if (adv_rc != 0) {
        ESP_LOGW(kTag, "set adv fields failed: rc=%d", adv_rc);
    }

    ble_hs_adv_fields rsp = {};
    rsp.uuids128 = &kServiceUuid;
    rsp.num_uuids128 = 1;
    rsp.uuids128_is_complete = 1;
    const int rsp_rc = ble_gap_adv_rsp_set_fields(&rsp);
    if (rsp_rc != 0) {
        ESP_LOGW(kTag, "set scan rsp failed: rc=%d", rsp_rc);
    }

    adv_params.conn_mode = BLE_GAP_CONN_MODE_UND;
    adv_params.disc_mode = BLE_GAP_DISC_MODE_GEN;
    const int rc = ble_gap_adv_start(g_own_addr_type, nullptr, BLE_HS_FOREVER,
                                     &adv_params, [](ble_gap_event *event, void *) -> int {
                                         switch (event->type) {
                                         case BLE_GAP_EVENT_CONNECT:
                                             ESP_LOGI(kTag, "central %s: status=%d",
                                                      event->connect.status == 0 ? "connected" : "connect failed",
                                                      event->connect.status);
                                             if (event->connect.status != 0) {
                                                 schedule_advertising();
                                             }
                                             return 0;
                                         case BLE_GAP_EVENT_DISCONNECT:
                                             ESP_LOGI(kTag, "central disconnected: reason=%d",
                                                      event->disconnect.reason);
                                             schedule_advertising();
                                             return 0;
                                         case BLE_GAP_EVENT_ADV_COMPLETE:
                                             schedule_advertising();
                                             return 0;
                                         default:
                                             return 0;
                                         }
                                     },
                                     nullptr);
    if (rc != 0) {
        ESP_LOGW(kTag, "start advertising failed: rc=%d", rc);
    }
}

/**
 * @brief 收到 GATT 写入后解析并回调上层。
 */
int gatt_access_cb(uint16_t, uint16_t, ble_gatt_access_ctxt *ctxt, void *)
{
    if (ctxt->op != BLE_GATT_ACCESS_OP_WRITE_CHR) {
        return BLE_ATT_ERR_UNLIKELY;
    }

    char payload[kMaxPayloadLen + 1] = {};
    uint16_t len = 0;
    const int rc = ble_hs_mbuf_to_flat(ctxt->om, payload, kMaxPayloadLen, &len);
    if (rc != 0) {
        ESP_LOGW(kTag, "copy payload failed: rc=%d", rc);
        return BLE_ATT_ERR_INVALID_ATTR_VALUE_LEN;
    }
    payload[len] = '\0';

    BlePetEvent event = {};
    if (!parse_pet_payload(payload, len, &event)) {
        ESP_LOGW(kTag, "bad payload");
        return BLE_ATT_ERR_INVALID_ATTR_VALUE_LEN;
    }

    ESP_LOGI(kTag, "pet event: mood=%s text=%s", event.mood, event.text);
    if (g_callback != nullptr) {
        g_callback(event, g_user_ctx);
    }
    return 0;
}

const ble_gatt_chr_def kGattCharacteristics[] = {
    {
        reinterpret_cast<const ble_uuid_t *>(&kWriteUuid),
        gatt_access_cb,
        nullptr,
        nullptr,
        BLE_GATT_CHR_F_WRITE | BLE_GATT_CHR_F_WRITE_NO_RSP,
        0,
        nullptr,
        nullptr,
    },
    {
        nullptr,
        nullptr,
        nullptr,
        nullptr,
        0,
        0,
        nullptr,
        nullptr,
    },
};

const ble_gatt_svc_def kGattServices[] = {
    {
        BLE_GATT_SVC_TYPE_PRIMARY,
        reinterpret_cast<const ble_uuid_t *>(&kServiceUuid),
        nullptr,
        kGattCharacteristics,
    },
    {
        0,
        nullptr,
        nullptr,
        nullptr,
    },
};

/**
 * @brief NimBLE host 同步后开始广播。
 */
void on_sync()
{
    int rc = ble_hs_util_ensure_addr(0);
    if (rc != 0) {
        ESP_LOGW(kTag, "ensure addr failed: rc=%d", rc);
        return;
    }
    rc = ble_hs_id_infer_auto(0, &g_own_addr_type);
    if (rc != 0) {
        ESP_LOGW(kTag, "infer addr failed: rc=%d", rc);
        return;
    }
    start_advertising();
    g_ready.store(true, std::memory_order_release);
    ESP_LOGI(kTag, "BLE bridge ready: name=%s", kDeviceName);
}

void on_reset(int reason)
{
    g_ready.store(false, std::memory_order_release);
    ESP_LOGW(kTag, "nimble reset: reason=%d", reason);
}

void host_task(void *)
{
    nimble_port_run();
    nimble_port_freertos_deinit();
}

} // namespace

esp_err_t ble_bridge_init(BlePetEventCallback callback, void *user_ctx)
{
    if (g_ready.load(std::memory_order_acquire)) {
        return ESP_OK;
    }
    g_callback = callback;
    g_user_ctx = user_ctx;

    ESP_RETURN_ON_ERROR(nimble_port_init(), kTag, "nimble init failed");
    ble_npl_event_init(&g_adv_event, advertising_event_cb, nullptr);
    ble_hs_cfg.reset_cb = on_reset;
    ble_hs_cfg.sync_cb = on_sync;
    ble_hs_cfg.store_status_cb = ble_store_util_status_rr;

    int rc = ble_svc_gap_device_name_set(kDeviceName);
    ESP_RETURN_ON_FALSE(rc == 0, ESP_FAIL, kTag, "set device name failed: rc=%d", rc);
    ble_svc_gap_init();
    ble_svc_gatt_init();

    rc = ble_gatts_count_cfg(kGattServices);
    ESP_RETURN_ON_FALSE(rc == 0, ESP_FAIL, kTag, "gatt count failed: rc=%d", rc);
    rc = ble_gatts_add_svcs(kGattServices);
    ESP_RETURN_ON_FALSE(rc == 0, ESP_FAIL, kTag, "gatt add failed: rc=%d", rc);

    ble_store_config_init();
    nimble_port_freertos_init(host_task);
    return ESP_OK;
}

bool ble_bridge_is_ready()
{
    return g_ready.load(std::memory_order_acquire);
}

#else

esp_err_t ble_bridge_init(BlePetEventCallback, void *)
{
    return ESP_ERR_NOT_SUPPORTED;
}

bool ble_bridge_is_ready()
{
    return false;
}

#endif
