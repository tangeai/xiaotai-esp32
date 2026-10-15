#!/usr/bin/env sh
set -eu
: "${IDF_PATH:?Set IDF_PATH to ESP-IDF 5.5.5}"
root=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
out="$root/build-host"
cc=${CC:-cc}
json="$IDF_PATH/components/json/cJSON"
mkdir -p "$out"
"$cc" -std=c11 -Wall -Wextra -Werror -I "$root/components/vb6824" \
    "$root/tests/test_vb6824_wire.c" \
    "$root/components/vb6824/vb6824_wire.c" -o "$out/test_vb6824_wire"
"$out/test_vb6824_wire"
"$cc" -std=c11 -Wall -Wextra -Werror "$root/tests/test_message_buffer.c" \
    -o "$out/test_message_buffer"
"$out/test_message_buffer"
"$cc" -std=c11 -Wall -Wextra -Werror -I "$root/main" \
    "$root/tests/test_boot_button_logic.c" "$root/main/boot_button_logic.c" \
    -o "$out/test_boot_button_logic"
"$out/test_boot_button_logic"
"$cc" -std=c11 -Wall -Wextra -Werror "$root/tests/test_state.c" \
    "$root/components/wechat_voip/voip_state.c" -o "$out/test_state"
"$cc" -std=c11 -Wall -Wextra -Werror -I "$json" "$root/tests/test_protocol.c" \
    "$root/components/wechat_voip/voip_protocol.c" "$json/cJSON.c" -lm -o "$out/test_protocol"
"$out/test_state"
"$out/test_protocol"
"$cc" -std=gnu11 -Wall -Wextra -Werror -I "$root/tests/stubs" \
    -I "$root/components/wechat_voip" -I "$root/components/rtc_adapter/include" \
    -I "$root/components/tirtc_sdk/include" "$root/tests/test_audio.c" \
    "$root/tests/audio_fixture.S" "$root/components/wechat_voip/sample_audio.c" \
    "$root/components/wechat_voip/opus_packet.c" \
    -lm -o "$out/test_audio"
"$out/test_audio"
"$cc" -std=gnu11 -Wall -Wextra -Werror -ffunction-sections -fdata-sections \
    -I "$root/tests/stubs" -I "$root/components/rtc_adapter/include" \
    -I "$root/components/tirtc_sdk/include" "$root/tests/test_rtc_inputs.c" \
    "$root/components/rtc_adapter/rtc_adapter.c" -Wl,--gc-sections -o "$out/test_rtc_inputs"
"$out/test_rtc_inputs"
"$cc" -std=gnu11 -Wall -Wextra -Werror -ffunction-sections -fdata-sections \
    -I "$root/tests/stubs" -I "$root/components/rtc_adapter/include" \
    -I "$root/components/tirtc_sdk/include" "$root/tests/test_rtc_contract.c" \
    "$root/components/rtc_adapter/rtc_adapter.c" -Wl,--gc-sections -o "$out/test_rtc_contract"
"$out/test_rtc_contract"
"$cc" -std=gnu11 -Wall -Wextra -Werror -ffunction-sections -fdata-sections \
    -I "$root/tests/voip_stubs" -I "$root/tests/stubs" -I "$json" \
    -I "$root/components/wechat_voip/include" -I "$root/components/demo_platform/include" \
    -I "$root/components/rtc_adapter/include" -I "$root/components/tirtc_sdk/include" \
    "$root/tests/test_voip_reject.c" "$json/cJSON.c" -Wl,--gc-sections -lm \
    -o "$out/test_voip_reject"
"$out/test_voip_reject"
"$cc" -std=gnu11 -Wall -Wextra -Werror -ffunction-sections -fdata-sections \
    -I "$root/tests/voip_stubs" -I "$root/tests/stubs" -I "$json" \
    -I "$root/components/wechat_voip/include" -I "$root/components/demo_platform/include" \
    -I "$root/components/rtc_adapter/include" -I "$root/components/tirtc_sdk/include" \
    "$root/tests/test_voip_messages.c" -Wl,--gc-sections -pthread -o "$out/test_voip_messages"
"$out/test_voip_messages"
"$cc" -std=gnu11 -Wall -Wextra -Werror -ffunction-sections -fdata-sections \
    -I "$root/tests/voip_stubs" -I "$root/tests/stubs" -I "$json" \
    -I "$root/components/wechat_voip/include" -I "$root/components/demo_platform/include" \
    -I "$root/components/rtc_adapter/include" -I "$root/components/tirtc_sdk/include" \
    "$root/tests/test_voip_join_lifetime.c" "$root/components/wechat_voip/voip_state.c" \
    "$root/components/wechat_voip/voip_protocol.c" "$json/cJSON.c" -Wl,--gc-sections -lm \
    -o "$out/test_voip_join_lifetime"
"$out/test_voip_join_lifetime"
"$cc" -std=c11 -Wall -Wextra -Werror -shared -fPIC \
    "$root/components/wechat_voip/opus_packet.c" -o "$out/libpacket_test.so"
python3 -m unittest discover -s "$root/tests" -v
