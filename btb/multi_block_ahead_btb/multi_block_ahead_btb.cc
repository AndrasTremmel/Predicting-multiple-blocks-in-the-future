// #include <algorithm>
// #include <array>
// #include <bitset>
// #include <cstdint>
// #include <cstdlib>
// #include <deque>
// #include <iomanip>
// #include <iostream>
// #include <map>
// #include <sstream>
// #include <utility>

// #include "msl/lru_table.h"
// #include "ooo_cpu.h"

// #define OPTIMIZATION_ON 1

// namespace
// {

// enum class branch_info {
//   INDIRECT,
//   RETURN,
//   ALWAYS_TAKEN,
//   CONDITIONAL,
// };

// enum class mbtb_transition : uint8_t { T, N, R };

// struct mbtb_entry_t {
//   uint64_t ip_tag = 0;
//   uint64_t target = 0;
//   branch_info type = branch_info::ALWAYS_TAKEN;
//   mbtb_transition transition = mbtb_transition::N;
//   uint64_t ind_ctx = 0;  // 0 unless predecessor was indirect


//   auto index() const { return ip_tag; }
//   auto tag() const { return (ip_tag) ^ static_cast<uint64_t>(transition) ^ ind_ctx; }
// };

// struct sas_record_t {
//   uint64_t target = 0;
//   branch_info type = branch_info::ALWAYS_TAKEN;
//   bool valid = false;
// };

// struct pending_mbtb_pred_t {
//   uint64_t ip;
//   uint64_t target;
//   bool was_hit;
// };

// // -------- per-branch-type indexing -----------------------------------------
// constexpr int N_BRANCH_TYPES = 7;
// static int branch_type_idx(uint8_t bt) {
//   switch (bt) {
//     case BRANCH_DIRECT_JUMP:   return 0;
//     case BRANCH_INDIRECT:      return 1;
//     case BRANCH_CONDITIONAL:   return 2;
//     case BRANCH_DIRECT_CALL:   return 3;
//     case BRANCH_INDIRECT_CALL: return 4;
//     case BRANCH_RETURN:        return 5;
//     default:                   return 6;
//   }
// }
// static const char* const BRANCH_TYPE_NAMES[N_BRANCH_TYPES] = {
//   "DIRECT_JUMP", "INDIRECT", "CONDITIONAL", "DIRECT_CALL",
//   "INDIRECT_CALL", "RETURN", "OTHER"
// };

// struct mbtb_stats_t {
//   // Aggregate counters.
//   uint64_t total_hits           = 0;
//   uint64_t total_misses         = 0;
//   uint64_t total_mispredictions = 0;
//   uint64_t total_correct        = 0;

//   // Per-branch-type breakdown (indexed by current branch's actual type).
//   uint64_t per_type_hits          [N_BRANCH_TYPES] = {};
//   uint64_t per_type_misses        [N_BRANCH_TYPES] = {};
//   uint64_t per_type_mispredictions[N_BRANCH_TYPES] = {};
//   uint64_t per_type_correct       [N_BRANCH_TYPES] = {};

//   // Per-PREVIOUS-branch-type breakdown (indexed by the type of the branch
//   // updated immediately before this one). The very first branch in the
//   // trace has no predecessor and is therefore not counted in this split.
//   uint64_t per_prev_type_hits          [N_BRANCH_TYPES] = {};
//   uint64_t per_prev_type_misses        [N_BRANCH_TYPES] = {};
//   uint64_t per_prev_type_mispredictions[N_BRANCH_TYPES] = {};
//   uint64_t per_prev_type_correct       [N_BRANCH_TYPES] = {};
// };

// constexpr std::size_t BTB_SET = 2048;
// constexpr std::size_t BTB_WAY = 4;
// constexpr std::size_t BTB_INDIRECT_SIZE = 4096;
// constexpr std::size_t RAS_SIZE = 64;
// constexpr std::size_t SAS_SIZE = 64;
// constexpr std::size_t CALL_SIZE_TRACKERS = 1024;

// class MultiBlockBTBContext {
//  public:
//   std::map<O3_CPU*, champsim::msl::lru_table<mbtb_entry_t>> MBTB;
//   std::map<O3_CPU*, std::array<uint64_t, BTB_INDIRECT_SIZE>> INDIRECT_BTB;
//   std::map<O3_CPU*, std::bitset<champsim::lg2(BTB_INDIRECT_SIZE)>> CONDITIONAL_HISTORY;
//   std::map<O3_CPU*, std::deque<uint64_t>> RAS;
//   std::map<O3_CPU*, std::deque<sas_record_t>> SAS;
//   std::map<O3_CPU*, std::array<uint64_t, CALL_SIZE_TRACKERS>> CALL_SIZE;
//   std::map<O3_CPU*, uint64_t> LAST_BRANCH_IP;
//   std::map<O3_CPU*, mbtb_transition> LAST_TRANSITION;
//   std::map<O3_CPU*, bool> LAST_BRANCH_WAS_RETURN;
//   std::map<O3_CPU*, uint64_t> LAST_RETURN_CALL_IP;
//   std::map<O3_CPU*, sas_record_t> PENDING_SAS_ENTRY;
//   std::map<O3_CPU*, std::deque<pending_mbtb_pred_t>> PRED_QUEUE;

//   std::map<O3_CPU*, uint64_t> LAST_INDIRECT_TARGET;  // 0 unless previous branch was indirect
//   // Type of the most recently UPDATED branch -- needed so that the next
//   // update can record its prev-branch-type stats. (Stats-only state.)
//   std::map<O3_CPU*, uint8_t> LAST_BRANCH_TYPE_FOR_STATS;
//   std::map<O3_CPU*, bool>    HAS_LAST_BRANCH;
//   std::map<O3_CPU*, mbtb_stats_t> STATS;

//   ~MultiBlockBTBContext() { print_all(); }

//   static void print_per_type(std::ostream& os,
//                              const char* label,
//                              const uint64_t (&arr)[N_BRANCH_TYPES]) {
//     os << "  " << label << ":";
//     for (int i = 0; i < N_BRANCH_TYPES; ++i)
//       os << " " << BRANCH_TYPE_NAMES[i] << "=" << arr[i];
//     os << "\n";
//   }

//   void print_all() {
//     for (auto& [cpu, stats] : STATS) {
//       (void)cpu;
//       std::ostringstream oss;
//       oss << std::fixed << std::setprecision(4);

//       oss << "\n========== MULTI-BLOCK BTB STATISTICS ==========\n";
//       oss << "Total hits:                " << stats.total_hits           << "\n";
//       oss << "Total misses:              " << stats.total_misses         << "\n";
//       oss << "Total mispredictions:      " << stats.total_mispredictions << "\n";
//       oss << "Total correct predictions: " << stats.total_correct        << "\n";
//       if ((stats.total_hits + stats.total_misses) > 0) {
//         oss << "Hit rate:                  "
//             << (100.0 * stats.total_hits / (stats.total_hits + stats.total_misses)) << "%\n";
//       }
//       if ((stats.total_correct + stats.total_mispredictions) > 0) {
//         oss << "Prediction accuracy:       "
//             << (100.0 * stats.total_correct
//                 / (stats.total_correct + stats.total_mispredictions)) << "%\n";
//       }

//       oss << "\n--- Per-branch-type breakdown (current branch) ---\n";
//       print_per_type(oss, "hits          ", stats.per_type_hits);
//       print_per_type(oss, "misses        ", stats.per_type_misses);
//       print_per_type(oss, "mispredictions", stats.per_type_mispredictions);
//       print_per_type(oss, "correct       ", stats.per_type_correct);

//       oss << "\n--- Per-previous-branch-type breakdown ---\n";
//       print_per_type(oss, "hits          ", stats.per_prev_type_hits);
//       print_per_type(oss, "misses        ", stats.per_prev_type_misses);
//       print_per_type(oss, "mispredictions", stats.per_prev_type_mispredictions);
//       print_per_type(oss, "correct       ", stats.per_prev_type_correct);

//       oss << "=================================================\n";
//       std::cerr << oss.str();
//     }
//     std::cerr << std::flush;
//   }
// };

// static MultiBlockBTBContext g_ctx;

// } // namespace

// void O3_CPU::initialize_btb()
// {
//   g_ctx.MBTB.insert({this, champsim::msl::lru_table<mbtb_entry_t>{BTB_SET, BTB_WAY}});
//   std::fill(std::begin(g_ctx.INDIRECT_BTB[this]), std::end(g_ctx.INDIRECT_BTB[this]), 0);
//   std::fill(std::begin(g_ctx.CALL_SIZE[this]), std::end(g_ctx.CALL_SIZE[this]), 4);
//   g_ctx.CONDITIONAL_HISTORY[this] = 0;
//   g_ctx.LAST_BRANCH_IP[this] = 0;
//   g_ctx.LAST_INDIRECT_TARGET[this] = 0;
//   g_ctx.LAST_TRANSITION[this] = mbtb_transition::N;
//   g_ctx.LAST_BRANCH_WAS_RETURN[this] = false;
//   g_ctx.LAST_RETURN_CALL_IP[this] = 0;
//   g_ctx.PENDING_SAS_ENTRY[this] = sas_record_t{};
//   g_ctx.RAS[this].clear();
//   g_ctx.SAS[this].clear();
//   g_ctx.PRED_QUEUE[this].clear();
//   g_ctx.LAST_BRANCH_TYPE_FOR_STATS[this] = 0;
//   g_ctx.HAS_LAST_BRANCH[this] = false;
//   g_ctx.STATS[this] = {};
// }

// std::pair<uint64_t, uint8_t> O3_CPU::btb_prediction(uint64_t ip)
// {
//   auto prev_ip   = g_ctx.LAST_BRANCH_IP[this];
//   auto trans     = g_ctx.LAST_TRANSITION[this] == mbtb_transition::R
//                        ? mbtb_transition::T : g_ctx.LAST_TRANSITION[this];
//   bool was_ret   = g_ctx.LAST_BRANCH_WAS_RETURN[this];
//   auto& pend_sas = g_ctx.PENDING_SAS_ENTRY[this];
//   uint64_t pr_call_ip  = g_ctx.LAST_RETURN_CALL_IP[this];
//   uint64_t ind_ctx = OPTIMIZATION_ON ? g_ctx.LAST_INDIRECT_TARGET[this] : 0;   // NEW

//   uint64_t predicted_target = 0;
//   uint8_t  always_taken     = false;
//   branch_info pred_type     = branch_info::ALWAYS_TAKEN;
//   bool was_hit              = false;

//   if (was_ret && pend_sas.valid) {
//     was_hit = true;
//     pred_type = pend_sas.type;
//     predicted_target = pend_sas.target;
//   } else {
//     auto entry = g_ctx.MBTB.at(this).check_hit(
//         {prev_ip, 0, branch_info::ALWAYS_TAKEN, trans, ind_ctx});
//     if (entry.has_value()) {
//       was_hit = true;
//       pred_type = entry->type;
//       predicted_target = entry->target;
//     }
//   }

//   if (was_hit) {
//     if (pred_type == branch_info::RETURN) {
//       if (std::empty(g_ctx.RAS[this])) {
//         predicted_target = 0;
//         always_taken = true;
//       } else {
//         auto target = g_ctx.RAS[this].back();
//         auto size = g_ctx.CALL_SIZE[this][target % std::size(g_ctx.CALL_SIZE[this])];
//         predicted_target = target + size;
//         always_taken = true;
//       }
//     } else if (pred_type == branch_info::INDIRECT) {
//       uint64_t hash = 0;
//       if (was_ret && pend_sas.valid) {
//         hash = (pr_call_ip) ^ g_ctx.CONDITIONAL_HISTORY[this].to_ullong() ^ static_cast<uint64_t>(mbtb_transition::R);
//       } else {
//         hash = ((prev_ip) ^ g_ctx.CONDITIONAL_HISTORY[this].to_ullong()) ^ static_cast<uint64_t>(trans);
//       }
//       predicted_target = g_ctx.INDIRECT_BTB[this][hash % std::size(g_ctx.INDIRECT_BTB[this])];
//       always_taken = true;
//     } else if (pred_type == branch_info::CONDITIONAL) {
//       always_taken = false;
//     } else {
//       always_taken = true;
//     }
//   }

//   g_ctx.PRED_QUEUE[this].push_back({ip, predicted_target, was_hit});
//   return {predicted_target, always_taken};
// }

// void O3_CPU::update_btb(uint64_t ip, uint64_t branch_target, uint8_t taken, uint8_t branch_type)
// {
//   auto& stats = g_ctx.STATS[this];

//   // ---- Drain PRED_QUEUE ------------------------------------------------
//   auto& q = g_ctx.PRED_QUEUE[this];
//   bool found = false;
//   pending_mbtb_pred_t pred{};
//   while (!q.empty()) {
//     pred = q.front();
//     q.pop_front();
//     if (pred.ip == ip) { found = true; break; }
//   }

//   // Compute the transition this branch produces.
//   mbtb_transition actual_trans;
//   if (branch_type == BRANCH_DIRECT_CALL || branch_type == BRANCH_INDIRECT_CALL) {
//     actual_trans = mbtb_transition::R;
//   } else if (taken &&
//              branch_type != BRANCH_RETURN &&
//              branch_type != BRANCH_DIRECT_CALL &&
//              branch_type != BRANCH_INDIRECT_CALL) {
//     actual_trans = mbtb_transition::T;
//   } else {
//     actual_trans = mbtb_transition::N;
//   }

//   bool prev_was_return = g_ctx.LAST_BRANCH_WAS_RETURN[this];
//   uint64_t pr_call_ip  = g_ctx.LAST_RETURN_CALL_IP[this];

//   // ---- Hit/miss + misprediction/correct accounting ---------------------
//   int  ti               = branch_type_idx(branch_type);
//   bool was_hit          = found && pred.was_hit;
//   bool target_required  = taken
//                           || (branch_type != BRANCH_CONDITIONAL && branch_type != BRANCH_OTHER);
//   bool target_correct   = was_hit && (pred.target == branch_target);
//   bool is_misprediction = target_required && !target_correct;

//   if (was_hit) {
//     stats.total_hits++;
//     stats.per_type_hits[ti]++;
//   } else {
//     stats.total_misses++;
//     stats.per_type_misses[ti]++;
//   }

//   if (is_misprediction) {
//     stats.total_mispredictions++;
//     stats.per_type_mispredictions[ti]++;
//   } else {
//     stats.total_correct++;
//     stats.per_type_correct[ti]++;
//   }

//   // Per-previous-branch-type split (skips the very first branch).
//   if (g_ctx.HAS_LAST_BRANCH[this]) {
//     int pti = branch_type_idx(g_ctx.LAST_BRANCH_TYPE_FOR_STATS[this]);
//     if (was_hit) stats.per_prev_type_hits[pti]++;
//     else         stats.per_prev_type_misses[pti]++;
//     if (is_misprediction) stats.per_prev_type_mispredictions[pti]++;
//     else                  stats.per_prev_type_correct[pti]++;
//   }

//   // ---- CALL: RAS + SAS push (UNCHANGED) --------------------------------
//   if (branch_type == BRANCH_DIRECT_CALL || branch_type == BRANCH_INDIRECT_CALL) {
//     g_ctx.RAS[this].push_back(ip);

//     sas_record_t snap{};
//     auto entry = g_ctx.MBTB.at(this).check_hit({ip, 0, branch_info::ALWAYS_TAKEN, mbtb_transition::R, 0});
//     if (entry.has_value()) {
//       snap.target = entry->target;
//       snap.type   = entry->type;
//       snap.valid  = true;
//     } else {
//       snap.valid = false;
//     }

//     g_ctx.SAS[this].push_back(snap);

//     if (std::size(g_ctx.RAS[this]) > RAS_SIZE) g_ctx.RAS[this].pop_front();
//     if (std::size(g_ctx.SAS[this]) > SAS_SIZE) g_ctx.SAS[this].pop_front();
//   }

//   // ---- RETURN: pop RAS + SAS (UNCHANGED) -------------------------------
//   bool         just_handled_return = false;
//   uint64_t     popped_call_ip      = 0;
//   sas_record_t popped_sas{};

//   if (branch_type == BRANCH_RETURN && !std::empty(g_ctx.RAS[this])) {
//     popped_call_ip = g_ctx.RAS[this].back();
//     g_ctx.RAS[this].pop_back();

//     if (!std::empty(g_ctx.SAS[this])) {
//       popped_sas = g_ctx.SAS[this].back();
//       g_ctx.SAS[this].pop_back();
//     } else {
//       popped_sas.valid = false;
//     }

//     auto estimated_size = std::abs((long)(popped_call_ip - branch_target));
//     if (estimated_size <= 10) {
//       g_ctx.CALL_SIZE[this][popped_call_ip % std::size(g_ctx.CALL_SIZE[this])] = estimated_size;
//     }

//     just_handled_return = true;
//   }

//   // ---- Indirect target / history (UNCHANGED) ---------------------------
//   auto prev_ip    = g_ctx.LAST_BRANCH_IP[this];
//   auto prev_trans = g_ctx.LAST_TRANSITION[this] == mbtb_transition::R ? mbtb_transition::T : g_ctx.LAST_TRANSITION[this];
//   auto& pend_sas = g_ctx.PENDING_SAS_ENTRY[this];

//   if (branch_type == BRANCH_INDIRECT || branch_type == BRANCH_INDIRECT_CALL) {
//     uint64_t hash = 0;
//     if (prev_was_return && pend_sas.valid) {
//       hash = (pr_call_ip) ^ g_ctx.CONDITIONAL_HISTORY[this].to_ullong() ^ static_cast<uint64_t>(mbtb_transition::R);
//     } else {
//       hash = ((prev_ip) ^ g_ctx.CONDITIONAL_HISTORY[this].to_ullong()) ^ static_cast<uint64_t>(prev_trans);
//     }
//     g_ctx.INDIRECT_BTB[this][hash % std::size(g_ctx.INDIRECT_BTB[this])]
//         = branch_target;
//   }

//   if (branch_type == BRANCH_CONDITIONAL || branch_type == BRANCH_OTHER) {
//     g_ctx.CONDITIONAL_HISTORY[this] <<= 1;
//     g_ctx.CONDITIONAL_HISTORY[this].set(0, taken);
//   }

//   // ---- Resolve type ----------------------------------------------------
//   auto type = branch_info::ALWAYS_TAKEN;
//   if      (branch_type == BRANCH_INDIRECT || branch_type == BRANCH_INDIRECT_CALL) type = branch_info::INDIRECT;
//   else if (branch_type == BRANCH_RETURN)                                          type = branch_info::RETURN;
//   else if (branch_type == BRANCH_CONDITIONAL || branch_type == BRANCH_OTHER)      type = branch_info::CONDITIONAL;

//   // ---- MBTB write (UNCHANGED) ------------------------------------------
//   if (prev_was_return) {
//     if (branch_target != 0) {
//       auto opt_entry = g_ctx.MBTB.at(this).check_hit({pr_call_ip, branch_target, type, mbtb_transition::R, 0});
//       if (opt_entry.has_value()) {
//         opt_entry->target = branch_target;
//         opt_entry->type = type;
//       }
//       g_ctx.MBTB.at(this).fill(opt_entry.value_or(mbtb_entry_t{pr_call_ip, branch_target, type, mbtb_transition::R, 0}));
//     }
//   } else {
//     prev_trans = g_ctx.LAST_TRANSITION[this] == mbtb_transition::R
//                      ? mbtb_transition::T : g_ctx.LAST_TRANSITION[this];

//     uint64_t ind_ctx = OPTIMIZATION_ON ? g_ctx.LAST_INDIRECT_TARGET[this] : 0;   // NEW

//     auto opt_entry = g_ctx.MBTB.at(this).check_hit({prev_ip, branch_target, type, prev_trans, ind_ctx});
//     if (opt_entry.has_value()) {
//       if (branch_target != 0) opt_entry->target = branch_target;
//       opt_entry->type = type;
//     }
//     if (branch_target != 0) {
//       g_ctx.MBTB.at(this).fill(
//           opt_entry.value_or(mbtb_entry_t{prev_ip, branch_target, type, prev_trans, ind_ctx}));
//     }
//   }

//   // ---- Roll forward state ----------------------------------------------
//   g_ctx.LAST_BRANCH_IP[this]  = ip;
//   g_ctx.LAST_TRANSITION[this] = actual_trans;

//   if (just_handled_return) {
//     g_ctx.LAST_BRANCH_WAS_RETURN[this] = true;
//     g_ctx.LAST_RETURN_CALL_IP[this]    = popped_call_ip;
//     g_ctx.PENDING_SAS_ENTRY[this]      = popped_sas;
//   } else {
//     g_ctx.LAST_BRANCH_WAS_RETURN[this]  = false;
//     g_ctx.PENDING_SAS_ENTRY[this].valid = false;
//   }

//   if (branch_type == BRANCH_INDIRECT || branch_type == BRANCH_INDIRECT_CALL) {
//     g_ctx.LAST_INDIRECT_TARGET[this] = branch_target;
//   } else {
//     g_ctx.LAST_INDIRECT_TARGET[this] = 0;
//   }

//   g_ctx.LAST_BRANCH_TYPE_FOR_STATS[this] = branch_type;
//   g_ctx.HAS_LAST_BRANCH[this] = true;
// }




// #include <algorithm>
// #include <array>
// #include <bitset>
// #include <cstdint>
// #include <cstdlib>
// #include <deque>
// #include <iomanip>
// #include <iostream>
// #include <map>
// #include <sstream>
// #include <utility>

// #include "msl/lru_table.h"
// #include "ooo_cpu.h"

// #define OPTIMIZATION_ON 0

// namespace
// {

// enum class branch_info {
//   INDIRECT,
//   RETURN,
//   ALWAYS_TAKEN,
//   CONDITIONAL,
// };

// enum class mbtb_transition : uint8_t { T, N, R };

// struct mbtb_entry_t {
//   uint64_t ip_tag = 0;
//   uint64_t block_start = 0;   // Ai: block start     -> index   [NEW]
//   uint64_t target = 0;
//   branch_info type = branch_info::ALWAYS_TAKEN;
//   mbtb_transition transition = mbtb_transition::N;
//   uint64_t ind_ctx = 0;  // 0 unless predecessor was indirect


//   auto index() const { return block_start; }
//   auto tag() const { return (ip_tag) ^ static_cast<uint64_t>(transition) ^ ind_ctx; }
// };

// struct sas_record_t {
//   uint64_t target = 0;
//   branch_info type = branch_info::ALWAYS_TAKEN;
//   bool valid = false;
//   uint64_t block_start = 0;   // [NEW] Ai of the call's block
// };

// struct pending_mbtb_pred_t {
//   uint64_t ip;
//   uint64_t target;
//   bool was_hit;
// };

// // -------- per-branch-type indexing -----------------------------------------
// constexpr int N_BRANCH_TYPES = 7;
// static int branch_type_idx(uint8_t bt) {
//   switch (bt) {
//     case BRANCH_DIRECT_JUMP:   return 0;
//     case BRANCH_INDIRECT:      return 1;
//     case BRANCH_CONDITIONAL:   return 2;
//     case BRANCH_DIRECT_CALL:   return 3;
//     case BRANCH_INDIRECT_CALL: return 4;
//     case BRANCH_RETURN:        return 5;
//     default:                   return 6;
//   }
// }
// static const char* const BRANCH_TYPE_NAMES[N_BRANCH_TYPES] = {
//   "DIRECT_JUMP", "INDIRECT", "CONDITIONAL", "DIRECT_CALL",
//   "INDIRECT_CALL", "RETURN", "OTHER"
// };

// struct mbtb_stats_t {
//   // Aggregate counters.
//   uint64_t total_hits           = 0;
//   uint64_t total_misses         = 0;
//   uint64_t total_mispredictions = 0;
//   uint64_t total_correct        = 0;

//   // Per-branch-type breakdown (indexed by current branch's actual type).
//   uint64_t per_type_hits          [N_BRANCH_TYPES] = {};
//   uint64_t per_type_misses        [N_BRANCH_TYPES] = {};
//   uint64_t per_type_mispredictions[N_BRANCH_TYPES] = {};
//   uint64_t per_type_correct       [N_BRANCH_TYPES] = {};

//   // Per-PREVIOUS-branch-type breakdown (indexed by the type of the branch
//   // updated immediately before this one). The very first branch in the
//   // trace has no predecessor and is therefore not counted in this split.
//   uint64_t per_prev_type_hits          [N_BRANCH_TYPES] = {};
//   uint64_t per_prev_type_misses        [N_BRANCH_TYPES] = {};
//   uint64_t per_prev_type_mispredictions[N_BRANCH_TYPES] = {};
//   uint64_t per_prev_type_correct       [N_BRANCH_TYPES] = {};
// };

// constexpr std::size_t BTB_SET = 2048;
// constexpr std::size_t BTB_WAY = 4;
// constexpr std::size_t BTB_INDIRECT_SIZE = 4096;
// constexpr std::size_t RAS_SIZE = 64;
// constexpr std::size_t SAS_SIZE = 64;
// constexpr std::size_t CALL_SIZE_TRACKERS = 1024;

// class MultiBlockBTBContext {
//  public:
//   std::map<O3_CPU*, champsim::msl::lru_table<mbtb_entry_t>> MBTB;
//   std::map<O3_CPU*, std::array<uint64_t, BTB_INDIRECT_SIZE>> INDIRECT_BTB;
//   std::map<O3_CPU*, std::bitset<champsim::lg2(BTB_INDIRECT_SIZE)>> CONDITIONAL_HISTORY;
//   std::map<O3_CPU*, std::deque<uint64_t>> RAS;
//   std::map<O3_CPU*, std::deque<sas_record_t>> SAS;
//   std::map<O3_CPU*, std::array<uint64_t, CALL_SIZE_TRACKERS>> CALL_SIZE;
//   std::map<O3_CPU*, uint64_t> LAST_BRANCH_IP;
//   std::map<O3_CPU*, mbtb_transition> LAST_TRANSITION;
//   std::map<O3_CPU*, bool> LAST_BRANCH_WAS_RETURN;
//   std::map<O3_CPU*, uint64_t> LAST_RETURN_CALL_IP;
//   std::map<O3_CPU*, sas_record_t> PENDING_SAS_ENTRY;
//   std::map<O3_CPU*, std::deque<pending_mbtb_pred_t>> PRED_QUEUE;

//   std::map<O3_CPU*, uint64_t> LAST_INDIRECT_TARGET;  // 0 unless previous branch was indirect
//   // Type of the most recently UPDATED branch -- needed so that the next
//   // update can record its prev-branch-type stats. (Stats-only state.)
//   std::map<O3_CPU*, uint8_t> LAST_BRANCH_TYPE_FOR_STATS;
//   std::map<O3_CPU*, bool>    HAS_LAST_BRANCH;
//   std::map<O3_CPU*, mbtb_stats_t> STATS;

//   std::map<O3_CPU*, uint64_t> CUR_BLOCK_START;      // live: start of block being fetched
//   std::map<O3_CPU*, bool>     NEW_BLOCK_PENDING;    // next fetched instr starts a new block
//   std::map<O3_CPU*, uint64_t> LAST_BLOCK_START_IP;  // Ai paired with LAST_BRANCH_IP

//   ~MultiBlockBTBContext() { print_all(); }

//   static void print_per_type(std::ostream& os,
//                              const char* label,
//                              const uint64_t (&arr)[N_BRANCH_TYPES]) {
//     os << "  " << label << ":";
//     for (int i = 0; i < N_BRANCH_TYPES; ++i)
//       os << " " << BRANCH_TYPE_NAMES[i] << "=" << arr[i];
//     os << "\n";
//   }

//   void print_all() {
//     for (auto& [cpu, stats] : STATS) {
//       (void)cpu;
//       std::ostringstream oss;
//       oss << std::fixed << std::setprecision(4);

//       oss << "\n========== MULTI-BLOCK BTB STATISTICS ==========\n";
//       oss << "Total hits:                " << stats.total_hits           << "\n";
//       oss << "Total misses:              " << stats.total_misses         << "\n";
//       oss << "Total mispredictions:      " << stats.total_mispredictions << "\n";
//       oss << "Total correct predictions: " << stats.total_correct        << "\n";
//       if ((stats.total_hits + stats.total_misses) > 0) {
//         oss << "Hit rate:                  "
//             << (100.0 * stats.total_hits / (stats.total_hits + stats.total_misses)) << "%\n";
//       }
//       if ((stats.total_correct + stats.total_mispredictions) > 0) {
//         oss << "Prediction accuracy:       "
//             << (100.0 * stats.total_correct
//                 / (stats.total_correct + stats.total_mispredictions)) << "%\n";
//       }

//       oss << "\n--- Per-branch-type breakdown (current branch) ---\n";
//       print_per_type(oss, "hits          ", stats.per_type_hits);
//       print_per_type(oss, "misses        ", stats.per_type_misses);
//       print_per_type(oss, "mispredictions", stats.per_type_mispredictions);
//       print_per_type(oss, "correct       ", stats.per_type_correct);

//       oss << "\n--- Per-previous-branch-type breakdown ---\n";
//       print_per_type(oss, "hits          ", stats.per_prev_type_hits);
//       print_per_type(oss, "misses        ", stats.per_prev_type_misses);
//       print_per_type(oss, "mispredictions", stats.per_prev_type_mispredictions);
//       print_per_type(oss, "correct       ", stats.per_prev_type_correct);

//       oss << "=================================================\n";
//       std::cerr << oss.str();
//     }
//     std::cerr << std::flush;
//   }
// };

// static MultiBlockBTBContext g_ctx;

// } // namespace

// void O3_CPU::initialize_btb()
// {
//   g_ctx.MBTB.insert({this, champsim::msl::lru_table<mbtb_entry_t>{BTB_SET, BTB_WAY}});
//   std::fill(std::begin(g_ctx.INDIRECT_BTB[this]), std::end(g_ctx.INDIRECT_BTB[this]), 0);
//   std::fill(std::begin(g_ctx.CALL_SIZE[this]), std::end(g_ctx.CALL_SIZE[this]), 4);
//   g_ctx.CONDITIONAL_HISTORY[this] = 0;
//   g_ctx.LAST_BRANCH_IP[this] = 0;
//   g_ctx.LAST_INDIRECT_TARGET[this] = 0;
//   g_ctx.LAST_TRANSITION[this] = mbtb_transition::N;
//   g_ctx.LAST_BRANCH_WAS_RETURN[this] = false;
//   g_ctx.LAST_RETURN_CALL_IP[this] = 0;
//   g_ctx.PENDING_SAS_ENTRY[this] = sas_record_t{};
//   g_ctx.RAS[this].clear();
//   g_ctx.SAS[this].clear();
//   g_ctx.PRED_QUEUE[this].clear();
//   g_ctx.LAST_BRANCH_TYPE_FOR_STATS[this] = 0;
//   g_ctx.HAS_LAST_BRANCH[this] = false;
//   g_ctx.STATS[this] = {};
// }

// std::pair<uint64_t, uint8_t> O3_CPU::btb_prediction(uint64_t ip)
// {

//   // Ai tracking: first instruction fetched after a branch is the new block's start.
//   if (g_ctx.NEW_BLOCK_PENDING[this]) {
//     g_ctx.CUR_BLOCK_START[this]   = ip;
//     g_ctx.NEW_BLOCK_PENDING[this] = false;
//   }
//   auto prev_block_start = g_ctx.LAST_BLOCK_START_IP[this];   // Ai of prev_ip's block


//   auto prev_ip   = g_ctx.LAST_BRANCH_IP[this];
//   auto trans     = g_ctx.LAST_TRANSITION[this] == mbtb_transition::R
//                        ? mbtb_transition::T : g_ctx.LAST_TRANSITION[this];
//   bool was_ret   = g_ctx.LAST_BRANCH_WAS_RETURN[this];
//   auto& pend_sas = g_ctx.PENDING_SAS_ENTRY[this];
//   uint64_t pr_call_ip  = g_ctx.LAST_RETURN_CALL_IP[this];
//   uint64_t ind_ctx = OPTIMIZATION_ON ? g_ctx.LAST_INDIRECT_TARGET[this] : 0;   // NEW

//   uint64_t predicted_target = 0;
//   uint8_t  always_taken     = false;
//   branch_info pred_type     = branch_info::ALWAYS_TAKEN;
//   bool was_hit              = false;

//   if (was_ret && pend_sas.valid) {
//     was_hit = true;
//     pred_type = pend_sas.type;
//     predicted_target = pend_sas.target;
//   } else {
//     auto entry = g_ctx.MBTB.at(this).check_hit(
//         {prev_ip, prev_block_start, 0, branch_info::ALWAYS_TAKEN, trans, ind_ctx});
//     if (entry.has_value()) {
//       was_hit = true;
//       pred_type = entry->type;
//       predicted_target = entry->target;
//     }
//   }

//   if (was_hit) {
//     if (pred_type == branch_info::RETURN) {
//       if (std::empty(g_ctx.RAS[this])) {
//         predicted_target = 0;
//         always_taken = true;
//       } else {
//         auto target = g_ctx.RAS[this].back();
//         auto size = g_ctx.CALL_SIZE[this][target % std::size(g_ctx.CALL_SIZE[this])];
//         predicted_target = target + size;
//         always_taken = true;
//       }
//     } else if (pred_type == branch_info::INDIRECT) {
//       uint64_t hash = 0;
//       if (was_ret && pend_sas.valid) {
//         hash = (pr_call_ip) ^ g_ctx.CONDITIONAL_HISTORY[this].to_ullong() ^ static_cast<uint64_t>(mbtb_transition::R);
//       } else {
//         hash = ((prev_ip) ^ g_ctx.CONDITIONAL_HISTORY[this].to_ullong()) ^ static_cast<uint64_t>(trans);
//       }
//       predicted_target = g_ctx.INDIRECT_BTB[this][hash % std::size(g_ctx.INDIRECT_BTB[this])];
//       always_taken = true;
//     } else if (pred_type == branch_info::CONDITIONAL) {
//       always_taken = false;
//     } else {
//       always_taken = true;
//     }
//   }

//   g_ctx.PRED_QUEUE[this].push_back({ip, predicted_target, was_hit});
//   return {predicted_target, always_taken};
// }

// void O3_CPU::update_btb(uint64_t ip, uint64_t branch_target, uint8_t taken, uint8_t branch_type)
// {
//   auto& stats = g_ctx.STATS[this];

//   // ---- Drain PRED_QUEUE ------------------------------------------------
//   auto& q = g_ctx.PRED_QUEUE[this];
//   bool found = false;
//   pending_mbtb_pred_t pred{};
//   while (!q.empty()) {
//     pred = q.front();
//     q.pop_front();
//     if (pred.ip == ip) { found = true; break; }
//   }

//   // Compute the transition this branch produces.
//   mbtb_transition actual_trans;
//   if (branch_type == BRANCH_DIRECT_CALL || branch_type == BRANCH_INDIRECT_CALL) {
//     actual_trans = mbtb_transition::R;
//   } else if (taken &&
//              branch_type != BRANCH_RETURN &&
//              branch_type != BRANCH_DIRECT_CALL &&
//              branch_type != BRANCH_INDIRECT_CALL) {
//     actual_trans = mbtb_transition::T;
//   } else {
//     actual_trans = mbtb_transition::N;
//   }


//   uint64_t cur_block_start  = g_ctx.CUR_BLOCK_START[this];      // Ai of THIS branch's block
//   uint64_t prev_block_start = g_ctx.LAST_BLOCK_START_IP[this];  // Ai of prev_ip's block
//   bool prev_was_return = g_ctx.LAST_BRANCH_WAS_RETURN[this];
//   uint64_t pr_call_ip  = g_ctx.LAST_RETURN_CALL_IP[this];

//   // ---- Hit/miss + misprediction/correct accounting ---------------------
//   int  ti               = branch_type_idx(branch_type);
//   bool was_hit          = found && pred.was_hit;
//   bool target_required  = taken
//                           || (branch_type != BRANCH_CONDITIONAL && branch_type != BRANCH_OTHER);
//   bool target_correct   = was_hit && (pred.target == branch_target);
//   bool is_misprediction = target_required && !target_correct;

//   if (was_hit) {
//     stats.total_hits++;
//     stats.per_type_hits[ti]++;
//   } else {
//     stats.total_misses++;
//     stats.per_type_misses[ti]++;
//   }

//   if (is_misprediction) {
//     stats.total_mispredictions++;
//     stats.per_type_mispredictions[ti]++;
//   } else {
//     stats.total_correct++;
//     stats.per_type_correct[ti]++;
//   }

//   // Per-previous-branch-type split (skips the very first branch).
//   if (g_ctx.HAS_LAST_BRANCH[this]) {
//     int pti = branch_type_idx(g_ctx.LAST_BRANCH_TYPE_FOR_STATS[this]);
//     if (was_hit) stats.per_prev_type_hits[pti]++;
//     else         stats.per_prev_type_misses[pti]++;
//     if (is_misprediction) stats.per_prev_type_mispredictions[pti]++;
//     else                  stats.per_prev_type_correct[pti]++;
//   }

//   // ---- CALL: RAS + SAS push (UNCHANGED) --------------------------------
//   if (branch_type == BRANCH_DIRECT_CALL || branch_type == BRANCH_INDIRECT_CALL) {
//     g_ctx.RAS[this].push_back(ip);

//     sas_record_t snap{};
//     snap.block_start = cur_block_start;                           // [NEW]
//     auto entry = g_ctx.MBTB.at(this).check_hit({ip, cur_block_start, 0, branch_info::ALWAYS_TAKEN, mbtb_transition::R, 0});
//     if (entry.has_value()) {
//       snap.target = entry->target;
//       snap.type   = entry->type;
//       snap.valid  = true;
//     } else {
//       snap.valid = false;
//     }

//     g_ctx.SAS[this].push_back(snap);

//     if (std::size(g_ctx.RAS[this]) > RAS_SIZE) g_ctx.RAS[this].pop_front();
//     if (std::size(g_ctx.SAS[this]) > SAS_SIZE) g_ctx.SAS[this].pop_front();
//   }

//   // ---- RETURN: pop RAS + SAS (UNCHANGED) -------------------------------
//   bool         just_handled_return = false;
//   uint64_t     popped_call_ip      = 0;
//   sas_record_t popped_sas{};

//   if (branch_type == BRANCH_RETURN && !std::empty(g_ctx.RAS[this])) {
//     popped_call_ip = g_ctx.RAS[this].back();
//     g_ctx.RAS[this].pop_back();

//     if (!std::empty(g_ctx.SAS[this])) {
//       popped_sas = g_ctx.SAS[this].back();
//       g_ctx.SAS[this].pop_back();
//     } else {
//       popped_sas.valid = false;
//     }

//     auto estimated_size = std::abs((long)(popped_call_ip - branch_target));
//     if (estimated_size <= 10) {
//       g_ctx.CALL_SIZE[this][popped_call_ip % std::size(g_ctx.CALL_SIZE[this])] = estimated_size;
//     }

//     just_handled_return = true;
//   }

//   // ---- Indirect target / history (UNCHANGED) ---------------------------
//   auto prev_ip    = g_ctx.LAST_BRANCH_IP[this];
//   auto prev_trans = g_ctx.LAST_TRANSITION[this] == mbtb_transition::R ? mbtb_transition::T : g_ctx.LAST_TRANSITION[this];
//   auto& pend_sas = g_ctx.PENDING_SAS_ENTRY[this];

//   if (branch_type == BRANCH_INDIRECT || branch_type == BRANCH_INDIRECT_CALL) {
//     uint64_t hash = 0;
//     if (prev_was_return && pend_sas.valid) {
//       hash = (pr_call_ip) ^ g_ctx.CONDITIONAL_HISTORY[this].to_ullong() ^ static_cast<uint64_t>(mbtb_transition::R);
//     } else {
//       hash = ((prev_ip) ^ g_ctx.CONDITIONAL_HISTORY[this].to_ullong()) ^ static_cast<uint64_t>(prev_trans);
//     }
//     g_ctx.INDIRECT_BTB[this][hash % std::size(g_ctx.INDIRECT_BTB[this])]
//         = branch_target;
//   }

//   if (branch_type == BRANCH_CONDITIONAL || branch_type == BRANCH_OTHER) {
//     g_ctx.CONDITIONAL_HISTORY[this] <<= 1;
//     g_ctx.CONDITIONAL_HISTORY[this].set(0, taken);
//   }

//   // ---- Resolve type ----------------------------------------------------
//   auto type = branch_info::ALWAYS_TAKEN;
//   if      (branch_type == BRANCH_INDIRECT || branch_type == BRANCH_INDIRECT_CALL) type = branch_info::INDIRECT;
//   else if (branch_type == BRANCH_RETURN)                                          type = branch_info::RETURN;
//   else if (branch_type == BRANCH_CONDITIONAL || branch_type == BRANCH_OTHER)      type = branch_info::CONDITIONAL;

//   // ---- MBTB write (UNCHANGED) ------------------------------------------
//   if (prev_was_return) {
//     if (branch_target != 0) {
//       auto opt_entry = g_ctx.MBTB.at(this).check_hit({pr_call_ip, pend_sas.block_start, branch_target, type, mbtb_transition::R, 0});
//       if (opt_entry.has_value()) {
//         opt_entry->target = branch_target;
//         opt_entry->type = type;
//       }
//       g_ctx.MBTB.at(this).fill(opt_entry.value_or(mbtb_entry_t{pr_call_ip, pend_sas.block_start, branch_target, type, mbtb_transition::R, 0}));
//     }
//   } else {
//     prev_trans = g_ctx.LAST_TRANSITION[this] == mbtb_transition::R
//                      ? mbtb_transition::T : g_ctx.LAST_TRANSITION[this];

//     uint64_t ind_ctx = OPTIMIZATION_ON ? g_ctx.LAST_INDIRECT_TARGET[this] : 0;   // NEW

//     auto opt_entry = g_ctx.MBTB.at(this).check_hit({prev_ip, prev_block_start, branch_target, type, prev_trans, ind_ctx});
//     if (opt_entry.has_value()) {
//       if (branch_target != 0) opt_entry->target = branch_target;
//       opt_entry->type = type;
//     }
//     if (branch_target != 0) {
//       g_ctx.MBTB.at(this).fill(
//           opt_entry.value_or(mbtb_entry_t{prev_ip, prev_block_start, branch_target, type, prev_trans, ind_ctx}));
//     }
//   }

//   // ---- Roll forward state ----------------------------------------------
//   g_ctx.LAST_BRANCH_IP[this]  = ip;
//   g_ctx.LAST_TRANSITION[this] = actual_trans;

//   g_ctx.LAST_BLOCK_START_IP[this] = cur_block_start;  // pair Ai with the branch just stored
//   g_ctx.NEW_BLOCK_PENDING[this]   = true;             // next fetched instr begins a new block

//   if (just_handled_return) {
//     g_ctx.LAST_BRANCH_WAS_RETURN[this] = true;
//     g_ctx.LAST_RETURN_CALL_IP[this]    = popped_call_ip;
//     g_ctx.PENDING_SAS_ENTRY[this]      = popped_sas;
//   } else {
//     g_ctx.LAST_BRANCH_WAS_RETURN[this]  = false;
//     g_ctx.PENDING_SAS_ENTRY[this].valid = false;
//   }

//   if (branch_type == BRANCH_INDIRECT || branch_type == BRANCH_INDIRECT_CALL) {
//     g_ctx.LAST_INDIRECT_TARGET[this] = branch_target;
//   } else {
//     g_ctx.LAST_INDIRECT_TARGET[this] = 0;
//   }

//   g_ctx.LAST_BRANCH_TYPE_FOR_STATS[this] = branch_type;
//   g_ctx.HAS_LAST_BRANCH[this] = true;
// }



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

// Two-block-ahead BTB entry:
//   index()  = Ai  (predecessor block start)
//   tag()    = (Aa << 2) | transition  (predecessor branch PC + transition)
//   payload  = predicts Bb (ending branch of current block), its type, and Ci
struct block_btb_entry_t {
  uint64_t block_start    = 0;
  uint64_t branch_ip      = 0;
  uint8_t  transition     = 1;
  uint64_t target         = 0;
  branch_info type        = branch_info::ALWAYS_TAKEN;
  uint64_t next_branch_ip = 0;

  auto index() const { return block_start; }
  auto tag() const { return (branch_ip) ^ static_cast<uint64_t>(transition); }
};

// SAS record — same pattern as your attached multi-block BTB
struct sas_record_t {
  uint64_t target = 0;
  branch_info type = branch_info::ALWAYS_TAKEN;
  bool valid = false;
  uint64_t block_start = 0;
  uint64_t next_branch_ip = 0;
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
};

class TwoBlockBTBContext {
 public:
  std::map<O3_CPU*, champsim::msl::lru_table<block_btb_entry_t>> BLOCK_BTB;
  std::map<O3_CPU*, std::array<uint64_t, BTB_INDIRECT_SIZE>> INDIRECT_BTB;
  std::map<O3_CPU*, std::bitset<champsim::lg2(BTB_INDIRECT_SIZE)>> CONDITIONAL_HISTORY;
  std::map<O3_CPU*, std::deque<uint64_t>> RAS;
  std::map<O3_CPU*, std::deque<sas_record_t>> SAS;
  std::map<O3_CPU*, std::array<uint64_t, CALL_SIZE_TRACKERS>> CALL_SIZE;
  std::map<O3_CPU*, btb_stats_t> STATS;
  std::map<O3_CPU*, O3_CPU::fetch_block_pred> LAST_PRED;

  std::map<O3_CPU*, sas_record_t> PENDING_SAS_ENTRY;
  std::map<O3_CPU*, uint64_t> LAST_RETURN_CALL_IP;
  std::map<O3_CPU*, bool> LAST_BLOCK_WAS_RETURN;

  ~TwoBlockBTBContext() { print_all(); }

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

      oss << "\n========== TWO-BLOCK-AHEAD BTB STATISTICS ==========\n";
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

      oss << "=====================================================\n";
      std::cerr << oss.str();
    }
    std::cerr << std::flush;
  }
};

static TwoBlockBTBContext g_ctx;

static uint8_t info_to_branch_type(branch_info info) {
  switch (info) {
    case branch_info::INDIRECT:     return BRANCH_INDIRECT;
    case branch_info::RETURN:       return BRANCH_RETURN;
    case branch_info::CONDITIONAL:  return BRANCH_CONDITIONAL;
    case branch_info::ALWAYS_TAKEN: return BRANCH_DIRECT_JUMP;
    default:                        return BRANCH_OTHER;
  }
}

} // namespace

void O3_CPU::initialize_btb()
{
  g_ctx.BLOCK_BTB.insert({this, champsim::msl::lru_table<block_btb_entry_t>{BTB_SET, BTB_WAY}});
  std::fill(std::begin(g_ctx.INDIRECT_BTB[this]), std::end(g_ctx.INDIRECT_BTB[this]), 0);
  std::fill(std::begin(g_ctx.CALL_SIZE[this]), std::end(g_ctx.CALL_SIZE[this]), 4);
  g_ctx.CONDITIONAL_HISTORY[this] = 0;
  g_ctx.STATS[this] = {};
  g_ctx.LAST_PRED[this] = {};
  g_ctx.SAS[this].clear();
  g_ctx.PENDING_SAS_ENTRY[this] = sas_record_t{};
  g_ctx.LAST_RETURN_CALL_IP[this] = 0;
  g_ctx.LAST_BLOCK_WAS_RETURN[this] = false;
}

/* ------------------------------------------------------------------ */
/*  Called at the start of every fetch block.                         */
/*  If a return was processed in the previous cycle, consume the     */
/*  pending SAS entry instead of doing a normal BTB lookup.           */
/* ------------------------------------------------------------------ */
void O3_CPU::btb_begin_block(uint64_t pred_block_start, uint64_t pred_branch_ip, uint8_t pred_transition)
{
  auto& pend_sas = g_ctx.PENDING_SAS_ENTRY[this];

  uint64_t pred_branch_ip_out = 0;
  uint64_t pred_target        = 0;
  bool     always_taken       = false;
  uint8_t  pred_type          = NOT_BRANCH;
  bool     was_hit            = false;

  pred_transition = pred_transition == 2 ? 0 : pred_transition;

  // 1. Pending SAS from a previous return (Figure 5b)
  if (g_ctx.LAST_BLOCK_WAS_RETURN[this] && pend_sas.valid) {
    was_hit = true;
    pred_branch_ip_out = pend_sas.next_branch_ip;
    pred_type          = info_to_branch_type(pend_sas.type);
    always_taken       = (pend_sas.type != ::branch_info::CONDITIONAL);

    if (pend_sas.type == ::branch_info::RETURN) {
      if (!std::empty(g_ctx.RAS[this])) {
        auto target = g_ctx.RAS[this].back();
        auto size   = g_ctx.CALL_SIZE[this][target % std::size(g_ctx.CALL_SIZE[this])];
        pred_target = target + size;
        always_taken = true;
      } else {
        pred_target = 0;
        always_taken = true;
      }
    }
    else if (pend_sas.type == ::branch_info::INDIRECT) {
      uint64_t hash = (g_ctx.LAST_RETURN_CALL_IP[this]) ^ g_ctx.CONDITIONAL_HISTORY[this].to_ullong() ^ static_cast<uint64_t>(2);
      pred_target = g_ctx.INDIRECT_BTB[this][hash % std::size(g_ctx.INDIRECT_BTB[this])];
      always_taken = true;
    }
    else {
      pred_target = pend_sas.target;
    }
  }
  else {
    // 2. Normal two-block-ahead BTB lookup
    auto btb_entry = g_ctx.BLOCK_BTB.at(this).check_hit(
        {pred_block_start, pred_branch_ip, pred_transition, 0, ::branch_info::ALWAYS_TAKEN, 0});

    if (btb_entry.has_value()) {
      was_hit = true;
      pred_branch_ip_out = btb_entry->next_branch_ip;
      pred_type          = info_to_branch_type(btb_entry->type);
      always_taken       = (btb_entry->type != ::branch_info::CONDITIONAL);

      if (btb_entry->type == ::branch_info::RETURN) {
        if (!std::empty(g_ctx.RAS[this])) {
          auto target = g_ctx.RAS[this].back();
          auto size   = g_ctx.CALL_SIZE[this][target % std::size(g_ctx.CALL_SIZE[this])];
          pred_target = target + size;
          always_taken = true;
        } else {
          pred_target = 0;
          always_taken = true;
        }
      }
      else if (btb_entry->type == ::branch_info::INDIRECT) {
        auto hash = (pred_branch_ip) ^ g_ctx.CONDITIONAL_HISTORY[this].to_ullong() ^ static_cast<uint64_t>(pred_transition);
        pred_target = g_ctx.INDIRECT_BTB[this][hash % std::size(g_ctx.INDIRECT_BTB[this])];
        always_taken = true;
      }
      else {
        pred_target = btb_entry->target;
      }
    }
  }

  this->fb_pred.pred_branch_ip    = pred_branch_ip_out;
  this->fb_pred.pred_target       = pred_target;
  this->fb_pred.pred_always_taken = always_taken;
  this->fb_pred.pred_branch_type  = pred_type;

  g_ctx.LAST_PRED[this] = this->fb_pred;
}

/* ------------------------------------------------------------------ */
/*  Legacy per-instruction interface — no-op.                         */
/* ------------------------------------------------------------------ */
std::pair<uint64_t, uint8_t> O3_CPU::btb_prediction(uint64_t ip)
{
  (void)ip;
  return {0, false};
}

void O3_CPU::update_btb(uint64_t ip, uint64_t branch_target, uint8_t taken, uint8_t branch_type)
{
  (void)ip;
  (void)branch_target;
  (void)taken;
  (void)branch_type;
  // All BTB-local updates are consolidated in btb_update_block()
}

/* ------------------------------------------------------------------ */
/*  Called once when a fetch block ends.                              */
/* ------------------------------------------------------------------ */
void O3_CPU::btb_update_block(uint64_t pred_block_start, uint64_t pred_branch_ip, uint8_t pred_transition,
                              uint64_t block_start, uint64_t branch_ip,
                              uint64_t branch_target, uint8_t taken,
                              uint8_t branch_type)
{
  auto& stats = g_ctx.STATS[this];
  int  ti = branch_type_idx(branch_type);

  // ---- Stats: compare actual outcome against prediction saved at btb_begin_block ----
  auto& pred = g_ctx.LAST_PRED[this];
  bool was_hit = false;

  if (branch_type == NOT_BRANCH) {
    was_hit = (pred.pred_branch_ip == 0);
  } else {
    was_hit = (pred.pred_branch_ip == branch_ip);
  }

  bool target_required = (branch_type != NOT_BRANCH) &&
                         (taken || (branch_type != BRANCH_CONDITIONAL && branch_type != BRANCH_OTHER));
  bool target_correct  = was_hit && (pred.pred_target == branch_target);
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

  // ---- Map branch type to internal enum ----
  auto type = ::branch_info::ALWAYS_TAKEN;
  if      ((branch_type == BRANCH_INDIRECT) || (branch_type == BRANCH_INDIRECT_CALL)) type = ::branch_info::INDIRECT;
  else if (branch_type == BRANCH_RETURN)                                              type = ::branch_info::RETURN;
  else if ((branch_type == BRANCH_CONDITIONAL) || (branch_type == BRANCH_OTHER))      type = ::branch_info::CONDITIONAL;
  else if (branch_type == NOT_BRANCH)                                                 type = ::branch_info::ALWAYS_TAKEN;

  uint64_t next_block_start = 0;
  if (branch_type != NOT_BRANCH && (taken || type != ::branch_info::CONDITIONAL)) {
    next_block_start = branch_target;
  }

  // ---- Write BTB entry PpR for the block after a return target (Figure 5c) ----
  if (g_ctx.LAST_BLOCK_WAS_RETURN[this] && g_ctx.PENDING_SAS_ENTRY[this].valid) {
    auto& pend_sas = g_ctx.PENDING_SAS_ENTRY[this];
    auto opt_entry = g_ctx.BLOCK_BTB.at(this).check_hit(
        {pend_sas.block_start, g_ctx.LAST_RETURN_CALL_IP[this], 2, 0, ::branch_info::ALWAYS_TAKEN, 0});
    if (opt_entry.has_value()) {
      opt_entry->next_branch_ip = branch_ip;
      opt_entry->type           = type;
      opt_entry->target         = next_block_start;
    }
    g_ctx.BLOCK_BTB.at(this).fill(
        opt_entry.value_or(::block_btb_entry_t{pend_sas.block_start,
                                               g_ctx.LAST_RETURN_CALL_IP[this],
                                               2, next_block_start, type, branch_ip}));
  }

  // ---- CALL: push RAS + SAS (Figure 5a) ----
  if (branch_type == BRANCH_DIRECT_CALL || branch_type == BRANCH_INDIRECT_CALL) {
    g_ctx.RAS[this].push_back(branch_ip);
    if (std::size(g_ctx.RAS[this]) > RAS_SIZE) g_ctx.RAS[this].pop_front();

    // Search BTB for PpR entry to push into SAS
    auto sas_btb = g_ctx.BLOCK_BTB.at(this).check_hit(
        {block_start, branch_ip, 2, 0, ::branch_info::ALWAYS_TAKEN, 0});
    sas_record_t snap{};
    snap.block_start = block_start;
    if (sas_btb.has_value()) {
      snap.target         = sas_btb->target;
      snap.type           = sas_btb->type;
      snap.valid          = true;
      snap.next_branch_ip = sas_btb->next_branch_ip;
    } else {
      snap.valid = false;
    }
    g_ctx.SAS[this].push_back(snap);
    if (std::size(g_ctx.SAS[this]) > RAS_SIZE) g_ctx.SAS[this].pop_front();
  }

  // ---- RETURN: pop RAS + SAS (Figure 5b) ----
  bool     just_handled_return = false;
  uint64_t popped_call_ip      = 0;
  sas_record_t popped_sas{};

  if (branch_type == BRANCH_RETURN && !std::empty(g_ctx.RAS[this])) {
    popped_call_ip = g_ctx.RAS[this].back();
    g_ctx.RAS[this].pop_back();

    if (!g_ctx.SAS[this].empty()) {
      popped_sas = g_ctx.SAS[this].back();
      g_ctx.SAS[this].pop_back();
    } else {
      popped_sas.valid = false;
    }

    auto estimated_size = (popped_call_ip > branch_target)
                              ? popped_call_ip - branch_target
                              : branch_target - popped_call_ip;
    if (estimated_size <= 10) {
      g_ctx.CALL_SIZE[this][popped_call_ip % std::size(g_ctx.CALL_SIZE[this])] = estimated_size;
    }

    just_handled_return = true;
  }


  pred_transition = pred_transition == 2 ? 0 : pred_transition;


  // ---- Indirect target / conditional history updates ----
  if ((branch_type == BRANCH_INDIRECT) || (branch_type == BRANCH_INDIRECT_CALL)) {
    uint64_t hash = 0;
    if (g_ctx.LAST_BLOCK_WAS_RETURN[this] && g_ctx.PENDING_SAS_ENTRY[this].valid) {
      hash = (g_ctx.LAST_RETURN_CALL_IP[this]) ^ g_ctx.CONDITIONAL_HISTORY[this].to_ullong() ^ static_cast<uint64_t>(2);
    } else {
      hash = ((pred_branch_ip) ^ g_ctx.CONDITIONAL_HISTORY[this].to_ullong()) ^ static_cast<uint64_t>(pred_transition);
    }
    g_ctx.INDIRECT_BTB[this][hash % std::size(g_ctx.INDIRECT_BTB[this])] = branch_target;
  }

  if ((branch_type == BRANCH_CONDITIONAL) || (branch_type == BRANCH_OTHER)) {
    g_ctx.CONDITIONAL_HISTORY[this] <<= 1;
    g_ctx.CONDITIONAL_HISTORY[this].set(0, taken);
  }

  // ---- Normal two-block-ahead BTB write (when predecessor was not a return) ----
  if (!g_ctx.LAST_BLOCK_WAS_RETURN[this]) {
    auto opt_entry = g_ctx.BLOCK_BTB.at(this).check_hit(
        {pred_block_start, pred_branch_ip, pred_transition, 0, ::branch_info::ALWAYS_TAKEN, 0});
    if (opt_entry.has_value()) {
      opt_entry->next_branch_ip = branch_ip;
      opt_entry->type           = type;
      opt_entry->target         = next_block_start;
    }
    g_ctx.BLOCK_BTB.at(this).fill(
        opt_entry.value_or(::block_btb_entry_t{pred_block_start, pred_branch_ip, pred_transition,
                                               next_block_start, type, branch_ip}));
  }

  // ---- Roll forward SAS / return state ----
  if (just_handled_return) {
    g_ctx.LAST_BLOCK_WAS_RETURN[this] = true;
    g_ctx.LAST_RETURN_CALL_IP[this]   = popped_call_ip;
    g_ctx.PENDING_SAS_ENTRY[this]     = popped_sas;
  } else {
    g_ctx.LAST_BLOCK_WAS_RETURN[this]  = false;
    g_ctx.PENDING_SAS_ENTRY[this].valid = false;
  }
}