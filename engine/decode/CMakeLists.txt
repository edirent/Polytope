add_library(engine_decode STATIC
    core/DecodePipeline.cpp
    core/DecodeWorker.cpp
    codecs/CodecRegistry.cpp
    codecs/NoneCodec.cpp
    codecs/GzipCodec.cpp
    codecs/Lz4Codec.cpp
    codecs/ZstdCodec.cpp
    json/JsonDecoder.cpp
    parser/ParserRegistry.cpp
    parser/polymarket/PolymarketParser.cpp
    normalize/EventNormalizer.cpp
    normalize/Normalizer.cpp
    publish/EventPublisher.cpp
    metrics/DecodeMetrics.cpp
    replay/DecodeReplayRunner.cpp
)

target_include_directories(engine_decode
    PUBLIC
        ${CMAKE_CURRENT_SOURCE_DIR}/public
        ${CMAKE_SOURCE_DIR}/engine
    PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}
)
