/* Copyright 2020 HPS/SAFARI Research Groups ... */

#ifndef SPEC_TAGE_SC_L_TAGE_HPP_
#define SPEC_TAGE_SC_L_TAGE_HPP_

#include <cmath>
#include <vector>
#include <iostream>
#include <iomanip>
#include <cstdint>

#include "utils.hpp"

namespace tagescl {

template <int history_size>
class Long_History_Register {
 public:
  Long_History_Register(int max_in_flight_bits) : history_bits_() {
    int log_buffer_size =
        get_min_num_bits_to_represent(history_size + max_in_flight_bits);
    buffer_size_ = 1 << log_buffer_size;
    buffer_access_mask_ = (1 << log_buffer_size) - 1;
    max_num_speculative_bits_ = buffer_size_ - history_size;
    history_bits_.resize(buffer_size_);
  }
  void push_bit(bool bit) {
    head_ -= 1;
    history_bits_[head_ & buffer_access_mask_] = bit;
    num_speculative_bits_ += 1;
    assert(num_speculative_bits_ <= max_num_speculative_bits_);
  }
  void rewind(int num_rewind_bits) {
    assert(num_rewind_bits > 0 && num_rewind_bits <= num_speculative_bits_);
    num_speculative_bits_ -= num_rewind_bits;
    head_ += num_rewind_bits;
  }
  void retire(int num_retire_bits) {
    assert(num_retire_bits > 0 && num_retire_bits <= num_speculative_bits_);
    num_speculative_bits_ -= num_retire_bits;
    commit_head_ += num_retire_bits;
  }
  bool operator[](size_t i) const {
    return history_bits_[(head_ + i) & buffer_access_mask_];
  }
  const int64_t& head_idx() const { return head_; }
  const int64_t& commit_head_idx() const { return commit_head_; }
 private:
  int num_speculative_bits_ = 0;
  std::vector<bool> history_bits_;
  int64_t head_ = 0;
  int64_t commit_head_ = 0;
  int64_t buffer_size_;
  int64_t buffer_access_mask_;
  int64_t max_num_speculative_bits_;
};

template <int history_size>
class Folded_History {
 public:
  Folded_History(int original_length, int compressed_length)
      : current_value_(0),
        original_length_(original_length),
        compressed_length_(compressed_length),
        outpoint_(original_length % compressed_length) {}
  int64_t get_value() const { return current_value_; }
  void update(const Long_History_Register<history_size>& history_register) {
    current_value_ = (current_value_ << 1) ^ history_register[0];
    current_value_ ^= history_register[original_length_] << outpoint_;
    current_value_ ^= current_value_ >> compressed_length_;
    current_value_ &= (1 << compressed_length_) - 1;
  }
  void update_reverse(
      const Long_History_Register<history_size>& history_register) {
    current_value_ ^= history_register[0];
    current_value_ ^= history_register[original_length_] << outpoint_;
    current_value_ = ((current_value_ & 1) << (compressed_length_ - 1)) |
                     (current_value_ >> 1);
    current_value_ &= (1 << compressed_length_) - 1;
  }
 private:
  int64_t current_value_;
  int original_length_;
  int compressed_length_;
  int outpoint_;
};

template <class TAGE_CONFIG>
struct Tage_History_Sizes {
  static constexpr int N = TAGE_CONFIG::NUM_HISTORIES;
  constexpr Tage_History_Sizes() : arr() {
    double max_history = static_cast<double>(TAGE_CONFIG::MAX_HISTORY_SIZE);
    double min_history = static_cast<double>(TAGE_CONFIG::MIN_HISTORY_SIZE);
    double min_max_ratio = max_history / min_history;
    for (int i = 0; i < N; ++i) {
      double geometric_power = static_cast<double>(i) / static_cast<double>(N - 1);
      double geometric_multiplier = pow(min_max_ratio, geometric_power);
      arr[i] = static_cast<int>(min_history * geometric_multiplier + 0.5);
    }
  }
  int arr[N];
};

template <class TAGE_CONFIG>
struct Tage_Tables_Enabled {
  static constexpr int N = TAGE_CONFIG::NUM_HISTORIES;
  constexpr Tage_Tables_Enabled() : arr() {
    for (int i = 1; i <= 2 * N; ++i) {
      bool is_even_table = (i - 1) & 1;
      bool is_middle_table = (i >= TAGE_CONFIG::FIRST_2WAY_TABLE) &&
                             (i <= TAGE_CONFIG::LAST_2WAY_TABLE);
      arr[i] = is_even_table || is_middle_table;
    }
    arr[4] = false;
    arr[2 * N - 2] = false;
    arr[8] = false;
    arr[2 * N - 6] = false;
  }
  bool arr[2 * N + 1];
};

template <class TAGE_CONFIG>
struct Tage_Tag_Bits {
  static constexpr int N = TAGE_CONFIG::NUM_HISTORIES;
  constexpr Tage_Tag_Bits() : arr() {
    for (int i = 0; i < N; ++i) {
      if ((2 * i + 1) < TAGE_CONFIG::FIRST_LONG_HISTORY_TABLE)
        arr[i] = TAGE_CONFIG::SHORT_HISTORY_TAG_BITS;
      else
        arr[i] = TAGE_CONFIG::LONG_HISTORY_TAG_BITS;
    }
  }
  int arr[N];
};

struct Bimodal_Output {
  bool prediction;
  bool confidence;
};

struct Matched_Table_Banks {
  int hit_bank;
  int alt_bank;
};

// ---- STATISTICS STRUCTURE ----
struct TageStats {
  uint64_t total_predictions = 0;
  uint64_t total_mispredictions = 0;
  uint64_t bimodal_used = 0;
  uint64_t bimodal_mispredictions = 0;
  uint64_t tagged_used = 0;
  uint64_t tagged_mispredictions = 0;
  uint64_t longest_match_correct = 0;
  uint64_t longest_match_wrong = 0;
  uint64_t alt_prediction_used = 0;
  uint64_t alt_prediction_correct = 0;
  uint64_t alt_prediction_wrong = 0;
  uint64_t weak_longest_correct = 0;
  uint64_t weak_longest_wrong = 0;
  uint64_t new_entries_allocated = 0;
  uint64_t alt_selector_mispredictions = 0;
  uint64_t history_rewinds = 0;
  uint64_t commits = 0;
  uint64_t bank_hits[45] = {};
  uint64_t confidence_high_correct = 0;
  uint64_t confidence_high_wrong = 0;
  uint64_t confidence_medium_correct = 0;
  uint64_t confidence_medium_wrong = 0;
  uint64_t confidence_low_correct = 0;
  uint64_t confidence_low_wrong = 0;

  void print(const char* name) const {
    std::cerr << "\n========== " << name << " STATISTICS ==========\n";
    std::cerr << "Total predictions:        " << total_predictions << "\n";
    std::cerr << "Total mispredictions:     " << total_mispredictions << "\n";
    if (total_predictions > 0)
      std::cerr << "Misprediction rate:       " << std::fixed << std::setprecision(6)
                << (100.0 * total_mispredictions / total_predictions) << "%\n";
    std::cerr << "Bimodal used:             " << bimodal_used << "\n";
    std::cerr << "Bimodal mispredictions:   " << bimodal_mispredictions << "\n";
    std::cerr << "Tagged used:              " << tagged_used << "\n";
    std::cerr << "Tagged mispredictions:    " << tagged_mispredictions << "\n";
    std::cerr << "Longest match correct:    " << longest_match_correct << "\n";
    std::cerr << "Longest match wrong:      " << longest_match_wrong << "\n";
    std::cerr << "Alt used correct:         " << alt_prediction_correct << "\n";
    std::cerr << "Alt used wrong:           " << alt_prediction_wrong << "\n";
    std::cerr << "Weak longest correct:     " << weak_longest_correct << "\n";
    std::cerr << "Weak longest wrong:       " << weak_longest_wrong << "\n";
    std::cerr << "New entries allocated:    " << new_entries_allocated << "\n";
    std::cerr << "High conf correct:        " << confidence_high_correct << "\n";
    std::cerr << "High conf wrong:          " << confidence_high_wrong << "\n";
    std::cerr << "Medium conf correct:      " << confidence_medium_correct << "\n";
    std::cerr << "Medium conf wrong:        " << confidence_medium_wrong << "\n";
    std::cerr << "Low conf correct:         " << confidence_low_correct << "\n";
    std::cerr << "Low conf wrong:           " << confidence_low_wrong << "\n";
    std::cerr << "Bank hit distribution:\n";
    for (int i = 1; i < 45; ++i) {
      if (bank_hits[i] > 0)
        std::cerr << "  Bank " << i << ": " << bank_hits[i] << "\n";
    }
    std::cerr << "=============================================\n";
    std::cerr << std::flush;
  }
};

template <class TAGE_CONFIG>
struct Tage_Prediction_Info {
  bool prediction;
  bool high_confidence;
  bool medium_confidence;
  bool low_confidence;
  bool longest_match_prediction;
  bool alt_prediction;
  bool alt_confidence;
  int hit_bank;
  int alt_bank;
  int indices[2 * TAGE_CONFIG::NUM_HISTORIES + 1];
  int tags[2 * TAGE_CONFIG::NUM_HISTORIES + 1];
  int num_global_history_bits;
  int64_t global_history_head_checkpoint_;
  int64_t path_history_checkpoint;
  int64_t path_history_commit_checkpoint;
  int8_t longest_match_counter_at_pred = 0;
  int8_t alt_match_counter_at_pred = 0;
};

template <class TAGE_CONFIG>
class Tage_Histories {
 public:
  Tage_Histories(int max_in_flight_branches)
      : history_register_(3 * max_in_flight_branches) {
    path_history_ = 0;
    intialize_folded_history();
  }
  void push_into_history(uint64_t br_pc, uint64_t br_target,
                         Branch_Type br_type, bool branch_dir,
                         Tage_Prediction_Info<TAGE_CONFIG>* prediction_info) {
    int num_bit_inserts = 2;
    if (br_type.is_indirect && !br_type.is_conditional) num_bit_inserts = 3;
    int pc_dir_hash = ((br_pc ^ (br_pc >> 2))) ^ branch_dir;
    int path_hash = br_pc ^ (br_pc >> 2) ^ (br_pc >> 4);
    if ((br_type.is_indirect && br_type.is_conditional) & branch_dir) {
      pc_dir_hash = (pc_dir_hash ^ (br_target >> 2));
      path_hash = path_hash ^ (br_target >> 2) ^ (br_target >> 4);
    }
    prediction_info->num_global_history_bits = num_bit_inserts;
    prediction_info->path_history_checkpoint = path_history_;
    prediction_info->global_history_head_checkpoint_ = history_register_.head_idx();
    for (int i = 0; i < num_bit_inserts; ++i) {
      history_register_.push_bit(pc_dir_hash & 1);
      pc_dir_hash >>= 1;
      path_history_ = (path_history_ << 1) ^ (path_hash & 127);
      path_hash >>= 1;
      for (int j = 0; j < TAGE_CONFIG::NUM_HISTORIES; ++j) {
        folded_histories_for_indices_[j].update(history_register_);
        folded_histories_for_tags_0_[j].update(history_register_);
        folded_histories_for_tags_1_[j].update(history_register_);
      }
    }
    path_history_ = path_history_ & ((1 << TAGE_CONFIG::PATH_HISTORY_WIDTH) - 1);
    prediction_info->path_history_commit_checkpoint = path_history_;
  }
  void intialize_folded_history(void);
  int64_t compute_path_hash(int64_t path_history, int max_width, int bank,
                            int index_size) const;
  static constexpr int twice_num_histories_ = 2 * TAGE_CONFIG::NUM_HISTORIES;
  static constexpr Tage_History_Sizes<TAGE_CONFIG> history_sizes_ = {};
  static constexpr Tage_Tag_Bits<TAGE_CONFIG> tag_bits_ = {};
  Long_History_Register<TAGE_CONFIG::MAX_HISTORY_SIZE> history_register_;
  std::vector<Folded_History<TAGE_CONFIG::MAX_HISTORY_SIZE>> folded_histories_for_indices_;
  std::vector<Folded_History<TAGE_CONFIG::MAX_HISTORY_SIZE>> folded_histories_for_tags_0_;
  std::vector<Folded_History<TAGE_CONFIG::MAX_HISTORY_SIZE>> folded_histories_for_tags_1_;
  int64_t path_history_;
  int64_t commit_path_history_;
};

template <class TAGE_CONFIG>
class Tage {
 public:
  Tage(Random_Number_Generator& random_number_gen, int max_in_flight_branches)
      : tagged_table_ptrs_(),
        tage_histories_(max_in_flight_branches),
        low_history_tagged_table_(),
        high_history_tagged_table_(),
        alt_selector_table_(),
        random_number_gen_(random_number_gen) {
    initialize_table_sizes();
    intialize_predictor_state();
  }
  const TageStats& get_stats() const { return stats_; }

  void get_prediction(uint64_t br_pc,
      Tage_Prediction_Info<TAGE_CONFIG>* prediction_info) const {
    fill_table_indices_tags(br_pc, prediction_info);
    auto& indices = prediction_info->indices;
    auto& tags = prediction_info->tags;

    Bimodal_Output bimodal_output = get_bimodal_prediction_confidence(br_pc);
    prediction_info->alt_prediction = bimodal_output.prediction;
    prediction_info->alt_confidence = bimodal_output.confidence;
    prediction_info->high_confidence = prediction_info->alt_confidence;
    prediction_info->medium_confidence = false;
    prediction_info->low_confidence = !prediction_info->high_confidence;
    prediction_info->prediction = prediction_info->alt_prediction;
    prediction_info->longest_match_prediction = prediction_info->alt_prediction;

    Matched_Table_Banks matched_banks =
        get_two_longest_matching_tables(indices, tags);
    prediction_info->hit_bank = matched_banks.hit_bank;
    prediction_info->alt_bank = matched_banks.alt_bank;

    if (prediction_info->hit_bank != 0) {
      int8_t longest_match_counter =
          tagged_table_ptrs_[prediction_info->hit_bank]
                            [indices[prediction_info->hit_bank]]
                                .pred_counter.get();
      prediction_info->longest_match_counter_at_pred = longest_match_counter;
      prediction_info->longest_match_prediction = longest_match_counter >= 0;

      if (prediction_info->alt_bank != 0) {
        int8_t alt_match_counter =
            tagged_table_ptrs_[prediction_info->alt_bank]
                              [indices[prediction_info->alt_bank]]
                                  .pred_counter.get();
        prediction_info->alt_match_counter_at_pred = alt_match_counter;
        prediction_info->alt_prediction = alt_match_counter >= 0;
        prediction_info->alt_confidence = std::abs(2 * alt_match_counter + 1) > 1;
      }

      int alt_selector_table_index =
          (((prediction_info->hit_bank - 1) / 8) << 1) +
          (prediction_info->alt_confidence ? 1 : 0);
      alt_selector_table_index =
          alt_selector_table_index %
          ((1 << TAGE_CONFIG::ALT_SELECTOR_LOG_TABLE_SIZE) - 1);
      bool use_alt = alt_selector_table_[alt_selector_table_index].get() >= 0;
      if ((!use_alt) || std::abs(2 * longest_match_counter + 1) > 1) {
        prediction_info->prediction = prediction_info->longest_match_prediction;
      } else {
        prediction_info->prediction = prediction_info->alt_prediction;
      }

      prediction_info->high_confidence =
          std::abs(2 * longest_match_counter + 1) >=
          ((1 << TAGE_CONFIG::PRED_COUNTER_WIDTH) - 1);
      prediction_info->medium_confidence =
          std::abs(2 * longest_match_counter + 1) == 5;
      prediction_info->low_confidence =
          std::abs(2 * longest_match_counter + 1) == 1;
    }
  }

  void update_speculative_state(
      uint64_t br_pc, uint64_t br_target, Branch_Type br_type,
      bool final_prediction,
      Tage_Prediction_Info<TAGE_CONFIG>* prediction_info) {
    tage_histories_.push_into_history(br_pc, br_target, br_type,
                                      final_prediction, prediction_info);
  }

  void commit_state(uint64_t br_pc, bool resolve_dir,
                    const Tage_Prediction_Info<TAGE_CONFIG>& prediction_info,
                    bool final_prediction,
                    bool count_stats) {
    const int* indices = prediction_info.indices;
    const int* tags = prediction_info.tags;

    tage_histories_.commit_path_history_ =
        prediction_info.path_history_commit_checkpoint;

    bool allocate_new_entry =
        (prediction_info.prediction != resolve_dir) &&
        (prediction_info.hit_bank <
         Tage_Histories<TAGE_CONFIG>::twice_num_histories_);

    if (prediction_info.hit_bank > 0) {
      Tagged_Entry& matched_entry =
          tagged_table_ptrs_[prediction_info.hit_bank]
                            [indices[prediction_info.hit_bank]];
      if (std::abs(2 * matched_entry.pred_counter.get() + 1) <= 1) {
        if (prediction_info.longest_match_prediction == resolve_dir) {
          allocate_new_entry = false;
        }

        if (prediction_info.longest_match_prediction !=
            prediction_info.alt_prediction) {
          int alt_selector_table_index =
              (((prediction_info.hit_bank - 1) / 8) << 1);
          alt_selector_table_index += prediction_info.alt_confidence ? 1 : 0;
          alt_selector_table_index =
              alt_selector_table_index %
              ((1 << TAGE_CONFIG::ALT_SELECTOR_LOG_TABLE_SIZE) - 1);

          alt_selector_table_[alt_selector_table_index].update(
              prediction_info.alt_prediction == resolve_dir);
          if (count_stats && prediction_info.alt_prediction != resolve_dir)
            stats_.alt_selector_mispredictions++;
        }
      }
    }

    if (final_prediction == resolve_dir) {
      if ((random_number_gen_() & 31) != 0) {
        allocate_new_entry = false;
      }
    }

    int num_allocated = 0;
    if (allocate_new_entry) {
      int num_extra_entries_to_allocate = TAGE_CONFIG::EXTRA_ENTRIES_TO_ALLOCATE;
      int tick_penalty = 0;
      int temp_value = 1;
      if ((random_number_gen_() & 127) < 32) temp_value = 2;
      int allocation_bank =
          ((((prediction_info.hit_bank - 1 + 2 * temp_value) & 0xffe)) ^
           (random_number_gen_() & 1));

      for (;
           allocation_bank < Tage_Histories<TAGE_CONFIG>::twice_num_histories_;
           allocation_bank += 2) {
        int i = allocation_bank + 1;
        bool done = false;
        if (tables_enabled_.arr[i]) {
          Tagged_Entry& bank_entry = tagged_table_ptrs_[i][indices[i]];
          if (bank_entry.useful.get() == 0) {
            if (std::abs(2 * bank_entry.pred_counter.get() + 1) <= 3) {
              bank_entry.tag = tags[i];
              bank_entry.pred_counter.set(resolve_dir ? 0 : -1);
              num_allocated += 1;
              if (num_extra_entries_to_allocate <= 0) break;
              allocation_bank += 2;
              done = true;
              num_extra_entries_to_allocate -= 1;
            } else {
              if (bank_entry.pred_counter.get() > 0)
                bank_entry.pred_counter.decrement();
              else
                bank_entry.pred_counter.increment();
            }
          } else {
            tick_penalty += 1;
          }
        }
        if (!done) {
          i = (allocation_bank ^ 1) + 1;
          if (tables_enabled_.arr[i]) {
            Tagged_Entry& bank_entry = tagged_table_ptrs_[i][indices[i]];
            if (bank_entry.useful.get() == 0) {
              if (std::abs(2 * bank_entry.pred_counter.get() + 1) <= 3) {
                bank_entry.tag = tags[i];
                bank_entry.pred_counter.set(resolve_dir ? 0 : -1);
                num_allocated += 1;
                if (num_extra_entries_to_allocate <= 0) break;
                allocation_bank += 2;
                num_extra_entries_to_allocate -= 1;
              } else {
                if (bank_entry.pred_counter.get() > 0)
                  bank_entry.pred_counter.decrement();
                else
                  bank_entry.pred_counter.increment();
              }
            } else {
              tick_penalty += 1;
            }
          }
        }
      }
      tick_ += (tick_penalty - 2 * num_allocated);
      tick_ = std::max(tick_, 0);
      if (tick_ >= TAGE_CONFIG::TICKS_UNTIL_USEFUL_SHIFT) {
        shift_tage_useful_bits(low_history_tagged_table_,
                               TAGE_CONFIG::SHORT_HISTORY_NUM_BANKS *
                                   (1 << TAGE_CONFIG::LOG_ENTRIES_PER_BANK));
        shift_tage_useful_bits(high_history_tagged_table_,
                               TAGE_CONFIG::LONG_HISTORY_NUM_BANKS *
                                   (1 << TAGE_CONFIG::LOG_ENTRIES_PER_BANK));
        tick_ = 0;
      }
    }
    if (count_stats) stats_.new_entries_allocated += num_allocated;

    // Update prediction counters
    if (prediction_info.hit_bank > 0) {
      Tagged_Entry& matched_entry =
          tagged_table_ptrs_[prediction_info.hit_bank]
                            [indices[prediction_info.hit_bank]];
      if (std::abs(2 * matched_entry.pred_counter.get() + 1) == 1) {
        if (prediction_info.longest_match_prediction !=
            resolve_dir) {
          if (prediction_info.alt_bank > 0) {
            Tagged_Entry& alt_matched_entry =
                tagged_table_ptrs_[prediction_info.alt_bank]
                                  [indices[prediction_info.alt_bank]];
            alt_matched_entry.pred_counter.update(resolve_dir);
          } else {
            update_bimodal(br_pc, resolve_dir);
          }
        }
      }
      matched_entry.pred_counter.update(resolve_dir);
      if (std::abs(2 * matched_entry.pred_counter.get() + 1) == 1)
        matched_entry.useful.set(0);
      if (prediction_info.alt_prediction == resolve_dir &&
          prediction_info.alt_bank > 0) {
        Tagged_Entry& alt_matched_entry =
            tagged_table_ptrs_[prediction_info.alt_bank]
                              [indices[prediction_info.alt_bank]];
        if (std::abs(2 * alt_matched_entry.pred_counter.get() + 1) == 7 &&
            matched_entry.useful.get() == 1 &&
            prediction_info.longest_match_prediction == resolve_dir) {
          matched_entry.useful.set(0);
        }
      }
    } else {
      update_bimodal(br_pc, resolve_dir);
    }

    if (prediction_info.longest_match_prediction !=
            prediction_info.alt_prediction &&
        prediction_info.longest_match_prediction == resolve_dir) {
      Tagged_Entry& matched_entry =
          tagged_table_ptrs_[prediction_info.hit_bank]
                            [indices[prediction_info.hit_bank]];
      matched_entry.useful.increment();
    }

    // ---- Statistics (only for conditional branches) ----
    if (count_stats) {
      stats_.commits++;
      stats_.total_predictions++;
      bool correct = (final_prediction == resolve_dir);
      if (!correct) stats_.total_mispredictions++;

      if (prediction_info.hit_bank == 0) {
        stats_.bimodal_used++;
        if (!correct) stats_.bimodal_mispredictions++;
      } else {
        stats_.tagged_used++;
        stats_.bank_hits[prediction_info.hit_bank]++;
        if (prediction_info.alt_bank != 0)
          stats_.bank_hits[prediction_info.alt_bank]++;

        if (!correct) stats_.tagged_mispredictions++;

        if (prediction_info.longest_match_prediction == resolve_dir)
          stats_.longest_match_correct++;
        else
          stats_.longest_match_wrong++;

        if (std::abs(2 * prediction_info.longest_match_counter_at_pred + 1) <= 1) {
          if (prediction_info.longest_match_prediction == resolve_dir)
            stats_.weak_longest_correct++;
          else
            stats_.weak_longest_wrong++;
        }

        bool used_alt = (prediction_info.prediction != prediction_info.longest_match_prediction);
        if (used_alt) {
          stats_.alt_prediction_used++;
          if (prediction_info.prediction == resolve_dir)
            stats_.alt_prediction_correct++;
          else
            stats_.alt_prediction_wrong++;
        }

        if (prediction_info.high_confidence) {
          if (correct) stats_.confidence_high_correct++;
          else stats_.confidence_high_wrong++;
        } else if (prediction_info.medium_confidence) {
          if (correct) stats_.confidence_medium_correct++;
          else stats_.confidence_medium_wrong++;
        } else {
          if (correct) stats_.confidence_low_correct++;
          else stats_.confidence_low_wrong++;
        }
      }
    }
  }

  void commit_state_at_retire(
      const Tage_Prediction_Info<TAGE_CONFIG>& prediction_info) {
    tage_histories_.history_register_.retire(
        prediction_info.num_global_history_bits);
  }

  void global_recover_speculative_state(
      const Tage_Prediction_Info<TAGE_CONFIG>& prediction_info) {
    int64_t num_flushed_bits =
        (prediction_info.global_history_head_checkpoint_ -
         tage_histories_.history_register_.head_idx());
    stats_.history_rewinds++;
    for (int i = 0; i < num_flushed_bits; ++i) {
      for (int j = 0; j < TAGE_CONFIG::NUM_HISTORIES; ++j) {
        tage_histories_.folded_histories_for_indices_[j].update_reverse(
            tage_histories_.history_register_);
        tage_histories_.folded_histories_for_tags_0_[j].update_reverse(
            tage_histories_.history_register_);
        tage_histories_.folded_histories_for_tags_1_[j].update_reverse(
            tage_histories_.history_register_);
      }
      tage_histories_.history_register_.rewind(1);
    }
    tage_histories_.path_history_ = prediction_info.path_history_checkpoint;
  }

  void local_recover_speculative_state(
      const Tage_Prediction_Info<TAGE_CONFIG>& prediction_info) {}

  static void build_empty_prediction(
      Tage_Prediction_Info<TAGE_CONFIG>* prediction_info) {
    *prediction_info = {};
  }

 private:
  struct Bimodal_Entry {
    int8_t hysteresis = 1;
    int8_t prediction = 0;
  };
  struct Tagged_Entry {
    Saturating_Counter<TAGE_CONFIG::PRED_COUNTER_WIDTH, true> pred_counter;
    Saturating_Counter<TAGE_CONFIG::USEFUL_BITS, false> useful;
    int tag = 0;
    Tagged_Entry() : pred_counter(0), useful(0) {}
  };
  void initialize_tag_bits(void);
  void initialize_table_sizes(void);
  void intialize_predictor_state(void);
  void fill_table_indices_tags(
      uint64_t br_pc, Tage_Prediction_Info<TAGE_CONFIG>* tage_output) const;
  Bimodal_Output get_bimodal_prediction_confidence(uint64_t br_pc) const;
  void update_bimodal(uint64_t br_pc, bool resolve_dir);
  Matched_Table_Banks get_two_longest_matching_tables(int indices[],
                                                      int tags[]) const;
  void shift_tage_useful_bits(Tagged_Entry* table, int size);

  static constexpr Tage_Tables_Enabled<TAGE_CONFIG> tables_enabled_ = {};
  Tagged_Entry*
      tagged_table_ptrs_[Tage_Histories<TAGE_CONFIG>::twice_num_histories_ + 1];
  Tage_Histories<TAGE_CONFIG> tage_histories_;
  Bimodal_Entry bimodal_table_[1 << TAGE_CONFIG::BIMODAL_LOG_TABLES_SIZE];
  Tagged_Entry
      low_history_tagged_table_[TAGE_CONFIG::SHORT_HISTORY_NUM_BANKS *
                                (1 << TAGE_CONFIG::LOG_ENTRIES_PER_BANK)];
  Tagged_Entry
      high_history_tagged_table_[TAGE_CONFIG::LONG_HISTORY_NUM_BANKS *
                                 (1 << TAGE_CONFIG::LOG_ENTRIES_PER_BANK)];
  Saturating_Counter<TAGE_CONFIG::ALT_SELECTOR_ENTRY_WIDTH, true>
      alt_selector_table_[1 << TAGE_CONFIG::ALT_SELECTOR_LOG_TABLE_SIZE];
  int tick_;
  Random_Number_Generator& random_number_gen_;
  mutable TageStats stats_;
};

template <class TAGE_CONFIG>
constexpr Tage_History_Sizes<TAGE_CONFIG>
    Tage_Histories<TAGE_CONFIG>::history_sizes_;

template <class TAGE_CONFIG>
constexpr Tage_Tables_Enabled<TAGE_CONFIG> Tage<TAGE_CONFIG>::tables_enabled_;

template <class TAGE_CONFIG>
constexpr Tage_Tag_Bits<TAGE_CONFIG> Tage_Histories<TAGE_CONFIG>::tag_bits_;

template <class TAGE_CONFIG>
void Tage<TAGE_CONFIG>::initialize_table_sizes(void) {
  for (int i = 1; i < TAGE_CONFIG::FIRST_LONG_HISTORY_TABLE; ++i)
    tagged_table_ptrs_[i] = low_history_tagged_table_;
  for (int i = TAGE_CONFIG::FIRST_LONG_HISTORY_TABLE;
       i <= Tage_Histories<TAGE_CONFIG>::twice_num_histories_; ++i)
    tagged_table_ptrs_[i] = high_history_tagged_table_;
}

template <class TAGE_CONFIG>
void Tage_Histories<TAGE_CONFIG>::intialize_folded_history(void) {
  for (int i = 0; i < TAGE_CONFIG::NUM_HISTORIES; i++) {
    const int LOG_ENTRIES_PER_BANK2 = TAGE_CONFIG::LOG_ENTRIES_PER_BANK;
    folded_histories_for_indices_.emplace_back(history_sizes_.arr[i], LOG_ENTRIES_PER_BANK2);
    folded_histories_for_tags_0_.emplace_back(history_sizes_.arr[i], tag_bits_.arr[i]);
    folded_histories_for_tags_1_.emplace_back(history_sizes_.arr[i], tag_bits_.arr[i] - 1);
  }
}

template <class TAGE_CONFIG>
void Tage<TAGE_CONFIG>::intialize_predictor_state(void) {
  tick_ = 0;
  random_number_gen_.phist_ptr_ = &tage_histories_.commit_path_history_;
  random_number_gen_.ptghist_ptr_ = &tage_histories_.history_register_.commit_head_idx();
}

template <class TAGE_CONFIG>
int64_t Tage_Histories<TAGE_CONFIG>::compute_path_hash(int64_t path_history,
                                                       int max_width, int bank,
                                                       int index_size) const {
  int64_t temp1, temp2;
  path_history = (path_history & ((1 << max_width) - 1));
  temp1 = (path_history & ((1 << index_size) - 1));
  temp2 = (path_history >> index_size);
  if (bank < index_size) {
    temp2 = ((temp2 << bank) & ((1 << index_size) - 1)) +
            (temp2 >> (index_size - bank));
  }
  path_history = temp1 ^ temp2;
  if (bank < index_size) {
    path_history = ((path_history << bank) & ((1 << index_size) - 1)) +
                   (path_history >> (index_size - bank));
  }
  return path_history;
}

template <class TAGE_CONFIG>
void Tage<TAGE_CONFIG>::fill_table_indices_tags(
    uint64_t br_pc, Tage_Prediction_Info<TAGE_CONFIG>* output) const {
  for (int i = 1; i <= Tage_Histories<TAGE_CONFIG>::twice_num_histories_;
       i += 2) {
    if (tables_enabled_.arr[i] || tables_enabled_.arr[i + 1]) {
      int max_path_width =
          (tage_histories_.history_sizes_.arr[(i - 1) / 2] >
           TAGE_CONFIG::PATH_HISTORY_WIDTH)
              ? TAGE_CONFIG::PATH_HISTORY_WIDTH
              : tage_histories_.history_sizes_.arr[(i - 1) / 2];
      int64_t path_hash = tage_histories_.compute_path_hash(
          tage_histories_.path_history_, max_path_width, i,
          TAGE_CONFIG::LOG_ENTRIES_PER_BANK);
      int64_t index = br_pc;
      index ^= br_pc >> (std::abs(TAGE_CONFIG::LOG_ENTRIES_PER_BANK - i) + 1);
      index ^= tage_histories_.folded_histories_for_indices_[(i - 1) / 2]
                   .get_value();
      index ^= path_hash;
      output->indices[i] =
          index & ((1 << TAGE_CONFIG::LOG_ENTRIES_PER_BANK) - 1);

      int64_t tag = br_pc;
      tag ^=
          tage_histories_.folded_histories_for_tags_0_[(i - 1) / 2].get_value();
      tag ^=
          tage_histories_.folded_histories_for_tags_1_[(i - 1) / 2].get_value()
          << 1;
      output->tags[i] =
          tag & ((1 << tage_histories_.tag_bits_.arr[(i - 1) / 2]) - 1);

      output->tags[i + 1] = output->tags[i];
      output->indices[i + 1] =
          output->indices[i] ^
          (output->tags[i] & ((1 << TAGE_CONFIG::LOG_ENTRIES_PER_BANK) - 1));
    }
  }
  int temp = (br_pc ^
              (tage_histories_.path_history_ &
               ((int64_t(1)
                 << tage_histories_.history_sizes_
                        .arr[(TAGE_CONFIG::FIRST_LONG_HISTORY_TABLE - 1) / 2]) - 1))) %
             TAGE_CONFIG::LONG_HISTORY_NUM_BANKS;
  for (int i = TAGE_CONFIG::FIRST_LONG_HISTORY_TABLE;
       i <= Tage_Histories<TAGE_CONFIG>::twice_num_histories_; ++i) {
    if (tables_enabled_.arr[i]) {
      output->indices[i] += (temp << TAGE_CONFIG::LOG_ENTRIES_PER_BANK);
      temp++;
      temp = temp % TAGE_CONFIG::LONG_HISTORY_NUM_BANKS;
    }
  }
  temp = (br_pc ^ (tage_histories_.path_history_ &
                   ((1 << tage_histories_.history_sizes_.arr[0]) - 1))) %
         TAGE_CONFIG::SHORT_HISTORY_NUM_BANKS;
  for (int i = 1; i <= TAGE_CONFIG::FIRST_LONG_HISTORY_TABLE - 1; ++i) {
    if (tables_enabled_.arr[i]) {
      output->indices[i] += (temp << TAGE_CONFIG::LOG_ENTRIES_PER_BANK);
      temp++;
      temp = temp % TAGE_CONFIG::SHORT_HISTORY_NUM_BANKS;
    }
  }
}

template <class TAGE_CONFIG>
Bimodal_Output Tage<TAGE_CONFIG>::get_bimodal_prediction_confidence(
    uint64_t br_pc) const {
  Bimodal_Output output;
  int index = (br_pc ^ (br_pc >> 2)) &
              ((1 << TAGE_CONFIG::BIMODAL_LOG_TABLES_SIZE) - 1);
  int8_t bimodal_output =
      (bimodal_table_[index].prediction << 1) +
      (bimodal_table_[index >> TAGE_CONFIG::BIMODAL_HYSTERESIS_SHIFT]
           .hysteresis);
  output.prediction = bimodal_table_[index].prediction > 0;
  output.confidence = (bimodal_output == 0) || (bimodal_output == 3);
  return output;
}

template <class TAGE_CONFIG>
void Tage<TAGE_CONFIG>::update_bimodal(uint64_t br_pc, bool resolve_dir) {
  int index = (br_pc ^ (br_pc >> 2)) &
              ((1 << TAGE_CONFIG::BIMODAL_LOG_TABLES_SIZE) - 1);
  int8_t bimodal_output =
      (bimodal_table_[index].prediction << 1) +
      (bimodal_table_[index >> TAGE_CONFIG::BIMODAL_HYSTERESIS_SHIFT]
           .hysteresis);
  if (resolve_dir && bimodal_output < 3) {
    bimodal_output += 1;
  } else if (!resolve_dir && bimodal_output > 0) {
    bimodal_output -= 1;
  }
  bimodal_table_[index].prediction = bimodal_output >> 1;
  bimodal_table_[index >> TAGE_CONFIG::BIMODAL_HYSTERESIS_SHIFT].hysteresis =
      (bimodal_output & 1);
}

template <class TAGE_CONFIG>
Matched_Table_Banks Tage<TAGE_CONFIG>::get_two_longest_matching_tables(
    int indices[], int tags[]) const {
  int first_match = 0;
  int second_match = 0;
  for (int i = 2 * TAGE_CONFIG::NUM_HISTORIES; i > 0; --i) {
    if (tables_enabled_.arr[i]) {
      if (tagged_table_ptrs_[i][indices[i]].tag == tags[i]) {
        if (first_match == 0) {
          first_match = i;
        } else {
          second_match = i;
          break;
        }
      }
    }
  }
  return Matched_Table_Banks{first_match, second_match};
}

template <class TAGE_CONFIG>
void Tage<TAGE_CONFIG>::shift_tage_useful_bits(Tagged_Entry* table, int size) {
  for (int i = 0; i < size; ++i) {
    table[i].useful.set(table[i].useful.get() >> 1);
  }
}

}  // namespace tagescl

#endif  // SPEC_TAGE_SC_L_TAGE_HPP_