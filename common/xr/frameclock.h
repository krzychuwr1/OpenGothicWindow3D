#pragma once

#include <algorithm>
#include <cstdint>

// XR predictions can move by several milliseconds even at a fixed display rate.
// Follow the display cadence, spreading small corrections over subsequent frames.
class XrFrameClock final {
  public:
    uint64_t advance(int64_t target, int64_t nominal) {
      const int64_t elapsed = target-lastTarget;
      if(lastTarget==0 || elapsed<=0 || elapsed>100000000) {
        lastTarget = logical = target;
        estimate = 0;
        return 0;
        }
      lastTarget = target;
      estimate = estimate==0 ? elapsed : estimate+(elapsed-estimate)/12;
      const int64_t period = nominal>0 ? nominal : estimate;
      const int64_t error = target-logical-period;
      const int64_t old = logical;
      logical += std::max(period/2,period+error/12);
      return uint64_t(logical/1000000-old/1000000);
      }

    int64_t simulationMillis() const { return logical/1000000; }
    int64_t lastTarget = 0;
  private:
    int64_t logical = 0;
    int64_t estimate = 0;
  };
