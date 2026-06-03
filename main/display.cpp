#include "display.hpp"

#include <algorithm>
#include <cstring>

#include "driver/gpio.h"
#include "driver/spi_master.h"
#include "esp_check.h"
#include "esp_heap_caps.h"
#include "esp_lcd_io_spi.h"
#include "esp_lcd_panel_io.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "sdkconfig.h"

#ifndef CONFIG_MOCHI_LCD_BACKLIGHT_ACTIVE_HIGH
#define CONFIG_MOCHI_LCD_BACKLIGHT_ACTIVE_HIGH 1
#endif

namespace {

constexpr const char *kTag = "mochi_display";
constexpr spi_host_device_t kSpiHost = SPI2_HOST;

constexpr uint8_t kCmdSwReset = 0x01;
constexpr uint8_t kCmdFrameRate1 = 0xB1;
constexpr uint8_t kCmdFrameRate2 = 0xB2;
constexpr uint8_t kCmdFrameRate3 = 0xB3;
constexpr uint8_t kCmdInvCtr = 0xB4;
constexpr uint8_t kCmdPwCtr1 = 0xC0;
constexpr uint8_t kCmdPwCtr2 = 0xC1;
constexpr uint8_t kCmdPwCtr3 = 0xC2;
constexpr uint8_t kCmdPwCtr4 = 0xC3;
constexpr uint8_t kCmdPwCtr5 = 0xC4;
constexpr uint8_t kCmdVmCtr1 = 0xC5;
constexpr uint8_t kCmdSleepOut = 0x11;
constexpr uint8_t kCmdColMod = 0x3A;
constexpr uint8_t kCmdMadCtl = 0x36;
constexpr uint8_t kCmdNorOn = 0x13;
constexpr uint8_t kCmdInvOn = 0x21;
constexpr uint8_t kCmdInvOff = 0x20;
constexpr uint8_t kCmdGmCtrP1 = 0xE0;
constexpr uint8_t kCmdGmCtrN1 = 0xE1;
constexpr uint8_t kCmdDispOn = 0x29;
constexpr uint8_t kCmdCaseT = 0x2A;
constexpr uint8_t kCmdRaseT = 0x2B;
constexpr uint8_t kCmdRamWr = 0x2C;

struct PanelSpec {
    const char *name;
    int width;
    int height;
    bool invert;
    bool st7735;
};

#if CONFIG_MOCHI_LCD_DRIVER_ST7735
constexpr PanelSpec kPanel = {"ST7735", 128, 160, false, true};
#else
constexpr PanelSpec kPanel = {"ST7789", 240, 240, true, false};
#endif

constexpr uint8_t kFont5x7[][5] = {
    {0x00,0x00,0x00,0x00,0x00}, {0x00,0x00,0x5F,0x00,0x00}, {0x00,0x07,0x00,0x07,0x00}, {0x14,0x7F,0x14,0x7F,0x14},
    {0x24,0x2A,0x7F,0x2A,0x12}, {0x23,0x13,0x08,0x64,0x62}, {0x36,0x49,0x55,0x22,0x50}, {0x00,0x05,0x03,0x00,0x00},
    {0x00,0x1C,0x22,0x41,0x00}, {0x00,0x41,0x22,0x1C,0x00}, {0x14,0x08,0x3E,0x08,0x14}, {0x08,0x08,0x3E,0x08,0x08},
    {0x00,0x50,0x30,0x00,0x00}, {0x08,0x08,0x08,0x08,0x08}, {0x00,0x60,0x60,0x00,0x00}, {0x20,0x10,0x08,0x04,0x02},
    {0x3E,0x51,0x49,0x45,0x3E}, {0x00,0x42,0x7F,0x40,0x00}, {0x42,0x61,0x51,0x49,0x46}, {0x21,0x41,0x45,0x4B,0x31},
    {0x18,0x14,0x12,0x7F,0x10}, {0x27,0x45,0x45,0x45,0x39}, {0x3C,0x4A,0x49,0x49,0x30}, {0x01,0x71,0x09,0x05,0x03},
    {0x36,0x49,0x49,0x49,0x36}, {0x06,0x49,0x49,0x29,0x1E}, {0x00,0x36,0x36,0x00,0x00}, {0x00,0x56,0x36,0x00,0x00},
    {0x08,0x14,0x22,0x41,0x00}, {0x14,0x14,0x14,0x14,0x14}, {0x00,0x41,0x22,0x14,0x08}, {0x02,0x01,0x51,0x09,0x06},
    {0x32,0x49,0x79,0x41,0x3E}, {0x7E,0x11,0x11,0x11,0x7E}, {0x7F,0x49,0x49,0x49,0x36}, {0x3E,0x41,0x41,0x41,0x22},
    {0x7F,0x41,0x41,0x22,0x1C}, {0x7F,0x49,0x49,0x49,0x41}, {0x7F,0x09,0x09,0x09,0x01}, {0x3E,0x41,0x49,0x49,0x7A},
    {0x7F,0x08,0x08,0x08,0x7F}, {0x00,0x41,0x7F,0x41,0x00}, {0x20,0x40,0x41,0x3F,0x01}, {0x7F,0x08,0x14,0x22,0x41},
    {0x7F,0x40,0x40,0x40,0x40}, {0x7F,0x02,0x0C,0x02,0x7F}, {0x7F,0x04,0x08,0x10,0x7F}, {0x3E,0x41,0x41,0x41,0x3E},
    {0x7F,0x09,0x09,0x09,0x06}, {0x3E,0x41,0x51,0x21,0x5E}, {0x7F,0x09,0x19,0x29,0x46}, {0x46,0x49,0x49,0x49,0x31},
    {0x01,0x01,0x7F,0x01,0x01}, {0x3F,0x40,0x40,0x40,0x3F}, {0x1F,0x20,0x40,0x20,0x1F}, {0x3F,0x40,0x38,0x40,0x3F},
    {0x63,0x14,0x08,0x14,0x63}, {0x07,0x08,0x70,0x08,0x07}, {0x61,0x51,0x49,0x45,0x43}, {0x00,0x7F,0x41,0x41,0x00},
    {0x02,0x04,0x08,0x10,0x20}, {0x00,0x41,0x41,0x7F,0x00}, {0x04,0x02,0x01,0x02,0x04}, {0x40,0x40,0x40,0x40,0x40},
    {0x00,0x01,0x02,0x04,0x00}, {0x20,0x54,0x54,0x54,0x78}, {0x7F,0x48,0x44,0x44,0x38}, {0x38,0x44,0x44,0x44,0x20},
    {0x38,0x44,0x44,0x48,0x7F}, {0x38,0x54,0x54,0x54,0x18}, {0x08,0x7E,0x09,0x01,0x02}, {0x0C,0x52,0x52,0x52,0x3E},
    {0x7F,0x08,0x04,0x04,0x78}, {0x00,0x44,0x7D,0x40,0x00}, {0x20,0x40,0x44,0x3D,0x00}, {0x7F,0x10,0x28,0x44,0x00},
    {0x00,0x41,0x7F,0x40,0x00}, {0x7C,0x04,0x18,0x04,0x78}, {0x7C,0x08,0x04,0x04,0x78}, {0x38,0x44,0x44,0x44,0x38},
    {0x7C,0x14,0x14,0x14,0x08}, {0x08,0x14,0x14,0x18,0x7C}, {0x7C,0x08,0x04,0x04,0x08}, {0x48,0x54,0x54,0x54,0x20},
    {0x04,0x3F,0x44,0x40,0x20}, {0x3C,0x40,0x40,0x20,0x7C}, {0x1C,0x20,0x40,0x20,0x1C}, {0x3C,0x40,0x30,0x40,0x3C},
    {0x44,0x28,0x10,0x28,0x44}, {0x0C,0x50,0x50,0x50,0x3C}, {0x44,0x64,0x54,0x4C,0x44}, {0x00,0x08,0x36,0x41,0x00},
    {0x00,0x00,0x7F,0x00,0x00}, {0x00,0x41,0x36,0x08,0x00}, {0x08,0x04,0x08,0x10,0x08}
};

} // namespace

esp_err_t MochiDisplay::init()
{
    width_ = kPanel.width;
    height_ = kPanel.height;
    framebuffer_ = static_cast<uint16_t *>(heap_caps_malloc(width_ * height_ * sizeof(uint16_t), MALLOC_CAP_DMA));
    ESP_RETURN_ON_FALSE(framebuffer_ != nullptr, ESP_ERR_NO_MEM, kTag, "framebuffer alloc failed");

    gpio_config_t bl_cfg = {};
    bl_cfg.pin_bit_mask = 1ULL << CONFIG_MOCHI_PIN_LCD_BL;
    bl_cfg.mode = GPIO_MODE_OUTPUT;
    ESP_RETURN_ON_ERROR(gpio_config(&bl_cfg), kTag, "backlight gpio config failed");
    setBacklight(true);

    spi_bus_config_t buscfg = {};
    buscfg.sclk_io_num = CONFIG_MOCHI_PIN_LCD_SCLK;
    buscfg.mosi_io_num = CONFIG_MOCHI_PIN_LCD_MOSI;
    buscfg.miso_io_num = -1;
    buscfg.quadwp_io_num = -1;
    buscfg.quadhd_io_num = -1;
    buscfg.max_transfer_sz = width_ * height_ * sizeof(uint16_t) + 8;
    ESP_RETURN_ON_ERROR(spi_bus_initialize(kSpiHost, &buscfg, SPI_DMA_CH_AUTO), kTag, "spi init failed");

    esp_lcd_panel_io_handle_t io = nullptr;
    esp_lcd_panel_io_spi_config_t io_config = {};
    io_config.cs_gpio_num = CONFIG_MOCHI_PIN_LCD_CS;
    io_config.dc_gpio_num = CONFIG_MOCHI_PIN_LCD_DC;
    io_config.spi_mode = 0;
    io_config.pclk_hz = CONFIG_MOCHI_LCD_SPI_CLOCK_HZ;
    io_config.trans_queue_depth = 10;
    io_config.lcd_cmd_bits = 8;
    io_config.lcd_param_bits = 8;
    ESP_RETURN_ON_ERROR(esp_lcd_new_panel_io_spi(kSpiHost, &io_config, &io), kTag, "panel io failed");

    io_ = io;

    gpio_config_t rst_cfg = {};
    rst_cfg.pin_bit_mask = 1ULL << CONFIG_MOCHI_PIN_LCD_RST;
    rst_cfg.mode = GPIO_MODE_OUTPUT;
    ESP_RETURN_ON_ERROR(gpio_config(&rst_cfg), kTag, "reset gpio config failed");
    gpio_set_level(static_cast<gpio_num_t>(CONFIG_MOCHI_PIN_LCD_RST), 0);
    vTaskDelay(pdMS_TO_TICKS(20));
    gpio_set_level(static_cast<gpio_num_t>(CONFIG_MOCHI_PIN_LCD_RST), 1);
    vTaskDelay(pdMS_TO_TICKS(120));

    ESP_RETURN_ON_ERROR(esp_lcd_panel_io_tx_param(io, kCmdSwReset, nullptr, 0), kTag, "lcd swreset failed");
    vTaskDelay(pdMS_TO_TICKS(150));
    ESP_RETURN_ON_ERROR(esp_lcd_panel_io_tx_param(io, kCmdSleepOut, nullptr, 0), kTag, "lcd sleep out failed");
    vTaskDelay(pdMS_TO_TICKS(120));

    if (kPanel.st7735) {
        const uint8_t fr1[] = {0x01, 0x2C, 0x2D};
        const uint8_t fr2[] = {0x01, 0x2C, 0x2D};
        const uint8_t fr3[] = {0x01, 0x2C, 0x2D, 0x01, 0x2C, 0x2D};
        const uint8_t invctr[] = {0x07};
        const uint8_t pw1[] = {0xA2, 0x02, 0x84};
        const uint8_t pw2[] = {0xC5};
        const uint8_t pw3[] = {0x0A, 0x00};
        const uint8_t pw4[] = {0x8A, 0x2A};
        const uint8_t pw5[] = {0x8A, 0xEE};
        const uint8_t vmctr1[] = {0x0E};
        const uint8_t gamma_p[] = {0x02,0x1C,0x07,0x12,0x37,0x32,0x29,0x2D,0x29,0x25,0x2B,0x39,0x00,0x01,0x03,0x10};
        const uint8_t gamma_n[] = {0x03,0x1D,0x07,0x06,0x2E,0x2C,0x29,0x2D,0x2E,0x2E,0x37,0x3F,0x00,0x00,0x02,0x10};
        ESP_RETURN_ON_ERROR(esp_lcd_panel_io_tx_param(io, kCmdFrameRate1, fr1, sizeof(fr1)), kTag, "st7735 fr1 failed");
        ESP_RETURN_ON_ERROR(esp_lcd_panel_io_tx_param(io, kCmdFrameRate2, fr2, sizeof(fr2)), kTag, "st7735 fr2 failed");
        ESP_RETURN_ON_ERROR(esp_lcd_panel_io_tx_param(io, kCmdFrameRate3, fr3, sizeof(fr3)), kTag, "st7735 fr3 failed");
        ESP_RETURN_ON_ERROR(esp_lcd_panel_io_tx_param(io, kCmdInvCtr, invctr, sizeof(invctr)), kTag, "st7735 invctr failed");
        ESP_RETURN_ON_ERROR(esp_lcd_panel_io_tx_param(io, kCmdPwCtr1, pw1, sizeof(pw1)), kTag, "st7735 pw1 failed");
        ESP_RETURN_ON_ERROR(esp_lcd_panel_io_tx_param(io, kCmdPwCtr2, pw2, sizeof(pw2)), kTag, "st7735 pw2 failed");
        ESP_RETURN_ON_ERROR(esp_lcd_panel_io_tx_param(io, kCmdPwCtr3, pw3, sizeof(pw3)), kTag, "st7735 pw3 failed");
        ESP_RETURN_ON_ERROR(esp_lcd_panel_io_tx_param(io, kCmdPwCtr4, pw4, sizeof(pw4)), kTag, "st7735 pw4 failed");
        ESP_RETURN_ON_ERROR(esp_lcd_panel_io_tx_param(io, kCmdPwCtr5, pw5, sizeof(pw5)), kTag, "st7735 pw5 failed");
        ESP_RETURN_ON_ERROR(esp_lcd_panel_io_tx_param(io, kCmdVmCtr1, vmctr1, sizeof(vmctr1)), kTag, "st7735 vmctr failed");
        ESP_RETURN_ON_ERROR(esp_lcd_panel_io_tx_param(io, kCmdGmCtrP1, gamma_p, sizeof(gamma_p)), kTag, "st7735 gamma p failed");
        ESP_RETURN_ON_ERROR(esp_lcd_panel_io_tx_param(io, kCmdGmCtrN1, gamma_n, sizeof(gamma_n)), kTag, "st7735 gamma n failed");
    }

    const uint8_t colmod = 0x05;
    ESP_RETURN_ON_ERROR(esp_lcd_panel_io_tx_param(io, kCmdColMod, &colmod, sizeof(colmod)), kTag, "lcd colmod failed");
    const uint8_t madctl = CONFIG_MOCHI_LCD_MADCTL;
    ESP_RETURN_ON_ERROR(esp_lcd_panel_io_tx_param(io, kCmdMadCtl, &madctl, sizeof(madctl)), kTag, "lcd madctl failed");
    if (kPanel.invert) {
        ESP_RETURN_ON_ERROR(esp_lcd_panel_io_tx_param(io, kCmdInvOn, nullptr, 0), kTag, "lcd invert failed");
    } else {
        ESP_RETURN_ON_ERROR(esp_lcd_panel_io_tx_param(io, kCmdInvOff, nullptr, 0), kTag, "lcd invert off failed");
    }
    ESP_RETURN_ON_ERROR(esp_lcd_panel_io_tx_param(io, kCmdNorOn, nullptr, 0), kTag, "lcd normal mode failed");
    ESP_RETURN_ON_ERROR(esp_lcd_panel_io_tx_param(io, kCmdDispOn, nullptr, 0), kTag, "lcd display on failed");
    vTaskDelay(pdMS_TO_TICKS(20));

    fillScreen(0x0000);
    flush();
    ESP_LOGI(kTag, "%s ready %dx%d, pins: mosi=%d sclk=%d cs=%d dc=%d rst=%d bl=%d",
             kPanel.name, width_, height_,
             CONFIG_MOCHI_PIN_LCD_MOSI, CONFIG_MOCHI_PIN_LCD_SCLK, CONFIG_MOCHI_PIN_LCD_CS,
             CONFIG_MOCHI_PIN_LCD_DC, CONFIG_MOCHI_PIN_LCD_RST, CONFIG_MOCHI_PIN_LCD_BL);
    return ESP_OK;
}

void MochiDisplay::setBacklight(bool on)
{
    const bool level = CONFIG_MOCHI_LCD_BACKLIGHT_ACTIVE_HIGH ? on : !on;
    gpio_set_level(static_cast<gpio_num_t>(CONFIG_MOCHI_PIN_LCD_BL), level ? 1 : 0);
}

const char *MochiDisplay::driverName() const
{
    return kPanel.name;
}

void MochiDisplay::flush()
{
    if (io_ == nullptr || framebuffer_ == nullptr) {
        return;
    }
    auto io = static_cast<esp_lcd_panel_io_handle_t>(io_);
    constexpr int kRowsPerFlush = 16;
    for (int y = 0; y < height_; y += kRowsPerFlush) {
        const int rows = std::min(kRowsPerFlush, height_ - y);
        setAddressWindow(0, y, width_, rows);
        esp_lcd_panel_io_tx_color(io, kCmdRamWr, framebuffer_ + y * width_, width_ * rows * sizeof(uint16_t));
    }
}

void MochiDisplay::flushRect(int x, int y, int w, int h)
{
    if (io_ == nullptr || framebuffer_ == nullptr || w <= 0 || h <= 0) {
        return;
    }
    const int x0 = std::max(0, x);
    const int y0 = std::max(0, y);
    const int x1 = std::min(width_, x + w);
    const int y1 = std::min(height_, y + h);
    if (x0 >= x1 || y0 >= y1) {
        return;
    }
    auto io = static_cast<esp_lcd_panel_io_handle_t>(io_);
    for (int row = y0; row < y1; ++row) {
        setAddressWindow(x0, row, x1 - x0, 1);
        esp_lcd_panel_io_tx_color(io, kCmdRamWr, framebuffer_ + row * width_ + x0, (x1 - x0) * sizeof(uint16_t));
    }
}

void MochiDisplay::fillScreen(uint16_t color)
{
    if (framebuffer_ == nullptr) {
        return;
    }
    std::fill(framebuffer_, framebuffer_ + width_ * height_, byteSwap16(color));
}

void MochiDisplay::fillRect(int x, int y, int w, int h, uint16_t color)
{
    if (w <= 0 || h <= 0 || framebuffer_ == nullptr) {
        return;
    }
    const int x0 = std::max(0, x);
    const int y0 = std::max(0, y);
    const int x1 = std::min(width_, x + w);
    const int y1 = std::min(height_, y + h);
    const uint16_t c = byteSwap16(color);
    for (int yy = y0; yy < y1; ++yy) {
        std::fill(framebuffer_ + yy * width_ + x0, framebuffer_ + yy * width_ + x1, c);
    }
}

void MochiDisplay::drawFastHLine(int x, int y, int w, uint16_t color)
{
    fillRect(x, y, w, 1, color);
}

void MochiDisplay::drawLine(int x0, int y0, int x1, int y1, uint16_t color)
{
    const bool steep = std::abs(y1 - y0) > std::abs(x1 - x0);
    if (steep) {
        std::swap(x0, y0);
        std::swap(x1, y1);
    }
    if (x0 > x1) {
        std::swap(x0, x1);
        std::swap(y0, y1);
    }

    const int dx = x1 - x0;
    const int dy = std::abs(y1 - y0);
    int err = dx / 2;
    const int ystep = (y0 < y1) ? 1 : -1;

    for (; x0 <= x1; ++x0) {
        if (steep) {
            drawPixel(y0, x0, color);
        } else {
            drawPixel(x0, y0, color);
        }
        err -= dy;
        if (err < 0) {
            y0 += ystep;
            err += dx;
        }
    }
}

void MochiDisplay::fillCircle(int x0, int y0, int radius, uint16_t color)
{
    for (int y = -radius; y <= radius; ++y) {
        for (int x = -radius; x <= radius; ++x) {
            if (x * x + y * y <= radius * radius) {
                drawPixel(x0 + x, y0 + y, color);
            }
        }
    }
}

void MochiDisplay::fillTriangle(int x0, int y0, int x1, int y1, int x2, int y2, uint16_t color)
{
    auto edge = [](int ax, int ay, int bx, int by, int px, int py) {
        return (px - ax) * (by - ay) - (py - ay) * (bx - ax);
    };

    const int min_x = std::max(0, std::min({x0, x1, x2}));
    const int max_x = std::min(width_ - 1, std::max({x0, x1, x2}));
    const int min_y = std::max(0, std::min({y0, y1, y2}));
    const int max_y = std::min(height_ - 1, std::max({y0, y1, y2}));

    const int area = edge(x0, y0, x1, y1, x2, y2);
    if (area == 0) {
        drawLine(x0, y0, x1, y1, color);
        drawLine(x1, y1, x2, y2, color);
        return;
    }

    for (int y = min_y; y <= max_y; ++y) {
        for (int x = min_x; x <= max_x; ++x) {
            const int w0 = edge(x1, y1, x2, y2, x, y);
            const int w1 = edge(x2, y2, x0, y0, x, y);
            const int w2 = edge(x0, y0, x1, y1, x, y);
            if ((w0 >= 0 && w1 >= 0 && w2 >= 0) || (w0 <= 0 && w1 <= 0 && w2 <= 0)) {
                drawPixel(x, y, color);
            }
        }
    }
}

void MochiDisplay::setTextColor(uint16_t color)
{
    text_color_ = color;
}

void MochiDisplay::setTextSize(uint8_t size)
{
    text_size_ = std::max<uint8_t>(1, size);
}

void MochiDisplay::setCursor(int x, int y)
{
    cursor_x_ = x;
    cursor_y_ = y;
}

void MochiDisplay::print(const char *text)
{
    if (text == nullptr) {
        return;
    }
    while (*text != '\0') {
        if (*text == '\n') {
            cursor_x_ = 0;
            cursor_y_ += 8 * text_size_;
        } else {
            drawChar(cursor_x_, cursor_y_, *text, text_color_, text_size_);
            cursor_x_ += 6 * text_size_;
        }
        ++text;
    }
}

void MochiDisplay::print(const std::string &text)
{
    print(text.c_str());
}

void MochiDisplay::drawPixel(int x, int y, uint16_t color)
{
    if (framebuffer_ == nullptr || x < 0 || x >= width_ || y < 0 || y >= height_) {
        return;
    }
    framebuffer_[y * width_ + x] = byteSwap16(color);
}

void MochiDisplay::drawChar(int x, int y, char c, uint16_t color, uint8_t size)
{
    if (c < 32 || c > 126) {
        c = '?';
    }
    const uint8_t *glyph = kFont5x7[static_cast<int>(c) - 32];
    for (int col = 0; col < 5; ++col) {
        uint8_t line = glyph[col];
        for (int row = 0; row < 7; ++row) {
            if ((line & 0x01) != 0) {
                fillRect(x + col * size, y + row * size, size, size, color);
            }
            line >>= 1;
        }
    }
}

uint16_t MochiDisplay::byteSwap16(uint16_t value)
{
    return static_cast<uint16_t>((value << 8) | (value >> 8));
}

void MochiDisplay::setAddressWindow(int x, int y, int w, int h)
{
    auto io = static_cast<esp_lcd_panel_io_handle_t>(io_);
    const uint16_t x0 = CONFIG_MOCHI_LCD_X_OFFSET + x;
    const uint16_t y0 = CONFIG_MOCHI_LCD_Y_OFFSET + y;
    const uint16_t x1 = CONFIG_MOCHI_LCD_X_OFFSET + x + w - 1;
    const uint16_t y1 = CONFIG_MOCHI_LCD_Y_OFFSET + y + h - 1;
    const uint8_t caset[] = {
        static_cast<uint8_t>(x0 >> 8), static_cast<uint8_t>(x0 & 0xFF),
        static_cast<uint8_t>(x1 >> 8), static_cast<uint8_t>(x1 & 0xFF),
    };
    const uint8_t raset[] = {
        static_cast<uint8_t>(y0 >> 8), static_cast<uint8_t>(y0 & 0xFF),
        static_cast<uint8_t>(y1 >> 8), static_cast<uint8_t>(y1 & 0xFF),
    };
    esp_lcd_panel_io_tx_param(io, kCmdCaseT, caset, sizeof(caset));
    esp_lcd_panel_io_tx_param(io, kCmdRaseT, raset, sizeof(raset));
}
