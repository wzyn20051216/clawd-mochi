#pragma once

#include <cstddef>
#include <cstdint>
#include <string>

#include "esp_err.h"

/**
 * @brief SPI LCD 显示与基础 2D 绘图封装。
 *
 * 本类提供一个接近 Arduino GFX 的轻量接口，内部维护 RGB565 帧缓冲。
 * 所有绘图先写入 RAM，再通过 flush() 刷新到 LCD，避免在业务层暴露
 * esp_lcd 细节，也便于从原始 .ino 逻辑迁移。
 */
class MochiDisplay {
public:
    /**
     * @brief 初始化 SPI 总线、LCD 面板和背光 GPIO。
     *
     * @return ESP_OK 表示初始化完成。
     */
    esp_err_t init();

    /**
     * @brief 获取当前 LCD 逻辑宽度。
     */
    int width() const { return width_; }

    /**
     * @brief 获取当前 LCD 逻辑高度。
     */
    int height() const { return height_; }

    /**
     * @brief 获取当前 LCD 驱动名称。
     */
    const char *driverName() const;

    /**
     * @brief 设置背光开关。
     *
     * @param on true 打开背光，false 关闭背光。
     */
    void setBacklight(bool on);

    /**
     * @brief 将当前帧缓冲刷新到屏幕。
     */
    void flush();

    /**
     * @brief 刷新指定矩形区域。
     *
     * @param x 左上角 X。
     * @param y 左上角 Y。
     * @param w 宽度。
     * @param h 高度。
     */
    void flushRect(int x, int y, int w, int h);

    /**
     * @brief 清空整屏为指定颜色。
     *
     * @param color RGB565 颜色。
     */
    void fillScreen(uint16_t color);

    /**
     * @brief 填充矩形。
     */
    void fillRect(int x, int y, int w, int h, uint16_t color);

    /**
     * @brief 绘制水平线。
     */
    void drawFastHLine(int x, int y, int w, uint16_t color);

    /**
     * @brief 绘制直线。
     */
    void drawLine(int x0, int y0, int x1, int y1, uint16_t color);

    /**
     * @brief 填充圆形。
     */
    void fillCircle(int x0, int y0, int radius, uint16_t color);

    /**
     * @brief 填充三角形。
     */
    void fillTriangle(int x0, int y0, int x1, int y1, int x2, int y2, uint16_t color);

    /**
     * @brief 设置文本绘制颜色。
     */
    void setTextColor(uint16_t color);

    /**
     * @brief 设置文本缩放倍数。
     */
    void setTextSize(uint8_t size);

    /**
     * @brief 设置文本光标。
     */
    void setCursor(int x, int y);

    /**
     * @brief 绘制字符串。
     */
    void print(const char *text);

    /**
     * @brief 绘制字符串。
     */
    void print(const std::string &text);

    /**
     * @brief RGB888 转 RGB565。
     */
    static constexpr uint16_t color565(uint8_t r, uint8_t g, uint8_t b)
    {
        return static_cast<uint16_t>(((r & 0xF8) << 8) | ((g & 0xFC) << 3) | (b >> 3));
    }

private:
    void drawPixel(int x, int y, uint16_t color);
    void drawChar(int x, int y, char c, uint16_t color, uint8_t size);
    void setAddressWindow(int x, int y, int w, int h);
    static uint16_t byteSwap16(uint16_t value);

    uint16_t *framebuffer_ = nullptr;
    void *io_ = nullptr;
    int width_ = 0;
    int height_ = 0;
    int cursor_x_ = 0;
    int cursor_y_ = 0;
    uint16_t text_color_ = 0xFFFF;
    uint8_t text_size_ = 1;
};
