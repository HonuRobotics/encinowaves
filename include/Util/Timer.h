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

#ifndef EHUKAI_UTIL_TIMER_H
#define EHUKAI_UTIL_TIMER_H

#include "Foundation.h"

#include <chrono>

namespace ehukai {
namespace Util {

//-*****************************************************************************
//! \brief Basic real-time stopwatch on the monotonic clock. Returns elapsed
//! time in seconds.
class Timer {
public:
  //! Creates a timer which is started by default
  Timer() { start(); }

  //! Begins the timer and resets the elapsed time to zero
  void start() {
    m_start   = Clock::now();
    m_stopped = -1.0;
  }

  //! Stops the timer and records elapsed time
  double stop() { return (m_stopped = secondsSinceStart()); }

  //! Returns the amount of time elapsed.
  double elapsed() const {
    return (m_stopped >= 0.0) ? m_stopped : secondsSinceStart();
  }

private:
  using Clock = std::chrono::steady_clock;

  double secondsSinceStart() const {
    return std::chrono::duration<double>(Clock::now() - m_start).count();
  }

  Clock::time_point m_start;
  double m_stopped = -1.0;
};

}  // namespace Util
}  // namespace ehukai

#endif
