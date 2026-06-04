#include <cassert>
#include <cstdint>
#include <vector>

#include <iostream>

#include "ooo_cpu.h"
#include "ahead_tage_wrapper.hpp"

struct ChampsimTageScl {
  using Impl = tagescl::Tage_SC_L<tagescl::CONFIG_64KB>;
  enum State {
    NONE,
    PREDICTED,
  };

  ChampsimTageScl(std::size_t max_inflight_branches)
      : impl(max_inflight_branches), id(0), state(NONE) {}

  Impl impl;
  std::uint64_t last_ip;
  std::uint32_t id;
  State state;
};

static std::vector<const O3_CPU*> cpus;
static std::vector<ChampsimTageScl> predictors;

static ChampsimTageScl& get_predictor(const O3_CPU* cpu) {
  for (std::size_t i = 0; i < cpus.size(); ++i) {
    if (cpus[i] == cpu) return predictors[i];
  }
  assert(false);
}

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


branch_prediction_pair O3_CPU::predict_branch_pair(std::uint64_t ip) {
  ChampsimTageScl& predictor = get_predictor(this);
  if (predictor.state == ChampsimTageScl::PREDICTED) {
    // If we get here is because last_branch_result was not called.
    // Hence, the last ip was not branch and we should retire it
    // without doing any operation such as updating the history.
    // predictor.impl.retire_non_branch_ip(predictor.id);
    tagescl::Branch_Type type;
    type.is_conditional = false;
    type.is_indirect = false;
    predictor.impl.commit_state_at_retire(predictor.id, predictor.last_ip, type,
                                          0, 0);
  }
  predictor.id = predictor.impl.get_new_branch_id();
  bool prediction = predictor.impl.get_prediction(predictor.id, ip, current_cycle);
  predictor.last_ip = ip;
  predictor.state = ChampsimTageScl::PREDICTED;
  return {static_cast<std::uint8_t>(prediction), static_cast<std::uint8_t>(prediction)};
}

void O3_CPU::last_branch_result(std::uint64_t ip, std::uint64_t target,
                                std::uint8_t taken, std::uint8_t branch_type) {
  ChampsimTageScl& predictor = get_predictor(this);
  assert(predictor.state == ChampsimTageScl::PREDICTED);
  assert(predictor.last_ip == ip);
  tagescl::Branch_Type type;
  type.is_conditional =
      branch_type == BRANCH_CONDITIONAL or branch_type == BRANCH_OTHER;
  type.is_indirect =
      branch_type == BRANCH_INDIRECT or branch_type == BRANCH_INDIRECT_CALL or
      branch_type == BRANCH_RETURN or branch_type == BRANCH_OTHER;
  predictor.impl.update_speculative_state(predictor.id, ip, type, taken,
                                          target);
  // if (type.is_conditional) {
  //   predictor.impl.commit_state(predictor.id, ip, type, taken, target);
  // }
  predictor.impl.commit_state(predictor.id, ip, type, taken, target);

  predictor.impl.commit_state_at_retire(predictor.id, ip, type, taken, target);
  predictor.state = ChampsimTageScl::NONE;
}