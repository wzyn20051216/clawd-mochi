#include "voice_module.hpp"

#include "driver/uart.h"
#include "esp_check.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "sdkconfig.h"

namespace {

constexpr const char *kTag = "mochi_voice_module";
constexpr uint8_t kFrameHead0 = 0xAA;
constexpr uint8_t kFrameHead1 = 0x55;
constexpr uint8_t kFrameTail = 0xFB;
constexpr size_t kRxBufferSize = 256;
constexpr size_t kRxChunkSize = 64;

VoiceModuleCallback g_callback = nullptr;
void *g_callback_ctx = nullptr;
TaskHandle_t g_task = nullptr;
portMUX_TYPE g_status_lock = portMUX_INITIALIZER_UNLOCKED;
VoiceModuleStatus g_status = {};

#if CONFIG_MOCHI_VOICE_MODULE_ENABLE
constexpr uart_port_t kUartPort = static_cast<uart_port_t>(CONFIG_MOCHI_VOICE_MODULE_UART_PORT);

void set_ready(bool ready)
{
    portENTER_CRITICAL(&g_status_lock);
    g_status.enabled = true;
    g_status.ready = ready;
    portEXIT_CRITICAL(&g_status_lock);
}

void record_bad_frame()
{
    portENTER_CRITICAL(&g_status_lock);
    ++g_status.bad_frame_count;
    portEXIT_CRITICAL(&g_status_lock);
}

void record_frame(uint16_t code)
{
    const int64_t now_ms = esp_timer_get_time() / 1000;
    portENTER_CRITICAL(&g_status_lock);
    g_status.last_code = code;
    ++g_status.frame_count;
    g_status.last_frame_ms = now_ms;
    portEXIT_CRITICAL(&g_status_lock);
}

void handle_stream_byte(uint8_t byte)
{
    static uint8_t state = 0;
    static uint8_t code_hi = 0;
    static uint8_t code_lo = 0;

    switch (state) {
    case 0:
        state = (byte == kFrameHead0) ? 1 : 0;
        break;
    case 1:
        if (byte == kFrameHead1) {
            state = 2;
        } else {
            state = (byte == kFrameHead0) ? 1 : 0;
            record_bad_frame();
        }
        break;
    case 2:
        code_hi = byte;
        state = 3;
        break;
    case 3:
        code_lo = byte;
        state = 4;
        break;
    case 4:
        if (byte == kFrameTail) {
            const uint16_t code = (static_cast<uint16_t>(code_hi) << 8) | code_lo;
            record_frame(code);
            ESP_LOGI(kTag, "voice frame: 0x%04X", code);
            if (g_callback != nullptr) {
                g_callback(code, g_callback_ctx);
            }
        } else {
            record_bad_frame();
            ESP_LOGW(kTag, "bad voice frame tail: 0x%02X", byte);
        }
        state = (byte == kFrameHead0) ? 1 : 0;
        break;
    default:
        state = 0;
        break;
    }
}

void task_voice_module_rx(void *)
{
    uint8_t data[kRxChunkSize] = {};
    set_ready(true);
    while (true) {
        const int len = uart_read_bytes(kUartPort, data, sizeof(data), pdMS_TO_TICKS(200));
        if (len <= 0) {
            continue;
        }
        for (int i = 0; i < len; ++i) {
            handle_stream_byte(data[i]);
        }
    }
}
#endif

} // namespace

esp_err_t voice_module_init(VoiceModuleCallback callback, void *user_ctx)
{
#if CONFIG_MOCHI_VOICE_MODULE_ENABLE
    g_callback = callback;
    g_callback_ctx = user_ctx;

    portENTER_CRITICAL(&g_status_lock);
    g_status = {};
    g_status.enabled = true;
    portEXIT_CRITICAL(&g_status_lock);

    uart_config_t uart_config = {};
    uart_config.baud_rate = CONFIG_MOCHI_VOICE_MODULE_BAUDRATE;
    uart_config.data_bits = UART_DATA_8_BITS;
    uart_config.parity = UART_PARITY_DISABLE;
    uart_config.stop_bits = UART_STOP_BITS_1;
    uart_config.flow_ctrl = UART_HW_FLOWCTRL_DISABLE;
    uart_config.source_clk = UART_SCLK_DEFAULT;

    ESP_RETURN_ON_ERROR(uart_driver_install(kUartPort, kRxBufferSize, 0, 0, nullptr, 0),
                        kTag, "uart driver install failed");
    ESP_RETURN_ON_ERROR(uart_param_config(kUartPort, &uart_config), kTag, "uart config failed");
    ESP_RETURN_ON_ERROR(uart_set_pin(kUartPort,
                                     CONFIG_MOCHI_VOICE_MODULE_TX_GPIO,
                                     CONFIG_MOCHI_VOICE_MODULE_RX_GPIO,
                                     UART_PIN_NO_CHANGE,
                                     UART_PIN_NO_CHANGE),
                        kTag, "uart pin config failed");

    if (xTaskCreate(task_voice_module_rx, "voice_uart", CONFIG_MOCHI_VOICE_MODULE_TASK_STACK,
                    nullptr, CONFIG_MOCHI_VOICE_MODULE_TASK_PRIO, &g_task) != pdPASS) {
        ESP_ERROR_CHECK_WITHOUT_ABORT(uart_driver_delete(kUartPort));
        return ESP_ERR_NO_MEM;
    }

    ESP_LOGI(kTag, "voice module ready: uart=%d baud=%d rx=%d tx=%d",
             static_cast<int>(kUartPort),
             CONFIG_MOCHI_VOICE_MODULE_BAUDRATE,
             CONFIG_MOCHI_VOICE_MODULE_RX_GPIO,
             CONFIG_MOCHI_VOICE_MODULE_TX_GPIO);
    return ESP_OK;
#else
    (void)callback;
    (void)user_ctx;
    g_status = {};
    g_status.enabled = false;
    return ESP_ERR_NOT_SUPPORTED;
#endif
}

VoiceModuleStatus voice_module_get_status()
{
    VoiceModuleStatus status = {};
#if CONFIG_MOCHI_VOICE_MODULE_ENABLE
    portENTER_CRITICAL(&g_status_lock);
    status = g_status;
    portEXIT_CRITICAL(&g_status_lock);
#else
    status.enabled = false;
#endif
    return status;
}

esp_err_t voice_module_send_code(uint16_t code)
{
#if CONFIG_MOCHI_VOICE_MODULE_ENABLE
    if (!voice_module_get_status().ready) {
        return ESP_ERR_INVALID_STATE;
    }
    const uint8_t frame[5] = {
        kFrameHead0,
        kFrameHead1,
        static_cast<uint8_t>(code >> 8),
        static_cast<uint8_t>(code & 0xFF),
        kFrameTail,
    };
    const int written = uart_write_bytes(kUartPort, frame, sizeof(frame));
    return written == static_cast<int>(sizeof(frame)) ? ESP_OK : ESP_FAIL;
#else
    (void)code;
    return ESP_ERR_NOT_SUPPORTED;
#endif
}
