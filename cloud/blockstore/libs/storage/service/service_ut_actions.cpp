#include "service_ut.h"

#include <cloud/blockstore/libs/storage/api/disk_registry.h>
#include <cloud/blockstore/libs/storage/api/ss_proxy.h>
#include <cloud/blockstore/libs/storage/api/volume.h>
#include <cloud/blockstore/libs/storage/core/config.h>
#include <cloud/blockstore/libs/storage/core/volume_model.h>
#include <cloud/blockstore/libs/storage/disk_registry/disk_registry_private.h>
#include <cloud/blockstore/libs/storage/protos/disk.pb.h>
#include <cloud/blockstore/libs/storage/protos_ydb/disk.pb.h>
#include <cloud/blockstore/private/api/protos/checkpoints.pb.h>
#include <cloud/blockstore/private/api/protos/disk.pb.h>
#include <cloud/blockstore/private/api/protos/volume.pb.h>

#include <library/cpp/json/json_writer.h>
#include <library/cpp/testing/unittest/registar.h>

#include <util/generic/size_literals.h>

using TVolumeParamMap = ::google::protobuf::Map<
    TString,
    NCloud::NBlockStore::NProto::TUpdateVolumeParamsMapValue>;

bool operator==(const TVolumeParamMap& lhs,
                const TVolumeParamMap& rhs)
{
    if (lhs.size() != rhs.size()) {
        return false;
    }

    return std::all_of(lhs.begin(), lhs.end(), [&](const auto& elem){
        const auto& a = elem.second;
        const auto& b = rhs.at(elem.first);
        return a.GetValue() == b.GetValue() && a.GetTtlMs() == b.GetTtlMs();
    });
}

namespace NCloud::NBlockStore::NStorage {

using namespace NKikimr;
using namespace std::string_literals;

////////////////////////////////////////////////////////////////////////////////

Y_UNIT_TEST_SUITE(TServiceActionsTest)
{
    Y_UNIT_TEST(ShouldForwardChangeStateRequestsToDiskRegistry)
    {
        auto drState = MakeIntrusive<TDiskRegistryState>();
        TTestEnv env(1, 1, 4, 1, {drState});

        NProto::TStorageServiceConfig config;
        config.SetAllocationUnitNonReplicatedSSD(100);
        ui32 nodeIdx = SetupTestEnv(env, config);

        TServiceClient service(env.GetRuntime(), nodeIdx);
        service.CreateVolume(
            DefaultDiskId,
            100_GB / DefaultBlockSize,
            DefaultBlockSize,
            "",
            "",
            NCloud::NProto::STORAGE_MEDIA_SSD_NONREPLICATED
        );

        auto* disk = drState->Disks.FindPtr(DefaultDiskId);
        UNIT_ASSERT(disk);
        UNIT_ASSERT_VALUES_UNEQUAL(0, disk->Devices.size());
        const auto& device = disk->Devices[0];
        UNIT_ASSERT_VALUES_EQUAL(
            static_cast<ui32>(NProto::DEVICE_STATE_ONLINE),
            static_cast<ui32>(device.GetState())
        );

        {
            NPrivateProto::TDiskRegistryChangeStateRequest request;
            auto* cds = request.MutableChangeDeviceState();
            cds->SetDeviceUUID(device.GetDeviceUUID());
            cds->SetState(static_cast<ui32>(NProto::DEVICE_STATE_ERROR));

            TString buf;
            google::protobuf::util::MessageToJsonString(request, &buf);
            service.SendExecuteActionRequest("DiskRegistryChangeState", buf);

            auto response = service.RecvExecuteActionResponse();
            UNIT_ASSERT_VALUES_EQUAL(E_ARGUMENT, response->GetStatus());

            request.SetMessage("some message");
            buf.clear();
            google::protobuf::util::MessageToJsonString(request, &buf);

            service.ExecuteAction("DiskRegistryChangeState", buf);
        }

        UNIT_ASSERT_VALUES_EQUAL(
            static_cast<ui32>(NProto::DEVICE_STATE_ERROR),
            static_cast<ui32>(device.GetState())
        );

        {
            NPrivateProto::TDiskRegistryChangeStateRequest request;
            request.SetMessage("some message");
            auto* cds = request.MutableChangeDeviceState();
            cds->SetDeviceUUID("nosuchdevice");
            cds->SetState(static_cast<ui32>(NProto::DEVICE_STATE_ERROR));

            TString buf;
            google::protobuf::util::MessageToJsonString(request, &buf);
            service.SendExecuteActionRequest("DiskRegistryChangeState", buf);

            auto response = service.RecvExecuteActionResponse();
            UNIT_ASSERT_VALUES_EQUAL(E_NOT_FOUND, response->GetStatus());
        }

        {
            NPrivateProto::TDiskRegistryChangeStateRequest request;
            request.SetMessage("some message");
            auto* cds = request.MutableChangeAgentState();
            cds->SetAgentId("someagent");
            cds->SetState(static_cast<ui32>(NProto::AGENT_STATE_UNAVAILABLE));

            TString buf;
            google::protobuf::util::MessageToJsonString(request, &buf);
            service.ExecuteAction("DiskRegistryChangeState", buf);
        }

        UNIT_ASSERT_VALUES_EQUAL(1, drState->AgentStates.size());
        UNIT_ASSERT_VALUES_EQUAL(
            "someagent",
            drState->AgentStates[0].first
        );
        UNIT_ASSERT_VALUES_EQUAL(
            static_cast<ui32>(NProto::AGENT_STATE_UNAVAILABLE),
            static_cast<ui32>(drState->AgentStates[0].second)
        );
    }

    Y_UNIT_TEST(ShouldModifyVolumeTagsAndDescribeVolume)
    {
        TTestEnv env;
        NProto::TStorageServiceConfig config;
        ui32 nodeIdx = SetupTestEnv(env, std::move(config));

        TServiceClient service(env.GetRuntime(), nodeIdx);
        service.CreateVolume();

        {
            // Incorrect ConfigVersion.
            NPrivateProto::TModifyTagsRequest request;
            request.SetDiskId(DefaultDiskId);
            request.SetConfigVersion(2);
            *request.AddTagsToAdd() = "a";

            TString buf;
            google::protobuf::util::MessageToJsonString(request, &buf);
            service.SendExecuteActionRequest("ModifyTags", buf);
            auto response = service.RecvExecuteActionResponse();
            UNIT_ASSERT_VALUES_EQUAL(E_ABORTED, response->GetStatus());
        }

        {
            // Pass ConfigVersion.
            NPrivateProto::TModifyTagsRequest request;
            request.SetDiskId(DefaultDiskId);
            request.SetConfigVersion(1);
            *request.AddTagsToAdd() = "a";
            *request.AddTagsToAdd() = "b";

            TString buf;
            google::protobuf::util::MessageToJsonString(request, &buf);
            service.ExecuteAction("ModifyTags", buf);
        }

        {
            // Do not pass ConfigVersion.
            NPrivateProto::TModifyTagsRequest request;
            request.SetDiskId(DefaultDiskId);
            *request.AddTagsToAdd() = "c";
            *request.AddTagsToRemove() = "b";
            *request.AddTagsToRemove() = "d";

            TString buf;
            google::protobuf::util::MessageToJsonString(request, &buf);
            service.ExecuteAction("ModifyTags", buf);
        }

        {
            NPrivateProto::TModifyTagsRequest request;
            request.SetDiskId(DefaultDiskId);
            *request.AddTagsToAdd() = ",,,";

            TString buf;
            google::protobuf::util::MessageToJsonString(request, &buf);
            service.SendExecuteActionRequest("ModifyTags", buf);
            auto response = service.RecvExecuteActionResponse();
            UNIT_ASSERT_VALUES_EQUAL(E_ARGUMENT, response->GetStatus());
        }

        {
            NPrivateProto::TDescribeVolumeRequest request;
            request.SetDiskId(DefaultDiskId);
            TString buf;
            google::protobuf::util::MessageToJsonString(request, &buf);
            auto response = service.ExecuteAction("DescribeVolume", buf);
            NKikimrSchemeOp::TBlockStoreVolumeDescription pathDescr;
            UNIT_ASSERT(google::protobuf::util::JsonStringToMessage(
                response->Record.GetOutput(),
                &pathDescr
            ).ok());
            const auto& config = pathDescr.GetVolumeConfig();
            UNIT_ASSERT_VALUES_EQUAL("a,c", config.GetTagsStr());
        }

        {
            NPrivateProto::TModifyTagsRequest request;
            request.SetDiskId(DefaultDiskId);
            *request.AddTagsToRemove() = "a";

            TString buf;
            google::protobuf::util::MessageToJsonString(request, &buf);
            service.ExecuteAction("ModifyTags", buf);
        }

        {
            NPrivateProto::TModifyTagsRequest request;
            request.SetDiskId(DefaultDiskId);
            *request.AddTagsToRemove() = "c";

            TString buf;
            google::protobuf::util::MessageToJsonString(request, &buf);
            service.ExecuteAction("ModifyTags", buf);
        }

        {
            NPrivateProto::TDescribeVolumeRequest request;
            request.SetDiskId(DefaultDiskId);
            TString buf;
            google::protobuf::util::MessageToJsonString(request, &buf);
            auto response = service.ExecuteAction("DescribeVolume", buf);
            NKikimrSchemeOp::TBlockStoreVolumeDescription pathDescr;
            UNIT_ASSERT(google::protobuf::util::JsonStringToMessage(
                response->Record.GetOutput(),
                &pathDescr
            ).ok());
            const auto& config = pathDescr.GetVolumeConfig();
            UNIT_ASSERT_VALUES_EQUAL("", config.GetTagsStr());
        }

        service.DestroyVolume();
    }

    Y_UNIT_TEST(ShouldAllowToChangeBaseDiskId)
    {
        TTestEnv env;
        NProto::TStorageServiceConfig config;
        ui32 nodeIdx = SetupTestEnv(env, std::move(config));

        TServiceClient service(env.GetRuntime(), nodeIdx);
        service.CreateVolume(DefaultDiskId);
        service.CreateVolume("new-base-disk");

        UNIT_ASSERT_EQUAL(0, GetVolumeConfig(service, DefaultDiskId).GetBaseDiskTabletId());

        {
            NPrivateProto::TRebaseVolumeRequest rebaseReq;
            rebaseReq.SetDiskId(DefaultDiskId);
            rebaseReq.SetTargetBaseDiskId("new-base-disk");
            rebaseReq.SetConfigVersion(1);

            TString buf;
            google::protobuf::util::MessageToJsonString(rebaseReq, &buf);
            service.ExecuteAction("rebasevolume", buf);
        }

        auto volumeConfig = GetVolumeConfig(service, DefaultDiskId);
        UNIT_ASSERT_VALUES_EQUAL("new-base-disk", volumeConfig.GetBaseDiskId());
        UNIT_ASSERT_UNEQUAL(0, volumeConfig.GetBaseDiskTabletId());
    }

    Y_UNIT_TEST(ShouldForwardDeleteCheckpointDataToVolume)
    {
        TTestEnv env;
        NProto::TStorageServiceConfig config;
        ui32 nodeIdx = SetupTestEnv(env, std::move(config));

        TServiceClient service(env.GetRuntime(), nodeIdx);
        service.CreateVolume(DefaultDiskId);
        auto createCheckpointResponse = service.CreateCheckpoint(DefaultDiskId, "c1");
        UNIT_ASSERT_VALUES_EQUAL(S_OK, createCheckpointResponse->GetStatus());

        {
            NPrivateProto::TDeleteCheckpointDataRequest deleteCheckpointDataRequest;
            deleteCheckpointDataRequest.SetDiskId(DefaultDiskId);
            deleteCheckpointDataRequest.SetCheckpointId("c1");

            TString buf;
            google::protobuf::util::MessageToJsonString(deleteCheckpointDataRequest, &buf);
            auto executeResponse = service.ExecuteAction("deletecheckpointdata", buf);
            UNIT_ASSERT_VALUES_EQUAL(S_OK, executeResponse->GetStatus());

            NPrivateProto::TDeleteCheckpointDataResponse deleteCheckpointDataResponse;
            UNIT_ASSERT(google::protobuf::util::JsonStringToMessage(
                executeResponse->Record.GetOutput(),
                &deleteCheckpointDataResponse
            ).ok());
            UNIT_ASSERT_VALUES_EQUAL(S_OK, deleteCheckpointDataResponse.GetError().GetCode());
        }

        auto deleteCheckpointResponse = service.DeleteCheckpoint(DefaultDiskId, "c1");
        UNIT_ASSERT_VALUES_EQUAL(S_OK, deleteCheckpointResponse->GetStatus());
    }

    Y_UNIT_TEST(ShouldForwardWritableStateRequestsToDiskRegistry)
    {
        auto drState = MakeIntrusive<TDiskRegistryState>();
        TTestEnv env(1, 1, 4, 1, {drState});

        NProto::TStorageServiceConfig config;
        ui32 nodeIdx = SetupTestEnv(env, std::move(config));

        TServiceClient service(env.GetRuntime(), nodeIdx);

        UNIT_ASSERT(!drState->WritableState);

        NProto::TSetWritableStateRequest request;
        request.SetState(true);

        TString buf;
        google::protobuf::util::MessageToJsonString(request, &buf);

        service.ExecuteAction("diskregistrysetwritablestate", buf);

        UNIT_ASSERT(drState->WritableState);
    }

    Y_UNIT_TEST(ShouldForwardBackupRequestsToDiskRegistry)
    {
        auto drState = MakeIntrusive<TDiskRegistryState>();
        TTestEnv env(1, 1, 4, 1, {drState});

        NProto::TStorageServiceConfig config;
        config.SetAllocationUnitNonReplicatedSSD(100);
        ui32 nodeIdx = SetupTestEnv(env, config);

        TServiceClient service(env.GetRuntime(), nodeIdx);
        service.CreateVolume(
            DefaultDiskId,
            100_GB / DefaultBlockSize,
            DefaultBlockSize,
            "",
            "",
            NCloud::NProto::STORAGE_MEDIA_SSD_NONREPLICATED
        );

        NProto::TBackupDiskRegistryStateResponse request;

        TString buf;
        google::protobuf::util::MessageToJsonString(request, &buf);

        auto response = service.ExecuteAction("backupdiskregistrystate", buf);

        NProto::TBackupDiskRegistryStateResponse proto;
        UNIT_ASSERT(google::protobuf::util::JsonStringToMessage(
            response->Record.GetOutput(),
            &proto
        ).ok());
        const auto& backup = proto.GetBackup();

        UNIT_ASSERT_VALUES_EQUAL(1, backup.DisksSize());
        UNIT_ASSERT_VALUES_EQUAL(DefaultDiskId, backup.GetDisks(0).GetDiskId());
    }

    Y_UNIT_TEST(ShouldForwardUpdatePlacementGroupSettingsRequestsToDiskRegistry)
    {
        auto drState = MakeIntrusive<TDiskRegistryState>();
        TTestEnv env(1, 1, 4, 1, {drState});

        NProto::TStorageServiceConfig config;
        ui32 nodeIdx = SetupTestEnv(env, config);

        TServiceClient service(env.GetRuntime(), nodeIdx);
        service.CreatePlacementGroup(
            "group-1",
            NProto::PLACEMENT_STRATEGY_SPREAD,
            0
        );

        NProto::TUpdatePlacementGroupSettingsRequest request;
        request.SetGroupId("group-1");
        request.SetConfigVersion(1);
        request.MutableSettings()->SetMaxDisksInGroup(100);

        TString buf;
        google::protobuf::util::MessageToJsonString(request, &buf);

        auto executeResponse = service.ExecuteAction("updateplacementgroupsettings", buf);
        UNIT_ASSERT_VALUES_EQUAL(S_OK, executeResponse->GetStatus());

        NProto::TBackupDiskRegistryStateResponse response;
        UNIT_ASSERT(google::protobuf::util::JsonStringToMessage(
            executeResponse->Record.GetOutput(),
            &response
        ).ok());

        UNIT_ASSERT_VALUES_EQUAL(S_OK, response.GetError().GetCode());
        const auto* group = drState->PlacementGroups.FindPtr("group-1");
        UNIT_ASSERT(group);
        UNIT_ASSERT_VALUES_EQUAL(100, group->Settings.GetMaxDisksInGroup());
    }

    Y_UNIT_TEST(ShouldRebindLocalVolumes)
    {
        TTestEnv env;
        NProto::TStorageServiceConfig config;
        ui32 nodeIdx = SetupTestEnv(env, std::move(config));

        TServiceClient service(env.GetRuntime(), nodeIdx);
        service.CreateVolume("vol0");   // local
        service.CreateVolume("vol1");   // local
        service.CreateVolume("vol2");   // remote
        service.CreateVolume("vol3");   // not mounted

        service.MountVolume("vol0");
        service.MountVolume("vol1");
        service.MountVolume(
            "vol2",
            TString(),  // instanceId
            TString(),  // token
            NProto::IPC_GRPC,
            NProto::VOLUME_ACCESS_READ_WRITE,
            NProto::VOLUME_MOUNT_REMOTE
        );

        TSet<TString> diskIds;

        env.GetRuntime().SetObserverFunc(
            [&] (TTestActorRuntimeBase& runtime, TAutoPtr<IEventHandle>& event) {
                switch (event->GetTypeRewrite()) {
                    case TEvService::EvChangeVolumeBindingRequest: {
                        auto* msg = event->Get<TEvService::TEvChangeVolumeBindingRequest>();
                        UNIT_ASSERT_VALUES_EQUAL(
                            static_cast<ui32>(TEvService::TChangeVolumeBindingRequest::EChangeBindingOp::RELEASE_TO_HIVE),
                            static_cast<ui32>(msg->Action)
                        );

                        diskIds.insert(msg->DiskId);
                    }
                }
                return TTestActorRuntime::DefaultObserverFunc(runtime, event);
            }
        );

        NPrivateProto::TRebindVolumesRequest request;
        request.SetBinding(2);  // REMOTE

        TString buf;
        google::protobuf::util::MessageToJsonString(request, &buf);

        service.ExecuteAction("rebindvolumes", buf);

        UNIT_ASSERT_VALUES_EQUAL(2, diskIds.size());
        UNIT_ASSERT_VALUES_EQUAL(diskIds.count("vol0"), 1);
        UNIT_ASSERT_VALUES_EQUAL(diskIds.count("vol1"), 1);    }

    Y_UNIT_TEST(ShouldDrainNode)
    {
        TTestEnv env;
        NProto::TStorageServiceConfig config;
        ui32 nodeIdx = SetupTestEnv(env, std::move(config));

        TServiceClient service(env.GetRuntime(), nodeIdx);

        ui64 observedNodeIdx = 0;
        bool observedKeepDown = false;

        env.GetRuntime().SetObserverFunc(
            [&] (TTestActorRuntimeBase& runtime, TAutoPtr<IEventHandle>& event) {
                switch (event->GetTypeRewrite()) {
                    case TEvHive::EvDrainNode: {
                        auto* msg = event->Get<TEvHive::TEvDrainNode>();
                        observedNodeIdx = msg->Record.GetNodeID();
                        observedKeepDown = msg->Record.GetKeepDown();
                    }
                }
                return TTestActorRuntime::DefaultObserverFunc(runtime, event);
            }
        );

        NPrivateProto::TDrainNodeRequest request;

        TString buf;
        google::protobuf::util::MessageToJsonString(request, &buf);

        service.ExecuteAction("drainnode", buf);

        UNIT_ASSERT_VALUES_EQUAL(
            service.GetSender().NodeId(),
            observedNodeIdx
        );

        UNIT_ASSERT(!observedKeepDown);

        request.SetKeepDown(true);

        buf.clear();
        google::protobuf::util::MessageToJsonString(request, &buf);

        service.ExecuteAction("drainnode", buf);

        UNIT_ASSERT_VALUES_EQUAL(
            service.GetSender().NodeId(),
            observedNodeIdx
        );

        UNIT_ASSERT(observedKeepDown);
    }

    Y_UNIT_TEST(ShouldForwardUpdateUsedBlocksToVolume)
    {
        TTestEnv env;
        NProto::TStorageServiceConfig config;
        ui32 nodeIdx = SetupTestEnv(env, std::move(config));

        TServiceClient service(env.GetRuntime(), nodeIdx);
        service.CreateVolume(DefaultDiskId);

        env.GetRuntime().SetObserverFunc(
            [&] (TTestActorRuntimeBase& runtime, TAutoPtr<IEventHandle>& event) {
                switch (event->GetTypeRewrite()) {
                    case TEvVolume::EvUpdateUsedBlocksRequest: {
                        auto* msg =
                            event->Get<TEvVolume::TEvUpdateUsedBlocksRequest>();
                        UNIT_ASSERT_VALUES_EQUAL(1, msg->Record.StartIndicesSize());
                        UNIT_ASSERT_VALUES_EQUAL(1, msg->Record.BlockCountsSize());
                        UNIT_ASSERT_VALUES_EQUAL(11, msg->Record.GetStartIndices(0));
                        UNIT_ASSERT_VALUES_EQUAL(22, msg->Record.GetBlockCounts(0));
                        UNIT_ASSERT_VALUES_EQUAL(true, msg->Record.GetUsed());
                    }
                }

                return TTestActorRuntime::DefaultObserverFunc(runtime, event);
            }
        );

        {
            NPrivateProto::TModifyTagsRequest request;
            request.SetDiskId(DefaultDiskId);
            *request.AddTagsToAdd() = "track-used";

            TString buf;
            google::protobuf::util::MessageToJsonString(request, &buf);
            service.ExecuteAction("ModifyTags", buf);
        }

        {
            NProto::TUpdateUsedBlocksRequest request;
            request.SetDiskId(DefaultDiskId);
            request.AddStartIndices(11);
            request.AddBlockCounts(22);
            request.SetUsed(true);

            TString buf;
            google::protobuf::util::MessageToJsonString(request, &buf);
            auto executeResponse = service.ExecuteAction("updateusedblocks", buf);
            UNIT_ASSERT_VALUES_EQUAL(S_OK, executeResponse->GetStatus());

            NProto::TUpdateUsedBlocksResponse response;
            UNIT_ASSERT(google::protobuf::util::JsonStringToMessage(
                executeResponse->Record.GetOutput(),
                &response
            ).ok());

            UNIT_ASSERT_VALUES_EQUAL(S_OK, response.GetError().GetCode());
        }
    }

    Y_UNIT_TEST(ShouldRebuildMetadataForPartitionVersion1)
    {
        TTestEnv env;
        NProto::TStorageServiceConfig config;
        ui32 nodeIdx = SetupTestEnv(env, std::move(config));

        TServiceClient service(env.GetRuntime(), nodeIdx);
        service.CreateVolume("vol0");

        auto sessionId = service.MountVolume("vol0")->Record.GetSessionId();

        service.WriteBlocks(
            "vol0",
            TBlockRange64::WithLength(0, 1024),
            sessionId,
            char(1));

        {
            NPrivateProto::TRebuildMetadataRequest request;
            request.SetDiskId("vol0");
            request.SetMetadataType(NPrivateProto::USED_BLOCKS);
            request.SetBatchSize(100);

            TString buf;
            google::protobuf::util::MessageToJsonString(request, &buf);

            auto response = service.ExecuteAction("rebuildmetadata", buf);
            NPrivateProto::TRebuildMetadataResponse metadataResponse;
            UNIT_ASSERT(google::protobuf::util::JsonStringToMessage(
                response->Record.GetOutput(),
                &metadataResponse
            ).ok());
        }

        {
            NPrivateProto::TGetRebuildMetadataStatusRequest request;
            request.SetDiskId("vol0");

            TString buf;
            google::protobuf::util::MessageToJsonString(request, &buf);

            auto response = service.ExecuteAction("getrebuildmetadatastatus", buf);
            NPrivateProto::TGetRebuildMetadataStatusResponse metadataResponse;

            UNIT_ASSERT(google::protobuf::util::JsonStringToMessage(
                response->Record.GetOutput(),
                &metadataResponse
            ).ok());

            UNIT_ASSERT_VALUES_EQUAL(1024, metadataResponse.GetProgress().GetTotal());
        }
    }

    Y_UNIT_TEST(ShouldRebuildMetadataBlockCountForPartitionVersion1)
    {
        TTestEnv env;
        NProto::TStorageServiceConfig config;
        ui32 nodeIdx = SetupTestEnv(env, std::move(config));

        TServiceClient service(env.GetRuntime(), nodeIdx);
        service.CreateVolume("vol0");

        auto sessionId = service.MountVolume("vol0")->Record.GetSessionId();

        service.WriteBlocks(
            "vol0",
            TBlockRange64::WithLength(0, 1024),
            sessionId,
            char(1));

        {
            NPrivateProto::TRebuildMetadataRequest request;
            request.SetDiskId("vol0");
            request.SetMetadataType(NPrivateProto::BLOCK_COUNT);
            request.SetBatchSize(100);

            TString buf;
            google::protobuf::util::MessageToJsonString(request, &buf);

            auto response = service.ExecuteAction("rebuildmetadata", buf);
            NPrivateProto::TRebuildMetadataResponse metadataResponse;
            UNIT_ASSERT(google::protobuf::util::JsonStringToMessage(
                response->Record.GetOutput(),
                &metadataResponse
            ).ok());
        }

        {
            NPrivateProto::TGetRebuildMetadataStatusRequest request;
            request.SetDiskId("vol0");

            TString buf;
            google::protobuf::util::MessageToJsonString(request, &buf);

            auto response = service.ExecuteAction("getrebuildmetadatastatus", buf);
            NPrivateProto::TGetRebuildMetadataStatusResponse metadataResponse;
            UNIT_ASSERT(google::protobuf::util::JsonStringToMessage(
                response->Record.GetOutput(),
                &metadataResponse
            ).ok());

            UNIT_ASSERT_VALUES_EQUAL(1, metadataResponse.GetProgress().GetTotal());
        }
    }

    Y_UNIT_TEST(ShouldScanDiskForPartitionVersion1)
    {
        TTestEnv env;
        NProto::TStorageServiceConfig config;
        const ui32 nodeIdx = SetupTestEnv(env, std::move(config));

        TServiceClient service(env.GetRuntime(), nodeIdx);
        service.CreateVolume("vol0");

        const auto sessionId = service.MountVolume("vol0")->Record.GetSessionId();

        service.WriteBlocks(
            "vol0",
            TBlockRange64::WithLength(0, 1024),
            sessionId,
            char(1));

        {
            NPrivateProto::TScanDiskRequest request;
            request.SetDiskId("vol0");
            request.SetBatchSize(100);

            TString buf;
            google::protobuf::util::MessageToJsonString(request, &buf);

            const auto response = service.ExecuteAction("scandisk", buf);
            NPrivateProto::TScanDiskResponse scanDiskResponse;
            UNIT_ASSERT(google::protobuf::util::JsonStringToMessage(
                response->Record.GetOutput(),
                &scanDiskResponse
            ).ok());
        }

        {
            NPrivateProto::TGetScanDiskStatusRequest request;
            request.SetDiskId("vol0");

            TString buf;
            google::protobuf::util::MessageToJsonString(request, &buf);

            const auto response = service.ExecuteAction("getscandiskstatus", buf);
            NPrivateProto::TGetScanDiskStatusResponse scanDiskResponse;
            UNIT_ASSERT(google::protobuf::util::JsonStringToMessage(
                response->Record.GetOutput(),
                &scanDiskResponse
            ).ok());

            UNIT_ASSERT_VALUES_EQUAL(
                1,
                scanDiskResponse.GetProgress().GetTotal());
            UNIT_ASSERT_VALUES_EQUAL(
                0,
                scanDiskResponse.GetProgress().GetBrokenBlobs().size());
        }
    }

    Y_UNIT_TEST(ShouldForceMigrate)
    {
        auto drState = MakeIntrusive<TDiskRegistryState>();
        TTestEnv env(1, 1, 4, 1, {drState});

        NProto::TStorageServiceConfig config;
        config.SetAllocationUnitNonReplicatedSSD(100);
        ui32 nodeIdx = SetupTestEnv(env, config);
        TServiceClient service(env.GetRuntime(), nodeIdx);

        service.CreateVolume(
            DefaultDiskId,
            100_GB / DefaultBlockSize,
            DefaultBlockSize,
            "",
            "",
            NCloud::NProto::STORAGE_MEDIA_SSD_NONREPLICATED
        );

        auto* disk = drState->Disks.FindPtr(DefaultDiskId);
        UNIT_ASSERT_VALUES_EQUAL(800, disk->Devices.size());

        NProto::TStartForceMigrationRequest request;
        request.SetSourceDiskId(DefaultDiskId);
        request.SetSourceDeviceId(disk->Devices[0].GetDeviceUUID());
        request.SetTargetDeviceId("uuid801");

        TString buf;
        google::protobuf::util::MessageToJsonString(request, &buf);

        const auto response = service.ExecuteAction("migrationdiskregistrydevice", buf);
        UNIT_ASSERT_VALUES_EQUAL(S_OK, response->GetStatus());
    }

    Y_UNIT_TEST(ShouldCreateDiskFromDevices)
    {
        auto drState = MakeIntrusive<TDiskRegistryState>();
        TTestEnv env(1, 1, 4, 1, {drState});

        NProto::TStorageServiceConfig config;
        config.SetAllocationUnitNonReplicatedSSD(100);
        ui32 nodeIdx = SetupTestEnv(env, config);
        TServiceClient service(env.GetRuntime(), nodeIdx);

        NProto::TCreateVolumeFromDevicesRequest request;
        request.SetDiskId("Disk-1");
        request.SetBlockSize(512);
        auto* devices = request.MutableDeviceUUIDs();
        devices->Add("uuid1");
        devices->Add("uuid42");

        TString buf;
        google::protobuf::util::MessageToJsonString(request, &buf);

        const auto response = service.ExecuteAction("creatediskfromdevices", buf);
        UNIT_ASSERT_VALUES_EQUAL(S_OK, response->GetStatus());
    }

    Y_UNIT_TEST(ShouldChangeDiskDevice)
    {
        auto drState = MakeIntrusive<TDiskRegistryState>();
        TTestEnv env(1, 1, 4, 1, {drState});

        NProto::TStorageServiceConfig config;
        config.SetAllocationUnitNonReplicatedSSD(100);
        ui32 nodeIdx = SetupTestEnv(env, config);

        TServiceClient service(env.GetRuntime(), nodeIdx);
        service.CreateVolume(
            "Disk-1",
            100_GB / DefaultBlockSize,
            DefaultBlockSize,
            "",
            "",
            NCloud::NProto::STORAGE_MEDIA_SSD_NONREPLICATED
        );

        NProto::TChangeDiskDeviceRequest request;
        request.SetDiskId("Disk-1");
        request.SetSourceDeviceId("uuid1");
        request.SetTargetDeviceId("uuid1000");

        TString buf;
        google::protobuf::util::MessageToJsonString(request, &buf);

        const auto response = service.ExecuteAction("changediskdevice", buf);
        UNIT_ASSERT_VALUES_EQUAL(S_OK, response->GetStatus());
    }

    Y_UNIT_TEST(ShouldSetupChannels)
    {
        TTestEnv env;
        NProto::TStorageServiceConfig config;
        config.SetAllocationUnitSSD(100);
        ui32 nodeIdx = SetupTestEnv(env, config);
        auto& runtime = env.GetRuntime();

        TServiceClient service(env.GetRuntime(), nodeIdx);

        service.CreateVolume("vol0");
        {
            NPrivateProto::TSetupChannelsRequest request;
            request.SetDiskId("vol0");
            request.MutableVolumeChannelsToPoolsKinds()->MutableData()->insert(
                {0, "pool-kind-2"});
            request.MutableVolumeChannelsToPoolsKinds()->MutableData()->insert(
                {1, "pool-kind-2"});
            request.SetIsPartitionsPoolKindSetManually(true);

            TString buf;
            google::protobuf::util::MessageToJsonString(request, &buf);

            const auto response = service.ExecuteAction("setupchannels", buf);
            NPrivateProto::TSetupChannelsResponse setupChannelsResponse;
            UNIT_ASSERT(google::protobuf::util::JsonStringToMessage(
                response->Record.GetOutput(),
                &setupChannelsResponse
            ).ok());
        }

        auto getPoolKindFromConfig = [](
            const NKikimrBlockStore::TVolumeConfig& volumeConfig,
            const ui32 ind)
        {
            return volumeConfig.GetExplicitChannelProfiles(ind).GetPoolKind();
        };

        bool describeReceived = false;
        ui32 requestCount = 0;
        runtime.SetObserverFunc(
            [&] (TTestActorRuntimeBase& runtime, TAutoPtr<IEventHandle>& event) {
                switch (event->GetTypeRewrite()) {
                    case TEvSSProxy::EvDescribeVolumeResponse: {
                        describeReceived = true;
                        auto* msg = event->Get<TEvSSProxy::TEvDescribeVolumeResponse>();
                        const auto& pathDescription = msg->PathDescription;
                        const auto& volumeDescription =
                            pathDescription.GetBlockStoreVolumeDescription();
                        const auto& volumeConfig = volumeDescription.GetVolumeConfig();
                        if (requestCount > 3) {
                            auto pool_kind = getPoolKindFromConfig(volumeConfig, 0);
                            UNIT_ASSERT_EQUAL(pool_kind, "pool-kind-1");
                            pool_kind = getPoolKindFromConfig(volumeConfig, 1);
                            UNIT_ASSERT_EQUAL(pool_kind, "pool-kind-1");
                            pool_kind = getPoolKindFromConfig(volumeConfig, 2);
                            UNIT_ASSERT_EQUAL(pool_kind, "pool-kind-1");
                            break;
                        }
                        auto pool_kind = getPoolKindFromConfig(volumeConfig, 0);
                        UNIT_ASSERT_EQUAL(pool_kind, "pool-kind-2");
                        pool_kind = getPoolKindFromConfig(volumeConfig, 1);
                        UNIT_ASSERT_EQUAL(pool_kind, "pool-kind-2");
                        if (requestCount > 1) {
                            pool_kind = getPoolKindFromConfig(volumeConfig, 2);
                            UNIT_ASSERT_EQUAL(pool_kind, "pool-kind-2");
                        }
                        if (requestCount > 2) {
                            pool_kind = getPoolKindFromConfig(volumeConfig, 3);
                            UNIT_ASSERT_EQUAL(pool_kind, "pool-kind-2");
                        }
                        break;
                    }
                }
                return TTestActorRuntime::DefaultObserverFunc(runtime, event);
            }
        );

        requestCount += 1;
        service.DescribeVolume("vol0");
        UNIT_ASSERT(describeReceived);

        // First two channels shouldn't change after another request
        {
            NPrivateProto::TSetupChannelsRequest request;
            request.SetDiskId("vol0");
            request.MutableVolumeChannelsToPoolsKinds()->MutableData()->insert(
                {2, "pool-kind-2"});
            request.SetIsPartitionsPoolKindSetManually(true);

            TString buf;
            google::protobuf::util::MessageToJsonString(request, &buf);

            const auto response = service.ExecuteAction("setupchannels", buf);
            UNIT_ASSERT_VALUES_EQUAL(S_OK, response->GetStatus());
        }

        // Check that nothing changes after resizing
        {
            auto request = service.CreateResizeVolumeRequest(
                "vol0",
                42949672
            );
            service.SendRequest(MakeStorageServiceId(), std::move(request));

            auto response = service.RecvResizeVolumeResponse();
            UNIT_ASSERT_VALUES_EQUAL(S_OK, response->GetStatus());
        }

        requestCount += 1;
        describeReceived = false;
        service.DescribeVolume("vol0");
        UNIT_ASSERT(describeReceived);

        // Channels shouldn't change after setting false flag
        {
            NPrivateProto::TSetupChannelsRequest request;
            request.SetDiskId("vol0");
            request.SetIsPartitionsPoolKindSetManually(false);
            request.MutableVolumeChannelsToPoolsKinds()->MutableData()->insert(
                {3, "pool-kind-2"});

            TString buf;
            google::protobuf::util::MessageToJsonString(request, &buf);

            const auto response = service.ExecuteAction("setupchannels", buf);
        }

        requestCount += 1;
        describeReceived = false;
        service.DescribeVolume("vol0");
        UNIT_ASSERT(describeReceived);

        // All channels should be from config after resizeVolume
        {
            auto request = service.CreateResizeVolumeRequest(
                "vol0",
                42949680
            );
            service.SendRequest(MakeStorageServiceId(), std::move(request));

            auto response = service.RecvResizeVolumeResponse();
            UNIT_ASSERT_VALUES_EQUAL(S_OK, response->GetStatus());
        }

        requestCount += 1;
        describeReceived = false;
        service.DescribeVolume("vol0");
        UNIT_ASSERT(describeReceived);
    }

    Y_UNIT_TEST(ShouldForwardUpdateParamsRequestsToDiskRegistry)
    {
        auto drState = MakeIntrusive<TDiskRegistryState>();
        TTestEnv env(1, 1, 4, 1, {drState});
        auto& runtime = env.GetRuntime();

        NProto::TStorageServiceConfig config;
        ui32 nodeIdx = SetupTestEnv(env, config);
        TServiceClient service(env.GetRuntime(), nodeIdx);

        NProto::TDiskRegistryAgentListRequestParams requestParams;
        requestParams.AddAgentIds("agent0");
        requestParams.SetTimeoutMs(600);
        requestParams.SetNewNonReplicatedAgentMinTimeoutMs(123);
        requestParams.SetNewNonReplicatedAgentMaxTimeoutMs(456);

        bool updateParamsReceived = false;
        runtime.SetObserverFunc(
            [&] (TTestActorRuntimeBase& runtime, TAutoPtr<IEventHandle>& event) {
                switch (event->GetTypeRewrite()) {
                    case TEvDiskRegistry::EvUpdateDiskRegistryAgentListParamsRequest: {
                        updateParamsReceived = true;
                        auto* msg = event->Get<TEvDiskRegistry::TEvUpdateDiskRegistryAgentListParamsRequest>();
                        const auto& params = msg->Record.GetParams();
                        UNIT_ASSERT(google::protobuf::util::MessageDifferencer::Equals(requestParams, params));
                    }
                }
                return TTestActorRuntime::DefaultObserverFunc(runtime, event);
            }
        );

        TString buf;
        google::protobuf::util::MessageToJsonString(requestParams, &buf);
        const auto response = service.ExecuteAction("updatediskregistryagentlistparams", buf);

        UNIT_ASSERT_VALUES_EQUAL(S_OK, response->GetStatus());
        UNIT_ASSERT(updateParamsReceived);
    }

    Y_UNIT_TEST(ShouldFinishFillDisk)
    {
        TTestEnv env;
        NProto::TStorageServiceConfig config;
        ui32 nodeIdx = SetupTestEnv(env, std::move(config));

        TServiceClient service(env.GetRuntime(), nodeIdx);

        auto request = service.CreateCreateVolumeRequest();
        request->Record.SetFillGeneration(1);
        service.SendRequest(MakeStorageServiceId(), std::move(request));

        auto response = service.RecvCreateVolumeResponse();
        UNIT_ASSERT_C(response->GetStatus() == S_OK, response->GetStatus());

        auto volumeConfig = GetVolumeConfig(service, DefaultDiskId);
        UNIT_ASSERT_VALUES_EQUAL(false, volumeConfig.GetIsFillFinished());

        {
            NPrivateProto::TFinishFillDiskRequest request;
            request.SetDiskId(DefaultDiskId);
            request.SetConfigVersion(1);
            request.SetFillGeneration(1);

            TString buf;
            google::protobuf::util::MessageToJsonString(request, &buf);
            service.ExecuteAction("finishfilldisk", buf);
        }

        volumeConfig = GetVolumeConfig(service, DefaultDiskId);
        UNIT_ASSERT_VALUES_EQUAL(true, volumeConfig.GetIsFillFinished());
    }

    Y_UNIT_TEST(ShouldValidateFinishFillDisk)
    {
        TTestEnv env;
        NProto::TStorageServiceConfig config;
        ui32 nodeIdx = SetupTestEnv(env, std::move(config));

        TServiceClient service(env.GetRuntime(), nodeIdx);

        auto request = service.CreateCreateVolumeRequest();
        request->Record.SetFillGeneration(1);
        service.SendRequest(MakeStorageServiceId(), std::move(request));

        auto response = service.RecvCreateVolumeResponse();
        UNIT_ASSERT_C(response->GetStatus() == S_OK, response->GetStatus());

        {
            // Incorrect ConfigVersion.
            NPrivateProto::TFinishFillDiskRequest request;
            request.SetDiskId(DefaultDiskId);
            request.SetConfigVersion(2);
            request.SetFillGeneration(1);

            TString buf;
            google::protobuf::util::MessageToJsonString(request, &buf);
            service.SendExecuteActionRequest("FinishFillDisk", buf);
            auto response = service.RecvExecuteActionResponse();
            UNIT_ASSERT_VALUES_EQUAL(E_ABORTED, response->GetStatus());
        }

        {
            // ConfigVersion should be supplied.
            NPrivateProto::TFinishFillDiskRequest request;
            request.SetDiskId(DefaultDiskId);
            request.SetFillGeneration(1);

            TString buf;
            google::protobuf::util::MessageToJsonString(request, &buf);
            service.SendExecuteActionRequest("FinishFillDisk", buf);
            auto response = service.RecvExecuteActionResponse();
            UNIT_ASSERT_VALUES_EQUAL(E_ARGUMENT, response->GetStatus());
        }

        {
            // FillGeneration should be supplied.
            NPrivateProto::TFinishFillDiskRequest request;
            request.SetDiskId(DefaultDiskId);
            request.SetConfigVersion(1);

            TString buf;
            google::protobuf::util::MessageToJsonString(request, &buf);
            service.SendExecuteActionRequest("FinishFillDisk", buf);
            auto response = service.RecvExecuteActionResponse();
            UNIT_ASSERT_VALUES_EQUAL(E_ARGUMENT, response->GetStatus());
        }

        {
            // Incorrect FillGeneration.
            NPrivateProto::TFinishFillDiskRequest request;
            request.SetDiskId(DefaultDiskId);
            request.SetConfigVersion(1);
            request.SetFillGeneration(2);

            TString buf;
            google::protobuf::util::MessageToJsonString(request, &buf);
            service.SendExecuteActionRequest("FinishFillDisk", buf);
            auto response = service.RecvExecuteActionResponse();

            ui32 errCode = MAKE_SCHEMESHARD_ERROR(NKikimrScheme::StatusPreconditionFailed);
            UNIT_ASSERT_VALUES_EQUAL(errCode, response->GetStatus());
        }

        auto volumeConfig = GetVolumeConfig(service, DefaultDiskId);
        UNIT_ASSERT_VALUES_EQUAL(false, volumeConfig.GetIsFillFinished());
    }

    Y_UNIT_TEST(ShouldUpdateVolumeParams)
    {
        TTestEnv env;
        NProto::TStorageServiceConfig config;
        config.SetAllocationUnitNonReplicatedSSD(100);
        ui32 nodeIdx = SetupTestEnv(env, config);

        TServiceClient service(env.GetRuntime(), nodeIdx);
        service.CreateVolume(
            DefaultDiskId,
            100_GB / DefaultBlockSize,
            DefaultBlockSize,
            "",
            "",
            NCloud::NProto::STORAGE_MEDIA_SSD_NONREPLICATED);

        TVolumeParamMap volumeParams;
        NProto::TUpdateVolumeParamsMapValue protoParam;
        protoParam.SetValue("10s");
        protoParam.SetTtlMs(100'000'000);
        volumeParams.insert({"max-timed-out-device-state-duration", protoParam});

        bool requestReceived = false;
        env.GetRuntime().SetObserverFunc(
            [&] (TTestActorRuntimeBase& runtime, TAutoPtr<IEventHandle>& event) {
                if (event->GetTypeRewrite() == TEvVolume::EvUpdateVolumeParamsRequest) {
                    auto* msg = event->Get<TEvVolume::TEvUpdateVolumeParamsRequest>();

                    UNIT_ASSERT_VALUES_EQUAL(msg->Record.GetDiskId(), DefaultDiskId);
                    UNIT_ASSERT_VALUES_EQUAL(msg->Record.GetVolumeParams(), volumeParams);
                    requestReceived = true;
                }
                return TTestActorRuntime::DefaultObserverFunc(runtime, event);
            }
        );

        NProto::TUpdateVolumeParamsRequest request;
        request.SetDiskId(DefaultDiskId);
        *request.MutableVolumeParams() = volumeParams;

        TString buf;
        google::protobuf::util::MessageToJsonString(request, &buf);
        service.ExecuteAction("UpdateVolumeParams", buf);

        UNIT_ASSERT(requestReceived);

        service.DestroyVolume();
    }

    void ShouldGetDependentDevicesTest(TVector<TString> returnedDiskIds)
    {
        TTestEnv env(1, 1, 4, 1, MakeIntrusive<TDiskRegistryState>());

        NProto::TStorageServiceConfig config;
        config.SetAllocationUnitNonReplicatedSSD(100);
        ui32 nodeIdx = SetupTestEnv(env, config);

        TServiceClient service(env.GetRuntime(), nodeIdx);

        bool requestReceived = false;

        env.GetRuntime().SetObserverFunc(
            [&] (TTestActorRuntimeBase& runtime, TAutoPtr<IEventHandle>& event) {
                switch (event->GetTypeRewrite()) {
                    case TEvDiskRegistryPrivate::EvGetDependentDisksRequest: {
                        auto* msg = event->Get<TEvDiskRegistryPrivate::TEvGetDependentDisksRequest>();

                        UNIT_ASSERT_VALUES_EQUAL("localhost", msg->Host);

                        requestReceived = true;
                        break;
                    }
                    case TEvDiskRegistryPrivate::EvGetDependentDisksResponse: {
                        auto* msg = event->Get<TEvDiskRegistryPrivate::TEvGetDependentDisksResponse>();
                        msg->DependentDiskIds = returnedDiskIds;
                        break;
                    }
                }
                return TTestActorRuntime::DefaultObserverFunc(runtime, event);
            }
        );

        const auto response = service.ExecuteAction("GetDependentDisks", R"({"Host":"localhost"})");
        UNIT_ASSERT(requestReceived);

        NJson::TJsonValue responseJson;
        if (!NJson::ReadJsonTree(response->Record.GetOutput(), &responseJson, false)) {
            UNIT_ASSERT(false);
        }

        const auto diskIdsJson = responseJson["DependentDiskIds"].GetArray();
        TVector<TString> responseDiskIds;
        for (const auto& diskIdJson: diskIdsJson) {
            responseDiskIds.emplace_back(diskIdJson.GetString());
        }
        UNIT_ASSERT_VALUES_EQUAL(returnedDiskIds, responseDiskIds);
    }


    Y_UNIT_TEST(ShouldGetDependentDevicesNoDisks)
    {
        ShouldGetDependentDevicesTest({});
    }

    Y_UNIT_TEST(ShouldGetDependentDevicesMultipleDisks)
    {
        ShouldGetDependentDevicesTest({"disk1", "disk2"});
    }
}

}   // namespace NCloud::NBlockStore::NStorage


template <>
void Out<TVolumeParamMap>(
    IOutputStream& out,
    const TVolumeParamMap& paramMap)
{
    out << '[';
    for (const auto& [key,value]: paramMap) {
        out << '{'
            << "Key: " << key << ", "
            << "Value: " << value.GetValue() << ", "
            << "Ttl: " << value.GetTtlMs()
            << '}';
    }
    out << ']';
}
