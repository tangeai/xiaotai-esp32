"""Test the product callbacks, rather than a second implementation of them."""
from pathlib import Path
import re
import sys

root = Path(__file__).resolve().parents[2]
profiles = [
    ('lcd320/src/starter_product_s3_room.inc', 's3_room_key_draw'),
    ('cores3/starter_product/src/starter_product_s3_room.inc', 's3_room_key_draw'),
    ('p4/starter_product/src/starter_product_room.inc', 'room_ui_key_draw'),
    ('wt99/starter_product/src/starter_product_room.inc', 'room_ui_key_draw'),
]
output = []
for index, (path, function) in enumerate(profiles):
    source = (root / 'main/xiaotai/ui' / path).read_text()
    match = re.search(r'static void ' + function + r'\(lv_event_t \*event\)\n\{.*?\n\}', source, re.S)
    assert match and 'LV_EVENT_DRAW_TASK_ADDED' in source and 'LV_OBJ_FLAG_SEND_DRAW_TASK_EVENTS' in source
    output.append(match[0].replace(function, f'profile_{index}'))
Path(sys.argv[1]).write_text('\n'.join(output) + '\n')
