add_library(engine_reward STATIC
    client/PrivateRewardAccountClient.cpp
    client/PublicRewardConfigClient.cpp
    features/RewardMarketFeatureBuilder.cpp
    filter/RewardMarketFilter.cpp
    normalize/RewardMarketNormalizer.cpp
    public/RewardTypes.cpp
    store/RewardUniverseStore.cpp
)

target_compile_features(engine_reward PUBLIC cxx_std_20)

target_include_directories(engine_reward PUBLIC
    ${CMAKE_SOURCE_DIR}
    ${CMAKE_SOURCE_DIR}/engine
)

target_link_libraries(engine_reward PUBLIC
    engine_strategy_market_making
)
