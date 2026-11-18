#pragma once

#include <array>
#include <bit>
#include <cstdint>
#include <cstring>

// The caller serializes allocations, releases, the copies themselves and trimming
// with shadow_mu. Only blocks with zero live pages can be decommitted.
class TexturePageArena {
public:
    static constexpr size_t page_size = 4096;
    static constexpr size_t pages_per_block = 512;
    static constexpr size_t block_size = page_size * pages_per_block;
    static constexpr size_t block_count = 128;
    static constexpr size_t capacity = pages_per_block * block_count;

    TexturePageArena() = default;
    TexturePageArena(const TexturePageArena&) = delete;
    TexturePageArena& operator=(const TexturePageArena&) = delete;

    ~TexturePageArena() {
        if (base_) VirtualFree(base_, 0, MEM_RELEASE);
    }

    uint8_t* allocate() {
        if (!base_) {
            base_ = static_cast<uint8_t*>(VirtualAlloc(nullptr, capacity * page_size, MEM_RESERVE, PAGE_READWRITE));
            if (!base_) return nullptr;
        }
        // Fill blocks in use first so a burst of small allocations does not strand
        // live pages in otherwise empty blocks that could be returned to Windows.
        size_t index = first(partial_);
        if (index == block_count) index = first(empty_);
        if (index == block_count) index = first(reserved_);
        if (index == block_count) return nullptr;

        Block& block = blocks_[index];
        uint8_t* const start = base_ + index * block_size;
        if (!block.committed) {
            if (!VirtualAlloc(start, block_size, MEM_COMMIT, PAGE_READWRITE)) return nullptr;
            if (block.previously_committed) ++recommits_;
            ++commits_;
            ++committed_;
            block.committed = block.previously_committed = true;
            clear(reserved_, index);
        }
        uint16_t page = block.next_unused;
        if (block.free_head == no_page) {
            ++block.next_unused;
        } else {
            page = block.free_head;
            std::memcpy(&block.free_head, start + page * page_size, sizeof(block.free_head));
        }
        clear(empty_, index);
        if (++block.live == pages_per_block) clear(partial_, index);
        else set(partial_, index);
        return start + page * page_size;
    }

    bool release(uint8_t* page, uint64_t sequence) {
        const uintptr_t addr = reinterpret_cast<uintptr_t>(page);
        const uintptr_t start = reinterpret_cast<uintptr_t>(base_);
        if (!base_ || addr < start || addr - start >= capacity * page_size) return false;
        const size_t offset = addr - start;
        const size_t index = offset / block_size;
        Block& block = blocks_[index];
        std::memcpy(page, &block.free_head, sizeof(block.free_head));
        block.free_head = (offset % block_size) / page_size;
        if (--block.live) {
            set(partial_, index);
        } else {
            clear(partial_, index);
            set(empty_, index);
            block.empty_at = sequence;
        }
        return true;
    }

    void trim_one(uint64_t sequence) {
        Mask candidates = empty_;
        for (size_t index = first(candidates); index < block_count; index = first(candidates)) {
            clear(candidates, index);
            Block& block = blocks_[index];
            if (sequence - block.empty_at < 256) continue;
            if (VirtualFree(base_ + index * block_size, block_size, MEM_DECOMMIT)) {
                block = Block{};
                block.previously_committed = true;
                clear(empty_, index);
                set(reserved_, index);
                --committed_;
                ++decommits_;
            }
            return;
        }
    }

    unsigned committed() const { return committed_; }
    unsigned empty() const { return std::popcount(empty_[0]) + std::popcount(empty_[1]); }
    uint64_t commits() const { return commits_; }
    uint64_t decommits() const { return decommits_; }
    uint64_t recommits() const { return recommits_; }

private:
    static constexpr uint16_t no_page = UINT16_MAX;
    using Mask = std::array<uint64_t, 2>;
    struct Block {
        uint64_t empty_at = 0;
        uint16_t free_head = no_page;
        uint16_t next_unused = 0;
        uint16_t live = 0;
        bool committed = false;
        bool previously_committed = false;
    };
    static size_t first(const Mask& mask) {
        return mask[0] ? std::countr_zero(mask[0]) : 64 + std::countr_zero(mask[1]);
    }
    static void set(Mask& mask, size_t index) { mask[index / 64] |= uint64_t{1} << (index % 64); }
    static void clear(Mask& mask, size_t index) { mask[index / 64] &= ~(uint64_t{1} << (index % 64)); }

    std::array<Block, block_count> blocks_{};
    Mask partial_{}, empty_{}, reserved_{UINT64_MAX, UINT64_MAX};
    uint8_t* base_ = nullptr;
    uint64_t commits_ = 0, decommits_ = 0, recommits_ = 0;
    unsigned committed_ = 0;
};
