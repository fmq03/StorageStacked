//
// Copyright (c) Blaise Tine.  All rights reserved.
//
//
// Use of this sample source code is subject to the terms of the Microsoft
// license agreement under which you licensed this sample source code. If
// you did not accept the terms of the license agreement, you are not
// authorized to use this sample source code. For the terms of the license,
// please see the license agreement between you and Microsoft or, if applicable,
// see the LICENSE.RTF on your install media or the root of your tools
// installation.
// THE SAMPLE SOURCE CODE IS PROVIDED "AS IS", WITH NO WARRANTIES OR
// INDEMNITIES.
//
#pragma once

#include "common.hpp"
#include "fixed.hpp"
#include <algorithm>
#include <math.h>

namespace cocogfx {

template <typename T> T Zero() { return static_cast<T>(0); }

template <typename T> T Half() { return static_cast<T>(0.5f); }

template <typename T> T One() { return static_cast<T>(1); }

template <typename T> T Two() { return static_cast<T>(2); }

template <typename T> T Min() { return static_cast<T>(-2147483647); }

template <typename T> T Max() { return static_cast<T>(2147483647); }

template <typename T> T Epsilon() { return static_cast<T>(0.00000001f); }

template <typename T> T PI() { return static_cast<T>(3.14159265f); }

template <typename T> T F128() { return static_cast<T>(128); }

template <typename T> T F90() { return static_cast<T>(90); }

template <typename T> T F180() { return static_cast<T>(180); }

template <typename T> T E() { return static_cast<T>(2.71828183f); }

template <typename T> T LogE() { return static_cast<T>(1.442695041f); }

template <typename T> T F02() { return static_cast<T>(0.2f); }

template <typename T> T F08() { return static_cast<T>(0.8f); }

///////////////////////////////////////////////////////////////////////////////

template <typename T>
struct TRect {
  enum { DIM = 4 };
DISABLE_WARNING_PUSH
DISABLE_WARNING_ANONYMOUS_STRUCT
  union {
    struct {
      T left;
      T top;
      T right;
      T bottom;
    };
    struct {
      T m[DIM];
    };
  };
DISABLE_WARNING_POP
};

template <typename T>
struct TVector1 {
  enum { DIM = 1 };
DISABLE_WARNING_PUSH
DISABLE_WARNING_ANONYMOUS_STRUCT
  union {
    struct {
      T x;
    };
    struct {
      T m[DIM];
    };
  };
DISABLE_WARNING_POP

  TVector1() {}

  TVector1(T x) : x(x) {}
};

template <typename T>
struct TVector2 {
  enum { DIM = 2 };
DISABLE_WARNING_PUSH
DISABLE_WARNING_ANONYMOUS_STRUCT
  union {
    struct {
      T x, y;
    };
    struct {
      T m[DIM];
    };
  };
DISABLE_WARNING_POP

  TVector2() {}

  TVector2(T x, T y) : x(x), y(y) {}
};

template <typename T>
struct TVector3 {
  enum { DIM = 3 };
DISABLE_WARNING_PUSH
DISABLE_WARNING_ANONYMOUS_STRUCT
  union {
    struct {
      T x, y, z;
    };
    struct {
      T m[DIM];
    };
  };
DISABLE_WARNING_POP

  TVector3() {}

  TVector3(T x, T y, T z) : x(x), y(y), z(z) {}
};

template <typename T>
struct TVector4 {
  enum { DIM = 4 };
DISABLE_WARNING_PUSH
DISABLE_WARNING_ANONYMOUS_STRUCT
  union {
    struct {
      T x, y, z, w;
    };
    struct {
      T m[DIM];
    };
  };
DISABLE_WARNING_POP

  TVector4() {}

  TVector4(T x, T y, T z, T w) : x(x), y(y), z(z), w(w) {}
};

template <typename T>
struct TMatrix44 {
DISABLE_WARNING_PUSH
DISABLE_WARNING_ANONYMOUS_STRUCT
  union {
    struct {
      T _11, _12, _13, _14;
      T _21, _22, _23, _24;
      T _31, _32, _33, _34;
      T _41, _42, _43, _44;
    };
    struct {
      T m[4][4];
    };
    struct {
      T _m[16];
    };
  };
DISABLE_WARNING_POP

  TMatrix44() {}

  TMatrix44(T m11, T m12, T m13, T m14,
            T m21, T m22, T m23, T m24,
            T m31, T m32, T m33, T m34,
            T m41, T m42, T m43, T m44) {
    this->_11 = m11;
    this->_12 = m12;
    this->_13 = m13;
    this->_14 = m14;
    this->_21 = m21;
    this->_22 = m22;
    this->_23 = m23;
    this->_24 = m24;
    this->_31 = m31;
    this->_32 = m32;
    this->_33 = m33;
    this->_34 = m34;
    this->_41 = m41;
    this->_42 = m42;
    this->_43 = m43;
    this->_44 = m44;
  }

  void toIdentity() {
    this->_11 = One<T>();
    this->_12 = Zero<T>();
    this->_13 = Zero<T>();
    this->_14 = Zero<T>();
    this->_21 = Zero<T>();
    this->_22 = One<T>();
    this->_23 = Zero<T>();
    this->_24 = Zero<T>();
    this->_31 = Zero<T>();
    this->_32 = Zero<T>();
    this->_33 = One<T>();
    this->_34 = Zero<T>();
    this->_41 = Zero<T>();
    this->_42 = Zero<T>();
    this->_43 = Zero<T>();
    this->_44 = One<T>();
  }
};

///////////////////////////////////////////////////////////////////////////////

template <typename T>
T DegToRad(T value) {
  return value * (PI<T>() / F180<T>());
}

template <typename T>
T RadToDeg(T value) {
  return value * (F180<T>() / PI<T>());
}

int32_t fxSin(int32_t x);

int32_t fxCos(int32_t x);

int32_t fxSqrt(int32_t rhs);

int32_t fxInvSqrt(int32_t x);

int32_t fxPow(int32_t x, int32_t y);

int32_t fxExp2(int32_t x);
  
inline int32_t Div255(int32_t x) {
  return (x + (x >> 8)) >> 8;
}  

} // namespace cocogfx

namespace std {

using namespace cocogfx;

template <uint32_t F, typename T>
TFixed<F,T> abs(TFixed<F,T> rhs) {
  return TFixed<F,T>::make(::abs(rhs.data()));
}

inline TFixed<16> sin(TFixed<16> rhs) {
  return TFixed<16>::make(fxSin(rhs.data()));
}

inline TFixed<16> cos(TFixed<16> rhs) {
  return TFixed<16>::make(fxCos(rhs.data()));
}

inline TFixed<16> sqrt(TFixed<16> rhs) {
  return TFixed<16>::make(fxSqrt(rhs.data()));
}

inline TFixed<16> pow(TFixed<16> lhs, TFixed<16> rhs) {
  return TFixed<16>::make(fxPow(lhs.data(), rhs.data()));
}

inline TFixed<16> exp2(TFixed<16> rhs) {
  return TFixed<16>::make(fxExp2(rhs.data()));
}

template <uint32_t F, typename T>
TFixed<F,T> exp(TFixed<F,T> rhs) {
  return TFixed<F,T>(std::exp(static_cast<float>(rhs)));
}

template <uint32_t F, typename T>
TFixed<F,T> ceil(TFixed<F,T> rhs) {
  return TFixed<F,T>::make((rhs.data() + TFixed<F,T>::MASK) & TFixed<F,T>::IMASK);
}

template <uint32_t F, typename T>
TFixed<F,T> floor(TFixed<F,T> rhs) {
  return TFixed<F,T>::make(rhs.data() & TFixed<F,T>::IMASK);
}

template <uint32_t F, typename T>
TFixed<F,T> round(TFixed<F,T> rhs) {
  return TFixed<F,T>::make((rhs.data() + TFixed<F,T>::HALF) & TFixed<F,T>::IMASK);
}

} // namespace std

///////////////////////////////////////////////////////////////////////////////

namespace cocogfx {

namespace detail {

template <typename Type, uint32_t Shift, bool = (Shift >= 32)>
struct ShiftInverter {
public:
  static Type call(int32_t value) {
    return static_cast<Type>((1 << Shift) / value);
  }
};

// ShiftInverter specialization for Shift >= 32
template <typename Type, uint32_t Shift>
struct ShiftInverter<Type, Shift, true> {
public:
  static Type call(int32_t value) {
    return value ? static_cast<Type>((static_cast<int64_t>(1) << Shift) / value) : 0;
  }
};

template <typename R, uint32_t F, typename T>
class Inverter {
public:
  static R Invert(TFixed<F,T> rhs) {
    return R::make(
      detail::ShiftInverter<
        typename R::data_type, TFixed<F,T>::FRAC + R::FRAC>::call(rhs.data()));
  }
};

template <uint32_t F, typename T>
class Inverter<float, F, T> {
public:
  static float Invert(TFixed<F,T> rhs) {
    return 1.0f / static_cast<float>(rhs);
  }
};

} // namespace detail

///////////////////////////////////////////////////////////////////////////////

template <typename R>
R FromUNORM8(int32_t rhs);

template <typename T>
int32_t ToUNORM8(T rhs);

template <typename T>
int32_t ToUNORM16(T rhs);

template <typename T>
bool IsAlmostZero(T rhs);

template <typename T>
T Sat(T rhs);

template <typename R, typename T>
R Ceil(T rhs);

template <typename R, typename T>
R Floori(T rhs);

template <typename R, typename T>
R Roundi(T rhs);

template <typename T>
T RSqrt(T rhs);

template <typename T>
T Dot(const TVector3<T> &lhs, const TVector3<T> &rhs);

template <typename T>
T Dot(const TVector4<T> &lhs, const TVector4<T> &rhs);

///////////////////////////////////////////////////////////////////////////////

template <>
inline TFixed<16> RSqrt(TFixed<16> rhs) {
  return TFixed<16>::make(fxInvSqrt(rhs.data()));
}

template <>
inline float RSqrt(float rhs) {
  assert(rhs != 0.0f);
  return 1.0f / std::sqrt(rhs);
}

///////////////////////////////////////////////////////////////////////////////

template <typename R>
R Dot(float a0, float b0,
      float a1, float b1,
      float a2, float b2,
      float a3, float b3) {
  return static_cast<R>(a0 * b0 + a1 * b1 + a2 * b2 + a3 * b3);
}

template <typename R, uint32_t F1, uint32_t F2, typename T1, typename T2>
R Dot(TFixed<F1,T1> a0, TFixed<F2,T2> b0,
      TFixed<F1,T1> a1, TFixed<F2,T2> b1,
      TFixed<F1,T1> a2, TFixed<F2,T2> b2,
      TFixed<F1,T1> a3, TFixed<F2,T2> b3) {
  int FRAC = TFixed<F1,T1>::FRAC + TFixed<F2,T2>::FRAC - R::FRAC;
  auto value = static_cast<int64_t>(a0.data()) * b0.data();
      value += static_cast<int64_t>(a1.data()) * b1.data();
      value += static_cast<int64_t>(a2.data()) * b2.data();
      value += static_cast<int64_t>(a3.data()) * b3.data();
  return R::make(value >> FRAC);
}

template <typename R>
R Dot(float a0, float b0,
      float a1, float b1,
      float a2, float b2) {
  return static_cast<R>(a0 * b0 + a1 * b1 + a2 * b2);
}

template <typename R, uint32_t F1, uint32_t F2, typename T1, typename T2>
R Dot(TFixed<F1,T1> a0, TFixed<F2,T2> b0,
      TFixed<F1,T1> a1, TFixed<F2,T2> b1,
      TFixed<F1,T1> a2, TFixed<F2,T2> b2) {
  int FRAC = TFixed<F1,T1>::FRAC + TFixed<F2,T2>::FRAC - R::FRAC;
  auto value = static_cast<int64_t>(a0.data()) * b0.data();
      value += static_cast<int64_t>(a1.data()) * b1.data();
      value += static_cast<int64_t>(a2.data()) * b2.data();
  return R::make(value >> FRAC);
}

template <typename R>
R Dot(float a0, float b0, float a1, float b1) {
  return static_cast<R>(a0 * b0 + a1 * b1);
}

template <typename R, uint32_t F1, uint32_t F2, typename T1, typename T2>
R Dot(TFixed<F1,T1> a0, TFixed<F2,T2> b0, TFixed<F1,T1> a1, TFixed<F2,T2> b1) {
  int FRAC = TFixed<F1,T1>::FRAC + TFixed<F2,T2>::FRAC - R::FRAC;
  auto value = static_cast<int64_t>(a0.data()) * b0.data();
      value += static_cast<int64_t>(a1.data()) * b1.data();
  return R::make(value >> FRAC);
}

template <typename R>
R Cross(float a0, float b0, float a1, float b1) {
  return static_cast<R>(a0 * b0 - a1 * b1);
}

template <typename R, uint32_t F1, uint32_t F2, typename T1, typename T2>
R Cross(TFixed<F1,T1> a0, TFixed<F2,T2> b0, TFixed<F1,T1> a1, TFixed<F2,T2> b1) {
  int FRAC = TFixed<F1,T1>::FRAC + TFixed<F2,T2>::FRAC - R::FRAC;
  auto value = static_cast<int64_t>(a0.data()) * b0.data();
      value -= static_cast<int64_t>(a1.data()) * b1.data();
  return R::make(value >> FRAC);
}

template <>
inline float FromUNORM8<float>(int32_t rhs) {
  return rhs * (1.0f / 255);
}

template <>
inline TFixed<16> FromUNORM8<TFixed<16>>(int32_t rhs) {
  return TFixed<16>::make((rhs << TFixed<16>::FRAC) / 255);
}

template <>
inline int32_t FromUNORM8<int32_t>(int32_t rhs) {
  return rhs;
}

inline int32_t Mul8(int32_t lhs, int32_t rhs) {
  return (lhs * rhs + 0xff) >> 8;
}

inline int32_t Add8(int32_t lhs, int32_t rhs) {
  return std::min<int32_t>(lhs + rhs, 0xff);
}

inline int32_t Sub8(int32_t lhs, int32_t rhs) {
  return std::max<int32_t>(lhs - rhs, 0);
}

inline int32_t Lerp8(int32_t lhs, int32_t rhs, int32_t frac) {
  return lhs + ((frac * (rhs - lhs) + 0xff) >> 8);
}

template <typename T>
T Sat(T rhs) {
  return std::min<T>(std::max<T>(rhs, Zero<T>()), One<T>());
}

template <typename T>
bool IsAlmostZero(T rhs) {
  return std::abs(rhs) <= Epsilon<T>();
}

//////////////////////////////////////////////////////////////////////////////

template <typename R>
R Ceil(float rhs) {
  return static_cast<R>(::ceil(rhs));
}

template <typename R>
R Floori(float rhs) {
  return static_cast<R>(::floor(rhs));
}

template <typename R>
R Roundi(float rhs) {
  return static_cast<R>(rhs + 0.5f);
}

template <>
inline int32_t ToUNORM8(float rhs) {
  assert((rhs >= 0.0f) && (rhs <= 1.0f));
  return static_cast<int32_t>(0xff * rhs);
}

template <>
inline int32_t ToUNORM16(float rhs) {
  assert((rhs >= 0.0f) && (rhs <= 1.0f));
  return static_cast<int32_t>(0xffff * rhs);
}

inline float FastMul(float lhs, float rhs) {
  return lhs * rhs;
}

template <typename R>
R Mul(float lhs, float rhs) {
  return static_cast<R>(lhs * rhs);
}

template <typename R>
R Mul(float lhs, int32_t rhs) {
  return static_cast<R>(lhs * rhs);
}

template <typename R, uint32_t F, typename T>
R Mul(float lhs, TFixed<F,T> rhs) {
  return static_cast<R>(lhs * rhs);
}

inline float MulDiv(float lhs, float rhs) {
  return lhs / rhs;
}

inline float Lerpf(float lhs, float rhs, float scalar) {
  assert((scalar >= 0.0f) && (scalar <= 1.0f));
  return lhs + (rhs - lhs) * scalar;
}

inline int32_t Lerp(int32_t lhs, int32_t rhs, float scalar) {
  assert((scalar >= 0.0f) && (scalar <= 1.0f));
  return lhs + static_cast<int32_t>((rhs - lhs) * scalar);
}

template <typename R>
R MulRnd(float a, float b) {
  return static_cast<R>(a * b);
}

template <typename R>
R MulRnd(float a, float b, float c) {
  return static_cast<R>(a * b * c);
}

template <typename R>
R ShiftLeft(float lhs, int32_t rhs) {
  float scale = static_cast<float>(1 << rhs);
  return static_cast<R>(lhs * scale);
}

template <typename R>
R ShiftRight(float lhs, int32_t rhs) {
  float scale = 1.0f / static_cast<float>(1 << rhs);
  return static_cast<R>(lhs * scale);
}

//////////////////////////////////////////////////////////////////////////////

template <typename R, uint32_t F, typename T>
R Ceil(TFixed<F,T> rhs) {
  int32_t value = rhs.data() + TFixed<F,T>::MASK;
  return static_cast<R>(value >> TFixed<F,T>::FRAC);
}

template <typename R, uint32_t F, typename T>
R Floori(TFixed<F,T> rhs) {
  return static_cast<R>(rhs.data() >> TFixed<F,T>::FRAC);
}

template <typename R, uint32_t F, typename T>
R Roundi(TFixed<F,T> rhs) {
  return static_cast<R>((rhs.data() + TFixed<F,T>::HALF) >> TFixed<F,T>::FRAC);
}

template <uint32_t F, typename T>
int32_t ToUNORM8(TFixed<F,T> rhs) {
  assert((rhs >= Zero<TFixed<F,T>>()) &&
         (rhs <= One<TFixed<F,T>>()));
  return (0xff * rhs.data()) >> TFixed<F,T>::FRAC;
}

template <uint32_t F, typename T>
int32_t ToUNORM16(TFixed<F,T> rhs) {
  assert((rhs >= Zero<TFixed<F,T>>()) &&
         (rhs <= One<TFixed<F,T>>()));
  return (0xffff * rhs.data()) >> TFixed<F,T>::FRAC;
}

template <typename R, uint32_t F, typename T>
R Inverse(TFixed<F,T> rhs) {
  return detail::Inverter<R, F, T>::Invert(rhs);
}

template <typename R>
R Inverse(float rhs) {
  assert(rhs != 0.0f);
  return static_cast<R>(1.0f / rhs);
}

template <typename R, uint32_t F1, uint32_t F2, typename T1, typename T2>
R FastMul(TFixed<F1,T1> lhs, TFixed<F2,T2> rhs) {
  assert((static_cast<int64_t>(lhs.data()) * rhs.data()) ==
         (lhs.data() * rhs.data()));
  int32_t FRAC = TFixed<F1,T1>::FRAC + TFixed<F2,T2>::FRAC - R::FRAC;
  return R::make((lhs.data() * rhs.data()) >> FRAC);
}

template <typename R, uint32_t F1, uint32_t F2, typename T1, typename T2>
R FastDiv(TFixed<F1,T1> lhs, TFixed<F2,T2> rhs) {
  int32_t FRAC = TFixed<F1,T1>::FRAC + TFixed<F2,T2>::FRAC - R::FRAC;
  assert((static_cast<int64_t>(lhs.data()) << FRAC) == (lhs.data() << FRAC));
  return R::make((lhs.data() << FRAC) / rhs.data());
}

template <typename R, uint32_t F, typename T>
R MulRnd(TFixed<F,T> lhs, int32_t rhs) {
  int32_t FRAC = TFixed<F,T>::FRAC - R::FRAC;
  int32_t HALF = 1 << (FRAC - 1);
  auto value = static_cast<int64_t>(lhs.data()) * rhs;
  return R::make((value + HALF) >> FRAC);
}

template <typename R, uint32_t F1, uint32_t F2, typename T1, typename T2>
R MulRnd(TFixed<F1,T1> lhs, TFixed<F2,T2> rhs) {
  int32_t FRAC = TFixed<F1,T1>::FRAC + TFixed<F2,T2>::FRAC - R::FRAC;
  int32_t HALF = 1 << (FRAC - 1);
  auto value = static_cast<int64_t>(lhs.data()) * rhs.data();
  return R::make((value + HALF) >> FRAC);
}

template <typename R, uint32_t F1, uint32_t F2, typename T1, typename T2>
R MulRnd(TFixed<F1,T1> a, TFixed<F2,T2> b, int32_t c) {
  int32_t FRAC = TFixed<F1,T1>::FRAC + TFixed<F2,T2>::FRAC - R::FRAC;
  auto HALF = static_cast<int64_t>(1) << (FRAC - 1);
  auto value = static_cast<int64_t>(a.data()) * b.data() * c;
  return R::make((value + HALF) >> FRAC);
}

template <typename R, uint32_t F1, uint32_t F2, uint32_t F3, typename T1,
          typename T2, typename T3>
R MulRnd(TFixed<F1,T1> a, TFixed<F2,T2> b, TFixed<F3, T3> c) {
  int32_t FRAC = TFixed<F1,T1>::FRAC + TFixed<F2,T2>::FRAC + TFixed<F3, T3>::FRAC - R::FRAC;
  auto HALF = static_cast<int64_t>(1) << (FRAC - 1);
  int64_t value = static_cast<int64_t>(a.data()) * b.data() * c.data();
  return R::make((value + HALF) >> FRAC);
}

template <typename R, uint32_t F, typename T>
R Mul(TFixed<F,T> lhs, TFixed<F,T> rhs) {
  int32_t FRAC = TFixed<F,T>::FRAC + TFixed<F,T>::FRAC - R::FRAC;
  auto value = static_cast<int64_t>(lhs.data()) * rhs.data();
  return R::make(value >> FRAC);
}

template <typename R, uint32_t F1, uint32_t F2, typename T1, typename T2>
R Mul(TFixed<F1,T1> lhs, TFixed<F2,T2> rhs) {
  int32_t FRAC = TFixed<F1,T1>::FRAC + TFixed<F2,T2>::FRAC - R::FRAC;
  auto value = static_cast<int64_t>(lhs.data()) * rhs.data();
  return R::make(value >> FRAC);
}

template <typename R, uint32_t F, typename T>
R Mul(TFixed<F,T> lhs, int32_t rhs) {
  int32_t FRAC = TFixed<F,T>::FRAC - R::FRAC;
  auto value = static_cast<int64_t>(lhs.data()) * rhs;
  return R::make(value >> FRAC);
}

template <typename R, uint32_t F, typename T>
R Mul(TFixed<F,T> lhs, float rhs) {
  return static_cast<R>(rhs * lhs);
}

template <typename R, uint32_t F1, uint32_t F2, typename T1, typename T2>
R MulShift(TFixed<F1,T1> lhs, TFixed<F2,T2> rhs, R a, int32_t shift) {
  int32_t FRAC = TFixed<F1,T1>::FRAC + TFixed<F2,T2>::FRAC - R::FRAC;
  auto value = static_cast<int64_t>(lhs.data()) * rhs.data();
  return R::make((value >> FRAC) - (a.data() >> shift));
}

template <uint32_t F1, uint32_t F2, typename T1, typename T2>
TFixed<F1,T1> Lerpf(TFixed<F1,T1> lhs, TFixed<F1,T1> rhs, TFixed<F2,T2> scalar) {
  assert((scalar >= Zero<TFixed<F2,T2>>()) &&
         (scalar <= One<TFixed<F2,T2>>()));
  return lhs + (rhs - lhs) * scalar;
}

template <uint32_t F, typename T>
TFixed<F,T> Lerpf(TFixed<F,T> lhs, TFixed<F,T> rhs, float scalar) {
  assert((scalar >= 0.0f) && (scalar <= 1.0f));
  return lhs + (rhs - lhs) * scalar;
}

template <uint32_t F, typename T>
int32_t Lerp(int32_t lhs, int32_t rhs, TFixed<F,T> scalar) {
  assert((scalar >= Zero<TFixed<F,T>>()) &&
         (scalar <= One<TFixed<F,T>>()));
  return lhs + static_cast<int32_t>((static_cast<int64_t>(rhs - lhs) * scalar.data()) >> TFixed<F,T>::FRAC);
}

template <typename R, uint32_t F, typename T>
R ShiftLeft(TFixed<F,T> lhs, int32_t rhs) {
  return R(lhs) << rhs;
}

template <typename R, uint32_t F, typename T>
R ShiftRight(TFixed<F,T> lhs, int32_t rhs) {
  return R(lhs) >> rhs;
}

//////////////////////////////////////////////////////////////////////////////

template <>
inline TFixed<16> Dot<TFixed<16>>(const TVector3<TFixed<16>> &lhs, const TVector3<TFixed<16>> &rhs) {
  auto xx = static_cast<int64_t>(lhs.x.data()) * rhs.x.data();
  auto yy = static_cast<int64_t>(lhs.y.data()) * rhs.y.data();
  auto zz = static_cast<int64_t>(lhs.z.data()) * rhs.z.data();
  return TFixed<16>::make((xx + yy + zz) >> TFixed<16>::FRAC);
}

template <>
inline TFixed<16> Dot<TFixed<16>>(const TVector4<TFixed<16>> &lhs, const TVector4<TFixed<16>> &rhs) {
  auto xx = static_cast<int64_t>(lhs.x.data()) * rhs.x.data();
  auto yy = static_cast<int64_t>(lhs.y.data()) * rhs.y.data();
  auto zz = static_cast<int64_t>(lhs.z.data()) * rhs.z.data();
  auto ww = static_cast<int64_t>(lhs.w.data()) * rhs.w.data();
  return TFixed<16>::make((xx + yy + zz + ww) >> TFixed<16>::FRAC);
}

template <>
inline float Dot<float>(const TVector3<float> &lhs, const TVector3<float> &rhs) {
  return lhs.x * rhs.x + lhs.y * rhs.y + lhs.z * rhs.z;
}

template <>
inline float Dot<float>(const TVector4<float> &lhs, const TVector4<float> &rhs) {
  return lhs.x * rhs.x + lhs.y * rhs.y + lhs.z * rhs.z + lhs.w * rhs.w;
}

template <typename T>
void Mul(TVector4<T> *pOut, const TVector3<T> &vIn, const TMatrix44<T> &mat) {
  assert(pOut);

  auto x = vIn.x;
  auto y = vIn.y;
  auto z = vIn.z;

  pOut->x = Dot<T>(x, mat._11, y, mat._21, z, mat._31) + mat._41;
  pOut->y = Dot<T>(x, mat._12, y, mat._22, z, mat._32) + mat._42;
  pOut->z = Dot<T>(x, mat._13, y, mat._23, z, mat._33) + mat._43;
  pOut->w = Dot<T>(x, mat._14, y, mat._24, z, mat._34) + mat._44;
}

template <typename T>
void Mul4x3(TVector3<T> *pOut, const TVector3<T> &vIn, const TMatrix44<T> &mat) {
  assert(pOut);

  auto x = vIn.x;
  auto y = vIn.y;
  auto z = vIn.z;

  pOut->x = Dot<T>(x, mat._11, y, mat._21, z, mat._31) + mat._41;
  pOut->y = Dot<T>(x, mat._12, y, mat._22, z, mat._32) + mat._42;
  pOut->z = Dot<T>(x, mat._13, y, mat._23, z, mat._33) + mat._43;
}

template <typename T>
void Mul(TVector3<T> *pOut, const TVector3<T> &vIn, const TMatrix44<T> &mat) {
  assert(pOut);

  auto x = vIn.x;
  auto y = vIn.y;
  auto z = vIn.z;

  pOut->x = Dot<T>(x, mat._11, y, mat._21, z, mat._31);
  pOut->y = Dot<T>(x, mat._12, y, mat._22, z, mat._32);
  pOut->z = Dot<T>(x, mat._13, y, mat._23, z, mat._33);
}

template <typename T>
void Mul(TVector4<T> *pOut, const TVector4<T> &vIn, const TMatrix44<T> &mat) {
  assert(pOut);

  auto x = vIn.x;
  auto y = vIn.y;
  auto z = vIn.z;
  auto w = vIn.w;

  pOut->x = Dot<T>(x, mat._11, y, mat._21, z, mat._31, w, mat._41);
  pOut->y = Dot<T>(x, mat._12, y, mat._22, z, mat._32, w, mat._42);
  pOut->z = Dot<T>(x, mat._13, y, mat._23, z, mat._33, w, mat._43);
  pOut->w = Dot<T>(x, mat._14, y, mat._24, z, mat._34, w, mat._44);
}

template <typename T>
void Mul(TVector3<T> *pOut, const TVector4<T> &vIn, const TMatrix44<T> &mat) {
  assert(pOut);

  auto x = vIn.x;
  auto y = vIn.y;
  auto z = vIn.z;
  auto w = vIn.w;

  pOut->x = Dot<T>(x, mat._11, y, mat._21, z, mat._31, w, mat._41);
  pOut->y = Dot<T>(x, mat._12, y, mat._22, z, mat._32, w, mat._42);
  pOut->z = Dot<T>(x, mat._13, y, mat._23, z, mat._33, w, mat._43);
}

template <typename T>
void Mul(TVector2<T> *pOut, const TVector4<T> &vIn, const TMatrix44<T> &mat) {
  assert(pOut);

  auto x = vIn.x;
  auto y = vIn.y;
  auto z = vIn.z;
  auto w = vIn.w;

  pOut->x = Dot<T>(x, mat._11, y, mat._21, z, mat._31, w, mat._41);
  pOut->y = Dot<T>(x, mat._12, y, mat._22, z, mat._32, w, mat._42);
}

template <typename T>
void Mul(TVector2<T> *pOut, const TVector2<T> &vIn, const TMatrix44<T> &mat) {
  assert(pOut);

  auto x = vIn.x;
  auto y = vIn.y;

  pOut->x = Dot<T>(x, mat._11, y, mat._21) + mat._41;
  pOut->y = Dot<T>(x, mat._12, y, mat._22) + mat._42;
}

template <typename T>
T Length(const TVector3<T> &vIn) {
  auto fDot = Dot<T>(vIn, vIn);
  if (!IsAlmostZero(fDot - One<T>())) {
    return std::sqrt(fDot);
  }
  return fDot;
}

template <typename T>
void Normalize(TVector3<T> *pOut) {
  assert(pOut);
  auto fDot = Dot<T>(*pOut, *pOut);
  if (!IsAlmostZero(fDot - One<T>()) &&
      !IsAlmostZero(fDot)) {
    auto fInvLen = RSqrt(fDot);
    pOut->x = pOut->x * fInvLen;
    pOut->y = pOut->y * fInvLen;
    pOut->z = pOut->z * fInvLen;
  }
}

template <typename T>
void Neg(TVector3<T> *pOut, const TVector3<T> &vIn) {
  pOut->x = -vIn.x;
  pOut->y = -vIn.y;
  pOut->z = -vIn.z;
}

template <typename T>
void Add(TVector3<T> *pOut, const TVector3<T> &v1, const TVector3<T> &v2) {
  pOut->x = v1.x + v2.x;
  pOut->y = v1.y + v2.y;
  pOut->z = v1.z + v2.z;
}

///////////////////////////////////////////////////////////////////////////////

template <typename T>
void Transpose(TMatrix44<T> *pOut, const TMatrix44<T> &matIn) {
  assert(pOut);

  pOut->_11 = matIn._11;
  pOut->_12 = matIn._21;
  pOut->_13 = matIn._31;
  pOut->_14 = matIn._41;

  pOut->_21 = matIn._12;
  pOut->_22 = matIn._22;
  pOut->_23 = matIn._32;
  pOut->_24 = matIn._42;

  pOut->_31 = matIn._13;
  pOut->_32 = matIn._23;
  pOut->_33 = matIn._33;
  pOut->_34 = matIn._43;

  pOut->_41 = matIn._14;
  pOut->_42 = matIn._24;
  pOut->_43 = matIn._34;
  pOut->_44 = matIn._44;
}

template <typename T>
void Transpose33(TMatrix44<T> *pOut, const TMatrix44<T> &matIn) {
  assert(pOut);

  pOut->_11 = matIn._11;
  pOut->_12 = matIn._21;
  pOut->_13 = matIn._31;

  pOut->_21 = matIn._12;
  pOut->_22 = matIn._22;
  pOut->_23 = matIn._32;

  pOut->_31 = matIn._13;
  pOut->_32 = matIn._23;
  pOut->_33 = matIn._33;
}

template <typename T>
void Scale(TMatrix44<T> *pOut, T x, T y, T z) {
  assert(pOut);

  pOut->toIdentity();

  pOut->_11 = x;
  pOut->_22 = y;
  pOut->_33 = z;
}

template <typename T>
void Translate(TMatrix44<T> *pOut, T x, T y, T z) {
  assert(pOut);

  pOut->toIdentity();

  pOut->_41 = x;
  pOut->_42 = y;
  pOut->_43 = z;
}

template <typename T>
void Mul(TMatrix44<T> *pOut, const TMatrix44<T> &mat1, const TMatrix44<T> &mat2) {
  assert(pOut);

  pOut->_11 = Dot<T>(mat1._11, mat2._11, mat1._21, mat2._12, mat1._31,
                        mat2._13, mat1._41, mat2._14);
  pOut->_12 = Dot<T>(mat1._12, mat2._11, mat1._22, mat2._12, mat1._32,
                        mat2._13, mat1._42, mat2._14);
  pOut->_13 = Dot<T>(mat1._13, mat2._11, mat1._23, mat2._12, mat1._33,
                        mat2._13, mat1._43, mat2._14);
  pOut->_14 = Dot<T>(mat1._14, mat2._11, mat1._24, mat2._12, mat1._34,
                        mat2._13, mat1._44, mat2._14);

  pOut->_21 = Dot<T>(mat1._11, mat2._21, mat1._21, mat2._22, mat1._31,
                        mat2._23, mat1._41, mat2._24);
  pOut->_22 = Dot<T>(mat1._12, mat2._21, mat1._22, mat2._22, mat1._32,
                        mat2._23, mat1._42, mat2._24);
  pOut->_23 = Dot<T>(mat1._13, mat2._21, mat1._23, mat2._22, mat1._33,
                        mat2._23, mat1._43, mat2._24);
  pOut->_24 = Dot<T>(mat1._14, mat2._21, mat1._24, mat2._22, mat1._34,
                        mat2._23, mat1._44, mat2._24);

  pOut->_31 = Dot<T>(mat1._11, mat2._31, mat1._21, mat2._32, mat1._31,
                        mat2._33, mat1._41, mat2._34);
  pOut->_32 = Dot<T>(mat1._12, mat2._31, mat1._22, mat2._32, mat1._32,
                        mat2._33, mat1._42, mat2._34);
  pOut->_33 = Dot<T>(mat1._13, mat2._31, mat1._23, mat2._32, mat1._33,
                        mat2._33, mat1._43, mat2._34);
  pOut->_34 = Dot<T>(mat1._14, mat2._31, mat1._24, mat2._32, mat1._34,
                        mat2._33, mat1._44, mat2._34);

  pOut->_41 = Dot<T>(mat1._11, mat2._41, mat1._21, mat2._42, mat1._31,
                        mat2._43, mat1._41, mat2._44);
  pOut->_42 = Dot<T>(mat1._12, mat2._41, mat1._22, mat2._42, mat1._32,
                        mat2._43, mat1._42, mat2._44);
  pOut->_43 = Dot<T>(mat1._13, mat2._41, mat1._23, mat2._42, mat1._33,
                        mat2._43, mat1._43, mat2._44);
  pOut->_44 = Dot<T>(mat1._14, mat2._41, mat1._24, mat2._42, mat1._34,
                        mat2._43, mat1._44, mat2._44);
}

template <typename T>
void Inverse33(TMatrix44<T> *pOut, const TMatrix44<T> &matIn) {
  assert(pOut);

  pOut->_11 = Cross<T>(matIn._22, matIn._33, matIn._23, matIn._32);
  pOut->_12 = -Cross<T>(matIn._21, matIn._33, matIn._23, matIn._31);
  pOut->_13 = Cross<T>(matIn._21, matIn._32, matIn._22, matIn._31);

  pOut->_21 = -Cross<T>(matIn._12, matIn._33, matIn._13, matIn._32);
  pOut->_22 = Cross<T>(matIn._11, matIn._33, matIn._13, matIn._31);
  pOut->_23 = -Cross<T>(matIn._11, matIn._32, matIn._12, matIn._31);

  pOut->_31 = Cross<T>(matIn._12, matIn._23, matIn._13, matIn._22);
  pOut->_32 = -Cross<T>(matIn._11, matIn._23, matIn._13, matIn._21);
  pOut->_33 = Cross<T>(matIn._11, matIn._22, matIn._12, matIn._21);

  auto fDet = Dot<T>(matIn._11, pOut->_11, matIn._21, pOut->_21,
                     matIn._31, pOut->_31);

  if (!IsAlmostZero(fDet - One<T>())) {
    auto fDetInv = Inverse<T>(fDet);

    pOut->_11 *= fDetInv;
    pOut->_12 *= fDetInv;
    pOut->_13 *= fDetInv;

    pOut->_21 *= fDetInv;
    pOut->_22 *= fDetInv;
    pOut->_23 *= fDetInv;

    pOut->_31 *= fDetInv;
    pOut->_32 *= fDetInv;
    pOut->_33 *= fDetInv;
  }
}

template <typename T>
void Inverse(TMatrix44<T> *pOut, const TMatrix44<T> &matIn) {
  assert(pOut);

  auto fB0 = Cross<T>(matIn._13, matIn._24, matIn._23, matIn._14);
  auto fB1 = Cross<T>(matIn._13, matIn._34, matIn._33, matIn._14);
  auto fB2 = Cross<T>(matIn._13, matIn._44, matIn._43, matIn._14);
  auto fB3 = Cross<T>(matIn._23, matIn._34, matIn._33, matIn._24);
  auto fB4 = Cross<T>(matIn._23, matIn._44, matIn._43, matIn._24);
  auto fB5 = Cross<T>(matIn._33, matIn._44, matIn._43, matIn._34);

  pOut->_11 = (matIn._22 * fB5 - matIn._32 * fB4 + matIn._42 * fB3);
  pOut->_12 = (-matIn._12 * fB5 + matIn._32 * fB2 - matIn._42 * fB1);
  pOut->_13 = (matIn._12 * fB4 - matIn._22 * fB2 + matIn._42 * fB0);
  pOut->_14 = (-matIn._12 * fB3 + matIn._22 * fB1 - matIn._32 * fB0);

  pOut->_21 = (-matIn._21 * fB5 + matIn._31 * fB4 - matIn._41 * fB3);
  pOut->_22 = (matIn._11 * fB5 - matIn._31 * fB2 + matIn._41 * fB1);
  pOut->_23 = (-matIn._11 * fB4 + matIn._21 * fB2 - matIn._41 * fB0);
  pOut->_24 = (matIn._11 * fB3 - matIn._21 * fB1 + matIn._31 * fB0);

  auto fA0 = Cross<T>(matIn._11, matIn._22, matIn._21, matIn._12);
  auto fA1 = Cross<T>(matIn._11, matIn._32, matIn._31, matIn._12);
  auto fA2 = Cross<T>(matIn._11, matIn._42, matIn._41, matIn._12);
  auto fA3 = Cross<T>(matIn._21, matIn._32, matIn._31, matIn._22);
  auto fA4 = Cross<T>(matIn._21, matIn._42, matIn._41, matIn._22);
  auto fA5 = Cross<T>(matIn._31, matIn._42, matIn._41, matIn._32);

  pOut->_31 = (matIn._24 * fA5 - matIn._34 * fA4 + matIn._44 * fA3);
  pOut->_32 = (-matIn._14 * fA5 + matIn._34 * fA2 - matIn._44 * fA1);
  pOut->_33 = (matIn._14 * fA4 - matIn._24 * fA2 + matIn._44 * fA0);
  pOut->_34 = (-matIn._14 * fA3 + matIn._24 * fA1 - matIn._34 * fA0);

  pOut->_41 = (-matIn._23 * fA5 + matIn._33 * fA4 - matIn._43 * fA3);
  pOut->_42 = (matIn._13 * fA5 - matIn._33 * fA2 + matIn._43 * fA1);
  pOut->_43 = (-matIn._13 * fA4 + matIn._23 * fA2 - matIn._43 * fA0);
  pOut->_44 = (matIn._13 * fA3 - matIn._23 * fA1 + matIn._33 * fA0);

  auto fDet = Dot<T>(fA0, fB5, fA2, fB3, fA3, fB2, fA5, fB0) -
              Dot<T>(fA1, fB4, fA4, fB1);

  if (!IsAlmostZero(fDet - One<T>())) {
    auto fDetInv = Inverse<T>(fDet);

    pOut->_11 *= fDetInv;
    pOut->_12 *= fDetInv;
    pOut->_13 *= fDetInv;
    pOut->_14 *= fDetInv;

    pOut->_21 *= fDetInv;
    pOut->_22 *= fDetInv;
    pOut->_23 *= fDetInv;
    pOut->_24 *= fDetInv;

    pOut->_31 *= fDetInv;
    pOut->_32 *= fDetInv;
    pOut->_33 *= fDetInv;
    pOut->_34 *= fDetInv;

    pOut->_41 *= fDetInv;
    pOut->_42 *= fDetInv;
    pOut->_43 *= fDetInv;
    pOut->_44 *= fDetInv;
  }
}

template <typename T>
void Ortho(TMatrix44<T> *pOut, T left, T right, T bottom, T top, T zNear, T zFar) {
  assert(pOut);

  auto fWidth = right - left;
  auto fHeight = top - bottom;
  auto fDepth = zFar - zNear;

  auto fInvWidth = (fWidth != Zero<T>())
                       ? Inverse<T>(fWidth)
                       : Zero<T>();
  auto fInvHeight = (fHeight != Zero<T>())
                        ? Inverse<T>(fHeight)
                        : Zero<T>();
  auto fInvDepth = (fDepth != Zero<T>())
                       ? Inverse<T>(fDepth)
                       : Zero<T>();

  pOut->_11 = Two<T>() * fInvWidth;
  pOut->_12 = Zero<T>();
  pOut->_13 = Zero<T>();
  pOut->_14 = Zero<T>();

  pOut->_21 = Zero<T>();
  pOut->_22 = Two<T>() * fInvHeight;
  pOut->_23 = Zero<T>();
  pOut->_24 = Zero<T>();

  pOut->_31 = Zero<T>();
  pOut->_32 = Zero<T>();
  pOut->_33 = -Two<T>() * fInvDepth;
  pOut->_34 = Zero<T>();

  pOut->_41 = -(right + left) * fInvWidth;
  pOut->_42 = -(top + bottom) * fInvHeight;
  pOut->_43 = -(zFar + zNear) * fInvDepth;
  pOut->_44 = One<T>();
}

template <typename T>
void Frustum(TMatrix44<T> *pOut, T left, T right, T bottom, T top, T zNear, T zFar) {
  assert(pOut);

  auto fWidth = right - left;
  auto fHeight = top - bottom;
  auto fDepth = zFar - zNear;

  auto fInvWidth = (fWidth != Zero<T>())
                       ? Inverse<T>(fWidth)
                       : Zero<T>();
  auto fInvHeight = (fHeight != Zero<T>())
                        ? Inverse<T>(fHeight)
                        : Zero<T>();
  auto fInvDepth = (fDepth != Zero<T>())
                       ? Inverse<T>(fDepth)
                       : Zero<T>();

  auto fTwoNear = Two<T>() * zNear;

  pOut->_11 = fTwoNear * fInvWidth;
  pOut->_12 = Zero<T>();
  pOut->_13 = Zero<T>();
  pOut->_14 = Zero<T>();

  pOut->_21 = Zero<T>();
  pOut->_22 = fTwoNear * fInvHeight;
  pOut->_23 = Zero<T>();
  pOut->_24 = Zero<T>();

  pOut->_31 = (right + left) * fInvWidth;
  pOut->_32 = (top + bottom) * fInvHeight;
  pOut->_33 = -(zFar + zNear) * fInvDepth;
  pOut->_34 = -One<T>();

  pOut->_41 = Zero<T>();
  pOut->_42 = Zero<T>();
  pOut->_43 = -zFar * (fTwoNear * fInvDepth);
  pOut->_44 = Zero<T>();
}

template <typename T>
void Rotate(TMatrix44<T> *pOut, T angle, T x, T y, T z) {
  assert(pOut);

  TVector3<T> vAxis(x, y, z);
  Normalize(&vAxis);

  auto fSin = std::sin(angle);
  auto fCos = std::cos(angle);
  auto fICos = One<T>() - fCos;

  auto _fA = fICos * vAxis.x * vAxis.y;
  auto _fB = fICos * vAxis.x * vAxis.z;
  auto _fC = fICos * vAxis.y * vAxis.z;

  pOut->_11 = fICos * vAxis.x * vAxis.x + fCos;
  pOut->_12 = _fA + vAxis.z * fSin;
  pOut->_13 = _fB - vAxis.y * fSin;
  pOut->_14 = Zero<T>();

  pOut->_21 = _fA - vAxis.z * fSin;
  pOut->_22 = fICos * vAxis.y * vAxis.y + fCos;
  pOut->_23 = _fC + vAxis.x * fSin;
  pOut->_24 = Zero<T>();

  pOut->_31 = _fB + vAxis.y * fSin;
  pOut->_32 = _fC - vAxis.x * fSin;
  pOut->_33 = fICos * vAxis.z * vAxis.z + fCos;
  pOut->_34 = Zero<T>();

  pOut->_41 = Zero<T>();
  pOut->_42 = Zero<T>();
  pOut->_43 = Zero<T>();
  pOut->_44 = One<T>();
}

///////////////////////////////////////////////////////////////////////////////

// Convert position from clip space to normalized device coordinates
template <typename T>
void ClipToNDC(TVector4<T>* out, const TVector4<T>& in) {
  auto rhw = (in.w != Zero<T>()) ? Inverse<T>(in.w) : Zero<T>();
  out->x = in.x * rhw;
  out->y = in.y * rhw;
  out->z = in.z * rhw;
  out->w = rhw;
}

// Convert position from clip space to homogenous device coordinates
template <typename T>
void ClipToHDC(TVector4<T>* out,
               const TVector4<T>& in,
               int32_t left,
               int32_t right,
               int32_t top,
               int32_t bottom,
               T near,
               T far) {
  auto minX   = static_cast<T>(left + right) / 2;
  auto scaleX = static_cast<T>(right - left) / 2;

  auto minY   = static_cast<T>(top + bottom) / 2;
  auto scaleY = static_cast<T>(bottom - top) / 2;

  auto minZ   = (near + far) / 2;
  auto scaleZ = (far - near) / 2;

  out->x = in.x * scaleX + in.w * minX;
  out->y = in.y * scaleY + in.w * minY;
  out->z = in.z * scaleZ + in.w * minZ;
  out->w = in.w;
}

// Convert position from NDC space to screen space
template <typename T>
void NDCToScreen(TVector4<T>* out,
                 const TVector4<T>& in,
                 int32_t left,
                 int32_t right,
                 int32_t top,
                 int32_t bottom,
                 T near,
                 T far) {
  auto minX   = static_cast<T>(left + right) / 2;
  auto scaleX = static_cast<T>(right - left) / 2;

  auto minY   = static_cast<T>(top + bottom) / 2;
  auto scaleY = static_cast<T>(bottom - top) / 2;

  auto minZ   = (near + far) / 2;
  auto scaleZ = (far - near) / 2;

  out->x = in.x * scaleX + minX;
  out->y = in.y * scaleY + minY;
  out->z = in.z * scaleZ + minZ;
  out->w = in.w;
}

// Convert position from clip space to screen space
template <typename T>
void ClipToScreen(TVector4<T>* out,
                  const TVector4<T>& in,
                  int32_t left,
                  int32_t right,
                  int32_t top,
                  int32_t bottom,
                  T near,
                  T far) {
  TVector4<T> tmp;
  ClipToNDC<T>(&tmp, in);
  NDCToScreen<T>(out, tmp, left, right, top, bottom, near, far);
}

///////////////////////////////////////////////////////////////////////////////

// Calculate triangle bounding box
template <typename T>
void CalcBoundingBox(TRect<T>* pOut,
                     const TVector2<T>& v0,
                     const TVector2<T>& v1,
                     const TVector2<T>& v2) {
  // Find min/max
  pOut->left   = static_cast<T>(std::min(v0.x, std::min(v1.x, v2.x)));
  pOut->right  = static_cast<T>(std::max(v0.x, std::max(v1.x, v2.x)));
  pOut->top    = static_cast<T>(std::min(v0.y, std::min(v1.y, v2.y)));
  pOut->bottom = static_cast<T>(std::max(v0.y, std::max(v1.y, v2.y)));
}

template <typename T>
bool Intersect(const TRect<T> *pSrc1, const TRect<T> *pSrc2) {
  return !(pSrc1->left >= pSrc2->right
        || pSrc1->right <= pSrc2->left
        || pSrc1->top >= pSrc2->bottom
        || pSrc1->bottom <= pSrc2->top);
}

template <typename T>
bool Intersect(TRect<T> *pDst, const TRect<T> *pSrc1, const TRect<T> *pSrc2) {
  assert(pDst);
  auto left   = std::max<T>(pSrc1->left,   pSrc2->left);
  auto top    = std::max<T>(pSrc1->top,    pSrc2->top);
  auto right  = std::max<T>(pSrc1->right,  pSrc2->right);
  auto bottom = std::max<T>(pSrc1->bottom, pSrc2->bottom);
  if (left >= right || top >= bottom)
    return false;
  pDst->left   = left;
  pDst->top    = top;
  pDst->right  = right;
  pDst->bottom = bottom;
  return true;
}

}
