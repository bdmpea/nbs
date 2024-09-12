#pragma once

#include "tablet_state_iface.h"

#include <cloud/filestore/libs/storage/tablet/tablet_schema.h>

#include <library/cpp/cache/cache.h>

namespace NCloud::NFileStore::NStorage {

////////////////////////////////////////////////////////////////////////////////

/**
 * @brief Stores the state of the index tables in memory. Can be used to perform
 * read-only operations.
 */
class TInMemoryIndexState : public IIndexTabletDatabase
{
public:
    explicit TInMemoryIndexState(IAllocator* allocator);

    void Reset(
        ui64 nodesCapacity,
        ui64 nodeAttrsCapacity,
        ui64 nodeRefsCapacity);

    //
    // Nodes
    //

    bool ReadNode(
        ui64 nodeId,
        ui64 commitId,
        TMaybe<IIndexTabletDatabase::TNode>& node) override;

private:
    void WriteNode(
        ui64 nodeId,
        ui64 commitId,
        const NProto::TNode& attrs);

    void DeleteNode(ui64 nodeId);

    //
    // Nodes_Ver
    //

public:
    bool ReadNodeVer(
        ui64 nodeId,
        ui64 commitId,
        TMaybe<IIndexTabletDatabase::TNode>& node) override;

    //
    // NodeAttrs
    //

    bool ReadNodeAttr(
        ui64 nodeId,
        ui64 commitId,
        const TString& name,
        TMaybe<IIndexTabletDatabase::TNodeAttr>& attr) override;

    bool ReadNodeAttrs(
        ui64 nodeId,
        ui64 commitId,
        TVector<IIndexTabletDatabase::TNodeAttr>& attrs) override;

private:
    void WriteNodeAttr(
        ui64 nodeId,
        ui64 commitId,
        const TString& name,
        const TString& value,
        ui64 version);

    void DeleteNodeAttr(ui64 nodeId, const TString& name);

    //
    // NodeAttrs_Ver
    //

public:
    bool ReadNodeAttrVer(
        ui64 nodeId,
        ui64 commitId,
        const TString& name,
        TMaybe<IIndexTabletDatabase::TNodeAttr>& attr) override;

    bool ReadNodeAttrVers(
        ui64 nodeId,
        ui64 commitId,
        TVector<IIndexTabletDatabase::TNodeAttr>& attrs) override;

    //
    // NodeRefs
    //

    bool ReadNodeRef(
        ui64 nodeId,
        ui64 commitId,
        const TString& name,
        TMaybe<IIndexTabletDatabase::TNodeRef>& ref) override;

    bool ReadNodeRefs(
        ui64 nodeId,
        ui64 commitId,
        const TString& cookie,
        TVector<IIndexTabletDatabase::TNodeRef>& refs,
        ui32 maxBytes,
        TString* next) override;

    bool PrechargeNodeRefs(
        ui64 nodeId,
        const TString& cookie,
        ui32 bytesToPrecharge) override;

private:
    void WriteNodeRef(
        ui64 nodeId,
        ui64 commitId,
        const TString& name,
        ui64 childNode,
        const TString& followerId,
        const TString& followerName);

    void DeleteNodeRef(ui64 nodeId, const TString& name);

    //
    // NodeRefs_Ver
    //

public:
    bool ReadNodeRefVer(
        ui64 nodeId,
        ui64 commitId,
        const TString& name,
        TMaybe<IIndexTabletDatabase::TNodeRef>& ref) override;

    bool ReadNodeRefVers(
        ui64 nodeId,
        ui64 commitId,
        TVector<IIndexTabletDatabase::TNodeRef>& refs) override;

    //
    // CheckpointNodes
    //

    bool ReadCheckpointNodes(
        ui64 checkpointId,
        TVector<ui64>& nodes,
        size_t maxCount) override;

private:
    // TODO(#1146): use LRU cache / something with better eviction policy
    ui64 NodesCapacity = 0;
    ui64 NodeAttrsCapacity = 0;
    ui64 NodeRefsCapacity = 0;

    //
    // Nodes
    //

    struct TNodeRow
    {
        ui64 CommitId = 0;
        NProto::TNode Node;
    };

    THashMap<ui64, TNodeRow> Nodes;

    //
    // NodeAttrs
    //

    struct TNodeAttrsKey
    {
        TNodeAttrsKey(ui64 nodeId, const TString& name)
            : NodeId(nodeId)
            , Name(name)
        {}

        ui64 NodeId = 0;
        TString Name;

        bool operator==(const TNodeAttrsKey& rhs) const
        {
            return std::tie(NodeId, Name) == std::tie(rhs.NodeId, rhs.Name);
        }
    };

    struct TNodeAttrsKeyHash
    {
        size_t operator()(const TNodeAttrsKey& key) const
        {
            return MultiHash(key.NodeId, key.Name);
        }
    };

    struct TNodeAttrsRow
    {
        ui64 CommitId = 0;
        TString Value;
        ui64 Version = 0;
    };

    THashMap<TNodeAttrsKey, TNodeAttrsRow, TNodeAttrsKeyHash> NodeAttrs;

    //
    // NodeRefs
    //

    struct TNodeRefsKey
    {
        TNodeRefsKey(ui64 nodeId, const TString& name)
            : NodeId(nodeId)
            , Name(name)
        {}

        ui64 NodeId = 0;
        TString Name;

        bool operator==(const TNodeRefsKey& rhs) const
        {
            return std::tie(NodeId, Name) == std::tie(rhs.NodeId, rhs.Name);
        }
    };

    struct TNodeRefsKeyHash
    {
        size_t operator()(const TNodeRefsKey& key) const
        {
            return MultiHash(key.NodeId, key.Name);
        }
    };

    struct TNodeRefsRow
    {
        ui64 CommitId = 0;
        ui64 ChildId = 0;
        TString FollowerId;
        TString FollowerName;
    };

    THashMap<TNodeRefsKey, TNodeRefsRow, TNodeRefsKeyHash> NodeRefs;

public:
    struct TWriteNodeRequest
    {
        ui64 NodeId = 0;
        TNodeRow Row;
    };

    struct TDeleteNodeRequest
    {
        ui64 NodeId = 0;
    };

    struct TWriteNodeAttrsRequest
    {
        TNodeAttrsKey NodeAttrsKey;
        TNodeAttrsRow NodeAttrsRow;
    };

    using TDeleteNodeAttrsRequest = TNodeAttrsKey;

    struct TWriteNodeRefsRequest
    {
        TNodeRefsKey NodeRefsKey;
        TNodeRefsRow NodeRefsRow;
    };

    using TDeleteNodeRefsRequest = TNodeRefsKey;

    using TIndexStateRequest = std::variant<
        TWriteNodeRequest,
        TDeleteNodeRequest,
        TWriteNodeAttrsRequest,
        TDeleteNodeAttrsRequest,
        TWriteNodeRefsRequest,
        TDeleteNodeRefsRequest>;

    void UpdateState(const TVector<TIndexStateRequest>& nodeUpdates);
};

}   // namespace NCloud::NFileStore::NStorage
