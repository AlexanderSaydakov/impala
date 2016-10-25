// Licensed to the Apache Software Foundation (ASF) under one
// or more contributor license agreements.  See the NOTICE file
// distributed with this work for additional information
// regarding copyright ownership.  The ASF licenses this file
// to you under the Apache License, Version 2.0 (the
// "License"); you may not use this file except in compliance
// with the License.  You may obtain a copy of the License at
//
//   http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing,
// software distributed under the License is distributed on an
// "AS IS" BASIS, WITHOUT WARRANTIES OR CONDITIONS OF ANY
// KIND, either express or implied.  See the License for the
// specific language governing permissions and limitations
// under the License.

#include "exec/kudu-scanner.h"

#include <kudu/client/row_result.h>
#include <thrift/protocol/TDebugProtocol.h>
#include <vector>

#include "exprs/expr.h"
#include "exprs/expr-context.h"
#include "exec/kudu-util.h"
#include "runtime/mem-pool.h"
#include "runtime/mem-tracker.h"
#include "runtime/runtime-state.h"
#include "runtime/row-batch.h"
#include "runtime/string-value.h"
#include "runtime/tuple-row.h"
#include "gutil/gscoped_ptr.h"
#include "gutil/strings/substitute.h"
#include "util/jni-util.h"
#include "util/periodic-counter-updater.h"
#include "util/runtime-profile-counters.h"

#include "common/names.h"

using kudu::client::KuduClient;
using kudu::client::KuduScanBatch;
using kudu::client::KuduSchema;
using kudu::client::KuduTable;

DEFINE_bool(pick_only_leaders_for_tests, false,
            "Whether to pick only leader replicas, for tests purposes only.");
DEFINE_int32(kudu_scanner_keep_alive_period_sec, 15,
    "The period at which Kudu Scanners should send keep-alive requests to the tablet "
    "server to ensure that scanners do not time out.");

DEFINE_int32(kudu_scanner_timeout_sec, 60,
             "The timeout used for Kudu Scan requests.");

namespace impala {

KuduScanner::KuduScanner(KuduScanNode* scan_node, RuntimeState* state)
  : scan_node_(scan_node),
    state_(state),
    cur_kudu_batch_num_read_(0),
    last_alive_time_micros_(0),
    num_string_slots_(0) {
}

Status KuduScanner::Open() {
  // Store columns that need relocation when materialized into the
  // destination row batch.
  for (int i = 0; i < scan_node_->tuple_desc_->slots().size(); ++i) {
    if (scan_node_->tuple_desc_->slots()[i]->type().IsStringType()) {
      string_slots_.push_back(scan_node_->tuple_desc_->slots()[i]);
      ++num_string_slots_;
    }
  }
  return scan_node_->GetConjunctCtxs(&conjunct_ctxs_);
}

void KuduScanner::KeepKuduScannerAlive() {
  if (scanner_ == NULL) return;
  int64_t now = MonotonicMicros();
  int64_t keepalive_us = FLAGS_kudu_scanner_keep_alive_period_sec * 1e6;
  if (now < last_alive_time_micros_ + keepalive_us) {
    return;
  }
  // If we fail to send a keepalive, it isn't a big deal. The Kudu
  // client code doesn't handle cross-replica failover or retries when
  // the server is busy, so it's better to just ignore errors here. In
  // the worst case, we will just fail next time we try to fetch a batch
  // if the scan is unrecoverable.
  kudu::Status s = scanner_->KeepAlive();
  if (!s.ok()) {
    VLOG(1) << "Unable to keep the Kudu scanner alive: " << s.ToString();
    return;
  }
  last_alive_time_micros_ = now;
}

Status KuduScanner::GetNext(RowBatch* row_batch, bool* eos) {
  int64_t tuple_buffer_size;
  uint8_t* tuple_buffer;
  RETURN_IF_ERROR(
      row_batch->ResizeAndAllocateTupleBuffer(state_, &tuple_buffer_size, &tuple_buffer));
  Tuple* tuple = reinterpret_cast<Tuple*>(tuple_buffer);

  // Main scan loop:
  // Tries to fill 'row_batch' with rows from cur_kudu_batch_.
  // If there are no rows to decode, tries to get the next row batch from kudu.
  // If this scanner has no more rows, the scanner is closed and eos is returned.
  while (!*eos) {
    RETURN_IF_CANCELLED(state_);

    if (cur_kudu_batch_num_read_ < cur_kudu_batch_.NumRows()) {
      bool batch_done = false;
      RETURN_IF_ERROR(DecodeRowsIntoRowBatch(row_batch, &tuple, &batch_done));
      if (batch_done) break;
    }

    if (scanner_->HasMoreRows()) {
      RETURN_IF_ERROR(GetNextScannerBatch());
      continue;
    }

    CloseCurrentClientScanner();
    *eos = true;
  }
  return Status::OK();
}

void KuduScanner::Close() {
  if (scanner_) CloseCurrentClientScanner();
  Expr::Close(conjunct_ctxs_, state_);
}

Status KuduScanner::OpenNextScanToken(const string& scan_token)  {
  DCHECK(scanner_ == NULL);
  kudu::client::KuduScanner* scanner;
  KUDU_RETURN_IF_ERROR(kudu::client::KuduScanToken::DeserializeIntoScanner(
      scan_node_->kudu_client(), scan_token, &scanner),
      "Unable to deserialize scan token");
  scanner_.reset(scanner);

  if (UNLIKELY(FLAGS_pick_only_leaders_for_tests)) {
    KUDU_RETURN_IF_ERROR(scanner_->SetSelection(kudu::client::KuduClient::LEADER_ONLY),
                         "Could not set replica selection.");
  }

  KUDU_RETURN_IF_ERROR(scanner_->SetTimeoutMillis(FLAGS_kudu_scanner_timeout_sec * 1000),
      "Could not set scanner timeout");

  {
    SCOPED_TIMER(state_->total_storage_wait_timer());
    KUDU_RETURN_IF_ERROR(scanner_->Open(), "Unable to open scanner");
  }
  return Status::OK();
}

void KuduScanner::CloseCurrentClientScanner() {
  DCHECK_NOTNULL(scanner_.get());
  scanner_->Close();
  scanner_.reset();
}

Status KuduScanner::HandleEmptyProjection(RowBatch* row_batch, bool* batch_done) {
  int num_rows_remaining = cur_kudu_batch_.NumRows() - cur_kudu_batch_num_read_;
  int rows_to_add = std::min(row_batch->capacity() - row_batch->num_rows(),
      num_rows_remaining);
  cur_kudu_batch_num_read_ += rows_to_add;
  row_batch->CommitRows(rows_to_add);
  // If we've reached the capacity, or the LIMIT for the scan, return.
  if (row_batch->AtCapacity() || scan_node_->ReachedLimit()) {
    *batch_done = true;
  }
  return Status::OK();
}

Status KuduScanner::DecodeRowsIntoRowBatch(RowBatch* row_batch,
    Tuple** tuple_mem, bool* batch_done) {

  // Short-circuit the count(*) case.
  if (scan_node_->tuple_desc_->slots().empty()) {
    return HandleEmptyProjection(row_batch, batch_done);
  }

  // TODO consider consolidating the tuple creation/initialization here with the version
  // that happens inside the loop.
  int idx = row_batch->AddRow();
  TupleRow* row = row_batch->GetRow(idx);
  (*tuple_mem)->ClearNullBits(*scan_node_->tuple_desc());
  row->SetTuple(tuple_idx(), *tuple_mem);

  int num_rows = cur_kudu_batch_.NumRows();
  // Now iterate through the Kudu rows.
  for (int krow_idx = cur_kudu_batch_num_read_; krow_idx < num_rows; ++krow_idx) {
    // Clear any NULL indicators set by a previous iteration.
    (*tuple_mem)->ClearNullBits(*scan_node_->tuple_desc());

    // Transform a Kudu row into an Impala row.
    KuduScanBatch::RowPtr krow = cur_kudu_batch_.Row(krow_idx);
    RETURN_IF_ERROR(KuduRowToImpalaTuple(krow, row_batch, *tuple_mem));
    ++cur_kudu_batch_num_read_;

    // Evaluate the conjuncts that haven't been pushed down to Kudu.
    if (conjunct_ctxs_.empty() ||
        ExecNode::EvalConjuncts(&conjunct_ctxs_[0], conjunct_ctxs_.size(), row)) {
      // Materialize those slots that require auxiliary memory
      RETURN_IF_ERROR(RelocateValuesFromKudu(*tuple_mem, row_batch->tuple_data_pool()));
      // If the conjuncts pass on the row commit it.
      row_batch->CommitLastRow();
      // If we've reached the capacity, or the LIMIT for the scan, return.
      if (row_batch->AtCapacity() || scan_node_->ReachedLimit()) {
        *batch_done = true;
        break;
      }
      // Add another row.
      idx = row_batch->AddRow();

      // Move to the next tuple in the tuple buffer.
      *tuple_mem = next_tuple(*tuple_mem);
      (*tuple_mem)->ClearNullBits(*scan_node_->tuple_desc());
      // Make 'row' point to the new row.
      row = row_batch->GetRow(idx);
      row->SetTuple(tuple_idx(), *tuple_mem);
    }
  }
  ExprContext::FreeLocalAllocations(conjunct_ctxs_);

  // Check the status in case an error status was set during conjunct evaluation.
  return state_->GetQueryStatus();
}

void KuduScanner::SetSlotToNull(Tuple* tuple, const SlotDescriptor& slot) {
  DCHECK(slot.is_nullable());
  tuple->SetNull(slot.null_indicator_offset());
}

bool KuduScanner::IsSlotNull(Tuple* tuple, const SlotDescriptor& slot) {
  return slot.is_nullable() && tuple->IsNull(slot.null_indicator_offset());
}

Status KuduScanner::RelocateValuesFromKudu(Tuple* tuple, MemPool* mem_pool) {
  for (int i = 0; i < num_string_slots_; ++i) {
    const SlotDescriptor* slot = string_slots_[i];
    // NULL handling was done in KuduRowToImpalaTuple.
    if (IsSlotNull(tuple, *slot)) continue;

    // Extract the string value.
    void* slot_ptr = tuple->GetSlot(slot->tuple_offset());
    DCHECK(slot->type().IsVarLenStringType());

    // The string value of the slot has a pointer to memory from the Kudu row.
    StringValue* val = reinterpret_cast<StringValue*>(slot_ptr);
    char* old_buf = val->ptr;
    // Kudu never returns values larger than 8MB
    DCHECK_LE(val->len, 8 * (1 << 20));
    val->ptr = reinterpret_cast<char*>(mem_pool->TryAllocate(val->len));
    if (LIKELY(val->len > 0)) {
      // The allocator returns a NULL ptr when out of memory.
      if (UNLIKELY(val->ptr == NULL)) {
        return mem_pool->mem_tracker()->MemLimitExceeded(state_,
            "Kudu scanner could not allocate memory for string", val->len);
      }
      memcpy(val->ptr, old_buf, val->len);
    }
  }
  return Status::OK();
}


Status KuduScanner::KuduRowToImpalaTuple(const KuduScanBatch::RowPtr& row,
    RowBatch* row_batch, Tuple* tuple) {
  for (int i = 0; i < scan_node_->tuple_desc_->slots().size(); ++i) {
    const SlotDescriptor* info = scan_node_->tuple_desc_->slots()[i];
    void* slot = tuple->GetSlot(info->tuple_offset());

    if (row.IsNull(i)) {
      SetSlotToNull(tuple, *info);
      continue;
    }

    int max_len = -1;
    switch (info->type().type) {
      case TYPE_VARCHAR:
        max_len = info->type().len;
        DCHECK_GT(max_len, 0);
        // Fallthrough intended.
      case TYPE_STRING: {
        // For types with auxiliary memory (String, Binary,...) store the original memory
        // location in the tuple to avoid the copy when the conjuncts do not pass. Relocate
        // the memory into the row batch's memory in a later step.
        kudu::Slice slice;
        KUDU_RETURN_IF_ERROR(row.GetString(i, &slice),
            "Error getting column value from Kudu.");
        StringValue* sv = reinterpret_cast<StringValue*>(slot);
        sv->ptr = const_cast<char*>(reinterpret_cast<const char*>(slice.data()));
        sv->len = static_cast<int>(slice.size());
        if (max_len > 0) sv->len = std::min(sv->len, max_len);
        break;
      }
      case TYPE_TINYINT:
        KUDU_RETURN_IF_ERROR(row.GetInt8(i, reinterpret_cast<int8_t*>(slot)),
            "Error getting column value from Kudu.");
        break;
      case TYPE_SMALLINT:
        KUDU_RETURN_IF_ERROR(row.GetInt16(i, reinterpret_cast<int16_t*>(slot)),
            "Error getting column value from Kudu.");
        break;
      case TYPE_INT:
        KUDU_RETURN_IF_ERROR(row.GetInt32(i, reinterpret_cast<int32_t*>(slot)),
            "Error getting column value from Kudu.");
        break;
      case TYPE_BIGINT:
        KUDU_RETURN_IF_ERROR(row.GetInt64(i, reinterpret_cast<int64_t*>(slot)),
            "Error getting column value from Kudu.");
        break;
      case TYPE_FLOAT:
        KUDU_RETURN_IF_ERROR(row.GetFloat(i, reinterpret_cast<float*>(slot)),
            "Error getting column value from Kudu.");
        break;
      case TYPE_DOUBLE:
        KUDU_RETURN_IF_ERROR(row.GetDouble(i, reinterpret_cast<double*>(slot)),
            "Error getting column value from Kudu.");
        break;
      case TYPE_BOOLEAN:
        KUDU_RETURN_IF_ERROR(row.GetBool(i, reinterpret_cast<bool*>(slot)),
            "Error getting column value from Kudu.");
        break;
      default:
        DCHECK(false) << "Impala type unsupported in Kudu: "
            << TypeToString(info->type().type);
        return Status(TErrorCode::IMPALA_KUDU_TYPE_MISSING,
            TypeToString(info->type().type));
    }
  }
  return Status::OK();
}


Status KuduScanner::GetNextScannerBatch() {
  SCOPED_TIMER(state_->total_storage_wait_timer());
  int64_t now = MonotonicMicros();
  KUDU_RETURN_IF_ERROR(scanner_->NextBatch(&cur_kudu_batch_), "Unable to advance iterator");
  COUNTER_ADD(scan_node_->kudu_round_trips(), 1);
  cur_kudu_batch_num_read_ = 0;
  COUNTER_ADD(scan_node_->rows_read_counter(), cur_kudu_batch_.NumRows());
  last_alive_time_micros_ = now;
  return Status::OK();
}

}  // namespace impala
