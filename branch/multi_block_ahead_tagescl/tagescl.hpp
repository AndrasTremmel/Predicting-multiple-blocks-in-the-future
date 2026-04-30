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

  // store if tage prediction is valid
  bool tage_prediction_valid = false;
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
        prediction_info_buffer_(max_in_flight_branches, MULTI_BLOCK_AHEAD_DISTANCE) {}

  uint32_t get_new_branch_id() override {
    uint32_t branch_id = prediction_info_buffer_.get_read_id();
    return branch_id;
  }

  // It uses the speculative state of the predictor to generate a prediction.
  // Should be called before update_speculative_state.
  bool get_prediction(uint32_t branch_id, uint64_t br_pc) override;

  // It updates the speculative state (e.g. to insert history bits in Tage's
  // global history register). For conditional branches, it should be called
  // after get_prediction() in the front-end of a pipeline. For unconditional
  // branches, it should be the only function called in the front-end.
  void update_speculative_state(uint32_t branch_id, uint64_t br_pc,
                                Branch_Type br_type, bool branch_dir,
                                uint64_t br_target) override;

  // Invokes the default update algorithm for updating the predictor state.
  // Can
  // be called either at the end of execute or retire. Note that even though
  // updating at the end of execute is speculative, committing the state
  // cannot
  // be undone.
  void commit_state(uint32_t branch_id, uint64_t br_pc, Branch_Type br_type,
                    bool resolve_dir) override;

  // Updates predictor states that are critical for algorithm correctness.
  // Thus, should always be called in the retire state and after
  // commit_state()
  // is called. branch_id is invalidated and should not be used anymore.
  void commit_state_at_retire(uint32_t branch_id, uint64_t br_pc,
                              Branch_Type br_type, bool resolve_dir,
                              uint64_t br_target) override;

 private:
  Random_Number_Generator random_number_gen_;
  Tage<typename CONFIG::TAGE> tage_;

  // Used for remembering necessary information gathered during prediction that are needed for update.
  CircularBuffer<Tage_SC_L_Prediction_Info<CONFIG>> prediction_info_buffer_;
};


template <class CONFIG>
bool Tage_SC_L<CONFIG>::get_prediction(uint32_t branch_id, uint64_t br_pc) {
  // ************************************************************
  // * Create new prediction and add it to the prediction buffer
  // ************************************************************
  auto& future_prediction_info = prediction_info_buffer_[branch_id + MULTI_BLOCK_AHEAD_DISTANCE];
  tage_.get_prediction(br_pc, &future_prediction_info.tage);
  future_prediction_info.tage_prediction_valid = true;
  future_prediction_info.tage.br_pc_used_for_pred_gen = br_pc;
  future_prediction_info.final_prediction = future_prediction_info.tage.prediction;



  // ***********************************************************
  // * Get the prediction info for the current branch
  // ***********************************************************
  //std::cout << "Starting prediction for branch id:: " << branch_id << " and branch pc: " << br_pc << std::endl;
  auto& prediction_info = prediction_info_buffer_[branch_id];

  // Update branch pc since it was unkown up until now
  prediction_info.br_pc = br_pc;


  if(prediction_info.tage_prediction_valid) {
      return prediction_info.final_prediction;
  }

  return false;
}


template <class CONFIG>
void Tage_SC_L<CONFIG>::commit_state(uint32_t branch_id, uint64_t br_pc,
                                     Branch_Type br_type, bool resolve_dir) {

  // // Only update TAGE for conditional branches                                  
  // if (!br_type.is_conditional) {
  //   return;
  // }
  auto& prediction_info = prediction_info_buffer_[branch_id];
  
  if(prediction_info.tage_prediction_valid) {
    tage_.commit_state(prediction_info.tage.br_pc_used_for_pred_gen, resolve_dir, prediction_info.tage, prediction_info.final_prediction);
  }
}


template <class CONFIG>
void Tage_SC_L<CONFIG>::commit_state_at_retire(uint32_t branch_id,
                                               uint64_t br_pc,
                                               Branch_Type br_type,
                                               bool resolve_dir,
                                               uint64_t br_target) {
  //std::cout << "Starting retire for branch id: " << branch_id << std::endl;                                                
  auto& prediction_info = prediction_info_buffer_[branch_id];
  // This can only hold for branch instructions as it 
  // requires speculative update to be already done 
  // on the instruction which is called from inside the 
  // O3_CPU::last_branch_result ChampSim API function
  // which is only called after O3_CPU::predict_branch if
  // the instruction is actually a branch instruction.
  if (prediction_info.updated_history) {
    //std::cout << "Updated history changed..." << std::endl;
    
    //std::cout << "Calling tage reire..." << std::endl;
    tage_.commit_state_at_retire(prediction_info.tage);
    
    //std::cout << "Deallocating front element of predicton info buffer..." << std::endl;
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