// Manifold -- generational handle storage, shared by both backends.
#pragma once

#include <cstdint>
#include <vector>

#include "core/log.h"

namespace mf::rhi {

// A slot map. Handles carry a generation, so a handle to a destroyed
// resource is rejected instead of quietly addressing whatever was put
// in its place -- the difference between a logged error naming the
// resource and a driver crash several frames later with no clue in it.
template <class T, class H>
class HandlePool {
public:
    H create() {
        uint32_t index;
        if (!free_.empty()) {
            index = free_.back();
            free_.pop_back();
        } else {
            index = uint32_t(slots_.size());
            slots_.push_back({});
        }
        Slot &s = slots_[index];
        s.live = true;
        // Generation 0 means "never valid", so it is skipped on wrap.
        if (s.generation == 0) s.generation = 1;
        s.value = T();
        live_count_++;
        H h;
        h.index = index;
        h.generation = s.generation;
        return h;
    }

    T *get(H h) {
        if (!h.valid() || h.index >= slots_.size()) return nullptr;
        Slot &s = slots_[h.index];
        if (!s.live || s.generation != h.generation) return nullptr;
        return &s.value;
    }
    const T *get(H h) const {
        return const_cast<HandlePool *>(this)->get(h);
    }

    // Logs which resource kind was misused, which is most of the work
    // of finding a use-after-free.
    T *get_checked(H h, const char *what) {
        T *p = get(h);
        if (!p)
            MF_ERROR("rhi: %s handle %u/%u is not live", what, h.index,
                     h.generation);
        return p;
    }

    bool destroy(H h) {
        if (!h.valid() || h.index >= slots_.size()) return false;
        Slot &s = slots_[h.index];
        if (!s.live || s.generation != h.generation) return false;
        s.live = false;
        s.value = T();
        s.generation++;
        if (s.generation == 0) s.generation = 1;
        free_.push_back(h.index);
        live_count_--;
        return true;
    }

    template <class Fn>
    void for_each(Fn &&fn) {
        for (size_t i = 0; i < slots_.size(); i++)
            if (slots_[i].live) fn(slots_[i].value);
    }

    size_t live_count() const { return live_count_; }
    size_t capacity() const { return slots_.size(); }
    void clear() {
        slots_.clear();
        free_.clear();
        live_count_ = 0;
    }

private:
    struct Slot {
        T value{};
        uint32_t generation = 0;
        bool live = false;
    };
    std::vector<Slot> slots_;
    std::vector<uint32_t> free_;
    size_t live_count_ = 0;
};

}  // namespace mf::rhi
