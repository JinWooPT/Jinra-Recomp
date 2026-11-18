#pragma once

namespace mkwvk::memory_submit_trace {
enum Phase { maps, take, collect, retire, stamp, shadow, sweep, shadow_copy, publish, recycle, destroy, shadow_decommit, phase_count };

#ifdef _WIN32
struct Sample {
    uint64_t first = 0, last = 0, ticks = 0, cycles = 0, items = 0, bytes = 0;
    unsigned calls = 0;
};
struct Trace {
    uint64_t value = 0;
    uint64_t start = 0;
    uint64_t cycles = 0;
    Sample samples[phase_count]{};
};
inline thread_local Trace* current = nullptr;

struct Submit {
    Trace trace{};
    explicit Submit(uint64_t value) {
        static const bool enabled = capture_trace::file() && getenv("JINRA_TEST_SUBMIT_MEMORY");
        if (!enabled || current) return;
        trace.value = value;
        trace.start = capture_trace::qpc();
        trace.cycles = capture_trace::thread_cycles();
        current = &trace;
    }
    ~Submit() {
        if (!trace.start) return;
        current = nullptr;
        const uint64_t end = capture_trace::qpc();
        static const uint64_t threshold = [] {
            LARGE_INTEGER frequency{};
            QueryPerformanceFrequency(&frequency);
            return uint64_t(frequency.QuadPart) * 3 / 1000;
        }();
        if (end - trace.start < threshold) return;
        const auto thread = GetCurrentThreadId();
        const auto frame = capture_trace::frame();
        capture_trace::add({"submit_memory", trace.start, frame, thread, end, trace.value,
                            capture_trace::thread_cycles() - trace.cycles});
        static constexpr const char* names[] = {
            "mem_maps", "mem_take", "mem_collect", "mem_retire", "mem_stamp", "mem_shadow",
            "mem_sweep", "mem_shadow_copy", "mem_publish", "mem_recycle", "mem_destroy", "mem_shadow_decommit"
        };
        for (int i = 0; i < phase_count; ++i) {
            const auto& s = trace.samples[i];
            if (!s.calls) continue;
            capture_trace::add({names[i], s.first, frame, thread, s.last, trace.value,
                                s.ticks, s.cycles, s.items, s.bytes, s.calls});
        }
    }
};
struct Scope {
    Sample* sample;
    uint64_t begin = 0, cycles = 0, items = 0, bytes = 0;
    explicit Scope(Phase phase, uint64_t count = 0, uint64_t size = 0)
        : sample(current ? &current->samples[phase] : nullptr), items(count), bytes(size) {
        if (!sample) return;
        begin = capture_trace::qpc();
        cycles = capture_trace::thread_cycles();
    }
    void stop() {
        if (!sample) return;
        const uint64_t finish = capture_trace::qpc();
        const uint64_t delta = capture_trace::thread_cycles() - cycles;
        if (!sample->calls++) sample->first = begin;
        sample->last = finish;
        sample->ticks += finish - begin;
        sample->cycles += delta;
        sample->items += items;
        sample->bytes += bytes;
        sample = nullptr;
    }
    ~Scope() { stop(); }
};
#else
struct Submit { explicit Submit(uint64_t) {} };
struct Scope {
    uint64_t items = 0, bytes = 0;
    explicit Scope(Phase, uint64_t = 0, uint64_t = 0) {}
    void stop() {}
};
#endif
}
