#include "person_estimator.h"
#include "ble_classify.h"

#include <algorithm>
#include <map>
#include <math.h>
#include <set>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <vector>

static constexpr int8_t ABSENT = INT8_MIN;
static constexpr uint8_t MIN_CONFIDENCE = 50;
static constexpr uint32_t CANDIDATE_CLASSES =
    (1u << BLE_CLASS_PHONE) | (1u << BLE_CLASS_WATCH) | (1u << BLE_CLASS_EARBUDS) |
    (1u << BLE_CLASS_TRACKER) | (1u << BLE_CLASS_LAPTOP) | (1u << BLE_CLASS_PERSONAL);
/* Only groups containing one of these count as a person; trackers, laptops and earbuds are companions. */
static constexpr uint32_t ANCHOR_CLASSES =
    (1u << BLE_CLASS_PHONE) | (1u << BLE_CLASS_WATCH) | (1u << BLE_CLASS_PERSONAL);
/* Below this RSSI variance (dB^2) across shared windows the trajectory carries no signal. */
static constexpr float MIN_RSSI_VARIANCE = 9.0f;
/* Devices silent for this many completed windows stop counting; they advertise every few seconds. */
static constexpr uint8_t ACTIVE_WINDOWS = 3;

static_assert(PE_HISTORY <= 32, "presence mask is 32 bits");

struct Device
{
    uint32_t id;
    uint32_t window; /* window index of slot 0 */
    uint32_t first_window;
    uint32_t first_ms;
    uint32_t last_ms;
    int32_t cur_sum;
    uint16_t cur_n;
    int8_t mean[PE_HISTORY];
    uint8_t device_class;
    uint8_t confidence;
    uint8_t vendor;
    uint32_t person_id;
};

struct PairScore
{
    float score;
    bool informative;
};

static std::vector<Device> g_devices;
static std::map<uint64_t, uint8_t> g_streak;
static std::set<uint64_t> g_confirmed;
static uint32_t g_last_window = UINT32_MAX;
static PersonEstimate g_estimate = {};
static pe_log_fn g_log = nullptr;

static void log_line(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
static void log_line(const char *fmt, ...)
{
    if (!g_log)
        return;
    char buf[256];
    va_list args;
    va_start(args, fmt);
    vsnprintf(buf, sizeof(buf), fmt, args);
    va_end(args);
    g_log(buf);
}

static uint64_t pair_key(uint32_t a, uint32_t b)
{
    return a < b ? ((uint64_t)a << 32) | b : ((uint64_t)b << 32) | a;
}

static Device *find(uint32_t id)
{
    for (auto &d : g_devices)
        if (d.id == id)
            return &d;
    return nullptr;
}

static bool is_active(const Device &d)
{
    if (d.cur_n > 0 || d.window == d.first_window)
        return true;
    for (uint8_t s = 1; s <= ACTIVE_WINDOWS; ++s)
        if (d.mean[s] != ABSENT)
            return true;
    return false;
}

static bool is_person_class(const Device &d)
{
    return d.device_class <= BLE_CLASS_UNKNOWN &&
           (CANDIDATE_CLASSES & (1u << d.device_class)) != 0 &&
           d.confidence >= MIN_CONFIDENCE;
}

static bool is_candidate(const Device &d)
{
    return is_person_class(d) && is_active(d);
}

static void advance(Device &d, uint32_t window)
{
    if (window <= d.window)
        return;
    d.mean[0] = d.cur_n ? (int8_t)(d.cur_sum / (int32_t)d.cur_n) : ABSENT;
    d.cur_sum = 0;
    d.cur_n = 0;
    const uint32_t shift = window - d.window;
    if (shift >= PE_HISTORY)
    {
        memset(d.mean, ABSENT, sizeof(d.mean));
    }
    else
    {
        memmove(d.mean + shift, d.mean, PE_HISTORY - shift);
        memset(d.mean, ABSENT, shift);
    }
    d.window = window;
}

/* Bit s set when completed slot s (1..span) was observed. */
static uint32_t presence_mask(const Device &d, uint32_t span)
{
    uint32_t mask = 0;
    for (uint32_t s = 1; s <= span; ++s)
        if (d.mean[s] != ABSENT)
            mask |= 1u << s;
    return mask;
}

static float class_compat(uint8_t a, uint8_t b)
{
    /* phone, watch, earbuds, tracker, laptop, personal */
    static const float M[6][6] = {
        {0.2f, 1.0f, 1.0f, 0.7f, 0.6f, 0.6f},
        {1.0f, 0.1f, 0.8f, 0.6f, 0.5f, 0.6f},
        {1.0f, 0.8f, 0.3f, 0.6f, 0.6f, 0.6f},
        {0.7f, 0.6f, 0.6f, 0.3f, 0.5f, 0.5f},
        {0.6f, 0.5f, 0.6f, 0.5f, 0.1f, 0.5f},
        {0.6f, 0.6f, 0.6f, 0.5f, 0.5f, 0.5f}};
    auto row = [](uint8_t c) -> int
    {
        switch (c)
        {
        case BLE_CLASS_PHONE: return 0;
        case BLE_CLASS_WATCH: return 1;
        case BLE_CLASS_EARBUDS: return 2;
        case BLE_CLASS_TRACKER: return 3;
        case BLE_CLASS_LAPTOP: return 4;
        default: return 5;
        }
    };
    return M[row(a)][row(b)];
}

static float ecosystem(uint8_t a, uint8_t b)
{
    auto major = [](uint8_t v)
    { return v == BLE_VENDOR_APPLE || v == BLE_VENDOR_SAMSUNG || v == BLE_VENDOR_GOOGLE; };
    if (a == b && a != BLE_VENDOR_NONE)
        return 1.0f;
    if (!major(a) || !major(b))
        return 0.5f;
    if (a == BLE_VENDOR_APPLE || b == BLE_VENDOR_APPLE)
        return 0.1f;
    return 0.6f; /* Samsung with Google/Fast Pair accessories */
}

static bool score_pair(const Device &a, const Device &b, PairScore *out)
{
    const uint32_t window = a.window;
    const uint32_t age = window - std::max(a.first_window, b.first_window);
    const uint32_t span = std::min<uint32_t>(age, PE_HISTORY - 1);
    const uint32_t pa = presence_mask(a, span);
    const uint32_t pb = presence_mask(b, span);
    const int both = __builtin_popcount(pa & pb);
    const int either = __builtin_popcount(pa | pb);
    if (both < PE_MIN_SHARED_WINDOWS)
        return false;

    const float cooc = (float)both / (float)either;

    float sa = 0, sb = 0;
    for (uint32_t s = 1; s <= span; ++s)
        if ((pa & pb) & (1u << s))
        {
            sa += a.mean[s];
            sb += b.mean[s];
        }
    const float ma = sa / both, mb = sb / both;
    float va = 0, vb = 0, cov = 0;
    for (uint32_t s = 1; s <= span; ++s)
        if ((pa & pb) & (1u << s))
        {
            const float da = a.mean[s] - ma, db = b.mean[s] - mb;
            va += da * da;
            vb += db * db;
            cov += da * db;
        }
    va /= both;
    vb /= both;
    cov /= both;
    const bool informative = both >= PE_MIN_RSSI_SAMPLES &&
                             va >= MIN_RSSI_VARIANCE && vb >= MIN_RSSI_VARIANCE;
    const float trajectory = informative ? std::max(0.0f, cov / sqrtf(va * vb)) : 0.5f;

    const uint32_t ov_start = std::max(a.first_ms, b.first_ms);
    const uint32_t ov_end = std::min(a.last_ms, b.last_ms);
    const uint32_t un = std::max(a.last_ms, b.last_ms) - std::min(a.first_ms, b.first_ms);
    const float overlap = un == 0 ? 1.0f : ov_end > ov_start ? (float)(ov_end - ov_start) / un : 0.0f;

    float score = 0.30f * cooc + 0.30f * trajectory +
                  0.20f * class_compat(a.device_class, b.device_class) +
                  0.15f * ecosystem(a.vendor, b.vendor) + 0.05f * overlap;
    /* Co-presence without shared RSSI movement cannot distinguish companions from neighbours. */
    if (!informative)
        score = std::min(score, PE_ASSOCIATE_SCORE - 0.01f);

    out->score = score;
    out->informative = informative;
    return true;
}

static void evaluate()
{
    std::vector<int> cand;
    for (int i = 0; i < (int)g_devices.size(); ++i)
    {
        g_devices[i].person_id = 0;
        if (is_candidate(g_devices[i]))
            cand.push_back(i);
    }

    struct Edge
    {
        int i, j;
        float score;
    };
    std::vector<Edge> edges;
    std::map<uint64_t, uint8_t> streak;
    std::set<uint64_t> confirmed;
    uint32_t unresolved = 0;

    for (size_t x = 0; x < cand.size(); ++x)
        for (size_t y = x + 1; y < cand.size(); ++y)
        {
            const Device &a = g_devices[cand[x]];
            const Device &b = g_devices[cand[y]];
            const uint64_t key = pair_key(a.id, b.id);
            const bool was_confirmed = g_confirmed.count(key) != 0;
            PairScore ps;
            const bool scored = score_pair(a, b, &ps);

            if (scored && ps.score >= PE_ASSOCIATE_SCORE)
            {
                auto it = g_streak.find(key);
                const uint8_t n = (uint8_t)std::min<int>((it == g_streak.end() ? 0 : it->second) + 1, 255);
                streak[key] = n;
                if (was_confirmed || n >= PE_CONFIRM_EVALS)
                {
                    confirmed.insert(key);
                    if (!was_confirmed)
                        log_line("associate %lu+%lu score=%.2f", (unsigned long)a.id,
                                 (unsigned long)b.id, ps.score);
                }
            }
            else if (scored && ps.score >= PE_SEPARATE_SCORE)
            {
                /* Hold existing state; do not build support toward association. */
                if (was_confirmed)
                    confirmed.insert(key);
                else
                    unresolved++;
            }
            else if (was_confirmed)
            {
                if (scored)
                    log_line("separate %lu+%lu score=%.2f", (unsigned long)a.id,
                             (unsigned long)b.id, ps.score);
                else
                    log_line("separate %lu+%lu insufficient evidence", (unsigned long)a.id,
                             (unsigned long)b.id);
            }

            if (confirmed.count(key))
                edges.push_back({cand[x], cand[y], scored ? ps.score : 0.0f});
        }

    g_streak.swap(streak);
    g_confirmed.swap(confirmed);

    /* Complete-linkage grouping: groups merge only if every cross pair is confirmed. */
    std::sort(edges.begin(), edges.end(), [](const Edge &l, const Edge &r)
              { return l.score > r.score; });
    std::map<int, std::vector<int>> groups;
    std::map<int, int> group_of;
    for (int i : cand)
    {
        groups[i] = {i};
        group_of[i] = i;
    }
    for (const Edge &e : edges)
    {
        const int gi = group_of[e.i], gj = group_of[e.j];
        if (gi == gj || groups[gi].size() + groups[gj].size() > PE_MAX_GROUP_SIZE)
            continue;
        bool consistent = true;
        for (int m : groups[gi])
            for (int n : groups[gj])
                if (!g_confirmed.count(pair_key(g_devices[m].id, g_devices[n].id)))
                    consistent = false;
        if (!consistent)
            continue;
        for (int n : groups[gj])
        {
            groups[gi].push_back(n);
            group_of[n] = gi;
        }
        groups.erase(gj);
    }

    uint32_t people = 0;
    for (const auto &g : groups)
    {
        uint32_t pid = UINT32_MAX;
        bool anchored = false;
        for (int m : g.second)
        {
            pid = std::min(pid, g_devices[m].id);
            anchored |= (ANCHOR_CLASSES & (1u << g_devices[m].device_class)) != 0;
        }
        for (int m : g.second)
            g_devices[m].person_id = pid;
        if (anchored)
            people++;
    }

    g_estimate.devices = (uint32_t)cand.size();
    g_estimate.people = people;
    g_estimate.associated_pairs = (uint32_t)g_confirmed.size();
    g_estimate.unresolved_pairs = unresolved;
}

void pe_set_logger(pe_log_fn fn) { g_log = fn; }

void pe_reset()
{
    g_devices.clear();
    g_streak.clear();
    g_confirmed.clear();
    g_estimate = {};
    g_last_window = UINT32_MAX;
}

void pe_observe(uint32_t device_id, uint32_t now_ms, int8_t rssi,
                uint8_t device_class, uint8_t confidence, uint8_t vendor)
{
    const uint32_t window = now_ms / PE_WINDOW_MS;
    Device *d = find(device_id);
    if (!d)
    {
        Device fresh = {};
        fresh.id = device_id;
        fresh.window = window;
        fresh.first_window = window;
        fresh.first_ms = now_ms;
        memset(fresh.mean, ABSENT, sizeof(fresh.mean));
        g_devices.push_back(fresh);
        d = &g_devices.back();
    }
    advance(*d, window);
    d->cur_sum += rssi;
    if (d->cur_n < UINT16_MAX)
        d->cur_n++;
    d->last_ms = now_ms;
    d->device_class = device_class;
    d->confidence = confidence;
    if (vendor != BLE_VENDOR_NONE)
        d->vendor = vendor;
}

void pe_forget(uint32_t device_id)
{
    for (auto it = g_devices.begin(); it != g_devices.end(); ++it)
        if (it->id == device_id)
        {
            g_devices.erase(it);
            return;
        }
}

/* Completed-window slot of the most recent (newest=true) or oldest sighting, or 0 if none. */
static uint8_t sighting_slot(const Device &d, bool newest)
{
    for (uint8_t i = 1; i < PE_HISTORY; ++i)
    {
        const uint8_t s = newest ? i : (uint8_t)(PE_HISTORY - i);
        if (d.mean[s] != ABSENT)
            return s;
    }
    return 0;
}

static void remap_pairs(uint32_t from, uint32_t to)
{
    std::map<uint64_t, uint8_t> streak;
    for (const auto &e : g_streak)
    {
        const uint32_t a = (uint32_t)(e.first >> 32), b = (uint32_t)e.first;
        const uint32_t na = a == from ? to : a, nb = b == from ? to : b;
        if (na != nb)
            streak[pair_key(na, nb)] = e.second;
    }
    g_streak.swap(streak);
    std::set<uint64_t> confirmed;
    for (uint64_t k : g_confirmed)
    {
        const uint32_t a = (uint32_t)(k >> 32), b = (uint32_t)k;
        const uint32_t na = a == from ? to : a, nb = b == from ? to : b;
        if (na != nb)
            confirmed.insert(pair_key(na, nb));
    }
    g_confirmed.swap(confirmed);
}

/* A new address that appears as a silent one stops is treated as the same device when the match is unique. */
static void link_rotations(uint32_t window)
{
    const uint32_t settle = 2 * PE_ROTATION_SLACK + 1;
    if (window < settle + PE_ROTATION_SLACK)
        return;
    const uint32_t born = window - settle;

    std::vector<std::pair<size_t, size_t>> matches; /* (new, old) */
    for (size_t n = 0; n < g_devices.size(); ++n)
    {
        const Device &nd = g_devices[n];
        const uint8_t ns = sighting_slot(nd, false);
        if (nd.first_window != born || !is_person_class(nd) || ns == 0)
            continue;
        for (size_t o = 0; o < g_devices.size(); ++o)
        {
            const Device &od = g_devices[o];
            const uint8_t os = sighting_slot(od, true);
            if (o == n || od.first_window >= born || od.cur_n > 0 || os == 0 ||
                !is_person_class(od) || od.vendor != nd.vendor)
                continue;
            const uint32_t last = window - os;
            if (last + PE_ROTATION_SLACK < born || last > born + PE_ROTATION_SLACK)
                continue;
            if (od.device_class != nd.device_class && od.device_class != BLE_CLASS_PERSONAL &&
                nd.device_class != BLE_CLASS_PERSONAL)
                continue;
            if (abs(od.mean[os] - nd.mean[ns]) > PE_ROTATION_MAX_RSSI_DELTA)
                continue;
            matches.push_back({n, o});
        }
    }

    std::vector<uint32_t> drop;
    for (const auto &m : matches)
    {
        const auto same_new = std::count_if(matches.begin(), matches.end(), [&](const std::pair<size_t, size_t> &x)
                                            { return x.first == m.first; });
        const auto same_old = std::count_if(matches.begin(), matches.end(), [&](const std::pair<size_t, size_t> &x)
                                            { return x.second == m.second; });
        if (same_new != 1 || same_old != 1)
            continue;
        Device &nd = g_devices[m.first];
        const Device &od = g_devices[m.second];
        for (uint8_t s = 1; s < PE_HISTORY; ++s)
            if (nd.mean[s] == ABSENT)
                nd.mean[s] = od.mean[s];
        nd.first_window = od.first_window;
        nd.first_ms = od.first_ms;
        remap_pairs(od.id, nd.id);
        log_line("rotate %lu->%lu", (unsigned long)od.id, (unsigned long)nd.id);
        drop.push_back(od.id);
    }
    for (uint32_t id : drop)
        pe_forget(id);
}

/* One line per closed window: id:mean_rssi for each candidate heard, for offline tuning. */
static void log_window(uint32_t window)
{
    char buf[200];
    int len = 0;
    for (const auto &d : g_devices)
    {
        if (!is_candidate(d) || d.mean[1] == ABSENT)
            continue;
        if (len > (int)sizeof(buf) - 16)
        {
            log_line("obs w=%lu%s", (unsigned long)(window - 1), buf);
            len = 0;
        }
        len += snprintf(buf + len, sizeof(buf) - len, " %lu:%d", (unsigned long)d.id, d.mean[1]);
    }
    if (len > 0)
        log_line("obs w=%lu%s", (unsigned long)(window - 1), buf);
}

bool pe_tick(uint32_t now_ms)
{
    const uint32_t window = now_ms / PE_WINDOW_MS;
    if (window == g_last_window)
        return false;
    g_last_window = window;
    for (auto &d : g_devices)
        advance(d, window);
    log_window(window);
    link_rotations(window);
    evaluate();
    return true;
}

PersonEstimate pe_get() { return g_estimate; }

uint32_t pe_person_id(uint32_t device_id)
{
    const Device *d = find(device_id);
    return d ? d->person_id : 0;
}

float pe_pair_score(uint32_t a, uint32_t b)
{
    const Device *da = find(a);
    const Device *db = find(b);
    PairScore ps;
    if (!da || !db || da->window != db->window || !score_pair(*da, *db, &ps))
        return -1.0f;
    return ps.score;
}
