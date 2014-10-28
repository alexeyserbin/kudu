// Copyright (c) 2013, Cloudera, inc.
// Confidential Cloudera Information: Covered by NDA.

#include <boost/assign/list_of.hpp>
#include <gflags/gflags.h>
#include <gtest/gtest.h>
#include <tr1/memory>

#include "kudu/common/schema.h"
#include "kudu/tablet/delta_store.h"
#include "kudu/tablet/deltafile.h"
#include "kudu/tablet/delta_tracker.h"
#include "kudu/gutil/algorithm.h"
#include "kudu/gutil/strings/strcat.h"
#include "kudu/util/memenv/memenv.h"
#include "kudu/util/test_macros.h"

DECLARE_int32(deltafile_block_size);
DEFINE_int32(first_row_to_update, 10000, "the first row to update");
DEFINE_int32(last_row_to_update, 100000, "the last row to update");
DEFINE_int32(n_verify, 1, "number of times to verify the updates"
             "(useful for benchmarks");

namespace kudu {
namespace tablet {

using fs::ReadableBlock;
using fs::WritableBlock;
using std::tr1::shared_ptr;
using util::gtl::is_sorted;

// Test path to write delta file to (in in-memory environment)
const char kTestPath[] = "/tmp/test";

class TestDeltaFile : public ::testing::Test {
 public:
  TestDeltaFile() :
    env_(NewMemEnv(Env::Default())),
    schema_(CreateSchema()),
    arena_(1024, 1024),
    kTestBlock("test-block-id") {
  }

 public:
  void SetUp() OVERRIDE {
    fs_manager_.reset(new FsManager(env_.get(), kTestPath));
    ASSERT_OK(fs_manager_->CreateInitialFileSystemLayout());
    ASSERT_OK(fs_manager_->Open());
  }

  static Schema CreateSchema() {
    SchemaBuilder builder;
    CHECK_OK(builder.AddColumn("val", UINT32));
    return builder.Build();
  }

  void WriteTestFile(int min_timestamp = 0, int max_timestamp = 0) {
    gscoped_ptr<WritableBlock> block;
    ASSERT_STATUS_OK(fs_manager_->CreateBlockWithId(kTestBlock, &block));
    DeltaFileWriter dfw(schema_, block.Pass());
    ASSERT_STATUS_OK(dfw.Start());

    // Update even numbered rows.
    faststring buf;

    DeltaStats stats(schema_.num_columns());
    for (int i = FLAGS_first_row_to_update; i <= FLAGS_last_row_to_update; i += 2) {
      for (int timestamp = min_timestamp; timestamp <= max_timestamp; timestamp++) {
        buf.clear();
        RowChangeListEncoder update(&schema_, &buf);
        uint32_t new_val = timestamp + i;
        update.AddColumnUpdate(schema_.column_id(0), &new_val);
        DeltaKey key(i, Timestamp(timestamp));
        RowChangeList rcl(buf);
        ASSERT_STATUS_OK_FAST(dfw.AppendDelta<REDO>(key, rcl));
        ASSERT_STATUS_OK_FAST(stats.UpdateStats(key.timestamp(), schema_, rcl));
      }
    }
    ASSERT_STATUS_OK(dfw.WriteDeltaStats(stats));
    ASSERT_STATUS_OK(dfw.Finish());
  }


  void DoTestRoundTrip() {
    // First write the file.
    WriteTestFile();

    // Then iterate back over it, applying deltas to a fake row block.
    for (int i = 0; i < FLAGS_n_verify; i++) {
      VerifyTestFile();
    }
  }

  Status OpenDeltaFileReader(const BlockId& block_id, shared_ptr<DeltaFileReader>* out) {
    gscoped_ptr<ReadableBlock> block;
    RETURN_NOT_OK(fs_manager_->OpenBlock(block_id, &block));
    return DeltaFileReader::Open(block.Pass(), block_id, out, REDO);
  }

  // TODO handle UNDO deltas
  Status OpenDeltaFileIterator(const BlockId& block_id, gscoped_ptr<DeltaIterator>* out) {
    shared_ptr<DeltaFileReader> reader;
    RETURN_NOT_OK(OpenDeltaFileReader(block_id, &reader));
    return OpenDeltaFileIteratorFromReader(REDO, reader, out);
  }

  Status OpenDeltaFileIteratorFromReader(DeltaType type,
                                         const shared_ptr<DeltaFileReader>& reader,
                                         gscoped_ptr<DeltaIterator>* out) {
    MvccSnapshot snap = type == REDO ?
                        MvccSnapshot::CreateSnapshotIncludingAllTransactions() :
                        MvccSnapshot::CreateSnapshotIncludingNoTransactions();
    DeltaIterator* raw_iter;
    RETURN_NOT_OK(reader->NewDeltaIterator(&schema_, snap, &raw_iter));
    out->reset(raw_iter);
    return Status::OK();
  }

  void VerifyTestFile() {
    shared_ptr<DeltaFileReader> reader;
    ASSERT_STATUS_OK(OpenDeltaFileReader(kTestBlock, &reader));
    ASSERT_EQ(((FLAGS_last_row_to_update - FLAGS_first_row_to_update) / 2) + 1,
              reader->delta_stats().update_count(0));
    ASSERT_EQ(0, reader->delta_stats().delete_count());
    gscoped_ptr<DeltaIterator> it;
    Status s = OpenDeltaFileIteratorFromReader(REDO, reader, &it);
    if (s.IsNotFound()) {
      FAIL() << "Iterator fell outside of the range of an include-all snapshot";
    }
    ASSERT_STATUS_OK(s);
    ASSERT_STATUS_OK(it->Init());

    RowBlock block(schema_, 100, &arena_);

    // Iterate through the faked table, starting with batches that
    // come before all of the updates, and extending a bit further
    // past the updates, to ensure that nothing breaks on the boundaries.
    ASSERT_STATUS_OK(it->SeekToOrdinal(0));

    int start_row = 0;
    while (start_row < FLAGS_last_row_to_update + 10000) {
      block.ZeroMemory();
      arena_.Reset();

      ASSERT_STATUS_OK_FAST(it->PrepareBatch(block.nrows()));
      ColumnBlock dst_col = block.column_block(0);
      ASSERT_STATUS_OK_FAST(it->ApplyUpdates(0, &dst_col));

      for (int i = 0; i < block.nrows(); i++) {
        uint32_t row = start_row + i;
        bool should_be_updated = (row >= FLAGS_first_row_to_update) &&
          (row <= FLAGS_last_row_to_update) &&
          (row % 2 == 0);

        DCHECK_EQ(block.row(i).cell_ptr(0), dst_col.cell_ptr(i));
        uint32_t updated_val = *schema_.ExtractColumnFromRow<UINT32>(block.row(i), 0);
        VLOG(2) << "row " << row << ": " << updated_val;
        uint32_t expected_val = should_be_updated ? row : 0;
        // Don't use ASSERT_EQ, since it's slow (records positive results, not just negative)
        if (updated_val != expected_val) {
          FAIL() << "failed on row " << row <<
            ": expected " << expected_val << ", got " << updated_val;
        }
      }

      start_row += block.nrows();
    }
  }

 protected:
  gscoped_ptr<Env> env_;
  gscoped_ptr<FsManager> fs_manager_;
  Schema schema_;
  Arena arena_;
  const BlockId kTestBlock;
};

TEST_F(TestDeltaFile, TestDumpDeltaFileIterator) {
  WriteTestFile();

  gscoped_ptr<DeltaIterator> it;
  Status s = OpenDeltaFileIterator(kTestBlock, &it);
  if (s.IsNotFound()) {
    FAIL() << "Iterator fell outside of the range of an include-all snapshot";
  }
  ASSERT_STATUS_OK(s);
  vector<string> it_contents;
  ASSERT_STATUS_OK(DebugDumpDeltaIterator(REDO,
                                          it.get(),
                                          schema_,
                                          ITERATE_OVER_ALL_ROWS,
                                          &it_contents));
  BOOST_FOREACH(const string& str, it_contents) {
    VLOG(1) << str;
  }
  ASSERT_TRUE(is_sorted(it_contents.begin(), it_contents.end()));
  ASSERT_EQ(it_contents.size(), (FLAGS_last_row_to_update - FLAGS_first_row_to_update) / 2 + 1);
}

TEST_F(TestDeltaFile, TestWriteDeltaFileIteratorToFile) {
  WriteTestFile();
  gscoped_ptr<DeltaIterator> it;
  Status s = OpenDeltaFileIterator(kTestBlock, &it);
  if (s.IsNotFound()) {
    FAIL() << "Iterator fell outside of the range of an include-all snapshot";
  }
  ASSERT_STATUS_OK(s);

  gscoped_ptr<WritableBlock> block;
  ASSERT_STATUS_OK(fs_manager_->CreateNewBlock(&block));
  BlockId block_id(block->id());
  DeltaFileWriter dfw(schema_, block.Pass());
  ASSERT_STATUS_OK(dfw.Start());
  ASSERT_STATUS_OK(WriteDeltaIteratorToFile<REDO>(it.get(),
                                                  schema_,
                                                  ITERATE_OVER_ALL_ROWS,
                                                  &dfw));
  ASSERT_STATUS_OK(dfw.Finish());


  // If delta stats are incorrect, then a Status::NotFound would be
  // returned.

  ASSERT_STATUS_OK(OpenDeltaFileIterator(block_id, &it));
  vector<string> it_contents;
  ASSERT_STATUS_OK(DebugDumpDeltaIterator(REDO,
                                          it.get(),
                                          schema_,
                                          ITERATE_OVER_ALL_ROWS,
                                          &it_contents));
  BOOST_FOREACH(const string& str, it_contents) {
    VLOG(1) << str;
  }
  ASSERT_TRUE(is_sorted(it_contents.begin(), it_contents.end()));
  ASSERT_EQ(it_contents.size(), (FLAGS_last_row_to_update - FLAGS_first_row_to_update) / 2 + 1);
}

TEST_F(TestDeltaFile, TestRoundTripTinyDeltaBlocks) {
  // Set block size small, so that we get good coverage
  // of the case where multiple delta blocks correspond to a
  // single underlying data block.
  google::FlagSaver saver;
  FLAGS_deltafile_block_size = 256;
  DoTestRoundTrip();
}

TEST_F(TestDeltaFile, TestRoundTrip) {
  DoTestRoundTrip();
}

TEST_F(TestDeltaFile, TestCollectMutations) {
  WriteTestFile();

  {
    gscoped_ptr<DeltaIterator> it;
    Status s = OpenDeltaFileIterator(kTestBlock, &it);
    if (s.IsNotFound()) {
      FAIL() << "Iterator fell outside of the range of an include-all snapshot";
    }
    ASSERT_STATUS_OK(s);

    ASSERT_STATUS_OK(it->Init());
    ASSERT_STATUS_OK(it->SeekToOrdinal(0));

    vector<Mutation *> mutations;
    mutations.resize(100);

    int start_row = 0;
    while (start_row < FLAGS_last_row_to_update + 10000) {
      std::fill(mutations.begin(), mutations.end(), reinterpret_cast<Mutation *>(NULL));

      arena_.Reset();
      ASSERT_STATUS_OK_FAST(it->PrepareBatch(mutations.size()));
      ASSERT_STATUS_OK(it->CollectMutations(&mutations, &arena_));

      for (int i = 0; i < mutations.size(); i++) {
        Mutation *mut_head = mutations[i];
        if (mut_head != NULL) {
          rowid_t row = start_row + i;
          string str = Mutation::StringifyMutationList(schema_, mut_head);
          VLOG(1) << "Mutation on row " << row << ": " << str;
        }
      }

      start_row += mutations.size();
    }
  }

}

TEST_F(TestDeltaFile, TestSkipsDeltasOutOfRange) {
  WriteTestFile(10, 20);
  shared_ptr<DeltaFileReader> reader;
  ASSERT_STATUS_OK(OpenDeltaFileReader(kTestBlock, &reader));

  gscoped_ptr<DeltaIterator> iter;

  // should skip
  MvccSnapshot snap1(Timestamp(9));
  ASSERT_FALSE(snap1.MayHaveCommittedTransactionsAtOrAfter(Timestamp(10)));
  DeltaIterator* raw_iter = NULL;
  Status s = reader->NewDeltaIterator(&schema_, snap1, &raw_iter);
  ASSERT_TRUE(s.IsNotFound());
  ASSERT_TRUE(raw_iter == NULL);

  // should include
  raw_iter = NULL;
  MvccSnapshot snap2(Timestamp(15));
  ASSERT_STATUS_OK(reader->NewDeltaIterator(&schema_, snap2, &raw_iter));
  ASSERT_TRUE(raw_iter != NULL);
  iter.reset(raw_iter);

  // should include
  raw_iter = NULL;
  MvccSnapshot snap3(Timestamp(21));
  ASSERT_STATUS_OK(reader->NewDeltaIterator(&schema_, snap3, &raw_iter));
  ASSERT_TRUE(raw_iter != NULL);
  iter.reset(raw_iter);
}

} // namespace tablet
} // namespace kudu
