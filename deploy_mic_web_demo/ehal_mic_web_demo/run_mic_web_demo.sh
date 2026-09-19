#!/bin/sh

ROOT_DIR=$(cd "$(dirname "$0")" && pwd)
PORT=${PORT:-8091}

cd "$ROOT_DIR" || exit 1
export LD_LIBRARY_PATH="$ROOT_DIR/lib:/app/lib/lib-hal:/app/lib/lib-chip:/app/lib/lib-thrid:/app/lib:${LD_LIBRARY_PATH:-}"

exec "$ROOT_DIR/bin/mic_web_demo" \
    --port "$PORT" \
    --web-root "$ROOT_DIR/web" \
    --default-config "$ROOT_DIR/configs/hal_default.json" \
    --pcm-runtime-config "$ROOT_DIR/configs/pcm_runtime.json" \
    --opus-runtime-config "$ROOT_DIR/configs/opus_runtime.json" \
    --playback-audio "$ROOT_DIR/audio/speaker_voice.wav"
