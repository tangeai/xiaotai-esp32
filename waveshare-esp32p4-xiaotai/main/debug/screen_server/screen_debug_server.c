#include "screen_debug_server.h"

#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/time.h>
#include <sys/types.h>

#include "esp_check.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/idf_additions.h"
#include "freertos/portmacro.h"
#include "freertos/task.h"
#include "lwip/inet.h"
#include "lwip/sockets.h"

#include "app_task_affinity.h"
#include "camera_pipeline.h"
#include "display.h"
#include "display_driver.h"
#include "network.h"

static const char *TAG = "screen_debug";

#ifndef CONFIG_APP_DEBUG_SCREEN_SERVER_PORT
#define CONFIG_APP_DEBUG_SCREEN_SERVER_PORT 8080
#endif

#define SCREEN_DEBUG_TASK_STACK_SIZE (6 * 1024)
#define SCREEN_DEBUG_TASK_PRIORITY 4
#define SCREEN_DEBUG_LISTEN_BACKLOG 2
#define SCREEN_DEBUG_REQUEST_MAX 512
#define SCREEN_DEBUG_TARGET_MAX 192
#define SCREEN_DEBUG_VALUE_MAX 32
#define SCREEN_DEBUG_JSON_STRING_MAX 96
#define SCREEN_DEBUG_RECV_TIMEOUT_SEC 2

static TaskHandle_t s_task;
static int s_listen_fd = -1;
static bool s_stop_requested;
static portMUX_TYPE s_lock = portMUX_INITIALIZER_UNLOCKED;

static const char s_index_html[] =
    "<!doctype html><html><head><meta charset=\"utf-8\">"
    "<meta name=\"viewport\" content=\"width=device-width,initial-scale=1\">"
    "<title>ESP32-P4 Screen Debug</title>"
    "<style>"
    "body{margin:0;background:#111827;color:#e5e7eb;font-family:system-ui,Segoe UI,sans-serif;}"
    "main{max-width:760px;margin:0 auto;padding:18px;}"
    ".top{display:flex;gap:12px;align-items:center;justify-content:space-between;flex-wrap:wrap;}"
    "h1{font-size:20px;margin:0;font-weight:650;}"
    ".meta{font-size:13px;color:#9ca3af;}"
    ".screen-wrap{margin-top:16px;background:#000;border:1px solid #374151;display:inline-block;line-height:0;}"
    "#screen{width:min(100%,640px);height:auto;image-rendering:auto;touch-action:none;cursor:crosshair;}"
    ".metrics{margin-top:10px;padding:10px 12px;border:1px solid #374151;background:#182033;color:#d1d5db;font-size:13px;border-radius:6px;line-height:1.5;}"
    ".bar{margin-top:12px;display:flex;gap:8px;align-items:center;flex-wrap:wrap;}"
    "button{background:#2563eb;color:#fff;border:0;border-radius:6px;padding:8px 12px;font-size:14px;}"
    "button:active{background:#1d4ed8;}"
    ".hint{font-size:13px;color:#9ca3af;}"
    "</style></head><body><main>"
    "<div class=\"top\"><h1>ESP32-P4 Screen Debug</h1><div id=\"status\" class=\"meta\">Connecting...</div></div>"
    "<div id=\"camera\" class=\"metrics\">Camera: idle</div>"
    "<div class=\"screen-wrap\"><img id=\"screen\" src=\"/screen.bmp\" alt=\"device screen\"></div>"
    "<div class=\"bar\"><button id=\"refresh\">Refresh</button><button id=\"pause\">Pause</button>"
    "<span class=\"hint\">Click the image to tap. Use wheel or touch drag to scroll.</span></div>"
    "</main><script>"
    "const img=document.getElementById('screen');"
    "const statusEl=document.getElementById('status');"
    "const cameraEl=document.getElementById('camera');"
    "const refreshBtn=document.getElementById('refresh');"
    "const pauseBtn=document.getElementById('pause');"
    "let paused=false,loading=false,w=480,h=320,lastTouch=null,cameraActive=false;"
    "function refresh(){if(paused||loading||cameraActive)return;loading=true;img.src='/screen.bmp?t='+Date.now();}"
    "function point(e){const r=img.getBoundingClientRect();return{"
    "x:Math.max(0,Math.min(w-1,Math.round((e.clientX-r.left)*w/r.width))),"
    "y:Math.max(0,Math.min(h-1,Math.round((e.clientY-r.top)*h/r.height)))}}"
    "function qs(o){return Object.keys(o).map(k=>k+'='+encodeURIComponent(o[k])).join('&')}"
    "function api(path,o){return fetch(path+'?'+qs(o),{cache:'no-store'}).then(()=>setTimeout(refresh,80))}"
    "img.addEventListener('click',e=>{const p=point(e);api('/api/tap',p)});"
    "img.addEventListener('wheel',e=>{e.preventDefault();const p=point(e);"
    "api('/api/scroll',{x:p.x,y:p.y,dx:Math.round(e.deltaX),dy:Math.round(e.deltaY)})},{passive:false});"
    "img.addEventListener('touchstart',e=>{if(e.touches.length===1)lastTouch=point(e.touches[0])},{passive:false});"
    "img.addEventListener('touchmove',e=>{if(e.touches.length!==1||!lastTouch)return;e.preventDefault();"
    "const p=point(e.touches[0]);const dx=p.x-lastTouch.x,dy=p.y-lastTouch.y;"
    "if(Math.abs(dx)+Math.abs(dy)>8){api('/api/scroll',{x:p.x,y:p.y,dx:dx,dy:dy});lastTouch=p;}},{passive:false});"
    "img.addEventListener('touchend',()=>{lastTouch=null});"
    "img.onload=()=>{loading=false};img.onerror=()=>{loading=false};"
    "refreshBtn.onclick=()=>{loading=false;refresh()};"
    "pauseBtn.onclick=()=>{paused=!paused;pauseBtn.textContent=paused?'Resume':'Pause'};"
    "function cameraText(c){if(!c||!c.running)return'Camera: idle';"
    "const fps=c.fps_x10?((c.fps_x10/10).toFixed(1)):'--';"
    "const br=c.bitrate_kbps?c.bitrate_kbps+' kbps':'-- kbps';"
    "const cfg=c.configured_bitrate_kbps?c.configured_bitrate_kbps+' kbps':'-- kbps';"
    "return'Camera: '+c.width+'x'+c.height+' | '+fps+' fps | '+br+' | target '+c.target_fps+' fps / '+cfg+' | '+(c.direct?'direct':'scaled');}"
    "function status(){fetch('/api/status',{cache:'no-store'}).then(r=>r.json()).then(s=>{w=s.width;h=s.height;"
    "cameraActive=!!(s.camera&&s.camera.enabled);"
    "statusEl.textContent=(s.ip||'no ip')+' '+w+'x'+h+' port '+s.port+(cameraActive?' | capture paused':'');"
    "cameraEl.textContent=cameraText(s.camera);}).catch(()=>{cameraActive=false;statusEl.textContent='offline';cameraEl.textContent='Camera: offline'});}"
    "setInterval(refresh,1500);setInterval(status,2000);status();refresh();"
    "</script></body></html>";

static bool screen_debug_should_stop(void)
{
    taskENTER_CRITICAL(&s_lock);
    bool stop_requested = s_stop_requested;
    taskEXIT_CRITICAL(&s_lock);
    return stop_requested;
}

static void screen_debug_set_listen_fd(int fd)
{
    taskENTER_CRITICAL(&s_lock);
    s_listen_fd = fd;
    taskEXIT_CRITICAL(&s_lock);
}

static void screen_debug_task_finished(int listen_fd)
{
    bool close_listen = false;

    taskENTER_CRITICAL(&s_lock);
    if (s_listen_fd == listen_fd) {
        s_listen_fd = -1;
        close_listen = true;
    }
    s_task = NULL;
    s_stop_requested = false;
    taskEXIT_CRITICAL(&s_lock);

    if (close_listen && listen_fd >= 0) {
        close(listen_fd);
    }
}

static esp_err_t screen_debug_send_all(int fd, const void *data, size_t len)
{
    const uint8_t *cursor = (const uint8_t *)data;
    size_t remaining = len;

    while (remaining > 0) {
        ssize_t sent = send(fd, cursor, remaining, 0);
        if (sent < 0) {
            if (errno == EINTR) {
                continue;
            }
            return ESP_FAIL;
        }
        if (sent == 0) {
            return ESP_FAIL;
        }
        cursor += sent;
        remaining -= (size_t)sent;
    }
    return ESP_OK;
}

static esp_err_t screen_debug_send_header(int fd,
                                          int status_code,
                                          const char *status_text,
                                          const char *content_type,
                                          size_t content_length)
{
    char header[256];
    int len = snprintf(header,
                       sizeof(header),
                       "HTTP/1.1 %d %s\r\n"
                       "Content-Type: %s\r\n"
                       "Content-Length: %u\r\n"
                       "Cache-Control: no-store, no-cache, must-revalidate, max-age=0\r\n"
                       "Pragma: no-cache\r\n"
                       "Access-Control-Allow-Origin: *\r\n"
                       "Connection: close\r\n"
                       "\r\n",
                       status_code,
                       status_text,
                       content_type,
                       (unsigned)content_length);
    if (len <= 0 || len >= (int)sizeof(header)) {
        return ESP_ERR_INVALID_SIZE;
    }
    return screen_debug_send_all(fd, header, (size_t)len);
}

static esp_err_t screen_debug_send_body(int fd,
                                        int status_code,
                                        const char *status_text,
                                        const char *content_type,
                                        const void *body,
                                        size_t body_len)
{
    ESP_RETURN_ON_ERROR(screen_debug_send_header(fd, status_code, status_text, content_type, body_len),
                        TAG,
                        "send header failed");
    return screen_debug_send_all(fd, body, body_len);
}

static esp_err_t screen_debug_send_json(int fd, int status_code, const char *status_text, const char *json)
{
    return screen_debug_send_body(fd,
                                  status_code,
                                  status_text,
                                  "application/json",
                                  json,
                                  strlen(json));
}

static esp_err_t screen_debug_send_error(int fd, int status_code, const char *status_text, const char *message)
{
    char body[128];
    int len = snprintf(body,
                       sizeof(body),
                       "{\"ok\":false,\"message\":\"%s\"}",
                       message != NULL ? message : "error");
    if (len <= 0 || len >= (int)sizeof(body)) {
        return ESP_ERR_INVALID_SIZE;
    }
    return screen_debug_send_body(fd,
                                  status_code,
                                  status_text,
                                  "application/json",
                                  body,
                                  (size_t)len);
}

static void screen_debug_json_escape(const char *src, char *dst, size_t dst_size)
{
    if (dst == NULL || dst_size == 0) {
        return;
    }
    if (src == NULL) {
        dst[0] = '\0';
        return;
    }

    size_t out = 0;
    while (*src != '\0' && out + 1 < dst_size) {
        char ch = *src++;
        if ((ch == '"' || ch == '\\') && out + 2 < dst_size) {
            dst[out++] = '\\';
            dst[out++] = ch;
        } else if ((unsigned char)ch >= 0x20) {
            dst[out++] = ch;
        }
    }
    dst[out] = '\0';
}

static esp_err_t screen_debug_read_request(int fd, char *request, size_t request_size)
{
    if (request == NULL || request_size < 2) {
        return ESP_ERR_INVALID_ARG;
    }

    size_t used = 0;
    while (used + 1 < request_size) {
        ssize_t got = recv(fd, request + used, request_size - used - 1, 0);
        if (got < 0) {
            if (errno == EINTR) {
                continue;
            }
            return ESP_FAIL;
        }
        if (got == 0) {
            break;
        }
        used += (size_t)got;
        request[used] = '\0';
        if (strstr(request, "\r\n\r\n") != NULL || strstr(request, "\n\n") != NULL) {
            return ESP_OK;
        }
    }

    request[used] = '\0';
    return used > 0 ? ESP_OK : ESP_FAIL;
}

static esp_err_t screen_debug_parse_request(char *request,
                                            char *path,
                                            size_t path_size,
                                            char *query,
                                            size_t query_size)
{
    char method[8];
    char target[SCREEN_DEBUG_TARGET_MAX];
    char version[16];

    if (sscanf(request, "%7s %191s %15s", method, target, version) != 3) {
        return ESP_ERR_INVALID_ARG;
    }
    if (strcmp(method, "GET") != 0) {
        return ESP_ERR_NOT_SUPPORTED;
    }

    char *query_start = strchr(target, '?');
    if (query_start != NULL) {
        *query_start = '\0';
        size_t query_len = strlen(query_start + 1);
        if (query_len >= query_size) {
            return ESP_ERR_INVALID_SIZE;
        }
        memcpy(query, query_start + 1, query_len + 1);
    } else {
        if (query_size == 0) {
            return ESP_ERR_INVALID_ARG;
        }
        query[0] = '\0';
    }

    size_t path_len = strlen(target);
    if (path_len == 0 || path_len >= path_size) {
        return ESP_ERR_INVALID_SIZE;
    }
    memcpy(path, target, path_len + 1);
    return ESP_OK;
}

static esp_err_t screen_debug_get_int_arg(const char *query, const char *name, int *value)
{
    if (query == NULL || name == NULL || value == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    const size_t name_len = strlen(name);
    const char *cursor = query;
    while (*cursor != '\0') {
        const char *item_end = strchr(cursor, '&');
        if (item_end == NULL) {
            item_end = cursor + strlen(cursor);
        }

        const char *equals = memchr(cursor, '=', (size_t)(item_end - cursor));
        if (equals != NULL &&
            (size_t)(equals - cursor) == name_len &&
            strncmp(cursor, name, name_len) == 0) {
            size_t raw_len = (size_t)(item_end - equals - 1);
            if (raw_len == 0 || raw_len >= SCREEN_DEBUG_VALUE_MAX) {
                return ESP_ERR_INVALID_SIZE;
            }

            char raw[SCREEN_DEBUG_VALUE_MAX];
            memcpy(raw, equals + 1, raw_len);
            raw[raw_len] = '\0';

            char *end = NULL;
            long parsed = strtol(raw, &end, 10);
            if (end == raw || *end != '\0') {
                return ESP_ERR_INVALID_ARG;
            }

            *value = (int)parsed;
            return ESP_OK;
        }

        cursor = *item_end == '&' ? item_end + 1 : item_end;
    }

    return ESP_ERR_NOT_FOUND;
}

static esp_err_t screen_debug_handle_index(int fd)
{
    return screen_debug_send_body(fd,
                                  200,
                                  "OK",
                                  "text/html; charset=utf-8",
                                  s_index_html,
                                  sizeof(s_index_html) - 1);
}

static esp_err_t screen_debug_handle_status(int fd)
{
    network_state_t network = {0};
    camera_pipeline_metrics_t camera = {0};
    char ip[SCREEN_DEBUG_JSON_STRING_MAX];
    char ssid[SCREEN_DEBUG_JSON_STRING_MAX];
    char body[880];

    network_get_state(&network);
    camera_pipeline_get_metrics(&camera);
    screen_debug_json_escape(network.ip_addr, ip, sizeof(ip));
    screen_debug_json_escape(network.ssid, ssid, sizeof(ssid));

    int len = snprintf(body,
                       sizeof(body),
                       "{\"ok\":true,\"connected\":%s,\"ip\":\"%s\",\"ssid\":\"%s\","
                       "\"width\":%u,\"height\":%u,\"port\":%u,\"uptime_ms\":%llu,"
                       "\"memory\":{\"internal_free\":%u,\"internal_largest\":%u,"
                       "\"dma_free\":%u,\"dma_largest\":%u,\"psram_free\":%u,"
                       "\"psram_largest\":%u},"
                       "\"camera\":{\"running\":%s,\"enabled\":%s,\"width\":%u,\"height\":%u,"
                       "\"target_fps\":%u,\"fps_x10\":%lu,\"bitrate_kbps\":%lu,"
                       "\"configured_bitrate_kbps\":%lu,\"avg_payload\":%lu,"
                       "\"drop\":%lu,\"cap_fail\":%lu,\"enc_fail\":%lu,\"direct\":%s}}",
                       network.connected ? "true" : "false",
                       ip,
                       ssid,
                       (unsigned)display_driver_width(),
                       (unsigned)display_driver_height(),
                       (unsigned)CONFIG_APP_DEBUG_SCREEN_SERVER_PORT,
                       (unsigned long long)(esp_timer_get_time() / 1000ULL),
                       (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT),
                       (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT),
                       (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_DMA),
                       (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL | MALLOC_CAP_DMA),
                       (unsigned)heap_caps_get_free_size(MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT),
                       (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT),
                       camera.running ? "true" : "false",
                       camera.rtc_enabled ? "true" : "false",
                       (unsigned)camera.width,
                       (unsigned)camera.height,
                       (unsigned)camera.target_fps,
                       (unsigned long)camera.measured_fps_x10,
                       (unsigned long)camera.measured_bitrate_kbps,
                       (unsigned long)(camera.configured_bitrate_bps / 1000U),
                       (unsigned long)camera.avg_payload_bytes,
                       (unsigned long)camera.dropped_frames,
                       (unsigned long)camera.capture_failures,
                       (unsigned long)camera.encode_failures,
                       camera.direct_input ? "true" : "false");
    if (len <= 0 || len >= (int)sizeof(body)) {
        return screen_debug_send_error(fd, 500, "Internal Server Error", "status too large");
    }

    return screen_debug_send_body(fd, 200, "OK", "application/json", body, (size_t)len);
}

static esp_err_t screen_debug_handle_bmp(int fd)
{
    uint8_t *bmp_data = NULL;
    size_t bmp_size = 0;

    if (camera_pipeline_is_rtc_video_active()) {
        return screen_debug_send_error(fd,
                                       409,
                                       "Conflict",
                                       "screen capture paused while RTC video is active");
    }

    esp_err_t ret = display_capture_bmp(&bmp_data, &bmp_size);
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "capture screen failed: %s", esp_err_to_name(ret));
        return screen_debug_send_error(fd, 503, "Service Unavailable", esp_err_to_name(ret));
    }

    ret = screen_debug_send_body(fd, 200, "OK", "image/bmp", bmp_data, bmp_size);
    free(bmp_data);
    return ret;
}

static esp_err_t screen_debug_handle_tap(int fd, const char *query)
{
    int x = 0;
    int y = 0;

    if (screen_debug_get_int_arg(query, "x", &x) != ESP_OK ||
        screen_debug_get_int_arg(query, "y", &y) != ESP_OK) {
        return screen_debug_send_error(fd, 400, "Bad Request", "missing x or y");
    }
    if (x < 0 || y < 0 ||
        x >= (int)display_driver_width() ||
        y >= (int)display_driver_height()) {
        return screen_debug_send_error(fd, 400, "Bad Request", "tap out of range");
    }

    esp_err_t ret = display_debug_tap_async((uint16_t)x, (uint16_t)y);
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "tap dispatch failed: x=%d y=%d ret=%s", x, y, esp_err_to_name(ret));
        return screen_debug_send_error(fd, 503, "Service Unavailable", esp_err_to_name(ret));
    }

    return screen_debug_send_json(fd, 200, "OK", "{\"ok\":true}");
}

static esp_err_t screen_debug_handle_scroll(int fd, const char *query)
{
    int x = 0;
    int y = 0;
    int dx = 0;
    int dy = 0;

    if (screen_debug_get_int_arg(query, "x", &x) != ESP_OK ||
        screen_debug_get_int_arg(query, "y", &y) != ESP_OK ||
        screen_debug_get_int_arg(query, "dx", &dx) != ESP_OK ||
        screen_debug_get_int_arg(query, "dy", &dy) != ESP_OK) {
        return screen_debug_send_error(fd, 400, "Bad Request", "missing scroll args");
    }
    if (x < 0 || y < 0 ||
        x >= (int)display_driver_width() ||
        y >= (int)display_driver_height() ||
        dx < INT16_MIN || dx > INT16_MAX ||
        dy < INT16_MIN || dy > INT16_MAX) {
        return screen_debug_send_error(fd, 400, "Bad Request", "scroll out of range");
    }

    esp_err_t ret = display_debug_scroll_async((uint16_t)x, (uint16_t)y, (int16_t)dx, (int16_t)dy);
    if (ret != ESP_OK) {
        ESP_LOGW(TAG,
                 "scroll dispatch failed: x=%d y=%d dx=%d dy=%d ret=%s",
                 x,
                 y,
                 dx,
                 dy,
                 esp_err_to_name(ret));
        return screen_debug_send_error(fd, 503, "Service Unavailable", esp_err_to_name(ret));
    }

    return screen_debug_send_json(fd, 200, "OK", "{\"ok\":true}");
}

static void screen_debug_handle_client(int fd)
{
    char request[SCREEN_DEBUG_REQUEST_MAX];
    char path[SCREEN_DEBUG_TARGET_MAX];
    char query[SCREEN_DEBUG_TARGET_MAX];

    struct timeval timeout = {
        .tv_sec = SCREEN_DEBUG_RECV_TIMEOUT_SEC,
        .tv_usec = 0,
    };
    (void)setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));
    (void)setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &timeout, sizeof(timeout));

    esp_err_t ret = screen_debug_read_request(fd, request, sizeof(request));
    if (ret != ESP_OK) {
        ESP_LOGD(TAG, "read request failed: %s", esp_err_to_name(ret));
        return;
    }

    ret = screen_debug_parse_request(request, path, sizeof(path), query, sizeof(query));
    if (ret == ESP_ERR_NOT_SUPPORTED) {
        (void)screen_debug_send_error(fd, 405, "Method Not Allowed", "only GET is supported");
        return;
    }
    if (ret != ESP_OK) {
        (void)screen_debug_send_error(fd, 400, "Bad Request", "invalid request");
        return;
    }

    if (strcmp(path, "/") == 0) {
        ret = screen_debug_handle_index(fd);
    } else if (strcmp(path, "/screen.bmp") == 0) {
        ret = screen_debug_handle_bmp(fd);
    } else if (strcmp(path, "/api/status") == 0) {
        ret = screen_debug_handle_status(fd);
    } else if (strcmp(path, "/api/tap") == 0) {
        ret = screen_debug_handle_tap(fd, query);
    } else if (strcmp(path, "/api/scroll") == 0) {
        ret = screen_debug_handle_scroll(fd, query);
    } else {
        ret = screen_debug_send_error(fd, 404, "Not Found", "not found");
    }

    if (ret != ESP_OK) {
        ESP_LOGD(TAG, "response failed: path=%s ret=%s errno=%d", path, esp_err_to_name(ret), errno);
    }
}

static esp_err_t screen_debug_create_listen_socket(int *listen_fd)
{
    if (listen_fd == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    int fd = socket(AF_INET, SOCK_STREAM, IPPROTO_IP);
    if (fd < 0) {
        ESP_LOGE(TAG, "socket failed: errno=%d", errno);
        return ESP_FAIL;
    }

    int reuse = 1;
    (void)setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse));

    struct sockaddr_in address = {
        .sin_family = AF_INET,
        .sin_port = htons(CONFIG_APP_DEBUG_SCREEN_SERVER_PORT),
        .sin_addr.s_addr = htonl(INADDR_ANY),
    };

    if (bind(fd, (const struct sockaddr *)&address, sizeof(address)) != 0) {
        ESP_LOGE(TAG, "bind failed: port=%u errno=%d", (unsigned)CONFIG_APP_DEBUG_SCREEN_SERVER_PORT, errno);
        close(fd);
        return ESP_FAIL;
    }
    if (listen(fd, SCREEN_DEBUG_LISTEN_BACKLOG) != 0) {
        ESP_LOGE(TAG, "listen failed: errno=%d", errno);
        close(fd);
        return ESP_FAIL;
    }

    *listen_fd = fd;
    return ESP_OK;
}

static void screen_debug_log_started(void)
{
    network_state_t network = {0};
    network_get_state(&network);
    if (network.connected && network.ip_addr[0] != '\0') {
        ESP_LOGI(TAG,
                 "screen debug server started: http://%s:%u/",
                 network.ip_addr,
                 (unsigned)CONFIG_APP_DEBUG_SCREEN_SERVER_PORT);
    } else {
        ESP_LOGI(TAG,
                 "screen debug server started on port %u, waiting for WiFi",
                 (unsigned)CONFIG_APP_DEBUG_SCREEN_SERVER_PORT);
    }
}

static void screen_debug_task(void *arg)
{
    (void)arg;

    int listen_fd = -1;
    esp_err_t ret = screen_debug_create_listen_socket(&listen_fd);
    if (ret != ESP_OK) {
        screen_debug_task_finished(listen_fd);
        vTaskDeleteWithCaps(NULL);
        return;
    }

    screen_debug_set_listen_fd(listen_fd);
    screen_debug_log_started();

    while (!screen_debug_should_stop()) {
        struct sockaddr_in6 source_addr;
        socklen_t addr_len = sizeof(source_addr);
        int client_fd = accept(listen_fd, (struct sockaddr *)&source_addr, &addr_len);
        if (client_fd < 0) {
            if (!screen_debug_should_stop()) {
                ESP_LOGD(TAG, "accept failed: errno=%d", errno);
                vTaskDelay(pdMS_TO_TICKS(50));
            }
            continue;
        }

        screen_debug_handle_client(client_fd);
        close(client_fd);
    }

    screen_debug_task_finished(listen_fd);
    ESP_LOGI(TAG, "screen debug server stopped");
    vTaskDeleteWithCaps(NULL);
}

esp_err_t screen_debug_server_start(void)
{
    taskENTER_CRITICAL(&s_lock);
    bool already_running = s_task != NULL;
    if (!already_running) {
        s_stop_requested = false;
        s_listen_fd = -1;
    }
    taskEXIT_CRITICAL(&s_lock);

    if (already_running) {
        return ESP_OK;
    }

    TaskHandle_t task = NULL;
    BaseType_t task_ret = xTaskCreatePinnedToCoreWithCaps(screen_debug_task,
                                                          "screen_debug",
                                                          SCREEN_DEBUG_TASK_STACK_SIZE,
                                                          NULL,
                                                          SCREEN_DEBUG_TASK_PRIORITY,
                                                          &task,
                                                          APP_TASK_CORE_UI,
                                                          MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (task_ret != pdPASS || task == NULL) {
        ESP_LOGE(TAG, "create screen debug task failed");
        return ESP_ERR_NO_MEM;
    }

    taskENTER_CRITICAL(&s_lock);
    s_task = task;
    taskEXIT_CRITICAL(&s_lock);
    return ESP_OK;
}

void screen_debug_server_stop(void)
{
    int listen_fd = -1;

    taskENTER_CRITICAL(&s_lock);
    s_stop_requested = true;
    listen_fd = s_listen_fd;
    s_listen_fd = -1;
    taskEXIT_CRITICAL(&s_lock);

    if (listen_fd >= 0) {
        shutdown(listen_fd, SHUT_RDWR);
        close(listen_fd);
    }
}

bool screen_debug_server_is_running(void)
{
    taskENTER_CRITICAL(&s_lock);
    bool running = s_task != NULL;
    taskEXIT_CRITICAL(&s_lock);
    return running;
}
