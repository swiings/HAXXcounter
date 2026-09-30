#include "ble_classify.h"

#include <assert.h>
#include <limits.h>
#include <string.h>

static ble_info_t parse(const uint8_t *payload, size_t len, const uint8_t address[6], uint8_t address_type)
{
    ble_info_t info;
    assert(ble_parse(payload, len, address, address_type, -55, &info));
    return info;
}

int main()
{
    const uint8_t rpa[6] = {1, 2, 3, 4, 5, 0x40};
    const uint8_t apple_nearby_a[] = {7, 0xFF, 0x4C, 0x00, 0x10, 0x02, 0xAA, 0xBB};
    const uint8_t apple_nearby_b[] = {7, 0xFF, 0x4C, 0x00, 0x10, 0x02, 0xCC, 0xDD};
    const ble_info_t apple_a = parse(apple_nearby_a, sizeof(apple_nearby_a), rpa, 1);
    const ble_info_t apple_b = parse(apple_nearby_b, sizeof(apple_nearby_b), rpa, 1);
    assert(apple_a.device_class == BLE_CLASS_PERSONAL);
    assert(apple_a.confidence == 60);
    assert(apple_a.vendor == BLE_VENDOR_APPLE);
    assert(apple_a.apple_type == 0x10);
    assert(apple_a.evidence & BLE_EVIDENCE_APPLE_NEARBY);
    assert(apple_a.fp_quality == BLE_FP_WEAK);
    assert(apple_a.fingerprint == apple_b.fingerprint);
    assert(apple_a.addr_kind == BLE_ADDR_RPA);

    const uint8_t multiple_manufacturers[] = {
        4, 0xFF, 0x34, 0x12, 0xAA,
        7, 0xFF, 0x4C, 0x00, 0x10, 0x02, 0xCC, 0xDD};
    const ble_info_t apple_after_other_vendor = parse(
        multiple_manufacturers, sizeof(multiple_manufacturers), rpa, 1);
    assert(apple_after_other_vendor.device_class == BLE_CLASS_PERSONAL);
    assert(apple_after_other_vendor.fingerprint == apple_a.fingerprint);

    const uint8_t apple_handoff[] = {7, 0xFF, 0x4C, 0x00, 0x0C, 0x02, 0xAA, 0xBB};
    const ble_info_t handoff = parse(apple_handoff, sizeof(apple_handoff), rpa, 1);
    assert(handoff.device_class == BLE_CLASS_PERSONAL);
    assert(handoff.evidence & BLE_EVIDENCE_APPLE_HANDOFF);

    const uint8_t phone_appearance[] = {3, 0x19, 0x40, 0x00};
    const ble_info_t appearance = parse(phone_appearance, sizeof(phone_appearance), rpa, 1);
    assert(appearance.device_class == BLE_CLASS_PHONE);
    assert(appearance.evidence & BLE_EVIDENCE_APPEARANCE);

    const uint8_t nearby_with_watch_appearance[] = {
        3, 0x19, 0xC0, 0x00,
        7, 0xFF, 0x4C, 0x00, 0x10, 0x02, 0xAA, 0xBB};
    const ble_info_t watch = parse(nearby_with_watch_appearance,
                                   sizeof(nearby_with_watch_appearance), rpa, 1);
    assert(watch.device_class == BLE_CLASS_WATCH);

    const uint8_t nearby_with_computer_appearance[] = {
        3, 0x19, 0x80, 0x00,
        7, 0xFF, 0x4C, 0x00, 0x10, 0x02, 0xAA, 0xBB};
    const ble_info_t computer = parse(nearby_with_computer_appearance,
                                      sizeof(nearby_with_computer_appearance), rpa, 1);
    assert(computer.device_class == BLE_CLASS_LAPTOP);

    const uint8_t apple_pairing[] = {7, 0xFF, 0x4C, 0x00, 0x07, 0x02, 0x01, 0x02};
    const ble_info_t earbuds = parse(apple_pairing, sizeof(apple_pairing), rpa, 1);
    assert(earbuds.device_class == BLE_CLASS_EARBUDS);

    const uint8_t fast_pair_service_data[] = {4, 0x16, 0x2C, 0xFE, 0x01};
    const ble_info_t fast_pair = parse(fast_pair_service_data, sizeof(fast_pair_service_data), rpa, 1);
    assert(fast_pair.device_class == BLE_CLASS_EARBUDS);
    assert(fast_pair.evidence & BLE_EVIDENCE_FAST_PAIR);

    const uint8_t fast_pair_uuid_list[] = {3, 0x03, 0x2C, 0xFE};
    const ble_info_t fast_pair_uuid = parse(fast_pair_uuid_list, sizeof(fast_pair_uuid_list), rpa, 1);
    assert(fast_pair_uuid.device_class == BLE_CLASS_EARBUDS);

    const uint8_t smart_tag[] = {4, 0x16, 0x5A, 0xFD, 0x01};
    const ble_info_t tracker = parse(smart_tag, sizeof(smart_tag), rpa, 1);
    assert(tracker.device_class == BLE_CLASS_TRACKER);

    const uint8_t flags_dual_mode[] = {2, 0x01, 0x02};
    const ble_info_t dual_mode = parse(flags_dual_mode, sizeof(flags_dual_mode), rpa, 1);
    assert(dual_mode.device_class == BLE_CLASS_LAPTOP);
    const uint8_t flags_le_only[] = {2, 0x01, 0x06};
    const ble_info_t le_only = parse(flags_le_only, sizeof(flags_le_only), rpa, 1);
    assert(le_only.device_class == BLE_CLASS_PHONE);
    assert(!(le_only.evidence & BLE_EVIDENCE_DUAL_MODE));

    const uint8_t static_random[6] = {1, 2, 3, 4, 5, 0xC0};
    const uint8_t empty_payload = 0;
    const ble_info_t stable = parse(&empty_payload, 0, static_random, 1);
    assert(stable.addr_kind == BLE_ADDR_STATIC_RANDOM);
    assert(stable.fp_quality == BLE_FP_STRONG);

    const uint8_t short_name[] = {7, 0x08, 'i', 'P', 'h', 'o', 'n', 'e'};
    const ble_info_t short_named = parse(short_name, sizeof(short_name), rpa, 1);
    assert(short_named.fp_quality == BLE_FP_NONE);
    const uint8_t complete_name[] = {7, 0x09, 'D', 'a', 'n', ' ', 'P', 'h'};
    const ble_info_t fully_named = parse(complete_name, sizeof(complete_name), rpa, 1);
    assert(fully_named.fp_quality == BLE_FP_STRONG);

    const uint8_t malformed[] = {5, 0xFF, 0x4C};
    ble_info_t ignored;
    assert(!ble_parse(malformed, sizeof(malformed), rpa, 1, -55, &ignored));

    /* Captured Nearby payloads: iPhone (len 7), MacBook Pro (len 6). */
    const uint8_t iphone_nearby[] = {12, 0xFF, 0x4C, 0x00, 0x10, 0x07, 0x3D, 0x1F, 0xEB, 0xC1, 0x6B, 0x81, 0x18};
    assert(parse(iphone_nearby, sizeof(iphone_nearby), rpa, 1).device_class == BLE_CLASS_PHONE);
    const uint8_t mac_nearby[] = {11, 0xFF, 0x4C, 0x00, 0x10, 0x06, 0x40, 0x1D, 0x99, 0xCC, 0x81, 0x0A};
    assert(parse(mac_nearby, sizeof(mac_nearby), rpa, 1).device_class == BLE_CLASS_LAPTOP);
    const uint8_t watch_nearby[] = {10, 0xFF, 0x4C, 0x00, 0x10, 0x05, 0x00, 0x00, 0x00, 0x00, 0x00};
    assert(parse(watch_nearby, sizeof(watch_nearby), rpa, 1).device_class == BLE_CLASS_WATCH);

    assert(INT8_MIN == -128);
    return 0;
}