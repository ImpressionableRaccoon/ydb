#pragma once
#include <util/generic/set.h>
#include <util/system/hp_timer.h>

#include <ydb/core/base/quoter.h>
#include <ydb/core/keyvalue/keyvalue_events.h>
#include <library/cpp/actors/core/actor.h>
#include <library/cpp/actors/core/hfunc.h>
#include <library/cpp/actors/core/log.h>
#include <library/cpp/sliding_window/sliding_window.h>
#include <ydb/core/protos/pqconfig.pb.h>
#include <ydb/core/persqueue/events/internal.h>
#include <ydb/library/persqueue/counter_time_keeper/counter_time_keeper.h>
#include <ydb/library/persqueue/topic_parser/topic_parser.h>

#include "key.h"
#include "blob.h"
#include "subscriber.h"
#include "header.h"
#include "user_info.h"
#include "sourceid.h"
#include "ownerinfo.h"

#include <variant>

namespace NKikimr {
namespace NPQ {

class TKeyLevel;

static const ui32 MAX_BLOB_PART_SIZE = 500 << 10; //500Kb

typedef TProtobufTabletLabeledCounters<EPartitionLabeledCounters_descriptor> TPartitionLabeledCounters;


struct TDataKey {
    TKey Key;
    ui32 Size;
    TInstant Timestamp;
    ui64 CumulativeSize;
};

ui64 GetOffsetEstimate(const std::deque<TDataKey>& container, TInstant timestamp, ui64 headOffset);

struct TMirrorerInfo;

class TPartition : public TActorBootstrapped<TPartition> {
private:
    static constexpr ui32 MAX_ERRORS_COUNT_TO_STORE = 10;

private:
    struct THasDataReq;
    struct THasDataDeadline;

    //answer for requests when data arrives and drop deadlined requests
    void ProcessHasDataRequests(const TActorContext& ctx);

    void FillReadFromTimestamps(const NKikimrPQ::TPQTabletConfig& config, const TActorContext& ctx);
    void ProcessUserActs(TUserInfo& userInfo, const TActorContext& ctx);

    void ReplyError(const TActorContext& ctx, const ui64 dst, NPersQueue::NErrorCode::EErrorCode errorCode, const TString& error);
    void ReplyErrorForStoredWrites(const TActorContext& ctx);
    void ReplyOk(const TActorContext& ctx, const ui64 dst);
    void ReplyWrite(
        const TActorContext& ctx, ui64 dst, const TString& sourceId, ui64 seqNo, ui16 partNo, ui16 totalParts,
        ui64 offset, TInstant writeTimestamp, bool already, ui64 maxSeqNo,
        ui64 partitionQuotedTime, TDuration topicQuotedTime, ui64 queueTime, ui64 writeTime);
    void ReplyGetClientOffsetOk(const TActorContext& ctx, const ui64 dst, const i64 offset,
        const TInstant writeTimestamp, const TInstant createTimestamp);

    void ReplyOwnerOk(const TActorContext& ctx, const ui64 dst, const TString& ownerCookie);

    void Handle(TEvPersQueue::TEvHasDataInfo::TPtr& ev, const TActorContext& ctx);

    void Handle(TEvPQ::TEvMirrorerCounters::TPtr& ev, const TActorContext& ctx);
    void Handle(NReadSpeedLimiterEvents::TEvCounters::TPtr& ev, const TActorContext& ctx);

    //answer for reads for Timestamps
    void Handle(TEvPQ::TEvProxyResponse::TPtr& ev, const TActorContext& ctx);
    void Handle(TEvPQ::TEvError::TPtr& ev, const TActorContext& ctx);
    void ProcessTimestampRead(const TActorContext& ctx);

    void HandleOnInit(TEvKeyValue::TEvResponse::TPtr& ev, const TActorContext& ctx);

    void HandleGetDiskStatus(const NKikimrClient::TResponse& res, const TActorContext& ctx);
    void HandleInfoRangeRead(const NKikimrClient::TKeyValueResponse::TReadRangeResult& range, const TActorContext& ctx);
    void HandleDataRangeRead(const NKikimrClient::TKeyValueResponse::TReadRangeResult& range, const TActorContext& ctx);
    void HandleMetaRead(const NKikimrClient::TKeyValueResponse::TReadResult& response, const TActorContext& ctx);

    //forms DataKeysBody and other partition's info
    //ctx here only for logging
    void FillBlobsMetaData(const NKikimrClient::TKeyValueResponse::TReadRangeResult& range, const TActorContext& ctx);
    //will form head and request data keys from head or finish initialization
    void FormHeadAndProceed(const TActorContext& ctx);
    void HandleDataRead(const NKikimrClient::TResponse& range, const TActorContext& ctx);
    void InitComplete(const TActorContext& ctx);


    void Handle(TEvPQ::TEvChangeOwner::TPtr& ev, const TActorContext& ctx);
    void ProcessChangeOwnerRequests(const TActorContext& ctx);
    void ProcessChangeOwnerRequest(TAutoPtr<TEvPQ::TEvChangeOwner> ev, const TActorContext& ctx);

    void Handle(TEvPQ::TEvBlobResponse::TPtr& ev, const TActorContext& ctx);

    void Handle(TEvPQ::TEvChangePartitionConfig::TPtr& ev, const TActorContext& ctx);

    void Handle(TEvPQ::TEvGetClientOffset::TPtr& ev, const TActorContext& ctx);

    void Handle(TEvPQ::TEvUpdateWriteTimestamp::TPtr& ev, const TActorContext& ctx);

    void Handle(TEvPQ::TEvSetClientInfo::TPtr& ev, const TActorContext& ctx);
    void WriteClientInfo(const ui64 cookie, TUserInfo& ui,  const TActorContext& ctx);


    void HandleOnInit(TEvPQ::TEvPartitionOffsets::TPtr& ev, const TActorContext& ctx);
    void HandleOnInit(TEvPQ::TEvPartitionStatus::TPtr& ev, const TActorContext& ctx);
    void Handle(TEvPQ::TEvPartitionOffsets::TPtr& ev, const TActorContext& ctx);
    void Handle(TEvPQ::TEvPartitionStatus::TPtr& ev, const TActorContext& ctx);
    void Handle(TEvPQ::TEvGetPartitionClientInfo::TPtr& ev, const TActorContext& ctx);

    void Handle(TEvPersQueue::TEvReportPartitionError::TPtr& ev, const TActorContext& ctx);
    void LogAndCollectError(const NKikimrPQ::TStatusResponse::TErrorMessage& error, const TActorContext& ctx);
    void LogAndCollectError(NKikimrServices::EServiceKikimr service, const TString& msg, const TActorContext& ctx);

    void HandleOnIdle(TEvPQ::TEvUpdateAvailableSize::TPtr& ev, const TActorContext& ctx);
    void HandleOnWrite(TEvPQ::TEvUpdateAvailableSize::TPtr& ev, const TActorContext& ctx);

    void Handle(TEvPQ::TEvQuotaDeadlineCheck::TPtr& ev, const TActorContext& ctx);

    void UpdateAvailableSize(const TActorContext& ctx);
    void ScheduleUpdateAvailableSize(const TActorContext& ctx);

    void Handle(TEvPQ::TEvGetMaxSeqNoRequest::TPtr& ev, const TActorContext& ctx);

    void Handle(TEvPQ::TEvReadTimeout::TPtr& ev, const TActorContext& ctx);
    void HandleWakeup(const TActorContext& ctx);
    void Handle(TEvents::TEvPoisonPill::TPtr& ev, const TActorContext& ctx);

    void Handle(TEvPQ::TEvRead::TPtr& ev, const TActorContext& ctx);
    void Handle(NReadSpeedLimiterEvents::TEvResponse::TPtr& ev, const TActorContext& ctx);
    void DoRead(TEvPQ::TEvRead::TPtr ev, TDuration waitQuotaTime, const TActorContext& ctx);
    void OnReadRequestFinished(TReadInfo&& info, ui64 answerSize);

    // will return rcount and rsize also
    TVector<TRequestedBlob> GetReadRequestFromBody(const ui64 startOffset, const ui16 partNo, const ui32 maxCount, const ui32 maxSize, ui32* rcount, ui32* rsize);
    TVector<TClientBlob>    GetReadRequestFromHead(const ui64 startOffset, const ui16 partNo, const ui32 maxCount, const ui32 maxSize, const ui64 readTimestampMs, ui32* rcount, ui32* rsize, ui64* insideHeadOffset);
    void ProcessRead(const TActorContext& ctx, TReadInfo&& info, const ui64 cookie, bool subscription);

    void HandleOnIdle(TEvPQ::TEvWrite::TPtr& ev, const TActorContext& ctx);
    void HandleOnWrite(TEvPQ::TEvWrite::TPtr& ev, const TActorContext& ctx);

    void HandleOnIdle(TEvPQ::TEvRegisterMessageGroup::TPtr& ev, const TActorContext& ctx);
    void HandleOnWrite(TEvPQ::TEvRegisterMessageGroup::TPtr& ev, const TActorContext& ctx);

    void HandleOnIdle(TEvPQ::TEvDeregisterMessageGroup::TPtr& ev, const TActorContext& ctx);
    void HandleOnWrite(TEvPQ::TEvDeregisterMessageGroup::TPtr& ev, const TActorContext& ctx);

    void HandleOnIdle(TEvPQ::TEvSplitMessageGroup::TPtr& ev, const TActorContext& ctx);
    void HandleOnWrite(TEvPQ::TEvSplitMessageGroup::TPtr& ev, const TActorContext& ctx);

    void Handle(TEvQuota::TEvClearance::TPtr& ev, const TActorContext& ctx);
    bool CleanUp(TEvKeyValue::TEvRequest* request, bool hasWrites, const TActorContext& ctx);

    //will fill sourceIds, request and NewHead
    //returns true if head is compacted
    bool AppendHeadWithNewWrites(TEvKeyValue::TEvRequest* request, const TActorContext& ctx, TSourceIdWriter& sourceIdWriter);
    std::pair<TKey, ui32> GetNewWriteKey(bool headCleared);
    void AddNewWriteBlob(std::pair<TKey, ui32>& res, TEvKeyValue::TEvRequest* request, bool headCleared, const TActorContext& ctx);

    bool ProcessWrites(TEvKeyValue::TEvRequest* request, const TActorContext& ctx);
    void FilterDeadlinedWrites(const TActorContext& ctx);
    void SetDeadlinesForWrites(const TActorContext& ctx);

    void ReadTimestampForOffset(const TString& user, TUserInfo& ui, const TActorContext& ctx);
    void ProcessTimestampsForNewData(const ui64 prevEndOffset, const TActorContext& ctx);
    void ReportLabeledCounters(const TActorContext& ctx);
    ui64 GetSizeLag(i64 offset);

    void Handle(TEvKeyValue::TEvResponse::TPtr& ev, const TActorContext& ctx);
    void HandleSetOffsetResponse(NKikimrClient::TResponse& response, const TActorContext& ctx);
    void HandleWriteResponse(const TActorContext& ctx);
    void Handle(TEvPQ::TEvHandleWriteResponse::TPtr&, const TActorContext& ctx);


    void AnswerCurrentWrites(const TActorContext& ctx);
    void SyncMemoryStateWithKVState(const TActorContext& ctx);

    //only Writes container is filled; only DISK_IS_FULL can be here
    void CancelAllWritesOnIdle(const TActorContext& ctx);
    //additional contaiters are half-filled, need to clear them too
    struct TWriteMsg; // forward
    void CancelAllWritesOnWrite(const TActorContext& ctx, TEvKeyValue::TEvRequest* request, const TString& errorStr,
                                const TWriteMsg& p, TSourceIdWriter& sourceIdWriter, NPersQueue::NErrorCode::EErrorCode errorCode);


    void FailBadClient(const TActorContext& ctx);
    void ClearOldHead(const ui64 offset, const ui16 partNo, TEvKeyValue::TEvRequest* request);

    void HandleMonitoring(TEvPQ::TEvMonRequest::TPtr& ev, const TActorContext& ctx);

    void InitUserInfoForImportantClients(const TActorContext& ctx);


    THashMap<TString, TOwnerInfo>::iterator DropOwner(THashMap<TString, TOwnerInfo>::iterator& it, const TActorContext& ctx);

    void Handle(TEvPQ::TEvPipeDisconnected::TPtr& ev, const TActorContext& ctx);

    void Handle(TEvPQ::TEvReserveBytes::TPtr& ev, const TActorContext& ctx);
    void ProcessReserveRequests(const TActorContext& ctx);

    void CreateMirrorerActor();
    bool IsQuotingEnabled() const;

    void SetupTopicCounters(const TActorContext& ctx);
    void SetupStreamCounters(const TActorContext& ctx);

    ui64 GetUsedStorage(const TActorContext& ctx);

public:
    static constexpr NKikimrServices::TActivity::EType ActorActivityType() {
        return NKikimrServices::TActivity::PERSQUEUE_PARTITION_ACTOR;
    }

    TPartition(ui64 tabletId, ui32 partition, const TActorId& tablet, const TActorId& blobCache,
               const NPersQueue::TTopicConverterPtr& topicConverter, bool isLocalDC, TString dcId,
               const NKikimrPQ::TPQTabletConfig& config, const TTabletCountersBase& counters,
               const TActorContext& ctx, bool newPartition = false);

    void Bootstrap(const TActorContext& ctx);


    //Bootstrap sends kvRead
    //Become StateInit
    //StateInit
    //wait for correct result, cache all
    //Become StateIdle
    //StateIdle
    //got read - make kvRead
    //got kvReadResult - answer read
    //got write - make kvWrite, Become StateWrite
    //StateWrite
    // got read - ...
    // got kwReadResult - ...
    //got write - store it inflight
    //got kwWriteResult - check it, become StateIdle of StateWrite(and write inflight)

private:
    template <typename TEv>
    TString EventStr(const char * func, const TEv& ev) {
        TStringStream ss;
        ss << func << " event# " << ev->GetTypeRewrite() << " (" << ev->GetBase()->ToStringHeader() << "), Tablet " << Tablet << ", Partition " << Partition
           << ", Sender " << ev->Sender.ToString() << ", Recipient " << ev->Recipient.ToString() << ", Cookie: " << ev->Cookie;
        return ss.Str();
    }

    STFUNC(StateInit)
    {
        NPersQueue::TCounterTimeKeeper keeper(Counters.Cumulative()[COUNTER_PQ_TABLET_CPU_USAGE]);

        LOG_TRACE_S(ctx, NKikimrServices::PERSQUEUE, EventStr("StateInit", ev));

        TRACE_EVENT(NKikimrServices::PERSQUEUE);
        switch (ev->GetTypeRewrite()) {
            CFunc(TEvents::TSystem::Wakeup, HandleWakeup);
            HFuncTraced(TEvKeyValue::TEvResponse, HandleOnInit); //result of reads
            HFuncTraced(TEvents::TEvPoisonPill, Handle);
            HFuncTraced(TEvPQ::TEvMonRequest, HandleMonitoring);
            HFuncTraced(TEvPQ::TEvChangePartitionConfig, Handle);
            HFuncTraced(TEvPQ::TEvPartitionOffsets, HandleOnInit);
            HFuncTraced(TEvPQ::TEvPartitionStatus, HandleOnInit);
            HFuncTraced(TEvPersQueue::TEvReportPartitionError, Handle);
            HFuncTraced(TEvPersQueue::TEvHasDataInfo, Handle);
            HFuncTraced(TEvPQ::TEvMirrorerCounters, Handle);
            HFuncTraced(NReadSpeedLimiterEvents::TEvCounters, Handle);
            HFuncTraced(TEvPQ::TEvGetPartitionClientInfo, Handle);
        default:
            LOG_ERROR_S(ctx, NKikimrServices::PERSQUEUE, "Unexpected " << EventStr("StateInit", ev));
            break;
        };
    }

    STFUNC(StateIdle)
    {
        NPersQueue::TCounterTimeKeeper keeper(Counters.Cumulative()[COUNTER_PQ_TABLET_CPU_USAGE]);

        LOG_TRACE_S(ctx, NKikimrServices::PERSQUEUE, EventStr("StateIdle", ev));

        TRACE_EVENT(NKikimrServices::PERSQUEUE);
        switch (ev->GetTypeRewrite()) {
            CFunc(TEvents::TSystem::Wakeup, HandleWakeup);
            HFuncTraced(TEvKeyValue::TEvResponse, Handle);
            HFuncTraced(TEvPQ::TEvBlobResponse, Handle);
            HFuncTraced(TEvPQ::TEvWrite, HandleOnIdle);
            HFuncTraced(TEvPQ::TEvRead, Handle);
            HFuncTraced(NReadSpeedLimiterEvents::TEvResponse, Handle);
            HFuncTraced(TEvPQ::TEvReadTimeout, Handle);
            HFuncTraced(TEvents::TEvPoisonPill, Handle);
            HFuncTraced(TEvPQ::TEvMonRequest, HandleMonitoring);
            HFuncTraced(TEvPQ::TEvGetMaxSeqNoRequest, Handle);
            HFuncTraced(TEvPQ::TEvChangePartitionConfig, Handle);
            HFuncTraced(TEvPQ::TEvGetClientOffset, Handle);
            HFuncTraced(TEvPQ::TEvUpdateWriteTimestamp, Handle);
            HFuncTraced(TEvPQ::TEvSetClientInfo, Handle);
            HFuncTraced(TEvPQ::TEvPartitionOffsets, Handle);
            HFuncTraced(TEvPQ::TEvPartitionStatus, Handle);
            HFuncTraced(TEvPersQueue::TEvReportPartitionError, Handle);
            HFuncTraced(TEvPQ::TEvChangeOwner, Handle);
            HFuncTraced(TEvPersQueue::TEvHasDataInfo, Handle);
            HFuncTraced(TEvPQ::TEvMirrorerCounters, Handle);
            HFuncTraced(NReadSpeedLimiterEvents::TEvCounters, Handle);
            HFuncTraced(TEvPQ::TEvProxyResponse, Handle);
            HFuncTraced(TEvPQ::TEvError, Handle);
            HFuncTraced(TEvPQ::TEvGetPartitionClientInfo, Handle);
            HFuncTraced(TEvPQ::TEvUpdateAvailableSize, HandleOnIdle);
            HFuncTraced(TEvPQ::TEvReserveBytes, Handle);
            HFuncTraced(TEvPQ::TEvPipeDisconnected, Handle);
            HFuncTraced(TEvQuota::TEvClearance, Handle);
            HFuncTraced(TEvPQ::TEvQuotaDeadlineCheck, Handle);
            HFuncTraced(TEvPQ::TEvRegisterMessageGroup, HandleOnIdle);
            HFuncTraced(TEvPQ::TEvDeregisterMessageGroup, HandleOnIdle);
            HFuncTraced(TEvPQ::TEvSplitMessageGroup, HandleOnIdle);

        default:
            LOG_ERROR_S(ctx, NKikimrServices::PERSQUEUE, "Unexpected " << EventStr("StateIdle", ev));
            break;
        };
    }

    STFUNC(StateWrite)
    {
        NPersQueue::TCounterTimeKeeper keeper(Counters.Cumulative()[COUNTER_PQ_TABLET_CPU_USAGE]);

        LOG_TRACE_S(ctx, NKikimrServices::PERSQUEUE, EventStr("StateWrite", ev));

        TRACE_EVENT(NKikimrServices::PERSQUEUE);
        switch (ev->GetTypeRewrite()) {
            CFunc(TEvents::TSystem::Wakeup, HandleWakeup);
            HFuncTraced(TEvKeyValue::TEvResponse, Handle);
            HFuncTraced(TEvPQ::TEvHandleWriteResponse, Handle);
            HFuncTraced(TEvPQ::TEvBlobResponse, Handle);
            HFuncTraced(TEvPQ::TEvWrite, HandleOnWrite);
            HFuncTraced(TEvPQ::TEvRead, Handle);
            HFuncTraced(NReadSpeedLimiterEvents::TEvResponse, Handle);
            HFuncTraced(TEvPQ::TEvReadTimeout, Handle);
            HFuncTraced(TEvents::TEvPoisonPill, Handle);
            HFuncTraced(TEvPQ::TEvMonRequest, HandleMonitoring);
            HFuncTraced(TEvPQ::TEvGetMaxSeqNoRequest, Handle);
            HFuncTraced(TEvPQ::TEvGetClientOffset, Handle);
            HFuncTraced(TEvPQ::TEvUpdateWriteTimestamp, Handle);
            HFuncTraced(TEvPQ::TEvSetClientInfo, Handle);
            HFuncTraced(TEvPQ::TEvPartitionOffsets, Handle);
            HFuncTraced(TEvPQ::TEvPartitionStatus, Handle);
            HFuncTraced(TEvPersQueue::TEvReportPartitionError, Handle);
            HFuncTraced(TEvPQ::TEvChangeOwner, Handle);
            HFuncTraced(TEvPQ::TEvChangePartitionConfig, Handle);
            HFuncTraced(TEvPersQueue::TEvHasDataInfo, Handle);
            HFuncTraced(TEvPQ::TEvMirrorerCounters, Handle);
            HFuncTraced(NReadSpeedLimiterEvents::TEvCounters, Handle);
            HFuncTraced(TEvPQ::TEvProxyResponse, Handle);
            HFuncTraced(TEvPQ::TEvError, Handle);
            HFuncTraced(TEvPQ::TEvReserveBytes, Handle);
            HFuncTraced(TEvPQ::TEvGetPartitionClientInfo, Handle);
            HFuncTraced(TEvPQ::TEvPipeDisconnected, Handle);
            HFuncTraced(TEvPQ::TEvUpdateAvailableSize, HandleOnWrite);
            HFuncTraced(TEvPQ::TEvQuotaDeadlineCheck, Handle);
            HFuncTraced(TEvQuota::TEvClearance, Handle);
            HFuncTraced(TEvPQ::TEvRegisterMessageGroup, HandleOnWrite);
            HFuncTraced(TEvPQ::TEvDeregisterMessageGroup, HandleOnWrite);
            HFuncTraced(TEvPQ::TEvSplitMessageGroup, HandleOnWrite);

        default:
            LOG_ERROR_S(ctx, NKikimrServices::PERSQUEUE, "Unexpected " << EventStr("StateWrite", ev));
            break;
        };
    }

    bool CleanUpBlobs(TEvKeyValue::TEvRequest *request, bool hasWrites, const TActorContext& ctx);
    std::pair<TKey, ui32> Compact(const TKey& key, const ui32 size, bool headCleared);

    void HandleWrites(const TActorContext& ctx);
    void BecomeIdle(const TActorContext& ctx);

    void CheckHeadConsistency() const;

    std::pair<TInstant, TInstant> GetTime(const TUserInfo& userInfo, ui64 offset) const;
    TInstant GetWriteTimeEstimate(ui64 offset) const;

    ui32 NextChannel(bool isHead, ui32 blobSize);

    void WriteBlobWithQuota(THolder<TEvKeyValue::TEvRequest>&& request);
    void AddMetaKey(TEvKeyValue::TEvRequest* request);

    size_t GetQuotaRequestSize(const TEvKeyValue::TEvRequest& request);
    void RequestQuotaForWriteBlobRequest(size_t dataSize, ui64 cookie);
    void CalcTopicWriteQuotaParams();
    bool WaitingForPreviousBlobQuota() const;

private:
    void UpdateUserInfoEndOffset(const TInstant& now);

    void UpdateWriteBufferIsFullState(const TInstant& now);

    enum EInitState {
        WaitDiskStatus,
        WaitInfoRange,
        WaitDataRange,
        WaitDataRead,
        WaitMetaRead
    };


    struct TUserCookie {
        TString User;
        ui64 Cookie;
    };


    ui64 TabletID;
    ui32 Partition;
    NKikimrPQ::TPQTabletConfig Config;
    NPersQueue::TTopicConverterPtr TopicConverter;
    bool IsLocalDC;
    TString DCId;

    ui32 MaxBlobSize;
    const ui32 TotalLevels = 4;
    TVector<ui32> CompactLevelBorder;
    ui32 TotalMaxCount;
    ui32 MaxSizeCheck;

//                           [ 8+Mb][ 8+Mb ][not compacted data    ] [ data sended to KV but not yet confirmed]
//ofsets in partition:       101 102|103 104|105 106 107 108 109 110|111 112 113
//                            ^               ^                       ^
//                          StartOffset     HeadOffset                EndOffset
//                          [DataKeysBody  ][DataKeysHead                      ]
    ui64 StartOffset;
    ui64 EndOffset;

    ui64 WriteInflightSize;
    TActorId Tablet;
    TActorId BlobCache;

    EInitState InitState;

    struct TWriteMsg {
        ui64 Cookie;
        TMaybe<ui64> Offset;
        TEvPQ::TEvWrite::TMsg Msg;
    };

    struct TOwnershipMsg {
        ui64 Cookie;
        TString OwnerCookie;
    };

    struct TRegisterMessageGroupMsg {
        ui64 Cookie;
        TEvPQ::TEvRegisterMessageGroup::TBody Body;

        explicit TRegisterMessageGroupMsg(TEvPQ::TEvRegisterMessageGroup& ev)
            : Cookie(ev.Cookie)
            , Body(std::move(ev.Body))
        {
        }
    };

    struct TDeregisterMessageGroupMsg {
        ui64 Cookie;
        TEvPQ::TEvDeregisterMessageGroup::TBody Body;

        explicit TDeregisterMessageGroupMsg(TEvPQ::TEvDeregisterMessageGroup& ev)
            : Cookie(ev.Cookie)
            , Body(std::move(ev.Body))
        {
        }
    };

    struct TSplitMessageGroupMsg {
        ui64 Cookie;
        TVector<TEvPQ::TEvDeregisterMessageGroup::TBody> Deregistrations;
        TVector<TEvPQ::TEvRegisterMessageGroup::TBody> Registrations;

        explicit TSplitMessageGroupMsg(ui64 cookie)
            : Cookie(cookie)
        {
        }
    };

    struct TMessage {
        std::variant<
            TWriteMsg,
            TOwnershipMsg,
            TRegisterMessageGroupMsg,
            TDeregisterMessageGroupMsg,
            TSplitMessageGroupMsg
        > Body;

        ui64 QuotedTime;
        ui64 QueueTime;
        ui64 WriteTime;

        template <typename T>
        explicit TMessage(T&& body, ui64 quotedTime, ui64 queueTime, ui64 writeTime)
            : Body(std::forward<T>(body))
            , QuotedTime(quotedTime)
            , QueueTime(queueTime)
            , WriteTime(writeTime)
        {
        }

        ui64 GetCookie() const {
            switch (Body.index()) {
            case 0:
                return std::get<0>(Body).Cookie;
            case 1:
                return std::get<1>(Body).Cookie;
            case 2:
                return std::get<2>(Body).Cookie;
            case 3:
                return std::get<3>(Body).Cookie;
            case 4:
                return std::get<4>(Body).Cookie;
            default:
                Y_FAIL("unreachable");
            }
        }

        #define DEFINE_CHECKER_GETTER(name, i) \
            bool Is##name() const { \
                return Body.index() == i; \
            } \
            const auto& Get##name() const { \
                Y_VERIFY(Is##name()); \
                return std::get<i>(Body); \
            } \
            auto& Get##name() { \
                Y_VERIFY(Is##name()); \
                return std::get<i>(Body); \
            }

        DEFINE_CHECKER_GETTER(Write, 0)
        DEFINE_CHECKER_GETTER(Ownership, 1)
        DEFINE_CHECKER_GETTER(RegisterMessageGroup, 2)
        DEFINE_CHECKER_GETTER(DeregisterMessageGroup, 3)
        DEFINE_CHECKER_GETTER(SplitMessageGroup, 4)

        #undef DEFINE_CHECKER_GETTER
    };

    std::deque<TMessage> Requests;
    std::deque<TMessage> Responses;

    THead Head;
    THead NewHead;
    TPartitionedBlob PartitionedBlob;
    std::deque<std::pair<TKey, ui32>> CompactedKeys; //key and blob size
    TDataKey NewHeadKey;

    ui64 BodySize;
    ui32 MaxWriteResponsesSize;

    std::deque<TDataKey> DataKeysBody;
    TVector<TKeyLevel> DataKeysHead;
    std::deque<TDataKey> HeadKeys;

    std::deque<std::pair<ui64,ui64>> GapOffsets;
    ui64 GapSize;

    TString CloudId;
    TString DbId;
    TString FolderId;

    TUsersInfoStorage UsersInfoStorage;

    std::deque<std::pair<TString, ui64>> UpdateUserInfoTimestamp;
    bool ReadingTimestamp;
    TString ReadingForUser;
    ui64 ReadingForUserReadRuleGeneration;
    ui64 ReadingForOffset;

    THashMap<ui64, TString> CookieToUser;
    ui64 SetOffsetCookie;

    THashMap<ui64, TReadInfo> ReadInfo;    // cookie -> {...}
    ui64 Cookie;
    TInstant CreationTime;
    TDuration InitDuration;
    bool InitDone;
    const bool NewPartition;

    THashMap<TString, NKikimr::NPQ::TOwnerInfo> Owners;
    THashSet<TActorId> OwnerPipes;

    TSourceIdStorage SourceIdStorage;

    std::deque<THolder<TEvPQ::TEvChangeOwner>> WaitToChangeOwner;

    TTabletCountersBase Counters;
    THolder<TPartitionLabeledCounters> PartitionLabeledCounters;

    TSubscriber Subscriber;

    TInstant WriteCycleStartTime;
    ui32 WriteCycleSize;
    ui32 WriteNewSize;
    ui32 WriteNewSizeInternal;
    ui64 WriteNewSizeUncompressed;
    ui32 WriteNewMessages;
    ui32 WriteNewMessagesInternal;

    TInstant CurrentTimestamp;

    bool DiskIsFull;

    TSet<THasDataReq> HasDataRequests;
    TSet<THasDataDeadline> HasDataDeadlines;
    ui64 HasDataReqNum;

    TQuotaTracker WriteQuota;
    THolder<TPercentileCounter> PartitionWriteQuotaWaitCounter;
    TInstant QuotaDeadline = TInstant::Zero();

    TVector<NSlidingWindow::TSlidingWindow<NSlidingWindow::TSumOperation<ui64>>> AvgWriteBytes;
    TVector<NSlidingWindow::TSlidingWindow<NSlidingWindow::TSumOperation<ui64>>> AvgQuotaBytes;


    ui64 ReservedSize;
    std::deque<THolder<TEvPQ::TEvReserveBytes>> ReserveRequests;

    ui32 Channel;
    TVector<ui32> TotalChannelWritesByHead;

    TWorkingTimeCounter WriteBufferIsFullCounter;

    TInstant WriteTimestamp;
    TInstant WriteTimestampEstimate;
    bool ManageWriteTimestampEstimate = true;
    NSlidingWindow::TSlidingWindow<NSlidingWindow::TMaxOperation<ui64>> WriteLagMs;
    THolder<TPercentileCounter> InputTimeLag;
    THolder<TPercentileCounter> MessageSize;
    TPercentileCounter WriteLatency;
    NKikimr::NPQ::TMultiCounter SLIBigLatency;
    NKikimr::NPQ::TMultiCounter WritesTotal;

    NKikimr::NPQ::TMultiCounter BytesWritten;
    NKikimr::NPQ::TMultiCounter BytesWrittenUncompressed;
    NKikimr::NPQ::TMultiCounter BytesWrittenComp;
    NKikimr::NPQ::TMultiCounter MsgsWritten;

    // Writing blob with topic quota variables
    ui64 TopicQuotaRequestCookie = 0;
    // Wait topic quota metrics
    THolder<TPercentileCounter> TopicWriteQuotaWaitCounter;
    TInstant StartTopicQuotaWaitTimeForCurrentBlob;
    TInstant WriteStartTime;
    TDuration TopicQuotaWaitTimeForCurrentBlob;
    // Topic quota parameters
    TString TopicWriteQuoterPath;
    TString TopicWriteQuotaResourcePath;
    ui64 NextTopicWriteQuotaRequestCookie = 1;

    TDeque<NKikimrPQ::TStatusResponse::TErrorMessage> Errors;

    THolder<TMirrorerInfo> Mirrorer;

    TInstant LastUsedStorageMeterTimestamp;
};

}// NPQ
}// NKikimr
