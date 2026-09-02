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

/* Returns true if this MAC should be counted in the current mode.
 * first_byte is the first byte of the MAC in standard (big-endian) notation.
 * Bit 1 of that byte is the IEEE "locally administered" flag, set by devices
 * that generate their own random address (phones, tablets). Hardware-assigned
 * OUI MACs (headphones, LoRa radios, APs, IoT) have this bit clear. */
static inline bool mac_passes_filter(uint8_t first_byte)
{
    return true;
}

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
    bool is_phone;
    std::string name; /* WiFi: probed/AP SSID — the only "name" WiFi gives us */
};

static std::map<std::string, MacEntry> g_macs;

/* Per-interval counters — written from WiFi/BLE tasks, read+reset from main task. */
static std::atomic<uint32_t> g_new_wifi{0};
static std::atomic<uint32_t> g_new_ble{0};
static std::atomic<uint32_t> g_evicted{0};
static std::atomic<uint32_t> g_filtered_wifi{0};
static std::atomic<uint32_t> g_filtered_ble{0};

static std::string manufacturer_data_to_key(const std::string &manufacturer_data)
{
    if (manufacturer_data.empty())
        return {};

    std::string key;
    key.reserve(manufacturer_data.size() * 2);

    static const char hex[] = "0123456789abcdef";
    for (unsigned char byte : manufacturer_data)
    {
        key.push_back(hex[(byte >> 4) & 0x0F]);
        key.push_back(hex[byte & 0x0F]);
    }

    return key;
}

static std::string mac_table_key(const std::string &mac, const std::string &manufacturer_data)
{
    const std::string manufacturer_key = manufacturer_data_to_key(manufacturer_data);
    return manufacturer_key.empty() ? mac : manufacturer_key;
}

/* Returns true if the key was not already in the table (genuinely new device).
 * When BLE manufacturer data is present it is used as the dedupe identity so
 * rotating random MACs from the same handset are not double-counted.
 * passing_rssi=false keeps an already-known device alive but never counts it. */
static bool mac_upsert(const std::string &mac, const std::string &manufacturer_data = "", bool passing_rssi = true,
                       MacSource source = MacSource::BLE, bool is_phone = false, const std::string &name = "")
{
    std::string identity = manufacturer_data;

    /* Apple devices: use the FULL payload as identity. Truncating to 3 bytes
     * would merge every iPhone broadcasting the same message type. AirPods
     * (0x07) keep 6 bytes so the dynamic battery byte doesn't fork identity. */
    if (manufacturer_data.size() >= 4 &&
        (unsigned char)manufacturer_data[0] == 0x4C &&
        (unsigned char)manufacturer_data[1] == 0x00 &&
        (unsigned char)manufacturer_data[2] == 0x07 &&
        manufacturer_data.size() >= 6)
    {
        identity = manufacturer_data.substr(0, 6);
    }

    /* Hex-encode so keys are printable and binary NULs can't truncate logs */
    const std::string key = mac + "_" + manufacturer_data_to_key(identity);
    bool is_new = false;

    if (xSemaphoreTake(g_mac_mutex, portMAX_DELAY) == pdTRUE)
    {
        /* A matching mfg identity means the same device even when the BLE
         * random MAC has rotated — refresh that entry and report duplicate. */
        if (!identity.empty())
        {
            const std::string suffix = "_" + manufacturer_data_to_key(identity);
            for (auto &entry : g_macs)
            {
                const std::string &k = entry.first;
                if (k.size() >= suffix.size() &&
                    k.compare(k.size() - suffix.size(), suffix.size(), suffix) == 0)
                {
                    entry.second.last_seen = millis();
                    if (entry.second.name.empty() && !name.empty())
                        entry.second.name = name;
                    xSemaphoreGive(g_mac_mutex);
                    return false;
                }
            }
        }

        auto it = g_macs.find(key);
        if (it == g_macs.end() && passing_rssi)
        {
            g_macs[key] = {millis(), source, is_phone, name};
            is_new = true;
        }
        else if (it != g_macs.end())
        {
            it->second.last_seen = millis(); /* hysteresis: weak sighting still keeps device alive */
            if (it->second.name.empty() && !name.empty())
                it->second.name = name;
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
        xSemaphoreGive(g_mac_mutex);
        Serial.printf(CLR_GREEN "%06lu %-11s C1  cleared MAC table\n" CLR_RESET, millis() / 1000, "[counter]");
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
                Serial.printf(CLR_GREEN "%06lu %-11s C2  stale MAC " CLR_C164 "%s\n" CLR_RESET,
                              millis() / 1000, "[counter]", it->first.c_str());
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
    //                   millis() / 1000, "[counter]", g_evicted.load());
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

    /* Directed probe requests carry the SSID tag (element ID 0) right after
     * the 24-byte header; wildcard probes have a zero-length tag (no name). */
    std::string wifi_name;
    if (packet_length > 26 && payload[24] == 0)
    {
        uint8_t elem_len = payload[25];
        if (elem_len > 0 && (size_t)(26 + elem_len) <= packet_length)
            wifi_name.assign(reinterpret_cast<const char *>(&payload[26]), elem_len);
    }

    /* Drop frames below the RSSI threshold (too far away) */
    if (pkt->rx_ctrl.rssi < g_rssi_threshold)
    {
        g_filtered_wifi++;
        if (SHOW_DETAILS)
            Serial.printf(CLR_YELLOW "%06lu %-11s W14 " CLR_CYAN "WiFi RSSI too low " CLR_C164 "%s " CLR_YELLOW "rssi=%4d\n" CLR_RESET,
                          millis() / 1000, "[wifi]", buf18, pkt->rx_ctrl.rssi);
        return;
    }

    if (mac_upsert(buf18, "", true, MacSource::WIFI, false, wifi_name))
    {
        // bump the counters
        g_new_wifi++;
        Serial.printf(CLR_YELLOW "%06lu %-11s W1  " CLR_RED "NEW WiFi device " CLR_C164 "%s " CLR_YELLOW "rssi=%4d\n" CLR_RESET,
                      millis() / 1000, "[wifi]", buf18, pkt->rx_ctrl.rssi);
    }
    else
    {
        // duplicate sighting, already in the mac table
        g_filtered_wifi++;
        if (SHOW_DETAILS)
            Serial.printf(CLR_YELLOW "%06lu %-11s W12 " CLR_CYAN "DUP WiFi device " CLR_C164 "%s " CLR_YELLOW "rssi=%4d\n" CLR_RESET, millis() / 1000, "[wifi]", buf18, pkt->rx_ctrl.rssi);
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
    Serial.printf(CLR_YELLOW "%06lu %-11s W2  WiFi promiscuous sniffer running\n" CLR_RESET, millis() / 1000, "[wifi]", millis() / 1000);
}

// Helper function to safely read bytes from the manufacturer data
uint8_t get_byte(const std::string &data, size_t index)
{
    return static_cast<uint8_t>(data[index]);
}

/* =========================================================================
   BLE — passive scan
   ========================================================================= */
class HAXXScanCallbacks : public NimBLEScanCallbacks
{
    void onResult(const NimBLEAdvertisedDevice *advertisedDevice)
    {
        NimBLEAddress address = advertisedDevice->getAddress();

        std::string device_name = advertisedDevice->haveName() ? advertisedDevice->getName() : "Unknown device";
        std::string macAddress = advertisedDevice->getAddress().toString();
        std::string manufacturer_data = advertisedDevice->haveManufacturerData() ? advertisedDevice->getManufacturerData() : "";
        const std::string manufacturer_hex = manufacturer_data_to_key(manufacturer_data); // for logging/debugging purposes
        size_t data_len = manufacturer_data.length();
        bool is_this_a_phone = false;

        if (advertisedDevice->haveManufacturerData())
        {

            // Manufacturer data must contain at least 2 bytes for the Company ID
            if (data_len >= 2)
            {
                uint8_t id0 = get_byte(manufacturer_data, 0);
                uint8_t id1 = get_byte(manufacturer_data, 1);

                // 1. APPLE CHECK (Company ID: 0x004C)
                if (id0 == 0x4C && id1 == 0x00)
                {
                    // Serial.printf(CLR_MAG "%06lu %-11s B7  Apple manufacturer data: %s\n" CLR_RESET,
                    //          millis() / 1000, "[ble]", manufacturer_hex.c_str());

                    size_t index = 2; // Skip past 2-byte company identifier

                    if (index + 2 <= data_len)
                    {
                        uint8_t applePacketType = get_byte(manufacturer_data, index);
                        uint8_t packetLength = get_byte(manufacturer_data, index + 1);
                        uint8_t appleActionSubType = get_byte(manufacturer_data, index + 2);

                        is_this_a_phone = true;
                        if (SHOW_DETAILS)
                        {
                            Serial.printf(CLR_BLUE "%06lu %-11s B15 " CLR_C190 "Apple personal device " CLR_BLUE "detected " CLR_C164 "%s %s action:0x%02X subtype:0x%02X\n" CLR_RESET,
                                          millis() / 1000, "[ble]",
                                          advertisedDevice->getAddress().toString().c_str(),
                                          manufacturer_hex.c_str(),
                                          applePacketType, appleActionSubType);
                        }
                    }

                    //     // CRITICAL PROTECTION: Prevent length overflows and infinite loops
                    //     if (packetLength == 0 || (index + 2 + packetLength) > data_len)
                    //     {
                    //         Serial.printf(CLR_RED "%06lu %-11s B5  malformed Apple manufacturer data: length=%zu, index=%zu, packetLength=%u\n" CLR_RESET,
                    //                       millis() / 1000, "[ble]", data_len, index, packetLength);
                    //         break;
                    //     }

                    //     // Apple (0x4C) parsing loop
                    //     if (applePacketType == 0x07)
                    //     {
                    //         is_this_a_phone = true;
                    //         Serial.printf(CLR_BLUE "%06lu %-11s B11 " CLR_C190 "iPhone " CLR_BLUE "detected via AirPods (0x07)\n" CLR_C164 "%s %s\n" CLR_RESET,
                    //                       millis() / 1000, "[ble]",
                    //                       advertisedDevice->getAddress().toString().c_str(),
                    //                       manufacturer_hex.c_str());
                    //         break;
                    //     }

                    //     if (applePacketType == 0x10)
                    //     {
                    //         uint8_t appleActionSubType = get_byte(manufacturer_data, index + 2);
                    //         // Serial.printf(CLR_BLUE "%06lu %-11s B15 " CLR_BLUE "before Phone test" CLR_C164 "%s %s " CLR_BLUE "Apple action type==0x%02X : subtype=0x%02X\n" CLR_RESET,
                    //         //               millis() / 1000, "[ble]",
                    //         //               advertisedDevice->getAddress().toString().c_str(),
                    //         //               manufacturer_hex.c_str(),
                    //         //               applePacketType, appleActionSubType);

                    //         if (appleActionSubType == 0x02 || // Screen locked
                    //             appleActionSubType == 0x07 || // Active User / Screen On
                    //             appleActionSubType == 0x09 || // Active User / Video/Media Focus
                    //             appleActionSubType == 0x05 || // Audio Playing / Screen Locked:
                    //             appleActionSubType == 0x08 || // Wi-Fi Password Sharing
                    //             appleActionSubType == 0x3D || // (UWB) Spatial Proximity
                    //             appleActionSubType == 0x03 || // Idle User
                    //             appleActionSubType == 0x0F || // Nearby Action Trigger
                    //             appleActionSubType == 0x4D || // Universal Clipboard:
                    //             appleActionSubType == 0x01)   // Activity Reporting Disabled)
                    //         {
                    //             is_this_a_phone = true;
                    //             Serial.printf(CLR_BLUE "%06lu %-11s B15 " CLR_C190 "Apple personal device " CLR_BLUE "detected " CLR_C164 "%s %s action:0x%02X subtype:0x%02X\n" CLR_RESET,
                    //                           millis() / 1000, "[ble]",
                    //                           advertisedDevice->getAddress().toString().c_str(),
                    //                           manufacturer_hex.c_str(),
                    //                           applePacketType, appleActionSubType);
                    //             break;
                    //         }
                    //         // Note: 0x09 (Find My) is excluded here because MacBooks, AirTags, and iPads all broadcast it.
                    //         // Serial.printf(CLR_GREEN "%06lu %-11s B9  Apple packet type==0x%02X : subtype=0x%02X\n" CLR_RESET,
                    //         //               millis() / 1000, "[ble]", applePacketType, appleActionSubType);
                    //     }
                    //     else if (applePacketType == 0x12) // Proximity Beacon
                    //     {
                    //         uint8_t appleActionSubType = get_byte(manufacturer_data, index + 2);
                    //         // Serial.printf(CLR_GREEN "%06lu %-11s B12 Apple packet type==0x%02X : subtype=0x%02X\n" CLR_RESET,
                    //         //               millis() / 1000, "[ble]", applePacketType, appleActionSubType);

                    //         if (packetLength == 2)
                    //         {
                    //             uint8_t deviceClass = get_byte(manufacturer_data, index + 2);
                    //             if (deviceClass == 0xD0 || deviceClass == 0xD4)
                    //             {
                    //                 is_this_a_phone = false;
                    //                 Serial.printf(CLR_BLUE "%06lu %-11s B16 " CLR_BLUE "signal detected from deviceclass:0x%02X and applePacketType:0x%02X\n" CLR_C164 "%s %s\n" CLR_RESET,
                    //                               millis() / 1000, "[ble]",
                    //                               deviceClass,
                    //                               applePacketType,
                    //                               advertisedDevice->getAddress().toString().c_str(),
                    //                               manufacturer_hex.c_str());
                    //                 break;
                    //             }
                    //         }
                    //         else if (packetLength >= 15)
                    //         {
                    //             // Long 0x12 packets (like your 25-byte hits) are usually iPhones tracking state,
                    //             // but let's make sure it isn't an Apple Watch setup stream
                    //             uint8_t streamType = get_byte(manufacturer_data, index + 2);
                    //             if (streamType != 0xD0)
                    //             {
                    //                 is_this_a_phone = true;
                    //                 Serial.printf(CLR_MAG "%06lu %-11s B6  " CLR_C190 "iPhone " CLR_BLUE "detected via Long Proximity\n" CLR_C164 "%s %s\n" CLR_RESET,
                    //                               millis() / 1000, "[ble]",
                    //                               advertisedDevice->getAddress().toString().c_str(),
                    //                               manufacturer_hex.c_str());
                    //                 break;
                    //             }
                    //         }
                    //         else
                    //         {
                    //             Serial.printf(CLR_RED "%06lu %-11s B10 malformed Apple Proximity Beacon data: length=%zu, index=%zu, packetLength=%u\n" CLR_RESET,
                    //                           millis() / 1000, "[ble]", data_len, index, packetLength);
                    //         }
                    //     }
                    //     else if (applePacketType == 0x16) // Nearby Info
                    //     {
                    //         uint8_t appleActionSubType = get_byte(manufacturer_data, index + 2);
                    //         // Serial.printf(CLR_GREEN "%06lu %-11s B12 Apple packet type==0x%02X : subtype=0x%02X\n" CLR_RESET,
                    //         //               millis() / 1000, "[ble]", applePacketType, appleActionSubType);

                    //         if (packetLength == 8)
                    //         {
                    //             uint8_t flags = get_byte(manufacturer_data, index + 2);
                    //             // On a MacBook or AppleTV, the primary Nearby Info byte is highly predictable (typically 0x00 or 0x04).
                    //             // iPhones shifting states usually broadcast a dynamic bitmask (like 0x20, 0x1C, 0x0C).
                    //             if (flags != 0x00 && flags != 0x04)
                    //             {
                    //                 is_this_a_phone = true;
                    //                 Serial.printf(CLR_GREEN "%06lu %-11s B14 " CLR_C190 "iPhone " CLR_BLUE "detected via Nearby Info (0x16)\n" CLR_RESET, millis() / 1000, "[ble]");
                    //                 break;
                    //             }
                    //         }
                    //         else
                    //         {
                    //             Serial.printf(CLR_RED "%06lu %-11s B11 malformed Apple Nearby Info data: length=%zu, index=%zu, packetLength=%u\n" CLR_RESET,
                    //                           millis() / 1000, "[ble]", data_len, index, packetLength);
                    //         }
                    //     }

                    //     // Advance safely inside the boundaries
                    //     index += 2 + packetLength;
                    // }
                }
                // 2. SAMSUNG CHECK (Company ID: 0x0075)
                else if (id0 == 0x75 && id1 == 0x00 && data_len >= 4)
                {
                    uint8_t samsungDataType = get_byte(manufacturer_data, 2);

                    // 0x01, 0x02, 0x03 map to SmartThings presence/proximity
                    if (samsungDataType == 0x01 || samsungDataType == 0x02 || samsungDataType == 0x03)
                    {
                        // SMARTPHONE FILTER: SmartTags use predictable, fixed short packets (under 9 bytes)
                        // Phones broadcasting state/sync tags send much longer byte lists (> 12 bytes)
                        if (data_len > 11)
                        {
                            is_this_a_phone = true;
                            Serial.printf(CLR_MAG "%06lu %-11s B18 " CLR_C190 "Samsung phone " CLR_BLUE "detected via SmartThings\n" CLR_C164 "%s %s\n" CLR_RESET,
                                          millis() / 1000, "[ble]",
                                          advertisedDevice->getAddress().toString().c_str(),
                                          manufacturer_hex.c_str());
                        }
                    }
                }
                // 3. GOOGLE / ANDROID QUICK SHARE CHECK (Company ID: 0xE000)
                else if (id0 == 0xE0 && id1 == 0x00)
                {
                    // Google Quick Share broadcasts on 0x00E0.
                    // Simple Android beacons/tags are tiny, while Quick Share/Fast Pair frameworks require extended metadata payloads.
                    if (data_len >= 10)
                    {
                        is_this_a_phone = true;
                        Serial.printf(CLR_MAG "%06lu %-11s B18 " CLR_C190 "Android phone " CLR_BLUE "detected via Quick Share\n" CLR_C164 "%s %s\n" CLR_RESET,
                                      millis() / 1000, "[ble]",
                                      advertisedDevice->getAddress().toString().c_str(),
                                      manufacturer_hex.c_str());
                    }
                }
                // 4. Check for Android Fast Pair / Wearables (Google UUID 0xFE2C)
                else if (advertisedDevice->haveServiceUUID())
                {
                    if (advertisedDevice->getServiceUUID().toString() == "0000fe2c-0000-1000-8000-00805f9b34fb")
                        is_this_a_phone = true;
                }
            }
        }
        else
        {
            // no manufacturer data on device
            is_this_a_phone = false;
            if (SHOW_DETAILS)
            {
                Serial.printf(CLR_BLUE "%06lu %-11s B19 " CLR_CYAN "No Manufacturer Data on device " CLR_C164 "%s %s " CLR_C190 "%s" CLR_BLUE "rssi:%4d" CLR_RESET "\n",
                              millis() / 1000, "[ble]",
                              advertisedDevice->getAddress().toString().c_str(),
                              advertisedDevice->getName().c_str(),
                              is_this_a_phone ? "phone " : "",
                              advertisedDevice->getRSSI());
            }
        }

        // 4. Fallback: Check if it uses a Randomized Private MAC Address (Most mobile phones/wearables randomize, static IoT tech usually does not)
        if (advertisedDevice->getAddressType() == 1) // random mac address?
            is_this_a_phone = true;                  // is a personal device if true

        // 5. Check RSSI Level to see if over threshold
        if (advertisedDevice->getRSSI() < g_rssi_threshold)
        {
            g_filtered_ble++;
            if (SHOW_DETAILS)
            {
                Serial.printf(CLR_BLUE "%06lu %-11s B17 " CLR_CYAN "BLE RSSI too low " CLR_C164 "%s %s " CLR_BLUE "%s " CLR_C190 "%s" CLR_BLUE "rssi:%4d" CLR_RESET "\n",
                              millis() / 1000, "[ble]",
                              advertisedDevice->getAddress().toString().c_str(),
                              manufacturer_hex.c_str(),
                              advertisedDevice->getName().c_str(),
                              is_this_a_phone ? "phone " : "",
                              advertisedDevice->getRSSI());
            }
            return; // exit early if RSSI is below threshold
        }

        // Write this MAC to the MAC table and add to the counts.
        // If a manufacturer payload is present, it becomes the dedupe identity so
        // a rotating BLE random address from the same handset is not counted twice.
        if (mac_upsert(advertisedDevice->getAddress().toString(), manufacturer_data, true, MacSource::BLE, is_this_a_phone))
        {
            // bump the counters
            g_new_ble++;
            if (SHOW_DETAILS)
            {
                Serial.printf(CLR_BLUE "%06lu %-11s B1  " CLR_RED "NEW BLE device " CLR_C164 "%s %s " CLR_BLUE "%s " CLR_C190 "%s" CLR_BLUE "rssi:%4d" CLR_RESET "\n",
                              millis() / 1000, "[ble]",
                              advertisedDevice->getAddress().toString().c_str(),
                              manufacturer_hex.c_str(),
                              advertisedDevice->getName().c_str(),
                              is_this_a_phone ? "phone " : "",
                              advertisedDevice->getRSSI());
            }
        }
        else
        {
            // already exists in the mac table, a duplicate sighting
            g_filtered_ble++;
            if (SHOW_DETAILS)
            {
                Serial.printf(CLR_BLUE "%06lu %-11s B13 " CLR_CYAN "DUP BLE device " CLR_C164 "%s %s " CLR_BLUE "%s " CLR_C190 "%s" CLR_BLUE "rssi:%4d" CLR_RESET "\n",
                              millis() / 1000, "[ble]",
                              advertisedDevice->getAddress().toString().c_str(),
                              manufacturer_hex.c_str(),
                              advertisedDevice->getName().c_str(),
                              is_this_a_phone ? "phone " : "",
                              advertisedDevice->getRSSI());
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
    scan->setWindow(90);    /* 90 ms on = 90% duty, leaves air time for advertising */
    scan->start(0, false);
    Serial.printf(CLR_BLUE "%06lu %-11s B3  BLE sniffer started, interval: 100ms, window: 90ms\n" CLR_RESET, millis() / 1000, "[ble]");
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
        Serial.printf(CLR_BLUE "%06lu %-11s B4  reset BLE hardware duplicate filter\n" CLR_RESET, millis() / 1000, "[ble]");
        ble_start();

        /* WiFi probing only for ALL DEVICES */

        esp_wifi_set_promiscuous(false);
        Serial.printf(CLR_YELLOW "%06lu %-11s W3  end WiFi promiscuous mode\n" CLR_RESET, millis() / 1000, "[wifi]");

        /* WiFi: active scan stimulates nearby devices to respond */
        esp_wifi_set_mode(WIFI_MODE_STA);
        wifi_scan_config_t scan_cfg = {};
        scan_cfg.scan_type = WIFI_SCAN_TYPE_ACTIVE;
        scan_cfg.scan_time.active.min = 100;
        scan_cfg.scan_time.active.max = 200;
        Serial.printf(CLR_YELLOW "%06lu %-11s W4  starting WiFi burst\n" CLR_RESET, millis() / 1000, "[wifi]");

        if (esp_wifi_scan_start(&scan_cfg, true) == ESP_OK)
        {
            uint16_t n = 0;
            esp_wifi_scan_get_ap_num(&n);
            Serial.printf(CLR_YELLOW "%06lu %-11s W5  WiFi burst complete. %u MAC(s) to analyze\n" CLR_RESET, millis() / 1000, "[wifi]", n);
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
                        const std::string ap_ssid(reinterpret_cast<const char *>(aps[i].ssid));
                        if (mac_upsert(buf, "", true, MacSource::WIFI, false, ap_ssid))
                        {
                            Serial.printf(CLR_YELLOW "%06lu %-11s W7  " CLR_RED "NEW WiFi device " CLR_C164 "%s " CLR_YELLOW "%s\n" CLR_RESET, millis() / 1000, "[wifi]", buf, aps[i].ssid);
                        }
                        else
                        {
                            g_filtered_wifi++;
                            if (SHOW_DETAILS)
                                Serial.printf(CLR_YELLOW "%06lu %-11s W13 " CLR_CYAN "DUP WiFi burst device " CLR_C164 "%s " CLR_YELLOW "%s\n" CLR_RESET, millis() / 1000, "[wifi]", buf, aps[i].ssid);
                        }
                    }
                    free(aps);
                }
            }
        }
        // Serial.printf(CLR_YELLOW "%06lu %-11s W8 end WiFi AP scan mode\n" CLR_RESET, millis() / 1000, "[wifi]");

        esp_wifi_set_promiscuous_rx_cb(wifi_sniffer_cb);
        esp_wifi_set_channel(g_channel, WIFI_SECOND_CHAN_NONE);
        esp_wifi_set_mode(WIFI_MODE_NULL);
        Serial.printf(CLR_YELLOW "%06lu %-11s W9  starting WiFi promiscuous mode\n" CLR_RESET, millis() / 1000, "[wifi]");
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
            if (entry.second.is_phone)
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

void counter_init()
{
    g_mac_mutex = xSemaphoreCreateMutex();
    configASSERT(g_mac_mutex);
    esp_coex_preference_set(ESP_COEX_PREFER_BT);
    wifi_init(); //  set the callback function for, and start the WiFi promiscuous sniffer
    ble_init();  //  set the callback function for, and start the BLE scanner in Active mode
    Serial.printf(CLR_GREEN "%06lu %-11s C4  starting burst scheduling, pinned to Core: %d\n" CLR_RESET, millis() / 1000, "[counter]", xTaskGetCoreID(g_burst_task));

    xTaskCreatePinnedToCore(burst_scan_task, "burst_scan", 6144,
                            nullptr, 1, &g_burst_task,
                            0); /* Pin explicitly to Core 0 */
    Serial.printf(CLR_GREEN "%06lu %-11s C5  init complete \n" CLR_RESET, millis() / 1000, "[counter]");
}

uint32_t counter_get()
{
    uint32_t n = 0;
    if (xSemaphoreTake(g_mac_mutex, portMAX_DELAY) == pdTRUE)
    {
        if (CURRENT_SCAN_MODE == ScanMode::ALL_DEVICES)
        {
            n = (uint32_t)g_macs.size();
        }
        else
        {
            for (const auto &entry : g_macs)
            {
                if (entry.second.is_phone)
                    n++;
            }
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
    if (now - g_last_burst_ms >= BURST_INTERVAL_MS)
    {
        g_last_burst_ms = now;
        xTaskNotifyGive(g_burst_task);
    }
}

const char *counter_mode_label()
{
    static char buf[64];
    // snprintf(buf, sizeof(buf), "signals %ddBm or more", counter_get_rssi());
    snprintf(buf, sizeof(buf), CURRENT_SCAN_MODE == ScanMode::ALL_DEVICES ? "all devices > %ddBm" : "personal devices > %ddBm", counter_get_rssi(), counter_get_rssi());
    return buf;
}
void counter_set_rssi(int dbm)
{
    g_rssi_threshold = constrain(dbm, RSSI_MIN, RSSI_MAX);
    /* Clear the table so stale entries from the old threshold are removed */
    mac_clear();
    g_last_burst_ms = millis();
    xTaskNotifyGive(g_burst_task);
    Serial.printf(CLR_GREEN "%06lu %-11s C7  RSSI threshold set to → %d dBm\n" CLR_RESET, millis() / 1000, "[counter]", g_rssi_threshold);
}

int counter_get_rssi() { return g_rssi_threshold; }

void counter_dump_table()
{
    if (xSemaphoreTake(g_mac_mutex, portMAX_DELAY) == pdTRUE)
    {
        const uint32_t now = millis();
        Serial.printf(CLR_GREEN "%06lu %-11s C8  MAC table dump — %u %s\n" CLR_RESET,
                      now / 1000, "[counter]", (unsigned)g_macs.size(),
                      g_macs.size() == 1 ? "entry" : "entries");
        for (const auto &entry : g_macs)
        {
            const char *source_str = (entry.second.source == MacSource::WIFI) ? "wifi" : "ble";
            const char *phone_suffix = entry.second.is_phone ? "phone " : "";
            std::string name_suffix = entry.second.name.empty() ? "" : (" \"" + entry.second.name + "\"");
            Serial.printf(CLR_GREEN "%06lu %-11s C9  " CLR_C164 "%s %s " CLR_C190 "%s " CLR_BLUE "%s age=%lus\n" CLR_RESET,
                          now / 1000, "[counter]",
                          entry.first.c_str(),
                          source_str,
                          phone_suffix,
                          name_suffix.c_str(),
                          (unsigned long)((now - entry.second.last_seen) / 1000));
        }
        xSemaphoreGive(g_mac_mutex);
    }
}
