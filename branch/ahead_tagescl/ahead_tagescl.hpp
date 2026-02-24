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
#include "2tag_tage.hpp"
#include "ahead_tagescl_configs.hpp"
#include "utils.hpp"
#include "btb.hpp"
#include <deque>


#define cycle_count 0

struct delay_queue_entry{
  //std::vector<bool> future_tage_preds;
  //std::vector<bool> tage_pred_confs;
  //std::vector<bool> tage_pred_used;
  bool future_tage_preds[SND_TAG_NO_PRED];
  bool tage_pred_confs[SND_TAG_NO_PRED];
  bool tage_pred_used[SND_TAG_NO_PRED];
  int tage_hit_bank[SND_TAG_NO_PRED];
  int tage_hit_index[SND_TAG_NO_PRED];
  int tage_alt_bank[SND_TAG_NO_PRED];
  int tage_alt_index[SND_TAG_NO_PRED];
  int tage_use_alt[SND_TAG_NO_PRED];

  uint32_t branch_id;
  uint64_t insert_cycle;
  uint64_t br_pc;
  uint64_t br_npc; //(luke) next pc
  bool current_pred;
  bool is_ret; 
};

std::deque<delay_queue_entry> future_tage_response_delay_queue;



uns get_recent_hist_hash()
{
    if (SND_TAG_NO_PRED == 1)
        return 0;

    constexpr uns TAG_BITS = __builtin_ctz(SND_TAG_NO_PRED);
    constexpr uns TAG_MASK = SND_TAG_NO_PRED - 1;

    uns res = 0;
    uns depth = 0;

    for (int i = (int)future_tage_response_delay_queue.size() - 1;
         i >= 0 && depth < AHEAD_DISTANCE;
         --i, ++depth)
    {
        const auto& entry = future_tage_response_delay_queue[i];

        uint64_t current = entry.br_npc;

        // 1) XOR direction
        if (!FFP_HASH_DIR_ONLY)
            res ^= entry.current_pred;

        // 2) XOR selected PC bits
        if (!FFP_HASH_DIR_ONLY && FFP_HASH_PC_BITS > 0) {
            for (uns b = 0; b < FFP_HASH_PC_BITS; b++) {
                res ^= (current >> (b + depth)) & 1;
            }
        }
        else if (!FFP_HASH_DIR_ONLY && FFP_HASH_PC_BITS == 0) {
            // Default folding: XOR folded PC chunk
            res ^= (current ^ (current >> TAG_BITS));
        }

        // 3) Rotate-left within TAG_BITS
        res = ((res << 1) | (res >> (TAG_BITS - 1))) & TAG_MASK;
    }

    return res & TAG_MASK;
}


namespace tagescl {

template <class CONFIG>
struct Tage_SC_L_Prediction_Info {
  Tage_Prediction_Info_2tag<typename CONFIG::TAGE> tage;
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
  virtual void retire_non_branch_ip(uint32_t branch_id) = 0;
  virtual void flush_branch(uint32_t branch_id) = 0;
  virtual void flush_branch_and_repair_state(uint32_t branch_id, uint64_t br_pc,
                                             Branch_Type br_type,
                                             bool resolve_dir,
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
        statistical_corrector_(),
        loop_predictor_(random_number_gen_),
        // TODO: abstract the btb characteristics into the config file
        btb_(256, 2);
        loop_predictor_beneficial_(-1),
        prediction_info_buffer_(max_in_flight_branches) {}

  // Gets a new branch_id for a new in-flight branch. The id remains valid
  // until
  // the branch is retired or flushed. The class internally maintains metadata
  // for each in-flight branch. The rest of the public functions in this class
  // need the id of a branch to work on.
  uint32_t get_new_branch_id() override {
    uint32_t branch_id = prediction_info_buffer_.allocate_back();
    auto& prediction_info = prediction_info_buffer_[branch_id];
    Tage<typename CONFIG::TAGE>::build_empty_prediction(&prediction_info.tage);
    Loop_Predictor<typename CONFIG::LOOP>::build_empty_prediction(
        &prediction_info.loop);
    prediction_info.updated_history = false;
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

  // Removes a non-branch instruction from the system. Invalidates branch_id.
  // Should be called directly after get_new_branch_id().
  void retire_non_branch_ip(uint32_t branch_id) override;

  // Flushes the branch and all branches that came after it
  // and repairs the speculative state of the predictor.
  // It invalidates the branch id of all branches after the flushed branch
  // (including the flushed branch).
  void flush_branch(uint32_t branch_id) override;

  // Flushes the branch and all branches that came after it
  // and repairs the speculative state of the predictor.
  // It invalidates the branch ids of all branches
  // strictly after the flushed branch.
  void flush_branch_and_repair_state(uint32_t branch_id, uint64_t br_pc,
                                     Branch_Type br_type, bool resolve_dir,
                                     uint64_t br_target) override;

 private:
  Random_Number_Generator random_number_gen_;
  Tage_2tag<typename CONFIG::TAGE> tage_;
  Statistical_Corrector<CONFIG> statistical_corrector_;
  Loop_Predictor<typename CONFIG::LOOP> loop_predictor_;
  L0BTB btb_;

  // Counter for choosing between Tage and Loop Predictor.
  Saturating_Counter<CONFIG::CONFIDENCE_COUNTER_WIDTH, true>
      loop_predictor_beneficial_;

  // Used for remembering necessary information gathered during prediction
  // that
  // are needed for update.
  Circular_Buffer<Tage_SC_L_Prediction_Info<CONFIG>> prediction_info_buffer_;
};

template <class CONFIG>
bool Tage_SC_L<CONFIG>::get_prediction(uint32_t branch_id, uint64_t br_pc) {
  auto& prediction_info = prediction_info_buffer_[branch_id];

  uns fft_picker = get_recent_hist_hash();

  // Create new prediction queue entry and add it to the queue
  delay_queue_entry temp_entry;

  for(uns i = 0; i < SND_TAG_NO_PRED; i++){
    temp_entry.future_tage_preds[i] =   tage_.get_prediction(br_pc, 
      &prediction_info.tage, i);
    temp_entry.tage_pred_confs[i] = prediction_info.tage.high_confidence[i];
    temp_entry.tage_pred_used[i] = false;
    int hit_bank = prediction_info.tage.hit_bank[i];
    temp_entry.tage_hit_bank[i] = hit_bank;
    temp_entry.tage_hit_index[i] = prediction_info.tage.indices[hit_bank];
    int alt_bank = prediction_info.tage.alt_bank[i];
    temp_entry.tage_alt_bank[i] = alt_bank;
    temp_entry.tage_alt_index[i] = prediction_info.tage.indices[alt_bank];
    temp_entry.tage_use_alt[i] =  prediction_info.tage.use_alt[i];
  }            
              
  temp_entry.br_pc = br_pc;
  //temp_entry.br_npc = op->oracle_info.npc;
  temp_entry.insert_cycle = cycle_count++;
  temp_entry.branch_id = branch_id;
  //temp_entry.is_ret= op->table_info->cf_type == CF_RET;
  future_tage_response_delay_queue.push_back(temp_entry);


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
                     prediction_info.final_prediction);
}

template <class CONFIG>
void Tage_SC_L<CONFIG>::flush_branch_and_repair_state(uint32_t branch_id,
                                                      uint64_t br_pc,
                                                      Branch_Type br_type,
                                                      bool resolve_dir,
                                                      uint64_t br_target) {
  // First iterate over all flushed branches from youngest to oldest and call
  // local recovery functions.
  for (uint32_t id = prediction_info_buffer_.back_id();
       id - branch_id < (uint32_t{1} << 31); --id) {
    auto& prediction_info = prediction_info_buffer_[id];
    tage_.local_recover_speculative_state(prediction_info.tage);
    if (CONFIG::USE_LOOP_PREDICTOR) {
      loop_predictor_.local_recover_speculative_state(prediction_info.loop);
    }
    if (CONFIG::USE_SC) {
      statistical_corrector_.local_recover_speculative_state(
          prediction_info.br_pc, prediction_info.sc);
    }
  }
  prediction_info_buffer_.deallocate_after(branch_id);

  // Now call global recovery functions.
  auto& prediction_info = prediction_info_buffer_[branch_id];
  tage_.global_recover_speculative_state(prediction_info.tage);
  if (CONFIG::USE_LOOP_PREDICTOR) {
    loop_predictor_.global_recover_speculative_state(prediction_info.loop);
  }
  if (CONFIG::USE_SC) {
    statistical_corrector_.global_recover_speculative_state(prediction_info.sc);
  }

  random_number_gen_.seed_ = prediction_info.rng_seed;

  // Finally, update the speculative histories again using the resolved
  // direction of the branch.
  tage_.update_speculative_state(br_pc, br_target, br_type, resolve_dir,
                                 &prediction_info.tage);
  if (CONFIG::USE_LOOP_PREDICTOR) {
    loop_predictor_.update_speculative_state(prediction_info.loop);
  }
  if (CONFIG::USE_SC) {
    statistical_corrector_.update_speculative_state(
        br_pc, resolve_dir, br_target, br_type, &prediction_info.sc);
  }
}

template <class CONFIG>
void Tage_SC_L<CONFIG>::flush_branch(uint32_t branch_id) {
  // First iterate over all flushed branches from youngest to oldest and
  // call local recovery functions.
  for (uint32_t id = prediction_info_buffer_.back_id();
       id - branch_id < (uint32_t{1} << 31); --id) {
    auto& prediction_info = prediction_info_buffer_[id];
    tage_.local_recover_speculative_state(prediction_info.tage);
    if (CONFIG::USE_LOOP_PREDICTOR) {
      loop_predictor_.local_recover_speculative_state(prediction_info.loop);
    }
    if (CONFIG::USE_SC) {
      statistical_corrector_.local_recover_speculative_state(
          prediction_info.br_pc, prediction_info.sc);
    }
  }

  auto& prediction_info = prediction_info_buffer_[branch_id];
  prediction_info_buffer_.deallocate_and_after(branch_id);

  // Now call global recovery functions.
  tage_.global_recover_speculative_state(prediction_info.tage);
  if (CONFIG::USE_LOOP_PREDICTOR) {
    loop_predictor_.global_recover_speculative_state(prediction_info.loop);
  }
  if (CONFIG::USE_SC) {
    statistical_corrector_.global_recover_speculative_state(prediction_info.sc);
  }

  random_number_gen_.seed_ = prediction_info.rng_seed;
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
void Tage_SC_L<CONFIG>::retire_non_branch_ip(uint32_t branch_id) {
  // std::cerr << "retire_non_branch_ip(" << branch_id << ")\n";
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