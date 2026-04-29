#pragma once
#include <cstdint>
#include <vector>
#include <cassert>

struct L0BTBEntry {
    uint64_t pc = 0;
    uint64_t target = 0;

    int counter = 0;          // 2-bit saturating counter (0–3)

    int use_bm_ctr = 0;       // 3-bit confidence counter (0-7)
    bool use_bm = false;

    int use_gshare_ctr = 0;   // (optional)
    bool use_gshare = false;

    bool valid = false;
};



class L0BTB {
public:
    L0BTB(size_t size, size_t assoc)
        : SIZE(size),
          ASSOC(assoc),
          NUM_SETS(size / assoc),
          sets(NUM_SETS, std::vector<L0BTBEntry>(ASSOC)),
          lru(NUM_SETS, std::vector<int>(ASSOC, 0)) {}

    struct Result {
        bool hit;
        L0BTBEntry* entry;
        size_t set;
        size_t way;
    };

    // -----------------------------
    // Probe (read-only)
    // -----------------------------
    Result probe(uint64_t pc) {
        size_t set = index(pc);
        for (size_t way = 0; way < ASSOC; way++) {
            if (sets[set][way].valid && sets[set][way].pc == pc) {
                return {true, &sets[set][way], set, way};
            }
        }
        return {false, nullptr, set, 0};
    }

    // -----------------------------
    // Access (for update)
    // -----------------------------
    Result access(uint64_t pc) {
        auto res = probe(pc);
        if (res.hit) {
            touch(res.set, res.way);
        }
        return res;
    }

    // -----------------------------
    // Insert
    // -----------------------------
    void insert(uint64_t pc, uint64_t target, int counter_init = 2) {
        size_t set = index(pc);

        size_t victim = find_victim(set);

        auto& e = sets[set][victim];
        e.pc = pc;
        e.target = target;
        e.counter = counter_init;
        e.use_bm_ctr = 0;
        e.use_bm = false;
        e.use_gshare_ctr = 0;
        e.use_gshare = false;
        e.valid = true;

        touch(set, victim);
    }

private:
    size_t SIZE;
    size_t ASSOC;
    size_t NUM_SETS;

    std::vector<std::vector<L0BTBEntry>> sets;
    std::vector<std::vector<int>> lru;

    size_t index(uint64_t pc) const {
        return (pc >> 2) % NUM_SETS;
    }

    void touch(size_t set, size_t way) {
        for (size_t i = 0; i < ASSOC; i++) {
            if (lru[set][i] < lru[set][way])
                lru[set][i]++;
        }
        lru[set][way] = 0;
    }

    size_t find_victim(size_t set) {
        for (size_t way = 0; way < ASSOC; way++) {
            if (!sets[set][way].valid)
                return way;
        }

        size_t victim = 0;
        int max_lru = -1;

        for (size_t way = 0; way < ASSOC; way++) {
            if (lru[set][way] > max_lru) {
                max_lru = lru[set][way];
                victim = way;
            }
        }

        return victim;
    }
};
