#include "vk_alloc.h"

#include <algorithm>
#include <cstdio>

#include "core/log.h"

namespace wr::vk {

bool Allocator::init(VkPhysicalDevice physical, VkDevice device,
                     VkDeviceSize block_size) {
    physical_ = physical;
    device_ = device;
    block_size_ = block_size;
    vkGetPhysicalDeviceMemoryProperties(physical, &props_);
    return true;
}

void Allocator::shutdown() {
    for (Block &b : blocks_) {
        if (b.live)
            WR_WARN("vk: memory block of type %u freed with %u live allocations",
                    b.type_index, b.live);
        if (b.mapped) vkUnmapMemory(device_, b.memory);
        if (b.memory) vkFreeMemory(device_, b.memory, nullptr);
    }
    blocks_.clear();
}

uint64_t Allocator::device_local_bytes() const {
    uint64_t total = 0;
    for (uint32_t i = 0; i < props_.memoryHeapCount; i++)
        if (props_.memoryHeaps[i].flags & VK_MEMORY_HEAP_DEVICE_LOCAL_BIT)
            total += props_.memoryHeaps[i].size;
    return total;
}

// Prefer a type that has the nice-to-haves, settle for one that has the
// must-haves. On an integrated GPU every type is device local and host
// visible, so the preference costs nothing; on a discrete one it is the
// difference between uploads going over PCIe every frame and not.
int Allocator::find_memory_type(uint32_t type_bits, VkMemoryPropertyFlags required,
                                VkMemoryPropertyFlags preferred) const {
    for (int pass = 0; pass < 2; pass++) {
        VkMemoryPropertyFlags want = required | (pass == 0 ? preferred : 0);
        for (uint32_t i = 0; i < props_.memoryTypeCount; i++) {
            if (!(type_bits & (1u << i))) continue;
            if ((props_.memoryTypes[i].propertyFlags & want) == want) return int(i);
        }
    }
    return -1;
}

int Allocator::new_block(uint32_t type_index, VkDeviceSize size) {
    Block b;
    b.type_index = type_index;
    b.size = std::max(size, block_size_);
    VkMemoryAllocateInfo ai{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
    ai.allocationSize = b.size;
    ai.memoryTypeIndex = type_index;
    if (vkAllocateMemory(device_, &ai, nullptr, &b.memory) != VK_SUCCESS) {
        // Retry at the requested size: a 64MB block may not fit when a
        // 2MB one would.
        b.size = size;
        ai.allocationSize = size;
        if (vkAllocateMemory(device_, &ai, nullptr, &b.memory) != VK_SUCCESS) {
            WR_ERROR("vk: out of memory allocating %llu bytes of type %u",
                     (unsigned long long)size, type_index);
            return -1;
        }
    }
    if (props_.memoryTypes[type_index].propertyFlags &
        VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT) {
        if (vkMapMemory(device_, b.memory, 0, VK_WHOLE_SIZE, 0, &b.mapped) !=
            VK_SUCCESS)
            b.mapped = nullptr;
    }
    b.free.push_back({0, b.size});
    blocks_.push_back(b);
    return int(blocks_.size()) - 1;
}

Allocation Allocator::allocate(const VkMemoryRequirements &req, MemoryUsage usage,
                               bool dedicated) {
    VkMemoryPropertyFlags required = 0, preferred = 0;
    switch (usage) {
        case MemoryUsage::GpuOnly:
            required = VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT;
            break;
        case MemoryUsage::CpuToGpu:
            required = VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
                       VK_MEMORY_PROPERTY_HOST_COHERENT_BIT;
            preferred = VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT;
            break;
        case MemoryUsage::GpuToCpu:
            required = VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
                       VK_MEMORY_PROPERTY_HOST_COHERENT_BIT;
            preferred = VK_MEMORY_PROPERTY_HOST_CACHED_BIT;
            break;
    }
    int type = find_memory_type(req.memoryTypeBits, required, preferred);
    if (type < 0 && usage == MemoryUsage::GpuOnly)
        // A machine with no device-local type at all: use anything.
        type = find_memory_type(req.memoryTypeBits, 0, 0);
    if (type < 0) {
        WR_ERROR("vk: no memory type satisfies bits 0x%x", req.memoryTypeBits);
        return {};
    }

    // Anything bigger than a quarter of a block gets its own
    // allocation: packing it would strand the rest of the block.
    if (dedicated || req.size > block_size_ / 4) {
        VkMemoryAllocateInfo ai{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
        ai.allocationSize = req.size;
        ai.memoryTypeIndex = uint32_t(type);
        Allocation a;
        if (vkAllocateMemory(device_, &ai, nullptr, &a.memory) != VK_SUCCESS) {
            WR_ERROR("vk: dedicated allocation of %llu bytes failed",
                     (unsigned long long)req.size);
            return {};
        }
        a.offset = 0;
        a.size = req.size;
        a.type_index = uint32_t(type);
        a.block = -1;
        if (props_.memoryTypes[type].propertyFlags &
            VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT)
            vkMapMemory(device_, a.memory, 0, VK_WHOLE_SIZE, 0, &a.mapped);
        dedicated_count_++;
        dedicated_bytes_ += req.size;
        return a;
    }

    const VkDeviceSize align = std::max<VkDeviceSize>(req.alignment, 1);
    for (size_t bi = 0; bi < blocks_.size(); bi++) {
        Block &b = blocks_[bi];
        if (b.type_index != uint32_t(type)) continue;
        for (size_t ri = 0; ri < b.free.size(); ri++) {
            Range &r = b.free[ri];
            VkDeviceSize start = (r.offset + align - 1) / align * align;
            VkDeviceSize pad = start - r.offset;
            if (r.size < pad + req.size) continue;

            Allocation a;
            a.memory = b.memory;
            a.offset = start;
            a.size = req.size;
            a.type_index = b.type_index;
            a.block = int32_t(bi);
            a.mapped = b.mapped ? (char *)b.mapped + start : nullptr;

            // Split what is left, keeping the padding as its own free
            // range so alignment does not leak memory over a session.
            VkDeviceSize tail_offset = start + req.size;
            VkDeviceSize tail_size = r.offset + r.size - tail_offset;
            if (pad > 0) {
                r.size = pad;
                if (tail_size > 0) b.free.push_back({tail_offset, tail_size});
            } else if (tail_size > 0) {
                r.offset = tail_offset;
                r.size = tail_size;
            } else {
                b.free.erase(b.free.begin() + long(ri));
            }
            b.used += req.size;
            b.live++;
            return a;
        }
    }

    int bi = new_block(uint32_t(type), req.size + align);
    if (bi < 0) return {};
    return allocate(req, usage, false);
}

void Allocator::free(const Allocation &a) {
    if (!a.valid()) return;
    if (a.block < 0) {
        if (a.mapped) vkUnmapMemory(device_, a.memory);
        vkFreeMemory(device_, a.memory, nullptr);
        if (dedicated_count_) dedicated_count_--;
        dedicated_bytes_ -= std::min<uint64_t>(dedicated_bytes_, a.size);
        return;
    }
    Block &b = blocks_[size_t(a.block)];
    b.used -= std::min(b.used, a.size);
    if (b.live) b.live--;
    b.free.push_back({a.offset, a.size});

    // Coalesce. Without this a block fragments into thousands of
    // adjacent free ranges over a long session and allocation slows to
    // a crawl.
    std::sort(b.free.begin(), b.free.end(),
              [](const Range &x, const Range &y) { return x.offset < y.offset; });
    std::vector<Range> merged;
    merged.reserve(b.free.size());
    for (const Range &r : b.free) {
        if (!merged.empty() && merged.back().offset + merged.back().size == r.offset)
            merged.back().size += r.size;
        else
            merged.push_back(r);
    }
    b.free.swap(merged);
}

Allocator::Stats Allocator::stats() const {
    Stats s;
    for (const Block &b : blocks_) {
        s.reserved += b.size;
        s.used += b.used;
        s.allocations += b.live;
    }
    s.blocks = uint32_t(blocks_.size());
    s.reserved += dedicated_bytes_;
    s.used += dedicated_bytes_;
    s.dedicated = dedicated_count_;
    s.allocations += dedicated_count_;
    return s;
}

std::string Allocator::report() const {
    Stats s = stats();
    char b[256];
    std::snprintf(b, sizeof(b),
                  "vk memory: %.1f MB used of %.1f MB reserved in %u blocks, "
                  "%u allocations (%u dedicated)",
                  double(s.used) / 1048576.0, double(s.reserved) / 1048576.0,
                  s.blocks, s.allocations, s.dedicated);
    return b;
}

}  // namespace wr::vk
