#!/usr/bin/env bash
set -euo pipefail
project_dir="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
for tool in python3 cc c++ cmake; do
    if ! command -v "$tool" >/dev/null 2>&1; then
        printf 'Missing host test tool: %s. See README.md for prerequisites.\n' "$tool" >&2
        exit 1
    fi
done
for component in espressif__cjson espressif__esp-tflite-micro lvgl__lvgl; do
    if [[ ! -d "$project_dir/managed_components/$component" ]]; then
        printf 'Missing %s: run idf.py reconfigure in the ESP-IDF environment first.\n' "$component" >&2
        exit 1
    fi
done
python3 "$project_dir/tools/test_build_policies.py"
python3 "$project_dir/tools/test_mqtt_psram_buffers.py"
python3 "$project_dir/tools/test_product_preferences.py"
python3 "$project_dir/tools/test_home_wake_hint.py"
python3 "$project_dir/tools/test_face_animation.py"
python3 "$project_dir/tools/test_nvs_worker.py"
python3 "$project_dir/tools/test_runtime_config.py"
python3 "$project_dir/tools/test_runtime_init.py"
python3 "$project_dir/tools/test_voice_reliability.py"
python3 "$project_dir/tools/test_ai_start_contract.py"
python3 "$project_dir/tools/test_ai_audio_drain.py"
python3 "$project_dir/tools/test_room.py"
python3 "$project_dir/tools/test_room_ui.py"
python3 "$project_dir/tools/test_release_assets.py"
python3 "$project_dir/tools/test_captive_portal_contract.py"
python3 "$project_dir/tools/test_station_identity.py"
python3 "$project_dir/tools/test_binding_prompt_cancel.py"
python3 "$project_dir/tools/test_binding_lifecycle.py"
python3 "$project_dir/tools/test_platform_http_trace.py"
python3 "$project_dir/tools/test_clock_fallback.py"
python3 "$project_dir/tools/test_portal_interface.py"
python3 "$project_dir/tools/test_sdk_start_diagnostic.py"
python3 "$project_dir/tools/test_tflm_conv_channels.py"
python3 "$project_dir/tools/test_wifi_credentials.py"
python3 "$project_dir/tools/test_wifi_portal.py"
python3 "$project_dir/tools/test_playout.py"
python3 "$project_dir/tools/test_wifi_manual_disconnect.py"
python3 "$project_dir/tools/test_offline_onboarding.py"
python3 "$project_dir/tools/test_voip_task_cleanup.py"
python3 "$project_dir/tools/test_call_room_recovery.py"
python3 "$project_dir/tools/test_contact_entry.py"
python3 "$project_dir/tools/test_contact_query.py"
python3 "$project_dir/tools/test_screen_event_lifetime.py"
bash "$project_dir/tools/test_audio_resampler.sh"
python3 "$project_dir/tools/test_audio_only.py"
python3 "$project_dir/tools/test_media_platform_contract.py"
python3 "$project_dir/tools/test_audio_boot_gain.py"
