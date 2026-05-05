/*
 * Extended Baseline BTB with branch-only diagnostic statistics.
 * Non-branch instructions are filtered via a FIFO prediction queue.
 * Statistics are printed via static destructor at program exit.
 */

#include <algorithm>
#include <bitset>
#include <deque>
#include <map>
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

constexpr std::size_t BTB_SET = 2048;
constexpr std::size_t BTB_WAY = 4;
constexpr std::size_t BTB_INDIRECT_SIZE = 4096;
constexpr std::size_t RAS_SIZE = 64;
constexpr std::size_t CALL_SIZE_TRACKERS = 1024;

struct btb_entry_t {
  uint64_t ip_tag = 0;
  uint64_t target = 0;
  branch_info type = branch_info::ALWAYS_TAKEN;

  auto index() const { return ip_tag >> 2; }
  auto tag() const { return ip_tag >> 2; }
};

// ---- FIFO queue to filter out non-branch instructions ----
struct pending_btb_pred_t {
  uint64_t ip;
  uint64_t target;
  branch_info type;
  bool was_hit;
  bool ras_empty; // meaningful only when type==RETURN
};

struct btb_stats_t {
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
};

std::map<O3_CPU*, champsim::msl::lru_table<btb_entry_t>> BTB;
std::map<O3_CPU*, std::array<uint64_t, BTB_INDIRECT_SIZE>> INDIRECT_BTB;
std::map<O3_CPU*, std::bitset<champsim::lg2(BTB_INDIRECT_SIZE)>> CONDITIONAL_HISTORY;
std::map<O3_CPU*, std::deque<uint64_t>> RAS;
std::map<O3_CPU*, std::array<uint64_t, CALL_SIZE_TRACKERS>> CALL_SIZE;

std::map<O3_CPU*, std::deque<pending_btb_pred_t>> BTB_PRED_QUEUE;
std::map<O3_CPU*, btb_stats_t> BTB_STATS;

// ---- Static printer that runs at program exit ----
struct BaselineBTBStatsPrinter {
  ~BaselineBTBStatsPrinter() {
    for (auto& [cpu, stats] : BTB_STATS) {
      (void)cpu; // unused in print
      std::cout << "\n========== BASELINE BTB STATISTICS ==========\n";
      std::cout << "Branch updates:           " << stats.total_updates << "\n";
      std::cout << "Total hits:               " << stats.total_hits << "\n";
      std::cout << "Total misses:             " << stats.total_misses << "\n";
      if ((stats.total_hits + stats.total_misses) > 0)
        std::cout << "Hit rate:                 " << std::fixed << std::setprecision(4)
                  << (100.0 * stats.total_hits / (stats.total_hits + stats.total_misses)) << "%\n";
      std::cout << "Target correct:           " << stats.target_correct << "\n";
      std::cout << "Target wrong:             " << stats.target_wrong << "\n";
      if ((stats.target_correct + stats.target_wrong) > 0)
        std::cout << "Target accuracy:          " << std::fixed << std::setprecision(4)
                  << (100.0 * stats.target_correct / (stats.target_correct + stats.target_wrong)) << "%\n";
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
      std::cout << "=============================================\n";
    }
  }
};
static BaselineBTBStatsPrinter baseline_btb_printer;

} // namespace

void O3_CPU::initialize_btb()
{
  ::BTB.insert({this, champsim::msl::lru_table<btb_entry_t>{BTB_SET, BTB_WAY}});
  std::fill(std::begin(::INDIRECT_BTB[this]), std::end(::INDIRECT_BTB[this]), 0);
  std::fill(std::begin(::CALL_SIZE[this]), std::end(::CALL_SIZE[this]), 4);
  ::CONDITIONAL_HISTORY[this] = 0;
  ::BTB_PRED_QUEUE[this].clear();
  ::BTB_STATS[this] = {};
}

std::pair<uint64_t, uint8_t> O3_CPU::btb_prediction(uint64_t ip)
{
  auto btb_entry = ::BTB.at(this).check_hit({ip, 0, ::branch_info::ALWAYS_TAKEN});

  uint64_t predicted_target = 0;
  uint8_t always_taken = false;
  branch_info pred_type = ::branch_info::ALWAYS_TAKEN;
  bool was_hit = false;
  bool ras_empty = false;

  if (btb_entry.has_value()) {
    was_hit = true;
    pred_type = btb_entry->type;

    if (btb_entry->type == ::branch_info::RETURN) {
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
    else if (btb_entry->type == ::branch_info::INDIRECT) {
      auto hash = (ip >> 2) ^ ::CONDITIONAL_HISTORY[this].to_ullong();
      predicted_target = ::INDIRECT_BTB[this][hash % std::size(::INDIRECT_BTB[this])];
      always_taken = true;
    }
    else {
      predicted_target = btb_entry->target;
      always_taken = (btb_entry->type != ::branch_info::CONDITIONAL);
    }
  }

  ::BTB_PRED_QUEUE[this].push_back({ip, predicted_target, pred_type, was_hit, ras_empty});
  return {predicted_target, always_taken};
}

void O3_CPU::update_btb(uint64_t ip, uint64_t branch_target, uint8_t taken, uint8_t branch_type)
{
  auto& stats = ::BTB_STATS[this];
  stats.total_updates++;
  if (taken) stats.taken_count++; else stats.not_taken_count++;

  // ---- Drain queue until we find the matching branch IP ----
  auto& q = ::BTB_PRED_QUEUE[this];
  bool found = false;
  pending_btb_pred_t pred;
  while (!q.empty()) {
    pred = q.front();
    q.pop_front();
    if (pred.ip == ip) {
      found = true;
      break;
    }
    // Non-branch instructions are silently discarded here
  }

  if (found) {
    if (pred.was_hit) {
      stats.total_hits++;
      switch (pred.type) {
        case branch_info::CONDITIONAL: stats.conditional_hits++; break;
        case branch_info::INDIRECT:    stats.indirect_hits++;    break;
        case branch_info::RETURN:        stats.return_hits++;      break;
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
    // No pending prediction found for this branch
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

  // ---- RAS / indirect maintenance ----
  if (branch_type == BRANCH_DIRECT_CALL || branch_type == BRANCH_INDIRECT_CALL) {
    RAS[this].push_back(ip);
    if (std::size(RAS[this]) > RAS_SIZE)
      RAS[this].pop_front();
  }

  if ((branch_type == BRANCH_INDIRECT) || (branch_type == BRANCH_INDIRECT_CALL)) {
    auto hash = (ip >> 2) ^ ::CONDITIONAL_HISTORY[this].to_ullong();
    ::INDIRECT_BTB[this][hash % std::size(::INDIRECT_BTB[this])] = branch_target;
  }

  if ((branch_type == BRANCH_CONDITIONAL) || (branch_type == BRANCH_OTHER)) {
    ::CONDITIONAL_HISTORY[this] <<= 1;
    ::CONDITIONAL_HISTORY[this].set(0, taken);
  }

  if (branch_type == BRANCH_RETURN && !std::empty(::RAS[this])) {
    auto call_ip = ::RAS[this].back();
    ::RAS[this].pop_back();
    auto estimated_call_instr_size = (call_ip > branch_target) ? call_ip - branch_target : branch_target - call_ip;
    if (estimated_call_instr_size <= 10) {
      ::CALL_SIZE[this][call_ip % std::size(::CALL_SIZE[this])] = estimated_call_instr_size;
    }
  }

  // ---- Update BTB entry ----
  auto type = ::branch_info::ALWAYS_TAKEN;
  if ((branch_type == BRANCH_INDIRECT) || (branch_type == BRANCH_INDIRECT_CALL))
    type = ::branch_info::INDIRECT;
  else if (branch_type == BRANCH_RETURN)
    type = ::branch_info::RETURN;
  else if ((branch_type == BRANCH_CONDITIONAL) || (branch_type == BRANCH_OTHER))
    type = ::branch_info::CONDITIONAL;

  auto opt_entry = ::BTB.at(this).check_hit({ip, branch_target, type});
  if (opt_entry.has_value()) {
    opt_entry->type = type;
    if (branch_target != 0)
      opt_entry->target = branch_target;
  }

  if (branch_target != 0) {
    ::BTB.at(this).fill(opt_entry.value_or(::btb_entry_t{ip, branch_target, type}));
  }
}