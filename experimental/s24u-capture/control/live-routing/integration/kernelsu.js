/*
 * kernelsu.js — thin bridge over the KernelSU / APatch / MMRL WebUI host API.
 *
 * The host injects a global `ksu` object with:
 *     ksu.exec(command, optionsJson, callbackName)
 *        -> later calls window[callbackName](errno, stdout, stderr)
 *     ksu.toast(message)
 *
 * We wrap exec() in a Promise. When no host is present (e.g. opened in a desktop
 * browser for design work) we fall back to a mock so the page still renders.
 *
 * Exposes window.ClarifaeKSU = { exec, toast, isMock }.
 */
(function () {
  'use strict';

  var hasHost = typeof window.ksu === 'object' && typeof window.ksu.exec === 'function';

  function genCb() {
    return 'clf_cb_' + Math.random().toString(36).slice(2) + '_' + Date.now().toString(36);
  }

  function exec(command, options) {
    options = options || {};
    var requested = Number(options.timeout);
    var seconds = isFinite(requested) && requested > 0 ? Math.max(8, Math.min(70, requested)) : 8;
    var hostOptions = {};
    Object.keys(options).forEach(function (key) { hostOptions[key] = options[key]; });
    hostOptions.timeout = seconds;
    return new Promise(function (resolve) {
      if (!hasHost) { resolve(mockExec(command)); return; }
      var cb = genCb();
      var done = false;
      var timer;
      function finish(errno, stdout, stderr, completionUnknown) {
        if (done) return;
        done = true;
        clearTimeout(timer);
        try { delete window[cb]; } catch (e) { window[cb] = undefined; }
        var code = Number(errno);
        if ((typeof errno !== 'number' && typeof errno !== 'string') ||
            (typeof errno === 'string' && !/^-?[0-9]+$/.test(errno)) ||
            !isFinite(code) || Math.floor(code) !== code) code = -1;
        resolve({ errno: code, stdout: stdout == null ? '' : String(stdout), stderr: stderr == null ? '' : String(stderr), completionUnknown: !!completionUnknown || code < 0 });
      }
      window[cb] = finish;
      // Bounded UI wait; timing out does not prove the host cancelled the command.
      timer = setTimeout(function () { finish(-2, '', 'exec timeout; command completion unknown', true); }, seconds * 1000);
      try {
        window.ksu.exec(command, JSON.stringify(hostOptions), cb);
      } catch (e) {
        finish(-1, '', String(e), true);
      }
    });
  }

  function toast(msg) {
    if (hasHost && typeof window.ksu.toast === 'function') {
      try { window.ksu.toast(msg); return; } catch (e) { /* fall through */ }
    }
    console.log('[toast] ' + msg);
  }

  // ---- design-preview mock (only used outside a root WebUI host) ----
  function mockExec(command) {
    var out = '';
    if (command.indexOf('status-json') !== -1) {
      out = JSON.stringify({
        enabled: true, mode: 1, mode_name: 'RNNoise (Low latency)',
        in_enabled: true, out_enabled: true,
        strength: 70, out_strength: 60, atten_db: 24, vad: 50, targets: 'mic,voice_comm',
        out_targets: 'music,voice_call',
        highpass: true, autogain: false, loglevel: 1,
        lib32: false, lib64: true, xml_patched: true,
        model_rnnoise: true, model_dtln: false, model_df: false,
        audioserver: true, effect_loaded: false, version: 'v1.0.0 (preview)'
      });
    } else if (command.indexOf('get-json') !== -1) {
      out = JSON.stringify({
        enabled: true, mode: 1, mode_name: 'RNNoise (Low latency)',
        in_enabled: true, out_enabled: true,
        strength: 70, out_strength: 60, atten_db: 24, vad: 50, targets: 'mic,voice_comm',
        out_targets: 'music,voice_call',
        highpass: true, autogain: false, loglevel: 1
      });
    } else if (command.indexOf(' log') !== -1) {
      out = '[preview] no device — clarifae-ctl log unavailable.';
    } else {
      out = 'ok (preview)';
    }
    return Promise.resolve({ errno: 0, stdout: out, stderr: '' });
  }

  window.ClarifaeKSU = { exec: exec, toast: toast, isMock: !hasHost };
})();
