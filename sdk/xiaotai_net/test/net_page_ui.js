// SPDX-License-Identifier: MIT
// Replaces legacy: tests/regression/test_native_wifi_maintenance.py
'use strict';

const assert = require('assert').strict;
const fs = require('fs');
const path = require('path');
const {TextEncoder} = require('util');
const vm = require('vm');

const html = fs.readFileSync(path.join(__dirname, '../web/setup.html'), 'utf8');
const script = /<script>([\s\S]*?)<\/script>/.exec(html)[1];

function portal(transactions) {
  const elements = new Map();
  const submissions = [];
  const polls = [];
  let clock = 0;
  let replies = [];
  const get = id => {
    if (!elements.has(id)) {
      elements.set(id, {
        hidden: ['form', 'message', 'complete'].includes(id), disabled: false, value: '',
        classList: {toggle() {}}, firstElementChild: {classList: {toggle() {}}},
        addEventListener(event, fn) { this[event] = fn; },
        replaceChildren() {}, setAttribute() {}, scrollIntoView() {}, focus() {},
      });
    }
    return elements.get(id);
  };

  vm.runInNewContext(script, {
    document: {getElementById: get},
    // File preview registers the real handlers without loading the network list.
    location: {protocol: 'file:'},
    AbortController: class {
      constructor() { this.signal = {}; }
      abort() {}
    },
    TextEncoder,
    matchMedia: () => ({matches: true}),
    Date: {now: () => clock},
    setTimeout(fn, ms) {
      return setImmediate(() => {
        clock += ms;
        fn();
      });
    },
    clearTimeout: clearImmediate,
    async fetch(route, options) {
      let body;
      if (route === '/api/wifi') {
        assert.equal(options.method, 'POST');
        const transaction = transactions[submissions.length];
        submissions.push(JSON.parse(options.body).ssid);
        assert(transaction);
        replies = transaction.slice();
        body = {accepted: true, verify: true, restart: true};
      } else {
        assert.equal(route, '/api/wifi/status');
        body = replies.shift();
        assert(body);
        const visible = get('message');
        polls.push({hidden: visible.hidden, text: visible.textContent});
      }
      return {ok: true, status: 200, text: async () => JSON.stringify(body)};
    },
  }, {filename: 'setup.html'});

  const submit = () => get('form').submit({preventDefault() {}});
  async function enter(ssid) {
    get('manual').click();
    get('ssid').value = ssid;
    get('password').value = 'synthetic-pass';
    await submit();
  }
  return {get, enter, submit, submissions, polls};
}

function checkFailure(page, message) {
  assert.equal(page.get('message').hidden, false);
  assert.equal(page.get('message').textContent, message);
  assert.equal(page.get('form').hidden, false);
  assert.equal(page.get('submit').disabled, false);
  assert.equal(page.get('complete').hidden, true);
}

async function main() {
  const verifying = {state: 'verifying'};
  const page = portal([
    [verifying, {state: 'rejected', error: 'timeout'}],
    [verifying, {state: 'storing'}, {state: 'done', restart: true}],
  ]);
  await page.enter('synthetic-ap');
  checkFailure(page, '连接网络超时，请检查信号和密码后重试。');
  await page.enter('replacement-ap');
  assert.deepEqual(page.polls[0], {hidden: false, text: '小钛正在验证网络，请稍等。'});
  assert.deepEqual(page.polls[4], {hidden: false, text: '小钛正在保存网络，请稍等。'});
  assert.equal(page.get('complete').hidden, false);
  assert.equal(page.get('form').hidden, true);
  assert.equal(page.get('picker').hidden, true);
  assert.equal(page.get('complete-next').textContent, '小钛正在重启并连接 Wi-Fi，请在设备屏幕上查看联网状态。');
  assert.equal(page.get('password').value, '');
  await page.submit();
  assert.deepEqual(page.submissions, ['synthetic-ap', 'replacement-ap']);
  console.log('net_page_ui: verifying, timeout/refill, saving, success, no duplicate POST');

  const storage = portal([[verifying, {state: 'store_failed', error: 'storage'}]]);
  await storage.enter('synthetic-ap');
  checkFailure(storage, '设备未能保存配置。请在设备上重试保存。');
  assert.deepEqual(storage.submissions, ['synthetic-ap']);
  console.log('net_page_ui: storage failure shown; device-side retry message, no duplicate POST');
}

main().catch(error => { console.error(error); process.exitCode = 1; });
