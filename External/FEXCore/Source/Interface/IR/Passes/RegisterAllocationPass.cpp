#include "Common/BitSet.h"
#include "Interface/IR/Passes/RegisterAllocationPass.h"
#include "Interface/IR/Passes.h"
#include "Interface/Core/OpcodeDispatcher.h"

#include <iterator>

#include "Interface/IR/Passes/RegisterGraph.inc"

namespace FEXCore::IR {
  class ConstrainedRAPass final : public RegisterAllocationPass {
    public:
      ConstrainedRAPass();
      ~ConstrainedRAPass();
      bool Run(IREmitter *IREmit) override;

      void AllocateRegisterSet(uint32_t RegisterCount, uint32_t ClassCount) override;
      void AddRegisters(FEXCore::IR::RegisterClassType Class, uint32_t RegisterCount) override;
      void AddRegisterConflict(FEXCore::IR::RegisterClassType ClassConflict, uint32_t RegConflict, FEXCore::IR::RegisterClassType Class, uint32_t Reg) override;
      void AllocateRegisterConflicts(FEXCore::IR::RegisterClassType Class, uint32_t NumConflicts) override;

      /**
       * @brief Returns the register and class encoded together
       * Top 32bits is the class, lower 32bits is the register
       */
      uint64_t GetDestRegister(uint32_t Node) override;
    private:

      std::vector<uint32_t> PhysicalRegisterCount;
      std::vector<uint32_t> TopRAPressure;

      RegisterGraph *Graph;
      std::unique_ptr<FEXCore::IR::Pass> LocalCompaction;

      void SpillRegisters(FEXCore::IR::IREmitter *IREmit);

      std::vector<LiveRange> LiveRanges;

      using BlockInterferences = std::vector<uint32_t>;

      std::unordered_map<uint32_t, BlockInterferences> LocalBlockInterferences;
      BlockInterferences GlobalBlockInterferences;

      void CalculateBlockInterferences(FEXCore::IR::IRListView<false> *IR);
      void CalculateBlockNodeInterference(FEXCore::IR::IRListView<false> *IR);
      void AllocateVirtualRegisters();

      FEXCore::IR::NodeWrapperIterator FindFirstUse(FEXCore::IR::IREmitter *IREmit, FEXCore::IR::OrderedNode* Node, FEXCore::IR::NodeWrapperIterator Begin, FEXCore::IR::NodeWrapperIterator End);
      FEXCore::IR::NodeWrapperIterator FindLastUseBefore(FEXCore::IR::IREmitter *IREmit, FEXCore::IR::OrderedNode* Node, FEXCore::IR::NodeWrapperIterator Begin, FEXCore::IR::NodeWrapperIterator End);

      uint32_t FindNodeToSpill(IREmitter *IREmit, RegisterNode *RegisterNode, uint32_t CurrentLocation, LiveRange const *OpLiveRange, int32_t RematCost = -1);
      uint32_t FindSpillSlot(uint32_t Node, FEXCore::IR::RegisterClassType RegisterClass);

      bool RunAllocateVirtualRegisters(IREmitter *IREmit);
  };

  ConstrainedRAPass::ConstrainedRAPass() {
    LocalCompaction.reset(FEXCore::IR::CreateIRCompaction());
  }

  ConstrainedRAPass::~ConstrainedRAPass() {
    FreeRegisterGraph(Graph);
  }

  void ConstrainedRAPass::AllocateRegisterSet(uint32_t RegisterCount, uint32_t ClassCount) {
    // We don't care about Max register count
    PhysicalRegisterCount.resize(ClassCount);
    TopRAPressure.resize(ClassCount);

    Graph = AllocateRegisterGraph(ClassCount);
  }

  void ConstrainedRAPass::AddRegisters(FEXCore::IR::RegisterClassType Class, uint32_t RegisterCount) {
    AllocateRegisters(Graph, Class, DEFAULT_VIRTUAL_REG_COUNT);
    AllocatePhysicalRegisters(Graph, Class, RegisterCount);
    PhysicalRegisterCount[Class] = RegisterCount;
  }

  void ConstrainedRAPass::AddRegisterConflict(FEXCore::IR::RegisterClassType ClassConflict, uint32_t RegConflict, FEXCore::IR::RegisterClassType Class, uint32_t Reg) {
    VirtualAddRegisterConflict(Graph, ClassConflict, RegConflict, Class, Reg);
  }

  void ConstrainedRAPass::AllocateRegisterConflicts(FEXCore::IR::RegisterClassType Class, uint32_t NumConflicts) {
    VirtualAllocateRegisterConflicts(Graph, Class, NumConflicts);
  }

  uint64_t ConstrainedRAPass::GetDestRegister(uint32_t Node) {
    return Graph->Nodes[Node].Head.RegAndClass;
  }

  void ConstrainedRAPass::CalculateBlockInterferences(FEXCore::IR::IRListView<false> *IR) {
    using namespace FEXCore;
    uintptr_t ListBegin = IR->GetListData();
    uintptr_t DataBegin = IR->GetData();

    auto Begin = IR->begin();
    auto Op = Begin();

    IR::OrderedNode *RealNode = Op->GetNode(ListBegin);
    auto HeaderOp = RealNode->Op(DataBegin)->CW<FEXCore::IR::IROp_IRHeader>();
    LogMan::Throw::A(HeaderOp->Header.Op == IR::OP_IRHEADER, "First op wasn't IRHeader");

    IR::OrderedNode *BlockNode = HeaderOp->Blocks.GetNode(ListBegin);

    while (1) {
      auto BlockIROp = BlockNode->Op(DataBegin)->CW<FEXCore::IR::IROp_CodeBlock>();
      LogMan::Throw::A(BlockIROp->Header.Op == IR::OP_CODEBLOCK, "IR type failed to be a code block");

      BlockInterferences *BlockInterferenceVector = &LocalBlockInterferences.try_emplace(BlockNode->Wrapped(ListBegin).ID()).first->second;
      BlockInterferenceVector->reserve(BlockIROp->Last.ID() - BlockIROp->Begin.ID());

      // We grab these nodes this way so we can iterate easily
      auto CodeBegin = IR->at(BlockIROp->Begin);
      auto CodeLast = IR->at(BlockIROp->Last);
      while (1) {
        auto CodeOp = CodeBegin();
        uint32_t Node = CodeOp->ID();
        LiveRange *NodeLiveRange = &LiveRanges[Node];

        if (NodeLiveRange->Begin >= BlockIROp->Begin.ID() &&
            NodeLiveRange->End <= BlockIROp->Last.ID()) {
          // If the live range of this node is FULLY inside of the block
          // Then add it to the block specific interference list
          BlockInterferenceVector->emplace_back(Node);
        }
        else {
          // If the live range is not fully inside the block then add it to the global interference list
          GlobalBlockInterferences.emplace_back(Node);
        }

        // CodeLast is inclusive. So we still need to dump the CodeLast op as well
        if (CodeBegin == CodeLast) {
          break;
        }
        ++CodeBegin;
      }

      if (BlockIROp->Next.ID() == 0) {
        break;
      } else {
        BlockNode = BlockIROp->Next.GetNode(ListBegin);
      }
    }
  }

  void ConstrainedRAPass::CalculateBlockNodeInterference(FEXCore::IR::IRListView<false> *IR) {
    auto AddInterference = [&](uint32_t Node1, uint32_t Node2) {
      RegisterNode *Node = &Graph->Nodes[Node1];
      Node->Interference.Set(Node2);
      Node->InterferenceList[Node->Head.InterferenceCount++] = Node2;
    };

    auto CheckInterferenceNodeSizes = [&](uint32_t Node1, uint32_t MaxNewNodes) {
      RegisterNode *Node = &Graph->Nodes[Node1];
      uint32_t NewListMax = Node->Head.InterferenceCount + MaxNewNodes;
      if (Node->InterferenceListSize <= NewListMax) {
        Node->InterferenceListSize = std::max(Node->InterferenceListSize * 2U, (uint32_t)AlignUp(NewListMax, DEFAULT_INTERFERENCE_LIST_COUNT));
        Node->InterferenceList = reinterpret_cast<uint32_t*>(realloc(Node->InterferenceList, Node->InterferenceListSize * sizeof(uint32_t)));
      }
    };
    using namespace FEXCore;
    uintptr_t ListBegin = IR->GetListData();
    uintptr_t DataBegin = IR->GetData();

    auto Begin = IR->begin();
    auto Op = Begin();

    IR::OrderedNode *RealNode = Op->GetNode(ListBegin);
    auto HeaderOp = RealNode->Op(DataBegin)->CW<FEXCore::IR::IROp_IRHeader>();
    LogMan::Throw::A(HeaderOp->Header.Op == IR::OP_IRHEADER, "First op wasn't IRHeader");

    IR::OrderedNode *BlockNode = HeaderOp->Blocks.GetNode(ListBegin);

    while (1) {
      auto BlockIROp = BlockNode->Op(DataBegin)->CW<FEXCore::IR::IROp_CodeBlock>();
      LogMan::Throw::A(BlockIROp->Header.Op == IR::OP_CODEBLOCK, "IR type failed to be a code block");

      BlockInterferences *BlockInterferenceVector = &LocalBlockInterferences.try_emplace(BlockNode->Wrapped(ListBegin).ID()).first->second;

      std::vector<uint32_t> Interferences;
      Interferences.reserve(BlockInterferenceVector->size() + GlobalBlockInterferences.size());

      // We grab these nodes this way so we can iterate easily
      auto CodeBegin = IR->at(BlockIROp->Begin);
      auto CodeLast = IR->at(BlockIROp->Last);
      while (1) {
        auto CodeOp = CodeBegin();
        uint32_t Node = CodeOp->ID();

        // Check for every interference with the local block's interference
        for (auto RHSNode : *BlockInterferenceVector) {
          if (!(LiveRanges[Node].Begin >= LiveRanges[RHSNode].End ||
                LiveRanges[RHSNode].Begin >= LiveRanges[Node].End)) {
            Interferences.emplace_back(RHSNode);
          }
        }

        // Now check the global block interference vector
        for (auto RHSNode : GlobalBlockInterferences) {
          if (!(LiveRanges[Node].Begin >= LiveRanges[RHSNode].End ||
                LiveRanges[RHSNode].Begin >= LiveRanges[Node].End)) {
            Interferences.emplace_back(RHSNode);
          }
        }

        CheckInterferenceNodeSizes(Node, Interferences.size());
        for (auto RHSNode : Interferences) {
          AddInterference(Node, RHSNode);
        }

        for (auto RHSNode : Interferences) {
          AddInterference(RHSNode, Node);
          CheckInterferenceNodeSizes(RHSNode, 0);
        }

        Interferences.clear();

        // CodeLast is inclusive. So we still need to dump the CodeLast op as well
        if (CodeBegin == CodeLast) {
          break;
        }
        ++CodeBegin;
      }

      if (BlockIROp->Next.ID() == 0) {
        break;
      } else {
        BlockNode = BlockIROp->Next.GetNode(ListBegin);
      }
    }
  }

  void ConstrainedRAPass::AllocateVirtualRegisters() {
    for (uint32_t i = 0; i < Graph->NodeCount; ++i) {
      RegisterNode *CurrentNode = &Graph->Nodes[i];
      if (CurrentNode->Head.RegAndClass == INVALID_REGCLASS)
        continue;

      FEXCore::IR::RegisterClassType RegClass = FEXCore::IR::RegisterClassType{uint32_t(CurrentNode->Head.RegAndClass >> 32)};
      uint64_t RegAndClass = ~0ULL;
      RegisterClass *RAClass = &Graph->Set.Classes[RegClass];
      if (CurrentNode->Head.TiePartner) {
        // In the case that we have a list of nodes that need the same register allocated we need to do something special
        // We need to gather the data from the forward linked list and make sure they all match the virtual register
        std::vector<RegisterNode *> Nodes;
        auto CurrentPartner = CurrentNode;
        while (CurrentPartner) {
          Nodes.emplace_back(CurrentPartner);
          CurrentPartner = CurrentPartner->Head.TiePartner;
        }

        for (uint32_t ri = 0; ri < RAClass->Count; ++ri) {
          uint64_t RegisterToCheck = (static_cast<uint64_t>(RegClass) << 32) + ri;
          if (!DoesNodeSetInterfereWithRegister(Graph, Nodes, RegisterToCheck)) {
            RegAndClass = RegisterToCheck;
            break;
          }
        }

        // If we failed to find a virtual register then allocate more space for them
        if (RegAndClass == ~0ULL) {
          RegAndClass = (static_cast<uint64_t>(RegClass.Val) << 32);
          RegAndClass |= AllocateMoreRegisters(Graph, RegClass);
        }

        TopRAPressure[RegClass] = std::max((uint32_t)RegAndClass, TopRAPressure[RegClass]);

        // Walk the partners and ensure they are all set to the same register now
        for (auto Partner : Nodes) {
          Partner->Head.RegAndClass = RegAndClass;
        }
      }
      else {
        for (uint32_t ri = 0; ri < RAClass->Count; ++ri) {
          uint64_t RegisterToCheck = (static_cast<uint64_t>(RegClass) << 32) + ri;
          if (!DoesNodeInterfereWithRegister(Graph, CurrentNode, RegisterToCheck)) {
            RegAndClass = RegisterToCheck;
            break;
          }
        }

        // If we failed to find a virtual register then allocate more space for them
        if (RegAndClass == ~0ULL) {
          RegAndClass = (static_cast<uint64_t>(RegClass.Val) << 32);
          RegAndClass |= AllocateMoreRegisters(Graph, RegClass);
        }

        TopRAPressure[RegClass] = std::max((uint32_t)RegAndClass, TopRAPressure[RegClass]);
        CurrentNode->Head.RegAndClass = RegAndClass;
      }
    }
  }

  FEXCore::IR::NodeWrapperIterator ConstrainedRAPass::FindFirstUse(FEXCore::IR::IREmitter *IREmit, FEXCore::IR::OrderedNode* Node, FEXCore::IR::NodeWrapperIterator Begin, FEXCore::IR::NodeWrapperIterator End) {
    auto CurrentIR = IREmit->ViewIR();
    uintptr_t ListBegin = CurrentIR.GetListData();
    uintptr_t DataBegin = CurrentIR.GetData();

    uint32_t SearchID = Node->Wrapped(ListBegin).ID();

    while (1) {
      using namespace FEXCore::IR;
      OrderedNodeWrapper *WrapperOp = Begin();
      OrderedNode *RealNode = WrapperOp->GetNode(ListBegin);
      FEXCore::IR::IROp_Header *IROp = RealNode->Op(DataBegin);

      uint8_t NumArgs = FEXCore::IR::GetArgs(IROp->Op);
      for (uint8_t i = 0; i < NumArgs; ++i) {
        uint32_t ArgNode = IROp->Args[i].ID();
        if (ArgNode == SearchID) {
          return Begin;
        }
      }

      // CodeLast is inclusive. So we still need to dump the CodeLast op as well
      if (Begin == End) {
        break;
      }

      ++Begin;
    }

    return FEXCore::IR::NodeWrapperIterator::Invalid();
  }

  FEXCore::IR::NodeWrapperIterator ConstrainedRAPass::FindLastUseBefore(FEXCore::IR::IREmitter *IREmit, FEXCore::IR::OrderedNode* Node, FEXCore::IR::NodeWrapperIterator Begin, FEXCore::IR::NodeWrapperIterator End) {
    auto CurrentIR = IREmit->ViewIR();
    uintptr_t ListBegin = CurrentIR.GetListData();
    uintptr_t DataBegin = CurrentIR.GetData();

    uint32_t SearchID = Node->Wrapped(ListBegin).ID();

    while (1) {
      using namespace FEXCore::IR;
      OrderedNodeWrapper *WrapperOp = End();
      OrderedNode *RealNode = WrapperOp->GetNode(ListBegin);
      FEXCore::IR::IROp_Header *IROp = RealNode->Op(DataBegin);

      if (Node == RealNode) {
        // We walked back all the way to the definition of the IR op
        return End;
      }

      uint8_t NumArgs = FEXCore::IR::GetArgs(IROp->Op);
      for (uint8_t i = 0; i < NumArgs; ++i) {
        uint32_t ArgNode = IROp->Args[i].ID();
        if (ArgNode == SearchID) {
          return End;
        }
      }

      // CodeLast is inclusive. So we still need to dump the CodeLast op as well
      if (Begin == End) {
        break;
      }

      --End;
    }

    return FEXCore::IR::NodeWrapperIterator::Invalid();
  }

  uint32_t ConstrainedRAPass::FindNodeToSpill(IREmitter *IREmit, RegisterNode *RegisterNode, uint32_t CurrentLocation, LiveRange const *OpLiveRange, int32_t RematCost) {
    auto IR = IREmit->ViewIR();
    uintptr_t ListBegin = IR.GetListData();

    uint32_t InterferenceToSpill = ~0U;
    uint32_t InterferenceFarthestNextUse = 0;

    IR::OrderedNodeWrapper NodeOpBegin = IR::OrderedNodeWrapper::WrapOffset(CurrentLocation * sizeof(IR::OrderedNode));
    IR::OrderedNodeWrapper NodeOpEnd = IR::OrderedNodeWrapper::WrapOffset(OpLiveRange->End * sizeof(IR::OrderedNode));
    auto NodeOpBeginIter = IR.at(NodeOpBegin);
    auto NodeOpEndIter = IR.at(NodeOpEnd);

    // Couldn't find register to spill
    // Be more aggressive
    if (InterferenceToSpill == ~0U) {
      for (uint32_t j = 0; j < RegisterNode->Head.InterferenceCount; ++j) {
        uint32_t InterferenceNode = RegisterNode->InterferenceList[j];
        auto *InterferenceLiveRange = &LiveRanges[InterferenceNode];
        if (InterferenceLiveRange->RematCost == -1 ||
            (RematCost != -1 && InterferenceLiveRange->RematCost != RematCost)) {
          continue;
        }

        // If this node's live range fully encompasses the live range of the interference node
        // then spilling that interference node will not lower RA
        // | Our Node             |        Interference |
        // | ========================================== |
        // | 0 - Assign           |                     |
        // | 1                    |              Assign |
        // | 2                    |                     |
        // | 3                    |            Last Use |
        // | 4                    |                     |
        // | 5 - Last Use         |                     |
        // | Range - (0, 5]       |              (1, 3] |
        if (OpLiveRange->Begin <= InterferenceLiveRange->Begin &&
            OpLiveRange->End >= InterferenceLiveRange->End) {
          continue;
        }

        FEXCore::IR::OrderedNodeWrapper InterferenceOp = IR::OrderedNodeWrapper::WrapOffset(InterferenceNode * sizeof(IR::OrderedNode));
        FEXCore::IR::OrderedNode *InterferenceOrderedNode = InterferenceOp.GetNode(ListBegin);

        IR::OrderedNodeWrapper InterferenceNodeOpBegin = IR::OrderedNodeWrapper::WrapOffset(InterferenceLiveRange->Begin * sizeof(IR::OrderedNode));
        IR::OrderedNodeWrapper InterferenceNodeOpEnd = IR::OrderedNodeWrapper::WrapOffset(InterferenceLiveRange->End * sizeof(IR::OrderedNode));
        auto InterferenceNodeOpBeginIter = IR.at(InterferenceNodeOpBegin);
        auto InterferenceNodeOpEndIter = IR.at(InterferenceNodeOpEnd);

        bool Found{};

        // If the nodes live range is entirely encompassed by the interference node's range
        // then spilling that range will /potentially/ lower RA
        // Will only lower register pressure if the interference node does NOT have a use inside of
        // this live range's use
        // | Our Node             |        Interference |
        // | ========================================== |
        // | 0                    |              Assign |
        // | 1 - Assign           |            (No Use) |
        // | 2                    |            (No Use) |
        // | 3 - Last Use         |            (No Use) |
        // | 4                    |                     |
        // | 5                    |            Last Use |
        // | Range - (1, 3]       |              (0, 5] |
        if (CurrentLocation > InterferenceLiveRange->Begin &&
            OpLiveRange->End < InterferenceLiveRange->End) {

          // This will only save register pressure if the interference node
          // does NOT have a use inside of this this node's live range
          // Search only inside the source node's live range to see if there is a use
          auto FirstUseLocation = FindFirstUse(IREmit, InterferenceOrderedNode, NodeOpBeginIter, NodeOpEndIter);
          if (FirstUseLocation == IR::NodeWrapperIterator::Invalid()) {
            // Looks like there isn't a usage of this interference node inside our node's live range
            // This means it is safe to spill this node and it'll result in in lower RA
            // Proper calculation of cost to spill would be to calculate the two distances from
            // (Node->Begin - InterferencePrevUse) + (InterferenceNextUse - Node->End)
            // This would ensure something will spill earlier if its previous use and next use are farther away
            auto InterferenceNodeNextUse = FindFirstUse(IREmit, InterferenceOrderedNode, NodeOpBeginIter, InterferenceNodeOpEndIter);
            auto InterferenceNodePrevUse = FindLastUseBefore(IREmit, InterferenceOrderedNode, InterferenceNodeOpBeginIter, NodeOpBeginIter);
            LogMan::Throw::A(InterferenceNodeNextUse != IR::NodeWrapperIterator::Invalid(), "Couldn't find next usage of op");
            // If there is no use of the interference op prior to our op then it only has initial definition
            if (InterferenceNodePrevUse == IR::NodeWrapperIterator::Invalid()) InterferenceNodePrevUse = InterferenceNodeOpBeginIter;

            uint32_t NextUseDistance = InterferenceNodeNextUse()->ID() - CurrentLocation;
            if (NextUseDistance >= InterferenceFarthestNextUse) {
              Found = true;
              InterferenceToSpill = j;
              InterferenceFarthestNextUse = NextUseDistance;
            }
          }
        }
      }
    }

    if (InterferenceToSpill == ~0U) {
      for (uint32_t j = 0; j < RegisterNode->Head.InterferenceCount; ++j) {
        uint32_t InterferenceNode = RegisterNode->InterferenceList[j];
        auto *InterferenceLiveRange = &LiveRanges[InterferenceNode];
        if (InterferenceLiveRange->RematCost == -1 ||
            (RematCost != -1 && InterferenceLiveRange->RematCost != RematCost)) {
          continue;
        }

        // If this node's live range fully encompasses the live range of the interference node
        // then spilling that interference node will not lower RA
        // | Our Node             |        Interference |
        // | ========================================== |
        // | 0 - Assign           |                     |
        // | 1                    |              Assign |
        // | 2                    |                     |
        // | 3                    |            Last Use |
        // | 4                    |                     |
        // | 5 - Last Use         |                     |
        // | Range - (0, 5]       |              (1, 3] |
        if (OpLiveRange->Begin <= InterferenceLiveRange->Begin &&
            OpLiveRange->End >= InterferenceLiveRange->End) {
          continue;
        }

        FEXCore::IR::OrderedNodeWrapper InterferenceOp = IR::OrderedNodeWrapper::WrapOffset(InterferenceNode * sizeof(IR::OrderedNode));
        FEXCore::IR::OrderedNode *InterferenceOrderedNode = InterferenceOp.GetNode(ListBegin);

        IR::OrderedNodeWrapper InterferenceNodeOpEnd = IR::OrderedNodeWrapper::WrapOffset(InterferenceLiveRange->End * sizeof(IR::OrderedNode));
        auto InterferenceNodeOpEndIter = IR.at(InterferenceNodeOpEnd);

        bool Found{};

        // If the node's live range intersects the interference node
        // but the interference node only overlaps the beginning of our live range
        // then spilling the register will lower register pressure if there is not
        // a use of the interference register at the same node as assignment
        // (So we can spill just before current node assignment)
        // | Our Node             |        Interference |
        // | ========================================== |
        // | 0                    |              Assign |
        // | 1 - Assign           |            (No Use) |
        // | 2                    |            (No Use) |
        // | 3                    |            Last Use |
        // | 4                    |                     |
        // | 5 - Last Use         |                     |
        // | Range - (1, 5]       |              (0, 3] |
        if (!Found &&
            CurrentLocation > InterferenceLiveRange->Begin &&
            OpLiveRange->End > InterferenceLiveRange->End) {
          auto FirstUseLocation = FindFirstUse(IREmit, InterferenceOrderedNode, NodeOpBeginIter, NodeOpBeginIter);

          if (FirstUseLocation == IR::NodeWrapperIterator::Invalid()) {
            // This means that the assignment of our register doesn't use this interference node
            // So we are safe to spill this interference node before assignment of our current node
            auto InterferenceNodeNextUse = FindFirstUse(IREmit, InterferenceOrderedNode, NodeOpBeginIter, InterferenceNodeOpEndIter);
            uint32_t NextUseDistance = InterferenceNodeNextUse()->ID() - CurrentLocation;
            if (NextUseDistance >= InterferenceFarthestNextUse) {
              Found = true;

              InterferenceToSpill = j;
              InterferenceFarthestNextUse = NextUseDistance;
            }
          }
        }

        // If the node's live range intersects the interference node
        // but the interference node only overlaps the end of our live range
        // then spilling the register will lower register pressure if there is
        // not a use of the interference register at the same node as the other node's
        // last use
        // | Our Node             |        Interference |
        // | ========================================== |
        // | 0 - Assign           |                     |
        // | 1                    |                     |
        // | 2                    |              Assign |
        // | 3 - Last Use         |            (No Use) |
        // | 4                    |            (No Use) |
        // | 5                    |            Last Use |
        // | Range - (1, 3]       |              (2, 5] |

        // XXX: This route has a bug in it so it is purposely disabled for now
        if (false && !Found &&
            CurrentLocation <= InterferenceLiveRange->Begin &&
            OpLiveRange->End <= InterferenceLiveRange->End) {
          auto FirstUseLocation = FindFirstUse(IREmit, InterferenceOrderedNode, NodeOpEndIter, NodeOpEndIter);

          if (FirstUseLocation == IR::NodeWrapperIterator::Invalid()) {
            // This means that the assignment of our the interference register doesn't overlap
            // with the final usage of our register, we can spill it and reduce usage
            auto InterferenceNodeNextUse = FindFirstUse(IREmit, InterferenceOrderedNode, NodeOpBeginIter, InterferenceNodeOpEndIter);
            uint32_t NextUseDistance = InterferenceNodeNextUse()->ID() - CurrentLocation;
            if (NextUseDistance >= InterferenceFarthestNextUse) {
              Found = true;

              InterferenceToSpill = j;
              InterferenceFarthestNextUse = NextUseDistance;
            }
          }
        }
      }
    }

    // If we are looking for a specific node then we can safely return not found
    if (RematCost != -1 && InterferenceToSpill == ~0U) {
      return ~0U;
    }

    if (InterferenceToSpill == ~0U) {
      LogMan::Msg::D("node %%ssa%d has %ld interferences, was dumped in to virtual reg %d. Live Range[%d, %d)",
        CurrentLocation, RegisterNode->Head.InterferenceCount, RegisterNode->Head.RegAndClass,
        OpLiveRange->Begin, OpLiveRange->End);
      for (uint32_t j = 0; j < RegisterNode->Head.InterferenceCount; ++j) {
        uint32_t InterferenceNode = RegisterNode->InterferenceList[j];
        auto *InterferenceLiveRange = &LiveRanges[InterferenceNode];

        LogMan::Msg::D("\tInt%d: %%ssa%d Remat: %d [%d, %d)", j, InterferenceNode, InterferenceLiveRange->RematCost, InterferenceLiveRange->Begin, InterferenceLiveRange->End);
      }
    }
    LogMan::Throw::A(InterferenceToSpill != ~0U, "Couldn't find Node to spill");

    return RegisterNode->InterferenceList[InterferenceToSpill];
  }

  uint32_t ConstrainedRAPass::FindSpillSlot(uint32_t Node, FEXCore::IR::RegisterClassType RegisterClass) {
    RegisterNode *CurrentNode = &Graph->Nodes[Node];
    LiveRange *NodeLiveRange = &LiveRanges[Node];
    for (uint32_t i = 0; i < Graph->SpillStack.size(); ++i) {
      SpillStackUnit *SpillUnit = &Graph->SpillStack.at(i);
      if (NodeLiveRange->Begin <= SpillUnit->SpillRange.End &&
          SpillUnit->SpillRange.Begin <= NodeLiveRange->End) {
        SpillUnit->SpillRange.Begin = std::min(SpillUnit->SpillRange.Begin, LiveRanges[Node].Begin);
        SpillUnit->SpillRange.End = std::max(SpillUnit->SpillRange.End, LiveRanges[Node].End);
        CurrentNode->Head.SpillSlot = i;
        return i;
      }
    }

    // Couldn't find a spill slot so just make a new one
    auto StackItem = Graph->SpillStack.emplace_back(SpillStackUnit{Node, RegisterClass});
    StackItem.SpillRange.Begin = NodeLiveRange->Begin;
    StackItem.SpillRange.End = NodeLiveRange->End;
    CurrentNode->Head.SpillSlot = SpillSlotCount;
    SpillSlotCount++;
    return CurrentNode->Head.SpillSlot;
  }

  void ConstrainedRAPass::SpillRegisters(FEXCore::IR::IREmitter *IREmit) {
    using namespace FEXCore;

    auto IR = IREmit->ViewIR();
    uintptr_t ListBegin = IR.GetListData();
    uintptr_t DataBegin = IR.GetData();

    auto Begin = IR.begin();
    auto Op = Begin();
    auto LastCursor = IREmit->GetWriteCursor();

    IR::OrderedNode *RealNode = Op->GetNode(ListBegin);
    auto HeaderOp = RealNode->Op(DataBegin)->CW<FEXCore::IR::IROp_IRHeader>();
    LogMan::Throw::A(HeaderOp->Header.Op == IR::OP_IRHEADER, "First op wasn't IRHeader");

    IR::OrderedNode *BlockNode = HeaderOp->Blocks.GetNode(ListBegin);

    while (1) {
      auto BlockIROp = BlockNode->Op(DataBegin)->CW<FEXCore::IR::IROp_CodeBlock>();
      LogMan::Throw::A(BlockIROp->Header.Op == IR::OP_CODEBLOCK, "IR type failed to be a code block");

      // We grab these nodes this way so we can iterate easily
      auto CodeBegin = IR.at(BlockIROp->Begin);
      auto CodeLast = IR.at(BlockIROp->Last);

      auto BlockBegin = CodeBegin;

      while (1) {
        auto CodeOp = CodeBegin();
        IR::OrderedNode *CodeNode = CodeOp->GetNode(ListBegin);
        auto IROp = CodeNode->Op(DataBegin);

        if (IROp->HasDest) {
          uint32_t Node = CodeOp->ID();
          RegisterNode *CurrentNode = &Graph->Nodes[Node];
          LiveRange *OpLiveRange = &LiveRanges[Node];

          // If this node is allocated above the number of physical registers we have then we need to search the interference list and spill the one
          // that is cheapest
          FEXCore::IR::RegisterClassType RegClass = FEXCore::IR::RegisterClassType{uint32_t(CurrentNode->Head.RegAndClass >> 32)};
          bool NeedsToSpill = (uint32_t)CurrentNode->Head.RegAndClass >= PhysicalRegisterCount.at(RegClass);

          if (NeedsToSpill) {
            bool Spilled = false;

            // First let's just check for constants that we can just rematerialize instead of spilling
            uint32_t InterferenceNode = FindNodeToSpill(IREmit, CurrentNode, Node, OpLiveRange, 1);
            if (InterferenceNode != ~0U) {
              // We want to end the live range of this value here and continue it on first use
              IR::OrderedNodeWrapper ConstantOp = IR::OrderedNodeWrapper::WrapOffset(InterferenceNode * sizeof(IR::OrderedNode));
              IR::OrderedNode *ConstantNode = ConstantOp.GetNode(ListBegin);
              FEXCore::IR::IROp_Constant const *ConstantIROp = ConstantNode->Op(DataBegin)->C<IR::IROp_Constant>();
              LogMan::Throw::A(ConstantIROp->Header.Op == IR::OP_CONSTANT, "This needs to be const");
              // First op post Spill
              auto NextIter = CodeBegin;
              auto FirstUseLocation = FindFirstUse(IREmit, ConstantNode, NextIter, CodeLast);
              LogMan::Throw::A(FirstUseLocation != IR::NodeWrapperIterator::Invalid(), "At %%ssa%d Spilling Op %%ssa%d but Failure to find op use", CodeOp->ID(), InterferenceNode);
              if (FirstUseLocation != IR::NodeWrapperIterator::Invalid()) {
                --FirstUseLocation;
                IR::OrderedNodeWrapper *FirstUseOp = FirstUseLocation();
                IR::OrderedNode *FirstUseOrderedNode = FirstUseOp->GetNode(ListBegin);
                IREmit->SetWriteCursor(FirstUseOrderedNode);
                auto FilledConstant = IREmit->_Constant(ConstantIROp->Constant);
                IREmit->ReplaceAllUsesWithInclusive(ConstantNode, FilledConstant, FirstUseLocation, CodeLast);
                Spilled = true;
              }
            }

            // If we didn't remat a constant then we need to do some real spilling
            if (!Spilled) {
              uint32_t InterferenceNode = FindNodeToSpill(IREmit, CurrentNode, Node, OpLiveRange);
              if (InterferenceNode != ~0U) {
                FEXCore::IR::RegisterClassType InterferenceRegClass = FEXCore::IR::RegisterClassType{uint32_t(Graph->Nodes[InterferenceNode].Head.RegAndClass >> 32)};
                uint32_t SpillSlot = FindSpillSlot(InterferenceNode, InterferenceRegClass);
                RegisterNode *InterferenceRegisterNode = &Graph->Nodes[InterferenceNode];
                LogMan::Throw::A(SpillSlot != ~0U, "Interference Node doesn't have a spill slot!");
                LogMan::Throw::A((InterferenceRegisterNode->Head.RegAndClass & ~0U) != ~0U, "Interference node never assigned a register?");
                LogMan::Throw::A(InterferenceRegClass != ~0U, "Interference node never assigned a register class?");
                LogMan::Throw::A(InterferenceRegisterNode->Head.TiePartner == nullptr, "We don't support spilling PHI nodes currently");

                // This is the op that we need to dump
                FEXCore::IR::OrderedNodeWrapper InterferenceOp = IR::OrderedNodeWrapper::WrapOffset(InterferenceNode * sizeof(IR::OrderedNode));
                FEXCore::IR::OrderedNode *InterferenceOrderedNode = InterferenceOp.GetNode(ListBegin);
                FEXCore::IR::IROp_Header *InterferenceIROp = InterferenceOrderedNode->Op(DataBegin);

                // This will find the last use of this definition
                // Walks from CodeBegin -> BlockBegin to find the last Use
                // Which this is walking backwards to find the first use
                auto LastUseOp = FindLastUseBefore(IREmit, InterferenceOrderedNode, BlockBegin, CodeBegin);

                // Set the write cursor to point of last usage
                IREmit->SetWriteCursor(LastUseOp()->GetNode(ListBegin));

                // Actually spill the node now
                auto SpillOp = IREmit->_SpillRegister(InterferenceOrderedNode, SpillSlot, InterferenceRegClass);
                SpillOp.first->Header.Size = InterferenceIROp->Size;
                SpillOp.first->Header.ElementSize = InterferenceIROp->ElementSize;

                {
                  // Search from the point of spilling to find the first use
                  // Set the write cursor to the first location found and fill at that point
                  auto FirstIter = IR.at(SpillOp.Node->Wrapped(ListBegin));
                  // Just past the spill
                  ++FirstIter;
                  auto FirstUseLocation = FindFirstUse(IREmit, InterferenceOrderedNode, FirstIter, CodeLast);

                  LogMan::Throw::A(FirstUseLocation != NodeWrapperIterator::Invalid(), "At %%ssa%d Spilling Op %%ssa%d but Failure to find op use", CodeOp->ID(), InterferenceNode);
                  if (FirstUseLocation != IR::NodeWrapperIterator::Invalid()) {
                    // We want to fill just before the first use
                    --FirstUseLocation;
                    IR::OrderedNodeWrapper *FirstUseOp = FirstUseLocation();
                    IR::OrderedNode *FirstUseOrderedNode = FirstUseOp->GetNode(ListBegin);

                    IREmit->SetWriteCursor(FirstUseOrderedNode);

                    auto FilledInterference = IREmit->_FillRegister(SpillSlot, InterferenceRegClass);
                    FilledInterference.first->Header.Size = InterferenceIROp->Size;
                    FilledInterference.first->Header.ElementSize = InterferenceIROp->ElementSize;
                    IREmit->ReplaceAllUsesWithInclusive(InterferenceOrderedNode, FilledInterference, FirstUseLocation, CodeLast);
                    Spilled = true;
                  }
                }
              }
            }

            IREmit->SetWriteCursor(LastCursor);
            // We can't spill multiple times in a row. Need to restart
            if (Spilled) {
              return;
            }
          }
        }

        // CodeLast is inclusive. So we still need to dump the CodeLast op as well
        if (CodeBegin == CodeLast) {
          break;
        }
        ++CodeBegin;
      }

      if (BlockIROp->Next.ID() == 0) {
        break;
      } else {
        BlockNode = BlockIROp->Next.GetNode(ListBegin);
      }
    }
  }

  bool ConstrainedRAPass::RunAllocateVirtualRegisters(FEXCore::IR::IREmitter *IREmit) {
    using namespace FEXCore;
    bool Changed = false;

    GlobalBlockInterferences.clear();
    LocalBlockInterferences.clear();

    TopRAPressure.assign(TopRAPressure.size(), 0);

    // We need to rerun compaction every step
    Changed |= LocalCompaction->Run(IREmit);
    auto IR = IREmit->ViewIR();

    uint32_t SSACount = IR.GetSSACount();

    ResetRegisterGraph(Graph, SSACount);
    FindNodeClasses(Graph, &IR);
    CalculateLiveRange(Graph, &IR, &LiveRanges);

    // Linear foward scan based interference calculation is faster for smaller blocks
    // Smarter block based interference calculation is faster for larger blocks
    if (SSACount >= 2048) {
      CalculateBlockInterferences(&IR);
      CalculateBlockNodeInterference(&IR);
    }
    else {
      CalculateNodeInterference(Graph, &IR, &LiveRanges);
    }
    AllocateVirtualRegisters();

    return Changed;
  }


  bool ConstrainedRAPass::Run(IREmitter *IREmit) {
    bool Changed = false;

    auto IR = IREmit->ViewIR();
    uintptr_t ListBegin = IR.GetListData();
    uintptr_t DataBegin = IR.GetData();

    auto Begin = IR.begin();
    auto Op = Begin();

    IR::OrderedNode *RealNode = Op->GetNode(ListBegin);
    auto HeaderOp = RealNode->Op(DataBegin)->CW<FEXCore::IR::IROp_IRHeader>();
    LogMan::Throw::A(HeaderOp->Header.Op == IR::OP_IRHEADER, "First op wasn't IRHeader");

    if (HeaderOp->ShouldInterpret) {
      return false;
    }

    SpillSlotCount = 0;
    Graph->SpillStack.clear();

    while (1) {
      HadFullRA = true;

      // Virtual allocation pass runs the compaction pass per run
      Changed |= RunAllocateVirtualRegisters(IREmit);

      for (size_t i = 0; i < PhysicalRegisterCount.size(); ++i) {
        // Virtual registers fit completely within physical registers
        // Remap virtual 1:1 to physical
        HadFullRA &= TopRAPressure[i] < PhysicalRegisterCount[i];
      }

      if (HadFullRA) {
        break;
      }

      SpillRegisters(IREmit);
      Changed = true;
    }

    return Changed;
  }

  FEXCore::IR::RegisterAllocationPass* CreateRegisterAllocationPass() {
    return new ConstrainedRAPass{};
  }
}
