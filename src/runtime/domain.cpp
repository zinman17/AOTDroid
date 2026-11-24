// ============================================================================
//  AOTDroid-Adaptive-iOS  |  src/runtime/domain.cpp   (fork-less isolator)
// ============================================================================
#include "domain.h"
#include "../linker/syscall_bridge.h"
#include <signal.h>

namespace aotdroid {

namespace {
thread_local DomainId t_currentDomain = kInvalidDomain;
thread_local std::string t_threadName = "aotdroid";
thread_local int t_tid = 0;

std::atomic<int> g_nextTid{100};

struct CloneArgs {
  uint64_t flags;
  void* childStack;
  void* parentTid;
  void* childTid;
  void* tls;
  DomainId newDomain;      // for fork-style clones
  void (*fn)(void*);       // host-test path
  void* fnArg;
};

// pthread child for thread-style clone (CLONE_VM|CLONE_THREAD): the child
// must appear to return 0 from the clone() call site. On device builds the
// arm64 path in syscall_bridge.cpp performs the register-level continuation;
// this entry covers the portable/test path where clone is invoked with an
// explicit fn (host unit tests and the container bootstrapper).
void* cloneChildEntry(void* p) {
  CloneArgs* ca = (CloneArgs*)p;
  t_tid = g_nextTid.fetch_add(1);
  if (ca->tls) aotdroid_set_android_tls_c(ca->tls);
  if (ca->newDomain != kInvalidDomain) t_currentDomain = ca->newDomain;
  if (ca->childTid) *(int*)ca->childTid = t_tid;
  int rc = 0;
  if (ca->fn) { ca->fn(ca->fnArg); }      // host/test continuation
  else if (ca->newDomain != kInvalidDomain) {
    // fork-style child on host builds: exit immediately with a clean status
    domains().domainExitCurrent(0);
    return nullptr;
  }
  domains().threadExit(rc);
  return nullptr;
}
}  // namespace

DomainTable& DomainTable::get() { static DomainTable t; return t; }

DomainId DomainTable::current() {
  return t_currentDomain == kInvalidDomain ? 1 : t_currentDomain;
}
DomainId DomainTable::attach(DomainId d) {
  DomainId old = t_currentDomain;
  t_currentDomain = d;
  return old;
}

DomainId DomainTable::create(const std::string& name, DomainId parent, int pidHint) {
  auto d = std::make_shared<ProcessDomain>();
  std::lock_guard<std::mutex> lk(mu_);
  d->id = nextId_++;
  d->pid = pidHint ? pidHint : nextPid_++;
  d->ppid = parent == kInvalidDomain ? 0 : (int)parent;
  d->name = name;
  byId_[d->id] = d;
  byPid_[d->pid] = d->id;
  return d->id;
}
ProcessDomain* DomainTable::find(DomainId d) {
  std::lock_guard<std::mutex> lk(mu_);
  auto it = byId_.find(d);
  return it == byId_.end() ? nullptr : it->second.get();
}
ProcessDomain* DomainTable::findByPid(int pid) {
  std::lock_guard<std::mutex> lk(mu_);
  auto it = byPid_.find(pid);
  if (it == byPid_.end()) return nullptr;
  auto dit = byId_.find(it->second);
  return dit == byId_.end() ? nullptr : dit->second.get();
}

long DomainTable::currentPid() {
  ProcessDomain* d = find(current());
  return d ? d->pid : 1;
}
long DomainTable::parentPid() {
  ProcessDomain* d = find(current());
  return d ? d->ppid : 0;
}
long DomainTable::currentTid() {
  if (t_tid == 0) t_tid = g_nextTid.fetch_add(1);
  return t_tid;
}
const std::string& DomainTable::currentCwd() {
  static thread_local std::string cwd = "/";
  ProcessDomain* d = find(current());
  if (d) { std::lock_guard<std::mutex> lk(d->mu); cwd = d->cwd; }
  return cwd;
}
void DomainTable::setCwd(const char* path) {
  ProcessDomain* d = find(current());
  if (d && path) { std::lock_guard<std::mutex> lk(d->mu); d->cwd = path; }
}
const std::string& DomainTable::threadName() { return t_threadName; }
void DomainTable::setThreadName(const char* name) {
  if (name) t_threadName = name;
}

long DomainTable::clone(uint64_t flags, void* childStack, void* parentTid,
                        void* childTid, void* tls) {
  constexpr uint64_t kCLONE_VM = 0x100, kCLONE_SIGHAND = 0x800,
                     kCLONE_THREAD = 0x10000,
                     kCLONE_SETTLS = 0x80000;   (void)kCLONE_SETTLS;
  bool threadStyle = (flags & kCLONE_THREAD) && (flags & kCLONE_VM) &&
                     (flags & kCLONE_SIGHAND);
  if (!threadStyle) {
    // fork-style clone: only audited callers may create new domains
    const std::string& nm = threadName();
    bool audited = nm.find("init") != std::string::npos ||
                   nm.find("app_process") != std::string::npos ||
                   nm.find("zygote") != std::string::npos ||
                   nm.find("aotdroid") != std::string::npos;
    if (!audited) {
      AOTD_LOGE("domain", "fork-style clone denied for unaudited caller '%s'",
                nm.c_str());
      return -EPERM;
    }
  }
  auto* ca = new CloneArgs();
  ca->flags = flags; ca->childStack = childStack; ca->parentTid = parentTid;
  ca->childTid = childTid; ca->tls = tls;
  ca->newDomain = kInvalidDomain;
  ca->fn = hostCloneChild_; ca->fnArg = hostCloneChildArg;
  DomainId childDomain = current();
  if (!threadStyle) {
    childDomain = create("fork-child", current());
    ca->newDomain = childDomain;
  }
  ProcessDomain* pd = find(current());
  if (pd) pd->threadCount.fetch_add(1);
  pthread_t t;
  pthread_attr_t attr;
  pthread_attr_init(&attr);
  if (childStack) {
    // stack grows down: we cannot know the requested size; default 8 MiB
    pthread_attr_setstack(&attr, childStack, 8 << 20);
  }
  if (pthread_create(&t, &attr, cloneChildEntry, ca) != 0) {
    delete ca;
    return -EAGAIN;
  }
  pthread_attr_destroy(&attr);
  // publish the child tid to the parent (CLONE_PARENT_SETTID)
  int ctid = g_nextTid.load();   // approximate; corrected by CHILD_CLEARTID path
  if (parentTid && (flags & 0x100000 /*CLONE_PARENT_SETTID*/)) *(int*)parentTid = ctid;
  return ctid;   // parent sees the child tid; child path returns 0 on device
}

void DomainTable::execve(const void* execImageOpaque, char* const argv[],
                         char* const envp[]) {
  auto* img = (const ExecImageLite*)execImageOpaque;
  if (!img || !img->entry) {
    AOTD_LOGE("domain", "execve: no entry for image");
    threadExit(127);
  }
  AOTD_LOGI("domain", "execve: entering %s", img->path.c_str());
  // The container bootstrapper registers entries as `int (*)(int, char**, char**)`
  auto entry = (int (*)(int, char**, char**))img->entry;
  int argc = 0;
  while (argv && argv[argc]) argc++;
  int rc = entry(argc, (char**)argv, (char**)envp);
  threadExit(rc);
  __builtin_unreachable();
}

void DomainTable::threadExit(int code) {
  ProcessDomain* d = find(current());
  if (d && d->threadCount.fetch_sub(1) == 1) {
    d->exited = true;
    d->exitStatus = code;
    d->exitCv.notify_all();
    BinderEmu::get().domainExit(d->id);
  }
  pthread_exit(nullptr);
}

void DomainTable::domainExitCurrent(int code) {
  ProcessDomain* d = find(current());
  if (d) {
    d->exited = true;
    d->exitStatus = code;
    d->exitCv.notify_all();
    BinderEmu::get().domainExit(d->id);
    // synthetic SIGCHLD to the parent domain's chain
    ProcessDomain* p = findByPid(d->ppid);
    if (p) {
      SigChainEntry* e = sigChainGet(SIGCHLD);
      if (e && e->handler) e->handler(SIGCHLD, nullptr, nullptr);
    }
  }
  pthread_exit(nullptr);
}

long DomainTable::wait4(int pid, int* status, int options) {
  constexpr int kWNOHANG = 1;
  auto tryWait = [&](int* got) -> bool {
    std::vector<std::shared_ptr<ProcessDomain>> candidates;
    {
      std::lock_guard<std::mutex> lk(mu_);
      if (pid > 0) {
        auto it = byPid_.find(pid);
        if (it != byPid_.end()) {
          auto dit = byId_.find(it->second);
          if (dit != byId_.end()) candidates.push_back(dit->second);
        }
      } else if (pid == -1) {
        int myPid = (int)currentPid();
        for (auto& kv : byId_)
          if (kv.second->ppid == myPid) candidates.push_back(kv.second);
      }
    }
    for (auto& d : candidates) {
      if (d->exited) {
        if (got) *got = d->pid;
        if (status) *status = d->exitStatus << 8;
        return true;
      }
    }
    return false;
  };
  int got = 0;
  if (tryWait(&got)) return got;
  if (options & kWNOHANG) return 0;
  ProcessDomain* self = find(current());
  if (!self) return -ECHILD;
  std::unique_lock<std::mutex> lk(self->mu);
  bool done = self->exitCv.wait_for(lk, std::chrono::seconds(5), [&] { return tryWait(&got); });
  return done ? got : -EINTR;
}

long DomainTable::tgkill(int tgid, int tid, int sig) {
  SigChainEntry* e = sigChainGet(sig);
  if (e && e->handler) {
    e->handler(sig, nullptr, nullptr);
    return 0;
  }
  return 0;
}

void DomainTable::cowAddRange(DomainId d, uint64_t start, uint64_t len) {
  ProcessDomain* pd = find(d);
  if (!pd) return;
  std::lock_guard<std::mutex> lk(pd->mu);
  pd->cowRanges.push_back({start, len});
#ifdef AOTDROID_APPLE
  mprotect((void*)(uintptr_t)start, (size_t)len, PROT_READ);
#endif
}

bool DomainTable::cowHandleWriteFault(void* faultAddr) {
  uint64_t a = (uint64_t)(uintptr_t)faultAddr;
  ProcessDomain* pd = find(current());
  if (!pd) return false;
  std::lock_guard<std::mutex> lk(pd->mu);
  for (auto& r : pd->cowRanges) {
    if (a >= r.start && a < r.start + r.len) {
      // page-granular copy-on-write inside the guard range
      uint64_t page = a & ~16383ull;
      size_t len = (size_t)std::min<uint64_t>(r.start + r.len - page, 16384);
      void* copy = mmap(nullptr, len, PROT_READ | PROT_WRITE,
                        MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
      if (copy == MAP_FAILED) return false;
      memcpy(copy, (void*)(uintptr_t)page, len);
      mprotect((void*)(uintptr_t)page, len, PROT_READ | PROT_WRITE);
      memcpy((void*)(uintptr_t)page, copy, len);
      munmap(copy, len);
      return true;
    }
  }
  return false;
}

void DomainTable::setHostCloneChildFn(void (*fn)(void*)) { hostCloneChild_ = fn; }

}  // namespace aotdroid
