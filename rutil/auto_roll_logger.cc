//  Copyright (c) 2011-present, Facebook, Inc.  All rights reserved.
//  This source code is licensed under the BSD-style license found in the
//  LICENSE file in the root directory of this source tree. An additional grant
//  of patent rights can be found in the PATENTS file in the same directory.
//
#include "rocksutil/auto_roll_logger.h"

namespace rocksutil {

std::string InfoLogFileName(const std::string& log_path) {
  return log_path + "/LOG";
}

static std::string OldInfoLogFileName(const std::string& log_path,
    uint64_t ts) {
  char buf[50];
  snprintf(buf, sizeof(buf), "%llu", static_cast<unsigned long long>(ts));

  return log_path + "/LOG.old." + buf;
}

// -- AutoRollLogger
Status AutoRollLogger::ResetLogger() {
  status_ = env_->NewLogger(log_fname_, &logger_);

  if (!status_.ok()) {
    return status_;
  }

  if (logger_->GetLogFileSize() == Logger::kDoNotSupportGetLogFileSize) {
    status_ = Status::NotSupported(
        "The underlying logger doesn't support GetLogFileSize()");
  }
  if (status_.ok()) {
    cached_now = static_cast<uint64_t>(env_->NowMicros() * 1e-6);
    ctime_ = cached_now;
    cached_now_access_count = 0;
  }

  return status_;
}

void AutoRollLogger::RollLogFile() {
  // This function is called when log is rotating. Two rotations
  // can happen quickly (NowMicro returns same value). To not overwrite
  // previous log file we increment by one micro second and try again.
  uint64_t now = env_->NowMicros();
  std::string old_fname;
  do {
    old_fname = OldInfoLogFileName(
      log_path_, now);
    now++;
  } while (env_->FileExists(old_fname).ok());
  env_->RenameFile(log_fname_, old_fname);
}

std::string AutoRollLogger::ValistToString(const char* format,
                                           va_list args) const {
  // Any log messages longer than 1024 will get truncated.
  // The user is responsible for chopping longer messages into multi line log
  static const int MAXBUFFERSIZE = 1024;
  char buffer[MAXBUFFERSIZE];

  int count = vsnprintf(buffer, MAXBUFFERSIZE, format, args);
  (void) count;
  assert(count >= 0);

  return buffer;
}

void AutoRollLogger::LogInternal(const char* format, ...) {
  mutex_.AssertHeld();
  va_list args;
  va_start(args, format);
  logger_->Logv(format, args);
  va_end(args);
}

void AutoRollLogger::Logv(const char* format, va_list ap) {
  assert(GetStatus().ok());

  std::shared_ptr<Logger> logger;
  {
    MutexLock l(&mutex_);
    if ((kLogFileTimeToRoll > 0 && LogExpired()) ||
        (kMaxLogFileSize > 0 && logger_->GetLogFileSize() >= kMaxLogFileSize)) {
      RollLogFile();
      Status s = ResetLogger();
      if (!s.ok()) {
        // can't really log the error if creating a new LOG file failed
        return;
      }

      WriteHeaderInfo();
    }

    // pin down the current logger_ instance before releasing the mutex.
    logger = logger_;
  }

  // Another thread could have put a new Logger instance into logger_ by now.
  // However, since logger is still hanging on to the previous instance
  // (reference count is not zero), we don't have to worry about it being
  // deleted while we are accessing it.
  // Note that logv itself is not mutex protected to allow maximum concurrency,
  // as thread safety should have been handled by the underlying logger.
  logger->Logv(format, ap);
}

void AutoRollLogger::WriteHeaderInfo() {
  mutex_.AssertHeld();
  for (auto& header : headers_) {
    LogInternal("%s", header.c_str());
  }
}

void AutoRollLogger::LogHeader(const char* format, va_list args) {
  // header message are to be retained in memory. Since we cannot make any
  // assumptions about the data contained in va_list, we will retain them as
  // strings
  va_list tmp;
  va_copy(tmp, args);
  std::string data = ValistToString(format, tmp);
  va_end(tmp);

  MutexLock l(&mutex_);
  headers_.push_back(data);

  // Log the original message to the current log
  logger_->Logv(format, args);
}

bool AutoRollLogger::LogExpired() {
  if (cached_now_access_count >= call_NowMicros_every_N_records_) {
    cached_now = static_cast<uint64_t>(env_->NowMicros() * 1e-6);
    cached_now_access_count = 0;
  }

  ++cached_now_access_count;
  return cached_now >= ctime_ + kLogFileTimeToRoll;
}

//Status CreateLoggerFromOptions(Env* env, const std::string& log_path,
Status CreateLogger(const std::string& log_path,
         std::shared_ptr<Logger>* logger, Env* env,
         size_t log_max_size, size_t log_file_time_to_roll,
         const InfoLogLevel log_level) {

  std::string fname =
      InfoLogFileName(log_path);

  env->CreateDirIfMissing(log_path);  // In case it does not exist
  // Currently we only support roll by time-to-roll and log size
  if (log_file_time_to_roll > 0 || log_max_size > 0) {
    AutoRollLogger* result = new AutoRollLogger(
        env, log_path, log_max_size, log_file_time_to_roll,
        log_level);
    Status s = result->GetStatus();
    if (!s.ok()) {
      delete result;
    } else {
      logger->reset(result);
    }
    return s;
  } else {
    // Open a log file in the same directory as the db
    env->RenameFile(
        fname, OldInfoLogFileName(log_path, env->NowMicros()));
    auto s = env->NewLogger(fname, logger);
    if (logger->get() != nullptr) {
      (*logger)->SetInfoLogLevel(log_level);
    }
    return s;
  }
}

}  // namespace rocksutil
