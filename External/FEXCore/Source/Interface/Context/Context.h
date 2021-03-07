#pragma once
#include "Common/JitSymbols.h"
#include "Interface/Core/CPUID.h"
#include "Interface/Core/Frontend.h"
#include "Interface/Core/HostFeatures.h"
#include "Interface/Core/InternalThreadState.h"
#include "Interface/Core/X86HelperGen.h"
#include "Interface/IR/PassManager.h"
#include "Interface/IR/Passes/RegisterAllocationPass.h"
#include <FEXCore/Config/Config.h>
#include <FEXCore/Core/CPUBackend.h>
#include <FEXCore/Utils/Event.h>
#include <stdint.h>

#include <memory>
#include <map>
#include <unordered_map>
#include <set>
#include <mutex>
#include <istream>
#include <ostream>
#include <functional>

namespace FEXCore {
class ThunkHandler;
class BlockSamplingData;
class GdbServer;
class SiganlDelegator;

namespace CPU {
  class Arm64JITCore;
  class X86JITCore;
}
namespace HLE {
class SyscallHandler;
}
}

namespace FEXCore::IR {
  class RegisterAllocationPass;
  class RegisterAllocationData;
  class IRListView;
namespace Validation {
  class IRValidation;
}
}

namespace FEXCore::Context {
  enum CoreRunningMode {
    MODE_RUN        = 0,
    MODE_SINGLESTEP = 1,
  };

  struct Context {
    friend class FEXCore::HLE::SyscallHandler;
  #ifdef JIT_ARM64
    friend class FEXCore::CPU::Arm64JITCore;
  #endif
  #ifdef JIT_X86_64
    friend class FEXCore::CPU::X86JITCore;
  #endif

    friend class FEXCore::IR::Validation::IRValidation;

    struct {
      bool Multiblock {false};
      bool BreakOnFrontendFailure {true};
      int64_t MaxInstPerBlock {-1LL};
      uint64_t VirtualMemSize {1ULL << 36};
      CoreRunningMode RunningMode {CoreRunningMode::MODE_RUN};
      FEXCore::Config::ConfigCore Core {FEXCore::Config::CONFIG_INTERPRETER};
      bool GdbServer {false};
      std::string RootFSPath;
      std::string ThunkLibsPath;

      bool Is64BitMode {true};
      bool TSOEnabled {true};
      FEXCore::Config::ConfigSMCChecks SMCChecks {FEXCore::Config::CONFIG_SMC_MMAN};
      bool ABILocalFlags {false};
      bool ABINoPF {false};

      bool AOTIRCapture {false};
      bool AOTIRLoad {false};

      std::string DumpIR;

      // this is for internal use
      bool ValidateIRarser { false };

    } Config;

    using IntCallbackReturn =  __attribute__((naked)) void(*)(FEXCore::Core::InternalThreadState *Thread, volatile void *Host_RSP);
    IntCallbackReturn InterpreterCallbackReturn;

    FEXCore::HostFeatures HostFeatures;

    std::mutex ThreadCreationMutex;
    uint64_t ThreadID{};
    FEXCore::Core::InternalThreadState* ParentThread;
    std::vector<FEXCore::Core::InternalThreadState*> Threads;
    std::atomic_bool CoreShuttingDown{false};

    std::mutex IdleWaitMutex;
    std::condition_variable IdleWaitCV;
    std::atomic<uint32_t> IdleWaitRefCount{};

    Event PauseWait;
    bool Running{};

    FEXCore::CPUIDEmu CPUID;
    FEXCore::HLE::SyscallHandler *SyscallHandler{};
    std::unique_ptr<FEXCore::ThunkHandler> ThunkHandler;

    CustomCPUFactoryType CustomCPUFactory;
    std::function<void(uint64_t ThreadId, FEXCore::Context::ExitReason)> CustomExitHandler;

    struct AOTIRCacheEntry {
      uint64_t start;
      uint64_t len;
      uint64_t crc;
      IR::IRListView *IR;
      IR::RegisterAllocationData *RAData;
    };

    std::function<std::unique_ptr<std::istream>(const std::string&)> AOTIRLoader;
    std::unordered_map<std::string, std::map<uint64_t, AOTIRCacheEntry>> AOTIRCache;

    struct AddrToFileEntry {
      uint64_t Start;
      uint64_t Len;
      uint64_t Offset;
      std::string fileid;
      void *CachedFileEntry;
    };

    std::map<uint64_t, AddrToFileEntry> AddrToFile;


#ifdef BLOCKSTATS
    std::unique_ptr<FEXCore::BlockSamplingData> BlockData;
#endif

    SignalDelegator *SignalDelegation{};
    X86GeneratedCode X86CodeGen;

    Context();
    ~Context();

    bool InitCore(FEXCore::CodeLoader *Loader);
    FEXCore::Context::ExitReason RunUntilExit();
    int GetProgramStatus();
    bool IsPaused() const { return !Running; }
    void Pause();
    void Run();
    void WaitForThreadsToRun();
    void Step();
    void Stop(bool IgnoreCurrentThread);
    void WaitForIdle();
    void StopThread(FEXCore::Core::InternalThreadState *Thread);
    void SignalThread(FEXCore::Core::InternalThreadState *Thread, FEXCore::Core::SignalEvent Event);

    bool GetGdbServerStatus() { return (bool)DebugServer; }
    void StartGdbServer();
    void StopGdbServer();
    void HandleCallback(uint64_t RIP);
    void RegisterHostSignalHandler(int Signal, HostSignalDelegatorFunction Func);
    void RegisterFrontendHostSignalHandler(int Signal, HostSignalDelegatorFunction Func);

    static void RemoveCodeEntry(FEXCore::Core::InternalThreadState *Thread, uint64_t GuestRIP);

    // Wrapper which takes CpuStateFrame instead of InternalThreadState
    static void RemoveCodeEntryFromJit(FEXCore::Core::CpuStateFrame *Frame, uint64_t GuestRIP) {
      RemoveCodeEntry(Frame->Thread, GuestRIP);
    }

    // Debugger interface
    void CompileRIP(FEXCore::Core::InternalThreadState *Thread, uint64_t RIP);
    uint64_t GetThreadCount() const;
    FEXCore::Core::RuntimeStats *GetRuntimeStatsForThread(uint64_t Thread);
    FEXCore::Core::CPUState GetCPUState();
    bool GetDebugDataForRIP(uint64_t RIP, FEXCore::Core::DebugData *Data);
    bool FindHostCodeForRIP(uint64_t RIP, uint8_t **Code);

    // XXX:
    // bool FindIRForRIP(uint64_t RIP, FEXCore::IR::IntrusiveIRList **ir);
    // void SetIRForRIP(uint64_t RIP, FEXCore::IR::IntrusiveIRList *const ir);
    void LoadEntryList();

    std::tuple<FEXCore::IR::IRListView *, FEXCore::IR::RegisterAllocationData *, uint64_t, uint64_t, uint64_t, uint64_t> GenerateIR(FEXCore::Core::InternalThreadState *Thread, uint64_t GuestRIP);

    std::tuple<void *, FEXCore::IR::IRListView *, FEXCore::Core::DebugData *, FEXCore::IR::RegisterAllocationData *, bool, uint64_t, uint64_t> CompileCode(FEXCore::Core::InternalThreadState *Thread, uint64_t GuestRIP);
    uintptr_t CompileBlock(FEXCore::Core::CpuStateFrame *Frame, uint64_t GuestRIP);

    bool LoadAOTIRCache(std::istream &stream);
    bool WriteAOTIRCache(std::function<std::unique_ptr<std::ostream>(const std::string&)> CacheWriter);
    // Used for thread creation from syscalls
    void InitializeCompiler(FEXCore::Core::InternalThreadState* State, bool CompileThread);
    FEXCore::Core::InternalThreadState* CreateThread(FEXCore::Core::CPUState *NewThreadState, uint64_t ParentTID);
    void InitializeThreadData(FEXCore::Core::InternalThreadState *Thread);
    void InitializeThread(FEXCore::Core::InternalThreadState *Thread);
    void CopyMemoryMapping(FEXCore::Core::InternalThreadState *ParentThread, FEXCore::Core::InternalThreadState *ChildThread);
    void RunThread(FEXCore::Core::InternalThreadState *Thread);

    std::vector<FEXCore::Core::InternalThreadState*> *const GetThreads() { return &Threads; }

    void AddNamedRegion(uintptr_t Base, uintptr_t Size, uintptr_t Offset, const std::string &filename);
    void RemoveNamedRegion(uintptr_t Base, uintptr_t Size);

#if ENABLE_JITSYMBOLS
    FEXCore::JITSymbols Symbols;
#endif

  protected:
    void ClearCodeCache(FEXCore::Core::InternalThreadState *Thread, bool AlsoClearIRCache);

  private:
    void WaitForIdleWithTimeout();

    void ExecutionThread(FEXCore::Core::InternalThreadState *Thread);
    void NotifyPause();

    void AddBlockMapping(FEXCore::Core::InternalThreadState *Thread, uint64_t Address, void *Ptr, uint64_t Start, uint64_t Length);

    FEXCore::CodeLoader *LocalLoader{};

    // Entry Cache
    bool GetFilenameHash(std::string const &Filename, std::string &Hash);
    void AddThreadRIPsToEntryList(FEXCore::Core::InternalThreadState *Thread);
    void SaveEntryList();
    std::set<uint64_t> EntryList;
    std::vector<uint64_t> InitLocations;
    uint64_t StartingRIP;
    std::mutex ExitMutex;
    std::unique_ptr<GdbServer> DebugServer;

    bool StartPaused = false;
    FEXCore::Config::Value<std::string> AppFilename{FEXCore::Config::CONFIG_APP_FILENAME, ""};
  };

  uint64_t HandleSyscall(FEXCore::HLE::SyscallHandler *Handler, FEXCore::Core::CpuStateFrame *Frame, FEXCore::HLE::SyscallArguments *Args);
}
