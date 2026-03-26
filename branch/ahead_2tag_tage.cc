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

#include "ahead_2tag_tage.h"

#include <iostream>
#include <unordered_map>
#include <memory>
#include <deque>

#include "libs/cache_lib/cache.h"
extern "C" {
#include "bp.param.h"
#include "core.param.h"
#include "globals/assert.h"
#include "table_info.h"
}

#include "bp/template_lib/tagescl_2tag.h"

#define DEBUG(proc_id, args...) _DEBUG(proc_id, DEBUG_BP_DIR, ##args)

namespace {

struct l0_btb_entry {
    Addr pc;
    Addr target;
    int counter;

    int use_bm_ctr;
    Flag use_bm;
    int use_gshare_ctr;
    Flag use_gshare;
};


struct delay_queue_entry{
  //std::vector<Flag> future_tage_preds;
  //std::vector<Flag> tage_pred_confs;
  //std::vector<Flag> tage_pred_used;
  Flag future_tage_preds[MAX_SECONDARY_TAG_BITS];
  Flag tage_pred_confs[MAX_SECONDARY_TAG_BITS];
  Flag tage_pred_used[MAX_SECONDARY_TAG_BITS];
  int tage_hit_bank[MAX_SECONDARY_TAG_BITS];
  int tage_hit_index[MAX_SECONDARY_TAG_BITS];
  int tage_alt_bank[MAX_SECONDARY_TAG_BITS];
  int tage_alt_index[MAX_SECONDARY_TAG_BITS];
  int tage_use_alt[MAX_SECONDARY_TAG_BITS];

  int64_t branch_id;
  uint64_t insert_cycle;
  uint64_t br_pc;
  uint64_t br_npc; //(luke) next pc
  Flag current_pred;
  Flag is_ret; 
};

std::vector<Cache_cpp<l0_btb_entry>> l0_across_all_cores;
// A vector of TAGE-SC-L tables. One table per core.
std::vector<std::unique_ptr<Tage_SC_L_Base_2tag>> tagescl_predictors;

//(luke) the prediction queue. each entry is an array of tage predictions for each possible missing history combination (or more specifically 2nd tag hash if 2nd tag bits < ahead distance).
std::deque<delay_queue_entry> future_tage_response_delay_queue;

//uns32 global_tage_alloc_count = 0;

// Helper function for producing a Branch_Type struct.
Branch_Type get_branch_type(uns proc_id, Cf_Type cf_type) {
  Branch_Type br_type;
  switch(cf_type) {
    case CF_BR:
    case CF_CALL:
      br_type.is_conditional = false;
      br_type.is_indirect    = false;
      break;
    case CF_CBR:
      br_type.is_conditional = true;
      br_type.is_indirect    = false;
      break;
    case CF_IBR:
    case CF_ICALL:
    case CF_ICO:
    case CF_RET:
    case CF_SYS:
      br_type.is_conditional = false;
      br_type.is_indirect    = true;
      break;
    default:
      // Should never see non-control flow instructions or invalid CF
      // types in the branch predictor.
      ASSERT(proc_id, false);
      break;
  }
  return br_type;
}

//(luke) hash alg to find the real 2nd tag for the missing history. iterates over the prediction queue to find the 'real' (predicted) branch outcomes. if the number of tag bits is enough to capture all possible histories (i.e. 2nd tag bits >= ahead distance) it just xor's the directions. if not it hashes the branch targets too according to Figure 6 in the paper.
//FFP_NUM_TAGE = number of potential missing history patterns that the 2nd tag can capture, i.e. 2 ^ 2nd tag bits
uns get_recent_hist_hash(Op* op){
  uns res = 0;
  uns j = 0;
  uint64_t current;
  if(FFP_NUM_TAGE == 1){
    return 0;
  }
  if(AHEAD_DISTANCE == 0){
    ASSERT(0, FFP_NUM_TAGE == 8);
    auto pc = op->inst_info->addr;
    return ((pc>>1) ^ (pc>>4)) & 0x07;
  }
  for(int i = (int)future_tage_response_delay_queue.size() - 1; i >= 0; i--){
    if(j == AHEAD_DISTANCE){
      break;
    }
    j++;
    current = future_tage_response_delay_queue[i].br_npc;
    if(FFP_NUM_TAGE == 1){
      res = 0;
    }
    else if(FFP_NUM_TAGE == 2){
      if(FFP_HASH_DIR){
        res = res ^ future_tage_response_delay_queue[i].current_pred;
      }
      for(uns j = 0; j < FFP_HASH_PC_BITS; j++){ 
        res = res ^ (current & 0x01);
        current = current >> 1;
      }
    }
    else if (FFP_NUM_TAGE == 4) {
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
    else if(FFP_NUM_TAGE == 8){
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
    else if(FFP_NUM_TAGE == 16){
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
    else if(FFP_NUM_TAGE == 32){
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
    else if(FFP_NUM_TAGE == 64){
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
    else if(FFP_NUM_TAGE == 128){
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
    else if(FFP_NUM_TAGE == 256){
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
    else if(FFP_NUM_TAGE == 512){
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
    else if(FFP_NUM_TAGE == 1024){
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
    else if(FFP_NUM_TAGE == 2048){
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
    else if(FFP_NUM_TAGE == 4096){
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
}  // end of anonymous namespace

void bp_ahead_2tag_tage_init() {
  if(tagescl_predictors.size() == 0){
    tagescl_predictors.reserve(NUM_CORES);
    for(uns i = 0; i < NUM_CORES; i++){
      if(FUTURE_TAGE_SIZE_KB == 64){
        if(TAGE_7TABLES){
          tagescl_predictors.push_back(
            std::make_unique<Tage_SC_L_2tag<TAGE_SC_L_CONFIG_64KB_7TABLE>>(2*NODE_TABLE_SIZE));
        }
        else{
          tagescl_predictors.push_back(
            std::make_unique<Tage_SC_L_2tag<TAGE_SC_L_CONFIG_64KB>>(2*NODE_TABLE_SIZE));
        }
      }
      else if(FUTURE_TAGE_SIZE_KB == 128){
        tagescl_predictors.push_back(
          std::make_unique<Tage_SC_L_2tag<TAGE_SC_L_CONFIG_128KB_V1>>(NODE_TABLE_SIZE + AHEAD_DISTANCE));
      }
      else{
        ASSERT(0, FALSE);
      }
      l0_across_all_cores.push_back(Cache_cpp<l0_btb_entry>("l0_btb", L0_BTB_SIZE, L0_BTB_ASSOC, 1, SRRIP_REPL));
    }
  }
  ASSERT(0, BP_UPDATE_AT_RETIRE);
  ASSERT(0, FFP_NUM_TAGE <= MAX_SECONDARY_TAG_BITS);
  ASSERTM(0, tagescl_predictors.size() == NUM_CORES,
          "future_tages 2 tag not initialized correctly");
  ASSERTM(0, l0_across_all_cores.size() == NUM_CORES,
          "l0 btb not initialized correctly");
}

//(luke) i think the branch_id essentially functions as a sequence number. its used later to match prediction queue entries to the branch instructions that need them.
//could be wrong though.
void bp_ahead_2tag_tage_timestamp(Op* op) { 
  uns proc_id = op->proc_id;
  int64_t old_id = tagescl_predictors[proc_id]->get_old_branch_id(AHEAD_DISTANCE);
  int64_t new_id = tagescl_predictors[proc_id]->get_new_branch_id();
  op->recovery_info.future_tage_branch_id = new_id;
  op->recovery_info.future_tage_update_id = old_id;

  op->oracle_info.ffp_bm_override = false;
  op->oracle_info.ffp_gshare_override = false;
  op->oracle_info.useful_ffp_gshare_override = false;
  op->oracle_info.bad_ffp_gshare_override = false;
  DEBUG(0, "op %llu get new id %ld, old id %ld\n", op->op_num, new_id, old_id);
}

//(luke) basic overview:
//1. create the tage predictions for the (ahead distance)th next branch. store them in temp_entry then push to the prediction queue
//2. get the btb prediction for the current branch
//3. search through the prediction queue to find the tage prediction for this current branch. select the right one with the outcome of get_recent_hist_hash.
//4. determine if the prediction was late depending on the cycle the corresponding prediction queue entry was created and the current cycle
uns8 bp_ahead_2tag_tage_pred(Op* op) {
  uns proc_id = op->proc_id;

  uns fft_picker = get_recent_hist_hash(op);
  op->ffp_recent_hist_hash = fft_picker;

  DEBUG(0, "Predicting for op_num:%s addr:%s, t_dir:%d\n",
        unsstr64(op->op_num), hexstr64s(op->inst_info->addr), op->oracle_info.dir);

  delay_queue_entry temp_entry;
  for(uns i = 0; i < FFP_NUM_TAGE; i++){
    op->oracle_info.ffp_tage_index = i;
    op->oracle_info.ffp_tage_conf[i] = 0;
    temp_entry.future_tage_preds[i] = tagescl_predictors[proc_id]->get_prediction(
      op->recovery_info.future_tage_branch_id, op->inst_info->addr, op, i);
    temp_entry.tage_pred_confs[i] = op->oracle_info.ffp_tage_conf[i];
    temp_entry.tage_pred_used[i] = false;
    temp_entry.tage_hit_bank[i] = op->oracle_info.hit_bank;
    temp_entry.tage_hit_index[i] = op->oracle_info.hit_index;
    temp_entry.tage_alt_bank[i] = op->oracle_info.alt_bank;
    temp_entry.tage_alt_index[i] = op->oracle_info.alt_index;
    temp_entry.tage_use_alt[i] = op->oracle_info.use_alt;
  }            
              
  temp_entry.br_pc = op->inst_info->addr;
  temp_entry.br_npc = op->oracle_info.npc;
  temp_entry.insert_cycle = cycle_count;
  temp_entry.branch_id = op->recovery_info.future_tage_branch_id;
  temp_entry.is_ret= op->table_info->cf_type == CF_RET;
  future_tage_response_delay_queue.push_back(temp_entry);

  //if(!op->off_path){
  //  fprintf(stderr, "PUSH: currently at %llx, pred pc %lx, dir: %d, current id %lld\n", 
  //    op->inst_info->addr, temp_entry.future_tage_preds.pc, temp_entry.future_tage_preds.pred, op->recovery_info.future_tage_branch_id);
  //}

  auto& l0_btb = l0_across_all_cores.at(op->proc_id);

  Addr pc = op->inst_info->addr;
  Cache_access_result<l0_btb_entry> res = l0_btb.probe(op->proc_id, pc);
  Flag hit = res.hit;
  Flag counter_taken = res.data.counter>1;
  //Flag btb_prediction = USE_2_BIT_COUNTER_IN_L0 ? hit : hit && counter_taken;
  Flag btb_prediction = hit;
  if(USE_2_BIT_COUNTER_IN_L0){
    btb_prediction = btb_prediction && counter_taken;
  }
  //if(!op->off_path){
  //  printf("L0BTB: pc %llx id %lld, pred is %d, true_dir %d\n", op->inst_info->addr, op->recovery_info.future_tage_branch_id, btb_prediction, op->oracle_info.dir);
  //}
  op->oracle_info.ffp_l0_btb_pred = btb_prediction;
  if(hit){
    op->oracle_info.ffp_l0_btb_miss = false;
  }
  else{
    op->oracle_info.ffp_l0_btb_miss = true;
  }

  //if(op->table_info->cf_type == CF_BR || op->table_info->cf_type == CF_CALL){
  if(op->table_info->cf_type != CF_CBR){
    //even if l0_btb_miss, still return true,  
    return true;
  }

  Flag found_it = FALSE;

  for(auto& element : future_tage_response_delay_queue){
    if(element.branch_id == op->recovery_info.future_tage_update_id){
      if(AHEAD_DISTANCE == 0){
        ASSERT(0, element.insert_cycle == cycle_count);
      }
      found_it = true;
      Flag fft_final_pred = FALSE;
      fft_final_pred = element.future_tage_preds[fft_picker];
      if(element.tage_pred_used[fft_picker]){
        STAT_EVENT(0, FFP_CONFLICT);
      }
      else{
        element.tage_pred_used[fft_picker] = true;
      }
      //Flag fft_final_conf = element.tage_pred_confs[fft_picker];
      op->oracle_info.ffp_pred = fft_final_pred;
      op->oracle_info.ffp_late = false;
      op->oracle_info.ffp_cycle_available = element.insert_cycle + FUTURE_TAGE_LATENCY;
      //if(op->inst_info->addr == 0x86da4f){
      ////if(element.br_pc == 0x4bf95e){
      //  printf("pdir:%d, tdir:%d, missing:",op->oracle_info.ffp_pred, op->oracle_info.dir);
      //  uns j = 0;
      //  for(int i = (int)future_tage_response_delay_queue.size() - 1; i >= 0; i--){
      //    if(j == (AHEAD_DISTANCE + 1)){
      //      break;
      //    }
      //    printf("%lx, ", future_tage_response_delay_queue[i].br_pc);
      //    j++;
      //  }
      //  printf("tag2:%d, hbank:%d, hindex:%d, abank:%d, aindex:%d, alt:%d\n", 
      //    fft_picker, element.tage_hit_bank[fft_picker], element.tage_hit_index[fft_picker],
      //    element.tage_alt_bank[fft_picker], element.tage_alt_index[fft_picker], element.tage_use_alt[fft_picker]);
      //  printf("bm pred %d, ffp pred %d, use bm %d, l0 btb hit %d\n", btb_prediction, fft_final_pred, res.data.use_bm, res.hit);
      //}
      DEBUG(0,"bm pred %d, use bm %d, l0 hit %d\n", btb_prediction, res.data.use_bm, res.hit);
      if(FFP_USE_BM && res.data.use_bm && res.hit && AHEAD_DISTANCE != 0){
      //if(FFP_USE_BM && res.data.use_bm && res.hit && (res.data.counter == 3 || res.data.counter == 0)){
      //doesn't work, 0404_ffp0_rotate_bm_ctrconf_gshare2bit
        op->oracle_info.ffp_cycle_available = cycle_count;
        op->oracle_info.ffp_bm_override = true;
        if(!op->off_path){
          STAT_EVENT(op->proc_id, FFP_BM_OVERRIDE);
          DEBUG(0,"BM OVERRIDE\n");
          if(btb_prediction != fft_final_pred){
            if(btb_prediction == op->oracle_info.dir){
              STAT_EVENT(0, FFP_USEFUL_BM_OVERRIDE);
            }
            else{
              STAT_EVENT(0, FFP_BAD_BM_OVERRIDE);
            }
          }
        }
        //if(FFP_USE_TAGE && btb_prediction != op->oracle_info.ffp_tage_pred){
        //  op->oracle_info.ffp_cycle_available = cycle_count + LATE_BP_LATENCY - 1;
        //  return op->oracle_info.ffp_tage_pred;
        //}
        return btb_prediction;
      }
      if(cycle_count - element.insert_cycle >= FUTURE_TAGE_LATENCY){
        return fft_final_pred;
      }
      else{
        if(!op->off_path){
          STAT_EVENT(op->proc_id, FFP_LATE);  
        }
        op->oracle_info.ffp_late = true;
        if(FFP_USE_LATE_PRED){
          return fft_final_pred;
        }
      }
      break;
    }
  }  
  if(op->recovery_info.future_tage_update_id != -1){
    ASSERT(0, found_it);
  }
  if(FFP_USE_LATE_PRED){
    ASSERT(0, FALSE || (!found_it));
  }
  return btb_prediction;
}

void bp_ahead_2tag_tage_spec_update(Op* op) {
  uns proc_id = op->proc_id;
  DEBUG(proc_id, "spec update on op %llu, branch id %lld\n", op->op_num, op->recovery_info.future_tage_branch_id);
  
  tagescl_predictors[proc_id]->update_speculative_state_2tag(
    op->recovery_info.future_tage_branch_id, op->inst_info->addr,
    get_branch_type(proc_id, op->table_info->cf_type), op->oracle_info.pred,
    op->oracle_info.target);
  ASSERT(0, future_tage_response_delay_queue.back().branch_id == op->recovery_info.future_tage_branch_id);
  future_tage_response_delay_queue.back().current_pred = op->oracle_info.pred;
}

void bp_ahead_2tag_tage_update(Op* op) {
  const uns   proc_id      = op->proc_id;
  auto cf_type             = op->table_info->cf_type;
  const Addr  pc           = op->inst_info->addr;
  ASSERT(0, !op->off_path);
  //Flag skip_tage = false;
  
  if(cf_type <= CF_CALL){ //only direct branches update the l0 btb
    auto&       l0_btb       = l0_across_all_cores.at(proc_id);

    if(op->oracle_info.dir) {
      auto access_res = l0_btb.access(proc_id, pc);
      if(!access_res.hit){
        l0_btb_entry new_entry = {pc, op->oracle_info.target, 2, 0, false, 0, false};
        #if ENABLE_GLOBAL_DEBUG_PRINT
        auto insert_res = l0_btb.insert(proc_id, pc, /*is_prefetch =*/ FALSE, new_entry);
        DEBUG(proc_id, "write l0btb op %llu, pc=x%llx, repl: %d, replpc = %llx\n", op->op_num, pc, insert_res.hit, insert_res.line_addr);
        #else
        l0_btb.insert(proc_id, pc, /*is_prefetch =*/ FALSE, new_entry);
        #endif
      }
      else 
      {
        if(access_res.data.counter < 3){
          access_res.data.counter++;
        }
        Flag bm_correct = op->oracle_info.dir == op->oracle_info.ffp_l0_btb_pred;
        Flag ffp_correct = op->oracle_info.dir == op->oracle_info.ffp_pred;
        if(FFP_USE_BM){
          if(bm_correct && (!ffp_correct)){
            if(access_res.data.use_bm_ctr < 7){
              access_res.data.use_bm_ctr++;
            }
            if(access_res.data.use_bm_ctr > FFP_BM_THRESH){
              access_res.data.use_bm = true;
            }
          }
          else if((!bm_correct) && ffp_correct){
            if(FFP_KILL_BM_ONE_WRONG){
              access_res.data.use_bm_ctr = 0;
              access_res.data.use_bm = false;
            }
            else{
              if(access_res.data.use_bm_ctr > -8){
                access_res.data.use_bm_ctr--;
              }
              if(access_res.data.use_bm_ctr < FFP_FFP_THRESH){
                access_res.data.use_bm = false;
              }
            }
          }
          //if(access_res.data.use_bm_ctr == 7 && FFP_SKIP_TAGE_IF_USE_BM){
          //  skip_tage = true;
          //}
          ASSERT(proc_id, access_res.data.use_bm_ctr <= 7 && access_res.data.use_bm_ctr >= -8);
          DEBUG(proc_id, "opnum %llu, pc %llx bm_correct %d, ffp_correct %d, use_bm_ctr %d, use_bm %d\n", op->op_num, pc, bm_correct, ffp_correct, access_res.data.use_bm_ctr, access_res.data.use_bm);
        }
        l0_btb.update(access_res);
      }
    } 
    else{
      auto access_res = l0_btb.probe(proc_id, pc);
      Flag bm_correct = op->oracle_info.dir == op->oracle_info.ffp_l0_btb_pred;
      Flag ffp_correct = op->oracle_info.dir == op->oracle_info.ffp_pred;
      if(access_res.hit)
      {
        if(access_res.data.counter > 0){
          access_res.data.counter--;
        }
        if(FFP_USE_BM){
          if(bm_correct && (!ffp_correct)){
            if(access_res.data.use_bm_ctr < 7){
              access_res.data.use_bm_ctr++;
            }
            if(access_res.data.use_bm_ctr > FFP_BM_THRESH){
              access_res.data.use_bm = true;
            }
          }
          else if((!bm_correct) && ffp_correct){
            if(FFP_KILL_BM_ONE_WRONG){
              access_res.data.use_bm_ctr = 0;
              access_res.data.use_bm = false;
            }
            else{
              if(access_res.data.use_bm_ctr > -8){
                access_res.data.use_bm_ctr--;
              }
              if(access_res.data.use_bm_ctr < FFP_FFP_THRESH){
                access_res.data.use_bm = false;
              }
            }
          }
          //if(access_res.data.use_bm_ctr == 7 && FFP_SKIP_TAGE_IF_USE_BM){
          //  skip_tage = true;
          //}
          ASSERT(proc_id, access_res.data.use_bm_ctr <= 7 && access_res.data.use_bm_ctr >= -8);
          DEBUG(proc_id, "opnum %llu, pc %llx bm_correct %d, ffp_correct %d, use_bm_ctr %d, use_bm %d\n", op->op_num, pc, bm_correct, ffp_correct, access_res.data.use_bm_ctr, access_res.data.use_bm);
        }
        l0_btb.update(access_res);
      }
      else if((op->oracle_info.ffp_l0_btb_pred == op->oracle_info.dir) && (op->oracle_info.ffp_pred != op->oracle_info.dir)){
        ASSERT(0, op->oracle_info.ffp_pred);
        l0_btb_entry new_entry = {pc, op->oracle_info.target, 1, 0, false, 0, false};
        #if ENABLE_GLOBAL_DEBUG_PRINT
        auto insert_res = l0_btb.insert(proc_id, pc, /*is_prefetch =*/ FALSE, new_entry);
        DEBUG(proc_id, "write l0btb op %llu, pc=x%llx, repl: %d, replpc = %llx\n", op->op_num, pc, insert_res.hit, insert_res.line_addr);
        #else
        l0_btb.insert(proc_id, pc, /*is_prefetch =*/ FALSE, new_entry);
        #endif
      }
    }
  }

  if(op->recovery_info.future_tage_update_id != -1){
    //printf("updating op %llu, pc:%llx\n", op->op_num, op->inst_info->addr);
    DEBUG(0, "updating tage with br id %lld\n", op->recovery_info.future_tage_branch_id);
    ASSERT(0, op->recovery_info.future_tage_update_id == future_tage_response_delay_queue.front().branch_id);
    op->oracle_info.tage_alloc = false;
    tagescl_predictors[proc_id]->commit_state(
      op->recovery_info.future_tage_update_id, op->inst_info->addr,
      get_branch_type(proc_id, op->table_info->cf_type), op->oracle_info.dir, op, op->ffp_recent_hist_hash);
    if(op->oracle_info.tage_alloc){
      STAT_EVENT(op->proc_id, FFP_BANK0_ALLOC + op->ffp_recent_hist_hash);
    }
    //printf("done update\n");
  }
}

void bp_ahead_2tag_tage_retire(Op* op) {
  uns proc_id = op->proc_id;
  DEBUG(0, "retire op %llu, br id %lld\n", op->op_num, op->recovery_info.future_tage_branch_id);
  if(op->recovery_info.future_tage_update_id != -1){
    ASSERT(0, future_tage_response_delay_queue.front().branch_id == op->recovery_info.future_tage_update_id);
    future_tage_response_delay_queue.pop_front();
  }
  tagescl_predictors[proc_id]->commit_state_at_retire_ahead(
    op->recovery_info.future_tage_branch_id, op->inst_info->addr,
    get_branch_type(proc_id, op->table_info->cf_type), op->oracle_info.dir,
    op->oracle_info.target, op->recovery_info.future_tage_update_id);
}

void bp_ahead_2tag_tage_recover(Recovery_Info* recovery_info) {
  uns proc_id = recovery_info->proc_id;
  auto mispred_id = recovery_info->future_tage_branch_id;
  DEBUG(0, "recover op %llu, br id %lld\n", recovery_info->op_num, mispred_id);
  uns nums_to_pop = 0;
  for(uint32_t i = future_tage_response_delay_queue.size() - 1; i >= 0 ; i--){
    delay_queue_entry temp = future_tage_response_delay_queue[i];
    if(temp.branch_id > mispred_id){
      nums_to_pop++;
    }
    else{
      break;
    }
  }
  //fprintf(stderr, "mispred id: %lld, ", mispred_id);
  for(uns i = 0; i < nums_to_pop; i++){
    future_tage_response_delay_queue.pop_back();
  }
  ASSERT(0, future_tage_response_delay_queue.back().branch_id == mispred_id);
  future_tage_response_delay_queue.back().current_pred = recovery_info->new_dir;
  tagescl_predictors[proc_id]->flush_branch_and_repair_state(
    recovery_info->future_tage_branch_id, recovery_info->PC,
    get_branch_type(proc_id, recovery_info->cf_type), recovery_info->new_dir,
    recovery_info->branchTarget);
}
