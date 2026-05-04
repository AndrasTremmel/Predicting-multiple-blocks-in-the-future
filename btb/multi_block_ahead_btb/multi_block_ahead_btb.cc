#include <cstdint>
#include <map>
#include <array>
#include <bitset>
#include <deque>
#include <algorithm>
#include <utility>
#include <cstdlib>
#include <iostream>

#include "msl/lru_table.h"
#include "ooo_cpu.h"

// Uncomment the following line to enable BTB end-of-simulation debug statistics
#define BTB_DEBUG_STATS




namespace
{

enum class branch_info {
  INDIRECT,
  RETURN,
  ALWAYS_TAKEN,
  CONDITIONAL,
};

// ---- MBTB EXTENSION ----
enum class mbtb_transition : uint8_t {
  T,
  N,
  R
};

struct mbtb_entry_t {
  uint64_t ip_tag = 0;        // Aa (previous block branch)
  uint64_t target = 0;        // Ci (future block target)
  branch_info type = branch_info::ALWAYS_TAKEN;

  mbtb_transition transition = mbtb_transition::N;

  auto index() const { return ip_tag >> 2; }
  auto tag() const { return (ip_tag >> 2) ^ static_cast<uint64_t>(transition); }
};

// ---- STORAGE ----
constexpr std::size_t BTB_SET = 2048;
constexpr std::size_t BTB_WAY = 4;
constexpr std::size_t BTB_INDIRECT_SIZE = 4096;
constexpr std::size_t RAS_SIZE = 64;
constexpr std::size_t SAS_SIZE = 64;
constexpr std::size_t CALL_SIZE_TRACKERS = 1024;

// MBTB replaces BTB
std::map<O3_CPU*, champsim::msl::lru_table<mbtb_entry_t>> MBTB;

std::map<O3_CPU*, std::array<uint64_t, BTB_INDIRECT_SIZE>> INDIRECT_BTB;
std::map<O3_CPU*, std::bitset<champsim::lg2(BTB_INDIRECT_SIZE)>> CONDITIONAL_HISTORY;

std::map<O3_CPU*, std::deque<uint64_t>> RAS;
std::map<O3_CPU*, std::deque<uint64_t>> SAS;

std::map<O3_CPU*, std::array<uint64_t, CALL_SIZE_TRACKERS>> CALL_SIZE;

// ---- Track previous block branch (Aa) ----
std::map<O3_CPU*, uint64_t> LAST_BRANCH_IP;
std::map<O3_CPU*, mbtb_transition> LAST_TRANSITION;

#ifdef BTB_DEBUG_STATS
struct MBTBStats {
    uint64_t total_lookups = 0;
    uint64_t total_misses = 0;
    uint64_t return_hits = 0;
    uint64_t indirect_hits = 0;
    uint64_t conditional_hits = 0;
    uint64_t always_taken_hits = 0;
};

std::map<O3_CPU*, MBTBStats> MBTB_STATS;

struct MBTBStatsPrinter {
    ~MBTBStatsPrinter() {
        for (const auto& pair : MBTB_STATS) {
            const auto& s = pair.second;
            std::cerr << "\n========== MULTI-BLOCK BTB DEBUG STATISTICS ==========\n";
            std::cerr << "Total lookups:         " << s.total_lookups << "\n";
            std::cerr << "Total misses:          " << s.total_misses << "\n";
            if (s.total_lookups > 0)
                std::cerr << "Miss rate:             " << (100.0 * s.total_misses / s.total_lookups) << "%\n";
            std::cerr << "Return hits:           " << s.return_hits << "\n";
            std::cerr << "Indirect hits:         " << s.indirect_hits << "\n";
            std::cerr << "Conditional hits:      " << s.conditional_hits << "\n";
            std::cerr << "Always taken hits:     " << s.always_taken_hits << "\n";
            std::cerr << "=====================================================\n";
        }
    }
};
static MBTBStatsPrinter mbtb_stats_printer;
#endif

} // namespace


void O3_CPU::initialize_btb()
{
  ::MBTB.insert({this, champsim::msl::lru_table<mbtb_entry_t>{BTB_SET, BTB_WAY}});

  std::fill(std::begin(::INDIRECT_BTB[this]), std::end(::INDIRECT_BTB[this]), 0);
  std::fill(std::begin(::CALL_SIZE[this]), std::end(::CALL_SIZE[this]), 4);

  ::CONDITIONAL_HISTORY[this] = 0;
  ::LAST_BRANCH_IP[this] = 0;
  ::LAST_TRANSITION[this] = mbtb_transition::N;
}


std::pair<uint64_t, uint8_t> O3_CPU::btb_prediction(uint64_t ip)
{
  auto prev_ip = ::LAST_BRANCH_IP[this];
  auto trans   = ::LAST_TRANSITION[this];

  auto entry = ::MBTB.at(this).check_hit({prev_ip, 0, branch_info::ALWAYS_TAKEN, trans});

#ifdef BTB_DEBUG_STATS
  auto& s = ::MBTB_STATS[this];
  s.total_lookups++;
  if (!entry.has_value()) {
    s.total_misses++;
  } else {
    if (entry->type == branch_info::RETURN) s.return_hits++;
    else if (entry->type == branch_info::INDIRECT) s.indirect_hits++;
    else if (entry->type == branch_info::CONDITIONAL) s.conditional_hits++;
    else if (entry->type == branch_info::ALWAYS_TAKEN) s.always_taken_hits++;
  }
#endif

  if (!entry.has_value())
    return {0, false};

  // ---- RETURN handling via RAS ----
  if (entry->type == branch_info::RETURN) {
    if (std::empty(::RAS[this]))
      return {0, true};

    auto target = ::RAS[this].back();
    auto size = ::CALL_SIZE[this][target % std::size(::CALL_SIZE[this])];
    return {target + size, true};
  }

  // ---- INDIRECT ----
  if (entry->type == branch_info::INDIRECT) {
    auto hash = (ip >> 2) ^ ::CONDITIONAL_HISTORY[this].to_ullong();
    return {::INDIRECT_BTB[this][hash % std::size(::INDIRECT_BTB[this])], true};
  }

  return {entry->target, entry->type != branch_info::CONDITIONAL};
}




void O3_CPU::update_btb(uint64_t ip, uint64_t branch_target, uint8_t taken, uint8_t branch_type)
{
  // ---- CALL handling ----
  if (branch_type == BRANCH_DIRECT_CALL || branch_type == BRANCH_INDIRECT_CALL) {
    ::RAS[this].push_back(ip);
    ::SAS[this].push_back(ip);

    if (std::size(::RAS[this]) > RAS_SIZE)
      ::RAS[this].pop_front();

    if (std::size(::SAS[this]) > SAS_SIZE)
      ::SAS[this].pop_front();
  }

  // ---- INDIRECT ----
  if ((branch_type == BRANCH_INDIRECT) || (branch_type == BRANCH_INDIRECT_CALL)) {
    auto hash = (ip >> 2) ^ ::CONDITIONAL_HISTORY[this].to_ullong();
    ::INDIRECT_BTB[this][hash % std::size(::INDIRECT_BTB[this])] = branch_target;
  }


  // ---- CONDITIONAL HISTORY ----
  if ((branch_type == BRANCH_CONDITIONAL) || (branch_type == BRANCH_OTHER)) {
    ::CONDITIONAL_HISTORY[this] <<= 1;
    ::CONDITIONAL_HISTORY[this].set(0, taken);
  }

  // ---- RETURN ----
  if (branch_type == BRANCH_RETURN && !std::empty(::RAS[this])) {
    auto call_ip = ::RAS[this].back();
    ::RAS[this].pop_back();

    if (!std::empty(::SAS[this]))
      ::SAS[this].pop_back();

    auto estimated_call_instr_size = (call_ip > branch_target) ? call_ip - branch_target : branch_target - call_ip;
    if (estimated_call_instr_size <= 10) {
      ::CALL_SIZE[this][call_ip % std::size(::CALL_SIZE[this])] = estimated_call_instr_size;
    }
  }

 




  // ---- Determine type ----
  auto type = branch_info::ALWAYS_TAKEN;

  if ((branch_type == BRANCH_INDIRECT) || (branch_type == BRANCH_INDIRECT_CALL))
    type = branch_info::INDIRECT;
  else if (branch_type == BRANCH_RETURN)
    type = branch_info::RETURN;
  else if ((branch_type == BRANCH_CONDITIONAL) || (branch_type == BRANCH_OTHER))
    type = branch_info::CONDITIONAL;

  // ---- Transition ----
  mbtb_transition trans;

  // R: previous instruction is a CALL
  if (branch_type == BRANCH_DIRECT_CALL || branch_type == BRANCH_INDIRECT_CALL) {
    trans = mbtb_transition::R;
  }
  // T: taken branch that is NOT a call/return
  else if (taken &&
    branch_type != BRANCH_RETURN &&
    branch_type != BRANCH_DIRECT_CALL &&
    branch_type != BRANCH_INDIRECT_CALL) {
    trans = mbtb_transition::T;
  }
  // N: everything else (non-branch OR not-taken conditional)
  else {
    trans = mbtb_transition::N;
  }

  
  // ---- Update MBTB using PREVIOUS branch ----
  auto prev_ip = ::LAST_BRANCH_IP[this];
  auto prev_trans = ::LAST_TRANSITION[this];

  auto opt_entry = ::MBTB.at(this).check_hit({prev_ip, branch_target, type, prev_trans});

  if (opt_entry.has_value()) {
    if (branch_target != 0) {
      opt_entry->target = branch_target;
    }
    opt_entry->type = type;
  }

  if (branch_target != 0) {
    ::MBTB.at(this).fill(
        opt_entry.value_or(mbtb_entry_t{prev_ip, branch_target, type, prev_trans})
    );
  }

  // ---- Update LAST (Aa tracking) ----
  ::LAST_BRANCH_IP[this] = ip;
  ::LAST_TRANSITION[this] = trans;
}




// #include <cstdint>
// #include <map>
// #include <array>
// #include <vector>
// #include <bitset>
// #include <deque>
// #include <algorithm>
// #include <utility>
// #include <cstdlib>
// #include <iostream>

// #include "msl/lru_table.h"
// #include "ooo_cpu.h"

// // Uncomment to enable end-of-simulation debug stats
// #define BTB_DEBUG_STATS


// namespace
// {

// enum class branch_info {
//   INDIRECT,
//   RETURN,
//   ALWAYS_TAKEN,
//   CONDITIONAL,
// };

// enum class mbtb_transition : uint8_t {
//   T,
//   N,
//   R
// };

// // =============================================================
// // Successor-PC hash. Used by the consumption side of the buffer
// // to pick the right candidate among multiple matching ways.
// // 16 bits gives ~1/65k random-collision probability.
// // =============================================================
// inline uint16_t pc_hash(uint64_t pc) {
//   uint64_t x = pc >> 2;
//   x ^= x >> 16;
//   x ^= x >> 32;
//   return static_cast<uint16_t>(x);
// }

// // =============================================================
// // Hand-rolled set-associative MBTB (replaces champsim::msl::lru_table
// // because we need multi-way readout at lookup time -- the lru_table
// // API only returns one entry per check_hit).
// //
// // Index = (prev_pc >> 2) % BTB_SET
// // Within a set, multiple ways can store the same (prev_pc, transition)
// // with DIFFERENT succ_pc_hash values, so two distinct successors B
// // and B' of the same predecessor A no longer evict each other.
// // =============================================================
// struct mbtb_way_t {
//   bool             valid = false;
//   uint64_t         prev_pc = 0;
//   mbtb_transition  transition = mbtb_transition::N;
//   uint16_t         succ_pc_hash = 0;
//   uint64_t         target = 0;
//   branch_info      type = branch_info::ALWAYS_TAKEN;
//   uint64_t         lru_ts = 0;     // higher = more recently used
// };

// constexpr std::size_t BTB_SET = 2048;
// constexpr std::size_t BTB_WAY = 4;
// constexpr std::size_t BTB_INDIRECT_SIZE = 4096;
// constexpr std::size_t RAS_SIZE = 64;
// constexpr std::size_t SAS_SIZE = 64;
// constexpr std::size_t CALL_SIZE_TRACKERS = 1024;

// using mbtb_set_t = std::array<mbtb_way_t, BTB_WAY>;
// using mbtb_t     = std::vector<mbtb_set_t>;

// std::map<O3_CPU*, mbtb_t> MBTB;
// std::map<O3_CPU*, uint64_t> MBTB_TIME;   // monotonic clock for LRU stamps

// // Substructures: unchanged from your original
// std::map<O3_CPU*, std::array<uint64_t, BTB_INDIRECT_SIZE>> INDIRECT_BTB;
// std::map<O3_CPU*, std::bitset<champsim::lg2(BTB_INDIRECT_SIZE)>> CONDITIONAL_HISTORY;
// std::map<O3_CPU*, std::deque<uint64_t>> RAS;
// std::map<O3_CPU*, std::deque<uint64_t>> SAS;
// std::map<O3_CPU*, std::array<uint64_t, CALL_SIZE_TRACKERS>> CALL_SIZE;
// std::map<O3_CPU*, uint64_t> LAST_BRANCH_IP;
// std::map<O3_CPU*, mbtb_transition> LAST_TRANSITION;

// // =============================================================
// // DEFERRED-PREDICTION BUFFER -- now MULTI-WAY.
// // At lookup time we copy ALL matching ways from the set into
// // this buffer. At consumption time the next btb_prediction()
// // call scans these candidates for one whose succ_pc_hash
// // matches pc_hash(actual_ip).
// // =============================================================
// struct buffered_candidate_t {
//   bool         valid = false;
//   uint16_t     expected_succ_pc_hash = 0;
//   uint64_t     target = 0;
//   branch_info  type = branch_info::ALWAYS_TAKEN;
// };

// struct pred_buffer_t {
//   std::array<buffered_candidate_t, BTB_WAY> candidates;
//   uint8_t num_candidates = 0;
// };
// std::map<O3_CPU*, pred_buffer_t> PRED_BUFFER;

// // =============================================================
// // Helpers
// // =============================================================
// inline std::size_t set_index_of(uint64_t prev_pc) {
//   return (prev_pc >> 2) % BTB_SET;
// }

// // Find the way in this set matching (prev_pc, transition, succ_pc_hash).
// // Returns BTB_WAY (an invalid index) if no match.
// inline std::size_t find_way_exact(const mbtb_set_t& set,
//                                   uint64_t prev_pc,
//                                   mbtb_transition trans,
//                                   uint16_t succ_hash)
// {
//   for (std::size_t w = 0; w < BTB_WAY; ++w) {
//     const auto& e = set[w];
//     if (e.valid && e.prev_pc == prev_pc && e.transition == trans
//                 && e.succ_pc_hash == succ_hash) {
//       return w;
//     }
//   }
//   return BTB_WAY;
// }

// // Find an invalid (free) slot in the set. Returns BTB_WAY if none.
// inline std::size_t find_free_way(const mbtb_set_t& set) {
//   for (std::size_t w = 0; w < BTB_WAY; ++w) {
//     if (!set[w].valid) return w;
//   }
//   return BTB_WAY;
// }

// // Find the LRU way in the set.
// inline std::size_t find_lru_way(const mbtb_set_t& set) {
//   std::size_t lru = 0;
//   for (std::size_t w = 1; w < BTB_WAY; ++w) {
//     if (set[w].lru_ts < set[lru].lru_ts) lru = w;
//   }
//   return lru;
// }


// #ifdef BTB_DEBUG_STATS
// struct MBTBStats {
//     uint64_t total_lookups = 0;
//     uint64_t no_candidate = 0;       // lookup found no matching (prev_pc, transition)
//     uint64_t validation_failures = 0; // candidates exist but none matched succ_pc_hash
//     uint64_t hits_returned = 0;      // a candidate matched and was used
//     uint64_t total_candidates_packed = 0;  // sum of candidates across lookups
//     uint64_t lookups_with_multi_candidates = 0;  // diagnostic: how often we held >1
//     uint64_t return_hits = 0;
//     uint64_t indirect_hits = 0;
//     uint64_t conditional_hits = 0;
//     uint64_t always_taken_hits = 0;
// };

// std::map<O3_CPU*, MBTBStats> MBTB_STATS;

// struct MBTBStatsPrinter {
//     ~MBTBStatsPrinter() {
//         for (const auto& pair : MBTB_STATS) {
//             const auto& s = pair.second;
//             std::cerr << "\n========== MULTI-BLOCK BTB DEBUG STATISTICS ==========\n";
//             std::cerr << "Total lookups:               " << s.total_lookups << "\n";
//             std::cerr << "No-candidate (true miss):    " << s.no_candidate << "\n";
//             std::cerr << "Validation failures:         " << s.validation_failures << "\n";
//             std::cerr << "Hits returned:               " << s.hits_returned << "\n";
//             if (s.total_lookups > 0) {
//                 std::cerr << "Miss rate (no-cand):         "
//                           << (100.0 * s.no_candidate / s.total_lookups) << "%\n";
//                 std::cerr << "Validation fail rate:        "
//                           << (100.0 * s.validation_failures / s.total_lookups) << "%\n";
//                 std::cerr << "Hit-return rate:             "
//                           << (100.0 * s.hits_returned / s.total_lookups) << "%\n";
//                 std::cerr << "Avg candidates per lookup:   "
//                           << ((double)s.total_candidates_packed / s.total_lookups) << "\n";
//                 std::cerr << "Lookups w/ multi-candidate:  " << s.lookups_with_multi_candidates
//                           << " (" << (100.0 * s.lookups_with_multi_candidates / s.total_lookups)
//                           << "%)\n";
//             }
//             std::cerr << "Return hits:                 " << s.return_hits << "\n";
//             std::cerr << "Indirect hits:               " << s.indirect_hits << "\n";
//             std::cerr << "Conditional hits:            " << s.conditional_hits << "\n";
//             std::cerr << "Always taken hits:           " << s.always_taken_hits << "\n";
//             std::cerr << "=====================================================\n";
//         }
//     }
// };
// static MBTBStatsPrinter mbtb_stats_printer;
// #endif

// } // namespace


// void O3_CPU::initialize_btb()
// {
//   ::MBTB[this] = mbtb_t(BTB_SET);          // all ways default-constructed (valid=false)
//   ::MBTB_TIME[this] = 0;

//   std::fill(std::begin(::INDIRECT_BTB[this]), std::end(::INDIRECT_BTB[this]), 0);
//   std::fill(std::begin(::CALL_SIZE[this]), std::end(::CALL_SIZE[this]), 4);

//   ::CONDITIONAL_HISTORY[this] = 0;
//   ::LAST_BRANCH_IP[this] = 0;
//   ::LAST_TRANSITION[this] = mbtb_transition::N;

//   ::PRED_BUFFER[this] = pred_buffer_t{};
// }


// std::pair<uint64_t, uint8_t> O3_CPU::btb_prediction(uint64_t ip)
// {
//   auto& buf = ::PRED_BUFFER[this];

//   std::pair<uint64_t, uint8_t> result;
//   bool prediction_usable = false;
//   buffered_candidate_t winner{};

// #ifdef BTB_DEBUG_STATS
//   auto& s = ::MBTB_STATS[this];
//   s.total_lookups++;
//   s.total_candidates_packed += buf.num_candidates;
//   if (buf.num_candidates > 1) s.lookups_with_multi_candidates++;
// #endif

//   if (buf.num_candidates == 0) {
// #ifdef BTB_DEBUG_STATS
//     s.no_candidate++;
// #endif
//   } else {
//     uint16_t want_hash = pc_hash(ip);
//     for (uint8_t i = 0; i < buf.num_candidates; ++i) {
//       if (buf.candidates[i].valid &&
//           buf.candidates[i].expected_succ_pc_hash == want_hash) {
//         winner = buf.candidates[i];
//         prediction_usable = true;
//         break;
//       }
//     }
//     if (!prediction_usable) {
// #ifdef BTB_DEBUG_STATS
//       s.validation_failures++;
// #endif
//     }
//   }

//   if (!prediction_usable) {
//     result = {0, false};
//   } else {
// #ifdef BTB_DEBUG_STATS
//     s.hits_returned++;
//     if      (winner.type == branch_info::RETURN)       s.return_hits++;
//     else if (winner.type == branch_info::INDIRECT)     s.indirect_hits++;
//     else if (winner.type == branch_info::CONDITIONAL)  s.conditional_hits++;
//     else if (winner.type == branch_info::ALWAYS_TAKEN) s.always_taken_hits++;
// #endif

//     if (winner.type == branch_info::RETURN) {
//       if (std::empty(::RAS[this])) {
//         result = {0, true};
//       } else {
//         auto target = ::RAS[this].back();
//         auto size = ::CALL_SIZE[this][target % std::size(::CALL_SIZE[this])];
//         result = {target + size, true};
//       }
//     } else if (winner.type == branch_info::INDIRECT) {
//       auto hash = (ip >> 2) ^ ::CONDITIONAL_HISTORY[this].to_ullong();
//       result = {::INDIRECT_BTB[this][hash % std::size(::INDIRECT_BTB[this])], true};
//     } else {
//       result = {winner.target, winner.type != branch_info::CONDITIONAL};
//     }
//   }

//   // =============================================================
//   // CRITICAL FIX: Only destroy the prediction when we actually
//   // used it.  Non-branch instructions MUST NOT clobber the buffer.
//   // =============================================================
//   if (prediction_usable) {
//     buf.num_candidates = 0;
//     for (auto& c : buf.candidates) c.valid = false;
//   }

//   return result;
// }



// void O3_CPU::update_btb(uint64_t ip, uint64_t branch_target, uint8_t taken, uint8_t branch_type)
// {
//   // ---- CALL handling ----
//   if (branch_type == BRANCH_DIRECT_CALL || branch_type == BRANCH_INDIRECT_CALL) {
//     ::RAS[this].push_back(ip);
//     ::SAS[this].push_back(ip);

//     if (std::size(::RAS[this]) > RAS_SIZE)
//       ::RAS[this].pop_front();

//     if (std::size(::SAS[this]) > SAS_SIZE)
//       ::SAS[this].pop_front();
//   }

//   // ---- INDIRECT ----
//   if ((branch_type == BRANCH_INDIRECT) || (branch_type == BRANCH_INDIRECT_CALL)) {
//     auto hash = (ip >> 2) ^ ::CONDITIONAL_HISTORY[this].to_ullong();
//     ::INDIRECT_BTB[this][hash % std::size(::INDIRECT_BTB[this])] = branch_target;
//   }

//   // ---- CONDITIONAL HISTORY ----
//   if ((branch_type == BRANCH_CONDITIONAL) || (branch_type == BRANCH_OTHER)) {
//     ::CONDITIONAL_HISTORY[this] <<= 1;
//     ::CONDITIONAL_HISTORY[this].set(0, taken);
//   }

//   // ---- RETURN ----
//   if (branch_type == BRANCH_RETURN && !std::empty(::RAS[this])) {
//     auto call_ip = ::RAS[this].back();
//     ::RAS[this].pop_back();

//     if (!std::empty(::SAS[this]))
//       ::SAS[this].pop_back();

//     auto estimated_call_instr_size = (call_ip > branch_target) ? call_ip - branch_target : branch_target - call_ip;
//     if (estimated_call_instr_size <= 10) {
//       ::CALL_SIZE[this][call_ip % std::size(::CALL_SIZE[this])] = estimated_call_instr_size;
//     }
//   }

//   // ---- Determine type ----
//   auto type = branch_info::ALWAYS_TAKEN;

//   if ((branch_type == BRANCH_INDIRECT) || (branch_type == BRANCH_INDIRECT_CALL))
//     type = branch_info::INDIRECT;
//   else if (branch_type == BRANCH_RETURN)
//     type = branch_info::RETURN;
//   else if ((branch_type == BRANCH_CONDITIONAL) || (branch_type == BRANCH_OTHER))
//     type = branch_info::CONDITIONAL;

//   // ---- Transition ----
//   mbtb_transition trans;
//   if (branch_type == BRANCH_DIRECT_CALL || branch_type == BRANCH_INDIRECT_CALL) {
//     trans = mbtb_transition::R;
//   } else if (taken &&
//              branch_type != BRANCH_RETURN &&
//              branch_type != BRANCH_DIRECT_CALL &&
//              branch_type != BRANCH_INDIRECT_CALL) {
//     trans = mbtb_transition::T;
//   } else {
//     trans = mbtb_transition::N;
//   }

//   // =============================================================
//   // Update MBTB.
//   // The entry being written represents the transition
//   //   (LAST_BRANCH_IP, LAST_TRANSITION) -> ip
//   // =============================================================
//   if (branch_target != 0) {
//     uint64_t prev_ip = ::LAST_BRANCH_IP[this];
//     mbtb_transition prev_trans = ::LAST_TRANSITION[this];
//     uint16_t this_succ_hash = pc_hash(ip);

//     std::size_t set_idx = set_index_of(prev_ip);
//     auto& set = ::MBTB[this][set_idx];

//     std::size_t target_way = find_way_exact(set, prev_ip, prev_trans, this_succ_hash);
//     if (target_way == BTB_WAY) {
//       target_way = find_free_way(set);
//       if (target_way == BTB_WAY) {
//         target_way = find_lru_way(set);
//       }
//     }

//     auto& victim = set[target_way];
//     victim.valid        = true;
//     victim.prev_pc      = prev_ip;
//     victim.transition   = prev_trans;
//     victim.succ_pc_hash = this_succ_hash;
//     victim.target       = branch_target;
//     victim.type         = type;
//     victim.lru_ts       = ++::MBTB_TIME[this];
//   }

//   // ---- Update LAST (Aa tracking) ----
//   ::LAST_BRANCH_IP[this] = ip;
//   ::LAST_TRANSITION[this] = trans;

//   // =============================================================
//   // CRITICAL FIX: Refill the prediction buffer using the branch
//   // we just fetched as the new predecessor.  This buffer now
//   // survives across all subsequent non-branch instructions until
//   // the predicted successor branch is encountered.
//   // =============================================================
//   auto& buf = ::PRED_BUFFER[this];
//   buf.num_candidates = 0;
//   for (auto& c : buf.candidates) c.valid = false;

//   uint64_t prev_ip_for_next = ::LAST_BRANCH_IP[this];   // == ip
//   mbtb_transition trans_for_next = ::LAST_TRANSITION[this];
//   std::size_t set_idx = set_index_of(prev_ip_for_next);

//   auto& set = ::MBTB[this][set_idx];
//   for (std::size_t w = 0; w < BTB_WAY; ++w) {
//     const auto& e = set[w];
//     if (e.valid && e.prev_pc == prev_ip_for_next && e.transition == trans_for_next) {
//       buf.candidates[buf.num_candidates] = buffered_candidate_t{
//           true, e.succ_pc_hash, e.target, e.type};
//       buf.num_candidates++;
//       set[w].lru_ts = ++::MBTB_TIME[this];
//       if (buf.num_candidates >= BTB_WAY) break;
//     }
//   }
// }