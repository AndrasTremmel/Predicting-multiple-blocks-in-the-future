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
#include <fstream>
#include <sstream>

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

class MultiBlockBTBContext {
 public:
  std::map<O3_CPU*, champsim::msl::lru_table<mbtb_entry_t>> MBTB;
  std::map<O3_CPU*, std::array<uint64_t, BTB_INDIRECT_SIZE>> INDIRECT_BTB;
  std::map<O3_CPU*, std::bitset<champsim::lg2(BTB_INDIRECT_SIZE)>> CONDITIONAL_HISTORY;
  std::map<O3_CPU*, std::deque<uint64_t>> RAS;
  std::map<O3_CPU*, std::deque<uint64_t>> SAS;
  std::map<O3_CPU*, std::array<uint64_t, CALL_SIZE_TRACKERS>> CALL_SIZE;
  std::map<O3_CPU*, uint64_t> LAST_BRANCH_IP;
  std::map<O3_CPU*, mbtb_transition> LAST_TRANSITION;
  std::map<O3_CPU*, std::deque<pending_mbtb_pred_t>> PRED_QUEUE;
  std::map<O3_CPU*, mbtb_stats_t> STATS;

  ~MultiBlockBTBContext() {
    print_all();
  }

  void print_all() {
    auto out = [&](const std::string& s) {
      std::cerr << s;
    };

    for (auto& [cpu, stats] : STATS) {
      (void)cpu;
      out("\n========== MULTI-BLOCK BTB STATISTICS ==========\n");
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
      out("\n--- MBTB-specific diagnostics ---\n");
      out("Transition hits T/N/R:    " + std::to_string(stats.transition_hits[0]) + " / "
          + std::to_string(stats.transition_hits[1]) + " / " + std::to_string(stats.transition_hits[2]) + "\n");
      out("Transition misses T/N/R:  " + std::to_string(stats.transition_misses[0]) + " / "
          + std::to_string(stats.transition_misses[1]) + " / " + std::to_string(stats.transition_misses[2]) + "\n");
      out("Correct transition type:  " + std::to_string(stats.correct_transition_type) + "\n");
      out("Wrong transition type:    " + std::to_string(stats.wrong_transition_type) + "\n");
      out("Prev IP match:            " + std::to_string(stats.prev_ip_match) + "\n");
      out("Prev IP mismatch:         " + std::to_string(stats.prev_ip_mismatch) + "\n");
      out("SAS pushes:               " + std::to_string(stats.sas_pushes) + "\n");
      out("SAS pops:                 " + std::to_string(stats.sas_pops) + "\n");
      out("SAS empty on pop:         " + std::to_string(stats.sas_empty_on_pop) + "\n");
      out("=================================================\n");
    }

    std::cerr << std::flush;
  }
};

static MultiBlockBTBContext g_ctx;

} // namespace

void O3_CPU::initialize_btb()
{
  g_ctx.MBTB.insert({this, champsim::msl::lru_table<mbtb_entry_t>{BTB_SET, BTB_WAY}});
  std::fill(std::begin(g_ctx.INDIRECT_BTB[this]), std::end(g_ctx.INDIRECT_BTB[this]), 0);
  std::fill(std::begin(g_ctx.CALL_SIZE[this]), std::end(g_ctx.CALL_SIZE[this]), 4);
  g_ctx.CONDITIONAL_HISTORY[this] = 0;
  g_ctx.LAST_BRANCH_IP[this] = 0;
  g_ctx.LAST_TRANSITION[this] = mbtb_transition::N;
  g_ctx.PRED_QUEUE[this].clear();
  g_ctx.STATS[this] = {};
}

std::pair<uint64_t, uint8_t> O3_CPU::btb_prediction(uint64_t ip)
{
  auto prev_ip = g_ctx.LAST_BRANCH_IP[this];
  auto trans   = g_ctx.LAST_TRANSITION[this];

  auto entry = g_ctx.MBTB.at(this).check_hit({prev_ip, 0, branch_info::ALWAYS_TAKEN, trans});

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
    else if (entry->type == branch_info::INDIRECT) {
      auto hash = (ip >> 2) ^ g_ctx.CONDITIONAL_HISTORY[this].to_ullong();
      predicted_target = g_ctx.INDIRECT_BTB[this][hash % std::size(g_ctx.INDIRECT_BTB[this])];
      always_taken = true;
    }
    else if (entry->type == branch_info::CONDITIONAL) {
      always_taken = false;
    }
    else {
      always_taken = true;
    }
  }

  g_ctx.PRED_QUEUE[this].push_back({ip, prev_ip, trans, predicted_target, pred_type, was_hit, ras_empty});
  return {predicted_target, always_taken};
}

void O3_CPU::update_btb(uint64_t ip, uint64_t branch_target, uint8_t taken, uint8_t branch_type)
{
  auto& stats = g_ctx.STATS[this];
  stats.total_updates++;
  if (taken) stats.taken_count++; else stats.not_taken_count++;

  auto& q = g_ctx.PRED_QUEUE[this];
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
    uint64_t actual_prev_ip = g_ctx.LAST_BRANCH_IP[this];
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

  if (branch_type == BRANCH_DIRECT_CALL || branch_type == BRANCH_INDIRECT_CALL) {
    g_ctx.RAS[this].push_back(ip);
    g_ctx.SAS[this].push_back(ip);
    stats.sas_pushes++;
    if (std::size(g_ctx.RAS[this]) > RAS_SIZE)
      g_ctx.RAS[this].pop_front();
    if (std::size(g_ctx.SAS[this]) > SAS_SIZE)
      g_ctx.SAS[this].pop_front();
  }

  if (branch_type == BRANCH_RETURN && !std::empty(g_ctx.RAS[this])) {
    auto call_ip = g_ctx.RAS[this].back();
    g_ctx.RAS[this].pop_back();

    if (!std::empty(g_ctx.SAS[this])) {
      g_ctx.SAS[this].pop_back();
      stats.sas_pops++;
    } else {
      stats.sas_empty_on_pop++;
    }

    auto estimated_size = std::abs((long)(call_ip - branch_target));
    if (estimated_size <= 10) {
      g_ctx.CALL_SIZE[this][call_ip % std::size(g_ctx.CALL_SIZE[this])] = estimated_size;
    }
  }

  if ((branch_type == BRANCH_INDIRECT) || (branch_type == BRANCH_INDIRECT_CALL)) {
    auto hash = (ip >> 2) ^ g_ctx.CONDITIONAL_HISTORY[this].to_ullong();
    g_ctx.INDIRECT_BTB[this][hash % std::size(g_ctx.INDIRECT_BTB[this])] = branch_target;
  }

  if ((branch_type == BRANCH_CONDITIONAL) || (branch_type == BRANCH_OTHER)) {
    g_ctx.CONDITIONAL_HISTORY[this] <<= 1;
    g_ctx.CONDITIONAL_HISTORY[this].set(0, taken);
  }

  auto type = branch_info::ALWAYS_TAKEN;
  if ((branch_type == BRANCH_INDIRECT) || (branch_type == BRANCH_INDIRECT_CALL))
    type = branch_info::INDIRECT;
  else if (branch_type == BRANCH_RETURN)
    type = branch_info::RETURN;
  else if ((branch_type == BRANCH_CONDITIONAL) || (branch_type == BRANCH_OTHER))
    type = branch_info::CONDITIONAL;

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

  auto prev_ip = g_ctx.LAST_BRANCH_IP[this];
  auto prev_trans = g_ctx.LAST_TRANSITION[this];

  auto opt_entry = g_ctx.MBTB.at(this).check_hit({prev_ip, branch_target, type, prev_trans});

  if (opt_entry.has_value()) {
    if (branch_target != 0) opt_entry->target = branch_target;
    opt_entry->type = type;
  }

  if (branch_target != 0) {
    g_ctx.MBTB.at(this).fill(
        opt_entry.value_or(mbtb_entry_t{prev_ip, branch_target, type, prev_trans})
    );
  }

  g_ctx.LAST_BRANCH_IP[this] = ip;
  g_ctx.LAST_TRANSITION[this] = trans;
}

