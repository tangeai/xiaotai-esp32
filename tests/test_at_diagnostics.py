"""Exercise the shared AT boundary with screen/no-screen and failing owners."""
import importlib.util
from pathlib import Path
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]


class Diagnostics(unittest.TestCase):
    def test_bulk_output_priority_restored(self):
        source = (ROOT / "main/xiaotai/input/console/src/starter_console.c").read_text(encoding="utf-8")
        start = source.index("static esp_err_t console_screen(void)")
        function = source[start:source.index("\n}\n", start) + 3]
        self.assertIn("case XIAOTAI_AT_SCREEN: return console_screen();", source)
        with tempfile.TemporaryDirectory() as folder:
            path = Path(folder)
            body = r'''
#include <assert.h>
#include <stddef.h>
typedef int esp_err_t;
typedef unsigned UBaseType_t;
#define tskIDLE_PRIORITY 0
static unsigned priority, changes, transfers;
static int result;
static unsigned uxTaskPriorityGet(void *task) { assert(!task); return priority; }
static void vTaskPrioritySet(void *task, unsigned next) { assert(!task); priority=next; ++changes; }
static int starter_diagnostics_screen(void) { assert(priority==0); ++transfers; return result; }
'''
            body += function + r'''
int main(void) {
    for(unsigned original=0; original<5; ++original) {
        for(int error=0; error<10; ++error) {
            priority=original; changes=transfers=0; result=error;
            assert(console_screen()==error);
            assert(priority==original && changes==2 && transfers==1);
        }
    }
}
'''
            (path / "priority.c").write_text(body)
            subprocess.run(["cc", "-std=c11", "-Wall", "-Wextra", "-Werror",
                            "-fsanitize=address,undefined", "-no-pie", str(path / "priority.c"),
                            "-o", str(path / "priority")], check=True)
            subprocess.run([str(path / "priority")], check=True)

    def test_ui_snapshot_ownership(self):
        source = (ROOT / "main/xiaotai/ui/lcd320/src/starter_product.c").read_text(encoding="utf-8")
        def function(signature):
            start = source.index(signature)
            brace = source.index("{", start)
            depth, end = 1, brace + 1
            while depth:
                depth += (source[end] == "{") - (source[end] == "}")
                end += 1
            return source[start:end]
        with tempfile.TemporaryDirectory() as folder:
            path = Path(folder)
            (path / "esp_err.h").write_text('''#pragma once
typedef int esp_err_t;
enum {ESP_OK, ESP_ERR_INVALID_ARG, ESP_ERR_TIMEOUT, ESP_ERR_INVALID_STATE,
ESP_ERR_NO_MEM, ESP_ERR_INVALID_SIZE, ESP_ERR_NOT_SUPPORTED, ESP_FAIL};
''')
            body = r'''
#include <assert.h>
#include <stdlib.h>
#include <string.h>
#include <stdatomic.h>
#include "xiaotai_ui_diagnostics.h"
#define LCD_H_RES 2
#define LCD_V_RES 2
#define LV_COLOR_FORMAT_RGB565 1
#define LV_DRAW_BUF_ALIGN 4
#define LV_RESULT_OK 0
#define MALLOC_CAP_SPIRAM 1
#define MALLOC_CAP_8BIT 2
#define STARTER_RUNTIME_WAITING 0
#define pdTRUE 1
#define pdMS_TO_TICKS(x) (x)
typedef struct { int state; } starter_runtime_status_t;
typedef struct { struct { unsigned w,h,stride; } header; uint8_t *data; } lv_draw_buf_t;
static uint8_t bytes[]={1,2,3,4,99,99,5,6,7,8,99,99};
static void *s_display=(void*)1;
static bool s_started=true, s_display_awake=true;
static unsigned s_page=3, s_diagnostic_ticks=99, s_capture_stack_min_free;
static int locked, lock_fail, alloc_fail, snapshot_fail, captures, state, bad_size;
static int lvgl_port_lock(int ms) { assert(ms==100 && !locked); if(lock_fail)return 0; locked=1; return 1; }
static void lvgl_port_unlock(void) { assert(locked); locked=0; }
static void *lv_display_get_screen_active(void *d) { assert(locked && d); return d; }
static unsigned lv_obj_get_child_count(void *obj) { assert(obj && locked); return 7; }
static starter_runtime_status_t starter_runtime_status(void) { return (starter_runtime_status_t){state}; }
#if CONFIG_XIAOTAI_DEVELOPMENT_CONSOLE && LV_USE_SNAPSHOT
typedef void *SemaphoreHandle_t;
static void *s_capture_queue=(void*)2, *pending;
static int semaphores, queue_fail, sem_fail, wait_mode;
static void diagnostics_capture_tick(void);
static unsigned uxTaskGetStackHighWaterMark(void *task) { assert(!task && locked); return 12000; }
static SemaphoreHandle_t xSemaphoreCreateBinary(void) { if(sem_fail)return NULL; ++semaphores; return calloc(1,sizeof(int)); }
static void vSemaphoreDelete(void *p) { --semaphores; free(p); }
static void xSemaphoreGive(void *p) { assert(locked); *(int*)p=1; }
static int xQueueSend(void *q, const void *item, int wait) {
    assert(q==s_capture_queue && !locked && wait==0);
    if(queue_fail || pending)return 0; memcpy(&pending,item,sizeof(pending)); return 1;
}
static int xQueueReceive(void *q, void *item, int wait) {
    assert(q==s_capture_queue && locked && wait==0);
    if(!pending)return 0; memcpy(item,&pending,sizeof(pending)); pending=NULL; return 1;
}
static int xSemaphoreTake(void *p, int wait) {
    assert(!locked && wait==2000);
    if(wait_mode==1)return 0; /* Timeout before worker starts. */
    if(wait_mode==3)state=1; /* Owner must recheck admission. */
    locked=1; diagnostics_capture_tick(); locked=0;
    if(wait_mode==2)return 0; /* Completion races timeout. */
    return *(int*)p;
}
static unsigned lv_draw_buf_width_to_stride(unsigned w, int cf) { assert(w==2 && cf==1); return 6; }
static void *heap_caps_aligned_alloc(size_t alignment, size_t size, int caps) { assert(alignment==4 && size==12 && caps==3 && locked); return alloc_fail?NULL:malloc(size); }
static int lv_draw_buf_init(lv_draw_buf_t *out, unsigned w, unsigned h, int cf, unsigned stride, uint8_t *data, size_t size) {
    assert(w==2 && h==2 && cf==1 && stride==6 && size==12 && locked);
    *out=(lv_draw_buf_t){{w,h,stride},data}; return LV_RESULT_OK;
}
static int lv_snapshot_take_to_draw_buf(void *obj, int cf, lv_draw_buf_t *out) {
    assert(obj && cf==1 && locked); ++captures;
    if(snapshot_fail)return 1;
    memcpy(out->data,bytes,sizeof(bytes)); if(bad_size)out->header.w=3; return LV_RESULT_OK;
}
'''
            body += function("static esp_err_t capture_frame_on_ui(") + "\n"
            declaration = source.index("typedef struct {\n    atomic_uint references;")
            body += source[declaration:source.index("} capture_request_t;", declaration) + len("} capture_request_t;")] + "\n"
            body += function("static void capture_request_release(") + "\n"
            body += function("static void diagnostics_capture_tick(void)\n{") + "\n#endif\n"
            body += function("esp_err_t xiaotai_ui_inspect(") + "\n" + function("esp_err_t xiaotai_ui_capture(")
            body += r'''
int main(void) {
    xiaotai_ui_snapshot_t s; xiaotai_ui_frame_t f;
    assert(xiaotai_ui_inspect(NULL)==ESP_ERR_INVALID_ARG);
    lock_fail=1; assert(xiaotai_ui_inspect(&s)==ESP_ERR_TIMEOUT && !locked);
    lock_fail=0; s_display=NULL; assert(xiaotai_ui_inspect(&s)==ESP_ERR_INVALID_STATE && !locked);
    s_display=(void*)1; assert(xiaotai_ui_inspect(&s)==0 && !locked);
    assert(s.ready && s.awake && s.width==2 && s.height==2 && s.page==3 && s.objects==7 && s.ticks==99);
    assert(xiaotai_ui_capture(NULL)==ESP_ERR_INVALID_ARG);
#if CONFIG_XIAOTAI_DEVELOPMENT_CONSOLE && LV_USE_SNAPSHOT
    s_capture_queue=NULL; assert(xiaotai_ui_capture(&f)==ESP_ERR_INVALID_STATE);
    s_capture_queue=(void*)2;
    state=1; assert(xiaotai_ui_capture(&f)==ESP_ERR_INVALID_STATE && !f.pixels && !locked);
    state=0; snapshot_fail=1; assert(xiaotai_ui_capture(&f)==ESP_FAIL && !locked && captures==1);
    snapshot_fail=0; alloc_fail=1; assert(xiaotai_ui_capture(&f)==ESP_ERR_NO_MEM && !locked && captures==1);
    alloc_fail=0; bad_size=1; assert(xiaotai_ui_capture(&f)==ESP_ERR_INVALID_SIZE && !locked && captures==2);
    bad_size=0; assert(xiaotai_ui_capture(&f)==0 && !locked && captures==3);
    uint8_t expected[]={1,2,3,4,5,6,7,8}; assert(f.size==8 && !memcmp(f.pixels,expected,8)); free(f.pixels);
    assert(!pending && semaphores==0 && s_capture_stack_min_free==12000);
    queue_fail=1; assert(xiaotai_ui_capture(&f)==ESP_ERR_TIMEOUT && !pending && !semaphores);
    queue_fail=0; sem_fail=1; assert(xiaotai_ui_capture(&f)==ESP_ERR_NO_MEM && !semaphores);
    sem_fail=0; wait_mode=1; assert(xiaotai_ui_capture(&f)==ESP_ERR_TIMEOUT && pending && semaphores==1);
    /* Caller stack is gone; late worker cancels and frees its own job. */
    locked=1; diagnostics_capture_tick(); locked=0; assert(!pending && !semaphores && captures==3);
    wait_mode=2; assert(xiaotai_ui_capture(&f)==ESP_ERR_TIMEOUT && !pending && !semaphores && captures==4);
    wait_mode=3; assert(xiaotai_ui_capture(&f)==ESP_ERR_INVALID_STATE && !pending && !semaphores && captures==4);
    state=0; wait_mode=0;
    for(int i=0;i<1000;i++) { assert(xiaotai_ui_capture(&f)==0); free(f.pixels); assert(!pending && !semaphores); }
#else
    assert(xiaotai_ui_capture(&f)==ESP_ERR_NOT_SUPPORTED && !f.pixels && !locked);
#endif
    return 0;
}
'''
            (path / "test.c").write_text(body)
            for enabled in (0, 1):
                subprocess.run(["gcc", "-std=c17", "-fsanitize=address,undefined", "-fno-pie", "-no-pie",
                                f"-DCONFIG_XIAOTAI_DEVELOPMENT_CONSOLE={enabled}", f"-DLV_USE_SNAPSHOT={enabled}",
                                "-I", str(path), "-I", str(ROOT / "main/xiaotai/diagnostics/include"),
                                str(path / "test.c"), "-o", str(path / "test")], check=True)
                subprocess.run([str(path / "test")], check=True)

    def test_real_at_dispatch(self):
        with tempfile.TemporaryDirectory() as folder:
            path = Path(folder)
            (path / "esp_err.h").write_text('''#pragma once
typedef int esp_err_t;
#define ESP_OK 0
#define ESP_ERR_INVALID_ARG 1
#define ESP_ERR_NOT_SUPPORTED 2
#define ESP_ERR_INVALID_STATE 3
#define ESP_ERR_TIMEOUT 4
static inline const char *esp_err_to_name(int e) {
return e==1?"ESP_ERR_INVALID_ARG":e==2?"ESP_ERR_NOT_SUPPORTED":e==3?"ESP_ERR_INVALID_STATE":"ESP_ERR_TIMEOUT";
}
''')
            (path / "test.c").write_text(r'''
#include <assert.h>
#include <string.h>
#include "xiaotai_at.h"
static int calls, lines, result, last;
static char output[200];
static void line(void *ctx, const char *s) { assert(ctx == &calls); ++lines; strcpy(output,s); }
static int execute(void *ctx, xiaotai_at_operation_t op) {
    assert(ctx == &calls); ++calls; last=op; return result;
}
int main(void) {
    xiaotai_at_backend_t b={&calls,execute,line};
    char *good[]={"AT","AT+HELP","AT+VERSION","AT+STATUS","AT+UI","AT+SCREEN"};
    for(int i=0;i<6;i++) {
        calls=lines=0; assert(xiaotai_at_execute(&b,1,&good[i])==0);
        assert(calls==(i>=2)); assert(lines==(i==1?2:1)); assert(!strcmp(output,"OK"));
    }
    char *ai[]={"AT+AI","START","extra"};
    assert(xiaotai_at_execute(&b,2,ai)==0 && last==XIAOTAI_AT_AI_START);
    ai[1]="STOP"; result=ESP_ERR_TIMEOUT;
    assert(xiaotai_at_execute(&b,2,ai)==ESP_ERR_TIMEOUT && last==XIAOTAI_AT_AI_STOP);
    assert(!strcmp(output,"ERROR: ESP_ERR_TIMEOUT"));
    calls=lines=0;
    assert(xiaotai_at_execute(&b,3,ai)==ESP_ERR_INVALID_ARG && calls==0 && lines==1);
    ai[1]="start"; assert(xiaotai_at_execute(&b,2,ai)==ESP_ERR_INVALID_ARG);
    assert(xiaotai_at_execute(&b,1,ai)==ESP_ERR_INVALID_ARG);
    ai[0]="AT+UI"; assert(xiaotai_at_execute(&b,2,ai)==ESP_ERR_INVALID_ARG);
    ai[0]="AT+EP?"; assert(xiaotai_at_execute(&b,1,ai)==ESP_ERR_NOT_SUPPORTED);
    assert(xiaotai_at_execute(&b,0,0)==ESP_ERR_INVALID_ARG);
    ai[0]="AT+SCREEN"; result=ESP_ERR_NOT_SUPPORTED;
    assert(xiaotai_at_execute(&b,1,ai)==ESP_ERR_NOT_SUPPORTED);
    assert(!strcmp(output,"ERROR: ESP_ERR_NOT_SUPPORTED"));
    result=ESP_ERR_INVALID_STATE;
    assert(xiaotai_at_execute(&b,1,ai)==ESP_ERR_INVALID_STATE);
    b.execute=0; assert(xiaotai_at_execute(&b,1,ai)==ESP_ERR_NOT_SUPPORTED);
    b.line=0; assert(xiaotai_at_execute(&b,1,ai)==ESP_ERR_INVALID_ARG);
    return 0;
}
''')
            subprocess.run(["gcc", "-std=c17", "-Wall", "-Wextra", "-Werror", "-fsanitize=address,undefined", "-fno-pie", "-no-pie",
                            "-I", str(path), "-I", str(ROOT / "main/xiaotai/input/at/include"),
                            str(path / "test.c"), str(ROOT / "main/xiaotai/input/at/src/xiaotai_at.c"),
                            "-o", str(path / "test")], check=True)
            subprocess.run([str(path / "test")], check=True)

    def test_screen_transport_integrity(self):
        spec = importlib.util.spec_from_file_location("decode_screen", ROOT / "scripts/decode_screen.py")
        decoder = importlib.util.module_from_spec(spec)
        spec.loader.exec_module(decoder)
        pixels = bytes.fromhex("00f8e0071f00")
        digest = 2166136261
        for byte in pixels:
            digest = ((digest ^ byte) * 16777619) & 0xffffffff
        log = (f"+SCREEN: format=RGB565LE width=3 height=1 bytes=6 fnv={digest:08x}\r\n"
               f"unrelated log\n+SCREEN-DATA: seq=0 hex={pixels.hex()}\r\n"
               f"+SCREEN-END: fnv={digest:08x}\r\n").encode()
        w, h, rgb = decoder.decode(log)
        self.assertEqual((w,h,rgb), (3,1,bytes([255,0,0,0,255,0,0,0,255])))
        self.assertTrue(decoder.png(w,h,rgb).startswith(b"\x89PNG"))
        for bad in (log.replace(b"seq=0", b"seq=1"), log.replace(b"00f8", b"00f9"),
                    log.replace(b"bytes=6", b"bytes=8"), log.replace(b"width=3", b"width=99999"),
                    log.replace(b"+SCREEN-END", b"incomplete"), log.replace(b"e007", b"")):
            with self.assertRaises(ValueError): decoder.decode(bad)


if __name__ == "__main__":
    unittest.main()
