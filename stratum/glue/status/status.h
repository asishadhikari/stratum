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


#ifndef STRATUM_GLUE_STATUS_STATUS_H_
#define STRATUM_GLUE_STATUS_STATUS_H_

#include <iosfwd>
#include <string>

#include "google/protobuf/stubs/atomicops.h"
#include "third_party/stratum/glue/logging.h"

// TODO: Move to Abseil-status when it is available.
//
namespace util {
namespace error {
enum Code {
  OK = 0,
  CANCELLED = 1,
  UNKNOWN = 2,
  INVALID_ARGUMENT = 3,
  DEADLINE_EXCEEDED = 4,
  NOT_FOUND = 5,
  ALREADY_EXISTS = 6,
  PERMISSION_DENIED = 7,
  UNAUTHENTICATED = 16,
  RESOURCE_EXHAUSTED = 8,
  FAILED_PRECONDITION = 9,
  ABORTED = 10,
  OUT_OF_RANGE = 11,
  UNIMPLEMENTED = 12,
  INTERNAL = 13,
  UNAVAILABLE = 14,
  DATA_LOSS = 15,
  DO_NOT_USE_RESERVED_FOR_FUTURE_EXPANSION_USE_DEFAULT_IN_SWITCH_INSTEAD_ = 20,
  // **DO NOT ADD ANYTHING TO THIS**
  // This list is owned by google3 core language team.
};
static const enum Code Code_MIN = Code::OK;
static const enum Code Code_MAX = Code::DATA_LOSS;
inline bool Code_IsValid(int c) { return (c >= Code_MIN) && (c <= Code_MAX); }
}  // end namespace error
}  // end namespace util

namespace util {

// An ErrorSpace is a collection of related numeric error codes.  For
// example, all Posix errno values may be placed in the same
// ErrorSpace, all bigtable errors may be placed in the same
// ErrorSpace, etc.
//
// We recommend that new APIs use the canonical error space (and the
// corresponding two-argument constructor below) instead of creating a
// new error space per API
class ErrorSpace;

// Used by ::util::Status to store proto payload. Not for public use
class InternalStatusPayload;

// Returned Status objects may not be ignored.
// Note: Disabled for SWIG as it doesn't parse attributes correctly.
#if defined(CLANG_WARN_UNUSED_RESULT) && !defined(SWIG)
class CLANG_WARN_UNUSED_RESULT Status;
#endif

class Status final {
 public:
  // Creates a "successful" status.
  Status();

  // Create a status in the canonical error space with the specified
  // code, and error message.  If "code == 0", error_message is
  // ignored and a Status object identical to Status::OK is
  // constructed.
  Status(::util::error::Code code, const std::string& error_message);

  // Creates a status in the specified "space", "code" and the
  // associated error message and optional payload.  If "code == 0",
  // (space,msg,payload) are ignored and a Status object identical to
  // Status::OK is constructed.
  //
  // New APIs should use the canonical error space and the preceding
  // two-argument constructor.
  //
  // REQUIRES: space != NULL
  Status(const ErrorSpace* space, int code, const std::string& msg);

  Status(const Status&);
  Status& operator=(const Status& x);
  ~Status();

  // For backwards compatibility, provide aliases of some the
  // canonical error codes defined in codes.proto.
  enum GenericCode {
    OK_CODE = 0,         // No error
    CANCELLED_CODE = 1,  // For cancelled operations
    UNKNOWN_CODE = 2,    // For unknown spaces/codes
  };

  // Some pre-defined Status objects
  static const Status& OK;  // Identical to 0-arg constructor
  static const Status& CANCELLED;
  static const Status& UNKNOWN;

  // Return the canonical error space.
  static const ErrorSpace* canonical_space();

  // Store the specified error and optional payload in this Status object.
  // If "code == 0", (space,msg,payload) are ignored and a Status object
  // identical to Status::OK is constructed.  Sets any payload to NULL.
  // REQUIRES: code == 0 OR space != NULL
  void SetError(const ErrorSpace* space, int code, const std::string& msg);

  // If "ok()", stores "new_status" into *this.  If "!ok()", preserves
  // the current "error_code()/error_message()/error_space()/payload()",
  // but may augment with additional information about "new_status".
  //
  // Convenient way of keeping track of the first error encountered.
  // Instead of:
  //   if (overall_status.ok()) overall_status = new_status
  // Use:
  //   overall_status.Update(new_status);
  void Update(const Status& new_status);

  // Clear this status object to contain the OK code and no error message.
  void Clear();

  // Accessor
  bool ok() const MUST_USE_RESULT;
  int error_code() const;
  const std::string& error_message() const;
  const ErrorSpace* error_space() const;

  // Returns the canonical code for this Status value.  Automatically
  // converts to the canonical space if necessary.
  ::util::error::Code CanonicalCode() const;

  // Sets the equivalent canonical code for a Status with a
  // non-canonical error space.
  void SetCanonicalCode(int canonical_code);

  bool operator==(const Status& x) const;
  bool operator!=(const Status& x) const;

  // Returns true iff this->CanonicalCode() == expected.
  bool Matches(::util::error::Code expected) const;

  // Returns true iff this has the same error_space, error_code,
  // and canonical_code as "x".  I.e., the two Status objects are
  // identical except possibly for the error message and payload.
  bool Matches(const Status& x) const;

  // Return a combination of the error code name and message.
  std::string ToString() const;

  // Returns a copy of the status object in the canonical error space.  This
  // will use the canonical code from the status protocol buffer (if present) or
  // the result of passing this status to the ErrorSpace CanonicalCode method.
  Status ToCanonical() const;

  // If this->Matches(x), return without doing anything.
  // Else die with an appropriate error message.
  void CheckMatches(const Status& x) const;

  // Ignores any errors. This method does nothing except potentially suppress
  // complaints from any tools that are checking that errors are not dropped on
  // the floor.
  void IgnoreError() const;

  // Swap the contents of *this with *that
  void Swap(util::Status* that) {
    Rep* t = this->rep_;
    this->rep_ = that->rep_;
    that->rep_ = t;
  }

  // Returns a copy of the status object with error message and
  // payload stripped off. Useful for comparing against expected
  // status when error message might vary, e.g.
  //     EXPECT_EQ(expected_status, real_status.StripMessage());
  Status StripMessage() const;

 private:
  // Use atomic ops from the protobuf library.
  typedef google::protobuf::internal::Atomic32 Atomic32;

  inline static bool RefCountDec(volatile Atomic32* ptr) {
    return google::protobuf::internal::Barrier_AtomicIncrement(ptr, -1) != 0;
  }

  inline static void RefCountInc(volatile Atomic32* ptr) {
    google::protobuf::internal::NoBarrier_AtomicIncrement(ptr, 1);
  }

  inline static Atomic32 NoBarrier_Load(volatile const Atomic32* ptr) {
    return google::protobuf::internal::NoBarrier_Load(ptr);
  }

  inline static bool RefCountIsOne(const volatile Atomic32* ptr) {
    return google::protobuf::internal::Acquire_Load(ptr) == 1;
  }

  typedef InternalStatusPayload Payload;

  // Reference-counted representation
  static const Atomic32 kGlobalRef = ~static_cast<Atomic32>(0);
  struct Rep {
    Atomic32 ref;
    int code;                     // code >= 0
    int canonical_code;           // 0 means use space to calculate
    const ErrorSpace* space_ptr;  // NULL means canonical_space()
    std::string* message_ptr;     // NULL means empty
    Payload* payload;             // If non-NULL, owned by this object
  };
  Rep* rep_;  // Never NULL.

  static void UnrefSlow(Rep*);
  inline static void Ref(Rep* r) {
    // Do not adjust refs for globals
    if (NoBarrier_Load(&r->ref) != kGlobalRef) {
      RefCountInc(&r->ref);
    }
  }
  inline static void Unref(Rep* r) {
    // Do not adjust refs for globals
    if (NoBarrier_Load(&r->ref) != kGlobalRef) {
      UnrefSlow(r);
    }
  }

  void InternalSet(const ErrorSpace* space, int code, const std::string& msg,
                   Payload* payload, int canonical_code);

  // Returns the canonical code from the status protocol buffer (if present) or
  // the result of passing this status to the ErrorSpace CanonicalCode method.
  int RawCanonicalCode() const;

  // REQUIRES: !ok()
  // Ensures rep_ is not shared with any other Status.
  void PrepareToModify();

  // Takes ownership of payload.
  static Rep* NewRep(const ErrorSpace*, int, const std::string&,
                     Payload* payload, int canonical_code);
  // Takes ownership of payload.
  static void ResetRep(Rep* rep, const ErrorSpace*, int, const std::string&,
                       Payload* payload, int canonical_code);
  static bool EqualsSlow(const ::util::Status& a, const ::util::Status& b);

  // Machinery for linker initialization of the global Status objects.
  struct Pod;
  static Rep global_reps[];
  static const Pod globals[];
  static const std::string* EmptyString();
};

// Base class for all error spaces.  An error space is a collection
// of related error codes.  All error codes are non-zero.
// Zero always means success.
//
// NOTE:
// All ErrorSpace objects must be created before the end of the module
// initializer phase (see "base/googleinit.h"). In particular, ErrorSpace
// objects should not be lazily created unless some mechanism forces this to
// occur in the module initializer phase. In most cases, ErrorSpace objects
// should just be created by a module initializer e.g.:
//
//     REGISTER_MODULE_INITIALIZER(ThisModule, {
//         ThisModule::InitErrorSpace();
//         ... other module initialization
//     });
//
// This rule ensures that ErrorSpace::Find(), which cannot be called until
// after the module initializer phase, will see a complete ErrorSpace
// registry.
//
class ErrorSpace {
 public:
  // Return the name of this error space
  const std::string& SpaceName() const { return name_; }

  // Return a string corresponding to the specified error code.
  virtual std::string String(int code) const = 0;

  // Return the equivalent canonical code for the given status. ErrorSpace
  // implementations should override this method to provide a custom
  // mapping. The default is to always return UNKNOWN. It is an error to pass a
  // Status that does not belong to this space; ErrorSpace implementations
  // should return UNKNOWN in that case.
  virtual ::util::error::Code CanonicalCode(
      const ::util::Status& status) const {
    return error::UNKNOWN;
  }

  // Find the error-space with the specified name.  Return the
  // space object, or NULL if not found.
  //
  // NOTE: Do not call Find() until after InitGoogle() returns.
  // Otherwise, some module intializers that register error spaces may not
  // have executed and Find() might not locate the error space of
  // interest.
  static ErrorSpace* Find(const std::string& name);

  // ErrorSpace is neither copyable nor movable.
  ErrorSpace(const ErrorSpace&) = delete;
  ErrorSpace& operator=(const ErrorSpace&) = delete;

 protected:
  explicit ErrorSpace(const char* name);

  // Prevent deletions of ErrorSpace objects by random clients
  virtual ~ErrorSpace();

 private:
  const std::string name_;
};

// ::util::Status success comparison.
// This is better than CHECK((val).ok()) because the embedded
// error string gets printed by the CHECK_EQ.
#define CHECK_OK(val) CHECK_EQ(::util::Status::OK, (val))
#define QCHECK_OK(val) QCHECK_EQ(::util::Status::OK, (val))
#define DCHECK_OK(val) DCHECK_EQ(::util::Status::OK, (val))

// -----------------------------------------------------------------
// Implementation details follow

inline Status::Status() : rep_(&global_reps[0]) {}

inline Status::Status(const Status& x) : rep_(x.rep_) { Ref(rep_); }

inline Status& Status::operator=(const Status& x) {
  Rep* old_rep = rep_;
  if (x.rep_ != old_rep) {
    Ref(x.rep_);
    rep_ = x.rep_;
    Unref(old_rep);
  }
  return *this;
}

inline void Status::Update(const Status& new_status) {
  if (ok()) {
    *this = new_status;
  }
}

inline Status::~Status() { Unref(rep_); }

inline bool Status::ok() const { return rep_->code == 0; }

inline int Status::error_code() const { return rep_->code; }

inline const std::string& Status::error_message() const {
  return rep_->message_ptr ? *rep_->message_ptr : *EmptyString();
}

inline const ErrorSpace* Status::error_space() const {
  return rep_->space_ptr ? rep_->space_ptr : canonical_space();
}

inline bool Status::Matches(const Status& x) const {
  return (this->error_code() == x.error_code() &&
          this->error_space() == x.error_space() &&
          this->RawCanonicalCode() == x.RawCanonicalCode());
}

inline bool Status::operator==(const Status& x) const {
  return (this->rep_ == x.rep_) || EqualsSlow(*this, x);
}

inline bool Status::operator!=(const Status& x) const { return !(*this == x); }

inline bool Status::Matches(::util::error::Code expected) const {
  return CanonicalCode() == expected;
}

extern std::ostream& operator<<(std::ostream& os, const Status& x);

// Returns an OK status, equivalent to a default constructed instance. This was
// recently introduced in google3 in CL/132673373 and now everything is being
// moved to use this instead.
Status OkStatus();

#ifndef SWIG
inline Status OkStatus() { return Status(); }
#endif  // SWIG

}  // namespace util

#endif  // STRATUM_GLUE_STATUS_STATUS_H_