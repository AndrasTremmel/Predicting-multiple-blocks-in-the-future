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
  // Aggregate counters.
  uint64_t total_hits           = 0;
  uint64_t total_misses         = 0;
  uint64_t total_mispredictions = 0;
  uint64_t total_correct        = 0;

  // Per-branch-type breakdown (indexed by current branch's actual type).
  uint64_t per_type_hits          [N_BRANCH_TYPES] = {};
  uint64_t per_type_misses        [N_BRANCH_TYPES] = {};
  uint64_t per_type_mispredictions[N_BRANCH_TYPES] = {};
  uint64_t per_type_correct       [N_BRANCH_TYPES] = {};

  // Per-PREVIOUS-branch-type breakdown (indexed by the type of the branch
  // updated immediately before this one). The very first branch in the
  // trace has no predecessor and is therefore not counted in this split.
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
  std::map<O3_CPU*, uint64_t> CUR_FETCH_ADDR;   // start address of current fetch block
  std::map<O3_CPU*, bool>     NEW_FETCH_BLOCK;    // true => next prediction begins a new block

  // Type of the most recently UPDATED branch -- needed so that the next
  // update can record its prev-branch-type stats. (Stats-only state.)
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

} // namespace

void O3_CPU::initialize_btb()
{
  g_ctx.BTB.insert({this, champsim::msl::lru_table<btb_entry_t>{BTB_SET, BTB_WAY}});
  std::fill(std::begin(g_ctx.INDIRECT_BTB[this]), std::end(g_ctx.INDIRECT_BTB[this]), 0);
  std::fill(std::begin(g_ctx.CALL_SIZE[this]), std::end(g_ctx.CALL_SIZE[this]), 4);
  g_ctx.CONDITIONAL_HISTORY[this] = 0;
  g_ctx.PRED_QUEUE[this].clear();
  g_ctx.STATS[this] = {};
  g_ctx.CUR_FETCH_ADDR[this] = 0;
  g_ctx.NEW_FETCH_BLOCK[this] = true;
  g_ctx.LAST_BRANCH_TYPE_FOR_STATS[this] = 0;
  g_ctx.HAS_LAST_BRANCH[this] = false;
}

std::pair<uint64_t, uint8_t> O3_CPU::btb_prediction(uint64_t ip)
{
  // The first instruction fetched after a branch target has ip == fetch block start.
  if (g_ctx.NEW_FETCH_BLOCK[this]) {
    g_ctx.CUR_FETCH_ADDR[this] = ip;
    g_ctx.NEW_FETCH_BLOCK[this] = false;
  }
  auto fetch_addr = g_ctx.CUR_FETCH_ADDR[this];

  auto btb_entry = g_ctx.BTB.at(this).check_hit({ip, fetch_addr, 0, ::branch_info::ALWAYS_TAKEN});

  uint64_t predicted_target = 0;
  uint8_t  always_taken     = false;
  bool     was_hit          = false;

  if (btb_entry.has_value()) {
    was_hit = true;

    if (btb_entry->type == ::branch_info::RETURN) {
      if (std::empty(g_ctx.RAS[this])) {
        predicted_target = 0;
        always_taken = true;
      } else {
        auto target = g_ctx.RAS[this].back();
        auto size = g_ctx.CALL_SIZE[this][target % std::size(g_ctx.CALL_SIZE[this])];
        predicted_target = target + size;
        always_taken = true;
      }
    }
    else if (btb_entry->type == ::branch_info::INDIRECT) {
      auto hash = (ip) ^ g_ctx.CONDITIONAL_HISTORY[this].to_ullong();
      predicted_target = g_ctx.INDIRECT_BTB[this][hash % std::size(g_ctx.INDIRECT_BTB[this])];
      always_taken = true;
    }
    else {
      predicted_target = btb_entry->target;
      always_taken = (btb_entry->type != ::branch_info::CONDITIONAL);
    }
  }

  g_ctx.PRED_QUEUE[this].push_back({ip, fetch_addr, predicted_target, was_hit});
  return {predicted_target, always_taken};
}

void O3_CPU::update_btb(uint64_t ip, uint64_t branch_target, uint8_t taken, uint8_t branch_type)
{
  auto& stats = g_ctx.STATS[this];

  // ---- Drain PRED_QUEUE ------------------------------------------------
  auto& q = g_ctx.PRED_QUEUE[this];
  bool found = false;
  pending_btb_pred_t pred{};
  while (!q.empty()) {
    pred = q.front();
    q.pop_front();
    if (pred.ip == ip) { found = true; break; }
  }
  auto fetch_addr = g_ctx.CUR_FETCH_ADDR[this];

  // ---- Hit/miss + misprediction/correct accounting ---------------------
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

  // Per-previous-branch-type split (skips the very first branch).
  if (g_ctx.HAS_LAST_BRANCH[this]) {
    int pti = branch_type_idx(g_ctx.LAST_BRANCH_TYPE_FOR_STATS[this]);
    if (was_hit) stats.per_prev_type_hits[pti]++;
    else         stats.per_prev_type_misses[pti]++;
    if (is_misprediction) stats.per_prev_type_mispredictions[pti]++;
    else                  stats.per_prev_type_correct[pti]++;
  }

  // ---- RAS / state updates ---------------------------------
  if (branch_type == BRANCH_DIRECT_CALL || branch_type == BRANCH_INDIRECT_CALL) {
    g_ctx.RAS[this].push_back(ip);
    if (std::size(g_ctx.RAS[this]) > RAS_SIZE) g_ctx.RAS[this].pop_front();
  }
  if ((branch_type == BRANCH_INDIRECT) || (branch_type == BRANCH_INDIRECT_CALL)) {
    auto hash = (ip) ^ g_ctx.CONDITIONAL_HISTORY[this].to_ullong();
    g_ctx.INDIRECT_BTB[this][hash % std::size(g_ctx.INDIRECT_BTB[this])] = branch_target;
  }
  if ((branch_type == BRANCH_CONDITIONAL) || (branch_type == BRANCH_OTHER)) {
    g_ctx.CONDITIONAL_HISTORY[this] <<= 1;
    g_ctx.CONDITIONAL_HISTORY[this].set(0, taken);
  }
  if (branch_type == BRANCH_RETURN && !std::empty(g_ctx.RAS[this])) {
    auto call_ip = g_ctx.RAS[this].back();
    g_ctx.RAS[this].pop_back();
    auto estimated_call_instr_size = (call_ip > branch_target)
                                         ? call_ip - branch_target
                                         : branch_target - call_ip;
    if (estimated_call_instr_size <= 10) {
      g_ctx.CALL_SIZE[this][call_ip % std::size(g_ctx.CALL_SIZE[this])] = estimated_call_instr_size;
    }
  }

  auto type = ::branch_info::ALWAYS_TAKEN;
  if      ((branch_type == BRANCH_INDIRECT) || (branch_type == BRANCH_INDIRECT_CALL)) type = ::branch_info::INDIRECT;
  else if (branch_type == BRANCH_RETURN)                                              type = ::branch_info::RETURN;
  else if ((branch_type == BRANCH_CONDITIONAL) || (branch_type == BRANCH_OTHER))      type = ::branch_info::CONDITIONAL;

  auto opt_entry = g_ctx.BTB.at(this).check_hit({ip, fetch_addr, branch_target, type});
  if (opt_entry.has_value()) {
    opt_entry->type = type;
    if (branch_target != 0) opt_entry->target = branch_target;
  }
  if (branch_target != 0) {
    g_ctx.BTB.at(this).fill(opt_entry.value_or(::btb_entry_t{ip, fetch_addr, branch_target, type}));
  }

  // Next fetch will start from a new block (target or fall-through)
  g_ctx.NEW_FETCH_BLOCK[this] = true;

  // Roll forward stats-only state
  g_ctx.LAST_BRANCH_TYPE_FOR_STATS[this] = branch_type;
  g_ctx.HAS_LAST_BRANCH[this] = true;
}