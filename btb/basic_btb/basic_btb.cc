// #include <algorithm>
// #include <array>
// #include <bitset>
// #include <deque>
// #include <map>
// #include <iostream>
// #include <iomanip>
// #include <sstream>
// #include <fstream>
// #include <cstdlib>
// #include <cstdint>

// #include "msl/lru_table.h"
// #include "ooo_cpu.h"

// namespace
// {
// enum class branch_info {
//   INDIRECT,
//   RETURN,
//   ALWAYS_TAKEN,
//   CONDITIONAL,
// };

// constexpr std::size_t BTB_SET = 2048;
// constexpr std::size_t BTB_WAY = 4;
// constexpr std::size_t BTB_INDIRECT_SIZE = 4096;
// constexpr std::size_t RAS_SIZE = 64;
// constexpr std::size_t CALL_SIZE_TRACKERS = 1024;

// // -------- per-actual-type indexing helpers (SHARED with MBTB file) --------
// constexpr int N_ACTUAL_TYPES = 7;
// static int actual_type_idx(uint8_t bt) {
//   switch (bt) {
//     case BRANCH_DIRECT_JUMP:   return 0;
//     case BRANCH_INDIRECT:      return 1;
//     case BRANCH_CONDITIONAL:   return 2;
//     case BRANCH_DIRECT_CALL:   return 3;
//     case BRANCH_INDIRECT_CALL: return 4;
//     case BRANCH_RETURN:        return 5;
//     default:                   return 6; // BRANCH_OTHER / unknown
//   }
// }
// static const char* const ACTUAL_TYPE_NAMES[N_ACTUAL_TYPES] = {
//   "DIRECT_JUMP", "INDIRECT", "CONDITIONAL", "DIRECT_CALL",
//   "INDIRECT_CALL", "RETURN", "OTHER"
// };

// // "What type would update_btb assign to this branch_type?"
// // Used so we can detect "predicted-type matched the type the BTB would have
// // written for this branch" without depending on the slot's stored type.
// static branch_info expected_info(uint8_t bt) {
//   if (bt == BRANCH_INDIRECT || bt == BRANCH_INDIRECT_CALL) return branch_info::INDIRECT;
//   if (bt == BRANCH_RETURN)                                 return branch_info::RETURN;
//   if (bt == BRANCH_CONDITIONAL || bt == BRANCH_OTHER)      return branch_info::CONDITIONAL;
//   return branch_info::ALWAYS_TAKEN;
// }

// struct btb_entry_t {
//   uint64_t ip_tag = 0;
//   uint64_t target = 0;
//   branch_info type = branch_info::ALWAYS_TAKEN;

//   auto index() const { return ip_tag; }
//   auto tag() const { return ip_tag; }

// };

// struct pending_btb_pred_t {
//   uint64_t ip;
//   uint64_t target;
//   branch_info type;
//   bool was_hit;
//   bool ras_empty;
// };

// struct btb_stats_t {
//   // ============ LEGACY COUNTERS (preserved verbatim) ============
//   uint64_t total_updates = 0;
//   uint64_t taken_count = 0;
//   uint64_t not_taken_count = 0;
//   uint64_t total_hits = 0;
//   uint64_t total_misses = 0;
//   uint64_t target_correct = 0;
//   uint64_t target_wrong = 0;
//   uint64_t conditional_hits = 0;
//   uint64_t conditional_misses = 0;
//   uint64_t indirect_hits = 0;
//   uint64_t indirect_misses = 0;
//   uint64_t return_hits = 0;
//   uint64_t return_misses = 0;
//   uint64_t always_taken_hits = 0;
//   uint64_t always_taken_misses = 0;
//   uint64_t ras_hits = 0;
//   uint64_t ras_misses = 0;
//   uint64_t ras_target_correct = 0;
//   uint64_t ras_target_wrong = 0;
//   uint64_t indirect_target_correct = 0;
//   uint64_t indirect_target_wrong = 0;
//   uint64_t btb_target_correct = 0;
//   uint64_t btb_target_wrong = 0;

//   // ============ NEW: per-ACTUAL-type counters (unambiguous) ============
//   uint64_t actual_total[N_ACTUAL_TYPES]               = {};
//   uint64_t actual_btb_hit[N_ACTUAL_TYPES]             = {};
//   uint64_t actual_btb_miss[N_ACTUAL_TYPES]            = {};
//   uint64_t actual_type_match[N_ACTUAL_TYPES]          = {};
//   uint64_t actual_type_mismatch[N_ACTUAL_TYPES]       = {};
//   uint64_t actual_target_correct[N_ACTUAL_TYPES]      = {};
//   uint64_t actual_target_wrong[N_ACTUAL_TYPES]        = {};
//   uint64_t actual_target_dontcare[N_ACTUAL_TYPES]     = {}; // not-taken cond, target irrelevant

//   // "Spurious wrong-target": pred.target != branch_target was counted as
//   // target_wrong, but the actual branch was a not-taken conditional that
//   // the BTB had misclassified into a non-conditional slot. Without that
//   // misclassification, target_used would have been false. This counter
//   // isolates how much of legacy target_wrong is a counting artifact.
//   uint64_t spurious_target_wrong_from_misclassify[N_ACTUAL_TYPES] = {};

//   // ============ NEW: POST-RETURN split ============
//   // Post-return == this branch's previous update was a BRANCH_RETURN.
//   uint64_t pr_total = 0;
//   uint64_t pr_actual_total[N_ACTUAL_TYPES]            = {};
//   uint64_t pr_actual_btb_hit[N_ACTUAL_TYPES]          = {};
//   uint64_t pr_actual_target_correct[N_ACTUAL_TYPES]   = {};
//   uint64_t pr_actual_target_wrong[N_ACTUAL_TYPES]     = {};
//   uint64_t pr_actual_target_dontcare[N_ACTUAL_TYPES]  = {};

//   // ============ NEW: NON-POST-RETURN split ============
//   uint64_t npr_total = 0;
//   uint64_t npr_actual_total[N_ACTUAL_TYPES]           = {};
//   uint64_t npr_actual_target_correct[N_ACTUAL_TYPES]  = {};
//   uint64_t npr_actual_target_wrong[N_ACTUAL_TYPES]    = {};
//   uint64_t npr_actual_target_dontcare[N_ACTUAL_TYPES] = {};
// };

// class BaselineBTBContext {
//  public:
//   std::map<O3_CPU*, champsim::msl::lru_table<btb_entry_t>> BTB;
//   std::map<O3_CPU*, std::array<uint64_t, BTB_INDIRECT_SIZE>> INDIRECT_BTB;
//   std::map<O3_CPU*, std::bitset<champsim::lg2(BTB_INDIRECT_SIZE)>> CONDITIONAL_HISTORY;
//   std::map<O3_CPU*, std::deque<uint64_t>> RAS;
//   std::map<O3_CPU*, std::array<uint64_t, CALL_SIZE_TRACKERS>> CALL_SIZE;
//   std::map<O3_CPU*, std::deque<pending_btb_pred_t>> PRED_QUEUE;
//   // NEW: track previous branch's type to detect post-return updates.
//   std::map<O3_CPU*, uint8_t> LAST_BRANCH_TYPE_FOR_STATS;
//   std::map<O3_CPU*, bool>    HAS_LAST_BRANCH;
//   std::map<O3_CPU*, btb_stats_t> STATS;

//   ~BaselineBTBContext() { print_all(); }

//   static void print_per_type(std::ostream& os,
//                              const char* label,
//                              const uint64_t (&arr)[N_ACTUAL_TYPES]) {
//     os << "  " << label << ":";
//     for (int i = 0; i < N_ACTUAL_TYPES; ++i)
//       os << " " << ACTUAL_TYPE_NAMES[i] << "=" << arr[i];
//     os << "\n";
//   }

//   void print_all() {
//     for (auto& [cpu, stats] : STATS) {
//       (void)cpu;
//       std::ostringstream oss;
//       oss << std::fixed << std::setprecision(4);

//       oss << "\n========== BASELINE BTB STATISTICS ==========\n";
//       oss << "Branch updates:           " << stats.total_updates << "\n";
//       oss << "Total hits:               " << stats.total_hits << "\n";
//       oss << "Total misses:             " << stats.total_misses << "\n";
//       if ((stats.total_hits + stats.total_misses) > 0) {
//         oss << "Hit rate:                 "
//             << (100.0 * stats.total_hits / (stats.total_hits + stats.total_misses)) << "%\n";
//       }
//       oss << "Target correct:           " << stats.target_correct << "\n";
//       oss << "Target wrong:             " << stats.target_wrong   << "\n";
//       if ((stats.target_correct + stats.target_wrong) > 0) {
//         oss << "Target accuracy:          "
//             << (100.0 * stats.target_correct / (stats.target_correct + stats.target_wrong)) << "%\n";
//       }
//       // Legacy hit/miss buckets -- KEPT but flagged.
//       oss << "[legacy, hit=by-pred-type, miss=by-actual-type]\n";
//       oss << "Conditional hits:         " << stats.conditional_hits   << "\n";
//       oss << "Conditional misses:       " << stats.conditional_misses << "\n";
//       oss << "Indirect hits:            " << stats.indirect_hits      << "\n";
//       oss << "Indirect misses:          " << stats.indirect_misses    << "\n";
//       oss << "Return hits:              " << stats.return_hits        << "\n";
//       oss << "Return misses:            " << stats.return_misses      << "\n";
//       oss << "Always taken hits:        " << stats.always_taken_hits   << "\n";
//       oss << "Always taken misses:      " << stats.always_taken_misses << "\n";
//       oss << "RAS hits:                 " << stats.ras_hits << "\n";
//       oss << "RAS misses:               " << stats.ras_misses << "\n";
//       // Legacy target buckets -- KEPT but flagged.
//       oss << "[legacy, all target-* counters below indexed by PREDICTED type]\n";
//       oss << "RAS target correct:       " << stats.ras_target_correct << "\n";
//       oss << "RAS target wrong:         " << stats.ras_target_wrong   << "\n";
//       oss << "Indirect target correct:  " << stats.indirect_target_correct << "\n";
//       oss << "Indirect target wrong:    " << stats.indirect_target_wrong   << "\n";
//       oss << "BTB target correct:       " << stats.btb_target_correct << "\n";
//       oss << "BTB target wrong:         " << stats.btb_target_wrong   << "\n";

//       oss << "\n--- Per-ACTUAL-type (use these to compare across predictors) ---\n";
//       print_per_type(oss, "actual_total                 ", stats.actual_total);
//       print_per_type(oss, "actual_btb_hit               ", stats.actual_btb_hit);
//       print_per_type(oss, "actual_btb_miss              ", stats.actual_btb_miss);
//       print_per_type(oss, "actual_type_match            ", stats.actual_type_match);
//       print_per_type(oss, "actual_type_mismatch         ", stats.actual_type_mismatch);
//       print_per_type(oss, "actual_target_correct        ", stats.actual_target_correct);
//       print_per_type(oss, "actual_target_wrong          ", stats.actual_target_wrong);
//       print_per_type(oss, "actual_target_dontcare       ", stats.actual_target_dontcare);
//       print_per_type(oss, "spurious_target_wrong_artif. ", stats.spurious_target_wrong_from_misclassify);

//       oss << "\n--- POST-RETURN branches only (prev branch was BRANCH_RETURN) ---\n";
//       oss << "  pr_total: " << stats.pr_total << "\n";
//       print_per_type(oss, "pr_actual_total              ", stats.pr_actual_total);
//       print_per_type(oss, "pr_actual_btb_hit            ", stats.pr_actual_btb_hit);
//       print_per_type(oss, "pr_actual_target_correct     ", stats.pr_actual_target_correct);
//       print_per_type(oss, "pr_actual_target_wrong       ", stats.pr_actual_target_wrong);
//       print_per_type(oss, "pr_actual_target_dontcare    ", stats.pr_actual_target_dontcare);

//       oss << "\n--- NON-POST-RETURN branches only ---\n";
//       oss << "  npr_total: " << stats.npr_total << "\n";
//       print_per_type(oss, "npr_actual_total             ", stats.npr_actual_total);
//       print_per_type(oss, "npr_actual_target_correct    ", stats.npr_actual_target_correct);
//       print_per_type(oss, "npr_actual_target_wrong      ", stats.npr_actual_target_wrong);
//       print_per_type(oss, "npr_actual_target_dontcare   ", stats.npr_actual_target_dontcare);

//       oss << "=============================================\n";
//       std::cerr << oss.str();
//     }
//     std::cerr << std::flush;
//   }
// };

// static BaselineBTBContext g_ctx;

// } // namespace

// void O3_CPU::initialize_btb()
// {
//   g_ctx.BTB.insert({this, champsim::msl::lru_table<btb_entry_t>{BTB_SET, BTB_WAY}});
//   std::fill(std::begin(g_ctx.INDIRECT_BTB[this]), std::end(g_ctx.INDIRECT_BTB[this]), 0);
//   std::fill(std::begin(g_ctx.CALL_SIZE[this]), std::end(g_ctx.CALL_SIZE[this]), 4);
//   g_ctx.CONDITIONAL_HISTORY[this] = 0;
//   g_ctx.PRED_QUEUE[this].clear();
//   g_ctx.HAS_LAST_BRANCH[this] = false;
//   g_ctx.LAST_BRANCH_TYPE_FOR_STATS[this] = 0;
//   g_ctx.STATS[this] = {};
// }

// std::pair<uint64_t, uint8_t> O3_CPU::btb_prediction(uint64_t ip)
// {
//   auto btb_entry = g_ctx.BTB.at(this).check_hit({ip, 0, ::branch_info::ALWAYS_TAKEN});

//   uint64_t predicted_target = 0;
//   uint8_t always_taken = false;
//   branch_info pred_type = ::branch_info::ALWAYS_TAKEN;
//   bool was_hit = false;
//   bool ras_empty = false;

//   if (btb_entry.has_value()) {
//     was_hit = true;
//     pred_type = btb_entry->type;

//     if (btb_entry->type == ::branch_info::RETURN) {
//       if (std::empty(g_ctx.RAS[this])) {
//         ras_empty = true;
//         predicted_target = 0;
//         always_taken = true;
//       } else {
//         ras_empty = false;
//         auto target = g_ctx.RAS[this].back();
//         auto size = g_ctx.CALL_SIZE[this][target % std::size(g_ctx.CALL_SIZE[this])];
//         predicted_target = target + size;
//         always_taken = true;
//       }
//     }
//     else if (btb_entry->type == ::branch_info::INDIRECT) {
//       auto hash = (ip) ^ g_ctx.CONDITIONAL_HISTORY[this].to_ullong();
//       predicted_target = g_ctx.INDIRECT_BTB[this][hash % std::size(g_ctx.INDIRECT_BTB[this])];
//       always_taken = true;
//     }
//     else {
//       predicted_target = btb_entry->target;
//       always_taken = (btb_entry->type != ::branch_info::CONDITIONAL);
//     }
//   }

//   g_ctx.PRED_QUEUE[this].push_back({ip, predicted_target, pred_type, was_hit, ras_empty});
//   return {predicted_target, always_taken};
// }

// void O3_CPU::update_btb(uint64_t ip, uint64_t branch_target, uint8_t taken, uint8_t branch_type)
// {
//   auto& stats = g_ctx.STATS[this];
//   stats.total_updates++;
//   if (taken) stats.taken_count++; else stats.not_taken_count++;

//   // ---- Detect post-return ----------------------------------------------
//   bool is_post_return = g_ctx.HAS_LAST_BRANCH[this] &&
//                         (g_ctx.LAST_BRANCH_TYPE_FOR_STATS[this] == BRANCH_RETURN);

//   // ---- Drain PRED_QUEUE ------------------------------------------------
//   auto& q = g_ctx.PRED_QUEUE[this];
//   bool found = false;
//   pending_btb_pred_t pred{};
//   while (!q.empty()) {
//     pred = q.front();
//     q.pop_front();
//     if (pred.ip == ip) { found = true; break; }
//   }

//   // ---- Per-actual-type accounting -------------------------------------
//   int ai = actual_type_idx(branch_type);
//   stats.actual_total[ai]++;
//   if (is_post_return) {
//     stats.pr_total++;
//     stats.pr_actual_total[ai]++;
//   } else {
//     stats.npr_total++;
//     stats.npr_actual_total[ai]++;
//   }

//   branch_info expected = expected_info(branch_type);

//   if (found && pred.was_hit) {
//     stats.actual_btb_hit[ai]++;
//     if (is_post_return) stats.pr_actual_btb_hit[ai]++;
//     if (pred.type == expected) stats.actual_type_match[ai]++;
//     else                       stats.actual_type_mismatch[ai]++;
//   } else {
//     stats.actual_btb_miss[ai]++;
//   }

//   // Per-actual-type target accounting -- target_used here is based on the
//   // ACTUAL outcome (taken or non-conditional), independent of pred.type.
//   bool true_target_used = taken || (branch_type != BRANCH_CONDITIONAL && branch_type != BRANCH_OTHER);
//   if (!true_target_used) {
//     stats.actual_target_dontcare[ai]++;
//     if (is_post_return) stats.pr_actual_target_dontcare[ai]++;
//     else                stats.npr_actual_target_dontcare[ai]++;
//   } else if (found) {
//     // Treat a BTB miss as a target wrong (no prediction → 0 ≠ branch_target).
//     bool tgt_ok = pred.was_hit && (pred.target == branch_target);
//     if (tgt_ok) {
//       stats.actual_target_correct[ai]++;
//       if (is_post_return) stats.pr_actual_target_correct[ai]++;
//       else                stats.npr_actual_target_correct[ai]++;
//     } else {
//       stats.actual_target_wrong[ai]++;
//       if (is_post_return) stats.pr_actual_target_wrong[ai]++;
//       else                stats.npr_actual_target_wrong[ai]++;
//     }
//   } else {
//     stats.actual_target_wrong[ai]++;
//     if (is_post_return) stats.pr_actual_target_wrong[ai]++;
//     else                stats.npr_actual_target_wrong[ai]++;
//   }

//   // Diagnostic: how many legacy "target_wrong" hits were spuriously
//   // counted because the slot misclassified a not-taken conditional?
//   if (found && pred.was_hit &&
//       (branch_type == BRANCH_CONDITIONAL || branch_type == BRANCH_OTHER) &&
//       !taken &&
//       pred.type != branch_info::CONDITIONAL) {
//     stats.spurious_target_wrong_from_misclassify[ai]++;
//   }

//   // ---- LEGACY accounting (preserved exactly) ---------------------------
//   if (found) {
//     if (pred.was_hit) {
//       stats.total_hits++;
//       switch (pred.type) {
//         case branch_info::CONDITIONAL: stats.conditional_hits++; break;
//         case branch_info::INDIRECT:    stats.indirect_hits++;    break;
//         case branch_info::RETURN:      stats.return_hits++;      break;
//         default:                       stats.always_taken_hits++; break;
//       }
//       if (pred.type == branch_info::RETURN) {
//         if (pred.ras_empty) stats.ras_misses++;
//         else                stats.ras_hits++;
//       }
//       bool target_used = taken || (pred.type != branch_info::CONDITIONAL);
//       if (target_used) {
//         if (pred.target == branch_target) {
//           stats.target_correct++;
//           switch (pred.type) {
//             case branch_info::RETURN:   stats.ras_target_correct++; break;
//             case branch_info::INDIRECT: stats.indirect_target_correct++; break;
//             default:                    stats.btb_target_correct++; break;
//           }
//         } else {
//           stats.target_wrong++;
//           switch (pred.type) {
//             case branch_info::RETURN:   stats.ras_target_wrong++; break;
//             case branch_info::INDIRECT: stats.indirect_target_wrong++; break;
//             default:                    stats.btb_target_wrong++; break;
//           }
//         }
//       }
//     } else {
//       stats.total_misses++;
//       switch (branch_type) {
//         case BRANCH_CONDITIONAL:
//         case BRANCH_OTHER:               stats.conditional_misses++; break;
//         case BRANCH_INDIRECT:
//         case BRANCH_INDIRECT_CALL:       stats.indirect_misses++;    break;
//         case BRANCH_RETURN:              stats.return_misses++;      break;
//         default:                         stats.always_taken_misses++; break;
//       }
//     }
//   } else {
//     stats.total_misses++;
//     switch (branch_type) {
//       case BRANCH_CONDITIONAL:
//       case BRANCH_OTHER:                 stats.conditional_misses++; break;
//       case BRANCH_INDIRECT:
//       case BRANCH_INDIRECT_CALL:         stats.indirect_misses++;    break;
//       case BRANCH_RETURN:                stats.return_misses++;      break;
//       default:                           stats.always_taken_misses++; break;
//     }
//   }

//   // ---- RAS / state updates (UNCHANGED) ----------------------------------
//   if (branch_type == BRANCH_DIRECT_CALL || branch_type == BRANCH_INDIRECT_CALL) {
//     g_ctx.RAS[this].push_back(ip);
//     if (std::size(g_ctx.RAS[this]) > RAS_SIZE) g_ctx.RAS[this].pop_front();
//   }
//   if ((branch_type == BRANCH_INDIRECT) || (branch_type == BRANCH_INDIRECT_CALL)) {
//     auto hash = (ip) ^ g_ctx.CONDITIONAL_HISTORY[this].to_ullong();
//     g_ctx.INDIRECT_BTB[this][hash % std::size(g_ctx.INDIRECT_BTB[this])] = branch_target;
//   }
//   if ((branch_type == BRANCH_CONDITIONAL) || (branch_type == BRANCH_OTHER)) {
//     g_ctx.CONDITIONAL_HISTORY[this] <<= 1;
//     g_ctx.CONDITIONAL_HISTORY[this].set(0, taken);
//   }
//   if (branch_type == BRANCH_RETURN && !std::empty(g_ctx.RAS[this])) {
//     auto call_ip = g_ctx.RAS[this].back();
//     g_ctx.RAS[this].pop_back();
//     auto estimated_call_instr_size = (call_ip > branch_target) ? call_ip - branch_target : branch_target - call_ip;
//     if (estimated_call_instr_size <= 10) {
//       g_ctx.CALL_SIZE[this][call_ip % std::size(g_ctx.CALL_SIZE[this])] = estimated_call_instr_size;
//     }
//   }

//   auto type = ::branch_info::ALWAYS_TAKEN;
//   if ((branch_type == BRANCH_INDIRECT) || (branch_type == BRANCH_INDIRECT_CALL)) type = ::branch_info::INDIRECT;
//   else if (branch_type == BRANCH_RETURN)                                          type = ::branch_info::RETURN;
//   else if ((branch_type == BRANCH_CONDITIONAL) || (branch_type == BRANCH_OTHER))  type = ::branch_info::CONDITIONAL;

//   auto opt_entry = g_ctx.BTB.at(this).check_hit({ip, branch_target, type});
//   if (opt_entry.has_value()) {
//     opt_entry->type = type;
//     if (branch_target != 0) opt_entry->target = branch_target;
//   }
//   if (branch_target != 0) {
//     g_ctx.BTB.at(this).fill(opt_entry.value_or(::btb_entry_t{ip, branch_target, type}));
//   }

//   // Roll forward post-return tracking.
//   g_ctx.LAST_BRANCH_TYPE_FOR_STATS[this] = branch_type;
//   g_ctx.HAS_LAST_BRANCH[this] = true;
// }


#include <algorithm>
#include <array>
#include <bitset>
#include <deque>
#include <map>
#include <iostream>
#include <iomanip>
#include <sstream>
#include <fstream>
#include <cstdlib>
#include <cstdint>

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

// -------- per-actual-type indexing helpers (SHARED with MBTB file) --------
constexpr int N_ACTUAL_TYPES = 7;
static int actual_type_idx(uint8_t bt) {
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
static const char* const ACTUAL_TYPE_NAMES[N_ACTUAL_TYPES] = {
  "DIRECT_JUMP", "INDIRECT", "CONDITIONAL", "DIRECT_CALL",
  "INDIRECT_CALL", "RETURN", "OTHER"
};

// "What type would update_btb assign to this branch_type?"
// Used so we can detect "predicted-type matched the type the BTB would have
// written for this branch" without depending on the slot's stored type.
static branch_info expected_info(uint8_t bt) {
  if (bt == BRANCH_INDIRECT || bt == BRANCH_INDIRECT_CALL) return branch_info::INDIRECT;
  if (bt == BRANCH_RETURN)                                 return branch_info::RETURN;
  if (bt == BRANCH_CONDITIONAL || bt == BRANCH_OTHER)      return branch_info::CONDITIONAL;
  return branch_info::ALWAYS_TAKEN;
}

struct btb_entry_t {
  uint64_t ip_tag = 0;
  uint64_t target = 0;
  branch_info type = branch_info::ALWAYS_TAKEN;

  auto index() const { return ip_tag; }
  auto tag() const { return ip_tag; }

};

struct pending_btb_pred_t {
  uint64_t ip;
  uint64_t target;
  branch_info type;
  bool was_hit;
  bool ras_empty;
};

struct btb_stats_t {
  // ============ LEGACY COUNTERS (preserved verbatim) ============
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

  // ============ NEW: per-ACTUAL-type counters (unambiguous) ============
  uint64_t actual_total[N_ACTUAL_TYPES]               = {};
  uint64_t actual_btb_hit[N_ACTUAL_TYPES]             = {};
  uint64_t actual_btb_miss[N_ACTUAL_TYPES]            = {};
  uint64_t actual_type_match[N_ACTUAL_TYPES]          = {};
  uint64_t actual_type_mismatch[N_ACTUAL_TYPES]       = {};
  uint64_t actual_target_correct[N_ACTUAL_TYPES]      = {};
  uint64_t actual_target_wrong[N_ACTUAL_TYPES]        = {};
  uint64_t actual_target_dontcare[N_ACTUAL_TYPES]     = {}; // not-taken cond, target irrelevant

  // "Spurious wrong-target": pred.target != branch_target was counted as
  // target_wrong, but the actual branch was a not-taken conditional that
  // the BTB had misclassified into a non-conditional slot. Without that
  // misclassification, target_used would have been false. This counter
  // isolates how much of legacy target_wrong is a counting artifact.
  uint64_t spurious_target_wrong_from_misclassify[N_ACTUAL_TYPES] = {};

  // ============ NEW: POST-RETURN split ============
  // Post-return == this branch's previous update was a BRANCH_RETURN.
  uint64_t pr_total = 0;
  uint64_t pr_actual_total[N_ACTUAL_TYPES]            = {};
  uint64_t pr_actual_btb_hit[N_ACTUAL_TYPES]          = {};
  uint64_t pr_actual_target_correct[N_ACTUAL_TYPES]   = {};
  uint64_t pr_actual_target_wrong[N_ACTUAL_TYPES]     = {};
  uint64_t pr_actual_target_dontcare[N_ACTUAL_TYPES]  = {};

  // ============ NEW: NON-POST-RETURN split ============
  uint64_t npr_total = 0;
  uint64_t npr_actual_total[N_ACTUAL_TYPES]           = {};
  uint64_t npr_actual_target_correct[N_ACTUAL_TYPES]  = {};
  uint64_t npr_actual_target_wrong[N_ACTUAL_TYPES]    = {};
  uint64_t npr_actual_target_dontcare[N_ACTUAL_TYPES] = {};
};

class BaselineBTBContext {
 public:
  std::map<O3_CPU*, champsim::msl::lru_table<btb_entry_t>> BTB;
  std::map<O3_CPU*, std::array<uint64_t, BTB_INDIRECT_SIZE>> INDIRECT_BTB;
  std::map<O3_CPU*, std::bitset<champsim::lg2(BTB_INDIRECT_SIZE)>> CONDITIONAL_HISTORY;
  std::map<O3_CPU*, std::deque<uint64_t>> RAS;
  std::map<O3_CPU*, std::array<uint64_t, CALL_SIZE_TRACKERS>> CALL_SIZE;
  std::map<O3_CPU*, std::deque<pending_btb_pred_t>> PRED_QUEUE;
  // NEW: track previous branch's type to detect post-return updates.
  std::map<O3_CPU*, uint8_t> LAST_BRANCH_TYPE_FOR_STATS;
  std::map<O3_CPU*, bool>    HAS_LAST_BRANCH;
  std::map<O3_CPU*, btb_stats_t> STATS;

  ~BaselineBTBContext() { print_all(); }

  static void print_per_type(std::ostream& os,
                             const char* label,
                             const uint64_t (&arr)[N_ACTUAL_TYPES]) {
    os << "  " << label << ":";
    for (int i = 0; i < N_ACTUAL_TYPES; ++i)
      os << " " << ACTUAL_TYPE_NAMES[i] << "=" << arr[i];
    os << "\n";
  }

  void print_all() {
    for (auto& [cpu, stats] : STATS) {
      (void)cpu;
      std::ostringstream oss;
      oss << std::fixed << std::setprecision(4);

      oss << "\n========== BASELINE BTB STATISTICS ==========\n";
      oss << "Branch updates:           " << stats.total_updates << "\n";
      oss << "Total hits:               " << stats.total_hits << "\n";
      oss << "Total misses:             " << stats.total_misses << "\n";
      if ((stats.total_hits + stats.total_misses) > 0) {
        oss << "Hit rate:                 "
            << (100.0 * stats.total_hits / (stats.total_hits + stats.total_misses)) << "%\n";
      }
      oss << "Target correct:           " << stats.target_correct << "\n";
      oss << "Target wrong:             " << stats.target_wrong   << "\n";
      if ((stats.target_correct + stats.target_wrong) > 0) {
        oss << "Target accuracy:          "
            << (100.0 * stats.target_correct / (stats.target_correct + stats.target_wrong)) << "%\n";
      }
      // Legacy hit/miss buckets -- KEPT but flagged.
      oss << "[legacy, hit=by-pred-type, miss=by-actual-type]\n";
      oss << "Conditional hits:         " << stats.conditional_hits   << "\n";
      oss << "Conditional misses:       " << stats.conditional_misses << "\n";
      oss << "Indirect hits:            " << stats.indirect_hits      << "\n";
      oss << "Indirect misses:          " << stats.indirect_misses    << "\n";
      oss << "Return hits:              " << stats.return_hits        << "\n";
      oss << "Return misses:            " << stats.return_misses      << "\n";
      oss << "Always taken hits:        " << stats.always_taken_hits   << "\n";
      oss << "Always taken misses:      " << stats.always_taken_misses << "\n";
      oss << "RAS hits:                 " << stats.ras_hits << "\n";
      oss << "RAS misses:               " << stats.ras_misses << "\n";
      // Legacy target buckets -- KEPT but flagged.
      oss << "[legacy, all target-* counters below indexed by PREDICTED type]\n";
      oss << "RAS target correct:       " << stats.ras_target_correct << "\n";
      oss << "RAS target wrong:         " << stats.ras_target_wrong   << "\n";
      oss << "Indirect target correct:  " << stats.indirect_target_correct << "\n";
      oss << "Indirect target wrong:    " << stats.indirect_target_wrong   << "\n";
      oss << "BTB target correct:       " << stats.btb_target_correct << "\n";
      oss << "BTB target wrong:         " << stats.btb_target_wrong   << "\n";

      oss << "\n--- Per-ACTUAL-type (use these to compare across predictors) ---\n";
      print_per_type(oss, "actual_total                 ", stats.actual_total);
      print_per_type(oss, "actual_btb_hit               ", stats.actual_btb_hit);
      print_per_type(oss, "actual_btb_miss              ", stats.actual_btb_miss);
      print_per_type(oss, "actual_type_match            ", stats.actual_type_match);
      print_per_type(oss, "actual_type_mismatch         ", stats.actual_type_mismatch);
      print_per_type(oss, "actual_target_correct        ", stats.actual_target_correct);
      print_per_type(oss, "actual_target_wrong          ", stats.actual_target_wrong);
      print_per_type(oss, "actual_target_dontcare       ", stats.actual_target_dontcare);
      print_per_type(oss, "spurious_target_wrong_artif. ", stats.spurious_target_wrong_from_misclassify);

      oss << "\n--- POST-RETURN branches only (prev branch was BRANCH_RETURN) ---\n";
      oss << "  pr_total: " << stats.pr_total << "\n";
      print_per_type(oss, "pr_actual_total              ", stats.pr_actual_total);
      print_per_type(oss, "pr_actual_btb_hit            ", stats.pr_actual_btb_hit);
      print_per_type(oss, "pr_actual_target_correct     ", stats.pr_actual_target_correct);
      print_per_type(oss, "pr_actual_target_wrong       ", stats.pr_actual_target_wrong);
      print_per_type(oss, "pr_actual_target_dontcare    ", stats.pr_actual_target_dontcare);

      oss << "\n--- NON-POST-RETURN branches only ---\n";
      oss << "  npr_total: " << stats.npr_total << "\n";
      print_per_type(oss, "npr_actual_total             ", stats.npr_actual_total);
      print_per_type(oss, "npr_actual_target_correct    ", stats.npr_actual_target_correct);
      print_per_type(oss, "npr_actual_target_wrong      ", stats.npr_actual_target_wrong);
      print_per_type(oss, "npr_actual_target_dontcare   ", stats.npr_actual_target_dontcare);

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
  g_ctx.HAS_LAST_BRANCH[this] = false;
  g_ctx.LAST_BRANCH_TYPE_FOR_STATS[this] = 0;
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
      auto hash = (ip) ^ g_ctx.CONDITIONAL_HISTORY[this].to_ullong();
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

  // ---- Detect post-return ----------------------------------------------
  bool is_post_return = g_ctx.HAS_LAST_BRANCH[this] &&
                        (g_ctx.LAST_BRANCH_TYPE_FOR_STATS[this] == BRANCH_RETURN);

  // ---- Drain PRED_QUEUE ------------------------------------------------
  auto& q = g_ctx.PRED_QUEUE[this];
  bool found = false;
  pending_btb_pred_t pred{};
  while (!q.empty()) {
    pred = q.front();
    q.pop_front();
    if (pred.ip == ip) { found = true; break; }
  }

  // ---- Per-actual-type accounting -------------------------------------
  int ai = actual_type_idx(branch_type);
  stats.actual_total[ai]++;
  if (is_post_return) {
    stats.pr_total++;
    stats.pr_actual_total[ai]++;
  } else {
    stats.npr_total++;
    stats.npr_actual_total[ai]++;
  }

  branch_info expected = expected_info(branch_type);

  if (found && pred.was_hit) {
    stats.actual_btb_hit[ai]++;
    if (is_post_return) stats.pr_actual_btb_hit[ai]++;
    if (pred.type == expected) stats.actual_type_match[ai]++;
    else                       stats.actual_type_mismatch[ai]++;
  } else {
    stats.actual_btb_miss[ai]++;
  }

  // Per-actual-type target accounting -- target_used here is based on the
  // ACTUAL outcome (taken or non-conditional), independent of pred.type.
  bool true_target_used = taken || (branch_type != BRANCH_CONDITIONAL && branch_type != BRANCH_OTHER);
  if (!true_target_used) {
    stats.actual_target_dontcare[ai]++;
    if (is_post_return) stats.pr_actual_target_dontcare[ai]++;
    else                stats.npr_actual_target_dontcare[ai]++;
  } else if (found) {
    // Treat a BTB miss as a target wrong (no prediction → 0 ≠ branch_target).
    bool tgt_ok = pred.was_hit && (pred.target == branch_target);
    if (tgt_ok) {
      stats.actual_target_correct[ai]++;
      if (is_post_return) stats.pr_actual_target_correct[ai]++;
      else                stats.npr_actual_target_correct[ai]++;
    } else {
      stats.actual_target_wrong[ai]++;
      if (is_post_return) stats.pr_actual_target_wrong[ai]++;
      else                stats.npr_actual_target_wrong[ai]++;
    }
  } else {
    stats.actual_target_wrong[ai]++;
    if (is_post_return) stats.pr_actual_target_wrong[ai]++;
    else                stats.npr_actual_target_wrong[ai]++;
  }

  // Diagnostic: how many legacy "target_wrong" hits were spuriously
  // counted because the slot misclassified a not-taken conditional?
  if (found && pred.was_hit &&
      (branch_type == BRANCH_CONDITIONAL || branch_type == BRANCH_OTHER) &&
      !taken &&
      pred.type != branch_info::CONDITIONAL) {
    stats.spurious_target_wrong_from_misclassify[ai]++;
  }

  // ---- LEGACY accounting (preserved exactly) ---------------------------
  if (found) {
    if (pred.was_hit) {
      stats.total_hits++;
      switch (pred.type) {
        case branch_info::CONDITIONAL: stats.conditional_hits++; break;
        case branch_info::INDIRECT:    stats.indirect_hits++;    break;
        case branch_info::RETURN:      stats.return_hits++;      break;
        default:                       stats.always_taken_hits++; break;
      }
      if (pred.type == branch_info::RETURN) {
        if (pred.ras_empty) stats.ras_misses++;
        else                stats.ras_hits++;
      }
      bool target_used = taken || (pred.type != branch_info::CONDITIONAL);
      if (target_used) {
        if (pred.target == branch_target) {
          stats.target_correct++;
          switch (pred.type) {
            case branch_info::RETURN:   stats.ras_target_correct++; break;
            case branch_info::INDIRECT: stats.indirect_target_correct++; break;
            default:                    stats.btb_target_correct++; break;
          }
        } else {
          stats.target_wrong++;
          switch (pred.type) {
            case branch_info::RETURN:   stats.ras_target_wrong++; break;
            case branch_info::INDIRECT: stats.indirect_target_wrong++; break;
            default:                    stats.btb_target_wrong++; break;
          }
        }
      }
    } else {
      stats.total_misses++;
      switch (branch_type) {
        case BRANCH_CONDITIONAL:
        case BRANCH_OTHER:               stats.conditional_misses++; break;
        case BRANCH_INDIRECT:
        case BRANCH_INDIRECT_CALL:       stats.indirect_misses++;    break;
        case BRANCH_RETURN:              stats.return_misses++;      break;
        default:                         stats.always_taken_misses++; break;
      }
    }
  } else {
    stats.total_misses++;
    switch (branch_type) {
      case BRANCH_CONDITIONAL:
      case BRANCH_OTHER:                 stats.conditional_misses++; break;
      case BRANCH_INDIRECT:
      case BRANCH_INDIRECT_CALL:         stats.indirect_misses++;    break;
      case BRANCH_RETURN:                stats.return_misses++;      break;
      default:                           stats.always_taken_misses++; break;
    }
  }

  // ---- RAS / state updates (UNCHANGED) ----------------------------------
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
    auto estimated_call_instr_size = (call_ip > branch_target) ? call_ip - branch_target : branch_target - call_ip;
    if (estimated_call_instr_size <= 10) {
      g_ctx.CALL_SIZE[this][call_ip % std::size(g_ctx.CALL_SIZE[this])] = estimated_call_instr_size;
    }
  }

  auto type = ::branch_info::ALWAYS_TAKEN;
  if ((branch_type == BRANCH_INDIRECT) || (branch_type == BRANCH_INDIRECT_CALL)) type = ::branch_info::INDIRECT;
  else if (branch_type == BRANCH_RETURN)                                          type = ::branch_info::RETURN;
  else if ((branch_type == BRANCH_CONDITIONAL) || (branch_type == BRANCH_OTHER))  type = ::branch_info::CONDITIONAL;

  auto opt_entry = g_ctx.BTB.at(this).check_hit({ip, branch_target, type});
  if (opt_entry.has_value()) {
    opt_entry->type = type;
    if (branch_target != 0) opt_entry->target = branch_target;
  }
  if (branch_target != 0) {
    g_ctx.BTB.at(this).fill(opt_entry.value_or(::btb_entry_t{ip, branch_target, type}));
  }

  // Roll forward post-return tracking.
  g_ctx.LAST_BRANCH_TYPE_FOR_STATS[this] = branch_type;
  g_ctx.HAS_LAST_BRANCH[this] = true;
}