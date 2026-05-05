/* Copyright 2020 HPS/SAFARI Research Groups ... */

#ifndef SPEC_TAGE_SC_L_TAGESCL_HPP_
#define SPEC_TAGE_SC_L_TAGESCL_HPP_

#include "tage.hpp"
#include "tagescl_configs.hpp"
#include "utils.hpp"

namespace tagescl {

template <class CONFIG>
struct Tage_SC_L_Prediction_Info {
  Tage_Prediction_Info<typename CONFIG::TAGE> tage;
  uint64_t br_pc;
  int rng_seed;
  bool final_prediction;
  bool updated_history;
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
        prediction_info_buffer_(max_in_flight_branches) {}

  uint32_t get_new_branch_id() override {
    uint32_t branch_id = prediction_info_buffer_.allocate_back();
    auto& prediction_info = prediction_info_buffer_[branch_id];
    Tage<typename CONFIG::TAGE>::build_empty_prediction(&prediction_info.tage);
    prediction_info.updated_history = false;
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
    tage_.get_stats().print("BASELINE TAGE");
  }

 private:
  Random_Number_Generator random_number_gen_;
  Tage<typename CONFIG::TAGE> tage_;
  Circular_Buffer<Tage_SC_L_Prediction_Info<CONFIG>> prediction_info_buffer_;
};

template <class CONFIG>
bool Tage_SC_L<CONFIG>::get_prediction(uint32_t branch_id, uint64_t br_pc) {
  auto& prediction_info = prediction_info_buffer_[branch_id];
  tage_.get_prediction(br_pc, &prediction_info.tage);
  prediction_info.final_prediction = prediction_info.tage.prediction;
  return prediction_info.final_prediction;
}

template <class CONFIG>
void Tage_SC_L<CONFIG>::commit_state(uint32_t branch_id, uint64_t br_pc,
                                     Branch_Type br_type, bool resolve_dir) {
  if (!br_type.is_conditional) {
    return;
  }
  auto& prediction_info = prediction_info_buffer_[branch_id];
  tage_.commit_state(br_pc, resolve_dir, prediction_info.tage,
                     prediction_info.final_prediction, true);
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
  }
  prediction_info_buffer_.deallocate_front(branch_id);
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
  prediction_info.br_pc = br_pc;
}

}  // namespace tagescl

#endif  // SPEC_TAGE_SC_L_TAGESCL_HPP_