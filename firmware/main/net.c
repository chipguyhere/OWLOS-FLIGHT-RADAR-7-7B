#include "net.h"
#include <stdio.h>
#include <string.h>
#include <time.h>
#include "esp_crt_bundle.h"
#include "esp_event.h"
#include "esp_http_client.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_netif_sntp.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

static const char *TAG = "net";
static volatile int s_up, s_why;
static char s_ip[16];
static int s_started, s_have_ssid;

/* the networks it remembers and the one it tries now. the event task goes round them, the site
 * and the screen change them: both hold the lock, for the list and for the driver's settings */
static SemaphoreHandle_t s_lock;
static nets_t s_nets;
static int    s_at;
static char   s_name[33];

/* looking round for networks, for the screen's WiFi page. the driver will not look while it is
 * joining, and with a network set that is not there it joins every two seconds: the joining
 * waits while it looks */
static volatile int s_scan, s_scan_started;       // SCAN_*
static uint32_t     s_scan_asked, s_scan_try;
static ap_t         s_aps[UI_APS];
static int          s_naps;
static portMUX_TYPE s_scan_mux = portMUX_INITIALIZER_UNLOCKED;

static void scan_collect(int ok)
{
    /* off the stack: this runs on the event task, 2.5 KB, and the log line below wants most of
     * that. with these 650 bytes on it the log of a board that writes to two ports ran past the
     * stack's end into the event queue's lock, and the next event hung the core for good (a
     * viewer's 5", 2026-11-05: its core dump has the lock 12 bytes under the stack) */
    static ap_t got[UI_APS];
    static wifi_ap_record_t r;
    int ng = 0;
    uint16_t n = 0;
    if (ok) esp_wifi_scan_get_ap_num(&n);
    while (n-- && esp_wifi_scan_get_ap_record(&r) == ESP_OK) {   // one at a time: no list of them all in the heap
        const char *name = (const char *)r.ssid;
        if (!name[0]) continue;                                   // a hidden one
        int k = 0;
        while (k < ng && strcmp(got[k].ssid, name)) k++;
        if (k == ng) {
            if (ng < UI_APS) ng++;
            else if (r.rssi <= got[k = ng - 1].rssi) continue;    // full: only a stronger one in place of the weakest
            snprintf(got[k].ssid, sizeof got[k].ssid, "%s", name);
            got[k].rssi = r.rssi;
            got[k].open = r.authmode == WIFI_AUTH_OPEN;
        } else if (r.rssi > got[k].rssi) {
            got[k].rssi = r.rssi;                                 // the same name twice: the nearer one
        }
        while (k > 0 && got[k - 1].rssi < got[k].rssi) {         // strongest first
            ap_t t = got[k]; got[k] = got[k - 1]; got[k - 1] = t;
            k--;
        }
    }
    esp_wifi_clear_ap_list();
    taskENTER_CRITICAL(&s_scan_mux);
    if (ok) {
        memcpy(s_aps, got, sizeof got);
        s_naps = ng;
    }
    s_scan = ok ? SCAN_DONE : SCAN_FAILED;
    s_scan_started = 0;
    taskEXIT_CRITICAL(&s_scan_mux);
    ESP_LOGI(TAG, "looked round: %d networks%s", ng, ok ? "" : ", the scan failed");
}

// the driver joins this one from now on. false: it is in the middle of something, nothing changed
static int apply(int at)
{
    static wifi_config_t wc;                          // off the stack: the event task comes here too, see scan_collect
    const net_kept_t *n = &s_nets.n[at];
    memset(&wc, 0, sizeof wc);
    // the driver's fields need no ending zero when full: a 32 letter name fills all 32
    memcpy(wc.sta.ssid, n->ssid, strnlen(n->ssid, sizeof wc.sta.ssid));
    memcpy(wc.sta.password, n->pass, strnlen(n->pass, sizeof wc.sta.password));
    wc.sta.threshold.authmode = n->pass[0] ? WIFI_AUTH_WEP : WIFI_AUTH_OPEN;
    /* a network served by several access points (fork addition, every board): look on every channel and
     * join the strongest. ESP-IDF's default takes the first it hears, the lowest channel, which here was
     * one far off (-85 dBm) with another 3 m away (-47 dBm) */
    wc.sta.scan_method = WIFI_ALL_CHANNEL_SCAN;
    wc.sta.sort_method = WIFI_CONNECT_AP_BY_SIGNAL;
    s_have_ssid = n->ssid[0] != 0;
    if (esp_wifi_set_config(WIFI_IF_STA, &wc) != ESP_OK) return 0;
    s_why = WHY_NONE;                                 // a new network: nothing known about it yet
    s_at = at;
    memcpy(s_name, n->ssid, sizeof s_name);
    return 1;
}

/* this one did not answer: on to the next, round and round, and the one in reach comes up. with
 * one network nothing changes */
static void next_network(void)
{
    xSemaphoreTake(s_lock, portMAX_DELAY);
    if (s_nets.count > 1 && apply((s_at + 1) % s_nets.count)) ESP_LOGI(TAG, "on to network \"%s\"", s_name);
    xSemaphoreGive(s_lock);
}

static void on_event(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    (void)arg;
    if (base == WIFI_EVENT && id == WIFI_EVENT_STA_START) {
        if (s_have_ssid) esp_wifi_connect();
    } else if (base == WIFI_EVENT && id == WIFI_EVENT_STA_DISCONNECTED) {
        s_up = 0;
        s_ip[0] = 0;
        const wifi_event_sta_disconnected_t *d = data;
        int reason = d ? d->reason : -1;
        // the password is wrong, or there is nobody of that name. the screen says which
        s_why = reason == WIFI_REASON_AUTH_FAIL || reason == WIFI_REASON_4WAY_HANDSHAKE_TIMEOUT ||
                reason == WIFI_REASON_HANDSHAKE_TIMEOUT ? WHY_PASSWORD : WHY_NOT_FOUND;
        ESP_LOGW(TAG, "WiFi lost (reason %d), trying again", reason);
        vTaskDelay(pdMS_TO_TICKS(2000));              // the event task waits, the router gets a breath
        if (s_have_ssid && s_scan != SCAN_RUNNING) {
            if (reason != WIFI_REASON_ASSOC_LEAVE) next_network();   // that one is the board's own doing
            esp_wifi_connect();
        }
    } else if (base == WIFI_EVENT && id == WIFI_EVENT_SCAN_DONE) {
        const wifi_event_sta_scan_done_t *e = data;
        scan_collect(e && e->status == 0);
        if (s_have_ssid && !s_up) esp_wifi_connect(); // back to joining
    } else if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP) {
        const ip_event_got_ip_t *e = data;
        snprintf(s_ip, sizeof s_ip, IPSTR, IP2STR(&e->ip_info.ip));
        s_up = 1;
        s_why = WHY_NONE;
        ESP_LOGI(TAG, "WiFi up, %s, network \"%s\"", s_ip, s_name);
    }
}

void net_scan_request(uint32_t now)
{
    taskENTER_CRITICAL(&s_scan_mux);
    s_scan = SCAN_RUNNING;                            // what it found before stays till the new list
    s_scan_started = 0;
    s_scan_asked = now;
    s_scan_try = now - 1000u;
    taskEXIT_CRITICAL(&s_scan_mux);
}

void net_scan_poll(uint32_t now)
{
    if (s_scan != SCAN_RUNNING) return;
    if (s_scan_started) {
        if (now - s_scan_asked > 20000u) {            // its answer never came
            s_scan = SCAN_FAILED;
            s_scan_started = 0;
            if (s_have_ssid && !s_up) esp_wifi_connect();
        }
        return;
    }
    if (now - s_scan_try < 300u) return;
    s_scan_try = now;
    if (!s_up) esp_wifi_disconnect();                 // a join in the way: stop it, it comes back after
    wifi_scan_config_t sc = { .show_hidden = false };
    esp_err_t e = esp_wifi_scan_start(&sc, false);
    if (e == ESP_OK) s_scan_started = 1;
    else if (now - s_scan_asked > 8000u) {
        s_scan = SCAN_FAILED;
        ESP_LOGW(TAG, "could not look round: %s", esp_err_to_name(e));
        if (s_have_ssid && !s_up) esp_wifi_connect();
    }
}

int net_scan_get(ap_t *out, int *n)
{
    taskENTER_CRITICAL(&s_scan_mux);
    memcpy(out, s_aps, sizeof s_aps);
    *n = s_naps;
    int st = s_scan;
    taskEXIT_CRITICAL(&s_scan_mux);
    return st;
}

int net_why(void) { return s_have_ssid && !s_up ? s_why : WHY_NONE; }

void net_start(const nets_t *l)
{
    if (s_started) return;
    s_started = 1;
    s_lock = xSemaphoreCreateMutex();
    s_nets = *l;
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    esp_netif_create_default_wifi_sta();
    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));
    ESP_ERROR_CHECK(esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID, on_event, NULL));
    ESP_ERROR_CHECK(esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP, on_event, NULL));
    ESP_ERROR_CHECK(esp_wifi_set_storage(WIFI_STORAGE_RAM));    // our NVS keeps the settings, not the driver's
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    apply(0);
    ESP_ERROR_CHECK(esp_wifi_start());
#if RADAR_OVERLAY
    /* the Waveshare 7/7B (fork addition): no modem sleep. their antenna sits behind a 7" panel and its
     * backlight, the signal is weak (-77 to -85 dBm seen), and a radio that sleeps between beacons there
     * missed DNS answers and lost the network. the boards run from a wall supply */
    esp_wifi_set_ps(WIFI_PS_NONE);
#endif
    ESP_LOGI(TAG, "WiFi started, network \"%s\", %d kept", s_name, s_nets.count);
}

void net_reconnect(const nets_t *l)
{
    if (!s_started) { net_start(l); return; }
    xSemaphoreTake(s_lock, portMAX_DELAY);
    esp_wifi_disconnect();
    s_nets = *l;
    apply(0);
    xSemaphoreGive(s_lock);
    if (s_have_ssid) esp_wifi_connect();
    ESP_LOGI(TAG, "WiFi settings changed, network \"%s\", %d kept", s_name, s_nets.count);
}

int net_up(void) { return s_up; }

void net_ssid(char *buf, int cap) { snprintf(buf, (size_t)cap, "%s", s_name); }

void net_ip(char *buf, int cap) { snprintf(buf, (size_t)cap, "%s", s_ip); }

void net_time_start(void)
{
    static int started;
    if (started) return;
    started = 1;
    esp_sntp_config_t c = ESP_NETIF_SNTP_DEFAULT_CONFIG("pool.ntp.org");
    esp_netif_sntp_init(&c);
    ESP_LOGI(TAG, "asking for the time");
}

uint32_t net_utc(void)
{
    time_t t = time(NULL);
    return t > 1700000000 ? (uint32_t)t : 0;          // not set yet, it counts from 1970
}

int net_get(const char *url, const char *bearer, net_sink_fn sink, void *ctx, int *status)
{
    static char buf[1024];                            // one piece; the fetch task is the only caller
    *status = 0;
    if (!s_up) return -1;
    esp_http_client_config_t c = {
        .url = url,
        .crt_bundle_attach = esp_crt_bundle_attach,
        .timeout_ms = 9000,
        .user_agent = NET_USER_AGENT,
        .buffer_size = 2048,
        .buffer_size_tx = 1024,
    };
    esp_http_client_handle_t h = esp_http_client_init(&c);
    if (!h) return -1;
    esp_http_client_set_header(h, "Accept", "application/json");
    if (bearer && bearer[0]) {
        static char auth[640];
        snprintf(auth, sizeof auth, "Bearer %s", bearer);
        esp_http_client_set_header(h, "Authorization", auth);
        esp_http_client_set_header(h, "Accept-Version", "v1");
    }
    int len = -1;
    if (esp_http_client_open(h, 0) == ESP_OK) {
        esp_http_client_fetch_headers(h);
        *status = esp_http_client_get_status_code(h);
        len = 0;
        while (*status == 200) {
            int r = esp_http_client_read(h, buf, sizeof buf);
            if (r <= 0) break;
            sink(ctx, buf, r);
            len += r;
        }
    }
    esp_http_client_close(h);
    esp_http_client_cleanup(h);
    return len;
}

int net_post_json(const char *url, const char *body, int *status)
{
    *status = 0;
    if (!s_up) return -1;
    esp_http_client_config_t c = {
        .url = url,
        .method = HTTP_METHOD_POST,
        .crt_bundle_attach = esp_crt_bundle_attach,
        .timeout_ms = 4000,                           // a server that is down holds the planes up this long, no more
        .user_agent = NET_USER_AGENT,
        .buffer_size = 512,
        .buffer_size_tx = 512,
    };
    esp_http_client_handle_t h = esp_http_client_init(&c);
    if (!h) return -1;
    esp_http_client_set_header(h, "Content-Type", "application/json");
    const int len = (int)strlen(body);
    int r = -1;
    if (esp_http_client_open(h, len) == ESP_OK && esp_http_client_write(h, body, len) == len) {
        esp_http_client_fetch_headers(h);
        *status = esp_http_client_get_status_code(h);
        r = 0;
    }
    esp_http_client_close(h);
    esp_http_client_cleanup(h);
    return r;
}
