#include "counter.h"
#include "ble_classify.h"
#include "person_estimator.h"

#include <atomic>
#include <limits.h>
#include <map>
#include <string>
#include <stdlib.h>

#include "esp_wifi.h"
#include "esp_event.h"
#include "esp_coexist.h"
#include "nvs_flash.h"

#include <NimBLEDevice.h>
#include <NimBLEScan.h>
#include <NimBLEAdvertisedDevice.h>

#define CLR_RESET "\033[0m"
#define CLR_MAG "\033[35m"
#define CLR_BMAG "\033[95m"
#define CLR_RED "\033[31m"
#define CLR_GREEN "\033[32m"
#define CLR_BLUE "\033[34m"
#define CLR_BBLUE "\033[1;34m"
#define CLR_YELLOW "\033[33m"
#define CLR_CYAN "\033[36m"
#define CLR_HYEL "\033[0;93m"
#define CLR_C164 "\033[38;5;164m"
#define CLR_C190 "\033[38;5;190m"

/* =========================================================================
   Mode and RSSI threshold
   ========================================================================= */
ScanMode CURRENT_SCAN_MODE = ScanMode::ALL_DEVICES;
static int g_rssi_threshold = RSSI_DEFAULT;

/* =========================================================================
   MAC table
   ========================================================================= */
static SemaphoreHandle_t g_mac_mutex = nullptr;

enum class MacSource : uint8_t
{
    WIFI,
    BLE
};

struct MacEntry
{
    uint32_t last_seen;
    MacSource source;
    uint8_t device_class;
    uint8_t confidence;
    uint32_t fingerprint;
    uint8_t fp_quality;
    int8_t last_rssi;
    uint32_t device_id; /* survives address-rotation merges; keys the person estimator */
};

static std::map<uint64_t, MacEntry> g_macs;
static uint32_t g_next_device_id = 1;

/* Per-interval counters — written from WiFi/BLE tasks, read+reset from main task. */
static std::atomic<uint32_t> g_new_wifi{0};
static std::atomic<uint32_t> g_new_ble{0};
static std::atomic<uint32_t> g_evicted{0};
static std::atomic<uint32_t> g_filtered_wifi{0};
static std::atomic<uint32_t> g_filtered_ble{0};

static uint64_t pack_address(const uint8_t address[6])
{
    uint64_t packed = 0;
    for (uint8_t i = 0; i < 6; ++i)
        packed = (packed << 8) | address[i];
    return packed;
}

static uint64_t pack_ble_address(const uint8_t address_le[6])
{
    uint64_t packed = 0;
    for (int i = 5; i >= 0; --i)
        packed = (packed << 8) | address_le[i];
    return packed;
}

struct Bucket
{
    const char *label;
    uint32_t class_mask;
    uint8_t min_confidence;
};

static constexpr uint32_t CLASS_BIT(uint8_t device_class)
{
    return 1u << device_class;
}

static constexpr uint32_t ALL_CLASSES = (1u << (BLE_CLASS_UNKNOWN + 1)) - 1u;
static const Bucket BUCKETS[] = {
    {"all devices", ALL_CLASSES, 0},
    {"personal", CLASS_BIT(BLE_CLASS_PHONE) | CLASS_BIT(BLE_CLASS_WATCH) |
                     CLASS_BIT(BLE_CLASS_EARBUDS) | CLASS_BIT(BLE_CLASS_TRACKER) |
                     CLASS_BIT(BLE_CLASS_LAPTOP) | CLASS_BIT(BLE_CLASS_PERSONAL),
     50},
    {"phones", CLASS_BIT(BLE_CLASS_PHONE), 60},
    {"est. people", 0, 0}};
static_assert(sizeof(BUCKETS) / sizeof(BUCKETS[0]) == static_cast<size_t>(ScanMode::BUCKET_COUNT),
              "one bucket per scan mode");

static uint64_t parse_mac(const std::string &mac)
{
    uint8_t bytes[6] = {};
    const char *cursor = mac.c_str();
    for (uint8_t i = 0; i < 6; ++i)
    {
        char *end = nullptr;
        const unsigned long value = strtoul(cursor, &end, 16);
        if (end == cursor || value > 0xFF || (i < 5 && *end != ':'))
            return 0;
        bytes[i] = (uint8_t)value;
        cursor = i < 5 ? end + 1 : end;
    }
    return pack_address(bytes);
}

static void format_mac(uint64_t mac, char out[18])
{
    snprintf(out, 18, "%02llx:%02llx:%02llx:%02llx:%02llx:%02llx",
             (unsigned long long)((mac >> 40) & 0xFF),
             (unsigned long long)((mac >> 32) & 0xFF),
             (unsigned long long)((mac >> 24) & 0xFF),
             (unsigned long long)((mac >> 16) & 0xFF),
             (unsigned long long)((mac >> 8) & 0xFF),
             (unsigned long long)(mac & 0xFF));
}

static void refresh_entry(MacEntry &entry, const ble_info_t &info, uint32_t now)
{
    entry.last_seen = now;
    entry.last_rssi = info.rssi;
    const bool ambiguous_handoff_downgrade =
        entry.device_class == BLE_CLASS_PHONE &&
        info.device_class == BLE_CLASS_PERSONAL &&
        (info.evidence & BLE_EVIDENCE_APPLE_HANDOFF) != 0 &&
        (info.evidence & (BLE_EVIDENCE_APPLE_ACTION | BLE_EVIDENCE_APPEARANCE)) == 0;
    if (info.confidence > entry.confidence || ambiguous_handoff_downgrade)
    {
        entry.device_class = info.device_class;
        entry.confidence = info.confidence;
    }
}

static bool mac_upsert(const ble_info_t &info, bool passing_rssi = true)
{
    const uint64_t mac = pack_address(info.addr);
    const uint32_t now = millis();
    bool is_new = false;
    const MacEntry *observed = nullptr;

    if (xSemaphoreTake(g_mac_mutex, portMAX_DELAY) == pdTRUE)
    {
        auto current = g_macs.find(mac);
        if (current != g_macs.end())
        {
            refresh_entry(current->second, info, now);
            if (info.fp_quality != BLE_FP_NONE && info.fp_quality >= current->second.fp_quality)
            {
                current->second.fingerprint = info.fingerprint;
                current->second.fp_quality = info.fp_quality;
            }
            observed = &current->second;
        }
        else if (passing_rssi)
        {
            auto match = g_macs.end();
            if (info.fp_quality == BLE_FP_STRONG)
            {
                for (auto it = g_macs.begin(); it != g_macs.end(); ++it)
                {
                    if (it->second.source == MacSource::BLE &&
                        it->second.fp_quality == BLE_FP_STRONG &&
                        it->second.fingerprint == info.fingerprint)
                    {
                        match = it;
                        break;
                    }
                }
            }
            else if (info.fp_quality == BLE_FP_WEAK)
            {
                int best_rssi_delta = 7;
                for (auto it = g_macs.begin(); it != g_macs.end(); ++it)
                {
                    const uint32_t quiet_ms = now - it->second.last_seen;
                    const int rssi_delta = abs((int)info.rssi - it->second.last_rssi);
                    if (it->second.source == MacSource::BLE &&
                        it->second.fp_quality == BLE_FP_WEAK &&
                        it->second.fingerprint == info.fingerprint &&
                        quiet_ms >= 10'000 && quiet_ms <= 15'000 &&
                        quiet_ms <= DEDUP_WINDOW_MS &&
                        rssi_delta <= 6 && rssi_delta < best_rssi_delta)
                    {
                        match = it;
                        best_rssi_delta = rssi_delta;
                    }
                }
            }

            if (match != g_macs.end())
            {
                MacEntry merged = match->second;
                g_macs.erase(match);
                refresh_entry(merged, info, now);
                if (info.fp_quality > merged.fp_quality)
                {
                    merged.fingerprint = info.fingerprint;
                    merged.fp_quality = info.fp_quality;
                }
                observed = &g_macs.emplace(mac, merged).first->second;
            }
            else
            {
                MacEntry entry = {};
                entry.last_seen = now;
                entry.source = MacSource::BLE;
                entry.device_class = info.device_class;
                entry.confidence = info.confidence;
                entry.fingerprint = info.fingerprint;
                entry.fp_quality = info.fp_quality;
                entry.device_id = g_next_device_id++;
                refresh_entry(entry, info, now);
                observed = &g_macs.emplace(mac, entry).first->second;
                is_new = true;
            }
        }
        if (observed)
            pe_observe(observed->device_id, now, info.rssi, observed->device_class,
                       observed->confidence, info.vendor);
        xSemaphoreGive(g_mac_mutex);
    }

    return is_new;
}

static bool mac_upsert_wifi(const std::string &mac_text)
{
    const uint64_t mac = parse_mac(mac_text);
    if (mac == 0)
        return false;

    bool is_new = false;
    const uint32_t now = millis();
    if (xSemaphoreTake(g_mac_mutex, portMAX_DELAY) == pdTRUE)
    {
        auto it = g_macs.find(mac);
        if (it == g_macs.end())
        {
            MacEntry entry = {};
            entry.last_seen = now;
            entry.source = MacSource::WIFI;
            entry.device_class = BLE_CLASS_UNKNOWN;
            entry.last_rssi = INT8_MIN;
            g_macs.emplace(mac, entry);
            is_new = true;
        }
        else
        {
            it->second.last_seen = now;
        }
        xSemaphoreGive(g_mac_mutex);
    }
    return is_new;
}
static void mac_clear()
{
    if (xSemaphoreTake(g_mac_mutex, portMAX_DELAY) == pdTRUE)
    {
        g_macs.clear();
        pe_reset();
        xSemaphoreGive(g_mac_mutex);
        Serial.printf(CLR_GREEN "%06lu %-11s C1  cleared MAC table\n" CLR_RESET, millis() / 10, "[counter]");
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
            if ((now - it->second.last_seen) > DEDUP_WINDOW_MS)
            {
                char mac_text[18];
                format_mac(it->first, mac_text);
                Serial.printf(CLR_GREEN "%06lu %-11s C2  stale MAC " CLR_C164 "%s\n" CLR_RESET,
                              millis() / 10, "[counter]", mac_text);
                if (it->second.source == MacSource::BLE)
                    pe_forget(it->second.device_id);
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
    // if (g_evicted > 0 && n > 0)
    // {
    //     Serial.printf(CLR_GREEN "%06lu %-11s C3  evicted %u stale MAC(s)\n" CLR_RESET,
    //                   millis() / 10, "[counter]", g_evicted.load());
    // }
}

/* =========================================================================
   WiFi — 802.11 promiscuous probe-request sniffer
   ========================================================================= */
#define FC_TYPE(fc0) (((fc0) >> 2) & 0x03)
#define FC_SUBTYPE(fc0) (((fc0) >> 4) & 0x0F)
#define MGMT_TYPE 0
#define PROBE_REQ_SUB 4
#define SRC_MAC_OFFSET 10

static void IRAM_ATTR wifi_sniffer_cb(void *buf, wifi_promiscuous_pkt_type_t type)
{
    // We only care about Management frames (like Probe Requests)
    if (type != WIFI_PKT_MGMT)
        return;

    // get the WiFi packet payload
    wifi_promiscuous_pkt_t *pkt = (wifi_promiscuous_pkt_t *)buf;
    uint8_t *payload = pkt->payload;

    // Extract the total length from the packet control header
    uint16_t packet_length = pkt->rx_ctrl.sig_len;

    // Byte 0 contains Frame Type and Subtype. 0x40 is a Probe Request.
    uint8_t frame_control = payload[0];
    bool is_probe_request = (frame_control == 0x40 || frame_control == 0x00);

    // Source MAC address is located at bytes 10 to 15 in the 802.11 header
    uint8_t *src_mac = &payload[10];

    char buf18[18];
    snprintf(buf18, sizeof(buf18), "%02x:%02x:%02x:%02x:%02x:%02x",
             src_mac[0], src_mac[1], src_mac[2], src_mac[3], src_mac[4], src_mac[5]);

    /* Drop frames below the RSSI threshold (too far away) */
    if (pkt->rx_ctrl.rssi < g_rssi_threshold)
    {
        g_filtered_wifi++;
        if (SHOW_DETAILS)
            Serial.printf(CLR_YELLOW "%06lu %-11s W14 " CLR_CYAN "WiFi RSSI too low " CLR_C164 "%s " CLR_YELLOW "rssi=%4d\n" CLR_RESET,
                          millis() / 10, "[wifi]", buf18, pkt->rx_ctrl.rssi);
        return;
    }

    if (mac_upsert_wifi(buf18))
    {
        // bump the counters
        g_new_wifi++;
        Serial.printf(CLR_YELLOW "%06lu %-11s W1  " CLR_RED "NEW WiFi device " CLR_C164 "%s " CLR_YELLOW "rssi=%4d\n" CLR_RESET,
                      millis() / 10, "[wifi]", buf18, pkt->rx_ctrl.rssi);
    }
    else
    {
        // duplicate sighting, already in the mac table
        g_filtered_wifi++;
        if (SHOW_DETAILS)
            Serial.printf(CLR_YELLOW "%06lu %-11s W12 " CLR_CYAN "DUP WiFi device " CLR_C164 "%s " CLR_YELLOW "rssi=%4d\n" CLR_RESET, millis() / 10, "[wifi]", buf18, pkt->rx_ctrl.rssi);
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
    Serial.printf(CLR_YELLOW "%06lu %-11s W2  WiFi promiscuous sniffer running\n" CLR_RESET, millis() / 10, "[wifi]", millis() / 10);
}

/* =========================================================================
   BLE — passive scan
   ========================================================================= */
static const char *ble_class_name(uint8_t device_class)
{
    static const char *const names[] = {
        "phone", "watch", "earbuds", "tracker", "laptop", "tv_media", "beacon", "personal", "unknown"};
    return device_class <= BLE_CLASS_UNKNOWN ? names[device_class] : "unknown";
}

class HAXXScanCallbacks : public NimBLEScanCallbacks
{
    void onResult(const NimBLEAdvertisedDevice *advertisedDevice)
    {
        NimBLEAddress address = advertisedDevice->getAddress();
        const std::vector<uint8_t> &payload = advertisedDevice->getPayload();
        ble_info_t info;
        if (!ble_parse(payload.data(), payload.size(),
                       address.getVal(), advertisedDevice->getAddressType(),
                       advertisedDevice->getRSSI(), &info))
            return;

        char mac_text[18];
        format_mac(pack_address(info.addr), mac_text);

        // Weak sightings refresh a known address but cannot add or merge devices.
        if (advertisedDevice->getRSSI() < g_rssi_threshold)
        {
            mac_upsert(info, false);
            g_filtered_ble++;
            if (SHOW_BLE_DUPS)
            {
                Serial.printf(CLR_BLUE "%06lu %-11s B17 " CLR_CYAN "BLE RSSI too low " CLR_C164 "%s " CLR_C190 "class=%s confidence=%u evidence=0x%08lx rssi:%4d\n" CLR_RESET,
                              millis() / 10, "[ble]",
                              mac_text, ble_class_name(info.device_class),
                              info.confidence, (unsigned long)info.evidence, info.rssi);
            }
            return;
        }

        if (mac_upsert(info))
        {
            g_new_ble++;
            /* Raw Apple manufacturer bytes, to learn which Nearby fields separate Macs from iPhones. */
            char mfg_hex[2 * 31 + 1] = "";
            for (uint8_t m = 0; m < advertisedDevice->getManufacturerDataCount(); ++m)
            {
                const std::string mfg = advertisedDevice->getManufacturerData(m);
                if (mfg.size() < 2 || (uint8_t)mfg[0] != 0x4C || mfg[1] != 0x00)
                    continue;
                for (size_t b = 0; b < mfg.size() && b < 31; ++b)
                    snprintf(mfg_hex + 2 * b, 3, "%02x", (uint8_t)mfg[b]);
                break;
            }
            Serial.printf(CLR_BLUE "%06lu %-11s B1  " CLR_RED "NEW BLE device " CLR_C164 "%s " CLR_C190 "class=%s confidence=%u evidence=0x%08lx rssi:%4d apple=%s\n" CLR_RESET,
                              millis() / 10, "[ble]",
                              mac_text, ble_class_name(info.device_class),
                              info.confidence, (unsigned long)info.evidence, info.rssi,
                              mfg_hex[0] ? mfg_hex : "-");
        }
        else
        {
            g_filtered_ble++;
            if (SHOW_BLE_DUPS)
            {
                Serial.printf(CLR_BLUE "%06lu %-11s B13 " CLR_CYAN "DUP BLE device " CLR_C164 "%s " CLR_C190 "class=%s confidence=%u evidence=0x%08lx rssi:%4d\n" CLR_RESET,
                              millis() / 10, "[ble]",
                              mac_text, ble_class_name(info.device_class),
                              info.confidence, (unsigned long)info.evidence, info.rssi);
            }
        }
    }
};

static HAXXScanCallbacks g_ble_callbacks;
static void ble_start()
{
    NimBLEScan *scan = NimBLEDevice::getScan();
    /* Duplicates are needed for continuous RSSI trajectories, not just discovery. */
    scan->setScanCallbacks(&g_ble_callbacks, true);
    scan->setActiveScan(true);
    scan->setInterval(100); /* 100 ms cycle */
    scan->setWindow(90);    /* 90 ms on = 90% duty, leaves air time for advertising */
    scan->start(0, false);
    Serial.printf(CLR_BLUE "%06lu %-11s B3  BLE sniffer started, interval: 100ms, window: 90ms\n" CLR_RESET, millis() / 10, "[ble]");
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

// burst_scan_task started in setup()
static void burst_scan_task(void *)
{
    while (true)
    {
        ulTaskNotifyTake(pdTRUE, portMAX_DELAY);

        /* BLE: stop → clear → restart resets hardware duplicate filter */
        NimBLEScan *ble = NimBLEDevice::getScan();
        ble->stop();
        ble->clearResults();
        Serial.printf(CLR_BLUE "%06lu %-11s B4  reset BLE hardware duplicate filter\n" CLR_RESET, millis() / 10, "[ble]");
        ble_start();

        /* WiFi probing only for ALL DEVICES */

        esp_wifi_set_promiscuous(false);
        Serial.printf(CLR_YELLOW "%06lu %-11s W3  end WiFi promiscuous mode\n" CLR_RESET, millis() / 10, "[wifi]");

        /* WiFi: active scan stimulates nearby devices to respond */
        esp_wifi_set_mode(WIFI_MODE_STA);
        wifi_scan_config_t scan_cfg = {};
        scan_cfg.scan_type = WIFI_SCAN_TYPE_ACTIVE;
        scan_cfg.scan_time.active.min = 100;
        scan_cfg.scan_time.active.max = 200;
        Serial.printf(CLR_YELLOW "%06lu %-11s W4  starting WiFi burst\n" CLR_RESET, millis() / 10, "[wifi]");

        if (esp_wifi_scan_start(&scan_cfg, true) == ESP_OK)
        {
            uint16_t n = 0;
            esp_wifi_scan_get_ap_num(&n);
            Serial.printf(CLR_YELLOW "%06lu %-11s W5  WiFi burst complete. %u MAC(s) to analyze\n" CLR_RESET, millis() / 10, "[wifi]", n);
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
                        char buf[18];
                        snprintf(buf, sizeof(buf),
                                 "%02x:%02x:%02x:%02x:%02x:%02x",
                                 m[0], m[1], m[2], m[3], m[4], m[5]);
                        if (mac_upsert_wifi(buf))
                        {
                            Serial.printf(CLR_YELLOW "%06lu %-11s W7  " CLR_RED "NEW WiFi device " CLR_C164 "%s\n" CLR_RESET, millis() / 10, "[wifi]", buf);
                        }
                        else
                        {
                            g_filtered_wifi++;
                            if (SHOW_DETAILS)
                                Serial.printf(CLR_YELLOW "%06lu %-11s W13 " CLR_CYAN "DUP WiFi burst device " CLR_C164 "%s\n" CLR_RESET, millis() / 10, "[wifi]", buf);
                        }
                    }
                    free(aps);
                }
            }
        }
        // Serial.printf(CLR_YELLOW "%06lu %-11s W8 end WiFi AP scan mode\n" CLR_RESET, millis() / 10, "[wifi]");

        esp_wifi_set_promiscuous_rx_cb(wifi_sniffer_cb);
        esp_wifi_set_channel(g_channel, WIFI_SECOND_CHAN_NONE);
        esp_wifi_set_mode(WIFI_MODE_NULL);
        Serial.printf(CLR_YELLOW "%06lu %-11s W9  starting WiFi promiscuous mode\n" CLR_RESET, millis() / 10, "[wifi]");
        esp_wifi_set_promiscuous(true);
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
    if (xSemaphoreTake(g_mac_mutex, portMAX_DELAY) == pdTRUE)
    {
        for (const auto &entry : g_macs)
        {
            if (entry.second.source == MacSource::WIFI)
                s.live_wifi++;
            else
                s.live_ble++;
            if (entry.second.device_class == BLE_CLASS_PHONE)
                s.live_phones++;
        }
        xSemaphoreGive(g_mac_mutex);
    }
    s.filtered_wifi = g_filtered_wifi.exchange(0);
    s.filtered_ble = g_filtered_ble.exchange(0);

    s.evicted = g_evicted.exchange(0);

    s.channel = g_channel;
    s.rssi = g_rssi_threshold;
    return s;
}

static void people_log(const char *line)
{
    Serial.printf(CLR_BMAG "%06lu %-11s P2  %s\n" CLR_RESET, millis() / 10, "[people]", line);
}

void counter_init()
{
    g_mac_mutex = xSemaphoreCreateMutex();
    configASSERT(g_mac_mutex);
    pe_set_logger(people_log);
    esp_coex_preference_set(ESP_COEX_PREFER_BT);
    wifi_init(); //  set the callback function for, and start the WiFi promiscuous sniffer
    ble_init();  //  set the callback function for, and start the BLE scanner in Active mode
    Serial.printf(CLR_GREEN "%06lu %-11s C4  starting burst scheduling, pinned to Core: %d\n" CLR_RESET, millis() / 10, "[counter]", xTaskGetCoreID(g_burst_task));

    xTaskCreatePinnedToCore(burst_scan_task, "burst_scan", 6144,
                            nullptr, 1, &g_burst_task,
                            0); /* Pin explicitly to Core 0 */
    Serial.printf(CLR_GREEN "%06lu %-11s C5  init complete \n" CLR_RESET, millis() / 10, "[counter]");
}

uint32_t counter_get(uint8_t bucket_id)
{
    if (bucket_id >= sizeof(BUCKETS) / sizeof(BUCKETS[0]))
        return 0;

    const Bucket &bucket = BUCKETS[bucket_id];
    uint32_t n = 0;
    if (bucket_id == static_cast<uint8_t>(ScanMode::PEOPLE_ESTIMATE))
        return counter_get_people().people;
    if (xSemaphoreTake(g_mac_mutex, portMAX_DELAY) == pdTRUE)
    {
        for (const auto &entry : g_macs)
        {
            const MacEntry &device = entry.second;
            if (device.source == MacSource::WIFI)
            {
                if (bucket_id == static_cast<uint8_t>(ScanMode::ALL_DEVICES))
                    n++;
                continue;
            }
            const bool class_matches = (bucket.class_mask & CLASS_BIT(device.device_class)) != 0;
            if (class_matches && device.confidence >= bucket.min_confidence)
                n++;
        }
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
    // evict every 5 seconds
    if (now - g_last_evict_ms >= 5'000)
    {
        g_last_evict_ms = now;
        evict_stale();
    }
    if (xSemaphoreTake(g_mac_mutex, portMAX_DELAY) == pdTRUE)
    {
        static PersonEstimate last = {};
        if (pe_tick(now))
        {
            const PersonEstimate est = pe_get();
            if (est.people != last.people || est.devices != last.devices ||
                est.associated_pairs != last.associated_pairs ||
                est.unresolved_pairs != last.unresolved_pairs)
            {
                Serial.printf(CLR_BMAG "%06lu %-11s P1  devices=%lu people=%lu associated=%lu unresolved=%lu\n" CLR_RESET,
                              now / 10, "[people]", (unsigned long)est.devices,
                              (unsigned long)est.people, (unsigned long)est.associated_pairs,
                              (unsigned long)est.unresolved_pairs);
                last = est;
            }
        }
        xSemaphoreGive(g_mac_mutex);
    }
    if (now - g_last_burst_ms >= BURST_INTERVAL_MS)
    {
        g_last_burst_ms = now;
        xTaskNotifyGive(g_burst_task);
    }
}

const char *counter_mode_label()
{
    static char buf[64];
    uint8_t bucket_id = static_cast<uint8_t>(CURRENT_SCAN_MODE);
    if (bucket_id >= sizeof(BUCKETS) / sizeof(BUCKETS[0]))
        bucket_id = static_cast<uint8_t>(ScanMode::ALL_DEVICES);
    snprintf(buf, sizeof(buf), "%s > %ddBm", BUCKETS[bucket_id].label, counter_get_rssi());
    return buf;
}
void counter_set_rssi(int dbm)
{
    g_rssi_threshold = constrain(dbm, RSSI_MIN, RSSI_MAX);
    /* Clear the table so stale entries from the old threshold are removed */
    mac_clear();
    g_last_burst_ms = millis();
    xTaskNotifyGive(g_burst_task);
    Serial.printf(CLR_GREEN "%06lu %-11s C7  RSSI threshold set to → %d dBm\n" CLR_RESET, millis() / 10, "[counter]", g_rssi_threshold);
}

int counter_get_rssi() { return g_rssi_threshold; }

PersonEstimate counter_get_people()
{
    PersonEstimate est = {};
    if (xSemaphoreTake(g_mac_mutex, portMAX_DELAY) == pdTRUE)
    {
        est = pe_get();
        xSemaphoreGive(g_mac_mutex);
    }
    return est;
}

uint32_t counter_dump_table()
{
    const uint32_t stamp = millis() / 10;
    if (xSemaphoreTake(g_mac_mutex, portMAX_DELAY) == pdTRUE)
    {
        const uint32_t now = millis();
        const PersonEstimate est = pe_get();
        Serial.printf(CLR_GREEN "%06lu %-11s C8  MAC table dump #%06lu — %u %s, people=%lu\n" CLR_RESET,
                      (unsigned long)stamp, "[counter]", (unsigned long)stamp,
                      (unsigned)g_macs.size(), g_macs.size() == 1 ? "entry" : "entries",
                      (unsigned long)est.people);
        for (const auto &entry : g_macs)
        {
            const char *source_str = (entry.second.source == MacSource::WIFI) ? "wifi" : "ble";
            const char *class_name = entry.second.source == MacSource::BLE
                                         ? ble_class_name(entry.second.device_class)
                                         : "wifi";
            char mac_text[18];
            format_mac(entry.first, mac_text);
            const uint32_t person = entry.second.source == MacSource::BLE
                                        ? pe_person_id(entry.second.device_id)
                                        : 0;
            Serial.printf(CLR_GREEN "%06lu %-11s C9  " CLR_C164 "%s %s " CLR_C190 "%s " CLR_BLUE "confidence=%u rssi=%d fp=%u fpq=%u age=%lus dev=%lu person=%lu\n" CLR_RESET,
                          (unsigned long)stamp, "[counter]",
                          mac_text, source_str, class_name,
                          entry.second.confidence,
                          entry.second.last_rssi,
                          entry.second.fingerprint,
                          entry.second.fp_quality,
                          (unsigned long)((now - entry.second.last_seen) / 1000),
                          (unsigned long)entry.second.device_id,
                          (unsigned long)person);
        }
        xSemaphoreGive(g_mac_mutex);
    }
    return stamp;
}
