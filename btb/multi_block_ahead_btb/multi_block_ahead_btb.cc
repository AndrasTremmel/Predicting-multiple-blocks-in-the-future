/*
 * Extended Multi-Block Ahead BTB with branch-only diagnostic statistics.
 */

#include <cstdint>
#include <map>
#include <array>
#include <bitset>
#include <deque>
#include <algorithm>
#include <utility>
#include <cstdlib>
#include <iostream>
#include <iomanip>

#include "msl/lru_table.h"
#include "ooo_cpu.h"

namespace
{

enum class branch_info {
  INDIRECT,
  RETURN,
  ALWAYS_TAKEN,
  CONDITIONAL,
};

enum class mbtb_transition : uint8_t {
  T,
  N,
  R
};

struct mbtb_entry_t {
  uint64_t ip_tag = 0;
  uint64_t target = 0;
  branch_info type = branch_info::ALWAYS_TAKEN;
  mbtb_transition transition = mbtb_transition::N;

  auto index() const { return ip_tag >> 2; }
  auto tag() const { return (ip_tag >> 2) ^ static_cast<uint64_t>(transition); }
};

// ---- FIFO queue to filter non-branch instructions ----
struct pending_mbtb_pred_t {
  uint64_t ip;
  uint64_t prev_ip;
  mbtb_transition trans;
  uint64_t target;
  branch_info type;
  bool was_hit;
  bool ras_empty;
};

struct mbtb_stats_t {
  uint64_t total_updates = 0;
  uint64_t taken_count = 0;
  uint64_t not_taken_count = 0;

  uint64_t total_hits = 0;
  uint64_t total_misses = 0;
  uint64_t target_correct = 0;
  uint64_t target_wrong = 0;

  uint64_t conditional_hits = 0;
  uint64_t conditional_misses = 0;
  uint64_t indirect_hits = 0;
  uint64_t indirect_misses = 0;
  uint64_t return_hits = 0;
  uint64_t return_misses = 0;
  uint64_t always_taken_hits = 0;
  uint64_t always_taken_misses = 0;

  uint64_t ras_hits = 0;
  uint64_t ras_misses = 0;
  uint64_t ras_target_correct = 0;
  uint64_t ras_target_wrong = 0;

  uint64_t indirect_target_correct = 0;
  uint64_t indirect_target_wrong = 0;
  uint64_t btb_target_correct = 0;
  uint64_t btb_target_wrong = 0;

  // MBTB-specific
  uint64_t transition_hits[3] = {0,0,0};
  uint64_t transition_misses[3] = {0,0,0};
  uint64_t correct_transition_type = 0;
  uint64_t wrong_transition_type = 0;
  uint64_t prev_ip_match = 0;
  uint64_t prev_ip_mismatch = 0;
  uint64_t sas_pushes = 0;
  uint64_t sas_pops = 0;
  uint64_t sas_empty_on_pop = 0;
};

constexpr std::size_t BTB_SET = 2048;
constexpr std::size_t BTB_WAY = 4;
constexpr std::size_t BTB_INDIRECT_SIZE = 4096;
constexpr std::size_t RAS_SIZE = 64;
constexpr std::size_t SAS_SIZE = 64;
constexpr std::size_t CALL_SIZE_TRACKERS = 1024;

std::map<O3_CPU*, champsim::msl::lru_table<mbtb_entry_t>> MBTB;
std::map<O3_CPU*, std::array<uint64_t, BTB_INDIRECT_SIZE>> INDIRECT_BTB;
std::map<O3_CPU*, std::bitset<champsim::lg2(BTB_INDIRECT_SIZE)>> CONDITIONAL_HISTORY;
std::map<O3_CPU*, std::deque<uint64_t>> RAS;
std::map<O3_CPU*, std::deque<uint64_t>> SAS;
std::map<O3_CPU*, std::array<uint64_t, CALL_SIZE_TRACKERS>> CALL_SIZE;

std::map<O3_CPU*, uint64_t> LAST_BRANCH_IP;
std::map<O3_CPU*, mbtb_transition> LAST_TRANSITION;

std::map<O3_CPU*, std::deque<pending_mbtb_pred_t>> MBTB_PRED_QUEUE;
std::map<O3_CPU*, mbtb_stats_t> MBTB_STATS;

// ---- Static printer that runs at program exit ----
struct MultiBlockBTBStatsPrinter {
  ~MultiBlockBTBStatsPrinter() {
    for (auto& [cpu, stats] : MBTB_STATS) {
      (void)cpu;
      std::cout << "\n========== MULTI-BLOCK BTB STATISTICS ==========\n";
      std::cout << "Branch updates:           " << stats.total_updates << "\n";
      std::cout << "Total hits:               " << stats.total_hits << "\n";
      std::cout << "Total misses:             " << stats.total_misses << "\n";
      if ((stats.total_hits + stats.total_misses) > 0)
        std::cout << "Hit rate:                 " << std::fixed << std::setprecision(4)
                  << (100.0 * stats.total_hits / (stats.total_hits + stats.total_misses)) << "%\n";
      std::cout << "Target correct:           " << stats.target_correct << "\n";
      std::cout << "Target wrong:             " << stats.target_wrong << "\n";
      std::cout << "Conditional hits:         " << stats.conditional_hits << "\n";
      std::cout << "Conditional misses:       " << stats.conditional_misses << "\n";
      std::cout << "Indirect hits:            " << stats.indirect_hits << "\n";
      std::cout << "Indirect misses:          " << stats.indirect_misses << "\n";
      std::cout << "Return hits:              " << stats.return_hits << "\n";
      std::cout << "Return misses:            " << stats.return_misses << "\n";
      std::cout << "Always taken hits:        " << stats.always_taken_hits << "\n";
      std::cout << "Always taken misses:      " << stats.always_taken_misses << "\n";
      std::cout << "RAS hits:                 " << stats.ras_hits << "\n";
      std::cout << "RAS misses:               " << stats.ras_misses << "\n";
      std::cout << "RAS target correct:       " << stats.ras_target_correct << "\n";
      std::cout << "RAS target wrong:         " << stats.ras_target_wrong << "\n";
      std::cout << "Indirect target correct:  " << stats.indirect_target_correct << "\n";
      std::cout << "Indirect target wrong:    " << stats.indirect_target_wrong << "\n";
      std::cout << "BTB target correct:       " << stats.btb_target_correct << "\n";
      std::cout << "BTB target wrong:         " << stats.btb_target_wrong << "\n";
      std::cout << "\n--- MBTB-specific diagnostics ---\n";
      std::cout << "Transition hits T/N/R:    " << stats.transition_hits[0] << " / "
                << stats.transition_hits[1] << " / " << stats.transition_hits[2] << "\n";
      std::cout << "Transition misses T/N/R:  " << stats.transition_misses[0] << " / "
                << stats.transition_misses[1] << " / " << stats.transition_misses[2] << "\n";
      std::cout << "Correct transition type:  " << stats.correct_transition_type << "\n";
      std::cout << "Wrong transition type:    " << stats.wrong_transition_type << "\n";
      std::cout << "Prev IP match:            " << stats.prev_ip_match << "\n";
      std::cout << "Prev IP mismatch:         " << stats.prev_ip_mismatch << "\n";
      std::cout << "SAS pushes:               " << stats.sas_pushes << "\n";
      std::cout << "SAS pops:                 " << stats.sas_pops << "\n";
      std::cout << "SAS empty on pop:         " << stats.sas_empty_on_pop << "\n";
      std::cout << "=================================================\n";
    }
  }
};
static MultiBlockBTBStatsPrinter multi_block_btb_printer;

} // namespace


void O3_CPU::initialize_btb()
{
  ::MBTB.insert({this, champsim::msl::lru_table<mbtb_entry_t>{BTB_SET, BTB_WAY}});
  std::fill(std::begin(::INDIRECT_BTB[this]), std::end(::INDIRECT_BTB[this]), 0);
  std::fill(std::begin(::CALL_SIZE[this]), std::end(::CALL_SIZE[this]), 4);
  ::CONDITIONAL_HISTORY[this] = 0;
  ::LAST_BRANCH_IP[this] = 0;
  ::LAST_TRANSITION[this] = mbtb_transition::N;
  ::MBTB_PRED_QUEUE[this].clear();
  ::MBTB_STATS[this] = {};
}


std::pair<uint64_t, uint8_t> O3_CPU::btb_prediction(uint64_t ip)
{
  auto prev_ip = ::LAST_BRANCH_IP[this];
  auto trans   = ::LAST_TRANSITION[this];

  auto entry = ::MBTB.at(this).check_hit({prev_ip, 0, branch_info::ALWAYS_TAKEN, trans});

  uint64_t predicted_target = 0;
  uint8_t always_taken = false;
  branch_info pred_type = branch_info::ALWAYS_TAKEN;
  bool was_hit = false;
  bool ras_empty = false;

  if (entry.has_value()) {
    was_hit = true;
    pred_type = entry->type;
    predicted_target = entry->target;

    if (entry->type == branch_info::RETURN) {
      if (std::empty(::RAS[this])) {
        ras_empty = true;
        predicted_target = 0;
        always_taken = true;
      } else {
        ras_empty = false;
        auto target = ::RAS[this].back();
        auto size = ::CALL_SIZE[this][target % std::size(::CALL_SIZE[this])];
        predicted_target = target + size;
        always_taken = true;
      }
    }
    else if (entry->type == branch_info::INDIRECT) {
      auto hash = (ip >> 2) ^ ::CONDITIONAL_HISTORY[this].to_ullong();
      predicted_target = ::INDIRECT_BTB[this][hash % std::size(::INDIRECT_BTB[this])];
      always_taken = true;
    }
    else if (entry->type == branch_info::CONDITIONAL) {
      always_taken = false;
    }
    else {
      always_taken = true;
    }
  }

  ::MBTB_PRED_QUEUE[this].push_back({ip, prev_ip, trans, predicted_target, pred_type, was_hit, ras_empty});
  return {predicted_target, always_taken};
}


void O3_CPU::update_btb(uint64_t ip, uint64_t branch_target, uint8_t taken, uint8_t branch_type)
{
  auto& stats = ::MBTB_STATS[this];
  stats.total_updates++;
  if (taken) stats.taken_count++; else stats.not_taken_count++;

  // ---- Drain queue until matching branch IP (non-branches discarded) ----
  auto& q = ::MBTB_PRED_QUEUE[this];
  bool found = false;
  pending_mbtb_pred_t pred;
  while (!q.empty()) {
    pred = q.front();
    q.pop_front();
    if (pred.ip == ip) {
      found = true;
      break;
    }
  }

  // Determine actual transition type for diagnostics
  mbtb_transition actual_trans;
  if (branch_type == BRANCH_DIRECT_CALL || branch_type == BRANCH_INDIRECT_CALL) {
    actual_trans = mbtb_transition::R;
  }
  else if (taken &&
    branch_type != BRANCH_RETURN &&
    branch_type != BRANCH_DIRECT_CALL &&
    branch_type != BRANCH_INDIRECT_CALL) {
    actual_trans = mbtb_transition::T;
  }
  else {
    actual_trans = mbtb_transition::N;
  }

  if (found) {
    // Compare prev_ip and transition used at prediction time vs actual
    uint64_t actual_prev_ip = ::LAST_BRANCH_IP[this];
    if (pred.prev_ip == actual_prev_ip) stats.prev_ip_match++;
    else stats.prev_ip_mismatch++;

    if (pred.trans == actual_trans) stats.correct_transition_type++;
    else stats.wrong_transition_type++;

    if (pred.was_hit) {
      stats.total_hits++;
      stats.transition_hits[static_cast<int>(pred.trans)]++;
      switch (pred.type) {
        case branch_info::CONDITIONAL: stats.conditional_hits++; break;
        case branch_info::INDIRECT:    stats.indirect_hits++; break;
        case branch_info::RETURN:        stats.return_hits++; break;
        default:                       stats.always_taken_hits++; break;
      }

      if (pred.type == branch_info::RETURN) {
        if (pred.ras_empty) stats.ras_misses++;
        else stats.ras_hits++;
      }

      bool target_used = taken || (pred.type != branch_info::CONDITIONAL);
      if (target_used) {
        if (pred.target == branch_target) {
          stats.target_correct++;
          switch (pred.type) {
            case branch_info::RETURN:      stats.ras_target_correct++; break;
            case branch_info::INDIRECT:    stats.indirect_target_correct++; break;
            default:                       stats.btb_target_correct++; break;
          }
        } else {
          stats.target_wrong++;
          switch (pred.type) {
            case branch_info::RETURN:      stats.ras_target_wrong++; break;
            case branch_info::INDIRECT:    stats.indirect_target_wrong++; break;
            default:                       stats.btb_target_wrong++; break;
          }
        }
      }
    } else {
      stats.total_misses++;
      stats.transition_misses[static_cast<int>(pred.trans)]++;
      switch (branch_type) {
        case BRANCH_CONDITIONAL:
        case BRANCH_OTHER:               stats.conditional_misses++; break;
        case BRANCH_INDIRECT:
        case BRANCH_INDIRECT_CALL:       stats.indirect_misses++; break;
        case BRANCH_RETURN:              stats.return_misses++; break;
        default:                         stats.always_taken_misses++; break;
      }
    }
  } else {
    stats.total_misses++;
    switch (branch_type) {
      case BRANCH_CONDITIONAL:
      case BRANCH_OTHER:                 stats.conditional_misses++; break;
      case BRANCH_INDIRECT:
      case BRANCH_INDIRECT_CALL:         stats.indirect_misses++; break;
      case BRANCH_RETURN:                stats.return_misses++; break;
      default:                           stats.always_taken_misses++; break;
    }
  }

  // ---- CALL handling ----
  if (branch_type == BRANCH_DIRECT_CALL || branch_type == BRANCH_INDIRECT_CALL) {
    ::RAS[this].push_back(ip);
    ::SAS[this].push_back(ip);
    stats.sas_pushes++;
    if (std::size(::RAS[this]) > RAS_SIZE)
      ::RAS[this].pop_front();
    if (std::size(::SAS[this]) > SAS_SIZE)
      ::SAS[this].pop_front();
  }

  // ---- RETURN ----
  if (branch_type == BRANCH_RETURN && !std::empty(::RAS[this])) {
    auto call_ip = ::RAS[this].back();
    ::RAS[this].pop_back();

    if (!std::empty(::SAS[this])) {
      ::SAS[this].pop_back();
      stats.sas_pops++;
    } else {
      stats.sas_empty_on_pop++;
    }

    auto estimated_size = std::abs((long)(call_ip - branch_target));
    if (estimated_size <= 10) {
      ::CALL_SIZE[this][call_ip % std::size(::CALL_SIZE[this])] = estimated_size;
    }
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
  if (branch_type == BRANCH_DIRECT_CALL || branch_type == BRANCH_INDIRECT_CALL) {
    trans = mbtb_transition::R;
  }
  else if (taken &&
    branch_type != BRANCH_RETURN &&
    branch_type != BRANCH_DIRECT_CALL &&
    branch_type != BRANCH_INDIRECT_CALL) {
    trans = mbtb_transition::T;
  }
  else {
    trans = mbtb_transition::N;
  }

  // ---- Update MBTB using PREVIOUS branch ----
  auto prev_ip = ::LAST_BRANCH_IP[this];
  auto prev_trans = ::LAST_TRANSITION[this];

  auto opt_entry = ::MBTB.at(this).check_hit({prev_ip, branch_target, type, prev_trans});

  if (opt_entry.has_value()) {
    if (branch_target != 0) opt_entry->target = branch_target;
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