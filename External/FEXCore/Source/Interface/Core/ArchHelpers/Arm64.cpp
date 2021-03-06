#include <FEXCore/Utils/LogManager.h>

#include <atomic>
#include <stdint.h>

#include <signal.h>

namespace FEXCore::ArchHelpers::Arm64 {
static uint64_t LoadAcquire64(uint64_t Addr) {
  std::atomic<uint64_t> *Atom = reinterpret_cast<std::atomic<uint64_t>*>(Addr);
  return Atom->load(std::memory_order_acquire);
}

static bool StoreCAS64(uint64_t &Expected, uint64_t Val, uint64_t Addr) {
  std::atomic<uint64_t> *Atom = reinterpret_cast<std::atomic<uint64_t>*>(Addr);
  return Atom->compare_exchange_strong(Expected, Val);
}

static uint32_t LoadAcquire32(uint64_t Addr) {
  std::atomic<uint32_t> *Atom = reinterpret_cast<std::atomic<uint32_t>*>(Addr);
  return Atom->load(std::memory_order_acquire);
}

static bool StoreCAS32(uint32_t &Expected, uint32_t Val, uint64_t Addr) {
  std::atomic<uint32_t> *Atom = reinterpret_cast<std::atomic<uint32_t>*>(Addr);
  return Atom->compare_exchange_strong(Expected, Val);
}

static uint8_t LoadAcquire8(uint64_t Addr) {
  std::atomic<uint8_t> *Atom = reinterpret_cast<std::atomic<uint8_t>*>(Addr);
  return Atom->load(std::memory_order_acquire);
}

static bool StoreCAS8(uint8_t &Expected, uint8_t Val, uint64_t Addr) {
  std::atomic<uint8_t> *Atom = reinterpret_cast<std::atomic<uint8_t>*>(Addr);
  return Atom->compare_exchange_strong(Expected, Val);
}

bool HandleCASPAL(void *_mcontext, void *_info, uint32_t Instr) {
  mcontext_t* mcontext = reinterpret_cast<mcontext_t*>(_mcontext);
  siginfo_t* info = reinterpret_cast<siginfo_t*>(_info);

  if (info->si_code != BUS_ADRALN) {
    // This only handles alignment problems
    return false;
  }

  uint32_t Size = (Instr >> 30) & 1;

  uint32_t DesiredReg1 = Instr & 0b11111;
  uint32_t DesiredReg2 = DesiredReg1 + 1;
  uint32_t ExpectedReg1 = (Instr >> 16) & 0b11111;
  uint32_t ExpectedReg2 = ExpectedReg1 + 1;
  uint32_t AddressReg = (Instr >> 5) & 0b11111;

  if (Size == 0) {
    // 32bit
    uint64_t Addr = mcontext->regs[AddressReg];

    uint32_t DesiredLower = mcontext->regs[DesiredReg1];
    uint32_t DesiredUpper = mcontext->regs[DesiredReg2];

    uint32_t ExpectedLower = mcontext->regs[ExpectedReg1];
    uint32_t ExpectedUpper = mcontext->regs[ExpectedReg2];

    // Cross-cacheline CAS doesn't work on ARM
    // It isn't even guaranteed to work on x86
    // Intel will do a "split lock" which locks the full bus
    // AMD will tear instead
    // Both cross-cacheline and cross 16byte both need dual CAS loops that can tear
    // ARMv8.4 LSE2 solves all atomic issues except cross-cacheline

    uint64_t AlignmentMask = 0b1111;
    if ((Addr & AlignmentMask) > 8) {
      uint64_t Alignment = Addr & 0b111;
      Addr &= ~0b111ULL;
      uint64_t AddrUpper = Addr + 8;

      // Crosses a 16byte boundary
      // Need to do 256bit atomic, but since that doesn't exist we need to do a dual CAS loop
      __uint128_t Mask = ~0ULL;
      Mask <<= Alignment * 8;
      __uint128_t NegMask = ~Mask;
      __uint128_t TmpExpected{};
      __uint128_t TmpDesired{};

      __uint128_t Desired = DesiredUpper;
      Desired <<= 32;
      Desired |= DesiredLower;
      Desired <<= Alignment * 8;

      __uint128_t Expected = ExpectedUpper;
      Expected <<= 32;
      Expected |= ExpectedLower;
      Expected <<= Alignment * 8;

      while (1) {
        __uint128_t LoadOrderUpper = LoadAcquire64(AddrUpper);
        LoadOrderUpper <<= 64;
        __uint128_t TmpActual = LoadOrderUpper | LoadAcquire64(Addr);

        // Set up expected
        TmpExpected = TmpActual;
        TmpExpected &= NegMask;
        TmpExpected |= Expected;

        // Set up desired
        TmpDesired = TmpExpected;
        TmpDesired &= NegMask;
        TmpDesired |= Desired;

        uint64_t TmpExpectedLower = TmpExpected;
        uint64_t TmpExpectedUpper = TmpExpected >> 64;

        uint64_t TmpDesiredLower = TmpDesired;
        uint64_t TmpDesiredUpper = TmpDesired >> 64;

        if (TmpExpected == TmpActual) {
          if (StoreCAS64(TmpExpectedUpper, TmpDesiredUpper, AddrUpper)) {
            if (StoreCAS64(TmpExpectedLower, TmpDesiredLower, Addr)) {
              // Stored successfully
              return true;
            }
            else {
              // CAS managed to tear, we can't really solve this
              // Continue down the path to let the guest know values weren't expected
            }
          }

          TmpExpected = TmpExpectedUpper;
          TmpExpected <<= 64;
          TmpExpected |= TmpExpectedLower;
        }
        else {
          // Mismatch up front
          TmpExpected = TmpActual;
        }

        // Not successful
        // Now we need to check the results to see if we need to try again
        __uint128_t FailedResultOurBits = TmpExpected & Mask;
        __uint128_t FailedResultNotOurBits = TmpExpected & NegMask;

        __uint128_t FailedDesiredOurBits = TmpDesired & Mask;
        __uint128_t FailedDesiredNotOurBits = TmpDesired & NegMask;
        if ((FailedResultNotOurBits ^ FailedDesiredNotOurBits) != 0) {
          // If the bits changed that weren't part of our regular CAS then we need to try again
          continue;
        }
        if ((FailedResultOurBits ^ FailedDesiredOurBits) != 0) {
          // If the bits changed that we were wanting to change then we have failed and can return
          // We need to extract the bits and return them in EXPECTED
          uint64_t FailedResult = FailedResultOurBits >> (Alignment * 8);
          mcontext->regs[ExpectedReg1] = FailedResult & ~0U;
          mcontext->regs[ExpectedReg2] = FailedResult >> 32;
          return true;
        }
      }
    }
    else {
      // Fits within a 16byte region
      uint64_t Alignment = Addr & 0b1111;
      Addr &= ~0b1111ULL;
      std::atomic<__uint128_t> *Atomic128 = reinterpret_cast<std::atomic<__uint128_t>*>(Addr);

      __uint128_t Mask = ~0ULL;
      Mask <<= Alignment * 8;
      __uint128_t NegMask = ~Mask;
      __uint128_t TmpExpected{};
      __uint128_t TmpDesired{};

      __uint128_t Desired = (uint64_t)DesiredUpper << 32 | DesiredLower;
      Desired <<= Alignment * 8;

      __uint128_t Expected = (uint64_t)ExpectedUpper << 32 | ExpectedLower;
      Expected <<= Alignment * 8;

      while (1) {
        TmpExpected = Atomic128->load();

        // Set up expected
        TmpExpected &= NegMask;
        TmpExpected |= Expected;

        // Set up desired
        TmpDesired = TmpExpected;
        TmpDesired &= NegMask;
        TmpDesired |= Desired;

        bool CASResult = Atomic128->compare_exchange_strong(TmpExpected, TmpDesired);
        if (CASResult) {
          // Successful, so we are done
          return true;
        }
        else {
          // Not successful
          // Now we need to check the results to see if we need to try again
          __uint128_t FailedResultOurBits = TmpExpected & Mask;
          __uint128_t FailedResultNotOurBits = TmpExpected & NegMask;

          __uint128_t FailedDesiredOurBits = TmpDesired & Mask;
          __uint128_t FailedDesiredNotOurBits = TmpDesired & NegMask;
          if ((FailedResultNotOurBits ^ FailedDesiredNotOurBits) != 0) {
            // If the bits changed that weren't part of our regular CAS then we need to try again
            continue;
          }
          if ((FailedResultOurBits ^ FailedDesiredOurBits) != 0) {
            // If the bits changed that we were wanting to change then we have failed and can return
            // We need to extract the bits and return them in EXPECTED
            uint64_t FailedResult = FailedResultOurBits >> (Alignment * 8);
            mcontext->regs[ExpectedReg1] = FailedResult & ~0U;
            mcontext->regs[ExpectedReg2] = FailedResult >> 32;
            return true;
          }
        }
      }
    }
  }
  return false;
}

bool HandleCASAL(void *_mcontext, void *_info, uint32_t Instr) {
  mcontext_t* mcontext = reinterpret_cast<mcontext_t*>(_mcontext);
  siginfo_t* info = reinterpret_cast<siginfo_t*>(_info);

  if (info->si_code != BUS_ADRALN) {
    // This only handles alignment problems
    return false;
  }

  uint32_t Size = 1 << (Instr >> 30);

  uint32_t DesiredReg = Instr & 0b11111;
  uint32_t ExpectedReg = (Instr >> 16) & 0b11111;
  uint32_t AddressReg = (Instr >> 5) & 0b11111;

  uint64_t Addr = mcontext->regs[AddressReg];

  // Cross-cacheline CAS doesn't work on ARM
  // It isn't even guaranteed to work on x86
  // Intel will do a "split lock" which locks the full bus
  // AMD will tear instead
  // Both cross-cacheline and cross 16byte both need dual CAS loops that can tear
  // ARMv8.4 LSE2 solves all atomic issues except cross-cacheline

  // 8bit can't be unaligned
  // Only need to handle 16, 32, 64
  if (Size == 2) {
    // 16 bit
    uint64_t AlignmentMask = 0b1111;
    if ((Addr & AlignmentMask) == 15) {
      // Address crosses over 16byte or 64byte threshold
      // Need a dual 8bit CAS loop
      uint64_t AddrUpper = Addr + 1;

      uint16_t Desired = mcontext->regs[DesiredReg];
      uint16_t Expected = mcontext->regs[ExpectedReg];

      uint8_t DesiredLower = Desired;
      uint8_t DesiredUpper = Desired >> 8;

      uint8_t ExpectedLower = Expected;
      uint8_t ExpectedUpper = Expected >> 8;

      uint8_t ActualUpper{};
      uint8_t ActualLower{};
      // Careful ordering here
      ActualUpper = LoadAcquire8(AddrUpper);
      ActualLower = LoadAcquire8(Addr);
      if (ActualUpper == ExpectedUpper &&
          ActualLower == ExpectedLower) {
        if (StoreCAS8(ExpectedUpper, DesiredUpper, AddrUpper)) {
          if (StoreCAS8(ExpectedLower, DesiredLower, Addr)) {
            // Stored successfully
            return true;
          }
          else {
            // CAS managed to tear, we can't really solve this
            // Continue down the path to let the guest know values weren't expected
          }
        }

        ActualLower = ExpectedLower;
        ActualUpper = ExpectedUpper;
      }

      // If the bits changed that we were wanting to change then we have failed and can return
      // We need to extract the bits and return them in EXPECTED
      uint16_t FailedResult = ActualUpper;
      FailedResult <<= 8;
      FailedResult |= ActualLower;
      mcontext->regs[ExpectedReg] = FailedResult;
      return true;
    }
    else {
      AlignmentMask = 0b111;
      if ((Addr & AlignmentMask) == 7) {
        // Crosses 8byte boundary
        // Needs 128bit CAS
        // Fits within a 16byte region
        uint64_t Alignment = Addr & 0b1111;
        Addr &= ~0b1111ULL;
        std::atomic<__uint128_t> *Atomic128 = reinterpret_cast<std::atomic<__uint128_t>*>(Addr);

        __uint128_t Mask = ~0U;
        Mask <<= Alignment * 8;
        __uint128_t NegMask = ~Mask;
        __uint128_t TmpExpected{};
        __uint128_t TmpDesired{};

        __uint128_t Desired = mcontext->regs[DesiredReg] & 0xFFFF;
        Desired <<= Alignment * 8;

        __uint128_t Expected = mcontext->regs[ExpectedReg] & 0xFFFF;
        Expected <<= Alignment * 8;

        while (1) {
          TmpExpected = Atomic128->load();

          // Set up expected
          TmpExpected &= NegMask;
          TmpExpected |= Expected;

          // Set up desired
          TmpDesired = TmpExpected;
          TmpDesired &= NegMask;
          TmpDesired |= Desired;

          bool CASResult = Atomic128->compare_exchange_strong(TmpExpected, TmpDesired);
          if (CASResult) {
            // Successful, so we are done
            return true;
          }
          else {
            // Not successful
            // Now we need to check the results to see if we need to try again
            __uint128_t FailedResultOurBits = TmpExpected & Mask;
            __uint128_t FailedResultNotOurBits = TmpExpected & NegMask;

            __uint128_t FailedDesiredOurBits = TmpDesired & Mask;
            __uint128_t FailedDesiredNotOurBits = TmpDesired & NegMask;
            if ((FailedResultNotOurBits ^ FailedDesiredNotOurBits) != 0) {
              // If the bits changed that weren't part of our regular CAS then we need to try again
              continue;
            }
            if ((FailedResultOurBits ^ FailedDesiredOurBits) != 0) {
              // If the bits changed that we were wanting to change then we have failed and can return
              // We need to extract the bits and return them in EXPECTED
              uint32_t FailedResult = FailedResultOurBits >> (Alignment * 8);
              mcontext->regs[ExpectedReg] = FailedResult;
              return true;
            }
          }
        }
      }
      else {
        AlignmentMask = 0b11;
        if ((Addr & AlignmentMask) == 3) {
          // Crosses 4byte boundary
          // Needs 64bit CAS
          uint64_t Alignment = Addr & AlignmentMask;
          Addr &= ~AlignmentMask;

          uint64_t Mask = 0xFFFF;
          Mask <<= Alignment * 8;

          uint64_t NegMask = ~Mask;

          uint64_t TmpExpected{};
          uint64_t TmpDesired{};

          uint64_t Desired = mcontext->regs[DesiredReg] & 0xFFFF;
          Desired <<= Alignment * 8;

          uint64_t Expected = mcontext->regs[ExpectedReg] & 0xFFFF;
          Expected <<= Alignment * 8;

          std::atomic<uint64_t> *Atomic = reinterpret_cast<std::atomic<uint64_t>*>(Addr);
          while (1) {
            TmpExpected = Atomic->load();

            // Set up expected
            TmpExpected &= NegMask;
            TmpExpected |= Expected;

            // Set up desired
            TmpDesired = TmpExpected;
            TmpDesired &= NegMask;
            TmpDesired |= Desired;

            bool CASResult = Atomic->compare_exchange_strong(TmpExpected, TmpDesired);
            if (CASResult) {
              // Successful, so we are done
              return true;
            }
            else {
              // Not successful
              // Now we need to check the results to see if we can try again
              uint64_t FailedResultOurBits = TmpExpected & Mask;
              uint64_t FailedResultNotOurBits = TmpExpected & NegMask;

              uint64_t FailedDesiredOurBits = TmpDesired & Mask;
              uint64_t FailedDesiredNotOurBits = TmpDesired & NegMask;

              if ((FailedResultNotOurBits ^ FailedDesiredNotOurBits) != 0) {
                // If the bits changed that weren't part of our regular CAS then we need to try again
                continue;
              }
              if ((FailedResultOurBits ^ FailedDesiredOurBits) != 0) {
                // If the bits changed that we were wanting to change then we have failed and can return
                // We need to extract the bits and return them in EXPECTED
                uint16_t FailedResult = FailedResultOurBits >> (Alignment * 8);
                mcontext->regs[ExpectedReg] = FailedResult;
                return true;
              }
            }
          }
        }
        else {
          // Fits within 4byte boundary
          // Only needs 32bit CAS
          // Only alignment offset will be 1 here
          uint64_t Alignment = Addr & AlignmentMask;
          Addr &= ~AlignmentMask;

          uint32_t Mask = 0xFFFF;
          Mask <<= Alignment * 8;

          uint32_t NegMask = ~Mask;

          uint32_t TmpExpected{};
          uint32_t TmpDesired{};

          uint32_t Desired = mcontext->regs[DesiredReg] & 0xFFFF;
          Desired <<= Alignment * 8;

          uint32_t Expected = mcontext->regs[ExpectedReg] & 0xFFFF;
          Expected <<= Alignment * 8;

          std::atomic<uint32_t> *Atomic = reinterpret_cast<std::atomic<uint32_t>*>(Addr);
          while (1) {
            TmpExpected = Atomic->load();

            // Set up expected
            TmpExpected &= NegMask;
            TmpExpected |= Expected;

            // Set up desired
            TmpDesired = TmpExpected;
            TmpDesired &= NegMask;
            TmpDesired |= Desired;

            bool CASResult = Atomic->compare_exchange_strong(TmpExpected, TmpDesired);
            if (CASResult) {
              // Successful, so we are done
              return true;
            }
            else {
              // Not successful
              // Now we need to check the results to see if we can try again
              uint32_t FailedResultOurBits = TmpExpected & Mask;
              uint32_t FailedResultNotOurBits = TmpExpected & NegMask;

              uint32_t FailedDesiredOurBits = TmpDesired & Mask;
              uint32_t FailedDesiredNotOurBits = TmpDesired & NegMask;

              if ((FailedResultNotOurBits ^ FailedDesiredNotOurBits) != 0) {
                // If the bits changed that weren't part of our regular CAS then we need to try again
                continue;
              }
              if ((FailedResultOurBits ^ FailedDesiredOurBits) != 0) {
                // If the bits changed that we were wanting to change then we have failed and can return
                // We need to extract the bits and return them in EXPECTED
                uint16_t FailedResult = FailedResultOurBits >> (Alignment * 8);
                mcontext->regs[ExpectedReg] = FailedResult;
                return true;
              }
            }
          }
        }
      }
    }
  }
  else if (Size == 4) {
    // 32bit
    uint64_t AlignmentMask = 0b1111;
    if ((Addr & AlignmentMask) > 12) {
      // Address crosses over 16byte threshold
      // Needs dual 4 byte CAS loop
      uint64_t Alignment = Addr & 0b11;
      Addr &= ~0b11;

      uint64_t AddrUpper = Addr + 4;

      uint64_t Desired = mcontext->regs[DesiredReg] & ~0U;
      uint64_t Expected = mcontext->regs[ExpectedReg] & ~0U;

      Desired <<= Alignment * 8;
      Expected <<= Alignment * 8;

      uint64_t Mask = ~0U;
      Mask <<= Alignment * 8;
      uint64_t NegMask = ~Mask;

      // Careful ordering here
      while (1) {
        uint64_t LoadOrderUpper = LoadAcquire32(AddrUpper);
        LoadOrderUpper <<= 32;
        uint64_t TmpActual = LoadOrderUpper | LoadAcquire32(Addr);

        uint64_t TmpExpected = TmpActual;
        TmpExpected &= NegMask;
        TmpExpected |= Expected;

        uint64_t TmpDesired = TmpExpected;
        TmpDesired &= NegMask;
        TmpDesired |= Desired;

        if (TmpExpected == TmpActual) {
          uint32_t TmpExpectedLower = TmpExpected;
          uint32_t TmpExpectedUpper = TmpExpected >> 32;

          uint32_t TmpDesiredLower = TmpDesired;
          uint32_t TmpDesiredUpper = TmpDesired >> 32;

          if (StoreCAS32(TmpExpectedUpper, TmpDesiredUpper, AddrUpper)) {
            if (StoreCAS32(TmpExpectedLower, TmpDesiredLower, Addr)) {
              // Stored successfully
              return true;
            }
            else {
              // CAS managed to tear, we can't really solve this
              // Continue down the path to let the guest know values weren't expected
            }
          }

          TmpExpected = TmpExpectedUpper;
          TmpExpected <<= 32;
          TmpExpected |= TmpExpectedLower;
        }
        else {
          // Mismatch up front
          TmpExpected = TmpActual;
        }

        // Not successful
        // Now we need to check the results to see if we need to try again
        uint64_t FailedResultOurBits = TmpExpected & Mask;
        uint64_t FailedResultNotOurBits = TmpExpected & NegMask;

        uint64_t FailedDesiredOurBits = TmpDesired & Mask;
        uint64_t FailedDesiredNotOurBits = TmpDesired & NegMask;
        if ((FailedResultNotOurBits ^ FailedDesiredNotOurBits) != 0) {
          // If the bits changed that weren't part of our regular CAS then we need to try again
          continue;
        }
        if ((FailedResultOurBits ^ FailedDesiredOurBits) != 0) {
          // If the bits changed that we were wanting to change then we have failed and can return
          // We need to extract the bits and return them in EXPECTED
          uint32_t FailedResult = FailedResultOurBits >> (Alignment * 8);
          mcontext->regs[ExpectedReg] = FailedResult;
          return true;
        }
      }
    }
    else {
      AlignmentMask = 0b111;
      if ((Addr & AlignmentMask) >= 5) {
        // Crosses 8byte boundary
        // Needs 128bit CAS
        // Fits within a 16byte region
        uint64_t Alignment = Addr & 0b1111;
        Addr &= ~0b1111ULL;
        std::atomic<__uint128_t> *Atomic128 = reinterpret_cast<std::atomic<__uint128_t>*>(Addr);

        __uint128_t Mask = ~0U;
        Mask <<= Alignment * 8;
        __uint128_t NegMask = ~Mask;
        __uint128_t TmpExpected{};
        __uint128_t TmpDesired{};

        __uint128_t Desired = mcontext->regs[DesiredReg] & ~0U;
        Desired <<= Alignment * 8;

        __uint128_t Expected = mcontext->regs[ExpectedReg] & ~0U;
        Expected <<= Alignment * 8;

        while (1) {
          TmpExpected = Atomic128->load();

          // Set up expected
          TmpExpected &= NegMask;
          TmpExpected |= Expected;

          // Set up desired
          TmpDesired = TmpExpected;
          TmpDesired &= NegMask;
          TmpDesired |= Desired;

          bool CASResult = Atomic128->compare_exchange_strong(TmpExpected, TmpDesired);
          if (CASResult) {
            // Successful, so we are done
            return true;
          }
          else {
            // Not successful
            // Now we need to check the results to see if we need to try again
            __uint128_t FailedResultOurBits = TmpExpected & Mask;
            __uint128_t FailedResultNotOurBits = TmpExpected & NegMask;

            __uint128_t FailedDesiredOurBits = TmpDesired & Mask;
            __uint128_t FailedDesiredNotOurBits = TmpDesired & NegMask;
            if ((FailedResultNotOurBits ^ FailedDesiredNotOurBits) != 0) {
              // If the bits changed that weren't part of our regular CAS then we need to try again
              continue;
            }
            if ((FailedResultOurBits ^ FailedDesiredOurBits) != 0) {
              // If the bits changed that we were wanting to change then we have failed and can return
              // We need to extract the bits and return them in EXPECTED
              uint32_t FailedResult = FailedResultOurBits >> (Alignment * 8);
              mcontext->regs[ExpectedReg] = FailedResult;
              return true;
            }
          }
        }
      }
      else {
        // Fits within 8byte boundary
        // Only needs 64bit CAS
        // Alignments can be [1,5)
        uint64_t Alignment = Addr & AlignmentMask;
        Addr &= ~AlignmentMask;

        uint64_t Mask = ~0U;
        Mask <<= Alignment * 8;

        uint64_t NegMask = ~Mask;

        uint64_t TmpExpected{};
        uint64_t TmpDesired{};

        uint64_t Desired = mcontext->regs[DesiredReg] & ~0U;
        Desired <<= Alignment * 8;

        uint64_t Expected = mcontext->regs[ExpectedReg] & ~0U;
        Expected <<= Alignment * 8;

        std::atomic<uint64_t> *Atomic = reinterpret_cast<std::atomic<uint64_t>*>(Addr);
        while (1) {
          TmpExpected = Atomic->load();

          // Set up expected
          TmpExpected &= NegMask;
          TmpExpected |= Expected;

          // Set up desired
          TmpDesired = TmpExpected;
          TmpDesired &= NegMask;
          TmpDesired |= Desired;

          bool CASResult = Atomic->compare_exchange_strong(TmpExpected, TmpDesired);
          if (CASResult) {
            // Successful, so we are done
            return true;
          }
          else {
            // Not successful
            // Now we need to check the results to see if we can try again
            uint64_t FailedResultOurBits = TmpExpected & Mask;
            uint64_t FailedResultNotOurBits = TmpExpected & NegMask;

            uint64_t FailedDesiredOurBits = TmpDesired & Mask;
            uint64_t FailedDesiredNotOurBits = TmpDesired & NegMask;

            if ((FailedResultNotOurBits ^ FailedDesiredNotOurBits) != 0) {
              // If the bits changed that weren't part of our regular CAS then we need to try again
              continue;
            }
            if ((FailedResultOurBits ^ FailedDesiredOurBits) != 0) {
              // If the bits changed that we were wanting to change then we have failed and can return
              // We need to extract the bits and return them in EXPECTED
              uint32_t FailedResult = FailedResultOurBits >> (Alignment * 8);
              mcontext->regs[ExpectedReg] = FailedResult;
              return true;
            }
          }
        }
      }
    }
  }
  else if (Size == 8) {
    // 64bit
    uint64_t AlignmentMask = 0b1111;
    if ((Addr & AlignmentMask) > 8) {
      uint64_t Alignment = Addr & 0b111;
      Addr &= ~0b111ULL;
      uint64_t AddrUpper = Addr + 8;

      // Crosses a 16byte boundary
      // Need to do 256bit atomic, but since that doesn't exist we need to do a dual CAS loop
      __uint128_t Mask = ~0ULL;
      Mask <<= Alignment * 8;
      __uint128_t NegMask = ~Mask;
      __uint128_t TmpExpected{};
      __uint128_t TmpDesired{};

      __uint128_t Desired = mcontext->regs[DesiredReg];
      Desired <<= Alignment * 8;

      __uint128_t Expected = mcontext->regs[ExpectedReg];
      Expected <<= Alignment * 8;

      while (1) {
        __uint128_t LoadOrderUpper = LoadAcquire64(AddrUpper);
        LoadOrderUpper <<= 64;
        __uint128_t TmpActual = LoadOrderUpper | LoadAcquire64(Addr);

        // Set up expected
        TmpExpected = TmpActual;
        TmpExpected &= NegMask;
        TmpExpected |= Expected;

        // Set up desired
        TmpDesired = TmpExpected;
        TmpDesired &= NegMask;
        TmpDesired |= Desired;

        uint64_t TmpExpectedLower = TmpExpected;
        uint64_t TmpExpectedUpper = TmpExpected >> 64;

        uint64_t TmpDesiredLower = TmpDesired;
        uint64_t TmpDesiredUpper = TmpDesired >> 64;

        if (TmpExpected == TmpActual) {
          if (StoreCAS64(TmpExpectedUpper, TmpDesiredUpper, AddrUpper)) {
            if (StoreCAS64(TmpExpectedLower, TmpDesiredLower, Addr)) {
              // Stored successfully
              return true;
            }
            else {
              // CAS managed to tear, we can't really solve this
              // Continue down the path to let the guest know values weren't expected
            }
          }

          TmpExpected = TmpExpectedUpper;
          TmpExpected <<= 64;
          TmpExpected |= TmpExpectedLower;
        }
        else {
          // Mismatch up front
          TmpExpected = TmpActual;
        }

        // Not successful
        // Now we need to check the results to see if we need to try again
        __uint128_t FailedResultOurBits = TmpExpected & Mask;
        __uint128_t FailedResultNotOurBits = TmpExpected & NegMask;

        __uint128_t FailedDesiredOurBits = TmpDesired & Mask;
        __uint128_t FailedDesiredNotOurBits = TmpDesired & NegMask;
        if ((FailedResultNotOurBits ^ FailedDesiredNotOurBits) != 0) {
          // If the bits changed that weren't part of our regular CAS then we need to try again
          continue;
        }
        if ((FailedResultOurBits ^ FailedDesiredOurBits) != 0) {
          // If the bits changed that we were wanting to change then we have failed and can return
          // We need to extract the bits and return them in EXPECTED
          uint64_t FailedResult = FailedResultOurBits >> (Alignment * 8);
          mcontext->regs[ExpectedReg] = FailedResult;
          return true;
        }
      }
    }
    else {
      // Fits within a 16byte region
      uint64_t Alignment = Addr & AlignmentMask;
      Addr &= ~AlignmentMask;
      std::atomic<__uint128_t> *Atomic128 = reinterpret_cast<std::atomic<__uint128_t>*>(Addr);

      __uint128_t Mask = ~0ULL;
      Mask <<= Alignment * 8;
      __uint128_t NegMask = ~Mask;
      __uint128_t TmpExpected{};
      __uint128_t TmpDesired{};

      __uint128_t Desired = mcontext->regs[DesiredReg];
      Desired <<= Alignment * 8;

      __uint128_t Expected = mcontext->regs[ExpectedReg];
      Expected <<= Alignment * 8;

      while (1) {
        TmpExpected = Atomic128->load();

        // Set up expected
        TmpExpected &= NegMask;
        TmpExpected |= Expected;

        // Set up desired
        TmpDesired = TmpExpected;
        TmpDesired &= NegMask;
        TmpDesired |= Desired;

        bool CASResult = Atomic128->compare_exchange_strong(TmpExpected, TmpDesired);
        if (CASResult) {
          // Successful, so we are done
          return true;
        }
        else {
          // Not successful
          // Now we need to check the results to see if we need to try again
          __uint128_t FailedResultOurBits = TmpExpected & Mask;
          __uint128_t FailedResultNotOurBits = TmpExpected & NegMask;

          __uint128_t FailedDesiredOurBits = TmpDesired & Mask;
          __uint128_t FailedDesiredNotOurBits = TmpDesired & NegMask;
          if ((FailedResultNotOurBits ^ FailedDesiredNotOurBits) != 0) {
            // If the bits changed that weren't part of our regular CAS then we need to try again
            continue;
          }
          if ((FailedResultOurBits ^ FailedDesiredOurBits) != 0) {
            // If the bits changed that we were wanting to change then we have failed and can return
            // We need to extract the bits and return them in EXPECTED
            uint64_t FailedResult = FailedResultOurBits >> (Alignment * 8);
            mcontext->regs[ExpectedReg] = FailedResult;
            return true;
          }

          // If we got here, that means the CAS failed
          // NotOurBits didn't change and bits we cared about didn't change
          ERROR_AND_DIE("Impossible");
        }
      }
    }
  }

  return false;
}

}
