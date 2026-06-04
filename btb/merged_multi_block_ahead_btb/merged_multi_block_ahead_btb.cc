#include <algorithm>
#include <array>
#include <bitset>
#include <cstdint>
#include <cstdlib>
#include <deque>
#include <iomanip>
#include <iostream>
#include <map>
#include <sstream>
#include <utility>

#include "msl/lru_table.h"
#include "ooo_cpu.h"

#define OPTIMIZATION_ON 0

// ============================================================================
// SINGLE-CYCLE MULTI-BLOCK BTB  (original small configuration)
// ============================================================================
namespace {
namespace sc_mbtb {

enum class branch_info {
  INDIRECT,
  RETURN,
  ALWAYS_TAKEN,
  CONDITIONAL,
};

enum class mbtb_transition : uint8_t { T, N, R };

struct mbtb_entry_t {
  uint64_t ip_tag = 0;
  uint64_t block_start = 0;
  uint64_t target = 0;
  branch_info type = branch_info::ALWAYS_TAKEN;
  mbtb_transition transition = mbtb_transition::N;
  uint64_t ind_ctx = 0;

  auto index() const { return block_start; }
  auto tag() const { return (ip_tag) ^ static_cast<uint64_t>(transition) ^ ind_ctx; }
};

struct sas_record_t {
  uint64_t target = 0;
  branch_info type = branch_info::ALWAYS_TAKEN;
  bool valid = false;
  uint64_t block_start = 0;
};

struct pending_mbtb_pred_t {
  uint64_t ip;
  uint64_t target;
  bool was_hit;
};

constexpr int N_BRANCH_TYPES = 7;
static int branch_type_idx(uint8_t bt) {
  switch (bt) {
    case BRANCH_DIRECT_JUMP:   return 0;
    case BRANCH_INDIRECT:      return 1;
    case BRANCH_CONDITIONAL:   return 2;
    case BRANCH_DIRECT_CALL:   return 3;
    case BRANCH_INDIRECT_CALL: return 4;
    case BRANCH_RETURN:        return 5;
    default:                   return 6;
  }
}
static const char* const BRANCH_TYPE_NAMES[N_BRANCH_TYPES] = {
  "DIRECT_JUMP", "INDIRECT", "CONDITIONAL", "DIRECT_CALL",
  "INDIRECT_CALL", "RETURN", "OTHER"
};

struct mbtb_stats_t {
  uint64_t total_hits           = 0;
  uint64_t total_misses         = 0;
  uint64_t total_mispredictions = 0;
  uint64_t total_correct        = 0;

  uint64_t per_type_hits          [N_BRANCH_TYPES] = {};
  uint64_t per_type_misses        [N_BRANCH_TYPES] = {};
  uint64_t per_type_mispredictions[N_BRANCH_TYPES] = {};
  uint64_t per_type_correct       [N_BRANCH_TYPES] = {};

  uint64_t per_prev_type_hits          [N_BRANCH_TYPES] = {};
  uint64_t per_prev_type_misses        [N_BRANCH_TYPES] = {};
  uint64_t per_prev_type_mispredictions[N_BRANCH_TYPES] = {};
  uint64_t per_prev_type_correct       [N_BRANCH_TYPES] = {};
};

constexpr std::size_t BTB_SET = 256;
constexpr std::size_t BTB_WAY = 4;
constexpr std::size_t BTB_INDIRECT_SIZE = 256;
constexpr std::size_t RAS_SIZE = 64;
constexpr std::size_t SAS_SIZE = 64;
constexpr std::size_t CALL_SIZE_TRACKERS = 256;

class MultiBlockBTBContext {
 public:
  std::map<O3_CPU*, champsim::msl::lru_table<mbtb_entry_t>> MBTB;
  std::map<O3_CPU*, std::array<uint64_t, BTB_INDIRECT_SIZE>> INDIRECT_BTB;
  std::map<O3_CPU*, std::bitset<champsim::lg2(BTB_INDIRECT_SIZE)>> CONDITIONAL_HISTORY;
  std::map<O3_CPU*, std::deque<uint64_t>> RAS;
  std::map<O3_CPU*, std::deque<sas_record_t>> SAS;
  std::map<O3_CPU*, std::array<uint64_t, CALL_SIZE_TRACKERS>> CALL_SIZE;
  std::map<O3_CPU*, uint64_t> LAST_BRANCH_IP;
  std::map<O3_CPU*, mbtb_transition> LAST_TRANSITION;
  std::map<O3_CPU*, bool> LAST_BRANCH_WAS_RETURN;
  std::map<O3_CPU*, uint64_t> LAST_RETURN_CALL_IP;
  std::map<O3_CPU*, sas_record_t> PENDING_SAS_ENTRY;
  std::map<O3_CPU*, std::deque<pending_mbtb_pred_t>> PRED_QUEUE;

  std::map<O3_CPU*, uint64_t> LAST_INDIRECT_TARGET;
  std::map<O3_CPU*, uint8_t> LAST_BRANCH_TYPE_FOR_STATS;
  std::map<O3_CPU*, bool>    HAS_LAST_BRANCH;
  std::map<O3_CPU*, mbtb_stats_t> STATS;

  std::map<O3_CPU*, uint64_t> CUR_BLOCK_START;
  std::map<O3_CPU*, bool>     NEW_BLOCK_PENDING;
  std::map<O3_CPU*, uint64_t> LAST_BLOCK_START_IP;

  ~MultiBlockBTBContext() { print_all(); }

  static void print_per_type(std::ostream& os,
                             const char* label,
                             const uint64_t (&arr)[N_BRANCH_TYPES]) {
    os << "  " << label << ":";
    for (int i = 0; i < N_BRANCH_TYPES; ++i)
      os << " " << BRANCH_TYPE_NAMES[i] << "=" << arr[i];
    os << "\n";
  }

  void print_all() {
    for (auto& [cpu, stats] : STATS) {
      (void)cpu;
      std::ostringstream oss;
      oss << std::fixed << std::setprecision(4);

      oss << "\n========== MULTI-BLOCK BTB STATISTICS ==========\n";
      oss << "Total hits:                " << stats.total_hits           << "\n";
      oss << "Total misses:              " << stats.total_misses         << "\n";
      oss << "Total mispredictions:      " << stats.total_mispredictions << "\n";
      oss << "Total correct predictions: " << stats.total_correct        << "\n";
      if ((stats.total_hits + stats.total_misses) > 0) {
        oss << "Hit rate:                  "
            << (100.0 * stats.total_hits / (stats.total_hits + stats.total_misses)) << "%\n";
      }
      if ((stats.total_correct + stats.total_mispredictions) > 0) {
        oss << "Prediction accuracy:       "
            << (100.0 * stats.total_correct
                / (stats.total_correct + stats.total_mispredictions)) << "%\n";
      }

      oss << "\n--- Per-branch-type breakdown (current branch) ---\n";
      print_per_type(oss, "hits          ", stats.per_type_hits);
      print_per_type(oss, "misses        ", stats.per_type_misses);
      print_per_type(oss, "mispredictions", stats.per_type_mispredictions);
      print_per_type(oss, "correct       ", stats.per_type_correct);

      oss << "\n--- Per-previous-branch-type breakdown ---\n";
      print_per_type(oss, "hits          ", stats.per_prev_type_hits);
      print_per_type(oss, "misses        ", stats.per_prev_type_misses);
      print_per_type(oss, "mispredictions", stats.per_prev_type_mispredictions);
      print_per_type(oss, "correct       ", stats.per_prev_type_correct);

      oss << "=================================================\n";
      std::cerr << oss.str();
    }
    std::cerr << std::flush;
  }
};

static MultiBlockBTBContext g_ctx;

void initialize_btb(O3_CPU* cpu)
{
  g_ctx.MBTB.insert({cpu, champsim::msl::lru_table<mbtb_entry_t>{BTB_SET, BTB_WAY}});
  std::fill(std::begin(g_ctx.INDIRECT_BTB[cpu]), std::end(g_ctx.INDIRECT_BTB[cpu]), 0);
  std::fill(std::begin(g_ctx.CALL_SIZE[cpu]), std::end(g_ctx.CALL_SIZE[cpu]), 4);
  g_ctx.CONDITIONAL_HISTORY[cpu] = 0;
  g_ctx.LAST_BRANCH_IP[cpu] = 0;
  g_ctx.LAST_INDIRECT_TARGET[cpu] = 0;
  g_ctx.LAST_TRANSITION[cpu] = mbtb_transition::N;
  g_ctx.LAST_BRANCH_WAS_RETURN[cpu] = false;
  g_ctx.LAST_RETURN_CALL_IP[cpu] = 0;
  g_ctx.PENDING_SAS_ENTRY[cpu] = sas_record_t{};
  g_ctx.RAS[cpu].clear();
  g_ctx.SAS[cpu].clear();
  g_ctx.PRED_QUEUE[cpu].clear();
  g_ctx.LAST_BRANCH_TYPE_FOR_STATS[cpu] = 0;
  g_ctx.HAS_LAST_BRANCH[cpu] = false;
  g_ctx.STATS[cpu] = {};
}

std::pair<uint64_t, uint8_t> btb_prediction(O3_CPU* cpu, uint64_t ip)
{
  if (g_ctx.NEW_BLOCK_PENDING[cpu]) {
    g_ctx.CUR_BLOCK_START[cpu]   = ip;
    g_ctx.NEW_BLOCK_PENDING[cpu] = false;
  }
  auto prev_block_start = g_ctx.LAST_BLOCK_START_IP[cpu];

  auto prev_ip   = g_ctx.LAST_BRANCH_IP[cpu];
  auto trans     = g_ctx.LAST_TRANSITION[cpu] == mbtb_transition::R
                       ? mbtb_transition::T : g_ctx.LAST_TRANSITION[cpu];
  bool was_ret   = g_ctx.LAST_BRANCH_WAS_RETURN[cpu];
  auto& pend_sas = g_ctx.PENDING_SAS_ENTRY[cpu];
  uint64_t pr_call_ip  = g_ctx.LAST_RETURN_CALL_IP[cpu];
  uint64_t ind_ctx = OPTIMIZATION_ON ? g_ctx.LAST_INDIRECT_TARGET[cpu] : 0;

  uint64_t predicted_target = 0;
  uint8_t  always_taken     = false;
  branch_info pred_type     = branch_info::ALWAYS_TAKEN;
  bool was_hit              = false;

  if (was_ret && pend_sas.valid) {
    was_hit = true;
    pred_type = pend_sas.type;
    predicted_target = pend_sas.target;
  } else {
    auto entry = g_ctx.MBTB.at(cpu).check_hit(
        {prev_ip, prev_block_start, 0, branch_info::ALWAYS_TAKEN, trans, ind_ctx});
    if (entry.has_value()) {
      was_hit = true;
      pred_type = entry->type;
      predicted_target = entry->target;
    }
  }

  if (was_hit) {
    if (pred_type == branch_info::RETURN) {
      if (std::empty(g_ctx.RAS[cpu])) {
        predicted_target = 0;
        always_taken = true;
      } else {
        auto target = g_ctx.RAS[cpu].back();
        auto size = g_ctx.CALL_SIZE[cpu][target % std::size(g_ctx.CALL_SIZE[cpu])];
        predicted_target = target + size;
        always_taken = true;
      }
    } else if (pred_type == branch_info::INDIRECT) {
      uint64_t hash = 0;
      if (was_ret && pend_sas.valid) {
        hash = (pr_call_ip) ^ g_ctx.CONDITIONAL_HISTORY[cpu].to_ullong() ^ static_cast<uint64_t>(mbtb_transition::R);
      } else {
        hash = ((prev_ip) ^ g_ctx.CONDITIONAL_HISTORY[cpu].to_ullong()) ^ static_cast<uint64_t>(trans);
      }
      predicted_target = g_ctx.INDIRECT_BTB[cpu][hash % std::size(g_ctx.INDIRECT_BTB[cpu])];
      always_taken = true;
    } else if (pred_type == branch_info::CONDITIONAL) {
      always_taken = false;
    } else {
      always_taken = true;
    }
  }

  g_ctx.PRED_QUEUE[cpu].push_back({ip, predicted_target, was_hit});
  return {predicted_target, always_taken};
}

void update_btb(O3_CPU* cpu, uint64_t ip, uint64_t branch_target, uint8_t taken, uint8_t branch_type)
{
  auto& stats = g_ctx.STATS[cpu];

  auto& q = g_ctx.PRED_QUEUE[cpu];
  bool found = false;
  pending_mbtb_pred_t pred{};
  while (!q.empty()) {
    pred = q.front();
    q.pop_front();
    if (pred.ip == ip) { found = true; break; }
  }

  mbtb_transition actual_trans;
  if (branch_type == BRANCH_DIRECT_CALL || branch_type == BRANCH_INDIRECT_CALL) {
    actual_trans = mbtb_transition::R;
  } else if (taken &&
             branch_type != BRANCH_RETURN &&
             branch_type != BRANCH_DIRECT_CALL &&
             branch_type != BRANCH_INDIRECT_CALL) {
    actual_trans = mbtb_transition::T;
  } else {
    actual_trans = mbtb_transition::N;
  }

  uint64_t cur_block_start  = g_ctx.CUR_BLOCK_START[cpu];
  uint64_t prev_block_start = g_ctx.LAST_BLOCK_START_IP[cpu];
  bool prev_was_return = g_ctx.LAST_BRANCH_WAS_RETURN[cpu];
  uint64_t pr_call_ip  = g_ctx.LAST_RETURN_CALL_IP[cpu];

  int  ti               = branch_type_idx(branch_type);
  bool was_hit          = found && pred.was_hit;
  bool target_required  = taken
                          || (branch_type != BRANCH_CONDITIONAL && branch_type != BRANCH_OTHER);
  bool target_correct   = was_hit && (pred.target == branch_target);
  bool is_misprediction = target_required && !target_correct;

  if (was_hit) {
    stats.total_hits++;
    stats.per_type_hits[ti]++;
  } else {
    stats.total_misses++;
    stats.per_type_misses[ti]++;
  }

  if (is_misprediction) {
    stats.total_mispredictions++;
    stats.per_type_mispredictions[ti]++;
  } else {
    stats.total_correct++;
    stats.per_type_correct[ti]++;
  }

  if (g_ctx.HAS_LAST_BRANCH[cpu]) {
    int pti = branch_type_idx(g_ctx.LAST_BRANCH_TYPE_FOR_STATS[cpu]);
    if (was_hit) stats.per_prev_type_hits[pti]++;
    else         stats.per_prev_type_misses[pti]++;
    if (is_misprediction) stats.per_prev_type_mispredictions[pti]++;
    else                  stats.per_prev_type_correct[pti]++;
  }

  if (branch_type == BRANCH_DIRECT_CALL || branch_type == BRANCH_INDIRECT_CALL) {
    g_ctx.RAS[cpu].push_back(ip);

    sas_record_t snap{};
    snap.block_start = cur_block_start;
    auto entry = g_ctx.MBTB.at(cpu).check_hit({ip, cur_block_start, 0, branch_info::ALWAYS_TAKEN, mbtb_transition::R, 0});
    if (entry.has_value()) {
      snap.target = entry->target;
      snap.type   = entry->type;
      snap.valid  = true;
    } else {
      snap.valid = false;
    }

    g_ctx.SAS[cpu].push_back(snap);

    if (std::size(g_ctx.RAS[cpu]) > RAS_SIZE) g_ctx.RAS[cpu].pop_front();
    if (std::size(g_ctx.SAS[cpu]) > SAS_SIZE) g_ctx.SAS[cpu].pop_front();
  }

  bool         just_handled_return = false;
  uint64_t     popped_call_ip      = 0;
  sas_record_t popped_sas{};

  if (branch_type == BRANCH_RETURN && !std::empty(g_ctx.RAS[cpu])) {
    popped_call_ip = g_ctx.RAS[cpu].back();
    g_ctx.RAS[cpu].pop_back();

    if (!std::empty(g_ctx.SAS[cpu])) {
      popped_sas = g_ctx.SAS[cpu].back();
      g_ctx.SAS[cpu].pop_back();
    } else {
      popped_sas.valid = false;
    }

    auto estimated_size = std::abs((long)(popped_call_ip - branch_target));
    if (estimated_size <= 10) {
      g_ctx.CALL_SIZE[cpu][popped_call_ip % std::size(g_ctx.CALL_SIZE[cpu])] = estimated_size;
    }

    just_handled_return = true;
  }

  auto prev_ip    = g_ctx.LAST_BRANCH_IP[cpu];
  auto prev_trans = g_ctx.LAST_TRANSITION[cpu] == mbtb_transition::R ? mbtb_transition::T : g_ctx.LAST_TRANSITION[cpu];
  auto& pend_sas = g_ctx.PENDING_SAS_ENTRY[cpu];

  if (branch_type == BRANCH_INDIRECT || branch_type == BRANCH_INDIRECT_CALL) {
    uint64_t hash = 0;
    if (prev_was_return && pend_sas.valid) {
      hash = (pr_call_ip) ^ g_ctx.CONDITIONAL_HISTORY[cpu].to_ullong() ^ static_cast<uint64_t>(mbtb_transition::R);
    } else {
      hash = ((prev_ip) ^ g_ctx.CONDITIONAL_HISTORY[cpu].to_ullong()) ^ static_cast<uint64_t>(prev_trans);
    }
    g_ctx.INDIRECT_BTB[cpu][hash % std::size(g_ctx.INDIRECT_BTB[cpu])]
        = branch_target;
  }

  if (branch_type == BRANCH_CONDITIONAL || branch_type == BRANCH_OTHER) {
    g_ctx.CONDITIONAL_HISTORY[cpu] <<= 1;
    g_ctx.CONDITIONAL_HISTORY[cpu].set(0, taken);
  }

  auto type = branch_info::ALWAYS_TAKEN;
  if      (branch_type == BRANCH_INDIRECT || branch_type == BRANCH_INDIRECT_CALL) type = branch_info::INDIRECT;
  else if (branch_type == BRANCH_RETURN)                                          type = branch_info::RETURN;
  else if (branch_type == BRANCH_CONDITIONAL || branch_type == BRANCH_OTHER)      type = branch_info::CONDITIONAL;

  if (prev_was_return) {
    if (branch_target != 0) {
      auto opt_entry = g_ctx.MBTB.at(cpu).check_hit({pr_call_ip, pend_sas.block_start, branch_target, type, mbtb_transition::R, 0});
      if (opt_entry.has_value()) {
        opt_entry->target = branch_target;
        opt_entry->type = type;
      }
      g_ctx.MBTB.at(cpu).fill(opt_entry.value_or(mbtb_entry_t{pr_call_ip, pend_sas.block_start, branch_target, type, mbtb_transition::R, 0}));
    }
  } else {
    prev_trans = g_ctx.LAST_TRANSITION[cpu] == mbtb_transition::R
                     ? mbtb_transition::T : g_ctx.LAST_TRANSITION[cpu];

    uint64_t ind_ctx = OPTIMIZATION_ON ? g_ctx.LAST_INDIRECT_TARGET[cpu] : 0;

    auto opt_entry = g_ctx.MBTB.at(cpu).check_hit({prev_ip, prev_block_start, branch_target, type, prev_trans, ind_ctx});
    if (opt_entry.has_value()) {
      if (branch_target != 0) opt_entry->target = branch_target;
      opt_entry->type = type;
    }
    if (branch_target != 0) {
      g_ctx.MBTB.at(cpu).fill(
          opt_entry.value_or(mbtb_entry_t{prev_ip, prev_block_start, branch_target, type, prev_trans, ind_ctx}));
    }
  }

  g_ctx.LAST_BRANCH_IP[cpu]  = ip;
  g_ctx.LAST_TRANSITION[cpu] = actual_trans;

  g_ctx.LAST_BLOCK_START_IP[cpu] = cur_block_start;
  g_ctx.NEW_BLOCK_PENDING[cpu]   = true;

  if (just_handled_return) {
    g_ctx.LAST_BRANCH_WAS_RETURN[cpu] = true;
    g_ctx.LAST_RETURN_CALL_IP[cpu]    = popped_call_ip;
    g_ctx.PENDING_SAS_ENTRY[cpu]      = popped_sas;
  } else {
    g_ctx.LAST_BRANCH_WAS_RETURN[cpu]  = false;
    g_ctx.PENDING_SAS_ENTRY[cpu].valid = false;
  }

  if (branch_type == BRANCH_INDIRECT || branch_type == BRANCH_INDIRECT_CALL) {
    g_ctx.LAST_INDIRECT_TARGET[cpu] = branch_target;
  } else {
    g_ctx.LAST_INDIRECT_TARGET[cpu] = 0;
  }

  g_ctx.LAST_BRANCH_TYPE_FOR_STATS[cpu] = branch_type;
  g_ctx.HAS_LAST_BRANCH[cpu] = true;
}

} // namespace sc_mbtb
} // anonymous namespace


// ============================================================================
// MULTI-CYCLE MULTI-BLOCK BTB  (original large configuration)
// ============================================================================
namespace {
namespace mc_mbtb {

enum class branch_info {
  INDIRECT,
  RETURN,
  ALWAYS_TAKEN,
  CONDITIONAL,
};

enum class mbtb_transition : uint8_t { T, N, R };

struct mbtb_entry_t {
  uint64_t ip_tag = 0;
  uint64_t block_start = 0;
  uint64_t target = 0;
  branch_info type = branch_info::ALWAYS_TAKEN;
  mbtb_transition transition = mbtb_transition::N;
  uint64_t ind_ctx = 0;

  auto index() const { return block_start; }
  auto tag() const { return (ip_tag) ^ static_cast<uint64_t>(transition) ^ ind_ctx; }
};

struct sas_record_t {
  uint64_t target = 0;
  branch_info type = branch_info::ALWAYS_TAKEN;
  bool valid = false;
  uint64_t block_start = 0;
};

struct pending_mbtb_pred_t {
  uint64_t ip;
  uint64_t target;
  bool was_hit;
};

constexpr int N_BRANCH_TYPES = 7;
static int branch_type_idx(uint8_t bt) {
  switch (bt) {
    case BRANCH_DIRECT_JUMP:   return 0;
    case BRANCH_INDIRECT:      return 1;
    case BRANCH_CONDITIONAL:   return 2;
    case BRANCH_DIRECT_CALL:   return 3;
    case BRANCH_INDIRECT_CALL: return 4;
    case BRANCH_RETURN:        return 5;
    default:                   return 6;
  }
}
static const char* const BRANCH_TYPE_NAMES[N_BRANCH_TYPES] = {
  "DIRECT_JUMP", "INDIRECT", "CONDITIONAL", "DIRECT_CALL",
  "INDIRECT_CALL", "RETURN", "OTHER"
};

struct mbtb_stats_t {
  uint64_t total_hits           = 0;
  uint64_t total_misses         = 0;
  uint64_t total_mispredictions = 0;
  uint64_t total_correct        = 0;

  uint64_t per_type_hits          [N_BRANCH_TYPES] = {};
  uint64_t per_type_misses        [N_BRANCH_TYPES] = {};
  uint64_t per_type_mispredictions[N_BRANCH_TYPES] = {};
  uint64_t per_type_correct       [N_BRANCH_TYPES] = {};

  uint64_t per_prev_type_hits          [N_BRANCH_TYPES] = {};
  uint64_t per_prev_type_misses        [N_BRANCH_TYPES] = {};
  uint64_t per_prev_type_mispredictions[N_BRANCH_TYPES] = {};
  uint64_t per_prev_type_correct       [N_BRANCH_TYPES] = {};
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
  std::map<O3_CPU*, std::deque<sas_record_t>> SAS;
  std::map<O3_CPU*, std::array<uint64_t, CALL_SIZE_TRACKERS>> CALL_SIZE;
  std::map<O3_CPU*, uint64_t> LAST_BRANCH_IP;
  std::map<O3_CPU*, mbtb_transition> LAST_TRANSITION;
  std::map<O3_CPU*, bool> LAST_BRANCH_WAS_RETURN;
  std::map<O3_CPU*, uint64_t> LAST_RETURN_CALL_IP;
  std::map<O3_CPU*, sas_record_t> PENDING_SAS_ENTRY;
  std::map<O3_CPU*, std::deque<pending_mbtb_pred_t>> PRED_QUEUE;

  std::map<O3_CPU*, uint64_t> LAST_INDIRECT_TARGET;
  std::map<O3_CPU*, uint8_t> LAST_BRANCH_TYPE_FOR_STATS;
  std::map<O3_CPU*, bool>    HAS_LAST_BRANCH;
  std::map<O3_CPU*, mbtb_stats_t> STATS;

  std::map<O3_CPU*, uint64_t> CUR_BLOCK_START;
  std::map<O3_CPU*, bool>     NEW_BLOCK_PENDING;
  std::map<O3_CPU*, uint64_t> LAST_BLOCK_START_IP;

  ~MultiBlockBTBContext() { print_all(); }

  static void print_per_type(std::ostream& os,
                             const char* label,
                             const uint64_t (&arr)[N_BRANCH_TYPES]) {
    os << "  " << label << ":";
    for (int i = 0; i < N_BRANCH_TYPES; ++i)
      os << " " << BRANCH_TYPE_NAMES[i] << "=" << arr[i];
    os << "\n";
  }

  void print_all() {
    for (auto& [cpu, stats] : STATS) {
      (void)cpu;
      std::ostringstream oss;
      oss << std::fixed << std::setprecision(4);

      oss << "\n========== MULTI-BLOCK BTB STATISTICS ==========\n";
      oss << "Total hits:                " << stats.total_hits           << "\n";
      oss << "Total misses:              " << stats.total_misses         << "\n";
      oss << "Total mispredictions:      " << stats.total_mispredictions << "\n";
      oss << "Total correct predictions: " << stats.total_correct        << "\n";
      if ((stats.total_hits + stats.total_misses) > 0) {
        oss << "Hit rate:                  "
            << (100.0 * stats.total_hits / (stats.total_hits + stats.total_misses)) << "%\n";
      }
      if ((stats.total_correct + stats.total_mispredictions) > 0) {
        oss << "Prediction accuracy:       "
            << (100.0 * stats.total_correct
                / (stats.total_correct + stats.total_mispredictions)) << "%\n";
      }

      oss << "\n--- Per-branch-type breakdown (current branch) ---\n";
      print_per_type(oss, "hits          ", stats.per_type_hits);
      print_per_type(oss, "misses        ", stats.per_type_misses);
      print_per_type(oss, "mispredictions", stats.per_type_mispredictions);
      print_per_type(oss, "correct       ", stats.per_type_correct);

      oss << "\n--- Per-previous-branch-type breakdown ---\n";
      print_per_type(oss, "hits          ", stats.per_prev_type_hits);
      print_per_type(oss, "misses        ", stats.per_prev_type_misses);
      print_per_type(oss, "mispredictions", stats.per_prev_type_mispredictions);
      print_per_type(oss, "correct       ", stats.per_prev_type_correct);

      oss << "=================================================\n";
      std::cerr << oss.str();
    }
    std::cerr << std::flush;
  }
};

static MultiBlockBTBContext g_ctx;

void initialize_btb(O3_CPU* cpu)
{
  g_ctx.MBTB.insert({cpu, champsim::msl::lru_table<mbtb_entry_t>{BTB_SET, BTB_WAY}});
  std::fill(std::begin(g_ctx.INDIRECT_BTB[cpu]), std::end(g_ctx.INDIRECT_BTB[cpu]), 0);
  std::fill(std::begin(g_ctx.CALL_SIZE[cpu]), std::end(g_ctx.CALL_SIZE[cpu]), 4);
  g_ctx.CONDITIONAL_HISTORY[cpu] = 0;
  g_ctx.LAST_BRANCH_IP[cpu] = 0;
  g_ctx.LAST_INDIRECT_TARGET[cpu] = 0;
  g_ctx.LAST_TRANSITION[cpu] = mbtb_transition::N;
  g_ctx.LAST_BRANCH_WAS_RETURN[cpu] = false;
  g_ctx.LAST_RETURN_CALL_IP[cpu] = 0;
  g_ctx.PENDING_SAS_ENTRY[cpu] = sas_record_t{};
  g_ctx.RAS[cpu].clear();
  g_ctx.SAS[cpu].clear();
  g_ctx.PRED_QUEUE[cpu].clear();
  g_ctx.LAST_BRANCH_TYPE_FOR_STATS[cpu] = 0;
  g_ctx.HAS_LAST_BRANCH[cpu] = false;
  g_ctx.STATS[cpu] = {};
}

std::pair<uint64_t, uint8_t> btb_prediction(O3_CPU* cpu, uint64_t ip)
{
  if (g_ctx.NEW_BLOCK_PENDING[cpu]) {
    g_ctx.CUR_BLOCK_START[cpu]   = ip;
    g_ctx.NEW_BLOCK_PENDING[cpu] = false;
  }
  auto prev_block_start = g_ctx.LAST_BLOCK_START_IP[cpu];

  auto prev_ip   = g_ctx.LAST_BRANCH_IP[cpu];
  auto trans     = g_ctx.LAST_TRANSITION[cpu] == mbtb_transition::R
                       ? mbtb_transition::T : g_ctx.LAST_TRANSITION[cpu];
  bool was_ret   = g_ctx.LAST_BRANCH_WAS_RETURN[cpu];
  auto& pend_sas = g_ctx.PENDING_SAS_ENTRY[cpu];
  uint64_t pr_call_ip  = g_ctx.LAST_RETURN_CALL_IP[cpu];
  uint64_t ind_ctx = OPTIMIZATION_ON ? g_ctx.LAST_INDIRECT_TARGET[cpu] : 0;

  uint64_t predicted_target = 0;
  uint8_t  always_taken     = false;
  branch_info pred_type     = branch_info::ALWAYS_TAKEN;
  bool was_hit              = false;

  if (was_ret && pend_sas.valid) {
    was_hit = true;
    pred_type = pend_sas.type;
    predicted_target = pend_sas.target;
  } else {
    auto entry = g_ctx.MBTB.at(cpu).check_hit(
        {prev_ip, prev_block_start, 0, branch_info::ALWAYS_TAKEN, trans, ind_ctx});
    if (entry.has_value()) {
      was_hit = true;
      pred_type = entry->type;
      predicted_target = entry->target;
    }
  }

  if (was_hit) {
    if (pred_type == branch_info::RETURN) {
      if (std::empty(g_ctx.RAS[cpu])) {
        predicted_target = 0;
        always_taken = true;
      } else {
        auto target = g_ctx.RAS[cpu].back();
        auto size = g_ctx.CALL_SIZE[cpu][target % std::size(g_ctx.CALL_SIZE[cpu])];
        predicted_target = target + size;
        always_taken = true;
      }
    } else if (pred_type == branch_info::INDIRECT) {
      uint64_t hash = 0;
      if (was_ret && pend_sas.valid) {
        hash = (pr_call_ip) ^ g_ctx.CONDITIONAL_HISTORY[cpu].to_ullong() ^ static_cast<uint64_t>(mbtb_transition::R);
      } else {
        hash = ((prev_ip) ^ g_ctx.CONDITIONAL_HISTORY[cpu].to_ullong()) ^ static_cast<uint64_t>(trans);
      }
      predicted_target = g_ctx.INDIRECT_BTB[cpu][hash % std::size(g_ctx.INDIRECT_BTB[cpu])];
      always_taken = true;
    } else if (pred_type == branch_info::CONDITIONAL) {
      always_taken = false;
    } else {
      always_taken = true;
    }
  }

  g_ctx.PRED_QUEUE[cpu].push_back({ip, predicted_target, was_hit});
  return {predicted_target, always_taken};
}

void update_btb(O3_CPU* cpu, uint64_t ip, uint64_t branch_target, uint8_t taken, uint8_t branch_type)
{
  auto& stats = g_ctx.STATS[cpu];

  auto& q = g_ctx.PRED_QUEUE[cpu];
  bool found = false;
  pending_mbtb_pred_t pred{};
  while (!q.empty()) {
    pred = q.front();
    q.pop_front();
    if (pred.ip == ip) { found = true; break; }
  }

  mbtb_transition actual_trans;
  if (branch_type == BRANCH_DIRECT_CALL || branch_type == BRANCH_INDIRECT_CALL) {
    actual_trans = mbtb_transition::R;
  } else if (taken &&
             branch_type != BRANCH_RETURN &&
             branch_type != BRANCH_DIRECT_CALL &&
             branch_type != BRANCH_INDIRECT_CALL) {
    actual_trans = mbtb_transition::T;
  } else {
    actual_trans = mbtb_transition::N;
  }

  uint64_t cur_block_start  = g_ctx.CUR_BLOCK_START[cpu];
  uint64_t prev_block_start = g_ctx.LAST_BLOCK_START_IP[cpu];
  bool prev_was_return = g_ctx.LAST_BRANCH_WAS_RETURN[cpu];
  uint64_t pr_call_ip  = g_ctx.LAST_RETURN_CALL_IP[cpu];

  int  ti               = branch_type_idx(branch_type);
  bool was_hit          = found && pred.was_hit;
  bool target_required  = taken
                          || (branch_type != BRANCH_CONDITIONAL && branch_type != BRANCH_OTHER);
  bool target_correct   = was_hit && (pred.target == branch_target);
  bool is_misprediction = target_required && !target_correct;

  if (was_hit) {
    stats.total_hits++;
    stats.per_type_hits[ti]++;
  } else {
    stats.total_misses++;
    stats.per_type_misses[ti]++;
  }

  if (is_misprediction) {
    stats.total_mispredictions++;
    stats.per_type_mispredictions[ti]++;
  } else {
    stats.total_correct++;
    stats.per_type_correct[ti]++;
  }

  if (g_ctx.HAS_LAST_BRANCH[cpu]) {
    int pti = branch_type_idx(g_ctx.LAST_BRANCH_TYPE_FOR_STATS[cpu]);
    if (was_hit) stats.per_prev_type_hits[pti]++;
    else         stats.per_prev_type_misses[pti]++;
    if (is_misprediction) stats.per_prev_type_mispredictions[pti]++;
    else                  stats.per_prev_type_correct[pti]++;
  }

  if (branch_type == BRANCH_DIRECT_CALL || branch_type == BRANCH_INDIRECT_CALL) {
    g_ctx.RAS[cpu].push_back(ip);

    sas_record_t snap{};
    snap.block_start = cur_block_start;
    auto entry = g_ctx.MBTB.at(cpu).check_hit({ip, cur_block_start, 0, branch_info::ALWAYS_TAKEN, mbtb_transition::R, 0});
    if (entry.has_value()) {
      snap.target = entry->target;
      snap.type   = entry->type;
      snap.valid  = true;
    } else {
      snap.valid = false;
    }

    g_ctx.SAS[cpu].push_back(snap);

    if (std::size(g_ctx.RAS[cpu]) > RAS_SIZE) g_ctx.RAS[cpu].pop_front();
    if (std::size(g_ctx.SAS[cpu]) > SAS_SIZE) g_ctx.SAS[cpu].pop_front();
  }

  bool         just_handled_return = false;
  uint64_t     popped_call_ip      = 0;
  sas_record_t popped_sas{};

  if (branch_type == BRANCH_RETURN && !std::empty(g_ctx.RAS[cpu])) {
    popped_call_ip = g_ctx.RAS[cpu].back();
    g_ctx.RAS[cpu].pop_back();

    if (!std::empty(g_ctx.SAS[cpu])) {
      popped_sas = g_ctx.SAS[cpu].back();
      g_ctx.SAS[cpu].pop_back();
    } else {
      popped_sas.valid = false;
    }

    auto estimated_size = std::abs((long)(popped_call_ip - branch_target));
    if (estimated_size <= 10) {
      g_ctx.CALL_SIZE[cpu][popped_call_ip % std::size(g_ctx.CALL_SIZE[cpu])] = estimated_size;
    }

    just_handled_return = true;
  }

  auto prev_ip    = g_ctx.LAST_BRANCH_IP[cpu];
  auto prev_trans = g_ctx.LAST_TRANSITION[cpu] == mbtb_transition::R ? mbtb_transition::T : g_ctx.LAST_TRANSITION[cpu];
  auto& pend_sas = g_ctx.PENDING_SAS_ENTRY[cpu];

  if (branch_type == BRANCH_INDIRECT || branch_type == BRANCH_INDIRECT_CALL) {
    uint64_t hash = 0;
    if (prev_was_return && pend_sas.valid) {
      hash = (pr_call_ip) ^ g_ctx.CONDITIONAL_HISTORY[cpu].to_ullong() ^ static_cast<uint64_t>(mbtb_transition::R);
    } else {
      hash = ((prev_ip) ^ g_ctx.CONDITIONAL_HISTORY[cpu].to_ullong()) ^ static_cast<uint64_t>(prev_trans);
    }
    g_ctx.INDIRECT_BTB[cpu][hash % std::size(g_ctx.INDIRECT_BTB[cpu])]
        = branch_target;
  }

  if (branch_type == BRANCH_CONDITIONAL || branch_type == BRANCH_OTHER) {
    g_ctx.CONDITIONAL_HISTORY[cpu] <<= 1;
    g_ctx.CONDITIONAL_HISTORY[cpu].set(0, taken);
  }

  auto type = branch_info::ALWAYS_TAKEN;
  if      (branch_type == BRANCH_INDIRECT || branch_type == BRANCH_INDIRECT_CALL) type = branch_info::INDIRECT;
  else if (branch_type == BRANCH_RETURN)                                          type = branch_info::RETURN;
  else if (branch_type == BRANCH_CONDITIONAL || branch_type == BRANCH_OTHER)      type = branch_info::CONDITIONAL;

  if (prev_was_return) {
    if (branch_target != 0) {
      auto opt_entry = g_ctx.MBTB.at(cpu).check_hit({pr_call_ip, pend_sas.block_start, branch_target, type, mbtb_transition::R, 0});
      if (opt_entry.has_value()) {
        opt_entry->target = branch_target;
        opt_entry->type = type;
      }
      g_ctx.MBTB.at(cpu).fill(opt_entry.value_or(mbtb_entry_t{pr_call_ip, pend_sas.block_start, branch_target, type, mbtb_transition::R, 0}));
    }
  } else {
    prev_trans = g_ctx.LAST_TRANSITION[cpu] == mbtb_transition::R
                     ? mbtb_transition::T : g_ctx.LAST_TRANSITION[cpu];

    uint64_t ind_ctx = OPTIMIZATION_ON ? g_ctx.LAST_INDIRECT_TARGET[cpu] : 0;

    auto opt_entry = g_ctx.MBTB.at(cpu).check_hit({prev_ip, prev_block_start, branch_target, type, prev_trans, ind_ctx});
    if (opt_entry.has_value()) {
      if (branch_target != 0) opt_entry->target = branch_target;
      opt_entry->type = type;
    }
    if (branch_target != 0) {
      g_ctx.MBTB.at(cpu).fill(
          opt_entry.value_or(mbtb_entry_t{prev_ip, prev_block_start, branch_target, type, prev_trans, ind_ctx}));
    }
  }

  g_ctx.LAST_BRANCH_IP[cpu]  = ip;
  g_ctx.LAST_TRANSITION[cpu] = actual_trans;

  g_ctx.LAST_BLOCK_START_IP[cpu] = cur_block_start;
  g_ctx.NEW_BLOCK_PENDING[cpu]   = true;

  if (just_handled_return) {
    g_ctx.LAST_BRANCH_WAS_RETURN[cpu] = true;
    g_ctx.LAST_RETURN_CALL_IP[cpu]    = popped_call_ip;
    g_ctx.PENDING_SAS_ENTRY[cpu]      = popped_sas;
  } else {
    g_ctx.LAST_BRANCH_WAS_RETURN[cpu]  = false;
    g_ctx.PENDING_SAS_ENTRY[cpu].valid = false;
  }

  if (branch_type == BRANCH_INDIRECT || branch_type == BRANCH_INDIRECT_CALL) {
    g_ctx.LAST_INDIRECT_TARGET[cpu] = branch_target;
  } else {
    g_ctx.LAST_INDIRECT_TARGET[cpu] = 0;
  }

  g_ctx.LAST_BRANCH_TYPE_FOR_STATS[cpu] = branch_type;
  g_ctx.HAS_LAST_BRANCH[cpu] = true;
}

} // namespace mc_mbtb
} // anonymous namespace

// ============================================================================
// PUBLIC WRAPPER
// ============================================================================

void O3_CPU::initialize_btb()
{
  sc_mbtb::initialize_btb(this);
  mc_mbtb::initialize_btb(this);
}

std::pair<uint64_t, uint8_t> O3_CPU::btb_prediction(uint64_t ip)
{
  return sc_mbtb::btb_prediction(this, ip);
}

btb_prediction_pair O3_CPU::predict_btb_pair(uint64_t ip)
{
  auto single = sc_mbtb::btb_prediction(this, ip);
  auto multi  = mc_mbtb::btb_prediction(this, ip);
  return {single, multi};
}

void O3_CPU::update_btb(uint64_t ip, uint64_t branch_target, uint8_t taken, uint8_t branch_type)
{
  sc_mbtb::update_btb(this, ip, branch_target, taken, branch_type);
  mc_mbtb::update_btb(this, ip, branch_target, taken, branch_type);
}