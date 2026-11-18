// Record when large private address ranges first appear, without touching their pages.
#ifdef _WIN32
void jinra_startup_arenas(const char* phase) {
    static const bool trace = std::getenv("JINRA_TEST_STARTUP_ARENAS") != nullptr;
    if (!trace) return;
    SYSTEM_INFO system{};
    GetSystemInfo(&system);
    const auto last = reinterpret_cast<uintptr_t>(system.lpMaximumApplicationAddress);
    uintptr_t next = reinterpret_cast<uintptr_t>(system.lpMinimumApplicationAddress);
    struct Region {
        uintptr_t base = 0;
        size_t reserved = 0;
        size_t committed = 0;
        bool private_pages = false;
    } region;
    const auto print = [&] {
        if (region.base && region.private_pages && region.reserved >= (512ull << 20))
            logf("Jinra startup arena phase=%s base=%016llx reserve=%llu commit=%llu", phase,
                 (unsigned long long)region.base, (unsigned long long)region.reserved,
                 (unsigned long long)region.committed);
    };
    while (next <= last) {
        MEMORY_BASIC_INFORMATION entry{};
        if (VirtualQuery(reinterpret_cast<void*>(next), &entry, sizeof entry) != sizeof entry) break;
        const auto base = reinterpret_cast<uintptr_t>(entry.AllocationBase);
        if (base != region.base || entry.State == MEM_FREE) {
            print();
            region = Region{base};
        }
        if (entry.State != MEM_FREE) {
            region.reserved += entry.RegionSize;
            if (entry.Type == MEM_PRIVATE) region.private_pages = true;
            if (entry.State == MEM_COMMIT) region.committed += entry.RegionSize;
        }
        const auto end = reinterpret_cast<uintptr_t>(entry.BaseAddress) + entry.RegionSize;
        if (end <= next) break;
        next = end;
    }
    print();
}
#endif
