//  Copyright (c) 2011-present, Facebook, Inc.  All rights reserved.
//  This source code is licensed under both the GPLv2 (found in the
//  COPYING file in the root directory) and Apache 2.0 License
//  (found in the LICENSE.Apache file in the root directory).
//
// Copyright (c) 2011 The LevelDB Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file. See the AUTHORS file for names of contributors.

#include "db/compaction_picker_universal.h"
#ifndef ROCKSDB_LITE

#ifndef __STDC_FORMAT_MACROS
#define __STDC_FORMAT_MACROS
#endif

#include <inttypes.h>
#include <limits>
#include <numeric>
#include <queue>
#include <string>
#include <utility>
#include "db/column_family.h"
#include "db/map_builder.h"
#include "monitoring/statistics.h"
#include "util/c_style_callback.h"
#include "util/filename.h"
#include "util/log_buffer.h"
#include "util/random.h"
#include "util/string_util.h"
#include "util/sync_point.h"

namespace rocksdb {
namespace {
// Used in universal compaction when trivial move is enabled.
// This structure is used for the construction of min heap
// that contains the file meta data, the level of the file
// and the index of the file in that level

struct InputFileInfo {
  InputFileInfo() : f(nullptr), level(0), index(0) {}

  FileMetaData* f;
  size_t level;
  size_t index;
};

// Used in universal compaction when trivial move is enabled.
// This comparator is used for the construction of min heap
// based on the smallest key of the file.
struct SmallestKeyHeapComparator {
  explicit SmallestKeyHeapComparator(const Comparator* icmp) { icmp_ = icmp; }

  bool operator()(InputFileInfo i1, InputFileInfo i2) const {
    return (icmp_->Compare(i1.f->smallest.Encode(),
                           i2.f->smallest.Encode()) > 0);
  }

 private:
  const Comparator* icmp_;
};

typedef std::priority_queue<InputFileInfo, std::vector<InputFileInfo>,
                            SmallestKeyHeapComparator>
    SmallestKeyHeap;

// This function creates the heap that is used to find if the files are
// overlapping during universal compaction when the allow_trivial_move
// is set.
SmallestKeyHeap create_level_heap(Compaction* c, const Comparator* icmp) {
  SmallestKeyHeap smallest_key_priority_q =
      SmallestKeyHeap(SmallestKeyHeapComparator(icmp));

  InputFileInfo input_file;

  for (size_t l = 0; l < c->num_input_levels(); l++) {
    if (c->num_input_files(l) != 0) {
      if (l == 0 && c->start_level() == 0) {
        for (size_t i = 0; i < c->num_input_files(0); i++) {
          input_file.f = c->input(0, i);
          input_file.level = 0;
          input_file.index = i;
          smallest_key_priority_q.push(std::move(input_file));
        }
      } else {
        input_file.f = c->input(l, 0);
        input_file.level = l;
        input_file.index = 0;
        smallest_key_priority_q.push(std::move(input_file));
      }
    }
  }
  return smallest_key_priority_q;
}

#ifndef NDEBUG
// smallest_seqno and largest_seqno are set iff. `files` is not empty.
void GetSmallestLargestSeqno(const std::vector<FileMetaData*>& files,
                             SequenceNumber* smallest_seqno,
                             SequenceNumber* largest_seqno) {
  bool is_first = true;
  for (FileMetaData* f : files) {
    assert(f->fd.smallest_seqno <= f->fd.largest_seqno);
    if (is_first) {
      is_first = false;
      *smallest_seqno = f->fd.smallest_seqno;
      *largest_seqno = f->fd.largest_seqno;
    } else {
      if (f->fd.smallest_seqno < *smallest_seqno) {
        *smallest_seqno = f->fd.smallest_seqno;
      }
      if (f->fd.largest_seqno > *largest_seqno) {
        *largest_seqno = f->fd.largest_seqno;
      }
    }
  }
}
#endif
}  // namespace

// Algorithm that checks to see if there are any overlapping
// files in the input
bool UniversalCompactionPicker::IsInputFilesNonOverlapping(Compaction* c) {
  int first_iter = 1;

  InputFileInfo prev, curr, next;

  SmallestKeyHeap smallest_key_priority_q = create_level_heap(c, icmp_);
  if (smallest_key_priority_q.size() <= 1) {
    return true;
  }

  while (!smallest_key_priority_q.empty()) {
    curr = smallest_key_priority_q.top();
    smallest_key_priority_q.pop();

    if (first_iter) {
      prev = curr;
      first_iter = 0;
    } else {
      if (icmp_->Compare(prev.f->largest.Encode(),
                         curr.f->smallest.Encode()) >= 0) {
        // found overlapping files, return false
        return false;
      }
      assert(icmp_->Compare(curr.f->largest.Encode(),
                            prev.f->largest.Encode()) > 0);
      prev = curr;
    }

    next.f = nullptr;

    if (c->level(curr.level) != 0 &&
        curr.index < c->num_input_files(curr.level) - 1) {
      next.f = c->input(curr.level, curr.index + 1);
      next.level = curr.level;
      next.index = curr.index + 1;
    }

    if (next.f) {
      smallest_key_priority_q.push(std::move(next));
    }
  }
  return true;
}

bool UniversalCompactionPicker::NeedsCompaction(
    const VersionStorageInfo* vstorage) const {
  const int kLevel0 = 0;
  if (vstorage->CompactionScore(kLevel0) >= 1) {
    return true;
  }
  if (!vstorage->FilesMarkedForCompaction().empty()) {
    return true;
  }
  if (vstorage->has_space_amplification()) {
    return true;
  }
  return false;
}

void UniversalCompactionPicker::SortedRun::Dump(char* out_buf,
                                                size_t out_buf_size,
                                                bool print_path) const {
  if (level == 0) {
    assert(file != nullptr);
    if (file->fd.GetPathId() == 0 || !print_path) {
      snprintf(out_buf, out_buf_size, "file %" PRIu64, file->fd.GetNumber());
    } else {
      snprintf(out_buf, out_buf_size,
               "file %" PRIu64
               "(path "
               "%" PRIu32 ")",
               file->fd.GetNumber(), file->fd.GetPathId());
    }
  } else {
    snprintf(out_buf, out_buf_size, "level %d", level);
  }
}

void UniversalCompactionPicker::SortedRun::DumpSizeInfo(
    char* out_buf, size_t out_buf_size, size_t sorted_run_count) const {
  if (level == 0) {
    assert(file != nullptr);
    snprintf(out_buf, out_buf_size,
             "file %" PRIu64 "[%" ROCKSDB_PRIszt
             "] "
             "with size %" PRIu64 " (compensated size %" PRIu64 ")",
             file->fd.GetNumber(), sorted_run_count, file->fd.GetFileSize(),
             file->compensated_file_size);
  } else {
    snprintf(out_buf, out_buf_size,
             "level %d[%" ROCKSDB_PRIszt
             "] "
             "with size %" PRIu64 " (compensated size %" PRIu64 ")",
             level, sorted_run_count, size, compensated_file_size);
  }
}

static size_t GetFilesSize(const FileMetaData* f, uint64_t file_number,
                           const VersionStorageInfo& vstorage) {
  auto& depend_files = vstorage.depend_files();
  if (f == nullptr) {
    auto find = depend_files.find(file_number);
    if (find == depend_files.end()) {
      // TODO log error
      return 0;
    }
    f = find->second;
  } else {
    assert(file_number == uint64_t(-1));
  }
  uint64_t file_size = f->fd.GetFileSize();
  if (f->sst_purpose != 0) {
    for (auto depend : f->sst_depend) {
      file_size += GetFilesSize(nullptr, depend, vstorage);
    }
  }
  return file_size;
}

std::vector<UniversalCompactionPicker::SortedRun>
UniversalCompactionPicker::CalculateSortedRuns(
    const VersionStorageInfo& vstorage, const ImmutableCFOptions& /*ioptions*/,
    const MutableCFOptions& mutable_cf_options) {
  std::vector<UniversalCompactionPicker::SortedRun> ret;
  for (FileMetaData* f : vstorage.LevelFiles(0)) {
    ret.emplace_back(0, f, GetFilesSize(f, uint64_t(-1), vstorage),
                     f->compensated_file_size, f->being_compacted);
  }
  for (int level = 1; level < vstorage.num_levels(); level++) {
    uint64_t total_compensated_size = 0U;
    uint64_t total_size = 0U;
    bool being_compacted = false;
    bool is_first = true;
    for (FileMetaData* f : vstorage.LevelFiles(level)) {
      total_compensated_size += f->compensated_file_size;
      total_size += GetFilesSize(f, uint64_t(-1), vstorage);
      if (mutable_cf_options.compaction_options_universal.allow_trivial_move) {
        if (f->being_compacted) {
          being_compacted = f->being_compacted;
        }
      } else {
        // Compaction always includes all files for a non-zero level, so for a
        // non-zero level, all the files should share the same being_compacted
        // value.
        // This assumption is only valid when
        // mutable_cf_options.compaction_options_universal.allow_trivial_move is
        // false
        assert(is_first || f->being_compacted == being_compacted);
      }
      if (is_first) {
        being_compacted = f->being_compacted;
        is_first = false;
      }
    }
    if (total_compensated_size > 0) {
      ret.emplace_back(level, nullptr, total_size, total_compensated_size,
                       being_compacted);
    }
  }
  return ret;
}

// Universal style of compaction. Pick files that are contiguous in
// time-range to compact.
Compaction* UniversalCompactionPicker::PickCompaction(
    const std::string& cf_name, const MutableCFOptions& mutable_cf_options,
    VersionStorageInfo* vstorage, LogBuffer* log_buffer) {
  const int kLevel0 = 0;
  double score = vstorage->CompactionScore(kLevel0);
  std::vector<SortedRun> sorted_runs =
      CalculateSortedRuns(*vstorage, ioptions_, mutable_cf_options);

  if (sorted_runs.size() == 0 ||
      (vstorage->FilesMarkedForCompaction().empty() &&
       !vstorage->has_space_amplification() &&
       sorted_runs.size() < (unsigned int)mutable_cf_options
                                .level0_file_num_compaction_trigger)) {
    ROCKS_LOG_BUFFER(log_buffer, "[%s] Universal: nothing to do\n",
                     cf_name.c_str());
    TEST_SYNC_POINT_CALLBACK("UniversalCompactionPicker::PickCompaction:Return",
                             nullptr);
    return nullptr;
  }
  VersionStorageInfo::LevelSummaryStorage tmp;
  ROCKS_LOG_BUFFER_MAX_SZ(
      log_buffer, 3072,
      "[%s] Universal: sorted runs files(%" ROCKSDB_PRIszt "): %s\n",
      cf_name.c_str(), sorted_runs.size(), vstorage->LevelSummary(&tmp));

  // Check for size amplification first.
  Compaction* c = nullptr;
  if (vstorage->has_space_amplification() ||
      sorted_runs.size() >=
          static_cast<size_t>(
              mutable_cf_options.level0_file_num_compaction_trigger)) {
    if (mutable_cf_options.enable_lazy_compaction) {
      bool has_map_compaction = false;
      for (auto cip : compactions_in_progress_) {
        if (cip->compaction_purpose() == kMapSst) {
          has_map_compaction = true;
          break;
        }
      }
      size_t reduce_sorted_run_target =
          mutable_cf_options.level0_file_num_compaction_trigger +
          ioptions_.num_levels - 1;
      if (has_map_compaction ||
          (c = PickTrivialMoveCompaction(cf_name, mutable_cf_options, vstorage,
                                         log_buffer)) != nullptr) {
        reduce_sorted_run_target = size_t(-1);
      } else if (table_cache_ != nullptr && sorted_runs.size() > 1 &&
                 sorted_runs.size() <= reduce_sorted_run_target) {
        size_t level_read_amp_count = 0;
        for (auto& sr : sorted_runs) {
          FileMetaData* f;
          if (sr.level > 0) {
            if (!vstorage->has_space_amplification(sr.level)) {
              continue;
            }
            auto& level_files = vstorage->LevelFiles(sr.level);
            if (level_files.size() > 1) {
              // PickCompositeCompaction for rebuild map
              reduce_sorted_run_target = size_t(-1);
              break;
            }
            f = level_files.front();
          } else {
            if (sr.file->sst_purpose != kMapSst) {
              continue;
            }
            f = sr.file;
          }
          std::shared_ptr<const TableProperties> porps;
          auto s = table_cache_->GetTableProperties(
              env_options_, *icmp_, f->fd, &porps,
              mutable_cf_options.prefix_extractor.get(), false);
          if (s.ok()) {
            size_t read_amp = GetSstReadAmp(porps->user_collected_properties);
            if (read_amp > 1) {
              level_read_amp_count += read_amp;
            }
          }
        }
        if (level_read_amp_count < reduce_sorted_run_target) {
          reduce_sorted_run_target = std::max<size_t>(
              mutable_cf_options.level0_file_num_compaction_trigger,
              sorted_runs.size() - 1);
        }
      }
      if (sorted_runs.size() > reduce_sorted_run_target &&
          (c = PickCompactionToReduceSortedRuns(
               cf_name, mutable_cf_options, vstorage, score, &sorted_runs,
               reduce_sorted_run_target, log_buffer)) != nullptr) {
        ROCKS_LOG_BUFFER(log_buffer,
                         "[%s] Universal: compacting for lazy compaction\n",
                         cf_name.c_str());
      }
    } else if ((c = PickCompactionToReduceSizeAmp(cf_name, mutable_cf_options,
                                                  vstorage, score, sorted_runs,
                                                  log_buffer)) != nullptr) {
      ROCKS_LOG_BUFFER(log_buffer, "[%s] Universal: compacting for size amp\n",
                       cf_name.c_str());
    } else {
      // Size amplification is within limits. Try reducing read
      // amplification while maintaining file size ratios.
      unsigned int ratio =
          mutable_cf_options.compaction_options_universal.size_ratio;

      if ((c = PickCompactionToReduceSortedRunsOld(
               cf_name, mutable_cf_options, vstorage, score, ratio, UINT_MAX,
               sorted_runs, log_buffer)) != nullptr) {
        ROCKS_LOG_BUFFER(log_buffer,
                         "[%s] Universal: compacting for size ratio\n",
                         cf_name.c_str());
      } else {
        // Size amplification and file size ratios are within configured limits.
        // If max read amplification is exceeding configured limits, then force
        // compaction without looking at filesize ratios and try to reduce
        // the number of files to fewer than level0_file_num_compaction_trigger.
        // This is guaranteed by NeedsCompaction()
        assert(sorted_runs.size() >=
               static_cast<size_t>(
                   mutable_cf_options.level0_file_num_compaction_trigger));
        // Get the total number of sorted runs that are not being compacted
        int num_sr_not_compacted = 0;
        for (size_t i = 0; i < sorted_runs.size(); i++) {
          if (sorted_runs[i].being_compacted == false) {
            num_sr_not_compacted++;
          }
        }

        // The number of sorted runs that are not being compacted is greater
        // than the maximum allowed number of sorted runs
        if (num_sr_not_compacted >
            mutable_cf_options.level0_file_num_compaction_trigger) {
          unsigned int num_files =
              num_sr_not_compacted -
              mutable_cf_options.level0_file_num_compaction_trigger + 1;
          if ((c = PickCompactionToReduceSortedRunsOld(
                   cf_name, mutable_cf_options, vstorage, score, UINT_MAX,
                   num_files, sorted_runs, log_buffer)) != nullptr) {
            ROCKS_LOG_BUFFER(log_buffer,
                             "[%s] Universal: compacting for file num -- %u\n",
                             cf_name.c_str(), num_files);
          }
        }
      }
    }
  }
  if (c == nullptr && table_cache_ != nullptr) {
    c = PickCompositeCompaction(cf_name, mutable_cf_options, vstorage,
                                sorted_runs, log_buffer);
  }

  if (c == nullptr) {
    if ((c = PickDeleteTriggeredCompaction(cf_name, mutable_cf_options,
                                           vstorage, score, sorted_runs,
                                           log_buffer)) != nullptr) {
      ROCKS_LOG_BUFFER(log_buffer,
                       "[%s] Universal: delete triggered compaction\n",
                       cf_name.c_str());
    }
  }

  if (c == nullptr) {
    TEST_SYNC_POINT_CALLBACK("UniversalCompactionPicker::PickCompaction:Return",
                             nullptr);
    return nullptr;
  }

  bool allow_trivial_move =
      mutable_cf_options.compaction_options_universal.allow_trivial_move;
  if (c->compaction_reason() != CompactionReason::kTrivialMoveLevel &&
      allow_trivial_move) {
    // check level has map or link sst
    for (auto& level_files : *c->inputs()) {
      if (vstorage->has_space_amplification(level_files.level)) {
        allow_trivial_move = false;
        break;
      }
    }
  }
  if (allow_trivial_move) {
    c->set_is_trivial_move(IsInputFilesNonOverlapping(c));
    assert(c->compaction_reason() != CompactionReason::kTrivialMoveLevel ||
           c->is_trivial_move());
  }

// validate that all the chosen files of L0 are non overlapping in time
#ifndef NDEBUG
  SequenceNumber prev_smallest_seqno = 0U;
  bool is_first = true;

  size_t level_index = 0U;
  if (c->start_level() == 0) {
    for (auto f : *c->inputs(0)) {
      assert(f->fd.smallest_seqno <= f->fd.largest_seqno);
      if (is_first) {
        is_first = false;
      }
      prev_smallest_seqno = f->fd.smallest_seqno;
    }
    level_index = 1U;
  }
  for (; level_index < c->num_input_levels(); level_index++) {
    if (c->num_input_files(level_index) != 0) {
      SequenceNumber smallest_seqno = 0U;
      SequenceNumber largest_seqno = 0U;
      GetSmallestLargestSeqno(*(c->inputs(level_index)), &smallest_seqno,
                              &largest_seqno);
      if (is_first) {
        is_first = false;
      } else if (prev_smallest_seqno > 0) {
        // A level is considered as the bottommost level if there are
        // no files in higher levels or if files in higher levels do
        // not overlap with the files being compacted. Sequence numbers
        // of files in bottommost level can be set to 0 to help
        // compression. As a result, the following assert may not hold
        // if the prev_smallest_seqno is 0.
        assert(prev_smallest_seqno > largest_seqno);
      }
      prev_smallest_seqno = smallest_seqno;
    }
  }
#endif
  // update statistics
  MeasureTime(ioptions_.statistics, NUM_FILES_IN_SINGLE_COMPACTION,
              c->inputs(0)->size());

  RegisterCompaction(c);
  vstorage->ComputeCompactionScore(ioptions_, mutable_cf_options);

  TEST_SYNC_POINT_CALLBACK("UniversalCompactionPicker::PickCompaction:Return",
                           c);
  return c;
}

Compaction* UniversalCompactionPicker::CompactRange(
    const std::string& cf_name, const MutableCFOptions& mutable_cf_options,
    VersionStorageInfo* vstorage, int input_level, int output_level,
    uint32_t output_path_id, uint32_t max_subcompactions,
    const InternalKey* begin, const InternalKey* end,
    InternalKey** compaction_end, bool* manual_conflict,
    const std::unordered_set<uint64_t>* files_being_compact,
    bool enable_lazy_compaction) {
  if (input_level == ColumnFamilyData::kCompactAllLevels &&
      enable_lazy_compaction) {
    auto hit_sst = [&](const FileMetaData* f) {
      if (files_being_compact->count(f->fd.GetNumber()) > 0) {
        return true;
      }
      auto& depend_files = vstorage->depend_files();
      for (auto file_number : f->sst_depend) {
        if (files_being_compact->count(file_number) > 0) {
          return true;
        }
        auto find = depend_files.find(file_number);
        if (find == depend_files.end()) {
          // TODO: log error
          continue;
        }
        for (auto file_number_depend : find->second->sst_depend) {
          if (files_being_compact->count(file_number_depend) > 0) {
            return true;
          }
        };
      }
      return false;
    };
    size_t hit_count = 0;
    int new_input_level = -1;
    for (int level = 0; level < vstorage->num_levels(); ++level) {
      for (auto f : vstorage->LevelFiles(level)) {
        if (hit_sst(f)) {
          ++hit_count;
          new_input_level = level;
          break;
        }
      }
    }
    if (hit_count == 0) {
      return nullptr;
    }
    if (hit_count == 1) {
      input_level = new_input_level;
    }
  }
  if (input_level == ColumnFamilyData::kCompactAllLevels) {
    assert(ioptions_.compaction_style == kCompactionStyleUniversal);

    // Universal compaction with more than one level always compacts all the
    // files together to the last level.
    assert(vstorage->num_levels() > 1);
    // DBImpl::CompactRange() set output level to be the last level
    if (ioptions_.allow_ingest_behind) {
      assert(output_level == vstorage->num_levels() - 2);
    } else {
      assert(output_level == vstorage->num_levels() - 1);
    }
    // DBImpl::RunManualCompaction will make full range for universal compaction
    assert(begin == nullptr);
    assert(end == nullptr);

    int start_level = 0;
    for (; start_level < vstorage->num_levels() &&
           vstorage->NumLevelFiles(start_level) == 0;
         start_level++) {
    }
    if (start_level == vstorage->num_levels()) {
      return nullptr;
    }

    if ((start_level == 0) && (!level0_compactions_in_progress_.empty())) {
      *manual_conflict = true;
      // Only one level 0 compaction allowed
      return nullptr;
    }

    std::vector<CompactionInputFiles> inputs(vstorage->num_levels() -
                                             start_level);
    for (int level = start_level; level < vstorage->num_levels(); level++) {
      inputs[level - start_level].level = level;
      auto& files = inputs[level - start_level].files;
      for (FileMetaData* f : vstorage->LevelFiles(level)) {
        files.push_back(f);
      }
      if (AreFilesInCompaction(files)) {
        *manual_conflict = true;
        return nullptr;
      }
    }

    // 2 non-exclusive manual compactions could run at the same time producing
    // overlaping outputs in the same level.
    if (FilesRangeOverlapWithCompaction(inputs, output_level)) {
      // This compaction output could potentially conflict with the output
      // of a currently running compaction, we cannot run it.
      *manual_conflict = true;
      return nullptr;
    }

    CompactionParams params(vstorage, ioptions_, mutable_cf_options);
    params.inputs = std::move(inputs);
    params.output_level = output_level;
    params.target_file_size = MaxFileSizeForLevel(
        mutable_cf_options, output_level, ioptions_.compaction_style);
    params.max_compaction_bytes = LLONG_MAX;
    params.output_path_id = output_path_id;
    params.compression = GetCompressionType(
        ioptions_, vstorage, mutable_cf_options, output_level, 1);
    params.compression_opts =
        GetCompressionOptions(ioptions_, vstorage, output_level);
    params.max_subcompactions = max_subcompactions;
    params.manual_compaction = true;
    if (enable_lazy_compaction) {
      params.max_subcompactions = 1;
      params.compaction_purpose = kMapSst;
    } else {
      *compaction_end = nullptr;
    }

    Compaction* c = new Compaction(std::move(params));
    RegisterCompaction(c);
    return c;
  }

  if (!enable_lazy_compaction) {
    return CompactionPicker::CompactRange(
        cf_name, mutable_cf_options, vstorage, input_level, output_level,
        output_path_id, max_subcompactions, begin, end, compaction_end,
        manual_conflict, files_being_compact, enable_lazy_compaction);
  }
  LogBuffer log_buffer(InfoLogLevel::INFO_LEVEL, ioptions_.info_log);
  auto c = PickRangeCompaction(cf_name, mutable_cf_options, vstorage,
                               input_level, begin, end, files_being_compact,
                               manual_conflict, &log_buffer);
  log_buffer.FlushBufferToLog();
  return c;
}

uint32_t UniversalCompactionPicker::GetPathId(
    const ImmutableCFOptions& ioptions,
    const MutableCFOptions& mutable_cf_options, uint64_t file_size) {
  // Two conditions need to be satisfied:
  // (1) the target path needs to be able to hold the file's size
  // (2) Total size left in this and previous paths need to be not
  //     smaller than expected future file size before this new file is
  //     compacted, which is estimated based on size_ratio.
  // For example, if now we are compacting files of size (1, 1, 2, 4, 8),
  // we will make sure the target file, probably with size of 16, will be
  // placed in a path so that eventually when new files are generated and
  // compacted to (1, 1, 2, 4, 8, 16), all those files can be stored in or
  // before the path we chose.
  //
  // TODO(sdong): now the case of multiple column families is not
  // considered in this algorithm. So the target size can be violated in
  // that case. We need to improve it.
  uint64_t accumulated_size = 0;
  uint64_t future_size =
      file_size *
      (100 - mutable_cf_options.compaction_options_universal.size_ratio) / 100;
  uint32_t p = 0;
  assert(!ioptions.cf_paths.empty());
  for (; p < ioptions.cf_paths.size() - 1; p++) {
    uint64_t target_size = ioptions.cf_paths[p].target_size;
    if (target_size > file_size &&
        accumulated_size + (target_size - file_size) > future_size) {
      return p;
    }
    accumulated_size += target_size;
  }
  return p;
}

namespace {

struct SortedRunGroup {
  size_t start;
  size_t count;
  double ratio;
};

double GenSortedRunGroup(const std::vector<double>& sr, size_t group,
                         std::vector<SortedRunGroup>* output_group) {
  auto Q = [](std::vector<double>::const_iterator b,
              std::vector<double>::const_iterator e, size_t g) {
    double S = std::accumulate(b, e, 0.0);
    // sum of [q, q^2, q^3, ... , q^n]
    auto F = [](double q, size_t n) {
      return (std::pow(q, n + 1) - q) / (q - 1);
    };
    // let S = ∑q^i, i in <1..n>, seek q
    double q = std::pow(S, 1.0 / g);
    if (S <= g + 1) {
      q = 1;
    } else {
      // Newton-Raphson method
      for (size_t c = 0; c < 8; ++c) {
        double Fp = q, q_k = q;
        for (size_t k = 2; k <= g; ++k) {
          Fp += k * (q_k *= q);
        }
        q -= (F(q, g) - S) / Fp;
      }
    }
    return q;
  };
  auto& o = *output_group;
  o.resize(group);
  double ret_q = Q(sr.begin(), sr.end(), group);
  size_t sr_size = sr.size();
  size_t g = group;
  double q = ret_q;
  for (size_t i = g - 1; q > 1 && i > 0; --i) {
    size_t e = g - i;
    double new_q = Q(sr.begin(), sr.begin() + sr_size - e, g - e);
    if (new_q < q) {
      for (size_t j = i; j < g; ++j) {
        size_t start = j + sr_size - g;
        o[j].ratio = sr[start];
        o[j].count = 1;
        o[j].start = start;
      }
      sr_size -= e;
      g -= e;
      q = new_q;
    }
  }
  // Standard Deviation pattern matching
  double sr_acc = sr[sr_size - 1];
  double q_acc = std::pow(q, g);
  int q_i = int(g) - 1;
  o[q_i].ratio = sr_acc;
  o[0].start = 0;
  for (int i = int(sr_size) - 2; i >= 0; --i) {
    double new_acc = sr_acc + sr[i];
    if ((i < q_i || sr_acc > q_acc ||
         std::abs(new_acc - q_acc) > std::abs(sr_acc - q_acc)) &&
        q_i > 0) {
      o[q_i].start = i + 1;
      q_acc += std::pow(q, q_i--);
      o[q_i].ratio = 0;
    }
    sr_acc = new_acc;
    o[q_i].ratio += sr[i];
  }
  for (size_t i = 1; i < g; ++i) {
    o[i - 1].count = o[i].start - o[i - 1].start;
  }
  o[g - 1].count = sr_size - o[g - 1].start;
  return ret_q;
}

}  // namespace

//
// Consider compaction files based on their size differences with
// the next file in time order.
//
Compaction* UniversalCompactionPicker::PickCompactionToReduceSortedRunsOld(
    const std::string& cf_name, const MutableCFOptions& mutable_cf_options,
    VersionStorageInfo* vstorage, double score, unsigned int ratio,
    unsigned int max_number_of_files_to_compact,
    const std::vector<SortedRun>& sorted_runs, LogBuffer* log_buffer) {
  unsigned int min_merge_width =
      mutable_cf_options.compaction_options_universal.min_merge_width;
  unsigned int max_merge_width =
      mutable_cf_options.compaction_options_universal.max_merge_width;

  const SortedRun* sr = nullptr;
  bool done = false;
  size_t start_index = 0;
  unsigned int candidate_count = 0;

  unsigned int max_files_to_compact =
      std::min(max_merge_width, max_number_of_files_to_compact);
  min_merge_width = std::max(min_merge_width, 2U);

  // Caller checks the size before executing this function. This invariant is
  // important because otherwise we may have a possible integer underflow when
  // dealing with unsigned types.
  assert(sorted_runs.size() > 0);

  // Considers a candidate file only if it is smaller than the
  // total size accumulated so far.
  for (size_t loop = 0; loop < sorted_runs.size(); loop++) {
    candidate_count = 0;

    // Skip files that are already being compacted
    for (sr = nullptr; loop < sorted_runs.size(); loop++) {
      sr = &sorted_runs[loop];

      if (!sr->being_compacted) {
        candidate_count = 1;
        break;
      }
      char file_num_buf[kFormatFileNumberBufSize];
      sr->Dump(file_num_buf, sizeof(file_num_buf));
      ROCKS_LOG_BUFFER(log_buffer,
                       "[%s] Universal: %s"
                       "[%d] being compacted, skipping",
                       cf_name.c_str(), file_num_buf, loop);

      sr = nullptr;
    }

    // This file is not being compacted. Consider it as the
    // first candidate to be compacted.
    uint64_t candidate_size = sr != nullptr ? sr->compensated_file_size : 0;
    if (sr != nullptr) {
      char file_num_buf[kFormatFileNumberBufSize];
      sr->Dump(file_num_buf, sizeof(file_num_buf), true);
      ROCKS_LOG_BUFFER(log_buffer, "[%s] Universal: Possible candidate %s[%d].",
                       cf_name.c_str(), file_num_buf, loop);
    }

    // Check if the succeeding files need compaction.
    for (size_t i = loop + 1;
         candidate_count < max_files_to_compact && i < sorted_runs.size();
         i++) {
      const SortedRun* succeeding_sr = &sorted_runs[i];
      if (succeeding_sr->being_compacted) {
        break;
      }
      // Pick files if the total/last candidate file size (increased by the
      // specified ratio) is still larger than the next candidate file.
      // candidate_size is the total size of files picked so far with the
      // default kCompactionStopStyleTotalSize; with
      // kCompactionStopStyleSimilarSize, it's simply the size of the last
      // picked file.
      double sz = candidate_size * (100.0 + ratio) / 100.0;
      if (sz < static_cast<double>(succeeding_sr->size)) {
        break;
      }
      if (mutable_cf_options.compaction_options_universal.stop_style ==
          kCompactionStopStyleSimilarSize) {
        // Similar-size stopping rule: also check the last picked file isn't
        // far larger than the next candidate file.
        sz = (succeeding_sr->size * (100.0 + ratio)) / 100.0;
        if (sz < static_cast<double>(candidate_size)) {
          // If the small file we've encountered begins a run of similar-size
          // files, we'll pick them up on a future iteration of the outer
          // loop. If it's some lonely straggler, it'll eventually get picked
          // by the last-resort read amp strategy which disregards size ratios.
          break;
        }
        candidate_size = succeeding_sr->compensated_file_size;
      } else {  // default kCompactionStopStyleTotalSize
        candidate_size += succeeding_sr->compensated_file_size;
      }
      candidate_count++;
    }

    // Found a series of consecutive files that need compaction.
    if (candidate_count >= (unsigned int)min_merge_width) {
      start_index = loop;
      done = true;
      break;
    } else {
      for (size_t i = loop;
           i < loop + candidate_count && i < sorted_runs.size(); i++) {
        const SortedRun* skipping_sr = &sorted_runs[i];
        char file_num_buf[256];
        skipping_sr->DumpSizeInfo(file_num_buf, sizeof(file_num_buf), loop);
        ROCKS_LOG_BUFFER(log_buffer, "[%s] Universal: Skipping %s",
                         cf_name.c_str(), file_num_buf);
      }
    }
  }
  if (!done || candidate_count <= 1) {
    return nullptr;
  }
  size_t first_index_after = start_index + candidate_count;
  // Compression is enabled if files compacted earlier already reached
  // size ratio of compression.
  bool enable_compression = true;
  int ratio_to_compress =
      mutable_cf_options.compaction_options_universal.compression_size_percent;
  if (ratio_to_compress >= 0) {
    uint64_t total_size = 0;
    for (auto& sorted_run : sorted_runs) {
      total_size += sorted_run.compensated_file_size;
    }

    uint64_t older_file_size = 0;
    for (size_t i = sorted_runs.size() - 1; i >= first_index_after; i--) {
      older_file_size += sorted_runs[i].size;
      if (older_file_size * 100L >= total_size * (long)ratio_to_compress) {
        enable_compression = false;
        break;
      }
    }
  }

  uint64_t estimated_total_size = 0;
  for (unsigned int i = 0; i < first_index_after; i++) {
    estimated_total_size += sorted_runs[i].size;
  }
  uint32_t path_id =
      GetPathId(ioptions_, mutable_cf_options, estimated_total_size);
  int start_level = sorted_runs[start_index].level;
  int output_level;
  if (first_index_after == sorted_runs.size()) {
    output_level = vstorage->num_levels() - 1;
  } else if (sorted_runs[first_index_after].level == 0) {
    output_level = 0;
  } else {
    output_level = sorted_runs[first_index_after].level - 1;
  }

  // last level is reserved for the files ingested behind
  if (ioptions_.allow_ingest_behind &&
      (output_level == vstorage->num_levels() - 1)) {
    assert(output_level > 1);
    output_level--;
  }

  std::vector<CompactionInputFiles> inputs(vstorage->num_levels());
  for (size_t i = 0; i < inputs.size(); ++i) {
    inputs[i].level = start_level + static_cast<int>(i);
  }
  for (size_t i = start_index; i < first_index_after; i++) {
    auto& picking_sr = sorted_runs[i];
    if (picking_sr.level == 0) {
      FileMetaData* picking_file = picking_sr.file;
      inputs[0].files.push_back(picking_file);
    } else {
      auto& files = inputs[picking_sr.level - start_level].files;
      for (auto* f : vstorage->LevelFiles(picking_sr.level)) {
        files.push_back(f);
      }
    }
    char file_num_buf[256];
    picking_sr.DumpSizeInfo(file_num_buf, sizeof(file_num_buf), i);
    ROCKS_LOG_BUFFER(log_buffer, "[%s] Universal: Picking %s", cf_name.c_str(),
                     file_num_buf);
  }

  CompactionReason compaction_reason;
  if (max_number_of_files_to_compact == UINT_MAX) {
    compaction_reason = CompactionReason::kUniversalSizeRatio;
  } else {
    compaction_reason = CompactionReason::kUniversalSortedRunNum;
  }
  CompactionParams params(vstorage, ioptions_, mutable_cf_options);
  params.inputs = std::move(inputs);
  params.output_level = output_level;
  params.target_file_size = MaxFileSizeForLevel(
      mutable_cf_options, output_level, kCompactionStyleUniversal);
  params.max_compaction_bytes = LLONG_MAX;
  params.output_path_id = path_id;
  params.compression =
      GetCompressionType(ioptions_, vstorage, mutable_cf_options, output_level,
                         1, enable_compression);
  params.compression_opts = GetCompressionOptions(
      ioptions_, vstorage, output_level, enable_compression);
  params.score = score;
  params.compaction_reason = compaction_reason;

  return new Compaction(std::move(params));
}

// Look at overall size amplification. If size amplification
// exceeeds the configured value, then do a compaction
// of the candidate files all the way upto the earliest
// base file (overrides configured values of file-size ratios,
// min_merge_width and max_merge_width).
//
Compaction* UniversalCompactionPicker::PickCompactionToReduceSizeAmp(
    const std::string& cf_name, const MutableCFOptions& mutable_cf_options,
    VersionStorageInfo* vstorage, double score,
    const std::vector<SortedRun>& sorted_runs, LogBuffer* log_buffer) {
  // percentage flexibility while reducing size amplification
  uint64_t ratio = mutable_cf_options.compaction_options_universal
                       .max_size_amplification_percent;

  unsigned int candidate_count = 0;
  uint64_t candidate_size = 0;
  size_t start_index = 0;
  const SortedRun* sr = nullptr;

  if (sorted_runs.back().being_compacted) {
    return nullptr;
  }

  // Skip files that are already being compacted
  for (size_t loop = 0; loop < sorted_runs.size() - 1; loop++) {
    sr = &sorted_runs[loop];
    if (!sr->being_compacted) {
      start_index = loop;  // Consider this as the first candidate.
      break;
    }
    char file_num_buf[kFormatFileNumberBufSize];
    sr->Dump(file_num_buf, sizeof(file_num_buf), true);
    ROCKS_LOG_BUFFER(log_buffer, "[%s] Universal: skipping %s[%d] compacted %s",
                     cf_name.c_str(), file_num_buf, loop,
                     " cannot be a candidate to reduce size amp.\n");
    sr = nullptr;
  }

  if (sr == nullptr) {
    return nullptr;  // no candidate files
  }
  {
    char file_num_buf[kFormatFileNumberBufSize];
    sr->Dump(file_num_buf, sizeof(file_num_buf), true);
    ROCKS_LOG_BUFFER(
        log_buffer,
        "[%s] Universal: First candidate %s[%" ROCKSDB_PRIszt "] %s",
        cf_name.c_str(), file_num_buf, start_index, " to reduce size amp.\n");
  }

  // keep adding up all the remaining files
  for (size_t loop = start_index; loop < sorted_runs.size() - 1; loop++) {
    sr = &sorted_runs[loop];
    if (sr->being_compacted) {
      char file_num_buf[kFormatFileNumberBufSize];
      sr->Dump(file_num_buf, sizeof(file_num_buf), true);
      ROCKS_LOG_BUFFER(
          log_buffer, "[%s] Universal: Possible candidate %s[%d] %s",
          cf_name.c_str(), file_num_buf, start_index,
          " is already being compacted. No size amp reduction possible.\n");
      return nullptr;
    }
    candidate_size += sr->compensated_file_size;
    candidate_count++;
  }
  if (candidate_count == 0) {
    return nullptr;
  }

  // size of earliest file
  uint64_t earliest_file_size = sorted_runs.back().size;

  // size amplification = percentage of additional size
  if (candidate_size * 100 < ratio * earliest_file_size) {
    ROCKS_LOG_BUFFER(
        log_buffer,
        "[%s] Universal: size amp not needed. newer-files-total-size %" PRIu64
        " earliest-file-size %" PRIu64,
        cf_name.c_str(), candidate_size, earliest_file_size);
    return nullptr;
  } else {
    ROCKS_LOG_BUFFER(
        log_buffer,
        "[%s] Universal: size amp needed. newer-files-total-size %" PRIu64
        " earliest-file-size %" PRIu64,
        cf_name.c_str(), candidate_size, earliest_file_size);
  }
  assert(start_index < sorted_runs.size() - 1);

  // Estimate total file size
  uint64_t estimated_total_size = 0;
  for (size_t loop = start_index; loop < sorted_runs.size(); loop++) {
    estimated_total_size += sorted_runs[loop].size;
  }
  uint32_t path_id =
      GetPathId(ioptions_, mutable_cf_options, estimated_total_size);
  int start_level = sorted_runs[start_index].level;

  std::vector<CompactionInputFiles> inputs(vstorage->num_levels());
  for (size_t i = 0; i < inputs.size(); ++i) {
    inputs[i].level = start_level + static_cast<int>(i);
  }
  // We always compact all the files, so always compress.
  for (size_t loop = start_index; loop < sorted_runs.size(); loop++) {
    auto& picking_sr = sorted_runs[loop];
    if (picking_sr.level == 0) {
      FileMetaData* f = picking_sr.file;
      inputs[0].files.push_back(f);
    } else {
      auto& files = inputs[picking_sr.level - start_level].files;
      for (auto* f : vstorage->LevelFiles(picking_sr.level)) {
        files.push_back(f);
      }
    }
    char file_num_buf[256];
    picking_sr.DumpSizeInfo(file_num_buf, sizeof(file_num_buf), loop);
    ROCKS_LOG_BUFFER(log_buffer, "[%s] Universal: size amp picking %s",
                     cf_name.c_str(), file_num_buf);
  }

  // output files at the bottom most level, unless it's reserved
  int output_level = vstorage->num_levels() - 1;
  // last level is reserved for the files ingested behind
  if (ioptions_.allow_ingest_behind) {
    assert(output_level > 1);
    output_level--;
  }

  CompactionParams params(vstorage, ioptions_, mutable_cf_options);
  params.inputs = std::move(inputs);
  params.output_level = output_level;
  params.target_file_size = MaxFileSizeForLevel(
      mutable_cf_options, output_level, kCompactionStyleUniversal);
  params.max_compaction_bytes = LLONG_MAX;
  params.output_path_id = path_id;
  params.compression = GetCompressionType(ioptions_, vstorage,
                                          mutable_cf_options, output_level, 1);
  params.compression_opts =
      GetCompressionOptions(ioptions_, vstorage, output_level);
  params.score = score;
  params.compaction_reason = CompactionReason::kUniversalSizeAmplification;

  return new Compaction(std::move(params));
}

// Pick files marked for compaction. Typically, files are marked by
// CompactOnDeleteCollector due to the presence of tombstones.
Compaction* UniversalCompactionPicker::PickDeleteTriggeredCompaction(
    const std::string& cf_name, const MutableCFOptions& mutable_cf_options,
    VersionStorageInfo* vstorage, double score,
    const std::vector<SortedRun>& /*sorted_runs*/, LogBuffer* /*log_buffer*/) {
  CompactionInputFiles start_level_inputs;
  int output_level;
  std::vector<CompactionInputFiles> inputs;

  if (vstorage->num_levels() == 1) {
    // This is single level universal. Since we're basically trying to reclaim
    // space by processing files marked for compaction due to high tombstone
    // density, let's do the same thing as compaction to reduce size amp which
    // has the same goals.
    bool compact = false;

    start_level_inputs.level = 0;
    start_level_inputs.files.clear();
    output_level = 0;
    for (FileMetaData* f : vstorage->LevelFiles(0)) {
      if (f->marked_for_compaction) {
        compact = true;
      }
      if (compact) {
        start_level_inputs.files.push_back(f);
      }
    }
    if (start_level_inputs.size() <= 1) {
      // If only the last file in L0 is marked for compaction, ignore it
      return nullptr;
    }
    inputs.push_back(start_level_inputs);
  } else {
    int start_level;

    // For multi-level universal, the strategy is to make this look more like
    // leveled. We pick one of the files marked for compaction and compact with
    // overlapping files in the adjacent level.
    PickFilesMarkedForCompaction(cf_name, vstorage, &start_level, &output_level,
                                 &start_level_inputs);
    if (start_level_inputs.empty()) {
      return nullptr;
    }

    // Pick the first non-empty level after the start_level
    for (output_level = start_level + 1; output_level < vstorage->num_levels();
         output_level++) {
      if (vstorage->NumLevelFiles(output_level) != 0) {
        break;
      }
    }

    // If all higher levels are empty, pick the highest level as output level
    if (output_level == vstorage->num_levels()) {
      if (start_level == 0) {
        output_level = vstorage->num_levels() - 1;
      } else {
        // If start level is non-zero and all higher levels are empty, this
        // compaction will translate into a trivial move. Since the idea is
        // to reclaim space and trivial move doesn't help with that, we
        // skip compaction in this case and return nullptr
        return nullptr;
      }
    }
    if (ioptions_.allow_ingest_behind &&
        output_level == vstorage->num_levels() - 1) {
      assert(output_level > 1);
      output_level--;
    }

    if (output_level != 0) {
      if (start_level == 0) {
        if (!GetOverlappingL0Files(vstorage, &start_level_inputs, output_level,
                                   nullptr)) {
          return nullptr;
        }
      }

      CompactionInputFiles output_level_inputs;
      int parent_index = -1;

      output_level_inputs.level = output_level;
      if (!SetupOtherInputs(cf_name, mutable_cf_options, vstorage,
                            &start_level_inputs, &output_level_inputs,
                            &parent_index, -1)) {
        return nullptr;
      }
      inputs.push_back(start_level_inputs);
      if (!output_level_inputs.empty()) {
        inputs.push_back(output_level_inputs);
      }
      if (FilesRangeOverlapWithCompaction(inputs, output_level)) {
        return nullptr;
      }
    } else {
      inputs.push_back(start_level_inputs);
    }
  }

  uint64_t estimated_total_size = 0;
  // Use size of the output level as estimated file size
  for (FileMetaData* f : vstorage->LevelFiles(output_level)) {
    estimated_total_size += f->fd.GetFileSize();
  }
  uint32_t path_id =
      GetPathId(ioptions_, mutable_cf_options, estimated_total_size);
  SstPurpose compaction_purpose = kEssenceSst;
  uint32_t max_subcompactions = 0;
  if (mutable_cf_options.enable_lazy_compaction && output_level != 0) {
    compaction_purpose = kMapSst;
    max_subcompactions = 1;
  }
  CompactionParams params(vstorage, ioptions_, mutable_cf_options);
  params.inputs = std::move(inputs);
  params.output_level = output_level;
  params.target_file_size = MaxFileSizeForLevel(
      mutable_cf_options, output_level, kCompactionStyleUniversal);
  params.max_compaction_bytes = LLONG_MAX;
  params.output_path_id = path_id;
  params.compression = GetCompressionType(ioptions_, vstorage,
                                          mutable_cf_options, output_level, 1);
  params.compression_opts =
      GetCompressionOptions(ioptions_, vstorage, output_level);
  params.max_subcompactions = max_subcompactions;
  params.manual_compaction = true;
  params.score = score;
  params.compaction_purpose = compaction_purpose;
  params.compaction_reason = CompactionReason::kFilesMarkedForCompaction;

  return new Compaction(std::move(params));
}

Compaction* UniversalCompactionPicker::PickTrivialMoveCompaction(
    const std::string& /*cf_name*/, const MutableCFOptions& mutable_cf_options,
    VersionStorageInfo* vstorage, LogBuffer* /*log_buffer*/) {
  if (!mutable_cf_options.compaction_options_universal.allow_trivial_move) {
    return nullptr;
  }
  int output_level = vstorage->num_levels() - 1;
  // last level is reserved for the files ingested behind
  if (ioptions_.allow_ingest_behind) {
    --output_level;
  }
  int start_level = 0;
  auto is_compaction_output_level = [&](int l) {
    for (auto c : compactions_in_progress_) {
      if (c->output_level() == l) {
        return true;
      }
    }
    return false;
  };
  while (true) {
    // found an empty level
    for (; output_level >= 1; --output_level) {
      if (vstorage->LevelFiles(output_level).empty() &&
          !is_compaction_output_level(output_level)) {
        break;
      }
    }
    if (output_level < 1) {
      return nullptr;
    }
    bool found_start_level = false;
    // found an non empty level
    for (start_level = output_level - 1; start_level > 0; --start_level) {
      if (is_compaction_output_level(start_level)) {
        break;
      }
      if (!vstorage->LevelFiles(start_level).empty()) {
        found_start_level = true;
        break;
      }
    }
    if (start_level == 0) {
      // will move lv0 last sst
      break;
    }
    if (found_start_level &&
        !AreFilesInCompaction(vstorage->LevelFiles(start_level))) {
      break;
    }
    output_level = start_level - 1;
  }
  CompactionInputFiles inputs;
  inputs.level = start_level;
  uint32_t path_id = 0;
  if (start_level == 0) {
    auto& level0_files = vstorage->LevelFiles(0);
    if (level0_files.empty() || level0_files.back()->being_compacted) {
      return nullptr;
    }
    FileMetaData* meta = level0_files.back();
    inputs.files = {meta};
    path_id = meta->fd.GetPathId();
  } else {
    inputs.files = vstorage->LevelFiles(start_level);
    path_id = inputs.files.front()->fd.GetPathId();
  }
  assert(!AreFilesInCompaction(inputs.files));
  CompactionParams params(vstorage, ioptions_, mutable_cf_options);
  params.inputs = {std::move(inputs)};
  params.output_level = output_level;
  params.output_path_id = path_id;
  params.compression_opts = ioptions_.compression_opts;
  params.compaction_reason = CompactionReason::kTrivialMoveLevel;

  return new Compaction(std::move(params));
}

Compaction* UniversalCompactionPicker::PickCompositeCompaction(
    const std::string& cf_name, const MutableCFOptions& mutable_cf_options,
    VersionStorageInfo* vstorage, const std::vector<SortedRun>& sorted_runs,
    LogBuffer* log_buffer) {
  if (!vstorage->has_space_amplification()) {
    return nullptr;
  }
  CompactionInputFiles inputs;
  inputs.level = -1;
  size_t max_read_amp = 0;
  for (auto rit = sorted_runs.rbegin(); rit != sorted_runs.rend(); ++rit) {
    auto& sr = *rit;
    if (sr.wait_reduce) {
      continue;
    }
    FileMetaData* f;
    if (sr.level > 0) {
      if (!vstorage->has_space_amplification(sr.level)) {
        continue;
      }
      auto& level_files = vstorage->LevelFiles(sr.level);
      if (AreFilesInCompaction(level_files)) {
        continue;
      }
      if (level_files.size() > 1) {
        inputs.level = sr.level;
        inputs.files.clear();
        break;
      }
      f = level_files.front();
    } else {
      if (sr.file->being_compacted || sr.file->sst_purpose != kMapSst) {
        continue;
      }
      f = sr.file;
    }
    std::shared_ptr<const TableProperties> porps;
    auto s = table_cache_->GetTableProperties(
        env_options_, *icmp_, f->fd, &porps,
        mutable_cf_options.prefix_extractor.get(), false);
    if (s.ok()) {
      size_t level_space_amplification =
          GetSstReadAmp(porps->user_collected_properties);
      if (level_space_amplification >= max_read_amp) {
        max_read_amp = level_space_amplification;
        inputs.level = sr.level;
        inputs.files = {f};
      }
    }
  }
  if (inputs.level == -1) {
    return nullptr;
  }
  SstPurpose compaction_purpose = kEssenceSst;
  uint32_t max_subcompactions = ioptions_.max_subcompactions;
  std::vector<RangeStorage> input_range;

  auto new_compaction = [&] {
    auto uc = ioptions_.user_comparator;
    // remove empty ranges
    if (input_range.size() > 1) {
      for (auto it = input_range.begin() + 1; it != input_range.end();) {
        if (uc->Compare(it->start, it[-1].start) == 0 ||
            uc->Compare(it->limit, it[-1].limit) == 0) {
          it[-1].limit = std::move(it->limit);
          it[-1].include_limit = it->include_limit;
          it = input_range.erase(it);
        } else {
          ++it;
        }
      }
    }
    assert(std::is_sorted(
        input_range.begin(), input_range.end(),
        [uc](const RangeStorage& a, const RangeStorage& b) {
          return uc->Compare(a.start, b.start) < 0;
        }));
    assert(std::is_sorted(
        input_range.begin(), input_range.end(),
        [uc](const RangeStorage& a, const RangeStorage& b) {
          return uc->Compare(a.limit, b.limit) < 0;
        }));
    assert(std::find_if(
        input_range.begin(), input_range.end(),
        [uc](const RangeStorage& r) {
          return uc->Compare(r.start, r.limit) > 0;
        }) == input_range.end());
    uint64_t estimated_total_size = 0;
    for (auto f : inputs.files) {
      estimated_total_size += f->fd.file_size;
    }
    uint32_t path_id =
        GetPathId(ioptions_, mutable_cf_options, estimated_total_size);

    CompactionParams params(vstorage, ioptions_, mutable_cf_options);
    params.inputs = {inputs};
    params.output_level = inputs.level;
    params.target_file_size =
        MaxFileSizeForLevel(mutable_cf_options, std::max(1, inputs.level),
                            kCompactionStyleUniversal);
    params.max_compaction_bytes = LLONG_MAX;
    params.output_path_id = path_id;
    params.compression = GetCompressionType(
        ioptions_, vstorage, mutable_cf_options, inputs.level, 1, true);
    params.compression_opts =
        GetCompressionOptions(ioptions_, vstorage, inputs.level, true);
    params.max_subcompactions = max_subcompactions;
    params.score = 0;
    params.partial_compaction = true;
    params.compaction_purpose = compaction_purpose;
    params.input_range = std::move(input_range);
    params.compaction_reason = CompactionReason::kCompositeAmplification;

    return new Compaction(std::move(params));
  };

  if (inputs.files.empty()) {
    inputs.files = vstorage->LevelFiles(inputs.level);
    assert(inputs.files.size() > 1);
    compaction_purpose = kMapSst;
    max_subcompactions = 1;
    return new_compaction();
  }
  Arena arena;
  DependFileMap empty_depend_files;
  ReadOptions options;
  ScopedArenaIterator iter(table_cache_->NewIterator(
      options, env_options_, *icmp_, *inputs.files.front(), empty_depend_files,
      nullptr, mutable_cf_options.prefix_extractor.get(), nullptr, nullptr,
      false, &arena, true, inputs.level));
  if (!iter->status().ok()) {
    ROCKS_LOG_BUFFER(log_buffer, "[%s] Universal: Read map sst error %s.",
                     cf_name.c_str(), iter->status().getState());
    return nullptr;
  }
  auto is_perfect = [=](const MapSstElement& e) {
    if (e.link_.size() != 1) {
      return false;
    }
    auto& depend_files = vstorage->depend_files();
    auto find = depend_files.find(e.link_.front().file_number);
    if (find == depend_files.end()) {
      // TODO log error
      return false;
    }
    auto f = find->second;
    if (f->sst_purpose != 0) {
      return false;
    }
    Range r(e.smallest_key_, e.largest_key_, e.include_smallest_,
            e.include_largest_);
    return IsPrefaceRange(r, f, *icmp_);
  };
  auto assign_user_key = [](std::string& key, const Slice& ikey) {
    Slice ukey = ExtractUserKey(ikey);
    key.assign(ukey.data(), ukey.size());
  };

  struct FileUseInfo {
    uint64_t size;
    uint64_t used;
  };
  std::unordered_map<uint64_t, FileUseInfo> file_used;
  MapSstElement map_element;
  RangeStorage range;
  auto uc = ioptions_.internal_comparator.user_comparator();
  auto set_include_limit = [&] {
    range.include_limit = true;
    auto uend = inputs.files.front()->largest.user_key();
    assert(uc->Compare(range.limit, uend) <= 0);
    range.limit.assign(uend.data(), uend.size());
  };
  bool has_start = false;
  size_t counter = 0;
  for (iter->SeekToFirst(); iter->Valid(); iter->Next()) {
    ++counter;
    if (!map_element.Decode(iter->key(), iter->value())) {
      // TODO log error info
      return nullptr;
    }
    if (is_perfect(map_element)) {
      continue;
    }
    size_t sum = 0, max = 0;
    for (auto& l : map_element.link_) {
      sum += l.size;
      max = std::max<size_t>(max, l.size);
      auto find = file_used.find(l.file_number);
      if (find == file_used.end()) {
        FileUseInfo info = {
            GetFilesSize(nullptr, l.file_number, *vstorage), l.size
        };
        file_used.emplace(l.file_number, info);
      } else {
        find->second.used += l.size;
      }
    }
    if (map_element.link_.size() > 2 && (sum - max) * 2 < max) {
      if (!has_start) {
        has_start = true;
        assign_user_key(range.start, map_element.smallest_key_);
      }
      assign_user_key(range.limit, map_element.largest_key_);
    } else {
      if (has_start) {
        has_start = false;
        if (uc->Compare(ExtractUserKey(map_element.smallest_key_),
                        range.limit) != 0) {
          assign_user_key(range.limit, map_element.smallest_key_);
          range.include_start = true;
          range.include_limit = false;
          input_range.emplace_back(std::move(range));
          if (input_range.size() >= ioptions_.max_subcompactions) {
            break;
          }
        }
      }
    }
  }
  if (has_start) {
    set_include_limit();
    input_range.emplace_back(std::move(range));
  }
  if (!input_range.empty()) {
    compaction_purpose = kLinkSst;
    return new_compaction();
  }
  std::vector<InternalKey> internal_key_storage;
  internal_key_storage.resize(counter *= 2);
  struct HeapItem {
    Slice k;
    double s;
    operator double() const noexcept {
      return s;
    }
  };
  std::vector<HeapItem> priority_heap;
  for (iter->SeekToFirst(); iter->Valid(); iter->Next()) {
    if (!map_element.Decode(iter->key(), iter->value())) {
      // TODO log error info
      return nullptr;
    }
    double p = map_element.link_.size();
    size_t size = 0, used = 0;
    for (auto& l : map_element.link_) {
      auto find = file_used.find(l.file_number);
      if (find == file_used.end()) {
        p = -1;
        break;
      }
      size += find->second.size;
      used += find->second.used;
    }
    if (p < 0) {
      continue;
    }
    p += 2.0 * (size - std::min(used, size)) / size;
    assert(counter > 0);
    internal_key_storage[--counter].DecodeFrom(
        map_element.largest_key_);
    HeapItem item = {
        internal_key_storage[counter].Encode(), p
    };
    priority_heap.push_back(item);
  }
  std::make_heap(priority_heap.begin(), priority_heap.end(), std::less<double>());
  struct Comp {
    const InternalKeyComparator* c;
    bool operator()(const Slice& a, const Slice& b) const {
      return c->Compare(a, b) < 0;
    }
  } c = {icmp_};
  std::set<Slice, Comp> unique_check(c);
  auto push_unique = [&](const Slice& key) {
    assert(counter > 0);
    internal_key_storage[--counter].DecodeFrom(key);
    unique_check.emplace(internal_key_storage[counter].Encode());
  };
  size_t max_file_size_for_leval = size_t(
      MaxFileSizeForLevel(mutable_cf_options, std::max(1, inputs.level),
                          kCompactionStyleUniversal) * 2);
  auto estimate_size = [](const MapSstElement& element) {
    size_t sum = 0;
    for (auto& l : element.link_) {
      sum += l.size;
    }
    return sum;
  };
  while (!priority_heap.empty()) {
    auto key = priority_heap.front().k;
    std::pop_heap(priority_heap.begin(), priority_heap.end(), std::less<double>());
    priority_heap.pop_back();
    iter->Seek(key);
    assert(iter->Valid());
    if (unique_check.count(iter->key()) > 0) {
      continue;
    }
    map_element.Decode(iter->key(), iter->value());
    assign_user_key(range.start, map_element.smallest_key_);
    assign_user_key(range.limit, map_element.largest_key_);
    range.include_start = true;
    range.include_limit = false;
    size_t sum = estimate_size(map_element);
    push_unique(iter->key());
    while (sum < max_file_size_for_leval) {
      iter->Next();
      if (!iter->Valid()) {
        set_include_limit();
        break;
      }
      map_element.Decode(iter->key(), iter->value());
      if (unique_check.count(iter->key()) > 0 ||
          (is_perfect(map_element) &&
           uc->Compare(ExtractUserKey(map_element.smallest_key_),
                       range.limit) != 0)) {
        assign_user_key(range.limit, map_element.smallest_key_);
        break;
      } else {
        assign_user_key(range.limit, map_element.largest_key_);
        sum += estimate_size(map_element);
        push_unique(iter->key());
      }
    }
    if (sum < max_file_size_for_leval) {
      iter->SeekForPrev(key);
      do {
        iter->Prev();
        if (!iter->Valid() || unique_check.count(iter->key()) > 0) {
          break;
        }
        map_element.Decode(iter->key(), iter->value());
        if (is_perfect(map_element)) {
          break;
        }
        assign_user_key(range.start, map_element.smallest_key_);
        sum += estimate_size(map_element);
        push_unique(iter->key());
      } while (sum < max_file_size_for_leval);
    }
    input_range.emplace_back(std::move(range));
    if (input_range.size() >= ioptions_.max_subcompactions) {
      break;
    }
  }
  if (!input_range.empty()) {
    std::sort(input_range.begin(), input_range.end(),
              [=](const RangeStorage& a, const RangeStorage& b) {
                int r = uc->Compare(a.limit, b.limit);
                if (r == 0) {
                  r = int(a.include_limit) - int(b.include_limit);
                }
                if (r == 0) {
                  r = uc->Compare(a.start, b.start);
                }
                if (r == 0) {
                  r = int(b.include_start) - int(a.include_start);
                }
                return r < 0;
              });
    compaction_purpose = kEssenceSst;
    return new_compaction();
  }
  has_start = false;
  for (iter->SeekToFirst(); iter->Valid(); iter->Next()) {
    map_element.Decode(iter->key(), iter->value());
    assert(map_element.link_.size() == 1);

    if (has_start) {
      if (is_perfect(map_element) &&
          uc->Compare(ExtractUserKey(map_element.smallest_key_), range.limit) !=
          0) {
        has_start = false;
        assign_user_key(range.limit, map_element.smallest_key_);
        range.include_start = true;
        range.include_limit = false;
        input_range.emplace_back(std::move(range));
        if (input_range.size() >= ioptions_.max_subcompactions) {
          break;
        }
      } else {
        assign_user_key(range.limit, map_element.largest_key_);
      }
    } else {
      if (is_perfect(map_element)) {
        continue;
      }
      has_start = true;
      assign_user_key(range.start, map_element.smallest_key_);
      assign_user_key(range.limit, map_element.largest_key_);
    }
  }
  if (has_start) {
    range.include_start = true;
    set_include_limit();
    input_range.emplace_back(std::move(range));
  }
  if (!input_range.empty()) {
    compaction_purpose = kEssenceSst;
    return new_compaction();
  }
  if (inputs.level != 0) {
    max_subcompactions = 1;
    compaction_purpose = kMapSst;
    return new_compaction();
  }
  return nullptr;
}

Compaction* UniversalCompactionPicker::PickRangeCompaction(
    const std::string& cf_name, const MutableCFOptions& mutable_cf_options,
    VersionStorageInfo* vstorage, int level, const InternalKey* begin,
    const InternalKey* end,
    const std::unordered_set<uint64_t>* files_being_compact,
    bool* manual_conflict, LogBuffer* log_buffer) {
  auto& level_files = vstorage->LevelFiles(level);

  if (files_being_compact == nullptr || files_being_compact->empty() ||
      level_files.empty()) {
    return nullptr;
  }
  if (AreFilesInCompaction(level_files)) {
    *manual_conflict = true;
    return nullptr;
  }
  CompactionInputFiles inputs;
  inputs.level = level;
  inputs.files = level_files;

  if (level == 0 && level_files.size() > 1) {
    uint32_t path_id = GetPathId(ioptions_, mutable_cf_options, 1ULL << 20);
    CompactionParams params(vstorage, ioptions_, mutable_cf_options);

    params.inputs = {std::move(inputs)};
    params.output_level = level;
    params.target_file_size = MaxFileSizeForLevel(mutable_cf_options, level,
                                                  kCompactionStyleUniversal);
    params.output_path_id = path_id;
    params.compression_opts = ioptions_.compression_opts;
    params.score = 0;
    params.compaction_purpose = kMapSst;
    return new Compaction(std::move(params));
  }

  std::vector<RangeStorage> input_range;
  Arena arena;
  DependFileMap empty_depend_files;
  ReadOptions options;
  ScopedArenaIterator iter(NewMapElementIterator(
      level_files.data(), level_files.size(), table_cache_, options,
      env_options_, icmp_, mutable_cf_options.prefix_extractor.get(), &arena));
  if (!iter->status().ok()) {
    ROCKS_LOG_BUFFER(log_buffer, "[%s] Universal: Read level files error %s.",
                     cf_name.c_str(), iter->status().getState());
    return nullptr;
  }

  MapSstElement map_element;
  RangeStorage range;
  auto& ic = ioptions_.internal_comparator;
  auto assign_user_key = [](std::string& key, const Slice& ikey) {
    Slice ukey = ExtractUserKey(ikey);
    key.assign(ukey.data(), ukey.size());
  };
  auto need_compact = [&](const MapSstElement& e) {
    if (begin != nullptr && ic.Compare(e.largest_key_, begin->Encode()) < 0) {
      return false;
    }
    if (end != nullptr && ic.Compare(e.smallest_key_, end->Encode()) > 0) {
      return false;
    }
    auto& depend_files = vstorage->depend_files();
    for (auto& link : e.link_) {
      if (files_being_compact->count(link.file_number) > 0) {
        return true;
      }
      auto find = depend_files.find(link.file_number);
      if (find == depend_files.end()) {
        // TODO: log error
        continue;
      }
      auto f = find->second;
      for (auto file_number : f->sst_depend) {
        if (files_being_compact->count(file_number) > 0) {
          return true;
        }
      };
    }
    return false;
  };
  bool has_start = false;
  size_t max_compaction_bytes = mutable_cf_options.max_compaction_bytes;
  size_t subcompact_size = 0;
  size_t estimated_total_size = 0;
  for (iter->SeekToFirst(); iter->Valid(); iter->Next()) {
    map_element.Decode(iter->key(), iter->value());

    if (has_start) {
      if (need_compact(map_element)) {
        if (subcompact_size < max_compaction_bytes) {
          subcompact_size += map_element.EstimateSize();
          assign_user_key(range.limit, map_element.largest_key_);
        } else {
          assign_user_key(range.limit, map_element.smallest_key_);
          range.include_start = true;
          range.include_limit = false;
          estimated_total_size += subcompact_size;
          input_range.emplace_back(std::move(range));
          if (input_range.size() >= ioptions_.max_subcompactions) {
            has_start = false;
            break;
          }
          subcompact_size += map_element.EstimateSize();
          assign_user_key(range.start, map_element.smallest_key_);
          assign_user_key(range.limit, map_element.largest_key_);
        }
      } else {
        has_start = false;
        assign_user_key(range.limit, map_element.smallest_key_);
        range.include_start = true;
        range.include_limit = false;
        estimated_total_size += subcompact_size;
        input_range.emplace_back(std::move(range));
        if (input_range.size() >= ioptions_.max_subcompactions) {
          break;
        }
        subcompact_size = 0;
      }
    } else {
      if (!need_compact(map_element)) {
        continue;
      }
      subcompact_size += map_element.EstimateSize();
      has_start = true;
      assign_user_key(range.start, map_element.smallest_key_);
      assign_user_key(range.limit, map_element.largest_key_);
    }
  }
  if (has_start) {
    range.include_start = true;
    range.include_limit = true;
    Slice end_key;
    if (level == 0) {
      for (auto f : level_files) {
        if (end_key.empty() || ic.Compare(f->largest.Encode(), end_key) > 0) {
          end_key = f->largest.Encode();
        }
      }
    } else {
      end_key = level_files.back()->largest.Encode();
    }
    end_key = ExtractUserKey(end_key);
    assert(ic.user_comparator()->Compare(range.limit, end_key) <= 0);
    range.limit.assign(end_key.data(), end_key.size());
    estimated_total_size += subcompact_size;
    input_range.emplace_back(std::move(range));
  }
  if (input_range.empty()) {
    return nullptr;
  }
  uint32_t path_id =
      GetPathId(ioptions_, mutable_cf_options, estimated_total_size);
  CompactionParams params(vstorage, ioptions_, mutable_cf_options);

  params.inputs = {std::move(inputs)};
  params.output_level = level;
  params.target_file_size = MaxFileSizeForLevel(
      mutable_cf_options, std::max(1, level), kCompactionStyleUniversal);
  params.max_compaction_bytes = LLONG_MAX;
  params.output_path_id = path_id;
  params.compression =
      GetCompressionType(ioptions_, vstorage, mutable_cf_options, level, 1);
  params.compression_opts = GetCompressionOptions(ioptions_, vstorage, level);
  params.score = 0;
  params.input_range = std::move(input_range);
  params.partial_compaction = true;
  params.compaction_purpose = kEssenceSst;
  return new Compaction(std::move(params));
}

//
// Consider compaction files based on their size differences with
// the next file in time order.
//
Compaction* UniversalCompactionPicker::PickCompactionToReduceSortedRuns(
    const std::string& cf_name, const MutableCFOptions& mutable_cf_options,
    VersionStorageInfo* vstorage, double score,
    std::vector<SortedRun>* sorted_runs_ptr, size_t reduce_sorted_run_target,
    LogBuffer* log_buffer) {
  auto& sorted_runs = *sorted_runs_ptr;
  if (reduce_sorted_run_target == 0) {
    reduce_sorted_run_target = sorted_runs.size();
  }
  std::vector<double> sorted_run_ratio(sorted_runs.size());
  double base_size = double(mutable_cf_options.write_buffer_size);
  std::transform(sorted_runs.begin(), sorted_runs.end(),
                 sorted_run_ratio.begin(),
                 [&](const SortedRun& sr) { return sr.size / base_size; });
  std::vector<SortedRunGroup> group;
  auto common_ratio =
      GenSortedRunGroup(sorted_run_ratio, reduce_sorted_run_target, &group);
  ROCKS_LOG_BUFFER(
      log_buffer,
      "[%s] Universal: reduce to %zd sorted runs, common ratio = %f\n",
      cf_name.c_str(), reduce_sorted_run_target, common_ratio);
  size_t start_index = 0, end_index = 0;
  for (size_t group_i = 0; group_i < group.size(); ++group_i) {
    const auto& g = group[group_i];
    bool being_compacted = false;
    if (g.count > 1) {
      for (size_t sr_i = g.start, sr_end = g.start + g.count; sr_i < sr_end;
           ++sr_i) {
        being_compacted |= sorted_runs[sr_i].being_compacted;
        sorted_runs[sr_i].wait_reduce = true;
      }
    }
    if (end_index != 0) {
      continue;
    }
    if (g.count == 1) {
      ROCKS_LOG_BUFFER(log_buffer,
                       "[%s] Universal: group %zd, count = %zd, size = %zd, "
                       "single sorted sun, skip\n",
                       cf_name.c_str(), group_i + 1, g.count,
                       size_t(g.ratio * base_size));
      continue;
    }
    if (being_compacted) {
      ROCKS_LOG_BUFFER(log_buffer,
                       "[%s] Universal: group %zd, count = %zd, size = %zd, "
                       "being compacted, skip\n",
                       cf_name.c_str(), group_i + 1, g.count,
                       size_t(g.ratio * base_size));
      continue;
    }
    start_index = g.start;
    end_index = g.start + g.count;
  }
  if (end_index == 0) {
    return nullptr;
  }

  // Compression is enabled if files compacted earlier already reached
  // size ratio of compression.
  bool enable_compression = true;
  int ratio_to_compress =
      mutable_cf_options.compaction_options_universal.compression_size_percent;
  if (ratio_to_compress >= 0) {
    uint64_t total_size = 0;
    for (auto& sorted_run : sorted_runs) {
      total_size += sorted_run.compensated_file_size;
    }

    uint64_t older_file_size = 0;
    for (size_t i = sorted_runs.size() - 1; i >= end_index; i--) {
      older_file_size += sorted_runs[i].size;
      if (older_file_size * 100L >= total_size * (long)ratio_to_compress) {
        enable_compression = false;
        break;
      }
    }
  }
  uint64_t estimated_total_size = 0;
  for (size_t i = start_index; i < end_index; i++) {
    estimated_total_size += sorted_runs[i].size;
  }
  uint32_t path_id =
      GetPathId(ioptions_, mutable_cf_options, estimated_total_size);
  int start_level = sorted_runs[start_index].level;
  int output_level;
  if (end_index == sorted_runs.size()) {
    output_level = vstorage->num_levels() - 1;
  } else if (sorted_runs[end_index].level == 0) {
    output_level = 0;
  } else {
    output_level = sorted_runs[end_index].level - 1;
  }

  // last level is reserved for the files ingested behind
  if (ioptions_.allow_ingest_behind &&
      (output_level == vstorage->num_levels() - 1)) {
    assert(output_level > 1);
    output_level--;
  }

  std::vector<CompactionInputFiles> inputs(end_index - start_index);
  for (size_t i = 0; i < inputs.size(); ++i) {
    inputs[i].level = start_level + static_cast<int>(i);
  }
  char file_num_buf[kFormatFileNumberBufSize];
  for (size_t i = start_index; i < end_index; i++) {
    auto& picking_sr = sorted_runs[i];
    if (picking_sr.level == 0) {
      FileMetaData* picking_file = picking_sr.file;
      inputs[0].files.push_back(picking_file);
    } else {
      inputs[picking_sr.level - start_level].files =
          vstorage->LevelFiles(picking_sr.level);
    }
    picking_sr.DumpSizeInfo(file_num_buf, sizeof(file_num_buf), i);
    ROCKS_LOG_BUFFER(log_buffer, "[%s] Universal: Picking %s", cf_name.c_str(),
                     file_num_buf);
  }

  CompactionParams params(vstorage, ioptions_, mutable_cf_options);
  params.inputs = std::move(inputs);
  params.output_level = output_level;
  params.target_file_size = MaxFileSizeForLevel(
      mutable_cf_options, output_level, kCompactionStyleUniversal);
  params.max_compaction_bytes = LLONG_MAX;
  params.output_path_id = path_id;
  params.compression =
      GetCompressionType(ioptions_, vstorage, mutable_cf_options, start_level,
                         1, enable_compression);
  params.compression_opts = GetCompressionOptions(
      ioptions_, vstorage, start_level, enable_compression);
  params.max_subcompactions = 1;
  params.score = score;
  params.compaction_purpose = kMapSst;
  params.compaction_reason = CompactionReason::kUniversalSortedRunNum;

  return new Compaction(std::move(params));
}

}  // namespace rocksdb

#endif  // !ROCKSDB_LITE
