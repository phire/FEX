#include "Interface/IR/PassManager.h"
#include "Interface/Core/OpcodeDispatcher.h"
#include <FEXCore/Core/CoreState.h>
#include <FEXCore/Core/X86Enums.h>

#include <array>
#include <utility>

namespace FEXCore::IR {

class ReplaceX87 final : public FEXCore::IR::Pass {
public:
  bool Run(IREmitter *IREmit) override;

private:

  OrderedNode* ConvertArgToDouble(IREmitter *IREmit, OrderedNodeWrapper Arg);
};

OrderedNode* ReplaceX87::ConvertArgToDouble(IREmitter *IREmit, OrderedNodeWrapper Arg) {
  auto Node = IREmit->UnwrapNode(Arg);
  auto IROp = IREmit->GetOpHeader(Arg);

  switch (IROp->Op) {
    case OP_F80CVTTO: {
      auto Op = IROp->C<IROp_F80CVTTo>();
      if (Op->Size == 8) {
        // We already have a double
        return IREmit->UnwrapNode(Op->X80Src);
      } else { // Op-Size == 4
        LogMan::Throw::A(Op->Size == 4, "Invalid Size");
        // We have a float, need to convert to double


        auto Double = IREmit->_Float_FToF(8, 4, IREmit->UnwrapNode(Op->X80Src));
        return Double;
      }
      break;
    }
  }
}

bool ReplaceX87::Run(IREmitter *IREmit) {
  FEX_CONFIG_OPT(Unsafe, UNSAFE_REPLACE_X87);
  FEX_CONFIG_OPT(Multiblock, MULTIBLOCK);

  if (Multiblock) {
    // We don't currently have any safe replacement operations
    // And multiblock support isn't implemented
    return false;
  }

  bool Changed = false;
  auto CurrentIR = IREmit->ViewIR();
  auto OriginalWriteCursor = IREmit->GetWriteCursor();

  int CurrentTop = 0;
  OrderedNode* LastTopUse = nullptr;

  std::array<std::pair<OrderedNode*, const FEXCore::IR::IROp_X87StackStore*>, 8> StackStores{};

  // Elimitate stores
  for (auto [CodeNode, IROp] : CurrentIR.GetAllCode()) {
    switch (IROp->Op) {
// Do Context Load/Store Elimination just X87 stack operations

      case FEXCore::IR::OP_X87ADJUSTTOP: {
        auto Op = IROp->CW<IR::IROp_X87AdjustTop>();
        int Offset = Op->Offset;
        CurrentTop += Offset;

        LastTopUse = CodeNode;

        // Remove all Adjust nodes
        IREmit->Remove(CodeNode);
        Changed = true;

        break;
      }
      case FEXCore::IR::OP_X87STACKSTORE: {
        auto Op = IROp->CW<IR::IROp_X87StackStore>();
        int Offset = CurrentTop + Op->OffsetFromTop;

        if (StackStores[Offset & 7].first) {

          // All loads have already been replaced, we can just remove it
          IREmit->Remove(StackStores[Offset & 7].first);
        }
        Op->OffsetFromTop = Offset;
        Changed = true;

        LastTopUse = CodeNode;
        StackStores[Offset & 7] = std::make_pair(CodeNode, Op);
        break;
      }
      case FEXCore::IR::OP_X87STACKLOAD: {
        auto Op = IROp->CW<IR::IROp_X87StackLoad>();
        int Offset = CurrentTop + Op->OffsetFromTop;

        auto [StoreNode, StoreOp] = StackStores[Offset & 7];
        if (StoreOp) {
          auto StoreValue = StoreOp->Value;

          //  Replace this load with store value
          IREmit->ReplaceAllUsesWith(CodeNode, IREmit->UnwrapNode(StoreValue));
        }

        LastTopUse = CodeNode;
        Op->OffsetFromTop = Offset;
        Changed = true;

        break;
      }
      case FEXCore::IR::OP_X87GETTOP: {
        if (CurrentTop) {
          // Flush current top offset
          IREmit->SetWriteCursor(CodeNode);
          IREmit->_X87AdjustTop(CurrentTop);

          // Weird things are happening, Lets just bail
          IREmit->SetWriteCursor(OriginalWriteCursor);
          return Changed;
        }
        break;
      }
      case FEXCore::IR::OP_X87SETTOP: {
        // Weird things are happening, Lets just bail
        IREmit->SetWriteCursor(OriginalWriteCursor);
        return Changed;

        // TODO: it would be possible to continue it's a common pattern to reset top then do math.
      }

 // Lower x87 ops to doubles
      case FEXCore::IR::OP_F80ADD: {
        auto Op = IROp->C<IROp_F80Add>();

        IREmit->SetWriteCursor(CodeNode);
        auto Arg1 = ConvertArgToDouble(IREmit, Op->X80Src1);
        auto Arg2 = ConvertArgToDouble(IREmit, Op->X80Src2);

        auto Result = IREmit->_VFAdd(8, 8, Arg1, Arg2);

        // Idealy most of these will get DCEed later
        auto F80Result = IREmit->_F80CVTTo(Result, 8);
        IREmit->ReplaceAllUsesWith(CodeNode, F80Result);

        //IREmit->
      }

      default: break;
    }
  }

  if (LastTopUse && CurrentTop != 0) {
    // We want our top update to be after the last time top was used
    auto it = IREmit->GetIterator(IREmit->WrapNode(LastTopUse));
    auto [afterNode, afterOp] = *++it;
    IREmit->SetWriteCursor(afterNode);
    IREmit->_X87AdjustTop(CurrentTop);
  }

  // Anything that is above the current top pointer is empty and will throw an underflow/overflow exception if accessed
  // So we can safely delete it
  for (int i = 0; (i & 7) != (CurrentTop & 7); i++) {
    if (StackStores[i & 7].first) {
      IREmit->Remove(StackStores[i & 7].first);
      Changed = true;
    }
  }

  // for (auto [CodeNode, IROp] : CurrentIR.GetAllCode()) {
  //   switch (IROp->Op) {
  //     case FEXCore::IR::OP_X87ADD: {
  //       auto Op = IROp->C<IR::IROp_X87Add>();

  //       //if (Op->X80Src1)

  //       break;
  //     }
  //   }
  // }

  IREmit->SetWriteCursor(OriginalWriteCursor);
  return Changed;
}

FEXCore::IR::Pass* CreateReplaceX87() {
  return new ReplaceX87{};
}


}