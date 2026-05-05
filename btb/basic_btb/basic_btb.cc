/*
 * Extended Baseline BTB with branch-only diagnostic statistics.
 * Statistics printed from a global object's destructor.
 */

#include <algorithm>
#include <bitset>
#include <deque>
#include <map>
#include <iostream>
#include <iomanip>
#include <fstream>
#include <cstdlib>

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

struct pending_btb_pred_t {
  uint64_t ip;
  uint64_t target;
  branch_info type;
  bool was_hit;
  bool ras_empty;
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

class BaselineBTBContext {
 public:
  std::map<O3_CPU*, champsim::msl::lru_table<btb_entry_t>> BTB;
  std::map<O3_CPU*, std::array<uint64_t, BTB_INDIRECT_SIZE>> INDIRECT_BTB;
  std::map<O3_CPU*, std::bitset<champsim::lg2(BTB_INDIRECT_SIZE)>> CONDITIONAL_HISTORY;
  std::map<O3_CPU*, std::deque<uint64_t>> RAS;
  std::map<O3_CPU*, std::array<uint64_t, CALL_SIZE_TRACKERS>> CALL_SIZE;
  std::map<O3_CPU*, std::deque<pending_btb_pred_t>> PRED_QUEUE;
  std::map<O3_CPU*, btb_stats_t> STATS;

  ~BaselineBTBContext() {
    print_all();
  }

  void print_all() {
    auto out = [&](const std::string& s) {
      std::cerr << s;
    };

    for (auto& [cpu, stats] : STATS) {
      (void)cpu;
      out("\n========== BASELINE BTB STATISTICS ==========\n");
      out("Branch updates:           " + std::to_string(stats.total_updates) + "\n");
      out("Total hits:               " + std::to_string(stats.total_hits) + "\n");
      out("Total misses:             " + std::to_string(stats.total_misses) + "\n");
      if ((stats.total_hits + stats.total_misses) > 0) {
        std::ostringstream oss;
        oss << std::fixed << std::setprecision(4)
            << (100.0 * stats.total_hits / (stats.total_hits + stats.total_misses));
        out("Hit rate:                 " + oss.str() + "%\n");
      }
      out("Target correct:           " + std::to_string(stats.target_correct) + "\n");
      out("Target wrong:             " + std::to_string(stats.target_wrong) + "\n");
      if ((stats.target_correct + stats.target_wrong) > 0) {
        std::ostringstream oss;
        oss << std::fixed << std::setprecision(4)
            << (100.0 * stats.target_correct / (stats.target_correct + stats.target_wrong));
        out("Target accuracy:          " + oss.str() + "%\n");
      }
      out("Conditional hits:         " + std::to_string(stats.conditional_hits) + "\n");
      out("Conditional misses:       " + std::to_string(stats.conditional_misses) + "\n");
      out("Indirect hits:            " + std::to_string(stats.indirect_hits) + "\n");
      out("Indirect misses:          " + std::to_string(stats.indirect_misses) + "\n");
      out("Return hits:              " + std::to_string(stats.return_hits) + "\n");
      out("Return misses:            " + std::to_string(stats.return_misses) + "\n");
      out("Always taken hits:        " + std::to_string(stats.always_taken_hits) + "\n");
      out("Always taken misses:      " + std::to_string(stats.always_taken_misses) + "\n");
      out("RAS hits:                 " + std::to_string(stats.ras_hits) + "\n");
      out("RAS misses:               " + std::to_string(stats.ras_misses) + "\n");
      out("RAS target correct:       " + std::to_string(stats.ras_target_correct) + "\n");
      out("RAS target wrong:         " + std::to_string(stats.ras_target_wrong) + "\n");
      out("Indirect target correct:  " + std::to_string(stats.indirect_target_correct) + "\n");
      out("Indirect target wrong:    " + std::to_string(stats.indirect_target_wrong) + "\n");
      out("BTB target correct:       " + std::to_string(stats.btb_target_correct) + "\n");
      out("BTB target wrong:         " + std::to_string(stats.btb_target_wrong) + "\n");
      out("=============================================\n");
    }

    std::cerr << std::flush;
  }
};

// Global singleton - destructor runs at program exit
static BaselineBTBContext g_ctx;

} // namespace

void O3_CPU::initialize_btb()
{
  g_ctx.BTB.insert({this, champsim::msl::lru_table<btb_entry_t>{BTB_SET, BTB_WAY}});
  std::fill(std::begin(g_ctx.INDIRECT_BTB[this]), std::end(g_ctx.INDIRECT_BTB[this]), 0);
  std::fill(std::begin(g_ctx.CALL_SIZE[this]), std::end(g_ctx.CALL_SIZE[this]), 4);
  g_ctx.CONDITIONAL_HISTORY[this] = 0;
  g_ctx.PRED_QUEUE[this].clear();
  g_ctx.STATS[this] = {};
}

std::pair<uint64_t, uint8_t> O3_CPU::btb_prediction(uint64_t ip)
{
  auto btb_entry = g_ctx.BTB.at(this).check_hit({ip, 0, ::branch_info::ALWAYS_TAKEN});

  uint64_t predicted_target = 0;
  uint8_t always_taken = false;
  branch_info pred_type = ::branch_info::ALWAYS_TAKEN;
  bool was_hit = false;
  bool ras_empty = false;

  if (btb_entry.has_value()) {
    was_hit = true;
    pred_type = btb_entry->type;

    if (btb_entry->type == ::branch_info::RETURN) {
      if (std::empty(g_ctx.RAS[this])) {
        ras_empty = true;
        predicted_target = 0;
        always_taken = true;
      } else {
        ras_empty = false;
        auto target = g_ctx.RAS[this].back();
        auto size = g_ctx.CALL_SIZE[this][target % std::size(g_ctx.CALL_SIZE[this])];
        predicted_target = target + size;
        always_taken = true;
      }
    }
    else if (btb_entry->type == ::branch_info::INDIRECT) {
      auto hash = (ip >> 2) ^ g_ctx.CONDITIONAL_HISTORY[this].to_ullong();
      predicted_target = g_ctx.INDIRECT_BTB[this][hash % std::size(g_ctx.INDIRECT_BTB[this])];
      always_taken = true;
    }
    else {
      predicted_target = btb_entry->target;
      always_taken = (btb_entry->type != ::branch_info::CONDITIONAL);
    }
  }

  g_ctx.PRED_QUEUE[this].push_back({ip, predicted_target, pred_type, was_hit, ras_empty});
  return {predicted_target, always_taken};
}

void O3_CPU::update_btb(uint64_t ip, uint64_t branch_target, uint8_t taken, uint8_t branch_type)
{
  auto& stats = g_ctx.STATS[this];
  stats.total_updates++;
  if (taken) stats.taken_count++; else stats.not_taken_count++;

  auto& q = g_ctx.PRED_QUEUE[this];
  bool found = false;
  pending_btb_pred_t pred;
  while (!q.empty()) {
    pred = q.front();
    q.pop_front();
    if (pred.ip == ip) {
      found = true;
      break;
    }
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

  if (branch_type == BRANCH_DIRECT_CALL || branch_type == BRANCH_INDIRECT_CALL) {
    g_ctx.RAS[this].push_back(ip);
    if (std::size(g_ctx.RAS[this]) > RAS_SIZE)
      g_ctx.RAS[this].pop_front();
  }

  if ((branch_type == BRANCH_INDIRECT) || (branch_type == BRANCH_INDIRECT_CALL)) {
    auto hash = (ip >> 2) ^ g_ctx.CONDITIONAL_HISTORY[this].to_ullong();
    g_ctx.INDIRECT_BTB[this][hash % std::size(g_ctx.INDIRECT_BTB[this])] = branch_target;
  }

  if ((branch_type == BRANCH_CONDITIONAL) || (branch_type == BRANCH_OTHER)) {
    g_ctx.CONDITIONAL_HISTORY[this] <<= 1;
    g_ctx.CONDITIONAL_HISTORY[this].set(0, taken);
  }

  if (branch_type == BRANCH_RETURN && !std::empty(g_ctx.RAS[this])) {
    auto call_ip = g_ctx.RAS[this].back();
    g_ctx.RAS[this].pop_back();
    auto estimated_call_instr_size = (call_ip > branch_target) ? call_ip - branch_target : branch_target - call_ip;
    if (estimated_call_instr_size <= 10) {
      g_ctx.CALL_SIZE[this][call_ip % std::size(g_ctx.CALL_SIZE[this])] = estimated_call_instr_size;
    }
  }

  auto type = ::branch_info::ALWAYS_TAKEN;
  if ((branch_type == BRANCH_INDIRECT) || (branch_type == BRANCH_INDIRECT_CALL))
    type = ::branch_info::INDIRECT;
  else if (branch_type == BRANCH_RETURN)
    type = ::branch_info::RETURN;
  else if ((branch_type == BRANCH_CONDITIONAL) || (branch_type == BRANCH_OTHER))
    type = ::branch_info::CONDITIONAL;

  auto opt_entry = g_ctx.BTB.at(this).check_hit({ip, branch_target, type});
  if (opt_entry.has_value()) {
    opt_entry->type = type;
    if (branch_target != 0)
      opt_entry->target = branch_target;
  }

  if (branch_target != 0) {
    g_ctx.BTB.at(this).fill(opt_entry.value_or(::btb_entry_t{ip, branch_target, type}));
  }
}