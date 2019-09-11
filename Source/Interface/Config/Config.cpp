#include "LogManager.h"
#include "Interface/Context/Context.h"

#include <FEXCore/Config/Config.h>

namespace FEXCore::Config {
  void SetConfig(FEXCore::Context::Context *CTX, ConfigOption Option, uint64_t Config) {
    switch (Option) {
    case FEXCore::Config::CONFIG_MULTIBLOCK:
      CTX->Config.Multiblock = Config != 0;
    break;
    case FEXCore::Config::CONFIG_MAXBLOCKINST:
      CTX->Config.MaxInstPerBlock = Config;
    break;
    case FEXCore::Config::CONFIG_DEFAULTCORE:
      CTX->Config.Core = static_cast<FEXCore::Config::ConfigCore>(Config);
    break;
    case FEXCore::Config::CONFIG_VIRTUALMEMSIZE:
      CTX->Config.VirtualMemSize = Config;
    break;
    case FEXCore::Config::CONFIG_SINGLESTEP:
      CTX->RunningMode = Config != 0 ? FEXCore::Context::CoreRunningMode::MODE_SINGLESTEP : FEXCore::Context::CoreRunningMode::MODE_RUN;
    break;
    default: LogMan::Msg::A("Unknown configuration option");
    }
  }

  uint64_t GetConfig(FEXCore::Context::Context *CTX, ConfigOption Option) {
    switch (Option) {
    case FEXCore::Config::CONFIG_MULTIBLOCK:
      return CTX->Config.Multiblock;
    break;
    case FEXCore::Config::CONFIG_MAXBLOCKINST:
      return CTX->Config.MaxInstPerBlock;
    break;
    case FEXCore::Config::CONFIG_DEFAULTCORE:
      return CTX->Config.Core;
    break;
    case FEXCore::Config::CONFIG_VIRTUALMEMSIZE:
      return CTX->Config.VirtualMemSize;
    break;
    case FEXCore::Config::CONFIG_SINGLESTEP:
      return CTX->RunningMode == FEXCore::Context::CoreRunningMode::MODE_SINGLESTEP ? 1 : 0;
    break;
    default: LogMan::Msg::A("Unknown configuration option");
    }

    return 0;
  }
}
