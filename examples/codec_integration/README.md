# Task-08 真实项目接入示例（Proto / FlatBuffers）

本目录给出一个可直接迁移到真实项目的 Task-08 示例。

- `game_message.proto`：Protobuf schema
- `game_message.fbs`：FlatBuffers schema
- `codec_integration_examples.cpp`：已按 `mini::codec` 接口接入的示例

## 1. 生成代码（真实项目必做）

在 `examples/codec_integration` 目录下执行：

```bash
cd /home/xyq/mini-trantor/examples/codec_integration
mkdir -p generated

# Protobuf
protoc \
  --proto_path=. \
  --cpp_out=generated \
  game_message.proto

# FlatBuffers
flatc \
  --cpp \
  --gen-object-api \
  -o generated \
  game_message.fbs
```

生成的文件（真实项目中通常会放进 `build` 产物目录）：

- `generated/game_message.pb.h`
- `generated/game_message.pb.cc`
- `generated/game_message_generated.h`

## 2. 接入方式

### 2.1 Protobuf

```cpp
using GameMessageCodec = mini::codec::ProtobufAdapter<game::protocol::GameMessage>;

mini::rpc::RpcChannel rpc{loop};
GameMessageCodec codec;

// 发送：按方法名发起 RPC，带 typed payload
rpc.sendRequest(conn, "Game.Move", requestMsg, codec,
                [](const std::string& err, const std::string& payload){
    if (!err.empty()) {
        // encode 或超时失败
    } else {
        // 处理响应 payload
    }
});

// 接收：业务层按 method 绑定
rpc.setRequestCallback([&codec](std::string_view method,
                                std::string_view payload,
                                auto respond,
                                auto respondErr){
    game::protocol::GameMessage req;
    std::string err;
    if (!codec.decodeMessage(payload, req, &err)) {
        respondErr(err);
        return;
    }
    // ... 业务处理 req ...
    respond("ok");
});
```

### 2.2 FlatBuffers（`FlatBuffersAdapter` + 自定义编码闭包）

```cpp
using GameStateCodec = mini::codec::FlatBuffersAdapter<FlatGameState>;

const GameStateCodec& codec = [] {
    static const GameStateCodec c(
        [](const FlatGameState& msg, std::string* payload) {
            return encodeFlatGame(msg, payload); // 用户提供序列化函数
        },
        [](std::string_view data, FlatGameState& out) {
            return decodeFlatGame(data, out);   // 用户提供反序列化函数
        });
    return c;
}();

mini::net::broadcast::BroadcastDispatcher dispatcher{baseLoop};
std::vector<mini::net::broadcast::BroadcastRouter::LoopBatch> batches = router.route(sessionIds);
dispatcher.dispatch(std::move(batches), stateMsg, codec, &err);
```

`encodeFlatGame` / `decodeFlatGame` 可直接参考：

- `flatbuffers::FlatBufferBuilder` 构建 `game::protocol::CreateGameMessage`
- 使用 `game::protocol::VerifyGameMessageBuffer(Verifier)` 校验

## 3. 可选 CMake 示例（可直接落地）

```cmake
# 真实项目里把生成文件加入构建
find_package(Protobuf REQUIRED)
# 如果使用 CMake 的 FlatBuffers 包
find_package(FlatBuffers REQUIRED)

add_library(game_schemas
    examples/codec_integration/generated/game_message.pb.cc
)

target_include_directories(game_schemas
    PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/examples/codec_integration/generated
)

target_link_libraries(game_schemas
    PRIVATE
    protobuf::libprotobuf
)

add_executable(codec_integration_example
    examples/codec_integration/codec_integration_examples.cpp
)

target_include_directories(codec_integration_example
    PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}
        ${CMAKE_CURRENT_SOURCE_DIR}/examples/codec_integration/generated
)

target_link_libraries(codec_integration_example
    PRIVATE
        mini_trantor
        game_schemas
)

target_link_libraries(codec_integration_example
    PRIVATE
        FlatBuffers::flatbuffers  # 仅当需要 FBS 分支时
)
```

## 4. 说明

- `codec_integration_examples.cpp` 用预编译检测 (`__has_include`) 做了 `PROTO` / `FLATBUFFERS` 分支隔离；
  没有生成文件时不会进入对应代码，便于仓库自带示例长期存在。
- 真实项目建议在业务编译目标里把 `PROTO`、`FLATBUFFERS` 分支都打开（同时引入对应库），
  并将生成目录加入 `include_directories`。
