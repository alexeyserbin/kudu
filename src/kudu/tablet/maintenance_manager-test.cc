// Copyright (c) 2013, Cloudera, inc.
// Confidential Cloudera Information: Covered by NDA.
#include <gtest/gtest.h>
#include <gflags/gflags.h>
#include <tr1/memory>
#include <vector>

#include "kudu/gutil/strings/substitute.h"
#include "kudu/tablet/maintenance_manager.h"
#include "kudu/tablet/tablet.pb.h"
#include "kudu/util/metrics.h"
#include "kudu/util/test_macros.h"
#include "kudu/util/thread.h"

using std::tr1::shared_ptr;
using std::vector;
using strings::Substitute;
using kudu::tablet::MaintenanceManagerStatusPB;

METRIC_DEFINE_gauge_uint32(running_gauge, kudu::MetricUnit::kMaintenanceOperations, "");

METRIC_DEFINE_histogram(duration_histogram, kudu::MetricUnit::kSeconds, "", 60000000LU, 2);

namespace kudu {

// Just create the MaintenanceManager and then shut it down, to make sure
// there are no race conditions there.
TEST(MaintenanceManagerTest, TestCreateAndShutdown) {
  MaintenanceManager::Options options;
  options.num_threads = 2;
  options.polling_interval_ms = 1;
  options.memory_limit = 1000;
  options.max_ts_anchored_secs = 1000;
  shared_ptr<MaintenanceManager> manager(new MaintenanceManager(options));
  ASSERT_STATUS_OK(manager->Init());
  manager->Shutdown();
}

enum TestMaintenanceOpState {
  OP_DISABLED,
  OP_RUNNABLE,
  OP_RUNNING,
  OP_FINISHED,
};

class TestMaintenanceOp : public MaintenanceOp {
 public:
  TestMaintenanceOp(const std::string& name,
                    TestMaintenanceOpState state)
    : MaintenanceOp(name),
      state_(state),
      ram_anchored_(500),
      ts_anchored_ms_(0),
      perf_improvement_(0),
      metric_ctx_(&metric_registry_, "test"),
      duration_histogram_(METRIC_duration_histogram.Instantiate(metric_ctx_)),
      running_gauge_(AtomicGauge<uint32_t>::Instantiate(METRIC_running_gauge, metric_ctx_)) { }

  virtual ~TestMaintenanceOp() {
  }

  virtual bool Prepare() OVERRIDE {
    boost::lock_guard<boost::mutex> guard(lock_);
    if (state_ != OP_RUNNABLE) {
      return false;
    }
    state_ = OP_RUNNING;
    state_change_cond_.notify_all();
    DLOG(INFO) << "Prepared op " << name();
    return true;
  }

  virtual void Perform() OVERRIDE {
    DLOG(INFO) << "Performing op " << name();
    boost::unique_lock<boost::mutex> guard(lock_);
    CHECK_EQ(OP_RUNNING, state_);
    state_ = OP_FINISHED;
    state_change_cond_.notify_all();
  }

  virtual void UpdateStats(MaintenanceOpStats* stats) OVERRIDE {
    boost::lock_guard<boost::mutex> guard(lock_);
    stats->runnable = (state_ == OP_RUNNABLE);
    stats->ram_anchored = ram_anchored_;
    stats->ts_anchored_secs = ts_anchored_ms_;
    stats->perf_improvement = perf_improvement_;
  }

  void Enable() {
    boost::unique_lock<boost::mutex> guard(lock_);
    DCHECK((state_ == OP_DISABLED) || (state_ == OP_FINISHED));
    state_ = OP_RUNNABLE;
    state_change_cond_.notify_all();
  }

  void WaitForState(TestMaintenanceOpState state) {
    boost::unique_lock<boost::mutex> guard(lock_);
    while (true) {
      if (state_ == state) {
        return;
      }
      state_change_cond_.wait(guard);
    }
  }

  bool WaitForStateWithTimeout(TestMaintenanceOpState state, int ms) {
    boost::unique_lock<boost::mutex> guard(lock_);
    boost::system_time stop_time = boost::get_system_time() +
      boost::posix_time::milliseconds(ms);
    while (true) {
      if (state_ == state) {
        return true;
      }
      if (boost::get_system_time() > stop_time) {
        return false;
      }
      state_change_cond_.timed_wait(guard, stop_time);
    }
  }

  void set_ram_anchored(uint64_t ram_anchored) {
    boost::lock_guard<boost::mutex> guard(lock_);
    ram_anchored_ = ram_anchored;
  }

  void set_ts_anchored_secs(uint64_t ts_anchored_secs) {
    boost::lock_guard<boost::mutex> guard(lock_);
    ts_anchored_ms_ = ts_anchored_secs;
  }

  void set_perf_improvement(uint64_t perf_improvement) {
    boost::lock_guard<boost::mutex> guard(lock_);
    perf_improvement_ = perf_improvement;
  }

  virtual Histogram* DurationHistogram() {
    return duration_histogram_;
  }

  virtual AtomicGauge<uint32_t>* RunningGauge() {
    return running_gauge_;
  }

 private:
  boost::mutex lock_;
  boost::condition_variable state_change_cond_;
  enum TestMaintenanceOpState state_;
  uint64_t ram_anchored_;
  uint64_t ts_anchored_ms_;
  uint64_t perf_improvement_;
  MetricRegistry metric_registry_;
  MetricContext metric_ctx_;
  Histogram* duration_histogram_;
  AtomicGauge<uint32_t>* running_gauge_;
};

// Create an op and wait for it to start running.  Unregister it while it is
// running and verify that UnregisterOp waits for it to finish before
// proceeding.
TEST(MaintenanceManagerTest, TestRegisterUnregister) {
  MaintenanceManager::Options options;
  options.num_threads = 2;
  options.polling_interval_ms = 1;
  options.memory_limit = 1;
  options.max_ts_anchored_secs = 1000;
  shared_ptr<MaintenanceManager> manager(new MaintenanceManager(options));
  ASSERT_STATUS_OK(manager->Init());
  TestMaintenanceOp op1("1", OP_DISABLED);
  manager->RegisterOp(&op1);
  scoped_refptr<kudu::Thread> thread;
  CHECK_OK(Thread::Create("TestThread", "TestRegisterUnregister",
        boost::bind(&TestMaintenanceOp::Enable, &op1), &thread));
  op1.WaitForState(OP_FINISHED);
  manager->UnregisterOp(&op1);
  ThreadJoiner(thread.get()).Join();
  manager->Shutdown();
}

// Test that we'll run an operation that doesn't improve performance when memory
// pressure gets high.
TEST(MaintenanceManagerTest, TestMemoryPressure) {
  MaintenanceManager::Options options;
  options.num_threads = 2;
  options.polling_interval_ms = 1;
  options.memory_limit = 1000;
  options.max_ts_anchored_secs = 1000;
  shared_ptr<MaintenanceManager> manager(new MaintenanceManager(options));
  ASSERT_STATUS_OK(manager->Init());
  TestMaintenanceOp op("op", OP_RUNNABLE);
  op.set_perf_improvement(0);
  op.set_ram_anchored(100);
  manager->RegisterOp(&op);

  // At first, we don't want to run this, since there is no perf_improvement.
  CHECK_EQ(false, op.WaitForStateWithTimeout(OP_FINISHED, 20));

  // set the ram_anchored by the high mem op so high that we'll have to run it.
  scoped_refptr<kudu::Thread> thread;
  CHECK_OK(Thread::Create("TestThread", "MaintenanceManagerTest",
      boost::bind(&TestMaintenanceOp::set_ram_anchored, &op, 1100), &thread));
  op.WaitForState(OP_FINISHED);
  manager->UnregisterOp(&op);
  ThreadJoiner(thread.get()).Join();
  manager->Shutdown();
}

// Test adding operations and make sure that the history of recently completed operations
// is correct in that it wraps around and doesn't grow.
TEST(MaintenanceManagerTest, TestCompletedOpsHistory) {
  int history_size = 4;
  MaintenanceManager::Options options;
  options.num_threads = 2;
  options.polling_interval_ms = 1;
  options.memory_limit = 1000;
  options.max_ts_anchored_secs = 1000;
  options.history_size = history_size;

  shared_ptr<MaintenanceManager> manager(new MaintenanceManager(options));
  ASSERT_STATUS_OK(manager->Init());

  for (int i = 0; i < 5; i++) {
    string name = Substitute("op$0", i);
    TestMaintenanceOp op(name, OP_RUNNABLE);
    op.set_perf_improvement(1);
    op.set_ram_anchored(100);
    manager->RegisterOp(&op);

    CHECK_EQ(true, op.WaitForStateWithTimeout(OP_FINISHED, 200));
    manager->UnregisterOp(&op);

    MaintenanceManagerStatusPB status_pb;
    manager->GetMaintenanceManagerStatusDump(&status_pb);
    // The size should be at most the history_size.
    ASSERT_GE(history_size, status_pb.completed_operations_size());
    // See that we have the right name, even if we wrap around.
    ASSERT_EQ(name, status_pb.completed_operations(i % 4).name());
  }
  manager->Shutdown();
}

} // namespace kudu
