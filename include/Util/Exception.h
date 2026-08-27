//-*****************************************************************************
// Copyright 2015 Christopher Jon Horvath
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//     http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.
//-*****************************************************************************

#ifndef EHUKAI_UTIL_EXCEPTION_H
#define EHUKAI_UTIL_EXCEPTION_H

#include "Foundation.h"

#include <stdexcept>

namespace ehukai {
namespace Util {

//-*****************************************************************************
//! Base class for all exceptions in the ehukai libraries. It is most
//! commonly thrown using the EWAV_THROW and EWAV_ASSERT macros below.
class Exception : public std::runtime_error {
public:
  //! Creates an exception with an empty message string.
  Exception()
    : std::runtime_error("") {}

  //! Creates an exception with an explicit message string.
  explicit Exception(const std::string &str)
    : std::runtime_error(str) {}
};

//-*****************************************************************************
//! Prints the message to stderr and aborts. Used by the DEBUG assert
//! variants so a failed assertion leaves a traceable stack.
[[noreturn]] void EwavDebugAssertFail(const char *msg) noexcept;

//-*****************************************************************************
// This macro will cause an abort.
#define EWAV_FAIL(TEXT)                                        \
  do {                                                         \
    std::stringstream sstr;                                    \
    sstr << TEXT;                                              \
    sstr << "\nFile: " << __FILE__ << std::endl                \
         << "Line: " << __LINE__ << std::endl;                 \
    ehukai::Util::EwavDebugAssertFail(sstr.str().c_str()); \
  } while (0)

//-*****************************************************************************
//! convenient macro which may be used with std::iostream syntax
//! EWAV_THROW( "this integer: " << myInt << " is bad" )
#define EWAV_THROW(TEXT)                          \
  do {                                            \
    std::stringstream sstr;                       \
    sstr << TEXT;                                 \
    sstr << "\nFile: " << __FILE__ << std::endl   \
         << "Line: " << __LINE__ << std::endl;    \
    ehukai::Util::Exception exc(sstr.str()); \
    throw(exc);                                   \
  } while (0)

//-*****************************************************************************
#ifdef DEBUG

#define EWAV_ASSERT(COND, TEXT) \
  do {                          \
    if (!(COND)) {              \
      EWAV_FAIL(TEXT);          \
    }                           \
  } while (0)

#define EWAV_DEBUG_ASSERT(COND, TEXT) \
  do {                                \
    if (!(COND)) {                    \
      EWAV_FAIL(TEXT);                \
    }                                 \
  } while (0)

#else

#define EWAV_ASSERT(COND, TEXT) \
  do {                          \
    if (!(COND)) {              \
      EWAV_THROW(TEXT);         \
    }                           \
  } while (0)

#define EWAV_DEBUG_ASSERT(COND, TEXT) (static_cast<void>(0))

#endif

}  // namespace Util
}  // namespace ehukai

#endif
