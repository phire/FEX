#include "Interface/Core/JIT/x86_64/JITClass.h"
#include "Interface/IR/Passes/RegisterAllocationPass.h"

namespace FEXCore::CPU {
#define DEF_OP(x) void X86JITCore::Op_##x(FEXCore::IR::IROp_Header *IROp, uint32_t Node)
DEF_OP(CASPair) {
  auto Op = IROp->C<IR::IROp_CAS>();
  uint8_t OpSize = IROp->Size;

  // Args[0]: Desired
  // Args[1]: Expected
  // Args[2]: Pointer
  // DataSrc = *Src1
  // if (DataSrc == Src3) { *Src1 == Src2; } Src2 = DataSrc
  // This will write to memory! Careful!
  // Third operand must be a calculated guest memory address
  //OrderedNode *CASResult = _CAS(Src3, Src2, Src1);
  auto Dst = GetSrcPair<RA_64>(Node);
  auto Expected = GetSrcPair<RA_64>(Op->Header.Args[0].ID());
  auto Desired = GetSrcPair<RA_64>(Op->Header.Args[1].ID());
  auto MemSrc = GetSrc<RA_64>(Op->Header.Args[2].ID());

  Xbyak::Reg MemReg = MemSrc;

  mov(rax, Expected.first);
  mov(rdx, Expected.second);

  mov(rbx, Desired.first);
  mov(rcx, Desired.second);

  // RDI(Or Source) now contains pointer
  // RDX:RAX contains our expected value
  // RCX:RBX contains our desired

  lock();

  switch (OpSize) {
    case 4: {
      cmpxchg8b(dword [MemReg]);
      // EDX:EAX now contains the result
      mov(Dst.first.cvt32(), eax);
      mov(Dst.second.cvt32(), edx);
    break;
    }
    case 8: {
      cmpxchg16b(qword [MemReg]);
      // RDX:RAX now contains the result
      mov(Dst.first, rax);
      mov(Dst.second, rdx);
    break;
    }
    default: LogMan::Msg::A("Unsupported: %d", OpSize);
  }
}

DEF_OP(CAS) {
  auto Op = IROp->C<IR::IROp_CAS>();
  uint8_t OpSize = IROp->Size;

  // Args[0]: Desired
  // Args[1]: Expected
  // Args[2]: Pointer
  // DataSrc = *Src1
  // if (DataSrc == Src3) { *Src1 == Src2; } Src2 = DataSrc
  // This will write to memory! Careful!
  // Third operand must be a calculated guest memory address
  //OrderedNode *CASResult = _CAS(Src3, Src2, Src1);

  Xbyak::Reg MemReg = GetSrc<RA_64>(Op->Header.Args[2].ID());

  mov(rdx, GetSrc<RA_64>(Op->Header.Args[1].ID()));
  mov(rax, GetSrc<RA_64>(Op->Header.Args[0].ID()));

  // RCX now contains pointer
  // RAX contains our expected value
  // RDX contains our desired

  lock();

  switch (OpSize) {
  case 1: {
    cmpxchg(byte [MemReg], dl);
    movzx(rax, al);
  break;
  }
  case 2: {
    cmpxchg(word [MemReg], dx);
    movzx(rax, ax);
  break;
  }
  case 4: {
    cmpxchg(dword [MemReg], edx);
  break;
  }
  case 8: {
    cmpxchg(qword [MemReg], rdx);
  break;
  }
  default: LogMan::Msg::A("Unsupported: %d", OpSize);
  }

  // RAX now contains the result
  mov (GetDst<RA_64>(Node), rax);
}

DEF_OP(AtomicAdd) {
  auto Op = IROp->C<IR::IROp_AtomicAdd>();

  Xbyak::Reg MemReg = GetSrc<RA_64>(Op->Header.Args[0].ID());

  lock();
  switch (Op->Size) {
    case 1:
      add(byte [MemReg], GetSrc<RA_8>(Op->Header.Args[1].ID()));
      break;
    case 2:
      add(word [MemReg], GetSrc<RA_16>(Op->Header.Args[1].ID()));
      break;
    case 4:
      add(dword [MemReg], GetSrc<RA_32>(Op->Header.Args[1].ID()));
      break;
    case 8:
      add(qword [MemReg], GetSrc<RA_64>(Op->Header.Args[1].ID()));
      break;
    default:  LogMan::Msg::A("Unhandled AtomicAdd size: %d", Op->Size);
  }
}

DEF_OP(AtomicSub) {
  auto Op = IROp->C<IR::IROp_AtomicSub>();

  Xbyak::Reg MemReg = GetSrc<RA_64>(Op->Header.Args[0].ID());
  lock();
  switch (Op->Size) {
    case 1:
      sub(byte [MemReg], GetSrc<RA_8>(Op->Header.Args[1].ID()));
      break;
    case 2:
      sub(word [MemReg], GetSrc<RA_16>(Op->Header.Args[1].ID()));
      break;
    case 4:
      sub(dword [MemReg], GetSrc<RA_32>(Op->Header.Args[1].ID()));
      break;
    case 8:
      sub(qword [MemReg], GetSrc<RA_64>(Op->Header.Args[1].ID()));
      break;
    default:  LogMan::Msg::A("Unhandled AtomicAdd size: %d", Op->Size);
  }
}

DEF_OP(AtomicAnd) {
  auto Op = IROp->C<IR::IROp_AtomicAnd>();

  Xbyak::Reg MemReg = GetSrc<RA_64>(Op->Header.Args[0].ID());
  lock();
  switch (Op->Size) {
    case 1:
      and_(byte [MemReg], GetSrc<RA_8>(Op->Header.Args[1].ID()));
      break;
    case 2:
      and_(word [MemReg], GetSrc<RA_16>(Op->Header.Args[1].ID()));
      break;
    case 4:
      and_(dword [MemReg], GetSrc<RA_32>(Op->Header.Args[1].ID()));
      break;
    case 8:
      and_(qword [MemReg], GetSrc<RA_64>(Op->Header.Args[1].ID()));
      break;
    default:  LogMan::Msg::A("Unhandled AtomicAdd size: %d", Op->Size);
  }
}

DEF_OP(AtomicOr) {
  auto Op = IROp->C<IR::IROp_AtomicOr>();

  Xbyak::Reg MemReg = GetSrc<RA_64>(Op->Header.Args[0].ID());
  lock();
  switch (Op->Size) {
    case 1:
      or_(byte [MemReg], GetSrc<RA_8>(Op->Header.Args[1].ID()));
      break;
    case 2:
      or_(word [MemReg], GetSrc<RA_16>(Op->Header.Args[1].ID()));
      break;
    case 4:
      or_(dword [MemReg], GetSrc<RA_32>(Op->Header.Args[1].ID()));
      break;
    case 8:
      or_(qword [MemReg], GetSrc<RA_64>(Op->Header.Args[1].ID()));
      break;
    default:  LogMan::Msg::A("Unhandled AtomicAdd size: %d", Op->Size);
  }
}

DEF_OP(AtomicXor) {
  auto Op = IROp->C<IR::IROp_AtomicXor>();

  Xbyak::Reg MemReg = GetSrc<RA_64>(Op->Header.Args[0].ID());
  lock();
  switch (Op->Size) {
    case 1:
      xor_(byte [MemReg], GetSrc<RA_8>(Op->Header.Args[1].ID()));
      break;
    case 2:
      xor_(word [MemReg], GetSrc<RA_16>(Op->Header.Args[1].ID()));
      break;
    case 4:
      xor_(dword [MemReg], GetSrc<RA_32>(Op->Header.Args[1].ID()));
      break;
    case 8:
      xor_(qword [MemReg], GetSrc<RA_64>(Op->Header.Args[1].ID()));
      break;
    default:  LogMan::Msg::A("Unhandled AtomicAdd size: %d", Op->Size);
  }
}

DEF_OP(AtomicSwap) {
  auto Op = IROp->C<IR::IROp_AtomicSwap>();

  Xbyak::Reg MemReg = rax;
  mov(MemReg, GetSrc<RA_64>(Op->Header.Args[0].ID()));

  switch (Op->Size) {
    case 1:
      mov(GetDst<RA_8>(Node), GetSrc<RA_8>(Op->Header.Args[1].ID()));
      lock();
      xchg(byte [MemReg], GetDst<RA_8>(Node));
      break;
    case 2:
      mov(GetDst<RA_16>(Node), GetSrc<RA_16>(Op->Header.Args[1].ID()));
      lock();
      xchg(word [MemReg], GetDst<RA_16>(Node));
      break;
    case 4:
      mov(GetDst<RA_32>(Node), GetSrc<RA_32>(Op->Header.Args[1].ID()));
      lock();
      xchg(dword [MemReg], GetDst<RA_32>(Node));
      break;
    case 8:
      mov(GetDst<RA_64>(Node), GetSrc<RA_64>(Op->Header.Args[1].ID()));
      lock();
      xchg(qword [MemReg], GetDst<RA_64>(Node));
      break;
    default:  LogMan::Msg::A("Unhandled AtomicAdd size: %d", Op->Size);
  }
}

DEF_OP(AtomicFetchAdd) {
  auto Op = IROp->C<IR::IROp_AtomicFetchAdd>();

  Xbyak::Reg MemReg = GetSrc<RA_64>(Op->Header.Args[0].ID());
  switch (Op->Size) {
    case 1:
      mov(cl, GetSrc<RA_8>(Op->Header.Args[1].ID()));
      lock();
      xadd(byte [MemReg], cl);
      movzx(GetDst<RA_32>(Node), cl);
      break;
    case 2:
      mov(cx, GetSrc<RA_16>(Op->Header.Args[1].ID()));
      lock();
      xadd(word [MemReg], cx);
      movzx(GetDst<RA_32>(Node), cx);
      break;
    case 4:
      mov(ecx, GetSrc<RA_32>(Op->Header.Args[1].ID()));
      lock();
      xadd(dword [MemReg], ecx);
      mov(GetDst<RA_32>(Node), ecx);
      break;
    case 8:
      mov(rcx, GetSrc<RA_64>(Op->Header.Args[1].ID()));
      lock();
      xadd(qword [MemReg], rcx);
      mov(GetDst<RA_64>(Node), rcx);
      break;
    default:  LogMan::Msg::A("Unhandled AtomicFetchAdd size: %d", Op->Size);
  }
}

DEF_OP(AtomicFetchSub) {
  auto Op = IROp->C<IR::IROp_AtomicFetchSub>();

  Xbyak::Reg MemReg = GetSrc<RA_64>(Op->Header.Args[0].ID());
  switch (Op->Size) {
    case 1:
      mov(cl, GetSrc<RA_8>(Op->Header.Args[1].ID()));
      neg(cl);
      lock();
      xadd(byte [MemReg], cl);
      movzx(GetDst<RA_32>(Node), cl);
      break;
    case 2:
      mov(cx, GetSrc<RA_16>(Op->Header.Args[1].ID()));
      neg(cx);
      lock();
      xadd(word [MemReg], cx);
      movzx(GetDst<RA_32>(Node), cx);
      break;
    case 4:
      mov(ecx, GetSrc<RA_32>(Op->Header.Args[1].ID()));
      neg(ecx);
      lock();
      xadd(dword [MemReg], ecx);
      mov(GetDst<RA_32>(Node), ecx);
      break;
    case 8:
      mov(rcx, GetSrc<RA_64>(Op->Header.Args[1].ID()));
      neg(rcx);
      lock();
      xadd(qword [MemReg], rcx);
      mov(GetDst<RA_64>(Node), rcx);
      break;
    default:  LogMan::Msg::A("Unhandled AtomicFetchAdd size: %d", Op->Size);
  }
}

DEF_OP(AtomicFetchAnd) {
  auto Op = IROp->C<IR::IROp_AtomicFetchAnd>();

  // TMP1 = rax
  Xbyak::Reg MemReg = GetSrc<RA_64>(Op->Header.Args[0].ID());

  switch (Op->Size) {
    case 1: {
      mov(TMP1.cvt8(), byte [MemReg]);

      Label Loop;
      L(Loop);
      mov(TMP2.cvt8(), TMP1.cvt8());
      mov(TMP3.cvt8(), TMP1.cvt8());
      and_(TMP2.cvt8(), GetSrc<RA_8>(Op->Header.Args[1].ID()));

      // Updates RAX with the value from memory
      lock(); cmpxchg(byte [MemReg], TMP2.cvt8());
      jne(Loop);
      // Result is the previous value from memory, which is currently in TMP3
      movzx(GetDst<RA_64>(Node), TMP3.cvt8());
      break;
    }
    case 2: {
      mov(TMP1.cvt16(), word [MemReg]);

      Label Loop;
      L(Loop);
      mov(TMP2.cvt16(), TMP1.cvt16());
      mov(TMP3.cvt16(), TMP1.cvt16());
      and_(TMP2.cvt16(), GetSrc<RA_16>(Op->Header.Args[1].ID()));

      // Updates RAX with the value from memory
      lock(); cmpxchg(word [MemReg], TMP2.cvt16());
      jne(Loop);

      // Result is the previous value from memory, which is currently in TMP3
      movzx(GetDst<RA_64>(Node), TMP3.cvt16());
      break;
    }
    case 4: {
      mov(TMP1.cvt32(), dword [MemReg]);

      Label Loop;
      L(Loop);
      mov(TMP2.cvt32(), TMP1.cvt32());
      mov(TMP3.cvt32(), TMP1.cvt32());
      and_(TMP2.cvt32(), GetSrc<RA_32>(Op->Header.Args[1].ID()));

      // Updates RAX with the value from memory
      lock(); cmpxchg(dword [MemReg], TMP2.cvt32());
      jne(Loop);

      // Result is the previous value from memory, which is currently in TMP3
      mov(GetDst<RA_32>(Node), TMP3.cvt32());
      break;
    }
    case 8: {
      mov(TMP1.cvt64(), qword [MemReg]);

      Label Loop;
      L(Loop);
      mov(TMP2.cvt64(), TMP1.cvt64());
      mov(TMP3.cvt64(), TMP1.cvt64());
      and_(TMP2.cvt64(), GetSrc<RA_64>(Op->Header.Args[1].ID()));

      // Updates RAX with the value from memory
      lock(); cmpxchg(qword [MemReg], TMP2.cvt64());
      jne(Loop);

      // Result is the previous value from memory, which is currently in TMP3
      mov(GetDst<RA_64>(Node), TMP3.cvt64());
      break;
    }
    default:  LogMan::Msg::A("Unhandled AtomicFetchAdd size: %d", Op->Size);
  }
}

DEF_OP(AtomicFetchOr) {
  auto Op = IROp->C<IR::IROp_AtomicFetchOr>();

  // TMP1 = rax
  Xbyak::Reg MemReg = GetSrc<RA_64>(Op->Header.Args[0].ID());
  switch (Op->Size) {
    case 1: {
      mov(TMP1.cvt8(), byte [MemReg]);

      Label Loop;
      L(Loop);
      mov(TMP2.cvt8(), TMP1.cvt8());
      mov(TMP3.cvt8(), TMP1.cvt8());
      or_(TMP2.cvt8(), GetSrc<RA_8>(Op->Header.Args[1].ID()));

      // Updates RAX with the value from memory
      lock(); cmpxchg(byte [MemReg], TMP2.cvt8());
      jne(Loop);
      // Result is the previous value from memory, which is currently in TMP3
      movzx(GetDst<RA_64>(Node), TMP3.cvt8());
      break;
    }
    case 2: {
      mov(TMP1.cvt16(), word [MemReg]);

      Label Loop;
      L(Loop);
      mov(TMP2.cvt16(), TMP1.cvt16());
      mov(TMP3.cvt16(), TMP1.cvt16());
      or_(TMP2.cvt16(), GetSrc<RA_16>(Op->Header.Args[1].ID()));

      // Updates RAX with the value from memory
      lock(); cmpxchg(word [MemReg], TMP2.cvt16());
      jne(Loop);

      // Result is the previous value from memory, which is currently in TMP3
      movzx(GetDst<RA_64>(Node), TMP3.cvt16());
      break;
    }
    case 4: {
      mov(TMP1.cvt32(), dword [MemReg]);

      Label Loop;
      L(Loop);
      mov(TMP2.cvt32(), TMP1.cvt32());
      mov(TMP3.cvt32(), TMP1.cvt32());
      or_(TMP2.cvt32(), GetSrc<RA_32>(Op->Header.Args[1].ID()));

      // Updates RAX with the value from memory
      lock(); cmpxchg(dword [MemReg], TMP2.cvt32());
      jne(Loop);

      // Result is the previous value from memory, which is currently in TMP3
      mov(GetDst<RA_32>(Node), TMP3.cvt32());
      break;
    }
    case 8: {
      mov(TMP1.cvt64(), qword [MemReg]);

      Label Loop;
      L(Loop);
      mov(TMP2.cvt64(), TMP1.cvt64());
      mov(TMP3.cvt64(), TMP1.cvt64());
      or_(TMP2.cvt64(), GetSrc<RA_64>(Op->Header.Args[1].ID()));

      // Updates RAX with the value from memory
      lock(); cmpxchg(qword [MemReg], TMP2.cvt64());
      jne(Loop);

      // Result is the previous value from memory, which is currently in TMP3
      mov(GetDst<RA_64>(Node), TMP3.cvt64());
      break;
    }
    default:  LogMan::Msg::A("Unhandled AtomicFetchAdd size: %d", Op->Size);
  }
}

DEF_OP(AtomicFetchXor) {
  auto Op = IROp->C<IR::IROp_AtomicFetchXor>();

  // TMP1 = rax
  Xbyak::Reg MemReg = GetSrc<RA_64>(Op->Header.Args[0].ID());
  switch (Op->Size) {
    case 1: {
      mov(TMP1.cvt8(), byte [MemReg]);

      Label Loop;
      L(Loop);
      mov(TMP2.cvt8(), TMP1.cvt8());
      mov(TMP3.cvt8(), TMP1.cvt8());
      xor_(TMP2.cvt8(), GetSrc<RA_8>(Op->Header.Args[1].ID()));

      // Updates RAX with the value from memory
      lock(); cmpxchg(byte [MemReg], TMP2.cvt8());
      jne(Loop);
      // Result is the previous value from memory, which is currently in TMP3
      movzx(GetDst<RA_64>(Node), TMP3.cvt8());
      break;
    }
    case 2: {
      mov(TMP1.cvt16(), word [MemReg]);

      Label Loop;
      L(Loop);
      mov(TMP2.cvt16(), TMP1.cvt16());
      mov(TMP3.cvt16(), TMP1.cvt16());
      xor_(TMP2.cvt16(), GetSrc<RA_16>(Op->Header.Args[1].ID()));

      // Updates RAX with the value from memory
      lock(); cmpxchg(word [MemReg], TMP2.cvt16());
      jne(Loop);

      // Result is the previous value from memory, which is currently in TMP3
      movzx(GetDst<RA_64>(Node), TMP3.cvt16());
      break;
    }
    case 4: {
      mov(TMP1.cvt32(), dword [MemReg]);

      Label Loop;
      L(Loop);
      mov(TMP2.cvt32(), TMP1.cvt32());
      mov(TMP3.cvt32(), TMP1.cvt32());
      xor_(TMP2.cvt32(), GetSrc<RA_32>(Op->Header.Args[1].ID()));

      // Updates RAX with the value from memory
      lock(); cmpxchg(dword [MemReg], TMP2.cvt32());
      jne(Loop);

      // Result is the previous value from memory, which is currently in TMP3
      mov(GetDst<RA_32>(Node), TMP3.cvt32());
      break;
    }
    case 8: {
      mov(TMP1.cvt64(), qword [MemReg]);

      Label Loop;
      L(Loop);
      mov(TMP2.cvt64(), TMP1.cvt64());
      mov(TMP3.cvt64(), TMP1.cvt64());
      xor_(TMP2.cvt64(), GetSrc<RA_64>(Op->Header.Args[1].ID()));

      // Updates RAX with the value from memory
      lock(); cmpxchg(qword [MemReg], TMP2.cvt64());
      jne(Loop);

      // Result is the previous value from memory, which is currently in TMP3
      mov(GetDst<RA_64>(Node), TMP3.cvt64());
      break;
    }
    default:  LogMan::Msg::A("Unhandled AtomicFetchAdd size: %d", Op->Size);
  }
}

#undef DEF_OP
void X86JITCore::RegisterAtomicHandlers() {
#define REGISTER_OP(op, x) OpHandlers[FEXCore::IR::IROps::OP_##op] = &X86JITCore::Op_##x
  REGISTER_OP(CASPAIR,        CASPair);
  REGISTER_OP(CAS,            CAS);
  REGISTER_OP(ATOMICADD,      AtomicAdd);
  REGISTER_OP(ATOMICSUB,      AtomicSub);
  REGISTER_OP(ATOMICAND,      AtomicAnd);
  REGISTER_OP(ATOMICOR,       AtomicOr);
  REGISTER_OP(ATOMICXOR,      AtomicXor);
  REGISTER_OP(ATOMICSWAP,     AtomicSwap);
  REGISTER_OP(ATOMICFETCHADD, AtomicFetchAdd);
  REGISTER_OP(ATOMICFETCHSUB, AtomicFetchSub);
  REGISTER_OP(ATOMICFETCHAND, AtomicFetchAnd);
  REGISTER_OP(ATOMICFETCHOR,  AtomicFetchOr);
  REGISTER_OP(ATOMICFETCHXOR, AtomicFetchXor);
#undef REGISTER_OP
}
}

