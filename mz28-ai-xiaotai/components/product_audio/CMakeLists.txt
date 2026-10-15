idf_component_register(SRCS "product_audio.c"
                       INCLUDE_DIRS "include"
                       REQUIRES vb6824 rtc_adapter esp_timer freertos
                       PRIV_REQUIRES espressif__esp_audio_codec
                       EMBED_FILES
                           "assets/prompts/activation.opuspkt"
                           "assets/prompts/wifi_provisioning.opuspkt"
                           "assets/prompts/success.opuspkt"
                           "assets/prompts/popup.opuspkt"
                           "assets/prompts/exclamation.opuspkt"
                           "assets/prompts/call_ended.opuspkt"
                           "assets/prompts/ring_incoming.opuspkt"
                           "assets/prompts/ring_outgoing.opuspkt"
                           "assets/prompts/digit_0.opuspkt"
                           "assets/prompts/digit_1.opuspkt"
                           "assets/prompts/digit_2.opuspkt"
                           "assets/prompts/digit_3.opuspkt"
                           "assets/prompts/digit_4.opuspkt"
                           "assets/prompts/digit_5.opuspkt"
                           "assets/prompts/digit_6.opuspkt"
                           "assets/prompts/digit_7.opuspkt"
                           "assets/prompts/digit_8.opuspkt"
                           "assets/prompts/digit_9.opuspkt")
