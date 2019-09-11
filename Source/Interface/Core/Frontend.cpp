#include "Interface/Context/Context.h"
#include "Interface/Core/Frontend.h"
#include "Interface/Core/InternalThreadState.h"
#include "LogManager.h"

#include <array>
#include <cstring>
#include <FEXCore/Core/X86Enums.h>
#include <FEXCore/Debug/X86Tables.h>

namespace FEXCore::Frontend {
using namespace FEXCore::X86Tables;
constexpr size_t MAX_INST_SIZE = 15;

static uint32_t MapModRMToReg(uint8_t REX, uint8_t bits, bool HighBits, bool HasREX, bool HasXMM, uint8_t InvalidOffset = 16) {
  constexpr std::array<uint64_t, 16> GPRIndexes = {
    // Classical ordering?
    FEXCore::X86State::REG_RAX,
    FEXCore::X86State::REG_RCX,
    FEXCore::X86State::REG_RDX,
    FEXCore::X86State::REG_RBX,
    FEXCore::X86State::REG_RSP,
    FEXCore::X86State::REG_RBP,
    FEXCore::X86State::REG_RSI,
    FEXCore::X86State::REG_RDI,
    FEXCore::X86State::REG_R8,
    FEXCore::X86State::REG_R9,
    FEXCore::X86State::REG_R10,
    FEXCore::X86State::REG_R11,
    FEXCore::X86State::REG_R12,
    FEXCore::X86State::REG_R13,
    FEXCore::X86State::REG_R14,
    FEXCore::X86State::REG_R15,
  };

  constexpr std::array<uint64_t, 16> GPR8BitHighIndexes = {
    // Classical ordering?
    FEXCore::X86State::REG_RAX,
    FEXCore::X86State::REG_RCX,
    FEXCore::X86State::REG_RDX,
    FEXCore::X86State::REG_RBX,
    FEXCore::X86State::REG_RAX,
    FEXCore::X86State::REG_RCX,
    FEXCore::X86State::REG_RDX,
    FEXCore::X86State::REG_RBX,
    FEXCore::X86State::REG_R8,
    FEXCore::X86State::REG_R9,
    FEXCore::X86State::REG_R10,
    FEXCore::X86State::REG_R11,
    FEXCore::X86State::REG_R12,
    FEXCore::X86State::REG_R13,
    FEXCore::X86State::REG_R14,
    FEXCore::X86State::REG_R15,
  };

  constexpr std::array<uint64_t, 16> XMMIndexes = {
    FEXCore::X86State::REG_XMM_0,
    FEXCore::X86State::REG_XMM_1,
    FEXCore::X86State::REG_XMM_2,
    FEXCore::X86State::REG_XMM_3,
    FEXCore::X86State::REG_XMM_4,
    FEXCore::X86State::REG_XMM_5,
    FEXCore::X86State::REG_XMM_6,
    FEXCore::X86State::REG_XMM_7,
    FEXCore::X86State::REG_XMM_8,
    FEXCore::X86State::REG_XMM_9,
    FEXCore::X86State::REG_XMM_10,
    FEXCore::X86State::REG_XMM_11,
    FEXCore::X86State::REG_XMM_12,
    FEXCore::X86State::REG_XMM_13,
    FEXCore::X86State::REG_XMM_14,
    FEXCore::X86State::REG_XMM_15,
  };

  const std::array<uint64_t, 16> *GPRs = &GPRIndexes;
  if (HasXMM) {
    GPRs = &XMMIndexes;
  }
  else if (HighBits && !HasREX) {
    GPRs = &GPR8BitHighIndexes;
  }

  uint8_t Offset = (REX << 3) | bits;

  if (Offset == InvalidOffset) {
    return FEXCore::X86State::REG_INVALID;
  }
  return (*GPRs)[(REX << 3) | bits];
}

Decoder::Decoder(FEXCore::Context::Context *ctx)
  : CTX {ctx} {
  DecodedBuffer.resize(DefaultDecodedBufferSize);
}

bool Decoder::DecodeInstruction(uint8_t const* InstStream, uint64_t PC) {
  uint8_t InstructionSize = 0;
  std::array<uint8_t, MAX_INST_SIZE> Instruction;
  bool InstructionDecoded = false;
  bool ErrorDuringDecoding = false;
  auto ReadByte = [InstStream, &Instruction, &InstructionSize]() -> uint8_t {
    uint8_t Byte = InstStream[InstructionSize];
    InstructionSize++;
    LogMan::Throw::A(InstructionSize < MAX_INST_SIZE, "Max instruction size exceeded!");
    Instruction[InstructionSize] = Byte;
    return Byte;
  };

  auto PeekByte = [InstStream, &InstructionSize](uint8_t Offset) -> uint8_t {
    uint8_t Byte = InstStream[InstructionSize + Offset];
    return Byte;
  };

  auto ReadData = [&ReadByte, InstStream, &InstructionSize](size_t Size) -> uint64_t {
    uint64_t Res;
#define READ_DATA(x, y) \
    case x: { \
      y const *Data = reinterpret_cast<y const*>(&InstStream[InstructionSize]); \
      Res = *Data; \
    } \
    break

    switch (Size) {
    case 0: return 0;
    READ_DATA(1, uint8_t);
    READ_DATA(2, uint16_t);
    READ_DATA(4, uint32_t);
    READ_DATA(8, uint64_t);
    default:
    LogMan::Msg::A("Unknown data size to read");
    }
#undef READ_DATA

    for(size_t i = 0; i < Size; ++i) {
      ReadByte();
    }
    return Res;
  };

  auto &DecodeInst = DecodedBuffer[DecodedSize];
  memset(&DecodeInst, 0, sizeof(DecodedInst));

  auto DecodeModRM = [&DecodeInst](FEXCore::X86Tables::ModRMDecoded ModRM, uint8_t *Displacement) {
    // Do we have an offset?
    if (ModRM.mod == 0b01) {
      *Displacement = 1;
    }
    else if (ModRM.mod == 0b10) {
      *Displacement = 4;
    }
    else if (ModRM.mod == 0 && ModRM.rm == 0b101)
      *Displacement = 4;

    // Ensure this flag is set
    DecodeInst.Flags |= DecodeFlags::FLAG_MODRM_PRESENT;
  };

  auto DecodeSIB = [&ReadByte, &DecodeInst](FEXCore::X86Tables::ModRMDecoded ModRM, uint8_t *Displacement) -> bool {
    bool HasSIB = ((ModRM.mod != 0b11) &&
                  (ModRM.rm == 0b100));

    if (HasSIB) {
      FEXCore::X86Tables::SIBDecoded SIB;
      if (DecodeInst.DecodedSIB) {
        SIB.Hex = DecodeInst.SIB;
      }
      else {
        // Haven't yet grabbed SIB, pull it now
        DecodeInst.SIB = ReadByte();
        SIB.Hex = DecodeInst.SIB;
        DecodeInst.DecodedSIB = true;
      }

      // Ensure this flag is set
      DecodeInst.Flags |= DecodeFlags::FLAG_SIB_PRESENT;

      // If the SIB base is 0b101, aka BP or R13 then we have a 32bit displacement
      if (ModRM.mod == 0b01) {
        *Displacement = 1;
      }
      else if (ModRM.mod == 0b10) {
        *Displacement = 4;
      }
      else if (ModRM.mod == 0b00 && ModRM.rm == 0b101) {
        *Displacement = 4;
      }
      else if (ModRM.mod == 0b00 && ModRM.rm == 0b100 && SIB.base == 0b101) {
        *Displacement = 4;
      }
    }

    return HasSIB;
  };

  auto NormalOp = [&DecodeModRM, &DecodeSIB, &ReadByte, &ReadData, &DecodeInst, &InstructionSize](auto &Table, auto Op) -> bool {
    FEXCore::X86Tables::X86InstInfo *Info = &Table[Op];

    DecodeInst.OP = Op;
    DecodeInst.TableInfo = Info;

    // XXX: Once we support 32bit x86 then this will be necessary to support
    if (Info->Type == FEXCore::X86Tables::TYPE_LEGACY_PREFIX) {
      DecodeInst.Flags |= DecodeFlags::FLAG_LEGACY_PREFIX;
      LogMan::Msg::A("Legacy Prefix");
      return false;
    }
    if (Info->Type == FEXCore::X86Tables::TYPE_UNKNOWN ||
        Info->Type == FEXCore::X86Tables::TYPE_INVALID) {
      LogMan::Msg::A("Invalid or Unknown instruction: %s 0x%04x 0x%lx", Info->Name, Op, DecodeInst.PC);
      return false;
    }
    if (Info->Type >= FEXCore::X86Tables::TYPE_GROUP_1 &&
        Info->Type <= FEXCore::X86Tables::TYPE_GROUP_P) {
      LogMan::Msg::A("Group Ops should have been decoded before this!");
      return false;
    }

    // New instruction size decoding
    {
      // Decode destinations first
      uint32_t DstSizeFlag = FEXCore::X86Tables::InstFlags::GetSizeDstFlags(Info->Flags);
      uint32_t SrcSizeFlag = FEXCore::X86Tables::InstFlags::GetSizeSrcFlags(Info->Flags);

      if (DstSizeFlag == FEXCore::X86Tables::InstFlags::SIZE_8BIT) {
        DecodeInst.Flags |= DecodeFlags::GenSizeDstSize(DecodeFlags::SIZE_8BIT);
      }
      else if (DstSizeFlag == FEXCore::X86Tables::InstFlags::SIZE_16BIT) {
        DecodeInst.Flags |= DecodeFlags::GenSizeDstSize(DecodeFlags::SIZE_16BIT);
      }
      else if (DstSizeFlag == FEXCore::X86Tables::InstFlags::SIZE_128BIT) {
        DecodeInst.Flags |= DecodeFlags::GenSizeDstSize(DecodeFlags::SIZE_128BIT);
      }
      else if (DecodeInst.Flags & DecodeFlags::FLAG_OPERAND_SIZE &&
        DstSizeFlag == FEXCore::X86Tables::InstFlags::SIZE_DEF) {
        // See table 1-2. Operand-Size Overrides for this decoding
        // If the default operating mode is 32bit and we have the operand size flag then the operating size drops to 16bit
        DecodeInst.Flags |= DecodeFlags::GenSizeDstSize(DecodeFlags::SIZE_16BIT);
      }
      else if (DecodeInst.Flags & DecodeFlags::FLAG_REX_WIDENING ||
        DstSizeFlag == FEXCore::X86Tables::InstFlags::SIZE_64BIT) {
        DecodeInst.Flags |= DecodeFlags::GenSizeDstSize(DecodeFlags::SIZE_64BIT);
      }
      else {
        DecodeInst.Flags |= DecodeFlags::GenSizeDstSize(DecodeFlags::SIZE_32BIT);
      }

      // Decode sources
      if (SrcSizeFlag == FEXCore::X86Tables::InstFlags::SIZE_8BIT) {
        DecodeInst.Flags |= DecodeFlags::GenSizeSrcSize(DecodeFlags::SIZE_8BIT);
      }
      else if (SrcSizeFlag == FEXCore::X86Tables::InstFlags::SIZE_16BIT) {
        DecodeInst.Flags |= DecodeFlags::GenSizeSrcSize(DecodeFlags::SIZE_16BIT);
      }
      else if (SrcSizeFlag == FEXCore::X86Tables::InstFlags::SIZE_128BIT) {
        DecodeInst.Flags |= DecodeFlags::GenSizeSrcSize(DecodeFlags::SIZE_128BIT);
      }
      else if (DecodeInst.Flags & DecodeFlags::FLAG_OPERAND_SIZE &&
        SrcSizeFlag == FEXCore::X86Tables::InstFlags::SIZE_DEF) {
        // See table 1-2. Operand-Size Overrides for this decoding
        // If the default operating mode is 32bit and we have the operand size flag then the operating size drops to 16bit
        DecodeInst.Flags |= DecodeFlags::GenSizeSrcSize(DecodeFlags::SIZE_16BIT);
      }
      else if (DecodeInst.Flags & DecodeFlags::FLAG_REX_WIDENING ||
        SrcSizeFlag == FEXCore::X86Tables::InstFlags::SIZE_64BIT) {
        DecodeInst.Flags |= DecodeFlags::GenSizeSrcSize(DecodeFlags::SIZE_64BIT);
      }
      else {
        DecodeInst.Flags |= DecodeFlags::GenSizeSrcSize(DecodeFlags::SIZE_32BIT);
      }
    }

    // Is ModRM present via explicit instruction encoded or REX?
    bool HasMODRM = !!(DecodeInst.Flags & DecodeFlags::FLAG_MODRM_PRESENT);
    HasMODRM |= !!(Info->Flags & FEXCore::X86Tables::InstFlags::FLAGS_MODRM);

    bool HasSIB = false;
    bool HasWideningDisplacement = DecodeInst.Flags & DecodeFlags::FLAG_REX_WIDENING;
    bool HasNarrowingDisplacement = DecodeInst.Flags & DecodeFlags::FLAG_OPERAND_SIZE;
    // This is used for ModRM register modification
    // For both modrm.reg and modrm.rm(when mod == 0b11) when value is >= 0b100
    // then it changes from expected registers to the high 8bits of the lower registers
    // Bit annoying to support
    // In the case of no modrm (REX in byte situation) then it is unaffected
    bool Is8BitSrc = (DecodeFlags::GetSizeSrcFlags(DecodeInst.Flags) == DecodeFlags::SIZE_8BIT);
    bool Is8BitDest = (DecodeFlags::GetSizeDstFlags(DecodeInst.Flags) == DecodeFlags::SIZE_8BIT);
    bool HasREX = !!(DecodeInst.Flags & DecodeFlags::FLAG_REX_PREFIX);
    bool HasXMMSrc = !!(Info->Flags & FEXCore::X86Tables::InstFlags::FLAGS_XMM_FLAGS) && !HAS_XMM_SUBFLAG(Info->Flags, FEXCore::X86Tables::InstFlags::FLAGS_SF_SRC_GPR);
    bool HasXMMDst = !!(Info->Flags & FEXCore::X86Tables::InstFlags::FLAGS_XMM_FLAGS) && !HAS_XMM_SUBFLAG(Info->Flags, FEXCore::X86Tables::InstFlags::FLAGS_SF_DST_GPR);
    bool HasHighXMM = HAS_XMM_SUBFLAG(Info->Flags, FEXCore::X86Tables::InstFlags::FLAGS_SF_HIGH_XMM_REG);
    uint8_t Displacement = 0;

    auto *CurrentDest = &DecodeInst.Dest;

    if (HAS_NON_XMM_SUBFLAG(Info->Flags, FEXCore::X86Tables::InstFlags::FLAGS_SF_DST_RAX) ||
        HAS_NON_XMM_SUBFLAG(Info->Flags, FEXCore::X86Tables::InstFlags::FLAGS_SF_DST_RDX)) {
      // Some instructions hardcode their destination as RAX
      CurrentDest->TypeGPR.Type = DecodedOperand::TYPE_GPR;
      CurrentDest->TypeGPR.HighBits = false;
      CurrentDest->TypeGPR.GPR = HAS_NON_XMM_SUBFLAG(Info->Flags, FEXCore::X86Tables::InstFlags::FLAGS_SF_DST_RAX) ? FEXCore::X86State::REG_RAX : FEXCore::X86State::REG_RDX;
      CurrentDest = &DecodeInst.Src1;
    }

    if (HAS_NON_XMM_SUBFLAG(Info->Flags, FEXCore::X86Tables::InstFlags::FLAGS_SF_REX_IN_BYTE)) {
      LogMan::Throw::A(!HasMODRM, "This instruction shouldn't have ModRM!");

      // If the REX is in the byte that means the lower nibble of the OP contains the destination GPR
      // This also means that the destination is always a GPR on these ones
      // ADDITIONALLY:
      // If there is a REX prefix then that allows extended GPR usage
      CurrentDest->TypeGPR.Type = DecodedOperand::TYPE_GPR;
      DecodeInst.Dest.TypeGPR.HighBits = (Is8BitDest && !HasREX && (Op & 0b111) >= 0b100) || HasHighXMM;
      CurrentDest->TypeGPR.GPR = MapModRMToReg(DecodeInst.Flags & DecodeFlags::FLAG_REX_XGPR_B ? 1 : 0, Op & 0b111, Is8BitDest, HasREX, false);
    }

    if (HasMODRM) {
      if (!DecodeInst.DecodedModRM) {
        DecodeInst.ModRM = ReadByte();
        DecodeInst.DecodedModRM = true;
      }

      FEXCore::X86Tables::ModRMDecoded ModRM;
      ModRM.Hex = DecodeInst.ModRM;

      DecodeModRM(ModRM, &Displacement);
      HasSIB = DecodeSIB(ModRM, &Displacement);
    }

    uint8_t Bytes = Info->MoreBytes + Displacement;

    if ((Info->Flags & FEXCore::X86Tables::InstFlags::FLAGS_DISPLACE_SIZE_MUL_2) && HasWideningDisplacement) {
      Bytes <<= 1;
    }
    if ((Info->Flags & FEXCore::X86Tables::InstFlags::FLAGS_DISPLACE_SIZE_DIV_2) && HasNarrowingDisplacement) {
      Bytes >>= 1;
    }

    auto ModRMOperand = [&](FEXCore::X86Tables::DecodedOperand &GPR, FEXCore::X86Tables::DecodedOperand &NonGPR, bool HasXMMGPR, bool HasXMMNonGPR, bool GPR8Bit, bool NonGPR8Bit) {
      FEXCore::X86Tables::ModRMDecoded ModRM;
      ModRM.Hex = DecodeInst.ModRM;

      // Decode the GPR source first
      GPR.TypeGPR.Type = DecodedOperand::TYPE_GPR;
      GPR.TypeGPR.HighBits = (GPR8Bit && ModRM.reg >= 0b100 && !HasREX) || HasHighXMM;
      GPR.TypeGPR.GPR = MapModRMToReg(DecodeInst.Flags & DecodeFlags::FLAG_REX_XGPR_R ? 1 : 0, ModRM.reg, GPR8Bit, HasREX, HasXMMGPR);

      // ModRM.mod == 0b11 == Register
      // ModRM.Mod != 0b11 == Register-direct addressing
      if (ModRM.mod == 0b11) {
        NonGPR.TypeGPR.Type = DecodedOperand::TYPE_GPR;
        NonGPR.TypeGPR.HighBits = (NonGPR8Bit && ModRM.rm >= 0b100 && !HasREX) || HasHighXMM;
        NonGPR.TypeGPR.GPR = MapModRMToReg(DecodeInst.Flags & DecodeFlags::FLAG_REX_XGPR_B ? 1 : 0, ModRM.rm, NonGPR8Bit, HasREX, HasXMMNonGPR);
      }
      else {
        if (HasSIB) {
          // SIB
          FEXCore::X86Tables::SIBDecoded SIB;
          SIB.Hex = DecodeInst.SIB;
          NonGPR.TypeSIB.Type = DecodedOperand::TYPE_SIB;
          NonGPR.TypeSIB.Scale = 1 << SIB.scale;

          // The invalid encoding types are described at Table 1-12. "promoted nsigned is always non-zero"
          NonGPR.TypeSIB.Index = MapModRMToReg(DecodeInst.Flags & DecodeFlags::FLAG_REX_XGPR_X ? 1 : 0, SIB.index, false, false, false, 0b100);
          NonGPR.TypeSIB.Base  = MapModRMToReg(DecodeInst.Flags & DecodeFlags::FLAG_REX_XGPR_B ? 1 : 0, SIB.base, false, false, false, ModRM.mod == 0 ? 0b101 : 16);

          uint64_t Literal {0};
          LogMan::Throw::A(Displacement <= 4, "Number of bytes should be <= 4 for literal src");

          Literal = ReadData(Displacement);
          if (Displacement == 1) {
            Literal = static_cast<int8_t>(Literal);
          }
          Bytes -= Displacement;
          NonGPR.TypeSIB.Offset = Literal;
        }
        else if (ModRM.mod == 0) {
          // Explained in Table 1-14. "Operand Addressing Using ModRM and SIB Bytes"
          LogMan::Throw::A(ModRM.rm != 0b100, "Shouldn't have hit this here");
          if (ModRM.rm == 0b101) {
            // 32bit Displacement
            uint32_t Literal;
            Literal = ReadData(4);
            Bytes -= 4;

            NonGPR.TypeRIPLiteral.Type = DecodedOperand::TYPE_RIP_RELATIVE;
            NonGPR.TypeRIPLiteral.Literal = Literal;
          }
          else {
            // Register-direct addressing
            NonGPR.TypeGPR.Type = DecodedOperand::TYPE_GPR_DIRECT;
            NonGPR.TypeGPR.GPR = MapModRMToReg(DecodeInst.Flags & DecodeFlags::FLAG_REX_XGPR_B ? 1 : 0, ModRM.rm, false, false, false);
          }
        }
        else {
          uint8_t DisplacementSize = ModRM.mod == 1 ? 1 : 4;
          uint32_t Literal;
          Literal = ReadData(DisplacementSize);
          if (DisplacementSize == 1) {
            Literal = static_cast<int8_t>(Literal);
          }
          Bytes -= DisplacementSize;

          NonGPR.TypeGPRIndirect.Type = DecodedOperand::TYPE_GPR_INDIRECT;
          NonGPR.TypeGPRIndirect.GPR = MapModRMToReg(DecodeInst.Flags & DecodeFlags::FLAG_REX_XGPR_B ? 1 : 0, ModRM.rm, false, false, false);
          NonGPR.TypeGPRIndirect.Displacement = Literal;
        }
      }
    };

    if (Info->Flags & FEXCore::X86Tables::InstFlags::FLAGS_MODRM &&
       Info->Flags & FEXCore::X86Tables::InstFlags::FLAGS_SF_MOD_DST) {
      ModRMOperand(DecodeInst.Src1, DecodeInst.Dest, HasXMMSrc, HasXMMDst, Is8BitSrc, Is8BitDest);
    }

    // This is almost the same as when the ModRM is the destination type
    // The main different being that Dst and Src flip which bits that use (reg<->rm)
    auto *CurrentSrc = &DecodeInst.Src1;
    if (Info->Flags & FEXCore::X86Tables::InstFlags::FLAGS_MODRM &&
       !(Info->Flags & FEXCore::X86Tables::InstFlags::FLAGS_SF_MOD_DST)) {
      ModRMOperand(DecodeInst.Dest, DecodeInst.Src1, HasXMMDst, HasXMMSrc, Is8BitDest, Is8BitSrc);
      CurrentSrc = &DecodeInst.Src2;
    }
    else if (HAS_NON_XMM_SUBFLAG(Info->Flags, FEXCore::X86Tables::InstFlags::FLAGS_SF_SRC_RAX)) {
        CurrentSrc->TypeGPR.Type = DecodedOperand::TYPE_GPR;
        CurrentSrc->TypeGPR.HighBits = false;
        CurrentSrc->TypeGPR.GPR = FEXCore::X86State::REG_RAX;
        CurrentSrc = &DecodeInst.Src2;
    }
    else if (HAS_NON_XMM_SUBFLAG(Info->Flags, FEXCore::X86Tables::InstFlags::FLAGS_SF_SRC_RCX)) {
        CurrentSrc->TypeGPR.Type = DecodedOperand::TYPE_GPR;
        CurrentSrc->TypeGPR.HighBits = false;
        CurrentSrc->TypeGPR.GPR = FEXCore::X86State::REG_RCX;
        CurrentSrc = &DecodeInst.Src2;
    }

    if (Bytes != 0) {
      LogMan::Throw::A(Bytes <= 8, "Number of bytes should be <= 8 for literal src");

      CurrentSrc->TypeLiteral.Size = Bytes;

      uint64_t Literal {0};
      Literal = ReadData(Bytes);

      if (Info->Flags & FEXCore::X86Tables::InstFlags::FLAGS_SRC_SEXT) {
        if (Bytes == 1) {
          Literal = static_cast<int8_t>(Literal);
        }
        else if (Bytes == 2) {
          Literal = static_cast<int16_t>(Literal);
        }
        else {
          Literal = static_cast<int32_t>(Literal);
        }
      }

      Bytes = 0;
      CurrentSrc->TypeLiteral.Type = DecodedOperand::TYPE_LITERAL;
      CurrentSrc->TypeLiteral.Literal = Literal;
    }

    if (Bytes != 0) {
      LogMan::Msg::A("Inst at 0x%lx: 0x%04x '%s' Had an instruction of size %d with %d remaining", DecodeInst.PC, DecodeInst.OP, DecodeInst.TableInfo->Name, InstructionSize, Bytes);
    }
    LogMan::Throw::A(Bytes == 0, "Had undecoded bytes left in the instruction encoding");
    DecodeInst.InstSize = InstructionSize;
    return true;
  };

  auto NormalOpHeader = [&ReadByte, &DecodeInst, &NormalOp](auto &Table, auto Op) -> bool {
    FEXCore::X86Tables::X86InstInfo *Info = &Table[Op];

    DecodeInst.OP = Op;
    DecodeInst.TableInfo = Info;

    // XXX: Once we support 32bit x86 then this will be necessary to support
    if (Info->Type == FEXCore::X86Tables::TYPE_LEGACY_PREFIX) {
      DecodeInst.Flags |= DecodeFlags::FLAG_LEGACY_PREFIX;
      LogMan::Msg::A("Legacy Prefix");
      return false;
    }

    if (Info->Type == FEXCore::X86Tables::TYPE_UNKNOWN ||
        Info->Type == FEXCore::X86Tables::TYPE_INVALID) {
      LogMan::Msg::A("Invalid or Unknown instruction: %s 0x%04x 0x%lx", Info->Name, Op, DecodeInst.PC);
      return false;
    }

    if (Info->Type >= FEXCore::X86Tables::TYPE_GROUP_6 &&
        Info->Type <= FEXCore::X86Tables::TYPE_GROUP_P) {
#define OPD(group, prefix, Reg) (((group - FEXCore::X86Tables::TYPE_GROUP_6) << 5) | (prefix) << 3 | (Reg))
      constexpr uint16_t PF_NONE = 0;
      constexpr uint16_t PF_F3 = 1;
      constexpr uint16_t PF_66 = 2;
      constexpr uint16_t PF_F2 = 3;

      uint16_t PrefixType = PF_NONE;
      if (DecodeInst.LastEscapePrefix == 0xF3)
        PrefixType = PF_F3;
      else if (DecodeInst.LastEscapePrefix == 0xF2)
        PrefixType = PF_F2;
      else if (DecodeInst.LastEscapePrefix == 0x66)
        PrefixType = PF_66;

      // We have ModRM
      uint8_t ModRMByte = ReadByte();
      DecodeInst.ModRM = ModRMByte;
      DecodeInst.DecodedModRM = true;
      DecodeInst.Flags |= DecodeFlags::FLAG_MODRM_PRESENT;

      FEXCore::X86Tables::ModRMDecoded ModRM;
      ModRM.Hex = DecodeInst.ModRM;

      uint16_t Op = OPD(Info->Type, PrefixType, ModRM.reg);
      FEXCore::X86Tables::X86InstInfo *Info = &SecondInstGroupOps[Op];
#undef OPD
      if (Info->Type == FEXCore::X86Tables::TYPE_SECOND_GROUP_MODRM) {
        // Everything in this group is privileged instructions aside from XGETBV
        constexpr std::array<uint8_t, 8> RegToField = {
          255,
          0,
          1,
          2,
          255,
          255,
          255,
          3,
        };
        uint8_t Field = RegToField[ModRM.reg];
        LogMan::Throw::A(Field != 255, "Invalid field selected!");

        uint8_t Op = (Field << 3) | ModRM.rm;
        return NormalOp(SecondModRMTableOps, Op);
      }
      else {
        return NormalOp(SecondInstGroupOps, Op);
      }
    }
    else if (Info->Type == FEXCore::X86Tables::TYPE_X87_TABLE_PREFIX) {
      // We have ModRM
      uint8_t ModRMByte = ReadByte();
      DecodeInst.ModRM = ModRMByte;
      DecodeInst.DecodedModRM = true;
      DecodeInst.Flags |= DecodeFlags::FLAG_MODRM_PRESENT;

      FEXCore::X86Tables::ModRMDecoded ModRM;
      ModRM.Hex = DecodeInst.ModRM;

      uint16_t X87Op = ((Op - 0xD8) << 8) | ModRMByte;
      return NormalOp(X87Ops, X87Op);
    }
    else if (Info->Type >= FEXCore::X86Tables::TYPE_GROUP_1 &&
             Info->Type <= FEXCore::X86Tables::TYPE_GROUP_11) {
      uint8_t ModRMByte = ReadByte();
      DecodeInst.ModRM = ModRMByte;
      DecodeInst.DecodedModRM = true;
      DecodeInst.Flags |= DecodeFlags::FLAG_MODRM_PRESENT;

      FEXCore::X86Tables::ModRMDecoded ModRM;
      ModRM.Hex = DecodeInst.ModRM;

#define OPD(group, prefix, Reg) (((group - FEXCore::X86Tables::TYPE_GROUP_1) << 6) | (prefix) << 3 | (Reg))
      return NormalOp(PrimaryInstGroupOps, OPD(Info->Type, Info->MoreBytes, ModRM.reg));
#undef OPD
    }
    else if (Info->Type == FEXCore::X86Tables::TYPE_MODRM_TABLE_PREFIX) {
    }
    else if (Info->Type == FEXCore::X86Tables::TYPE_VEX_TABLE_PREFIX) {
      uint16_t map_select = 1;
      uint16_t pp = 0;

      uint8_t Byte1 = ReadByte();

      if (Op == 0xC5) { // Two byte VEX
        pp = Byte1 & 0b11;
      }
      else { // 0xC4 = Three byte VEX
        uint8_t Byte2 = ReadByte();
        pp = Byte2 & 0b11;
        map_select = Byte1 & 0b11111;
        LogMan::Throw::A(map_select >= 1 && map_select <= 3, "We don't understand a map_select of: %d", map_select);
      }

      uint16_t VEXOp = ReadByte();
#define OPD(map_select, pp, opcode) (((map_select - 1) << 10) | (pp << 8) | (opcode))
      return NormalOp(VEXTableOps, OPD(map_select, pp, VEXOp));
#undef OPD
    }
    else if (Info->Type == FEXCore::X86Tables::TYPE_XOP_TABLE_PREFIX) {
      LogMan::Msg::A("XOP and POP <modrm> aren't handled!");
      uint16_t Byte1 = ReadByte();
      uint16_t Byte2 = ReadByte();
      uint16_t XOPOp = ReadByte();
      uint16_t map_select = Byte1 & 0b11111;
      LogMan::Throw::A(map_select >= 8 && map_select <= 0xA, "We don't understand a map_select of: %d", map_select);
      uint16_t pp = Byte2 & 0b11;
      map_select -= 8;

#define OPD(group, pp, opcode) ( (group << 10) | (pp << 8) | (opcode))
      return NormalOp(XOPTableOps, OPD(map_select, pp, XOPOp));
#undef OPD
    }

    return NormalOp(Table, Op);
  };

  DecodeInst.PC = PC;
  while (!InstructionDecoded && !ErrorDuringDecoding) {
    uint8_t Op = ReadByte();
    switch (Op) {
    case 0x0F: {// Escape Op
      uint8_t EscapeOp = ReadByte();
      switch (EscapeOp) {
      case 0x0F: { // 3DNow!
        // 3DNow! Instruction Encoding: 0F 0F [ModRM] [SIB] [Displacement] [Opcode]
        // Decode ModRM
        uint8_t ModRMByte = ReadByte();
        DecodeInst.ModRM = ModRMByte;
        DecodeInst.DecodedModRM = true;
        DecodeInst.Flags |= DecodeFlags::FLAG_MODRM_PRESENT;

        FEXCore::X86Tables::ModRMDecoded ModRM;
        ModRM.Hex = DecodeInst.ModRM;

        uint8_t Displacement = 0;
        DecodeModRM(ModRM, &Displacement);
        DecodeSIB(ModRM, &Displacement);

        // Take a peek at the op just past the displacement
        uint8_t Op = PeekByte(Displacement);
        if (NormalOpHeader(FEXCore::X86Tables::DDDNowOps, Op)) {
          InstructionDecoded = true;
        }

        // Make sure to read the opcode in to our internal structure
        ReadByte();
      break;
      }
      case 0x38: { // F38 Table!
        constexpr uint16_t PF_38_NONE = 0;
        constexpr uint16_t PF_38_66 = 1;
        constexpr uint16_t PF_38_F2 = 2;

        uint16_t Prefix = PF_38_NONE;
        if (DecodeInst.LastEscapePrefix == 0xF2) // REPNE
          Prefix = PF_38_F2;
        else if (DecodeInst.LastEscapePrefix == 0x66) // Operand Size
          Prefix = PF_38_66;

        uint16_t Op = (Prefix << 8) | ReadByte();
        if (NormalOpHeader(FEXCore::X86Tables::H0F38TableOps, Op)) {
          InstructionDecoded = true;
        }
      break;
      }
      case 0x3A: { // F3A Table!
        constexpr uint16_t PF_3A_NONE = 0;
        constexpr uint16_t PF_3A_66   = 1;
        constexpr uint16_t PF_3A_REX  = (1 << 1);

        uint16_t Prefix = PF_3A_NONE;
        if (DecodeInst.LastEscapePrefix == 0x66) // Operand Size
          Prefix = PF_3A_66;

        if (DecodeInst.Flags & DecodeFlags::FLAG_REX_PREFIX)
          Prefix |= PF_3A_REX;

        uint16_t Op = (Prefix << 8) | ReadByte();
        if (NormalOpHeader(FEXCore::X86Tables::H0F3ATableOps, Op)) {
          InstructionDecoded = true;
        }
      break;
      }
      default: // Two byte table!
        // x86-64 abuses three legacy prefixes to extend the table encodings
        // 0x66 - Operand Size prefix
        // 0xF2 - REPNE prefix
        // 0xF3 - REP prefix
        // If any of these three prefixes are used then it falls down the subtable
        // Additionally: If you hit repeat of differnt prefixes then only the LAST one before this one works for subtable selection
        if (DecodeInst.LastEscapePrefix == 0xF3) { // REP
          // Remove prefix so it doesn't effect calculations.
          // This is only an escape prefix rather tan modifier now
          DecodeInst.Flags &= ~DecodeFlags::FLAG_REP_PREFIX;
          if (NormalOpHeader(FEXCore::X86Tables::RepModOps, EscapeOp)) {
            InstructionDecoded = true;
          }
        }
        else if (DecodeInst.LastEscapePrefix == 0xF2) { // REPNE
          // Remove prefix so it doesn't effect calculations.
          // This is only an escape prefix rather tan modifier now
          DecodeInst.Flags &= ~DecodeFlags::FLAG_REPNE_PREFIX;
          if (NormalOpHeader(FEXCore::X86Tables::RepNEModOps, EscapeOp)) {
            InstructionDecoded = true;
          }
        }
        else if (DecodeInst.LastEscapePrefix == 0x66) { // Operand Size
          // Remove prefix so it doesn't effect calculations.
          // This is only an escape prefix rather tan modifier now
          DecodeInst.Flags &= ~DecodeFlags::FLAG_OPERAND_SIZE;
          if (NormalOpHeader(FEXCore::X86Tables::OpSizeModOps, EscapeOp)) {
            InstructionDecoded = true;
          }
        }
        else if (NormalOpHeader(FEXCore::X86Tables::SecondBaseOps, EscapeOp)) {
          InstructionDecoded = true;
        }
      break;
      }
    break;
    }
    case 0x66: // Operand Size prefix
      DecodeInst.Flags |= DecodeFlags::FLAG_OPERAND_SIZE;
      DecodeInst.LastEscapePrefix = Op;
    break;
    case 0x67: // Address Size override prefix
      DecodeInst.Flags |= DecodeFlags::FLAG_ADDRESS_SIZE;
    break;
    case 0x26: // ES legacy prefix
    case 0x2E: // CS legacy prefix
    case 0x3E: // DS legacy prefix
      // Annoyingly GCC generates NOP ops with these prefixes
      // Just ignore them for now
      // eg. 66 2e 0f 1f 84 00 00 00 00 00 nop    WORD PTR cs:[rax+rax*1+0x0]
    break;
    case 0x40: // REX - 0x40-0x4F
    case 0x41:
    case 0x42:
    case 0x43:
    case 0x44:
    case 0x45:
    case 0x46:
    case 0x47:
    case 0x48:
    case 0x49:
    case 0x4A:
    case 0x4B:
    case 0x4C:
    case 0x4D:
    case 0x4E:
    case 0x4F: {
      DecodeInst.Flags |= DecodeFlags::FLAG_REX_PREFIX;

      // Widening displacement
      if (Op & 0b1000)
        DecodeInst.Flags |= DecodeFlags::FLAG_REX_WIDENING;

      // XGPR_B bit set
      if (Op & 0b0001)
        DecodeInst.Flags |= DecodeFlags::FLAG_REX_XGPR_B;

      // XGPR_X bit set
      if (Op & 0b0010)
        DecodeInst.Flags |= DecodeFlags::FLAG_REX_XGPR_X;

      // XGPR_R bit set
      if (Op & 0b0100)
        DecodeInst.Flags |= DecodeFlags::FLAG_REX_XGPR_R;

    break;
    }
    case 0xF0: // LOCK prefix
      DecodeInst.Flags |= DecodeFlags::FLAG_LOCK;
    break;
    case 0xF2: // REPNE prefix
      DecodeInst.Flags |= DecodeFlags::FLAG_REPNE_PREFIX;
      DecodeInst.LastEscapePrefix = Op;
    break;
    case 0xF3: // REP prefix
      DecodeInst.Flags |= DecodeFlags::FLAG_REP_PREFIX;
      DecodeInst.LastEscapePrefix = Op;
    break;
    case 0x64: // FS prefix
      DecodeInst.Flags |= DecodeFlags::FLAG_FS_PREFIX;
    break;
    case 0x65: // GS prefix
      DecodeInst.Flags |= DecodeFlags::FLAG_FS_PREFIX;
    break;
    default: { // Default base table
      if (NormalOpHeader(FEXCore::X86Tables::BaseOps, Op)) {
        InstructionDecoded = true;
      }
      else {
        LogMan::Msg::E("Error during instruction decoding");
        ErrorDuringDecoding = true;
      }
      break;
    }
    }

  }
  return !ErrorDuringDecoding;
}

bool Decoder::BlockEndCanContinuePast(FEXCore::X86Tables::DecodedInst const &Inst) {
  if (!CTX->Config.Multiblock)
     return false;

  // Have we had a conditional branch past this PC previously?
  // We can continue in this case
  //
  // ex.
  // test eax, eax
  // jne .Continue
  // ud2           <--- We can continue past this instruction, which is a block ender
  // .Continue:
  // ...

  return Inst.PC <= MaxCondBranchForward;
}

bool Decoder::BranchTargetInMultiblockRange(FEXCore::X86Tables::DecodedInst const &Inst) {
  if (!CTX->Config.Multiblock)
     return false;

  // If the RIP setting is conditional AND within our symbol range then it can be considered for multiblock
  uint64_t TargetRIP = 0;
  bool Conditional = true;
  switch (Inst.OP) {
  case 0x70 ... 0x7F: { // Conditional JUMP
    // Source is a literal
    // auto RIPOffset = LoadSource(Op, Op->Src1, Op->Flags);
    // auto RIPTargetConst = _Constant(Op->PC + Op->InstSize);
    // Target offset is PC + InstSize + Literal
    TargetRIP = Inst.PC + Inst.InstSize + Inst.Src1.TypeLiteral.Literal;
  break;
  }
  case 0xE9:
  case 0xEB: // Both are unconditional JMP instructions
    TargetRIP = Inst.PC + Inst.InstSize + Inst.Src1.TypeLiteral.Literal;
    Conditional = false;
  break;
  case 0xC2: // RET imm
  case 0xC3: // RET
    Conditional = false;
  break;
  default:
    return false;
  break;
  }

  // If the target RIP is within the symbol ranges then we are golden
  if (TargetRIP > SymbolMinAddress && TargetRIP <= SymbolMaxAddress) {
    // Update our conditional branch ranges before we return
    if (Conditional) {
      MaxCondBranchForward = std::max(MaxCondBranchForward, TargetRIP);
      MaxCondBranchBackwards = std::min(MaxCondBranchBackwards, TargetRIP);
    }
    //JumpTargets.emplace(TargetRIP);
    return true;
  }
  return false;
}

bool Decoder::DecodeInstructionsInBlock(uint8_t const* InstStream, uint64_t PC) {
  // Reset internal state management
  DecodedSize = 0;
  MaxCondBranchForward = 0;
  MaxCondBranchBackwards = ~0ULL;

  // XXX: Load symbol data
  SymbolAvailable = false;
  EntryPoint = PC;
  JumpTargets.clear();

  bool ErrorDuringDecoding = false;
  bool Done = false;
  uint64_t PCOffset = 0;

  // If we don't have symbols available then we become a bit optimistic about multiblock ranges
  if (!SymbolAvailable) {
    // If we don't have a symbol available then assume all branches are valid for multiblock
    SymbolMaxAddress = ~0ULL;
    SymbolMinAddress = 0;
  }

//  LogMan::Msg::I("============================");
//  LogMan::Msg::I(">>> Started decoding at 0x%lx", PC);
//  LogMan::Msg::I("============================");

  while(!Done) {
    ErrorDuringDecoding = !DecodeInstruction(InstStream, PC + PCOffset);
    if (ErrorDuringDecoding) {
      LogMan::Msg::D("Couldn't Decode something at 0x%lx, Started at 0x%lx", PC + PCOffset, PC);
      break;
    }
    auto &DecodeInst = DecodedBuffer[DecodedSize];
    ++DecodedSize;

    bool CanContinue = false;
    if (!(DecodeInst.TableInfo->Flags &
        (FEXCore::X86Tables::InstFlags::FLAGS_BLOCK_END | FEXCore::X86Tables::InstFlags::FLAGS_SETS_RIP))) {
      // If this isn't a block ender then we can keep going regardless
      CanContinue = true;
    }

    // If this is an instruction that just completely kills a block then just end currently
    // XXX: If we've had a conditional branch past this then keep going
    if (DecodeInst.TableInfo->Flags & FEXCore::X86Tables::InstFlags::FLAGS_BLOCK_END) {
      CanContinue = BlockEndCanContinuePast(DecodeInst);
    }

    if (DecodeInst.TableInfo->Flags & FEXCore::X86Tables::InstFlags::FLAGS_SETS_RIP) {
      // If we have multiblock enabled
      // If the branch target is within our multiblock range then we can keep going on
      // We don't want to short circuit this since we want to calculate our ranges still
      CanContinue = CanContinue | BranchTargetInMultiblockRange(DecodeInst);
    }

    if (!CanContinue) {
      break;
    }

    if (DecodedSize >= CTX->Config.MaxInstPerBlock) {
      break;
    }

    if (DecodedSize >= DecodedBuffer.size()) {
      break;
    }
    PCOffset += DecodeInst.InstSize;
    InstStream += DecodeInst.InstSize;
  }

  return !ErrorDuringDecoding;
}

}
