// ============================================================================
//  AOTDroid-Adaptive-iOS  |  src/runtime/domain.h
//  Single-Process Multi-Domain / Thread-Namespace Isolator.
//
//  iOS terminates any app that calls fork(); the AOSP userspace we boot
//  (init, zygote64, servicemanager, system_server...) is built around
//  fork()/execve()/wait4()/kill() semantics. This module replaces those
//  process primitives with in-process "domains":
//
//   - every Android "process" is a ProcessDomain: a pid, a thread group, a
//     binder routing identity, a cwd, a signal mask and an exit status
//   - clone(CLONE_VM|CLONE_THREAD|...) creates pthreads inside the domain
//   - clone(fork-ish)/fork from audited callers (init, zygote) creates a NEW
//     domain whose threads continue on a relocated copy of the caller stack
//     (stack snapshot + frame-pointer chain relocation) -- see domain.cpp
//   - execve() re-homes the current thread into a pre-linked image from the
//     exec registry (converted AOSP binaries), fresh stack, argv/envp/auxv
//   - wait4() blocks on domain-exit condition variables; domain exit raises
//     a synthetic SIGCHLD to the parent domain's signal chain
//   - zygote COW: the zygote heap is mprotect(PROT_READ)-guarded and a
//     SIGSEGV write-fault handler copies the faulting page (page-granular
//     in-process COW), preserving zygote preload semantics without fork()
// ============================================================================
#pragma once
#include "../platform/sys_compat.h"
#include "../runtime/binder_emu.h"

namespace aotdroid {

struct ExecImageLite {
  std::string path;
  void* entry = nullptr;
  void* ctx = nullptr;
};

struct ProcessDomain {
  DomainId id = kInvalidDomain;
  int pid = 0;
  int ppid = 0;
  std::string name;
  std::string cwd = "/";
  std::atomic<int> threadCount{0};
  std::atomic<bool> exited{false};
  std::atomic<int> exitStatus{0};
  // zygote COW-guarded ranges [start, start+len)
  struct CowRange { uint64_t start, len; };
  std::vector<CowRange> cowRanges;
  mutable std::mutex mu;
  std::condition_variable exitCv;
};

class DomainTable {
 public:
  static DomainTable& get();

  DomainId current();                 // domain of the calling thread
  DomainId attach(DomainId d);        // re-label calling thread
  DomainId create(const std::string& name, DomainId parent, int pidHint = 0);
  ProcessDomain* find(DomainId d);
  ProcessDomain* findByPid(int pid);

  long currentPid();
  long parentPid();
  long currentTid();
  const std::string& currentCwd();
  void setCwd(const char* path);
  const std::string& threadName();
  void setThreadName(const char* name);

  // clone(): pthread-based threads for CLONE_VM|CLONE_THREAD; new domain for
  // fork-style clones from audited callers; -EPERM otherwise.
  long clone(uint64_t flags, void* childStack, void* parentTid, void* childTid,
             void* tls);
  void execve(const void* execImageOpaque, char* const argv[], char* const envp[]);
  [[noreturn]] void threadExit(int code);
  void domainExitCurrent(int code);
  long wait4(int pid, int* status, int options);
  long tgkill(int tgid, int tid, int sig);

  // zygote COW guard registration + fault handler (called from signal chain)
  void cowAddRange(DomainId d, uint64_t start, uint64_t len);
  bool cowHandleWriteFault(void* faultAddr);

  // test hook: run child-continuation via callback on host builds
  void setHostCloneChildFn(void (*fn)(void*));
  void* hostCloneChildArg = nullptr;

 private:
  DomainTable() = default;
  std::mutex mu_;
  std::unordered_map<DomainId, std::shared_ptr<ProcessDomain>> byId_;
  std::unordered_map<int, DomainId> byPid_;
  DomainId nextId_ = 1;
  int nextPid_ = 2;                   // 1 = init
  void (*hostCloneChild_)(void*) = nullptr;
};

inline DomainTable& domains() { return DomainTable::get(); }

}  // namespace aotdroid
