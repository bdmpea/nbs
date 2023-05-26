#include "part_actor.h"

#include <cloud/blockstore/libs/diagnostics/critical_events.h>
#include <cloud/blockstore/libs/storage/core/probes.h>
#include <cloud/blockstore/libs/storage/partition/model/fresh_blob.h>

#include <ydb/core/base/blobstorage.h>

#include <library/cpp/actors/core/actor_bootstrapped.h>
#include <library/cpp/actors/core/hfunc.h>

namespace NCloud {
namespace NBlockStore {
namespace NStorage {
namespace NPartition {

using namespace NActors;

using namespace NKikimr;

LWTRACE_USING(BLOCKSTORE_STORAGE_PROVIDER);

// TODO: get rid of code duplication with part_actor_writeblob.cpp

namespace {

////////////////////////////////////////////////////////////////////////////////

class TPatchBlobActor final
    : public TActorBootstrapped<TPatchBlobActor>
{
    using TRequest = TEvPartitionPrivate::TEvPatchBlobRequest;
    using TResponse = TEvPartitionPrivate::TEvPatchBlobResponse;

private:
    const TActorId TabletActorId;
    const TRequestInfoPtr RequestInfo;

    const ui64 TabletId;
    const std::unique_ptr<TRequest> Request;

    TInstant RequestSent;
    TInstant ResponseReceived;
    TStorageStatusFlags StorageStatusFlags;
    double ApproximateFreeSpaceShare = 0;

    ui64 OriginalGroupId = 0;

public:
    TPatchBlobActor(
        const TActorId& tabletActorId,
        TRequestInfoPtr requestInfo,
        ui64 tabletId,
        std::unique_ptr<TRequest> request,
        ui64 originalGroupId);

    void Bootstrap(const TActorContext& ctx);

private:
    void SendPatchRequest(const TActorContext& ctx);

    void NotifyCompleted(const TActorContext& ctx, const NProto::TError& error);

    void ReplyAndDie(
        const TActorContext& ctx,
        std::unique_ptr<TResponse> response);

    void ReplyError(
        const TActorContext& ctx,
        const TEvBlobStorage::TEvPatchResult& response,
        const TString& description);

private:
    STFUNC(StateWork);

    void HandlePatchResult(
        const TEvBlobStorage::TEvPatchResult::TPtr& ev,
        const TActorContext& ctx);

    void HandlePoisonPill(
        const TEvents::TEvPoisonPill::TPtr& ev,
        const TActorContext& ctx);
};

////////////////////////////////////////////////////////////////////////////////

TPatchBlobActor::TPatchBlobActor(
        const TActorId& tabletActorId,
        TRequestInfoPtr requestInfo,
        ui64 tabletId,
        std::unique_ptr<TRequest> request,
        ui64 originalGroupId)
    : TabletActorId(tabletActorId)
    , RequestInfo(std::move(requestInfo))
    , TabletId(tabletId)
    , Request(std::move(request))
    , OriginalGroupId(originalGroupId)
{
    ActivityType = TBlockStoreActivities::PARTITION_WORKER;
}

void TPatchBlobActor::Bootstrap(const TActorContext& ctx)
{
    TRequestScope timer(*RequestInfo);

    Become(&TThis::StateWork);

    LWTRACK(
        RequestReceived_PartitionWorker,
        RequestInfo->CallContext->LWOrbit,
        "PatchBlob",
        RequestInfo->CallContext->RequestId);

    SendPatchRequest(ctx);
}

void TPatchBlobActor::SendPatchRequest(const TActorContext& ctx)
{
    auto request = std::make_unique<TEvBlobStorage::TEvPatch>(
        OriginalGroupId,
        MakeBlobId(TabletId, Request->OriginalBlobId),
        MakeBlobId(TabletId, Request->PatchedBlobId),
        0, // mask for bruteforcing
        std::move(Request->Diffs),
        Request->DiffCount,
        Request->Deadline);

    request->Orbit = std::move(RequestInfo->CallContext->LWOrbit);

    RequestSent = ctx.Now();

    SendToBSProxy(
        ctx,
        Request->Proxy,
        request.release());
}

void TPatchBlobActor::NotifyCompleted(
    const TActorContext& ctx,
    const NProto::TError& error)
{
    auto request = std::make_unique<TEvPartitionPrivate::TEvPatchBlobCompleted>(
        error);
    request->OriginalBlobId = Request->OriginalBlobId;
    request->PatchedBlobId = Request->PatchedBlobId;
    request->StorageStatusFlags = StorageStatusFlags;
    request->ApproximateFreeSpaceShare = ApproximateFreeSpaceShare;
    request->RequestTime = ResponseReceived - RequestSent;

    NCloud::Send(ctx, TabletActorId, std::move(request));
}

void TPatchBlobActor::ReplyAndDie(
    const TActorContext& ctx,
    std::unique_ptr<TResponse> response)
{
    NotifyCompleted(ctx, response->GetError());

    LWTRACK(
        ResponseSent_Partition,
        RequestInfo->CallContext->LWOrbit,
        "PatchBlob",
        RequestInfo->CallContext->RequestId);

    NCloud::Reply(ctx, *RequestInfo, std::move(response));
    Die(ctx);
}

void TPatchBlobActor::ReplyError(
    const TActorContext& ctx,
    const TEvBlobStorage::TEvPatchResult& response,
    const TString& description)
{
    LOG_ERROR(ctx, TBlockStoreComponents::PARTITION,
        "[%lu] TEvBlobStorage::TEvPatch failed: %s\n%s",
        TabletId,
        description.data(),
        response.Print(false).data());

    auto error = MakeError(E_REJECTED, "TEvBlobStorage::TEvPatch failed: " + description);
    ReplyAndDie(ctx, std::make_unique<TResponse>(error));
}

////////////////////////////////////////////////////////////////////////////////

void TPatchBlobActor::HandlePatchResult(
    const TEvBlobStorage::TEvPatchResult::TPtr& ev,
    const TActorContext& ctx)
{
    ResponseReceived = ctx.Now();

    const auto* msg = ev->Get();

    if (msg->Status != NKikimrProto::OK) {
        ReplyError(ctx, *msg, msg->ErrorReason);
        return;
    }

    auto blobId = MakeBlobId(TabletId, Request->PatchedBlobId);
    if (msg->Id != blobId) {
        ReplyError(ctx, *msg, "invalid response received");
        return;
    }

    StorageStatusFlags = msg->StatusFlags;
    ApproximateFreeSpaceShare = msg->ApproximateFreeSpaceShare;

    auto response = std::make_unique<TResponse>();
    response->ExecCycles = RequestInfo->GetExecCycles();

    ReplyAndDie(ctx, std::move(response));
}

void TPatchBlobActor::HandlePoisonPill(
    const TEvents::TEvPoisonPill::TPtr& ev,
    const TActorContext& ctx)
{
    Y_UNUSED(ev);

    auto response = std::make_unique<TEvPartitionPrivate::TEvPatchBlobResponse>(
        MakeError(E_REJECTED, "Tablet is dead"));

    ReplyAndDie(ctx, std::move(response));
}

STFUNC(TPatchBlobActor::StateWork)
{
    TRequestScope timer(*RequestInfo);

    switch (ev->GetTypeRewrite()) {
        HFunc(TEvents::TEvPoisonPill, HandlePoisonPill);

        HFunc(TEvBlobStorage::TEvPatchResult, HandlePatchResult);

        default:
            HandleUnexpectedEvent(ev, TBlockStoreComponents::PARTITION_WORKER);
            break;
    }
}

EChannelPermissions StorageStatusFlags2ChannelPermissions(TStorageStatusFlags ssf)
{
    const auto outOfSpaceMask = static_cast<NKikimrBlobStorage::EStatusFlags>(
        NKikimrBlobStorage::StatusDiskSpaceRed
        | NKikimrBlobStorage::StatusDiskSpaceOrange
        // no need to check StatusDiskSpaceBlack since BS won't accept any write requests in black state anyway
    );
    if (ssf.Check(outOfSpaceMask)) {
        return {};
    }

    if (ssf.Check(NKikimrBlobStorage::StatusDiskSpaceYellowStop)) {
        return EChannelPermission::SystemWritesAllowed;
    }

    return EChannelPermission::SystemWritesAllowed | EChannelPermission::UserWritesAllowed;
}

}   // namespace

////////////////////////////////////////////////////////////////////////////////

void TPartitionActor::HandlePatchBlob(
    const TEvPartitionPrivate::TEvPatchBlobRequest::TPtr& ev,
    const TActorContext& ctx)
{
    auto msg = ev->Release();

    auto requestInfo = CreateRequestInfo<TEvPartitionPrivate::TPatchBlobMethod>(
        ev->Sender,
        ev->Cookie,
        msg->CallContext);

    TRequestScope timer(*requestInfo);

    LWTRACK(
        RequestReceived_Partition,
        requestInfo->CallContext->LWOrbit,
        "PatchBlob",
        requestInfo->CallContext->RequestId);

    ui32 originalChannel = msg->OriginalBlobId.Channel();
    ui32 originalGroupId = Info()->GroupFor(
        originalChannel,
        msg->OriginalBlobId.Generation());

    ui32 channel = msg->PatchedBlobId.Channel();
    msg->Proxy = Info()->BSProxyIDForChannel(channel, msg->PatchedBlobId.Generation());

    State->EnqueueIORequest(channel, std::make_unique<TPatchBlobActor>(
        SelfId(),
        requestInfo,
        TabletID(),
        std::unique_ptr<TEvPartitionPrivate::TEvPatchBlobRequest>(msg.Release()),
        originalGroupId));

    ProcessIOQueue(ctx, channel);
}

void TPartitionActor::HandlePatchBlobCompleted(
    const TEvPartitionPrivate::TEvPatchBlobCompleted::TPtr& ev,
    const TActorContext& ctx)
{
    const auto* msg = ev->Get();

    Actors.erase(ev->Sender);

    ui32 patchedChannel = msg->PatchedBlobId.Channel();
    ui32 patchedGroup = Info()->GroupFor(
        patchedChannel,
        msg->PatchedBlobId.Generation());

    const auto isValidFlag = NKikimrBlobStorage::EStatusFlags::StatusIsValid;
    const auto yellowMoveFlag =
        NKikimrBlobStorage::EStatusFlags::StatusDiskSpaceLightYellowMove;

    if (msg->StorageStatusFlags.Check(isValidFlag)) {
        const auto permissions = StorageStatusFlags2ChannelPermissions(
            msg->StorageStatusFlags);
        UpdateChannelPermissions(ctx, patchedChannel, permissions);
        State->UpdateChannelFreeSpaceShare(
            patchedChannel,
            msg->ApproximateFreeSpaceShare);

        if (msg->StorageStatusFlags.Check(yellowMoveFlag)) {
            LOG_WARN(ctx, TBlockStoreComponents::PARTITION,
                "[%lu] Yellow move flag received for channel %u and group %u",
                TabletID(),
                patchedChannel,
                patchedGroup);

            ScheduleYellowStateUpdate(ctx);
            ReassignChannelsIfNeeded(ctx);
        }
    }

    if (FAILED(msg->GetStatus())) {
        LOG_WARN(ctx, TBlockStoreComponents::PARTITION,
            "[%lu] Stop tablet because of PatchBlob error: %s",
            TabletID(),
            FormatError(msg->GetError()).data());

        ReportTabletBSFailure();
        Suicide(ctx);
        return;
    }

    if (patchedGroup == Max<ui32>()) {
        Y_VERIFY_DEBUG(0, "HandlePatchBlobCompleted: invalid blob id received");
    } else {
        UpdateWriteThroughput(
            ctx.Now(),
            patchedChannel,
            patchedGroup,
            msg->PatchedBlobId.BlobSize());
    }
    UpdateNetworkStat(ctx.Now(), msg->PatchedBlobId.BlobSize());
    UpdateExecutorStats(ctx);

    PartCounters->RequestCounters.PatchBlob.AddRequest(
        msg->RequestTime.MicroSeconds(),
        msg->PatchedBlobId.BlobSize(),
        1,
        State->GetChannelDataKind(patchedChannel));

    State->CompleteIORequest(patchedChannel);

    ProcessIOQueue(ctx, patchedChannel);
}

}   // namespace NPartition
}   // namespace NStorage
}   // namespace NBlockStore
}   // namespace NCloud
