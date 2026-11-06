# Applied only after the pinned upstream source hash has been checked.
# Record ownership immediately and unwind the LVGL object before its buffers.
string(REPLACE
    "        if (disp_cfg->double_buffer) {"
    "        disp_ctx->draw_buffs[0] = buf1;\n        if (disp_cfg->double_buffer) {"
    source "${source}")
string(REPLACE
    "    disp = lv_display_create(disp_cfg->hres, disp_cfg->vres);"
    "    disp = lv_display_create(disp_cfg->hres, disp_cfg->vres);\n    ESP_GOTO_ON_FALSE(disp, ESP_ERR_NO_MEM, err, TAG, \"LVGL display allocation failed\");"
    source "${source}")
string(REPLACE
    "ESP_RETURN_ON_FALSE(display_color_format == LV_COLOR_FORMAT_RGB565\n                            || display_color_format == LV_COLOR_FORMAT_I1, NULL, TAG,"
    "ESP_GOTO_ON_FALSE(display_color_format == LV_COLOR_FORMAT_RGB565\n                            || display_color_format == LV_COLOR_FORMAT_I1, ESP_ERR_INVALID_ARG, err, TAG,"
    source "${source}")
string(REPLACE
    "        assert(disp_ctx->ppa_handle != NULL);"
    "        ESP_GOTO_ON_FALSE(disp_ctx->ppa_handle, ESP_ERR_NO_MEM, err, TAG, \"PPA rotation allocation failed\");"
    source "${source}")
set(old_cleanup [=[err:
    if (ret != ESP_OK) {
        if (disp_ctx->draw_buffs[0]) {
            free(disp_ctx->draw_buffs[0]);
        }
        if (disp_ctx->draw_buffs[1]) {
            free(disp_ctx->draw_buffs[1]);
        }
        if (disp_ctx->draw_buffs[2]) {
            free(disp_ctx->draw_buffs[2]);
        }
        if (disp_ctx->oled_buffer) {
            free(disp_ctx->oled_buffer);
        }
        if (disp_ctx) {
            free(disp_ctx);
        }
        if (trans_sem) {
            vSemaphoreDelete(trans_sem);
        }
    }
]=])
set(new_cleanup [=[err:
    if (ret != ESP_OK) {
        if (disp) {
            lv_display_delete(disp);
            disp = NULL;
        }
        if (disp_ctx) {
            free(disp_ctx->draw_buffs[0]);
            free(disp_ctx->draw_buffs[1]);
            free(disp_ctx->draw_buffs[2]);
            free(disp_ctx->oled_buffer);
#if LVGL_PORT_PPA
            if (disp_ctx->ppa_handle) lvgl_port_ppa_delete(disp_ctx->ppa_handle);
#endif
            free(disp_ctx);
        }
        if (trans_sem) vSemaphoreDelete(trans_sem);
    }
]=])
string(FIND "${source}" "${old_cleanup}" cleanup_anchor)
if(cleanup_anchor EQUAL -1)
    message(FATAL_ERROR "LVGL allocation rollback source changed")
endif()
string(REPLACE "${old_cleanup}" "${new_cleanup}" source "${source}")
