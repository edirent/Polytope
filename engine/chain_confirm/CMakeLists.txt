add_library(engine_chain_confirm STATIC
    ChainConfirmConfig.cpp
    ClassifiedFillRecord.cpp
    ConfirmedFill.cpp
    ConfirmedFillStore.cpp
    DirectionLabel.cpp
    EthLogDecoder.cpp
    FillReconciler.cpp
    OrderFilledDecoder.cpp
    PendingTradeHintRing.cpp
    PolygonHttpBackfillClient.cpp
    PolygonWsLogClient.cpp
    ReconciliationResult.cpp
)

target_include_directories(engine_chain_confirm PUBLIC
    ${CMAKE_SOURCE_DIR}/engine
)
