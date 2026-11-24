int main(void)
{
    eh_spi_rx_task(NULL);
    const uint16_t expected[] = {65535, 0, 3, 3, 2, 4};
    assert(step == 12 && transfers == 11 && dequeues == 11 && tx_frees == 1);
    assert(posts == 2 && locks == 0 && dispatches == 6 && hci == 1 && !s_dr_pending);
    assert(memcmp(delivered, expected, sizeof(expected)) == 0);
#ifdef DIAGNOSTICS
    assert(atomic_load(&s_diag_xfer) == 11 && atomic_load(&s_diag_sta) == 6);
    assert(atomic_load(&s_diag_bad) == 1 && atomic_load(&s_diag_dummy) == 1);
    assert(atomic_load(&s_diag_timeout_dr) == 2 && atomic_load(&s_diag_seq_gap) == 3);
    assert(atomic_load(&s_diag_seq_repeat) == 1);
    assert(atomic_load(&s_diag_dispatch_us) == 12 && atomic_load(&s_diag_xfer_us) == 12);
    eh_host_bus_spi_log_diagnostics();
    assert(strcmp(log_line, "NET SPI xfer=11 sta=6 bad=1 dummy=1 timeout_dr=2 seq_gap=3 seq_repeat=1 dispatch_max_us=12 xfer_max_us=12") == 0);
#endif
    puts("Hosted SPI: handshake, TX ownership, errors and RX sequence preserved");
    return 0;
}
