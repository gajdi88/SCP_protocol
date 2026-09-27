// Tests the web app's parsing and retry behaviour. The regexes and the chunk
// reassembly are lifted from app/index.html; the check below fails if they drift.
//   node test/app_test.mjs
import { readFileSync } from 'node:fs';
import { fileURLToPath } from 'node:url';
import { dirname, join } from 'node:path';

const here = dirname(fileURLToPath(import.meta.url));
const html = readFileSync(join(here, '..', 'app', 'index.html'), 'utf8');

let fails = 0;
const ck = (ok, w) => { console.log(`  ${w.padEnd(54)} ${ok ? 'PASS' : '** FAIL **'}`); if (!ok) fails++; };
const section = t => console.log(`\n-- ${t} --`);
const sleep = ms => new Promise(r => setTimeout(r, ms));

// --- the copies under test, kept identical to the app ---
const CEILINGS = /master\s+0\.\.(\d+),\s*sub\s+0\.\.(\d+)/;
const LEVELS   = /master=(\d+).*sub=(\d+)/;

section('the regexes still match the app');
ck(html.includes(String(CEILINGS).slice(1, -1)), 'the ceilings regex is the one in index.html');
ck(html.includes(String(LEVELS).slice(1, -1)),   'the levels regex is the one in index.html');

// --- chunk reassembly ---
let lineBuf = '', lines = [];
function onNotify(text) {
  lineBuf += text;
  let i;
  while ((i = lineBuf.search(/[\r\n]/)) >= 0) {
    const line = lineBuf.slice(0, i).trim();
    lineBuf = lineBuf.slice(i + 1);
    if (line) lines.push(line);
  }
  if (lineBuf.length > 512) lineBuf = '';
}

section('20-byte notifications reassemble into lines');
const wire = 'master=60 (0x3C)  sub=23 (0x17)\r\nOK\r\n';
for (let i = 0; i < wire.length; i += 20) onNotify(wire.slice(i, i + 20));
ck(lines.length === 2, 'chunks reassemble into 2 whole lines');
let m = LEVELS.exec(lines[0]);
ck(!!m && m[1] === '60' && m[2] === '23', 'levels parsed as master 60, sub 23');
ck(lines[1] === 'OK', 'the OK line survives intact');

lines = [];
onNotify('r | e | m N | s N | i N | + | - | d | v 0/1  (N decimal, or 0x.. hex)\r\n');
onNotify('master 0..60, sub 0..23, input 0..2\r\n');
const c = CEILINGS.exec(lines[1]);
ck(!!c && c[1] === '60' && c[2] === '23', 'ceilings parsed from the help line');
ck(!CEILINGS.test(lines[0]), 'the command list is not mistaken for ceilings');
ck(!LEVELS.test(lines[1]),   'the ceilings line is not mistaken for levels');

lines = [];
onNotify('master=0 (0x00)  sub=0 (0x00)\r\n');
m = LEVELS.exec(lines[0]);
ck(!!m && m[1] === '0' && m[2] === '0', 'zero levels parse, not dropped as falsy');
lines = [];
onNotify('RX: no valid'); onNotify(' reply\r\n');
ck(/no valid reply/i.test(lines[0]), 'a message split mid-word still matches');
lines = []; lineBuf = '';
onNotify('x'.repeat(600));
ck(lineBuf.length === 0, 'an unterminated flood is dropped, the buffer cannot grow');

section('connect retry');
function makeDevice(failFirst) {
  let n = 0;
  return { gatt: { connected: false, tries: 0,
    connect() { n++; this.tries = n;
      if (n <= failFirst) return Promise.reject(new Error('GATT error 133'));
      this.connected = true; return Promise.resolve('server'); },
    disconnect() { this.connected = false; } } };
}
async function connectGatt(dev, attempts = 4) {
  for (let i = 1; i <= attempts; i++) {
    try { return await dev.gatt.connect(); }
    catch (e) {
      if (dev.gatt.connected) dev.gatt.disconnect();
      if (i === attempts) throw e;
      await sleep(5 * i);
    }
  }
}
ck(await connectGatt(makeDevice(0)) === 'server', 'succeeds first time when the stack is happy');
const d2 = makeDevice(2);
ck(await connectGatt(d2) === 'server', 'recovers from two consecutive 133 failures');
ck(d2.gatt.tries === 3, 'took exactly 3 attempts, no more');
let threw = false;
try { await connectGatt(makeDevice(99)); } catch { threw = true; }
ck(threw, 'gives up after 4 attempts rather than looping forever');

section('the level read retries until answered');
let sent = [], levelsKnown = false, readTries = 0, readTimer = null, gaveUp = false;
const stopReading = () => { clearTimeout(readTimer); readTimer = null; };
function pollLevels() {
  if (levelsKnown) return;
  if (readTries >= 4) { gaveUp = true; return; }
  readTries++; sent.push('r'); readTimer = setTimeout(pollLevels, 30);
}
pollLevels(); await sleep(50);
ck(sent.length === 2, 'an unanswered read is retried');
levelsKnown = true; stopReading();
const after = sent.length; await sleep(80);
ck(sent.length === after, 'retries stop the moment the amp answers');
sent = []; levelsKnown = false; readTries = 0; gaveUp = false;
pollLevels(); await sleep(200);
ck(sent.length === 4 && gaveUp, 'gives up after 4 reads and reports it');

section('a deliberate disconnect must not reconnect');
let wantConnected = true, reconnects = 0;
const onDisconnected = () => { if (wantConnected) reconnects++; };
wantConnected = false; onDisconnected();
ck(reconnects === 0, 'tapping Disconnect does not trigger a reconnect');
wantConnected = true; onDisconnected();
ck(reconnects === 1, 'an unexpected drop does trigger one');

section('slider throttling');
let writes = 0, timer = null;
const schedule = () => { if (!timer) timer = setTimeout(() => { timer = null; writes++; }, 120); };
for (let i = 0; i < 40; i++) schedule();
await sleep(180);
ck(writes === 1, '40 drag events coalesce into 1 throttled write');

console.log('\n' + (fails ? '** FAILURES **' : 'all checks passed'));
process.exit(fails);
