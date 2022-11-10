#include "schemeshard__operation_part.h"
#include "schemeshard__operation_common.h"
#include "schemeshard_impl.h"

#include <ydb/core/base/subdomain.h>
#include <ydb/core/tx/tiering/cleaner_task.h>

namespace NKikimr {
namespace NSchemeShard {

namespace {

class TDropParts : public TSubOperationState {
private:
    TOperationId OperationId;

private:
    TString DebugHint() const override {
        return TStringBuilder()
                << "TDropColumnTable TDropParts"
                << " operationId#" << OperationId;
    }

public:
    TDropParts(TOperationId id)
        : OperationId(id)
    {
        IgnoreMessages(DebugHint(), {});
    }

    bool HandleReply(TEvColumnShard::TEvProposeTransactionResult::TPtr& ev, TOperationContext& context) override {
         return NTableState::CollectProposeTransactionResults(OperationId, ev, context);
    }

    bool ProgressState(TOperationContext& context) override {
        TTabletId ssId = context.SS->SelfTabletId();
        LOG_INFO_S(context.Ctx, NKikimrServices::FLAT_TX_SCHEMESHARD,
                   DebugHint() << " ProgressState"
                               << ", at schemeshard: " << ssId);

        TTxState* txState = context.SS->FindTx(OperationId);
        Y_VERIFY(txState);
        Y_VERIFY(txState->TxType == TTxState::TxDropColumnTable);

        TPathId pathId = txState->TargetPathId;

        txState->ClearShardsInProgress();

        auto seqNo = context.SS->StartRound(*txState);

        TString columnShardTxBody;
        {
            NKikimrTxColumnShard::TSchemaTxBody tx;
            context.SS->FillSeqNo(tx, seqNo);

            auto* drop = tx.MutableDropTable();

            drop->SetPathId(pathId.LocalPathId);

            Y_VERIFY(tx.SerializeToString(&columnShardTxBody));
        }

        for (auto& shard : txState->Shards) {
            Y_VERIFY(shard.TabletType == ETabletType::ColumnShard);

            TTabletId tabletId = context.SS->ShardInfos[shard.Idx].TabletID;

            {
                auto event = std::make_unique<TEvColumnShard::TEvProposeTransaction>(
                    NKikimrTxColumnShard::TX_KIND_SCHEMA,
                    context.SS->TabletID(),
                    context.Ctx.SelfID,
                    ui64(OperationId.GetTxId()),
                    columnShardTxBody,
                    context.SS->SelectProcessingPrarams(txState->TargetPathId));

                context.OnComplete.BindMsgToPipe(OperationId, tabletId, shard.Idx, event.release());
            }

            LOG_DEBUG_S(context.Ctx, NKikimrServices::FLAT_TX_SCHEMESHARD,
                        DebugHint() << " ProgressState"
                                    << " Propose modify scheme on shard"
                                    << " tabletId: " << tabletId);
        }

        txState->UpdateShardsInProgress();
        return false;
    }
};

class TPropose : public TSubOperationState {
private:
    TOperationId OperationId;

private:
    TString DebugHint() const override {
        return TStringBuilder()
                << "TDropColumnTable TPropose"
                << " operationId#" << OperationId;
    }

public:
    TPropose(TOperationId id)
        : OperationId(id)
    {
        IgnoreMessages(DebugHint(),
            {TEvColumnShard::TEvProposeTransactionResult::EventType});
    }

    bool HandleReply(TEvPrivate::TEvOperationPlan::TPtr& ev, TOperationContext& context) override {
        TStepId step = TStepId(ev->Get()->StepId);
        TTabletId ssId = context.SS->SelfTabletId();

        LOG_INFO_S(context.Ctx, NKikimrServices::FLAT_TX_SCHEMESHARD,
                   DebugHint() << " HandleReply TEvOperationPlan"
                               << " at schemeshard: " << ssId
                               << ", stepId: " << step);

        TTxState* txState = context.SS->FindTx(OperationId);
        Y_VERIFY(txState->TxType == TTxState::TxDropColumnTable);

        TPathId pathId = txState->TargetPathId;
        Y_VERIFY(context.SS->PathsById.contains(pathId));
        TPathElement::TPtr path = context.SS->PathsById.at(pathId);
        Y_VERIFY_S(context.SS->PathsById.contains(path->ParentPathId),
                   "no parent with id: " <<  path->ParentPathId << " for node with id: " << path->PathId);
        auto parentDir = context.SS->PathsById.at(path->ParentPathId);

        NIceDb::TNiceDb db(context.GetDB());

        Y_VERIFY(!path->Dropped());
        path->SetDropped(step, OperationId.GetTxId());
        context.SS->PersistDropStep(db, pathId, step, OperationId);

        auto domainInfo = context.SS->ResolveDomainInfo(pathId);
        domainInfo->DecPathsInside();
        parentDir->DecAliveChildren();

        context.SS->TabletCounters->Simple()[COUNTER_USER_ATTRIBUTES_COUNT].Sub(path->UserAttrs->Size());
        context.SS->PersistUserAttributes(db, path->PathId, path->UserAttrs, nullptr);

        ++parentDir->DirAlterVersion;
        context.SS->PersistPathDirAlterVersion(db, parentDir);
        context.SS->ClearDescribePathCaches(parentDir);
        context.SS->ClearDescribePathCaches(path);

        if (!context.SS->DisablePublicationsOfDropping) {
            context.OnComplete.PublishToSchemeBoard(OperationId, parentDir->PathId);
            context.OnComplete.PublishToSchemeBoard(OperationId, pathId);
        }

        context.SS->ChangeTxState(db, OperationId, TTxState::ProposedWaitParts);
        return true;
    }

    bool ProgressState(TOperationContext& context) override {
        TTabletId ssId = context.SS->SelfTabletId();

        LOG_INFO_S(context.Ctx, NKikimrServices::FLAT_TX_SCHEMESHARD,
                   DebugHint() << " ProgressState"
                               << " at schemeshard: " << ssId);

        TTxState* txState = context.SS->FindTx(OperationId);
        Y_VERIFY(txState);
        Y_VERIFY(txState->TxType == TTxState::TxDropColumnTable);

        TSet<TTabletId> shardSet;
        for (const auto& shard : txState->Shards) {
            TShardIdx idx = shard.Idx;
            Y_VERIFY(context.SS->ShardInfos.contains(idx));
            TTabletId tablet = context.SS->ShardInfos.at(idx).TabletID;
            shardSet.insert(tablet);
        }

        context.OnComplete.ProposeToCoordinator(OperationId, txState->TargetPathId, txState->MinStep, shardSet);
        return false;
    }
};

class TProposedWaitParts : public TSubOperationState {
private:
    TOperationId OperationId;

private:
    TString DebugHint() const override {
        return TStringBuilder()
                << "TDropColumnTable TProposedWaitParts"
                << " operationId#" << OperationId;
    }

public:
    TProposedWaitParts(TOperationId id)
        : OperationId(id)
    {
        IgnoreMessages(DebugHint(),
            {TEvColumnShard::TEvProposeTransactionResult::EventType,
             TEvPrivate::TEvOperationPlan::EventType});
    }

    bool HandleReply(TEvColumnShard::TEvNotifyTxCompletionResult::TPtr& ev, TOperationContext& context) override {
        TTxState* txState = context.SS->FindTx(OperationId);
        Y_VERIFY(txState);
        Y_VERIFY(txState->TxType == TTxState::TxDropColumnTable);

        auto shardId = TTabletId(ev->Get()->Record.GetOrigin());
        auto shardIdx = context.SS->MustGetShardIdx(shardId);
        Y_VERIFY(context.SS->ShardInfos.contains(shardIdx));

        txState->ShardsInProgress.erase(shardIdx);
        if (txState->ShardsInProgress.empty()) {
            return Finish(context);
        }

        return false;
    }

    bool Finish(TOperationContext& context) {
        NIceDb::TNiceDb db(context.GetDB());
        context.SS->ChangeTxState(db, OperationId, TTxState::ProposedDeleteParts);
        return true;
    }

    bool ProgressState(TOperationContext& context) override {
        TTabletId ssId = context.SS->SelfTabletId();

        LOG_INFO_S(context.Ctx, NKikimrServices::FLAT_TX_SCHEMESHARD,
                   DebugHint() << " ProgressState"
                               << " at schemeshard: " << ssId);

        TTxState* txState = context.SS->FindTx(OperationId);
        Y_VERIFY(txState);
        Y_VERIFY(txState->TxType == TTxState::TxDropColumnTable);

        txState->ClearShardsInProgress();

        for (auto& shard : txState->Shards) {
            Y_VERIFY(shard.TabletType == ETabletType::ColumnShard);

            TTabletId tabletId = context.SS->ShardInfos[shard.Idx].TabletID;
            auto event = std::make_unique<TEvColumnShard::TEvNotifyTxCompletion>(ui64(OperationId.GetTxId()));

            context.OnComplete.BindMsgToPipe(OperationId, tabletId, shard.Idx, event.release());
            txState->ShardsInProgress.insert(shard.Idx);

            LOG_DEBUG_S(context.Ctx, NKikimrServices::FLAT_TX_SCHEMESHARD,
                        DebugHint() << " ProgressState"
                                    << " wait for NotifyTxCompletionResult"
                                    << " tabletId: " << tabletId);
        }

        return false;
    }
};

class TProposedDeleteParts : public TSubOperationState {
private:
    TOperationId OperationId;

private:
    TString DebugHint() const override {
        return TStringBuilder()
                << "TDropColumnTable TProposedDeleteParts"
                << " operationId#" << OperationId;
    }

    bool Finish(TOperationContext& context) {
        TTxState* txState = context.SS->FindTx(OperationId);
        Y_VERIFY(txState);
        Y_VERIFY(txState->TxType == TTxState::TxDropColumnTable);

        bool isStandalone = false;
        {
            Y_VERIFY(context.SS->ColumnTables.contains(txState->TargetPathId));
            TColumnTableInfo::TPtr tableInfo = context.SS->ColumnTables.at(txState->TargetPathId);
            Y_VERIFY(tableInfo);
            isStandalone = tableInfo->IsStandalone();
        }

        NIceDb::TNiceDb db(context.GetDB());
        context.SS->PersistColumnTableRemove(db, txState->TargetPathId);

        if (isStandalone) {
            for (auto& shard : txState->Shards) {
                context.OnComplete.DeleteShard(shard.Idx);
            }
        }

        context.OnComplete.DoneOperation(OperationId);
        return true;
    }
public:
    TProposedDeleteParts(TOperationId id)
        : OperationId(id)
    {
        IgnoreMessages(DebugHint(),
            {TEvColumnShard::TEvProposeTransactionResult::EventType,
             TEvColumnShard::TEvNotifyTxCompletionResult::EventType,
             TEvPrivate::TEvOperationPlan::EventType});
    }

    bool HandleReply(NBackgroundTasks::TEvAddTaskResult::TPtr& ev, TOperationContext& context) override {
        Y_VERIFY(ev->Get()->IsSuccess());
        return Finish(context);
    }

    bool ProgressState(TOperationContext& context) override {
        TTabletId ssId = context.SS->SelfTabletId();
        TTxState* txState = context.SS->FindTx(OperationId);
        Y_VERIFY(txState);
        Y_VERIFY(txState->TxType == TTxState::TxDropColumnTable);

        LOG_INFO_S(context.Ctx, NKikimrServices::FLAT_TX_SCHEMESHARD,
                   DebugHint() << " ProgressState"
                               << ", at schemeshard: " << ssId);

        if (NBackgroundTasks::TServiceOperator::IsEnabled()) {
            NSchemeShard::TPath path = NSchemeShard::TPath::Init(txState->TargetPathId, context.SS);
            TColumnTableInfo::TPtr tableInfo = context.SS->ColumnTables.at(path.Base()->PathId);
            NSchemeShard::TPath ownerPath = tableInfo->IsStandalone() ? path : path.FindOlapStore();
            Y_VERIFY(path.IsResolved());
            NBackgroundTasks::TTask task(std::make_shared<NColumnShard::NTiers::TTaskCleanerActivity>(txState->TargetPathId.LocalPathId, ownerPath.PathString()), nullptr);
            task.SetId(OperationId.SerializeToString());
            context.SS->SelfId().Send(NBackgroundTasks::MakeServiceId(context.SS->SelfId().NodeId()), new NBackgroundTasks::TEvAddTask(std::move(task)));
            return false;
        } else {
            return Finish(context);
        }
    }
};

class TDropColumnTable : public TSubOperation {
public:
    TDropColumnTable(TOperationId id, const TTxTransaction& tx)
        : OperationId(id)
        , Transaction(tx)
    {}

    TDropColumnTable(TOperationId id, TTxState::ETxState state)
        : OperationId(id)
        , State(state)
    {
        SetState(SelectStateFunc(state));
    }

public:
    THolder<TProposeResponse> Propose(const TString&, TOperationContext& context) override {
        const TTabletId ssId = context.SS->SelfTabletId();

        const auto& drop = Transaction.GetDrop();

        const TString& parentPathStr = Transaction.GetWorkingDir();
        const TString& name = drop.GetName();
        auto opTxId = OperationId.GetTxId();

        LOG_NOTICE_S(context.Ctx, NKikimrServices::FLAT_TX_SCHEMESHARD,
                     "TDropColumnTable Propose"
                         << ", path: " << parentPathStr << "/" << name
                         << ", pathId: " << drop.GetId()
                         << ", opId: " << OperationId
                         << ", at schemeshard: " << ssId);

        auto result = MakeHolder<TProposeResponse>(NKikimrScheme::StatusAccepted, ui64(opTxId), ui64(ssId));

        TPath path = drop.HasId()
            ? TPath::Init(context.SS->MakeLocalId(drop.GetId()), context.SS)
            : TPath::Resolve(parentPathStr, context.SS).Dive(name);

        {
            TPath::TChecker checks = path.Check();
            checks
                .NotEmpty()
                .NotUnderDomainUpgrade()
                .IsAtLocalSchemeShard()
                .IsResolved()
                .NotDeleted()
                .IsColumnTable()
                .NotUnderDeleting()
                .NotUnderOperation();

            if (!checks) {
                result->SetError(checks.GetStatus(), checks.GetError());
                if (path.IsResolved() && path.Base()->IsColumnTable() && (path.Base()->PlannedToDrop() || path.Base()->Dropped())) {
                    result->SetPathDropTxId(ui64(path.Base()->DropTxId));
                    result->SetPathId(path.Base()->PathId.LocalPathId);
                }
                return result;
            }
        }

        TPath parent = path.Parent();
        {
            TPath::TChecker checks = parent.Check();
            checks
                .NotEmpty()
                .IsResolved()
                .NotDeleted()
                .IsLikeDirectory()
                .IsCommonSensePath()
                .NotUnderDeleting();

            if (!checks) {
                result->SetError(checks.GetStatus(), checks.GetError());
                return result;
            }
        }

        TString errStr;
        if (!context.SS->CheckApplyIf(Transaction, errStr)) {
            result->SetError(NKikimrScheme::StatusPreconditionFailed, errStr);
            return result;
        }
        if (!context.SS->CheckInFlightLimit(TTxState::TxDropColumnTable, errStr)) {
            result->SetError(NKikimrScheme::StatusResourceExhausted, errStr);
            return result;
        }

        TTxState& txState = context.SS->CreateTx(OperationId, TTxState::TxDropColumnTable, path.Base()->PathId);
        txState.State = TTxState::DropParts;
        // Dirty hack: drop step must not be zero because 0 is treated as "hasn't been dropped"
        txState.MinStep = TStepId(1);

        NIceDb::TNiceDb db(context.GetDB());

        Y_VERIFY(context.SS->ColumnTables.contains(path.Base()->PathId));
        TColumnTableInfo::TPtr tableInfo = context.SS->ColumnTables.at(path.Base()->PathId);

        if (tableInfo->IsStandalone()) {
            for (auto shardIdx : tableInfo->OwnedColumnShards) {
                Y_VERIFY_S(context.SS->ShardInfos.contains(shardIdx), "Unknown shardIdx " << shardIdx);
                txState.Shards.emplace_back(shardIdx, context.SS->ShardInfos[shardIdx].TabletType, TTxState::DropParts);

                context.SS->ShardInfos[shardIdx].CurrentTxId = opTxId;
                context.SS->PersistShardTx(db, shardIdx, opTxId);
            }
        } else {
            auto& storePathId = *tableInfo->OlapStorePathId;
            TPath storePath = TPath::Init(storePathId, context.SS);
            {
                TPath::TChecker checks = storePath.Check();
                checks
                    .NotEmpty()
                    .IsResolved()
                    .IsOlapStore()
                    .NotUnderOperation();

                if (!checks) {
                    result->SetError(checks.GetStatus(), checks.GetError());
                    return result;
                }
            }

            Y_VERIFY(context.SS->OlapStores.contains(storePathId));
            TOlapStoreInfo::TPtr storeInfo = context.SS->OlapStores.at(storePathId);

            Y_VERIFY(storeInfo->ColumnTables.contains(path->PathId));
            storeInfo->ColumnTablesUnderOperation.insert(path->PathId);

            // Sequentially chain operations in the same olap store
            if (context.SS->Operations.contains(storePath.Base()->LastTxId)) {
                context.OnComplete.Dependence(storePath.Base()->LastTxId, opTxId);
            }
            storePath.Base()->LastTxId = opTxId;
            context.SS->PersistLastTxId(db, storePath.Base());

            // TODO: we need to know all shards where this table has ever been created
            for (ui64 columnShardId : tableInfo->ColumnShards) {
                auto tabletId = TTabletId(columnShardId);
                auto shardIdx = context.SS->TabletIdToShardIdx.at(tabletId);

                Y_VERIFY_S(context.SS->ShardInfos.contains(shardIdx), "Unknown shardIdx " << shardIdx);
                txState.Shards.emplace_back(shardIdx, context.SS->ShardInfos[shardIdx].TabletType, TTxState::DropParts);

                context.SS->ShardInfos[shardIdx].CurrentTxId = opTxId;
                context.SS->PersistShardTx(db, shardIdx, opTxId);
            }
        }

        context.OnComplete.ActivateTx(OperationId);

        path.Base()->PathState = TPathElement::EPathState::EPathStateDrop;
        path.Base()->DropTxId = opTxId;
        path.Base()->LastTxId = opTxId;
        context.SS->PersistLastTxId(db, path.Base());

        context.SS->PersistTxState(db, OperationId);

        context.SS->TabletCounters->Simple()[COUNTER_COLUMN_TABLE_COUNT].Sub(1);

        Y_VERIFY_S(context.SS->PathsById.contains(path.Base()->ParentPathId),
                   "no parent with id: " << path.Base()->ParentPathId << " for node with id: " << path.Base()->PathId);
        ++parent.Base()->DirAlterVersion;
        context.SS->PersistPathDirAlterVersion(db, parent.Base());
        context.SS->ClearDescribePathCaches(parent.Base());
        context.SS->ClearDescribePathCaches(path.Base());

        if (!context.SS->DisablePublicationsOfDropping) {
            context.OnComplete.PublishToSchemeBoard(OperationId, parent.Base()->PathId);
            context.OnComplete.PublishToSchemeBoard(OperationId, path.Base()->PathId);
        }

        State = NextState();
        SetState(SelectStateFunc(State));
        return result;
    }

    void AbortPropose(TOperationContext&) override {
        Y_FAIL("no AbortPropose for TDropColumnTable");
    }

    void AbortUnsafe(TTxId forceDropTxId, TOperationContext& context) override {
        LOG_NOTICE_S(context.Ctx, NKikimrServices::FLAT_TX_SCHEMESHARD,
                     "TDropColumnTable AbortUnsafe"
                         << ", opId: " << OperationId
                         << ", forceDropId: " << forceDropTxId
                         << ", at schemeshard: " << context.SS->TabletID());

        TTxState* txState = context.SS->FindTx(OperationId);
        Y_VERIFY(txState);

        TPathId pathId = txState->TargetPathId;
        Y_VERIFY(context.SS->PathsById.contains(pathId));
        TPathElement::TPtr path = context.SS->PathsById.at(pathId);
        Y_VERIFY(path);

        if (path->Dropped()) {
            // We don't really need to do anything
        }

        context.OnComplete.DoneOperation(OperationId);
    }

private:
    TTxState::ETxState NextState() {
        return TTxState::DropParts;
    }

    TTxState::ETxState NextState(TTxState::ETxState state) {
        switch(state) {
        case TTxState::DropParts:
            return TTxState::Propose;
        case TTxState::Propose:
            return TTxState::ProposedWaitParts;
        case TTxState::ProposedWaitParts:
            return TTxState::ProposedDeleteParts;
        default:
            return TTxState::Invalid;
        }
    }

    TSubOperationState::TPtr SelectStateFunc(TTxState::ETxState state) {
        switch(state) {
        case TTxState::DropParts:
            return MakeHolder<TDropParts>(OperationId);
        case TTxState::Propose:
            return MakeHolder<TPropose>(OperationId);
        case TTxState::ProposedWaitParts:
            return MakeHolder<TProposedWaitParts>(OperationId);
        case TTxState::ProposedDeleteParts:
            return MakeHolder<TProposedDeleteParts>(OperationId);
        default:
            return nullptr;
        }
    }

    void StateDone(TOperationContext& context) override {
        State = NextState(State);

        if (State != TTxState::Invalid) {
            SetState(SelectStateFunc(State));
            context.OnComplete.ActivateTx(OperationId);
        }
    }

private:
    const TOperationId OperationId;
    TTxState::ETxState State = TTxState::Invalid;
    const NKikimrSchemeOp::TModifyScheme Transaction;
};

} // namespace

ISubOperationBase::TPtr CreateDropColumnTable(TOperationId id, const TTxTransaction& tx) {
    return new TDropColumnTable(id, tx);
}

ISubOperationBase::TPtr CreateDropColumnTable(TOperationId id, TTxState::ETxState state) {
    Y_VERIFY(state != TTxState::Invalid);
    return new TDropColumnTable(id, state);
}

} // namespace NSchemeShard
} // namespace NKikimr
