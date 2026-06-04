#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#include "ble_bridge.hpp"
#include "display.hpp"
#include "voice_module.hpp"

#include "esp_check.h"
#include "esp_event.h"
#include "esp_heap_caps.h"
#include "esp_http_server.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_random.h"
#include "esp_timer.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "mdns.h"
#include "nvs.h"
#include "nvs_flash.h"
#include "sdkconfig.h"

#ifndef CONFIG_MOCHI_WIFI_STA_SSID
#define CONFIG_MOCHI_WIFI_STA_SSID ""
#endif

#ifndef CONFIG_MOCHI_WIFI_STA_PASSWORD
#define CONFIG_MOCHI_WIFI_STA_PASSWORD ""
#endif

namespace {

constexpr const char *kTag = "clawd_mochi";
constexpr const char *kMdnsHost = "clawd-mochi";
constexpr const char *kMdnsName = "Clawd Mochi";
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
constexpr uint32_t kDefaultBgRgb = 0xFF8000;
constexpr const char *kNvsNamespace = "mochi";
constexpr uint32_t kAutoSleepTimeoutMs = 10 * 60 * 1000;
constexpr uint8_t kSleepBrightness = 10;
constexpr uint8_t kDefaultBrightness = 80;
constexpr uint16_t kVoiceSayWifiOk = 0xFF81;
constexpr uint16_t kVoiceSayWifiFail = 0xFF82;
constexpr uint16_t kVoiceSayDone = 0xFF83;
constexpr uint16_t kVoiceSayError = 0xFF84;
constexpr uint16_t kVoiceSayReady = 0xFF85;
constexpr uint16_t kVoiceSaySleep = 0xFF86;
constexpr uint16_t kVoiceSayThinking = 0xFF87;
constexpr uint16_t kVoiceSayRunning = 0xFF88;
constexpr uint16_t kVoiceSaySpeaking = 0xFF89;
constexpr uint16_t kVoiceSayNight = 0xFF8A;
constexpr uint16_t kVoiceSayDay = 0xFF8B;
constexpr uint16_t kVoiceSayScreenOff = 0xFF8C;
constexpr uint16_t kVoiceSayScreenOn = 0xFF8D;

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
    kFaceSurprise = 5,
    kFaceWink = 6,
    kFaceLove = 7,
    kFaceLook = 8,
};

enum UiEventType : uint8_t {
    kUiEventWifiRetryLater = 1,
    kUiEventWifiConnected = 2,
    kUiEventBlePet = 3,
};

struct UiEvent {
    UiEventType type;
    BlePetEvent pet;
};

MochiDisplay g_display;
httpd_handle_t g_server = nullptr;
SemaphoreHandle_t g_display_mutex = nullptr;
uint16_t g_orange = 0;
uint16_t g_dark_bg = 0;
uint16_t g_muted = 0;
uint16_t g_green = 0;
uint16_t g_anim_bg = 0;
uint16_t g_draw_bg = 0;
uint32_t g_bg_rgb = kDefaultBgRgb;
View g_current_view = kViewEyesNormal;
Face g_current_face = kFaceNormal;
bool g_busy = false;
bool g_backlight_on = true;
bool g_term_mode = false;
uint8_t g_anim_speed = 1;
uint8_t g_idle_activity = 2;
uint8_t g_backlight_brightness = kDefaultBrightness;
uint8_t g_awake_brightness = kDefaultBrightness;
uint32_t g_manual_anim_until_ms = 0;
uint32_t g_last_activity_ms = 0;
bool g_sleeping = false;
bool g_sta_connected = false;
esp_ip4_addr_t g_sta_ip = {};
std::string g_sta_ssid;
std::string g_sta_password;
bool g_sta_disabled = false;
bool g_sta_manual_pause = false;
uint8_t g_sta_last_disconnect_reason = 0;
uint8_t g_sta_retry_count = 0;
std::string g_term_lines[kTermRows];
uint8_t g_term_row = 0;
uint8_t g_term_col = 0;
QueueHandle_t g_voice_cmd_queue = nullptr;
QueueHandle_t g_ui_event_queue = nullptr;
uint32_t g_last_voice_say_ms = 0;
uint32_t g_last_wifi_ok_feedback_ms = 0;
uint32_t g_last_wifi_fail_feedback_ms = 0;

/**
 * @brief 保护 LCD 帧缓冲和 SPI flush，避免多个任务同时改屏幕。
 */
class DisplayLock {
public:
    DisplayLock()
    {
        if (g_display_mutex != nullptr) {
            taken_ = xSemaphoreTakeRecursive(g_display_mutex, portMAX_DELAY) == pdTRUE;
        }
    }

    ~DisplayLock()
    {
        if (taken_) {
            xSemaphoreGiveRecursive(g_display_mutex);
        }
    }

    DisplayLock(const DisplayLock &) = delete;
    DisplayLock &operator=(const DisplayLock &) = delete;

private:
    bool taken_ = false;
};

constexpr char kIndexHtml[] = R"HTML(
<!doctype html><html lang="zh-CN"><head>
<meta charset="utf-8"><meta name="viewport" content="width=device-width,initial-scale=1,user-scalable=no">
<title>Clawd Mochi</title>
<style>
*{box-sizing:border-box}body{margin:0;min-height:100vh;background:#1d1d20;color:#eee8df;font-family:Courier New,monospace;display:flex;align-items:center;justify-content:flex-start;flex-direction:column;padding:22px 14px 44px;gap:14px}
.title{color:#d65728;font-weight:700;letter-spacing:1px;text-align:center;line-height:1.35}.sub{color:#766c64;font-size:11px;letter-spacing:2px}
.grid{width:100%;max-width:390px;display:grid;grid-template-columns:1fr 1fr;gap:9px}.btn,.wide{border:1px solid #3f3936;background:#262529;color:#eee8df;border-radius:8px;padding:15px 8px;font:700 13px Courier New,monospace}
.btn:active,.wide:active{transform:scale(.96)}.btn.active{border-color:#d65728;background:#25150d}.wide{width:100%;max-width:390px}.row{width:100%;max-width:390px;display:flex;gap:10px;align-items:center;color:#8f867d;font-size:12px}
.row input,.row select{min-width:0;background:#111318;color:#eee8df;border:1px solid #3f3936;border-radius:8px;padding:10px;font:700 13px Courier New,monospace}.row select{flex:1}
input[type=range]{flex:1;accent-color:#d65728}.sw{width:54px;height:38px;border:1px solid #3f3936;border-radius:8px;background:#262529}.canvas{display:none;touch-action:none;background:#ff8000;width:240px;height:240px;border:1px solid #3f3936;image-rendering:pixelated}
.canvas.on{display:block}.term{display:none;width:100%;max-width:390px;grid-template-columns:1fr auto;gap:8px}.term.on{display:grid}.term input{min-width:0;background:#111318;color:#e8e4dc;border:1px solid #3f3936;border-radius:8px;padding:12px;font:700 16px Courier New,monospace}
.note,.status{font-size:11px;color:#746b63;text-align:center;max-width:390px;line-height:1.5}.status{display:none;text-align:left;width:100%;white-space:pre-wrap;background:#111318;border:1px solid #3f3936;border-radius:8px;padding:10px}.status.on{display:block}
</style></head><body>
<div class="title">/\\___/\\<br>(  o o  )<br>Clawd Mochi</div>
<div class="sub">ESP-IDF · ESP32-S3</div>
<div class="grid">
<button class="btn" data-v="0" onclick="cmd('w',0)">普通眼睛</button>
<button class="btn" data-v="1" onclick="cmd('s',1)">眯眯眼</button>
<button class="btn" data-v="2" onclick="cmd('d',2);openTerm()">Claude Code</button>
<button class="btn" data-v="3" onclick="openCanvas()">画板</button>
</div>
<div class="grid">
<button class="btn" onclick="face(2)">开心</button>
<button class="btn" onclick="face(3)">困困</button>
<button class="btn" onclick="face(4)">生气</button>
<button class="btn" onclick="face(0)">恢复表情</button>
</div>
<div class="grid">
<button class="btn" onclick="face(5)">惊讶</button>
<button class="btn" onclick="face(6)">眨单眼</button>
<button class="btn" onclick="face(7)">爱心眼</button>
<button class="btn" onclick="face(8)">斜眼</button>
</div>
<div class="row"><span>速度</span><input id="spd" type="range" min="1" max="3" value="1" oninput="speed(this.value)"><span id="sv">慢</span></div>
<div class="row"><span>活跃度</span><input id="act" type="range" min="1" max="3" value="2" oninput="activity(this.value)"><span id="av">普通</span></div>
<div class="row"><span>亮度</span><input id="br" type="range" min="5" max="100" value="80" oninput="brightness(this.value)"><span id="bv">80%</span></div>
<div class="row"><span>背景</span><input class="sw" id="bg" type="color" value="#ff8000" oninput="redraw()"><span>画笔</span><input class="sw" id="pen" type="color" value="#000000"></div>
<div class="row"><span>粗细</span><input id="psz" type="range" min="1" max="8" value="3"><span id="pv">3</span></div>
<button id="bl" class="wide" onclick="backlight()">打开屏幕</button>
<div class="row"><span>WiFi</span><select id="nets"><option value="">点击扫描</option></select><button class="btn" onclick="scanWifi()">扫描</button></div>
<div class="row"><span>密码</span><input id="wpwd" type="password" placeholder="WiFi password"><button class="btn" onclick="connectWifi()">连接</button></div>
<div id="wifiMsg" class="status"></div>
<button class="wide" onclick="forgetWifi()">临时断开WiFi</button>
<button class="wide" onclick="reconnectWifi()">重连已保存WiFi</button>
<div class="grid">
<button class="btn" onclick="randomFace()">随机表情</button>
<button class="btn" onclick="randomColor()">随机颜色</button>
<button class="btn" onclick="night()">夜间模式</button>
<button class="btn" onclick="day()">日间模式</button>
<button class="btn" onclick="statusView()">状态</button>
<button class="btn" onclick="openCanvas()">开始画画</button>
<button class="btn" onclick="factoryReset()">恢复出厂</button>
<button class="btn" onclick="refresh()">刷新状态</button>
</div>
<canvas id="cv" class="canvas" width="240" height="240"></canvas>
<div class="grid">
<button class="btn" onclick="erase()">橡皮擦</button>
<button class="btn" onclick="pen()">黑色画笔</button>
<button class="btn" onclick="clearCanvas()">清空画板</button>
<button class="btn" onclick="openCanvas()">继续画</button>
</div>
<div id="term" class="term"><input id="tin" maxlength="1" autocomplete="off"><button class="wide" onclick="sendChar()">发送</button></div>
<button id="done" class="wide" style="display:none" onclick="closeCanvas()">完成</button>
<div id="stat" class="status"></div>
<div class="note">连接热点 ClaWD-Mochi，密码 clawd1234，打开 192.168.4.1 控制桌面小屏。</div>
<script>
let bl=true, drawing=false, pts=[], lcdW=240, lcdH=240; const cv=document.getElementById('cv'), ctx=cv.getContext('2d');
const labels={1:'慢',2:'正常',3:'快'}, alabels={1:'安静',2:'普通',3:'活泼'};
function req(u){return fetch(u,{cache:'no-store'}).catch(()=>{});}
function active(v){document.querySelectorAll('.btn').forEach(b=>b.classList.toggle('active',b.dataset.v==v));}
function cmd(k,v){closeCanvas(false);req('/cmd?k='+k);active(v)}
function face(v){closeCanvas(false);req('/face?v='+v);active(v===1?1:0)}
function speed(v){document.getElementById('sv').textContent=labels[v];req('/speed?v='+v)}
function activity(v){document.getElementById('av').textContent=alabels[v];req('/activity?v='+v)}
function brightness(v){document.getElementById('bv').textContent=v+'%';bl=true;document.getElementById('bl').textContent='打开屏幕';req('/brightness?v='+v)}
function randomFace(){closeCanvas(false);req('/random?what=face').then(()=>refresh())}
function randomColor(){closeCanvas(false);req('/random?what=color').then(()=>refresh())}
function night(){closeCanvas(false);req('/night').then(()=>refresh())}
function day(){closeCanvas(false);req('/day').then(()=>refresh())}
function factoryReset(){closeCanvas(false);if(confirm('恢复默认设置？'))req('/factory').then(()=>refresh())}
function statusView(){fetch('/state',{cache:'no-store'}).then(r=>r.json()).then(j=>{const s=document.getElementById('stat');s.classList.toggle('on');s.textContent='驱动: '+j.driver+'\\n尺寸: '+j.w+'x'+j.h+'\\n表情: '+j.face+'\\n背景: '+j.bg+'\\n速度: '+labels[j.speed||1]+'\\n活跃度: '+alabels[j.activity||2]+'\\n亮度: '+j.brightness+'%\\n蓝牙桥接: '+(j.ble_ready?'已开启':'未开启')+'\\n语音模块: '+(j.voice_ready?'已连接':(j.voice_enabled?'等待串口':'未启用'))+'\\n语音命令: '+(j.voice_code||'0x0000')+'\\nSTA: '+(j.sta?j.sta:'未连接')+'\\nLAN: '+(j.ip||'-')+'\\nWiFi状态: '+(j.reason||'正常')+'\\n睡眠: '+(j.sleep?'是':'否')+'\\n运行: '+j.uptime+' 秒\\n剩余内存: '+j.heap+' bytes'})}
function refresh(){fetch('/state',{cache:'no-store'}).then(applyState)}
function scanWifi(){const n=document.getElementById('nets');n.innerHTML='<option>扫描中...</option>';fetch('/wifi/scan',{cache:'no-store'}).then(r=>r.json()).then(j=>{n.innerHTML='';(j.nets||[]).forEach(x=>{const o=document.createElement('option');o.value=x.ssid;o.textContent=x.ssid+' ('+x.rssi+'dBm)';n.appendChild(o)});if(!n.options.length)n.innerHTML='<option value="">没扫到</option>'})}
function msg(t){const s=document.getElementById('wifiMsg');s.classList.add('on');s.innerHTML=t}
function connectWifi(){const ssid=document.getElementById('nets').value,pwd=document.getElementById('wpwd').value;if(!ssid){alert('先选择 WiFi');return}msg('正在连接 '+ssid+' ...');fetch('/wifi/connect?ssid='+encodeURIComponent(ssid)+'&pwd='+encodeURIComponent(pwd),{cache:'no-store'}).then(r=>r.json()).then(j=>{if(j.connected){msg('连接成功！<br>切换到目标WiFi后请跳转到：<a style=\"color:#d65728\" href=\"'+j.url+'\">'+j.url+'</a><br>备用 IP：<a style=\"color:#d65728\" href=\"http://'+j.ip+'\">http://'+j.ip+'</a><br>桥接 host：'+j.host)}else{msg('连接失败：'+(j.reason||'未知错误')+'<br>请检查密码或距离路由器远近。')}refresh()}).catch(()=>msg('连接请求失败，请重新打开页面再试。'))}
function forgetWifi(){if(confirm('临时断开当前 WiFi？保存的密码不会删除。'))req('/wifi/forget').then(()=>{msg('已临时断开，保存的 WiFi 没删除。<br>如果当前页面失去连接，请切换到热点 ClaWD-Mochi，打开：<a style=\"color:#d65728\" href=\"http://192.168.4.1\">http://192.168.4.1</a><br>部分设备也可继续用：<a style=\"color:#d65728\" href=\"http://clawd-mochi.local\">http://clawd-mochi.local</a>');refresh()})}
function reconnectWifi(){msg('正在重连已保存 WiFi ...');fetch('/wifi/connect?saved=1',{cache:'no-store'}).then(r=>r.json()).then(j=>{if(j.connected){msg('重连成功！<br>请跳转到：<a style=\"color:#d65728\" href=\"'+j.url+'\">'+j.url+'</a><br>备用 IP：<a style=\"color:#d65728\" href=\"http://'+j.ip+'\">http://'+j.ip+'</a>')}else{msg('重连失败：'+(j.reason||'没有保存的 WiFi')+'<br>可以连接热点 ClaWD-Mochi 后重新配网，系统也会每 10 秒后台重试。')}refresh()}).catch(()=>msg('重连请求失败，请重新打开页面再试。'))}
function setCanvasSize(w,h){lcdW=w;lcdH=h;cv.width=w;cv.height=h;cv.style.width=Math.min(300,w*1.6)+'px';cv.style.height=Math.min(300,h*1.6)+'px'}
function paintCanvasOnly(){const bg=document.getElementById('bg').value;ctx.fillStyle=bg;ctx.fillRect(0,0,lcdW,lcdH)}
function redraw(){paintCanvasOnly();req('/redraw?bg='+encodeURIComponent(document.getElementById('bg').value))}
function backlight(){bl=!bl;document.getElementById('bl').textContent=bl?'打开屏幕':'关闭屏幕';req('/backlight?on='+(bl?1:0))}
function openTerm(){document.getElementById('term').classList.add('on');document.getElementById('tin').focus()}
function sendChar(){const i=document.getElementById('tin'); if(i.value){req('/char?c='+encodeURIComponent(i.value));i.value='';i.focus()}}
function openCanvas(){document.getElementById('term').classList.remove('on');cv.classList.add('on');document.getElementById('done').style.display='block';active(3);clearCanvas();req('/canvas?on=1')}
function closeCanvas(send=true){cv.classList.remove('on');document.getElementById('done').style.display='none';if(send)req('/cmd?k=w')}
function erase(){document.getElementById('pen').value=document.getElementById('bg').value}
function pen(){document.getElementById('pen').value='#000000'}
function clearCanvas(){const bg=document.getElementById('bg').value;ctx.fillStyle=bg;ctx.fillRect(0,0,lcdW,lcdH);req('/draw/clear?bg='+encodeURIComponent(bg))}
function pos(e){const r=cv.getBoundingClientRect();return [Math.round((e.clientX-r.left)*lcdW/r.width),Math.round((e.clientY-r.top)*lcdH/r.height)]}
function flush(keep=false){if(pts.length<1)return;const last=pts[pts.length-1];req('/draw/stroke?pen='+encodeURIComponent(document.getElementById('pen').value)+'&size='+document.getElementById('psz').value+'&pts='+encodeURIComponent(pts.map(p=>p[0]+','+p[1]).join(';')));pts=keep?[last]:[]}
function down(e){e.preventDefault();cv.setPointerCapture&&cv.setPointerCapture(e.pointerId);drawing=true;pts=[pos(e)];flush(true)}
function move(e){if(!drawing)return;e.preventDefault();const p=pos(e), q=pts[pts.length-1]||p;ctx.strokeStyle=document.getElementById('pen').value;ctx.lineWidth=document.getElementById('psz').value;ctx.lineCap='round';ctx.beginPath();ctx.moveTo(q[0],q[1]);ctx.lineTo(p[0],p[1]);ctx.stroke();pts.push(p);if(pts.length>6)flush(true)}
function up(){drawing=false;flush(false)}
document.getElementById('psz').addEventListener('input',e=>document.getElementById('pv').textContent=e.target.value);
function pe(e){return e.touches?{clientX:e.touches[0].clientX,clientY:e.touches[0].clientY,pointerId:1,preventDefault:()=>e.preventDefault()}:e}
if(window.PointerEvent){cv.addEventListener('pointerdown',down,{passive:false});cv.addEventListener('pointermove',move,{passive:false});cv.addEventListener('pointerup',up);cv.addEventListener('pointercancel',up);cv.addEventListener('pointerleave',up)}else{cv.addEventListener('touchstart',e=>down(pe(e)),{passive:false});cv.addEventListener('touchmove',e=>move(pe(e)),{passive:false});cv.addEventListener('touchend',up);cv.addEventListener('mousedown',down,{passive:false});cv.addEventListener('mousemove',move,{passive:false});cv.addEventListener('mouseup',up)}
window.addEventListener('keydown',e=>{if(document.activeElement.id==='tin')return; if(e.key==='w')cmd('w',0); if(e.key==='s')cmd('s',1); if(e.key==='d'){cmd('d',2);openTerm()}});
function applyState(j){setCanvasSize(j.w||240,j.h||240);bl=j.bl!==false;document.getElementById('spd').value=j.speed||1;document.getElementById('sv').textContent=labels[j.speed||1];document.getElementById('act').value=j.activity||2;document.getElementById('av').textContent=alabels[j.activity||2];document.getElementById('br').value=j.brightness||80;document.getElementById('bv').textContent=(j.brightness||80)+'%';if(j.bg)document.getElementById('bg').value=j.bg;document.getElementById('bl').textContent=bl?'打开屏幕':'关闭屏幕';if(j.sta){const n=document.getElementById('nets');if(!n.value){n.innerHTML='<option value=\"'+j.sta+'\">'+j.sta+'</option>'}}active(j.view||0);paintCanvasOnly()}
fetch('/state').then(r=>r.json()).then(applyState).catch(()=>{paintCanvasOnly()});
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

uint16_t rgb888_to_rgb565(uint32_t rgb)
{
    return MochiDisplay::color565((rgb >> 16) & 0xFF, (rgb >> 8) & 0xFF, rgb & 0xFF);
}

std::string rgb888_to_hex(uint32_t rgb)
{
    char text[8] = {};
    std::snprintf(text, sizeof(text), "#%06X", static_cast<unsigned>(rgb & 0xFFFFFF));
    return text;
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

uint32_t hex_to_rgb888(std::string hex, uint32_t fallback = kDefaultBgRgb)
{
    if (!hex.empty() && hex[0] == '#') {
        hex.erase(0, 1);
    }
    if (hex.size() != 6) {
        return fallback;
    }
    char *end = nullptr;
    const long value = std::strtol(hex.c_str(), &end, 16);
    if (end == hex.c_str()) {
        return fallback;
    }
    return static_cast<uint32_t>(value) & 0xFFFFFF;
}

void save_settings()
{
    nvs_handle_t handle = 0;
    esp_err_t err = nvs_open(kNvsNamespace, NVS_READWRITE, &handle);
    if (err != ESP_OK) {
        ESP_LOGW(kTag, "nvs open for save failed: %s", esp_err_to_name(err));
        return;
    }
    nvs_set_u32(handle, "bg", g_bg_rgb);
    nvs_set_u8(handle, "bright", g_awake_brightness);
    if (!g_sta_ssid.empty()) {
        nvs_set_str(handle, "sta_ssid", g_sta_ssid.c_str());
        nvs_set_str(handle, "sta_pwd", g_sta_password.c_str());
    }
    nvs_commit(handle);
    nvs_close(handle);
}

void load_settings()
{
    nvs_handle_t handle = 0;
    esp_err_t err = nvs_open(kNvsNamespace, NVS_READONLY, &handle);
    if (err != ESP_OK) {
        return;
    }
    uint32_t bg = kDefaultBgRgb;
    uint8_t bright = kDefaultBrightness;
    uint8_t sta_disabled = 0;
    char sta_ssid[33] = {};
    char sta_pwd[65] = {};
    size_t sta_ssid_len = sizeof(sta_ssid);
    size_t sta_pwd_len = sizeof(sta_pwd);
    nvs_get_u32(handle, "bg", &bg);
    nvs_get_u8(handle, "bright", &bright);
    nvs_get_u8(handle, "sta_disabled", &sta_disabled);
    nvs_get_str(handle, "sta_ssid", sta_ssid, &sta_ssid_len);
    nvs_get_str(handle, "sta_pwd", sta_pwd, &sta_pwd_len);
    nvs_close(handle);

    g_bg_rgb = bg & 0xFFFFFF;
    g_anim_bg = rgb888_to_rgb565(g_bg_rgb);
    g_draw_bg = g_anim_bg;
    g_current_face = kFaceNormal;
    g_current_view = kViewEyesNormal;
    g_anim_speed = 2;
    g_idle_activity = 2;
    g_backlight_brightness = std::clamp<uint8_t>(bright, 5, 100);
    g_awake_brightness = g_backlight_brightness;
    g_sta_ssid = sta_ssid;
    g_sta_password = sta_pwd;
    g_sta_disabled = sta_disabled != 0;
}

void set_brightness(uint8_t percent);
void set_background_rgb(uint32_t rgb);
void draw_sleepy_eyes(uint8_t z_phase);
void anim_wake_up();
std::string sta_ip_text();
void task_restore_normal_face_once(void *);
void handle_pet_status(const std::string &mood, const std::string &text);

void apply_default_settings()
{
    set_background_rgb(kDefaultBgRgb);
    g_current_face = kFaceNormal;
    g_current_view = kViewEyesNormal;
    g_anim_speed = 2;
    g_idle_activity = 2;
    g_awake_brightness = kDefaultBrightness;
    set_brightness(kDefaultBrightness);
    g_term_mode = false;
    g_sleeping = false;
}

void erase_settings()
{
    nvs_handle_t handle = 0;
    if (nvs_open(kNvsNamespace, NVS_READWRITE, &handle) == ESP_OK) {
        nvs_erase_all(handle);
        nvs_commit(handle);
        nvs_close(handle);
    }
}

void save_sta_credentials(const std::string &ssid, const std::string &password)
{
    g_sta_ssid = ssid.substr(0, 32);
    g_sta_password = password.substr(0, 64);
    g_sta_disabled = false;
    g_sta_manual_pause = false;
    g_sta_retry_count = 0;
    nvs_handle_t handle = 0;
    if (nvs_open(kNvsNamespace, NVS_READWRITE, &handle) == ESP_OK) {
        nvs_set_str(handle, "sta_ssid", g_sta_ssid.c_str());
        nvs_set_str(handle, "sta_pwd", g_sta_password.c_str());
        nvs_set_u8(handle, "sta_disabled", 0);
        nvs_commit(handle);
        nvs_close(handle);
    }
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

std::string query_value(httpd_req_t *req, const char *key, size_t max_value_len = 4096)
{
    constexpr size_t kMaxQueryLen = 4096;
    const size_t query_len = httpd_req_get_url_query_len(req) + 1;
    if (query_len <= 1 || query_len > kMaxQueryLen) {
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
    std::string decoded = url_decode(value);
    if (decoded.size() > max_value_len) {
        decoded.resize(max_value_len);
    }
    return decoded;
}

void send_json(httpd_req_t *req, const char *json = "{\"ok\":1}")
{
    httpd_resp_set_type(req, "application/json");
    httpd_resp_sendstr(req, json);
}

void set_backlight(bool on)
{
    DisplayLock lock;
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
    DisplayLock lock;
    g_backlight_brightness = std::clamp<uint8_t>(percent, 5, 100);
    if (!g_sleeping) {
        g_awake_brightness = g_backlight_brightness;
    }
    g_backlight_on = true;
    g_display.setBacklightBrightness(g_backlight_brightness);
}

/**
 * @brief 进入桌宠休眠状态：降低背光并绘制闭眼睡眠表情。
 */
void enter_sleep_state()
{
    DisplayLock lock;
    if (!g_sleeping && g_backlight_brightness != kSleepBrightness) {
        g_awake_brightness = g_backlight_brightness;
    }
    g_sleeping = true;
    g_current_face = kFaceSleepy;
    g_current_view = kViewEyesNormal;
    g_term_mode = false;
    g_backlight_on = true;
    g_display.setBacklight(true);
    g_display.setBacklightBrightness(kSleepBrightness);
    g_backlight_brightness = kSleepBrightness;
    draw_sleepy_eyes(0);
}

/**
 * @brief 如果当前处于休眠，恢复背光并播放醒来动画。
 */
void wake_from_sleep_if_needed()
{
    DisplayLock lock;
    if (!g_sleeping) {
        return;
    }
    g_sleeping = false;
    g_backlight_on = true;
    g_display.setBacklight(true);
    g_display.setBacklightBrightness(g_awake_brightness);
    g_backlight_brightness = g_awake_brightness;
    g_current_view = kViewEyesNormal;
    g_term_mode = false;
    anim_wake_up();
}

void note_activity()
{
    g_last_activity_ms = tick_ms();
    wake_from_sleep_if_needed();
}

void set_background_rgb(uint32_t rgb)
{
    DisplayLock lock;
    g_bg_rgb = rgb & 0xFFFFFF;
    g_anim_bg = rgb888_to_rgb565(g_bg_rgb);
    g_draw_bg = g_anim_bg;
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

uint16_t face_accent_color(uint32_t rgb)
{
    return rgb888_to_rgb565(rgb);
}

void draw_thick_line(int x0, int y0, int x1, int y1, int radius, uint16_t color)
{
    const int r = std::max(0, radius);
    for (int off = -r; off <= r; ++off) {
        g_display.drawLine(x0 + off, y0, x1 + off, y1, color);
        g_display.drawLine(x0, y0 + off, x1, y1 + off, color);
    }
}

void draw_text_tag(const char *text, int y_offset = 0)
{
    if (g_display.height() < 135 || text == nullptr || text[0] == '\0') {
        return;
    }
    const int size = g_display.width() >= 200 ? 2 : 1;
    const int len = static_cast<int>(std::strlen(text));
    const int x = std::max(2, (g_display.width() - len * 6 * size) / 2);
    const int y = std::min(g_display.height() - 10 * size - 2, g_display.height() - scale_design(36) + y_offset);
    g_display.setTextColor(kBlack);
    g_display.setTextSize(size);
    g_display.setCursor(x, y);
    g_display.print(text);
}

void draw_blush(int cx, int cy)
{
    const uint16_t blush = face_accent_color(0xE45858);
    const int r = std::max(1, scale_design(3));
    const int gap = std::max(3, scale_design(8));
    g_display.fillCircle(cx - gap, cy, r, blush);
    g_display.fillCircle(cx, cy + std::max(1, scale_design(2)), r, blush);
    g_display.fillCircle(cx + gap, cy, r, blush);
}

void draw_small_heart(int cx, int cy, int size, uint16_t color)
{
    const int s = std::max(3, size);
    g_display.fillCircle(cx - s / 2, cy - s / 3, std::max(1, s / 2), color);
    g_display.fillCircle(cx + s / 2, cy - s / 3, std::max(1, s / 2), color);
    g_display.fillTriangle(cx - s, cy - s / 5, cx + s, cy - s / 5, cx, cy + s, color);
}

void draw_sparkle(int cx, int cy, int size, uint16_t color)
{
    const int s = std::max(3, size);
    draw_thick_line(cx, cy - s, cx, cy + s, 0, color);
    draw_thick_line(cx - s, cy, cx + s, cy, 0, color);
    draw_thick_line(cx - s / 2, cy - s / 2, cx + s / 2, cy + s / 2, 0, color);
    draw_thick_line(cx + s / 2, cy - s / 2, cx - s / 2, cy + s / 2, 0, color);
}

void draw_sweat_drop(int cx, int cy, int size)
{
    const uint16_t blue = face_accent_color(0x4DB3FF);
    const int s = std::max(4, size);
    g_display.fillCircle(cx, cy + s / 3, s / 2, blue);
    g_display.fillTriangle(cx, cy - s, cx - s / 2, cy + s / 3, cx + s / 2, cy + s / 3, blue);
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
        const int mouth_y = eye_cy() + scale_design(44);
        const int mouth_x = g_display.width() / 2;
        draw_thick_line(mouth_x - scale_design(9), mouth_y, mouth_x - scale_design(3), mouth_y + scale_design(5), scale_design(1), kBlack);
        draw_thick_line(mouth_x + scale_design(3), mouth_y + scale_design(5), mouth_x + scale_design(9), mouth_y, scale_design(1), kBlack);
    } else {
        g_display.fillRect(lx, ey + eye_h / 2 - 2, eye_w, 4, kBlack);
        g_display.fillRect(rx, ey + eye_h / 2 - 2, eye_w, 4, kBlack);
    }
    g_display.flush();
}

void draw_waking_eyes(int eye_h)
{
    g_display.fillScreen(g_anim_bg);
    const int16_t lx = eye_lx(0);
    const int16_t rx = eye_rx(0);
    const int16_t cy = eye_cy();
    const int eye_w = scale_design(kEyeWDesign);
    const int h = std::clamp(eye_h, std::max(3, scale_design(4)), scale_design(kEyeHDesign));
    const int16_t y = cy - h / 2;
    g_display.fillRect(lx, y, eye_w, h, kBlack);
    g_display.fillRect(rx, y, eye_w, h, kBlack);
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
        draw_blush(lx + eye_w / 2 - scale_design(22), cy + scale_design(22));
        draw_blush(rx + eye_w / 2 + scale_design(22), cy + scale_design(22));
    } else {
        g_display.fillRect(lx, cy - 3, eye_w, 6, kBlack);
        g_display.fillRect(rx, cy - 3, eye_w, 6, kBlack);
    }
    g_display.flush();
}

void draw_sleepy_eyes(uint8_t z_phase = 0)
{
    g_display.fillScreen(g_anim_bg);
    const int16_t lx = eye_lx(0);
    const int16_t rx = eye_rx(0);
    const int16_t cy = eye_cy();
    const int eye_w = scale_design(kEyeWDesign);
    const int thk = std::max(3, scale_design(7));
    draw_thick_line(lx, cy, lx + eye_w / 2, cy + scale_design(5), thk / 2, kBlack);
    draw_thick_line(lx + eye_w / 2, cy + scale_design(5), lx + eye_w, cy, thk / 2, kBlack);
    draw_thick_line(rx, cy, rx + eye_w / 2, cy + scale_design(5), thk / 2, kBlack);
    draw_thick_line(rx + eye_w / 2, cy + scale_design(5), rx + eye_w, cy, thk / 2, kBlack);
    if (z_phase > 0) {
        g_display.setTextColor(kBlack);
        g_display.setTextSize(g_display.width() >= 180 ? 2 : 1);
        const int16_t base_x = g_display.width() - scale_design(62);
        const int16_t base_y = scale_design(68);
        for (uint8_t i = 0; i < z_phase; ++i) {
            g_display.setCursor(base_x + scale_design(i * 13), base_y - scale_design(i * 13));
            g_display.print("Z");
        }
    }
    draw_text_tag("SLEEP");
    g_display.flush();
}

void draw_happy_eyes(int16_t bounce = 0)
{
    g_display.fillScreen(g_anim_bg);
    const int16_t lx = eye_lx(0);
    const int16_t rx = eye_rx(0);
    const int16_t cy = eye_cy() + scale_design(4) + bounce;
    const int eye_w = scale_design(kEyeWDesign);
    const int arm = scale_design(18);
    const int thk = std::max(3, scale_design(7));
    draw_thick_line(lx, cy, lx + eye_w / 2, cy - arm, thk / 2, kBlack);
    draw_thick_line(lx + eye_w / 2, cy - arm, lx + eye_w, cy, thk / 2, kBlack);
    draw_thick_line(rx, cy, rx + eye_w / 2, cy - arm, thk / 2, kBlack);
    draw_thick_line(rx + eye_w / 2, cy - arm, rx + eye_w, cy, thk / 2, kBlack);
    draw_blush(lx + eye_w / 2 - scale_design(26), cy + scale_design(20));
    draw_blush(rx + eye_w / 2 + scale_design(26), cy + scale_design(20));
    draw_sparkle(rx + eye_w + scale_design(24), cy - scale_design(30), scale_design(8), face_accent_color(0xFFE066));
    draw_text_tag("HAPPY");
    g_display.flush();
}

void draw_angry_eyes(int16_t jitter = 0)
{
    g_display.fillScreen(g_anim_bg);
    const int16_t lx = eye_lx(jitter);
    const int16_t rx = eye_rx(jitter);
    const int16_t ey = eye_y() + scale_design(10);
    const int eye_w = scale_design(kEyeWDesign);
    const int eye_h = std::max(6, scale_design(14));
    for (int i = 0; i < eye_h; ++i) {
        g_display.drawLine(lx, ey + i, lx + eye_w, ey + scale_design(18) + i, kBlack);
        g_display.drawLine(rx, ey + scale_design(18) + i, rx + eye_w, ey + i, kBlack);
    }
    draw_thick_line(lx - scale_design(8), ey - scale_design(18), lx + eye_w + scale_design(8), ey - scale_design(8), scale_design(3), kBlack);
    draw_thick_line(rx - scale_design(8), ey - scale_design(8), rx + eye_w + scale_design(8), ey - scale_design(18), scale_design(3), kBlack);
    const uint16_t red = face_accent_color(0xF04A32);
    draw_thick_line(g_display.width() - scale_design(40), scale_design(42), g_display.width() - scale_design(22), scale_design(24), scale_design(1), red);
    draw_thick_line(g_display.width() - scale_design(39), scale_design(24), g_display.width() - scale_design(23), scale_design(42), scale_design(1), red);
    draw_text_tag("ERROR");
    g_display.flush();
}

void draw_surprise_eyes(int radius_delta = 0)
{
    g_display.fillScreen(g_anim_bg);
    const int radius = std::max(4, scale_design(15) + radius_delta);
    const int lcx = eye_lx(0) + scale_design(kEyeWDesign) / 2;
    const int rcx = eye_rx(0) + scale_design(kEyeWDesign) / 2;
    const int cy = eye_cy();
    g_display.fillCircle(lcx, cy, radius, kBlack);
    g_display.fillCircle(rcx, cy, radius, kBlack);
    g_display.fillCircle(lcx + radius / 3, cy - radius / 3, std::max(1, radius / 4), g_anim_bg);
    g_display.fillCircle(rcx + radius / 3, cy - radius / 3, std::max(1, radius / 4), g_anim_bg);
    g_display.fillCircle(g_display.width() / 2, g_display.height() - scale_design(38), std::max(2, scale_design(7)), kBlack);
    draw_sweat_drop(rcx + scale_design(34), cy - scale_design(30), scale_design(10));
    draw_text_tag("WOW");
    g_display.flush();
}

void draw_wink_eyes()
{
    g_display.fillScreen(g_anim_bg);
    const int16_t lx = eye_lx(0);
    const int16_t rx = eye_rx(0);
    const int16_t ey = eye_y();
    const int eye_w = scale_design(kEyeWDesign);
    const int eye_h = scale_design(kEyeHDesign);
    g_display.fillRect(lx, ey, eye_w, eye_h, kBlack);
    const int cy = eye_cy();
    draw_thick_line(rx, cy, rx + eye_w / 2, cy + scale_design(6), scale_design(3), kBlack);
    draw_thick_line(rx + eye_w / 2, cy + scale_design(6), rx + eye_w, cy, scale_design(3), kBlack);
    draw_small_heart(rx + eye_w + scale_design(20), cy - scale_design(24), scale_design(7), face_accent_color(0xF0445E));
    draw_blush(lx - scale_design(18), cy + scale_design(18));
    draw_text_tag("HI");
    g_display.flush();
}

void draw_love_eyes(int pulse = 0)
{
    g_display.fillScreen(g_anim_bg);
    const int size = std::max(10, scale_design(21) + pulse);
    const uint16_t heart_col = face_accent_color(0xE91E63);
    auto heart = [&](int cx, int cy) {
        g_display.fillCircle(cx - size / 2, cy - size / 3, size / 2, heart_col);
        g_display.fillCircle(cx + size / 2, cy - size / 3, size / 2, heart_col);
        g_display.fillTriangle(cx - size, cy - size / 4, cx + size, cy - size / 4, cx, cy + size, heart_col);
    };
    heart(eye_lx(0) + scale_design(kEyeWDesign) / 2, eye_cy());
    heart(eye_rx(0) + scale_design(kEyeWDesign) / 2, eye_cy());
    draw_small_heart(g_display.width() / 2, eye_cy() + scale_design(38), scale_design(8) + pulse / 3, heart_col);
    draw_text_tag("LOVE");
    g_display.flush();
}

void draw_side_eye(int16_t glance = 0)
{
    g_display.fillScreen(g_anim_bg);
    const int16_t lx = eye_lx(-scale_design(8) + glance);
    const int16_t rx = eye_rx(-scale_design(8) + glance);
    const int16_t ey = eye_y() + scale_design(12);
    const int eye_w = scale_design(kEyeWDesign);
    const int eye_h = std::max(8, scale_design(18));
    g_display.fillRect(lx, ey, eye_w + scale_design(12), eye_h, kBlack);
    g_display.fillRect(rx, ey, eye_w + scale_design(12), eye_h, kBlack);
    const int pupil = std::max(2, scale_design(5));
    g_display.fillRect(lx + eye_w - scale_design(4), ey, pupil, eye_h, g_anim_bg);
    g_display.fillRect(rx + eye_w - scale_design(4), ey, pupil, eye_h, g_anim_bg);
    g_display.setTextColor(kBlack);
    g_display.setTextSize(g_display.width() >= 200 ? 2 : 1);
    g_display.setCursor(g_display.width() / 2 - scale_design(14), ey + eye_h + scale_design(14));
    g_display.print("...");
    draw_text_tag("WORK", scale_design(8));
    g_display.flush();
}

void draw_face(Face face, int16_t ox = 0, bool blink = false)
{
    DisplayLock lock;
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
    case kFaceSurprise:
        draw_surprise_eyes();
        break;
    case kFaceWink:
        draw_wink_eyes();
        break;
    case kFaceLove:
        draw_love_eyes();
        break;
    case kFaceLook:
        draw_side_eye();
        break;
    case kFaceNormal:
    default:
        draw_normal_eyes(ox, blink);
        break;
    }
}

Face mood_to_face(const std::string &mood)
{
    if (mood == "happy" || mood == "done" || mood == "ok") {
        return kFaceHappy;
    }
    if (mood == "angry" || mood == "error" || mood == "fail") {
        return kFaceAngry;
    }
    if (mood == "sleepy" || mood == "idle") {
        return kFaceSleepy;
    }
    if (mood == "surprise" || mood == "warn") {
        return kFaceSurprise;
    }
    if (mood == "love") {
        return kFaceLove;
    }
    if (mood == "thinking" || mood == "look") {
        return kFaceLook;
    }
    if (mood == "wink") {
        return kFaceWink;
    }
    return kFaceNormal;
}

std::string lcd_ascii_text(const std::string &text, size_t max_len = 24)
{
    std::string out;
    out.reserve(max_len);
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

std::string json_escape_utf8(const std::string &text, size_t max_len = 128)
{
    std::string out;
    out.reserve(std::min(text.size(), max_len));
    for (unsigned char ch : text) {
        if (out.size() >= max_len) {
            break;
        }
        if (ch == '\\' || ch == '"') {
            out.push_back('\\');
            out.push_back(static_cast<char>(ch));
        } else if (ch == '\n') {
            out += "\\n";
        } else if (ch == '\r') {
            out += "\\r";
        } else if (ch == '\t') {
            out += "\\t";
        } else if (ch < 32) {
            char escaped[7] = {};
            std::snprintf(escaped, sizeof(escaped), "\\u%04X", static_cast<unsigned>(ch));
            out += escaped;
        } else {
            out.push_back(static_cast<char>(ch));
        }
    }
    return out;
}

void draw_pet_notice(Face face, const std::string &text)
{
    DisplayLock lock;
    g_current_view = kViewEyesNormal;
    g_term_mode = false;
    draw_face(face);

    const std::string line = lcd_ascii_text(text.empty() ? "Mochi online" : text);
    if (!line.empty()) {
        const int text_size = g_display.width() >= 180 ? 2 : 1;
        const int strip_h = std::max(24, 14 * text_size);
        const int y = g_display.height() - strip_h;
        g_display.fillRect(0, y, g_display.width(), strip_h, g_dark_bg);
        g_display.drawFastHLine(0, y, g_display.width(), g_orange);
        g_display.setTextColor(kWhite);
        g_display.setTextSize(text_size);
        g_display.setCursor(6, y + 7);
        g_display.print(line);
        g_display.flush();
    }
}

void schedule_restore_normal()
{
    g_busy = true;
    if (xTaskCreate(task_restore_normal_face_once, "restore_face", 3072, nullptr, 4, nullptr) != pdPASS) {
        draw_face(kFaceNormal);
        g_busy = false;
    }
}

void draw_voice_notice(Face face, const std::string &text)
{
    note_activity();
    mark_manual_animation();
    g_busy = true;
    draw_pet_notice(face, text);
    schedule_restore_normal();
}

void set_voice_brightness_delta(int delta)
{
    const int next = std::clamp<int>(static_cast<int>(g_awake_brightness) + delta, 5, 100);
    set_brightness(static_cast<uint8_t>(next));
    save_settings();
}

void voice_say(uint16_t code, uint32_t min_gap_ms = 900)
{
    const uint32_t now = tick_ms();
    if (min_gap_ms > 0 && static_cast<int32_t>(now - g_last_voice_say_ms) < static_cast<int32_t>(min_gap_ms)) {
        return;
    }
    const esp_err_t err = voice_module_send_code(code);
    if (err == ESP_OK) {
        g_last_voice_say_ms = now;
    } else if (err != ESP_ERR_NOT_SUPPORTED && err != ESP_ERR_INVALID_STATE) {
        ESP_LOGW(kTag, "voice say failed: code=0x%04X err=%s", code, esp_err_to_name(err));
    }
}

void handle_voice_module_code(uint16_t code, void *)
{
    if (g_voice_cmd_queue == nullptr) {
        return;
    }
    if (xQueueSend(g_voice_cmd_queue, &code, 0) != pdPASS) {
        ESP_LOGW(kTag, "voice command queue full, drop code=0x%04X", code);
    }
}

void post_ui_event(UiEventType type)
{
    if (g_ui_event_queue == nullptr) {
        return;
    }
    UiEvent event{};
    event.type = type;
    if (xQueueSend(g_ui_event_queue, &event, 0) != pdPASS) {
        ESP_LOGW(kTag, "ui event queue full, drop type=%u", static_cast<unsigned>(type));
    }
}

/**
 * @brief BLE 收到桌宠状态后只入队，不在 NimBLE 回调里刷屏或播报。
 */
void handle_ble_pet_event(const BlePetEvent &pet, void *)
{
    if (g_ui_event_queue == nullptr) {
        return;
    }
    UiEvent event{};
    event.type = kUiEventBlePet;
    event.pet = pet;
    if (xQueueSend(g_ui_event_queue, &event, 0) != pdPASS) {
        ESP_LOGW(kTag, "ui event queue full, drop ble pet mood=%s", pet.mood);
    }
}

/**
 * @brief 显示并播报 WiFi 重连成功，避免网页连接和 IP 事件重复触发。
 */
void show_wifi_connected_feedback()
{
    const uint32_t now = tick_ms();
    if (g_last_wifi_ok_feedback_ms != 0 &&
        static_cast<int32_t>(now - g_last_wifi_ok_feedback_ms) < 2000) {
        return;
    }
    g_last_wifi_ok_feedback_ms = now;
    note_activity();
    wake_from_sleep_if_needed();
    g_busy = true;
    draw_pet_notice(kFaceHappy, "WiFi OK " + sta_ip_text());
    voice_say(kVoiceSayWifiOk, 0);
    schedule_restore_normal();
}

/**
 * @brief 显示并播报 WiFi 暂时不可用，随后恢复普通表情并保持后台重试。
 *
 * @param force true 表示上电等关键节点强制提示一次；false 表示按 30 秒节流。
 */
void show_wifi_retry_later_feedback(bool force = false)
{
    const uint32_t now = tick_ms();
    if (!force && g_last_wifi_fail_feedback_ms != 0 &&
        static_cast<int32_t>(now - g_last_wifi_fail_feedback_ms) < 30000) {
        return;
    }
    g_last_wifi_fail_feedback_ms = now;
    note_activity();
    wake_from_sleep_if_needed();
    g_busy = true;
    draw_pet_notice(kFaceAngry, "WiFi retry later");
    voice_say(kVoiceSayWifiFail, force ? 0 : 30000);
    schedule_restore_normal();
}

void process_voice_module_code(uint16_t code)
{
    ESP_LOGI(kTag, "voice command code=0x%04X", code);
    switch (code) {
    case 0xFF58:
        draw_voice_notice(kFaceHappy, "Voice ready");
        break;
    case 0x0100:
    case 0x0300:
        draw_voice_notice(kFaceHappy, "Hello");
        break;
    case 0x0200:
        ESP_LOGI(kTag, "ignore module auto rest frame; mochi sleep is state-driven");
        break;
    case 0x0003:
        draw_voice_notice(kFaceSleepy, "Sleep");
        enter_sleep_state();
        voice_say(kVoiceSaySleep, 0);
        break;
    case 0x026F:
        ESP_LOGI(kTag, "ignore module rest prompt frame; mochi sleep is state-driven");
        break;
    case 0x0400:
    case 0x0027:
        set_voice_brightness_delta(10);
        draw_voice_notice(kFaceHappy, "Bright +");
        break;
    case 0x0500:
    case 0x0028:
        set_voice_brightness_delta(-10);
        draw_voice_notice(kFaceSleepy, "Bright -");
        break;
    case 0x0600:
        set_brightness(100);
        save_settings();
        draw_voice_notice(kFaceHappy, "Bright max");
        break;
    case 0x0700:
        set_brightness(55);
        save_settings();
        draw_voice_notice(kFaceNormal, "Bright mid");
        break;
    case 0x0800:
        set_brightness(10);
        save_settings();
        draw_voice_notice(kFaceSleepy, "Bright min");
        break;
    case 0x0900:
        draw_voice_notice(kFaceHappy, "Voice on");
        break;
    case 0x0A00:
        draw_voice_notice(kFaceNormal, "Voice off");
        break;
    case 0x0001:
    case 0x0002:
        draw_voice_notice(kFaceNormal, "Stop");
        break;
    case 0x0004:
        draw_voice_notice(kFaceHappy, "Forward");
        break;
    case 0x0005:
        draw_voice_notice(kFaceSurprise, "Back");
        break;
    case 0x0006:
    case 0x0029:
        draw_voice_notice(kFaceLook, "Left");
        break;
    case 0x0007:
    case 0x002A:
        draw_voice_notice(kFaceLook, "Right");
        break;
    case 0x002B:
        draw_voice_notice(kFaceLove, "Hold");
        break;
    case 0x002C:
        draw_voice_notice(kFaceWink, "Release");
        break;
    case 0x0034:
        g_idle_activity = 3;
        save_settings();
        draw_voice_notice(kFaceLove, "Dance");
        break;
    case 0x0080:
        draw_voice_notice(kFaceNormal, "Normal");
        break;
    case 0x0081:
        draw_voice_notice(kFaceHappy, "Happy");
        break;
    case 0x0082:
        draw_voice_notice(kFaceSleepy, "Sleepy");
        break;
    case 0x0083:
        draw_voice_notice(kFaceAngry, "Angry");
        break;
    case 0x0084:
        draw_voice_notice(kFaceSurprise, "Surprise");
        break;
    case 0x0085:
        draw_voice_notice(kFaceWink, "Wink");
        break;
    case 0x0086:
        draw_voice_notice(kFaceLove, "Love");
        break;
    case 0x0087:
        draw_voice_notice(kFaceLook, "Look");
        break;
    case 0x0088:
        draw_voice_notice(kFaceSquish, "Squish");
        break;
    case 0x0090:
        draw_voice_notice(kFaceNormal, "Ready");
        voice_say(kVoiceSayReady);
        break;
    case 0x0091:
        draw_voice_notice(kFaceLook, "Thinking");
        voice_say(kVoiceSayThinking);
        break;
    case 0x0092:
        draw_voice_notice(kFaceSurprise, "Running");
        voice_say(kVoiceSayRunning);
        break;
    case 0x0093:
        draw_voice_notice(kFaceHappy, "Speaking");
        voice_say(kVoiceSaySpeaking);
        break;
    case 0x0094:
        draw_voice_notice(kFaceHappy, "Done");
        voice_say(kVoiceSayDone);
        break;
    case 0x0095:
        draw_voice_notice(kFaceAngry, "Error");
        voice_say(kVoiceSayError);
        break;
    case 0x00A0:
        set_backlight(true);
        draw_voice_notice(kFaceHappy, "Screen on");
        voice_say(kVoiceSayScreenOn);
        break;
    case 0x00A1:
        draw_voice_notice(kFaceSleepy, "Screen off");
        set_backlight(false);
        voice_say(kVoiceSayScreenOff);
        break;
    case 0x00A2:
        set_background_rgb(0x402000);
        g_anim_speed = 1;
        g_idle_activity = 1;
        set_brightness(18);
        save_settings();
        draw_voice_notice(kFaceSleepy, "Night");
        voice_say(kVoiceSayNight);
        break;
    case 0x00A3:
        set_background_rgb(kDefaultBgRgb);
        g_anim_speed = 2;
        g_idle_activity = 2;
        set_brightness(80);
        save_settings();
        draw_voice_notice(kFaceHappy, "Day");
        voice_say(kVoiceSayDay);
        break;
    case 0x00A4: {
        const int face = esp_random() % (static_cast<int>(kFaceLook) + 1);
        draw_voice_notice(static_cast<Face>(face), "Random face");
        break;
    }
    case 0x00A5: {
        const uint8_t r = 96 + (esp_random() % 160);
        const uint8_t g = 80 + (esp_random() % 176);
        const uint8_t b = esp_random() % 80;
        set_background_rgb((static_cast<uint32_t>(r) << 16) | (static_cast<uint32_t>(g) << 8) | b);
        save_settings();
        draw_voice_notice(kFaceHappy, "Random color");
        break;
    }
    case 0x00A6:
        g_idle_activity = 1;
        save_settings();
        draw_voice_notice(kFaceSleepy, "Quiet");
        break;
    case 0x00A7:
        g_idle_activity = 3;
        save_settings();
        draw_voice_notice(kFaceHappy, "Active");
        break;
    default: {
        char text[20] = {};
        std::snprintf(text, sizeof(text), "Voice %04X", static_cast<unsigned>(code));
        draw_voice_notice(kFaceSurprise, text);
        break;
    }
    }
}

void task_voice_command(void *)
{
    uint16_t code = 0;
    while (true) {
        if (xQueueReceive(g_voice_cmd_queue, &code, portMAX_DELAY) == pdTRUE) {
            process_voice_module_code(code);
        }
    }
}

void task_ui_event(void *)
{
    UiEvent event{};
    while (true) {
        if (xQueueReceive(g_ui_event_queue, &event, portMAX_DELAY) != pdTRUE) {
            continue;
        }
        switch (event.type) {
        case kUiEventWifiRetryLater:
            show_wifi_retry_later_feedback();
            break;
        case kUiEventWifiConnected:
            show_wifi_connected_feedback();
            break;
        case kUiEventBlePet:
            handle_pet_status(event.pet.mood, event.pet.text);
            break;
        default:
            break;
        }
    }
}

void draw_code_view()
{
    DisplayLock lock;
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
    DisplayLock lock;
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
    DisplayLock lock;
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
    DisplayLock lock;
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
    DisplayLock lock;
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

void anim_wake_up()
{
    DisplayLock lock;
    g_busy = true;
    g_current_face = kFaceNormal;
    g_current_view = kViewEyesNormal;
    const int full_h = scale_design(kEyeHDesign);
    const int frames[] = {
        std::max(3, scale_design(4)),
        std::max(5, full_h / 4),
        std::max(8, full_h / 2),
        full_h,
        std::max(4, full_h / 8),
        full_h,
    };
    for (int h : frames) {
        draw_waking_eyes(h);
        delay_ms(95);
    }
    draw_face(kFaceNormal);
    g_busy = false;
}

void anim_logo_reveal()
{
    DisplayLock lock;
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

std::string sta_ip_text()
{
    if (!g_sta_connected) {
        return "";
    }
    char ip[16] = {};
    esp_ip4addr_ntoa(&g_sta_ip, ip, sizeof(ip));
    return ip;
}

/**
 * @brief 获取桌宠在局域网内的固定 mDNS 主机名。
 *
 * @return 形如 clawd-mochi.local 的地址，供屏幕、网页和状态接口展示。
 */
std::string mdns_host_text()
{
    return std::string(kMdnsHost) + ".local";
}

bool sta_configured()
{
    if (g_sta_disabled || g_sta_manual_pause) {
        return false;
    }
    return !g_sta_ssid.empty() || std::strlen(CONFIG_MOCHI_WIFI_STA_SSID) > 0;
}

bool sta_credentials_available()
{
    return !g_sta_ssid.empty() || std::strlen(CONFIG_MOCHI_WIFI_STA_SSID) > 0;
}

std::string sta_ssid()
{
    return g_sta_ssid.empty() ? std::string(CONFIG_MOCHI_WIFI_STA_SSID) : g_sta_ssid;
}

std::string sta_password()
{
    return g_sta_ssid.empty() ? std::string(CONFIG_MOCHI_WIFI_STA_PASSWORD) : g_sta_password;
}

/**
 * @brief 将当前保存的 STA 账号密码写入 WiFi 驱动。
 *
 * @return ESP_OK 表示配置已写入；没有保存 SSID 时返回 ESP_ERR_INVALID_STATE。
 */
esp_err_t apply_sta_wifi_config()
{
    if (!sta_credentials_available()) {
        return ESP_ERR_INVALID_STATE;
    }

    wifi_config_t sta_config = {};
    const std::string ssid = sta_ssid();
    const std::string password = sta_password();
    std::strncpy(reinterpret_cast<char *>(sta_config.sta.ssid), ssid.c_str(), sizeof(sta_config.sta.ssid));
    std::strncpy(reinterpret_cast<char *>(sta_config.sta.password), password.c_str(), sizeof(sta_config.sta.password));
    sta_config.sta.threshold.authmode = WIFI_AUTH_WPA2_PSK;
    return esp_wifi_set_config(WIFI_IF_STA, &sta_config);
}

const char *sta_reason_text(uint8_t reason)
{
    if (g_sta_manual_pause) {
        return "已临时断开";
    }
    switch (reason) {
    case WIFI_REASON_AUTH_FAIL:
    case WIFI_REASON_4WAY_HANDSHAKE_TIMEOUT:
    case WIFI_REASON_HANDSHAKE_TIMEOUT:
        return "密码可能错误";
    case WIFI_REASON_NO_AP_FOUND:
        return "没有找到这个 WiFi";
    case WIFI_REASON_ASSOC_FAIL:
    case WIFI_REASON_BEACON_TIMEOUT:
        return "信号较弱或路由器拒绝连接";
    case 0:
        return "连接超时";
    default:
        return "连接失败";
    }
}

void draw_wifi_info()
{
    DisplayLock lock;
    g_display.fillScreen(g_dark_bg);
    g_display.fillRect(0, 0, g_display.width(), 4, g_orange);
    g_display.setTextColor(kWhite);
    g_display.setTextSize(g_display.width() >= 180 ? 2 : 1);
    g_display.setCursor(12, 16);
    g_display.print(g_sta_connected ? "LAN connected" : "WiFi: ClaWD-Mochi");
    g_display.setTextColor(g_muted);
    g_display.setTextSize(1);
    g_display.setCursor(12, 40);
    g_display.print(g_sta_connected ? "same router as PC" : "password: clawd1234");
    g_display.setTextColor(kWhite);
    g_display.setTextSize(g_display.width() >= 180 ? 2 : 1);
    g_display.setCursor(12, 62);
    g_display.print(g_sta_connected ? "Bridge host:" : "Open browser:");
    g_display.setTextColor(g_orange);
    g_display.setCursor(12, 84);
    const std::string ip = sta_ip_text();
    g_display.print(g_sta_connected ? mdns_host_text() : "192.168.4.1");
    g_display.setTextColor(g_muted);
    g_display.setTextSize(1);
    g_display.setCursor(12, 112);
    g_display.print(g_sta_connected ? ip : "fallback AP mode");
    g_display.setTextColor(g_green);
    g_display.setCursor(12, 130);
    g_display.print(ble_bridge_is_ready() ? "BLE bridge ready" : "BLE bridge starting");
    g_display.flush();
}

bool can_idle_animate()
{
    if (g_busy || g_term_mode || g_current_view == kViewCode || g_current_view == kViewDraw) {
        return false;
    }
    return static_cast<int32_t>(tick_ms() - g_manual_anim_until_ms) >= 0;
}

int idle_interval_ms()
{
    if (g_idle_activity == 1) {
        return 5200;
    }
    if (g_idle_activity == 3) {
        return 1700;
    }
    return 2800;
}

int idle_step_ms(int ms)
{
    if (g_idle_activity == 1) {
        return ms * 4 / 3;
    }
    if (g_idle_activity == 3) {
        return std::max(35, ms * 2 / 3);
    }
    return ms;
}

void task_auto_sleep(void *)
{
    while (true) {
        delay_ms(5000);
        if (g_sleeping || g_current_view == kViewDraw || g_term_mode) {
            continue;
        }
        const uint32_t now = tick_ms();
        if (static_cast<int32_t>(now - g_last_activity_ms) >= static_cast<int32_t>(kAutoSleepTimeoutMs)) {
            enter_sleep_state();
            voice_say(kVoiceSaySleep, 0);
        }
    }
}

void task_idle_face(void *)
{
    uint8_t cycle = 0;
    while (true) {
        delay_ms(idle_interval_ms());
        if (!can_idle_animate()) {
            continue;
        }
        DisplayLock lock;
        if (!can_idle_animate()) {
            continue;
        }
        g_busy = true;
        if (g_current_face == kFaceNormal) {
            const uint32_t action = esp_random() % (g_idle_activity == 3 ? 5 : 4);
            if (action == 0 || (g_idle_activity == 1 && (cycle % 2) == 0)) {
                draw_normal_eyes(0, true);
                delay_ms(idle_step_ms(85));
                draw_normal_eyes(0, false);
            } else if (action == 1) {
                draw_normal_eyes(-scale_design(12));
                delay_ms(idle_step_ms(145));
                draw_normal_eyes(scale_design(12));
                delay_ms(idle_step_ms(145));
                draw_normal_eyes();
            } else if (action == 2) {
                draw_normal_eyes(0, true);
                delay_ms(idle_step_ms(70));
                draw_normal_eyes(0, false);
                delay_ms(idle_step_ms(95));
                draw_normal_eyes(0, true);
                delay_ms(idle_step_ms(65));
                draw_normal_eyes(0, false);
            } else if (action == 3) {
                draw_waking_eyes(scale_design(kEyeHDesign) / 2);
                delay_ms(idle_step_ms(120));
                draw_normal_eyes();
            } else {
                draw_normal_eyes(-scale_design(16));
                delay_ms(idle_step_ms(95));
                draw_normal_eyes(-scale_design(4));
                delay_ms(idle_step_ms(95));
                draw_normal_eyes(scale_design(10));
                delay_ms(idle_step_ms(95));
                draw_normal_eyes();
            }
        } else if (g_current_face == kFaceSquish) {
            draw_squish_eyes(true);
            delay_ms(idle_step_ms(90));
            draw_squish_eyes(false);
        } else if (g_current_face == kFaceHappy) {
            draw_happy_eyes(-scale_design(5));
            delay_ms(idle_step_ms(85));
            draw_happy_eyes(scale_design(2));
            delay_ms(idle_step_ms(85));
            draw_happy_eyes();
        } else if (g_current_face == kFaceSleepy) {
            draw_sleepy_eyes(1);
            delay_ms(idle_step_ms(160));
            draw_sleepy_eyes(2);
            delay_ms(idle_step_ms(160));
            draw_sleepy_eyes(3);
            delay_ms(idle_step_ms(220));
            draw_sleepy_eyes();
        } else if (g_current_face == kFaceAngry) {
            for (uint8_t i = 0; i < 4; ++i) {
                draw_angry_eyes((i % 2 == 0) ? -scale_design(3) : scale_design(3));
                delay_ms(idle_step_ms(55));
            }
            draw_angry_eyes();
        } else if (g_current_face == kFaceSurprise) {
            draw_surprise_eyes(scale_design(5));
            delay_ms(idle_step_ms(110));
            draw_surprise_eyes(-scale_design(3));
            delay_ms(idle_step_ms(80));
            draw_surprise_eyes();
        } else if (g_current_face == kFaceLove) {
            draw_love_eyes(scale_design(5));
            delay_ms(idle_step_ms(95));
            draw_love_eyes();
            delay_ms(idle_step_ms(70));
            draw_love_eyes(scale_design(4));
            delay_ms(idle_step_ms(95));
            draw_love_eyes();
        } else if (g_current_face == kFaceLook) {
            draw_side_eye(-scale_design(8));
            delay_ms(idle_step_ms(180));
            draw_side_eye(scale_design(6));
            delay_ms(idle_step_ms(180));
            draw_side_eye();
        } else if (g_current_face == kFaceWink) {
            draw_normal_eyes();
            delay_ms(idle_step_ms(95));
            draw_wink_eyes();
        }
        ++cycle;
        g_busy = false;
    }
}

void task_restore_normal_face_once(void *)
{
    delay_ms(3000);
    if (!g_sleeping && !g_term_mode && g_current_view != kViewCode && g_current_view != kViewDraw) {
        g_current_view = kViewEyesNormal;
        draw_face(kFaceNormal);
    }
    g_busy = false;
    vTaskDelete(nullptr);
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
    note_activity();
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
    note_activity();
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
    note_activity();
    const std::string value = query_value(req, "v", 64);
    if (!value.empty()) {
        g_anim_speed = std::clamp(std::atoi(value.c_str()), 1, 3);
        save_settings();
    }
    send_json(req);
    return ESP_OK;
}

esp_err_t route_activity(httpd_req_t *req)
{
    note_activity();
    const std::string value = query_value(req, "v", 64);
    if (!value.empty()) {
        g_idle_activity = std::clamp(std::atoi(value.c_str()), 1, 3);
        save_settings();
    }
    send_json(req);
    return ESP_OK;
}

esp_err_t route_face(httpd_req_t *req)
{
    note_activity();
    const std::string value = query_value(req, "v", 64);
    if (!value.empty()) {
        const int face = std::clamp(std::atoi(value.c_str()), 0, static_cast<int>(kFaceLook));
        mark_manual_animation();
        g_current_view = (face == kFaceSquish) ? kViewEyesSquish : kViewEyesNormal;
        g_term_mode = false;
        draw_face(static_cast<Face>(face));
        save_settings();
    }
    send_json(req);
    return ESP_OK;
}

bool contains_status_word(std::string text, const char *word)
{
    std::transform(text.begin(), text.end(), text.begin(), [](unsigned char ch) {
        return static_cast<char>(std::tolower(ch));
    });
    return text.find(word) != std::string::npos;
}

void voice_say_pet_status(const std::string &mood, const std::string &text)
{
    if (mood == "error" || mood == "fail" || mood == "angry" ||
        contains_status_word(text, "fail") || contains_status_word(text, "error")) {
        voice_say(kVoiceSayError);
    } else if (mood == "speaking" || contains_status_word(text, "speaking")) {
        voice_say(kVoiceSaySpeaking);
    } else if (mood == "thinking" || contains_status_word(text, "thinking")) {
        voice_say(kVoiceSayThinking);
    } else if (mood == "running" || mood == "busy" || contains_status_word(text, "running")) {
        voice_say(kVoiceSayRunning);
    } else if (contains_status_word(text, "ready") || contains_status_word(text, "done") ||
               contains_status_word(text, "completed") || contains_status_word(text, " ok")) {
        voice_say(kVoiceSayDone);
    } else if (mood == "idle" || mood == "sleepy" || contains_status_word(text, "sleep")) {
        voice_say(kVoiceSaySleep);
    }
}

void handle_pet_status(const std::string &mood, const std::string &text)
{
    note_activity();
    mark_manual_animation();
    draw_pet_notice(mood_to_face(mood), text);
    voice_say_pet_status(mood, text);
}

esp_err_t route_pet(httpd_req_t *req)
{
    const std::string mood = query_value(req, "mood", 64);
    const std::string text = query_value(req, "text", 256);
    handle_pet_status(mood, text);
    send_json(req);
    return ESP_OK;
}

esp_err_t route_wifi_scan(httpd_req_t *req)
{
    note_activity();
    constexpr uint16_t kMaxAps = 12;
    wifi_ap_record_t aps[kMaxAps] = {};
    uint16_t count = kMaxAps;
    esp_err_t err = esp_wifi_scan_start(nullptr, true);
    if (err != ESP_OK) {
        ESP_LOGW(kTag, "wifi scan failed: %s", esp_err_to_name(err));
        send_json(req, "{\"nets\":[]}");
        return ESP_OK;
    }
    ESP_ERROR_CHECK_WITHOUT_ABORT(esp_wifi_scan_get_ap_records(&count, aps));
    std::string json = "{\"nets\":[";
    for (uint16_t i = 0; i < count; ++i) {
        if (i > 0) {
            json += ",";
        }
        json += "{\"ssid\":\"";
        json += json_escape_utf8(reinterpret_cast<const char *>(aps[i].ssid), 64);
        json += "\",\"rssi\":";
        json += std::to_string(aps[i].rssi);
        json += "}";
    }
    json += "]}";
    send_json(req, json.c_str());
    return ESP_OK;
}

esp_err_t route_wifi_connect(httpd_req_t *req)
{
    note_activity();
    const std::string ssid = query_value(req, "ssid", 96);
    const std::string password = query_value(req, "pwd", 128);
    const bool use_saved = query_value(req, "saved", 16) == "1";
    if (ssid.empty() && (!use_saved || !sta_credentials_available())) {
        httpd_resp_set_status(req, "400 Bad Request");
        send_json(req, "{\"e\":1,\"connected\":false,\"reason\":\"没有保存的 WiFi\"}");
        return ESP_OK;
    }

    if (!ssid.empty()) {
        save_sta_credentials(ssid, password);
    }
    g_sta_manual_pause = false;
    g_sta_disabled = false;
    g_sta_connected = false;
    g_sta_ip.addr = 0;
    g_sta_last_disconnect_reason = 0;
    g_sta_retry_count = 0;
    ESP_ERROR_CHECK_WITHOUT_ABORT(esp_wifi_disconnect());
    ESP_ERROR_CHECK_WITHOUT_ABORT(apply_sta_wifi_config());
    ESP_ERROR_CHECK_WITHOUT_ABORT(esp_wifi_connect());
    for (uint8_t i = 0; i < 32 && !g_sta_connected; ++i) {
        delay_ms(250);
    }
    if (g_sta_connected) {
        const std::string ip = sta_ip_text();
        show_wifi_connected_feedback();
        std::string json = "{\"ok\":1,\"connected\":true,\"ip\":\"";
        json += ip;
        json += "\",\"url\":\"http://";
        json += mdns_host_text();
        json += "\",\"host\":\"";
        json += mdns_host_text();
        json += "\"}";
        send_json(req, json.c_str());
    } else {
        const char *reason = sta_reason_text(g_sta_last_disconnect_reason);
        draw_pet_notice(kFaceAngry, reason);
        voice_say(kVoiceSayWifiFail, 0);
        std::string json = "{\"ok\":0,\"connected\":false,\"reason\":\"";
        json += json_escape_utf8(reason, 80);
        json += "\"}";
        send_json(req, json.c_str());
    }
    return ESP_OK;
}

esp_err_t route_wifi_forget(httpd_req_t *req)
{
    note_activity();
    g_sta_manual_pause = true;
    g_sta_connected = false;
    g_sta_ip.addr = 0;
    g_sta_retry_count = 0;
    ESP_ERROR_CHECK_WITHOUT_ABORT(esp_wifi_disconnect());
    draw_pet_notice(kFaceNormal, "WiFi paused");
    send_json(req, "{\"ok\":1,\"paused\":true,\"saved\":true,\"ap\":\"http://192.168.4.1\"}");
    return ESP_OK;
}

esp_err_t route_redraw(httpd_req_t *req)
{
    note_activity();
    const std::string bg = query_value(req, "bg", 128);
    if (!bg.empty()) {
        set_background_rgb(hex_to_rgb888(bg, g_bg_rgb));
        save_settings();
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
    note_activity();
    const std::string on = query_value(req, "on", 64);
    if (on == "1") {
        DisplayLock lock;
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
    note_activity();
    const std::string bg = query_value(req, "bg", 128);
    set_background_rgb(hex_to_rgb888(bg.empty() ? "#ff8000" : bg, g_bg_rgb));
    DisplayLock lock;
    g_current_view = kViewDraw;
    g_term_mode = false;
    g_display.fillScreen(g_draw_bg);
    g_display.flush();
    save_settings();
    send_json(req);
    return ESP_OK;
}

esp_err_t route_draw_stroke(httpd_req_t *req)
{
    note_activity();
    const std::string pen = query_value(req, "pen", 128);
    const std::string size_text = query_value(req, "size", 64);
    const std::string data = query_value(req, "pts", 4096);
    if (pen.empty() || data.empty()) {
        send_json(req);
        return ESP_OK;
    }

    const uint16_t color = hex_to_rgb565(pen);
    const int brush = std::clamp(std::atoi(size_text.empty() ? "3" : size_text.c_str()), 1, 8);
    const int radius = std::max(1, brush / 2);
    DisplayLock lock;
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
                for (int off = -radius; off <= radius; ++off) {
                    g_display.drawLine(prev_x + off, prev_y, x + off, y, color);
                    g_display.drawLine(prev_x, prev_y + off, x, y + off, color);
                }
                min_x = std::min<int>(min_x, std::min(prev_x, x) - radius - 2);
                min_y = std::min<int>(min_y, std::min(prev_y, y) - radius - 2);
                max_x = std::max<int>(max_x, std::max(prev_x, x) + radius + 3);
                max_y = std::max<int>(max_y, std::max(prev_y, y) + radius + 3);
            } else {
                g_display.fillCircle(x, y, radius, color);
                min_x = std::min<int>(min_x, x - radius - 2);
                min_y = std::min<int>(min_y, y - radius - 2);
                max_x = std::max<int>(max_x, x + radius + 3);
                max_y = std::max<int>(max_y, y + radius + 3);
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
    note_activity();
    set_backlight(query_value(req, "on", 64) == "1");
    send_json(req);
    return ESP_OK;
}

esp_err_t route_brightness(httpd_req_t *req)
{
    note_activity();
    const std::string value = query_value(req, "v", 64);
    if (!value.empty()) {
        set_brightness(static_cast<uint8_t>(std::atoi(value.c_str())));
        save_settings();
    }
    send_json(req);
    return ESP_OK;
}

esp_err_t route_random(httpd_req_t *req)
{
    note_activity();
    const std::string what = query_value(req, "what", 64);
    mark_manual_animation();
    if (what == "face") {
        const int face = esp_random() % (static_cast<int>(kFaceLook) + 1);
        g_current_view = (face == kFaceSquish) ? kViewEyesSquish : kViewEyesNormal;
        g_term_mode = false;
        draw_face(static_cast<Face>(face));
    } else {
        const uint8_t r = 96 + (esp_random() % 160);
        const uint8_t g = 80 + (esp_random() % 176);
        const uint8_t b = esp_random() % 80;
        set_background_rgb((static_cast<uint32_t>(r) << 16) | (static_cast<uint32_t>(g) << 8) | b);
        g_current_view = (g_current_face == kFaceSquish) ? kViewEyesSquish : kViewEyesNormal;
        g_term_mode = false;
        draw_face(g_current_face);
    }
    save_settings();
    send_json(req);
    return ESP_OK;
}

esp_err_t route_night(httpd_req_t *req)
{
    note_activity();
    mark_manual_animation();
    set_background_rgb(0x402000);
    g_anim_speed = 1;
    g_idle_activity = 1;
    set_brightness(18);
    g_current_face = kFaceSleepy;
    g_current_view = kViewEyesNormal;
    g_term_mode = false;
    draw_face(g_current_face);
    save_settings();
    send_json(req);
    return ESP_OK;
}

esp_err_t route_day(httpd_req_t *req)
{
    note_activity();
    mark_manual_animation();
    set_background_rgb(kDefaultBgRgb);
    g_anim_speed = 2;
    g_idle_activity = 2;
    set_brightness(80);
    g_current_face = kFaceNormal;
    g_current_view = kViewEyesNormal;
    g_term_mode = false;
    draw_face(g_current_face);
    save_settings();
    send_json(req);
    return ESP_OK;
}

esp_err_t route_factory(httpd_req_t *req)
{
    note_activity();
    erase_settings();
    apply_default_settings();
    draw_face(g_current_face);
    send_json(req);
    return ESP_OK;
}

esp_err_t route_state(httpd_req_t *req)
{
    const VoiceModuleStatus voice = voice_module_get_status();
    char voice_code[7] = {};
    std::snprintf(voice_code, sizeof(voice_code), "0x%04X", static_cast<unsigned>(voice.last_code));
    char json[1024];
    const int written = std::snprintf(json, sizeof(json),
                                      "{\"view\":%u,\"face\":%u,\"busy\":%s,\"term\":%s,\"bl\":%s,\"sleep\":%s,\"voice_enabled\":%s,\"voice_ready\":%s,\"ble_ready\":%s,\"voice_code\":\"%s\",\"voice_frames\":%u,\"voice_bad\":%u,\"speed\":%u,\"activity\":%u,\"brightness\":%u,\"bg\":\"%s\",\"sta\":\"%s\",\"ip\":\"%s\",\"mdns\":\"%s\",\"reason\":\"%s\",\"uptime\":%lld,\"heap\":%u,\"w\":%d,\"h\":%d,\"driver\":\"%s\"}",
                                      static_cast<unsigned>(g_current_view),
                                      static_cast<unsigned>(g_current_face),
                                      g_busy ? "true" : "false",
                                      g_term_mode ? "true" : "false",
                                      g_backlight_on ? "true" : "false",
                                      g_sleeping ? "true" : "false",
                                      voice.enabled ? "true" : "false",
                                      voice.ready ? "true" : "false",
                                      ble_bridge_is_ready() ? "true" : "false",
                                      voice_code,
                                      static_cast<unsigned>(voice.frame_count),
                                      static_cast<unsigned>(voice.bad_frame_count),
                                      static_cast<unsigned>(g_anim_speed),
                                      static_cast<unsigned>(g_idle_activity),
                                      static_cast<unsigned>(g_backlight_brightness),
                                      rgb888_to_hex(g_bg_rgb).c_str(),
                                      json_escape_utf8(sta_ssid(), 64).c_str(),
                                      sta_ip_text().c_str(),
                                      g_sta_connected ? mdns_host_text().c_str() : "",
                                      g_sta_connected ? "正常" : json_escape_utf8(sta_reason_text(g_sta_last_disconnect_reason), 80).c_str(),
                                      static_cast<long long>(esp_timer_get_time() / 1000000),
                                      static_cast<unsigned>(heap_caps_get_free_size(MALLOC_CAP_8BIT)),
                                      g_display.width(),
                                      g_display.height(),
                                      g_display.driverName());
    if (written < 0 || written >= static_cast<int>(sizeof(json))) {
        ESP_LOGW(kTag, "state json truncated: written=%d", written);
    }
    send_json(req, json);
    return ESP_OK;
}

/**
 * @brief 启动 mDNS 自动发现服务。
 *
 * 注册 A 记录和 _http._tcp 服务，让电脑或手机可通过
 * http://clawd-mochi.local 访问控制页，避免用户手动记录动态 IP。
 *
 * @return ESP_OK 表示启动成功，其他值表示 mDNS 初始化或服务注册失败。
 */
esp_err_t start_mdns_service()
{
    ESP_RETURN_ON_ERROR(mdns_init(), kTag, "mdns init failed");
    ESP_RETURN_ON_ERROR(mdns_hostname_set(kMdnsHost), kTag, "mdns hostname failed");
    ESP_RETURN_ON_ERROR(mdns_instance_name_set(kMdnsName), kTag, "mdns instance failed");

    mdns_txt_item_t txt[] = {
        {"board", "esp32-s3"},
        {"app", "clawd-mochi"},
        {"path", "/"},
    };
    ESP_RETURN_ON_ERROR(mdns_service_add(kMdnsName, "_http", "_tcp", 80, txt, sizeof(txt) / sizeof(txt[0])),
                        kTag, "mdns http service failed");
    ESP_LOGI(kTag, "mDNS started: %s.local", kMdnsHost);
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
    config.max_uri_handlers = 24;
    config.lru_purge_enable = true;
    ESP_RETURN_ON_ERROR(httpd_start(&g_server, &config), kTag, "http server start failed");
    register_uri("/", HTTP_GET, route_root);
    register_uri("/cmd", HTTP_GET, route_cmd);
    register_uri("/char", HTTP_GET, route_char);
    register_uri("/speed", HTTP_GET, route_speed);
    register_uri("/activity", HTTP_GET, route_activity);
    register_uri("/face", HTTP_GET, route_face);
    register_uri("/pet", HTTP_GET, route_pet);
    register_uri("/wifi/scan", HTTP_GET, route_wifi_scan);
    register_uri("/wifi/connect", HTTP_GET, route_wifi_connect);
    register_uri("/wifi/forget", HTTP_GET, route_wifi_forget);
    register_uri("/redraw", HTTP_GET, route_redraw);
    register_uri("/canvas", HTTP_GET, route_canvas);
    register_uri("/draw/clear", HTTP_GET, route_draw_clear);
    register_uri("/draw/stroke", HTTP_GET, route_draw_stroke);
    register_uri("/backlight", HTTP_GET, route_backlight);
    register_uri("/brightness", HTTP_GET, route_brightness);
    register_uri("/random", HTTP_GET, route_random);
    register_uri("/night", HTTP_GET, route_night);
    register_uri("/day", HTTP_GET, route_day);
    register_uri("/factory", HTTP_GET, route_factory);
    register_uri("/state", HTTP_GET, route_state);
    return ESP_OK;
}

void wifi_event_handler(void *, esp_event_base_t event_base, int32_t event_id, void *event_data)
{
    if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_START) {
        if (sta_configured()) {
            esp_wifi_connect();
        }
    } else if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_DISCONNECTED) {
        const auto *event = static_cast<wifi_event_sta_disconnected_t *>(event_data);
        g_sta_connected = false;
        g_sta_ip.addr = 0;
        g_sta_last_disconnect_reason = event ? event->reason : 0;
        if (sta_configured()) {
            ++g_sta_retry_count;
            if (g_sta_retry_count >= 3) {
                ESP_LOGW(kTag, "station disconnected, reason=%u, wait for 10s retry", g_sta_last_disconnect_reason);
                post_ui_event(kUiEventWifiRetryLater);
            } else {
                ESP_LOGW(kTag, "station disconnected, reason=%u, retrying", g_sta_last_disconnect_reason);
                esp_wifi_connect();
            }
        }
    } else if (event_base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP) {
        const auto *event = static_cast<ip_event_got_ip_t *>(event_data);
        g_sta_ip = event->ip_info.ip;
        g_sta_connected = true;
        g_sta_last_disconnect_reason = 0;
        g_sta_retry_count = 0;
        ESP_LOGI(kTag, "station got ip: " IPSTR, IP2STR(&g_sta_ip));
        post_ui_event(kUiEventWifiConnected);
    }
}

/**
 * @brief 后台 WiFi 重连任务。
 *
 * STA 连续失败三次后不再紧密重试，避免页面和热点模式被反复打断；
 * 之后每 10 秒尝试一次，保存的 NVS WiFi 配置不会被清除。
 */
void task_wifi_reconnect(void *)
{
    while (true) {
        delay_ms(10000);
        if (!g_sta_connected && sta_configured() && g_sta_retry_count >= 3) {
            ESP_LOGI(kTag, "periodic station retry: ssid=%s", sta_ssid().c_str());
            ESP_ERROR_CHECK_WITHOUT_ABORT(apply_sta_wifi_config());
            ESP_ERROR_CHECK_WITHOUT_ABORT(esp_wifi_connect());
        }
    }
}

esp_err_t wifi_init_apsta()
{
    ESP_RETURN_ON_ERROR(esp_netif_init(), kTag, "netif init failed");
    ESP_RETURN_ON_ERROR(esp_event_loop_create_default(), kTag, "event loop failed");
    esp_netif_create_default_wifi_ap();
    esp_netif_create_default_wifi_sta();

    wifi_init_config_t init_config = WIFI_INIT_CONFIG_DEFAULT();
    ESP_RETURN_ON_ERROR(esp_wifi_init(&init_config), kTag, "wifi init failed");
    ESP_RETURN_ON_ERROR(esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID, wifi_event_handler, nullptr), kTag, "wifi event handler failed");
    ESP_RETURN_ON_ERROR(esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP, wifi_event_handler, nullptr), kTag, "ip event handler failed");

    wifi_config_t ap_config = {};
    std::strncpy(reinterpret_cast<char *>(ap_config.ap.ssid), CONFIG_MOCHI_WIFI_AP_SSID, sizeof(ap_config.ap.ssid));
    ap_config.ap.ssid_len = std::strlen(CONFIG_MOCHI_WIFI_AP_SSID);
    std::strncpy(reinterpret_cast<char *>(ap_config.ap.password), CONFIG_MOCHI_WIFI_AP_PASSWORD, sizeof(ap_config.ap.password));
    ap_config.ap.channel = 1;
    ap_config.ap.max_connection = 4;
    ap_config.ap.authmode = WIFI_AUTH_WPA_WPA2_PSK;
    if (std::strlen(CONFIG_MOCHI_WIFI_AP_PASSWORD) == 0) {
        ap_config.ap.authmode = WIFI_AUTH_OPEN;
    }

    ESP_RETURN_ON_ERROR(esp_wifi_set_mode(WIFI_MODE_APSTA), kTag, "wifi mode failed");
    ESP_RETURN_ON_ERROR(esp_wifi_set_config(WIFI_IF_AP, &ap_config), kTag, "ap config failed");

    if (sta_configured()) {
        ESP_RETURN_ON_ERROR(apply_sta_wifi_config(), kTag, "sta config failed");
    }

    ESP_RETURN_ON_ERROR(esp_wifi_start(), kTag, "wifi start failed");
    ESP_LOGI(kTag, "softAP started: ssid=%s password=%s ip=192.168.4.1",
             CONFIG_MOCHI_WIFI_AP_SSID, CONFIG_MOCHI_WIFI_AP_PASSWORD);
    if (sta_configured()) {
        ESP_LOGI(kTag, "station connecting: ssid=%s", sta_ssid().c_str());
    } else {
        ESP_LOGI(kTag, "station disabled; configure WiFi from web page");
    }
    return ESP_OK;
}

void init_colours()
{
    g_orange = MochiDisplay::color565(255, 128, 0);
    g_dark_bg = MochiDisplay::color565(10, 12, 16);
    g_muted = MochiDisplay::color565(90, 88, 86);
    g_green = MochiDisplay::color565(80, 220, 130);
    set_background_rgb(g_bg_rgb);
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
    g_display_mutex = xSemaphoreCreateRecursiveMutex();
    ESP_ERROR_CHECK(g_display_mutex == nullptr ? ESP_ERR_NO_MEM : ESP_OK);
    ESP_ERROR_CHECK(g_display.init());
    init_colours();
    load_settings();
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
    ESP_ERROR_CHECK(wifi_init_apsta());
    ESP_ERROR_CHECK(start_mdns_service());
    ESP_ERROR_CHECK(start_http_server());
    g_voice_cmd_queue = xQueueCreate(8, sizeof(uint16_t));
    ESP_ERROR_CHECK(g_voice_cmd_queue == nullptr ? ESP_ERR_NO_MEM : ESP_OK);
    g_ui_event_queue = xQueueCreate(8, sizeof(UiEvent));
    ESP_ERROR_CHECK(g_ui_event_queue == nullptr ? ESP_ERR_NO_MEM : ESP_OK);
    ESP_ERROR_CHECK(xTaskCreate(task_ui_event, "ui_event", 4096, nullptr, 4, nullptr) == pdPASS ? ESP_OK : ESP_ERR_NO_MEM);
    ESP_ERROR_CHECK(xTaskCreate(task_voice_command, "voice_cmd", 6144, nullptr, 4, nullptr) == pdPASS ? ESP_OK : ESP_ERR_NO_MEM);
    ESP_ERROR_CHECK_WITHOUT_ABORT(ble_bridge_init(handle_ble_pet_event, nullptr));
    ESP_ERROR_CHECK_WITHOUT_ABORT(voice_module_init(handle_voice_module_code, nullptr));
    note_activity();
    xTaskCreate(task_auto_sleep, "auto_sleep", 3072, nullptr, 3, nullptr);
    xTaskCreate(task_wifi_reconnect, "wifi_retry", 3072, nullptr, 3, nullptr);
    for (uint8_t i = 0; i < 20 && sta_configured() && !g_sta_connected; ++i) {
        delay_ms(250);
    }
    const bool startup_wifi_failed = sta_configured() && !g_sta_connected;
    draw_wifi_info();
    if (startup_wifi_failed) {
        show_wifi_retry_later_feedback(true);
        delay_ms(3200);
    } else {
        delay_ms(2200);
        anim_wake_up();
    }
    xTaskCreate(task_idle_face, "idle_face", 4096, nullptr, 4, nullptr);

    while (true) {
        delay_ms(1000);
    }
}
