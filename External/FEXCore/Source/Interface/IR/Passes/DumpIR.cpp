
#include "Interface/IR/PassManager.h"
#include "Interface/Core/OpcodeDispatcher.h"

#include <string>

namespace FEXCore::IR {

class DumpIR final : public FEXCore::IR::Pass {
public:
  DumpIR(std::string Name, bool WithRA) : Name(Name), WithRA(WithRA) {}
  bool Run(IREmitter *IREmit) override;

private:
  std::string Name;
  bool WithRA;
};


bool DumpIR::Run(IREmitter *IREmit) {
    auto CurrentIR = IREmit->ViewIR();

    IR::RegisterAllocationData *RAData = nullptr;
    if (WithRA && Manager->HasRAPass()) {
        RAData = Manager->GetRAPass() ? Manager->GetRAPass()->GetAllocationData() : nullptr;
    }

    std::stringstream Out;
    Out << "IR at " << Name << std::endl;
    FEXCore::IR::Dump(&Out, &CurrentIR, RAData);

    LogMan::Msg::I("%s", Out.str().c_str());

    return false;
}

FEXCore::IR::Pass* CreateDumpIR(std::string Name, bool WithRa) {
  return new DumpIR(Name, WithRa);
}

}
