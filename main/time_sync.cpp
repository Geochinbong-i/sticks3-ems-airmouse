#include "time_sync.h"
#include "time_sync_parse.h"
#include "app_settings.h"
#include "airmouse/airmouse_ble.h"
#include "input_util.h"
#include "sound_fx.h"
#include "ui_common.h"

#include "M5Unified.h"
#include "esp_event.h"
#include "esp_http_server.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_netif_sntp.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "freertos/task.h"

#include <cstdlib>
#include <cstring>
#include <ctime>
#include <sys/time.h>

namespace {

static constexpr const char *kTag = "time_sync";

bool s_netif_ready = false;
esp_netif_t *s_ap_netif = nullptr;
esp_netif_t *s_sta_netif = nullptr;
httpd_handle_t s_httpd = nullptr;
bool s_wifi_started = false;
bool s_ble_sync_started = false;

portMUX_TYPE s_sync_mux = portMUX_INITIALIZER_UNLOCKED;
volatile bool s_ble_pending = false;
uint8_t s_ble_buf[32]{};
uint16_t s_ble_len = 0;

volatile bool s_http_pending = false;
int64_t s_http_epoch = 0;
bool s_http_has_tz = false;
int s_http_tz_hours = 9;

volatile bool s_wifi_cfg_saved_notice = false;

static const char kPortalHtml[] = R"rawhtml(<!DOCTYPE html>
<html><head><meta charset="utf-8"><meta name="viewport" content="width=device-width,initial-scale=1">
<title>StickS3 Time Sync</title>
<style>
body{font-family:system-ui,sans-serif;background:#0b1018;color:#eef2f7;margin:0;padding:18px;max-width:440px}
.card{background:#151e2c;border:1px solid #2b3c54;border-radius:12px;padding:16px;margin-bottom:14px}
h2{margin:0 0 8px 0;font-size:18px;color:#80d8ff}
.clk{font:700 28px/1.2 monospace;color:#69f0ae;margin:8px 0}
button,input{width:100%;box-sizing:border-box;font-size:15px;padding:11px;border-radius:8px;margin-top:8px}
input{background:#0b1018;border:1px solid #354966;color:#fff}
button{background:#1976d2;color:#fff;border:0;font-weight:700;cursor:pointer}
.sub{background:#263547}
.msg{margin-top:8px;font-size:13px;color:#80cbc4}
</style></head><body>
<div class="card">
  <h2>StickS3 Time Sync</h2>
  <div id="now" class="clk">--:--:--</div>
  <button onclick="syncNow()">1-Tap Sync Phone Time</button>
  <div id="msg" class="msg">Ready</div>
</div>
<div class="card">
  <h2>Save Wi-Fi for SNTP</h2>
  <form onsubmit="saveWifi(event)">
    <input id="ssid" placeholder="Wi-Fi SSID" maxlength="32" required>
    <input id="pass" type="password" placeholder="Password (blank if open)" maxlength="64">
    <button class="sub" type="submit">Save Wi-Fi Credentials</button>
  </form>
  <div id="wmsg" class="msg"></div>
</div>
<script>
function pad(n){return n<10?'0'+n:''+n;}
function tick(){
  var d=new Date();
  document.getElementById('now').textContent=pad(d.getHours())+':'+pad(d.getMinutes())+':'+pad(d.getSeconds());
}
setInterval(tick,500); tick();
function syncNow(){
  var ep=Math.floor(Date.now()/1000);
  var tz=Math.round(-new Date().getTimezoneOffset()/60);
  fetch('/sync?epoch='+ep+'&tz='+tz).then(r=>r.text()).then(t=>{
    document.getElementById('msg').textContent='Synced: '+t;
  }).catch(e=>{document.getElementById('msg').textContent='Error: '+e;});
}
function saveWifi(ev){
  ev.preventDefault();
  var s=encodeURIComponent(document.getElementById('ssid').value);
  var p=encodeURIComponent(document.getElementById('pass').value);
  fetch('/wifi?ssid='+s+'&pass='+p).then(r=>r.text()).then(t=>{
    document.getElementById('wmsg').textContent=t;
  });
}
</script></body></html>)rawhtml";

void write_rtc_from_time_t(time_t utc_sec) {
    struct timeval tv = { .tv_sec = utc_sec, .tv_usec = 0 };
    settimeofday(&tv, nullptr);

    if (!M5.Rtc.isEnabled()) return;
    struct tm local_tm{};
    localtime_r(&utc_sec, &local_tm);

    m5::rtc_datetime_t dt{};
    dt.date.year = (int16_t)(local_tm.tm_year + 1900);
    dt.date.month = (int8_t)(local_tm.tm_mon + 1);
    dt.date.date = (int8_t)local_tm.tm_mday;
    dt.date.weekDay = (int8_t)local_tm.tm_wday;
    dt.time.hours = (int8_t)local_tm.tm_hour;
    dt.time.minutes = (int8_t)local_tm.tm_min;
    dt.time.seconds = (int8_t)local_tm.tm_sec;
    M5.Rtc.setDateTime(dt);
}

void write_rtc_from_local_fields(int year, int month, int day, int hour, int min, int sec) {
    struct tm local_tm{};
    local_tm.tm_year = year - 1900;
    local_tm.tm_mon = month - 1;
    local_tm.tm_mday = day;
    local_tm.tm_hour = hour;
    local_tm.tm_min = min;
    local_tm.tm_sec = sec;
    local_tm.tm_isdst = 0;
    time_t utc_sec = mktime(&local_tm);
    if (utc_sec > 0) {
        write_rtc_from_time_t(utc_sec);
    }
}

void on_ble_time_write_cb(const uint8_t *data, uint16_t len) {
    if (!data || len == 0) return;
    uint16_t n = (len < sizeof(s_ble_buf)) ? len : (uint16_t)sizeof(s_ble_buf);
    portENTER_CRITICAL(&s_sync_mux);
    std::memcpy(s_ble_buf, data, n);
    s_ble_len = n;
    s_ble_pending = true;
    portEXIT_CRITICAL(&s_sync_mux);
}

bool ensure_netif_ready() {
    if (s_netif_ready) return true;
    esp_err_t err = esp_netif_init();
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) return false;
    err = esp_event_loop_create_default();
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) return false;
    s_ap_netif = esp_netif_create_default_wifi_ap();
    s_sta_netif = esp_netif_create_default_wifi_sta();
    s_netif_ready = true;
    return true;
}

esp_err_t http_root_handler(httpd_req_t *req) {
    httpd_resp_set_type(req, "text/html");
    return httpd_resp_send(req, kPortalHtml, HTTPD_RESP_USE_STRLEN);
}

esp_err_t http_sync_handler(httpd_req_t *req) {
    char q[128]{};
    if (httpd_req_get_url_query_str(req, q, sizeof(q)) != ESP_OK) {
        return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Missing query");
    }
    char ep_s[32]{};
    if (httpd_query_key_value(q, "epoch", ep_s, sizeof(ep_s)) != ESP_OK) {
        return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Missing epoch");
    }
    long long ep = std::strtoll(ep_s, nullptr, 10);
    if (ep < 1700000000LL || ep > 4000000000LL) {
        return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Invalid epoch");
    }

    char tz_s[16]{};
    bool has_tz = false;
    int tz_hr = g_settings.tz_offset_hours;
    if (httpd_query_key_value(q, "tz", tz_s, sizeof(tz_s)) == ESP_OK) {
        int v = (int)std::strtol(tz_s, nullptr, 10);
        if (v >= -12 && v <= 14) {
            tz_hr = v;
            has_tz = true;
        }
    }

    portENTER_CRITICAL(&s_sync_mux);
    s_http_epoch = (int64_t)ep;
    s_http_has_tz = has_tz;
    s_http_tz_hours = tz_hr;
    s_http_pending = true;
    portEXIT_CRITICAL(&s_sync_mux);

    httpd_resp_set_type(req, "text/plain");
    return httpd_resp_sendstr(req, "OK");
}

esp_err_t http_wifi_handler(httpd_req_t *req) {
    char q[192]{};
    if (httpd_req_get_url_query_str(req, q, sizeof(q)) != ESP_OK) {
        return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Missing query");
    }
    char ssid[64]{};
    char pass[96]{};
    if (httpd_query_key_value(q, "ssid", ssid, sizeof(ssid)) != ESP_OK) {
        return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Missing ssid");
    }
    httpd_query_key_value(q, "pass", pass, sizeof(pass));
    url_decode_inplace(ssid);
    url_decode_inplace(pass);

    std::strncpy(g_settings.wifi_ssid, ssid, sizeof(g_settings.wifi_ssid) - 1);
    g_settings.wifi_ssid[sizeof(g_settings.wifi_ssid) - 1] = '\0';
    std::strncpy(g_settings.wifi_pass, pass, sizeof(g_settings.wifi_pass) - 1);
    g_settings.wifi_pass[sizeof(g_settings.wifi_pass) - 1] = '\0';
    app_settings_save();

    s_wifi_cfg_saved_notice = true;
    httpd_resp_set_type(req, "text/plain");
    return httpd_resp_sendstr(req, "Saved! Now select 2. Wi-Fi SNTP on StickS3.");
}

void stop_active_transports() {
    if (s_httpd) {
        httpd_stop(s_httpd);
        s_httpd = nullptr;
    }
    if (s_wifi_started) {
        esp_wifi_stop();
        esp_wifi_deinit();
        s_wifi_started = false;
    }
    if (s_ble_sync_started) {
        airmouse_ble_stop_time_sync();
        s_ble_sync_started = false;
    }
}

bool start_ap_portal() {
    stop_active_transports();
    if (!ensure_netif_ready()) return false;

    wifi_init_config_t init_cfg = WIFI_INIT_CONFIG_DEFAULT();
    if (esp_wifi_init(&init_cfg) != ESP_OK) return false;

    wifi_config_t ap_cfg{};
    std::strncpy((char *)ap_cfg.ap.ssid, "StickS3-SYNC", sizeof(ap_cfg.ap.ssid) - 1);
    ap_cfg.ap.ssid_len = (uint8_t)std::strlen("StickS3-SYNC");
    ap_cfg.ap.channel = 6;
    ap_cfg.ap.max_connection = 2;
    ap_cfg.ap.authmode = WIFI_AUTH_OPEN;

    if (esp_wifi_set_mode(WIFI_MODE_AP) != ESP_OK) { esp_wifi_deinit(); return false; }
    if (esp_wifi_set_config(WIFI_IF_AP, &ap_cfg) != ESP_OK) { esp_wifi_deinit(); return false; }
    if (esp_wifi_start() != ESP_OK) { esp_wifi_deinit(); return false; }
    s_wifi_started = true;

    httpd_config_t hcfg = HTTPD_DEFAULT_CONFIG();
    hcfg.lru_purge_enable = true;
    if (httpd_start(&s_httpd, &hcfg) != ESP_OK) {
        stop_active_transports();
        return false;
    }

    httpd_uri_t u_root = { .uri = "/", .method = HTTP_GET, .handler = http_root_handler, .user_ctx = nullptr };
    httpd_uri_t u_sync = { .uri = "/sync", .method = HTTP_GET, .handler = http_sync_handler, .user_ctx = nullptr };
    httpd_uri_t u_wifi = { .uri = "/wifi", .method = HTTP_GET, .handler = http_wifi_handler, .user_ctx = nullptr };
    httpd_register_uri_handler(s_httpd, &u_root);
    httpd_register_uri_handler(s_httpd, &u_sync);
    httpd_register_uri_handler(s_httpd, &u_wifi);
    return true;
}

struct StaSyncCtx {
    EventGroupHandle_t eg;
};

void sta_event_cb(void *arg, esp_event_base_t base, int32_t id, void *data) {
    (void)data;
    auto *ctx = (StaSyncCtx *)arg;
    if (base == WIFI_EVENT && id == WIFI_EVENT_STA_START) {
        esp_wifi_connect();
    } else if (base == WIFI_EVENT && id == WIFI_EVENT_STA_DISCONNECTED) {
        xEventGroupSetBits(ctx->eg, BIT1);
    } else if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP) {
        xEventGroupSetBits(ctx->eg, BIT0);
    }
}

bool run_sta_sntp(const char *ssid, const char *pass, M5Canvas &canvas) {
    stop_active_transports();
    if (!ssid || ssid[0] == '\0') return false;
    if (!ensure_netif_ready()) return false;

    wifi_init_config_t init_cfg = WIFI_INIT_CONFIG_DEFAULT();
    if (esp_wifi_init(&init_cfg) != ESP_OK) return false;

    EventGroupHandle_t eg = xEventGroupCreate();
    StaSyncCtx ctx{eg};
    esp_event_handler_instance_t inst_wifi = nullptr;
    esp_event_handler_instance_t inst_ip = nullptr;
    esp_event_handler_instance_register(WIFI_EVENT, ESP_EVENT_ANY_ID, &sta_event_cb, &ctx, &inst_wifi);
    esp_event_handler_instance_register(IP_EVENT, IP_EVENT_STA_GOT_IP, &sta_event_cb, &ctx, &inst_ip);

    wifi_config_t sta_cfg{};
    std::strncpy((char *)sta_cfg.sta.ssid, ssid, sizeof(sta_cfg.sta.ssid) - 1);
    if (pass && pass[0] != '\0') {
        std::strncpy((char *)sta_cfg.sta.password, pass, sizeof(sta_cfg.sta.password) - 1);
    } else {
        sta_cfg.sta.threshold.authmode = WIFI_AUTH_OPEN;
    }

    esp_wifi_set_mode(WIFI_MODE_STA);
    esp_wifi_set_config(WIFI_IF_STA, &sta_cfg);
    esp_wifi_start();
    s_wifi_started = true;

    canvas.fillScreen(0x0B0F17);
    ui_draw_header(canvas, "WI-FI SNTP", 0x1D283A, 0x80D8FF);
    canvas.setFont(&fonts::Font2);
    canvas.setTextColor(0xE2E8F0, 0x0B0F17);
    canvas.setCursor(10, 64);
    canvas.print("Connecting Wi-Fi");
    canvas.setCursor(10, 84);
    canvas.print(ssid);
    canvas.pushSprite(0, 0);

    EventBits_t bits = xEventGroupWaitBits(eg, BIT0 | BIT1, pdFALSE, pdFALSE, pdMS_TO_TICKS(8000));
    bool ok = false;
    if (bits & BIT0) {
        canvas.setTextColor(0x80D8FF, 0x0B0F17);
        canvas.setCursor(10, 114);
        canvas.print("Querying NTP...");
        canvas.pushSprite(0, 0);

        esp_sntp_config_t sntp_cfg = ESP_NETIF_SNTP_DEFAULT_CONFIG("pool.ntp.org");
        if (esp_netif_sntp_init(&sntp_cfg) == ESP_OK) {
            if (esp_netif_sntp_sync_wait(pdMS_TO_TICKS(6000)) == ESP_OK) {
                time_t now = std::time(nullptr);
                if (now >= 1700000000LL) {
                    write_rtc_from_time_t(now);
                    ok = true;
                }
            }
            esp_netif_sntp_deinit();
        }
    }

    esp_event_handler_instance_unregister(WIFI_EVENT, ESP_EVENT_ANY_ID, inst_wifi);
    esp_event_handler_instance_unregister(IP_EVENT, IP_EVENT_STA_GOT_IP, inst_ip);
    vEventGroupDelete(eg);
    stop_active_transports();
    return ok;
}

} // namespace

void time_sync_apply_tz() {
    char tz[16];
    format_posix_tz(g_settings.tz_offset_hours, tz, sizeof(tz));
    setenv("TZ", tz, 1);
    tzset();
}

void time_sync_init_from_rtc() {
    time_sync_apply_tz();
    if (!M5.Rtc.isEnabled()) {
        ESP_LOGW(kTag, "RX8130 RTC not detected");
        return;
    }

    auto dt = M5.Rtc.getDateTime();
    if (dt.date.year >= 2024 && dt.date.year <= 2099 &&
        dt.date.month >= 1 && dt.date.month <= 12 &&
        dt.date.date >= 1 && dt.date.date <= 31) {
        struct tm t{};
        t.tm_year = dt.date.year - 1900;
        t.tm_mon = dt.date.month - 1;
        t.tm_mday = dt.date.date;
        t.tm_hour = dt.time.hours;
        t.tm_min = dt.time.minutes;
        t.tm_sec = dt.time.seconds;
        t.tm_isdst = 0;
        time_t epoch = mktime(&t);
        if (epoch >= 1700000000LL) {
            struct timeval tv = { .tv_sec = epoch, .tv_usec = 0 };
            settimeofday(&tv, nullptr);
            ESP_LOGI(kTag, "RTC restored %04d-%02d-%02d %02d:%02d:%02d",
                     dt.date.year, dt.date.month, dt.date.date,
                     dt.time.hours, dt.time.minutes, dt.time.seconds);
        }
    }
}

bool time_sync_is_valid() {
    return std::time(nullptr) >= 1700000000LL;
}

void time_sync_ui_run() {
    M5Canvas canvas(&M5.Display);
    canvas.createSprite(M5.Display.width(), M5.Display.height());
    input_wait_all_release();

    int sel = 0;
    const char *methods[3] = {
        "1. Wi-Fi AP Push",
        "2. Wi-Fi SNTP",
        "3. BLE Time Sync",
    };

    const char *banner_msg = "Press B to start";
    uint32_t banner_col = 0xB0BEC5;
    int64_t last_draw_ms = 0;

    while (true) {
        input_update();
        int64_t now_ms = input_now_ms();

        // Check pending HTTP push update (AP portal)
        if (s_http_pending) {
            int64_t ep = 0;
            bool has_tz = false;
            int tz_hr = 9;
            portENTER_CRITICAL(&s_sync_mux);
            ep = s_http_epoch;
            has_tz = s_http_has_tz;
            tz_hr = s_http_tz_hours;
            s_http_pending = false;
            portEXIT_CRITICAL(&s_sync_mux);

            if (has_tz) {
                g_settings.tz_offset_hours = tz_hr;
                app_settings_save();
                time_sync_apply_tz();
            }
            write_rtc_from_time_t((time_t)ep);
            sound_fx_confirm();
            banner_msg = "AP TIME SYNCED!";
            banner_col = 0x69F0AE;
            last_draw_ms = 0;
        }

        if (s_wifi_cfg_saved_notice) {
            s_wifi_cfg_saved_notice = false;
            sound_fx_confirm();
            banner_msg = "Wi-Fi Saved!";
            banner_col = 0x69F0AE;
            last_draw_ms = 0;
        }

        // Check pending BLE CTS / GATT write
        if (s_ble_pending) {
            uint8_t buf[32]{};
            uint16_t len = 0;
            portENTER_CRITICAL(&s_sync_mux);
            len = s_ble_len;
            std::memcpy(buf, s_ble_buf, len);
            s_ble_pending = false;
            portEXIT_CRITICAL(&s_sync_mux);

            ParsedTimePayload p = parse_ble_time_payload(buf, len);
            if (p.kind == TimePayloadKind::LocalFields) {
                write_rtc_from_local_fields(p.year, p.month, p.day, p.hour, p.min, p.sec);
                sound_fx_confirm();
                banner_msg = "BLE TIME SYNCED!";
                banner_col = 0x69F0AE;
            } else if (p.kind == TimePayloadKind::UnixEpochUtc) {
                write_rtc_from_time_t((time_t)p.epoch_sec);
                sound_fx_confirm();
                banner_msg = "BLE TIME SYNCED!";
                banner_col = 0x69F0AE;
            } else {
                sound_fx_click();
                banner_msg = "Invalid BLE data";
                banner_col = 0xFF8A80;
            }
            last_draw_ms = 0;
        }

        if (input_ab_held(500)) {
            stop_active_transports();
            sound_fx_click();
            input_wait_ab_release();
            canvas.deleteSprite();
            return;
        }

        if (input_a_pressed()) {
            sel = (sel + 1) % 3;
            stop_active_transports();
            banner_msg = "Press B to start";
            banner_col = 0xB0BEC5;
            sound_fx_click();
            last_draw_ms = 0;
        }

        if (input_b_pressed()) {
            sound_fx_confirm();
            if (sel == 0) {
                if (start_ap_portal()) {
                    banner_msg = "AP ACTIVE";
                    banner_col = 0x80D8FF;
                } else {
                    banner_msg = "AP start failed";
                    banner_col = 0xFF8A80;
                }
            } else if (sel == 1) {
                if (g_settings.wifi_ssid[0] == '\0') {
                    banner_msg = "Set Wi-Fi in AP";
                    banner_col = 0xFFD54F;
                } else if (run_sta_sntp(g_settings.wifi_ssid, g_settings.wifi_pass, canvas)) {
                    sound_fx_confirm();
                    banner_msg = "NTP SYNCED!";
                    banner_col = 0x69F0AE;
                } else {
                    banner_msg = "NTP sync failed";
                    banner_col = 0xFF8A80;
                }
            } else if (sel == 2) {
                stop_active_transports();
                if (airmouse_ble_start_time_sync(on_ble_time_write_cb)) {
                    s_ble_sync_started = true;
                    banner_msg = "BLE ADVERTISING";
                    banner_col = 0x80D8FF;
                } else {
                    banner_msg = "BLE init failed";
                    banner_col = 0xFF8A80;
                }
            }
            last_draw_ms = 0;
        }

        if ((now_ms - last_draw_ms) >= 200) {
            last_draw_ms = now_ms;
            int w = canvas.width();
            canvas.fillScreen(0x0B0F17);
            ui_draw_header(canvas, "TIME SYNC", 0x1D283A, 0x80D8FF);

            // Live RX8130 / System Clock card
            time_t now = std::time(nullptr);
            struct tm tm_now{};
            localtime_r(&now, &tm_now);
            bool synced = (now >= 1700000000LL);

            canvas.drawRoundRect(4, 24, w - 8, 52, 5, 0x263547);
            canvas.fillRoundRect(5, 25, w - 10, 50, 4, 0x101824);

            canvas.setFont(&fonts::Font2);
            canvas.setTextSize(1);
            canvas.setTextWrap(false);
            canvas.setTextColor(0x90A4AE, 0x101824);
            canvas.setCursor(9, 26);
            canvas.printf("RTC UTC%+d", g_settings.tz_offset_hours);

            canvas.setTextColor(synced ? 0x69F0AE : 0xFFAB40, 0x101824);
            const char *sflag = synced ? "SYNCED" : "UNSET";
            canvas.setCursor(w - canvas.textWidth(sflag) - 9, 26);
            canvas.print(sflag);

            char dbuf[20], tbuf[16];
            std::snprintf(dbuf, sizeof(dbuf), "%04d-%02d-%02d",
                          tm_now.tm_year + 1900, tm_now.tm_mon + 1, tm_now.tm_mday);
            std::snprintf(tbuf, sizeof(tbuf), "%02d:%02d:%02d",
                          tm_now.tm_hour, tm_now.tm_min, tm_now.tm_sec);

            canvas.setTextColor(0xFFFFFF, 0x101824);
            canvas.setCursor(9, 41);
            canvas.print(dbuf);

            canvas.setTextColor(synced ? 0x69F0AE : 0xCFD8DC, 0x101824);
            canvas.setCursor(9, 57);
            canvas.print(tbuf);

            // 3 Methods list
            int y = 80;
            for (int i = 0; i < 3; i++) {
                bool is_sel = (i == sel);
                uint32_t bg = is_sel ? 0x1976D2 : 0x111824;
                uint32_t fg = is_sel ? 0xFFFFFF : 0xCFD8DC;
                canvas.fillRoundRect(4, y, w - 8, 21, 3, bg);
                canvas.setFont(&fonts::Font2);
                canvas.setTextColor(fg, bg);
                canvas.setCursor(8, y + 2);
                canvas.print(methods[i]);
                y += 23;
            }

            // Dynamic Transport Status Box
            canvas.drawRoundRect(4, 151, w - 8, 56, 5, 0x29B6F6);
            canvas.fillRoundRect(5, 152, w - 10, 54, 4, 0x0B1B29);
            canvas.setFont(&fonts::Font2);
            canvas.setTextColor(banner_col, 0x0B1B29);
            canvas.setCursor(9, 154);
            canvas.print(banner_msg);

            canvas.setTextColor(0xE2E8F0, 0x0B1B29);
            if (sel == 0) {
                canvas.setCursor(9, 171);
                canvas.print("SSID: StickS3-SYNC");
                canvas.setCursor(9, 187);
                canvas.print("Open: 192.168.4.1");
            } else if (sel == 1) {
                canvas.setCursor(9, 171);
                if (g_settings.wifi_ssid[0] != '\0') {
                    char sb[24];
                    std::snprintf(sb, sizeof(sb), "STA:%s", g_settings.wifi_ssid);
                    canvas.print(sb);
                } else {
                    canvas.print("STA: (Use AP to set)");
                }
                canvas.setCursor(9, 187);
                canvas.print("NTP: pool.ntp.org");
            } else {
                canvas.setCursor(9, 171);
                canvas.print("DEV: StickS3-Time");
                canvas.setCursor(9, 187);
                canvas.print("CTS: 0x1805/0x2A2B");
            }

            ui_draw_footer2(canvas, "A: Select  B: Start", "Hold A+B : Go Back");
            canvas.pushSprite(0, 0);
        }

        vTaskDelay(pdMS_TO_TICKS(25));
    }
}
