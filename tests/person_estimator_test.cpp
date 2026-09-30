#include "person_estimator.h"
#include "ble_classify.h"

#include <assert.h>
#include <math.h>
#include <stdio.h>

static void observe_window(uint32_t window, uint32_t id, int rssi, uint8_t device_class, uint8_t vendor)
{
    for (uint32_t k = 0; k < 5; ++k)
        pe_observe(id, window * PE_WINDOW_MS + k * 900, (int8_t)rssi, device_class, 70, vendor);
}

static void tick(uint32_t window) { pe_tick(window * PE_WINDOW_MS); }

int main()
{
    /* Phone and watch walking together; a stranger's phone moves independently. */
    pe_reset();
    for (uint32_t w = 0; w < 30; ++w)
    {
        const int shared = -65 + (int)lroundf(10.0f * sinf(w * 0.6f));
        const int stranger = -70 + (int)lroundf(10.0f * sinf(w * 1.7f + 2.0f));
        observe_window(w, 1, shared, BLE_CLASS_PHONE, BLE_VENDOR_APPLE);
        observe_window(w, 2, shared - 4, BLE_CLASS_WATCH, BLE_VENDOR_APPLE);
        observe_window(w, 3, stranger, BLE_CLASS_PHONE, BLE_VENDOR_APPLE);
        tick(w + 1);
    }
    PersonEstimate est = pe_get();
    assert(est.devices == 3);
    assert(est.people == 2);
    assert(pe_person_id(1) == pe_person_id(2));
    assert(pe_person_id(3) == 3);
    assert(pe_pair_score(1, 2) >= PE_ASSOCIATE_SCORE);
    assert(pe_pair_score(1, 3) < PE_ASSOCIATE_SCORE);

    /* The watch is left behind: independent behaviour splits the group. */
    for (uint32_t w = 30; w < 60; ++w)
    {
        const int shared = -65 + (int)lroundf(10.0f * sinf(w * 0.6f));
        observe_window(w, 1, shared, BLE_CLASS_PHONE, BLE_VENDOR_APPLE);
        if (w % 3 == 0)
            observe_window(w, 2, -80, BLE_CLASS_WATCH, BLE_VENDOR_APPLE);
        tick(w + 1);
    }
    assert(pe_person_id(1) != pe_person_id(2));

    /* Stationary companions: flat RSSI cannot prove shared ownership. */
    pe_reset();
    for (uint32_t w = 0; w < 30; ++w)
    {
        observe_window(w, 10, -60, BLE_CLASS_PHONE, BLE_VENDOR_APPLE);
        observe_window(w, 11, -62, BLE_CLASS_WATCH, BLE_VENDOR_APPLE);
        tick(w + 1);
    }
    est = pe_get();
    assert(est.people == 2);
    assert(est.unresolved_pairs == 1);

    /* Too few shared windows: no association yet. */
    pe_reset();
    for (uint32_t w = 0; w < 4; ++w)
    {
        const int shared = -65 + (int)lroundf(10.0f * sinf(w * 0.6f));
        observe_window(w, 20, shared, BLE_CLASS_PHONE, BLE_VENDOR_SAMSUNG);
        observe_window(w, 21, shared, BLE_CLASS_WATCH, BLE_VENDOR_SAMSUNG);
        tick(w + 1);
    }
    assert(pe_get().people == 2);
    assert(pe_pair_score(20, 21) < 0);

    /* Non-personal classes are not candidates. */
    pe_reset();
    observe_window(0, 30, -60, BLE_CLASS_TV_MEDIA, BLE_VENDOR_NONE);
    tick(1);
    assert(pe_get().devices == 0);
    assert(pe_person_id(30) == 0);

    /* Forgetting a device removes it from the estimate. */
    observe_window(1, 31, -60, BLE_CLASS_PHONE, BLE_VENDOR_APPLE);
    tick(2);
    assert(pe_get().people == 1);
    pe_forget(31);
    tick(3);
    assert(pe_get().people == 0);

    /* A device that goes quiet stops counting before the counter evicts it. */
    pe_reset();
    for (uint32_t w = 0; w < 10; ++w)
    {
        observe_window(w, 40, -60, BLE_CLASS_PHONE, BLE_VENDOR_APPLE);
        if (w < 5)
            observe_window(w, 41, -60, BLE_CLASS_PHONE, BLE_VENDOR_APPLE);
        tick(w + 1);
    }
    assert(pe_get().people == 1);
    assert(pe_person_id(41) == 0);

    /* A small drift on a distant device is not a shared trajectory. */
    pe_reset();
    const int mover[] = {-55, -55, -55, -55, -60, -61, -63, -66, -66, -64, -62, -61};
    const int still[] = {-84, -84, -84, -83, -84, -84, -87, -90, -89, -90, -90, -89};
    for (uint32_t w = 0; w < 12; ++w)
    {
        observe_window(w, 50, mover[w], BLE_CLASS_PERSONAL, BLE_VENDOR_APPLE);
        observe_window(w, 51, still[w], BLE_CLASS_TRACKER, BLE_VENDOR_APPLE);
        tick(w + 1);
    }
    assert(pe_person_id(50) != pe_person_id(51));

    /* Trackers and laptops alone are not people; a phone is. */
    pe_reset();
    for (uint32_t w = 0; w < 3; ++w)
    {
        observe_window(w, 70, -60, BLE_CLASS_TRACKER, BLE_VENDOR_APPLE);
        observe_window(w, 71, -70, BLE_CLASS_LAPTOP, BLE_VENDOR_APPLE);
        tick(w + 1);
    }
    assert(pe_get().devices == 2);
    assert(pe_get().people == 0);
    observe_window(3, 72, -65, BLE_CLASS_PERSONAL, BLE_VENDOR_APPLE);
    tick(4);
    assert(pe_get().people == 1);

    /* Watch rotates its address mid-walk: history and link carry over to the new address. */
    pe_reset();
    for (uint32_t w = 0; w < 30; ++w)
    {
        const int shared = -65 + (int)lroundf(10.0f * sinf(w * 0.6f));
        observe_window(w, 60, shared, BLE_CLASS_PHONE, BLE_VENDOR_APPLE);
        observe_window(w, w < 20 ? 61 : 62, shared - 3, BLE_CLASS_PERSONAL, BLE_VENDOR_APPLE);
        observe_window(w, 63, -70 + (int)lroundf(10.0f * sinf(w * 1.7f + 2.0f)), BLE_CLASS_PHONE, BLE_VENDOR_APPLE);
        if (w == 18)
            assert(pe_person_id(60) == pe_person_id(61));
        tick(w + 1);
    }
    assert(pe_person_id(61) == 0);
    assert(pe_person_id(60) == pe_person_id(62));
    assert(pe_get().people == 2);

    puts("person_estimator_test: ok");
    return 0;
}
