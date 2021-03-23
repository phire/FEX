#include "Interface/IR/PassManager.h"
#include "Interface/Core/OpcodeDispatcher.h"
#include <FEXCore/Core/CoreState.h>
#include <FEXCore/Core/X86Enums.h>

namespace FEXCore::IR {

class LowerX87 final : public FEXCore::IR::Pass {
public:
  bool Run(IREmitter *IREmit) override;
};

bool LowerX87::Run(IREmitter *IREmit) {
  bool Changed = false;
  auto CurrentIR = IREmit->ViewIR();
  auto OriginalWriteCursor = IREmit->GetWriteCursor();

  for (auto [CodeNode, IROp] : CurrentIR.GetAllCode()) {
    switch (IROp->Op) {
      case FEXCore::IR::OP_X87GETTOP: {
        IREmit->SetWriteCursor(CodeNode);

        auto result = IREmit->_LoadContext(1, offsetof(FEXCore::Core::CPUState, flags) + FEXCore::X86State::X87FLAG_TOP_LOC, GPRClass);
        IREmit->ReplaceAllUsesWith(CodeNode, result);
        Changed = true;
        break;
      }
      case FEXCore::IR::OP_X87SETTOP: {
        auto Op = IROp->C<IR::IROp_X87SetTop>();
        IREmit->SetWriteCursor(CodeNode);

        IREmit->_StoreContext(GPRClass, 1, offsetof(FEXCore::Core::CPUState, flags) + FEXCore::X86State::X87FLAG_TOP_LOC, IREmit->UnwrapNode(Op->Top));
        IREmit->Remove(CodeNode);
        Changed = true;
        break;
      }
      case FEXCore::IR::OP_X87ADJUSTTOP: {
        auto Op = IROp->C<IR::IROp_X87AdjustTop>();
        auto Offset = Op->Offset;
        IREmit->SetWriteCursor(CodeNode);

        auto top = IREmit->_LoadContext(1, offsetof(FEXCore::Core::CPUState, flags) + FEXCore::X86State::X87FLAG_TOP_LOC, GPRClass);
        auto mask = IREmit->_Constant(7);
        OrderedNode* sum = Offset >= 0 ? IREmit->_Add(top, IREmit->_Constant(Offset)) : IREmit->_Sub(top, IREmit->_Constant(-Offset));
        auto new_top = IREmit->_And(sum, mask);

        IREmit->_StoreContext(GPRClass, 1, offsetof(FEXCore::Core::CPUState, flags) + FEXCore::X86State::X87FLAG_TOP_LOC, new_top);
        IREmit->Remove(CodeNode);
        Changed = true;
        break;
      }
      case FEXCore::IR::OP_X87STACKLOAD: {
        auto Op = IROp->C<IR::IROp_X87StackLoad>();
        auto Offset = Op->OffsetFromTop;
        IREmit->SetWriteCursor(CodeNode);

        auto top = IREmit->_LoadContext(1, offsetof(FEXCore::Core::CPUState, flags) + FEXCore::X86State::X87FLAG_TOP_LOC, GPRClass);

        OrderedNode *IndexToLoad = top;
        if (Op->OffsetFromTop != 0) {
          auto mask = IREmit->_Constant(7);
          OrderedNode* sum = Offset >= 0 ? IREmit->_Add(top, IREmit->_Constant(Offset)) : IREmit->_Sub(top, IREmit->_Constant(-Offset));
          IndexToLoad = IREmit->_And(sum, mask);
        }

        auto Result = IREmit->_LoadContextIndexed(IndexToLoad, 16, offsetof(FEXCore::Core::CPUState, mm[0][0]), 16, FPRClass);
        IREmit->ReplaceAllUsesWith(CodeNode, Result);
        break;
      }
      case FEXCore::IR::OP_X87STACKSTORE: {
        auto Op = IROp->C<IR::IROp_X87StackStore>();
        auto Offset = Op->OffsetFromTop;
        IREmit->SetWriteCursor(CodeNode);

        auto top = IREmit->_LoadContext(1, offsetof(FEXCore::Core::CPUState, flags) + FEXCore::X86State::X87FLAG_TOP_LOC, GPRClass);

        OrderedNode *IndexToStore = top;
        if (Op->OffsetFromTop != 0) {
          auto mask = IREmit->_Constant(7);
          OrderedNode* sum = Offset >= 0 ? IREmit->_Add(top, IREmit->_Constant(Offset)) : IREmit->_Sub(top, IREmit->_Constant(-Offset));
          IndexToStore = IREmit->_And(sum, mask);
        }

        IREmit->_StoreContextIndexed(IREmit->UnwrapNode(Op->Value), IndexToStore, 16, offsetof(FEXCore::Core::CPUState, mm[0][0]), 16, FPRClass);
        IREmit->Remove(CodeNode);
        break;
      }
      default: break;
    }
  }

  IREmit->SetWriteCursor(OriginalWriteCursor);
  return Changed;
}

FEXCore::IR::Pass* CreateLowerX87() {
  return new LowerX87{};
}


}