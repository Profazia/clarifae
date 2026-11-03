/* Clarifae WebUI logic — talks to clarifae-ctl through the KernelSU bridge. */
(function () {
  'use strict';

  var KSU = window.ClarifaeKSU;
  var $ = function (id) { return document.getElementById(id); };
  var setTimers = Object.create(null);
  var pendingSets = Object.create(null);
  var saveFailures = Object.create(null);
  var commandQueue = Promise.resolve();
  var saveQueue = Promise.resolve();
  var pendingSaveCount = 0;
  var actionRunning = false;
  var mutationUncertain = false;
  var statusRequest = null;
  var busyControls = [];
  var refreshTimer = null;

  /*
   * Resolve clarifae-ctl by absolute path rather than trusting the WebUI shell's
   * PATH. The systemless /system/bin overlay isn't reliably on PATH in every
   * host's exec namespace (and isn't mounted at all until a reboot), so prefer
   * the always-present copies in the data dir / module dir, then fall back to
   * PATH. Avoids "clarifae-ctl: inaccessible or not found".
   */
  var CTL_RESOLVE =
    'C=/data/adb/clarifae/clarifae-ctl; ' +
    '[ -x "$C" ] || C=/data/adb/modules/clarifae/system/bin/clarifae-ctl; ' +
    '[ -x "$C" ] || C=clarifae-ctl; ';
  function shellQuote(value) { return "'" + String(value).replace(/'/g, "'\\''") + "'"; }
  function ctl(args) {
    var mutation = ['set', 'set-many', 'apply', 'reset', 'restart-audio'].indexOf(args[0]) !== -1;
    var next = commandQueue.then(function () {
      if (mutation && mutationUncertain) {
        return { errno: -3, stdout: '', stderr: 'Changes blocked: an earlier command may still be running. Verify its completion and saved settings before reopening the WebUI.' };
      }
      return KSU.exec(CTL_RESOLVE + '"$C" ' + args.map(shellQuote).join(' '), { timeout: 65 });
    }).catch(function (e) {
      return { errno: -1, stdout: '', stderr: String(e && e.message || e), completionUnknown: true };
    }).then(function (r) {
      if (mutation && (r.completionUnknown || r.errno < 0)) lockMutations();
      return r;
    });
    commandQueue = next.then(function () {});
    return next;
  }

  // A timed-out/ambiguous mutation can still finish later in the host shell.
  // Quarantine subsequent writes, including already queued ones; a readback alone
  // cannot prove that old shell has exited. Recovery needs operator reconciliation.
  function lockMutations() {
    mutationUncertain = true;
    var warning = $('command-warning');
    warning.textContent = 'Command completion unknown. Further changes are blocked: an earlier command may still run. Verify host command completion and saved settings before reopening the WebUI; reopening does not cancel it.';
    warning.classList.remove('hidden');
    document.querySelectorAll('input, .chip, .mode-card, #btn-apply, #btn-restart, #btn-reset').forEach(function (el) { el.disabled = true; });
  }
  function errorText(r) { return r.stderr || r.stdout || ('exit ' + r.errno); }

  function flushPending() {
    Object.keys(pendingSets).forEach(function (key) {
      var pending = pendingSets[key];
      setKey(key, pending.value, pending.label);
    });
    return saveQueue;
  }

  function setBusy(busy) {
    if (busy) {
      busyControls = [];
      document.querySelectorAll('input, .chip, .mode-card, #btn-apply, #btn-restart, #btn-reset').forEach(function (el) {
        busyControls.push({ el: el, disabled: el.disabled });
        el.disabled = true;
      });
    } else {
      busyControls.forEach(function (item) { item.el.disabled = mutationUncertain || item.disabled; });
      busyControls = [];
    }
  }

  function runAction(action) {
    if (actionRunning) return Promise.resolve(false);
    actionRunning = true;
    setBusy(true);
    toast(action === 'apply' ? 'Syncing settings…' : action === 'reset' ? 'Resetting settings…' : 'Requesting audio restart…');
    return flushPending().then(function () {
      var failed = Object.keys(saveFailures);
      if (action !== 'reset' && failed.length) {
        toast('Not applied: failed saves (' + failed.join(', ') + '). Retry those settings first.');
        return false;
      }
      return ctl([action]).then(function (r) {
        if (r.errno !== 0) {
          toast((action === 'restart-audio' ? 'Restart request failed: ' : 'Not applied: ') + errorText(r));
          return false;
        }
        if (action === 'reset') {
          saveFailures = Object.create(null);
          toast('Defaults saved and settings synced; installed routes unchanged');
          return loadAll().then(function () { return true; });
        }
        toast(action === 'apply'
          ? 'Settings synced; installed routes unchanged'
          : 'Audio restart requested (not verified); installed routes unchanged');
        return true;
      });
    }).catch(function (e) {
      toast('Action failed: ' + String(e && e.message || e));
      return false;
    }).then(function (ok) {
      actionRunning = false;
      setBusy(false);
      if (ok && action !== 'reset') refreshStatus();
      return ok;
    });
  }

  function toast(msg) { KSU.toast(msg); showToast(msg); }
  function showToast(msg) {
    var t = $('toast');
    t.textContent = msg;
    t.classList.remove('hidden');
    clearTimeout(t._t);
    t._t = setTimeout(function () { t.classList.add('hidden'); }, 2200);
  }

  function parse(stdout) {
    try { return JSON.parse(stdout); } catch (e) { return null; }
  }

  /* Serialize writes, and retain failures until that key is saved successfully.
   * Merely draining the queue must never turn a failed save into a successful Apply. */
  function setKey(key, value, label) {
    clearTimeout(setTimers[key]);
    delete setTimers[key];
    delete pendingSets[key];
    pendingSaveCount++;
    var saved = saveQueue.then(function () { return ctl(['set', key, value]); }).then(function (r) {
      pendingSaveCount--;
      if (r.errno !== 0) {
        saveFailures[key] = true;
        toast('Failed: ' + key + ' (' + errorText(r) + ')');
      } else {
        delete saveFailures[key];
        if (label) toast(label);
      }
      return r;
    });
    saveQueue = saved.then(function () {});
    return saved;
  }

  function debouncedSet(key, value, label) {
    clearTimeout(setTimers[key]);
    pendingSets[key] = { value: value, label: label };
    setTimers[key] = setTimeout(function () {
      var pending = pendingSets[key];
      if (pending) setKey(key, pending.value, pending.label);
    }, 280);
  }

  /* ---------- rendering ---------- */
  function renderControls(s) {
    $('enable-toggle').checked = !!s.enabled;
    setActiveMode(s.mode);

    $('in_enabled').checked = !!s.in_enabled;
    $('out_enabled').checked = !!s.out_enabled;

    $('strength').value = s.strength;         $('strength-val').textContent = s.strength;
    $('out_strength').value = s.out_strength; $('out_strength-val').textContent = s.out_strength;
    $('atten_db').value = s.atten_db;         $('atten_db-val').textContent = s.atten_db + ' dB';
    $('vad').value = s.vad;                   $('vad-val').textContent = s.vad;

    $('highpass').checked = !!s.highpass;
    $('autogain').checked = !!s.autogain;

    var targets = (s.targets || '').split(',');
    document.querySelectorAll('#chips-in .chip').forEach(function (c) {
      c.classList.toggle('active', targets.indexOf(c.dataset.target) !== -1);
    });
    var otargets = (s.out_targets || '').split(',');
    document.querySelectorAll('#chips-out .chip').forEach(function (c) {
      c.classList.toggle('active', otargets.indexOf(c.dataset.otarget) !== -1);
    });
  }

  function setActiveMode(mode) {
    document.querySelectorAll('.mode-card').forEach(function (c) {
      c.classList.toggle('active', String(mode) === c.dataset.mode);
    });
    updateFallbackHint(mode);
  }

  var lastStatus = {};
  function updateFallbackHint(mode) {
    var s = lastStatus;
    var hint = $('mode-fallback-hint');
    if (String(mode) === '2' && s.model_dtln === false) {
      hint.textContent = 'DTLN models not installed — Clarifae will fall back to RNNoise. See INSTALL.md.';
    } else if (String(mode) === '3' && s.model_df === false) {
      hint.textContent = 'DeepFilterNet (libdf/model) not installed — Clarifae will fall back to RNNoise. See INSTALL.md.';
    } else { hint.textContent = ''; }
  }

  function modelReady(s, which) {
    if (which === 'rnnoise') return s.model_rnnoise;
    if (which === 'dtln') return s.model_dtln;
    if (which === 'df') return s.model_df;
    return false;
  }

  function renderStatus(s) {
    lastStatus = s;
    $('master-sub').textContent = s.enabled
      ? 'Enabled in settings — ' + (s.mode_name || 'check attachment and PCM processing separately') + '.'
      : 'Disabled in settings.';
    if (s.version) $('version').textContent = 'Clarifae ' + s.version;

    document.querySelectorAll('.mode-status').forEach(function (el) {
      var ready = modelReady(s, el.dataset.model);
      el.classList.remove('ready', 'missing');
      if (el.dataset.model === 'rnnoise') {
        el.textContent = ready ? '● installed' : '● library missing';
        el.classList.add(ready ? 'ready' : 'missing');
      } else {
        el.textContent = ready ? '● model ready' : '● model not installed';
        el.classList.add(ready ? 'ready' : 'missing');
      }
    });
    updateFallbackHint(s.mode);

    var rows = [
      ['Enabled', bool(s.enabled)],
      ['Mic cleanup', bool(s.in_enabled)],
      ['Playback cleanup', bool(s.out_enabled)],
      ['Effect detected (heuristic)', triState(s.effect_loaded)],
      ['audio_effects patched', bool(s.xml_patched)],
      ['Library (64-bit)', bool(s.lib64)],
      ['Library (32-bit)', s.lib32 ? yes('yes') : neutral('—')],
      ['audioserver', bool(s.audioserver)],
      ['RNNoise', bool(s.model_rnnoise)],
      ['DTLN model', s.model_dtln ? yes('yes') : warn('no')],
      ['DeepFilterNet', s.model_df ? yes('yes') : warn('no')]
    ];
    $('status-grid').innerHTML = rows.map(function (r) {
      return '<div class="status-item"><span class="k">' + r[0] + '</span>' + r[1] + '</div>';
    }).join('');
  }

  function yes(t) { return '<span class="v yes">' + t + '</span>'; }
  function no(t) { return '<span class="v no">' + t + '</span>'; }
  function warn(t) { return '<span class="v warn">' + t + '</span>'; }
  function neutral(t) { return '<span class="v">' + t + '</span>'; }
  function bool(b) { return b ? yes('yes') : no('no'); }
  function triState(b) { return b ? yes('detected') : warn('not detected'); }

  /* ---------- data flow ---------- */
  // A slow/absent bridge must not accumulate one queued poll every five seconds.
  function readStatus() {
    if (!statusRequest) {
      statusRequest = ctl(['status-json']).then(function (r) { statusRequest = null; return r; });
    }
    return statusRequest;
  }

  function loadAll() {
    return readStatus().then(function (r) {
      var s = r.errno === 0 ? parse(r.stdout) : null;
      if (!s) { toast('Could not read status: ' + (r.errno !== 0 ? errorText(r) : 'invalid JSON')); return; }
      renderControls(s);
      renderStatus(s);
    });
  }

  function refreshStatus(reportError) {
    if (reportError !== true && (actionRunning || pendingSaveCount || Object.keys(pendingSets).length)) return Promise.resolve();
    return readStatus().then(function (r) {
      var s = r.errno === 0 ? parse(r.stdout) : null;
      if (s) renderStatus(s);
      else if (reportError === true) toast('Could not read status: ' + (r.errno !== 0 ? errorText(r) : 'invalid JSON'));
    });
  }

  /* ---------- wiring ---------- */
  function wire() {
    $('enable-toggle').addEventListener('change', function () {
      setKey('enabled', this.checked ? 1 : 0, this.checked ? 'Clarity on' : 'Clarity paused')
        .then(refreshStatus);
    });
    $('in_enabled').addEventListener('change', function () {
      setKey('in_enabled', this.checked ? 1 : 0, this.checked ? 'Mic cleanup on' : 'Mic cleanup off').then(refreshStatus);
    });
    $('out_enabled').addEventListener('change', function () {
      setKey('out_enabled', this.checked ? 1 : 0, this.checked ? 'Playback cleanup on' : 'Playback cleanup off').then(refreshStatus);
    });

    document.querySelectorAll('.mode-card').forEach(function (card) {
      card.addEventListener('click', function () {
        var m = card.dataset.mode;
        setActiveMode(m);
        setKey('mode', m, 'Mode: ' + card.querySelector('.mode-name').textContent).then(refreshStatus);
      });
    });

    [['strength', ''], ['out_strength', ''], ['atten_db', ' dB'], ['vad', '']].forEach(function (pair) {
      var id = pair[0], suffix = pair[1];
      var el = $(id);
      el.addEventListener('input', function () {
        $(id + '-val').textContent = el.value + suffix;
        debouncedSet(id, el.value, null);
      });
    });

    $('highpass').addEventListener('change', function () { setKey('highpass', this.checked ? 1 : 0, 'Saved'); });
    $('autogain').addEventListener('change', function () { setKey('autogain', this.checked ? 1 : 0, 'Saved'); });

    // capture sources (input)
    document.querySelectorAll('#chips-in .chip').forEach(function (chip) {
      chip.addEventListener('click', function () {
        chip.classList.toggle('active');
        var sel = [];
        document.querySelectorAll('#chips-in .chip.active').forEach(function (c) { sel.push(c.dataset.target); });
        if (sel.length === 0) { chip.classList.add('active'); sel.push(chip.dataset.target); toast('Keep at least one source'); }
        setKey('targets', sel.join(','), 'Capture sources saved; reinstall/regenerate routes to change installed targets');
      });
    });

    // playback streams (output) — zero selections is allowed (= output off via targets)
    document.querySelectorAll('#chips-out .chip').forEach(function (chip) {
      chip.addEventListener('click', function () {
        chip.classList.toggle('active');
        var sel = [];
        document.querySelectorAll('#chips-out .chip.active').forEach(function (c) { sel.push(c.dataset.otarget); });
        setKey('out_targets', sel.join(','), 'Playback streams saved; reinstall/regenerate routes to change installed targets');
      });
    });

    $('btn-apply').addEventListener('click', function () {
      runAction('apply');
    });
    $('btn-restart').addEventListener('click', function () {
      if (typeof confirm !== 'function' || !confirm('Restart audioserver? Active recordings/calls will glitch briefly. No active-call protection is provided.')) return;
      runAction('restart-audio');
    });
    $('btn-log').addEventListener('click', function () {
      ctl(['log']).then(function (r) {
        if (r.errno !== 0) toast('Could not read log: ' + errorText(r));
        $('log-content').textContent = (r.errno !== 0 ? 'Log command failed: ' + errorText(r) + '\n' : '') + (r.stdout || '(empty)');
        $('log-modal').classList.remove('hidden');
      });
    });
    $('log-close').addEventListener('click', function () { $('log-modal').classList.add('hidden'); });
    $('log-modal').addEventListener('click', function (e) { if (e.target === this) this.classList.add('hidden'); });
    $('btn-reset').addEventListener('click', function () {
      if (typeof confirm === 'function' && !confirm('Reset all Clarifae settings to defaults?')) return;
      runAction('reset');
    });
    $('btn-refresh').addEventListener('click', function () { refreshStatus(true); });
  }

  /* ---------- init ---------- */
  document.addEventListener('DOMContentLoaded', function () {
    if (KSU.isMock) { var b = $('env-badge'); b.textContent = 'preview'; b.classList.remove('hidden'); }
    wire();
    loadAll();
    refreshTimer = setInterval(refreshStatus, 5000);
  });
})();
