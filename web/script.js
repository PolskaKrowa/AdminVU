/* ============================================================
   script.js  –  shared JS for all dashboard pages
   ============================================================ */

/* ── Utilities ──────────────────────────────────────────────────────────── */

/**
 * fmt(date) → "HH:MM:SS" string from a Date object (or now if omitted).
 */
function fmt(date = new Date()) {
    const pad = n => String(n).padStart(2, '0');
    return `${pad(date.getHours())}:${pad(date.getMinutes())}:${pad(date.getSeconds())}`;
}

/**
 * fmtDuration(seconds) → "Xh Ym Zs" string.
 */
function fmtDuration(s) {
    const h = Math.floor(s / 3600);
    const m = Math.floor((s % 3600) / 60);
    const sec = s % 60;
    return `${String(h).padStart(2, '0')}:${String(m).padStart(2, '0')}:${String(sec).padStart(2, '0')}`;
}

/**
 * qs(selector, root) – shorthand querySelector.
 */
function qs(selector, root = document) {
    return root.querySelector(selector);
}

/* ── Activity log ───────────────────────────────────────────────────────── */

const LogLevel = Object.freeze({
    INFO:  'log-info',
    OK:    'log-ok',
    WARN:  'log-warn',
    ERROR: 'log-error',
});

/**
 * appendLog(message, level)
 * Adds a timestamped line to the #log-body element (if present on the page).
 */
function appendLog(message, level = LogLevel.INFO) {
    const body = qs('#log-body');
    if (!body) return;

    const line = document.createElement('div');
    line.className = `log-line ${level}`;
    line.innerHTML = `
        <span class="log-time">${fmt()}</span>
        <span class="log-msg">${message}</span>
    `;

    body.prepend(line);

    /* Keep the log from growing unbounded in the DOM */
    const MAX_LINES = 100;
    const lines = body.querySelectorAll('.log-line');
    if (lines.length > MAX_LINES) {
        for (let i = MAX_LINES; i < lines.length; i++)
            lines[i].remove();
    }
}

/* ── Status indicator ───────────────────────────────────────────────────── */

/**
 * setStatus('online' | 'offline' | 'connecting')
 * Updates the header status dot and label.
 */
function setStatus(state) {
    const dot   = qs('#status-dot');
    const label = qs('#status-label');
    if (!dot || !label) return;

    dot.className   = `status-dot ${state}`;
    label.textContent = state;
}

/* ── Stat card helpers ──────────────────────────────────────────────────── */

/**
 * setCardValue(id, value)
 * Updates the .card-value inside a card by the card's element id.
 */
function setCardValue(cardId, value) {
    const card = qs(`#${cardId}`);
    if (!card) return;
    const el = card.querySelector('.card-value');
    if (el) el.textContent = value;
}

/* ── Footer clock ───────────────────────────────────────────────────────── */

function startFooterClock() {
    const el = qs('#footer-time');
    if (!el) return;
    const tick = () => { el.textContent = fmt(); };
    tick();
    setInterval(tick, 1000);
}

/* ── Uptime counter ─────────────────────────────────────────────────────── */

let _uptimeStart = Date.now();

function startUptimeClock() {
    const el = qs('#uptime-value');
    if (!el) return;

    setInterval(() => {
        const elapsed = Math.floor((Date.now() - _uptimeStart) / 1000);
        el.textContent = fmtDuration(elapsed);
    }, 1000);
}

/* ── Stamp the init-time log line ───────────────────────────────────────── */

(function stampInitTime() {
    const el = qs('#init-time');
    if (el) el.textContent = fmt();
})();

/* ── Log clear button ───────────────────────────────────────────────────── */

(function bindLogClear() {
    const btn = qs('#log-clear');
    if (!btn) return;
    btn.addEventListener('click', () => {
        const body = qs('#log-body');
        if (body) body.innerHTML = '';
        appendLog('Log cleared.', LogLevel.INFO);
    });
})();

/* ── Placeholder: future API polling ────────────────────────────────────── */
/*
 * When you add a /api/status endpoint to the HTTP server you can poll it here:
 *
 * async function pollStatus() {
 *     try {
 *         const res  = await fetch('/api/status');
 *         const data = await res.json();
 *         setStatus('online');
 *         setCardValue('card-guilds',   data.guild_count);
 *         setCardValue('card-warnings', data.warning_count);
 *         appendLog('Status refreshed.', LogLevel.OK);
 *     } catch (err) {
 *         setStatus('offline');
 *         appendLog('Could not reach API: ' + err.message, LogLevel.ERROR);
 *     }
 * }
 *
 * pollStatus();
 * setInterval(pollStatus, 10_000);
 */

/* ── Boot sequence ──────────────────────────────────────────────────────── */

document.addEventListener('DOMContentLoaded', () => {
    startFooterClock();
    startUptimeClock();

    /* Simulated initial status – replace with real API call when ready */
    setStatus('online');
    appendLog('Connected to local server.', LogLevel.OK);
});