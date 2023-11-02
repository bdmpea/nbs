LIBRARY()

SRCS(
    yql_generic_cluster_config.cpp
    yql_generic_datasink.cpp
    yql_generic_datasink_execution.cpp
    yql_generic_datasink_type_ann.cpp
    yql_generic_datasource.cpp
    yql_generic_datasource_type_ann.cpp
    yql_generic_dq_integration.cpp
    yql_generic_io_discovery.cpp
    yql_generic_load_meta.cpp
    yql_generic_logical_opt.cpp
    yql_generic_mkql_compiler.cpp
    yql_generic_physical_opt.cpp
    yql_generic_predicate_pushdown.cpp
    yql_generic_provider.cpp
    yql_generic_provider.h
    yql_generic_provider_impl.h
    yql_generic_settings.h
    yql_generic_settings.cpp
    yql_generic_state.h
    yql_generic_state.cpp
)

YQL_LAST_ABI_VERSION()

PEERDIR(
    contrib/libs/fmt
    library/cpp/json
    library/cpp/random_provider
    library/cpp/time_provider
    ydb/core/fq/libs/result_formatter
    ydb/library/yql/ast
    ydb/library/yql/core
    ydb/library/yql/core/type_ann
    ydb/library/yql/dq/expr_nodes
    ydb/library/yql/dq/integration
    ydb/library/yql/minikql/comp_nodes/llvm
    ydb/library/yql/providers/common/config
    ydb/library/yql/providers/common/db_id_async_resolver
    ydb/library/yql/providers/common/dq
    ydb/library/yql/providers/common/mkql
    ydb/library/yql/providers/common/proto
    ydb/library/yql/providers/common/provider
    ydb/library/yql/providers/common/pushdown
    ydb/library/yql/providers/common/structured_token
    ydb/library/yql/providers/common/transform
    ydb/library/yql/providers/dq/common
    ydb/library/yql/providers/dq/expr_nodes
    ydb/library/yql/providers/generic/expr_nodes
    ydb/library/yql/providers/generic/proto
    ydb/library/yql/providers/generic/connector/libcpp
)

END()

RECURSE_FOR_TESTS(ut)
