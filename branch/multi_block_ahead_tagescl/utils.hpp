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

#ifndef SPEC_TAGE_SC_L_UTILS_HPP_
#define SPEC_TAGE_SC_L_UTILS_HPP_

#include <cassert>
#include <stdio.h>


namespace tagescl {

inline int get_min_num_bits_to_represent(int x) {
  assert(x > 0);
  int num_bits = 1;
  while (true) {
    if (1 << num_bits >= x) {
      return num_bits;
    }

    num_bits += 1;
  }
  assert(false);
}

/* Copying the implementation of std::conditional because PinCRT does not
 * include it*/
template <bool B, class T, class F>
struct conditional_type {
  typedef T type;
};

template <class T, class F>
struct conditional_type<false, T, F> {
  typedef F type;
};

template <int width>
struct Smallest_Int_Type {
  // This is logically equivalent to the following code assuming you could use
  // if statements for conditional type declarations:
  // if (width < 8) using type = int8_t;
  // else if (width < 16) using type = int16_t;
  // else if (width < 32) using type = int32_t;
  // else using type = int64_t;
  using type = typename conditional_type<
      (width < 8), int8_t,
      typename conditional_type<
          (width < 16), int16_t,
          typename conditional_type<(width < 32), int32_t,
                                    int64_t>::type>::type>::type;
};

/* Saturating counters: Could be signed or unsigned. Can be
 * directly updated using increment() or decrement(). Alternatively, one can
 * call upodate with a boolean indicting the direction.  */
template <int width, bool is_signed>
class Saturating_Counter {
 public:
  using Int_Type = typename Smallest_Int_Type<width>::type;

  Saturating_Counter() { set(0); }
  Saturating_Counter(Int_Type init_value) { set(init_value); }

  // Returns the current value of the counter.
  Int_Type get() const { return counter_; }

  // If condition is true, increments the counter, otherwise, decrements the
  // counter.
  void update(bool condition) {
    if (condition) {
      increment();
    } else {
      decrement();
    }
  }

  // Increments the counter (saturating).
  void increment(void) {
    if (counter_ < counter_max_) {
      counter_ += 1;
    }
  }

  // Decrements the counter (saturating).
  void decrement(void) {
    if (counter_ > counter_min_) {
      counter_ -= 1;
    }
  }

  // Sets the counter to value (value should be within the minimum and
  // maximum values).
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

// This is an ugly way to do random number generation, but I want to keep it
// compatible with Seznec for now.
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
class CircularBuffer {
public:
    CircularBuffer(size_t inflight_branches,
                      size_t multi_block_ahead_distance)
        : capacity_(inflight_branches + multi_block_ahead_distance),
          buffer_(capacity_),
        //valid_(capacity_, false),
          read_id_(0),
          alloc_id_(multi_block_ahead_distance)
    {
        assert(capacity_ > 0);
    }

    size_t get_capacity() const { return capacity_; }

    uint32_t get_read_id() const { return read_id_; }

    uint32_t get_alloc_id() const { return alloc_id_; }



    bool contains(uint32_t id) const {
        return (id >= read_id_) && (id <= alloc_id_);
    }


    T& operator[](uint32_t id) {
      assert(contains(id));
      size_t idx = physical_index(id);
      return buffer_[idx];    
    }


    void clear(uint32_t id) {
      assert(contains(id));
      size_t idx = physical_index(id);

      // Reset the stored object using its default constructor
      buffer_[idx] = T{};
    }

    void deallocate_front(uint32_t pop_id) {
      assert(pop_id == read_id_);
      clear(pop_id);
      read_id_++;
      alloc_id_++;
    }


private:
    size_t physical_index(uint32_t id) const
    {
        return static_cast<size_t>(id % capacity_);
    }

private:
    size_t capacity_;

    std::vector<T> buffer_;

    uint32_t read_id_;        // Oldest valid branch
    uint32_t alloc_id_;  // Next branch ID to allocate
};



}  // namespace tagescl

#endif  // SPEC_TAGE_SC_L_UTILS_HPP_