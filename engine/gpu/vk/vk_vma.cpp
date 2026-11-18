// Vulkan Memory Allocator (v3.3.0, from Eglantine's CPM cache: nx2/build.py adds its include path): images and
// device buffers are sub-allocated from large blocks (NVIDIA allows ~4096 allocations, and the imported pools
// already take about 2400 of them). Its Vulkan functions come from vkGetInstanceProcAddr / vkGetDeviceProcAddr.
#define VMA_IMPLEMENTATION
#define VMA_STATIC_VULKAN_FUNCTIONS 0
#define VMA_DYNAMIC_VULKAN_FUNCTIONS 1
#include "vk_internal.h"
#include <vk_mem_alloc.h>
#include <algorithm>
#include <atomic>
#include <cstdlib>
#include <vector>
#include <cstring>
#include <mutex>
#include <unordered_map>
#ifdef _WIN32
#include <windows.h>
#endif

#ifndef JINRA_ENV_H   // (env_on: the switches' one rule)
#include "jinra_env.h"
#endif
#include "vram_compact.h"
#include "vk_work.h"   // (last: RX_VK_RECORDER=defer drains before the destructions and the invalidate below)

namespace mkwvk {

VmaAllocator vma;

namespace {
const char* const census_prefix = std::getenv("JINRA_TEST_MEMORY_CENSUS");
std::atomic<uint64_t> census_frame;
thread_local const char* device_image_pool;
struct DeviceBlock { const char* pool; uint64_t size; };
std::mutex census_block_mu;
std::unordered_map<VkDeviceMemory, DeviceBlock> census_blocks;

VKAPI_ATTR void VKAPI_CALL census_free(VkDevice device, VkDeviceMemory memory,
                                      const VkAllocationCallbacks* callbacks) {
    DeviceBlock block{};
    {
        std::lock_guard lock(census_block_mu);
        const auto it = census_blocks.find(memory);
        if (it != census_blocks.end()) {
            block = it->second;
            census_blocks.erase(it);
        }
    }
    const auto start = block.pool ? load_path_qpc() : 0;
    vkFreeMemory(device, memory, callbacks);
    if (block.pool) {
        logf("vk: census free %s frame %llu qpc %llu end %llu bytes %llu", block.pool,
             (unsigned long long)census_frame.load(), (unsigned long long)start,
             (unsigned long long)load_path_qpc(), (unsigned long long)block.size);
    }
}

VKAPI_ATTR VkResult VKAPI_CALL census_allocate(VkDevice device, const VkMemoryAllocateInfo* info,
                                              const VkAllocationCallbacks* callbacks, VkDeviceMemory* memory) {
    if (!device_image_pool) return vkAllocateMemory(device, info, callbacks, memory);
    const uint64_t begin = load_path_qpc();
    const VkResult result = vkAllocateMemory(device, info, callbacks, memory);
    const uint64_t end = load_path_qpc();
    if (result == VK_SUCCESS && (std::strcmp(device_image_pool, "render-targets") == 0 ||
                                 std::strcmp(device_image_pool, "render-targets-small") == 0)) {
        std::lock_guard lock(census_block_mu);
        census_blocks.emplace(*memory, DeviceBlock{device_image_pool, info->allocationSize});
    }
    logf("vk: census allocate %s frame %llu qpc %llu end %llu bytes %llu result %d", device_image_pool,
         (unsigned long long)census_frame.load(), (unsigned long long)begin, (unsigned long long)end,
         (unsigned long long)info->allocationSize, (int)result);
    return result;
}
}  // namespace

#ifdef _WIN32
namespace {

struct MappingEvent {
    uint64_t qpc;
    uint64_t memory;
    uint64_t address;
    uint64_t allocation_base;
    uint64_t offset;
    uint64_t bytes;
    uint32_t memory_type;
    char operation;
};

constexpr size_t mapping_event_capacity = 32768;
MappingEvent mapping_events[mapping_event_capacity]{};
std::atomic<bool> mapping_event_ready[mapping_event_capacity]{};
std::atomic<size_t> mapping_event_count{};
const char* mapping_output = nullptr;
PFN_vkUnmapMemory unmap_memory = nullptr;

void record_mapping(char operation, VkDeviceMemory memory, void* address, uint64_t offset,
                    uint64_t bytes, uint32_t memory_type, void* allocation_base = nullptr) {
    if (!mapping_output) return;
    const size_t index = mapping_event_count.fetch_add(1, std::memory_order_relaxed);
    if (index >= mapping_event_capacity) return;
    mapping_events[index] = {load_path_qpc(), (uint64_t)(uintptr_t)memory, (uint64_t)(uintptr_t)address,
                             (uint64_t)(uintptr_t)allocation_base, offset, bytes, memory_type, operation};
    mapping_event_ready[index].store(true, std::memory_order_release);
}

void save_mappings() {
    FILE* stream = std::fopen(mapping_output, "wb");
    if (!stream) return;
    std::fputs("qpc,op,memory,address,allocation_base,offset,bytes,memory_type\n", stream);
    const size_t total = mapping_event_count.load(std::memory_order_acquire);
    const size_t used = std::min(total, mapping_event_capacity);
    size_t incomplete = 0;
    for (size_t index = 0; index < used; ++index) {
        if (!mapping_event_ready[index].load(std::memory_order_acquire)) {
            ++incomplete;
            continue;
        }
        const auto& event = mapping_events[index];
        std::fprintf(stream, "%llu,%c,%llx,%llx,%llx,%llu,%llu,%u\n", (unsigned long long)event.qpc,
                     event.operation, (unsigned long long)event.memory, (unsigned long long)event.address,
                     (unsigned long long)event.allocation_base,
                     (unsigned long long)event.offset, (unsigned long long)event.bytes, event.memory_type);
    }
    std::fprintf(stream, "# lost=%llu\n", (unsigned long long)(total - used + incomplete));
    std::fclose(stream);
}

VKAPI_ATTR VkResult VKAPI_CALL mapped_allocate(VkDevice device, const VkMemoryAllocateInfo* info,
                                              const VkAllocationCallbacks* callbacks, VkDeviceMemory* memory) {
    const VkResult result = census_prefix ? census_allocate(device, info, callbacks, memory)
                                         : vkAllocateMemory(device, info, callbacks, memory);
    if (result == VK_SUCCESS)
        record_mapping('A', *memory, nullptr, 0, info->allocationSize, info->memoryTypeIndex);
    return result;
}

VKAPI_ATTR void VKAPI_CALL mapped_free(VkDevice device, VkDeviceMemory memory,
                                     const VkAllocationCallbacks* callbacks) {
    if (census_prefix) census_free(device, memory, callbacks);
    else vkFreeMemory(device, memory, callbacks);
    record_mapping('F', memory, nullptr, 0, 0, 0);
}

VKAPI_ATTR VkResult VKAPI_CALL mapped_map(VkDevice device, VkDeviceMemory memory, VkDeviceSize offset,
                                         VkDeviceSize bytes, VkMemoryMapFlags flags, void** data) {
    const VkResult result = vkMapMemory(device, memory, offset, bytes, flags, data);
    if (result == VK_SUCCESS) {
        MEMORY_BASIC_INFORMATION region{};
        VirtualQuery(*data, &region, sizeof region);
        record_mapping('M', memory, *data, offset, bytes, 0, region.AllocationBase);
    }
    return result;
}

VKAPI_ATTR void VKAPI_CALL mapped_unmap(VkDevice device, VkDeviceMemory memory) {
    unmap_memory(device, memory);
    record_mapping('U', memory, nullptr, 0, 0, 0);
}

void observe_vma_mappings(VmaVulkanFunctions& functions) {
    const char* path = std::getenv("JINRA_TEST_VMA_MAPPINGS");
    if (!path || !*path) return;
    unmap_memory = reinterpret_cast<PFN_vkUnmapMemory>(functions.vkGetDeviceProcAddr(dev.device, "vkUnmapMemory"));
    if (!unmap_memory || std::atexit(save_mappings) != 0) return;
    mapping_output = path;
    functions.vkAllocateMemory = mapped_allocate;
    functions.vkFreeMemory = mapped_free;
    functions.vkMapMemory = mapped_map;
    functions.vkUnmapMemory = mapped_unmap;
    for (uint32_t type = 0; type < dev.memory.memoryTypeCount; ++type) {
        const auto& properties = dev.memory.memoryTypes[type];
        logf("Jinra VMA mapping type=%u heap=%u flags=%u", type, properties.heapIndex, properties.propertyFlags);
    }
}

} // namespace
#endif

bool vma_init(PFN_vkGetInstanceProcAddr gipa) {
    VmaVulkanFunctions f{};
    f.vkGetInstanceProcAddr = gipa;
    f.vkGetDeviceProcAddr = vkGetDeviceProcAddr;
    if (census_prefix) {
        f.vkAllocateMemory = census_allocate;
        f.vkFreeMemory = census_free;
    }
#ifdef _WIN32
    observe_vma_mappings(f);
#endif
    VmaAllocatorCreateInfo ci{};
    ci.flags = VMA_ALLOCATOR_CREATE_BUFFER_DEVICE_ADDRESS_BIT |
               (dev.memory_budget ? VMA_ALLOCATOR_CREATE_EXT_MEMORY_BUDGET_BIT : 0);
    ci.physicalDevice = dev.physical;
    ci.device = dev.device;
    ci.instance = dev.instance;
    ci.vulkanApiVersion = VK_API_VERSION_1_3;
    ci.pVulkanFunctions = &f;
    ci.preferredLargeHeapBlockSize = 64ull << 20;   // (64 MB blocks: Eglantine found 256 MB ones wasted VRAM)
    return vmaCreateAllocator(&ci, &vma) == VK_SUCCESS;
}

// Images come from a pool of 128 MB blocks: on their own, VMA gave most of them dedicated memory (the driver prefers it
// for render targets, and anything over half a default block), a vkAllocateMemory and a kernel call for each, and
// the dynamic resolution makes a new set of render targets every few frames. Images too large for a block, or of a
// memory type the pool's is not, are allocated as before.
VmaPool image_pool = VK_NULL_HANDLE;
bool image_pool_tried = false;
// (RX_VK_IMAGE_BLOCK_MB=<n>, 16 and up: the block's size. VMA keeps one block empty for the next image, 128 MB of VRAM that a 64 MB block
// would halve, at twice the memory objects: the driver allows ~4096, the imported pools take ~2400)
const uint64_t kImageBlock = (getenv("RX_VK_IMAGE_BLOCK_MB") ? (uint64_t)std::max(16, atoi(getenv("RX_VK_IMAGE_BLOCK_MB"))) : 128u) << 20;

// and the render targets' images in blocks of their own (texture_get's for a target: OoT's transient ones, made and let go
// all the time). In the same blocks as the textures that stay, each one gone left a hole beside a texture that kept its block: the
// pool's 47-54 blocks (6-6.9 GB) held 1.9-3.6 GB of images in Kokiri Forest, VRAM sat at 93-98% of what the driver allows, and an
// image made there then took 30-70 ms (RTX 5070). RX_VK_TARGET_BLOCKS=0: in the textures' blocks, as before
VmaPool target_image_pool = VK_NULL_HANDLE;
VmaPool small_target_image_pool = VK_NULL_HANDLE;
uint32_t target_memory_type;
bool target_pool_tried = false;
VmaPool images_pool();
VmaPool target_images_pool() {
    if (target_pool_tried) return target_image_pool;
    target_pool_tried = true;
    static const bool off = getenv("RX_VK_TARGET_BLOCKS") && !atoi(getenv("RX_VK_TARGET_BLOCKS"));
    if (off || !images_pool()) return VK_NULL_HANDLE;
    VkImageCreateInfo rep{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};   // (the image pool's representative image: the same memory type)
    rep.imageType = VK_IMAGE_TYPE_2D;
    rep.format = VK_FORMAT_R8G8B8A8_UNORM;
    rep.extent = {256, 256, 1};
    rep.mipLevels = rep.arrayLayers = 1;
    rep.samples = VK_SAMPLE_COUNT_1_BIT;
    rep.usage = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;
    VmaAllocationCreateInfo ai{};
    ai.usage = VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE;
    ai.requiredFlags = VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT;
    uint32_t type = 0;
    if (vmaFindMemoryTypeIndexForImageInfo(vma, &rep, &ai, &type) != VK_SUCCESS) return VK_NULL_HANDLE;
    VmaPoolCreateInfo pi{};
    pi.memoryTypeIndex = type;
    pi.blockSize = kImageBlock;
    if (vmaCreatePool(vma, &pi, &target_image_pool) != VK_SUCCESS) target_image_pool = VK_NULL_HANDLE;
    if (census_prefix && target_image_pool) vmaSetPoolName(vma, target_image_pool, "render-targets");
    if (target_image_pool && env_on("RX_VK_RT_SMALL_POOL")) {
        pi.blockSize = 16ull << 20;
        if (vmaCreatePool(vma, &pi, &small_target_image_pool) == VK_SUCCESS) {
            target_memory_type = type;
            if (census_prefix) vmaSetPoolName(vma, small_target_image_pool, "render-targets-small");
            logf("vk: small render targets use 16 MiB blocks, memory type %u", type);
        }
    }
    logf("vk: render targets' images from blocks of their own (%llu MB, memory type %u)%s", (unsigned long long)(kImageBlock >> 20),
         type, target_image_pool ? "" : ": not made");
    return target_image_pool;
}

// (RX 7800 XT: part of a pool's blocks made at the start, not when the game first needs them. Each is a vkAllocateMemory of
// 128 MB, ~170 ms on AMD's driver, on the replay thread: ~70 of them in a walk across Hyrule Field, the frame rate dropping to the low
// 40s whenever new ground came in. In the pool the sampled textures are made in and no other, 15% of the card's memory, 2 to 24 blocks:
// vram_compact::blocks_at_start. RX_VK_IMAGE_PREALLOC=<blocks>: that many, 0: none)
static size_t blocks_at_start(bool holds_textures) {
    if (!holds_textures) return 0;
    uint64_t heap = 0;
    for (uint32_t h = 0; h < dev.memory.memoryHeapCount; h++)
        if (dev.memory.memoryHeaps[h].flags & VK_MEMORY_HEAP_DEVICE_LOCAL_BIT) heap = std::max<uint64_t>(heap, dev.memory.memoryHeaps[h].size);
    if (const char* e = getenv("RX_VK_IMAGE_PREALLOC")) return (size_t)std::max(0, atoi(e));
    return (size_t)vram_compact::blocks_at_start(heap, kImageBlock, holds_textures);
}
/// The pool with n blocks made now, or empty when there is no room for them (its blocks made as needed); the blocks made
static size_t make_pool(VmaPoolCreateInfo pi, size_t n, VmaPool& pool) {
    pi.minBlockCount = n;
    if (n && vmaCreatePool(vma, &pi, &pool) == VK_SUCCESS) return n;
    pi.minBlockCount = 0;
    if (vmaCreatePool(vma, &pi, &pool) != VK_SUCCESS) pool = VK_NULL_HANDLE;
    return 0;
}

VmaPool images_pool() {
    if (image_pool_tried) return image_pool;
    image_pool_tried = true;
    VkImageCreateInfo rep{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};   // (a representative image: the pool's memory type)
    rep.imageType = VK_IMAGE_TYPE_2D;
    rep.format = VK_FORMAT_R8G8B8A8_UNORM;
    rep.extent = {256, 256, 1};
    rep.mipLevels = rep.arrayLayers = 1;
    rep.samples = VK_SAMPLE_COUNT_1_BIT;
    rep.usage = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;
    VmaAllocationCreateInfo ai{};
    ai.usage = VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE;
    ai.requiredFlags = VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT;
    uint32_t type = 0;
    if (vmaFindMemoryTypeIndexForImageInfo(vma, &rep, &ai, &type) != VK_SUCCESS) return VK_NULL_HANDLE;
    VmaPoolCreateInfo pi{};
    pi.memoryTypeIndex = type;
    pi.blockSize = kImageBlock;
    const size_t n = make_pool(pi, blocks_at_start(!vram_compact::enabled(getenv("RX_VK_COMPACT"))), image_pool);
    logf("vk: images from a pool of %llu MB blocks (memory type %u), %zu made at the start%s", (unsigned long long)(kImageBlock >> 20), type,
         n, image_pool ? "" : ": not made");
    return image_pool;
}

// The sampled textures' own pool, when they are compacted (unless RX_VK_COMPACT=0): VMA's defragmentation asks to move any
// allocation of the pool it works on, and only vk_texture.cpp's images can be moved (it knows what they hold and who uses
// them) or kept from being freed while a pass is open. The render targets' images stay in their blocks above; DLSS's, the
// frame interpolation's and the scratch ones in the images' pool
VmaPool texture_pool = VK_NULL_HANDLE;
bool texture_pool_tried = false;
uint64_t texture_pool_kept = 0;   // (its blocks made at the start, which VMA keeps: bytes)
VmaPool textures_pool() {
    if (texture_pool_tried) return texture_pool;
    texture_pool_tried = true;
    VkImageCreateInfo rep{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};   // (the image pool's representative image: the same memory type)
    rep.imageType = VK_IMAGE_TYPE_2D;
    rep.format = VK_FORMAT_R8G8B8A8_UNORM;
    rep.extent = {256, 256, 1};
    rep.mipLevels = rep.arrayLayers = 1;
    rep.samples = VK_SAMPLE_COUNT_1_BIT;
    rep.usage = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;
    VmaAllocationCreateInfo ai{};
    ai.usage = VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE;
    ai.requiredFlags = VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT;
    uint32_t type = 0;
    if (vmaFindMemoryTypeIndexForImageInfo(vma, &rep, &ai, &type) != VK_SUCCESS) return VK_NULL_HANDLE;
    VmaPoolCreateInfo pi{};
    pi.memoryTypeIndex = type;
    pi.blockSize = kImageBlock;
    texture_pool_kept = make_pool(pi, blocks_at_start(true), texture_pool) * kImageBlock;
    logf("vk: textures from a pool of their own, %llu MB blocks (RX_VK_COMPACT), %llu made at the start%s",
         (unsigned long long)(kImageBlock >> 20), (unsigned long long)(texture_pool_kept / kImageBlock), texture_pool ? "" : ": not made");
    return texture_pool;
}
bool textures_pool_usage(uint64_t* block_bytes, uint64_t* free_bytes, uint64_t* kept_bytes) {
    if (!texture_pool) return false;
    VmaStatistics st{};
    vmaGetPoolStatistics(vma, texture_pool, &st);
    *block_bytes = st.blockBytes, *free_bytes = st.blockBytes - std::min(st.blockBytes, st.allocationBytes);
    if (kept_bytes) *kept_bytes = texture_pool_kept;
    return true;
}

// ---- the textures' pool compacted: VMA's defragmentation context and its open pass ----
namespace {
VmaDefragmentationContext compact_ctx = VK_NULL_HANDLE;
VmaDefragmentationPassMoveInfo compact_pass{};
bool compact_in_pass = false;
bool compact_over(uint64_t* freed_bytes, uint32_t* freed_blocks) {
    VmaDefragmentationStats st{};
    vmaEndDefragmentation(vma, compact_ctx, &st);
    compact_ctx = VK_NULL_HANDLE;
    if (freed_bytes) *freed_bytes = st.bytesFreed;
    if (freed_blocks) *freed_blocks = st.deviceMemoryBlocksFreed;
    return false;
}
}
bool image_compact_begin(uint64_t max_bytes_a_pass, uint32_t max_moves_a_pass) {
    if (compact_ctx || !texture_pool) return false;
    VmaDefragmentationInfo di{};
    di.pool = texture_pool;
    di.maxBytesPerPass = max_bytes_a_pass;
    di.maxAllocationsPerPass = max_moves_a_pass;
    return vmaBeginDefragmentation(vma, &di, &compact_ctx) == VK_SUCCESS;
}
bool image_compact_active() { return compact_ctx != VK_NULL_HANDLE; }
bool image_compact_pass(std::vector<ImageMove>& moves, uint64_t* freed_bytes, uint32_t* freed_blocks) {
    moves.clear();
    if (!compact_ctx || compact_in_pass) return false;
    const VkResult r = vmaBeginDefragmentationPass(vma, compact_ctx, &compact_pass);
    if (r != VK_INCOMPLETE) return compact_over(freed_bytes, freed_blocks);   // (VK_SUCCESS: nothing left to move; or an error)
    compact_in_pass = true;
    for (uint32_t i = 0; i < compact_pass.moveCount; i++) moves.push_back({compact_pass.pMoves[i].srcAllocation});
    return true;
}
bool image_compact_bind(size_t i, VkImage image) {
    return compact_in_pass && i < compact_pass.moveCount &&
           vmaBindImageMemory(vma, compact_pass.pMoves[i].dstTmpAllocation, image) == VK_SUCCESS;
}
void image_compact_ignore(size_t i) {
    if (compact_in_pass && i < compact_pass.moveCount)
        compact_pass.pMoves[i].operation = VMA_DEFRAGMENTATION_MOVE_OPERATION_IGNORE;
}
bool image_compact_pass_end(uint64_t* freed_bytes, uint32_t* freed_blocks) {
    if (!compact_ctx || !compact_in_pass) return false;
    compact_in_pass = false;
    if (vmaEndDefragmentationPass(vma, compact_ctx, &compact_pass) == VK_INCOMPLETE) return true;
    return compact_over(freed_bytes, freed_blocks);
}

bool create_image(const VkImageCreateInfo& ci, VkImage& image, VmaAllocation& allocation, bool dedicated, bool target, bool textures) {
    VmaAllocationCreateInfo ai{};
    ai.usage = VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE;
    // (in VRAM or not at all: an image VMA puts in system memory when VRAM is full is read over PCIe every frame,
    // and takes the RAM the game and everything else on the machine need)
    ai.requiredFlags = VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT;
    if (dev.memory_budget) ai.flags = VMA_ALLOCATION_CREATE_WITHIN_BUDGET_BIT;
    if (dedicated) {   // (a memory object of its own, which freeing gives back whole: a pool's block is kept by VMA when it empties)
        VmaAllocationCreateInfo dai = ai;
        dai.flags |= VMA_ALLOCATION_CREATE_DEDICATED_MEMORY_BIT;
        if (vmaCreateImage(vma, &ci, &dai, &image, &allocation, nullptr) == VK_SUCCESS) return true;
    }
    static const bool no_pool = env_on("RX_VK_NO_IMAGE_POOL");
    if (VmaPool pool = no_pool ? VK_NULL_HANDLE
                       : textures && textures_pool() ? textures_pool()
                       : target && target_images_pool() ? target_images_pool() : images_pool()) {
        VmaAllocationCreateInfo pai = ai;
        pai.pool = pool;
        pai.usage = VMA_MEMORY_USAGE_UNKNOWN;
        if (pool == target_image_pool && small_target_image_pool && !dedicated && !ci.pNext &&
            ci.imageType == VK_IMAGE_TYPE_2D &&
            !(ci.flags & (VK_IMAGE_CREATE_SPARSE_BINDING_BIT | VK_IMAGE_CREATE_DISJOINT_BIT)) &&
            uint64_t(ci.extent.width) * ci.extent.height < (2ull << 20)) {
            static const auto requirements = reinterpret_cast<PFN_vkGetDeviceImageMemoryRequirements>(
                vkGetDeviceProcAddr(dev.device, "vkGetDeviceImageMemoryRequirements"));
            if (requirements) {
                VkDeviceImageMemoryRequirements query{VK_STRUCTURE_TYPE_DEVICE_IMAGE_MEMORY_REQUIREMENTS};
                query.pCreateInfo = &ci;
                VkMemoryDedicatedRequirements dedicated_req{VK_STRUCTURE_TYPE_MEMORY_DEDICATED_REQUIREMENTS};
                VkMemoryRequirements2 req{VK_STRUCTURE_TYPE_MEMORY_REQUIREMENTS_2, &dedicated_req};
                requirements(dev.device, &query, &req);
                if (req.memoryRequirements.size < (2ull << 20) && !dedicated_req.requiresDedicatedAllocation &&
                    (req.memoryRequirements.memoryTypeBits & (1u << target_memory_type))) {
                    pai.pool = small_target_image_pool;
                    if (census_prefix) device_image_pool = "render-targets-small";
                    const auto result = vmaCreateImage(vma, &ci, &pai, &image, &allocation, nullptr);
                    device_image_pool = nullptr;
                    if (result == VK_SUCCESS) return true;
                    pai.pool = pool;
                }
            }
        }
        if (census_prefix) {
            device_image_pool = pool == target_image_pool ? "render-targets" : pool == texture_pool ? "textures" : "images";
        }
        const VkResult result = vmaCreateImage(vma, &ci, &pai, &image, &allocation, nullptr);
        device_image_pool = nullptr;
        if (result == VK_SUCCESS) return true;
    }
    if (vmaCreateImage(vma, &ci, &ai, &image, &allocation, nullptr) == VK_SUCCESS) return true;
    if (ai.flags & VMA_ALLOCATION_CREATE_WITHIN_BUDGET_BIT) {
        static std::atomic<int> told_budget{0};
        if (told_budget++ < 10) logf("vk: image over budget, retrying without WITHIN_BUDGET");
        VmaAllocationCreateInfo vai = ai;
        vai.flags &= ~VMA_ALLOCATION_CREATE_WITHIN_BUDGET_BIT;
        if (vmaCreateImage(vma, &ci, &vai, &image, &allocation, nullptr) == VK_SUCCESS) return true;
    }
    // VRAM is full (a 4 GB card at a high resolution, #96): an image left unmade is a black screen, so it goes
    // to system memory instead, slow but drawn
    if (env_on("RX_VK_IMAGES_VRAM_ONLY")) return false;
    VmaAllocationCreateInfo sai{};
    sai.usage = VMA_MEMORY_USAGE_AUTO;
    if (vmaCreateImage(vma, &ci, &sai, &image, &allocation, nullptr) != VK_SUCCESS) return false;
    static std::atomic<int> told{0};
    if (told++ < 4) logf("vk: VRAM full: a %ux%u image (format %d) in system memory", ci.extent.width, ci.extent.height, (int)ci.format);
    return true;
}
void destroy_image(VkImage image, VmaAllocation allocation) {
    vmaDestroyImage(vma, image, allocation);
}
#ifdef _WIN32
void jinra_capture_block_log(const char* label, char status, VmaAllocation allocation, uint64_t bytes,
                             uint64_t age, uint64_t highest_fence, uint64_t completed_fence) {
    VmaAllocationInfo2 info{};
    vmaGetAllocationInfo2(vma, allocation, &info);
    const auto& basic = info.allocationInfo;
    logf("Jinra capture block %s class=%c bytes=%llu age=%llu fence=%llu completed=%llu "
         "memory=%llx offset=%llu block=%llu type=%u dedicated=%u",
         label, status, (unsigned long long)bytes, (unsigned long long)age,
         (unsigned long long)highest_fence, (unsigned long long)completed_fence,
         (unsigned long long)(uintptr_t)basic.deviceMemory, (unsigned long long)basic.offset,
         (unsigned long long)info.blockSize, basic.memoryType, (unsigned)info.dedicatedMemory);
}
#endif
uint64_t allocation_size(VmaAllocation allocation) {
    VmaAllocationInfo info{};
    if (allocation) vmaGetAllocationInfo(vma, allocation, &info);
    return info.size;
}
bool create_buffer(uint64_t size, VkBufferUsageFlags usage, bool host_visible, VkBuffer& buffer,
                   VmaAllocation& allocation, void** mapped, bool dedicated) {
    VkBufferCreateInfo bi{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
    bi.size = size;
    bi.usage = usage;
    VmaAllocationCreateInfo ai{};
    ai.usage = VMA_MEMORY_USAGE_AUTO;
    if (host_visible) ai.flags = VMA_ALLOCATION_CREATE_HOST_ACCESS_SEQUENTIAL_WRITE_BIT | VMA_ALLOCATION_CREATE_MAPPED_BIT;
    // (a buffer with VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT gets VK_MEMORY_ALLOCATE_DEVICE_ADDRESS_BIT from VMA:
    // vma_init sets VMA_ALLOCATOR_CREATE_BUFFER_DEVICE_ADDRESS_BIT)
    static const bool blocks = env_on("RX_VK_BLOCK_BUFFERS");
    if (dedicated && !blocks) ai.flags |= VMA_ALLOCATION_CREATE_DEDICATED_MEMORY_BIT;
    VmaAllocationInfo info{};
    if (vmaCreateBuffer(vma, &bi, &ai, &buffer, &allocation, &info) != VK_SUCCESS) return false;
    if (mapped) *mapped = info.pMappedData;
    return true;
}
/// A buffer in cached host memory, mapped: written on one thread, read back on the CPU too (the captures: the game's
/// threads copy pages into it, the replay reads texture pages out of it and the GPU copies the rest from it)
bool create_buffer_cached(uint64_t size, VkBufferUsageFlags usage, VkBuffer& buffer, VmaAllocation& allocation,
                          void** mapped, bool dedicated) {
    VkBufferCreateInfo bi{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
    bi.size = size;
    bi.usage = usage;
    VmaAllocationCreateInfo ai{};
    ai.usage = VMA_MEMORY_USAGE_AUTO_PREFER_HOST;
    ai.flags = VMA_ALLOCATION_CREATE_HOST_ACCESS_RANDOM_BIT | VMA_ALLOCATION_CREATE_MAPPED_BIT;
    if (dedicated) ai.flags |= VMA_ALLOCATION_CREATE_DEDICATED_MEMORY_BIT;   // (its own memory object: no part of a 64 MB block left unused)
    VmaAllocationInfo info{};
    if (vmaCreateBuffer(vma, &bi, &ai, &buffer, &allocation, &info) != VK_SUCCESS) return false;
    if (mapped) *mapped = info.pMappedData;
    return true;
}
bool host_cached(VmaAllocation allocation) {
    VkMemoryPropertyFlags f = 0;
    if (allocation) vmaGetAllocationMemoryProperties(vma, allocation, &f);
    return (f & VK_MEMORY_PROPERTY_HOST_CACHED_BIT) != 0;
}
void flush_mapped(VmaAllocation allocation, uint64_t size) {
    if (size) vmaFlushAllocation(vma, allocation, 0, size);
}
void flush_mapped_range(VmaAllocation allocation, uint64_t offset, uint64_t size) {
    if (size) vmaFlushAllocation(vma, allocation, offset, size);
}
void invalidate_mapped(VmaAllocation allocation, uint64_t size) {
    if (size) vmaInvalidateAllocation(vma, allocation, 0, size);
}
bool flush_mapped_checked(VmaAllocation allocation, uint64_t size) {
    return !size || vmaFlushAllocation(vma, allocation, 0, size) == VK_SUCCESS;
}
bool invalidate_mapped_checked(VmaAllocation allocation, uint64_t size) {
    return !size || vmaInvalidateAllocation(vma, allocation, 0, size) == VK_SUCCESS;
}
void destroy_buffer(VkBuffer buffer, VmaAllocation allocation) {
    vmaDestroyBuffer(vma, buffer, allocation);
}
bool create_device_buffer(uint64_t size, VkBufferUsageFlags usage, VkBuffer& buffer, VmaAllocation& allocation, bool dedicated) {
    VkBufferCreateInfo bi{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
    bi.size = size;
    bi.usage = usage;
    VmaAllocationCreateInfo ai{};
    ai.usage = VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE;
    ai.requiredFlags = VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT;
    if (dev.memory_budget) ai.flags = VMA_ALLOCATION_CREATE_WITHIN_BUDGET_BIT;
    if (dedicated) ai.flags |= VMA_ALLOCATION_CREATE_DEDICATED_MEMORY_BIT;
    return vmaCreateBuffer(vma, &bi, &ai, &buffer, &allocation, nullptr) == VK_SUCCESS;
}
/// (every heap: what VMA has in blocks and allocations there, what the process uses of it, its budget; one line)
/// The image pools: their blocks, the images in them, and the free ranges between them (every 600 presents, with the heaps)
void vma_log_image_blocks() {
    for (VmaPool pool : {image_pool, target_image_pool, texture_pool}) {
        if (!pool) continue;
        VmaDetailedStatistics d{};
        vmaCalculatePoolStatistics(vma, pool, &d);
        logf("vk: image blocks%s: %u blocks %llu MB, %u images %llu MB; %u free ranges, the largest %llu MB",
             pool == target_image_pool ? " (render targets)" : pool == texture_pool ? " (textures, compacted)" : "", d.statistics.blockCount,
             (unsigned long long)(d.statistics.blockBytes >> 20), d.statistics.allocationCount,
             (unsigned long long)(d.statistics.allocationBytes >> 20), d.unusedRangeCount,
             (unsigned long long)(d.unusedRangeSizeMax >> 20));
    }
}
void diagnostic_memory_census(uint64_t frame) {
    if (!census_prefix || !vma) return;
    census_frame = frame;
    if (frame % 30) return;
    const std::string request = std::string(census_prefix) + ".request";
    FILE* marker = std::fopen(request.c_str(), "r");
    if (!marker) return;
    std::fclose(marker);
    if (std::remove(request.c_str())) return;

    const uint64_t begin = load_path_qpc();
    const std::string file = std::string(census_prefix) + "-" + std::to_string(frame) + ".json";
    if (FILE* output = std::fopen(file.c_str(), "wb")) {
        char* stats = nullptr;
        vmaBuildStatsString(vma, &stats, VK_TRUE);
        std::fputs(stats, output);
        vmaFreeStatsString(vma, stats);
        std::fclose(output);
    }
#ifdef _WIN32
    struct HeapTotals { DWORD size; SIZE_T allocated, committed, reserved, limit; };
    using Summary = BOOL(WINAPI*)(HANDLE, DWORD, HeapTotals*);
    static const auto summarize = reinterpret_cast<Summary>(GetProcAddress(GetModuleHandleW(L"kernel32.dll"),
                                                                         "HeapSummary"));
    if (summarize) {
        HANDLE heaps[256];
        const DWORD count = GetProcessHeaps(256, heaps);
        if (count && count <= 256) {
            const HANDLE process_heap = GetProcessHeap();
            for (DWORD i = 0; i < count; ++i) {
                HeapTotals totals{sizeof(HeapTotals), 0, 0, 0, 0};
                if (!summarize(heaps[i], 0, &totals)) {
                    logf("vk: census heap failed frame %llu id %p error %lu", (unsigned long long)frame,
                         heaps[i], (unsigned long)GetLastError());
                    continue;
                }
                logf("vk: census heap frame %llu id %p main %u allocated %llu committed %llu reserved %llu",
                     (unsigned long long)frame, heaps[i], heaps[i] == process_heap ? 1u : 0u,
                     (unsigned long long)totals.allocated, (unsigned long long)totals.committed,
                     (unsigned long long)totals.reserved);
            }
        } else {
            logf("vk: census heap count unavailable frame %llu count %lu", (unsigned long long)frame, (unsigned long)count);
        }
    }
#endif
    uint64_t usage = 0, budget = 0;
    const bool known = vram_usage_now(&usage, &budget);
    VmaBudget heaps[VK_MAX_MEMORY_HEAPS]{};
    vmaGetHeapBudgets(vma, heaps);
    for (uint32_t h = 0; h < dev.memory.memoryHeapCount; ++h) {
        logf("vk: census heap-budget frame %llu heap %u local %u usage %llu blocks %llu allocations %llu",
             (unsigned long long)frame, h, (dev.memory.memoryHeaps[h].flags & VK_MEMORY_HEAP_DEVICE_LOCAL_BIT) ? 1u : 0u,
             (unsigned long long)heaps[h].usage, (unsigned long long)heaps[h].statistics.blockBytes,
             (unsigned long long)heaps[h].statistics.allocationBytes);
    }
    logf("vk: census sample frame %llu qpc %llu end %llu usage %llu budget %llu known %u",
         (unsigned long long)frame, (unsigned long long)begin, (unsigned long long)load_path_qpc(),
         (unsigned long long)usage, (unsigned long long)budget, known ? 1u : 0u);
}

void vma_log_heaps() {
    VmaBudget b[VK_MAX_MEMORY_HEAPS]{};
    vmaGetHeapBudgets(vma, b);
    char line[1024];
    int n = snprintf(line, sizeof line, "vk: memory heaps:");
    for (uint32_t h = 0; h < dev.memory.memoryHeapCount && n < 900; h++)
        n += snprintf(line + n, sizeof line - n, " heap %u%s: %u blocks %llu MB, %u allocations %llu MB; used %llu of %llu MB;", h,
                      (dev.memory.memoryHeaps[h].flags & VK_MEMORY_HEAP_DEVICE_LOCAL_BIT) ? " (device)" : " (system)",
                      b[h].statistics.blockCount, (unsigned long long)(b[h].statistics.blockBytes >> 20),
                      b[h].statistics.allocationCount, (unsigned long long)(b[h].statistics.allocationBytes >> 20),
                      (unsigned long long)(b[h].usage >> 20), (unsigned long long)(b[h].budget >> 20));
    logf("%s", line);
}
bool vram_usage(uint64_t* used, uint64_t* budget, uint64_t* block_free) {
    VmaBudget b[VK_MAX_MEMORY_HEAPS]{};
    vmaGetHeapBudgets(vma, b);
    uint64_t u = 0, t = 0, f = 0;
    for (uint32_t h = 0; h < dev.memory.memoryHeapCount; h++)
        if (dev.memory.memoryHeaps[h].flags & VK_MEMORY_HEAP_DEVICE_LOCAL_BIT) {
            u += b[h].usage, t += b[h].budget;
            // (the free space in VMA's blocks: the driver counts it as the process's, new allocations take it first)
            f += b[h].statistics.blockBytes - std::min(b[h].statistics.blockBytes, b[h].statistics.allocationBytes);
        }
    // (a test: RX_VK_TEST_ALLOWED_MB=<n> reports what the driver allows as at most n MB, so a 16 GB card plays the eviction and
    // keep-rendered paths an 8 GB card takes when its VRAM fills (the rainbow textures of issue 993 were never seen here otherwise))
    static const uint64_t test_allowed = getenv("RX_VK_TEST_ALLOWED_MB") ? (uint64_t)atoi(getenv("RX_VK_TEST_ALLOWED_MB")) << 20 : 0;
    if (test_allowed && t > test_allowed) t = test_allowed;
    *used = u, *budget = t;
    if (block_free) *block_free = f;
    return t != 0;
}

/// The system-memory heaps the GPU reaches: what the process uses there (the imported guest memory too: pinned by the
/// driver) and what the driver allows it, asked now (a new frame index makes VMA ask the driver again); false when unknown
namespace {
std::atomic<uint32_t> budget_frame{0};   // (VMA's frame index: a new one makes it ask the driver for the budgets again)
}
bool vram_usage_now(uint64_t* used, uint64_t* budget) {
    vmaSetCurrentFrameIndex(vma, ++budget_frame);
    return vram_usage(used, budget) && dev.memory_budget;
}
bool system_heap_usage(uint64_t* used, uint64_t* budget) {
    vmaSetCurrentFrameIndex(vma, ++budget_frame);
    VmaBudget b[VK_MAX_MEMORY_HEAPS]{};
    vmaGetHeapBudgets(vma, b);
    uint64_t u = 0, t = 0;
    for (uint32_t h = 0; h < dev.memory.memoryHeapCount; h++)
        if (!(dev.memory.memoryHeaps[h].flags & VK_MEMORY_HEAP_DEVICE_LOCAL_BIT)) u += b[h].usage, t += b[h].budget;
    *used = u, *budget = t;
    return dev.memory_budget && t != 0;
}

} // namespace mkwvk
