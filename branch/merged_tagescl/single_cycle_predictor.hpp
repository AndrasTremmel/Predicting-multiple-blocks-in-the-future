#pragma once
#include <cstdint>
#include <vector>
#include <cstddef>

class SingleCyclePredictor {
public:
    explicit SingleCyclePredictor(std::size_t size = 1024, std::size_t assoc = 4)
        : SIZE(size),
          ASSOC(assoc),
          NUM_SETS(size / assoc),
          sets(NUM_SETS, std::vector<<Entry>(ASSOC)),
          lru(NUM_SETS, std::vector<int>(ASSOC, 0)) {}

    // Return true => predicted taken, false => predicted not-taken
    bool predict(uint64_t ip) {
        size_t set = index(ip);
        for (size_t way = 0; way < ASSOC; ++way) {
            if (sets[set][way].valid && sets[set][way].pc == ip) {
                return sets[set][way].counter >= 2;
            }
        }
        return false;  // miss → not-taken (or change to true if you prefer)
    }

    // Saturating-counter update
    void update(uint64_t ip, bool taken) {
        size_t set = index(ip);
        // Hit → update counter and LRU
        for (size_t way = 0; way < ASSOC; ++way) {
            if (sets[set][way].valid && sets[set][way].pc == ip) {
                update_counter(sets[set][way].counter, taken);
                touch(set, way);
                return;
            }
        }
        // Miss → allocate
        size_t way = find_victim(set);
        auto& e = sets[set][way];
        e.pc = ip;
        e.counter = taken ? 3 : 0;  // strong initial state
        e.valid = true;
        touch(set, way);
    }

private:
    struct Entry {
        uint64_t pc = 0;
        uint8_t counter = 2;   // 2-bit saturating counter
        bool valid = false;
    };

    size_t SIZE;
    size_t ASSOC;
    size_t NUM_SETS;
    std::vector<std::vector<<Entry>> sets;
    std::vector<std::vector<int>> lru;

    size_t index(uint64_t ip) const {
        return (ip >> 2) % NUM_SETS;
    }

    void update_counter(uint8_t& cnt, bool taken) {
        if (taken) {
            if (cnt < 3) ++cnt;
        } else {
            if (cnt > 0) --cnt;
        }
    }

    void touch(size_t set, size_t way) {
        for (size_t i = 0; i < ASSOC; ++i) {
            if (lru[set][i] < lru[set][way])
                ++lru[set][i];
        }
        lru[set][way] = 0;
    }

    size_t find_victim(size_t set) {
        for (size_t way = 0; way < ASSOC; ++way) {
            if (!sets[set][way].valid)
                return way;
        }
        size_t victim = 0;
        int max_lru = -1;
        for (size_t way = 0; way < ASSOC; ++way) {
            if (lru[set][way] > max_lru) {
                max_lru = lru[set][way];
                victim = way;
            }
        }
        return victim;
    }
};