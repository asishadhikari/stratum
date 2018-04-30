/*
 * Copyright 2018 Google LLC
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */


#ifndef STRATUM_GLUE_STATUS_STATUSOR_H_
#define STRATUM_GLUE_STATUS_STATUSOR_H_

// StatusOr<T> is the union of a Status object and a T
// object. StatusOr models the concept of an object that is either a
// usable value, or an error Status explaining why such a value is
// not present. To this end, StatusOr<T> does not allow its Status
// value to be Status::OK. Further, StatusOr<T*> does not allow the
// contained pointer to be NULL.
//
// The primary use-case for StatusOr<T> is as the return value of a
// function which may fail.
//
// Example client usage for a StatusOr<T>, where T is not a pointer:
//
//  StatusOr<float> result = DoBigCalculationThatCouldFail();
//  if (result.ok()) {
//    float answer = result.ValueOrDie();
//    printf("Big calculation yielded: %f", answer);
//  } else {
//    LOG(ERROR) << result.status();
//  }
//
// Example client usage for a StatusOr<T*>:
//
//  StatusOr<Foo*> result = FooFactory::MakeNewFoo(arg);
//  if (result.ok()) {
//    std::unique_ptr<Foo> foo(result.ValueOrDie());
//    foo->DoSomethingCool();
//  } else {
//    LOG(ERROR) << result.status();
//  }
//
// Example client usage for a StatusOr<std::unique_ptr<T>>:
//
//  StatusOr<std::unique_ptr<Foo>> result = FooFactory::MakeNewFoo(arg);
//  if (result.ok()) {
//    std::unique_ptr<Foo> foo = result.ConsumeValueOrDie();
//    foo->DoSomethingCool();
//  } else {
//    LOG(ERROR) << result.status();
//  }
//
// Example factory implementation returning StatusOr<T*>:
//
//  StatusOr<Foo*> FooFactory::MakeNewFoo(int arg) {
//    if (arg <= 0) {
//      return ::util::Status(::util::error::INVALID_ARGUMENT,
//                            "Arg must be positive");
//    } else {
//      return new Foo(arg);
//    }
//  }
//

#include <new>
#include <string>
#include <type_traits>
#include <utility>

#include "third_party/stratum/glue/logging.h"
#include "third_party/stratum/glue/status/status.h"

namespace util {

template <typename T>
#if !defined(CLANG_WARN_UNUSED_RESULT) || defined(SWIG)
class StatusOr {
#else
class CLANG_WARN_UNUSED_RESULT StatusOr {
#endif
  template <typename U>
  friend class StatusOr;

 public:
  // Construct a new StatusOr with Status::UNKNOWN status
  StatusOr();

  // Construct a new StatusOr with the given non-ok status. After calling
  // this constructor, calls to ValueOrDie() will CHECK-fail.
  //
  // NOTE: Not explicit - we want to use StatusOr<T> as a return
  // value, so it is convenient and sensible to be able to do 'return
  // Status()' when the return type is StatusOr<T>.
  //
  // REQUIRES: status != Status::OK. This requirement is DCHECKed.
  // In optimized builds, passing Status::OK here will have the effect
  // of passing PosixErrorSpace::EINVAL as a fallback.
  StatusOr(const ::util::Status& status);  // NOLINT

  // Construct a new StatusOr with the given value. If T is a plain pointer,
  // value must not be NULL. After calling this constructor, calls to
  // ValueOrDie() will succeed, and calls to status() will return OK.
  //
  // NOTE: Not explicit - we want to use StatusOr<T> as a return type
  // so it is convenient and sensible to be able to do 'return T()'
  // when when the return type is StatusOr<T>.
  //
  // REQUIRES: if T is a plain pointer, value != NULL. This requirement is
  // DCHECKed. In optimized builds, passing a NULL pointer here will have
  // the effect of passing PosixErrorSpace::EINVAL as a fallback.
  StatusOr(const T& value);  // NOLINT

  // Copy constructor.
  StatusOr(const StatusOr& other) = default;

  // Conversion copy constructor, T must be copy constructible from U
  template <typename U>
  StatusOr(const StatusOr<U>& other);

  // Assignment operator.
  StatusOr& operator=(const StatusOr& other) = default;

  // Conversion assignment operator, T must be assignable from U
  template <typename U>
  StatusOr& operator=(const StatusOr<U>& other);

#ifndef SWIG
  // Move constructor and move-assignment operator.
  StatusOr(StatusOr&& other) = default;
  StatusOr& operator=(StatusOr&& other) = default;

  // Rvalue-reference overloads of the other constructors and assignment
  // operators, to support move-only types and avoid unnecessary copying.
  //
  // Implementation note: we could avoid all these rvalue-reference overloads
  // if the existing lvalue-reference overloads took their arguments by value
  // instead. I think this would also let us omit the conversion assignment
  // operator altogether, since we'd get the same functionality for free
  // from the implicit conversion constructor and ordinary assignment.
  // However, this could result in extra copy operations unless we use
  // std::move to avoid them, and we can't use std::move because this code
  // needs to be portable to C++03.
  StatusOr(T&& value);  // NOLINT
  template <typename U>
  StatusOr(StatusOr<U>&& other);  // NOLINT
  template <typename U>
  StatusOr& operator=(StatusOr<U>&& other);
#endif  // SWIG

  // Returns a reference to our status. If this contains a T, then
  // returns Status::OK.
  const ::util::Status& status() const;

  // Returns this->status().ok()
  bool ok() const;

  // Returns a reference to our current value, or CHECK-fails if !this->ok().
  // If you need to initialize a T object from the stored value,
  // ConsumeValueOrDie() may be more efficient.
  const T& ValueOrDie() const;

  // Returns our current value, or CHECK-fails if !this->ok(). Use this if
  // you would otherwise want to say std::move(s.ValueOrDie()), for example
  // if you need to initialize a T object from the stored value and you don't
  // need subsequent access to the stored value. It uses T's move constructor,
  // if it has one, so it will work with move-only types, and will often be
  // more efficient than ValueOrDie, but may leave the stored value
  // in an arbitrary valid state.
  T ConsumeValueOrDie();

 private:
  ::util::Status status_;
  T value_;
};

////////////////////////////////////////////////////////////////////////////////
// Implementation details for StatusOr<T>

namespace internal {

class StatusOrHelper {
 public:
  // Move type-agnostic error handling to the .cc.
  static ::util::Status HandleInvalidStatusCtorArg();
  static ::util::Status HandleNullObjectCtorArg();
  static void Crash(const ::util::Status& status);

  // Customized behavior for StatusOr<T> vs. StatusOr<T*>
  template <typename T>
  struct Specialize;
};

template <typename T>
struct StatusOrHelper::Specialize {
  // For non-pointer T, a reference can never be NULL.
  static inline bool IsValueNull(const T& t) { return false; }
};

template <typename T>
struct StatusOrHelper::Specialize<T*> {
  static inline bool IsValueNull(const T* t) { return t == NULL; }
};

}  // namespace internal

template <typename T>
inline StatusOr<T>::StatusOr()
    : status_(::util::Status::UNKNOWN), value_() {}

template <typename T>
inline StatusOr<T>::StatusOr(const ::util::Status& status)
    : status_(status), value_() {
  if (status_.ok()) {
    status_ = internal::StatusOrHelper::HandleInvalidStatusCtorArg();
  }
}

template <typename T>
inline StatusOr<T>::StatusOr(const T& value)
    : status_(::util::Status::OK), value_(value) {
  if (internal::StatusOrHelper::Specialize<T>::IsValueNull(value_)) {
    status_ = internal::StatusOrHelper::HandleNullObjectCtorArg();
  }
}

template <typename T>
template <typename U>
inline StatusOr<T>::StatusOr(const StatusOr<U>& other)
    : status_(other.status_), value_(other.value_) {}

template <typename T>
template <typename U>
inline StatusOr<T>& StatusOr<T>::operator=(const StatusOr<U>& other) {
  status_ = other.status_;
  value_ = other.value_;
  return *this;
}

#ifndef SWIG
template <typename T>
inline StatusOr<T>::StatusOr(T&& value)
    : status_(::util::Status::OK), value_(std::move(value)) {
  if (internal::StatusOrHelper::Specialize<T>::IsValueNull(value_)) {
    status_ = internal::StatusOrHelper::HandleNullObjectCtorArg();
  }
}

template <typename T>
template <typename U>
inline StatusOr<T>::StatusOr(StatusOr<U>&& other)  // NOLINT
    : status_(other.status_),
      value_(std::move(other.value_)) {}

template <typename T>
template <typename U>
inline StatusOr<T>& StatusOr<T>::operator=(StatusOr<U>&& other) {
  status_ = other.status_;
  value_ = std::move(other.value_);
  return *this;
}

#endif  // SWIG

template <typename T>
inline const ::util::Status& StatusOr<T>::status() const {
  return status_;
}

template <typename T>
inline bool StatusOr<T>::ok() const {
  return status_.ok();
}

template <typename T>
inline const T& StatusOr<T>::ValueOrDie() const {
  if (!status_.ok()) {
    internal::StatusOrHelper::Crash(status_);
  }
  return value_;
}

template <typename T>
inline T StatusOr<T>::ConsumeValueOrDie() {
  if (!status_.ok()) {
    internal::StatusOrHelper::Crash(status_);
  }
  return std::move(value_);
}

}  // namespace util

#endif  // STRATUM_GLUE_STATUS_STATUSOR_H_