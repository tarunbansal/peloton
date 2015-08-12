//===----------------------------------------------------------------------===//
//
//                         PelotonDB
//
// seq_scan_executor.cpp
//
// Identification: src/backend/executor/seq_scan_executor.cpp
//
// Copyright (c) 2015, Carnegie Mellon University Database Group
//
//===----------------------------------------------------------------------===//

#include "backend/executor/seq_scan_executor.h"

#include <memory>
#include <utility>
#include <vector>

#include "backend/common/types.h"
#include "backend/executor/logical_tile.h"
#include "backend/executor/logical_tile_factory.h"
#include "backend/expression/abstract_expression.h"
#include "backend/expression/expression.h"
#include "backend/expression/container_tuple.h"
#include "backend/storage/data_table.h"
#include "backend/storage/tile_group.h"

#include "backend/common/logger.h"

namespace peloton {
namespace executor {

/**
 * @brief Constructor for seqscan executor.
 * @param node Seqscan node corresponding to this executor.
 */
SeqScanExecutor::SeqScanExecutor(planner::AbstractPlan *node,
                                 ExecutorContext *executor_context)
    : AbstractScanExecutor(node, executor_context) {}

/**
 * @brief Let base class DInit() first, then do mine.
 * @return true on success, false otherwise.
 */
bool SeqScanExecutor::DInit() {
  auto status = AbstractScanExecutor::DInit();

  if (!status) return false;

  current_tile_group_offset_ = START_OID;

  return true;
}

/**
 * @brief Creates logical tile from tile group and applies scan predicate.
 * @return true on success, false otherwise.
 */
bool SeqScanExecutor::DExecute() {

  // Scanning over a logical tile.
  if (children_.size() == 1) {
    LOG_TRACE("Seq Scan executor :: 1 child \n");

    assert(target_table_ == nullptr);

    if (!children_[0]->Execute()) {
      return false;
    }

    std::unique_ptr<LogicalTile> tile(children_[0]->GetOutput());
    if (predicate_ != nullptr) {
      // Invalidate tuples that don't satisfy the predicate.
      for (oid_t tuple_id : *tile) {
        expression::ContainerTuple<LogicalTile> tuple(tile.get(), tuple_id);
        if (predicate_->Evaluate(&tuple, nullptr, executor_context_)
                .IsFalse()) {
          tile->RemoveVisibility(tuple_id);
        }
      }
    }

    /* Hopefully we needn't do projections here */

    SetOutput(tile.release());
  }
  // Scanning a table
  else if (children_.size() == 0) {
    LOG_TRACE("Seq Scan executor :: 0 child \n");

    const planner::SeqScanPlan &node = GetPlanNode<planner::SeqScanPlan>();
    target_table_ = node.GetTable();
    assert(target_table_ != nullptr);

    table_tile_group_count_ = target_table_->GetTileGroupCount();
    if (column_ids_.empty()) {
      column_ids_.resize(target_table_->GetSchema()->GetColumnCount());
      std::iota(column_ids_.begin(), column_ids_.end(), 0);
    }

    // Retrieve next tile group.
    if (current_tile_group_offset_ == table_tile_group_count_) {
      return false;
    }

    LOG_TRACE("Current : %u Count : %u", current_tile_group_offset_,
             table_tile_group_count_);

    storage::TileGroup *tile_group =
        target_table_->GetTileGroup(current_tile_group_offset_++);

    storage::TileGroupHeader *tile_group_header = tile_group->GetHeader();
    auto transaction_ = executor_context_->GetTransaction();
    txn_id_t txn_id = transaction_->GetTransactionId();
    cid_t commit_id = transaction_->GetLastCommitId();
    oid_t active_tuple_count = tile_group->GetNextTupleSlot();

    // Print tile group visibility
    // tile_group_header->PrintVisibility(txn_id, commit_id);

    // Construct position list by looping through tile group
    // and applying the predicate.
    std::vector<oid_t> position_list;
    for (oid_t tuple_id = 0; tuple_id < active_tuple_count; tuple_id++) {
      if (tile_group_header->IsVisible(tuple_id, txn_id, commit_id) == false) {
        continue;
      }

      expression::ContainerTuple<storage::TileGroup> tuple(tile_group,
                                                           tuple_id);
      if (predicate_ == nullptr ||
          predicate_->Evaluate(&tuple, nullptr, executor_context_).IsTrue()) {
        position_list.push_back(tuple_id);
      }
    }

    // Construct logical tile.
    std::unique_ptr<LogicalTile> logical_tile(LogicalTileFactory::GetTile());
    const bool own_base_tile = false;
    const int position_list_idx = 0;
    logical_tile->AddPositionList(std::move(position_list));

    for (oid_t origin_column_id : column_ids_) {
      oid_t base_tile_offset, tile_column_id;

      tile_group->LocateTileAndColumn(origin_column_id, base_tile_offset,
                                      tile_column_id);

      logical_tile->AddColumn(tile_group->GetTile(base_tile_offset),
                              own_base_tile, tile_column_id, position_list_idx);
    }

    SetOutput(logical_tile.release());
  }

  return true;
}

}  // namespace executor
}  // namespace peloton
