#pragma once

#include "Interface/Core/ArchHelpers/Dwarf.h"

#include <stdint.h>
#include <utility>

namespace FEXCore::CPU {
class CodeBuffer {
public:
  CodeBuffer() {}
  CodeBuffer(size_t Size);
  ~CodeBuffer();
  uint8_t *Ptr{};
  size_t Size{};

  CodeBuffer(CodeBuffer&& other) noexcept {
    *this = std::move(other);
  }

  CodeBuffer& operator=(CodeBuffer&& other) noexcept {
    if (&other == this)
      return *this;

    Ptr = std::exchange(other.Ptr, nullptr); // Clear Ptr so destructor does nothing
    Size = other.Size;
    Dwarf = std::move(other.Dwarf);

    return *this;
  }

private:
  DwarfFrame Dwarf;
};

static_assert(std::is_nothrow_move_constructible<DwarfFrame>::value, "DwarfFrame should be noexcept MoveConstructible");


}
