// Host-only DOM/root-bridge mocks. No device, browser package or root access.
'use strict';
const { test } = require('node:test');
const assert = require('node:assert/strict');
const fs = require('node:fs');
const path = require('node:path');
const vm = require('node:vm');
const { spawnSync } = require('node:child_process');
const root = path.resolve(__dirname, '..');
const appSource = fs.readFileSync(path.join(root, 'integration/app.js'), 'utf8');
const bridgeSource = fs.readFileSync(path.join(root, 'integration/kernelsu.js'), 'utf8');
const marker = '  /* ---------- init ---------- */';
assert(appSource.includes(marker));
const instrumented = appSource.replace(marker,
  '  window.__test = {setKey, debouncedSet, runAction, shellQuote, flushPending, ctl, wire, loadAll, refreshStatus};\n' + marker);
const tick = () => new Promise(resolve => setImmediate(resolve));
const ok = (stdout = '') => ({ errno: 0, stdout, stderr: '' });
const fail = (stderr = 'fixture failure') => ({ errno: 1, stdout: '', stderr });
function deferred() { let resolve; const promise = new Promise(r => { resolve = r; }); return { promise, resolve }; }
function timers() {
  let serial = 0;
  const pending = new Map();
  return {
    pending,
    setTimeout(fn, ms) { const id = ++serial; pending.set(id, { fn, ms }); return id; },
    clearTimeout(id) { pending.delete(id); },
    fire(ms) { for (const [id, timer] of [...pending]) if (timer.ms === ms) { pending.delete(id); timer.fn(); } }
  };
}
function argv(command) {
  const marker = '"$C" ';
  const args = command.slice(command.lastIndexOf(marker) + marker.length);
  const result = spawnSync('/bin/sh', ['-c', 'set -- ' + args + '; printf \'%s\\0\' "$@"'], { encoding: 'utf8' });
  assert.equal(result.status, 0, result.stderr);
  return result.stdout.split('\0').slice(0, -1);
}
function app(handler) {
  const nodes = new Map(), commands = [], notices = [], prompts = [], clock = timers();
  let active = 0, maxActive = 0, confirmed = true;
  function node(id) {
    if (!nodes.has(id)) {
      const classes = new Set();
      nodes.set(id, {
        id, textContent: '', innerHTML: '', value: '', checked: false, disabled: false, dataset: {}, listeners: {},
        classList: {
          add(...names) { names.forEach(n => classes.add(n)); },
          remove(...names) { names.forEach(n => classes.delete(n)); },
          contains(name) { return classes.has(name); },
          toggle(name, force) { const add = force === undefined ? !classes.has(name) : force; if (add) classes.add(name); else classes.delete(name); return add; }
        },
        addEventListener(name, fn) { this.listeners[name] = fn; },
        dispatch(name, event = {}) { return this.listeners[name]?.call(this, event); },
        querySelector() { return { textContent: 'fixture mode' }; }
      });
    }
    return nodes.get(id);
  }
  const ids = ['enable-toggle', 'in_enabled', 'out_enabled', 'strength', 'out_strength', 'atten_db', 'vad', 'highpass', 'autogain',
    'btn-apply', 'btn-restart', 'btn-reset', 'btn-log', 'btn-refresh', 'log-close', 'log-modal', 'log-content', 'status-grid', 'toast'];
  ids.forEach(node);
  const inputs = ids.slice(0, 9).map(node);
  const modes = [1, 2, 3].map(n => { const el = node('mode-' + n); el.dataset.mode = String(n); return el; });
  const inChips = ['mic', 'voice_comm', 'camcorder', 'voice_rec'].map(target => { const el = node('in-' + target); el.dataset.target = target; return el; });
  const outChips = ['music', 'voice_call', 'ring', 'alarm', 'notification', 'system'].map(target => { const el = node('out-' + target); el.dataset.otarget = target; return el; });
  const selectors = {
    '.mode-card': modes, '.mode-status': [], '#chips-in .chip': inChips, '#chips-out .chip': outChips,
    'input, .chip, .mode-card, #btn-apply, #btn-restart, #btn-reset': [...inputs, ...inChips, ...outChips, ...modes, ...['btn-apply', 'btn-restart', 'btn-reset'].map(node)]
  };
  const status = { enabled: true, in_enabled: true, out_enabled: true, mode: 1, strength: 70, out_strength: 60,
    atten_db: 24, vad: 50, targets: 'mic,voice_comm', out_targets: '', highpass: true, autogain: false };
  const document = {
    getElementById: node, addEventListener() {},
    querySelectorAll(selector) {
      if (selector.endsWith('.chip.active')) return (selector.startsWith('#chips-in') ? inChips : outChips).filter(el => el.classList.contains('active'));
      assert(selector in selectors, 'unexpected selector: ' + selector);
      return selectors[selector];
    }
  };
  const window = { ClarifaeKSU: {
    isMock: false, toast: message => notices.push(message),
    async exec(command, options) {
      const call = { command, args: argv(command), options };
      commands.push(call); active++; maxActive = Math.max(maxActive, active);
      try { return await (handler ? handler(call) : (call.args[0] === 'status-json' ? ok(JSON.stringify(status)) : ok())); }
      finally { active--; }
    }
  } };
  const context = { window, document, console, setTimeout: clock.setTimeout, clearTimeout: clock.clearTimeout,
    setInterval() {}, confirm(message) { prompts.push(message); return confirmed; } };
  vm.runInNewContext(instrumented, context, { filename: 'app.js' });
  return { api: window.__test, node, commands, notices, prompts, clock, context, get maxActive() { return maxActive; },
    confirm(value) { confirmed = value; } };
}
function mutations(ui) { return ui.commands.filter(c => c.args[0] !== 'status-json'); }
function actionCalls(ui, action) { return ui.commands.filter(c => c.args[0] === action); }

test('shell quoting preserves empty values', () => { const ui = app(); assert.equal(ui.api.shellQuote(''), "''"); });
test('shell quoting roundtrips arbitrary POSIX argument text', () => {
  const ui = app();
  for (const value of ["a'b", '"quoted"', '$HOME $(printf injected)', String.fromCharCode(96) + 'printf injected' + String.fromCharCode(96),
    'a;b|c&d', '*?[x]', '-n', 'a\\b=c', 'line\nbreak\t', 'é 音']) {
    assert.deepEqual(argv('"$C" ' + ui.api.shellQuote(value)), [value]);
  }
});
test('both setting key and value are quoted as one argument', async () => {
  const ui = app(); const key = 'bad; printf injected', value = "a'b $HOME";
  await ui.api.setKey(key, value);
  assert.deepEqual(ui.commands[0].args, ['set', key, value]); assert.equal(ui.commands[0].options.timeout, 65);
});
test('explicit empty playback targets reach the CLI', async () => {
  const ui = app(); await ui.api.setKey('out_targets', ''); assert.deepEqual(ui.commands[0].args, ['set', 'out_targets', '']);
});
test('saves serialize while the first bridge call is unresolved', async () => {
  const hold = deferred(); const ui = app(call => call.args[1] === 'strength' ? hold.promise : ok());
  const first = ui.api.setKey('strength', 71), second = ui.api.setKey('out_strength', 72);
  await tick(); assert.equal(ui.commands.length, 1); hold.resolve(ok()); await Promise.all([first, second]);
  assert.equal(ui.maxActive, 1); assert.deepEqual(mutations(ui).map(c => c.args[1]), ['strength', 'out_strength']);
});
test('debounce saves only the most recent value', async () => {
  const ui = app(); ui.api.debouncedSet('strength', 71); ui.api.debouncedSet('strength', 79);
  await tick(); assert.equal(ui.commands.length, 0); ui.clock.fire(280); await tick();
  assert.deepEqual(mutations(ui).map(c => c.args), [['set', 'strength', '79']]);
});
test('Apply flushes and awaits debounce before issuing apply with no duplicate', async () => {
  const ui = app(); ui.api.debouncedSet('out_strength', 79);
  assert.equal(await ui.api.runAction('apply'), true); await tick();
  assert.deepEqual(mutations(ui).map(c => c.args), [['set', 'out_strength', '79'], ['apply']]);
  ui.clock.fire(280); await tick(); assert.equal(mutations(ui).length, 2);
  assert(ui.notices.includes('Routing transaction committed (PCM not verified)'));
});
test('Apply awaits prior immediate writes and all pending keys', async () => {
  const hold = deferred(); const ui = app(call => call.args[1] === 'targets' ? hold.promise : ok());
  const saved = ui.api.setKey('targets', 'mic'); ui.api.debouncedSet('strength', 75); ui.api.debouncedSet('out_strength', 76);
  const applied = ui.api.runAction('apply'); await tick();
  assert.equal(actionCalls(ui, 'apply').length, 0); assert.equal(ui.node('btn-apply').disabled, true);
  hold.resolve(ok()); await saved; assert.equal(await applied, true);
  assert.deepEqual(mutations(ui).map(c => c.args[0] === 'set' ? c.args[1] : c.args[0]), ['targets', 'strength', 'out_strength', 'apply']);
  assert.equal(ui.maxActive, 1);
});
test('failed immediate save blocks Apply even after the save resolved', async () => {
  const ui = app(call => call.args[0] === 'set' ? fail('property write failed') : ok()); await ui.api.setKey('out_targets', '');
  assert.equal(await ui.api.runAction('apply'), false); assert.equal(actionCalls(ui, 'apply').length, 0);
  assert(ui.notices.some(n => n.includes('failed saves (out_targets)'))); assert(!ui.notices.includes('Routing transaction committed (PCM not verified)'));
});
test('failed flushed debounce blocks Apply and restores controls', async () => {
  const ui = app(() => fail('config write failed')); ui.api.debouncedSet('strength', 99);
  assert.equal(await ui.api.runAction('apply'), false); assert.equal(actionCalls(ui, 'apply').length, 0); assert.equal(ui.node('btn-apply').disabled, false);
});
test('pending save failure blocks an already clicked Apply', async () => {
  const hold = deferred(); const ui = app(() => hold.promise); ui.api.setKey('mode', 2);
  const applied = ui.api.runAction('apply'); await tick(); hold.resolve(fail('late failure'));
  assert.equal(await applied, false); assert.equal(actionCalls(ui, 'apply').length, 0);
});
test('unrelated successful save cannot clear an earlier failed key', async () => {
  const ui = app(call => call.args[1] === 'mode' ? fail() : ok()); await ui.api.setKey('mode', 2); await ui.api.setKey('strength', 71);
  assert.equal(await ui.api.runAction('apply'), false); assert.equal(actionCalls(ui, 'apply').length, 0);
});
test('successful explicit retry clears the failed key and permits Apply', async () => {
  let failure = true; const ui = app(() => failure ? fail() : ok()); await ui.api.setKey('mode', 2); failure = false; await ui.api.setKey('mode', 2);
  assert.equal(await ui.api.runAction('apply'), true); assert.equal(actionCalls(ui, 'apply').length, 1);
});
test('rejected bridge save is visible and blocks Apply', async () => {
  const ui = app(() => Promise.reject(new Error('bridge unavailable'))); assert.equal((await ui.api.setKey('enabled', 0)).errno, -1);
  assert.equal(await ui.api.runAction('apply'), false); assert(ui.notices.some(n => n.includes('bridge unavailable')));
});
test('timed-out save blocks already queued mutations and a same-key retry', async () => {
  const hold = deferred(); const ui = app(call => call.args[0] === 'set' ? hold.promise : ok('{}'));
  const first = ui.api.setKey('strength', 71), queued = ui.api.setKey('strength', 79);
  await tick(); assert.equal(mutations(ui).length, 1);
  hold.resolve({ errno: -2, stdout: '', stderr: 'completion unknown', completionUnknown: true });
  assert.equal((await first).errno, -2); assert.equal((await queued).errno, -3);
  assert.equal((await ui.api.setKey('strength', 80)).errno, -3);
  assert.equal(mutations(ui).length, 1); assert.equal(await ui.api.runAction('apply'), false);
  assert.equal(actionCalls(ui, 'apply').length, 0);
  assert.match(ui.node('command-warning').textContent, /Verify host command completion/);
  assert.equal(ui.node('strength').disabled, true); assert.equal(ui.node('btn-apply').disabled, true);
});
test('ambiguous Apply blocks later Apply Reset and Restart without host calls', async () => {
  const ui = app(() => ({ errno: -2, stdout: '', stderr: 'timeout', completionUnknown: true }));
  assert.equal(await ui.api.runAction('apply'), false);
  assert.equal(await ui.api.runAction('apply'), false); assert.equal(await ui.api.runAction('reset'), false);
  assert.equal(await ui.api.runAction('restart-audio'), false); assert.equal(mutations(ui).length, 1);
  assert.equal(ui.node('btn-reset').disabled, true); assert.equal(ui.node('btn-restart').disabled, true);
  assert(!ui.notices.includes('Routing transaction committed (PCM not verified)'));
});
test('ambiguous host exception quarantines writes even without timeout errno', async () => {
  const ui = app(() => Promise.reject(new Error('ambiguous dispatch exception')));
  assert.equal((await ui.api.setKey('mode', 2)).errno, -1);
  assert.equal((await ui.api.setKey('mode', 1)).errno, -3); assert.equal(mutations(ui).length, 1);
});
test('known nonzero save remains retryable instead of quarantining writes', async () => {
  let failed = true; const ui = app(() => failed ? fail('known exit 1') : ok());
  await ui.api.setKey('mode', 2); failed = false; await ui.api.setKey('mode', 2);
  assert.equal(await ui.api.runAction('apply'), true); assert.equal(ui.node('btn-apply').disabled, false);
});
test('read-only status still works but cannot unlock an uncertain mutation', async () => {
  const ui = app(call => call.args[0] === 'set' ? { errno: -2, stdout: '', stderr: 'timeout', completionUnknown: true } : ok('{}'));
  await ui.api.setKey('strength', 71); await ui.api.refreshStatus(true);
  assert.equal(actionCalls(ui, 'status-json').length, 1); assert.equal((await ui.api.setKey('strength', 79)).errno, -3);
  assert.equal(mutations(ui).length, 1); assert.equal(ui.node('strength').disabled, true);
});

test('nonzero Apply return overrides successful-looking stdout', async () => {
  const ui = app(() => ({ errno: 3, stdout: 'applied', stderr: 'sync failed' })); assert.equal(await ui.api.runAction('apply'), false);
  assert(ui.notices.includes('Not applied: sync failed')); assert(!ui.notices.includes('Routing transaction committed (PCM not verified)'));
});
test('action error uses stdout when stderr is empty', async () => {
  const ui = app(() => ({ errno: 4, stdout: 'partial failure', stderr: '' })); assert.equal(await ui.api.runAction('apply'), false);
  assert(ui.notices.includes('Not applied: partial failure'));
});
test('restart nonzero return never reports restarted or request success', async () => {
  const ui = app(() => fail('init denied restart')); assert.equal(await ui.api.runAction('restart-audio'), false);
  assert(ui.notices.includes('Restart request failed: init denied restart')); assert(!ui.notices.some(n => n.startsWith('Audio restart requested')));
  assert.equal(ui.node('btn-restart').disabled, false);
});
test('restart success says requested, not verified, with unchanged routes', async () => {
  const ui = app(); assert.equal(await ui.api.runAction('restart-audio'), true);
  assert(ui.notices.includes('Routing restart committed (PCM not verified)'));
  assert(!ui.notices.some(n => /Audio restarted|applies them live|calls.*protected/i.test(n)));
});
test('failed saves also block Restart', async () => {
  const ui = app(() => fail()); await ui.api.setKey('enabled', 1);
  assert.equal(await ui.api.runAction('restart-audio'), false); assert.equal(actionCalls(ui, 'restart-audio').length, 0);
});
test('duplicate action clicks do not enqueue another mutation', async () => {
  const hold = deferred(); const ui = app(() => hold.promise); const first = ui.api.runAction('apply'); await tick();
  assert.equal(await ui.api.runAction('apply'), false); assert.equal(await ui.api.runAction('restart-audio'), false);
  hold.resolve(ok()); assert.equal(await first, true); await tick(); assert.equal(mutations(ui).length, 1);
});
test('busy controls restore prior disabled state on failure', async () => {
  const ui = app(() => fail()); ui.node('mode-2').disabled = true; const done = ui.api.runAction('apply'); assert.equal(ui.node('strength').disabled, true);
  await done; assert.equal(ui.node('strength').disabled, false); assert.equal(ui.node('mode-2').disabled, true); assert.equal(ui.node('btn-reset').disabled, false);
});
test('reset failure is checked and never falsely reports defaults restored', async () => {
  const ui = app(() => fail('reset denied')); assert.equal(await ui.api.runAction('reset'), false);
  assert(ui.notices.includes('Not applied: reset denied')); assert.equal(actionCalls(ui, 'status-json').length, 0);
});
test('successful reset recovers a failed save and reloads controls', async () => {
  let rejectSave = true; const ui = app(call => call.args[0] === 'set' && rejectSave ? fail() :
    call.args[0] === 'status-json' ? ok(JSON.stringify({ mode: 1, strength: 70, out_strength: 60, targets: 'mic', out_targets: '' })) : ok());
  await ui.api.setKey('mode', 2); assert.equal(await ui.api.runAction('reset'), true); rejectSave = false;
  assert.equal(await ui.api.runAction('apply'), true); assert.equal(ui.node('strength').value, 70);
});
test('initial status rejects nonzero return even with valid stdout JSON', async () => {
  const ui = app(() => ({ errno: 1, stdout: '{"strength":99}', stderr: 'status denied' })); await ui.api.loadAll();
  assert.equal(ui.node('strength').value, ''); assert(ui.notices.includes('Could not read status: status denied'));
});
test('initial invalid JSON reports read failure', async () => {
  const ui = app(() => ok('not json')); await ui.api.loadAll(); assert(ui.notices.includes('Could not read status: invalid JSON'));
});
test('periodic status failures are quiet but manual refresh reports them', async () => {
  const ui = app(() => fail('status failure')); await ui.api.refreshStatus(); assert.equal(ui.notices.length, 0);
  await ui.api.refreshStatus(true); assert(ui.notices.includes('Could not read status: status failure'));
});
test('automatic status polling pauses while debounce saves or actions are pending', async () => {
  const hold = deferred(); const ui = app(() => hold.promise);
  ui.api.debouncedSet('strength', 79); await ui.api.refreshStatus(); assert.equal(ui.commands.length, 0);
  const applying = ui.api.runAction('apply'); await tick();
  await ui.api.refreshStatus(); await ui.api.refreshStatus(); assert.equal(actionCalls(ui, 'status-json').length, 0);
  hold.resolve(ok()); assert.equal(await applying, true); await tick();
});

test('slow status polls coalesce rather than starving queued saves', async () => {
  const hold = deferred(); const ui = app(call => call.args[0] === 'status-json' ? hold.promise : ok());
  const first = ui.api.refreshStatus(), second = ui.api.refreshStatus(), third = ui.api.refreshStatus();
  const save = ui.api.setKey('strength', 78); await tick();
  assert.equal(actionCalls(ui, 'status-json').length, 1);
  hold.resolve(ok('{}')); await Promise.all([first, second, third, save]);
  assert.equal(actionCalls(ui, 'status-json').length, 1); assert.deepEqual(mutations(ui)[0].args, ['set', 'strength', '78']);
});

test('Restart cancellation prevents commands and warning retains interruption risk', async () => {
  const ui = app(); ui.api.wire(); ui.confirm(false); ui.node('btn-restart').dispatch('click'); await tick(); assert.equal(ui.commands.length, 0);
  assert.match(ui.prompts[0], /Interrupts playback/); assert.match(ui.prompts[0], /do not use during calls or recordings/);
});
test('Restart fails closed if confirmation is unavailable', async () => {
  const ui = app(); ui.api.wire(); ui.context.confirm = undefined; ui.node('btn-restart').dispatch('click'); await tick(); assert.equal(ui.commands.length, 0);
});
test('target click saves empty output without claiming live route support', async () => {
  const ui = app(); ui.api.wire(); ui.node('out-music').classList.add('active'); ui.node('out-music').dispatch('click'); await tick();
  assert.deepEqual(mutations(ui)[0].args, ['set', 'out_targets', '']); assert(ui.notices.some(n => n.includes('press Apply')));
  assert(!ui.notices.some(n => /adapter|applied.*live/i.test(n)));
});
test('log action surfaces nonzero error instead of successful empty log', async () => {
  const ui = app(() => fail('log denied')); ui.api.wire(); ui.node('btn-log').dispatch('click'); await tick();
  assert(ui.notices.includes('Could not read log: log denied')); assert.match(ui.node('log-content').textContent, /Log command failed/);
});

function bridge(host = {}) {
  const clock = timers(); const calls = []; const window = {};
  if (host !== null) window.ksu = {
    exec(command, options, callback) { calls.push({ command, options: JSON.parse(options), callback }); if (host.exec) host.exec(window, callback); }, toast() {}
  };
  vm.runInNewContext(bridgeSource, { window, console, setTimeout: clock.setTimeout, clearTimeout: clock.clearTimeout }, { filename: 'kernelsu.js' });
  return { window, api: window.ClarifaeKSU, clock, calls };
}
test('bridge callback normalizes errno and deletes timer/callback', async () => {
  const b = bridge(); const promise = b.api.exec('fixture', { timeout: 65 }); const call = b.calls[0]; b.window[call.callback]('0', 'ok', '');
  const result = await promise; assert.equal(result.errno, 0); assert.equal(result.stdout, 'ok'); assert.equal(b.clock.pending.size, 0);
  assert.equal(b.window[call.callback], undefined); assert.equal(call.options.timeout, 65);
});
for (const errno of ['not-a-code', '', ' ', '0x0', null, undefined, false, 0.5, Infinity]) {
  test('bridge invalid errno fails closed: ' + String(errno), async () => {
    const b = bridge(); const promise = b.api.exec('fixture'); b.window[b.calls[0].callback](errno, '', ''); const result = await promise; assert.equal(result.errno, -1); assert.equal(result.completionUnknown, true);
  });
}
for (const [requested, expected] of [[undefined, 8], [65, 65], [999, 70], [1, 8], ['bad', 8], [-1, 8]]) {
  test('bridge timeout is finite and bounded: ' + String(requested), async () => {
    const b = bridge(); const promise = b.api.exec('fixture', { timeout: requested }); assert.equal(b.calls[0].options.timeout, expected);
    assert.equal([...b.clock.pending.values()][0].ms, expected * 1000); b.clock.fire(expected * 1000); const result = await promise;
    assert.equal(result.errno, -2); assert.equal(result.completionUnknown, true); assert.match(result.stderr, /completion unknown/); assert.equal(b.window[b.calls[0].callback], undefined); assert.equal(b.clock.pending.size, 0);
  });
}
test('bridge late callback after timeout is harmless', async () => {
  const b = bridge(); const promise = b.api.exec('fixture'); const callback = b.window[b.calls[0].callback]; b.clock.fire(8000);
  const result = await promise; callback(0, 'late success', ''); assert.equal(result.errno, -2); assert.equal(b.window[b.calls[0].callback], undefined);
});
test('bridge host exception cleans callback and timer', async () => {
  const b = bridge({ exec() { throw new Error('fixture throw'); } }); const result = await b.api.exec('fixture'); assert.equal(result.errno, -1);
  assert.match(result.stderr, /fixture throw/); assert.equal(result.completionUnknown, true); assert.equal(b.clock.pending.size, 0); assert.equal(b.window[b.calls[0].callback], undefined);
});
test('bridge synchronous callback also cleans timer', async () => {
  const b = bridge({ exec(window, callback) { window[callback](0, 'sync', ''); } });
  assert.equal((await b.api.exec('fixture')).stdout, 'sync'); assert.equal(b.clock.pending.size, 0);
});
test('desktop preview contains complete per-side default settings', async () => {
  const b = bridge(null); const result = await b.api.exec('status-json'); const s = JSON.parse(result.stdout); assert.equal(b.api.isMock, true);
  assert.equal(s.in_enabled, true); assert.equal(s.out_enabled, true); assert.equal(s.out_strength, 60); assert.equal(s.out_targets, 'music,voice_call'); assert.equal(b.clock.pending.size, 0);
});
