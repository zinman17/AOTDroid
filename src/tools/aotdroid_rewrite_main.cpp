// ============================================================================
//  AOTDroid-Adaptive-iOS  |  src/tools/aotdroid_rewrite_main.cpp
//  Offline AOT pipeline driver (module 2 CLI). Runs on Linux CI and macOS.
//
//  Usage: aotdroid-rewrite --in FILE --out-elf F --out-macho F
//                       [--soname S] [--page-size N] [--tls-layout TSV]
// ============================================================================
#include "aot/aot_rewriter.h"
#include <getopt.h>

int main(int argc, char** argv) {
  const char* in = nullptr;
  const char* outElf = nullptr;
  const char* outMacho = nullptr;
  const char* soname = nullptr;
  const char* tlsLayoutPath = nullptr;
  uint32_t pageSize = 16384;

  static option opts[] = {
    {"in", required_argument, nullptr, 'i'},
    {"out-elf", required_argument, nullptr, 'e'},
    {"out-macho", required_argument, nullptr, 'm'},
    {"soname", required_argument, nullptr, 's'},
    {"page-size", required_argument, nullptr, 'p'},
    {"tls-layout", required_argument, nullptr, 't'},
    {nullptr, 0, nullptr, 0},
  };
  int c;
  while ((c = getopt_long(argc, argv, "i:e:m:s:p:t:", opts, nullptr)) != -1) {
    switch (c) {
      case 'i': in = optarg; break;
      case 'e': outElf = optarg; break;
      case 'm': outMacho = optarg; break;
      case 's': soname = optarg; break;
      case 'p': pageSize = (uint32_t)strtoul(optarg, nullptr, 0); break;
      case 't': tlsLayoutPath = optarg; break;
      default: return 2;
    }
  }
  if (!in || (!outElf && !outMacho)) {
    fprintf(stderr,
      "usage: %s --in FILE [--out-elf F] [--out-macho F] [--soname S]\n"
      "                 [--page-size N] [--tls-layout TSV]\n",
      argv[0]);
    return 2;
  }

  aotdroid::RewriteConfig cfg;
  cfg.targetPageSize = pageSize;
  cfg.emitElf = outElf != nullptr;
  cfg.emitMachO = outMacho != nullptr;
  if (soname) cfg.sonameOverride = soname;
  if (tlsLayoutPath) {
    FILE* f = fopen(tlsLayoutPath, "r");
    if (!f) { perror("tls-layout"); return 1; }
    char line[512];
    while (fgets(line, sizeof(line), f)) {
      char name[256]; long long off;
      if (sscanf(line, "%255s %lld", name, &off) == 2)
        cfg.tlsLayout[name] = off;
    }
    fclose(f);
  }

  aotdroid::RewriteStats st;
  std::string err;
  if (!aotdroid::AotRewriter::rewriteFile(in, outElf, outMacho, cfg, &st, &err)) {
    fprintf(stderr, "aotdroid-rewrite: FAILED: %s\n", err.c_str());
    return 1;
  }
  printf("{\"input\":\"%s\",\"bytes\":%zu,\"oatContainer\":%s,"
         "\"oatVersion\":\"%s\",\"apiLevel\":%u,"
         "\"syscalls\":%zu,\"tlsReads\":%zu,\"tlsWrites\":%zu,"
         "\"veneers\":%zu,\"brkTrampolines\":%zu,"
         "\"relocsTranslated\":%zu,\"runtimeFixups\":%zu,\"slots\":%zu,"
         "\"machoBytes\":%zu}\n",
         in, st.inputBytes, st.oatContainer ? "true" : "false",
         st.oatVersion, st.oatApiLevel,
         st.syscallSites, st.tlsReadSites, st.tlsWriteSites,
         st.veneersEmitted, st.brkTrampolines,
         st.relocsTranslated, st.runtimeFixups, st.slotsEmitted,
         st.outputMachoBytes);
  return 0;
}
