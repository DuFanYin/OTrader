#!/bin/bash
# Regenerate the server's Python gRPC/protobuf stubs (app/server/optra/proto) from
# engine/entry/optra_engine.proto. The C++ code is not generated here: CMake generates it at
# build time (engine/entry/CMakeLists.txt).
# Runs in the server's uv environment; grpcio-tools is pinned to the locked grpcio, so the stubs
# never require a newer grpcio than the one installed.

set -e
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
PROTO_DIR="$ROOT/engine/entry"
OUT="$ROOT/app/server/optra/proto"
GRPCIO_TOOLS="grpcio-tools==1.82.1" # = grpcio in app/server/uv.lock

echo "Generating Python proto..."
(cd "$ROOT/app/server" && uv run --with "$GRPCIO_TOOLS" python -m grpc_tools.protoc \
    -I "$PROTO_DIR" --python_out="$OUT" --grpc_python_out="$OUT" \
    "$PROTO_DIR/optra_engine.proto")

# The generated stub imports its messages as a top-level module; the server imports optra.proto.
sed -i 's/^import optra_engine_pb2/from optra.proto import optra_engine_pb2/' \
    "$OUT/optra_engine_pb2_grpc.py"

echo "Done. Python stubs in $OUT"
