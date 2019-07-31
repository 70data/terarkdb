//  Copyright (c) 2011-present, Facebook, Inc.  All rights reserved.
//  This source code is licensed under both the GPLv2 (found in the
//  COPYING file in the root directory) and Apache 2.0 License
//  (found in the LICENSE.Apache file in the root directory).

#include "table/get_context.h"
#include "db/merge_helper.h"
#include "db/read_callback.h"
#include "monitoring/file_read_sample.h"
#include "monitoring/perf_context_imp.h"
#include "monitoring/statistics.h"
#include "rocksdb/env.h"
#include "rocksdb/merge_operator.h"
#include "rocksdb/statistics.h"

namespace rocksdb {

namespace {

void appendToReplayLog(std::string* replay_log, ValueType type, Slice value) {
#ifndef ROCKSDB_LITE
  if (replay_log) {
    if (replay_log->empty()) {
      // Optimization: in the common case of only one operation in the
      // log, we allocate the exact amount of space needed.
      replay_log->reserve(1 + VarintLength(value.size()) + value.size());
    }
    replay_log->push_back(type);
    PutLengthPrefixedSlice(replay_log, value);
  }
#else
  (void)replay_log;
  (void)type;
  (void)value;
#endif  // ROCKSDB_LITE
}

}  // namespace

GetContext::GetContext(const Comparator* ucmp,
                       const MergeOperator* merge_operator, Logger* logger,
                       Statistics* statistics, GetState init_state,
                       const Slice& user_key, PinnableSlice* pinnable_val,
                       bool* value_found, MergeContext* merge_context,
                       SequenceNumber* _max_covering_tombstone_seq, Env* env,
                       SequenceNumber* seq, ReadCallback* callback)
    : ucmp_(ucmp),
      merge_operator_(merge_operator),
      logger_(logger),
      statistics_(statistics),
      state_(init_state),
      user_key_(user_key),
      pinnable_val_(pinnable_val),
      value_found_(value_found),
      merge_context_(merge_context),
      max_covering_tombstone_seq_(_max_covering_tombstone_seq),
      env_(env),
      seq_(seq),
      min_seq_type_(0),
      replay_log_(nullptr),
      callback_(callback) {
  if (seq_) {
    *seq_ = kMaxSequenceNumber;
  }
  sample_ = should_sample_file_read();
}

// Called from TableCache::Get and Table::Get when file/block in which
// key may exist are not there in TableCache/BlockCache respectively. In this
// case we can't guarantee that key does not exist and are not permitted to do
// IO to be certain.Set the status=kFound and value_found=false to let the
// caller know that key may exist but is not there in memory
void GetContext::MarkKeyMayExist() {
  state_ = kFound;
  if (value_found_ != nullptr) {
    *value_found_ = false;
  }
}

void GetContext::SaveValue(const Slice& value, SequenceNumber /*seq*/) {
  assert(state_ == kNotFound);
  appendToReplayLog(replay_log_, kTypeValue, value);

  state_ = kFound;
  if (LIKELY(pinnable_val_ != nullptr)) {
    pinnable_val_->PinSelf(value);
  }
}

void GetContext::ReportCounters() {
  if (get_context_stats_.num_cache_hit > 0) {
    RecordTick(statistics_, BLOCK_CACHE_HIT, get_context_stats_.num_cache_hit);
  }
  if (get_context_stats_.num_cache_index_hit > 0) {
    RecordTick(statistics_, BLOCK_CACHE_INDEX_HIT,
               get_context_stats_.num_cache_index_hit);
  }
  if (get_context_stats_.num_cache_data_hit > 0) {
    RecordTick(statistics_, BLOCK_CACHE_DATA_HIT,
               get_context_stats_.num_cache_data_hit);
  }
  if (get_context_stats_.num_cache_filter_hit > 0) {
    RecordTick(statistics_, BLOCK_CACHE_FILTER_HIT,
               get_context_stats_.num_cache_filter_hit);
  }
  if (get_context_stats_.num_cache_index_miss > 0) {
    RecordTick(statistics_, BLOCK_CACHE_INDEX_MISS,
               get_context_stats_.num_cache_index_miss);
  }
  if (get_context_stats_.num_cache_filter_miss > 0) {
    RecordTick(statistics_, BLOCK_CACHE_FILTER_MISS,
               get_context_stats_.num_cache_filter_miss);
  }
  if (get_context_stats_.num_cache_data_miss > 0) {
    RecordTick(statistics_, BLOCK_CACHE_DATA_MISS,
               get_context_stats_.num_cache_data_miss);
  }
  if (get_context_stats_.num_cache_bytes_read > 0) {
    RecordTick(statistics_, BLOCK_CACHE_BYTES_READ,
               get_context_stats_.num_cache_bytes_read);
  }
  if (get_context_stats_.num_cache_miss > 0) {
    RecordTick(statistics_, BLOCK_CACHE_MISS,
               get_context_stats_.num_cache_miss);
  }
  if (get_context_stats_.num_cache_add > 0) {
    RecordTick(statistics_, BLOCK_CACHE_ADD, get_context_stats_.num_cache_add);
  }
  if (get_context_stats_.num_cache_bytes_write > 0) {
    RecordTick(statistics_, BLOCK_CACHE_BYTES_WRITE,
               get_context_stats_.num_cache_bytes_write);
  }
  if (get_context_stats_.num_cache_index_add > 0) {
    RecordTick(statistics_, BLOCK_CACHE_INDEX_ADD,
               get_context_stats_.num_cache_index_add);
  }
  if (get_context_stats_.num_cache_index_bytes_insert > 0) {
    RecordTick(statistics_, BLOCK_CACHE_INDEX_BYTES_INSERT,
               get_context_stats_.num_cache_index_bytes_insert);
  }
  if (get_context_stats_.num_cache_data_add > 0) {
    RecordTick(statistics_, BLOCK_CACHE_DATA_ADD,
               get_context_stats_.num_cache_data_add);
  }
  if (get_context_stats_.num_cache_data_bytes_insert > 0) {
    RecordTick(statistics_, BLOCK_CACHE_DATA_BYTES_INSERT,
               get_context_stats_.num_cache_data_bytes_insert);
  }
  if (get_context_stats_.num_cache_filter_add > 0) {
    RecordTick(statistics_, BLOCK_CACHE_FILTER_ADD,
               get_context_stats_.num_cache_filter_add);
  }
  if (get_context_stats_.num_cache_filter_bytes_insert > 0) {
    RecordTick(statistics_, BLOCK_CACHE_FILTER_BYTES_INSERT,
               get_context_stats_.num_cache_filter_bytes_insert);
  }
}

bool GetContext::SaveValue(const ParsedInternalKey& parsed_key,
                           const LazySlice& value, bool* matched,
                           Cleanable* value_pinner) {
  assert(matched);
  assert((state_ != kMerge && parsed_key.type != kTypeMerge) ||
         merge_context_ != nullptr);
  if (ucmp_->Equal(parsed_key.user_key, user_key_)) {
    if (PackSequenceAndType(parsed_key.sequence, parsed_key.type) <
        min_seq_type_) {
      // for map sst, this key is masked
      return false;
    }
    *matched = true;
    // If the value is not in the snapshot, skip it
    if (!CheckCallback(parsed_key.sequence)) {
      return true;  // to continue to the next seq
    }

    if (replay_log_) {
      if (!value.decode().ok()) {
        state_ = kCorrupt;
        return false;
      }
      appendToReplayLog(replay_log_, parsed_key.type, *value);
    }

    if (seq_ != nullptr) {
      // Set the sequence number if it is uninitialized
      if (*seq_ == kMaxSequenceNumber) {
        *seq_ = parsed_key.sequence;
      }
    }

    auto type = parsed_key.type;
    // Key matches. Process it
    if ((type == kTypeValue || type == kTypeMerge || type == kTypeValueIndex ||
         type == kTypeMergeIndex) && max_covering_tombstone_seq_ != nullptr &&
        *max_covering_tombstone_seq_ > parsed_key.sequence) {
      type = kTypeRangeDeletion;
    }
    switch (type) {
      case kTypeValue:
      case kTypeValueIndex:
        assert(state_ == kNotFound || state_ == kMerge);
        if (kNotFound == state_) {
          state_ = kFound;
          if (LIKELY(pinnable_val_ != nullptr)) {
            if (!value.decode().ok()) {
              state_ = kCorrupt;
              return false;
            }
            if (LIKELY(value_pinner != nullptr)) {
              // If the backing resources for the value are provided, pin them
              pinnable_val_->PinSlice(*value, value_pinner);
            } else {
              // Otherwise copy the value
              pinnable_val_->PinSelf(*value);
            }
          }
        } else if (kMerge == state_) {
          assert(merge_operator_ != nullptr);
          state_ = kFound;
          if (LIKELY(pinnable_val_ != nullptr)) {
            Status merge_status = MergeHelper::TimedFullMerge(
                merge_operator_, user_key_, &value,
                merge_context_->GetOperands(), pinnable_val_->GetSelf(),
                logger_, statistics_, env_);
            pinnable_val_->PinSelf();
            if (!merge_status.ok()) {
              state_ = kCorrupt;
            }
          }
        }
        return false;

      case kTypeDeletion:
      case kTypeSingleDeletion:
      case kTypeRangeDeletion:
        // TODO(noetzli): Verify correctness once merge of single-deletes
        // is supported
        assert(state_ == kNotFound || state_ == kMerge);
        if (kNotFound == state_) {
          state_ = kDeleted;
        } else if (kMerge == state_) {
          state_ = kFound;
          if (LIKELY(pinnable_val_ != nullptr)) {
            Status merge_status = MergeHelper::TimedFullMerge(
                merge_operator_, user_key_, nullptr,
                merge_context_->GetOperands(), pinnable_val_->GetSelf(),
                logger_, statistics_, env_);
            pinnable_val_->PinSelf();
            if (!merge_status.ok()) {
              state_ = kCorrupt;
            }
          }
        }
        return false;

      case kTypeMerge:
      case kTypeMergeIndex:
        assert(state_ == kNotFound || state_ == kMerge);
        state_ = kMerge;
        merge_context_->PushOperand(value, user_key_);
        if (merge_operator_ != nullptr &&
            merge_operator_->ShouldMerge(
                merge_context_->GetOperandsDirectionBackward())) {
          state_ = kFound;
          if (LIKELY(pinnable_val_ != nullptr)) {
            Status merge_status = MergeHelper::TimedFullMerge(
                merge_operator_, user_key_, nullptr,
                merge_context_->GetOperands(), pinnable_val_->GetSelf(),
                logger_, statistics_, env_);
            pinnable_val_->PinSelf();
            if (!merge_status.ok()) {
              state_ = kCorrupt;
            }
          }
          return false;
        }
        return true;

      default:
        assert(false);
        break;
    }
  }

  // state_ could be Corrupt, merge or notfound
  return false;
}

void replayGetContextLog(const Slice& replay_log, const Slice& user_key,
                         GetContext* get_context, Cleanable* value_pinner) {
#ifndef ROCKSDB_LITE
  Slice s = replay_log;
  while (s.size()) {
    auto type = static_cast<ValueType>(*s.data());
    s.remove_prefix(1);
    Slice value;
    bool ret = GetLengthPrefixedSlice(&s, &value);
    assert(ret);
    (void)ret;

    bool dont_care __attribute__((__unused__));
    // Since SequenceNumber is not stored and unknown, we will use
    // kMaxSequenceNumber.
    get_context->SaveValue(
        ParsedInternalKey(user_key, kMaxSequenceNumber, type),
        LazySlice(value), &dont_care, value_pinner);
  }
#else   // ROCKSDB_LITE
  (void)replay_log;
  (void)user_key;
  (void)get_context;
  (void)value_pinner;
  assert(false);
#endif  // ROCKSDB_LITE
}

}  // namespace rocksdb
