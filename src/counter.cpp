#include "counter.h"

#include <map>
#include <string>

#include "esp_wifi.h"
#include "esp_event.h"
#include "esp_coexist.h"
#include "nvs_flash.h"

#include <NimBLEDevice.h>
#include <NimBLEScan.h>
#include <NimBLEAdvertisedDevice.h>

/* =========================================================================
   MAC table — maps MAC string → last-seen millis()
   Written from WiFi sniffer task and NimBLE host task; protected by mutex.
   ========================================================================= */
static std::map<std::string, uint32_t> g_macs;
static SemaphoreHandle_t g_mac_mutex = nullptr;

static void mac_upsert(const std::string &key) {
    if (xSemaphoreTake(g_mac_mutex, portMAX_DELAY) == pdTRUE) {
        g_macs[key] = millis();
        xSemaphoreGive(g_mac_mutex);
    }
}

static void evict_stale() {
    uint32_t now = millis();
    if (xSemaphoreTake(g_mac_mutex, portMAX_DELAY) == pdTRUE) {
        for (auto it = g_macs.begin(); it != g_macs.end(); ) {
            if ((now - it->second) > DEDUP_WINDOW_MS)
                it = g_macs.erase(it);
            else
                ++it;
        }
        xSemaphoreGive(g_mac_mutex);
    }
}

/* =========================================================================
   WiFi — 802.11 promiscuous probe-request sniffer
   ========================================================================= */
#define FC_TYPE(fc0)    (((fc0) >> 2) & 0x03)
#define FC_SUBTYPE(fc0) (((fc0) >> 4) & 0x0F)
#define MGMT_TYPE      0
#define PROBE_REQ_SUB  4
#define SRC_MAC_OFFSET 10   /* source address offset in any mgmt frame */

static void IRAM_ATTR wifi_sniffer_cb(void *buf,
                                       wifi_promiscuous_pkt_type_t type) {
    if (type != WIFI_PKT_MGMT) return;

    const wifi_promiscuous_pkt_t *pkt =
        reinterpret_cast<const wifi_promiscuous_pkt_t *>(buf);
    const uint8_t *frame = pkt->payload;

    if (pkt->rx_ctrl.sig_len < 24) return;
    if (FC_TYPE(frame[0]) != MGMT_TYPE) return;
    if (FC_SUBTYPE(frame[0]) != PROBE_REQ_SUB) return;

    const uint8_t *m = frame + SRC_MAC_OFFSET;
    char buf18[18];
    snprintf(buf18, sizeof(buf18), "%02x:%02x:%02x:%02x:%02x:%02x",
             m[0], m[1], m[2], m[3], m[4], m[5]);
    mac_upsert(buf18);
}

static void wifi_init() {
    /* NVS is required by esp_wifi internals; we never write to flash. */
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES ||
        ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        nvs_flash_erase();
        nvs_flash_init();
    }

    ESP_ERROR_CHECK(esp_event_loop_create_default());

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));
    ESP_ERROR_CHECK(esp_wifi_set_storage(WIFI_STORAGE_RAM));
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_NULL));
    ESP_ERROR_CHECK(esp_wifi_start());

    esp_wifi_set_channel(1, WIFI_SECOND_CHAN_NONE);

    wifi_promiscuous_filter_t filt = {
        .filter_mask = WIFI_PROMIS_FILTER_MASK_MGMT
    };
    ESP_ERROR_CHECK(esp_wifi_set_promiscuous_filter(&filt));
    ESP_ERROR_CHECK(esp_wifi_set_promiscuous_rx_cb(wifi_sniffer_cb));
    ESP_ERROR_CHECK(esp_wifi_set_promiscuous(true));

    Serial.println("[counter] WiFi sniffer running");
}

/* =========================================================================
   BLE — passive scan for advertisement packets
   ========================================================================= */
class HAXXScanCallbacks : public NimBLEScanCallbacks {
    /* Called once per unique advertising address (NimBLE 2.x API) */
    void onDiscovered(const NimBLEAdvertisedDevice *dev) override {
        mac_upsert(dev->getAddress().toString());
    }
};

static HAXXScanCallbacks g_ble_callbacks;

static void ble_init() {
    NimBLEDevice::init("");
    NimBLEDevice::setPower(ESP_PWR_LVL_N0);

    NimBLEScan *scan = NimBLEDevice::getScan();
    scan->setScanCallbacks(&g_ble_callbacks, false /*no duplicates*/);
    scan->setActiveScan(false);   /* passive — don't transmit scan requests */
    scan->setInterval(100);
    scan->setWindow(99);
    scan->start(0, false);        /* 0 = scan indefinitely, non-blocking */

    Serial.println("[counter] BLE scan running");
}

/* =========================================================================
   Channel hopper
   ========================================================================= */
static uint8_t  g_channel       = 1;
static uint32_t g_last_hop_ms   = 0;
static uint32_t g_last_evict_ms = 0;

static void hop_channel() {
    g_channel = (g_channel % 13) + 1;
    esp_wifi_set_channel(g_channel, WIFI_SECOND_CHAN_NONE);
}

/* =========================================================================
   Public API
   ========================================================================= */
void counter_init() {
    g_mac_mutex = xSemaphoreCreateMutex();
    configASSERT(g_mac_mutex);

    /* Ask the firmware to balance radio time between WiFi and BLE */
    esp_coex_preference_set(ESP_COEX_PREFER_BALANCE);

    wifi_init();
    ble_init();
}

uint32_t counter_get() {
    uint32_t n = 0;
    if (xSemaphoreTake(g_mac_mutex, portMAX_DELAY) == pdTRUE) {
        n = (uint32_t)g_macs.size();
        xSemaphoreGive(g_mac_mutex);
    }
    return n;
}

void counter_tick() {
    uint32_t now = millis();

    if (now - g_last_hop_ms >= 500) {
        g_last_hop_ms = now;
        hop_channel();
    }

    if (now - g_last_evict_ms >= 10'000) {
        g_last_evict_ms = now;
        evict_stale();
    }
}
