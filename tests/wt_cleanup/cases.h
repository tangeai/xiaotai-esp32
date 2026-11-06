static void reset_fixture(void)
{
    s_panel = s_panel_io = s_dsi = s_touch = s_touch_io = s_phy_ldo = NULL;
    s_display = s_indev = NULL;
    s_backlight_ready = s_lvgl_ready = s_cleanup_failed = false;
    live_display = panel_live = locks = starts = cleanup_step = cleanup_fail = 0;
    fail_input = fail_display = fail_lock = fail_dpi = frees = 0;
}

int main(void)
{
    const bsp_display_cfg_t cfg = {0};
    for (int i = 0; i < 100; ++i) {
        reset_fixture();
        fail_input = 1;
        assert(bsp_display_start_with_config(&cfg) == NULL);
        assert(!s_cleanup_failed && !s_display && !s_panel && !live_display && !panel_live && !locks);
        assert(!s_dsi && !s_touch && !s_touch_io && !s_phy_ldo);
        fail_input = 0;
        assert(bsp_display_start_with_config(&cfg) != NULL);
        assert(bsp_display_start_with_config(&cfg) == s_display && starts == 2);
        assert(lvgl_port_lock(1000));
        cleanup_step = 0;
        assert(display_cleanup() == ESP_OK);
        lvgl_port_unlock();
        assert(!live_display && !panel_live);
    }
    /* Failure must keep the unfreed owner and must not return a partial display. */
    for (int failure = 1; failure <= 9; ++failure) {
        reset_fixture();
        fail_input = 1; cleanup_fail = failure;
        assert(bsp_display_start_with_config(&cfg) == NULL);
        assert(s_cleanup_failed && !locks);
        assert(bsp_display_start_with_config(&cfg) == NULL && starts == 1);
        if (failure == 1) assert(s_panel && s_display && panel_live && live_display);
    }
    for (int mode = 0; mode < 2; ++mode) {
        reset_fixture();
        fail_display = mode == 0; fail_lock = mode == 1;
        assert(bsp_display_start_with_config(&cfg) == NULL);
        assert(!s_cleanup_failed && !s_panel && !s_display && !locks);
    }
    reset_fixture();
    jd9165_panel_t *vendor = calloc(1, sizeof(*vendor));
    vendor->del = fake_dpi_del;
    vendor->reset_gpio_num = -1;
    esp_lcd_panel_t panel = {.user_data = vendor};
    fail_dpi = 1;
    assert(panel_jd9165_del(&panel) == ESP_FAIL && frees == 0 && panel.user_data == vendor);
    fail_dpi = 0;
    assert(panel_jd9165_del(&panel) == ESP_OK && frees == 1);
    puts("WT display: callback lifetime, cleanup errors, retained ownership and repeated start passed");
    return 0;
}
