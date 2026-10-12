#!/usr/bin/env bash
set -euo pipefail

project_dir="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
tmp_dir="$(mktemp -d)"
trap 'rm -rf "$tmp_dir"' EXIT

cc -std=c11 -Wall -Wextra -Werror \
   -I"$project_dir/components/starter_media/src" \
   "$project_dir/tools/test_audio_resampler.c" \
   "$project_dir/components/starter_media/src/starter_audio_resampler.c" \
   -o "$tmp_dir/test_audio_resampler"
"$tmp_dir/test_audio_resampler"
