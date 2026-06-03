/* Copyright 2020 HPS/SAFARI Research Groups ... (keep original header) */

#ifndef SPEC_TAGE_SC_L_UTILS_HPP_
#define SPEC_TAGE_SC_L_UTILS_HPP_

#include <cassert>

namespace tagescl {

inline int get_min_num_bits_to_represent(int x) {
  assert(x > 0);
  int num_bits = 1;
  while (true) {
    if (1 << num_bits >= x) return num_bits;
    num_bits += 1;
  }
  assert(false);
}

template <bool B, class T, class F>
struct conditional_type { typedef T type; };

template <class T, class F>
struct conditional_type<false, T, F> { typedef F type; };

template <int width>
struct Smallest_Int_Type {
  using type = typename conditional_type<
      (width < 8), int8_t,
      typename conditional_type<
          (width < 16), int16_t,
          typename conditional_type<(width < 32), int32_t,
                                    int64_t>::type>::type>::type;
};

template <int width, bool is_signed>
class Saturating_Counter {
 public:
  using Int_Type = typename Smallest_Int_Type<width>::type;
  Saturating_Counter() { set(0); }
  Saturating_Counter(Int_Type init_value) { set(init_value); }
  Int_Type get() const { return counter_; }
  void update(bool condition) { if (condition) increment(); else decrement(); }
  void increment(void) { if (counter_ < counter_max_) counter_ += 1; }
  void decrement(void) { if (counter_ > counter_min_) counter_ -= 1; }
  void set(Int_Type value) {
    assert(counter_min_ <= value && value <= counter_max_);
    counter_ = value;
  }
 private:
  static constexpr Int_Type counter_max_ =
      (is_signed ? ((1 << (width - 1)) - 1) : ((1 << width) - 1));
  static constexpr Int_Type counter_min_ =
      (is_signed ? -(1 << (width - 1)) : 0);
  Int_Type counter_;
};

class Random_Number_Generator {
 public:
  int operator()() {
    assert(phist_ptr_);
    assert(ptghist_ptr_);
    seed_++;
    seed_ ^= (*phist_ptr_);
    seed_ = (seed_ >> 21) + (seed_ << 11);
    seed_ ^= (int)(*ptghist_ptr_);
    seed_ = (seed_ >> 10) + (seed_ << 22);
    return (seed_);
  }
  int seed_ = 0;
  const int64_t* phist_ptr_;
  const int64_t* ptghist_ptr_;
};

struct Branch_Type {
  bool is_conditional;
  bool is_indirect;
};

template <typename T>
class Circular_Buffer {
 public:
  Circular_Buffer(unsigned max_size)
      : buffer_(1 << get_min_num_bits_to_represent(max_size)),
        buffer_access_mask_(buffer_.size() - 1),
        back_(-1), front_(-1), size_(0) {}
  T& operator[](uint32_t id) {
    assert(back_ - id < back_ - front_);
    return buffer_[id & buffer_access_mask_];
  }
  uint32_t back_id() const { return back_; }
  void deallocate_after(uint32_t id) {
    assert(back_ - id < back_ - front_);
    size_ -= (back_ - id);
    back_ = id;
  }
  void deallocate_and_after(uint32_t id) {
    assert((back_ - id + 1) < (back_ - front_ + 1));
    size_ -= (back_ - id + 1);
    back_ = id - 1;
  }
  uint32_t allocate_back() {
    assert(size_ < buffer_.size());
    back_ += 1;
    size_ += 1;
    return back_;
  }
  void deallocate_front(uint32_t pop_id) {
    front_ += 1;
    assert(pop_id == front_);
    assert(size_ > 0);
    size_ -= 1;
  }
 private:
  std::vector<T> buffer_;
  uint32_t buffer_access_mask_;
  uint32_t back_;
  uint32_t front_;
  uint32_t size_;
};

}  // namespace tagescl

#endif  // SPEC_TAGE_SC_L_UTILS_HPP_