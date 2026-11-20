#pragma once

#include <algorithm>
#include <cstdint>

// XR predictions can move by several milliseconds even at a fixed display rate.
// Follow the display cadence, spreading small corrections over subsequent frames.
class XrFrameClock final {
  public:
    static bool discontinuity(int64_t previous, int64_t target) {
      return previous==0 || target<=previous || target-previous>100000000;
      }

    uint64_t advance(int64_t target, int64_t nominal, bool useElapsed = false) {
      const int64_t elapsed = target-lastTarget;
      if(discontinuity(lastTarget,target)) {
        lastTarget = logical = target;
        estimate = 0;
        return 0;
        }
      lastTarget = target;
      if(useElapsed) {
        const int64_t old = logical;
        logical = target;
        return uint64_t(target/1000000-old/1000000);
        }
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
