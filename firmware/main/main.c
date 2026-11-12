/*
 * OWLOS Flight Radar - the aircraft around home on the ViewOwl screens (core/config.h).
 * core 0: WiFi, the fetch task (asks the server every few seconds, merges the answer into the
 *         sky), the console (settings from the site over the USB line)
 * core 1: the screen task - touch and BOOT in, copy the sky, draw the frame in strips
 * before the site sent any settings it shows the demo sky, so the board is never blank.
 */
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "button.h"
#include "console.h"
#include "demo.h"
#include "display.h"
#include "font.h"
#include "gfx.h"
#include "led.h"
#include "net.h"
#include "map.h"
#include "qr_site.h"
#include "sky.h"
#include "store.h"
#include "tiles.h"
#include "touch.h"
#include "ui.h"
#if RADAR_OVERLAY
#include "ovl.h"
#endif

#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_mac.h"
#include "esp_random.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "esp_task_wdt.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "nvs_flash.h"

// FLIGHT_VERSION comes from CHANGES.md (firmware/CMakeLists.txt)
#define DEMO_BYTES     6144            /* the demo's answer: 12 planes */
#define FREE_EVERY_MS  5000u           /* the free networks allow a request a second */
#define FR24_EVERY_MS  20000u          /* FR24 counts credits: slower */

static const char *TAG = "flight";

static CORE_BIG sky_t    s_sky;        // written by the fetch task, under s_lock
static demo_t            s_demo;
static fr_cfg_t          s_cfg;        // the settings in use: the screen changes them, the site too
static secrets_t         s_sec;
static nets_t            s_nets;       // the networks it remembers, the first is the one in s_sec
static SemaphoreHandle_t s_lock;
static TaskHandle_t      s_fetch;
static CORE_BIG ui_t     s_ui;
static volatile int      s_cfg_new;    // the site sent settings: the screen takes s_cfg
static volatile int      s_skip_align; // the page said skip the touch alignment
static volatile int      s_align_told = -1;   // what the page last heard of it, -1: tell it again
static char              s_server[24];
static int               s_http;
static uint32_t          s_fetched;
static volatile uint32_t s_wifi_try;   // when WiFi settings last came, from the site or the screen
static volatile int      s_demo_on;    // the demo sky is up: no WiFi set, or the one set is not there
static volatile int      s_screen = -1, s_screen_set = -1;   // the 2.8" CYD's screen revision: now, and what the site asks
static char              s_home_ask[64];   // a home typed on the screen, for the fetch task to find. "": none
static char              s_said[40];       // what the fetch task tells the screen: a home found, a watched plane
static uint32_t          s_said_ms;        // for how long
static volatile int      s_said_new;
static watchlist_t       s_watch;          // the planes to follow (core/watch.h), under s_lock
static volatile uint32_t s_watch_pick;     // one of them just came in range: the screen picks it
static uint32_t          s_watch_asked;    // ms of the last lookup of one, 0: ask now
static light_t           s_light;          // what happened, for the LED (core/light.h), under s_lock
static light_cfg_t       s_light_cfg;      // what lights it: the screen's and the site's, under s_lock
static scope_opt_t       s_opt;            // the radar's options: the gear's and the site's, under s_lock
static volatile int      s_opt_new;        // the site sent them: the screen takes them
static volatile int      s_light_new;      // the site sent LED settings: the screen takes them
static volatile int      s_light_test;     // the site asked for the LED's red, green, blue
static char              s_newest[12];     // the version the site has (update_check), "": not heard. under s_lock

/* how the last run ended, for a board found standing still: did it start again (and why), or
 * did the picture stop with the board running? the uptime goes to NVS once a minute (store.h):
 * RTC memory lost it when the USB line's reset pulled EN, the site's port may do that. the boot
 * log and GET say it */
static uint32_t           s_ran_s;     // how long the run before this one was, s, to a minute
static uint32_t           s_ran_kept;  // the uptime last saved, s
static const char        *s_reset;     // why it ended

static const char *reset_name(esp_reset_reason_t r)
{
    switch (r) {
    case ESP_RST_POWERON:  return "power on";
    case ESP_RST_EXT:      return "reset pin";
    case ESP_RST_SW:       return "restart";
    case ESP_RST_PANIC:    return "crash";
    case ESP_RST_INT_WDT:  return "interrupt watchdog";
    case ESP_RST_TASK_WDT: return "task watchdog";
    case ESP_RST_WDT:      return "watchdog";
    case ESP_RST_BROWNOUT: return "brownout";
    case ESP_RST_DEEPSLEEP: return "deep sleep";
    default:               return "other";
    }
}

static uint32_t now_ms(void) { return (uint32_t)(esp_timer_get_time() / 1000); }
static uint32_t now_us(void) { return (uint32_t)esp_timer_get_time(); }
static void lock(void) { xSemaphoreTake(s_lock, portMAX_DELAY); }
static void unlock(void) { xSemaphoreGive(s_lock); }

/* --- asking the server --- */

/* the open networks: no key, a request a second at most. latitude, longitude, radius in NM.
 * airplanes.live and adsb.one used to be here: both now answer 403 to anyone not let in */
static const char *const FREE_URL[FREE_SERVERS] = {
    "https://api.adsb.lol/v2/point/%.2f/%.2f/%d",
    "https://opendata.adsb.fi/api/v3/lat/%.2f/lon/%.2f/dist/%d",
};
static const char *const FREE_NAME[FREE_SERVERS] = { "ADSB.LOL", "ADSB.FI" };

/* the server learns where to look, not where you live: home goes out rounded to 0.01 degree,
 * about a kilometre. the scope keeps the exact point, the answer reaches 1.6 times past its rim */
static float vague(float deg) { return roundf(deg * 100.0f) / 100.0f; }

// a bit more than the scope, so planes on their way in show on the rim first. the flat square
// reaches further, into its corners
static float ask_km(const fr_cfg_t *c) { return range_km(c) * (!RADAR_ROUND && c->look != LOOK_RADAR ? 2.4f : 1.6f); }

static void free_url(const fr_cfg_t *c, int server, char *url, int cap)
{
    int nm = (int)ceilf(ask_km(c) / 1.852f);
    if (nm > 250) nm = 250;                           // the most they answer
    snprintf(url, (size_t)cap, FREE_URL[server], (double)vague(c->lat), (double)vague(c->lon), nm);
}

static void fr24_url(const fr_cfg_t *c, char *url, int cap)
{
    float lat = vague(c->lat), lon = vague(c->lon);
    float r = ask_km(c), dlat = r / 110.574f, dlon = r / (111.320f * cosf(lat * 3.14159265f / 180.0f));
    snprintf(url, (size_t)cap, "https://fr24api.flightradar24.com/api/live/flight-positions/full?bounds=%.2f,%.2f,%.2f,%.2f",
             (double)(lat + dlat), (double)(lat - dlat), (double)(lon - dlon), (double)(lon + dlon));
}

static void to_reader(void *ctx, const char *data, int len) { sky_read((sky_reader_t *)ctx, data, len); }

/* --- the ground under the scope: OpenStreetMap's tiles, downloaded once into the flash
 * (tiles.c) and read into a picture of the scope's size (core/map.c) when home or the range
 * changes. the screen task draws it, this one makes it: while it is being made it is not ready,
 * and the scope shows the glass alone for that moment --- */

/* the map's pieces: the IRAM the code leaves free first. it takes 32 bit words only, map.c
 * reads and writes nothing smaller, and TLS and the WiFi cannot use it at all */
static void *iram_first(size_t bytes)
{
#if CONFIG_IDF_TARGET_ESP32
    return heap_caps_malloc(bytes, MALLOC_CAP_EXEC | MALLOC_CAP_32BIT);
#elif DISP_LCD_RGB || DISP_LCD_DSI || DISP_WAVE7 || CONFIG_IDF_TARGET_ESP32C5
    return heap_caps_malloc(bytes, MALLOC_CAP_SPIRAM);  // the 5" and the Tab5: 190 KB, no such room in the RAM. the C5: 38 KB the strips want
#else
    (void)bytes;
    return NULL;                                       // the C3's RAM is one: malloc does it
#endif
}

static CORE_BIG map_t      s_map;
static CORE_BIG map_plan_t s_plan;
static int        s_map_read;          // tiles that went into s_map
static uint32_t   s_map_done;          // and which of the plan's, a bit each
static uint32_t   s_map_try;           // when it last asked for missing ones
_Static_assert(MAP_TILES_MAX <= 32, "a bit a tile in s_map_done");

static void map_service(const fr_cfg_t *c)
{
    if (!c->map) return;
    const float kpx = ui_ground_kpx(c);
    int w, h, disc;
    ui_ground_size(c, &w, &h, &disc);                       // the look sets its size: the whole screen or the disc
    int same = s_map.ready && s_map.lat == c->lat && s_map.lon == c->lon && s_map.kpx == kpx && s_map.w == w && s_map.h == h;
    if (same && (s_map_read == s_plan.ntiles || now_ms() - s_map_try < 15000u)) return;
    if (!same) map_plan(&s_plan, c->lat, c->lon, kpx, w, h, disc);   // a retry asks for the same tiles again
    s_map_try = now_ms();
    int kept = tiles_fetch(s_plan.tiles, s_plan.ntiles);      // the missing ones, when the WiFi is up
    uint32_t low_fetch = (uint32_t)heap_caps_get_minimum_free_size(MALLOC_CAP_8BIT);
    if (same && kept <= s_map_read) return;                   // nothing new to show
    int64_t t0 = esp_timer_get_time();
    /* a retry for the same plan reads only the tiles still missing, into the texels as they are.
     * it used to wipe them and read every tile again: with the memory short just then, a map that
     * showed went and stayed away till a restart */
    if (!same) {
        if (!map_alloc(&s_map, w, h, disc)) {
            ESP_LOGE(TAG, "map: no memory for %d x %d texels", w, h);
            return;
        }
        map_begin(&s_map);
        s_map_done = 0;
    }
    int read = 0, oom = 0;
    for (int k = 0; k < s_plan.ntiles; k++) {
        tile_src_t src;
        if (!(s_map_done & 1u << k) && tiles_open(&s_plan.tiles[k], &src)) {
            int r = map_tile(&s_map, &s_plan, k, tiles_read, &src);
            if (r == PNG_OK) s_map_done |= 1u << k;
            oom |= r == PNG_NO_MEMORY;
        }
        read += (s_map_done >> k) & 1;
    }
    map_end(&s_map, &s_plan);
    if (oom) ESP_LOGW(TAG, "map: no memory to read a tile in, heap %u, largest %u",
                      (unsigned)heap_caps_get_free_size(MALLOC_CAP_8BIT), (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_8BIT));
    s_map_read = read;
    ESP_LOGI(TAG, "map: zoom %d, %d of %d tiles, made in %d ms, heap %u, at least %u after the tiles came, %u after reading them",
             s_plan.z, read, s_plan.ntiles, (int)((esp_timer_get_time() - t0) / 1000), (unsigned)heap_caps_get_free_size(MALLOC_CAP_8BIT),
             (unsigned)low_fetch, (unsigned)heap_caps_get_minimum_free_size(MALLOC_CAP_8BIT));
}

/* --- home's town and country, when the site sent none: OpenStreetMap's Nominatim, asked once a
 * home with the same rounded point the plane servers get --- */

typedef struct { char *b; int n, cap; } text_t;

/* Nominatim's question and answer, for geocode() and the home search: both run on the fetch task,
 * one at a time. the ESP32's RAM is short, one pair is kept */
static CORE_BIG char s_osm_url[420], s_osm[2048];

static void to_text(void *ctx, const char *data, int len)
{
    text_t *t = ctx;
    if (len > t->cap - 1 - t->n) len = t->cap - 1 - t->n;
    memcpy(t->b + t->n, data, (size_t)len);
    t->n += len;
    t->b[t->n] = 0;
}

// the most a place can be called, from a city down to a county: the first one there
static const char *const PLACE_KIND[] = { "city", "town", "village", "municipality", "hamlet", "county", "state" };

/* home's country as OpenStreetMap has it, its two letters, "" till Nominatim said. at a home in Rusia the
 * board asks for no planes and no map. OSM puts Crimea and the occupied towns in Ukraine: asked 2026-11-10,
 * Simferopol, Sevastopol, Kerch, Donetsk, Luhansk, Mariupol and Melitopol all "ua", Narva "ee", Ivangorod
 * and Belgorod "ru". Nominatim silent: the board goes on as ever, nobody else is held up by it */
static char s_home_cc[3];

static int home_refused(void)
{
    lock();
    int r = !strcmp(s_home_cc, "ru");
    unlock();
    return r;
}

/* asked once a home, after power on too, with the same rounded point the plane servers get. a home
 * Nominatim did not answer about is asked again five minutes on */
static void geocode(void)
{
    static float asked_lat = 999, asked_lon = 999;
    static uint32_t asked_at;
    fr_cfg_t c;
    char cc[sizeof s_home_cc];
    lock();
    c = s_cfg;
    memcpy(cc, s_home_cc, sizeof cc);
    unlock();
    const int same_home = c.lat == asked_lat && c.lon == asked_lon;
    if (!net_up() || (same_home && (cc[0] || now_ms() - asked_at < 300000u))) return;
    asked_lat = c.lat;
    asked_lon = c.lon;
    asked_at = now_ms();
    snprintf(s_osm_url, sizeof s_osm_url, "https://nominatim.openstreetmap.org/reverse?format=jsonv2&zoom=10&accept-language=en&lat=%.2f&lon=%.2f",
             (double)vague(c.lat), (double)vague(c.lon));
    const char *buf = s_osm;
    text_t t = { s_osm, 0, sizeof s_osm };
    s_osm[0] = 0;
    int status = 0;
    net_get(s_osm_url, NULL, to_text, &t, &status);
    char place[sizeof c.place] = "", country[sizeof c.country] = "", code[sizeof s_home_cc] = "";
    for (int i = 0; i < (int)(sizeof PLACE_KIND / sizeof PLACE_KIND[0]) && !place[0]; i++) json_str(buf, PLACE_KIND[i], place, sizeof place);
    json_str(buf, "country", country, sizeof country);
    json_str(buf, "country_code", code, sizeof code);
    ESP_LOGI(TAG, "where home is: HTTP %d, \"%s\", \"%s\", %s", status, place, country, code[0] ? code : "no code");
    if (status != 200 || (!place[0] && !country[0] && !code[0])) return;
    int named = 0;
    lock();
    if (s_cfg.lat == c.lat && s_cfg.lon == c.lon) {           // still the same home
        memcpy(s_home_cc, code, sizeof code);
        if (!s_cfg.place[0] && place[0]) { memcpy(s_cfg.place, place, sizeof place); named = 1; }
        if (!s_cfg.country[0] && country[0]) { memcpy(s_cfg.country, country, sizeof country); named = 1; }
        if (named) s_cfg_new = 1;
        c = s_cfg;
    }
    unlock();
    if (named) store_save_cfg(&c);                     // saved only when a name came that it had not
}

/* --- a home typed on the screen. "48.9 18.1" is taken as it is, no WiFi needed. a name or a
 * postcode is asked of Nominatim's search, and its first answer taken: a town's centre --- */

// "48.9 18.1" or "48.9, 18.1": a point, 1. a postcode or a name: 0
static int as_point(const char *s, float *lat, float *lon)
{
    char *end;
    double a = strtod(s, &end);
    if (end == s) return 0;
    while (*end == ' ' || *end == ',') end++;
    const char *at = end;
    double b = strtod(at, &end);
    if (end == at) return 0;
    while (*end == ' ') end++;
    if (*end || a < -90 || a > 90 || b < -180 || b > 180) return 0;
    *lat = (float)a;
    *lon = (float)b;
    return 1;
}

// the words for a URL: letters and digits as they are, the rest as %XX
static void url_words(const char *s, char *out, int cap)
{
    static const char HEX[] = "0123456789ABCDEF";
    int n = 0;
    for (; *s && n < cap - 4; s++) {
        unsigned char ch = (unsigned char)*s;
        if ((ch >= '0' && ch <= '9') || (ch >= 'A' && ch <= 'Z') || (ch >= 'a' && ch <= 'z') || ch == '-' || ch == '.') {
            out[n++] = (char)ch;
        } else {
            out[n++] = '%';
            out[n++] = HEX[ch >> 4];
            out[n++] = HEX[ch & 15];
        }
    }
    out[n] = 0;
}

// the new home, as the site's SET makes it. no place: geocode() asks OSM for its name
static void home_set(float lat, float lon, const char *place, const char *country)
{
    fr_cfg_t c;
    lock();
    if (s_cfg.lat != lat || s_cfg.lon != lon) s_home_cc[0] = 0;   // another home: its country, geocode() asks
    s_cfg.lat = lat;
    s_cfg.lon = lon;
    snprintf(s_cfg.place, sizeof s_cfg.place, "%s", place);
    snprintf(s_cfg.country, sizeof s_cfg.country, "%s", country);
    sky_home(&s_sky, lat, lon);
    c = s_cfg;
    s_cfg_new = 1;
    unlock();
    store_save_cfg(&c);
}

/* a line for the screen's toast, s_lock held. wider than the screen: the shorter one, the rest
 * is on SET */
static void say_locked(const char *text, const char *shorter, uint32_t ms)
{
    if (snprintf(s_said, sizeof s_said, "%s", text) >= (int)sizeof s_said || font_width(&FONT_MEDIUM, s_said) > GFX_W - 24) {
        snprintf(s_said, sizeof s_said, "%s", shorter);
    }
    s_said_ms = ms;
    s_said_new = 1;
}

static void home_say(const char *text)
{
    lock();
    say_locked(text, "HOME FOUND", 4000u);
    unlock();
}

// one search of Nominatim: its first answer into s_osm, the HTTP status back
static int search(const char *q)
{
    int n = snprintf(s_osm_url, sizeof s_osm_url,
                     "https://nominatim.openstreetmap.org/search?format=jsonv2&limit=1&addressdetails=1&accept-language=en&q=");
    url_words(q, s_osm_url + n, (int)sizeof s_osm_url - n);
    text_t t = { s_osm, 0, sizeof s_osm };
    s_osm[0] = 0;
    int status = 0;
    net_get(s_osm_url, NULL, to_text, &t, &status);
    return status;
}

static void home_find(void)
{
    char ask[sizeof s_home_ask], here[sizeof s_cfg.country];
    lock();
    memcpy(ask, s_home_ask, sizeof ask);
    s_home_ask[0] = 0;
    memcpy(here, s_cfg.country, sizeof here);
    unlock();
    if (!ask[0]) return;
    float lat, lon;
    char said[sizeof s_said + 8];
    if (as_point(ask, &lat, &lon)) {
        home_set(lat, lon, "", "");
        snprintf(said, sizeof said, "HOME %.4f %.4f", (double)lat, (double)lon);
        home_say(said);
        ESP_LOGI(TAG, "home from the screen: a point");
        return;
    }
    if (!net_up()) { home_say("NO WIFI: TYPE LAT LON"); return; }
    const char *buf = s_osm;
    char slat[24] = "", slon[24] = "", place[sizeof s_cfg.place] = "", country[sizeof s_cfg.country] = "";
    int status = 0;
    /* a postcode is the same number in many countries: 10115 alone is Santo Domingo. one with a
     * digit in it is looked for in home's country first ("91101, Slovakia"), then anywhere. a town
     * is looked for as it is: "Bratislava, United Kingdom" finds a house of that name */
    if (strpbrk(ask, "0123456789") && here[0]) {
        char q[sizeof ask + sizeof here + 2];
        snprintf(q, sizeof q, "%s, %s", ask, here);
        status = search(q);
        json_str(buf, "lat", slat, sizeof slat);
        if (!slat[0]) vTaskDelay(pdMS_TO_TICKS(1100));    // Nominatim takes a request a second
    }
    if (!slat[0]) {
        status = search(ask);
        json_str(buf, "lat", slat, sizeof slat);
    }
    json_str(buf, "lon", slon, sizeof slon);
    ESP_LOGI(TAG, "home from the screen: HTTP %d, %s", status, slat[0] ? "found" : "not found");
    if (status != 200) { home_say("NO ANSWER: TRY AGAIN"); return; }
    if (!slat[0] || !slon[0]) { home_say("NOT FOUND: TRY ANOTHER"); return; }
    for (int i = 0; i < (int)(sizeof PLACE_KIND / sizeof PLACE_KIND[0]) && !place[0]; i++) json_str(buf, PLACE_KIND[i], place, sizeof place);
    if (!place[0]) json_str(buf, "name", place, sizeof place);
    json_str(buf, "country", country, sizeof country);
    home_set(strtof(slat, NULL), strtof(slon, NULL), place, country);
    snprintf(said, sizeof said, "HOME: %s", place[0] ? place : country);
    home_say(said);
}

/* --- a newer version? the site publishes the one it flashes in version.json (the first "## " line
 * of CHANGES.md, .github/workflows/pages.yml). asked once the WiFi is up, then twice a day. the
 * screen says it with a small note and ABOUT (core/ui_about.c), GET with "newest" --- */

#define UPDATE_URL      SITE_URL "version.json"
#define UPDATE_EVERY_MS (12u * 3600u * 1000u)
#define UPDATE_RETRY_MS (30u * 60u * 1000u)   /* no answer, or not one: again in half an hour */

/* --- counted on the site's map of boards, only when the owner allowed it (SET, COUNT ME, or the site's
 * question): every 10 minutes "this build, this version", nothing else. where to is in the site's
 * version.json, "stats": it says none, nothing is sent. server/ in the repository is that server --- */
#define ALIVE_EVERY_MS (10u * 60u * 1000u)
static uint8_t           s_help;           // 0 never asked, 1 allowed, 2 not (store_load_help). under s_lock
static volatile int      s_help_new;       // the site changed it: the screen takes it
static char              s_stats_url[64];  // from version.json, "": none. under s_lock
/* the radar without its panel, the scope in the middle (its three dots, or the site): kept like help */
static uint8_t           s_bare;           // store_load_bare. under s_lock
static volatile int      s_bare_new;       // the site changed it: the screen takes it

static const char *board_name(void);

static void alive_report(void)
{
    static uint32_t sent;                              // ms of the last report, 0: none yet
    char base[sizeof s_stats_url];
    lock();
    const int help = s_help;
    memcpy(base, s_stats_url, sizeof base);
    unlock();
    const uint32_t now = now_ms();
    if (help != 1 || !base[0] || !net_up() || (sent && now - sent < ALIVE_EVERY_MS)) return;
    sent = now ? now : 1;
    char url[sizeof base + 16], body[80];
    snprintf(url, sizeof url, "%s/api/alive", base);
    snprintf(body, sizeof body, "{\"build\":\"%s\",\"version\":\"%s\"}", board_name(), FLIGHT_VERSION);
    int status = 0;
    net_post_json(url, body, &status);
    ESP_LOGI(TAG, "counted on the map of boards: HTTP %d", status);
}

static void update_check(void)
{
    static uint32_t asked;                             // ms of the last question, 0: never
    static int heard;
    uint32_t now = now_ms();
    if (!net_up() || (asked && now - asked < (heard ? UPDATE_EVERY_MS : UPDATE_RETRY_MS))) return;
    asked = now ? now : 1;
    char buf[160], v[sizeof s_newest] = "";
    text_t t = { buf, 0, sizeof buf };
    buf[0] = 0;
    int status = 0;
    net_get(UPDATE_URL, NULL, to_text, &t, &status);
    heard = status == 200 && json_str(buf, "version", v, sizeof v) && v[0];
    for (const char *p = v; heard && *p; p++) heard = (*p >= '0' && *p <= '9') || *p == '.';   // digits and dots, or it is not one
    ESP_LOGI(TAG, "the site's version: HTTP %d, %s, this one %s", status, heard ? v : "none", FLIGHT_VERSION);
    if (!heard) return;
    // where boards are counted: an https address and nothing odd in it, or none. an answer that filled the
    // buffer may be cut in the middle of it: then none
    char stats[sizeof s_stats_url] = "";
    if (t.n < (int)sizeof buf - 1) json_str(buf, "stats", stats, sizeof stats);
    int fine = !strncmp(stats, "https://", 8) && strlen(stats) < sizeof stats - 1;
    for (const char *p = stats; fine && *p; p++) fine = *p > ' ' && *p != '"' && *p != '\\' && *p != '?' && *p != '#';
    for (size_t n = strlen(stats); fine && n > 8 && stats[n - 1] == '/'; n--) stats[n - 1] = 0;   // "/api/alive" goes after it
    lock();
    snprintf(s_newest, sizeof s_newest, "%s", v);
    snprintf(s_stats_url, sizeof s_stats_url, "%s", fine ? stats : "");
    unlock();
    if (ui_version_cmp(v, FLIGHT_VERSION) > 0) ESP_LOGI(TAG, "the site has a newer version: %s", v);
}

/* --- the watch list (core/watch.h): the sky round home says which are in range, these lookups
 * the rest, one plane at a time. both free networks answer one plane by callsign, registration
 * (with its dash: OE-IJW) or address; a list of them they do not take, only the first --- */

#define WATCH_EVERY_MS 15000u          /* a lookup every 15 s: 8 planes, each looked at every two minutes */

static const char *const WATCH_BASE[FREE_SERVERS] = { "https://api.adsb.lol/v2/", "https://opendata.adsb.fi/api/v2/" };
static const char *const WATCH_PATH[FREE_SERVERS][3] = {          // by callsign, registration, address
    { "callsign/", "reg/", "hex/" },
    { "callsign/", "registration/", "hex/" },
};
static const char *const WATCH_KIND[4] = { "?", "callsign", "registration", "address" };

// what the watch list said, for the LED. s_lock held
static void light_news(int news, uint32_t now)
{
    if (news == WATCH_NEWS_RANGE) light_event(&s_light, LIGHT_RANGE, now);
    else if (news == WATCH_NEWS_UP) light_event(&s_light, LIGHT_UP, now);
    else if (news == WATCH_NEWS_DOWN) light_event(&s_light, LIGHT_DOWN, now);
}

/* the answer is read with the planes' own reader: the plane of the list is among the ones it kept.
 * asked of the other free network than the planes': with both on one, adsb.lol said 429 */
static void watch_lookup(sky_reader_t *rd, int planes_server)
{
    const int server = (planes_server + 1) % FREE_SERVERS;
    uint32_t now = now_ms();
    if (s_watch_asked && now - s_watch_asked < WATCH_EVERY_MS) return;
    char id[WATCH_ID];
    int kind = WATCH_CALLSIGN;
    lock();
    int i = watch_next(&s_watch);
    if (i >= 0) {
        memcpy(id, s_watch.w[i].id, sizeof id);
        kind = watch_kind(&s_watch.w[i]);
        sky_read_begin(rd, &s_sky, 0, now);
    }
    unlock();
    if (i < 0) return;                                 // none, or all round home
    s_watch_asked = now;
    static char url[96];
    snprintf(url, sizeof url, "%s%s%s", WATCH_BASE[server], WATCH_PATH[server][kind - WATCH_CALLSIGN], id);
    int status = 0;
    net_get(url, NULL, to_reader, rd, &status);
    if (status != 200) {
        ESP_LOGW(TAG, "watch %s: %s by %s, HTTP %d", id, FREE_NAME[server], WATCH_KIND[kind], status);
        return;                                        // the same one again next time
    }
    char said[40] = "";
    const char *how = "not heard";
    lock();
    watch_t *w = &s_watch.w[i];
    if (!strcmp(w->id, id)) {                          // the list was not changed meanwhile
        const plane_t *p = NULL;
        for (int k = 0; k < rd->n && !p; k++) if (watch_is(w, &rd->got[k])) p = &rd->got[k];
        if (p) how = p->ground ? "on the ground" : "in the air";
        light_news(watch_answer(w, kind, p, now, said, sizeof said), now);
        if (said[0]) say_locked(said, "A WATCHED PLANE", 10000u);
    }
    unlock();
    ESP_LOGI(TAG, "watch %s: %s by %s, %s%s%s", id, FREE_NAME[server], WATCH_KIND[kind], how, said[0] ? ": " : "", said);
}

/* --- the page's radar (web/): while it asks, each answer goes out over the console too, a line a
 * plane in the free networks' own words, so its sky_read takes them as they are. a line each: a
 * list in one piece is more than the 3.5" has to spare, and the log may fall between lines. the
 * page says OWLOS SKY ON every half minute: a page that went away stops it by itself --- */
#define SKY_OUT_MS 60000u
static volatile uint32_t s_sky_until;                  // now_ms till which it goes out
static volatile uint32_t s_pick;                       // OWLOS PICK from the page: the plane picked there
static volatile int      s_pick_new;
static uint32_t          s_sel_told;                   // the picked plane the page knows of

static int page_there(void) { return (int32_t)(s_sky_until - now_ms()) > 0; }

static void sky_out(void)
{
    static const char *const CATEGORY[KINDS] = {
        [KIND_JET] = "A3", [KIND_LIGHT] = "A1", [KIND_BIZJET] = "A2", [KIND_HEAVY] = "A5", [KIND_FAST] = "A6",
        [KIND_HELI] = "A7", [KIND_GLIDER] = "B1", [KIND_BALLOON] = "B2", [KIND_DRONE] = "B6", [KIND_GROUND] = "C1",
    };
    static plane_t p;                                  // only this task: off its stack
    char server[sizeof s_server];
    if (!page_there()) return;
    lock();
    int n = s_sky.n, total = s_sky.total;
    snprintf(server, sizeof server, "%s", s_server);
    unlock();
    printf("OWLOS SKY BEGIN %s\n", server);
    for (int i = 0; i < n; i++) {
        lock();
        int here = i < s_sky.n;
        if (here) p = s_sky.p[i];
        unlock();
        if (!here) break;
        char alt[16];
        if (p.ground) snprintf(alt, sizeof alt, "\"ground\"");
        else snprintf(alt, sizeof alt, "%d", (int)p.alt);
        printf("OWLOS PLANE {\"hex\":\"%06x\",\"flight\":\"%s\",\"r\":\"%s\",\"t\":\"%s\",\"category\":\"%s\",\"lat\":%.5f,\"lon\":%.5f,"
               "\"alt_baro\":%s,\"gs\":%.1f,\"track\":%.1f,\"baro_rate\":%d,\"squawk\":\"%04d\",\"seen_pos\":%.1f,"
               "\"nav_altitude_mcp\":%d,\"nav_heading\":%d,\"ias\":%d,\"mach\":%.3f,\"wd\":%d,\"ws\":%d}\n",
               (unsigned)p.icao, p.flight, p.reg, p.type, CATEGORY[p.kind < KINDS ? p.kind : KIND_JET], (double)p.lat, (double)p.lon,
               alt, (double)p.gs, (double)p.track, (int)p.vrate, (int)p.squawk, (double)(now_ms() - p.seen) / 1000.0,
               p.sel_fl * 100, (int)p.sel_hdg, (int)p.ias, (double)p.mach / 1000.0, (int)p.wind_dir, (int)p.wind_kt);
    }
    printf("OWLOS SKY END %d\n", total);
}

#if RADAR_OVERLAY
/* --- a sky to test with (fork addition, Waveshare 7/7B builds): the same planes on every run, so builds
 * can be compared. OWLOS SKYSAVE keeps the sky as it is now (NVS); OWLOS SKYPLAY 1 shows that one, still,
 * from now on and after restarts, instead of asking the network; OWLOS SKYPLAY 0 goes back to it */
#include "nvs.h"
static CORE_BIG sky_t s_sky_saved;
static volatile int   s_skyplay, s_skysave;

static void skyplay_load(void)
{
    nvs_handle_t h;
    if (nvs_open("skyplay", NVS_READONLY, &h) != ESP_OK) return;
    uint8_t on = 0;
    size_t sz = sizeof s_sky_saved;
    if (nvs_get_u8(h, "on", &on) == ESP_OK && on && nvs_get_blob(h, "sky", &s_sky_saved, &sz) == ESP_OK &&
        sz == sizeof s_sky_saved) {
        s_skyplay = 1;
        ESP_LOGW(TAG, "test sky: %d planes, the same each run (OWLOS SKYPLAY 0 to go back to the network)", s_sky_saved.n);
    }
    nvs_close(h);
}

static void skyplay_store(int save, int on)
{
    nvs_handle_t h;
    if (nvs_open("skyplay", NVS_READWRITE, &h) != ESP_OK) return;
    if (save) {
        lock();
        s_sky_saved = s_sky;
        unlock();
        esp_err_t e = nvs_set_blob(h, "sky", &s_sky_saved, sizeof s_sky_saved);
        ESP_LOGW(TAG, "test sky kept: %d planes, %s", s_sky_saved.n, esp_err_to_name(e));
    }
    if (on >= 0) {
        nvs_set_u8(h, "on", (uint8_t)on);
        s_skyplay = on;
        ESP_LOGW(TAG, "test sky %s", on ? "shown from now on, after restarts too" : "off: the network again");
    }
    nvs_commit(h);
    nvs_close(h);
}

// the kept sky put in place, its planes held still where they were
static void skyplay_show(void)
{
    const uint32_t now = now_ms();
    lock();
    s_sky = s_sky_saved;
    for (int i = 0; i < s_sky.n; i++) {
        plane_t *p = &s_sky.p[i];
        p->gs = 0.0f;                                  // still: nothing to move it on
        p->seen = p->heard = now;
        p->first = now - 600000u;                      // long seen: no ring opening round it
    }
    s_sky.updated = now;
    s_sky.status = SKY_OK;
    unlock();
}
#endif

static void fetch_task(void *arg)
{
    (void)arg;
    static CORE_BIG sky_reader_t rd;                   // the answer is read into this as it comes
    static char url[200];
    int next = -1;                                     // free server to try, -1 = the one in the settings
    uint32_t down = 0;                                 // since when the WiFi is not there
#if RADAR_OVERLAY
    skyplay_load();
#endif
    for (;;) {
#if RADAR_OVERLAY
        if (s_skysave) {                               // asked on the console: done here, where the sky is written
            skyplay_store(s_skysave & 1, (s_skysave >> 1) - 1);
            s_skysave = 0;
        }
        if (s_skyplay) {                               // the kept sky instead of the network's
            skyplay_show();
            vTaskDelay(pdMS_TO_TICKS(1000));
            continue;
        }
#endif
        fr_cfg_t c;
        int configured, fr24;
        static char key[sizeof s_sec.key];
        home_find();                                   // first: the planes are asked round the new home
        lock();
        c = s_cfg;
        configured = s_sec.ssid[0] != 0;
        fr24 = c.source == SRC_FR24 && s_sec.key[0];
        memcpy(key, s_sec.key, sizeof key);
        sky_hide_ground(&s_sky, !c.ground);            // the screen's or the site's setting, at once
        unlock();
        geocode();                                     // home's country before anything is asked round it
        const int refused = home_refused();
        if (refused) {
            s_map.ready = 0;                           // no map of it either, the one shown goes
        } else {
            map_service(&c);                          // first: a new look or range shows its map before the planes come
        }
        uint32_t every = FREE_EVERY_MS;
        const int up = net_up();
        if (up && s_opt.clock) net_time_start();
        int trying = 0;
        if (configured && !up) {
            uint32_t now = now_ms();
            if (!down) down = now;
            trying = now - (s_wifi_try > down ? s_wifi_try : down) < 20000u;   // a router takes a while
        } else {
            down = 0;
        }
        /* no WiFi: none set, or the one set is not there. the demo sky flies then and the screen
         * says so in big letters; the real one comes back with the WiFi. the two never mix */
        const int demo = !configured || (!up && !trying);
        if (demo != s_demo_on) {
            lock();
            s_sky.n = 0;
            s_demo_on = demo;
            unlock();
        }
        if (demo) {
            static uint32_t told;                      // the demo answers like a server: every 5 s
            uint32_t now = now_ms();
            if (!configured || !s_sky.n || now - told >= FREE_EVERY_MS - 50u) {
                char *buf = malloc(DEMO_BYTES);
                if (!buf) { vTaskDelay(pdMS_TO_TICKS(1000)); continue; }
                told = now;
                int n = demo_json(&s_demo, c.lat, c.lon, range_km(&c), now, buf, DEMO_BYTES);
                lock();
                sky_read_begin(&rd, &s_sky, 0, now);
                sky_read(&rd, buf, n);
                sky_read_end(&rd, &s_sky);
                sky_forget(&s_sky, now);
                snprintf(s_server, sizeof s_server, "DEMO");
                s_fetched = now;
                unlock();
                free(buf);
                sky_out();
            }
            if (configured) every = 1000;              // the WiFi set may come back any moment: look each second
        } else if (!up) {
            lock();
            s_sky.status = SKY_CONNECTING;
            snprintf(s_sky.note, sizeof s_sky.note, "CONNECTING");
            unlock();
            every = 1000;
        } else if (refused) {
            static uint32_t said;
            lock();
            s_sky.n = 0;
            s_sky.status = SKY_HTTP;                   // said on the screen as what is wrong, in the alarm colour
            snprintf(s_sky.note, sizeof s_sky.note, "NOT AVAILABLE IN RUSIA");
            unlock();
            if (!said || now_ms() - said > 600000u) {
                ESP_LOGW(TAG, "home is in Rusia: no planes and no map are asked");
                said = now_ms();
            }
            update_check();
        } else {
            int server = next < 0 ? (c.server < FREE_SERVERS ? c.server : 0) : next;
            if (fr24) fr24_url(&c, url, sizeof url);
            else free_url(&c, server, url, sizeof url);
            static char said[sizeof url];
            if (strcmp(url, said)) {                   // what goes out, once each time it changes (no key in it)
                ESP_LOGI(TAG, "asking %s", url);
                snprintf(said, sizeof said, "%s", url);
            }
            int status = 0;
            int64_t t0 = esp_timer_get_time();
            lock();
            sky_read_begin(&rd, &s_sky, fr24, now_ms());   // only the home is taken from the sky here
            unlock();
            int len = net_get(url, fr24 ? key : NULL, to_reader, &rd, &status);
            int ms = (int)((esp_timer_get_time() - t0) / 1000);
            uint32_t now = now_ms();
            int n = -2;
            lock();
            if (len > 0 && status == 200) {
                rd.now = now;                          // heard now; positions keep the time the request went out
                n = sky_read_end(&rd, &s_sky);
                if (n < 0) {
                    s_sky.status = SKY_PARSE;
                    snprintf(s_sky.note, sizeof s_sky.note, "STRANGE ANSWER");
                } else {
                    snprintf(s_server, sizeof s_server, "%s", fr24 ? "FLIGHTRADAR24" : FREE_NAME[server]);
                    s_fetched = now;
                    char came[40] = "";
                    uint32_t pick = 0;
                    light_news(watch_sky(&s_watch, &s_sky, now, came, sizeof came, &pick), now);   // a watched one round home?
                    light_hold(&s_light, LIGHT_RANGE, watch_near(&s_watch) > 0, now);
                    light_sky(&s_light, &s_light_cfg, &s_sky, unit_km(c.dist_unit), now);   // an emergency, a plane near
                    if (came[0]) {
                        say_locked(came, "A WATCHED PLANE", 10000u);
                        ESP_LOGI(TAG, "watch: %s", came);
                    }
                    if (pick) s_watch_pick = pick;
                }
            } else {
                s_sky.status = SKY_HTTP;
                if (status) snprintf(s_sky.note, sizeof s_sky.note, "%s HTTP %d", fr24 ? "FR24" : FREE_NAME[server], status);
                else snprintf(s_sky.note, sizeof s_sky.note, "%s NO ANSWER", fr24 ? "FR24" : FREE_NAME[server]);
            }
            s_http = status;
            sky_forget(&s_sky, now);
            int planes = s_sky.n;
            unlock();
            if (n >= 0) sky_out();
            if (!fr24 && n < 0) next = (server + 1) % FREE_SERVERS;   // that one failed: the next one next time
            else if (!fr24) next = server;
            ESP_LOGI(TAG, "%s: HTTP %d, %d bytes in %d ms, %d planes in it, %d kept, %d on the list, heap %u, at least %u since power on",
                     fr24 ? "FR24" : FREE_NAME[server], status, len, ms, rd.total, n, planes,
                     (unsigned)heap_caps_get_free_size(MALLOC_CAP_8BIT), (unsigned)heap_caps_get_minimum_free_size(MALLOC_CAP_8BIT));
            if (fr24) every = FR24_EVERY_MS;
            if (status == 429) every *= 3;            // too many requests: slow down
            if (status != 429) watch_lookup(&rd, server);   // on the free networks, FR24 or not
            update_check();
            alive_report();
        }
        lock();                                        // the LED's no data: the WiFi set is not there, or no answer
        light_hold(&s_light, LIGHT_NODATA, configured && (demo || s_sky.status >= SKY_HTTP), now_ms());
        unlock();
        ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(every)); // a new setting cuts the wait short
    }
}

/* --- settings from the site, over the USB line (console.h) --- */

// a string as JSON wants it: a quote or a backslash in a network name would end it early
static const char *json_esc(const char *s, char *out, int cap)
{
    int n = 0;
    for (; *s && n < cap - 3; s++) {
        if (*s == '"' || *s == '\\') out[n++] = '\\';
        out[n++] = (unsigned char)*s < 32 ? '?' : *s;
    }
    out[n] = 0;
    return out;
}

static void say_cfg(void)
{
    char ssid[70], place[84], country[68], watch[WATCH_TEXT];
    static char nets[NETS_KEPT * 72];                  // used under the lock: the site's line and the screen both say this
    lock();
    watch_text(&s_watch, watch, sizeof watch);         // letters, digits, dashes and commas: nothing to escape
    nets[0] = 0;
    for (int i = 0, at = 0; i < s_nets.count; i++)     // names only: a password goes in, never out
        at += snprintf(nets + at, sizeof nets - (size_t)at, "%s\"%s\"", i ? "," : "", json_esc(s_nets.n[i].ssid, ssid, sizeof ssid));
    int led = 0;                                       // the LED's events as bits: 1 sos, 2 in range, 4 take-off, 8 landing, 16 near, 32 no data
    for (int ev = 0; ev < LIGHT_EVENTS; ev++) led |= s_light_cfg.on[ev] ? 1 << ev : 0;
    printf("OWLOS CFG {\"ssid\":\"%s\",\"nets\":[%s],\"lat\":%.5f,\"lon\":%.5f,\"place\":\"%s\",\"country\":\"%s\",\"src\":\"%s\",\"key\":%s,"
           "\"up\":%d,\"flip\":%d,\"theme\":%d,\"look\":%d,\"map\":%d,\"ground\":%d,\"watch\":\"%s\","
           "\"vectors\":%d,\"beam_step\":%d,\"clock\":%d,\"block\":%d,\"autopilot\":%d,\"lost\":%d,\"conflict\":%d,"
           "\"fl_lo\":%d,\"fl_hi\":%d,\"utc\":%u,"
           "\"led\":%d,\"led_level\":%d,\"led_hold\":%d,\"led_near\":%d,\"led_red\":%d,\"version\":\"%s\",\"newest\":\"%s\","
           "\"uptime\":%u,\"ran\":%u,\"reset\":\"%s\",\"screen\":%d,"
           "\"range\":%.2f,\"dist_unit\":%d,\"alt_unit\":%d,\"labels\":%d,\"help\":%d,\"bare\":%d}\n",
           json_esc(s_sec.ssid, ssid, sizeof ssid), nets, (double)s_cfg.lat, (double)s_cfg.lon,
           json_esc(s_cfg.place, place, sizeof place), json_esc(s_cfg.country, country, sizeof country),
           s_cfg.source == SRC_FR24 ? "fr24" : "free", s_sec.key[0] ? "true" : "false", s_cfg.up, s_cfg.flip,
           s_cfg.theme, s_cfg.look, s_cfg.map, s_cfg.ground, watch, s_opt.vectors, s_opt.beam_step, s_opt.clock,
           s_opt.block, s_opt.autopilot, s_opt.lost, s_opt.conflict, s_opt.fl_lo, s_opt.fl_hi,
           (unsigned)net_utc(), led, s_light_cfg.level, s_light_cfg.hold,
           light_near_units(&s_light_cfg), light_red_gpio(&s_light_cfg), FLIGHT_VERSION, s_newest,
           (unsigned)(now_ms() / 1000u), (unsigned)s_ran_s, s_reset, s_screen,
           (double)s_cfg.range, s_cfg.dist_unit, s_cfg.alt_unit, s_cfg.labels, s_help, s_bare);
    unlock();
}

/* the list changed: its first network is what the secrets keep, the others are saved here */
static void nets_changed(secrets_t *s)
{
    memcpy(s->ssid, s_nets.n[0].ssid, sizeof s->ssid);
    memcpy(s->pass, s_nets.n[0].pass, sizeof s->pass);
    store_save_nets(&s_nets);
}

/* the name of this build on the site */
static const char *board_name(void)
{
#if RADAR_DISPLAY == DISPLAY_ILI9341_320x240
    return "ili9341";
#elif RADAR_DISPLAY == DISPLAY_ST7796_480x320
    return "st7796";
#elif RADAR_DISPLAY == DISPLAY_ILI9486_480x320
    return "ili9486";
#elif RADAR_DISPLAY == DISPLAY_GC9A01_240x240
    return "gc9a01";
#elif RADAR_DISPLAY == DISPLAY_ES3C28P_320x240
    return "es3c28p";
#elif RADAR_DISPLAY == DISPLAY_NMCYDC5_320x240
    return "nmcydc5";
#elif RADAR_DISPLAY == DISPLAY_CROWADV28_320x240
    return "crowadv28";
#elif RADAR_DISPLAY == DISPLAY_CROWADV50_800x480
    return "crowadv50";
#elif RADAR_DISPLAY == DISPLAY_JC8048W550_800x480
    return "jc8048w550";
#elif RADAR_DISPLAY == DISPLAY_TAB5_1280x720
    return "tab5";
#elif RADAR_DISPLAY == DISPLAY_4827S043_480x272
    return "esp4827s043";
#elif RADAR_DISPLAY == DISPLAY_CROW70_800x480
    return "crow70";
#elif RADAR_DISPLAY == DISPLAY_WAVE7_800x480
    return "wave7";
#elif RADAR_DISPLAY == DISPLAY_WAVE7B_1024x600
    return "wave7b";
#else
    return "other";
#endif
}

/* who is on this cable: the page keeps several boards apart by the MAC, a port's number can change */
static void say_ready(void)
{
    uint8_t m[6] = { 0 };
#if CONFIG_IDF_TARGET_ESP32P4
    esp_read_mac(m, ESP_MAC_BASE);                      // the WiFi's MAC is the C6's: the P4's own base one, as fixed to this board
#else
    esp_read_mac(m, ESP_MAC_WIFI_STA);
#endif
    printf("OWLOS READY flight-radar %s %s %02x%02x%02x%02x%02x%02x\n", FLIGHT_VERSION, board_name(), m[0], m[1], m[2], m[3], m[4], m[5]);
}

static void on_line(const char *line)
{
    if (!strncmp(line, "OWLOS HELLO", 11)) { say_ready(); return; }
    if (!strncmp(line, "OWLOS GET", 9)) { say_cfg(); return; }
    if (!strcmp(line, "OWLOS SKIP")) { s_skip_align = 1; return; }
#if RADAR_OVERLAY
    // the test sky (see skyplay_load): bit 0 keep the sky now, bits 1.. 1 off / 2 on, 0 leave
    if (!strcmp(line, "OWLOS SKYSAVE")) { s_skysave = 1; if (s_fetch) xTaskNotifyGive(s_fetch); return; }
    if (!strcmp(line, "OWLOS SKYPLAY 1")) { s_skysave = 2 << 1; if (s_fetch) xTaskNotifyGive(s_fetch); return; }
    if (!strcmp(line, "OWLOS SKYPLAY 0")) { s_skysave = 1 << 1; if (s_fetch) xTaskNotifyGive(s_fetch); return; }
#endif
    if (!strcmp(line, "OWLOS SKY ON")) {               // the page's radar, see sky_out
        s_align_told = -1;
        int was = page_there();
        s_sky_until = now_ms() + SKY_OUT_MS;
        if (!was) xTaskNotifyGive(s_fetch);            // the first answer now, not in five seconds
        return;
    }
    if (!strcmp(line, "OWLOS SKY OFF")) { s_sky_until = now_ms(); return; }
    if (!strncmp(line, "OWLOS PICK ", 11)) {           // a plane tapped on the page, 0: let go
        s_pick = (uint32_t)strtoul(line + 11, NULL, 16);
        s_pick_new = 1;
        return;
    }
    if (strncmp(line, "OWLOS SET ", 10)) return;
    const char *j = line + 10;
    secrets_t sec;
    fr_cfg_t c;
    lock();
    sec = s_sec;
    c = s_cfg;
    unlock();
    double v;
    char src[8];
    int wifi = 0, home = 0;
    if (json_str(j, "ssid", sec.ssid, sizeof sec.ssid)) wifi = 1;
    if (json_str(j, "pass", sec.pass, sizeof sec.pass)) wifi = 1;
    char forget[33] = "";
    json_str(j, "forget", forget, sizeof forget);
    json_str(j, "key", sec.key, sizeof sec.key);
    if (json_num(j, "lat", &v)) { c.lat = (float)v; home = 1; }
    if (json_num(j, "lon", &v)) { c.lon = (float)v; home = 1; }
    int named = json_str(j, "place", c.place, sizeof c.place);
    int land = json_str(j, "country", c.country, sizeof c.country);
    if (home && !named) c.place[0] = 0;                    // a new home without a name: the board asks OSM for it
    if (home && !land) c.country[0] = 0;
    if (json_str(j, "src", src, sizeof src)) c.source = !strcmp(src, "fr24") ? SRC_FR24 : SRC_FREE;
    if (c.source == SRC_FR24 && !sec.key[0]) c.source = SRC_FREE;
    // how the screen shows it: a board without touch has no other way to these
    if (json_num(j, "up", &v)) c.up = (int16_t)((((int)lround(v)) % 360 + 360) % 360);
    if (json_num(j, "flip", &v)) c.flip = v != 0;
    if (json_num(j, "theme", &v) && v >= 0 && v < UI_THEMES) c.theme = (uint8_t)v;
    if (json_num(j, "look", &v) && v >= 0 && v < LOOKS) c.look = (uint8_t)v;
    if (json_num(j, "map", &v)) c.map = v != 0;
    if (json_num(j, "ground", &v)) c.ground = v != 0;
    // what the page's radar changes besides those
    if (json_num(j, "range", &v) && v >= RANGE_MIN && v <= RANGE_MAX) c.range = (float)v;
    if (json_num(j, "dist_unit", &v) && v >= 0 && v <= DIST_MI) c.dist_unit = (uint8_t)v;
    if (json_num(j, "alt_unit", &v) && v >= 0 && v <= ALT_M) c.alt_unit = (uint8_t)v;
    if (json_num(j, "labels", &v)) c.labels = v != 0;
    scope_opt_t opt;                                   // the radar's options
    lock();
    opt = s_opt;
    unlock();
    int opted = 0;
    const struct { const char *name; uint8_t *on; } SWITCHES[] = {
        { "vectors", &opt.vectors }, { "beam_step", &opt.beam_step }, { "clock", &opt.clock }, { "block", &opt.block },
        { "autopilot", &opt.autopilot }, { "lost", &opt.lost }, { "conflict", &opt.conflict },
    };
    for (size_t i = 0; i < sizeof SWITCHES / sizeof SWITCHES[0]; i++)
        if (json_num(j, SWITCHES[i].name, &v)) { *SWITCHES[i].on = v != 0; opted = 1; }
    if (json_num(j, "fl_lo", &v) && v >= 0 && v <= SCOPE_FL_MAX) { opt.fl_lo = (int16_t)v; opted = 1; }
    if (json_num(j, "fl_hi", &v) && v >= 0 && v <= SCOPE_FL_MAX) { opt.fl_hi = (int16_t)v; opted = 1; }
    if (json_num(j, "help", &v) && v >= 0 && v <= 2) {     // the site's question, answered there
        lock();
        s_help = (uint8_t)v;
        s_help_new = 1;
        unlock();
        store_save_help((int)v);
    }
    if (json_num(j, "bare", &v) && (v == 0 || v == 1)) {   // the radar's panel away or back, from the site
        lock();
        s_bare = (uint8_t)v;
        s_bare_new = 1;
        unlock();
        store_save_bare((int)v);
    }
    char watch[WATCH_TEXT];
    int watched = json_str(j, "watch", watch, sizeof watch);   // "" empties it
    light_cfg_t light;
    lock();
    light = s_light_cfg;
    unlock();
    int lit = 0;                                       // the LED: its events as bits (say_cfg), how bright, how long
    if (json_num(j, "led", &v) && v >= 0 && v < (1 << LIGHT_EVENTS)) {
        for (int ev = 0; ev < LIGHT_EVENTS; ev++) light.on[ev] = ((int)v >> ev) & 1;
        lit = 1;
    }
    if (json_num(j, "led_level", &v) && v >= 0 && v < LIGHT_LEVELS) { light.level = (uint8_t)v; lit = 1; }
    if (json_num(j, "led_hold", &v) && v >= 0 && v < LIGHT_HOLDS) { light.hold = (uint8_t)v; lit = 1; }
    int near = -1;                                     // 1, 2, 5 or 10 of the distance unit
    if (json_num(j, "led_near", &v) && (near = light_near_index((int)v)) >= 0) { light.near = (uint8_t)near; lit = 1; }
    int red = -1;                                      // the red one's GPIO, 4 or 22
    if (json_num(j, "led_red", &v) && (red = light_red_index((int)v)) >= 0) { light.red = (uint8_t)red; lit = 1; }
    if (json_num(j, "led_test", &v) && v) s_light_test = 1;   // nothing kept: the screen task shows it
#if LCD_PROFILES
    if (json_num(j, "screen", &v) && v >= 0 && v < LCD_PROFILES) s_screen_set = (int)v;   // the screen's start-up, board.h LCD_PROFILES: on the CYD 0 v1, 1 v2, 2 v3
#endif
    if (c.lat < -90 || c.lat > 90 || c.lon < -180 || c.lon > 180) { printf("OWLOS ERR home is not on Earth\n"); return; }
    lock();
    if (wifi && sec.ssid[0]) nets_put(&s_nets, sec.ssid, sec.pass);
    else if (wifi) nets_forget(&s_nets, s_sec.ssid);    // "ssid":"": the one in use goes, the next takes its place
    if (forget[0] && nets_forget(&s_nets, forget)) wifi = 1;
    if (wifi) nets_changed(&sec);
    unlock();
    store_save_secrets(&sec);
    store_save_cfg(&c);
    lock();
    s_sec = sec;
    if (s_cfg.lat != c.lat || s_cfg.lon != c.lon) s_home_cc[0] = 0;   // another home: its country, geocode() asks
    s_cfg = c;
    s_cfg_new = 1;
    if (home) sky_home(&s_sky, c.lat, c.lon);
    if (wifi) { s_sky.n = 0; s_sky.status = SKY_CONNECTING; s_wifi_try = now_ms(); }   // the demo planes go
    if (watched) {
        watch_set(&s_watch, watch);
        watch_text(&s_watch, watch, sizeof watch);   // as it is kept: capitals, each once
        s_watch_asked = 0;
    }
    if (lit) {
        s_light_cfg = light;
        s_light_new = 1;
    }
    if (opted) {
        s_opt = opt;
        s_opt_new = 1;
    }
    unlock();
    if (lit) store_save_light(&light);
    if (opted) store_save_opt(&opt);
    if (watched) {
        store_save_watch(watch);
        ESP_LOGI(TAG, "watch list from the site: %s", watch[0] ? watch : "none");
    }
    if (wifi) net_reconnect(&s_nets);
    ESP_LOGI(TAG, "settings from the site: network \"%s\", home %.4f %.4f, %s%s", sec.ssid, (double)c.lat, (double)c.lon,
             c.source == SRC_FR24 ? "FR24" : "free networks", sec.key[0] ? ", FR24 key stored" : "");
    printf("OWLOS OK\n");
    xTaskNotifyGive(s_fetch);
}

/* the screen's WiFi page: a network and the password typed for it, none typed for one it knows.
 * they go where the site's go, and the screen's copy of the password is wiped here */
static void wifi_from_screen(void)
{
    secrets_t sec;
    lock();
    sec = s_sec;
    const net_kept_t *kept = nets_find(&s_nets, s_ui.wifi_ssid);
    nets_put(&s_nets, s_ui.wifi_ssid, kept && !s_ui.wifi_pass[0] ? kept->pass : s_ui.wifi_pass);
    nets_changed(&sec);
    unlock();
    memset(s_ui.wifi_pass, 0, sizeof s_ui.wifi_pass);
    store_save_secrets(&sec);
    lock();
    s_sec = sec;
    s_sky.n = 0;
    s_sky.status = SKY_CONNECTING;
    s_wifi_try = now_ms();
    unlock();
    net_reconnect(&s_nets);
    memset(sec.pass, 0, sizeof sec.pass);
    ESP_LOGI(TAG, "WiFi from the screen: network \"%s\"", sec.ssid);
    xTaskNotifyGive(s_fetch);
}

/* --- the screen --- */

typedef struct { uint8_t down; int16_t x, y, rx, ry; uint32_t t; } tsample_t;
static tsample_t s_ts[24];
static int       s_nts;
static uint32_t  s_ts_last;
static uint32_t  s_touch_us, s_send_us;   // reading the touch and handing strips over, for the 10 s log

static void touch_sample(int force)
{
    uint32_t now = now_ms();
    if (s_nts >= (int)(sizeof s_ts / sizeof s_ts[0]) || (!force && now - s_ts_last < 15u)) return;
    int rx = 0, ry = 0, x = 0, y = 0;
    uint32_t t0 = now_us();
    int down = tp_read(&rx, &ry);
    s_touch_us += now_us() - t0;
    if (down) touch_apply(&s_ui.cal, rx, ry, &x, &y);
    s_ts[s_nts++] = (tsample_t){ (uint8_t)down, (int16_t)x, (int16_t)y, (int16_t)rx, (int16_t)ry, now };
    s_ts_last = now;
}

/* --- the second core: on the 3.5" in the radar look it draws every other strip while this core
 * draws the one above it, and the two go out in one transfer. its strip, its stack and the DMA
 * buffers' second half are taken only for that look: the square map wants the memory, and there
 * the SPI is what a frame waits for anyway --- */
#if DISP_HELPER
#define HELPER_STACK 7168
static TaskHandle_t      s_helper;
static SemaphoreHandle_t s_help_go, s_help_done;
static canvas_t          s_help_cv;            // the strip it draws now
static volatile int      s_help_stop;
static uint32_t          s_help_wait_us;       // this core waiting for it, for the 10 s log

static void helper_task(void *arg)
{
    (void)arg;
    for (;;) {
        xSemaphoreTake(s_help_go, portMAX_DELAY);
        if (s_help_stop) break;
        ui_strip(&s_ui, &s_help_cv);                   // the strips only read what ui_frame set up
        disp_pack_under(&s_help_cv);                   // into the transfer, under this core's strip
        xSemaphoreGive(s_help_done);
    }
    xSemaphoreGive(s_help_done);
    vTaskDelete(NULL);
}

// between frames: on when wanted and the memory is there (with room left for TLS), off otherwise
static void helper_set(int on)
{
    if (on == (s_helper != NULL)) return;
    const size_t px = GFX_PW * DISP_STRIP * sizeof(uint32_t), dma = GFX_PW * DISP_STRIP * sizeof(uint16_t) * 2;
    if (!on) {
        s_help_stop = 1;
        xSemaphoreGive(s_help_go);
        xSemaphoreTake(s_help_done, portMAX_DELAY);   // out of its loop, it deletes itself
        s_helper = NULL;
        heap_caps_free(s_help_cv.px);
        s_help_cv.px = NULL;
        disp_rows(DISP_STRIP);
        ESP_LOGI(TAG, "the second core stops drawing, heap %u", (unsigned)heap_caps_get_free_size(MALLOC_CAP_8BIT));
        return;
    }
    if (heap_caps_get_free_size(MALLOC_CAP_8BIT) < px + dma + HELPER_STACK + 48 * 1024) return;
    if (!s_help_go) {
        s_help_go = xSemaphoreCreateBinary();
        s_help_done = xSemaphoreCreateBinary();
    }
    s_help_cv = (canvas_t){ heap_caps_malloc(px, MALLOC_CAP_INTERNAL), 0, DISP_STRIP, 1 };
    if (!s_help_cv.px) return;
    s_help_stop = 0;
    if (!disp_rows(2 * DISP_STRIP) ||
        xTaskCreatePinnedToCore(helper_task, "strips", HELPER_STACK, NULL, 4, &s_helper, 0) != pdPASS) {
        s_helper = NULL;
        disp_rows(DISP_STRIP);
        heap_caps_free(s_help_cv.px);
        s_help_cv.px = NULL;
        return;
    }
    ESP_LOGI(TAG, "the second core draws every other strip, heap %u", (unsigned)heap_caps_get_free_size(MALLOC_CAP_8BIT));
}
#endif

#if LCD_PROFILES
/* the 2.8" CYD's screen revision (board.h): BOOT in the first seconds, or the site. saved */
static void pick_screen(int screen, uint32_t now, const char *by)
{
    disp_profile(screen);
    store_save_screen(screen);
    s_screen = screen;
#if RADAR_HAS_BUTTON
    ui_screen_name = disp_profile_name();
#endif
    snprintf(s_ui.toast, sizeof s_ui.toast, "SCREEN %s", disp_profile_name());
    s_ui.toast_until = now + 3000u;
    ESP_LOGI(TAG, "%s: the screen is now %s, saved", by, disp_profile_name());
}
#endif

/* the screen turned 180 degrees, from its setup page or from the site. the touch alignment
 * turns with it, so it needs no new crosses */
static void screen_flip(int on)
{
#if DISP_LCD_SPI || DISP_LCD_RGB || DISP_LCD_DSI || DISP_ILI9486 || DISP_WAVE7
    disp_flip(on);
#endif
#if RADAR_TOUCH_CAL
    touch_cal_t *c = &s_ui.cal;
    c->a = -c->a; c->b = -c->b; c->c = (float)(GFX_W - 1) - c->c;
    c->d = -c->d; c->e = -c->e; c->f = (float)(GFX_H - 1) - c->f;
    store_save_cal(c);
#endif
    (void)on;
}

#if RADAR_OVERLAY
/* --- the boards with the moving layer (config.h RADAR_OVERLAY, fork addition) ---
 * the first core: a pass of the ui loop is a panel frame. it makes the moving layer for the frame
 * that starts next, and hands the background a copy of the ui's state when the background is ready
 * for another frame.
 * the second core: the panel's fill (the library's task, top priority), and under it the background,
 * drawn strip by strip from that copy into the frame that does not show, flipped in when whole. it
 * gets whatever time the fill leaves. a copy and not the ui's own state: the two cores draw at once */
#include "freertos/idf_additions.h"
#define TOP_ROWS 16                                    // the moving layer's band: 64 KB of RAM at 1024 wide. fewer bands, less of each plane done again a band
static canvas_t     s_top;                             // kept at 0 between uses (ovl.c)
static uint32_t    *s_bg_px;                           // the background's strip, DISP_STRIP rows
static ui_t        *s_ui_bg;                           // the state the background is drawn from, in the PSRAM
static TaskHandle_t s_bg_task;
static volatile int s_bg_busy;                         // from the copy till the frame is drawn
static volatile uint32_t s_bg_frames, s_bg_us;
static volatile int s_bg_view = -1;                    // the page the last whole background was drawn for
static int          s_view_up = -1;                    // and the page of the one on the screen now
static uint32_t     s_top_us;

static void bg_task(void *arg)
{
    (void)arg;
    canvas_t cv = { s_bg_px, 0, DISP_STRIP, 0, UI_LAYER_BG };
    for (;;) {
        ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
        const int64_t t0 = esp_timer_get_time();
        disp_bg_begin();
        for (int y = 0; y < GFX_PH; y += DISP_STRIP) {
            cv.y0 = y;
            cv.h = y + DISP_STRIP <= GFX_PH ? DISP_STRIP : GFX_PH - y;
            ui_strip(s_ui_bg, &cv);
            disp_strip(&cv);
        }
        disp_frame_end();                              // up from the next panel frame on
        s_bg_view = s_ui_bg->view;
        s_bg_us += (uint32_t)(esp_timer_get_time() - t0);
        s_bg_frames++;
        s_bg_busy = 0;
        vTaskDelay(1);                                 // the idle task of this core gets its turn: the watchdog looks for it
    }
}

/* the screen comes up before the WiFi, from a short task on the second core: the bounce buffers
 * and the strips want internal RAM in one piece, which the WiFi and the tasks leave in bits, and the
 * library's fill task stays on the core it was started from */
static SemaphoreHandle_t s_early_done;

static void early_task(void *arg)
{
    (void)arg;
    disp_flip(s_cfg.flip);
    disp_init();
    s_top = (canvas_t){ heap_caps_calloc(GFX_PW * TOP_ROWS, sizeof(uint32_t), MALLOC_CAP_INTERNAL), 0, TOP_ROWS, 0, UI_LAYER_TOP };
    s_bg_px = heap_caps_malloc(GFX_PW * DISP_STRIP * sizeof(uint32_t), MALLOC_CAP_INTERNAL);
    s_ui_bg = heap_caps_malloc(sizeof(ui_t), MALLOC_CAP_SPIRAM);
    xSemaphoreGive(s_early_done);
    vTaskDelete(NULL);
}

static void screen_early(void)
{
    s_early_done = xSemaphoreCreateBinary();
    xTaskCreatePinnedToCore(early_task, "screen", 6144, NULL, 5, NULL, portNUM_PROCESSORS > 1 ? 1 : 0);
    xSemaphoreTake(s_early_done, portMAX_DELAY);
    /* its stack in the PSRAM: the internal RAM has none to spare, and nothing here runs with the cache off */
    if (s_bg_px && s_ui_bg &&
        xTaskCreatePinnedToCoreWithCaps(bg_task, "background", 8192, NULL, 1, &s_bg_task, 1, MALLOC_CAP_SPIRAM) != pdPASS)
        s_bg_task = NULL;
    ESP_LOGI(TAG, "screen up before the WiFi: RAM %u free, %u in one piece, background %s, ui_t %u bytes",
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL), (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL),
             s_bg_task ? "on the second core" : "NOT THERE", (unsigned)sizeof(ui_t));
}

/* the planes move a pixel or a few at a time: their layer is made again at most this often. the sweep
 * is not in it, the fill turns that every panel frame. a pass between makes nothing and leaves the core
 * to the touch, the network and the pages */
#ifndef OVL_LAYER_MS
#define OVL_LAYER_MS 100
#endif
static uint32_t s_layer_at;
static uint32_t s_page_at, s_page_ms;                  // when the last page drawn here was done, and how long it took

static void frame_layered(uint32_t now)
{
    const uint32_t period = disp_frame_us();
    const int64_t start = esp_timer_get_time();
    if (!s_bg_busy && disp_bg_ready()) s_view_up = s_bg_view;   // the last background drawn is the one up
    const int direct = s_ui.view != VIEW_MAIN;         // a page with nothing moving on it: drawn here, see below
    const int bg_go = s_bg_task && !direct && !s_bg_busy && disp_bg_ready();
    /* a finger turning the compass: the layer is the ring alone (ui_top_info), made every pass so the
     * ring follows the finger at the panel's pace. the planes and the map catch up when it lets go */
    const int turning = !direct && s_ui.t_down && s_ui.t_turn == 1 && s_ui.t_moved;
    const int build = direct || turning || now - s_layer_at >= OVL_LAYER_MS;
    /* a page drawn here leaves the core free after it for half as long as it took, and at least 50 ms:
     * drawn back to back, the menus kept the core so busy its idle task never ran, and the watchdog
     * restarted the board */
    const uint32_t rest = s_page_ms / 2 > 50u ? s_page_ms / 2 : 50u;
    const int draw_here = direct && !s_bg_busy && disp_bg_ready() && now - s_page_at >= rest;
    if (!build && !bg_go && !draw_here) {
        touch_sample(0);
        return;
    }
    /* where things are in the middle of the time this layer will show: it is up after this build and a
     * panel frame, and stays till the next one, so ahead by a frame, the build's time and half the time
     * between layers, all as measured. a fixed guess left the planes' glow 20-30 degrees behind the beam
     * once the layer came only every ~350 ms */
    static uint32_t s_build_ms = 20, s_gap_ms = OVL_LAYER_MS;
    static int64_t  s_prev_build;
    if (build && !direct && !turning) {               // the ring alone is quick: not counted
        const int64_t t = esp_timer_get_time();
        if (s_prev_build) s_gap_ms = (s_gap_ms * 3u + (uint32_t)((t - s_prev_build) / 1000)) / 4u;
        s_prev_build = t;
    }
    const uint32_t lead = build && !direct ? s_build_ms + s_gap_ms / 2u : 0u;
    ui_frame(&s_ui, now + period / 1000u + lead);
    if (bg_go) {                                       // the background is free for another frame: this state
        memcpy(s_ui_bg, &s_ui, sizeof(ui_t));
        s_bg_busy = 1;
        xTaskNotifyGive(s_bg_task);
    }
    if (!build) goto drawn;
    ui_top_t ti;
    ui_top_info(&s_ui, &ti);
    /* the moving layer only over the radar page that is up: leaving a menu, the sweep waited for the
     * radar's background to replace the menu's, instead of turning over the menu meanwhile */
    if (s_view_up != VIEW_MAIN) ti.any = ti.beam = 0;
    ovl_beam_t b = { ti.beam, ti.cx, ti.cy, ti.r, ti.prim, ti.focus, ti.turn_ms, ti.holes };
    for (int k = 0; k < ti.holes; k++) {
        b.hx0[k] = (int16_t)ti.hx0[k]; b.hy0[k] = (int16_t)ti.hy0[k];
        b.hx1[k] = (int16_t)ti.hx1[k]; b.hy1[k] = (int16_t)ti.hy1[k];
    }
    ovl_build_begin(&b);                               // an empty layer where nothing moves: the rows say so
    for (int y = ti.any ? ti.y0 / TOP_ROWS * TOP_ROWS : GFX_PH; y < ti.y1; y += TOP_ROWS) {
        s_top.y0 = y;
        s_top.h = y + TOP_ROWS <= GFX_PH ? TOP_ROWS : GFX_PH - y;
        ui_top_strip(&s_ui, &s_top);
        ovl_build_band(&s_top, ti.x0, ti.x1);
    }
    ovl_build_end(&b);
    s_top_us += (uint32_t)(esp_timer_get_time() - start);
    s_layer_at = (uint32_t)(esp_timer_get_time() / 1000);   // from its end: the core has that long to itself before the next
    if (!turning) s_build_ms = (s_build_ms * 3u + (uint32_t)((esp_timer_get_time() - start) / 1000)) / 4u;
    else s_prev_build = 0;                             // the gap after a turn is not a planes' gap
drawn:
    touch_sample(0);
    /* the settings, the keyboard, the lists: nothing moves there, this core has the time the moving
     * layer would take, and a key wants its answer now. so the page is drawn here, from the ui's own
     * state, not handed to the second core's background task (which takes ~1 s a frame under the fill) */
    if (direct && !s_bg_busy && disp_bg_ready()) {
        s_bg_busy = 1;                                 // the background task keeps off meanwhile
        const int64_t t0 = esp_timer_get_time();
        canvas_t cv = { s_top.px, 0, TOP_ROWS, 0, UI_LAYER_BG };
        disp_bg_begin();
        for (int y = 0; y < GFX_PH; y += TOP_ROWS) {
            cv.y0 = y;
            cv.h = y + TOP_ROWS <= GFX_PH ? TOP_ROWS : GFX_PH - y;
            ui_strip(&s_ui, &cv);
            disp_strip(&cv);
            touch_sample(0);                           // a finger is read between strips, as the original loop does
        }
        memset(s_top.px, 0, GFX_PW * TOP_ROWS * sizeof(uint32_t));   // the moving layer's band again: from nothing
        disp_frame_end();
        s_bg_view = s_ui.view;
        s_bg_us += (uint32_t)(esp_timer_get_time() - t0);
        s_bg_frames++;
        s_bg_busy = 0;
        s_page_ms = (uint32_t)((esp_timer_get_time() - t0) / 1000);
        s_page_at = (uint32_t)(esp_timer_get_time() / 1000);
    }
}
#endif

static void ui_task(void *arg)
{
    (void)arg;
    fr_cfg_t c;
    lock();
    c = s_cfg;
    unlock();
#if LCD_PROFILES
    static const int ORDER[LCD_PROFILES] = LCD_ORDER;  // what BOOT goes through, the first one if none was picked
    int screen = store_load_screen();                  // which CYD revision the screen is
    if (screen < 0 || screen >= LCD_PROFILES) screen = ORDER[0];
    disp_profile(screen);
    s_screen = screen;
#endif
#if RADAR_OVERLAY
    uint32_t *work = s_top.px;                         // screen_early made them
    ovl_waiter(xTaskGetCurrentTaskHandle());
    ovl_planner_start(disp_frame_us, 0, 6);            // the next frame's busiest chunks, beside and above this task
#else
#if DISP_LCD_SPI || DISP_LCD_RGB || DISP_LCD_DSI || DISP_ILI9486
    disp_flip(c.flip);
#endif
    disp_init();
    uint32_t *work = heap_caps_malloc(GFX_PW * DISP_STRIP * sizeof(uint32_t), MALLOC_CAP_INTERNAL);
#endif
    if (!work || !ui_init(&s_ui, now_ms())) {
        ESP_LOGE(TAG, "not enough memory for the screen: RAM %u free, %u in one piece, heap %u", (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
                 (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL), (unsigned)heap_caps_get_free_size(MALLOC_CAP_8BIT));
        vTaskDelete(NULL);
    }
    ui_set_cfg(&s_ui, &c);
    lock();
    s_ui.light = s_light_cfg;                          // what lights the LED on the back
    s_ui.opt = s_opt;
    s_ui.help = s_help;                                // counted on the site's map: SET's COUNT ME
    s_ui.bare = s_bare;                                // the radar without its panel, as it was left
    unlock();
    led_init(light_red_gpio(&s_ui.light));
    s_ui.map = &s_map;                                 // the fetch task makes it, the scope draws it
    ui_clock_us = now_us;                              // the layers get timed, the 10 s log says where it goes
    ui_version = FLIGHT_VERSION;                       // ABOUT says it, the note compares the site's with it
#if LCD_PROFILES && RADAR_HAS_BUTTON
    ui_screen_name = disp_profile_name();              // the screen offers the other ones, by BOOT. none: the site does
#endif
    button_init();
    esp_task_wdt_add(NULL);
    tp_init();
#if RADAR_OVERLAY
    /* the Waveshare 7/7B (fork addition): a capacitive GT911 that counts the glass's own pixels, so how
     * its numbers fall on the screen is known: the layout is the glass scaled down (config.h GFX_SCALE),
     * turned with the picture when it is turned. the five-cross alignment is never started. a port of
     * this build to a resistive panel (an XPT2046 and the like) wants it back: take this block out and
     * the stored-alignment check below runs as on the other boards */
    s_ui.cal = (touch_cal_t){ (float)GFX_W / (float)GFX_PW, 0, 0, 0, (float)GFX_H / (float)GFX_PH, 0 };
    if (c.flip) {                                      // as screen_flip turns it
        touch_cal_t *k = &s_ui.cal;
        k->a = -k->a; k->b = -k->b; k->c = (float)(GFX_W - 1) - k->c;
        k->d = -k->d; k->e = -k->e; k->f = (float)(GFX_H - 1) - k->f;
    }
#elif RADAR_TOUCH_CAL
    int have = store_load_cal(&s_ui.cal);
    const float want = (float)GFX_W / (float)tp_raw_span();
    float sx = fabsf(s_ui.cal.a) + fabsf(s_ui.cal.b), sy = fabsf(s_ui.cal.d) + fabsf(s_ui.cal.e);
    if (!have || sx < want / 3 || sx > want * 3 || sy < want / 3 || sy > want * 3) ui_start_calibration(&s_ui);
#endif
    ESP_LOGI(TAG, "%s %s, %dx%d, free heap %u", RADAR_NAME, FLIGHT_VERSION, GFX_W, GFX_H,
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_8BIT));

    uint32_t frames = 0, t_stat = now_ms(), busy_us = 0, dirty = 0;
    int light = 0;
    for (;;) {
        uint32_t now = now_ms();
        ui_cmd_t cmd;
        memset(&cmd, 0, sizeof cmd);

        int ev = button_poll(now);
#if LCD_PROFILES
        /* a short BOOT press starts the screen as the next CYD revision: on the touch alignment
         * page (the first thing after flashing) or in the first seconds after power on, both say
         * so on the screen. held on the alignment page it still skips the alignment */
        if (ev == BTN_SHORT && (s_ui.view == VIEW_CAL || now < UI_SCREEN_PICK_MS)) {
            int at = 0;
            while (at < LCD_PROFILES - 1 && ORDER[at] != screen) at++;
            screen = ORDER[(at + 1) % LCD_PROFILES];
            pick_screen(screen, now, "BOOT");
            ev = 0;
        }
        if (s_screen_set >= 0) {                       // the site says which
            screen = s_screen_set;
            s_screen_set = -1;
            pick_screen(screen, now, "the site");
        }
#endif
        if (ev) ui_button(&s_ui, ev == BTN_LONG ? 2 : 1, &cmd);
        if (s_skip_align) {
            s_skip_align = 0;
            if (s_ui.view == VIEW_CAL) ui_button(&s_ui, 1, &cmd);
        }
        touch_sample(1);
        int finger = 0;                                // a sample comes every frame, with a finger or without
        for (int i = 0; i < s_nts; i++) finger |= s_ts[i].down;
        if (finger || ev) {                            // a finger or BOOT: what the LED shows is seen, it goes out
            lock();
            light_touch(&s_light);
            unlock();
        }
        for (int i = 0; i < s_nts; i++) {
            const tsample_t *s = &s_ts[i];
            ui_touch(&s_ui, s->down, s->x, s->y, s->rx, s->ry, s->t, &cmd);
        }
        s_nts = 0;

        if (s_cfg_new) {                               // from the site: it is the master now
            int was = c.flip;
            lock();
            c = s_cfg;
            s_cfg_new = 0;
            unlock();
            ui_set_cfg(&s_ui, &c);
            if (c.flip != was) screen_flip(c.flip);
        }
        if (cmd.cfg) {
            int flipped = s_ui.cfg.flip != c.flip;
            lock();
            s_ui.cfg.lat = s_cfg.lat;                  // home and its name come from s_cfg: the site, or home_find
            s_ui.cfg.lon = s_cfg.lon;
            memcpy(s_ui.cfg.place, s_cfg.place, sizeof s_ui.cfg.place);
            memcpy(s_ui.cfg.country, s_cfg.country, sizeof s_ui.cfg.country);
            s_cfg = s_ui.cfg;
            unlock();
            c = s_ui.cfg;
            dirty = now ? now : 1;
            if (flipped) screen_flip(c.flip);
        }
        if (cmd.refetch) xTaskNotifyGive(s_fetch);
        if (cmd.scan) net_scan_request(now);
        if (cmd.wifi) wifi_from_screen();
        if (cmd.home) {                                // typed on the screen: the fetch task finds it
            lock();
            snprintf(s_home_ask, sizeof s_home_ask, "%s", s_ui.home_text);
            unlock();
            xTaskNotifyGive(s_fetch);
        }
        if (cmd.watch) {                               // a new watch list typed on the screen
            char text[WATCH_TEXT];
            lock();
            watch_set(&s_watch, s_ui.watch_text);
            watch_text(&s_watch, text, sizeof text);
            s_watch_asked = 0;
            unlock();
            store_save_watch(text);
            ESP_LOGI(TAG, "watch list from the screen: %s", text[0] ? text : "none");
            xTaskNotifyGive(s_fetch);
        }
        if (s_said_new) {                              // what the fetch task found: a home, a watched plane
            lock();
            snprintf(s_ui.toast, sizeof s_ui.toast, "%s", s_said);
            s_ui.toast_until = now + s_said_ms;
            s_said_new = 0;
            unlock();
        }
        if (s_watch_pick) {                            // a watched one came in range: picked, unless one is
            if (!s_ui.sel) s_ui.sel = s_watch_pick;
            s_watch_pick = 0;
        }
        net_scan_poll(now);
        if (dirty && now - dirty > 3000u) {           // holding + is one flash write, not twenty
            store_save_cfg(&s_ui.cfg);
            dirty = 0;
        }
        if (cmd.save_cal) store_save_cal(&s_ui.cal);
        if (s_light_new) {                             // the site's LED settings
            lock();
            s_ui.light = s_light_cfg;
            s_light_new = 0;
            unlock();
        }
        if (cmd.light) {                               // SET's LED rows
            lock();
            s_light_cfg = s_ui.light;
            unlock();
            store_save_light(&s_ui.light);
        }
        if (s_opt_new) {                               // the site's radar options
            lock();
            s_ui.opt = s_opt;
            s_opt_new = 0;
            unlock();
        }
        if (cmd.opt) {                                 // the gear's rows
            lock();
            s_opt = s_ui.opt;
            unlock();
            store_save_opt(&s_ui.opt);
        }
        if (s_help_new) {                              // counted on the map: the site's answer
            lock();
            s_ui.help = s_help;
            s_help_new = 0;
            unlock();
        }
        if (cmd.help) {                                // SET's COUNT ME
            lock();
            s_help = s_ui.help;
            unlock();
            store_save_help(s_ui.help);
        }
        if (s_bare_new) {                              // the site's: the radar's panel away or back
            lock();
            s_ui.bare = s_bare;
            s_bare_new = 0;
            unlock();
        }
        if (cmd.bare) {                                // the radar's three dots
            lock();
            s_bare = s_ui.bare;
            unlock();
            store_save_bare(s_ui.bare);
        }
        if (s_pick_new) {                              // tapped on the page: picked here too
            s_pick_new = 0;
            s_ui.sel = s_sel_told = s_pick;
        }
        if (page_there()) {                            // and what a finger did here goes there
            if (cmd.cfg || cmd.opt || cmd.help || cmd.bare) say_cfg();
            if (s_ui.sel != s_sel_told) {
                s_sel_told = s_ui.sel;
                printf("OWLOS SEL %06x\n", (unsigned)s_ui.sel);
            }
            if ((s_ui.view == VIEW_CAL) != s_align_told) {   // its screen waits for crosses: the page says so
                s_align_told = s_ui.view == VIEW_CAL;
                printf("OWLOS ALIGN %d\n", s_align_told);
            }
        }
        uint8_t rgb[3];
        lock();
        if (cmd.light_test || s_light_test) {
            light_test(&s_light, now);
            ESP_LOGI(TAG, "LED test from %s: red, green, blue", s_light_test ? "the site" : "the screen");
            s_light_test = 0;
        }
        light_colour(&s_light, &s_ui.light, now, rgb);
        int red = light_red_gpio(&s_ui.light);
        unlock();
        led_red(red);                                  // SET's RED ON row or the site moved it
        led_set(rgb);

        lock();
        memcpy(&s_ui.snap, &s_sky, sizeof s_sky);
        net_t *n = &s_ui.net;
        n->configured = s_sec.ssid[0] != 0;
        n->has_key = s_sec.key[0] != 0;
        n->demo = (uint8_t)s_demo_on;
        n->wifi = (uint8_t)net_up();
        n->why = (uint8_t)net_why();
        net_ssid(n->ssid, sizeof n->ssid);
        snprintf(n->server, sizeof n->server, "%s", s_server);
        n->http = s_http;
        n->fetched = s_fetched;
        n->utc = net_utc();
        s_ui.watch = s_watch;                          // SET says what the watched planes do
        memcpy(s_ui.newest, s_newest, sizeof s_ui.newest);   // and ABOUT what the site has
        unlock();
        net_ip(n->ip, sizeof n->ip);
        int naps = 0;
        n->scan = (uint8_t)net_scan_get(n->aps, &naps);   // what the WiFi page shows
        n->n_aps = (uint8_t)naps;
        for (int k = 0; k < naps; k++) n->aps[k].kept = nets_find(&s_nets, n->aps[k].ssid) != NULL;

#if DISP_HELPER
        helper_set(s_ui.view == VIEW_MAIN && s_ui.cfg.look == LOOK_RADAR);
        const int step = s_helper ? 2 * DISP_STRIP : DISP_STRIP;
#elif !RADAR_OVERLAY
        const int step = DISP_STRIP;
#endif
        int64_t t0 = esp_timer_get_time();
#if RADAR_OVERLAY
        frame_layered(now);
#else
        ui_frame(&s_ui, now);
        for (int sy0 = 0; sy0 < GFX_PH; sy0 += step) {
            canvas_t cv = { work, sy0, sy0 + DISP_STRIP <= GFX_PH ? DISP_STRIP : GFX_PH - sy0, 0 };
#if DISP_HELPER
            const int y1 = sy0 + DISP_STRIP, two = s_helper && y1 < GFX_PH;
            if (two) {                                 // the strip under it, on the other core meanwhile
                s_help_cv.y0 = y1;
                s_help_cv.h = y1 + DISP_STRIP <= GFX_PH ? DISP_STRIP : GFX_PH - y1;
                xSemaphoreGive(s_help_go);
            }
            ui_strip(&s_ui, &cv);
            if (two) {
                uint32_t tw = now_us();
                xSemaphoreTake(s_help_done, portMAX_DELAY);
                s_help_wait_us += now_us() - tw;
            }
            uint32_t ts = now_us();
            if (two) disp_strip2(&cv, s_help_cv.h);    // both in one transfer
            else disp_strip(&cv);
#else
            ui_strip(&s_ui, &cv);
            uint32_t ts = now_us();
            disp_strip(&cv);                           // packs it to 565, waits for the strip before to be out
#endif
            s_send_us += now_us() - ts;
            touch_sample(0);
        }
        disp_frame_end();
#endif
        busy_us += (uint32_t)(esp_timer_get_time() - t0);
        frames++;
        if (light < 100) disp_backlight(light += 10);

        if (now - t_stat >= 10000u) {
            uint32_t f = frames ? frames : 1;
            ESP_LOGI(TAG, "%u fps, %u ms a frame, view %d, %d planes, heap %u", (unsigned)(frames * 1000u / (now - t_stat)),
                     (unsigned)(busy_us / 1000u / f), s_ui.view, s_ui.snap.n, (unsigned)heap_caps_get_free_size(MALLOC_CAP_8BIT));
#if DISP_LCD_RGB || DISP_LCD_DSI || DISP_WAVE7
            // the heap above counts the PSRAM too: what the WiFi and the second core want is this
            ESP_LOGI(TAG, "RAM %u free, %u in one piece, %u at the least", (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
                     (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL), (unsigned)heap_caps_get_minimum_free_size(MALLOC_CAP_INTERNAL));
#endif
            const uint32_t *pr = s_ui.prof;                // where a frame's time goes, ms: what to make faster
            ESP_LOGI(TAG, "ms a frame: background %u, disc %u, compass %u, planes %u, panel %u, the rest %u, "
                          "sending %u, touch %u",
                     (unsigned)(pr[0] / 1000u / f), (unsigned)(pr[1] / 1000u / f), (unsigned)(pr[2] / 1000u / f),
                     (unsigned)(pr[3] / 1000u / f), (unsigned)(pr[4] / 1000u / f), (unsigned)(pr[5] / 1000u / f),
                     (unsigned)(s_send_us / 1000u / f), (unsigned)(s_touch_us / 1000u / f));
#if DISP_HELPER
            if (s_helper)                              // the layers above are this core's strips only
                ESP_LOGI(TAG, "the second core draws: waited for it %u ms a frame, its stack %u bytes free",
                         (unsigned)(s_help_wait_us / 1000u / f), (unsigned)uxTaskGetStackHighWaterMark(s_helper));
            s_help_wait_us = 0;
#endif
#if RADAR_OVERLAY
            ESP_LOGI(TAG, "moving layer: %u ms a frame to make; background: %u frames, %u ms each",
                     (unsigned)(s_top_us / 1000u / f), (unsigned)s_bg_frames,
                     (unsigned)(s_bg_frames ? s_bg_us / 1000u / s_bg_frames : 0));
            {
                ovl_stats_t os;
                ovl_stats(&os, 1);
                ESP_LOGI(TAG, "moving layer: %u panel frames, %u made, %u never shown, %u too full, %u words at most",
                         (unsigned)os.frames, (unsigned)os.built, (unsigned)os.skipped, (unsigned)os.overflow,
                         (unsigned)os.words_max);
                ESP_LOGI(TAG, "moving layer: %u busiest rows a layer made whole beforehand", (unsigned)(os.built ? os.busy / os.built : 0));
                ESP_LOGI(TAG, "moving layer: %u chunks composed beforehand on this core, %u of them used by the fill",
                         (unsigned)os.pre_made, (unsigned)os.pre_used);
                ovl_log_chunks();
                ESP_LOGI(TAG, "rows lost by where the beam was, 12 3 6 9 o'clock and between: %u %u %u | %u %u %u | %u %u %u | %u %u %u",
                         (unsigned)os.late_by_beam[0], (unsigned)os.late_by_beam[1], (unsigned)os.late_by_beam[2],
                         (unsigned)os.late_by_beam[3], (unsigned)os.late_by_beam[4], (unsigned)os.late_by_beam[5],
                         (unsigned)os.late_by_beam[6], (unsigned)os.late_by_beam[7], (unsigned)os.late_by_beam[8],
                         (unsigned)os.late_by_beam[9], (unsigned)os.late_by_beam[10], (unsigned)os.late_by_beam[11]);
                ESP_LOGI(TAG, "moving layer in the fill: sweep %u us a frame, layer %u us a frame, %u words a frame, "
                              "%u rows with the sweep alone",
                         (unsigned)(os.frames ? os.beam_cyc / 240u / os.frames : 0), (unsigned)(os.frames ? os.layer_cyc / 240u / os.frames : 0),
                         (unsigned)(os.frames ? os.words / os.frames : 0), (unsigned)os.layer_dropped);
            }
            disp_log();
            s_top_us = s_bg_us = s_bg_frames = 0;
            // the 7B's backlight is said again in disp_log, with what the board reports round it
#endif
            memset(s_ui.prof, 0, sizeof s_ui.prof);
            if (now / 1000u - s_ran_kept >= 60u) {     // a reset from here on knows how long this run was
                s_ran_kept = now / 1000u;
                store_save_ran(s_ran_kept);
            }
            s_send_us = s_touch_us = 0;
            frames = 0;
            busy_us = 0;
            t_stat = now;
        }
        esp_task_wdt_reset();
#if RADAR_OVERLAY
        /* till the next panel frame starts (ovl.c). a frame begun while this pass ran is let go: the pass
         * always waits for a fresh one, so this core is always free for part of a frame. with many planes
         * the moving layer takes longer than a frame, and with the fetch task's TLS the core had no time
         * left for its idle task: the watchdog restarted the board (2026-11-11). the layer then changes
         * every second or third panel frame; the sweep keeps every one, the fill draws it from the clock */
        ulTaskNotifyTake(pdTRUE, 0);
        ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(50));
#else
        vTaskDelay(1);
#endif
    }
}

void app_main(void)
{
    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        err = nvs_flash_init();
    }
    ESP_ERROR_CHECK(err);

    s_reset = reset_name(esp_reset_reason());
    s_ran_s = store_load_ran();
    store_save_ran(0);
    ESP_LOGI(TAG, "started after: %s; the run before lasted %u s%s", s_reset, (unsigned)s_ran_s,
             s_ran_s ? "" : " (under a minute, or not known)");

    s_lock = xSemaphoreCreateMutex();
    if (!store_load_cfg(&s_cfg)) fr_cfg_defaults(&s_cfg);
    store_load_secrets(&s_sec);
    store_load_nets(&s_sec, &s_nets);
    char watch[WATCH_TEXT];
    store_load_watch(watch, sizeof watch);
    watch_set(&s_watch, watch);
    store_load_light(&s_light_cfg);
    store_load_opt(&s_opt);
    s_help = (uint8_t)store_load_help();
    s_bare = (uint8_t)store_load_bare();
    s_demo_on = !s_sec.ssid[0];                         // no WiFi set: the demo from the first frame
    sky_init(&s_sky, s_cfg.lat, s_cfg.lon);
    demo_init(&s_demo, range_km(&s_cfg), esp_random());
    tiles_init();
    map_mem = iram_first;
#if DISP_LCD_DSI
    disp_board_up();                                   // the Tab5: the radio's power switch, before the WiFi asks for it
#endif
#if RADAR_OVERLAY
    screen_early();
#endif
    net_start(&s_nets);                                // up even without settings: a SET then just connects
    /* the TLS handshake runs on the fetch task, and the map is made there. one core (the C3): it
     * gets the screen's priority, the two take turns a tick each. under the screen it had less
     * than a millisecond a frame, a map took it over ten seconds and IDLE starved into the watchdog */
    xTaskCreatePinnedToCore(fetch_task, "fetch", 12288, NULL, portNUM_PROCESSORS > 1 ? 4 : 5, &s_fetch, 0);
#if RADAR_OVERLAY
    /* the Waveshare 7/7B: the second core is the panel's (its fill follows every chunk's copy down
     * the rows and lays the moving layer over them); the drawing goes on the first, beside the WiFi */
    xTaskCreatePinnedToCore(ui_task, "ui", 8192, NULL, 5, NULL, 0);
#else
    xTaskCreatePinnedToCore(ui_task, "ui", 8192, NULL, 5, NULL, portNUM_PROCESSORS > 1 ? 1 : 0);
#endif
    /* the console last, and READY with it: the site sends its SET the moment it reads READY. said
     * before, the SET was lost (no console yet) or notified a fetch task not made yet (an assert) */
    console_start(on_line);
    say_ready();
}
