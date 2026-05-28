add_library(engine_state
    EntityStateStore.cpp
    MarketStateView.cpp
    StateHasher.cpp
    book/LOBWriter.cpp
    chain/ChainStateWriter.cpp
    chain/ConfirmedTradeState.cpp
    chain/SettlementState.cpp
    chain/SettlementStateWriter.cpp
    core/GlobalBitmaskState.cpp
    core/MarketStateEvent.cpp
    core/MarketStateEventAdapter.cpp
    core/MarketStateStore.cpp
    quality/BookQualityState.cpp
    quality/DataQualityGate.cpp
    quality/ReconciliationState.cpp
    shard/LOBShard.cpp
    shard/ShardRouter.cpp
    snapshot/SnapshotPublisher.cpp
)

target_include_directories(engine_state PUBLIC
    ${CMAKE_SOURCE_DIR}/engine
)

target_link_libraries(engine_state PUBLIC
    engine_decode
    engine_chain_confirm
)
