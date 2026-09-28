//
// Created by zzm on 2026/9/22
// Part of RVision
//
#pragma once

#include "Support/Option.h"
#include "llvm/Support/WithColor.h"
#include "llvm/Support/raw_ostream.h"

namespace kelyra {
enum class LogLevel { Error, Warn, Info, Debug };

inline llvm::cl::opt<LogLevel> LogLevelOpt{
    "log-level", llvm::cl::desc("Log level:"),
    llvm::cl::values(
        clEnumValN(LogLevel::Error, "error", "Errors only"),
        clEnumValN(LogLevel::Warn, "warn", "Warnings and errors"),
        clEnumValN(LogLevel::Info, "info", "Info, warnings, errors"),
        clEnumValN(LogLevel::Debug, "debug", "Everything")),
    llvm::cl::init(LogLevel::Warn), llvm::cl::cat(Option::KelyraCategory)};

// 每个日志级别对应的前缀和颜色
template <LogLevel Level> struct LogTraits;

template <> struct LogTraits<LogLevel::Error> {
  static constexpr llvm::StringRef prefix() { return "[ERROR]"; }
  static llvm::raw_ostream::Colors color() { return llvm::raw_ostream::RED; }
};

template <> struct LogTraits<LogLevel::Warn> {
  static constexpr llvm::StringRef prefix() { return "[WARN ]"; }
  static llvm::raw_ostream::Colors color() { return llvm::raw_ostream::YELLOW; }
};

template <> struct LogTraits<LogLevel::Info> {
  static constexpr llvm::StringRef prefix() { return "[INFO ]"; }
  static llvm::raw_ostream::Colors color() { return llvm::raw_ostream::GREEN; }
};

template <> struct LogTraits<LogLevel::Debug> {
  static constexpr llvm::StringRef prefix() { return "[DEBUG]"; }
  static llvm::raw_ostream::Colors color() {
    return llvm::raw_ostream::MAGENTA;
  }
};

template <LogLevel Level> class KLog {
  llvm::raw_ostream &OS;

public:
  explicit KLog(llvm::raw_ostream &OS) : OS(OS) {}
  KLog(const KLog &) = delete;
  KLog &operator=(const KLog &) = delete;

  template <typename T> KLog &operator<<(const T &V) {
    if (!isEnabled())
      return *this;
    // 第一次输出时打印前缀
    if (!Started) {
      Started = true;
      llvm::WithColor(OS, LogTraits<Level>::color())
          << LogTraits<Level>::prefix() << " ";
    }
    OS << V;
    return *this;
  }

  // 暂不支持标准库 std::endl 的 manipulator 回调函数
  KLog &operator<<(llvm::raw_ostream &(*F)(llvm::raw_ostream &)) = delete;

private:
  bool Started = false;
  static bool isEnabled() {
    return static_cast<int>(Level) <= static_cast<int>(LogLevelOpt.getValue());
  }
};

// 函数式：每次返回临时对象
inline KLog<LogLevel::Error> kerr() {
  return KLog<LogLevel::Error>(llvm::errs());
}
inline KLog<LogLevel::Warn> kwarn() {
  return KLog<LogLevel::Warn>(llvm::errs());
}
inline KLog<LogLevel::Info> kinfo() {
  return KLog<LogLevel::Info>(llvm::errs());
}
inline KLog<LogLevel::Debug> kdbg() {
  return KLog<LogLevel::Debug>(llvm::errs());
}
} // namespace kelyra
