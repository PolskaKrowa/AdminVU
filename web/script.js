/* ============================================================
   script.js  –  shared JS for all dashboard pages
   ============================================================ */

/* ── Utilities ──────────────────────────────────────────────────────────── */

function fmt(date = new Date()) {
    const pad = n => String(n).padStart(2, '0');
    return `${pad(date.getHours())}:${pad(date.getMinutes())}:${pad(date.getSeconds())}`;
}

function fmtDuration(s) {
    const h = Math.floor(s / 3600);
    const m = Math.floor((s % 3600) / 60);
    const sec = s % 60;
    return `${String(h).padStart(2,'0')}:${String(m).padStart(2,'0')}:${String(sec).padStart(2,'0')}`;
}

/**
 * fmtTs(unixSeconds) – format a Unix timestamp as a compact local datetime.
 */
function fmtTs(ts) {
    if (!ts) return '—';
    const d = new Date(Number(ts) * 1000);
    const pad = n => String(n).padStart(2, '0');
    return `${d.getFullYear()}-${pad(d.getMonth()+1)}-${pad(d.getDate())} `
         + `${pad(d.getHours())}:${pad(d.getMinutes())}`;
}

/**
 * shortId(id) – abbreviate a Discord snowflake for display.
 * Shows last 6 digits to keep it recognisable without being overwhelming.
 */
function shortId(id) {
    if (!id || id === '0') return '—';
    const s = String(id);
    return s.length > 6 ? '…' + s.slice(-6) : s;
}

/** esc(str) – HTML-escape a string for safe insertion. */
function esc(str) {
    if (str == null) return '';
    return String(str)
        .replace(/&/g, '&amp;')
        .replace(/</g, '&lt;')
        .replace(/>/g, '&gt;')
        .replace(/"/g, '&quot;');
}

function qs(selector, root = document) { return root.querySelector(selector); }

/* ── API fetch helpers ──────────────────────────────────────────────────── */

/**
 * apiGet(url) – fetch JSON from the bot's API.
 * Returns the parsed object, or null on error.
 */
async function apiGet(url) {
    try {
        const res = await fetch(url);
        if (!res.ok) throw new Error(`HTTP ${res.status}`);
        return await res.json();
    } catch (err) {
        appendLog('API error: ' + err.message, LogLevel.ERROR);
        setStatus('offline');
        return null;
    }
}

/**
 * apiPost(url, body) – POST URL-encoded data, return parsed JSON or null.
 */
async function apiPost(url, body) {
    try {
        const res = await fetch(url, {
            method:  'POST',
            headers: { 'Content-Type': 'application/x-www-form-urlencoded' },
            body,
        });
        if (!res.ok) throw new Error(`HTTP ${res.status}`);
        return await res.json();
    } catch (err) {
        appendLog('API error: ' + err.message, LogLevel.ERROR);
        return null;
    }
}

/* ── Activity log ───────────────────────────────────────────────────────── */

const LogLevel = Object.freeze({
    INFO:  'log-info',
    OK:    'log-ok',
    WARN:  'log-warn',
    ERROR: 'log-error',
});

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

    const MAX = 100;
    const lines = body.querySelectorAll('.log-line');
    if (lines.length > MAX)
        for (let i = MAX; i < lines.length; i++) lines[i].remove();
}

/* ── Status indicator ───────────────────────────────────────────────────── */

function setStatus(state) {
    const dot   = qs('#status-dot');
    const label = qs('#status-label');
    if (!dot || !label) return;
    dot.className   = `status-dot ${state}`;
    label.textContent = state;
}

/* ── Stat card helpers ──────────────────────────────────────────────────── */

function setCardValue(cardId, value) {
    const card = qs(`#${cardId}`);
    if (!card) return;
    const el = card.querySelector('.card-value');
    if (el) el.textContent = value;
}

/* ── Clocks ─────────────────────────────────────────────────────────────── */

function startFooterClock() {
    const el = qs('#footer-time');
    if (!el) return;
    const tick = () => { el.textContent = fmt(); };
    tick();
    setInterval(tick, 1000);
}

let _uptimeStart = Date.now();

function startUptimeClock() {
    const el = qs('#uptime-value');
    if (!el) return;
    setInterval(() => {
        const elapsed = Math.floor((Date.now() - _uptimeStart) / 1000);
        el.textContent = fmtDuration(elapsed);
    }, 1000);
}

/* ── Init-time stamp on dashboard log ───────────────────────────────────── */
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

/* ── Copy-on-click for .copyable cells ──────────────────────────────────── */
document.addEventListener('click', e => {
    const el = e.target.closest('.copyable');
    if (!el) return;
    const text = el.getAttribute('title') || el.textContent;
    navigator.clipboard.writeText(text).then(() => {
        const orig = el.textContent;
        el.textContent = '✓ copied';
        setTimeout(() => { el.textContent = orig; }, 900);
    });
});

/* ── Boot ────────────────────────────────────────────────────────────────── */
document.addEventListener('DOMContentLoaded', () => {
    startFooterClock();
    setStatus('online');
});