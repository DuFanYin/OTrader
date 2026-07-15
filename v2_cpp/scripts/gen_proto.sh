#!/bin/bash
# Regenerate proto stubs for C++ and Python.
# Requires: protoc, grpc_cpp_plugin (C++), grpcio-tools (Python: pip install grpcio-tools)

set -e
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
PROTO_DIR="$ROOT/Otrader/proto"
PROTO_FILE="$PROTO_DIR/otrader_engine.proto"

# C++ (use Homebrew protoc on macOS if available)
if command -v /opt/homebrew/bin/protoc &>/dev/null; then
    PROTOC=/opt/homebrew/bin/protoc
    GRPC_PLUGIN=/opt/homebrew/bin/grpc_cpp_plugin
else
    PROTOC=protoc
    GRPC_PLUGIN=grpc_cpp_plugin
fi
echo "Generating C++ proto..."
$PROTOC -I "$PROTO_DIR" --cpp_out="$PROTO_DIR" --grpc_out="$PROTO_DIR" \
    --plugin=protoc-gen-grpc="$GRPC_PLUGIN" "$PROTO_FILE"
# ZMQ messages (protobuf only, no gRPC)
$PROTOC -I "$PROTO_DIR" --cpp_out="$PROTO_DIR" "$PROTO_DIR/zmq_messages.proto"

# Python (requires: pip install grpcio-tools)
echo "Generating Python proto..."
mkdir -p "$ROOT/backend/proto"
python -m grpc_tools.protoc -I "$PROTO_DIR" \
    --python_out="$ROOT/backend/proto" --grpc_python_out="$ROOT/backend/proto" \
    "$PROTO_FILE"

# Fix grpc import for backend.proto package (generated file uses "import otrader_engine_pb2")
if [ -f "$ROOT/backend/proto/otrader_engine_pb2_grpc.py" ]; then
    sed -i.bak 's/^import otrader_engine_pb2/from backend.proto import otrader_engine_pb2/' \
        "$ROOT/backend/proto/otrader_engine_pb2_grpc.py" 2>/dev/null || true
    rm -f "$ROOT/backend/proto/otrader_engine_pb2_grpc.py.bak" 2>/dev/null || true
fi

echo "Done. C++ outputs in $PROTO_DIR (Otrader/proto), Python in $ROOT/backend/proto"
