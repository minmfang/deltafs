/*
 * Copyright (c) 2015-2017 Carnegie Mellon University.
 *
 * All rights reserved.
 *
 * Use of this source code is governed by a BSD-style license that can be
 * found in the LICENSE file. See the AUTHORS file for names of contributors.
 */

#include "deltafs_plfsio_batch.h"
#include "deltafs_plfsio_internal.h"

#include "pdlfs-common/histogram.h"
#include "pdlfs-common/port.h"
#include "pdlfs-common/testharness.h"
#include "pdlfs-common/testutil.h"
#include "pdlfs-common/xxhash.h"

#include <stdlib.h>
#ifdef PDLFS_PLATFORM_POSIX
#ifdef PDLFS_OS_LINUX
#include <sched.h>
#include <sys/types.h>
#endif
#include <sys/resource.h>
#include <sys/time.h>
#endif

#include <map>

namespace pdlfs {
namespace plfsio {

class WriterBufTest {
 public:
  explicit WriterBufTest(uint32_t seed = 301) : num_entries_(0), rnd_(seed) {}

  Iterator* Flush() {
    buffer_.Finish();
    ASSERT_EQ(buffer_.NumEntries(), num_entries_);
    return buffer_.NewIterator();
  }

  void Add(uint64_t seq, size_t value_size = 32) {
    std::string key;
    PutFixed64(&key, seq);
    std::string value;
    test::RandomString(&rnd_, value_size, &value);
    kv_.insert(std::make_pair(key, value));
    buffer_.Add(key, value);
    num_entries_++;
  }

  void CheckFirst(Iterator* iter) {
    iter->SeekToFirst();
    ASSERT_TRUE(iter->Valid());
    Slice value = iter->value();
    ASSERT_TRUE(value == kv_.begin()->second);
    Slice key = iter->key();
    ASSERT_TRUE(key == kv_.begin()->first);
  }

  void CheckLast(Iterator* iter) {
    iter->SeekToLast();
    ASSERT_TRUE(iter->Valid());
    Slice value = iter->value();
    ASSERT_TRUE(value == kv_.rbegin()->second);
    Slice key = iter->key();
    ASSERT_TRUE(key == kv_.rbegin()->first);
  }

  std::map<std::string, std::string> kv_;

  uint32_t num_entries_;
  WriteBuffer buffer_;
  Random rnd_;
};

TEST(WriterBufTest, FixedSizedValue) {
  Add(3);
  Add(2);
  Add(1);
  Add(5);
  Add(4);

  Iterator* iter = Flush();
  CheckFirst(iter);
  CheckLast(iter);
  delete iter;
}

TEST(WriterBufTest, VariableSizedValue) {
  Add(3, 16);
  Add(2, 18);
  Add(1, 20);
  Add(5, 14);
  Add(4, 18);

  Iterator* iter = Flush();
  CheckFirst(iter);
  CheckLast(iter);
  delete iter;
}

static inline Env* TestEnv() {
  Env* env = port::posix::GetUnBufferedIOEnv();
  return env;
}

class PlfsIoTest {
 public:
  PlfsIoTest() {
    dirname_ = test::TmpDir() + "/plfsio_test";
    options_.total_memtable_budget = 1 << 20;
    options_.block_batch_size = 256 << 10;
    options_.block_size = 64 << 10;
    options_.verify_checksums = true;
    options_.paranoid_checks = true;
    options_.env = TestEnv();
    writer_ = NULL;
    reader_ = NULL;
    epoch_ = 0;
  }

  ~PlfsIoTest() {
    if (writer_ != NULL) {
      delete writer_;
    }
    if (reader_ != NULL) {
      delete reader_;
    }
  }

  void OpenWriter() {
    DestroyDir(dirname_, options_);
    Status s = DirWriter::Open(options_, dirname_, &writer_);
    ASSERT_OK(s);
  }

  void Finish() {
    ASSERT_OK(writer_->Finish());
    delete writer_;
    writer_ = NULL;
  }

  void OpenReader() {
    Status s = DirReader::Open(options_, dirname_, &reader_);
    ASSERT_OK(s);
  }

  void MakeEpoch() {
    if (writer_ == NULL) OpenWriter();
    ASSERT_OK(writer_->EpochFlush(epoch_));
    epoch_++;
  }

  void Write(const Slice& key, const Slice& value) {
    if (writer_ == NULL) OpenWriter();
    ASSERT_OK(writer_->Append(key, value, epoch_));
  }

  std::string Read(const Slice& key) {
    std::string tmp;
    if (writer_ != NULL) Finish();
    if (reader_ == NULL) OpenReader();
    ASSERT_OK(reader_->ReadAll(key, &tmp));
    return tmp;
  }

  DirOptions options_;
  std::string dirname_;
  DirWriter* writer_;
  DirReader* reader_;
  int epoch_;
};

TEST(PlfsIoTest, Empty) {
  MakeEpoch();
  std::string val = Read("non-exists");
  ASSERT_TRUE(val.empty());
}

TEST(PlfsIoTest, SingleEpoch) {
  Write("k1", "v1");
  Write("k2", "v2");
  Write("k3", "v3");
  Write("k4", "v4");
  Write("k5", "v5");
  Write("k6", "v6");
  MakeEpoch();
  ASSERT_EQ(Read("k1"), "v1");
  ASSERT_TRUE(Read("k1.1").empty());
  ASSERT_EQ(Read("k2"), "v2");
  ASSERT_TRUE(Read("k2.1").empty());
  ASSERT_EQ(Read("k3"), "v3");
  ASSERT_TRUE(Read("k3.1").empty());
  ASSERT_EQ(Read("k4"), "v4");
  ASSERT_TRUE(Read("k4.1").empty());
  ASSERT_EQ(Read("k5"), "v5");
  ASSERT_TRUE(Read("k5.1").empty());
  ASSERT_EQ(Read("k6"), "v6");
}

TEST(PlfsIoTest, MultiEpoch) {
  Write("k1", "v1");
  Write("k2", "v2");
  MakeEpoch();
  Write("k1", "v3");
  Write("k2", "v4");
  MakeEpoch();
  Write("k1", "v5");
  Write("k2", "v6");
  MakeEpoch();
  ASSERT_EQ(Read("k1"), "v1v3v5");
  ASSERT_TRUE(Read("k1.1").empty());
  ASSERT_EQ(Read("k2"), "v2v4v6");
}

TEST(PlfsIoTest, Snappy) {
  options_.compression = kSnappyCompression;
  options_.force_compression = true;
  Write("k1", "v1");
  Write("k2", "v2");
  MakeEpoch();
  Write("k1", "v3");
  Write("k2", "v4");
  MakeEpoch();
  Write("k1", "v5");
  Write("k2", "v6");
  MakeEpoch();
  ASSERT_EQ(Read("k1"), "v1v3v5");
  ASSERT_TRUE(Read("k1.1").empty());
  ASSERT_EQ(Read("k2"), "v2v4v6");
}

TEST(PlfsIoTest, LargeBatch) {
  const std::string dummy_val(32, 'x');
  const int batch_size = 64 << 10;
  char tmp[10];
  for (int i = 0; i < batch_size; i++) {
    snprintf(tmp, sizeof(tmp), "k%07d", i);
    Write(Slice(tmp), dummy_val);
  }
  MakeEpoch();
  for (int i = 0; i < batch_size; i++) {
    snprintf(tmp, sizeof(tmp), "k%07d", i);
    Write(Slice(tmp), dummy_val);
  }
  MakeEpoch();
  for (int i = 0; i < batch_size; i++) {
    snprintf(tmp, sizeof(tmp), "k%07d", i);
    ASSERT_EQ(Read(Slice(tmp)).size(), dummy_val.size() * 2) << tmp;
    if (i % 1024 == 1023) {
      fprintf(stderr, "key [%07d-%07d): OK\n", i - 1023, i + 1);
    }
  }
  ASSERT_TRUE(Read("kx").empty());
}

TEST(PlfsIoTest, NoFilter) {
  options_.bf_bits_per_key = 0;
  Write("k1", "v1");
  Write("k2", "v2");
  MakeEpoch();
  Write("k3", "v3");
  Write("k4", "v4");
  MakeEpoch();
  Write("k5", "v5");
  Write("k6", "v6");
  MakeEpoch();
  ASSERT_EQ(Read("k1"), "v1");
  ASSERT_TRUE(Read("k1.1").empty());
  ASSERT_EQ(Read("k2"), "v2");
  ASSERT_TRUE(Read("k2.1").empty());
  ASSERT_EQ(Read("k3"), "v3");
  ASSERT_TRUE(Read("k3.1").empty());
  ASSERT_EQ(Read("k4"), "v4");
  ASSERT_TRUE(Read("k4.1").empty());
  ASSERT_EQ(Read("k5"), "v5");
  ASSERT_TRUE(Read("k5.1").empty());
  ASSERT_EQ(Read("k6"), "v6");
}

TEST(PlfsIoTest, NoUniKeys) {
  options_.unique_keys = false;
  Write("k1", "v1");
  Write("k1", "v2");
  MakeEpoch();
  Write("k0", "v3");
  Write("k1", "v4");
  Write("k1", "v5");
  MakeEpoch();
  Write("k1", "v6");
  Write("k1", "v7");
  Write("k5", "v8");
  MakeEpoch();
  Write("k1", "v9");
  MakeEpoch();
  ASSERT_EQ(Read("k1"), "v1v2v4v5v6v7v9");
}

namespace {

class FakeWritableFile : public WritableFileWrapper {
 public:
  FakeWritableFile(Histogram* hist, uint64_t bytes_ps)
      : prev_write_micros(0), hist_(hist), bytes_ps_(bytes_ps) {}
  virtual ~FakeWritableFile() {}

  virtual Status Append(const Slice& data) {
    if (!data.empty()) {
      uint64_t now_micros = Env::Default()->NowMicros();
      if (prev_write_micros != 0) {
        hist_->Add(now_micros - prev_write_micros);
      }
      prev_write_micros = now_micros;
      int micros_to_delay =
          static_cast<int>(1000 * 1000 * data.size() / bytes_ps_);
      Env::Default()->SleepForMicroseconds(micros_to_delay);
    }
    return Status::OK();
  }

 private:
  uint64_t prev_write_micros;  // Timestamp of the previous write
  Histogram* hist_;            // Mean time between writes

  uint64_t bytes_ps_;  // Bytes per second
};

class FakeEnv : public EnvWrapper {
 public:
  explicit FakeEnv(uint64_t bytes_ps)
      : EnvWrapper(TestEnv()), bytes_ps_(bytes_ps) {}
  virtual ~FakeEnv() {
    HistIter iter = hists_.begin();
    for (; iter != hists_.end(); ++iter) {
      delete iter->second;
    }
  }

  virtual Status NewWritableFile(const Slice& f, WritableFile** r) {
    Histogram* hist = new Histogram;
    hists_.insert(std::make_pair(f.ToString(), hist));
    *r = new FakeWritableFile(hist, bytes_ps_);
    return Status::OK();
  }

  const Histogram* GetHist(const Slice& suffix) {
    HistIter iter = hists_.begin();
    for (; iter != hists_.end(); ++iter) {
      if (Slice(iter->first).ends_with(suffix)) {
        return iter->second;
      }
    }
    return NULL;
  }

 private:
  uint64_t bytes_ps_;  // Bytes per second

  typedef std::map<std::string, Histogram*> HistMap;
  typedef HistMap::iterator HistIter;

  HistMap hists_;
};

}  // anonymous namespace

class PlfsIoBench {
 public:
  static int GetOption(const char* key, int defval) {
    const char* env = getenv(key);
    if (env == NULL) {
      return defval;
    } else if (strlen(env) == 0) {
      return defval;
    } else {
      return atoi(env);
    }
  }

  PlfsIoBench() : home_(test::TmpDir() + "/plfsio_test_benchmark") {
    link_speed_ =
        GetOption("LINK_SPEED", 6);  // Burst-buffer link speed is 6 MBps
    ordered_keys_ = GetOption("ORDERED_KEYS", false);
    num_files_ = GetOption("NUM_FILES", 16);  // 16M files per epoch

    num_threads_ = GetOption("NUM_THREADS", 4);  // For bg compaction

    options_.rank = 0;
    options_.lg_parts = GetOption("LG_PARTS", 2);
    options_.skip_sort = ordered_keys_ != 0;
    options_.compression =
        GetOption("SNAPPY", false) ? kSnappyCompression : kNoCompression;
    options_.force_compression = true;
    options_.total_memtable_budget =
        static_cast<size_t>(GetOption("MEMTABLE_SIZE", 32) << 20);
    options_.block_size =
        static_cast<size_t>(GetOption("BLOCK_SIZE", 128) << 10);
    options_.block_batch_size =
        static_cast<size_t>(GetOption("BLOCK_BATCH_SIZE", 2) << 20);
    options_.block_util = GetOption("BLOCK_UTIL", 999) / 1000.0;
    options_.bf_bits_per_key = static_cast<size_t>(GetOption("BF_BITS", 10));
    options_.value_size = static_cast<size_t>(GetOption("VALUE_SIZE", 40));
    options_.key_size = static_cast<size_t>(GetOption("KEY_SIZE", 10));
    options_.data_buffer =
        static_cast<size_t>(GetOption("DATA_BUFFER", 8) << 20);
    options_.index_buffer =
        static_cast<size_t>(GetOption("INDEX_BUFFER", 2) << 20);

    writer_ = NULL;

    env_ = NULL;
  }

  ~PlfsIoBench() {
    delete writer_;
    writer_ = NULL;
    delete env_;
    env_ = NULL;
  }

  void LogAndApply() {
    DestroyDir(home_, options_);
    DoIt();
  }

  class BigBatch : public BatchCursor {
   public:
    BigBatch(const DirOptions& options, int num_files, bool ordered_keys)
        : key_size_(options.key_size),
          dummy_val_(options.value_size, 'x'),
          total_files_(static_cast<uint32_t>(num_files << 20)),
          ordered_keys_(ordered_keys),
          offset_(total_files_) {}

    virtual ~BigBatch() {}

    virtual Status status() const { return status_; }
    virtual bool Valid() const { return offset_ < total_files_; }
    virtual uint32_t offset() const { return offset_; }
    virtual Slice fid() const { return Slice(tmp_, key_size_); }
    virtual Slice data() const { return dummy_val_; }

    virtual void Seek(uint32_t offset) {
      offset_ = offset;
      if (Valid()) {
        MakeKey();
      }
    }

    virtual void Next() {
      offset_++;
      if (Valid()) {
        MakeKey();
      }
    }

   private:
    size_t key_size_;
    std::string dummy_val_;
    uint32_t total_files_;
    bool ordered_keys_;
    Status status_;

    void ToKey(int fid) {
      snprintf(tmp_, sizeof(tmp_), "%08x-%08x-%08x", fid, fid, fid);
    }

    void MakeKey() {
      if (offset_ % (1 << 20) == (1 << 20) - 1) {
        fprintf(stderr, "\r%.2f%%", 100.0 * (offset_ + 1) / total_files_);
      }
      uint32_t fid;
      if (!ordered_keys_) {
        fid = xxhash32(&offset_, sizeof(offset_), 0);
      } else {
        fid = offset_;
      }
      ToKey(fid);
    }

    uint32_t offset_;
    char tmp_[30];
  };

  void DoIt() {
    bool owns_pool = false;
    if (num_threads_ != 0) {
      options_.compaction_pool = ThreadPool::NewFixed(num_threads_);
      owns_pool = true;
    } else {
      options_.allow_env_threads = false;
      options_.compaction_pool = NULL;
    }
    bool owns_env = false;
    if (env_ == NULL) {
      env_ = new FakeEnv(static_cast<uint64_t>(link_speed_ << 20));
      owns_env = true;
    }
    options_.env = env_;
    Status s = DirWriter::Open(options_, home_, &writer_);
    ASSERT_OK(s) << "Cannot open dir";
    const uint64_t start = env_->NowMicros();
    fprintf(stderr, "Inserting data...\n");
    BigBatch batch(options_, num_files_, ordered_keys_);
    batch.Seek(0);
    for (; batch.Valid(); batch.Next()) {
      s = writer_->Append(batch.fid(), batch.data(), 0);
      ASSERT_OK(s) << "Cannot write";
    }
    fprintf(stderr, "\n");

    s = writer_->EpochFlush(0);
    ASSERT_OK(s) << "Cannot flush epoch";
    s = writer_->Finish();
    ASSERT_OK(s) << "Cannot finish";

    fprintf(stderr, "Done!\n");
    uint64_t dura = env_->NowMicros() - start;

    PrintStats(dura, owns_env);

    delete writer_;
    writer_ = NULL;

    if (owns_pool) {
      delete options_.compaction_pool;
      options_.compaction_pool = NULL;
    }
    if (owns_env) {
      delete options_.env;
      options_.env = NULL;
      env_ = NULL;
    }
  }

#ifdef PDLFS_PLATFORM_POSIX
  static inline double ToSecs(const struct timeval* tv) {
    return tv->tv_sec + tv->tv_usec / 1000.0 / 1000.0;
  }
#endif

  void PrintStats(uint64_t dura, bool owns_env) {
    const double k = 1000.0, ki = 1024.0;
    fprintf(stderr, "----------------------------------------\n");
    const uint64_t total_memory_usage = writer_->TEST_total_memory_usage();
    fprintf(stderr, "     Total Memory Usage: %.3f MB\n",
            total_memory_usage / ki / ki);
    fprintf(stderr, "             Total Time: %.3f s\n", dura / k / k);
    const IoStats stats = writer_->GetIoStats();
#ifdef PDLFS_PLATFORM_POSIX
    struct rusage usage;
    int r1 = getrusage(RUSAGE_SELF, &usage);
    ASSERT_TRUE(r1 == 0);
    fprintf(stderr, "          User CPU Time: %.3f s\n",
            ToSecs(&usage.ru_utime));
    fprintf(stderr, "        System CPU Time: %.3f s\n",
            ToSecs(&usage.ru_stime));
#ifdef PDLFS_OS_LINUX
    cpu_set_t cpu_set;
    CPU_ZERO(&cpu_set);
    int r2 = sched_getaffinity(getpid(), sizeof(cpu_set), &cpu_set);
    ASSERT_TRUE(r2 == 0);
    fprintf(stderr, "          Num CPU Cores: %d\n", CPU_COUNT(&cpu_set));
    fprintf(stderr, "              CPU Usage: %.1f%%\n",
            k * k * (ToSecs(&usage.ru_utime) + ToSecs(&usage.ru_stime)) /
                CPU_COUNT(&cpu_set) / dura * 100);
#endif
#endif
    fprintf(stderr, "      Ordered Insertion: %s\n",
            ordered_keys_ ? "Yes" : "No");
    fprintf(stderr, "    Indexes Compression: %s\n",
            options_.compression == kSnappyCompression ? "Yes" : "No");
    fprintf(stderr, "              BF Budget: %d (bits pey key)\n",
            int(options_.bf_bits_per_key));
    fprintf(stderr, "     Num Files Inserted: %d million\n", num_files_);
    fprintf(stderr, "        Total File Data: %d MB\n", 48 * num_files_);
    fprintf(stderr, "  Total MemTable Budget: %d MB\n",
            int(options_.total_memtable_budget) >> 20);
    fprintf(stderr, " Estimated SSTable Size: %.3f MB\n",
            writer_->TEST_estimated_sstable_size() / ki / ki);
    fprintf(stderr, "   Estimated Block Size: %d KB (util: %.1f%%)\n",
            int(options_.block_size) >> 10, options_.block_util * 100);
    fprintf(stderr, "Num MemTable Partitions: %d\n", 1 << options_.lg_parts);
    fprintf(stderr, "         Num Bg Threads: %d\n", num_threads_);
    if (owns_env) {
      fprintf(stderr, "    Emulated Link Speed: %d MB/s (per log)\n",
              link_speed_);
    } else {
      fprintf(stderr, "    Emulated Link Speed: N/A\n");
    }
    fprintf(stderr, "            Write Speed: %.3f MB/s (observed by app)\n",
            1.0 * k * k * (options_.key_size + options_.value_size) *
                num_files_ / dura);
    fprintf(stderr, "              Index Buf: %d MB (x%d)\n",
            int(options_.index_buffer) >> 20, 1 << options_.lg_parts);
    fprintf(stderr, "  Total SSTable Indexes: %.3f MB (before compression)\n",
            writer_->TEST_index_size() / ki / ki);
    fprintf(stderr, "               Total BF: %.3f MB (before compression)\n",
            writer_->TEST_filter_size() / ki / ki);
    fprintf(stderr, "     Final Phys Indexes: %.3f MB\n",
            stats.index_bytes / ki / ki);
    fprintf(stderr, "         Compaction Buf: %d MB (x%d)\n",
            int(options_.block_batch_size) >> 20, 1 << options_.lg_parts);
    fprintf(stderr, "               Data Buf: %d MB\n",
            int(options_.data_buffer) >> 20);
    fprintf(stderr, "     Total SSTable Data: %.3f MB\n",
            writer_->TEST_data_size() / ki / ki);
    fprintf(stderr, "        Final Phys Data: %.3f MB\n",
            stats.data_bytes / ki / ki);
    fprintf(stderr, "           Avg I/O Size: %.3f MB\n",
            1.0 * stats.data_bytes / stats.data_ops / ki / ki);
    if (owns_env) {
      const Histogram* hist = dynamic_cast<FakeEnv*>(env_)->GetHist(".dat");
      ASSERT_TRUE(hist != NULL);
      fprintf(stderr, "                   MTBW: %.3f s\n",
              hist->Average() / k / k);
    } else {
      fprintf(stderr, "                   MTBW: N/A\n");
    }
    const uint32_t num_tables = writer_->TEST_num_sstables();
    fprintf(stderr, "         Total SSTables: %d\n", int(num_tables));
    fprintf(stderr, " SSTables Per Partition: %.1f\n",
            1.0 * num_tables / (1 << options_.lg_parts));
    fprintf(stderr, "             Value Size: %d bytes\n",
            int(options_.value_size));
    fprintf(stderr, "               Key Size: %d bytes\n",
            int(options_.key_size));
  }

  int link_speed_;  // Link speed to emulate (in MBps)
  int ordered_keys_;
  int num_files_;    // Number of particle files (in millions)
  int num_threads_;  // Number of bg compaction threads
  const std::string home_;
  DirOptions options_;
  DirWriter* writer_;
  Env* env_;
};

namespace {

class StringWritableFile : public WritableFileWrapper {
 public:
  explicit StringWritableFile(std::string* buffer) : buf_(buffer) {}
  virtual ~StringWritableFile() {}

  virtual Status Append(const Slice& data) {
    buf_->append(data.data(), data.size());
    return Status::OK();
  }

 private:
  // Owned by external code
  std::string* buf_;
};

class StringFile : public SequentialFile, public RandomAccessFile {
 public:
  explicit StringFile(const std::string* buffer) : buf_(buffer), off_(0) {}
  virtual ~StringFile() {}

  virtual Status Read(uint64_t offset, size_t n, Slice* result,
                      char* scratch) const {
    if (offset > buf_->size()) {
      offset = buf_->size();
    }
    if (n > buf_->size() - offset) {
      n = buf_->size() - offset;
    }
    if (n != 0) {
      *result = Slice(buf_->data() + offset, n);
    } else {
      *result = Slice();
    }
    return Status::OK();
  }

  virtual Status Read(size_t n, Slice* result, char* scratch) {
    if (n > buf_->size() - off_) {
      n = buf_->size() - off_;
    }
    if (n != 0) {
      *result = Slice(buf_->data() + off_, n);
    } else {
      *result = Slice();
    }
    return Skip(n);
  }

  virtual Status Skip(uint64_t n) {
    if (n > buf_->size() - off_) {
      n = buf_->size() - off_;
    }
    off_ += n;
    return Status::OK();
  }

 private:
  // Owned by external code
  const std::string* buf_;
  size_t off_;
};

class StringEnv : public EnvWrapper {
 public:
  StringEnv() : EnvWrapper(TestEnv()) {}

  virtual ~StringEnv() {
    FSIter iter = fs_.begin();
    for (; iter != fs_.end(); ++iter) {
      delete iter->second;
    }
  }

  virtual Status NewWritableFile(const Slice& f, WritableFile** r) {
    std::string* buf = new std::string;
    fs_.insert(std::make_pair(f.ToString(), buf));
    *r = new StringWritableFile(buf);
    return Status::OK();
  }

  virtual Status NewRandomAccessFile(const Slice& f, RandomAccessFile** r) {
    std::string* buf = Find(f);
    if (buf == NULL) {
      *r = NULL;
      return Status::NotFound(Slice());
    } else {
      *r = new StringFile(buf);
      return Status::OK();
    }
  }

  virtual Status NewSequentialFile(const Slice& f, SequentialFile** r) {
    std::string* buf = Find(f);
    if (buf == NULL) {
      *r = NULL;
      return Status::NotFound(Slice());
    } else {
      *r = new StringFile(buf);
      return Status::OK();
    }
  }

  virtual Status GetFileSize(const Slice& f, uint64_t* s) {
    std::string* buf = Find(f);
    if (buf == NULL) {
      *s = 0;
      return Status::NotFound(Slice());
    } else {
      *s = buf->size();
      return Status::OK();
    }
  }

 private:
  typedef std::map<std::string, std::string*> FS;
  typedef FS::iterator FSIter;

  std::string* Find(const Slice& f) {
    FSIter iter = fs_.begin();
    for (; iter != fs_.end(); ++iter) {
      if (Slice(iter->first) == f) {
        return iter->second;
      }
    }
    return NULL;
  }

  FS fs_;
};

}  // anonymous namespace

class PlfsBfBench : PlfsIoBench {
 public:
  PlfsBfBench() : PlfsIoBench() {
    num_threads_ = 0;
    link_speed_ = 0;

    options_.verify_checksums = false;
    options_.paranoid_checks = false;

    block_buffer_ = new char[options_.block_size];
    env_ = new StringEnv;
  }

  ~PlfsBfBench() {
    delete[] block_buffer_;
    delete writer_;
    writer_ = NULL;
    delete reader_;
    reader_ = NULL;
    delete env_;
    env_ = NULL;
  }

  void LogAndApply() {
    DestroyDir(home_, options_);
    DoIt();
    RunQueries();
  }

  void RunQueries() {
    options_.allow_env_threads = false;
    options_.reader_pool = NULL;
    options_.env = env_;
    Status s = DirReader::Open(options_, home_, &reader_);
    ASSERT_OK(s) << "Cannot open dir";
    char tmp[30];
    fprintf(stderr, "Reading dir...\n");
    Slice key(tmp, options_.key_size);
    const int total_files = num_files_ << 20;
    uint64_t accumulated_seeks = 0;
    const uint64_t start = env_->NowMicros();
    for (int i = 0; i < total_files; i++) {
      std::string dummy_buf;
      const int fid = xxhash32(&i, sizeof(i), 0);
      snprintf(tmp, sizeof(tmp), "%08x-%08x-%08x", fid, fid, fid);
      s = reader_->ReadAll(key, &dummy_buf, block_buffer_, options_.block_size);
      ASSERT_OK(s) << "Cannot read";
      ASSERT_TRUE(dummy_buf.size() == options_.value_size);
      if (i % (1 << 18) == (1 << 18) - 1) {
        fprintf(stderr, "\r%.2f%%", 100.0 * (i + 1) / total_files);
      }
      const IoStats ios = reader_->GetIoStats();
      seeks_.Add(10.0 * (ios.data_ops - accumulated_seeks));
      accumulated_seeks = ios.data_ops;
    }
    fprintf(stderr, "\n");
    fprintf(stderr, "Done!\n");

    uint64_t dura = env_->NowMicros() - start;

    Report(dura);

    delete reader_;
    reader_ = NULL;
  }

  void Report(uint64_t dura) {
    const double k = 1000.0, ki = 1024.0;
    fprintf(stderr, "----------------------------------------\n");
    fprintf(stderr, "             Total Time: %.3f s\n", dura / k / k);
    fprintf(stderr, "          Avg Read Time: %.3f us (per file)\n",
            1.0 * dura / (num_files_ << 20));
    fprintf(stderr, " Avg Num Seeks Per Read: %.3f (per file)\n",
            seeks_.Average() / 10.0);
    fprintf(stderr, "              10%% Seeks: %.3f\n",
            seeks_.Percentile(10) / 10.0);
    fprintf(stderr, "              30%% Seeks: %.3f\n",
            seeks_.Percentile(30) / 10.0);
    fprintf(stderr, "              50%% Seeks: %.3f\n",
            seeks_.Percentile(50) / 10.0);
    fprintf(stderr, "              70%% Seeks: %.3f\n",
            seeks_.Percentile(70) / 10.0);
    fprintf(stderr, "              90%% Seeks: %.3f\n",
            seeks_.Percentile(90) / 10.0);
    fprintf(stderr, "              91%% Seeks: %.3f\n",
            seeks_.Percentile(91) / 10.0);
    fprintf(stderr, "              93%% Seeks: %.3f\n",
            seeks_.Percentile(93) / 10.0);
    fprintf(stderr, "              95%% Seeks: %.3f\n",
            seeks_.Percentile(95) / 10.0);
    fprintf(stderr, "              97%% Seeks: %.3f\n",
            seeks_.Percentile(97) / 10.0);
    fprintf(stderr, "              99%% Seeks: %.3f\n",
            seeks_.Percentile(99) / 10.0);
    const IoStats stats = reader_->GetIoStats();
    fprintf(stderr, "  Total Indexes Fetched: %.3f MB\n",
            1.0 * stats.index_bytes / ki / ki);
    fprintf(stderr, "     Total Data Fetched: %.3f TB\n",
            1.0 * stats.data_bytes / ki / ki / ki / ki);
    fprintf(stderr, "           Avg I/O size: %.3f KB\n",
            1.0 * stats.data_bytes / stats.data_ops / ki);
  }

 private:
  char* block_buffer_;
  DirReader* reader_;

  Histogram seeks_;
};

}  // namespace plfsio
}  // namespace pdlfs

#if defined(PDLFS_GFLAGS)
#include <gflags/gflags.h>
#endif
#if defined(PDLFS_GLOG)
#include <glog/logging.h>
#endif

static inline void BM_Usage() {
  fprintf(stderr, "Use --bench=io or --bench=bf to select a benchmark.\n");
}

static void BM_LogAndApply(int* argc, char*** argv) {
#if defined(PDLFS_GFLAGS)
  google::ParseCommandLineFlags(argc, argv, true);
#endif
#if defined(PDLFS_GLOG)
  google::InitGoogleLogging((*argv)[0]);
  google::InstallFailureSignalHandler();
#endif
  pdlfs::Slice bench_name;
  if (*argc > 1) {
    bench_name = pdlfs::Slice((*argv)[*argc - 1]);
  }
  if (*argc <= 1) {
    BM_Usage();
  } else if (bench_name == "--bench=io") {
    pdlfs::plfsio::PlfsIoBench bench;
    bench.LogAndApply();
  } else if (bench_name == "--bench=bf") {
    pdlfs::plfsio::PlfsBfBench bench;
    bench.LogAndApply();
  } else {
    BM_Usage();
  }
}

int main(int argc, char* argv[]) {
  pdlfs::Slice token;
  if (argc > 1) {
    token = pdlfs::Slice(argv[argc - 1]);
  }
  if (!token.starts_with("--bench")) {
    return pdlfs::test::RunAllTests(&argc, &argv);
  } else {
    BM_LogAndApply(&argc, &argv);
    return 0;
  }
}
