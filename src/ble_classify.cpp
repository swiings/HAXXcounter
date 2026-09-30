#include "ble_classify.h"

#include <limits.h>
#include <string.h>

namespace
{
constexpr uint8_t AD_FLAGS = 0x01;
constexpr uint8_t AD_UUID16_INCOMPLETE = 0x02;
constexpr uint8_t AD_UUID16_COMPLETE = 0x03;
constexpr uint8_t AD_NAME_SHORT = 0x08;
constexpr uint8_t AD_NAME_COMPLETE = 0x09;
constexpr uint8_t AD_APPEARANCE = 0x19;
constexpr uint8_t AD_SERVICE_DATA16 = 0x16;
constexpr uint8_t AD_MANUFACTURER = 0xFF;
constexpr uint8_t AD_FLAG_DUAL_MODE = 0x04;

uint32_t hash_byte(uint32_t hash, uint8_t value)
{
    return (hash ^ value) * 16777619u;
}

uint32_t hash_bytes(uint32_t hash, const uint8_t *bytes, size_t len)
{
    for (size_t i = 0; i < len; ++i)
        hash = hash_byte(hash, bytes[i]);
    return hash;
}

void classify(ble_info_t *out, uint8_t device_class, uint8_t confidence,
              uint32_t evidence)
{
    out->evidence |= evidence;
    if (confidence >= out->confidence)
    {
        out->device_class = device_class;
        out->confidence = confidence;
    }
}

void parse_manufacturer(const uint8_t *data, size_t len, ble_info_t *out)
{
    if (len < 2)
        return;

    const uint16_t company = (uint16_t)data[0] | ((uint16_t)data[1] << 8);
    switch (company)
    {
    case 0x004C:
        out->vendor = BLE_VENDOR_APPLE;
        for (size_t i = 2; i + 2 <= len;)
        {
            const uint8_t type = data[i++];
            const size_t item_len = data[i++];
            if (item_len > len - i)
                break;
            if (out->apple_type == 0)
                out->apple_type = type;
            switch (type)
            {
            case 0x10:
                classify(out, BLE_CLASS_PERSONAL, 60, BLE_EVIDENCE_APPLE_NEARBY);
                /* Nearby length observed on-site: 5 Watch, 6 MacBook Pro, 7 iPhone. OS-version dependent. */
                if (item_len == 5)
                    classify(out, BLE_CLASS_WATCH, 65, BLE_EVIDENCE_APPLE_NEARBY);
                else if (item_len == 6)
                    classify(out, BLE_CLASS_LAPTOP, 65, BLE_EVIDENCE_APPLE_NEARBY);
                else if (item_len == 7)
                    classify(out, BLE_CLASS_PHONE, 65, BLE_EVIDENCE_APPLE_NEARBY);
                break;
            case 0x0C:
                classify(out, BLE_CLASS_PERSONAL, 60, BLE_EVIDENCE_APPLE_HANDOFF);
                break;
            case 0x0F:
                classify(out, BLE_CLASS_PHONE, 80, BLE_EVIDENCE_APPLE_ACTION);
                break;
            case 0x07:
                classify(out, BLE_CLASS_EARBUDS, 85, BLE_EVIDENCE_APPLE_PAIRING);
                break;
            case 0x12:
                classify(out, BLE_CLASS_TRACKER, 90, BLE_EVIDENCE_APPLE_FIND_MY);
                break;
            case 0x09:
                classify(out, BLE_CLASS_TV_MEDIA, 85, BLE_EVIDENCE_APPLE_INFRASTRUCTURE);
                break;
            case 0x02:
                classify(out, BLE_CLASS_BEACON, 85, BLE_EVIDENCE_APPLE_INFRASTRUCTURE);
                break;
            default:
                break;
            }
            i += item_len;
        }
        break;
    case 0x0075:
        out->vendor = BLE_VENDOR_SAMSUNG;
        if (len > 11 && (data[2] == 0x01 || data[2] == 0x02 || data[2] == 0x03))
            classify(out, BLE_CLASS_PHONE, 65, BLE_EVIDENCE_SAMSUNG);
        break;
    case 0x00E0:
        out->vendor = BLE_VENDOR_GOOGLE;
        break;
    case 0x0006:
        out->vendor = BLE_VENDOR_MICROSOFT;
        classify(out, BLE_CLASS_LAPTOP, 75, BLE_EVIDENCE_MICROSOFT);
        break;
    case 0x0087:
        out->vendor = BLE_VENDOR_GARMIN;
        classify(out, BLE_CLASS_WATCH, 75, BLE_EVIDENCE_GARMIN);
        break;
    default:
        break;
    }
}

void classify_appearance(uint16_t appearance, ble_info_t *out)
{
    const uint16_t category = appearance & 0xFFC0;
    switch (category)
    {
    case 0x0040:
        classify(out, BLE_CLASS_PHONE, 65, BLE_EVIDENCE_APPEARANCE);
        break;
    case 0x0080:
        classify(out, BLE_CLASS_LAPTOP, 65, BLE_EVIDENCE_APPEARANCE);
        break;
    case 0x00C0:
        classify(out, BLE_CLASS_WATCH, 70, BLE_EVIDENCE_APPEARANCE);
        break;
    default:
        break;
    }
}

void make_fingerprint(ble_info_t *out, const uint8_t *manufacturer,
                      size_t manufacturer_len)
{
    uint32_t hash = 2166136261u;
    if (out->addr_kind == BLE_ADDR_PUBLIC || out->addr_kind == BLE_ADDR_STATIC_RANDOM)
    {
        hash = hash_byte(hash, out->addr_kind);
        hash = hash_bytes(hash, out->addr, sizeof(out->addr));
        out->fingerprint = hash;
        out->fp_quality = BLE_FP_STRONG;
        return;
    }

    if (out->name[0] != '\0' && (out->evidence & BLE_EVIDENCE_NAME))
    {
        hash = hash_bytes(hash, reinterpret_cast<const uint8_t *>(out->name), strlen(out->name));
        out->fingerprint = hash;
        out->fp_quality = BLE_FP_STRONG;
        return;
    }

    if (out->vendor == BLE_VENDOR_APPLE && manufacturer_len >= 4)
    {
        hash = hash_byte(hash, out->vendor);
        hash = hash_byte(hash, out->apple_type);
        hash = hash_byte(hash, (uint8_t)manufacturer_len);
        if (out->apple_type == 0x07 && manufacturer_len >= 6)
            hash = hash_bytes(hash, manufacturer + 4, 2);
        out->fingerprint = hash;
        out->fp_quality = BLE_FP_WEAK;
        return;
    }

    if (out->vendor == BLE_VENDOR_SAMSUNG || out->vendor == BLE_VENDOR_GOOGLE)
    {
        hash = hash_byte(hash, out->vendor);
        hash = hash_byte(hash, out->device_class);
        hash = hash_byte(hash, (uint8_t)manufacturer_len);
        out->fingerprint = hash;
        out->fp_quality = BLE_FP_WEAK;
    }
}
} // namespace

bool ble_parse(const uint8_t *payload, size_t len, const uint8_t addr_le[6],
               uint8_t addr_type, int8_t rssi,
               ble_info_t *out)
{
    if (payload == nullptr || addr_le == nullptr || out == nullptr)
        return false;

    memset(out, 0, sizeof(*out));
    memcpy(out->addr, addr_le, sizeof(out->addr));
    out->addr_kind = BLE_ADDR_UNKNOWN;
    out->rssi = rssi;
    out->device_class = BLE_CLASS_UNKNOWN;

    if (addr_type == 0)
        out->addr_kind = BLE_ADDR_PUBLIC;
    else if (addr_type == 1)
    {
        switch (addr_le[5] >> 6)
        {
        case 0x03:
            out->addr_kind = BLE_ADDR_STATIC_RANDOM;
            break;
        case 0x01:
            out->addr_kind = BLE_ADDR_RPA;
            out->evidence |= BLE_EVIDENCE_RPA;
            break;
        case 0x00:
            out->addr_kind = BLE_ADDR_NRPA;
            break;
        default:
            out->addr_kind = BLE_ADDR_UNKNOWN;
            break;
        }
    }

    const uint8_t *fingerprint_manufacturer = nullptr;
    size_t fingerprint_manufacturer_len = 0;
    for (size_t offset = 0; offset < len;)
    {
        const uint8_t field_len = payload[offset++];
        if (field_len == 0)
            break;
        if ((size_t)field_len > len - offset)
            return false;

        const uint8_t type = payload[offset++];
        const uint8_t *data = payload + offset;
        const size_t data_len = field_len - 1;
        switch (type)
        {
        case AD_FLAGS:
            if (data_len >= 1)
            {
                if ((data[0] & AD_FLAG_DUAL_MODE) == 0)
                {
                    out->evidence |= BLE_EVIDENCE_DUAL_MODE;
                    classify(out, BLE_CLASS_LAPTOP, 55, BLE_EVIDENCE_DUAL_MODE);
                }
            }
            break;
        case AD_UUID16_INCOMPLETE:
        case AD_UUID16_COMPLETE:
            for (size_t i = 0; i + 1 < data_len; i += 2)
            {
                const uint16_t uuid = (uint16_t)data[i] | ((uint16_t)data[i + 1] << 8);
                if (uuid == 0xFE2C)
                    classify(out, BLE_CLASS_EARBUDS, 75, BLE_EVIDENCE_FAST_PAIR);
            }
            break;
        case AD_NAME_SHORT:
        case AD_NAME_COMPLETE:
            if (out->name[0] == '\0' && data_len > 0)
            {
                size_t copy_len = data_len < sizeof(out->name) - 1 ? data_len : sizeof(out->name) - 1;
                memcpy(out->name, data, copy_len);
                out->name[copy_len] = '\0';
                if (type == AD_NAME_COMPLETE)
                    out->evidence |= BLE_EVIDENCE_NAME;
            }
            break;
        case AD_APPEARANCE:
            if (data_len >= 2)
            {
                const uint16_t appearance = (uint16_t)data[0] | ((uint16_t)data[1] << 8);
                classify_appearance(appearance, out);
            }
            break;
        case AD_SERVICE_DATA16:
            if (data_len >= 2)
            {
                const uint16_t uuid = (uint16_t)data[0] | ((uint16_t)data[1] << 8);
                if (uuid == 0xFE2C)
                    classify(out, BLE_CLASS_EARBUDS, 75, BLE_EVIDENCE_FAST_PAIR);
                else if (uuid == 0xFD5A)
                    classify(out, BLE_CLASS_TRACKER, 85, BLE_EVIDENCE_SMART_TAG);
                else if (uuid == 0xFEED || uuid == 0xFEEC)
                {
                    out->vendor = BLE_VENDOR_TILE;
                    classify(out, BLE_CLASS_TRACKER, 80, BLE_EVIDENCE_TILE);
                }
            }
            break;
        case AD_MANUFACTURER:
            if (data_len >= 2)
            {
                const uint16_t company = (uint16_t)data[0] | ((uint16_t)data[1] << 8);
                if (company == 0x004C || company == 0x0075 || company == 0x00E0)
                {
                    fingerprint_manufacturer = data;
                    fingerprint_manufacturer_len = data_len;
                }
            }
            parse_manufacturer(data, data_len, out);
            break;
        default:
            break;
        }
        offset += data_len;
    }

    if (out->addr_kind == BLE_ADDR_RPA && out->confidence < 50)
        classify(out, BLE_CLASS_PHONE, 50, BLE_EVIDENCE_RPA);

    make_fingerprint(out, fingerprint_manufacturer, fingerprint_manufacturer_len);
    return true;
}