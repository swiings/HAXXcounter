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

    uint8_t second_nibble = (first_byte & 0x0F);

    /* return true if locally administered or random MAC */
    return (second_nibble == 0x02 || second_nibble == 0x06 || second_nibble == 0x0A || second_nibble == 0x0E);

    // return (first_byte & 0x02) != 0; /* locally administered = random MAC */
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

// static bool mac_upsert(const std::string &mac, const std::string &manufacturer_data = "")
// {
//     const std::string key = mac_table_key(mac, manufacturer_data);
//     bool is_new = false;
//     if (xSemaphoreTake(g_mac_mutex, portMAX_DELAY) == pdTRUE)
//     {
//         is_new = (g_macs.count(key) == 0);
//         g_macs[key] = millis();
//         xSemaphoreGive(g_mac_mutex);
//     }
//     return is_new;
// }

/* Returns true if the key was not already in the table (genuinely new device).
 * When BLE manufacturer data is present it is used as the dedupe identity so
 * rotating random MACs from the same handset are not double-counted.
 */
static bool mac_upsert(const std::string &mac, const std::string &manufacturer_data = "")
{
    std::string masked_mfg = manufacturer_data;

    // Added [0] and [1] to check the first two bytes of the string
    if (manufacturer_data.size() >= 4 &&
        (unsigned char)manufacturer_data[0] == 0x4C &&
        (unsigned char)manufacturer_data[1] == 0x00)
    {

        // Added [2] to get the protocol type byte
        unsigned char protocol_type = (unsigned char)manufacturer_data[2];

        if (protocol_type == 0x07 && manufacturer_data.size() >= 6)
        {
            // AirPods / Beats: Keep Company ID, Type, Length, and 2-byte Hardware Model ID
            masked_mfg = manufacturer_data.substr(0, 6);
        }
        else
        {
            // For Nearby Info (0x10), Continuity (0x0C), Nearby V2 (0x16), and all other Apple types:
            // Keep just the Company ID (2 bytes) + Protocol Type (1 byte) = 3 bytes total
            masked_mfg = manufacturer_data.substr(0, 3);
        }
    }

    const std::string key = mac + "_" + masked_mfg;
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
            // if people/phones only, evict in half the time
            uint32_t dedupTimer = g_mode == MODE_PHONE_ESTIMATE ? DEDUP_WINDOW_MS / 2 : DEDUP_WINDOW_MS;

            if ((now - it->second) > dedupTimer)
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
    if (g_evicted > 0 && n > 0)
    {
        Serial.printf(CLR_GREEN "%06lu %-11s C3  evicted %u stale MAC(s)\n" CLR_RESET,
                      millis() / 1000, "[counter]", g_evicted.load());
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

// static void IRAM_ATTR wifi_sniffer_cb(void *buf, wifi_promiscuous_pkt_type_t type)
// {
//     if (type != WIFI_PKT_MGMT)
//         return;
//     const wifi_promiscuous_pkt_t *pkt = reinterpret_cast<const wifi_promiscuous_pkt_t *>(buf);
//     const uint8_t *frame = pkt->payload;
//     if (pkt->rx_ctrl.sig_len < 24)
//         return;
//     if (FC_TYPE(frame[0]) != MGMT_TYPE)
//         return;
//     if (FC_SUBTYPE(frame[0]) != PROBE_REQ_SUB)
//         return;

//     /* Drop frames below the RSSI threshold (too far away) */
//     if (pkt->rx_ctrl.rssi < g_rssi_threshold)
//         return;

//     /* In 802.11 frames the source MAC is in big-endian order;
//      * frame[SRC_MAC_OFFSET] is the first byte (the OUI/flag byte). */
//     const uint8_t first_byte = frame[SRC_MAC_OFFSET];
//     /* Bit 1 of first_byte is the IEEE "locally administered" flag.
//      * Set → device randomised its own MAC (phones/tablets).
//      * Clear → MAC was assigned by the manufacturer (hardware OUI). */
//     const bool is_random = (first_byte & 0x02) != 0;

//     if (g_mode == MODE_PHONE_ESTIMATE && !mac_passes_filter(first_byte))
//     {
//         g_filtered_wifi++;
//         return;
//     }

//     char buf18[18];
//     const uint8_t *m = frame + SRC_MAC_OFFSET;
//     snprintf(buf18, sizeof(buf18), "%02x:%02x:%02x:%02x:%02x:%02x",
//              m[0], m[1], m[2], m[3], m[4], m[5]);

//     if (mac_upsert(buf18))
//     {
//         g_new_wifi++;
//         Serial.printf(CLR_YELLOW "%06lu %-11s W1" CLR_RED " NEW WiFi " CLR_YELLOW "%s rssi=%4d %s\n" CLR_RESET,
//                       millis() / 1000, "[wifi]", buf18, pkt->rx_ctrl.rssi,
//                       is_random ? "random-MAC  -> phone/tablet"
//                                 : "OUI-MAC     -> hardware/AP");
//     }
// }

bool has_netgear_vendor_id(const uint8_t *payload, uint16_t length)
{
    // 802.11 management frames usually have the body start around byte 24 or 36
    // loop through the Information Elements (IEs)
    int index = 36;
    while (index < length - 2)
    {
        uint8_t ie_id = payload[index];
        uint8_t ie_len = payload[index + 1];

        // IE 221 (0xDD) is the Vendor Specific Element
        if (ie_id == 0xDD && ie_len >= 3)
        {
            // Check if the next 3 bytes match Netgear's Vendor OUI
            // Netgear uses a few, 00:14:6C is very common
            if (payload[index + 2] == 0x00 &&
                payload[index + 3] == 0x14 &&
                payload[index + 4] == 0x6C)
            {
                return true; // Confirmed Netgear-generated packet!
            }
        }
        index += 2 + ie_len; // Move to the next IE block
    }
    return false;
}

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

    /* Drop frames below the RSSI threshold (too far away) */
    if (pkt->rx_ctrl.rssi < g_rssi_threshold)
        return;

    // Byte 0 contains Frame Type and Subtype. 0x40 is a Probe Request.
    uint8_t frame_control = payload[0];
    bool is_probe_request = (frame_control == 0x40);

    // Source MAC address is located at bytes 10 to 15 in the 802.11 header
    uint8_t *src_mac = &payload[10];

    // Analyze the Fingerprint
    bool randomized = mac_passes_filter(src_mac[0]);

    char buf18[18];
    snprintf(buf18, sizeof(buf18), "%02x:%02x:%02x:%02x:%02x:%02x",
             src_mac[0], src_mac[1], src_mac[2], src_mac[3], src_mac[4], src_mac[5]);

    // Call a method to determine if this might be a router that randomizes the MAC, like Netgear
    bool is_router = has_netgear_vendor_id(payload, packet_length);

    if (g_mode == MODE_PHONE_ESTIMATE && is_router)
    {
        g_filtered_wifi++;
        Serial.printf(CLR_YELLOW "%06lu %-11s W11" CLR_CYAN " SKIP WiFi MAC, " CLR_C164 "%s " CLR_YELLOW "router/AP, Not a Personal Device" CLR_RESET "\n",
                      millis() / 1000, "[wifi]", buf18);
        return;
    }

    if (g_mode == MODE_PHONE_ESTIMATE && !randomized)
    {
        g_filtered_wifi++;
        Serial.printf(CLR_YELLOW "%06lu %-11s W6 " CLR_CYAN " SKIP WiFi MAC, " CLR_C164 "%s " CLR_YELLOW "hardware/AP, Not a Personal Device" CLR_RESET "\n",
                      millis() / 1000, "[wifi]", buf18,
                      " hardware/AP, ");
        return;
    }
    if (mac_upsert(buf18))
    {
        g_new_wifi++;
        Serial.printf(CLR_YELLOW "%06lu %-11s W1 " CLR_RED " NEW WiFi " CLR_C164 "%s " CLR_YELLOW "rssi=%4d %s\n" CLR_RESET,
                      millis() / 1000, "[wifi]", buf18, pkt->rx_ctrl.rssi,
                      randomized ? "random-MAC-> phone/tablet"
                                 : "OUI-MAC-> hardware/AP");
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
    //         Serial.printf(CLR_BLUE "%06lu %-11s" CLR_RED " NEW " CLR_BLUE "%s rssi=%4d %s" CLR_RESET "\n",
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
        NimBLEAddress address = advertisedDevice->getAddress();
        std::string manufacturer_data;
        if (advertisedDevice->haveManufacturerData())
        {
            manufacturer_data = advertisedDevice->getManufacturerData();
        }

        bool isActualPhone = false;
        bool isApple = false;
        bool isAndroid = false;
        std::string devType = "";
        // uint16_t companyId = 0;

        // Start Test code
        bool showAllMacsTestData = false;
        bool showJustPhonesTestData = false;

        // 1. Get the current MAC Address
        String macAddress = advertisedDevice->getAddress().toString().c_str();

        // 2. Extract Raw Payload Bytes using the new vector format
        const std::vector<uint8_t> &payload = advertisedDevice->getPayload();
        size_t payloadLength = payload.size();

        String structuralFingerprint = "";
        size_t index = 0;

        // Parse the raw payload into individual AD structures
        while (index < payloadLength)
        {
            uint8_t length = payload[index]; // Vectors support array indexing []
            if (length == 0)
                break; // End of packet

            if (index + length < payloadLength)
            {
                uint8_t type = payload[index + 1];
                // Build a signature of [Length:Type] pairs
                structuralFingerprint += "[" + String(length) + ":" + String(type, HEX) + "]";
            }
            index += length + 1; // Move to the next AD structure
        }

        // 3. Extract Manufacturer Specific Data (Type 0xFF)
        String mfgDataHex = "";
        if (advertisedDevice->haveManufacturerData())
        {
            std::string mfgData = advertisedDevice->getManufacturerData();
            for (char &c : mfgData)
            {
                char buf[3];
                sprintf(buf, "%02X", (unsigned char)c);
                mfgDataHex += buf;
            }
        }
        else
        {
            mfgDataHex = "NONE";
        }

        // 4. Extract Tx Power Level (Type 0x0A)
        int txPower = 0;
        bool hasTxPower = advertisedDevice->haveTXPower();
        if (hasTxPower)
        {
            txPower = advertisedDevice->getTXPower();
        }

        if (showAllMacsTestData)
        {
            // --- Print the unique profile to Serial Monitor ---
            Serial.printf(CLR_BLUE "%06lu %-11s B6  " CLR_BBLUE "MAC:" CLR_BLUE " %s  " CLR_BBLUE "Fingerprint:" CLR_BLUE " %s  " CLR_BBLUE "mfgData:" CLR_BLUE " %s  " CLR_BBLUE "txPower:" CLR_BLUE " %d  " CLR_RESET "\n",
                          millis() / 1000, "[ble]", macAddress.c_str(), structuralFingerprint.c_str(), mfgDataHex.c_str(), txPower);
        }

        // End Test code

        if (address.isRpa()) //  RPA devices are the newer phones and other devices that might change MACs randomly.
        {
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
                        // 🍏 Apple iPhone, iPad, Apple Watch, AirPods, AirTags
                        isActualPhone = true;
                        isApple = true;
                    }
                    else if (companyId == 0x00E0)
                    {
                        // Google Pixel Phones, Pixel Buds
                        isActualPhone = true;
                        isAndroid = true;
                    }
                    else if (companyId == 0x0075)
                    {
                        // Samsung Galaxy Phones, Galaxy Buds, Galaxy SmartTags
                        isActualPhone = true;
                        isAndroid = true;
                    }
                    else if (companyId == 0x0006)
                    {
                        // Microsoft Windows Laptops, Surface Tablets
                        isActualPhone = false;
                        isAndroid = false;
                    }
                    else if (companyId == 0x0059)
                    {
                        // Nordic Semi DIY Beacons, smart trackables, fitness tech
                        isActualPhone = false;
                        isAndroid = false;
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
                if (showJustPhonesTestData)
                {
                    // 1. Get the current MAC Address
                    String macAddress = advertisedDevice->getAddress().toString().c_str();

                    // 2. Extract Raw Payload Bytes using the new vector format
                    const std::vector<uint8_t> &payload = advertisedDevice->getPayload();
                    size_t payloadLength = payload.size();

                    String structuralFingerprint = "";
                    size_t index = 0;

                    // Parse the raw payload into individual AD structures
                    while (index < payloadLength)
                    {
                        uint8_t length = payload[index]; // Vectors support array indexing []
                        if (length == 0)
                            break; // End of packet

                        if (index + length < payloadLength)
                        {
                            uint8_t type = payload[index + 1];
                            // Build a signature of [Length:Type] pairs
                            structuralFingerprint += "[" + String(length) + ":" + String(type, HEX) + "]";
                        }
                        index += length + 1; // Move to the next AD structure
                    }

                    // 3. Extract Manufacturer Specific Data (Type 0xFF)
                    String mfgDataHex = "";
                    if (advertisedDevice->haveManufacturerData())
                    {
                        std::string mfgData = advertisedDevice->getManufacturerData();
                        for (char &c : mfgData)
                        {
                            char buf[3];
                            sprintf(buf, "%02X", (unsigned char)c);
                            mfgDataHex += buf;
                        }
                    }
                    else
                    {
                        mfgDataHex = "NONE";
                    }

                    // 4. Extract Tx Power Level (Type 0x0A)
                    int txPower = 0;
                    bool hasTxPower = advertisedDevice->haveTXPower();
                    if (hasTxPower)
                    {
                        txPower = advertisedDevice->getTXPower();
                    }

                    // --- Print the unique profile to Serial Monitor ---
                    Serial.printf(CLR_BLUE "%06lu %-11s B6  " CLR_BBLUE "MAC:" CLR_BLUE " %s  " CLR_BBLUE "Fingerprint:" CLR_BLUE " %s  " CLR_BBLUE "mfgData:" CLR_BLUE " %s  " CLR_BBLUE "txPower:" CLR_BLUE " %d  " CLR_RESET "\n",
                                  millis() / 1000, "[ble]", macAddress.c_str(), structuralFingerprint.c_str(), mfgDataHex.c_str(), txPower);
                }

                // End Test code

                if (isApple)
                {
                    std::string data = advertisedDevice->getManufacturerData();
                    uint8_t appleType = (uint8_t)data[2];
                    uint8_t appleLength = (uint8_t)data[3];

                    // Check if the packet is actually as long as Apple claims it is
                    if ((size_t)data.length() >= (size_t)(4 + appleLength))
                    {
                        switch (appleType)
                        {
                        case 0x02:
                            devType = "0x02 iBeacon";
                            break;

                        case 0x07:
                            devType = "0x07 AirPods";
                            break;

                        case 0x0C:
                            devType = "0x0C Continuity / Handoff data (often a Mac or iPhone)";
                            break;

                        case 0x10:
                            devType = "0x10 Nearby Info / Action with subtype ";
                            // Ensure we have at least one byte of data in the payload to check
                            if (appleLength > 0 && data.length() >= 5)
                            {
                                uint8_t deviceClassByte = (uint8_t)data[4];

                                // Filter based on known device flags / classes inside 0x10
                                if (deviceClassByte == 0x2C || deviceClassByte == 0x02 || deviceClassByte == 0x0C)
                                {
                                    devType += "0x2C, 0x02, or 0x0C (iPhone)";
                                }
                                else if (deviceClassByte == 0x0E)
                                {
                                    devType += "0x0E (Apple Watch)";
                                }
                                else if (deviceClassByte == 0x20 || deviceClassByte == 0x4C)
                                {
                                    devType += "0x20 or 0x4C (Mac / MacBook)";
                                }
                                else if (deviceClassByte == 0x39 || deviceClassByte == 0x30)
                                {
                                    devType += "0x39/0x30 (iOS Device - likely iPad or iPhone)";
                                }
                                else if (deviceClassByte == 0x3C)
                                {
                                    devType += "0x3C (iPad)";
                                }
                                else
                                {
                                    char hexBuf[4];
                                    sprintf(hexBuf, "%02X", deviceClassByte);
                                    devType += std::string(hexBuf) + " (Unknown)";
                                }
                            }
                            break;

                        case 0x12:
                            devType = "0x12 Find My / AirTag";
                            break;

                        case 0x16:
                            devType = "0x16 Apple Nearby Info V2";
                            break;

                        default:
                        {
                            char hexBuf[4];
                            sprintf(hexBuf, "%02X", appleType);
                            devType = "0x" + std::string(hexBuf) + " (Unknown Apple sub-type)";
                            break;
                        }
                        }
                    }
                }
            }
        }

        if (g_mode == MODE_PHONE_ESTIMATE && isActualPhone == false)
        {
            // fiter this one out
            g_filtered_ble++;
            Serial.printf(CLR_BLUE "%06lu %-11s B5 " CLR_CYAN " SKIP %s MAC, " CLR_C164 "%s " CLR_BLUE "%s Not a Personal Device" CLR_RESET "\n",
                          millis() / 1000, "[ble]",
                          advertisedDevice->getAddress().isRpa() ? "RPA" : "Public",
                          advertisedDevice->getAddress().toString().c_str(),
                          advertisedDevice->getName().c_str());
            return;
        }

        if (advertisedDevice->getRSSI() < g_rssi_threshold)
        {
            // filter this one out
            g_filtered_ble++;
            Serial.printf(CLR_BLUE "%06lu %-11s B0 " CLR_CYAN " SKIP MAC, RSSI Too Low, " CLR_C164 "%s " CLR_BLUE "%s rssi:%4d " CLR_RESET "\n",
                          millis() / 1000, "[ble]",
                          advertisedDevice->getAddress().toString().c_str(),
                          advertisedDevice->getName().c_str(),
                          advertisedDevice->getRSSI());
            return;
        }

        // Write this MAC to the MAC table and add to the counts.
        // If a manufacturer payload is present, it becomes the dedupe identity so
        // a rotating BLE random address from the same handset is not counted twice.
        if (mac_upsert(advertisedDevice->getAddress().toString(), manufacturer_data))
        {
            g_new_ble++;
            Serial.printf(CLR_BLUE "%06lu %-11s B1 " CLR_RED " NEW BLE " CLR_C164 "%s " CLR_BLUE "%s %s rssi:%4d %s device" CLR_RESET "\n",
                          millis() / 1000, "[ble]",
                          advertisedDevice->getAddress().toString().c_str(),
                          mfgDataHex.c_str(),
                          advertisedDevice->getName().c_str(),
                          advertisedDevice->getRSSI(),
                          isApple ? ("Apple " + std::string(devType)).c_str() : isAndroid ? "Android"
                                                                                          : "unknown");
        }
        else
        {
            // Not added since it was a duplicate
            g_filtered_ble++;
            Serial.printf(CLR_BLUE "%06lu %-11s B7 " CLR_CYAN " SKIP DUP MAC, " CLR_C164 "%s " CLR_BLUE "%s %s rssi:%4d %s device" CLR_RESET "\n",
                          millis() / 1000, "[ble]",
                          advertisedDevice->getAddress().toString().c_str(),
                          mfgDataHex.c_str(),
                          advertisedDevice->getName().c_str(),
                          advertisedDevice->getRSSI(),
                          isApple ? ("Apple " + std::string(devType)).c_str() : isAndroid ? "Android"
                                                                                          : "unknown");
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

                        if (g_mode == MODE_PHONE_ESTIMATE && !mac_passes_filter(m[0]))
                        {
                            Serial.printf(CLR_YELLOW "%06lu %-11s W10" CLR_CYAN " SKIP MAC " CLR_C164 "%s" CLR_YELLOW ", Not a Personal Device\n" CLR_RESET, millis() / 1000, "[wifi]", buf);
                            continue;
                        }

                        if (mac_upsert(buf))
                            Serial.printf(CLR_YELLOW "%06lu %-11s W7 " CLR_RED " NEW WiFi " CLR_C164 "%s\n" CLR_RESET, millis() / 1000, "[wifi]", buf);
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
    wifi_init(); //  set the callback function for, and start the WiFi promiscuous sniffer
    ble_init();  //  set the callback function for, and start the BLE scanner in Active mode
    xTaskCreatePinnedToCore(burst_scan_task, "burst_scan", 6144,
                            nullptr, 1, &g_burst_task,
                            0); /* Pin explicitly to Core 0 */
    const uint32_t now = millis();
    Serial.printf(CLR_GREEN "%06lu %-11s C4  burst scheduling started, pinned to Core: %d\n" CLR_RESET, now / 1000, "[counter]", xTaskGetCoreID(g_burst_task));
    Serial.printf(CLR_GREEN "%06lu %-11s C5  init complete \n" CLR_RESET, now / 1000, "[counter]");
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
    // evict every 5 seconds in phone estimate, 10 seconds for all devices
    if (now - g_last_evict_ms >= (g_mode == MODE_PHONE_ESTIMATE ? 5'000 : 10'000))
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
    Serial.printf(CLR_GREEN "%06lu %-11s C6  mode set to → %s\n" CLR_RESET, millis() / 1000, "[counter]", counter_mode_label());
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
    Serial.printf(CLR_GREEN "%06lu %-11s C7  RSSI threshold set to → %d dBm\n" CLR_RESET, millis() / 1000, "[counter]", g_rssi_threshold);
}

int counter_get_rssi() { return g_rssi_threshold; }
