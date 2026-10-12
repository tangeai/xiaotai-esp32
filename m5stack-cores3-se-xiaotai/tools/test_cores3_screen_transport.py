"""Check actual snapshot record transport, backpressure and failure semantics."""
from pathlib import Path
import os
import re
import subprocess
import tempfile

root = Path(__file__).resolve().parents[1]
source = (root / "components/starter_console/src/starter_console.c").read_text()
record = re.search(r"static esp_err_t console_write_record\(.*?\n\}", source, re.S)[0]
formatter = re.search(r"static int console_printf\(.*?\n\}", source, re.S)[0]
write = re.search(r"static esp_err_t screen_write_line\(.*?\n\}", source, re.S)[0]
command = re.search(r"static int command_screen\(.*?\n\}", source, re.S)[0]
assert "vTaskDelay" not in command
assert "screen_write_line" in command and "heap_caps_free(pixels)" in command
assert "error == ESP_OK && s_console_output_error != ESP_OK" in source
assert not re.search(r"(?<![A-Za-z_])printf\(", source)
prefix = r'''
#define _POSIX_C_SOURCE 200809L
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <stdarg.h>
#include <stdlib.h>
#include <string.h>
typedef int esp_err_t;
typedef int TickType_t;
#define ESP_OK 0
#define ESP_FAIL -1
#define ESP_ERR_INVALID_SIZE -2
#define ESP_ERR_TIMEOUT -3
#define ESP_ERR_INVALID_ARG -4
#define ESP_ERR_NO_MEM -5
#define MALLOC_CAP_SPIRAM 1
#define MALLOC_CAP_8BIT 2
#define pdMS_TO_TICKS(v) (v)
static unsigned waits, writes, fail_wait_at, fail_write_at, flushes, partial_write_at;
static esp_err_t s_console_output_error;
static int locked, frees, snapshot_fail, allocation_fail, flush_fail;
static char output[400000];
static size_t output_length;
static int fake_flush(FILE *f) { assert(f==stdout && locked==1); ++flushes; return flush_fail ? -1 : 0; }
static void fake_lock(FILE *f) { assert(f==stdout && locked==0); locked=1; }
static void fake_unlock(FILE *f) { assert(f==stdout && locked==1); locked=0; }
#define fflush fake_flush
#define flockfile fake_lock
#define funlockfile fake_unlock
static esp_err_t usb_serial_jtag_wait_tx_done(TickType_t timeout) {
    assert(locked && timeout==500); ++waits;
    return waits==fail_wait_at ? ESP_ERR_TIMEOUT : ESP_OK;
}
static int usb_serial_jtag_write_bytes(const char *line, size_t size, TickType_t timeout) {
    assert(locked && timeout==500 && size>0 && size<256);
    ++writes;
    if (writes==fail_write_at) return 0;
    if (writes==partial_write_at) return (int)size-1;
    assert(output_length+size<sizeof(output));
    memcpy(output+output_length,line,size); output_length+=size;
    output[output_length]=0;
    return (int)size;
}
static void *heap_caps_malloc(size_t bytes, int caps) {
    assert(bytes==153600 && caps==3);
    return allocation_fail ? NULL : malloc(bytes);
}
static void heap_caps_free(void *p) { assert(p); ++frees; free(p); }
static esp_err_t starter_product_debug_snapshot(uint8_t *p, size_t capacity, size_t *size, unsigned *page) {
    assert(capacity==153600 && locked==0);
    if(snapshot_fail) return ESP_FAIL;
    for(size_t i=0;i<capacity;++i) p[i]=(uint8_t)i;
    *size=capacity; *page=10; return ESP_OK;
}
static uint32_t esp_rom_crc32_le(uint32_t seed, const uint8_t *p, size_t length) {
    assert(seed==0 && p && length==153600); return 0x12345678;
}
static int at_result(esp_err_t err) { assert(locked==0); return err; }
static void reset(void) {
    waits=writes=fail_wait_at=fail_write_at=flushes=partial_write_at=0;
    s_console_output_error=ESP_OK;
    locked=frees=snapshot_fail=allocation_fail=flush_fail=0;
    output_length=0; output[0]=0;
}
'''
test = r'''
int main(void) {
    char text[1025], expected[2049];
    for(unsigned size=1;size<1024;++size) {
        reset(); size_t wire=0;
        for(unsigned n=0;n<size;++n) {
            text[n]=n%71==70 ? '\n' : (char)('A'+n%26);
            if(text[n]=='\n') expected[wire++]='\r';
            expected[wire++]=text[n];
        }
        text[size]=0; expected[wire]=0;
        int rc=console_printf("%s",text);
        if(wire>=1024) {
            assert(rc==-1 && s_console_output_error==ESP_ERR_INVALID_SIZE && writes==0);
        } else {
            assert(rc==(int)size && output_length==wire && strcmp(output,expected)==0);
            assert(writes==(wire+223)/224 && waits==writes+1 && locked==0);
        }
    }
    reset(); assert(console_printf("\r\n%s\n", "中文")>0);
    assert(strcmp(output,"\r\n中文\r\n")==0);
    reset(); assert(console_printf("")==0 && writes==0);
    memset(text,'x',1024); text[1024]=0;
    assert(console_printf("%s",text)==-1 && writes==0);
    for(unsigned fail=1;fail<=4;++fail) {
        memset(text,'z',800); text[800]=0;
        reset(); fail_write_at=fail;
        assert(console_printf("%s",text)==-1 && s_console_output_error==ESP_ERR_TIMEOUT && locked==0);
        reset(); partial_write_at=fail;
        assert(console_printf("%s",text)==-1 && s_console_output_error==ESP_ERR_TIMEOUT && locked==0);
        reset(); fail_wait_at=fail;
        assert(console_printf("%s",text)==-1 && s_console_output_error==ESP_ERR_TIMEOUT && locked==0);
    }
    reset(); assert(command_screen(1,NULL)==ESP_OK && frees==1);
    assert(writes==2402 && waits==4804 && flushes==2402);
    assert(strstr(output,"+SCREEN:320,240,rgb565-swap,page=10,bytes=153600,crc=12345678\r\n")==output);
    const char *p=strchr(output,'\n')+1;
    for(unsigned offset=0;offset<153600;offset+=64) {
        unsigned actual; int n=0;
        assert(sscanf(p,"+PIX:%u:%n",&actual,&n)==1 && actual==offset);
        p+=n;
        for(unsigned k=0;k<64;++k) {
            unsigned byte; assert(sscanf(p+2*k,"%2x",&byte)==1 && byte==(uint8_t)(offset+k));
        }
        assert(p[128]=='\r' && p[129]=='\n'); p+=130;
    }
    assert(strcmp(p,"+SCREEN:END\r\n")==0);
    for(unsigned fail=1;fail<=5;++fail) {
        reset(); fail_write_at=fail;
        assert(command_screen(1,NULL)==ESP_ERR_TIMEOUT && frees==1 && writes==fail);
        assert(strstr(output,"+SCREEN:END")==NULL);
        reset(); fail_wait_at=fail;
        assert(command_screen(1,NULL)==ESP_ERR_TIMEOUT && frees==1 && waits==fail);
        assert(strstr(output,"+SCREEN:END")==NULL);
    }
    reset(); flush_fail=1;
    assert(command_screen(1,NULL)==ESP_FAIL && frees==1 && writes==0);
    reset(); snapshot_fail=1;
    assert(command_screen(1,NULL)==ESP_FAIL && frees==1 && writes==0);
    reset(); allocation_fail=1;
    assert(command_screen(1,NULL)==ESP_ERR_NO_MEM && frees==0 && writes==0);
    reset(); assert(command_screen(2,NULL)==ESP_ERR_INVALID_ARG && frees==0);
    assert(screen_write_line("")==ESP_ERR_INVALID_SIZE);
    char oversized[257]; memset(oversized,'x',256); oversized[256]=0;
    assert(screen_write_line(oversized)==ESP_ERR_INVALID_SIZE);
    return 0;
}
'''
with tempfile.TemporaryDirectory(prefix="core-screen-") as temporary:
    path = Path(temporary)
    file = path / "test.c"
    file.write_text(prefix + record + formatter + write + command + test)
    subprocess.run([os.environ.get("CC", "cc"), "-std=c11", "-Wall", "-Wextra", "-Werror",
                    "-fsanitize=address,undefined", "-fno-omit-frame-pointer", "-no-pie",
                    str(file), "-o", str(path / "test")], check=True)
    subprocess.run([str(path / "test")], check=True)
print("PASS: 1023 console sizes/CRLF/UTF8/overflow/partial-write failures; 153600-byte screen/CRC offsets and PSRAM cleanup")
