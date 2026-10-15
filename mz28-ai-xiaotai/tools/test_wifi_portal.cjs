/* Execute the embedded page's actual JavaScript against a small DOM/API fixture. */
'use strict';
const assert = require('node:assert/strict');
const fs = require('node:fs');
const path = require('node:path');
const vm = require('node:vm');
const {test} = require('node:test');

class Element {
  constructor() {
    this.children = [];
    this.attributes = {};
    this.listeners = {};
    this.style = {};
    this.value = '';
    this.disabled = false;
    this.classList = {add() {}, remove() {}, toggle() {}};
  }
  append(...children) { this.children.push(...children); }
  replaceChildren(...children) { this.children = [...children]; }
  setAttribute(name, value) { this.attributes[name] = value; }
  getAttribute(name) { return this.attributes[name]; }
  addEventListener(name, callback) { this.listeners[name] = callback; }
  focus() {}
  scrollIntoView() {}
  get firstElementChild() { return this.children[0] || (this.children[0] = new Element()); }
}

function fixture(responses) {
  const elements = new Map();
  const element = id => {
    if (!elements.has(id)) elements.set(id, new Element());
    return elements.get(id);
  };
  const calls = [];
  const context = vm.createContext({
    document: {
      getElementById: element,
      createElement: () => new Element(),
      createElementNS: () => new Element(),
      createTextNode: value => ({textContent: value}),
    },
    TextEncoder, AbortController,
    setTimeout(fn, delay) { if (delay < 8000) queueMicrotask(fn); return 1; },
    clearTimeout() {},
    async fetch(url, options) {
      calls.push({url, options});
      assert(responses.length, 'unexpected request: ' + url);
      const {body, status = 200} = responses.shift();
      return {ok: status >= 200 && status < 300, status,
        text: async () => typeof body === 'string' ? body : JSON.stringify(body)};
    },
  });
  const html = fs.readFileSync(path.join(__dirname,
    '../components/wifi_provisioning/web/setup.html'), 'utf8');
  const script = html.match(/<script>([\s\S]*?)<\/script>/)[1];
  const startup = script.lastIndexOf('(async()=>');
  assert(startup > 0, 'expected explicit page startup');
  vm.runInContext(script.slice(0, startup), context);
  return {element, calls, context, run: code => vm.runInContext(code, context)};
}

test('scan results are rendered and refresh is re-enabled', async () => {
  const f = fixture([{body: {accepted: true}}, {body: {state: 'ready', networks: [
    {ssid: 'Ti', ssid_hex: '5469', rssi: -40, available: true, secure: true, supported: true},
    {ssid: 'Open', ssid_hex: '4f70656e', rssi: -60, available: true, secure: false, supported: true},
  ]}}]);
  await f.run('scan()');
  assert.equal(f.element('nearby').children.length, 2);
  assert.equal(f.element('refresh').disabled, false);
  assert.equal(f.element('scan-note').textContent, '扫描完成');
  assert.deepEqual(f.calls.map(c => c.url), ['/api/wifi/scan', '/api/wifi/networks']);
});

test('busy scan keeps the last network snapshot', async () => {
  const f = fixture([{body: {accepted: true}}, {body: {state: 'busy', networks: []}}]);
  f.run("state.networks=[{ssid:'Ti',ssid_hex:'5469',rssi:-40,available:true,supported:true}]");
  await f.run('scan()');
  assert.equal(f.element('nearby').children.length, 1);
  assert.equal(f.element('refresh').disabled, false);
});

test('a second scan is not sent while one is running', async () => {
  const f = fixture([]);
  f.run('state.scanning=true');
  await f.run('scan()');
  assert.equal(f.calls.length, 0);
});

test('accepted credentials are not presented as verified/saved', async () => {
  const f = fixture([{body: '正在验证网络，连接成功后设备会自动保存并重启。'}]);
  f.element('ssid').value = 'Ti';
  f.element('password').value = '12345678';
  await f.element('form').listeners.submit({preventDefault() {}});
  assert.equal(f.run('state.done'), false);
  assert.equal(f.element('save').textContent, '重新连接');
  assert.equal(f.element('fields').disabled, false);
  assert.equal(f.element('password').value, '');
});

test('in-flight verification conflict is explained accurately', async () => {
  const f = fixture([{status: 409, body: 'Wi-Fi verification already running'}]);
  f.element('ssid').value = 'Ti';
  f.element('password').value = '12345678';
  await f.element('form').listeners.submit({preventDefault() {}});
  assert.match(f.element('result').textContent, /正在验证网络/);
  assert.equal(f.element('fields').disabled, false);
});
