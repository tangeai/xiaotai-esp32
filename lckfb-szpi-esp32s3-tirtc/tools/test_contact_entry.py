#!/usr/bin/env python3
"""Both contact entry actions select, never dial; actual action branch replay."""
from pathlib import Path
import subprocess
import tempfile
source=(Path(__file__).resolve().parents[1]/"components/starter_product/src/starter_product.c").read_text()
start=source.index("action >= ACTION_CONTACT_CALL_BASE &&")
end=source.index("} else if (action >= ACTION_EMOJI_BASE",start)
branch="if ("+source[start:end]+"}"
code=r'''
#include <assert.h>
#include <stdint.h>
#define ACTION_CONTACT_BASE 100
#define ACTION_CONTACT_CALL_BASE 120
#define STARTER_PRODUCT_CONTACTS_MAX 10
#define PAGE_CONTACT_DETAIL 4
static uint8_t s_selected_contact;
static int page, dials;
static int enter_page(int p) {page=p; return 1;}
int starter_runtime_call_contact(int i) {(void)i; ++dials; return 0;}
static void action_test(int action) {
'''+branch+r'''
}
int main(void) {
    for(int i=0;i<10;i++) {
        page=0; action_test(100+i); assert(page==4 && s_selected_contact==i && !dials);
        page=0; action_test(120+i); assert(page==4 && s_selected_contact==i && !dials);
    }
}
'''
with tempfile.TemporaryDirectory(prefix="contact-entry-") as tmp:
    p=Path(tmp); (p/"test.c").write_text(code)
    subprocess.run(["cc","-Wall","-Wextra","-Werror",str(p/"test.c"),"-o",str(p/"test")],check=True)
    subprocess.run([str(p/"test")],check=True)
print("PASS: contact row and right button both open selection without dialing")
