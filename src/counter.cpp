#include "counter.h"

#include <atomic>
#include <map>
#include <string>

#include "esp_wifi.h"
#include "esp_event.h"
#include "esp_coexist.h"
#include "nvs_flash.h"

#include <NimBLEDevice.h>
#include <NimBLEScan.h>
#include <NimBLEAdvertisedDevice.h>

#define CLR_RESET "\033[0m"
#define CLR_MAG "\033[35m"
#define CLR_RED "\033[31m"
#define CLR_GREEN "\033[32m"
#define CLR_BLUE "\033[34m"
#define CLR_YELLOW "\033[33m"

/* =========================================================================
   Mode and RSSI threshold
   ========================================================================= */
static CounterMode g_mode = MODE_PHONE_ESTIMATE;
static int g_rssi_threshold = RSSI_DEFAULT;

/* Returns true if this MAC should be counted in the current mode.
 * first_byte is the first byte of the MAC in standard (big-endian) notation.
 * Bit 1 of that byte is the IEEE "locally administered" flag, set by devices
 * that generate their own random address (phones, tablets). Hardware-assigned
 * OUI MACs (headphones, LoRa radios, APs, IoT) have this bit clear. */
static inline bool mac_passes_filter(uint8_t first_byte)
{
    if (g_mode == MODE_ALL_DEVICES)
        return true;
    return (first_byte & 0x02) != 0; /* locally administered = random MAC */
}

/* =========================================================================
   MAC table
   ========================================================================= */
static std::map<std::string, uint32_t> g_macs;
static SemaphoreHandle_t g_mac_mutex = nullptr;

/* Per-interval counters — written from WiFi/BLE tasks, read+reset from main task. */
static std::atomic<uint32_t> g_new_wifi{0};
static std::atomic<uint32_t> g_new_ble{0};
static std::atomic<uint32_t> g_evicted{0};
static std::atomic<uint32_t> g_filtered_wifi{0};
static std::atomic<uint32_t> g_filtered_ble{0};

/* Returns true if the key was not already in the table (genuinely new MAC). */
static bool mac_upsert(const std::string &key)
{
    bool is_new = false;
    if (xSemaphoreTake(g_mac_mutex, portMAX_DELAY) == pdTRUE)
    {
        is_new = (g_macs.count(key) == 0);
        g_macs[key] = millis();
        xSemaphoreGive(g_mac_mutex);
    }
    return is_new;
}

static void mac_clear()
{
    if (xSemaphoreTake(g_mac_mutex, portMAX_DELAY) == pdTRUE)
    {
        g_macs.clear();
        xSemaphoreGive(g_mac_mutex);
        Serial.printf(CLR_GREEN "%06lu %-9s cleared MAC table\n" CLR_RESET, millis() / 1000, "[counter]");
    }
}

static void evict_stale()
{
    uint32_t now = millis();
    uint32_t n = 0;
    if (xSemaphoreTake(g_mac_mutex, portMAX_DELAY) == pdTRUE)
    {
        for (auto it = g_macs.begin(); it != g_macs.end();)
        {
            if ((now - it->second) > DEDUP_WINDOW_MS)
            {
                Serial.printf(CLR_GREEN "%06lu %-9s evicted stale MAC: %s\n" CLR_RESET, millis() / 1000, "[counter]", it->first.c_str());
                it = g_macs.erase(it);
                n++;
            }
            else
            {
                ++it;
            }
        }
        xSemaphoreGive(g_mac_mutex);
    }
    g_evicted += n;
    if (g_evicted > 0)
    {
        Serial.printf(CLR_GREEN "%06lu %-9s evicted %u stale MAC(s)\n" CLR_RESET, millis() / 1000, "[counter]", g_evicted.load());
    }
}

/* =========================================================================
   WiFi — 802.11 promiscuous probe-request sniffer
   ========================================================================= */
#define FC_TYPE(fc0) (((fc0) >> 2) & 0x03)
#define FC_SUBTYPE(fc0) (((fc0) >> 4) & 0x0F)
#define MGMT_TYPE 0
#define PROBE_REQ_SUB 4
#define SRC_MAC_OFFSET 10

static void IRAM_ATTR wifi_sniffer_cb(void *buf,
                                      wifi_promiscuous_pkt_type_t type)
{
    if (type != WIFI_PKT_MGMT)
        return;
    const wifi_promiscuous_pkt_t *pkt =
        reinterpret_cast<const wifi_promiscuous_pkt_t *>(buf);
    const uint8_t *frame = pkt->payload;
    if (pkt->rx_ctrl.sig_len < 24)
        return;
    if (FC_TYPE(frame[0]) != MGMT_TYPE)
        return;
    if (FC_SUBTYPE(frame[0]) != PROBE_REQ_SUB)
        return;

    /* Drop frames below the RSSI threshold (too far away) */
    if (pkt->rx_ctrl.rssi < g_rssi_threshold)
        return;

    /* In 802.11 frames the source MAC is in big-endian order;
     * frame[SRC_MAC_OFFSET] is the first byte (the OUI/flag byte). */
    const uint8_t first_byte = frame[SRC_MAC_OFFSET];
    /* Bit 1 of first_byte is the IEEE "locally administered" flag.
     * Set → device randomised its own MAC (phones/tablets).
     * Clear → MAC was assigned by the manufacturer (hardware OUI). */
    const bool is_random = (first_byte & 0x02) != 0;

    if (!mac_passes_filter(first_byte))
    {
        g_filtered_wifi++;
        return;
    }

    char buf18[18];
    const uint8_t *m = frame + SRC_MAC_OFFSET;
    snprintf(buf18, sizeof(buf18), "%02x:%02x:%02x:%02x:%02x:%02x",
             m[0], m[1], m[2], m[3], m[4], m[5]);

    if (mac_upsert(buf18))
    {
        g_new_wifi++;
        Serial.printf(CLR_YELLOW "%06lu %-9s " CLR_RED "NEW " CLR_YELLOW "%s rssi=%4d %s\n" CLR_RESET,
                      millis() / 1000, "[wifi]", buf18, pkt->rx_ctrl.rssi,
                      is_random ? "random-MAC  -> phone/tablet"
                                : "OUI-MAC     -> hardware/AP");
    }
}

static void wifi_init()
{
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES ||
        ret == ESP_ERR_NVS_NEW_VERSION_FOUND)
    {
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
        .filter_mask = WIFI_PROMIS_FILTER_MASK_MGMT};
    ESP_ERROR_CHECK(esp_wifi_set_promiscuous_filter(&filt));
    ESP_ERROR_CHECK(esp_wifi_set_promiscuous_rx_cb(wifi_sniffer_cb));
    ESP_ERROR_CHECK(esp_wifi_set_promiscuous(true));
    Serial.printf(CLR_YELLOW "%06lu %-9s WiFi promiscuous sniffer running\n" CLR_RESET, millis() / 1000, "[wifi]", millis() / 1000);
}

/* =========================================================================
   BLE — passive scan
   ========================================================================= */
class HAXXScanCallbacks : public NimBLEScanCallbacks
{
    // void onDiscovered(const NimBLEAdvertisedDevice *dev) override
    // {
    //     if (dev->getRSSI() < g_rssi_threshold)
    //         return;

    //     /* BLE public address = hardware OUI (like a WiFi AP MAC).
    //      * BLE random address = device-generated for privacy (like phones). */
    //     const bool is_public = dev->getAddress().isPublic();

    //     if (g_mode == MODE_PHONE_ESTIMATE && is_public)
    //     {
    //         g_filtered_ble++;
    //         return;
    //     }

    //     if (mac_upsert(dev->getAddress().toString()))
    //     {
    //         g_new_ble++;
    //         const uint32_t now = millis();
    //         Serial.printf(CLR_BLUE "%06lu %-9s" CLR_RED " NEW " CLR_BLUE "%s rssi=%4d %s" CLR_RESET "\n",
    //                       now / 1000,
    //                       "[ble]",
    //                       dev->getAddress().toString().c_str(),
    //                       dev->getRSSI(),
    //                       is_public ? "public-addr -> hardware/IoT"
    //                                 : "random-addr -> phone/tablet");
    //     }
    // }

    void onResult(const NimBLEAdvertisedDevice *advertisedDevice)
    {
        // printf("made it to NimBLE callback\n");
        if (advertisedDevice->getRSSI() < g_rssi_threshold)
        {
            g_filtered_ble++;
            return;
        }

        NimBLEAddress address = advertisedDevice->getAddress();

        // if (address.isRpa() && g_mode == MODE_PHONE_ESTIMATE)
        if (address.isRpa())
        {
            // 📱 CLASSIFIED AS PRIVACY-ENABLED DEVICE (Phones, Tablets, Wearables)
            bool isActualPhone = false;
            bool isApple = false;
            bool isAndroid = false;

            if (advertisedDevice->haveManufacturerData())
            {
                // printf("has manufacturer data\n");
                std::string data = advertisedDevice->getManufacturerData();

                // Manufacturer data must contain at least 2 bytes for the Company ID
                if (data.length() >= 2)
                {
                    // Extract the 16-bit Company Identifier (Little Endian format)
                    uint16_t companyId = (uint8_t)data[1] << 8 | (uint8_t)data[0];

                    if (companyId == 0x004C)
                    {
                        // 🍏 Apple Device found (iPhone / iPad)
                        isActualPhone = true;
                        isApple = true;
                    }
                    else if (companyId == 0x00E0)
                    {
                        // 🤖 Google/Android Device found
                        isActualPhone = true;
                        isAndroid = true;
                    }
                }
            }

            // Additional Android check: Google frequently broadcasts via Service UUIDs instead
            if (!isActualPhone && advertisedDevice->haveServiceUUID())
            {
                NimBLEUUID serviceUUID = advertisedDevice->getServiceUUID();
                // 0xFE2C = Google Fast Pair Service, 0xFD5A = Find My Device
                if (serviceUUID.toString() == "0xfe2c" || serviceUUID.toString() == "0xfd5a")
                {
                    // 🤖 Android Phone/Tablet detected via service packet
                    isActualPhone = true;
                    isAndroid = true;
                }
            }

            // if (g_mode == MODE_PHONE_ESTIMATE && isActualPhone)
            if (isActualPhone)
            {
                if (mac_upsert(advertisedDevice->getAddress().toString()))
                {
                    g_new_ble++;
                    Serial.printf(CLR_BLUE "%06lu %-9s" CLR_RED " NEW " CLR_BLUE "%s rssi=%4d %s device" CLR_RESET "\n",
                                  millis() / 1000, "[ble]",
                                  advertisedDevice->getAddress().toString().c_str(),
                                  advertisedDevice->getRSSI(),
                                  isApple ? "Apple" : isAndroid ? "Android" : "unknown");
                }
            }
        }

        if (g_mode == MODE_ALL_DEVICES)
        {
            // ⚙️ EVERYTHING ELSE PATH
            // Public, Static Random, and NRPA addresses (Smart TVs, Fixed IoT, Beacons)
            if (mac_upsert(advertisedDevice->getAddress().toString()))
            {
                // printf("made it to all device mac upsert\n");
                g_new_ble++;
                const uint32_t now = millis();
                Serial.printf(CLR_BLUE "%06lu %-9s" CLR_RED " NEW " CLR_BLUE "%s rssi=%4d %s" CLR_RESET "\n",
                              now / 1000,
                              "[ble]",
                              advertisedDevice->getAddress().toString().c_str(),
                              advertisedDevice->getRSSI(),
                              "public-addr -> hardware/IoT");
            }
        }
    }
};

static HAXXScanCallbacks g_ble_callbacks;

static void ble_start()
{
    NimBLEScan *scan = NimBLEDevice::getScan();
    scan->setScanCallbacks(&g_ble_callbacks, false);
    scan->setActiveScan(true);
    scan->setInterval(100); /* 100 ms cycle */
    scan->setWindow(80);    /* 80 ms on = 80% duty, leaves air time for advertising */
    scan->start(0, false);
    Serial.printf(CLR_BLUE "%06lu %-9s ble sniffer started, interval: 100ms, window: 80ms\n" CLR_RESET, millis() / 1000, "[ble]");
}

static void ble_init()
{
    NimBLEDevice::init("");
    NimBLEDevice::setPower(ESP_PWR_LVL_N0);
    ble_start();
}

/* =========================================================================
   Burst scan task
   ========================================================================= */
static TaskHandle_t g_burst_task = nullptr;

/* Defined in the channel-hopper section; used by burst_scan_task above */
static uint8_t g_channel = 1;

static void burst_scan_task(void *)
{
    while (true)
    {
        ulTaskNotifyTake(pdTRUE, portMAX_DELAY);

        /* BLE: stop → clear → restart resets hardware duplicate filter */
        NimBLEScan *ble = NimBLEDevice::getScan();
        ble->stop();
        ble->clearResults();
        Serial.printf(CLR_BLUE "%06lu %-9s ble scan stopped & cleared\n" CLR_RESET, millis() / 1000, "[ble]");
        ble_start();

        /* WiFi probing only for ALL DEVICES */
        if (g_mode == MODE_ALL_DEVICES)
        {
            esp_wifi_set_promiscuous(false);
            Serial.printf(CLR_YELLOW "%06lu %-9s end WiFi promiscuous mode\n" CLR_RESET, millis() / 1000, "[wifi]");

            /* WiFi: active scan stimulates nearby devices to respond */
            esp_wifi_set_mode(WIFI_MODE_STA);
            wifi_scan_config_t scan_cfg = {};
            scan_cfg.scan_type = WIFI_SCAN_TYPE_ACTIVE;
            scan_cfg.scan_time.active.min = 100;
            scan_cfg.scan_time.active.max = 200;
            Serial.printf(CLR_YELLOW "%06lu %-9s starting WiFi AP scan mode\n" CLR_RESET, millis() / 1000, "[wifi]");

            if (esp_wifi_scan_start(&scan_cfg, true) == ESP_OK)
            {
                Serial.printf(CLR_YELLOW "%06lu %-9s WiFi AP scan complete\n" CLR_RESET, millis() / 1000, "[wifi]");
                uint16_t n = 0;
                esp_wifi_scan_get_ap_num(&n);
                Serial.printf(CLR_YELLOW "%06lu %-9s APs found: %u\n" CLR_RESET, millis() / 1000, "[wifi]", n);
                if (n > 0)
                {
                    auto *aps = static_cast<wifi_ap_record_t *>(
                        malloc(n * sizeof(wifi_ap_record_t)));
                    if (aps)
                    {
                        esp_wifi_scan_get_ap_records(&n, aps);
                        for (int i = 0; i < n; i++)
                        {
                            const uint8_t *m = aps[i].bssid;
                            if (!mac_passes_filter(m[0]))
                                continue;
                            char buf[18];
                            snprintf(buf, sizeof(buf),
                                     "%02x:%02x:%02x:%02x:%02x:%02x",
                                     m[0], m[1], m[2], m[3], m[4], m[5]);
                            if (mac_upsert(buf))
                                Serial.printf(CLR_YELLOW "%06lu %-9s" CLR_RED " NEW AP " CLR_YELLOW "%s\n" CLR_RESET, millis() / 1000, "[wifi]", buf);
                        }
                        free(aps);
                    }
                }
            }
            Serial.printf(CLR_YELLOW "%06lu %-9s end WiFi AP scan mode\n" CLR_RESET, millis() / 1000, "[wifi]");

            esp_wifi_set_promiscuous_rx_cb(wifi_sniffer_cb);
            esp_wifi_set_channel(g_channel, WIFI_SECOND_CHAN_NONE);
            esp_wifi_set_mode(WIFI_MODE_NULL);
            Serial.printf(CLR_YELLOW "%06lu %-9s starting WiFi promiscuous mode\n" CLR_RESET, millis() / 1000, "[wifi]");
            esp_wifi_set_promiscuous(true);
            Serial.printf(CLR_YELLOW "%06lu %-9s burst done — mode=%s MACs=%u\n" CLR_RESET,
                          millis() / 1000, "[wifi]", counter_mode_label(), counter_get());
        }
    }
}

/* =========================================================================
   Channel hopper
   ========================================================================= */
static uint32_t g_last_hop_ms = 0;
static uint32_t g_last_evict_ms = 0;
static uint32_t g_last_burst_ms = 0;

static void hop_channel()
{
    g_channel = (g_channel % 13) + 1;
    esp_wifi_set_channel(g_channel, WIFI_SECOND_CHAN_NONE);
}

/* =========================================================================
   Public API
   ========================================================================= */
CounterStats counter_pop_stats()
{
    CounterStats s = {};
    if (xSemaphoreTake(g_mac_mutex, portMAX_DELAY) == pdTRUE)
    {
        s.table_size = (uint32_t)g_macs.size();
        xSemaphoreGive(g_mac_mutex);
    }
    s.new_wifi = g_new_wifi.exchange(0);
    s.new_ble = g_new_ble.exchange(0);
    s.evicted = g_evicted.exchange(0);
    s.filtered_wifi = g_filtered_wifi.exchange(0);
    s.filtered_ble = g_filtered_ble.exchange(0);
    s.channel = g_channel;
    s.rssi = g_rssi_threshold;
    s.mode = g_mode;
    return s;
}

void counter_init()
{
    g_mac_mutex = xSemaphoreCreateMutex();
    configASSERT(g_mac_mutex);
    esp_coex_preference_set(ESP_COEX_PREFER_BT);
    wifi_init();
    ble_init();
    xTaskCreatePinnedToCore(burst_scan_task, "burst_scan", 6144,
                            nullptr, 1, &g_burst_task,
                            0); /* Pin explicitly to Core 0 */
    const uint32_t now = millis();
    Serial.printf(CLR_GREEN "%06lu %-9s burst scheduling started, pinned to Core: %d\n" CLR_RESET, now / 1000, "[counter]", xTaskGetCoreID(g_burst_task));
    Serial.printf(CLR_GREEN "%06lu %-9s init complete \n" CLR_RESET, now / 1000, "[counter]");
}

uint32_t counter_get()
{
    uint32_t n = 0;
    if (xSemaphoreTake(g_mac_mutex, portMAX_DELAY) == pdTRUE)
    {
        n = (uint32_t)g_macs.size();
        xSemaphoreGive(g_mac_mutex);
    }
    return n;
}

void counter_tick()
{
    uint32_t now = millis();

    if (now - g_last_hop_ms >= 500)
    {
        g_last_hop_ms = now;
        hop_channel();
    }
    if (now - g_last_evict_ms >= 10'000)
    {
        g_last_evict_ms = now;
        evict_stale();
    }
    if (now - g_last_burst_ms >= BURST_INTERVAL_MS)
    {
        g_last_burst_ms = now;
        xTaskNotifyGive(g_burst_task);
    }
}

void counter_toggle_mode()
{
    g_mode = (g_mode == MODE_ALL_DEVICES) ? MODE_PHONE_ESTIMATE
                                          : MODE_ALL_DEVICES;
    /* Clear stale data so the count immediately reflects the new filter */
    mac_clear();

    /* Reset burst timer and fire immediately so fresh results arrive
     * without waiting up to 60 s */
    g_last_burst_ms = millis();
    xTaskNotifyGive(g_burst_task);
    Serial.printf(CLR_GREEN "%06lu %-9s mode → %s\n" CLR_RESET, millis() / 1000, "[counter]", counter_mode_label());
}

CounterMode counter_get_mode() { return g_mode; }

const char *counter_mode_label()
{
    return (g_mode == MODE_PHONE_ESTIMATE) ? "people estimate"
                                           : "all devices";
}

void counter_set_rssi(int dbm)
{
    g_rssi_threshold = constrain(dbm, RSSI_MIN, RSSI_MAX);
    /* Clear the table so stale entries from the old threshold are removed */
    mac_clear();
    g_last_burst_ms = millis();
    xTaskNotifyGive(g_burst_task);
    Serial.printf(CLR_GREEN "%06lu %-9s RSSI threshold → %d dBm\n" CLR_RESET, millis() / 1000, "[counter]", g_rssi_threshold);
}

int counter_get_rssi() { return g_rssi_threshold; }
