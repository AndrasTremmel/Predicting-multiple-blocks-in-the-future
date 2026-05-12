/* Copyright 2020 HPS/SAFARI Research Groups ... */

#ifndef SPEC_TAGE_SC_L_TAGESCL_HPP_
#define SPEC_TAGE_SC_L_TAGESCL_HPP_

#include "tage.hpp"
#include "tagescl_configs.hpp"
#include "utils.hpp"

#define MULTI_BLOCK_AHEAD_DISTANCE 1

namespace tagescl {

template <class CONFIG>
struct Tage_SC_L_Prediction_Info {
  Tage_Prediction_Info<typename CONFIG::TAGE> tage;
  uint64_t br_pc;
  int rng_seed;
  bool final_prediction;
  bool updated_history = false;
  bool tage_prediction_valid = false;
};

// ---- MULTI-BLOCK SPECIFIC STATISTICS ----
struct MultiBlockStats {
  uint64_t ahead_predictions_generated = 0;
  uint64_t ahead_predictions_valid = 0;
  uint64_t ahead_predictions_invalid = 0;
  uint64_t ahead_predictions_correct = 0;
  uint64_t ahead_predictions_wrong = 0;
  uint64_t overwritten_inflight_prediction = 0;
  uint64_t buffer_id_reuse = 0;
  uint64_t total_get_prediction_calls = 0;
  uint64_t total_new_branch_ids = 0;
  uint64_t pc_mismatch_predict_vs_commit = 0;
  uint64_t pc_match_predict_vs_commit = 0;
  uint64_t conditional_commits = 0;
  uint64_t unconditional_commits = 0;

  void print() const {
    std::cerr << "\n--- Multi-Block TAGE Diagnostics ---\n";
    std::cerr << "Ahead predictions generated:     " << ahead_predictions_generated << "\n";
    std::cerr << "Ahead predictions valid:         " << ahead_predictions_valid << "\n";
    std::cerr << "Ahead predictions invalid:       " << ahead_predictions_invalid << "\n";
    std::cerr << "Ahead predictions correct:       " << ahead_predictions_correct << "\n";
    std::cerr << "Ahead predictions wrong:         " << ahead_predictions_wrong << "\n";
    std::cerr << "Overwritten in-flight pred:      " << overwritten_inflight_prediction << "\n";
    std::cerr << "Buffer ID reuse detected:        " << buffer_id_reuse << "\n";
    std::cerr << "Total get_prediction calls:      " << total_get_prediction_calls << "\n";
    std::cerr << "Total new_branch_id calls:       " << total_new_branch_ids << "\n";
    std::cerr << "PC match (gen vs commit):        " << pc_match_predict_vs_commit << "\n";
    std::cerr << "PC mismatch (gen vs commit):     " << pc_mismatch_predict_vs_commit << "\n";
    std::cerr << "Conditional commits:             " << conditional_commits << "\n";
    std::cerr << "Unconditional commits:           " << unconditional_commits << "\n";
    std::cerr << "------------------------------------\n";
    std::cerr << std::flush;
  }
};

class Tage_SC_L_Base {
 public:
  virtual uint32_t get_new_branch_id() = 0;
  virtual bool get_prediction(uint32_t branch_id, uint64_t br_pc) = 0;
  virtual void update_speculative_state(uint32_t branch_id, uint64_t br_pc,
                                        Branch_Type br_type, bool branch_dir,
                                        uint64_t br_target) = 0;
  virtual void commit_state(uint32_t branch_id, uint64_t br_pc,
                            Branch_Type br_type, bool resolve_dir) = 0;
  virtual void commit_state_at_retire(uint32_t branch_id, uint64_t br_pc,
                                      Branch_Type br_type, bool resolve_dir,
                                      uint64_t br_target) = 0;
  virtual const TageStats& get_stats() const = 0;
  virtual void print_stats() const = 0;
};

template <class CONFIG>
class Tage_SC_L : public Tage_SC_L_Base {
 public:
  Tage_SC_L(int max_in_flight_branches)
      : tage_(random_number_gen_, max_in_flight_branches),
        prediction_info_buffer_(max_in_flight_branches, MULTI_BLOCK_AHEAD_DISTANCE) {}

  uint32_t get_new_branch_id() override {
    mb_stats_.total_new_branch_ids++;
    uint32_t branch_id = prediction_info_buffer_.get_read_id();
    auto& info = prediction_info_buffer_[branch_id];
    if (info.updated_history || info.tage_prediction_valid) {
      mb_stats_.buffer_id_reuse++;
    }
    return branch_id;
  }

  bool get_prediction(uint32_t branch_id, uint64_t br_pc) override;
  void update_speculative_state(uint32_t branch_id, uint64_t br_pc,
                                Branch_Type br_type, bool branch_dir,
                                uint64_t br_target) override;
  void commit_state(uint32_t branch_id, uint64_t br_pc,
                    Branch_Type br_type, bool resolve_dir) override;
  void commit_state_at_retire(uint32_t branch_id, uint64_t br_pc,
                              Branch_Type br_type, bool resolve_dir,
                              uint64_t br_target) override;

  const TageStats& get_stats() const override { return tage_.get_stats(); }
  void print_stats() const override {
    tage_.get_stats().print("MULTI-BLOCK TAGE");
    mb_stats_.print();
  }

 private:
  Random_Number_Generator random_number_gen_;
  Tage<typename CONFIG::TAGE> tage_;
  CircularBuffer<Tage_SC_L_Prediction_Info<CONFIG>> prediction_info_buffer_;
  mutable MultiBlockStats mb_stats_;
};

template <class CONFIG>
bool Tage_SC_L<CONFIG>::get_prediction(uint32_t branch_id, uint64_t br_pc) {
  mb_stats_.total_get_prediction_calls++;

  auto& future_prediction_info = prediction_info_buffer_[branch_id + MULTI_BLOCK_AHEAD_DISTANCE];

  if (future_prediction_info.tage_prediction_valid && future_prediction_info.updated_history) {
    mb_stats_.overwritten_inflight_prediction++;
  }

  tage_.get_prediction(br_pc, &future_prediction_info.tage);
  mb_stats_.ahead_predictions_generated++;
  future_prediction_info.tage_prediction_valid = true;
  future_prediction_info.tage.br_pc_used_for_pred_gen = br_pc;
  future_prediction_info.final_prediction = future_prediction_info.tage.prediction;

  auto& prediction_info = prediction_info_buffer_[branch_id];
  prediction_info.br_pc = br_pc;

  if (prediction_info.tage_prediction_valid) {
    mb_stats_.ahead_predictions_valid++;
    return prediction_info.final_prediction;
  }

  mb_stats_.ahead_predictions_invalid++;
  return false;
}

template <class CONFIG>
void Tage_SC_L<CONFIG>::commit_state(uint32_t branch_id, uint64_t br_pc,
                                     Branch_Type br_type, bool resolve_dir) {
  if (!br_type.is_conditional) {
    mb_stats_.unconditional_commits++;
    return;
  } else {
    mb_stats_.conditional_commits++;
  }

  auto& prediction_info = prediction_info_buffer_[branch_id];
  if(prediction_info.tage_prediction_valid) {
    bool correct = (prediction_info.final_prediction == resolve_dir);
    if (correct) mb_stats_.ahead_predictions_correct++;
    else mb_stats_.ahead_predictions_wrong++;

    tage_.commit_state(prediction_info.tage.br_pc_used_for_pred_gen, resolve_dir,
                       prediction_info.tage, prediction_info.final_prediction,
                       true);
  }
}

template <class CONFIG>
void Tage_SC_L<CONFIG>::commit_state_at_retire(uint32_t branch_id,
                                               uint64_t br_pc,
                                               Branch_Type br_type,
                                               bool resolve_dir,
                                               uint64_t br_target) {
  auto& prediction_info = prediction_info_buffer_[branch_id];
  if (prediction_info.updated_history) {
    tage_.commit_state_at_retire(prediction_info.tage);
    prediction_info_buffer_.deallocate_front(branch_id);

  }
  
}

template <class CONFIG>
void Tage_SC_L<CONFIG>::update_speculative_state(uint32_t branch_id,
                                                 uint64_t br_pc,
                                                 Branch_Type br_type,
                                                 bool branch_dir,
                                                 uint64_t br_target) {
  auto& prediction_info = prediction_info_buffer_[branch_id];
  prediction_info.rng_seed = random_number_gen_.seed_;
  prediction_info.updated_history = true;
  tage_.update_speculative_state(br_pc, br_target, br_type, branch_dir,
                                 &prediction_info.tage);
}

}  // namespace tagescl

#endif  // SPEC_TAGE_SC_L_TAGESCL_HPP_
