#include "dsproxy.h"
#include "dsproxy_mon.h"
#include <ydb/core/blobstorage/base/utility.h>
#include <util/generic/set.h>

namespace NKikimr {

////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
// MULTI COLLECT request
////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////

class TBlobStorageGroupMultiCollectRequest
        : public TBlobStorageGroupRequestActor<TBlobStorageGroupMultiCollectRequest> {
    struct TRequestInfo {
        bool IsReplied;
    };

    const ui64 TabletId;
    const ui32 RecordGeneration;
    const ui32 PerGenerationCounter; // monotone increasing cmd counter for RecordGeneration
    const ui32 Channel;

    const std::unique_ptr<TVector<TLogoBlobID> > Keep;
    const std::unique_ptr<TVector<TLogoBlobID> > DoNotKeep;
    const TInstant Deadline;

    const ui32 CollectGeneration;
    const ui32 CollectStep;
    const bool Hard;
    const bool Collect;

    ui64 FlagRequestsInFlight;
    ui64 CollectRequestsInFlight;

    TInstant StartTime;

    TStackVec<TRequestInfo, TypicalDisksInGroup> RequestInfos;

    void Handle(TEvBlobStorage::TEvCollectGarbageResult::TPtr &ev) {
        const TEvBlobStorage::TEvCollectGarbageResult &res = *ev->Get();
        R_LOG_ERROR_S("BPMC1", "Handle TEvCollectGarbageResult"
            << " status# " << NKikimrProto::EReplyStatus_Name(res.Status)
            << " FlagRequestsInFlight# " << FlagRequestsInFlight
            << " CollectRequestsInFlight " << CollectRequestsInFlight);

        if (res.Status != NKikimrProto::OK) {
            ReplyAndDie(res.Status);
            return;
        }

        Y_VERIFY(ev->Cookie < RequestInfos.size());
        TRequestInfo &info = RequestInfos[ev->Cookie];
        Y_VERIFY(!info.IsReplied);
        info.IsReplied = true;

        if (FlagRequestsInFlight) {
            FlagRequestsInFlight--;
            if (FlagRequestsInFlight == 0) {
                ui64 iterations =
                    TEvBlobStorage::TEvCollectGarbage::PerGenerationCounterStepSize(Keep.get(), DoNotKeep.get());
                ui64 idx = iterations - 1;
                SendRequest(idx, true);
            }
        } else {
            CollectRequestsInFlight--;
            Y_VERIFY(CollectRequestsInFlight == 0);
            ReplyAndDie(NKikimrProto::OK);
            return;
        }
    }

    friend class TBlobStorageGroupRequestActor<TBlobStorageGroupMultiCollectRequest>;
    void ReplyAndDie(NKikimrProto::EReplyStatus status) {
        std::unique_ptr<TEvBlobStorage::TEvCollectGarbageResult> ev(new TEvBlobStorage::TEvCollectGarbageResult(
            status, TabletId, RecordGeneration, PerGenerationCounter, Channel));
        ev->ErrorReason = ErrorReason;
        SendResponseAndDie(std::move(ev));
    }

    std::unique_ptr<IEventBase> RestartQuery(ui32) {
        Y_FAIL();
    }

public:
    static constexpr NKikimrServices::TActivity::EType ActorActivityType() {
        return NKikimrServices::TActivity::BS_PROXY_MULTICOLLECT_ACTOR;
    }

    static const auto& ActiveCounter(const TIntrusivePtr<TBlobStorageGroupProxyMon>& mon) {
        return mon->ActiveMultiCollect;
    }

    TBlobStorageGroupMultiCollectRequest(const TIntrusivePtr<TBlobStorageGroupInfo> &info,
            const TIntrusivePtr<TGroupQueues> &state, const TActorId &source,
            const TIntrusivePtr<TBlobStorageGroupProxyMon> &mon, TEvBlobStorage::TEvCollectGarbage *ev, ui64 cookie,
            NWilson::TTraceId traceId, TInstant now, TIntrusivePtr<TStoragePoolCounters> &storagePoolCounters)
        : TBlobStorageGroupRequestActor(info, state, mon, source, cookie, std::move(traceId),
                NKikimrServices::BS_PROXY_MULTICOLLECT, false, {}, now, storagePoolCounters, 0,
                "DSProxy.MultiCollect")
        , TabletId(ev->TabletId)
        , RecordGeneration(ev->RecordGeneration)
        , PerGenerationCounter(ev->PerGenerationCounter)
        , Channel(ev->Channel)
        , Keep(ev->Keep.Release())
        , DoNotKeep(ev->DoNotKeep.Release())
        , Deadline(ev->Deadline)
        , CollectGeneration(ev->CollectGeneration)
        , CollectStep(ev->CollectStep)
        , Hard(ev->Hard)
        , Collect(ev->Collect)
        , FlagRequestsInFlight(0)
        , CollectRequestsInFlight(0)
        , StartTime(now)
    {}

    void SendRequest(ui64 idx, bool isLast) {
        ui64 cookie = RequestInfos.size();
        RequestInfos.push_back({false});

        bool isCollect = isLast ? Collect : false;
        std::unique_ptr<TVector<TLogoBlobID>> keepPart;
        std::unique_ptr<TVector<TLogoBlobID>> doNotKeepPart;
        ui64 keepCount = Keep ? Keep->size() : 0;
        ui64 doNotKeepCount = DoNotKeep ? DoNotKeep->size() : 0;
        ui64 keepPartCount = 0;
        ui64 doNotKeepPartCount = 0;
        ui64 begin = idx * MaxCollectGarbageFlagsPerMessage;
        ui64 end = begin + MaxCollectGarbageFlagsPerMessage;
        if (begin < keepCount) {
            keepPartCount = Min(MaxCollectGarbageFlagsPerMessage, keepCount - begin);
            keepPart.reset(new TVector<TLogoBlobID>(keepPartCount));
            for (ui64 keepIdx = 0; keepIdx < keepPartCount; ++keepIdx) {
                (*keepPart)[keepIdx] = (*Keep)[begin + keepIdx];
            }
            if (end > keepCount) {
                doNotKeepPartCount = Min(doNotKeepCount, MaxCollectGarbageFlagsPerMessage - keepPartCount);
                doNotKeepPart.reset(new TVector<TLogoBlobID>(doNotKeepPartCount));
                for (ui64 doNotKeepIdx = 0; doNotKeepIdx < doNotKeepPartCount; ++doNotKeepIdx) {
                    (*doNotKeepPart)[doNotKeepIdx] = (*DoNotKeep)[doNotKeepIdx];
                }
            }
        } else {
            ui64 doNotKeepBegin = begin - keepCount;
            doNotKeepPartCount = Min(MaxCollectGarbageFlagsPerMessage, doNotKeepCount - doNotKeepBegin);
            doNotKeepPart.reset(new TVector<TLogoBlobID>(doNotKeepPartCount));
            for (ui64 doNotKeepIdx = 0; doNotKeepIdx < doNotKeepPartCount; ++doNotKeepIdx) {
                (*doNotKeepPart)[doNotKeepIdx] = (*DoNotKeep)[doNotKeepBegin + doNotKeepIdx];
            }
        }

        std::unique_ptr<TEvBlobStorage::TEvCollectGarbage> ev(new TEvBlobStorage::TEvCollectGarbage(
            TabletId, RecordGeneration, PerGenerationCounter + idx, Channel,
            isCollect, CollectGeneration, CollectStep, keepPart.release(), doNotKeepPart.release(), Deadline, false,
            Hard));
        R_LOG_DEBUG_S("BPMC3", "SendRequest idx# " << idx
            << " isLast# " << isLast
            << " ev# " << ev->ToString());
        SendToBSProxy(SelfId(), Info->GroupID, ev.release(), cookie, Span);

        if (isLast) {
            CollectRequestsInFlight++;
        } else {
            FlagRequestsInFlight++;
        }
    }

    void Bootstrap() {
        A_LOG_INFO_S("BPMC4", "bootstrap"
            << " ActorId# " << SelfId()
            << " Group# " << Info->GroupID
            << " TabletId# " << TabletId
            << " Channel# " << Channel
            << " RecordGeneration# " << RecordGeneration
            << " PerGenerationCounter# " << PerGenerationCounter
            << " Deadline# " << Deadline
            << " CollectGeneration# " << CollectGeneration
            << " CollectStep# " << CollectStep
            << " Collect# " << (Collect ? "true" : "false")
            << " Hard# " << (Hard ? "true" : "false"));

        for (const auto& item : Keep ? *Keep : TVector<TLogoBlobID>()) {
            A_LOG_INFO_S("BPMC5", "Keep# " << item);
        }

        for (const auto& item : DoNotKeep ? *DoNotKeep : TVector<TLogoBlobID>()) {
            A_LOG_INFO_S("BPMC6", "DoNotKeep# " << item);
        }

        ui64 iterations = TEvBlobStorage::TEvCollectGarbage::PerGenerationCounterStepSize(Keep.get(), DoNotKeep.get());
        for (ui64 idx = 0; idx < iterations - 1; ++idx) {
            SendRequest(idx, false);
        }
        Become(&TThis::StateWait);
    }

    STATEFN(StateWait) {
        if (ProcessEvent(ev)) {
            return;
        }
        switch (ev->GetTypeRewrite()) {
            hFunc(TEvBlobStorage::TEvCollectGarbageResult, Handle);
        }
    }
};

IActor* CreateBlobStorageGroupMultiCollectRequest(const TIntrusivePtr<TBlobStorageGroupInfo> &info,
        const TIntrusivePtr<TGroupQueues> &state, const TActorId &source,
        const TIntrusivePtr<TBlobStorageGroupProxyMon> &mon, TEvBlobStorage::TEvCollectGarbage *ev,
        ui64 cookie, NWilson::TTraceId traceId, TInstant now, TIntrusivePtr<TStoragePoolCounters> &storagePoolCounters) {
    return new TBlobStorageGroupMultiCollectRequest(info, state, source, mon, ev, cookie, std::move(traceId), now,
            storagePoolCounters);
}

}//NKikimr
