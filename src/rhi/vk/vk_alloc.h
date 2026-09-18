// Warren -- Vulkan device memory.
//
// Vulkan hands out memory in whole allocations and caps how many a
// process may hold -- 4096 on a lot of drivers, which a scene of any
// size passes before it finishes loading. So memory is taken in large
// blocks and cut up here.
//
// Deliberately a simple first-fit suballocator with per-memory-type
// pools rather than a dependency on VMA: it is three hundred lines, it
// is auditable, and the interface below is close enough to VMA's that
// swapping it in later is a change to this file alone.
#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "rhi/vk/vkfn.h"

namespace wr::vk {

struct Allocation {
    VkDeviceMemory memory = VK_NULL_HANDLE;
    VkDeviceSize offset = 0;
    VkDeviceSize size = 0;
    // Non-null for host-visible memory: the whole block is mapped once
    // and kept mapped, which is free on every driver worth the name and
    // removes a map/unmap pair from every upload.
    void *mapped = nullptr;
    uint32_t type_index = 0;
    int32_t block = -1;       // -1 marks a dedicated allocation
    bool valid() const { return memory != VK_NULL_HANDLE; }
};

enum class MemoryUsage : uint8_t {
    GpuOnly,      // DEVICE_LOCAL
    CpuToGpu,     // HOST_VISIBLE | HOST_COHERENT, device-local if offered
    GpuToCpu,     // HOST_VISIBLE | HOST_CACHED
};

class Allocator {
public:
    bool init(VkPhysicalDevice physical, VkDevice device,
              VkDeviceSize block_size = 64ull * 1024 * 1024);
    void shutdown();

    Allocation allocate(const VkMemoryRequirements &req, MemoryUsage usage,
                        bool dedicated = false);
    void free(const Allocation &a);

    // What is actually held, for a memory readout and a leak hunt.
    struct Stats {
        uint64_t reserved = 0;     // asked of the driver
        uint64_t used = 0;         // handed to resources
        uint32_t blocks = 0;
        uint32_t allocations = 0;
        uint32_t dedicated = 0;
    };
    Stats stats() const;
    std::string report() const;

    // Total device-local memory the adapter reports, for a caps line.
    uint64_t device_local_bytes() const;

private:
    struct Range {
        VkDeviceSize offset, size;
    };
    struct Block {
        VkDeviceMemory memory = VK_NULL_HANDLE;
        VkDeviceSize size = 0;
        void *mapped = nullptr;
        uint32_t type_index = 0;
        std::vector<Range> free;
        VkDeviceSize used = 0;
        uint32_t live = 0;
    };

    int find_memory_type(uint32_t type_bits, VkMemoryPropertyFlags required,
                         VkMemoryPropertyFlags preferred) const;
    int new_block(uint32_t type_index, VkDeviceSize size);

    VkPhysicalDevice physical_ = VK_NULL_HANDLE;
    VkDevice device_ = VK_NULL_HANDLE;
    VkPhysicalDeviceMemoryProperties props_{};
    VkDeviceSize block_size_ = 0;
    std::vector<Block> blocks_;
    uint32_t dedicated_count_ = 0;
    uint64_t dedicated_bytes_ = 0;
};

}  // namespace wr::vk
