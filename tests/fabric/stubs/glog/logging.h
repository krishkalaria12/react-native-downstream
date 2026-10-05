// A parse-only stand-in for glog's logging header.
//
// React Native's `react_native_assert` calls `google::FlushLogFiles` and reads
// `google::GLOG_INFO`; the assert macros themselves are expanded at the use site,
// so the whole surface a syntax-only check needs is the severity enum and one
// function. Nothing here is ever compiled into a shipped binary.

#pragma once

#include <iostream>

namespace google {

enum LogSeverity { GLOG_INFO = 0, GLOG_WARNING, GLOG_ERROR, GLOG_FATAL };

inline void InitGoogleLogging(const char * /*argv0*/) {}
inline void FlushLogFiles(LogSeverity /*severity*/) {}
inline void InstallFailureSignalHandler() {}

} // namespace google

#define LOG(severity) std::cerr
#define LOG_IF(severity, condition) std::cerr
#define VLOG(level) std::cerr
#define CHECK(condition) std::cerr
#define CHECK_EQ(a, b) std::cerr
#define DCHECK(condition) std::cerr
#define DCHECK_EQ(a, b) std::cerr