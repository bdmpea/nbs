#pragma once

#include <ydb/library/conclusion/status.h>
#include <ydb/public/api/protos/ydb_status_codes.pb.h>


#include <ydb/core/tx/scheme_cache/scheme_cache.h>
#include <ydb/core/tx/columnshard/columnshard.h>

#include <contrib/libs/apache/arrow/cpp/src/arrow/record_batch.h>


namespace NKikimr::NEvWrite {

using TEvWrite = TEvColumnShard::TEvWrite;
using TEvWriteResult = TEvColumnShard::TEvWriteResult;

class IShardsSplitter {
public:
    using TPtr = std::shared_ptr<IShardsSplitter>;
    using TYdbConclusionStatus = TConclusionSpecialStatus<Ydb::StatusIds::StatusCode, Ydb::StatusIds::SUCCESS, Ydb::StatusIds::SCHEME_ERROR>;

    class IEvWriteDataAccessor {
    public:
        using TPtr = std::shared_ptr<IEvWriteDataAccessor>;

        virtual bool HasDeserializedBatch() const {
            return !!GetDeserializedBatch();
        }
        virtual std::shared_ptr<arrow::RecordBatch> GetDeserializedBatch() const = 0;
        virtual TString GetSerializedData() const = 0;
        virtual ~IEvWriteDataAccessor() {}
    };

    class IShardInfo {
    public:
        using TPtr = std::shared_ptr<IShardInfo>;
        virtual ~IShardInfo() {}

        virtual void Serialize(TEvWrite& evWrite) const = 0;
        virtual ui64 GetBytes() const = 0;
        virtual ui32 GetRowsCount() const = 0;
    };

    class TFullSplitData {
    private:
        THashMap<ui64, std::vector<IShardInfo::TPtr>> ShardsInfo;
        ui32 AllShardsCount = 0;
    public:
        TFullSplitData(ui64 allShardsCount)
            : AllShardsCount(allShardsCount) {}

        TString ShortLogString(const ui32 sizeLimit) const;
        ui32 GetShardRequestsCount() const;
        const THashMap<ui64, std::vector<IShardInfo::TPtr>>& GetShardsInfo() const;
        ui32 GetShardsCount() const;
        void AddShardInfo(const ui64 tabletId, const IShardInfo::TPtr& info);
    };

    virtual ~IShardsSplitter() {}

    TYdbConclusionStatus SplitData(const NSchemeCache::TSchemeCacheNavigate::TEntry& schemeEntry, const IEvWriteDataAccessor& data) {
        TableId = schemeEntry.TableId.PathId.LocalPathId;
        return DoSplitData(schemeEntry, data);
    }

    ui64 GetTableId() const {
        return TableId;
    }

    const TFullSplitData& GetSplitData() const {
        Y_VERIFY(FullSplitData);
        return *FullSplitData;
    }

    static TPtr BuildSplitter(const NSchemeCache::TSchemeCacheNavigate::TEntry& schemeEntry);

private:
    virtual TYdbConclusionStatus DoSplitData(const NSchemeCache::TSchemeCacheNavigate::TEntry& schemeEntry, const IEvWriteDataAccessor& data) = 0;

    ui64 TableId = 0;
protected:
    std::optional<TFullSplitData> FullSplitData;
};
}
