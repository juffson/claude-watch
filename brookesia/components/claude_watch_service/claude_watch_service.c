#include "claude_watch_service.h"
#include "cw_status.h"

#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <time.h>
#include <sys/time.h>

#include "esp_log.h"
#include "esp_err.h"
#include "esp_wifi.h"
#include "esp_event.h"
#include "esp_netif.h"
#include "esp_netif_sntp.h"
#include "esp_http_server.h"
#include "esp_heap_caps.h"
#include "esp_system.h"
#include "nvs_flash.h"
#include "nvs.h"
#include "mdns.h"
#include "driver/usb_serial_jtag.h"
#include "driver/usb_serial_jtag_vfs.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "cw_service";

static char s_ssid[33] = "";
static char s_pass[65] = "";
static char s_ip[20] = "";
static volatile bool s_connected = false;
static bool s_mdns_up = false;
static bool s_sntp_up = false;
static httpd_handle_t s_httpd = NULL;

// ---------------- NVS creds ----------------
static void load_creds(void)
{
    nvs_handle_t h;
    if (nvs_open("cw", NVS_READONLY, &h) != ESP_OK) return;
    size_t n = sizeof(s_ssid);
    if (nvs_get_str(h, "ssid", s_ssid, &n) != ESP_OK) s_ssid[0] = 0;
    n = sizeof(s_pass);
    if (nvs_get_str(h, "pass", s_pass, &n) != ESP_OK) s_pass[0] = 0;
    nvs_close(h);
}

static void save_creds(void)
{
    nvs_handle_t h;
    if (nvs_open("cw", NVS_READWRITE, &h) != ESP_OK) return;
    nvs_set_str(h, "ssid", s_ssid);
    nvs_set_str(h, "pass", s_pass);
    nvs_commit(h);
    nvs_close(h);
}

// ---------------- Wi-Fi ----------------
static void apply_sta_config_and_connect(void)
{
    if (!s_ssid[0]) {
        ESP_LOGW(TAG, "no Wi-Fi credentials. USB console:  wifi <ssid> <password>");
        return;
    }
    wifi_config_t cfg = { 0 };
    strlcpy((char *)cfg.sta.ssid, s_ssid, sizeof(cfg.sta.ssid));
    strlcpy((char *)cfg.sta.password, s_pass, sizeof(cfg.sta.password));
    cfg.sta.threshold.authmode = s_pass[0] ? WIFI_AUTH_WPA_PSK : WIFI_AUTH_OPEN;
    esp_wifi_set_config(WIFI_IF_STA, &cfg);
    ESP_LOGI(TAG, "connecting to \"%s\"", s_ssid);
    esp_wifi_connect();
}

static void start_net_services(void)
{
    if (!s_mdns_up) {
        if (mdns_init() == ESP_OK) {
            mdns_hostname_set(CW_MDNS_HOSTNAME);
            mdns_instance_name_set("ClaudeWatch");
            mdns_service_add(NULL, "_http", "_tcp", CW_HTTP_PORT, NULL, 0);
            s_mdns_up = true;
        }
    }
    if (!s_sntp_up) {
        esp_sntp_config_t sc = ESP_NETIF_SNTP_DEFAULT_CONFIG(CW_NTP_SERVER);
        if (esp_netif_sntp_init(&sc) == ESP_OK) s_sntp_up = true;
    }
}

static void wifi_event(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    if (base == WIFI_EVENT) {
        if (id == WIFI_EVENT_STA_START) {
            apply_sta_config_and_connect();
        } else if (id == WIFI_EVENT_STA_DISCONNECTED) {
            if (s_connected) ESP_LOGW(TAG, "disconnected");
            s_connected = false;
            s_ip[0] = 0;
            if (s_ssid[0]) {
                vTaskDelay(pdMS_TO_TICKS(2000));
                esp_wifi_connect();
            }
        }
    } else if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP) {
        ip_event_got_ip_t *e = (ip_event_got_ip_t *)data;
        snprintf(s_ip, sizeof(s_ip), IPSTR, IP2STR(&e->ip_info.ip));
        s_connected = true;
        ESP_LOGI(TAG, "connected  ip=%s  http://%s.local/", s_ip, CW_MDNS_HOSTNAME);
        start_net_services();
    }
}

static void wifi_start(void)
{
    esp_err_t r = esp_netif_init();
    if (r != ESP_OK && r != ESP_ERR_INVALID_STATE) ESP_ERROR_CHECK(r);
    r = esp_event_loop_create_default();
    if (r != ESP_OK && r != ESP_ERR_INVALID_STATE) ESP_ERROR_CHECK(r);
    esp_netif_t *netif = esp_netif_create_default_wifi_sta();
    esp_netif_set_hostname(netif, CW_MDNS_HOSTNAME);

    wifi_init_config_t init = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&init));
    ESP_ERROR_CHECK(esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID, &wifi_event, NULL));
    ESP_ERROR_CHECK(esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP, &wifi_event, NULL));
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_set_storage(WIFI_STORAGE_RAM));
    ESP_ERROR_CHECK(esp_wifi_start());
    esp_wifi_set_ps(WIFI_PS_NONE);   // low-latency HTTP
}

void cw_service_set_wifi(const char *ssid, const char *pass)
{
    strlcpy(s_ssid, ssid ? ssid : "", sizeof(s_ssid));
    strlcpy(s_pass, pass ? pass : "", sizeof(s_pass));
    save_creds();
    ESP_LOGI(TAG, "saved \"%s\", reconnecting", s_ssid);
    esp_wifi_disconnect();
    apply_sta_config_and_connect();
}

bool cw_service_wifi_connected(void) { return s_connected; }
const char *cw_service_ip(void) { return s_ip; }
const char *cw_service_ssid(void) { return s_ssid; }

// ---------------- HTTP ----------------
static esp_err_t h_root(httpd_req_t *req)
{
    static const char help[] =
        "ClaudeWatch (ESP32-S3-Touch-AMOLED-1.75C, Brookesia app)\n\n"
        "GET  /status              current status JSON\n"
        "POST /status  {json}      {\"state\":\"working|idle|waiting|error|offline\",\n"
        "                           \"tool\":\"Bash\",\"project\":\"esp32\",\"msg\":\"...\",\"sessions\":1}\n"
        "POST /time?epoch=N        set clock (unix seconds)\n";
    httpd_resp_set_type(req, "text/plain");
    return httpd_resp_send(req, help, HTTPD_RESP_USE_STRLEN);
}

static esp_err_t h_get_status(httpd_req_t *req)
{
    char buf[640];
    cw_status_to_json(buf, sizeof(buf));
    httpd_resp_set_type(req, "application/json");
    return httpd_resp_send(req, buf, HTTPD_RESP_USE_STRLEN);
}

static esp_err_t h_post_status(httpd_req_t *req)
{
    char body[1024];
    int len = req->content_len;
    if (len <= 0 || len >= (int)sizeof(body)) {
        httpd_resp_set_status(req, "400 Bad Request");
        return httpd_resp_sendstr(req, "{\"ok\":false,\"error\":\"bad length\"}");
    }
    int got = 0;
    while (got < len) {
        int r = httpd_req_recv(req, body + got, len - got);
        if (r <= 0) return ESP_FAIL;
        got += r;
    }
    body[len] = 0;
    bool changed;
    httpd_resp_set_type(req, "application/json");
    if (!cw_status_apply_json(body, &changed)) {
        httpd_resp_set_status(req, "400 Bad Request");
        return httpd_resp_sendstr(req, "{\"ok\":false,\"error\":\"missing or invalid state\"}");
    }
    ESP_LOGI(TAG, "status <- %s", body);
    return httpd_resp_sendstr(req, "{\"ok\":true}");
}

static esp_err_t h_post_time(httpd_req_t *req)
{
    char q[64] = "", v[24] = "";
    if (httpd_req_get_url_query_str(req, q, sizeof(q)) == ESP_OK) httpd_query_key_value(q, "epoch", v, sizeof(v));
    if (!v[0]) {
        int r = httpd_req_recv(req, v, sizeof(v) - 1);
        if (r > 0) v[r] = 0;
    }
    long long epoch = atoll(v);
    httpd_resp_set_type(req, "application/json");
    if (epoch < 1700000000LL) {
        httpd_resp_set_status(req, "400 Bad Request");
        return httpd_resp_sendstr(req, "{\"ok\":false,\"error\":\"bad epoch\"}");
    }
    struct timeval tv = { .tv_sec = (time_t)epoch, .tv_usec = 0 };
    settimeofday(&tv, NULL);
    return httpd_resp_sendstr(req, "{\"ok\":true}");
}

static void http_start(void)
{
    httpd_config_t cfg = HTTPD_DEFAULT_CONFIG();
    cfg.server_port = CW_HTTP_PORT;
    cfg.lru_purge_enable = true;
    cfg.stack_size = 6144;
    if (httpd_start(&s_httpd, &cfg) != ESP_OK) {
        ESP_LOGE(TAG, "httpd start failed");
        return;
    }
    const httpd_uri_t routes[] = {
        { .uri = "/",       .method = HTTP_GET,  .handler = h_root },
        { .uri = "/status", .method = HTTP_GET,  .handler = h_get_status },
        { .uri = "/status", .method = HTTP_POST, .handler = h_post_status },
        { .uri = "/time",   .method = HTTP_POST, .handler = h_post_time },
    };
    for (size_t i = 0; i < sizeof(routes) / sizeof(routes[0]); i++) httpd_register_uri_handler(s_httpd, &routes[i]);
}

// ---------------- USB console ----------------
static void print_info(void)
{
    char js[640];
    cw_status_to_json(js, sizeof(js));
    time_t now = time(NULL);
    struct tm t;
    localtime_r(&now, &t);
    char ts[40];
    strftime(ts, sizeof(ts), "%Y-%m-%d %H:%M:%S", &t);
    printf("---- ClaudeWatch (Brookesia) ----\n");
    printf("time     : %s (%s)\n", ts, CW_TZ);
    printf("wifi     : %s  ssid=\"%s\"  ip=%s\n", s_connected ? "connected" : "down", s_ssid, s_ip);
    printf("mdns     : http://%s.local/\n", CW_MDNS_HOSTNAME);
    printf("heap     : %u internal free, %u psram free\n",
           (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL), (unsigned)heap_caps_get_free_size(MALLOC_CAP_SPIRAM));
    printf("status   : %s\n", js);
}

static void handle_line(char *line)
{
    while (*line == ' ' || *line == '\t') line++;
    size_t n = strlen(line);
    while (n && (line[n - 1] == '\r' || line[n - 1] == '\n' || line[n - 1] == ' ')) line[--n] = 0;
    if (!n) return;

    if (line[0] == '{') {
        bool changed;
        printf(cw_status_apply_json(line, &changed) ? "{\"ok\":true}\n" : "{\"ok\":false,\"error\":\"missing or invalid state\"}\n");
    } else if (strncmp(line, "wifi ", 5) == 0) {
        char *ssid = line + 5;
        while (*ssid == ' ') ssid++;
        char *pass = strchr(ssid, ' ');
        if (pass) { *pass = 0; pass++; while (*pass == ' ') pass++; } else pass = "";
        cw_service_set_wifi(ssid, pass);
    } else if (strncmp(line, "time ", 5) == 0) {
        long long epoch = atoll(line + 5);
        if (epoch < 1700000000LL) { printf("bad epoch\n"); return; }
        struct timeval tv = { .tv_sec = (time_t)epoch, .tv_usec = 0 };
        settimeofday(&tv, NULL);
        printf("[time] set from host\n");
    } else if (strcmp(line, "status") == 0) {
        char js[640];
        cw_status_to_json(js, sizeof(js));
        printf("%s\n", js);
    } else if (strcmp(line, "info") == 0) {
        print_info();
    } else if (strcmp(line, "reboot") == 0) {
        esp_restart();
    } else {
        printf("commands: wifi <ssid> <pass> | time <epoch> | {json status} | status | info | reboot\n");
    }
}

static void console_task(void *arg)
{
    usb_serial_jtag_driver_config_t cfg = USB_SERIAL_JTAG_DRIVER_CONFIG_DEFAULT();
    if (usb_serial_jtag_driver_install(&cfg) == ESP_OK) {
        usb_serial_jtag_vfs_use_driver();
    }
    setvbuf(stdin, NULL, _IONBF, 0);
    char line[512];
    while (fgets(line, sizeof(line), stdin)) {
        handle_line(line);
    }
    vTaskDelete(NULL);
}

// ---------------- tick ----------------
static void tick_task(void *arg)
{
    while (1) {
        cw_status_tick();
        vTaskDelay(pdMS_TO_TICKS(1000));
    }
}

void cw_service_start(void)
{
    setenv("TZ", CW_TZ, 1);
    tzset();
    cw_status_init();
    load_creds();
    wifi_start();
    http_start();
    xTaskCreate(console_task, "cw_console", 4096, NULL, 3, NULL);
    xTaskCreate(tick_task, "cw_tick", 2048, NULL, 2, NULL);
    ESP_LOGI(TAG, "started");
}
