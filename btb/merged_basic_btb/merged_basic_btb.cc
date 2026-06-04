#include <algorithm>
#include <array>
#include <bitset>
#include <cstdint>
#include <deque>
#include <iomanip>
#include <iostream>
#include <map>
#include <sstream>

#include "msl/lru_table.h"
#include "ooo_cpu.h"

// ============================================================================
// SINGLE-CYCLE BASIC BTB  (original small configuration)
// ============================================================================
namespace {
namespace sc_basic {

enum class branch_info {
  INDIRECT,
  RETURN,
  ALWAYS_TAKEN,
  CONDITIONAL,
};

constexpr std::size_t BTB_SET = 256;
constexpr std::size_t BTB_WAY = 4;
constexpr std::size_t BTB_INDIRECT_SIZE = 256;
constexpr std::size_t RAS_SIZE = 64;
constexpr std::size_t CALL_SIZE_TRACKERS = 256;

// -------- per-branch-type indexing -----------------------------------------
constexpr int N_BRANCH_TYPES = 7;
static int branch_type_idx(uint8_t bt) {
  switch (bt) {
    case BRANCH_DIRECT_JUMP:   return 0;
    case BRANCH_INDIRECT:      return 1;
    case BRANCH_CONDITIONAL:   return 2;
    case BRANCH_DIRECT_CALL:   return 3;
    case BRANCH_INDIRECT_CALL: return 4;
    case BRANCH_RETURN:        return 5;
    default:                   return 6; // BRANCH_OTHER / unknown
  }
}
static const char* const BRANCH_TYPE_NAMES[N_BRANCH_TYPES] = {
  "DIRECT_JUMP", "INDIRECT", "CONDITIONAL", "DIRECT_CALL",
  "INDIRECT_CALL", "RETURN", "OTHER"
};

struct btb_entry_t {
  uint64_t ip_tag = 0;
  uint64_t fetch_addr = 0;
  uint64_t target = 0;
  branch_info type = branch_info::ALWAYS_TAKEN;

  auto index() const { return fetch_addr; }
  auto tag() const { return fetch_addr; }
};

struct pending_btb_pred_t {
  uint64_t ip;
  uint64_t fetch_addr;
  uint64_t target;
  bool was_hit;
};

struct btb_stats_t {
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

class BaselineBTBContext {
 public:
  std::map<O3_CPU*, champsim::msl::lru_table<btb_entry_t>> BTB;
  std::map<O3_CPU*, std::array<uint64_t, BTB_INDIRECT_SIZE>> INDIRECT_BTB;
  std::map<O3_CPU*, std::bitset<champsim::lg2(BTB_INDIRECT_SIZE)>> CONDITIONAL_HISTORY;
  std::map<O3_CPU*, std::deque<uint64_t>> RAS;
  std::map<O3_CPU*, std::array<uint64_t, CALL_SIZE_TRACKERS>> CALL_SIZE;
  std::map<O3_CPU*, std::deque<pending_btb_pred_t>> PRED_QUEUE;
  std::map<O3_CPU*, btb_stats_t> STATS;
  std::map<O3_CPU*, uint64_t> CUR_FETCH_ADDR;
  std::map<O3_CPU*, bool>     NEW_FETCH_BLOCK;

  std::map<O3_CPU*, uint8_t> LAST_BRANCH_TYPE_FOR_STATS;
  std::map<O3_CPU*, bool>    HAS_LAST_BRANCH;

  ~BaselineBTBContext() { print_all(); }

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

      oss << "\n========== BASELINE BTB STATISTICS ==========\n";
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

      oss << "\n--- Per-branch-type breakdown ---\n";
      print_per_type(oss, "hits          ", stats.per_type_hits);
      print_per_type(oss, "misses        ", stats.per_type_misses);
      print_per_type(oss, "mispredictions", stats.per_type_mispredictions);
      print_per_type(oss, "correct       ", stats.per_type_correct);

      oss << "\n--- Per-previous-branch-type breakdown ---\n";
      print_per_type(oss, "hits          ", stats.per_prev_type_hits);
      print_per_type(oss, "misses        ", stats.per_prev_type_misses);
      print_per_type(oss, "mispredictions", stats.per_prev_type_mispredictions);
      print_per_type(oss, "correct       ", stats.per_prev_type_correct);

      oss << "=============================================\n";
      std::cerr << oss.str();
    }
    std::cerr << std::flush;
  }
};

static BaselineBTBContext g_ctx;

void initialize_btb(O3_CPU* cpu)
{
  g_ctx.BTB.insert({cpu, champsim::msl::lru_table<btb_entry_t>{BTB_SET, BTB_WAY}});
  std::fill(std::begin(g_ctx.INDIRECT_BTB[cpu]), std::end(g_ctx.INDIRECT_BTB[cpu]), 0);
  std::fill(std::begin(g_ctx.CALL_SIZE[cpu]), std::end(g_ctx.CALL_SIZE[cpu]), 4);
  g_ctx.CONDITIONAL_HISTORY[cpu] = 0;
  g_ctx.PRED_QUEUE[cpu].clear();
  g_ctx.STATS[cpu] = {};
  g_ctx.CUR_FETCH_ADDR[cpu] = 0;
  g_ctx.NEW_FETCH_BLOCK[cpu] = true;
  g_ctx.LAST_BRANCH_TYPE_FOR_STATS[cpu] = 0;
  g_ctx.HAS_LAST_BRANCH[cpu] = false;
}

std::pair<uint64_t, uint8_t> btb_prediction(O3_CPU* cpu, uint64_t ip)
{
  if (g_ctx.NEW_FETCH_BLOCK[cpu]) {
    g_ctx.CUR_FETCH_ADDR[cpu] = ip;
    g_ctx.NEW_FETCH_BLOCK[cpu] = false;
  }
  auto fetch_addr = g_ctx.CUR_FETCH_ADDR[cpu];

  auto btb_entry = g_ctx.BTB.at(cpu).check_hit({ip, fetch_addr, 0, branch_info::ALWAYS_TAKEN});

  uint64_t predicted_target = 0;
  uint8_t  always_taken     = false;
  bool     was_hit          = false;

  if (btb_entry.has_value()) {
    was_hit = true;

    if (btb_entry->type == branch_info::RETURN) {
      if (std::empty(g_ctx.RAS[cpu])) {
        predicted_target = 0;
        always_taken = true;
      } else {
        auto target = g_ctx.RAS[cpu].back();
        auto size = g_ctx.CALL_SIZE[cpu][target % std::size(g_ctx.CALL_SIZE[cpu])];
        predicted_target = target + size;
        always_taken = true;
      }
    }
    else if (btb_entry->type == branch_info::INDIRECT) {
      auto hash = (ip) ^ g_ctx.CONDITIONAL_HISTORY[cpu].to_ullong();
      predicted_target = g_ctx.INDIRECT_BTB[cpu][hash % std::size(g_ctx.INDIRECT_BTB[cpu])];
      always_taken = true;
    }
    else {
      predicted_target = btb_entry->target;
      always_taken = (btb_entry->type != branch_info::CONDITIONAL);
    }
  }

  g_ctx.PRED_QUEUE[cpu].push_back({ip, fetch_addr, predicted_target, was_hit});
  return {predicted_target, always_taken};
}

void update_btb(O3_CPU* cpu, uint64_t ip, uint64_t branch_target, uint8_t taken, uint8_t branch_type)
{
  auto& stats = g_ctx.STATS[cpu];

  auto& q = g_ctx.PRED_QUEUE[cpu];
  bool found = false;
  pending_btb_pred_t pred{};
  while (!q.empty()) {
    pred = q.front();
    q.pop_front();
    if (pred.ip == ip) { found = true; break; }
  }
  auto fetch_addr = g_ctx.CUR_FETCH_ADDR[cpu];

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
    if (std::size(g_ctx.RAS[cpu]) > RAS_SIZE) g_ctx.RAS[cpu].pop_front();
  }
  if ((branch_type == BRANCH_INDIRECT) || (branch_type == BRANCH_INDIRECT_CALL)) {
    auto hash = (ip) ^ g_ctx.CONDITIONAL_HISTORY[cpu].to_ullong();
    g_ctx.INDIRECT_BTB[cpu][hash % std::size(g_ctx.INDIRECT_BTB[cpu])] = branch_target;
  }
  if ((branch_type == BRANCH_CONDITIONAL) || (branch_type == BRANCH_OTHER)) {
    g_ctx.CONDITIONAL_HISTORY[cpu] <<= 1;
    g_ctx.CONDITIONAL_HISTORY[cpu].set(0, taken);
  }
  if (branch_type == BRANCH_RETURN && !std::empty(g_ctx.RAS[cpu])) {
    auto call_ip = g_ctx.RAS[cpu].back();
    g_ctx.RAS[cpu].pop_back();
    auto estimated_call_instr_size = (call_ip > branch_target)
                                       ? call_ip - branch_target
                                       : branch_target - call_ip;
    if (estimated_call_instr_size <= 10) {
      g_ctx.CALL_SIZE[cpu][call_ip % std::size(g_ctx.CALL_SIZE[cpu])] = estimated_call_instr_size;
    }
  }

  auto type = branch_info::ALWAYS_TAKEN;
  if      ((branch_type == BRANCH_INDIRECT) || (branch_type == BRANCH_INDIRECT_CALL)) type = branch_info::INDIRECT;
  else if (branch_type == BRANCH_RETURN)                                              type = branch_info::RETURN;
  else if ((branch_type == BRANCH_CONDITIONAL) || (branch_type == BRANCH_OTHER))      type = branch_info::CONDITIONAL;

  auto opt_entry = g_ctx.BTB.at(cpu).check_hit({ip, fetch_addr, branch_target, type});
  if (opt_entry.has_value()) {
    opt_entry->type = type;
    if (branch_target != 0) opt_entry->target = branch_target;
  }
  if (branch_target != 0) {
    g_ctx.BTB.at(cpu).fill(opt_entry.value_or(btb_entry_t{ip, fetch_addr, branch_target, type}));
  }

  g_ctx.NEW_FETCH_BLOCK[cpu] = true;

  g_ctx.LAST_BRANCH_TYPE_FOR_STATS[cpu] = branch_type;
  g_ctx.HAS_LAST_BRANCH[cpu] = true;
}

} // namespace sc_basic
} // anonymous namespace



// ============================================================================
// MULTI-CYCLE BASIC BTB  (original large configuration)
// ============================================================================
namespace {
namespace mc_basic {

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

struct btb_entry_t {
  uint64_t ip_tag = 0;
  uint64_t fetch_addr = 0;
  uint64_t target = 0;
  branch_info type = branch_info::ALWAYS_TAKEN;

  auto index() const { return fetch_addr; }
  auto tag() const { return fetch_addr; }
};

struct pending_btb_pred_t {
  uint64_t ip;
  uint64_t fetch_addr;
  uint64_t target;
  bool was_hit;
};

struct btb_stats_t {
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

class BaselineBTBContext {
 public:
  std::map<O3_CPU*, champsim::msl::lru_table<btb_entry_t>> BTB;
  std::map<O3_CPU*, std::array<uint64_t, BTB_INDIRECT_SIZE>> INDIRECT_BTB;
  std::map<O3_CPU*, std::bitset<champsim::lg2(BTB_INDIRECT_SIZE)>> CONDITIONAL_HISTORY;
  std::map<O3_CPU*, std::deque<uint64_t>> RAS;
  std::map<O3_CPU*, std::array<uint64_t, CALL_SIZE_TRACKERS>> CALL_SIZE;
  std::map<O3_CPU*, std::deque<pending_btb_pred_t>> PRED_QUEUE;
  std::map<O3_CPU*, btb_stats_t> STATS;
  std::map<O3_CPU*, uint64_t> CUR_FETCH_ADDR;
  std::map<O3_CPU*, bool>     NEW_FETCH_BLOCK;

  std::map<O3_CPU*, uint8_t> LAST_BRANCH_TYPE_FOR_STATS;
  std::map<O3_CPU*, bool>    HAS_LAST_BRANCH;

  ~BaselineBTBContext() { print_all(); }

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

      oss << "\n========== BASELINE BTB STATISTICS ==========\n";
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

      oss << "\n--- Per-branch-type breakdown ---\n";
      print_per_type(oss, "hits          ", stats.per_type_hits);
      print_per_type(oss, "misses        ", stats.per_type_misses);
      print_per_type(oss, "mispredictions", stats.per_type_mispredictions);
      print_per_type(oss, "correct       ", stats.per_type_correct);

      oss << "\n--- Per-previous-branch-type breakdown ---\n";
      print_per_type(oss, "hits          ", stats.per_prev_type_hits);
      print_per_type(oss, "misses        ", stats.per_prev_type_misses);
      print_per_type(oss, "mispredictions", stats.per_prev_type_mispredictions);
      print_per_type(oss, "correct       ", stats.per_prev_type_correct);

      oss << "=============================================\n";
      std::cerr << oss.str();
    }
    std::cerr << std::flush;
  }
};

static BaselineBTBContext g_ctx;

void initialize_btb(O3_CPU* cpu)
{
  g_ctx.BTB.insert({cpu, champsim::msl::lru_table<btb_entry_t>{BTB_SET, BTB_WAY}});
  std::fill(std::begin(g_ctx.INDIRECT_BTB[cpu]), std::end(g_ctx.INDIRECT_BTB[cpu]), 0);
  std::fill(std::begin(g_ctx.CALL_SIZE[cpu]), std::end(g_ctx.CALL_SIZE[cpu]), 4);
  g_ctx.CONDITIONAL_HISTORY[cpu] = 0;
  g_ctx.PRED_QUEUE[cpu].clear();
  g_ctx.STATS[cpu] = {};
  g_ctx.CUR_FETCH_ADDR[cpu] = 0;
  g_ctx.NEW_FETCH_BLOCK[cpu] = true;
  g_ctx.LAST_BRANCH_TYPE_FOR_STATS[cpu] = 0;
  g_ctx.HAS_LAST_BRANCH[cpu] = false;
}

std::pair<uint64_t, uint8_t> btb_prediction(O3_CPU* cpu, uint64_t ip)
{
  if (g_ctx.NEW_FETCH_BLOCK[cpu]) {
    g_ctx.CUR_FETCH_ADDR[cpu] = ip;
    g_ctx.NEW_FETCH_BLOCK[cpu] = false;
  }
  auto fetch_addr = g_ctx.CUR_FETCH_ADDR[cpu];

  auto btb_entry = g_ctx.BTB.at(cpu).check_hit({ip, fetch_addr, 0, branch_info::ALWAYS_TAKEN});

  uint64_t predicted_target = 0;
  uint8_t  always_taken     = false;
  bool     was_hit          = false;

  if (btb_entry.has_value()) {
    was_hit = true;

    if (btb_entry->type == branch_info::RETURN) {
      if (std::empty(g_ctx.RAS[cpu])) {
        predicted_target = 0;
        always_taken = true;
      } else {
        auto target = g_ctx.RAS[cpu].back();
        auto size = g_ctx.CALL_SIZE[cpu][target % std::size(g_ctx.CALL_SIZE[cpu])];
        predicted_target = target + size;
        always_taken = true;
      }
    }
    else if (btb_entry->type == branch_info::INDIRECT) {
      auto hash = (ip) ^ g_ctx.CONDITIONAL_HISTORY[cpu].to_ullong();
      predicted_target = g_ctx.INDIRECT_BTB[cpu][hash % std::size(g_ctx.INDIRECT_BTB[cpu])];
      always_taken = true;
    }
    else {
      predicted_target = btb_entry->target;
      always_taken = (btb_entry->type != branch_info::CONDITIONAL);
    }
  }

  g_ctx.PRED_QUEUE[cpu].push_back({ip, fetch_addr, predicted_target, was_hit});
  return {predicted_target, always_taken};
}

void update_btb(O3_CPU* cpu, uint64_t ip, uint64_t branch_target, uint8_t taken, uint8_t branch_type)
{
  auto& stats = g_ctx.STATS[cpu];

  auto& q = g_ctx.PRED_QUEUE[cpu];
  bool found = false;
  pending_btb_pred_t pred{};
  while (!q.empty()) {
    pred = q.front();
    q.pop_front();
    if (pred.ip == ip) { found = true; break; }
  }
  auto fetch_addr = g_ctx.CUR_FETCH_ADDR[cpu];

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
    if (std::size(g_ctx.RAS[cpu]) > RAS_SIZE) g_ctx.RAS[cpu].pop_front();
  }
  if ((branch_type == BRANCH_INDIRECT) || (branch_type == BRANCH_INDIRECT_CALL)) {
    auto hash = (ip) ^ g_ctx.CONDITIONAL_HISTORY[cpu].to_ullong();
    g_ctx.INDIRECT_BTB[cpu][hash % std::size(g_ctx.INDIRECT_BTB[cpu])] = branch_target;
  }
  if ((branch_type == BRANCH_CONDITIONAL) || (branch_type == BRANCH_OTHER)) {
    g_ctx.CONDITIONAL_HISTORY[cpu] <<= 1;
    g_ctx.CONDITIONAL_HISTORY[cpu].set(0, taken);
  }
  if (branch_type == BRANCH_RETURN && !std::empty(g_ctx.RAS[cpu])) {
    auto call_ip = g_ctx.RAS[cpu].back();
    g_ctx.RAS[cpu].pop_back();
    auto estimated_call_instr_size = (call_ip > branch_target)
                                       ? call_ip - branch_target
                                       : branch_target - call_ip;
    if (estimated_call_instr_size <= 10) {
      g_ctx.CALL_SIZE[cpu][call_ip % std::size(g_ctx.CALL_SIZE[cpu])] = estimated_call_instr_size;
    }
  }

  auto type = branch_info::ALWAYS_TAKEN;
  if      ((branch_type == BRANCH_INDIRECT) || (branch_type == BRANCH_INDIRECT_CALL)) type = branch_info::INDIRECT;
  else if (branch_type == BRANCH_RETURN)                                              type = branch_info::RETURN;
  else if ((branch_type == BRANCH_CONDITIONAL) || (branch_type == BRANCH_OTHER))      type = branch_info::CONDITIONAL;

  auto opt_entry = g_ctx.BTB.at(cpu).check_hit({ip, fetch_addr, branch_target, type});
  if (opt_entry.has_value()) {
    opt_entry->type = type;
    if (branch_target != 0) opt_entry->target = branch_target;
  }
  if (branch_target != 0) {
    g_ctx.BTB.at(cpu).fill(opt_entry.value_or(btb_entry_t{ip, fetch_addr, branch_target, type}));
  }

  g_ctx.NEW_FETCH_BLOCK[cpu] = true;

  g_ctx.LAST_BRANCH_TYPE_FOR_STATS[cpu] = branch_type;
  g_ctx.HAS_LAST_BRANCH[cpu] = true;
}

} // namespace mc_basic
} // anonymous namespace

// ============================================================================
// PUBLIC WRAPPER
// ============================================================================

void O3_CPU::initialize_btb()
{
  sc_basic::initialize_btb(this);
  mc_basic::initialize_btb(this);
}

std::pair<uint64_t, uint8_t> O3_CPU::btb_prediction(uint64_t ip)
{
  return sc_basic::btb_prediction(this, ip);
}

btb_prediction_pair O3_CPU::predict_btb_pair(uint64_t ip)
{
  auto single = sc_basic::btb_prediction(this, ip);
  auto multi  = mc_basic::btb_prediction(this, ip);
  return {single, multi};
}

void O3_CPU::update_btb(uint64_t ip, uint64_t branch_target, uint8_t taken, uint8_t branch_type)
{
  sc_basic::update_btb(this, ip, branch_target, taken, branch_type);
  mc_basic::update_btb(this, ip, branch_target, taken, branch_type);
}