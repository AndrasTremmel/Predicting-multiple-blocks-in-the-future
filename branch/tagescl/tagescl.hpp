/* Copyright 2020 HPS/SAFARI Research Groups
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files (the "Software"), to deal
 * in the Software without restriction, including without limitation the rights
 * to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
 * copies of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in
 * all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 * AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
 * SOFTWARE.
 */

#ifndef SPEC_TAGE_SC_L_TAGESCL_HPP_
#define SPEC_TAGE_SC_L_TAGESCL_HPP_

#include "statistical_corrector.hpp"
#include "tage.hpp"
#include "tagescl_configs.hpp"
#include "utils.hpp"

namespace tagescl {

template <class CONFIG>
struct Tage_SC_L_Prediction_Info {
  Tage_Prediction_Info<typename CONFIG::TAGE> tage;
  Loop_Prediction_Info<typename CONFIG::LOOP> loop;
  SC_Prediction_Info sc;
  uint64_t br_pc;
  int rng_seed;
  bool tage_or_loop_prediction;
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

/* Interface functions:
 *
 * warmup() a wrapper for updating predictor state during the warmup phase of a
 * simulation.
 *
 * predict_and_update() a wrapper for consecutive simultaneous prediction and
 * update that implement the idealistic algorithms without considering pipeline
 * requirements. (same as Championship Branch Prediction Interface)
 */
template <class CONFIG>
class Tage_SC_L : public Tage_SC_L_Base {
 public:
  Tage_SC_L(int max_in_flight_branches)
      : tage_(random_number_gen_, max_in_flight_branches),
        statistical_corrector_(),
        loop_predictor_(random_number_gen_),
        loop_predictor_beneficial_(-1),
        prediction_info_buffer_(max_in_flight_branches) {}


  uint32_t get_new_branch_id() override {
    uint32_t branch_id = prediction_info_buffer_.allocate_back();
    auto& prediction_info = prediction_info_buffer_[branch_id];
    Tage<typename CONFIG::TAGE>::build_empty_prediction(&prediction_info.tage);
    Loop_Predictor<typename CONFIG::LOOP>::build_empty_prediction(
        &prediction_info.loop);
    prediction_info.updated_history = false;
    return branch_id;
  }

  
  bool get_prediction(uint32_t branch_id, uint64_t br_pc) override;
  void update_speculative_state(uint32_t branch_id, uint64_t br_pc,
                                Branch_Type br_type, bool branch_dir,
                                uint64_t br_target) override;
  void commit_state(uint32_t branch_id, uint64_t br_pc, Branch_Type br_type,
                    bool resolve_dir) override;
  void commit_state_at_retire(uint32_t branch_id, uint64_t br_pc,
                              Branch_Type br_type, bool resolve_dir,
                              uint64_t br_target) override;
  const TageStats& get_stats() const override { return tage_.get_stats(); }
  void print_stats() const override {
    tage_.get_stats().print("BASELINE TAGE-SC-L");
  }



 private:
  Random_Number_Generator random_number_gen_;
  Tage<typename CONFIG::TAGE> tage_;
  Statistical_Corrector<CONFIG> statistical_corrector_;
  Loop_Predictor<typename CONFIG::LOOP> loop_predictor_;

  // Counter for choosing between Tage and Loop Predictor.
  Saturating_Counter<CONFIG::CONFIDENCE_COUNTER_WIDTH, true>
      loop_predictor_beneficial_;

  Circular_Buffer<Tage_SC_L_Prediction_Info<CONFIG>> prediction_info_buffer_;
};

template <class CONFIG>
bool Tage_SC_L<CONFIG>::get_prediction(uint32_t branch_id, uint64_t br_pc) {
  auto& prediction_info = prediction_info_buffer_[branch_id];

  // First, use Tage to make a prediction.
  tage_.get_prediction(br_pc, &prediction_info.tage);
  prediction_info.tage_or_loop_prediction = prediction_info.tage.prediction;

  if (CONFIG::USE_LOOP_PREDICTOR) {
    // Then, look up the loop predictor and override Tage's prediction if
    // the loop predictor is found to be beneficial.
    loop_predictor_.get_prediction(br_pc, &prediction_info.loop);
    if (loop_predictor_beneficial_.get() >= 0 && prediction_info.loop.valid) {
      prediction_info.tage_or_loop_prediction = prediction_info.loop.prediction;
    }
  }

  if (!CONFIG::USE_SC) {
    prediction_info.final_prediction = prediction_info.tage_or_loop_prediction;
  } else {
    statistical_corrector_.get_prediction(
        br_pc, prediction_info.tage, prediction_info.tage_or_loop_prediction,
        &prediction_info.sc);
    prediction_info.final_prediction = prediction_info.sc.prediction;
  }
  return prediction_info.final_prediction;
}

template <class CONFIG>
void Tage_SC_L<CONFIG>::commit_state(uint32_t branch_id, uint64_t br_pc,
                                     Branch_Type br_type, bool resolve_dir) {
  if (!br_type.is_conditional) {
    return;
  }
  auto& prediction_info = prediction_info_buffer_[branch_id];
  if (CONFIG::USE_SC) {
    statistical_corrector_.commit_state(
        br_pc, resolve_dir, prediction_info.tage, prediction_info.sc,
        prediction_info.tage_or_loop_prediction);
  }

  if (CONFIG::USE_LOOP_PREDICTOR) {
    if (prediction_info.loop.valid) {
      if (prediction_info.final_prediction != prediction_info.loop.prediction) {
        loop_predictor_beneficial_.update(resolve_dir ==
                                          prediction_info.loop.prediction);
      }
    }
    loop_predictor_.commit_state(
        br_pc, resolve_dir, prediction_info.loop,
        prediction_info.final_prediction != resolve_dir,
        prediction_info.tage.prediction);
  }

  tage_.commit_state(br_pc, resolve_dir, prediction_info.tage,
                     prediction_info.tage.prediction, true);
}


template <class CONFIG>
void Tage_SC_L<CONFIG>::commit_state_at_retire(uint32_t branch_id,
                                               uint64_t br_pc,
                                               Branch_Type br_type,
                                               bool resolve_dir,
                                               uint64_t br_target) {
  auto& prediction_info = prediction_info_buffer_[branch_id];
  if (prediction_info.updated_history) {
    if (CONFIG::USE_LOOP_PREDICTOR) {
      loop_predictor_.commit_state_at_retire(
        br_pc, resolve_dir, prediction_info.loop,
        prediction_info.final_prediction != resolve_dir,
        prediction_info.tage.prediction);
    }
    tage_.commit_state_at_retire(prediction_info.tage);
    if (CONFIG::USE_SC) {
      statistical_corrector_.commit_state_at_retire();
    }
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
  if (CONFIG::USE_LOOP_PREDICTOR) {
    loop_predictor_.update_speculative_state(prediction_info.loop);
  }
  if (CONFIG::USE_SC) {
    statistical_corrector_.update_speculative_state(
        br_pc, branch_dir, br_target, br_type, &prediction_info.sc);
  }
  prediction_info.br_pc = br_pc;
}

}  // namespace tagescl

#endif  // SPEC_TAGE_SC_L_TAGESCL_HPP_