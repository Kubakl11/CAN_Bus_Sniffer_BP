// ═══════════════════════════════════════════════════════════════
// data.js — datová vrstva dashboardu (AP / lokální webserver)
// ═══════════════════════════════════════════════════════════════

const API_URL          = '/api/data';
const CMD_URL          = '/api/cmd';
const FETCH_INTERVAL_MS = 200;   // 5 Hz polling

// posledni data z esp
let _lastData = null;

// ── Fetch loop ────────────────────────────────────────────────
async function _fetchLoop() {
    try {
        const res = await fetch(API_URL, {
            cache: 'no-store',
            signal: AbortSignal.timeout(400),  
        });
        if (res.ok) {
            _lastData = await res.json();
            _appendHistory(_lastData);
        }
    } catch (e) {
        // Síťová chyba nebo timeout — _lastData zůstane 
    }
    setTimeout(_fetchLoop, FETCH_INTERVAL_MS);
}

_fetchLoop();

// ── Veřejné API ───────────────────────────────────────────────

export function getData() {
    return _lastData || {};
}

// Odešle příkaz na ESP (SCAN / LOOP / STOP / READ)
export async function sendCmd(cmd) {
    try {
        await fetch(`${CMD_URL}?c=${cmd}`, {
            cache: 'no-store',
            signal: AbortSignal.timeout(400),
        });
    } catch (e) {
        console.warn('[data.js] sendCmd failed:', e);
    }
}

// ── Historie jízdy ────────────────────────────────────────────
// ukládá kazdou sekundu do RAM
//každých 5 s. Přežije zavření tabu / reload stránky.
// Limit: 10 800 vzorků = 30 minut @ 1 Hz.
const MAX_HISTORY = 10800;
let _tripHistory  = [];
let _lastSave     = 0;
let _lastPush     = 0;

// nacteni historie
(function _loadHistory() {
    try {
        const s = localStorage.getItem('tripHistory');
        if (s) _tripHistory = JSON.parse(s);
    } catch (e) {
        _tripHistory = [];
    }
})();

function _appendHistory(data) {
    const now = Date.now();
    if (now - _lastPush < 1000) return;   // max 1× za sekundu
    _lastPush = now;

    _tripHistory.push({ t: now, ...data });
    if (_tripHistory.length > MAX_HISTORY) _tripHistory.shift();

    if (now - _lastSave > 5000) {          // flush do localStorage každých 5 s
        try {
            localStorage.setItem('tripHistory', JSON.stringify(_tripHistory));
        } catch (e) {
            // localStorage plný → ořízne na polovnu a znovu
            _tripHistory = _tripHistory.slice(-Math.floor(MAX_HISTORY / 2));
            try { localStorage.setItem('tripHistory', JSON.stringify(_tripHistory)); }
            catch (_) { /* */ }
        }
        _lastSave = now;
    }
}

// Vrátí celou historii jízdy (pro graphs.html)
export function getTripHistory() {
    return _tripHistory;
}

// Smaže historii z RAM i localStorage
export function clearTripHistory() {
    _tripHistory = [];
    _lastPush    = 0;
    _lastSave    = 0;
    try { localStorage.removeItem('tripHistory'); } catch (_) {}
}

// Legacy stub — ponecháno kvůli kompatibilitě s dashboard.html
export function setScenario(_name) { /* no-op */ }