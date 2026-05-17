// #include <algorithm>
// #include <array>
// #include <bitset>
// #include <cstdint>
// #include <cstdlib>
// #include <deque>
// #include <fstream>
// #include <iomanip>
// #include <iostream>
// #include <map>
// #include <sstream>
// #include <unordered_map>
// #include <utility>

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

// enum class mbtb_transition : uint8_t { T, N, R };

// struct mbtb_entry_t {
//   uint64_t ip_tag = 0;
//   uint64_t target = 0;
//   branch_info type = branch_info::ALWAYS_TAKEN;
//   mbtb_transition transition = mbtb_transition::N;

//   auto index() const { return ip_tag; }
//   auto tag() const { return (ip_tag) ^ static_cast<uint64_t>(transition); }
// };

// struct sas_record_t {
//   uint64_t target = 0;
//   branch_info type = branch_info::ALWAYS_TAKEN;
//   bool valid = false;
// };

// struct pending_mbtb_pred_t {
//   uint64_t ip;
//   uint64_t prev_ip;
//   mbtb_transition trans;
//   uint64_t target;
//   branch_info type;
//   bool was_hit;
//   bool ras_empty;
//   bool from_sas;
// };

// // -------- per-actual-type indexing (MIRRORS the baseline file) ------------
// constexpr int N_ACTUAL_TYPES = 7;
// static int actual_type_idx(uint8_t bt) {
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
// static const char* const ACTUAL_TYPE_NAMES[N_ACTUAL_TYPES] = {
//   "DIRECT_JUMP", "INDIRECT", "CONDITIONAL", "DIRECT_CALL",
//   "INDIRECT_CALL", "RETURN", "OTHER"
// };
// static branch_info expected_info(uint8_t bt) {
//   if (bt == BRANCH_INDIRECT || bt == BRANCH_INDIRECT_CALL) return branch_info::INDIRECT;
//   if (bt == BRANCH_RETURN)                                 return branch_info::RETURN;
//   if (bt == BRANCH_CONDITIONAL || bt == BRANCH_OTHER)      return branch_info::CONDITIONAL;
//   return branch_info::ALWAYS_TAKEN;
// }

// struct mbtb_stats_t {
//   // ============ LEGACY (preserved) ============
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
//   uint64_t transition_hits[3]   = {0,0,0};
//   uint64_t transition_misses[3] = {0,0,0};
//   uint64_t prev_ip_match = 0;
//   uint64_t prev_ip_mismatch = 0;
//   uint64_t sas_pushes = 0;
//   uint64_t sas_pops = 0;
//   uint64_t sas_empty_on_pop = 0;
//   uint64_t sas_push_valid = 0;
//   uint64_t sas_push_invalid = 0;
//   uint64_t sas_pred_used = 0;
//   uint64_t sas_pred_fallback = 0;
//   uint64_t sas_pred_target_correct = 0;
//   uint64_t sas_pred_target_wrong = 0;
//   uint64_t sas_writes = 0;

//   // ============ NEW: per-ACTUAL-type (matches baseline file) ============
//   uint64_t actual_total[N_ACTUAL_TYPES]               = {};
//   uint64_t actual_btb_hit[N_ACTUAL_TYPES]             = {};
//   uint64_t actual_btb_miss[N_ACTUAL_TYPES]            = {};
//   uint64_t actual_type_match[N_ACTUAL_TYPES]          = {};
//   uint64_t actual_type_mismatch[N_ACTUAL_TYPES]       = {};
//   uint64_t actual_target_correct[N_ACTUAL_TYPES]      = {};
//   uint64_t actual_target_wrong[N_ACTUAL_TYPES]        = {};
//   uint64_t actual_target_dontcare[N_ACTUAL_TYPES]     = {};
//   uint64_t spurious_target_wrong_from_misclassify[N_ACTUAL_TYPES] = {};

//   uint64_t mispred_by_curr_type[N_ACTUAL_TYPES] = {};
//   uint64_t mispred_by_prev_type[N_ACTUAL_TYPES] = {};

//   // POST-RETURN split
//   uint64_t pr_total = 0;
//   uint64_t pr_actual_total[N_ACTUAL_TYPES]            = {};
//   uint64_t pr_actual_btb_hit[N_ACTUAL_TYPES]          = {};
//   uint64_t pr_actual_target_correct[N_ACTUAL_TYPES]   = {};
//   uint64_t pr_actual_target_wrong[N_ACTUAL_TYPES]     = {};
//   uint64_t pr_actual_target_dontcare[N_ACTUAL_TYPES]  = {};

//   // // NON-POST-RETURN split
//   // uint64_t npr_total = 0;
//   // uint64_t npr_actual_total[N_ACTUAL_TYPES]           = {};
//   // uint64_t npr_actual_target_correct[N_ACTUAL_TYPES]  = {};
//   // uint64_t npr_actual_target_wrong[N_ACTUAL_TYPES]    = {};
//   // uint64_t npr_actual_target_dontcare[N_ACTUAL_TYPES] = {};

//   // // ============ NEW: POST-INDIRECT split (added to test MBTB hypothesis) ===
//   // // Post-indirect == this branch's previous update was BRANCH_INDIRECT or
//   // // BRANCH_INDIRECT_CALL. The hypothesis: in MBTB the (prev_ip, trans)
//   // // lookup key is NOT unique to the predicted branch. When prev_ip is an
//   // // indirect branch dispatching to many downstream branches (e.g. an
//   // // interpreter computed-goto), one MBTB slot gets retrained with the
//   // // first branch of every handler in turn -- so the next branch is
//   // // mispredicted (wrong target AND wrong type) most of the time.
//   // // If correct, multi-block should show pi_actual_type_mismatch and
//   // // pi_actual_target_wrong concentrated heavily in this bucket.
//   // uint64_t pi_total = 0;
//   // uint64_t pi_actual_total[N_ACTUAL_TYPES]            = {};
//   // uint64_t pi_actual_btb_hit[N_ACTUAL_TYPES]          = {};
//   // uint64_t pi_actual_btb_miss[N_ACTUAL_TYPES]         = {};
//   // uint64_t pi_actual_type_match[N_ACTUAL_TYPES]       = {};
//   // uint64_t pi_actual_type_mismatch[N_ACTUAL_TYPES]    = {};
//   // uint64_t pi_actual_target_correct[N_ACTUAL_TYPES]   = {};
//   // uint64_t pi_actual_target_wrong[N_ACTUAL_TYPES]     = {};
//   // uint64_t pi_actual_target_dontcare[N_ACTUAL_TYPES]  = {};

//   // // ============ NEW: SAS-vs-MBTB attribution per ACTUAL type ============
//   // // For every prediction where pred.from_sas was true:
//   // uint64_t sas_served[N_ACTUAL_TYPES]                 = {};
//   // uint64_t sas_served_target_correct[N_ACTUAL_TYPES]  = {};
//   // uint64_t sas_served_target_wrong[N_ACTUAL_TYPES]    = {};
//   // uint64_t sas_served_target_dontcare[N_ACTUAL_TYPES] = {};
//   // uint64_t sas_served_type_match[N_ACTUAL_TYPES]      = {};
//   // uint64_t sas_served_type_mismatch[N_ACTUAL_TYPES]   = {};

//   // // Post-return predictions that fell through to MBTB (SAS invalid):
//   // uint64_t fallback_served[N_ACTUAL_TYPES]                 = {};
//   // uint64_t fallback_served_target_correct[N_ACTUAL_TYPES]  = {};
//   // uint64_t fallback_served_target_wrong[N_ACTUAL_TYPES]    = {};
//   // uint64_t fallback_served_target_dontcare[N_ACTUAL_TYPES] = {};

//   // // Non-post-return predictions (always MBTB, never SAS) -- attribution
//   // // of where their target accuracy is winning vs baseline.
//   // uint64_t mbtb_normal[N_ACTUAL_TYPES]                 = {};
//   // uint64_t mbtb_normal_target_correct[N_ACTUAL_TYPES]  = {};
//   // uint64_t mbtb_normal_target_wrong[N_ACTUAL_TYPES]    = {};
//   // uint64_t mbtb_normal_target_dontcare[N_ACTUAL_TYPES] = {};
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
//   // Tracks the branch_type of the most recently UPDATED branch -- needed to
//   // classify each update as post-return for the new statistics.
//   std::map<O3_CPU*, uint8_t> LAST_BRANCH_TYPE_FOR_STATS;
//   std::map<O3_CPU*, bool>    HAS_LAST_BRANCH;
//   std::map<O3_CPU*, mbtb_stats_t> STATS;

//   ~MultiBlockBTBContext() { print_all(); }

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

//       oss << "\n========== MULTI-BLOCK BTB STATISTICS ==========\n";
//       oss << "Branch updates:           " << stats.total_updates << "\n";
//       oss << "Total hits:               " << stats.total_hits << "\n";
//       oss << "Total misses:             " << stats.total_misses << "\n";
//       if ((stats.total_hits + stats.total_misses) > 0) {
//         oss << "Hit rate:                 "
//             << (100.0 * stats.total_hits / (stats.total_hits + stats.total_misses)) << "%\n";
//       }
//       oss << "Target correct:           " << stats.target_correct << "\n";
//       oss << "Target wrong:             " << stats.target_wrong << "\n";
//       if ((stats.target_correct + stats.target_wrong) > 0) {
//         oss << "Target accuracy:          "
//             << (100.0 * stats.target_correct / (stats.target_correct + stats.target_wrong)) << "%\n";
//       }
//       oss << "[legacy, hit=by-pred-type, miss=by-actual-type]\n";
//       oss << "Conditional hits:         " << stats.conditional_hits << "\n";
//       oss << "Conditional misses:       " << stats.conditional_misses << "\n";
//       oss << "Indirect hits:            " << stats.indirect_hits << "\n";
//       oss << "Indirect misses:          " << stats.indirect_misses << "\n";
//       oss << "Return hits:              " << stats.return_hits << "\n";
//       oss << "Return misses:            " << stats.return_misses << "\n";
//       oss << "Always taken hits:        " << stats.always_taken_hits << "\n";
//       oss << "Always taken misses:      " << stats.always_taken_misses << "\n";
//       oss << "RAS hits:                 " << stats.ras_hits << "\n";
//       oss << "RAS misses:               " << stats.ras_misses << "\n";
//       oss << "[legacy, target-* counters indexed by PREDICTED type]\n";
//       oss << "RAS target correct:       " << stats.ras_target_correct << "\n";
//       oss << "RAS target wrong:         " << stats.ras_target_wrong << "\n";
//       oss << "Indirect target correct:  " << stats.indirect_target_correct << "\n";
//       oss << "Indirect target wrong:    " << stats.indirect_target_wrong << "\n";
//       oss << "BTB target correct:       " << stats.btb_target_correct << "\n";
//       oss << "BTB target wrong:         " << stats.btb_target_wrong << "\n";

//       oss << "\n--- MBTB-specific diagnostics ---\n";
//       oss << "Transition hits T/N/R:    " << stats.transition_hits[0] << " / "
//           << stats.transition_hits[1] << " / " << stats.transition_hits[2] << "\n";
//       oss << "Transition misses T/N/R:  " << stats.transition_misses[0] << " / "
//           << stats.transition_misses[1] << " / " << stats.transition_misses[2] << "\n";
//       oss << "Prev IP match:            " << stats.prev_ip_match << "\n";
//       oss << "Prev IP mismatch:         " << stats.prev_ip_mismatch << "\n";
//       oss << "SAS pushes:               " << stats.sas_pushes << "\n";
//       oss << "  ... valid snapshots:    " << stats.sas_push_valid << "\n";
//       oss << "  ... invalid snapshots:  " << stats.sas_push_invalid << "\n";
//       oss << "SAS pops:                 " << stats.sas_pops << "\n";
//       oss << "SAS empty on pop:         " << stats.sas_empty_on_pop << "\n";
//       oss << "\n--- Legacy SAS prediction stats ---\n";
//       oss << "Post-return SAS used:     " << stats.sas_pred_used << "\n";
//       oss << "Post-return MBTB fallback:" << stats.sas_pred_fallback << "\n";
//       oss << "SAS pred target correct:  " << stats.sas_pred_target_correct << "\n";
//       oss << "SAS pred target wrong:    " << stats.sas_pred_target_wrong << "\n";
//       oss << "SAS entry writes:         " << stats.sas_writes << "\n";

//       oss << "\n--- Per-ACTUAL-type (matched with baseline file) ---\n";
//       print_per_type(oss, "actual_total                 ", stats.actual_total);
//       print_per_type(oss, "actual_btb_hit               ", stats.actual_btb_hit);
//       print_per_type(oss, "actual_btb_miss              ", stats.actual_btb_miss);
//       print_per_type(oss, "actual_type_match            ", stats.actual_type_match);
//       print_per_type(oss, "actual_type_mismatch         ", stats.actual_type_mismatch);
//       print_per_type(oss, "actual_target_correct        ", stats.actual_target_correct);
//       print_per_type(oss, "actual_target_wrong          ", stats.actual_target_wrong);
//       print_per_type(oss, "actual_target_dontcare       ", stats.actual_target_dontcare);
//       print_per_type(oss, "spurious_target_wrong_artif. ", stats.spurious_target_wrong_from_misclassify);

//       oss << "\n--- POST-RETURN branches only (matched with baseline file) ---\n";
//       oss << "  pr_total: " << stats.pr_total << "\n";
//       print_per_type(oss, "pr_actual_total              ", stats.pr_actual_total);
//       print_per_type(oss, "pr_actual_btb_hit            ", stats.pr_actual_btb_hit);
//       print_per_type(oss, "pr_actual_target_correct     ", stats.pr_actual_target_correct);
//       print_per_type(oss, "pr_actual_target_wrong       ", stats.pr_actual_target_wrong);
//       print_per_type(oss, "pr_actual_target_dontcare    ", stats.pr_actual_target_dontcare);

//       oss << "\n--- NON-POST-RETURN branches only (matched with baseline file) ---\n";
//       oss << "  npr_total: " << stats.npr_total << "\n";
//       print_per_type(oss, "npr_actual_total             ", stats.npr_actual_total);
//       print_per_type(oss, "npr_actual_target_correct    ", stats.npr_actual_target_correct);
//       print_per_type(oss, "npr_actual_target_wrong      ", stats.npr_actual_target_wrong);
//       print_per_type(oss, "npr_actual_target_dontcare   ", stats.npr_actual_target_dontcare);

//       // === NEW SECTION: post-indirect (the hypothesis test) ===
//       oss << "\n--- POST-INDIRECT branches only (prev was BRANCH_INDIRECT or BRANCH_INDIRECT_CALL) ---\n";
//       oss << "  pi_total: " << stats.pi_total << "\n";
//       print_per_type(oss, "pi_actual_total              ", stats.pi_actual_total);
//       print_per_type(oss, "pi_actual_btb_hit            ", stats.pi_actual_btb_hit);
//       print_per_type(oss, "pi_actual_btb_miss           ", stats.pi_actual_btb_miss);
//       print_per_type(oss, "pi_actual_type_match         ", stats.pi_actual_type_match);
//       print_per_type(oss, "pi_actual_type_mismatch      ", stats.pi_actual_type_mismatch);
//       print_per_type(oss, "pi_actual_target_correct     ", stats.pi_actual_target_correct);
//       print_per_type(oss, "pi_actual_target_wrong       ", stats.pi_actual_target_wrong);
//       print_per_type(oss, "pi_actual_target_dontcare    ", stats.pi_actual_target_dontcare);

//       oss << "\n--- SAS-served predictions (pred.from_sas == true) ---\n";
//       print_per_type(oss, "sas_served                   ", stats.sas_served);
//       print_per_type(oss, "sas_served_type_match        ", stats.sas_served_type_match);
//       print_per_type(oss, "sas_served_type_mismatch     ", stats.sas_served_type_mismatch);
//       print_per_type(oss, "sas_served_target_correct    ", stats.sas_served_target_correct);
//       print_per_type(oss, "sas_served_target_wrong      ", stats.sas_served_target_wrong);
//       print_per_type(oss, "sas_served_target_dontcare   ", stats.sas_served_target_dontcare);

//       oss << "\n--- Post-return predictions that FELL BACK to MBTB ---\n";
//       print_per_type(oss, "fallback_served              ", stats.fallback_served);
//       print_per_type(oss, "fallback_target_correct      ", stats.fallback_served_target_correct);
//       print_per_type(oss, "fallback_target_wrong        ", stats.fallback_served_target_wrong);
//       print_per_type(oss, "fallback_target_dontcare     ", stats.fallback_served_target_dontcare);

//       oss << "\n--- MBTB-normal (non-post-return, always MBTB) ---\n";
//       print_per_type(oss, "mbtb_normal                  ", stats.mbtb_normal);
//       print_per_type(oss, "mbtb_normal_target_correct   ", stats.mbtb_normal_target_correct);
//       print_per_type(oss, "mbtb_normal_target_wrong     ", stats.mbtb_normal_target_wrong);
//       print_per_type(oss, "mbtb_normal_target_dontcare  ", stats.mbtb_normal_target_dontcare);

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


//   uint64_t predicted_target = 0;
//   uint8_t  always_taken     = false;
//   branch_info pred_type     = branch_info::ALWAYS_TAKEN;
//   bool was_hit              = false;
//   bool ras_empty            = false;
//   bool from_sas             = false;

//   if (was_ret && pend_sas.valid) {
//     was_hit = true;
//     pred_type = pend_sas.type;
//     predicted_target = pend_sas.target;
//     from_sas = true;
//   } else {
//     auto entry = g_ctx.MBTB.at(this).check_hit(
//         {prev_ip, 0, branch_info::ALWAYS_TAKEN, trans});
//     if (entry.has_value()) {
//       was_hit = true;
//       pred_type = entry->type;
//       predicted_target = entry->target;
//     }
//   }

//   if (was_hit) {
//     if (pred_type == branch_info::RETURN) {
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
//     } else if (pred_type == branch_info::INDIRECT) {
//       // auto hash = (ip >> 2) ^ g_ctx.CONDITIONAL_HISTORY[this].to_ullong();
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

//   g_ctx.PRED_QUEUE[this].push_back({ip, prev_ip, trans, predicted_target,
//                                     pred_type, was_hit, ras_empty, from_sas});
//   return {predicted_target, always_taken};
// }

// void O3_CPU::update_btb(uint64_t ip, uint64_t branch_target, uint8_t taken, uint8_t branch_type)
// {
//   auto& stats = g_ctx.STATS[this];
//   stats.total_updates++;
//   if (taken) stats.taken_count++; else stats.not_taken_count++;

//   // ---- Detect post-return using last-UPDATED branch type ----------------
//   bool is_post_return_for_stats = g_ctx.HAS_LAST_BRANCH[this] &&
//                                   (g_ctx.LAST_BRANCH_TYPE_FOR_STATS[this] == BRANCH_RETURN);

//   // ---- NEW: Detect post-indirect (prev branch was BRANCH_INDIRECT or BRANCH_INDIRECT_CALL) ----
//   bool is_post_indirect_for_stats = g_ctx.HAS_LAST_BRANCH[this] &&
//       (g_ctx.LAST_BRANCH_TYPE_FOR_STATS[this] == BRANCH_INDIRECT ||
//        g_ctx.LAST_BRANCH_TYPE_FOR_STATS[this] == BRANCH_INDIRECT_CALL);

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

//   // ---- Per-actual-type accounting -------------------------------------
//   int ai = actual_type_idx(branch_type);
//   stats.actual_total[ai]++;
//   if (is_post_return_for_stats) {
//     stats.pr_total++;
//     stats.pr_actual_total[ai]++;
//   } else {
//     stats.npr_total++;
//     stats.npr_actual_total[ai]++;
//   }
//   // NEW: post-indirect split (independent of pr/npr).
//   if (is_post_indirect_for_stats) {
//     stats.pi_total++;
//     stats.pi_actual_total[ai]++;
//   }

//   branch_info expected = expected_info(branch_type);

//   if (found && pred.was_hit) {
//     stats.actual_btb_hit[ai]++;
//     if (is_post_return_for_stats) stats.pr_actual_btb_hit[ai]++;
//     if (pred.type == expected) stats.actual_type_match[ai]++;
//     else                       stats.actual_type_mismatch[ai]++;
//     // NEW:
//     if (is_post_indirect_for_stats) {
//       stats.pi_actual_btb_hit[ai]++;
//       if (pred.type == expected) stats.pi_actual_type_match[ai]++;
//       else                       stats.pi_actual_type_mismatch[ai]++;
//     }
//   } else {
//     stats.actual_btb_miss[ai]++;
//     // NEW:
//     if (is_post_indirect_for_stats) stats.pi_actual_btb_miss[ai]++;
//   }

//   bool true_target_used = taken || (branch_type != BRANCH_CONDITIONAL && branch_type != BRANCH_OTHER);

//   // Three-way attribution: SAS-served / fallback-served / mbtb-normal.
//   // pred.from_sas tells us the SAS path was used. If is_post_return_for_stats
//   // is true but pred.from_sas is false, this is a fallback (post-return that
//   // tried SAS but PENDING_SAS_ENTRY was invalid).
//   bool is_sas_served      = found && pred.from_sas;
//   bool is_fallback_served = found && !pred.from_sas && is_post_return_for_stats;
//   bool is_mbtb_normal     = found && !pred.from_sas && !is_post_return_for_stats;

//   if (!true_target_used) {
//     stats.actual_target_dontcare[ai]++;
//     if (is_post_return_for_stats) stats.pr_actual_target_dontcare[ai]++;
//     else                          stats.npr_actual_target_dontcare[ai]++;
//     if (is_sas_served)      stats.sas_served_target_dontcare[ai]++;
//     if (is_fallback_served) stats.fallback_served_target_dontcare[ai]++;
//     if (is_mbtb_normal)     stats.mbtb_normal_target_dontcare[ai]++;
//     // NEW:
//     if (is_post_indirect_for_stats) stats.pi_actual_target_dontcare[ai]++;
//   } else {
//     bool tgt_ok = found && pred.was_hit && (pred.target == branch_target);
//     if (tgt_ok) {
//       stats.actual_target_correct[ai]++;
//       if (is_post_return_for_stats) stats.pr_actual_target_correct[ai]++;
//       else                          stats.npr_actual_target_correct[ai]++;
//       if (is_sas_served)      stats.sas_served_target_correct[ai]++;
//       if (is_fallback_served) stats.fallback_served_target_correct[ai]++;
//       if (is_mbtb_normal)     stats.mbtb_normal_target_correct[ai]++;
//       // NEW:
//       if (is_post_indirect_for_stats) stats.pi_actual_target_correct[ai]++;
//     } else {
//       stats.actual_target_wrong[ai]++;
//       if (is_post_return_for_stats) stats.pr_actual_target_wrong[ai]++;
//       else                          stats.npr_actual_target_wrong[ai]++;
//       if (is_sas_served)      stats.sas_served_target_wrong[ai]++;
//       if (is_fallback_served) stats.fallback_served_target_wrong[ai]++;
//       if (is_mbtb_normal)     stats.mbtb_normal_target_wrong[ai]++;
//       // NEW:
//       if (is_post_indirect_for_stats) stats.pi_actual_target_wrong[ai]++;
//     }
//   }

//   if (is_sas_served) {
//     stats.sas_served[ai]++;
//     if (pred.type == expected) stats.sas_served_type_match[ai]++;
//     else                       stats.sas_served_type_mismatch[ai]++;
//   }
//   if (is_fallback_served) stats.fallback_served[ai]++;
//   if (is_mbtb_normal)     stats.mbtb_normal[ai]++;

//   if (found && pred.was_hit &&
//       (branch_type == BRANCH_CONDITIONAL || branch_type == BRANCH_OTHER) &&
//       !taken &&
//       pred.type != branch_info::CONDITIONAL) {
//     stats.spurious_target_wrong_from_misclassify[ai]++;
//   }

//   // ---- LEGACY accounting (preserved exactly from original file) --------
//   if (found) {
//     uint64_t actual_prev_ip = g_ctx.LAST_BRANCH_IP[this];
//     if (pred.prev_ip == actual_prev_ip) stats.prev_ip_match++;
//     else                                stats.prev_ip_mismatch++;

//     if (prev_was_return) {
//       if (pred.from_sas) stats.sas_pred_used++;
//       else               stats.sas_pred_fallback++;
//     }

//     if (pred.was_hit) {
//       stats.total_hits++;
//       stats.transition_hits[static_cast<int>(pred.trans)]++;
//       switch (pred.type) {
//         case branch_info::CONDITIONAL: stats.conditional_hits++; break;
//         case branch_info::INDIRECT:    stats.indirect_hits++; break;
//         case branch_info::RETURN:      stats.return_hits++; break;
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
//           if (pred.from_sas) stats.sas_pred_target_correct++;
//           switch (pred.type) {
//             case branch_info::RETURN:   stats.ras_target_correct++; break;
//             case branch_info::INDIRECT: stats.indirect_target_correct++; break;
//             default:                    stats.btb_target_correct++; break;
//           }
//         } else {
//           stats.target_wrong++;
//           if (pred.from_sas) stats.sas_pred_target_wrong++;
//           switch (pred.type) {
//             case branch_info::RETURN:   stats.ras_target_wrong++; break;
//             case branch_info::INDIRECT: stats.indirect_target_wrong++; break;
//             default:                    stats.btb_target_wrong++; break;
//           }
//         }
//       }
//     } else {
//       stats.total_misses++;
//       stats.transition_misses[static_cast<int>(pred.trans)]++;
//       switch (branch_type) {
//         case BRANCH_CONDITIONAL:
//         case BRANCH_OTHER:               stats.conditional_misses++; break;
//         case BRANCH_INDIRECT:
//         case BRANCH_INDIRECT_CALL:       stats.indirect_misses++; break;
//         case BRANCH_RETURN:              stats.return_misses++; break;
//         default:                         stats.always_taken_misses++; break;
//       }
//     }
//   } else {
//     stats.total_misses++;
//     switch (branch_type) {
//       case BRANCH_CONDITIONAL:
//       case BRANCH_OTHER:                 stats.conditional_misses++; break;
//       case BRANCH_INDIRECT:
//       case BRANCH_INDIRECT_CALL:         stats.indirect_misses++; break;
//       case BRANCH_RETURN:                stats.return_misses++; break;
//       default:                           stats.always_taken_misses++; break;
//     }
//   }

//   // ---- CALL: RAS + SAS push (UNCHANGED) --------------------------------
//   if (branch_type == BRANCH_DIRECT_CALL || branch_type == BRANCH_INDIRECT_CALL) {
//     g_ctx.RAS[this].push_back(ip);

//     sas_record_t snap{};
//     auto entry = g_ctx.MBTB.at(this).check_hit({ip, 0, branch_info::ALWAYS_TAKEN, mbtb_transition::R});
//     if (entry.has_value()) {
//       snap.target = entry->target;
//       snap.type   = entry->type;
//       snap.valid  = true;
//       stats.sas_push_valid++;
//     } else {
//       snap.valid = false;
//       stats.sas_push_invalid++;
//     }

//     g_ctx.SAS[this].push_back(snap);
//     stats.sas_pushes++;

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
//       stats.sas_pops++;
//     } else {
//       popped_sas.valid = false;
//       stats.sas_empty_on_pop++;
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
//     // auto hash = (ip >> 2) ^ g_ctx.CONDITIONAL_HISTORY[this].to_ullong();
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
//   if (branch_type == BRANCH_INDIRECT || branch_type == BRANCH_INDIRECT_CALL) type = branch_info::INDIRECT;
//   else if (branch_type == BRANCH_RETURN)                                      type = branch_info::RETURN;
//   else if (branch_type == BRANCH_CONDITIONAL || branch_type == BRANCH_OTHER)  type = branch_info::CONDITIONAL;

//   // ---- MBTB write (UNCHANGED) ------------------------------------------
//   if (prev_was_return) {
//     if (branch_target != 0) {
//       auto opt_entry = g_ctx.MBTB.at(this).check_hit({pr_call_ip, branch_target, type, mbtb_transition::R});
//       if (opt_entry.has_value()) {
//         opt_entry->target = branch_target;
//         opt_entry->type = type;
//       }
//       g_ctx.MBTB.at(this).fill(opt_entry.value_or(mbtb_entry_t{pr_call_ip, branch_target, type, mbtb_transition::R}));
//       stats.sas_writes++;
//     }
//   } else {
//     prev_trans = g_ctx.LAST_TRANSITION[this] == mbtb_transition::R
//                      ? mbtb_transition::T : g_ctx.LAST_TRANSITION[this];

//     auto opt_entry = g_ctx.MBTB.at(this).check_hit({prev_ip, branch_target, type, prev_trans});
//     if (opt_entry.has_value()) {
//       if (branch_target != 0) opt_entry->target = branch_target;
//       opt_entry->type = type;
//     }
//     if (branch_target != 0) {
//       g_ctx.MBTB.at(this).fill(
//           opt_entry.value_or(mbtb_entry_t{prev_ip, branch_target, type, prev_trans}));
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

//   g_ctx.LAST_BRANCH_TYPE_FOR_STATS[this] = branch_type;
//   g_ctx.HAS_LAST_BRANCH[this] = true;
// }



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

namespace
{

enum class branch_info {
  INDIRECT,
  RETURN,
  ALWAYS_TAKEN,
  CONDITIONAL,
};

enum class mbtb_transition : uint8_t { T, N, R };

struct mbtb_entry_t {
  uint64_t ip_tag = 0;
  uint64_t target = 0;
  branch_info type = branch_info::ALWAYS_TAKEN;
  mbtb_transition transition = mbtb_transition::N;

  auto index() const { return ip_tag; }
  auto tag() const { return (ip_tag) ^ static_cast<uint64_t>(transition); }
};

struct sas_record_t {
  uint64_t target = 0;
  branch_info type = branch_info::ALWAYS_TAKEN;
  bool valid = false;
};

struct pending_mbtb_pred_t {
  uint64_t ip;
  uint64_t target;
  bool was_hit;
};

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
    default:                   return 6;
  }
}
static const char* const BRANCH_TYPE_NAMES[N_BRANCH_TYPES] = {
  "DIRECT_JUMP", "INDIRECT", "CONDITIONAL", "DIRECT_CALL",
  "INDIRECT_CALL", "RETURN", "OTHER"
};

struct mbtb_stats_t {
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
  // Type of the most recently UPDATED branch -- needed so that the next
  // update can record its prev-branch-type stats. (Stats-only state.)
  std::map<O3_CPU*, uint8_t> LAST_BRANCH_TYPE_FOR_STATS;
  std::map<O3_CPU*, bool>    HAS_LAST_BRANCH;
  std::map<O3_CPU*, mbtb_stats_t> STATS;

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

} // namespace

void O3_CPU::initialize_btb()
{
  g_ctx.MBTB.insert({this, champsim::msl::lru_table<mbtb_entry_t>{BTB_SET, BTB_WAY}});
  std::fill(std::begin(g_ctx.INDIRECT_BTB[this]), std::end(g_ctx.INDIRECT_BTB[this]), 0);
  std::fill(std::begin(g_ctx.CALL_SIZE[this]), std::end(g_ctx.CALL_SIZE[this]), 4);
  g_ctx.CONDITIONAL_HISTORY[this] = 0;
  g_ctx.LAST_BRANCH_IP[this] = 0;
  g_ctx.LAST_TRANSITION[this] = mbtb_transition::N;
  g_ctx.LAST_BRANCH_WAS_RETURN[this] = false;
  g_ctx.LAST_RETURN_CALL_IP[this] = 0;
  g_ctx.PENDING_SAS_ENTRY[this] = sas_record_t{};
  g_ctx.RAS[this].clear();
  g_ctx.SAS[this].clear();
  g_ctx.PRED_QUEUE[this].clear();
  g_ctx.LAST_BRANCH_TYPE_FOR_STATS[this] = 0;
  g_ctx.HAS_LAST_BRANCH[this] = false;
  g_ctx.STATS[this] = {};
}

std::pair<uint64_t, uint8_t> O3_CPU::btb_prediction(uint64_t ip)
{
  auto prev_ip   = g_ctx.LAST_BRANCH_IP[this];
  auto trans     = g_ctx.LAST_TRANSITION[this] == mbtb_transition::R
                       ? mbtb_transition::T : g_ctx.LAST_TRANSITION[this];
  bool was_ret   = g_ctx.LAST_BRANCH_WAS_RETURN[this];
  auto& pend_sas = g_ctx.PENDING_SAS_ENTRY[this];
  uint64_t pr_call_ip  = g_ctx.LAST_RETURN_CALL_IP[this];

  uint64_t predicted_target = 0;
  uint8_t  always_taken     = false;
  branch_info pred_type     = branch_info::ALWAYS_TAKEN;
  bool was_hit              = false;

  if (was_ret && pend_sas.valid) {
    was_hit = true;
    pred_type = pend_sas.type;
    predicted_target = pend_sas.target;
  } else {
    auto entry = g_ctx.MBTB.at(this).check_hit(
        {prev_ip, 0, branch_info::ALWAYS_TAKEN, trans});
    if (entry.has_value()) {
      was_hit = true;
      pred_type = entry->type;
      predicted_target = entry->target;
    }
  }

  if (was_hit) {
    if (pred_type == branch_info::RETURN) {
      if (std::empty(g_ctx.RAS[this])) {
        predicted_target = 0;
        always_taken = true;
      } else {
        auto target = g_ctx.RAS[this].back();
        auto size = g_ctx.CALL_SIZE[this][target % std::size(g_ctx.CALL_SIZE[this])];
        predicted_target = target + size;
        always_taken = true;
      }
    } else if (pred_type == branch_info::INDIRECT) {
      uint64_t hash = 0;
      if (was_ret && pend_sas.valid) {
        hash = (pr_call_ip) ^ g_ctx.CONDITIONAL_HISTORY[this].to_ullong() ^ static_cast<uint64_t>(mbtb_transition::R);
      } else {
        hash = ((prev_ip) ^ g_ctx.CONDITIONAL_HISTORY[this].to_ullong()) ^ static_cast<uint64_t>(trans);
      }
      predicted_target = g_ctx.INDIRECT_BTB[this][hash % std::size(g_ctx.INDIRECT_BTB[this])];
      always_taken = true;
    } else if (pred_type == branch_info::CONDITIONAL) {
      always_taken = false;
    } else {
      always_taken = true;
    }
  }

  g_ctx.PRED_QUEUE[this].push_back({ip, predicted_target, was_hit});
  return {predicted_target, always_taken};
}

void O3_CPU::update_btb(uint64_t ip, uint64_t branch_target, uint8_t taken, uint8_t branch_type)
{
  auto& stats = g_ctx.STATS[this];

  // ---- Drain PRED_QUEUE ------------------------------------------------
  auto& q = g_ctx.PRED_QUEUE[this];
  bool found = false;
  pending_mbtb_pred_t pred{};
  while (!q.empty()) {
    pred = q.front();
    q.pop_front();
    if (pred.ip == ip) { found = true; break; }
  }

  // Compute the transition this branch produces.
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

  bool prev_was_return = g_ctx.LAST_BRANCH_WAS_RETURN[this];
  uint64_t pr_call_ip  = g_ctx.LAST_RETURN_CALL_IP[this];

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

  // ---- CALL: RAS + SAS push (UNCHANGED) --------------------------------
  if (branch_type == BRANCH_DIRECT_CALL || branch_type == BRANCH_INDIRECT_CALL) {
    g_ctx.RAS[this].push_back(ip);

    sas_record_t snap{};
    auto entry = g_ctx.MBTB.at(this).check_hit({ip, 0, branch_info::ALWAYS_TAKEN, mbtb_transition::R});
    if (entry.has_value()) {
      snap.target = entry->target;
      snap.type   = entry->type;
      snap.valid  = true;
    } else {
      snap.valid = false;
    }

    g_ctx.SAS[this].push_back(snap);

    if (std::size(g_ctx.RAS[this]) > RAS_SIZE) g_ctx.RAS[this].pop_front();
    if (std::size(g_ctx.SAS[this]) > SAS_SIZE) g_ctx.SAS[this].pop_front();
  }

  // ---- RETURN: pop RAS + SAS (UNCHANGED) -------------------------------
  bool         just_handled_return = false;
  uint64_t     popped_call_ip      = 0;
  sas_record_t popped_sas{};

  if (branch_type == BRANCH_RETURN && !std::empty(g_ctx.RAS[this])) {
    popped_call_ip = g_ctx.RAS[this].back();
    g_ctx.RAS[this].pop_back();

    if (!std::empty(g_ctx.SAS[this])) {
      popped_sas = g_ctx.SAS[this].back();
      g_ctx.SAS[this].pop_back();
    } else {
      popped_sas.valid = false;
    }

    auto estimated_size = std::abs((long)(popped_call_ip - branch_target));
    if (estimated_size <= 10) {
      g_ctx.CALL_SIZE[this][popped_call_ip % std::size(g_ctx.CALL_SIZE[this])] = estimated_size;
    }

    just_handled_return = true;
  }

  // ---- Indirect target / history (UNCHANGED) ---------------------------
  auto prev_ip    = g_ctx.LAST_BRANCH_IP[this];
  auto prev_trans = g_ctx.LAST_TRANSITION[this] == mbtb_transition::R ? mbtb_transition::T : g_ctx.LAST_TRANSITION[this];
  auto& pend_sas = g_ctx.PENDING_SAS_ENTRY[this];

  if (branch_type == BRANCH_INDIRECT || branch_type == BRANCH_INDIRECT_CALL) {
    uint64_t hash = 0;
    if (prev_was_return && pend_sas.valid) {
      hash = (pr_call_ip) ^ g_ctx.CONDITIONAL_HISTORY[this].to_ullong() ^ static_cast<uint64_t>(mbtb_transition::R);
    } else {
      hash = ((prev_ip) ^ g_ctx.CONDITIONAL_HISTORY[this].to_ullong()) ^ static_cast<uint64_t>(prev_trans);
    }
    g_ctx.INDIRECT_BTB[this][hash % std::size(g_ctx.INDIRECT_BTB[this])]
        = branch_target;
  }

  if (branch_type == BRANCH_CONDITIONAL || branch_type == BRANCH_OTHER) {
    g_ctx.CONDITIONAL_HISTORY[this] <<= 1;
    g_ctx.CONDITIONAL_HISTORY[this].set(0, taken);
  }

  // ---- Resolve type ----------------------------------------------------
  auto type = branch_info::ALWAYS_TAKEN;
  if      (branch_type == BRANCH_INDIRECT || branch_type == BRANCH_INDIRECT_CALL) type = branch_info::INDIRECT;
  else if (branch_type == BRANCH_RETURN)                                          type = branch_info::RETURN;
  else if (branch_type == BRANCH_CONDITIONAL || branch_type == BRANCH_OTHER)      type = branch_info::CONDITIONAL;

  // ---- MBTB write (UNCHANGED) ------------------------------------------
  if (prev_was_return) {
    if (branch_target != 0) {
      auto opt_entry = g_ctx.MBTB.at(this).check_hit({pr_call_ip, branch_target, type, mbtb_transition::R});
      if (opt_entry.has_value()) {
        opt_entry->target = branch_target;
        opt_entry->type = type;
      }
      g_ctx.MBTB.at(this).fill(opt_entry.value_or(mbtb_entry_t{pr_call_ip, branch_target, type, mbtb_transition::R}));
    }
  } else {
    prev_trans = g_ctx.LAST_TRANSITION[this] == mbtb_transition::R
                     ? mbtb_transition::T : g_ctx.LAST_TRANSITION[this];

    auto opt_entry = g_ctx.MBTB.at(this).check_hit({prev_ip, branch_target, type, prev_trans});
    if (opt_entry.has_value()) {
      if (branch_target != 0) opt_entry->target = branch_target;
      opt_entry->type = type;
    }
    if (branch_target != 0) {
      g_ctx.MBTB.at(this).fill(
          opt_entry.value_or(mbtb_entry_t{prev_ip, branch_target, type, prev_trans}));
    }
  }

  // ---- Roll forward state ----------------------------------------------
  g_ctx.LAST_BRANCH_IP[this]  = ip;
  g_ctx.LAST_TRANSITION[this] = actual_trans;

  if (just_handled_return) {
    g_ctx.LAST_BRANCH_WAS_RETURN[this] = true;
    g_ctx.LAST_RETURN_CALL_IP[this]    = popped_call_ip;
    g_ctx.PENDING_SAS_ENTRY[this]      = popped_sas;
  } else {
    g_ctx.LAST_BRANCH_WAS_RETURN[this]  = false;
    g_ctx.PENDING_SAS_ENTRY[this].valid = false;
  }

  g_ctx.LAST_BRANCH_TYPE_FOR_STATS[this] = branch_type;
  g_ctx.HAS_LAST_BRANCH[this] = true;
}