LIBRARY()

SRCS(
    events.cpp
)

PEERDIR(
    contrib/ydb/core/fq/libs/control_plane_storage/events
    contrib/ydb/core/fq/libs/events
    contrib/ydb/library/yql/public/issue/protos
    contrib/ydb/public/api/protos
)

END()
