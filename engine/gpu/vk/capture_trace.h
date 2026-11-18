#pragma once

// Optional capture diagnostics. The ring is saved only on normal process exit.
#include <cstdio>
#include <cstdlib>
#include <cstdint>
#include <mutex>
#include <vector>

namespace mkwvk::capture_trace {
#ifdef _WIN32
struct Wait {
    const char* route = "complete";
    uint64_t target = 0, seen_before = 0, seen_after = 0;
    int result = 1;
    uint64_t call_begin = 0, call_end = 0, cycles = 0, id = 0;
    DWORD cpu_begin = 0, cpu_end = 0;
    bool probed = false, cowait = false;
    unsigned arm = 0;
};

struct Event {
    const char* kind;
    uint64_t ticks, frame, thread;
    uint64_t a = 0, b = 0, c = 0, d = 0, e = 0, f = 0, g = 0, h = 0, i = 0;
};

inline uint64_t qpc() {
    LARGE_INTEGER counter{};
    QueryPerformanceCounter(&counter);
    return static_cast<uint64_t>(counter.QuadPart);
}

inline uint64_t thread_cycles() {
    ULONG64 cycles = 0;
    QueryThreadCycleTime(GetCurrentThread(), &cycles);
    return cycles;
}

inline const char* file() {
    static const char* path = getenv("RX_VK_CAPTURE_TRACE_FILE");
    return path;
}

struct Ring {
    const size_t capacity = getenv("JINRA_TEST_TIMING") ? (1u << 20) : (1u << 19);
    std::mutex lock;
    std::vector<Event> events = std::vector<Event>(capacity);
    uint64_t next = 0;

    void save() {
        std::lock_guard guard(lock);
        FILE* output = std::fopen(file(), "w");
        if (!output) return;
        LARGE_INTEGER frequency{};
        QueryPerformanceFrequency(&frequency);
        using U = unsigned long long;
        std::fprintf(output, "# qpc_hz=%llu,dropped=%llu\n", U(frequency.QuadPart), U(next > capacity ? next - capacity : 0));
        std::fputs("event,qpc,frame,thread,a,b,c,d,e,f,g,h,i\n", output);
        for (uint64_t n = next > capacity ? next - capacity : 0; n < next; ++n) {
            const Event& v = events[n % capacity];
            std::fprintf(output, "%s,%llu,%llu,%llu,%llu,%llu,%llu,%llu,%llu,%llu,%llu,%llu,%llu\n",
                         v.kind, U(v.ticks), U(v.frame), U(v.thread), U(v.a), U(v.b), U(v.c), U(v.d), U(v.e), U(v.f),
                         U(v.g), U(v.h), U(v.i));
        }
        std::fclose(output);
    }
};

inline Ring*& ring_slot() {
    static Ring* slot = nullptr;
    return slot;
}

inline void add(const Event& event) {
    // Kept alive until exit. The shutdown callback and a producer can share the lock without a lifetime race.
    static Ring* ring = [] {
        Ring* data = new Ring;
        ring_slot() = data;
        std::atexit([] { ring_slot()->save(); });
        return data;
    }();
    std::lock_guard guard(ring->lock);
    ring->events[ring->next++ % ring->capacity] = event;
}


uint64_t frame();
bool arrival_detail();
void dependency(uint64_t capture_id, uint64_t part, uint64_t whole);

#else
struct Wait {};
#endif
}

