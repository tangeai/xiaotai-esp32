"""Generate the 12-pixel Noto bitmap table from an OFL CJK font."""
import argparse
import hashlib
import json
from pathlib import Path
from PIL import Image, ImageDraw, ImageFont


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--font", type=Path, required=True)
    parser.add_argument("--font-index", type=int, default=2)
    args = parser.parse_args()
    root = Path(__file__).resolve().parents[1]
    font = ImageFont.truetype(str(args.font), 12, index=args.font_index)
    codes = list(range(32, 127)) + list(range(0x4E00, 0xA000))
    lines = ["// Generated Noto Sans CJK SC 12px; SIL Open Font License 1.1.",
             "static const struct { uint16_t code, rows[12]; } s_cn_glyphs[] = {"]
    for code in codes:
        width = 8 if code < 128 else 12
        glyph = Image.new("L", (24, 24))
        box = font.getbbox(chr(code))
        ImageDraw.Draw(glyph).text((-box[0], -box[1]), chr(code), font=font, fill=255)
        glyph = glyph.crop((0, 0, max(1, box[2] - box[0]), max(1, box[3] - box[1])))
        if glyph.width > width or glyph.height > 12:
            glyph = glyph.resize((min(width, glyph.width), min(12, glyph.height)), Image.Resampling.LANCZOS)
        cell = Image.new("L", (width, 12))
        cell.paste(glyph, ((width - glyph.width) // 2, (12 - glyph.height) // 2))
        rows = [sum((1 << x) for x in range(width) if cell.getpixel((x, y)) >= 100) for y in range(12)]
        lines.append("    {0x%04x, {%s}}," % (code, ",".join("0x%03x" % row for row in rows)))
    lines.append("};")
    (root / "components/xmini_ui/xmini_cn_font.inc").write_text("\n".join(lines) + "\n", encoding="ascii")
    manifest = {"family": "Noto Sans CJK SC", "license": "OFL-1.1", "pixels": 12,
                "source_url": "https://github.com/notofonts/noto-cjk", "font_index": args.font_index,
                "source_sha256": hashlib.sha256(args.font.read_bytes()).hexdigest(), "glyphs": len(codes)}
    (root / "components/xmini_ui/font_source.json").write_text(json.dumps(manifest, indent=2) + "\n", encoding="ascii")


if __name__ == "__main__":
    main()
