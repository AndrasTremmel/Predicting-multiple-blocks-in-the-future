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

#include "2tag_tage.hpp"
#include "ahead_tagescl_configs.hpp"
#include "utils.hpp"
#include "btb.hpp"
#include <deque>

typedef unsigned int uns;


#define AHEAD_DISTANCE 5
#define USE_2_BIT_COUNTER_IN_L0 1
#define FFP_HASH_DIR 1      // used only when SND_TAG_NO_PRED = 2
#define FFP_HASH_DIR_ONLY  ((1 << AHEAD_DISTANCE) <=  SND_TAG_NO_PRED)    // use PC as well for missing history hash computation
#define FFP_HASH_PC_BITS 1 // since we currently have SND_TAG_NO_PRED set to 32 (2^5), FFP_HASH_PC_BITS can only be 0,1 or 2
#define FFP_USE_BM 1
#define FFP_USE_LATE_PRED 1
#define FUTURE_TAGE_LATENCY 3
#define FFP_BM_THRESH 2
#define FFP_KILL_BM_ONE_WRONG 1

#define L0_BTB_SIZE 1024
#define L0_BTB_ASSOC 4

struct delay_queue_entry{
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
  uint64_t br_npc; 
  bool current_pred;
  bool is_ret; 
};



namespace tagescl {

template <class CONFIG>
struct Tage_SC_L_Prediction_Info {
  Tage_Prediction_Info_2tag<typename CONFIG::TAGE> tage;
  uint64_t br_pc;
  int rng_seed;
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
                            Branch_Type br_type, bool resolve_dir, uint64_t target) = 0;
  virtual void commit_state_at_retire(uint32_t branch_id, uint64_t br_pc,
                                      Branch_Type br_type, bool resolve_dir,
                                      uint64_t br_target) = 0;
  virtual uns get_recent_hist_hash(uint64_t br_pc) = 0;
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
        btb_(L0_BTB_SIZE, L0_BTB_ASSOC),
        prediction_info_buffer_(max_in_flight_branches, AHEAD_DISTANCE) {}

  // Gets a new branch_id for a new in-flight branch. The id remains valid
  // until
  // the branch is retired or flushed. The class internally maintains metadata
  // for each in-flight branch. The rest of the public functions in this class
  // need the id of a branch to work on.

  uint32_t get_new_branch_id() override {
    uint32_t branch_id = prediction_info_buffer_.get_read_id();
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
                    bool resolve_dir, uint64_t target) override;

  // Updates predictor states that are critical for algorithm correctness.
  // Thus, should always be called in the retire state and after
  // commit_state()
  // is called. branch_id is invalidated and should not be used anymore.
  void commit_state_at_retire(uint32_t branch_id, uint64_t br_pc,
                              Branch_Type br_type, bool resolve_dir,
                              uint64_t br_target) override;

  // Computes missing history hash for the current branch pc
  uns get_recent_hist_hash(uint64_t br_pc) override;


 private:
  Random_Number_Generator random_number_gen_;
  Tage_2tag<typename CONFIG::TAGE> tage_;
  L0BTB btb_;
  std::deque<delay_queue_entry> future_tage_response_delay_queue;

  // Used for remembering necessary information gathered during prediction that are needed for update.
  CircularBuffer<Tage_SC_L_Prediction_Info<CONFIG>> prediction_info_buffer_;
};

template <class CONFIG>
uns Tage_SC_L<CONFIG>::get_recent_hist_hash(uint64_t br_pc) {
  //std::cout << "Calculating missing history hash for branch pc: " << br_pc << std::endl;
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
      for(uns k = 0; k < FFP_HASH_PC_BITS; k++){ 
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
        assert(false);
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
        assert(AHEAD_DISTANCE == 3);  
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
        assert(false);
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
        assert(AHEAD_DISTANCE == 4);  
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
        assert(false);
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
        assert(AHEAD_DISTANCE == 5);  
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
        assert(false);
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
        assert(AHEAD_DISTANCE == 6);  
      }
      else if(FFP_HASH_PC_BITS == 1){
        res = res ^ (current >> 1) ^ (current >> 7);
      }
      else{
        assert(false);
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
        assert(AHEAD_DISTANCE == 7);  
      }
      else if(FFP_HASH_PC_BITS == 1){
        res = res ^ (current >> 1) ^ (current >> 8);
      }
      else{
        assert(false);
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
        assert(AHEAD_DISTANCE == 8);  
      }
      else if(FFP_HASH_PC_BITS == 1){
        res = res ^ (current >> 1) ^ (current >> 9);
      }
      else{
        assert(false);
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
        assert(AHEAD_DISTANCE == 9);  
      }
      else if(FFP_HASH_PC_BITS == 1){
        res = res ^ (current >> 1) ^ (current >> 10);
      }
      else{
        assert(false);
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
        assert(AHEAD_DISTANCE == 10);  
      }
      else if(FFP_HASH_PC_BITS == 1){
        res = res ^ (current >> 1) ^ (current >> 11);
      }
      else{
        assert(false);
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
        assert(AHEAD_DISTANCE == 11);  
      }
      else if(FFP_HASH_PC_BITS == 1){
        res = res ^ (current >> 1) ^ (current >> 12);
      }
      else{
        assert(false);
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
        assert(AHEAD_DISTANCE == 12);  
      }
      else if(FFP_HASH_PC_BITS == 1){
        res = res ^ (current >> 1) ^ (current >> 13);
      }
      else{
        assert(false);
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
      assert(false);
    }
  }
  //std::cout << "Calculated missing history hash for branch pc " << br_pc << " is " << res << std::endl;
  return res;
}

template <class CONFIG>
bool Tage_SC_L<CONFIG>::get_prediction(uint32_t branch_id, uint64_t br_pc, uint64_t current_cycle) {
  // ***********************************************************
  // * Get the prediction info for the current branch
  // ***********************************************************
  //std::cout << "Starting prediction for branch id:: " << branch_id << " and branch pc: " << br_pc << std::endl;
  auto& prediction_info = prediction_info_buffer_[branch_id];

  // Update branch pc since it was unkown up until now
  prediction_info.br_pc = br_pc;

  // ***********************************************************
  // * Read the single cycle latency btb as a default prediction 
  // ***********************************************************
  //std::cout << "Starting btb readout..." << std::endl;
  L0BTB::Result res = btb_.probe(br_pc);
  bool hit = res.hit;
  //std::cout << "btb hit: " << hit << std::endl;
  bool counter_taken = hit ? res.entry->counter > 1 : 0;
  bool btb_prediction = hit;
  if (USE_2_BIT_COUNTER_IN_L0) {
    btb_prediction = btb_prediction && counter_taken;
  }

  prediction_info.btb_prediction = btb_prediction;

  bool final_pred = btb_prediction;

  //std::cout << "Finished btb readout..." << std::endl;

  // ***************************************************************
  // * Compute missing history hash and select the final prediction
  // * from the right element of the prediction queue using the 
  // * computed hash if prediction info contains ahead tage prediction
  // ***************************************************************

  if(prediction_info.tage_prediction_valid) {
    //std::cout << "Tage preiction valid starting missing history hash computation and predicton queue readout..." << std::endl;

    uns fft_picker = get_recent_hist_hash(br_pc);

    for(auto&element : future_tage_response_delay_queue) {
      if (element.branch_id == branch_id) {
        //std::cout << "Found prediction queue entry for branch id: " << br_pc << std::endl;
        bool tage_pred = element.future_tage_preds[fft_picker];
        //std::cout << "Tage prediction is: " << tage_pred << std::endl;
        prediction_info.tage.final_prediction = tage_pred;
        //std::cout << "Set final prediction info to be tage prediction..." << std::endl;
        prediction_info.tage.tag2 = (int) fft_picker;
        //std::cout << "Casted unsigned int hash to integer..." << std::endl;

        if(element.tage_pred_used[fft_picker]){
          //std::cout << "Prediction has already been used..." << std::endl;
          // Happens when we use the prediction queue entry for a non-branch instruction 
          // TODO: We could set back the corresponding tage_pred_used entry back to false
          // but we dont use this field anywhere else so it doesnt really matter
        } else{
          element.tage_pred_used[fft_picker] = true;
        }

        //std::cout << "Checking if we should fall back to the btb bimodal prediction..." << std::endl;
        if(hit) {
          if(FFP_USE_BM && res.entry->use_bm && AHEAD_DISTANCE != 0) {
            //std::cout << "Use btb prediction: " << btb_prediction << " as final prediction..." << std::endl;
            final_pred = btb_prediction;
            break;
          }
        }
        

        //std::cout << "Checking if we can use tage prediction..." << std::endl;
        if((current_cycle - element.insert_cycle >= FUTURE_TAGE_LATENCY) || FFP_USE_LATE_PRED) {
          //std::cout << "Use tage prediction as final prediction..." << std::endl;
          final_pred = tage_pred;
          break;
        }
        
        break;
      }
    }
  }

  //std::cout << "Storing final prediction in prediction info entry..." << std::endl;
  prediction_info.final_prediction = final_pred;
  

  // ***********************************************************
  // * Create new prediction queue entry and add it to the queue
  // ***********************************************************

  //std::cout << "Creating prediction queue entry for branch id: " << branch_id + AHEAD_DISTANCE << std::endl;
  auto& future_prediction_info = prediction_info_buffer_[branch_id + AHEAD_DISTANCE];
  future_prediction_info.tage.final_prediction = false;
  future_prediction_info.tage_prediction_valid = true;


  delay_queue_entry temp_entry;

  for(uns i = 0; i < SND_TAG_NO_PRED; i++){
    tage_.get_prediction(br_pc, &future_prediction_info.tage, i);
    temp_entry.future_tage_preds[i] = future_prediction_info.tage.prediction[i];
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
  temp_entry.insert_cycle = current_cycle;
  temp_entry.branch_id = branch_id + AHEAD_DISTANCE;
  // TODO: check if we actually need/use this field
  //temp_entry.is_ret= op->table_info->cf_type == CF_RET;
  future_tage_response_delay_queue.push_back(temp_entry);
  //std::cout << "Added predicton queue entry to queue" << std::endl;


  return prediction_info.final_prediction;
}

template <class CONFIG>
void Tage_SC_L<CONFIG>::commit_state(uint32_t branch_id, uint64_t br_pc,
                                     Branch_Type br_type, bool resolve_dir, uint64_t target) {

  //std::cout << "Starting commit state for branch id: " << branch_id << std::endl;                                      
  auto& prediction_info = prediction_info_buffer_[branch_id];


   if(!br_type.is_indirect){ //only direct branches update the l0 btb
    //std::cout << "Start btb update..." << std::endl;

    if(resolve_dir) {
      auto access_res = btb_.access(br_pc);
      if(!access_res.hit) {
        btb_.insert(br_pc, target, 2);
      } else {
        if(access_res.entry->counter < 3){
          access_res.entry->counter++;
        }
        bool bm_correct = resolve_dir == prediction_info.btb_prediction;
        bool ffp_correct = resolve_dir == prediction_info.tage.final_prediction;
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
              if(access_res.entry->use_bm_ctr <= FFP_BM_THRESH){
                access_res.entry->use_bm = false;
              }
            }
          }
        }
      }
    } else {
      auto access_res = btb_.probe(br_pc);
      bool bm_correct = resolve_dir == prediction_info.btb_prediction;
      bool ffp_correct = resolve_dir == prediction_info.tage.final_prediction;
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
              if(access_res.entry->use_bm_ctr <= FFP_BM_THRESH){
                access_res.entry->use_bm = false;
              }
            }
          }
        }
      }
      else if((prediction_info.btb_prediction == resolve_dir) && (prediction_info.tage.final_prediction != resolve_dir)) {
        btb_.insert(br_pc, target, 1);
      }
    }
  }
  

  // Only update TAGE on conditional branches
  if (!br_type.is_conditional) {
    return;
  }
  if(prediction_info.tage_prediction_valid) {
    //std::cout << "Tage prediction valid, starting commit for tage..." << std::endl;
    tage_.commit_state(br_pc, resolve_dir, prediction_info.tage,
        prediction_info.final_prediction, prediction_info.tage.tag2);
    //std::cout << "Finished tage commit" << std::endl;
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
  
    //std::cout << "Calling tage retire..." << std::endl;
    tage_.commit_state_at_retire(prediction_info.tage);
    
    //std::cout << "Deallocating front element of predicton info buffer..." << std::endl;
    prediction_info_buffer_.deallocate_front(branch_id);
    // Remove the corresponding delay queue entry which should
    // be the front element if there is a prediction queue entry
    // for this branch. Notice that for the first AHEAD_DISTANCE
    // branches we do not have an entry in the prediction queue
    assert(!future_tage_response_delay_queue.empty());
    if(future_tage_response_delay_queue.front().branch_id == branch_id) {
      //std::cout << "Remove front element of prediction queue..." << std::endl;
      future_tage_response_delay_queue.pop_front();
    }
  } else {
    // Non-branch instruction so we remove the prediction queue entry
    // we created this cycle earlier
    //std::cout << "Removing freshly added prediction queue entry from the back since the current instruction is not a branch" << std::endl;
    assert(!future_tage_response_delay_queue.empty() && future_tage_response_delay_queue.back().branch_id == branch_id + AHEAD_DISTANCE);
    future_tage_response_delay_queue.pop_back();
  }
}


template <class CONFIG>
void Tage_SC_L<CONFIG>::update_speculative_state(uint32_t branch_id,
                                                 uint64_t br_pc,
                                                 Branch_Type br_type,
                                                 bool branch_dir,
                                                 uint64_t br_target) {
  //std::cout << "Starting speculative update..." << std::endl;                                                  
  auto& prediction_info = prediction_info_buffer_[branch_id];
  prediction_info.rng_seed = random_number_gen_.seed_;
  prediction_info.updated_history = true;
  //std::cout << "Starting tage speculative update..." << std::endl;
  tage_.update_speculative_state(br_pc, br_target, br_type, branch_dir,
                                 &prediction_info.tage);
  
  //std::cout << "Tage speculative update finished starting prediction queue entry back update for branch id: " << branch_id + AHEAD_DISTANCE << std::endl;
  assert(future_tage_response_delay_queue.back().branch_id == branch_id + AHEAD_DISTANCE);
  future_tage_response_delay_queue.back().current_pred = branch_dir;
  future_tage_response_delay_queue.back().br_npc = br_target;
  //std::cout << "Finished predicton queue entry update..." << std::endl;
}

}  // namespace tagescl

#endif  // SPEC_TAGE_SC_L_TAGESCL_HPP_