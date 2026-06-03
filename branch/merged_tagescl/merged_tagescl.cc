#include <cassert>
#include <cstdint>
#include <iostream>
#include <fstream>
#include <sstream>
#include "ooo_cpu.h"
#include "tagescl.hpp"
#include "single_cycle_predictor.hpp"

struct ChampsimTageScl {
  using Impl = tagescl::Tage_SC_L<tagescl::CONFIG_64KB>;
  enum State { NONE, PREDICTED };

  ChampsimTageScl(std::size_t max_inflight_branches)
      : impl(max_inflight_branches),
        sc_pred(1024, 4),   // 1K entries, 4-way set associative
        last_ip(0),
        id(0),
        state(NONE) {}

  Impl impl;
  SingleCyclePredictor sc_pred;

  std::uint64_t last_ip;
  std::uint32_t id;
  State state;
};

static std::vector<const O3_CPU*> cpus;
static std::vector<<ChampsimTageScl> predictors;

static ChampsimTageScl& get_predictor(const O3_CPU* cpu) {
  for (std::size_t i = 0; i < cpus.size(); ++i) {
    if (cpus[i] == cpu) return predictors[i];
  }
  assert(false);
}


class BaselineTagePrinter {
 public:
  ~BaselineTagePrinter() {
    for (auto& p : predictors) {
      p.impl.print_stats();
    }
    std::cerr << std::flush;
  }
};
static BaselineTagePrinter baseline_tage_printer;

void O3_CPU::initialize_branch_predictor() {
  cpus.push_back(this);
  predictors.emplace_back(1);
}

/* ------------------------------------------------------------------
 * Legacy entry point – kept so the old module bridge still compiles.
 * Returns the SINGLE-CYCLE prediction only.
 * ------------------------------------------------------------------ */
std::uint8_t O3_CPU::predict_branch(std::uint64_t ip) {
  return predict_branch_pair(ip).single_cycle;
}

/* ------------------------------------------------------------------
 * New pair entry point – called by ooo_cpu.cc when TWO_BLOCK_AHEAD
 * or the dual-prediction penalty path is enabled.
 * ------------------------------------------------------------------ */
branch_prediction_pair O3_CPU::predict_branch_pair(std::uint64_t ip) {
  ChampsimTageScl& predictor = get_predictor(this);

  // If the previous "branch" turned out to be a non-branch, retire it now.
  if (predictor.state == ChampsimTageScl::PREDICTED) {
    tagescl::Branch_Type type;
    type.is_conditional = false;
    type.is_indirect    = false;
    predictor.impl.commit_state_at_retire(predictor.id, predictor.last_ip,
                                          type, 0, 0);
  }

  predictor.id = predictor.impl.get_new_branch_id();

  // ---- Single-cycle prediction (fast bimodal) ----
  bool sc = predictor.sc_pred.predict(ip);

  // ---- Multi-cycle prediction (TAGE) ----
  /* BASELINE TAGE: remove the third argument (this->current_cycle) */
  bool mc = predictor.impl.get_prediction(predictor.id, ip);

  predictor.last_ip = ip;
  predictor.state    = ChampsimTageScl::PREDICTED;

  return {static_cast<std::uint8_t>(sc), static_cast<std::uint8_t>(mc)};
}

void O3_CPU::last_branch_result(std::uint64_t ip, std::uint64_t target,
                                std::uint8_t taken, std::uint8_t branch_type) {
  ChampsimTageScl& predictor = get_predictor(this);
  assert(predictor.state == ChampsimTageScl::PREDICTED);
  assert(predictor.last_ip == ip);

  // ---- Update single-cycle predictor ----
  predictor.sc_pred.update(ip, taken);

  // ---- Update multi-cycle TAGE ----
  tagescl::Branch_Type type;
  type.is_conditional =
      branch_type == BRANCH_CONDITIONAL or branch_type == BRANCH_OTHER;
  type.is_indirect =
      branch_type == BRANCH_INDIRECT or branch_type == BRANCH_INDIRECT_CALL or
      branch_type == BRANCH_RETURN or branch_type == BRANCH_OTHER;

  predictor.impl.update_speculative_state(predictor.id, ip, type, taken, target);

  /* BASELINE TAGE: use the 4-argument version instead:
   *   predictor.impl.commit_state(predictor.id, ip, type, taken);
   * Ahead-pipelined TAGE needs the resolved target as well: */
  predictor.impl.commit_state(predictor.id, ip, type, taken);

  predictor.impl.commit_state_at_retire(predictor.id, ip, type, taken, target);
  predictor.state = ChampsimTageScl::NONE;
}