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