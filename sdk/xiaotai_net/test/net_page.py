#!/usr/bin/env python3
"""Check the offline captive portal asset against the frozen route and keys."""
from html.parser import HTMLParser
from pathlib import Path
import re
import shutil
import subprocess


class Page(HTMLParser):
    def __init__(self):
        super().__init__()
        self.ids = set()
        self.symbols = set()
        self.uses = set()
        self.script = []
        self.in_script = False

    def handle_starttag(self, tag, attrs):
        values = dict(attrs)
        name = values.get("id")
        if name:
            assert name not in self.ids, name
            self.ids.add(name)
            if tag == "symbol":
                self.symbols.add(name)
        for attr in ("src", "href", "action"):
            if values.get(attr):
                assert values[attr].startswith("#"), (tag, attr, values[attr])
        if tag == "use":
            self.uses.add(values["href"][1:])
        if tag == "script":
            self.in_script = True

    def handle_endtag(self, tag):
        if tag == "script":
            self.in_script = False

    def handle_data(self, data):
        if self.in_script:
            self.script.append(data)


def main():
    component = Path(__file__).resolve().parents[1]
    draft = (component / "web/setup.html").read_text(encoding="utf-8")
    legacy = component / "test/legacy_portal"
    original = (legacy / "setup.html").read_text(encoding="utf-8")
    symbols = re.compile(r'<svg class="symbols".*?</svg>', re.DOTALL)
    assert symbols.search(draft).group() == symbols.search(original).group()
    
    page = Page()
    page.feed(draft)
    assert page.uses <= page.symbols
    script = "".join(page.script)
    assert not set(re.findall(r"\$\('([^']+)'\)", script)) - page.ids
    routes = set(re.findall(r"request\('(/api/[^']+)'", script))
    assert routes == {"/api/wifi", "/api/wifi/scan", "/api/wifi/status", "/api/wifi/networks"}
    assert set(re.findall(r"\bnetwork\.([a-z_]+)", script)) <= {
        "ssid", "ssid_hex", "rssi", "secure", "saved", "available", "supported",
    }
    assert "network.ssid_hex" in script and "payload.use_saved = true" in script
    assert "response.status === 409 && response.body.trim() === 'scan running'" in script
    assert script.count("await sendCredentials(payload)") == 2
    assert "if (await waitForScan()) response = await sendCredentials(payload);" in script
    assert "progress.state === 'done'" in script and "progress.state === 'store_failed'" in script
    assert "progress.restart" in script and "accepted.verify" in script
    assert ".innerHTML" not in script and "textContent = network.ssid" in script
    print("net_page: ok", flush=True)
    node = shutil.which("node") or shutil.which("nodejs")
    if node:
        subprocess.run([node, Path(__file__).resolve().with_name("net_page_ui.js")], check=True)
    else:
        print("net_page_ui: skipped (Node.js not installed); static checks passed")


if __name__ == "__main__":
    main()
