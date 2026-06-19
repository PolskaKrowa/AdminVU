/* ============================================================
   script.js  –  shared helpers for all AdminVU web pages
   ============================================================ */

/* ── Utilities ──────────────────────────────────────────────────────────── */

function fmt(date = new Date()) {
    const pad = n => String(n).padStart(2, '0');
    return `${pad(date.getHours())}:${pad(date.getMinutes())}:${pad(date.getSeconds())}`;
}

function fmtDuration(s) {
    s = Math.max(0, Math.floor(s));
    const h = Math.floor(s / 3600);
    const m = Math.floor((s % 3600) / 60);
    const sec = s % 60;
    return `${String(h).padStart(2,'0')}:${String(m).padStart(2,'0')}:${String(sec).padStart(2,'0')}`;
}

function fmtTs(ts) {
    if (!ts && ts !== 0) return '—';
    const d = new Date(Number(ts) * 1000);
    if (isNaN(d.getTime())) return '—';
    const pad = n => String(n).padStart(2, '0');
    return `${d.getFullYear()}-${pad(d.getMonth()+1)}-${pad(d.getDate())} `
         + `${pad(d.getHours())}:${pad(d.getMinutes())}`;
}

function shortId(id) {
    if (id == null || id === '0' || id === '') return '—';
    const s = String(id);
    return s.length > 6 ? '…' + s.slice(-6) : s;
}

function esc(str) {
    if (str == null) return '';
    return String(str)
        .replace(/&/g, '&amp;')
        .replace(/</g, '&lt;')
        .replace(/>/g, '&gt;')
        .replace(/"/g, '&quot;');
}

function qs(selector, root = document) { return root.querySelector(selector); }
function qsa(selector, root = document) { return Array.from(root.querySelectorAll(selector)); }

/* ── API fetch helpers ──────────────────────────────────────────────────── */

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

const STATUS_LABELS = { online: 'online', offline: 'offline', connecting: 'connecting…' };

function setStatus(state) {
    const dot   = qs('#status-dot');
    const label = qs('#status-label');
    if (dot)   dot.className = `status-dot ${state}`;
    if (label) label.textContent = STATUS_LABELS[state] || state;
}

/* ── Stat card helpers ──────────────────────────────────────────────────── */

function setCardValue(cardId, value) {
    const card = qs(`#${cardId}`);
    if (!card) return;
    const el = card.querySelector('.card-value');
    if (el) el.textContent = (value == null || value === '') ? '—' : value;
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

/* ── Theme toggle (light/dark via <html class="light">) ─────────────────── */

function initTheme() {
    const root = document.documentElement;
    try {
        const saved = localStorage.getItem('adminvu-theme');
        if (saved === 'light') root.classList.add('light');
    } catch (_) {}

    const update = () => {
        document.querySelectorAll('.theme-toggle').forEach(btn => {
            btn.textContent = root.classList.contains('light') ? '🌙' : '☀️';
            btn.setAttribute('aria-label',
                root.classList.contains('light') ? 'Switch to dark theme' : 'Switch to light theme');
        });
    };
    update();

    document.addEventListener('click', e => {
        const btn = e.target.closest('.theme-toggle');
        if (!btn) return;
        root.classList.toggle('light');
        try {
            localStorage.setItem('adminvu-theme',
                root.classList.contains('light') ? 'light' : 'dark');
        } catch (_) {}
        update();
    });
}

/* ── Init-time stamp on dashboard log ───────────────────────────────────── */
(function stampInitTime() {
    const el = qs('#init-time');
    if (el) el.textContent = fmt();
})();

/* ── Log clear button ───────────────────────────────────────────────────── */
(function bindLogClear() {
    document.addEventListener('click', e => {
        const btn = e.target.closest('#log-clear');
        if (!btn) return;
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
    if (navigator.clipboard && navigator.clipboard.writeText) {
        navigator.clipboard.writeText(text).then(() => {
            const orig = el.textContent;
            el.textContent = '✓ copied';
            setTimeout(() => { el.textContent = orig; }, 900);
        });
    }
});

/* ── Click-to-reveal for Discord spoilers (touch-friendly) ──────────────── */
document.addEventListener('click', e => {
    const sp = e.target.closest && e.target.closest('.dmd-spoiler');
    if (!sp) return;
    sp.setAttribute('data-revealed',
        sp.getAttribute('data-revealed') === 'true' ? 'false' : 'true');
});

/* ── Discord-style markdown renderer ───────────────────────────────────────
   Renders a SAFE HTML string from Discord-flavoured markdown.
   Supports: fenced code blocks (```), inline code (`), spoilers (||...||),
   single- and multi-line blockquotes (> / >>>), headings (# ## ###),
   bold (**), bold-italic (***), italic (* or _), underline (__),
   strikethrough (~~), and bare-URL links. Re-uses the shared esc() helper
   so the output is XSS-safe (esc is hoisted because it's a function decl).
   Placeholders (\u0001CBn\u0001 / \u0001ICn\u0001 / \u0001SPn\u0001) protect
   pre-extracted code/spoilers from the inline-formatter regexes.
   ─────────────────────────────────────────────────────────────────────────── */
function renderDiscordMarkdown(text) {
    if (!text) return '';
    var codeBlocks = [];
    var inlineCodes = [];
    var spoilers = [];

    // 1. Fenced code blocks ```...\n...```  (extract BEFORE escaping)
    text = text.replace(/```(?:[^\n`]*\n)?([\s\S]*?)```/g, function (_, code) {
        var html = '<pre class="dmd-pre"><code>' + esc(code.replace(/\n$/, '')) + '</code></pre>';
        codeBlocks.push(html);
        return '\u0001CB' + (codeBlocks.length - 1) + '\u0001';
    });

    // 2. Inline code `...`  (extract BEFORE escaping)
    text = text.replace(/`([^`\n]+)`/g, function (_, code) {
        inlineCodes.push('<code class="dmd-code">' + esc(code) + '</code>');
        return '\u0001IC' + (inlineCodes.length - 1) + '\u0001';
    });

    // 3. HTML-escape the rest. Note: '>' becomes '&gt;', so blockquote
    //    detection below must look for the escaped form '&gt;'. '#', '*',
    //    '_', '~', '|' are NOT escaped by esc(), so heading/inline detection
    //    works on them directly.
    text = esc(text);

    // 4. Spoilers ||...||  (inner text is already escaped from step 3)
    text = text.replace(/\|\|([^|\n]+?)\|\|/g, function (_, inner) {
        spoilers.push('<span class="dmd-spoiler" tabindex="0">' + inner + '</span>');
        return '\u0001SP' + (spoilers.length - 1) + '\u0001';
    });

    // 5-6. Block structure + inline formatting, line by line
    var lines = text.split('\n');
    var out = [];
    var i = 0;
    while (i < lines.length) {
        var line = lines[i];
        // >>> multi-line blockquote (rest of message)
        if (/^&gt;&gt;&gt;\s?/.test(line)) {
            var rest = [line.replace(/^&gt;&gt;&gt;\s?/, '')];
            for (var j = i + 1; j < lines.length; j++) rest.push(lines[j]);
            out.push('<blockquote class="dmd-quote">' + applyInline(rest.join('<br>')) + '</blockquote>');
            i = lines.length;
            break;
        }
        // consecutive "> " single-line quotes  ('>' is escaped to '&gt;')
        if (/^&gt;\s?/.test(line)) {
            var quoteLines = [];
            while (i < lines.length && /^&gt;\s?/.test(lines[i])) {
                quoteLines.push(lines[i].replace(/^&gt;\s?/, ''));
                i++;
            }
            out.push('<blockquote class="dmd-quote">' + applyInline(quoteLines.join('<br>')) + '</blockquote>');
            continue;
        }
        // headings (### ## #)  ('#' is NOT escaped by esc())
        if (/^###\s/.test(line)) { out.push('<h3 class="dmd-h dmd-h3">' + applyInline(line.replace(/^###\s/, '')) + '</h3>'); i++; continue; }
        if (/^##\s/.test(line))  { out.push('<h2 class="dmd-h dmd-h2">' + applyInline(line.replace(/^##\s/, ''))  + '</h2>'); i++; continue; }
        if (/^#\s/.test(line))   { out.push('<h1 class="dmd-h dmd-h1">' + applyInline(line.replace(/^#\s/, ''))   + '</h1>'); i++; continue; }
        // normal line
        out.push(applyInline(line));
        i++;
    }
    var html = out.join('<br>');

    // 7. Restore spoilers
    html = html.replace(/\u0001SP(\d+)\u0001/g, function (_, n) { return spoilers[+n]; });
    // 8. Restore inline code
    html = html.replace(/\u0001IC(\d+)\u0001/g, function (_, n) { return inlineCodes[+n]; });
    // 9. Restore code blocks
    html = html.replace(/\u0001CB(\d+)\u0001/g, function (_, n) { return codeBlocks[+n]; });
    return html;
}

// Inline markdown on already-escaped text. Returns HTML. Does NOT call esc()
// again — the text is already escaped (step 3 above) and any remaining
// placeholder tokens (\u0001CBn\u0001 etc.) contain no `* _ ~ |` so the inline
// regexes won't corrupt them.
function applyInline(s) {
    return s
        .replace(/\*\*\*([\s\S]+?)\*\*\*/g, '<strong><em>$1</em></strong>')
        .replace(/\*\*([\s\S]+?)\*\*/g, '<strong>$1</strong>')
        .replace(/__([\s\S]+?)__/g, '<u>$1</u>')
        .replace(/~~([\s\S]+?)~~/g, '<del>$1</del>')
        .replace(/\*([^*\n]+?)\*|_([^_\n]+?)_/g, '<em>$1$2</em>')
        .replace(/(https?:\/\/[^\s<]+)/g, '<a href="$1" target="_blank" rel="noopener noreferrer">$1</a>');
}

/* ── Page transitions ───────────────────────────────────────────────────── */
(function setupPageTransitions() {
    document.addEventListener('click', e => {
        const link = e.target.closest('.nav-link');
        if (!link) return;
        const href = link.getAttribute('href');
        if (!href || link.classList.contains('active')) return;
        e.preventDefault();
        document.body.classList.remove('loaded');
        setTimeout(() => { window.location.href = href; }, 310);
    }, true);
})();

/* ── Boot ────────────────────────────────────────────────────────────────── */
document.addEventListener('DOMContentLoaded', () => {
    initTheme();
    startFooterClock();
    setStatus('connecting');
    requestAnimationFrame(() => {
        requestAnimationFrame(() => {
            document.body.classList.add('loaded');
        });
    });
});
