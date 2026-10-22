#pragma once

#include "common.hpp"

namespace cocogfx {

template <uint32_t F, typename T = int32_t>
class TFixed {
private:

  template <uint32_t F2, typename T2> 
  struct Cast {
  private:
    template <bool isF2Bigger, bool isT2Bigger> struct Tag {};

    inline static T Convert(T2 value, Tag<false, false>) {
      return static_cast<T>(value) << (F - F2);
    }

    inline static T Convert(T2 value, Tag<false, true>) {
      return static_cast<T>(value) >> (F2 - F);
    }

    inline static T Convert(T2 value, Tag<true, false>) {
      return static_cast<T>(value << (F - F2));
    }

    inline static T Convert(T2 value, Tag<true, true>) {
      return static_cast<T>(value >> (F2 - F));
    }

  public:    
    inline static T Convert(T2 value) {
      return Convert(value, Tag<(sizeof(T2) > sizeof(T)), (F2 > F)>{});
    }  
  };

public:
  using data_type = T;

  static constexpr uint32_t FRAC = F;
  static constexpr uint32_t INT = sizeof(T) * 8 - FRAC;
  static constexpr uint32_t HFRAC = FRAC >> 1;
  static constexpr T ONE = static_cast<T>(1) << FRAC;
  static constexpr T MASK = ONE - 1;
  static constexpr T IMASK = ~MASK;
  static constexpr T HALF = ONE >> 1;
  static constexpr T TWO = ONE << 1;

  TFixed() {}

  explicit TFixed(int64_t rhs)
      : data_(static_cast<T>(rhs << FRAC)) {
    assert((static_cast<int64_t>(rhs) << FRAC) == data_);
  }

  explicit TFixed(uint64_t rhs)
      : data_(static_cast<T>(rhs << FRAC)) {
    assert((static_cast<int64_t>(rhs) << FRAC) == data_);
  }

  explicit TFixed(int32_t rhs)
      : data_(static_cast<T>(rhs << FRAC)) {
    assert((static_cast<int64_t>(rhs) << FRAC) == data_);
  }

  explicit TFixed(uint32_t rhs)
      : data_(static_cast<T>(rhs << FRAC)) {
    assert((static_cast<int64_t>(rhs) << FRAC) == data_);
  }

  explicit TFixed(int16_t rhs)
      : data_(static_cast<T>(rhs << FRAC)) {
    assert((static_cast<int64_t>(rhs) << FRAC) == data_);
  }

  explicit TFixed(uint16_t rhs)
      : data_(static_cast<T>(rhs << FRAC)) {
    assert((static_cast<int64_t>(rhs) << FRAC) == data_);
  }

  explicit TFixed(int8_t rhs)
      : data_(static_cast<T>(rhs << FRAC)) {
    assert((static_cast<int64_t>(rhs) << FRAC) == data_);
  }

  explicit TFixed(uint8_t rhs)
      : data_(static_cast<T>(rhs << FRAC)) {
    assert((static_cast<int64_t>(rhs) << FRAC) == data_);
  }

  template <uint32_t F2, typename T2>
  explicit TFixed(TFixed<F2, T2> rhs)
    : data_(Cast<F2, T2>::Convert(rhs.data()))
  {}

  explicit TFixed(float rhs)
      : data_(static_cast<T>(rhs * ONE)) {
    assert(data_ == static_cast<T>(rhs * ONE));
  }

  bool operator==(TFixed rhs) const {
    return (data_ == rhs.data_);
  }

  bool operator!=(TFixed rhs) const {
    return (data_ != rhs.data_);
  }

  bool operator<(TFixed rhs) const {
    return (data_ < rhs.data_);
  }

  bool operator<=(TFixed rhs) const {
    return (data_ <= rhs.data_);
  }

  bool operator>(TFixed rhs) const {
    return (data_ > rhs.data_);
  }

  bool operator>=(TFixed rhs) const {
    return (data_ >= rhs.data_);
  }

  TFixed operator-() const {
    return make(-data_);
  }

  TFixed operator+=(TFixed rhs) {
    *this = (*this) + rhs;
    return *this;
  }

  TFixed operator-=(TFixed rhs) {
    *this = (*this) - rhs;
    return *this;
  }

  TFixed operator*=(TFixed rhs) {
    *this = (*this) * rhs;
    return *this;
  }

  TFixed operator/=(TFixed rhs) {
    *this = (*this) / rhs;
    return *this;
  }

  template <uint32_t F2, typename T2>
  TFixed operator*=(TFixed<F2, T2> rhs) {
    *this = (*this) * rhs;
    return *this;
  }

  template <uint32_t F2, typename T2>
  TFixed operator/=(TFixed<F2, T2> rhs) {
    *this = (*this) / rhs;
    return *this;
  }

  TFixed operator*=(int32_t rhs) {
    *this = (*this) * rhs;
    return *this;
  }

  TFixed operator*=(uint32_t rhs) {
    *this = (*this) * rhs;
    return *this;
  }

  TFixed operator*=(float rhs) {
    *this = (*this) * rhs;
    return *this;
  }

  TFixed operator/=(int32_t rhs) {
    *this = (*this) / rhs;
    return *this;
  }

  TFixed operator/=(uint32_t rhs) {
    *this = (*this) / rhs;
    return *this;
  }

  TFixed operator/=(float rhs) {
    *this = (*this) / rhs;
    return *this;
  }

  friend TFixed operator+(TFixed lhs, TFixed rhs) {
    assert((static_cast<int64_t>(lhs.data_) + rhs.data_) == (lhs.data_ + rhs.data_));
    return TFixed::make(lhs.data_ + rhs.data_);
  }

  friend TFixed operator-(TFixed lhs, TFixed rhs) {
    assert((static_cast<int64_t>(lhs.data_) - rhs.data_) == (lhs.data_ - rhs.data_));
    return TFixed::make(lhs.data_ - rhs.data_);
  }

  friend TFixed operator*(TFixed lhs, TFixed rhs) {
    return TFixed::make((static_cast<int64_t>(lhs.data_) * rhs.data_) >> FRAC);
  }

  template <uint32_t F2, typename T2>
  friend TFixed operator*(TFixed lhs, TFixed<F2, T2> rhs) {
    return TFixed::make((static_cast<int64_t>(lhs.data_) * rhs.data()) >> F2);
  }

  friend TFixed operator/(TFixed lhs, TFixed rhs) {
    assert(rhs.data_ != 0);
    return TFixed::make((static_cast<int64_t>(lhs.data_) << FRAC) / rhs.data_);
  }

  template <uint32_t F2, typename T2>
  friend TFixed operator/(TFixed lhs, TFixed<F2, T2> rhs) {
    assert(rhs.data() != 0);
    return TFixed::make((static_cast<int64_t>(lhs.data_) << F2) / rhs.data());
  }

  friend float operator*(TFixed lhs, float rhs) {
    return static_cast<float>(lhs) * rhs;
  }

  friend float operator*(float lhs, TFixed rhs) {
    return lhs * static_cast<float>(rhs);
  }

  friend float operator/(TFixed lhs, float rhs) {
    return static_cast<float>(lhs) / rhs;
  }

  friend float operator/(float lhs, TFixed rhs) {
    return lhs / static_cast<float>(rhs);
  }

  friend TFixed operator*(TFixed lhs, char rhs) {
    return lhs * static_cast<int32_t>(rhs);
  }

  friend TFixed operator*(char lhs, TFixed rhs) {
    return rhs * lhs;
  }

  friend TFixed operator/(TFixed lhs, char rhs) {
    return lhs / static_cast<int32_t>(rhs);
  }

  friend TFixed operator/(char lhs, TFixed rhs) {
    return static_cast<int32_t>(lhs) / rhs;
  }

  friend TFixed operator*(TFixed lhs, uint8_t rhs) {
    return lhs * static_cast<int32_t>(rhs);
  }

  friend TFixed operator*(uint8_t lhs, TFixed rhs) {
    return rhs * lhs;
  }

  friend TFixed operator/(TFixed lhs, uint8_t rhs) {
    return lhs / static_cast<int32_t>(rhs);
  }

  friend TFixed operator/(uint8_t lhs, TFixed rhs) {
    return static_cast<int32_t>(lhs) / rhs;
  }

  friend TFixed operator*(TFixed lhs, short rhs) {
    return lhs * static_cast<int32_t>(rhs);
  }

  friend TFixed operator*(short lhs, TFixed rhs) {
    return rhs * lhs;
  }

  friend TFixed operator/(TFixed lhs, short rhs) {
    return lhs / static_cast<int32_t>(rhs);
  }

  friend TFixed operator/(short lhs, TFixed rhs) {
    return static_cast<int32_t>(lhs) / rhs;
  }

  friend TFixed operator*(TFixed lhs, uint16_t rhs) {
    return lhs * static_cast<int32_t>(rhs);
  }

  friend TFixed operator*(uint16_t lhs, TFixed rhs) {
    return rhs * lhs;
  }

  friend TFixed operator/(TFixed lhs, uint16_t rhs) {
    return lhs / static_cast<int32_t>(rhs);
  }

  friend TFixed operator/(uint16_t lhs, TFixed rhs) {
    return static_cast<int32_t>(lhs) / rhs;
  }

  friend TFixed operator*(TFixed lhs, int32_t rhs) {
    auto value = static_cast<T>(lhs.data_ * rhs);
    assert((lhs.data_ * static_cast<int64_t>(rhs)) == value);
    return TFixed::make(value);
  }

  friend TFixed operator*(int32_t lhs, TFixed rhs) {
    return rhs * lhs;
  }

  friend TFixed operator/(TFixed lhs, int32_t rhs) {
    assert(rhs);
    auto value = static_cast<T>(lhs.data_ / rhs);
    return TFixed::make(value);
  }

  friend TFixed operator/(int32_t lhs, TFixed rhs) {
    assert(rhs.data_);
    auto value = static_cast<T>((static_cast<int64_t>(lhs) << (2 * TFixed::FRAC)) / rhs.data_);
    return TFixed::make(value);
  }

  friend TFixed operator*(TFixed lhs, uint32_t rhs) {
    auto value = static_cast<T>(lhs.data_ * rhs);
    assert((lhs.data_ * static_cast<int64_t>(rhs)) == value);
    return TFixed::make(value);
  }

  friend TFixed operator*(uint32_t lhs, TFixed rhs) {
    return rhs * lhs;
  }

  friend TFixed operator/(TFixed lhs, uint32_t rhs) {
    assert(rhs);
    auto value = static_cast<T>(lhs.data_ / rhs);
    return TFixed::make(value);
  }

  friend TFixed operator/(uint32_t lhs, TFixed rhs) {
    assert(rhs.data_);
    auto value = static_cast<T>((static_cast<int64_t>(lhs) << (2 * TFixed::FRAC)) / rhs.data_);
    return TFixed::make(value);
  }

  friend TFixed operator<<(TFixed lhs, int32_t rhs) {
    auto value = static_cast<T>(lhs.data_ << rhs);
    assert((lhs.data_ << static_cast<int64_t>(rhs)) == value);
    return TFixed::make(value);
  }

  friend TFixed operator>>(TFixed lhs, int32_t rhs) {
    auto value = static_cast<T>(lhs.data_ >> rhs);
    return TFixed::make(value);
  }

  friend TFixed operator<<(TFixed lhs, uint32_t rhs) {
    auto value = static_cast<T>(lhs.data_ << rhs);
    assert((lhs.data_ << static_cast<int64_t>(rhs)) == value);
    return TFixed::make(value);
  }

  friend TFixed operator>>(TFixed lhs, uint32_t rhs) {
    auto value = static_cast<T>(lhs.data_ >> rhs);
    return TFixed::make(value);
  }

  static TFixed make(T value) {
    TFixed ret;
    ret.data_ = value;
    return ret;
  }

  explicit operator int64_t() const {
    return static_cast<int64_t>(data_ >> F);
  }

  explicit operator uint64_t() const {
    return static_cast<uint64_t>(data_ >> F);
  }

  explicit operator int32_t() const {
    return static_cast<int32_t>(data_ >> F);
  }

  explicit operator uint32_t() const {
    return static_cast<uint32_t>(data_ >> F);
  }

  explicit operator int16_t() const {
    return static_cast<int16_t>(data_ >> F);
  }

  explicit operator uint16_t() const {
    return static_cast<uint16_t>(data_ >> F);
  }

  explicit operator int8_t() const {
    return static_cast<int8_t>(data_ >> F);
  }

  explicit operator uint8_t() const {
    return static_cast<uint8_t>(data_ >> F);
  }

  template <uint32_t F2, typename T2>
  explicit operator TFixed<F2, T2>() const {
    return TFixed<F2, T2>(*this);
  }

  explicit operator float() const {
    return static_cast<float>(data_) / (static_cast<T>(1) << F);
  }

  T data() const {
    return data_;
  }

private:
  T data_;
};

}