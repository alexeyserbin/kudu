// Copyright (c) 2014, Cloudera, inc.
// Confidential Cloudera Information: Covered by NDA.
//
// Portions (c) 2011 The Chromium Authors.

#include "kudu/util/mutex.h"

#include <glog/logging.h>

#include "kudu/util/env.h"

namespace kudu {

Mutex::Mutex()
#ifndef NDEBUG
  : owning_tid_(0)
#endif
{
#ifndef NDEBUG
  // In debug, setup attributes for lock error checking.
  pthread_mutexattr_t mta;
  int rv = pthread_mutexattr_init(&mta);
  DCHECK_EQ(rv, 0) << ". " << strerror(rv);
  rv = pthread_mutexattr_settype(&mta, PTHREAD_MUTEX_ERRORCHECK);
  DCHECK_EQ(rv, 0) << ". " << strerror(rv);
  rv = pthread_mutex_init(&native_handle_, &mta);
  DCHECK_EQ(rv, 0) << ". " << strerror(rv);
  rv = pthread_mutexattr_destroy(&mta);
  DCHECK_EQ(rv, 0) << ". " << strerror(rv);
#else
  // In release, go with the default lock attributes.
  pthread_mutex_init(&native_handle_, NULL);
#endif
}

Mutex::~Mutex() {
  int rv = pthread_mutex_destroy(&native_handle_);
  DCHECK_EQ(rv, 0) << ". " << strerror(rv);
}

bool Mutex::TryAcquire() {
  int rv = pthread_mutex_trylock(&native_handle_);
  DCHECK(rv == 0 || rv == EBUSY) << ". " << strerror(rv);
#ifndef NDEBUG
  if (rv == 0) {
    CheckUnheldAndMark();
  }
#endif
  return rv == 0;
}

void Mutex::Acquire() {
  int rv = pthread_mutex_lock(&native_handle_);
  DCHECK_EQ(rv, 0) << ". " << strerror(rv);
#ifndef NDEBUG
  CheckUnheldAndMark();
#endif
}

void Mutex::Release() {
#ifndef NDEBUG
  CheckHeldAndUnmark();
#endif
  int rv = pthread_mutex_unlock(&native_handle_);
  DCHECK_EQ(rv, 0) << ". " << strerror(rv);
}

#ifndef NDEBUG
void Mutex::AssertAcquired() const {
  DCHECK_EQ(owning_tid_, Env::Default()->gettid());
}

void Mutex::CheckHeldAndUnmark() {
  AssertAcquired();
  owning_tid_ = 0;
}

void Mutex::CheckUnheldAndMark() {
  DCHECK_EQ(owning_tid_, 0);
  owning_tid_ = Env::Default()->gettid();
}

#endif

} // namespace kudu
