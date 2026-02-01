#include <atomic>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <iomanip>
#include <iostream>
#include <mutex>
#include <random>
#include <sstream>
#include <string>
#include <string_view>
#include <vector>

#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include <dbghelp.h>
#if defined(_MSC_VER)
#pragma comment(lib, "dbghelp.lib")
#endif
#else
#include <execinfo.h>
#endif

#include "leveldb/db.h"
#include "leveldb/env.h"
#include "leveldb/options.h"
#include "leveldb/slice.h"
#include "leveldb/status.h"

namespace {

struct TraceConfig {
  bool trace_open = true;
  bool trace_reads = true;
  bool trace_writes = true;
  bool trace_sync = false;
  bool symbolize = true;
  int max_traces = 200;
  int stack_depth = 64;
};

class StackTrace {
 public:
  static std::string Capture(const TraceConfig& cfg, int skip_frames) {
    if (cfg.stack_depth <= 0) return {};

#if defined(_WIN32)
    std::vector<void*> frames(static_cast<size_t>(cfg.stack_depth));
    USHORT captured = CaptureStackBackTrace(
        0, static_cast<DWORD>(frames.size()), frames.data(), nullptr);

    std::ostringstream out;
    for (USHORT i = static_cast<USHORT>(skip_frames); i < captured; ++i) {
      DWORD64 address = reinterpret_cast<DWORD64>(frames[i]);
      out << "  #" << std::setw(2) << std::setfill('0') << (i - skip_frames)
          << " ";
      out << "0x" << std::hex << address << std::dec;

      if (!cfg.symbolize) {
        out << "\n";
        continue;
      }

      std::call_once(sym_init_once_, []() {
        HANDLE process = GetCurrentProcess();
        SymSetOptions(SYMOPT_UNDNAME | SYMOPT_DEFERRED_LOADS |
                      SYMOPT_LOAD_LINES);
        SymInitialize(process, nullptr, TRUE);
      });

      HANDLE process = GetCurrentProcess();

      char symbol_buffer[sizeof(SYMBOL_INFO) + MAX_SYM_NAME];
      std::memset(symbol_buffer, 0, sizeof(symbol_buffer));
      auto* symbol = reinterpret_cast<SYMBOL_INFO*>(symbol_buffer);
      symbol->SizeOfStruct = sizeof(SYMBOL_INFO);
      symbol->MaxNameLen = MAX_SYM_NAME;

      DWORD64 displacement = 0;
      if (SymFromAddr(process, address, &displacement, symbol) != FALSE) {
        out << " " << symbol->Name;
      }

      IMAGEHLP_LINE64 line;
      std::memset(&line, 0, sizeof(line));
      line.SizeOfStruct = sizeof(line);
      DWORD line_displacement = 0;
      if (SymGetLineFromAddr64(process, address, &line_displacement, &line) !=
              FALSE &&
          line.FileName != nullptr) {
        out << " (" << line.FileName << ":" << line.LineNumber << ")";
      }
      out << "\n";
    }
    return out.str();
#else
    std::vector<void*> frames(static_cast<size_t>(cfg.stack_depth));
    int captured = backtrace(frames.data(), static_cast<int>(frames.size()));
    if (captured <= 0) return {};

    std::ostringstream out;
    if (!cfg.symbolize) {
      for (int i = skip_frames; i < captured; ++i) {
        out << "  #" << std::setw(2) << std::setfill('0') << (i - skip_frames)
            << " " << frames[i] << "\n";
      }
      return out.str();
    }

    char** symbols = backtrace_symbols(frames.data(), captured);
    if (symbols == nullptr) return {};
    for (int i = skip_frames; i < captured; ++i) {
      out << "  #" << std::setw(2) << std::setfill('0') << (i - skip_frames)
          << " " << symbols[i] << "\n";
    }
    std::free(symbols);
    return out.str();
#endif
  }

 private:
#if defined(_WIN32)
  static std::once_flag sym_init_once_;
#endif
};

#if defined(_WIN32)
std::once_flag StackTrace::sym_init_once_;
#endif

class TracingEnv;

class TracingSequentialFile : public leveldb::SequentialFile {
 public:
  TracingSequentialFile(std::string filename, leveldb::SequentialFile* target,
                        TracingEnv* env)
      : filename_(std::move(filename)), target_(target), env_(env) {}
  ~TracingSequentialFile() override { delete target_; }

  leveldb::Status Read(size_t n, leveldb::Slice* result,
                       char* scratch) override;
  leveldb::Status Skip(uint64_t n) override;

 private:
  const std::string filename_;
  leveldb::SequentialFile* const target_;
  TracingEnv* const env_;
};

class TracingRandomAccessFile : public leveldb::RandomAccessFile {
 public:
  TracingRandomAccessFile(std::string filename,
                          leveldb::RandomAccessFile* target, TracingEnv* env)
      : filename_(std::move(filename)), target_(target), env_(env) {}
  ~TracingRandomAccessFile() override { delete target_; }

  leveldb::Status Read(uint64_t offset, size_t n, leveldb::Slice* result,
                       char* scratch) const override;

 private:
  const std::string filename_;
  leveldb::RandomAccessFile* const target_;
  TracingEnv* const env_;
};

class TracingWritableFile : public leveldb::WritableFile {
 public:
  TracingWritableFile(std::string filename, leveldb::WritableFile* target,
                      TracingEnv* env)
      : filename_(std::move(filename)), target_(target), env_(env) {}
  ~TracingWritableFile() override { delete target_; }

  leveldb::Status Append(const leveldb::Slice& data) override;
  leveldb::Status Close() override;
  leveldb::Status Flush() override;
  leveldb::Status Sync() override;

 private:
  const std::string filename_;
  leveldb::WritableFile* const target_;
  TracingEnv* const env_;
};

class TracingEnv : public leveldb::EnvWrapper {
 public:
  TracingEnv(leveldb::Env* target, TraceConfig cfg)
      : leveldb::EnvWrapper(target),
        cfg_(cfg),
        remaining_traces_(cfg.max_traces) {}

  leveldb::Status NewSequentialFile(const std::string& fname,
                                   leveldb::SequentialFile** result) override {
    leveldb::SequentialFile* base = nullptr;
    leveldb::Status s = target()->NewSequentialFile(fname, &base);
    if (!s.ok()) return s;
    if (cfg_.trace_open) Trace("open_seq", fname, 0, s, /*skip=*/2);
    *result = new TracingSequentialFile(fname, base, this);
    return s;
  }

  leveldb::Status NewRandomAccessFile(
      const std::string& fname,
      leveldb::RandomAccessFile** result) override {
    leveldb::RandomAccessFile* base = nullptr;
    leveldb::Status s = target()->NewRandomAccessFile(fname, &base);
    if (!s.ok()) return s;
    if (cfg_.trace_open) Trace("open_rand", fname, 0, s, /*skip=*/2);
    *result = new TracingRandomAccessFile(fname, base, this);
    return s;
  }

  leveldb::Status NewWritableFile(const std::string& fname,
                                 leveldb::WritableFile** result) override {
    leveldb::WritableFile* base = nullptr;
    leveldb::Status s = target()->NewWritableFile(fname, &base);
    if (!s.ok()) return s;
    if (cfg_.trace_open) Trace("open_w", fname, 0, s, /*skip=*/2);
    *result = new TracingWritableFile(fname, base, this);
    return s;
  }

  leveldb::Status NewAppendableFile(const std::string& fname,
                                   leveldb::WritableFile** result) override {
    leveldb::WritableFile* base = nullptr;
    leveldb::Status s = target()->NewAppendableFile(fname, &base);
    if (!s.ok()) return s;
    if (cfg_.trace_open) Trace("open_a", fname, 0, s, /*skip=*/2);
    *result = new TracingWritableFile(fname, base, this);
    return s;
  }

  bool ShouldTrace() { return remaining_traces_.fetch_sub(1) > 0; }

  void Trace(std::string_view op, const std::string& filename, uint64_t n,
             const leveldb::Status& status, int skip_frames) {
    if (!ShouldTrace()) return;

    std::ostringstream out;
    out << "[leveldb-stack] " << op << " file=" << filename;
    if (n != 0) out << " n=" << n;
    out << " status=" << status.ToString() << "\n";
    if (cfg_.stack_depth > 0) {
      out << StackTrace::Capture(cfg_, skip_frames);
    }

    std::lock_guard<std::mutex> lock(print_mu_);
    std::cerr << out.str();
  }

  const TraceConfig& cfg() const { return cfg_; }

 private:
  const TraceConfig cfg_;
  std::atomic<int> remaining_traces_;
  std::mutex print_mu_;
};

leveldb::Status TracingSequentialFile::Read(size_t n, leveldb::Slice* result,
                                            char* scratch) {
  leveldb::Status s = target_->Read(n, result, scratch);
  if (env_->cfg().trace_reads) {
    env_->Trace("seq_read", filename_, n, s, /*skip=*/3);
  }
  return s;
}

leveldb::Status TracingSequentialFile::Skip(uint64_t n) {
  leveldb::Status s = target_->Skip(n);
  if (env_->cfg().trace_reads) {
    env_->Trace("seq_skip", filename_, n, s, /*skip=*/3);
  }
  return s;
}

leveldb::Status TracingRandomAccessFile::Read(uint64_t offset, size_t n,
                                              leveldb::Slice* result,
                                              char* scratch) const {
  leveldb::Status s = target_->Read(offset, n, result, scratch);
  if (env_->cfg().trace_reads) {
    std::ostringstream op;
    op << "rand_read(offset=" << offset << ")";
    env_->Trace(op.str(), filename_, n, s, /*skip=*/3);
  }
  return s;
}

leveldb::Status TracingWritableFile::Append(const leveldb::Slice& data) {
  leveldb::Status s = target_->Append(data);
  if (env_->cfg().trace_writes) {
    env_->Trace("append", filename_, data.size(), s, /*skip=*/3);
  }
  return s;
}

leveldb::Status TracingWritableFile::Close() {
  leveldb::Status s = target_->Close();
  if (env_->cfg().trace_open) {
    env_->Trace("close", filename_, 0, s, /*skip=*/3);
  }
  return s;
}

leveldb::Status TracingWritableFile::Flush() {
  leveldb::Status s = target_->Flush();
  if (env_->cfg().trace_sync) {
    env_->Trace("flush", filename_, 0, s, /*skip=*/3);
  }
  return s;
}

leveldb::Status TracingWritableFile::Sync() {
  leveldb::Status s = target_->Sync();
  if (env_->cfg().trace_sync) {
    env_->Trace("sync", filename_, 0, s, /*skip=*/3);
  }
  return s;
}

std::string RandomValue(size_t size, std::mt19937_64* rng) {
  static constexpr char kAlphabet[] =
      "0123456789abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ";
  std::uniform_int_distribution<size_t> dist(0, sizeof(kAlphabet) - 2);

  std::string s;
  s.resize(size);
  for (size_t i = 0; i < size; ++i) s[i] = kAlphabet[dist(*rng)];
  return s;
}

std::string KeyFor(int i) {
  std::ostringstream out;
  out << "key" << std::setw(8) << std::setfill('0') << i;
  return out.str();
}

int ParseIntFlag(int argc, char** argv, const char* name, int default_value) {
  std::string prefix = std::string("--") + name + "=";
  for (int i = 1; i < argc; ++i) {
    if (std::strncmp(argv[i], prefix.c_str(), prefix.size()) == 0) {
      return std::atoi(argv[i] + prefix.size());
    }
  }
  return default_value;
}

std::string ParseStringFlag(int argc, char** argv, const char* name,
                            std::string default_value) {
  std::string prefix = std::string("--") + name + "=";
  for (int i = 1; i < argc; ++i) {
    if (std::strncmp(argv[i], prefix.c_str(), prefix.size()) == 0) {
      return std::string(argv[i] + prefix.size());
    }
  }
  return default_value;
}

bool ParseBoolFlag(int argc, char** argv, const char* name,
                   bool default_value) {
  std::string prefix = std::string("--") + name + "=";
  for (int i = 1; i < argc; ++i) {
    if (std::strncmp(argv[i], prefix.c_str(), prefix.size()) == 0) {
      const char* v = argv[i] + prefix.size();
      return (std::strcmp(v, "1") == 0) || (std::strcmp(v, "true") == 0) ||
             (std::strcmp(v, "yes") == 0) || (std::strcmp(v, "on") == 0);
    }
  }
  return default_value;
}

void Usage(const char* argv0) {
  std::cerr
      << "Usage:\n"
      << "  " << argv0
      << " --db=PATH [--num_writes=N] [--num_reads=N] [--value_size=BYTES]\n"
      << "       [--sync_writes=0|1] [--max_traces=N] [--stack_depth=N]\n"
      << "       [--trace_open=0|1] [--trace_reads=0|1] [--trace_writes=0|1]\n"
      << "       [--trace_sync=0|1]\n\n"
      << "Tips:\n"
      << "  - Build with debug symbols (e.g. RelWithDebInfo) to see function\n"
      << "    names and file:line in the stack.\n"
      << "  - If output is too noisy, lower --max_traces or disable some trace\n"
      << "    categories.\n";
}

}  // namespace

int main(int argc, char** argv) {
  if (ParseBoolFlag(argc, argv, "help", false)) {
    Usage(argv[0]);
    return 0;
  }

  const std::string db_path =
      ParseStringFlag(argc, argv, "db", "stacktrace_db");
  const int num_writes = ParseIntFlag(argc, argv, "num_writes", 10);
  const int num_reads = ParseIntFlag(argc, argv, "num_reads", 10);
  const int value_size = ParseIntFlag(argc, argv, "value_size", 100);
  const bool sync_writes = ParseBoolFlag(argc, argv, "sync_writes", false);

  TraceConfig cfg;
  cfg.max_traces = ParseIntFlag(argc, argv, "max_traces", cfg.max_traces);
  cfg.stack_depth = ParseIntFlag(argc, argv, "stack_depth", cfg.stack_depth);
  cfg.trace_open = ParseBoolFlag(argc, argv, "trace_open", cfg.trace_open);
  cfg.trace_reads = ParseBoolFlag(argc, argv, "trace_reads", cfg.trace_reads);
  cfg.trace_writes =
      ParseBoolFlag(argc, argv, "trace_writes", cfg.trace_writes);
  cfg.trace_sync = ParseBoolFlag(argc, argv, "trace_sync", cfg.trace_sync);
  cfg.symbolize = ParseBoolFlag(argc, argv, "symbolize", cfg.symbolize);

  TracingEnv tracing_env(leveldb::Env::Default(), cfg);

  leveldb::Options options;
  options.create_if_missing = true;
  options.env = &tracing_env;

  leveldb::DB* db = nullptr;
  leveldb::Status s = leveldb::DB::Open(options, db_path, &db);
  if (!s.ok()) {
    std::cerr << "DB::Open failed: " << s.ToString() << "\n";
    return 1;
  }

  std::mt19937_64 rng(0xC0DEF00Du);

  leveldb::WriteOptions wopt;
  wopt.sync = sync_writes;

  std::cerr << "[leveldb-stack] DB opened at " << db_path << "\n";

  for (int i = 0; i < num_writes; ++i) {
    const std::string key = KeyFor(i);
    const std::string value = RandomValue(static_cast<size_t>(value_size), &rng);
    s = db->Put(wopt, key, value);
    if (!s.ok()) {
      std::cerr << "Put failed: " << s.ToString() << "\n";
      delete db;
      return 1;
    }
  }

  leveldb::ReadOptions ropt;
  for (int i = 0; i < num_reads; ++i) {
    const int k = (num_writes == 0) ? 0 : (i % num_writes);
    const std::string key = KeyFor(k);
    std::string value;
    s = db->Get(ropt, key, &value);
    if (!s.ok()) {
      std::cerr << "Get failed: " << s.ToString() << "\n";
      delete db;
      return 1;
    }
  }

  delete db;
  std::cerr << "[leveldb-stack] done\n";
  return 0;
}
