#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#include "display.hpp"

#include "esp_check.h"
#include "esp_event.h"
#include "esp_http_server.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "nvs.h"
#include "nvs_flash.h"
#include "sdkconfig.h"

namespace {

constexpr const char *kTag = "clawd_mochi";
constexpr uint16_t kWhite = 0xFFFF;
constexpr uint16_t kBlack = 0x0000;

constexpr int kDesignW = 240;
constexpr int kDesignH = 240;
constexpr int kEyeWDesign = 30;
constexpr int kEyeHDesign = 60;
constexpr int kEyeGapDesign = 120;
constexpr int kEyeOxDesign = 0;
constexpr int kEyeOyDesign = 40;
constexpr int kTermCols = 15;
constexpr int kTermRows = 8;
constexpr int kTermCharW = 12;
constexpr int kTermCharH = 20;
constexpr int kTermPadX = 8;
constexpr int kTermPadY = 18;
constexpr int kPrefixPx = 54;

enum View : uint8_t {
    kViewEyesNormal = 0,
    kViewEyesSquish = 1,
    kViewCode = 2,
    kViewDraw = 3,
};

enum Face : uint8_t {
    kFaceNormal = 0,
    kFaceSquish = 1,
    kFaceHappy = 2,
    kFaceSleepy = 3,
    kFaceAngry = 4,
};

MochiDisplay g_display;
httpd_handle_t g_server = nullptr;
uint16_t g_orange = 0;
uint16_t g_dark_bg = 0;
uint16_t g_muted = 0;
uint16_t g_green = 0;
uint16_t g_anim_bg = 0;
uint16_t g_draw_bg = 0;
View g_current_view = kViewEyesNormal;
Face g_current_face = kFaceNormal;
bool g_busy = false;
bool g_backlight_on = true;
bool g_term_mode = false;
uint8_t g_anim_speed = 1;
uint8_t g_backlight_brightness = 80;
uint32_t g_manual_anim_until_ms = 0;
std::string g_term_lines[kTermRows];
uint8_t g_term_row = 0;
uint8_t g_term_col = 0;

constexpr char kIndexHtml[] = R"HTML(
<!doctype html><html lang="zh-CN"><head>
<meta charset="utf-8"><meta name="viewport" content="width=device-width,initial-scale=1,user-scalable=no">
<title>Clawd Mochi</title>
<style>
*{box-sizing:border-box}body{margin:0;min-height:100vh;background:#1d1d20;color:#eee8df;font-family:Courier New,monospace;display:flex;align-items:center;justify-content:flex-start;flex-direction:column;padding:22px 14px 44px;gap:14px}
.title{color:#d65728;font-weight:700;letter-spacing:1px;text-align:center;line-height:1.35}.sub{color:#766c64;font-size:11px;letter-spacing:2px}
.grid{width:100%;max-width:390px;display:grid;grid-template-columns:1fr 1fr;gap:9px}.btn,.wide{border:1px solid #3f3936;background:#262529;color:#eee8df;border-radius:8px;padding:15px 8px;font:700 13px Courier New,monospace}
.btn:active,.wide:active{transform:scale(.96)}.btn.active{border-color:#d65728;background:#25150d}.wide{width:100%;max-width:390px}.row{width:100%;max-width:390px;display:flex;gap:10px;align-items:center;color:#8f867d;font-size:12px}
input[type=range]{flex:1;accent-color:#d65728}.sw{width:54px;height:38px;border:1px solid #3f3936;border-radius:8px;background:#262529}.canvas{display:none;touch-action:none;background:#ff8000;width:240px;height:240px;border:1px solid #3f3936;image-rendering:pixelated}
.canvas.on{display:block}.term{display:none;width:100%;max-width:390px;grid-template-columns:1fr auto;gap:8px}.term.on{display:grid}.term input{min-width:0;background:#111318;color:#e8e4dc;border:1px solid #3f3936;border-radius:8px;padding:12px;font:700 16px Courier New,monospace}
.note{font-size:11px;color:#746b63;text-align:center;max-width:390px;line-height:1.5}
</style></head><body>
<div class="title">/\\___/\\<br>(  o o  )<br>Clawd Mochi</div>
<div class="sub">ESP-IDF · ESP32-S3</div>
<div class="grid">
<button class="btn" data-v="0" onclick="cmd('w',0)">normal eyes</button>
<button class="btn" data-v="1" onclick="cmd('s',1)">squish eyes</button>
<button class="btn" data-v="2" onclick="cmd('d',2);openTerm()">Claude Code</button>
<button class="btn" data-v="3" onclick="openCanvas()">canvas</button>
</div>
<div class="grid">
<button class="btn" onclick="face(2)">happy</button>
<button class="btn" onclick="face(3)">sleepy</button>
<button class="btn" onclick="face(4)">angry</button>
<button class="btn" onclick="face(0)">reset face</button>
</div>
<div class="row"><span>speed</span><input id="spd" type="range" min="1" max="3" value="1" oninput="speed(this.value)"><span id="sv">slow</span></div>
<div class="row"><span>light</span><input id="br" type="range" min="5" max="100" value="80" oninput="brightness(this.value)"><span id="bv">80%</span></div>
<div class="row"><span>bg</span><input class="sw" id="bg" type="color" value="#ff8000" oninput="redraw()"><span>pen</span><input class="sw" id="pen" type="color" value="#000000"></div>
<button id="bl" class="wide" onclick="backlight()">display on</button>
<canvas id="cv" class="canvas" width="240" height="240"></canvas>
<div id="term" class="term"><input id="tin" maxlength="1" autocomplete="off"><button class="wide" onclick="sendChar()">send</button></div>
<button id="done" class="wide" style="display:none" onclick="closeCanvas()">done</button>
<div class="note">连接热点 ClaWD-Mochi，密码 clawd1234，打开 192.168.4.1 控制桌面小屏。</div>
<script>
let bl=true, drawing=false, pts=[], lcdW=240, lcdH=240; const cv=document.getElementById('cv'), ctx=cv.getContext('2d');
const labels={1:'slow',2:'normal',3:'fast'};
function req(u){return fetch(u,{cache:'no-store'}).catch(()=>{});}
function active(v){document.querySelectorAll('.btn').forEach(b=>b.classList.toggle('active',b.dataset.v==v));}
function cmd(k,v){closeCanvas(false);req('/cmd?k='+k);active(v)}
function face(v){closeCanvas(false);req('/face?v='+v);active(v===1?1:0)}
function speed(v){document.getElementById('sv').textContent=labels[v];req('/speed?v='+v)}
function brightness(v){document.getElementById('bv').textContent=v+'%';bl=true;document.getElementById('bl').textContent='display on';req('/brightness?v='+v)}
function setCanvasSize(w,h){lcdW=w;lcdH=h;cv.width=w;cv.height=h;cv.style.width=Math.min(300,w*1.6)+'px';cv.style.height=Math.min(300,h*1.6)+'px'}
function redraw(){const bg=document.getElementById('bg').value;ctx.fillStyle=bg;ctx.fillRect(0,0,lcdW,lcdH);req('/redraw?bg='+encodeURIComponent(bg))}
function backlight(){bl=!bl;document.getElementById('bl').textContent=bl?'display on':'display off';req('/backlight?on='+(bl?1:0))}
function openTerm(){document.getElementById('term').classList.add('on');document.getElementById('tin').focus()}
function sendChar(){const i=document.getElementById('tin'); if(i.value){req('/char?c='+encodeURIComponent(i.value));i.value='';i.focus()}}
function openCanvas(){document.getElementById('term').classList.remove('on');cv.classList.add('on');document.getElementById('done').style.display='block';active(3);redraw();req('/canvas?on=1')}
function closeCanvas(send=true){cv.classList.remove('on');document.getElementById('done').style.display='none';if(send)req('/cmd?k=w')}
function pos(e){const r=cv.getBoundingClientRect(),p=e.touches?e.touches[0]:e;return [Math.round((p.clientX-r.left)*lcdW/r.width),Math.round((p.clientY-r.top)*lcdH/r.height)]}
function flush(){if(pts.length<1)return;req('/draw/stroke?pen='+encodeURIComponent(document.getElementById('pen').value)+'&pts='+encodeURIComponent(pts.map(p=>p[0]+','+p[1]).join(';')));pts=[]}
function down(e){e.preventDefault();drawing=true;pts=[pos(e)]}
function move(e){if(!drawing)return;e.preventDefault();const p=pos(e), q=pts[pts.length-1];ctx.strokeStyle=document.getElementById('pen').value;ctx.lineWidth=3;ctx.lineCap='round';ctx.beginPath();ctx.moveTo(q[0],q[1]);ctx.lineTo(p[0],p[1]);ctx.stroke();pts.push(p);if(pts.length>10)flush()}
function up(){drawing=false;flush()}
['mousedown','touchstart'].forEach(e=>cv.addEventListener(e,down,{passive:false}));['mousemove','touchmove'].forEach(e=>cv.addEventListener(e,move,{passive:false}));['mouseup','mouseleave','touchend'].forEach(e=>cv.addEventListener(e,up));
window.addEventListener('keydown',e=>{if(document.activeElement.id==='tin')return; if(e.key==='w')cmd('w',0); if(e.key==='s')cmd('s',1); if(e.key==='d'){cmd('d',2);openTerm()}});
fetch('/state').then(r=>r.json()).then(j=>{setCanvasSize(j.w||240,j.h||240);bl=j.bl!==false;document.getElementById('spd').value=j.speed||1;document.getElementById('sv').textContent=labels[j.speed||1];document.getElementById('br').value=j.brightness||80;document.getElementById('bv').textContent=(j.brightness||80)+'%';document.getElementById('bl').textContent=bl?'display on':'display off';active(j.view||0);redraw()}).catch(()=>{redraw()});
</script></body></html>
)HTML";

int speed_ms(int ms)
{
    if (g_anim_speed == 3) {
        return std::max(1, ms / 2);
    }
    if (g_anim_speed == 1) {
        return ms * 2;
    }
    return ms;
}

int scale_design(int value)
{
    const int scale_x = g_display.width() * 1000 / kDesignW;
    const int scale_y = g_display.height() * 1000 / kDesignH;
    const int scale = std::min(scale_x, scale_y);
    return std::max(1, value * scale / 1000);
}

int term_rows()
{
    return std::clamp((g_display.height() - kTermPadY - 4) / kTermCharH, 3, kTermRows);
}

int term_cols()
{
    return std::clamp((g_display.width() - kTermPadX - kPrefixPx) / kTermCharW, 3, kTermCols);
}

void delay_ms(int ms)
{
    vTaskDelay(pdMS_TO_TICKS(ms));
}

uint32_t tick_ms()
{
    return static_cast<uint32_t>(xTaskGetTickCount() * portTICK_PERIOD_MS);
}

void mark_manual_animation()
{
    g_manual_anim_until_ms = tick_ms() + 2500;
}

uint16_t hex_to_rgb565(std::string hex)
{
    if (!hex.empty() && hex[0] == '#') {
        hex.erase(0, 1);
    }
    if (hex.size() != 6) {
        return kWhite;
    }
    char *end = nullptr;
    const long value = std::strtol(hex.c_str(), &end, 16);
    if (end == hex.c_str()) {
        return kWhite;
    }
    return MochiDisplay::color565((value >> 16) & 0xFF, (value >> 8) & 0xFF, value & 0xFF);
}

/**
 * @brief 将 URL 查询参数中的百分号编码还原为普通文本。
 *
 * ESP-IDF 的 httpd_query_key_value() 不会像 Arduino WebServer::arg()
 * 那样自动解码参数，因此网页传来的 %23ff8000 需要还原为 #ff8000。
 */
std::string url_decode(const char *text)
{
    auto hex_value = [](char ch) -> int {
        if (ch >= '0' && ch <= '9') {
            return ch - '0';
        }
        if (ch >= 'a' && ch <= 'f') {
            return ch - 'a' + 10;
        }
        if (ch >= 'A' && ch <= 'F') {
            return ch - 'A' + 10;
        }
        return -1;
    };

    std::string out;
    if (text == nullptr) {
        return out;
    }
    for (size_t i = 0; text[i] != '\0'; ++i) {
        if (text[i] == '%' && text[i + 1] != '\0' && text[i + 2] != '\0') {
            const int hi = hex_value(text[i + 1]);
            const int lo = hex_value(text[i + 2]);
            if (hi >= 0 && lo >= 0) {
                out.push_back(static_cast<char>((hi << 4) | lo));
                i += 2;
                continue;
            }
        }
        out.push_back(text[i] == '+' ? ' ' : text[i]);
    }
    return out;
}

std::string query_value(httpd_req_t *req, const char *key, size_t max_len = 4096)
{
    const size_t query_len = httpd_req_get_url_query_len(req) + 1;
    if (query_len <= 1 || query_len > max_len) {
        return {};
    }
    std::vector<char> query(query_len);
    if (httpd_req_get_url_query_str(req, query.data(), query.size()) != ESP_OK) {
        return {};
    }
    char value[4096] = {};
    if (httpd_query_key_value(query.data(), key, value, sizeof(value)) != ESP_OK) {
        return {};
    }
    return url_decode(value);
}

void send_json(httpd_req_t *req, const char *json = "{\"ok\":1}")
{
    httpd_resp_set_type(req, "application/json");
    httpd_resp_sendstr(req, json);
}

void set_backlight(bool on)
{
    g_backlight_on = on;
    g_display.setBacklight(on);
}

/**
 * @brief 设置背光亮度并保持开屏状态。
 *
 * @param percent 亮度百分比，限制在 5~100，避免误滑到完全看不见。
 */
void set_brightness(uint8_t percent)
{
    g_backlight_brightness = std::clamp<uint8_t>(percent, 5, 100);
    g_backlight_on = true;
    g_display.setBacklightBrightness(g_backlight_brightness);
}

int16_t eye_lx(int16_t ox)
{
    const int eye_w = scale_design(kEyeWDesign);
    const int gap = scale_design(kEyeGapDesign);
    return (g_display.width() - (eye_w * 2 + gap)) / 2 + scale_design(kEyeOxDesign) + ox;
}

int16_t eye_rx(int16_t ox)
{
    return eye_lx(ox) + scale_design(kEyeWDesign) + scale_design(kEyeGapDesign);
}

int16_t eye_y()
{
    return (g_display.height() - scale_design(kEyeHDesign)) / 2 - scale_design(kEyeOyDesign);
}

int16_t eye_cy()
{
    return eye_y() + scale_design(kEyeHDesign) / 2;
}

void draw_normal_eyes(int16_t ox = 0, bool blink = false)
{
    g_display.fillScreen(g_anim_bg);
    const int16_t lx = eye_lx(ox);
    const int16_t rx = eye_rx(ox);
    const int16_t ey = eye_y();
    const int eye_w = scale_design(kEyeWDesign);
    const int eye_h = scale_design(kEyeHDesign);
    if (!blink) {
        g_display.fillRect(lx, ey, eye_w, eye_h, kBlack);
        g_display.fillRect(rx, ey, eye_w, eye_h, kBlack);
    } else {
        g_display.fillRect(lx, ey + eye_h / 2 - 2, eye_w, 4, kBlack);
        g_display.fillRect(rx, ey + eye_h / 2 - 2, eye_w, 4, kBlack);
    }
    g_display.flush();
}

void draw_chevron(int16_t cx, int16_t cy, int16_t arm, int16_t reach, uint8_t thk, bool right_facing, uint16_t col)
{
    for (int8_t t = -static_cast<int8_t>(thk); t <= static_cast<int8_t>(thk); ++t) {
        if (right_facing) {
            g_display.drawLine(cx - reach / 2, cy - arm + t, cx + reach / 2, cy + t, col);
            g_display.drawLine(cx + reach / 2, cy + t, cx - reach / 2, cy + arm + t, col);
        } else {
            g_display.drawLine(cx + reach / 2, cy - arm + t, cx - reach / 2, cy + t, col);
            g_display.drawLine(cx - reach / 2, cy + t, cx + reach / 2, cy + arm + t, col);
        }
    }
}

void draw_squish_eyes(bool closed = false)
{
    g_display.fillScreen(g_anim_bg);
    const int16_t lx = eye_lx(0);
    const int16_t rx = eye_rx(0);
    const int16_t cy = eye_cy();
    const int eye_w = scale_design(kEyeWDesign);
    const int eye_h = scale_design(kEyeHDesign);
    if (!closed) {
        draw_chevron(lx + eye_w / 2, cy, eye_h / 2, eye_w / 2, std::max(3, scale_design(10)), true, kBlack);
        draw_chevron(rx + eye_w / 2, cy, eye_h / 2, eye_w / 2, std::max(3, scale_design(10)), false, kBlack);
    } else {
        g_display.fillRect(lx, cy - 3, eye_w, 6, kBlack);
        g_display.fillRect(rx, cy - 3, eye_w, 6, kBlack);
    }
    g_display.flush();
}

void draw_sleepy_eyes()
{
    g_display.fillScreen(g_anim_bg);
    const int16_t lx = eye_lx(0);
    const int16_t rx = eye_rx(0);
    const int16_t cy = eye_cy();
    const int eye_w = scale_design(kEyeWDesign);
    const int thk = std::max(3, scale_design(7));
    g_display.fillRect(lx, cy - thk / 2, eye_w, thk, kBlack);
    g_display.fillRect(rx, cy - thk / 2, eye_w, thk, kBlack);
    g_display.flush();
}

void draw_happy_eyes()
{
    g_display.fillScreen(g_anim_bg);
    const int16_t lx = eye_lx(0);
    const int16_t rx = eye_rx(0);
    const int16_t cy = eye_cy() + scale_design(4);
    const int eye_w = scale_design(kEyeWDesign);
    const int arm = scale_design(18);
    const int thk = std::max(2, scale_design(4));
    for (int t = 0; t < thk; ++t) {
        g_display.drawLine(lx, cy + t, lx + eye_w / 2, cy - arm + t, kBlack);
        g_display.drawLine(lx + eye_w / 2, cy - arm + t, lx + eye_w, cy + t, kBlack);
        g_display.drawLine(rx, cy + t, rx + eye_w / 2, cy - arm + t, kBlack);
        g_display.drawLine(rx + eye_w / 2, cy - arm + t, rx + eye_w, cy + t, kBlack);
    }
    g_display.flush();
}

void draw_angry_eyes()
{
    g_display.fillScreen(g_anim_bg);
    const int16_t lx = eye_lx(0);
    const int16_t rx = eye_rx(0);
    const int16_t ey = eye_y() + scale_design(10);
    const int eye_w = scale_design(kEyeWDesign);
    const int eye_h = std::max(6, scale_design(14));
    for (int i = 0; i < eye_h; ++i) {
        g_display.drawLine(lx, ey + i, lx + eye_w, ey + scale_design(18) + i, kBlack);
        g_display.drawLine(rx, ey + scale_design(18) + i, rx + eye_w, ey + i, kBlack);
    }
    g_display.flush();
}

void draw_face(Face face, int16_t ox = 0, bool blink = false)
{
    g_current_face = face;
    switch (face) {
    case kFaceSquish:
        draw_squish_eyes(blink);
        break;
    case kFaceHappy:
        draw_happy_eyes();
        break;
    case kFaceSleepy:
        draw_sleepy_eyes();
        break;
    case kFaceAngry:
        draw_angry_eyes();
        break;
    case kFaceNormal:
    default:
        draw_normal_eyes(ox, blink);
        break;
    }
}

void draw_code_view()
{
    g_term_mode = false;
    g_display.fillScreen(g_dark_bg);
    g_display.fillRect(0, 0, g_display.width(), 4, g_orange);
    g_display.fillRect(0, g_display.height() - 4, g_display.width(), 4, g_orange);
    const int title_size = g_display.width() >= 200 ? 4 : 3;
    g_display.setTextColor(g_orange);
    g_display.setTextSize(title_size);
    g_display.setCursor((g_display.width() - 36 * title_size) / 2, g_display.height() / 2 - 13 * title_size);
    g_display.print("Claude");
    g_display.setTextColor(kWhite);
    g_display.setCursor((g_display.width() - 24 * title_size) / 2, g_display.height() / 2 + 2 * title_size);
    g_display.print("Code");
    g_display.fillRect((g_display.width() - 24 * title_size) / 2, g_display.height() / 2 + 13 * title_size, 24 * title_size, 3, g_orange);
    g_display.flush();
}

void term_clear()
{
    for (auto &line : g_term_lines) {
        line.clear();
    }
    g_term_row = 0;
    g_term_col = 0;
}

void term_draw_prefix(int16_t yy)
{
    g_display.setTextColor(g_green);
    g_display.setTextSize(1);
    g_display.setCursor(kTermPadX, yy + 6);
    g_display.print("clawd:~$ ");
}

void term_draw_header()
{
    g_display.fillRect(0, 0, g_display.width(), kTermPadY + 1, g_dark_bg);
    g_display.setTextColor(g_orange);
    g_display.setTextSize(1);
    g_display.setCursor(kTermPadX, 4);
    g_display.print("clawd@mochi terminal");
    g_display.drawFastHLine(0, kTermPadY, g_display.width(), g_orange);
}

void term_draw_line(uint8_t row)
{
    const int16_t yy = kTermPadY + 4 + row * kTermCharH;
    g_display.fillRect(0, yy, g_display.width(), kTermCharH, g_dark_bg);
    if (row == g_term_row) {
        term_draw_prefix(yy);
    }
    g_display.setTextColor(kWhite);
    g_display.setTextSize(2);
    g_display.setCursor(kTermPadX + kPrefixPx, yy + 1);
    g_display.print(g_term_lines[row]);
    if (row == g_term_row) {
        const int16_t cx = kTermPadX + kPrefixPx + g_term_col * kTermCharW;
        g_display.fillRect(cx, yy + 1, kTermCharW - 2, kTermCharH - 2, g_green);
    }
}

void term_full_redraw()
{
    g_display.fillScreen(g_dark_bg);
    term_draw_header();
    for (uint8_t row = 0; row < term_rows(); ++row) {
        term_draw_line(row);
    }
    g_display.flush();
}

void term_scroll()
{
    const int rows = term_rows();
    for (uint8_t i = 0; i < rows - 1; ++i) {
        g_term_lines[i] = g_term_lines[i + 1];
    }
    g_term_lines[rows - 1].clear();
    g_term_row = rows - 1;
    term_full_redraw();
}

void term_add_char(char c)
{
    if (c == '\n' || c == '\r') {
        ++g_term_row;
        g_term_col = 0;
        if (g_term_row >= term_rows()) {
            term_scroll();
        } else {
            term_draw_line(g_term_row);
            g_display.flush();
        }
    } else if (c == '\b' || c == 127) {
        if (g_term_col > 0 && !g_term_lines[g_term_row].empty()) {
            --g_term_col;
            g_term_lines[g_term_row].pop_back();
            term_full_redraw();
        }
    } else if (std::isprint(static_cast<unsigned char>(c)) != 0) {
        if (g_term_col >= term_cols()) {
            ++g_term_row;
            g_term_col = 0;
            if (g_term_row >= term_rows()) {
                term_scroll();
            }
        }
        g_term_lines[g_term_row].push_back(c);
        ++g_term_col;
        term_full_redraw();
    }
}

void anim_normal_eyes()
{
    g_busy = true;
    mark_manual_animation();
    const int16_t offsets[] = {
        static_cast<int16_t>(-scale_design(16)),
        static_cast<int16_t>(scale_design(16)),
        static_cast<int16_t>(-scale_design(16)),
        static_cast<int16_t>(scale_design(16)),
        0,
    };
    for (int16_t offset : offsets) {
        draw_normal_eyes(offset);
        delay_ms(speed_ms(80));
    }
    draw_normal_eyes(0, true);
    delay_ms(speed_ms(100));
    draw_normal_eyes(0, false);
    delay_ms(speed_ms(70));
    draw_normal_eyes(0, true);
    delay_ms(speed_ms(70));
    draw_normal_eyes(0, false);
    g_busy = false;
}

void anim_squish_eyes()
{
    g_busy = true;
    mark_manual_animation();
    for (uint8_t i = 0; i < 3; ++i) {
        draw_squish_eyes(false);
        delay_ms(speed_ms(160));
        draw_squish_eyes(true);
        delay_ms(speed_ms(100));
    }
    draw_squish_eyes(false);
    g_busy = false;
}

void anim_logo_reveal()
{
    g_busy = true;
    mark_manual_animation();
    g_display.fillScreen(g_anim_bg);
    g_display.setTextColor(kWhite);
    const int final_size = g_display.width() >= 200 ? 4 : 2;
    for (int size = 1; size <= final_size; ++size) {
        g_display.fillScreen(g_anim_bg);
        g_display.setTextSize(size);
        g_display.setCursor((g_display.width() - 36 * size) / 2, g_display.height() / 2 - 14 * size);
        g_display.print("Clawd");
        g_display.setCursor((g_display.width() - 30 * size) / 2, g_display.height() / 2 + 2 * size);
        g_display.print("Mochi");
        g_display.flush();
        delay_ms(speed_ms(180));
    }
    g_display.fillCircle(g_display.width() / 3, g_display.height() / 4, scale_design(10), kWhite);
    g_display.fillCircle(g_display.width() * 2 / 3, g_display.height() / 4, scale_design(10), kWhite);
    g_display.drawLine(g_display.width() / 3 + scale_design(8), g_display.height() / 4 + scale_design(8),
                       g_display.width() / 2 - scale_design(18), g_display.height() / 2 - scale_design(28), kWhite);
    g_display.drawLine(g_display.width() * 2 / 3 - scale_design(8), g_display.height() / 4 + scale_design(8),
                       g_display.width() / 2 + scale_design(18), g_display.height() / 2 - scale_design(28), kWhite);
    g_display.fillRect(g_display.width() / 2 - scale_design(42), g_display.height() * 3 / 4, scale_design(84), 4, kWhite);
    g_display.flush();
    delay_ms(900);
    g_busy = false;
}

void draw_wifi_info()
{
    g_display.fillScreen(g_dark_bg);
    g_display.fillRect(0, 0, g_display.width(), 4, g_orange);
    g_display.setTextColor(kWhite);
    g_display.setTextSize(g_display.width() >= 180 ? 2 : 1);
    g_display.setCursor(12, 16);
    g_display.print("WiFi: ClaWD-Mochi");
    g_display.setTextColor(g_muted);
    g_display.setTextSize(1);
    g_display.setCursor(12, 40);
    g_display.print("password: clawd1234");
    g_display.setTextColor(kWhite);
    g_display.setTextSize(g_display.width() >= 180 ? 2 : 1);
    g_display.setCursor(12, 62);
    g_display.print("Open browser:");
    g_display.setTextColor(g_orange);
    g_display.setCursor(12, 84);
    g_display.print("192.168.4.1");
    g_display.setTextColor(g_muted);
    g_display.setTextSize(1);
    g_display.setCursor(12, 112);
    g_display.print("press web button");
    g_display.flush();
}

bool can_idle_animate()
{
    if (g_busy || g_term_mode || g_current_view == kViewCode || g_current_view == kViewDraw) {
        return false;
    }
    return static_cast<int32_t>(tick_ms() - g_manual_anim_until_ms) >= 0;
}

void task_idle_face(void *)
{
    uint8_t cycle = 0;
    while (true) {
        delay_ms(4200);
        if (!can_idle_animate()) {
            continue;
        }
        g_busy = true;
        if (g_current_face == kFaceNormal) {
            if ((cycle % 4) == 3) {
                draw_normal_eyes(-scale_design(12));
                delay_ms(180);
                draw_normal_eyes(scale_design(12));
                delay_ms(180);
            }
            draw_normal_eyes(0, true);
            delay_ms(90);
            draw_normal_eyes(0, false);
        } else if (g_current_face == kFaceSquish) {
            draw_squish_eyes(true);
            delay_ms(90);
            draw_squish_eyes(false);
        }
        ++cycle;
        g_busy = false;
    }
}

esp_err_t route_root(httpd_req_t *req)
{
    httpd_resp_set_hdr(req, "Cache-Control", "no-store, no-cache, must-revalidate");
    httpd_resp_set_hdr(req, "Pragma", "no-cache");
    httpd_resp_set_type(req, "text/html; charset=utf-8");
    return httpd_resp_send(req, kIndexHtml, HTTPD_RESP_USE_STRLEN);
}

esp_err_t route_cmd(httpd_req_t *req)
{
    const std::string key = query_value(req, "k", 64);
    if (key.empty()) {
        httpd_resp_set_status(req, "400 Bad Request");
        send_json(req, "{\"e\":1}");
        return ESP_OK;
    }
    const char c = key[0];
    send_json(req);

    if (g_term_mode) {
        if (c == 'q') {
            g_term_mode = false;
            draw_code_view();
        }
        return ESP_OK;
    }

    switch (c) {
    case 'w':
        g_current_view = kViewEyesNormal;
        g_current_face = kFaceNormal;
        anim_normal_eyes();
        break;
    case 's':
        g_current_view = kViewEyesSquish;
        g_current_face = kFaceSquish;
        anim_squish_eyes();
        break;
    case 'd':
        mark_manual_animation();
        g_current_view = kViewCode;
        draw_code_view();
        g_term_mode = true;
        term_clear();
        term_full_redraw();
        break;
    case 'a':
        g_current_view = kViewEyesNormal;
        anim_logo_reveal();
        break;
    default:
        break;
    }
    return ESP_OK;
}

esp_err_t route_char(httpd_req_t *req)
{
    if (g_term_mode) {
        const std::string value = query_value(req, "c", 128);
        if (!value.empty()) {
            term_add_char(value[0]);
        }
    }
    send_json(req);
    return ESP_OK;
}

esp_err_t route_speed(httpd_req_t *req)
{
    const std::string value = query_value(req, "v", 64);
    if (!value.empty()) {
        g_anim_speed = std::clamp(std::atoi(value.c_str()), 1, 3);
    }
    send_json(req);
    return ESP_OK;
}

esp_err_t route_face(httpd_req_t *req)
{
    const std::string value = query_value(req, "v", 64);
    if (!value.empty()) {
        const int face = std::clamp(std::atoi(value.c_str()), 0, 4);
        mark_manual_animation();
        g_current_view = (face == kFaceSquish) ? kViewEyesSquish : kViewEyesNormal;
        g_term_mode = false;
        draw_face(static_cast<Face>(face));
    }
    send_json(req);
    return ESP_OK;
}

esp_err_t route_redraw(httpd_req_t *req)
{
    const std::string bg = query_value(req, "bg", 128);
    if (!bg.empty()) {
        g_anim_bg = hex_to_rgb565(bg);
        g_draw_bg = g_anim_bg;
    }
    switch (g_current_view) {
    case kViewEyesNormal:
        draw_face(g_current_face);
        break;
    case kViewEyesSquish:
        draw_face(kFaceSquish);
        break;
    case kViewCode:
        draw_code_view();
        break;
    case kViewDraw:
        g_display.fillScreen(g_draw_bg);
        g_display.flush();
        break;
    }
    send_json(req);
    return ESP_OK;
}

esp_err_t route_canvas(httpd_req_t *req)
{
    const std::string on = query_value(req, "on", 64);
    if (on == "1") {
        g_current_view = kViewDraw;
        g_term_mode = false;
        g_display.fillScreen(g_draw_bg);
        g_display.flush();
    }
    send_json(req);
    return ESP_OK;
}

esp_err_t route_draw_clear(httpd_req_t *req)
{
    const std::string bg = query_value(req, "bg", 128);
    g_draw_bg = hex_to_rgb565(bg.empty() ? "#ff8000" : bg);
    g_anim_bg = g_draw_bg;
    g_current_view = kViewDraw;
    g_term_mode = false;
    g_display.fillScreen(g_draw_bg);
    g_display.flush();
    send_json(req);
    return ESP_OK;
}

esp_err_t route_draw_stroke(httpd_req_t *req)
{
    const std::string pen = query_value(req, "pen", 128);
    const std::string data = query_value(req, "pts", 4096);
    if (pen.empty() || data.empty()) {
        send_json(req);
        return ESP_OK;
    }

    const uint16_t color = hex_to_rgb565(pen);
    g_current_view = kViewDraw;
    int16_t prev_x = -1;
    int16_t prev_y = -1;
    int min_x = g_display.width();
    int min_y = g_display.height();
    int max_x = 0;
    int max_y = 0;
    size_t start = 0;
    while (start < data.size()) {
        const size_t semi = data.find(';', start);
        const std::string entry = data.substr(start, semi == std::string::npos ? std::string::npos : semi - start);
        const size_t comma = entry.find(',');
        if (comma != std::string::npos) {
            const int16_t x = static_cast<int16_t>(std::atoi(entry.substr(0, comma).c_str()));
            const int16_t y = static_cast<int16_t>(std::atoi(entry.substr(comma + 1).c_str()));
            if (prev_x >= 0) {
                g_display.drawLine(prev_x, prev_y, x, y, color);
                g_display.drawLine(prev_x + 1, prev_y, x + 1, y, color);
                g_display.drawLine(prev_x, prev_y + 1, x, y + 1, color);
                min_x = std::min<int>(min_x, std::min(prev_x, x) - 3);
                min_y = std::min<int>(min_y, std::min(prev_y, y) - 3);
                max_x = std::max<int>(max_x, std::max(prev_x, x) + 4);
                max_y = std::max<int>(max_y, std::max(prev_y, y) + 4);
            } else {
                g_display.fillCircle(x, y, 2, color);
                min_x = std::min<int>(min_x, x - 3);
                min_y = std::min<int>(min_y, y - 3);
                max_x = std::max<int>(max_x, x + 4);
                max_y = std::max<int>(max_y, y + 4);
            }
            prev_x = x;
            prev_y = y;
        }
        if (semi == std::string::npos) {
            break;
        }
        start = semi + 1;
    }
    if (min_x <= max_x && min_y <= max_y) {
        g_display.flushRect(min_x, min_y, max_x - min_x + 1, max_y - min_y + 1);
    }
    send_json(req);
    return ESP_OK;
}

esp_err_t route_backlight(httpd_req_t *req)
{
    set_backlight(query_value(req, "on", 64) == "1");
    send_json(req);
    return ESP_OK;
}

esp_err_t route_brightness(httpd_req_t *req)
{
    const std::string value = query_value(req, "v", 64);
    if (!value.empty()) {
        set_brightness(static_cast<uint8_t>(std::atoi(value.c_str())));
    }
    send_json(req);
    return ESP_OK;
}

esp_err_t route_state(httpd_req_t *req)
{
    char json[256];
    std::snprintf(json, sizeof(json),
                  "{\"view\":%u,\"face\":%u,\"busy\":%s,\"term\":%s,\"bl\":%s,\"speed\":%u,\"brightness\":%u,\"w\":%d,\"h\":%d,\"driver\":\"%s\"}",
                  static_cast<unsigned>(g_current_view),
                  static_cast<unsigned>(g_current_face),
                  g_busy ? "true" : "false",
                  g_term_mode ? "true" : "false",
                  g_backlight_on ? "true" : "false",
                  static_cast<unsigned>(g_anim_speed),
                  static_cast<unsigned>(g_backlight_brightness),
                  g_display.width(),
                  g_display.height(),
                  g_display.driverName());
    send_json(req, json);
    return ESP_OK;
}

void register_uri(const char *uri, httpd_method_t method, esp_err_t (*handler)(httpd_req_t *))
{
    httpd_uri_t cfg = {};
    cfg.uri = uri;
    cfg.method = method;
    cfg.handler = handler;
    httpd_register_uri_handler(g_server, &cfg);
}

esp_err_t start_http_server()
{
    httpd_config_t config = HTTPD_DEFAULT_CONFIG();
    config.uri_match_fn = httpd_uri_match_wildcard;
    config.stack_size = 8192;
    config.max_uri_handlers = 16;
    config.lru_purge_enable = true;
    ESP_RETURN_ON_ERROR(httpd_start(&g_server, &config), kTag, "http server start failed");
    register_uri("/", HTTP_GET, route_root);
    register_uri("/cmd", HTTP_GET, route_cmd);
    register_uri("/char", HTTP_GET, route_char);
    register_uri("/speed", HTTP_GET, route_speed);
    register_uri("/face", HTTP_GET, route_face);
    register_uri("/redraw", HTTP_GET, route_redraw);
    register_uri("/canvas", HTTP_GET, route_canvas);
    register_uri("/draw/clear", HTTP_GET, route_draw_clear);
    register_uri("/draw/stroke", HTTP_GET, route_draw_stroke);
    register_uri("/backlight", HTTP_GET, route_backlight);
    register_uri("/brightness", HTTP_GET, route_brightness);
    register_uri("/state", HTTP_GET, route_state);
    return ESP_OK;
}

esp_err_t wifi_init_softap()
{
    ESP_RETURN_ON_ERROR(esp_netif_init(), kTag, "netif init failed");
    ESP_RETURN_ON_ERROR(esp_event_loop_create_default(), kTag, "event loop failed");
    esp_netif_create_default_wifi_ap();

    wifi_init_config_t init_config = WIFI_INIT_CONFIG_DEFAULT();
    ESP_RETURN_ON_ERROR(esp_wifi_init(&init_config), kTag, "wifi init failed");

    wifi_config_t wifi_config = {};
    std::strncpy(reinterpret_cast<char *>(wifi_config.ap.ssid), CONFIG_MOCHI_WIFI_AP_SSID, sizeof(wifi_config.ap.ssid));
    wifi_config.ap.ssid_len = std::strlen(CONFIG_MOCHI_WIFI_AP_SSID);
    std::strncpy(reinterpret_cast<char *>(wifi_config.ap.password), CONFIG_MOCHI_WIFI_AP_PASSWORD, sizeof(wifi_config.ap.password));
    wifi_config.ap.channel = 1;
    wifi_config.ap.max_connection = 4;
    wifi_config.ap.authmode = WIFI_AUTH_WPA_WPA2_PSK;
    if (std::strlen(CONFIG_MOCHI_WIFI_AP_PASSWORD) == 0) {
        wifi_config.ap.authmode = WIFI_AUTH_OPEN;
    }

    ESP_RETURN_ON_ERROR(esp_wifi_set_mode(WIFI_MODE_AP), kTag, "wifi mode failed");
    ESP_RETURN_ON_ERROR(esp_wifi_set_config(WIFI_IF_AP, &wifi_config), kTag, "wifi config failed");
    ESP_RETURN_ON_ERROR(esp_wifi_start(), kTag, "wifi start failed");
    ESP_LOGI(kTag, "softAP started: ssid=%s password=%s ip=192.168.4.1",
             CONFIG_MOCHI_WIFI_AP_SSID, CONFIG_MOCHI_WIFI_AP_PASSWORD);
    return ESP_OK;
}

void init_colours()
{
    g_orange = MochiDisplay::color565(255, 128, 0);
    g_dark_bg = MochiDisplay::color565(10, 12, 16);
    g_muted = MochiDisplay::color565(90, 88, 86);
    g_green = MochiDisplay::color565(80, 220, 130);
    g_anim_bg = g_orange;
    g_draw_bg = g_orange;
}

void draw_lcd_self_test()
{
    const uint16_t red = MochiDisplay::color565(255, 0, 0);
    const uint16_t green = MochiDisplay::color565(0, 255, 0);
    const uint16_t blue = MochiDisplay::color565(0, 0, 255);
    const int band = std::max(1, g_display.width() / 4);
    g_display.fillScreen(kBlack);
    g_display.fillRect(0, 0, band, g_display.height(), red);
    g_display.fillRect(band, 0, band, g_display.height(), green);
    g_display.fillRect(band * 2, 0, band, g_display.height(), blue);
    g_display.fillRect(band * 3, 0, g_display.width() - band * 3, g_display.height(), kWhite);
    g_display.flush();
    delay_ms(900);
}

} // namespace

extern "C" void app_main(void)
{
    esp_err_t nvs_ret = nvs_flash_init();
    if (nvs_ret == ESP_ERR_NVS_NO_FREE_PAGES || nvs_ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        nvs_ret = nvs_flash_init();
    }
    ESP_ERROR_CHECK(nvs_ret);
    ESP_ERROR_CHECK(g_display.init());
    init_colours();
    set_brightness(g_backlight_brightness);
    draw_lcd_self_test();

    g_display.fillScreen(g_anim_bg);
    g_display.setTextColor(kWhite);
    const int splash_size = g_display.width() >= 200 ? 3 : 2;
    g_display.setTextSize(splash_size);
    g_display.setCursor((g_display.width() - 30 * splash_size) / 2, g_display.height() / 2 - 11 * splash_size);
    g_display.print("Clawd");
    g_display.setCursor((g_display.width() - 30 * splash_size) / 2, g_display.height() / 2 + 3 * splash_size);
    g_display.print("Mochi");
    g_display.flush();
    delay_ms(1200);

    anim_logo_reveal();
    ESP_ERROR_CHECK(wifi_init_softap());
    ESP_ERROR_CHECK(start_http_server());
    xTaskCreate(task_idle_face, "idle_face", 4096, nullptr, 4, nullptr);
    draw_wifi_info();

    while (true) {
        delay_ms(1000);
    }
}
