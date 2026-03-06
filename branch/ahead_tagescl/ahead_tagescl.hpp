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

//#include "statistical_corrector.hpp"
#include "2tag_tage.hpp"
#include "ahead_tagescl_configs.hpp"
#include "utils.hpp"
#include "btb.hpp"
#include <deque>

typedef unsigned int uns;


#define AHEAD_DISTANCE 5
#define USE_2_BIT_COUNTER_IN_L0 1
#define FFP_HASH_DIR 1
#define FFP_HASH_PC_BITS 8
#define FFP_USE_BM 1
#define FFP_USE_LATE_PRED 0
#define FUTURE_TAGE_LATENCY 3
#define FFP_BM_THRESH 2
#define FFP_KILL_BM_ONE_WRONG 0

#define L0_BTB_SIZE 1024
#define L0_BTB_ASSOC 4

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




uns get_recent_hist_hash(uint64_t br_pc) {
  uns res = 0;
  uns j = 0;
  uint64_t current;
  if(SND_TAG_NO_PRED == 1){
    return 0;
  }
  if(AHEAD_DISTANCE == 0){
    return ((br_pc>>1) ^ (br_pc>>4)) & 0x07;
  }
  for(int i = (int)future_tage_response_delay_queue.size() - 1; i >= 0; i--){
    if(j == AHEAD_DISTANCE){
      break;
    }
    j++;
    current = future_tage_response_delay_queue[i].br_npc;
    if(SND_TAG_NO_PRED == 1){
      res = 0;
    }
    else if(SND_TAG_NO_PRED == 2){
      if(FFP_HASH_DIR){
        res = res ^ future_tage_response_delay_queue[i].current_pred;
      }
      for(uns j = 0; j < FFP_HASH_PC_BITS; j++){ 
        res = res ^ (current & 0x01);
        current = current >> 1;
      }
    }
    else if (SND_TAG_NO_PRED == 4) {
      res = res ^ future_tage_response_delay_queue[i].current_pred;
      if(FFP_HASH_PC_BITS == 0){
        res = res ^ current ^ (current >> 2);
        res = res & 0x03;
      }
      else if(FFP_HASH_PC_BITS == 1){
        res = res ^ (current >> 1) ^ (current >> 3);
        res = res & 0x03;
      }
      else if(FFP_HASH_PC_BITS == 2){
        res = res ^ (current >> 2) ^ (current >> 4);
        res = res & 0x03;
      }
      else{
        ASSERT(0, FALSE);
      }
      if(res & 0x02){
        res = (res << 1) + 1;
      }
      else{
        res = res << 1;
      }
      res = res & 0x03;
    }
    else if(SND_TAG_NO_PRED == 8){
      res = res ^ future_tage_response_delay_queue[i].current_pred;
      if(FFP_HASH_DIR_ONLY){
        ASSERT(0, AHEAD_DISTANCE == 3);  
      }
      else if(FFP_HASH_PC_BITS == 0){
        res = res ^ current ^ (current >> 3);
      }
      else if(FFP_HASH_PC_BITS == 1){
        res = res ^ current;
      }
      else if(FFP_HASH_PC_BITS == 2){
        res = res ^ (current >> 1);
      }
      else if(FFP_HASH_PC_BITS == 3){
        res = res ^ (current >> 2);
      }
      else if(FFP_HASH_PC_BITS == 4){
        res = res ^ (current >> 3);
      }
      else if(FFP_HASH_PC_BITS == 5){
        res = res ^ (current >> 1) ^ (current >> 4);
      }
      else if(FFP_HASH_PC_BITS == 6){
        res = res ^ (current >> 2) ^ (current >> 5);
      }
      else{
        ASSERT(0, FALSE);
      }
      if(res & 0x04){
        res = (res << 1) + 1;
      }
      else{
        res = res << 1;
      }
      res = res & 0x07;
    }
    else if(SND_TAG_NO_PRED == 16){
      res = res ^ future_tage_response_delay_queue[i].current_pred;
      if(FFP_HASH_DIR_ONLY){
        ASSERT(0, AHEAD_DISTANCE == 4);  
      }
      else if(FFP_HASH_PC_BITS == 0){
        res = res ^ current ^ (current >> 4);
      }
      else if(FFP_HASH_PC_BITS == 1){
        res = res ^ (current >> 1) ^ (current >> 5);
      }
      else if(FFP_HASH_PC_BITS == 2){
        res = res ^ (current >> 2) ^ (current >> 6);
      }
      else{
        ASSERT(0, FALSE);
      }
      if(res & 0x08){
        res = (res << 1) + 1;
      }
      else{
        res = res << 1;
      }
      res = res & 0x0f;
    }
    else if(SND_TAG_NO_PRED == 32){
      res = res ^ future_tage_response_delay_queue[i].current_pred;
      if(FFP_HASH_DIR_ONLY){
        ASSERT(0, AHEAD_DISTANCE == 5);  
      }
      else if(FFP_HASH_PC_BITS == 0){
        res = res ^ current ^ (current >> 5);
      }
      else if(FFP_HASH_PC_BITS == 1){
        res = res ^ (current >> 1) ^ (current >> 6);
      }
      else if(FFP_HASH_PC_BITS == 2){
        res = res ^ (current >> 2) ^ (current >> 7);
      }
      else{
        ASSERT(0, FALSE);
      }
      if(res & 0x10){
        res = (res << 1) + 1;
      }
      else{
        res = res << 1;
      }
      res = res & 0x01f;
    }
    else if(SND_TAG_NO_PRED == 64){
      res = res ^ future_tage_response_delay_queue[i].current_pred;
      if(FFP_HASH_DIR_ONLY){
        ASSERT(0, AHEAD_DISTANCE == 6);  
      }
      else if(FFP_HASH_PC_BITS == 1){
        res = res ^ (current >> 1) ^ (current >> 7);
      }
      else{
        ASSERT(0, FALSE);
      }
      if(res & 0x20){
        res = (res << 1) + 1;
      }
      else{
        res = res << 1;
      }
      res = res & 0x03f;
    }
    else if(SND_TAG_NO_PRED == 128){
      res = res ^ future_tage_response_delay_queue[i].current_pred;
      if(FFP_HASH_DIR_ONLY){
        ASSERT(0, AHEAD_DISTANCE == 7);  
      }
      else if(FFP_HASH_PC_BITS == 1){
        res = res ^ (current >> 1) ^ (current >> 8);
      }
      else{
        ASSERT(0, FALSE);
      }
      if(res & 0x40){
        res = (res << 1) + 1;
      }
      else{
        res = res << 1;
      }
      res = res & 0x07f;
    }
    else if(SND_TAG_NO_PRED == 256){
      res = res ^ future_tage_response_delay_queue[i].current_pred;
      if(FFP_HASH_DIR_ONLY){
        ASSERT(0, AHEAD_DISTANCE == 8);  
      }
      else if(FFP_HASH_PC_BITS == 1){
        res = res ^ (current >> 1) ^ (current >> 9);
      }
      else{
        ASSERT(0, FALSE);
      }
      if(res & 0x80){
        res = (res << 1) + 1;
      }
      else{
        res = res << 1;
      }
      res = res & 0x0ff;
    }
    else if(SND_TAG_NO_PRED == 512){
      res = res ^ future_tage_response_delay_queue[i].current_pred;
      if(FFP_HASH_DIR_ONLY){
        ASSERT(0, AHEAD_DISTANCE == 9);  
      }
      else if(FFP_HASH_PC_BITS == 1){
        res = res ^ (current >> 1) ^ (current >> 10);
      }
      else{
        ASSERT(0, FALSE);
      }
      if(res & 0x100){
        res = (res << 1) + 1;
      }
      else{
        res = res << 1;
      }
      res = res & 0x1ff;
    }
    else if(SND_TAG_NO_PRED == 1024){
      res = res ^ future_tage_response_delay_queue[i].current_pred;
      if(FFP_HASH_DIR_ONLY){
        ASSERT(0, AHEAD_DISTANCE == 10);  
      }
      else if(FFP_HASH_PC_BITS == 1){
        res = res ^ (current >> 1) ^ (current >> 11);
      }
      else{
        ASSERT(0, FALSE);
      }
      if(res & 0x200){
        res = (res << 1) + 1;
      }
      else{
        res = res << 1;
      }
      res = res & 0x3ff;
    }
    else if(SND_TAG_NO_PRED == 2048){
      res = res ^ future_tage_response_delay_queue[i].current_pred;
      if(FFP_HASH_DIR_ONLY){
        ASSERT(0, AHEAD_DISTANCE == 11);  
      }
      else if(FFP_HASH_PC_BITS == 1){
        res = res ^ (current >> 1) ^ (current >> 12);
      }
      else{
        ASSERT(0, FALSE);
      }
      if(res & 0x400){
        res = (res << 1) + 1;
      }
      else{
        res = res << 1;
      }
      res = res & 0x7ff;
    }
    else if(SND_TAG_NO_PRED == 4096){
      res = res ^ future_tage_response_delay_queue[i].current_pred;
      if(FFP_HASH_DIR_ONLY){
        ASSERT(0, AHEAD_DISTANCE == 12);  
      }
      else if(FFP_HASH_PC_BITS == 1){
        res = res ^ (current >> 1) ^ (current >> 13);
      }
      else{
        ASSERT(0, FALSE);
      }
      if(res & 0x800){
        res = (res << 1) + 1;
      }
      else{
        res = res << 1;
      }
      res = res & 0xfff;
    }
    else{
      ASSERT(0, FALSE);
    }
  }
  return res;
}
  


namespace tagescl {

template <class CONFIG>
struct Tage_SC_L_Prediction_Info {
  Tage_Prediction_Info_2tag<typename CONFIG::TAGE> tage;
  //Loop_Prediction_Info<typename CONFIG::LOOP> loop;
  //SC_Prediction_Info sc;
  uint64_t br_pc;
  int rng_seed;
  //bool tage_or_loop_prediction;
  bool final_prediction;
  bool updated_history = false;
  // Store btb predictoion for later use in commit stage
  bool btb_prediction;
  bool tage_prediction_valid = false;
};

class Tage_SC_L_Base {
 public:
  virtual uint32_t get_new_branch_id() = 0;
  virtual bool get_prediction(uint32_t branch_id, uint64_t br_pc, uint64_t br_npc) = 0;
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
        //statistical_corrector_(),
        //loop_predictor_(random_number_gen_),
        // TODO: abstract the btb characteristics into the config file
        btb_(L0_BTB_SIZE, L0_BTB_ASSOC),

        //loop_predictor_beneficial_(-1),
        prediction_info_buffer_(max_in_flight_branches, AHEAD_DISTANCE) {}

  // Gets a new branch_id for a new in-flight branch. The id remains valid
  // until
  // the branch is retired or flushed. The class internally maintains metadata
  // for each in-flight branch. The rest of the public functions in this class
  // need the id of a branch to work on.

  // TODO: change the new branch id giving function
  uint32_t get_new_branch_id() override {
    uint32_t branch_id = prediction_info_buffer_.get_read_id_();
    //auto& prediction_info = prediction_info_buffer_[branch_id];
    //Tage<typename CONFIG::TAGE>::build_empty_prediction(&prediction_info.tage);
    //Loop_Predictor<typename CONFIG::LOOP>::build_empty_prediction(
    //    &prediction_info.loop);
    //prediction_info.updated_history = false;
    return branch_id;
  }

  // It uses the speculative state of the predictor to generate a prediction.
  // Should be called before update_speculative_state.
  bool get_prediction(uint32_t branch_id, uint64_t br_pc, uint64_t current_cycle) override;

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
  //Statistical_Corrector<CONFIG> statistical_corrector_;
  //Loop_Predictor<typename CONFIG::LOOP> loop_predictor_;
  L0BTB btb_;
  std::deque<delay_queue_entry> future_tage_response_delay_queue;

  // Counter for choosing between Tage and Loop Predictor.
  // Saturating_Counter<CONFIG::CONFIDENCE_COUNTER_WIDTH, true>
  //     loop_predictor_beneficial_;

  // Used for remembering necessary information gathered during prediction
  // that
  // are needed for update.
  CircularBuffer<Tage_SC_L_Prediction_Info<CONFIG>> prediction_info_buffer_;
};

template <class CONFIG>
bool Tage_SC_L<CONFIG>::get_prediction(uint32_t branch_id, uint64_t br_pc, uint64_t current_cycle) {
  // ***********************************************************
  // * Get the prediction info for the current branch
  // ***********************************************************
  auto& prediction_info = prediction_info_buffer_[branch_id];

  // Update branch pc since it was unkown up until now
  prediction_info.br_pc = br_pc;

  // ***********************************************************
  // * Read the single cycle latency btb as a default prediction 
  // ***********************************************************

  L0BTB::Result res = btb_.probe(br_pc);
  bool hit = res.hit;
  bool counter_taken = hit ? res.entry->counter > 1 : 0;
  bool btb_prediction = hit;
  if (USE_2_BIT_COUNTER_IN_L0) {
    btb_prediction = btb_prediction && counter_taken;
  }

  prediction_info.btb_prediction = btb_prediction;

  bool final_pred = btb_prediction;


  // ***************************************************************
  // * Compute missing history hash and select the final prediction
  // * from the right element of the prediction queue using the 
  // * computed hash if prediction info contains ahead tage prediction
  // ***************************************************************

  if(prediction_info.tage_prediction_valid) {
    uns fft_picker = get_recent_hist_hash(br_pc);
    bool found_it = FALSE;

    for(auto&element : future_tage_response_delay_queue) {
      if (element.branch_id == branch_id) {
        found_it = TRUE;
        bool tage_pred = element.future_tage_preds[fft_picker];
        prediction_info.tage.final_prediction = tage_pred;
        prediction_info.tage.tag2 = (int) fft_picker;

        if(element.tage_pred_used[fft_picker]){
          // We might add some statistics logging macro to account for the conflict (STAT_EVENT(0, FFP_CONFLICT);)
        } else{
          element.tage_pred_used[fft_picker] = true;
        }

        if(FFP_USE_BM && res.data->use_bm && hit && AHEAD_DISTANCE != 0) {
          final_pred = btb_prediction;
          break;
        }

        if((current_cycle - element.insert_cycle >= FUTURE_TAGE_LATENCY) || FFP_USE_LATE_PRED) {
          final_pred = tage_pred;
          break;
        }
        
        break;
      }
    }
  }

  prediction_info.final_prediction = final_pred;
  


  // // ***************************************************************
  // // * Use the loop predictor and the statistical corrector to 
  // // * adjust the tage prediction if needed
  // // ***************************************************************

  // old_prediction_info.tage_or_loop_prediction = final_pred;

  // if (CONFIG::USE_LOOP_PREDICTOR) {
  //   // Then, look up the loop predictor and override Tage's prediction if
  //   // the loop predictor is found to be beneficial.
  //   loop_predictor_.get_prediction(br_pc, &old_prediction_info.loop);
  //   if (loop_predictor_beneficial_.get() >= 0 && old_prediction_info.loop.valid) {
  //     old_prediction_info.tage_or_loop_prediction = old_prediction_info.loop.prediction;
  //   }
  // }

  // if (!CONFIG::USE_SC) {
  //   old_prediction_info.final_prediction = predictold_prediction_infoion_info.tage_or_loop_prediction;
  // } else {
  //   statistical_corrector_.get_prediction(
  //       br_pc, old_prediction_info.tage, old_prediction_info.tage_or_loop_prediction,
  //       &old_prediction_info.sc);
  //   old_prediction_info.final_prediction = old_prediction_info.sc.prediction;
  // }



  // ***********************************************************
  // * Create new prediction queue entry and add it to the queue
  // ***********************************************************

  auto& future_prediction_info = prediction_info_buffer_[branch_id + AHEAD_DISTANCE];
  future_prediction_info.tage.final_prediction = false;
  future_prediction_info.tage_prediction_valid = true;


  delay_queue_entry temp_entry;

  for(uns i = 0; i < SND_TAG_NO_PRED; i++){
    temp_entry.future_tage_preds[i] =   tage_.get_prediction(br_pc, &future_prediction_info.tage, i);
    temp_entry.tage_pred_confs[i] = future_prediction_info.tage.high_confidence[i];
    temp_entry.tage_pred_used[i] = false;
    int hit_bank = future_prediction_info.tage.hit_bank[i];
    temp_entry.tage_hit_bank[i] = hit_bank;
    temp_entry.tage_hit_index[i] = future_prediction_info.tage.indices[hit_bank];
    int alt_bank = future_prediction_info.tage.alt_bank[i];
    temp_entry.tage_alt_bank[i] = alt_bank;
    temp_entry.tage_alt_index[i] = future_prediction_info.tage.indices[alt_bank];
    temp_entry.tage_use_alt[i] =  future_prediction_info.tage.use_alt[i];
  }            
              
  temp_entry.br_pc = br_pc;
  //temp_entry.br_npc = br_npc;
  temp_entry.insert_cycle = current_cycle;
  temp_entry.branch_id = branch_id + AHEAD_DISTANCE;
  //temp_entry.current_pred = prediction_info.final_prediction;
  // TODO: check if we actually need/use this field
  //temp_entry.is_ret= op->table_info->cf_type == CF_RET;
  future_tage_response_delay_queue.push_back(temp_entry);



  return prediction_info.final_prediction;

}

template <class CONFIG>
void Tage_SC_L<CONFIG>::commit_state(uint32_t branch_id, uint64_t br_pc,
                                     Branch_Type br_type, bool resolve_dir, uint64_t target) {

  auto& prediction_info = prediction_info_buffer_[branch_id];


   if(!br_type.is_indirect){ //only direct branches update the l0 btb

    if(resolve_dir) {
      auto access_res = btb_.access(br_pc);
      if(!access_res.hit) {
        btb_.insert(br_pc, target, 2);
      } else {
        if(access_res.entry->counter < 3){
          access_res.entry->counter++;
        }
        bool bm_correct = resolve_dir == prediction_info.btb_prediction;
        bool ffp_correct = resolve_dir == prediction_info.final_prediction;
        if(FFP_USE_BM){
          if(bm_correct && (!ffp_correct)){
            if(access_res.entry->use_bm_ctr < 7){
              access_res.entry->use_bm_ctr++;
            }
            if(access_res.entry->use_bm_ctr > FFP_BM_THRESH){
              access_res.entry->use_bm = true;
            }
          }
          else if((!bm_correct) && ffp_correct){
            if(FFP_KILL_BM_ONE_WRONG){
              access_res.entry->use_bm_ctr = 0;
              access_res.entry->use_bm = false;
            }
            else{
              if(access_res.entry->use_bm_ctr > 0){
                access_res.entry->use_bm_ctr--;
              }
              if(access_res.entry->use_bm_ctr <= FFP_FFP_THRESH){
                access_res.entry->use_bm = false;
              }
            }
          }
        }
      }
    } else {
      auto access_res = btb_.probe(br_pc);
      bool bm_correct = resolve_dir == prediction_info.btb_prediction;
      bool ffp_correct = resolve_dir == prediction_info.final_prediction;
      if(access_res.hit) {
        if(access_res.entry->counter > 0){
          access_res.entry->counter--;
        }
        if(FFP_USE_BM){
          if(bm_correct && (!ffp_correct)){
            if(access_res.entry->use_bm_ctr < 7){
              access_res.entry->use_bm_ctr++;
            }
            if(access_res.entry->use_bm_ctr > FFP_BM_THRESH){
              access_res.entry->use_bm = true;
            }
          }
          else if((!bm_correct) && ffp_correct){
            if(FFP_KILL_BM_ONE_WRONG){
              access_res.entry->use_bm_ctr = 0;
              access_res.entry->use_bm = false;
            } else {
              if(access_res.entry->use_bm_ctr > 0){
                access_res.entry->use_bm_ctr--;
              }
              if(access_res.entry->use_bm_ctr <= FFP_FFP_THRESH){
                access_res.entry->use_bm = false;
              }
            }
          }
        }
      }
      else if((prediction_info.btb_prediction == resolve_dir) && (prediction_info.final_prediction != resolve_dir)) {
        btb_.insert(br_pc, target, 1);
      }
    }
  }
  

  // if (CONFIG::USE_SC) {
  //   statistical_corrector_.commit_state(
  //       br_pc, resolve_dir, prediction_info.tage, prediction_info.sc,
  //       prediction_info.tage_or_loop_prediction);
  // }

  // if (CONFIG::USE_LOOP_PREDICTOR) {
  //   if (prediction_info.loop.valid) {
  //     if (prediction_info.final_prediction != prediction_info.loop.prediction) {
  //       loop_predictor_beneficial_.update(resolve_dir ==
  //                                         prediction_info.loop.prediction);
  //     }
  //   }
  //   loop_predictor_.commit_state(
  //       br_pc, resolve_dir, prediction_info.loop,
  //       prediction_info.final_prediction != resolve_dir,
  //       prediction_info.tage.final_prediction);
  // }
  if(prediction_info.tage_prediction_valid) {
    tage_.commit_state(br_pc, resolve_dir, prediction_info.tage,
        prediction_info.final_prediction, prediction_info.tage.tag2);
  }
}



  // ***********************************************************
  // * We do not use the recover functions in ChampSim since we 
  // * only have 1 in-flight branch at any time, so there is no
  // * need for a rollback and flush
  // ***********************************************************
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



// ***********************************************************
// * We do not use the recover functions in ChampSim since we 
// * only have 1 in-flight branch at any time, so there is no
// * need for a rollback and flush
// ***********************************************************
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
    // if (CONFIG::USE_LOOP_PREDICTOR) {
    //   loop_predictor_.commit_state_at_retire(
    //     br_pc, resolve_dir, prediction_info.loop,
    //     prediction_info.final_prediction != resolve_dir,
    //     prediction_info.tage.final_prediction);
    // }
    tage_.commit_state_at_retire(prediction_info.tage);
    // if (CONFIG::USE_SC) {
    //   statistical_corrector_.commit_state_at_retire();
    // }

    prediction_info_buffer_.deallocate_front(branch_id);
    // Remove the corresponding delay queue entry which should
    // be the front element if there is a prediction queue entry
    // for this branch. Notice that for the first AHEAD_DISTANCE
    // branches we do not have an entry in the prediction queue
    if(future_tage_response_delay_queue.front().branch_id == branch_id) {
      future_tage_response_delay_queue.pop_front();
    }
  }
}

template <class CONFIG>
void Tage_SC_L<CONFIG>::retire_non_branch_ip(uint32_t branch_id) {
  // std::cerr << "retire_non_branch_ip(" << branch_id << ")\n";
  //prediction_info_buffer_.deallocate_front(branch_id);
  future_tage_response_delay_queue.pop_back();
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
  // if (CONFIG::USE_LOOP_PREDICTOR) {
  //   loop_predictor_.update_speculative_state(prediction_info.loop);
  // }
  // if (CONFIG::USE_SC) {
  //   statistical_corrector_.update_speculative_state(
  //       br_pc, branch_dir, br_target, br_type, &prediction_info.sc);
  // }

  future_tage_response_delay_queue.back().current_pred = branch_dir;
  future_tage_response_delay_queue.back().br_npc = br_target;
}

}  // namespace tagescl

#endif  // SPEC_TAGE_SC_L_TAGESCL_HPP_