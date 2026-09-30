#pragma once

#include <stddef.h>
#include <stdint.h>

enum BleAddressKind : uint8_t
{
    BLE_ADDR_PUBLIC,
    BLE_ADDR_STATIC_RANDOM,
    BLE_ADDR_RPA,
    BLE_ADDR_NRPA,
    BLE_ADDR_UNKNOWN
};

enum BleVendor : uint8_t
{
    BLE_VENDOR_NONE,
    BLE_VENDOR_APPLE,
    BLE_VENDOR_SAMSUNG,
    BLE_VENDOR_GOOGLE,
    BLE_VENDOR_MICROSOFT,
    BLE_VENDOR_GARMIN,
    BLE_VENDOR_TILE
};

enum BleDeviceClass : uint8_t
{
    BLE_CLASS_PHONE,
    BLE_CLASS_WATCH,
    BLE_CLASS_EARBUDS,
    BLE_CLASS_TRACKER,
    BLE_CLASS_LAPTOP,
    BLE_CLASS_TV_MEDIA,
    BLE_CLASS_BEACON,
    BLE_CLASS_PERSONAL,
    BLE_CLASS_UNKNOWN
};

enum BleFingerprintQuality : uint8_t
{
    BLE_FP_NONE,
    BLE_FP_WEAK,
    BLE_FP_STRONG
};

enum BleEvidence : uint32_t
{
    BLE_EVIDENCE_APPLE_NEARBY = 1u << 0,
    BLE_EVIDENCE_APPLE_HANDOFF = 1u << 1,
    BLE_EVIDENCE_APPLE_ACTION = 1u << 2,
    BLE_EVIDENCE_APPLE_PAIRING = 1u << 3,
    BLE_EVIDENCE_APPLE_FIND_MY = 1u << 4,
    BLE_EVIDENCE_APPLE_INFRASTRUCTURE = 1u << 5,
    BLE_EVIDENCE_FAST_PAIR = 1u << 6,
    BLE_EVIDENCE_SMART_TAG = 1u << 7,
    BLE_EVIDENCE_TILE = 1u << 8,
    BLE_EVIDENCE_MICROSOFT = 1u << 9,
    BLE_EVIDENCE_GARMIN = 1u << 10,
    BLE_EVIDENCE_APPEARANCE = 1u << 11,
    BLE_EVIDENCE_DUAL_MODE = 1u << 12,
    BLE_EVIDENCE_RPA = 1u << 13,
    BLE_EVIDENCE_NAME = 1u << 14,
    BLE_EVIDENCE_SAMSUNG = 1u << 15
};

struct ble_info_t
{
    uint8_t addr[6];
    uint8_t addr_kind;
    int8_t rssi;
    uint8_t vendor;
    uint8_t apple_type;
    char name[20];
    uint8_t device_class;
    uint8_t confidence;
    uint32_t evidence;
    uint32_t fingerprint;
    uint8_t fp_quality;
};

/* addr_type follows the Bluetooth address type: 0 public, 1 random. */
bool ble_parse(const uint8_t *payload, size_t len, const uint8_t addr_le[6],
               uint8_t addr_type, int8_t rssi,
               ble_info_t *out);