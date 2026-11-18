#pragma once

#ifdef _WIN32
#include <tlhelp32.h>

namespace mkwvk::capture_trace {

inline uint64_t cpu() {
    PROCESSOR_NUMBER number{};
    GetCurrentProcessorNumberEx(&number);
    return uint64_t(number.Group) * 64 + number.Number;
}

inline unsigned wait_arm(uint64_t id) {
    // Stable per-wait assignment. Half untouched, a quarter polled, a quarter polled with a second wait.
    id = (id ^ (id >> 30)) * 0xbf58476d1ce4e5b9ull;
    id = (id ^ (id >> 27)) * 0x94d049bb133111ebull;
    const unsigned sample = (id ^ (id >> 31)) & 3;
    return sample < 2 ? 0 : sample - 1;
}

struct CoRequest {
    uint64_t id = 0, target = 0, cpu = 0;
    KAFFINITY excluded = 0;
    VkSemaphore semaphore = VK_NULL_HANDLE;
};

class CoWait {
    HANDLE event_ = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    std::mutex mutex_;
    CoRequest request_;
    bool busy_ = false;

    void run() {
        SetThreadDescription(GetCurrentThread(), L"Jinra Vulkan co-wait");
        while (WaitForSingleObject(event_, INFINITE) == WAIT_OBJECT_0) {
            CoRequest req;
            {
                std::lock_guard lock(mutex_);
                req = request_;
            }
            DWORD_PTR mask = 0, system = 0;
            GetProcessAffinityMask(GetCurrentProcess(), &mask, &system);
            GROUP_AFFINITY affinity{};
            affinity.Group = WORD(req.cpu / 64);
            affinity.Mask = mask & ~req.excluded;
            const bool pinned = affinity.Mask && SetThreadGroupAffinity(GetCurrentThread(), &affinity, nullptr);
            const uint64_t start = qpc();
            uint64_t end = start;
            const uint64_t from = cpu();
            VkResult result = VK_ERROR_INITIALIZATION_FAILED;
            if (pinned && !(req.excluded & (KAFFINITY(1) << (from % 64)))) {
                VkSemaphoreWaitInfo info{VK_STRUCTURE_TYPE_SEMAPHORE_WAIT_INFO};
                info.semaphoreCount = 1;
                info.pSemaphores = &req.semaphore;
                info.pValues = &req.target;
                result = vkWaitSemaphores(dev.device, &info, 200000000ull);
                end = qpc();
            }
            add({"cowait", start, 0, GetCurrentThreadId(), req.id, req.target, end, from, cpu(),
                 uint64_t(result == VK_SUCCESS), uint64_t(pinned), req.excluded});
            std::lock_guard lock(mutex_);
            busy_ = false;
        }
    }

public:
    CoWait() { if (event_) std::thread([this] { run(); }).detach(); }

    bool arm(CoRequest request) {
        std::lock_guard lock(mutex_);
        if (!event_ || busy_) return false;
        request_ = request;
        busy_ = SetEvent(event_);
        return busy_;
    }
};

inline uint64_t snapshot_threads(uint64_t capture, bool config) {
    const uint64_t start = qpc();
    const HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
    if (snapshot == INVALID_HANDLE_VALUE) return 0;
    THREADENTRY32 entry{};
    entry.dwSize = sizeof entry;
    const DWORD pid = GetCurrentProcessId();
    if (Thread32First(snapshot, &entry)) do {
        if (entry.th32OwnerProcessID != pid) continue;
        const HANDLE thread = OpenThread(THREAD_QUERY_INFORMATION, FALSE, entry.th32ThreadID);
        if (!thread) continue;
        ULONG64 cycles = 0;
        if (QueryThreadCycleTime(thread, &cycles))
            add({"thread_cycles", qpc(), 0, entry.th32ThreadID, capture, cycles});
        if (config) {
            GROUP_AFFINITY group{};
            PROCESSOR_NUMBER ideal{};
            BOOL disabled = FALSE;
            ULONG ids[64]{}, count = 0;
            const BOOL have_group = GetThreadGroupAffinity(thread, &group);
            const BOOL have_ideal = GetThreadIdealProcessorEx(thread, &ideal);
            const BOOL have_boost = GetThreadPriorityBoost(thread, &disabled);
            using ReadCpuSets = BOOL (WINAPI*)(HANDLE, PULONG, ULONG, PULONG);
            static const auto read_sets = reinterpret_cast<ReadCpuSets>(
                GetProcAddress(GetModuleHandleW(L"kernel32.dll"), "GetThreadSelectedCpuSets"));
            const BOOL have_sets = read_sets && read_sets(thread, ids, 64, &count);
            const int priority = GetThreadPriority(thread);
            add({"thread_config", qpc(), 0, entry.th32ThreadID, group.Mask, group.Group,
                 uint64_t(ideal.Group) * 64 + ideal.Number, uint64_t(priority + 32), uint64_t(disabled),
                 uint64_t(have_sets ? count : 0), uint64_t(have_group | (have_ideal << 1) | (have_boost << 2) | (have_sets << 3))});
            if (have_sets) for (ULONG n = 0; n < count; ++n)
                add({"thread_cpuset", qpc(), 0, entry.th32ThreadID, ids[n]});
            PWSTR name = nullptr;
            if (SUCCEEDED(GetThreadDescription(thread, &name)) && name) {
                // A separate record per word preserves the name in the numeric ring.
                for (uint64_t n = 0; name[n] && n < 100; ++n)
                    add({"thread_name", qpc(), 0, entry.th32ThreadID, n, uint64_t(name[n])});
                LocalFree(name);
            }
        }
        CloseHandle(thread);
    } while (Thread32Next(snapshot, &entry));
    CloseHandle(snapshot);
    return qpc() - start;
}

inline void sample_idle(uint64_t capture, uint64_t waiting_cpu, uint64_t mask) {
    ULONG64 cycles[64]{};
    ULONG bytes = sizeof cycles;
    const uint64_t start = qpc();
    if (!QueryIdleProcessorCycleTimeEx(USHORT(waiting_cpu / 64), &bytes, cycles)) return;
    const uint64_t end = qpc();
    for (unsigned n = 0; n < bytes / sizeof cycles[0]; ++n) {
        if (!(mask & (uint64_t(1) << n))) continue;
        add({"cpu_idle", start, 0, GetCurrentThreadId(), capture, waiting_cpu / 64 * 64 + n, cycles[n], end});
    }
}

struct FrameCpu {
    const char* kind;
    uint64_t start = 0, cycles = 0, first_cpu = 0;
    explicit FrameCpu(const char* label) : kind(label) {
        static const bool enabled = file() && env_on("RX_THREAD_CONFIG_LOG");
        if (enabled) {
            start = qpc();
            cycles = thread_cycles();
            first_cpu = cpu();
        }
    }
    ~FrameCpu() {
        if (start) add({kind, start, frame(), GetCurrentThreadId(), qpc(), thread_cycles() - cycles, first_cpu, cpu()});
    }
};

}
#endif


