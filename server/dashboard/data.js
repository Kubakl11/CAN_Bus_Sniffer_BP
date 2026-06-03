// ═══════════════════════════════════════════════════════════════
// data.js — LTE dashboard data layer
//
// Real-time:  GET /data/latest  (5 Hz polling, state_cache merge)
// Historie:   GET /data/range   (pakety z DB za casovy rozsah)
// ═══════════════════════════════════════════════════════════════

const FETCH_INTERVAL_MS = 200;   // 5 Hz
const STALE_THRESHOLD_MS = 5000; // po 5s bez dat = stale

let _cache    = {};
let _lastOk   = 0;      // timestamp posledniho uspesneho fetche

// ── Real-time polling ─────────────────────────────────────────
async function _fetchLatest() {
    try {
        const r = await fetch('/data/latest', {
            cache: 'no-store',
            signal: AbortSignal.timeout(400),
        });
        if (r.ok) {
            const j = await r.json();
            // server uz posila spravne klice bez nonce/type
            Object.assign(_cache, j);
            _lastOk = Date.now();
        }
    } catch (_) {
        // Sit. chyba — cache zustane z posledniho uspesneho fetche
    }
}

_fetchLatest();
setInterval(_fetchLatest, FETCH_INTERVAL_MS);

// ── Verejne API ───────────────────────────────────────────────

/**
 * Vrati posledni zname hodnoty vsech PIDu.
 * Klice odpovidaji pidList[].key z firmware (rpm, speed, coolantTemp, ...).
 * Hodnota null = PID neprisel nebo auto nepodporuje.
 */
export function getData() {
    return { ..._cache };
}


 /* Vrati true pokud posledni uspesny fetch byl pred vice nez STALE_THRESHOLD_MS.
 * Dashboard muze zobrazit "No signal" indikator.
 */
export function isStale() {
    if (_lastOk === 0) return true;
    return Date.now() - _lastOk > STALE_THRESHOLD_MS;
}

/**
 * Vrati pole historickych paketu z DB.
 * @param {string} fromISO  - ISO timestamp zacatku (null = bez omezeni)
 * @param {string} toISO    - ISO timestamp konce   (null = bez omezeni)
 * @param {number} limit    - max pocet paketu (default 1000)
 * @returns {Promise<Array>}
 */
export async function getRange(fromISO, toISO, limit = 1000) {
    try {
        let url = `/data/range?limit=${limit}`;
        if (fromISO) url += `&from=${encodeURIComponent(fromISO)}`;
        if (toISO)   url += `&to=${encodeURIComponent(toISO)}`;
        const r = await fetch(url, { cache: 'no-store' });
        if (!r.ok) return [];
        return await r.json();
    } catch (_) {
        return [];
    }
}

/**
 * Vrati poslednich N paketu z DB (chronologicky od nejstarsiho)
 */
export async function getLastN(n = 300) {
    const rows = await getRange(null, null, n);
    return rows.reverse();   // DB vraci DESC, grafy chteji ASC
}

// Legacy stubs — zachovano pro kompatibilitu
export function setScenario(_name) { /* no-op */ }
export function getTripHistory()   { return []; }
export function clearTripHistory() { /* no-op — data jsou v DB */ }
export async function sendCmd(_cmd) { /* no-op — pres /command endpoint */ }