#pragma once

// These probes measure the existing wait. They never substitute for it.
#include "cowait_trace.h"
#ifdef _WIN32
namespace mkwvk::capture_trace {

class SemaphoreProbe {
    struct Request {
        uint64_t serial = 0, id = 0, target = 0, qpc = 0, cpu = 0, excluded = 0;
        VkSemaphore semaphore = VK_NULL_HANDLE;
    };

    HANDLE event_ = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    HANDLE timer_ = CreateWaitableTimerExW(nullptr, nullptr, 0x00000002, TIMER_ALL_ACCESS);
    std::mutex request_lock_;
    std::mutex lease_;
    Request request_;
    std::atomic<uint64_t> live_{0};
    uint64_t next_ = 0;
    uint64_t hz_ = 0;
    int mode_ = 0;
    bool high_resolution_ = timer_ != nullptr;
    bool idle_ = env_on("RX_VK_CPU_IDLE_LOG");
    bool config_ = env_on("RX_THREAD_CONFIG_LOG");

    void run() {
        SetThreadDescription(GetCurrentThread(), L"Jinra semaphore probe");
        uint64_t last_config = 0;
        for (;;) {
            if (WaitForSingleObject(event_, INFINITE) != WAIT_OBJECT_0) return;
            Request req;
            {
                std::lock_guard lock(request_lock_);
                req = request_;
            }
            const uint64_t step = hz_ / 4000;
            uint64_t due = req.qpc;
            bool cycles_sampled = false;
            for (unsigned sample = 0; sample < 400; ++sample) {
                if (live_.load(std::memory_order_acquire) != req.serial) break;
                const uint64_t start = qpc();
                const DWORD cpu = GetCurrentProcessorNumber();
                uint64_t value = 0;
                const VkResult result = mode_ == 1 ? vkGetSemaphoreCounterValue(dev.device, req.semaphore, &value) : VK_SUCCESS;
                const uint64_t end = qpc();
                add({"probe", start, 0, GetCurrentThreadId(), req.id, req.target, value, end, due, cpu,
                     uint64_t(result == VK_SUCCESS), uint64_t(mode_), uint64_t(high_resolution_)});
                if (result != VK_SUCCESS) break;
                if (idle_) sample_idle(req.id, req.cpu, req.excluded);
                if (idle_ && start - req.qpc > hz_ / 500 && !cycles_sampled) {
                    snapshot_threads(req.id, false);
                    cycles_sampled = true;
                }
                due += step;
                if (due <= end) due = end + step;
                const uint64_t now = qpc();
                const uint64_t remainder = due > now ? due - now : 1;
                LARGE_INTEGER delay{};
                delay.QuadPart = -static_cast<int64_t>(1 + (remainder * 10000000ull) / hz_);
                if (!SetWaitableTimer(timer_, &delay, 0, nullptr, nullptr, FALSE) ||
                    WaitForSingleObject(timer_, INFINITE) != WAIT_OBJECT_0) return;
            }
            const uint64_t now = qpc();
            if (config_ && (!last_config || now - last_config >= hz_)) {
                last_config = now;
                snapshot_threads(0, true);
            }
        }
    }

public:
    explicit SemaphoreProbe(int mode) : mode_(mode) {
        LARGE_INTEGER frequency{};
        QueryPerformanceFrequency(&frequency);
        hz_ = uint64_t(frequency.QuadPart);
        if (!timer_) timer_ = CreateWaitableTimerW(nullptr, FALSE, nullptr);
        if (event_ && timer_ && hz_ >= 4000) std::thread([this] { run(); }).detach();
    }

    bool arm(uint64_t id, VkSemaphore semaphore, uint64_t target, uint64_t cpu, uint64_t excluded) {
        if (!event_ || !timer_ || hz_ < 4000 || !lease_.try_lock()) return false;
        {
            std::lock_guard lock(request_lock_);
            request_ = {++next_, id, target, qpc(), cpu, excluded, semaphore};
            live_.store(next_, std::memory_order_release);
        }
        if (SetEvent(event_)) return true;
        disarm();
        return false;
    }

    void disarm() {
        live_.store(0, std::memory_order_release);
        lease_.unlock();
    }
};

struct ProbeLease {
    SemaphoreProbe* probe = nullptr;
    ProbeLease(Wait* diagnostic, VkSemaphore semaphore, uint64_t target) {
        if (!diagnostic) return;
        static SemaphoreProbe* service = []() -> SemaphoreProbe* {
            const char* mode = getenv("RX_VK_WAIT_PROBE");
            const int value = mode ? atoi(mode) : 0;
            return value == 1 || value == 2 ? new SemaphoreProbe(value) : nullptr;
        }();
        static const bool enabled = env_on("RX_VK_COWAIT");
        diagnostic->arm = enabled ? wait_arm(diagnostic->id) : (service ? 1 : 0);
        if (!diagnostic->arm && !env_on("RX_THREAD_CONFIG_LOG")) return;
        const uint64_t waiting_cpu = cpu();
        GROUP_AFFINITY affinity{};
        GetThreadGroupAffinity(GetCurrentThread(), &affinity);
        const uint64_t pair = host && host->reserved_cpus ? host->reserved_cpus() : 0;
        const uint64_t excluded = (pair & (uint64_t(1) << (waiting_cpu % 64))) ? pair
                                  : uint64_t(1) << (waiting_cpu % 64);
        add({"wait_config", qpc(), frame(), GetCurrentThreadId(), diagnostic->id, affinity.Mask,
             affinity.Group, waiting_cpu, uint64_t(GetThreadPriority(GetCurrentThread()) + 32), excluded});
        if (diagnostic->arm && service && service->arm(diagnostic->id, semaphore, target, waiting_cpu, excluded)) probe = service;
        if (diagnostic->arm == 2) {
            static auto* cowait = new CoWait;
            diagnostic->cowait = cowait->arm({diagnostic->id, target, waiting_cpu, excluded, semaphore});
        }
    }
    ~ProbeLease() { if (probe) probe->disarm(); }
    ProbeLease(const ProbeLease&) = delete;
    ProbeLease& operator=(const ProbeLease&) = delete;
};

}
#endif



