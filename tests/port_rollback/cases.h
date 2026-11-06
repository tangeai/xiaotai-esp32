static lv_display_t *create_display(int mode)
{
    lvgl_port_display_cfg_t cfg = {
        .panel_handle = panel_buffers, .buffer_size = 64, .hres = 8, .vres = 8,
        .color_format = LV_COLOR_FORMAT_RGB565, .double_buffer = true,
    };
    lvgl_port_disp_priv_cfg_t priv = {.disp_type = LVGL_PORT_DISP_TYPE_DSI, .avoid_tearing = true};
    if (mode == 1) cfg.flags.sw_rotate = true;
    if (mode == 2) cfg.flags.direct_mode = true;
    if (mode == 3) { cfg.monochrome = true; cfg.color_format = LV_COLOR_FORMAT_I1; }
    if (mode == 4) { cfg.flags.direct_mode = true; cfg.buffer_size = 32; }
    if (mode == 5) { cfg.monochrome = true; cfg.color_format = LV_COLOR_FORMAT_RGB888; }
    if (mode == 6) { cfg.flags.full_refresh = true; cfg.buffer_size = 32; }
    return lvgl_port_add_disp_priv(&cfg, mode == 2 ? &priv : NULL);
}

int main(void)
{
    for (int cycle = 0; cycle < 100; ++cycle) {
        for (int mode = 0; mode < 7; ++mode) {
            alloc_calls = 0; fail_at = 0;
            lv_display_t *display = create_display(mode);
            int count = alloc_calls;
            assert((display != NULL) == (mode < 4));
            if (display) assert(lvgl_port_remove_disp(display) == ESP_OK);
            assert(live == 0);
            for (int failure = 1; failure <= count; ++failure) {
                alloc_calls = 0; fail_at = failure;
                assert(create_display(mode) == NULL);
                assert(live == 0);
            }
        }
        frame_failure = 1; fail_at = 0;
        assert(create_display(2) == NULL);
        assert(live == 0);
        frame_failure = 0;
    }
    puts("LVGL port: 100 repeated allocation-failure sweeps, validation failures and borrowed-buffer ownership passed");
    return 0;
}
