from flask import Flask, request, jsonify, send_from_directory
import psycopg
import psycopg.rows
import json
import os

app = Flask(__name__)

# ============================================================================
#  KONFIGURACE
# ============================================================================
DATABASE_URL = os.environ.get("DATABASE_URL") or os.environ.get("POSTGRES_URL") or os.environ.get("PG_URL")
print(f"ENV vars: {list(os.environ.keys())}", flush=True)
print(f"ALL ENV: {[k for k in os.environ.keys() if 'DATA' in k or 'POST' in k or 'PG' in k or 'DB' in k]}", flush=True)
SECRET_TOKEN  = os.environ.get("SECRET_TOKEN")
DASHBOARD_DIR = "dashboard"

# RAM state
last_nonce  = 0
state_cache = {}   # posledni znamy stav vsech PIDu (merge pres fast/full pakety)


# ============================================================================
#  DATABAZE
# ============================================================================
def db_conn():
    return psycopg.connect(DATABASE_URL)


def init_db():
    if not DATABASE_URL:
        print("CHYBA: Chybí DATABASE_URL! Databáze se neinicializuje.", flush=True)
        return
    
    try:
        print("Pokus o připojení k DB a vytvoření tabulek...", flush=True)
        con = db_conn()
        cur = con.cursor()
        cur.execute("""
            CREATE TABLE IF NOT EXISTS packets (
                id        SERIAL PRIMARY KEY,
                timestamp TIMESTAMP DEFAULT CURRENT_TIMESTAMP,
                data      JSONB,
                event     TEXT
            )
        """)
        cur.execute("""
            CREATE TABLE IF NOT EXISTS commands (
                id        SERIAL PRIMARY KEY,
                timestamp TIMESTAMP DEFAULT CURRENT_TIMESTAMP,
                cmd       TEXT,
                executed  BOOLEAN DEFAULT FALSE
            )
        """)
        cur.execute("CREATE INDEX IF NOT EXISTS idx_packets_ts ON packets(timestamp DESC)")
        con.commit()
        cur.close()
        con.close()
        print("DB inicializována úspěšně.", flush=True)
    except Exception as e:
        print(f"Kritická chyba DB při startu: {e}", flush=True)


def check_auth(req):
    if SECRET_TOKEN is None:
        return False
    return req.headers.get("Authorization", "") == f"Bearer {SECRET_TOKEN}"


# ============================================================================
#  ESP -> SERVER
# ============================================================================
@app.route("/data", methods=["POST"])
def receive_data():
    global last_nonce, state_cache

    print("--- PŘÍCHOZÍ REQUEST ---", flush=True)
    print(f"Surová data: {request.get_data(as_text=True)}", flush=True)

    if not check_auth(request):
        return jsonify({"error": "unauthorized"}), 401

    data = request.get_json(silent=True)
    if not data:
        return jsonify({"status": "error", "msg": "no JSON"}), 400

    # Robustni nonce parsing — string, float, missing key vse OK
    try:
        nonce = int(data.get("nonce", 0))
    except (ValueError, TypeError):
        nonce = 0

    # Replay check — ale tolerantni vuci ESP rebootu (bootCount jde dolu)
    # Pokud novy nonce je o vic nez 1e9 mensi nez last_nonce, povazuje to za reboot
    if nonce > 0:
        if nonce <= last_nonce:
            if last_nonce - nonce > 1_000_000_000:
                # ESP se rebootlo s nizsim bootCount — reset last_nonce
                print(f"[NONCE] ESP reboot detected ({last_nonce} -> {nonce}), resetting", flush=True)
                last_nonce = nonce
            else:
                return jsonify({"error": "replay detected", "last": last_nonce}), 400
        else:
            last_nonce = nonce

    event = data.get("event")

    # Mergne do cache jen "uzitecne" klice — bez nonce/type ktere by zaplnily dashboard

    SKIP_KEYS = {"nonce", "type"}
    pkt_type = data.get("type", "full")
    for k, v in data.items():
        if k not in SKIP_KEYS:
            # Fast paket nepřepíše hodnoty které uz mame z full paketu
            if pkt_type == "fast" or k not in state_cache:
                state_cache[k] = v
            elif pkt_type == "full":
                state_cache[k] = v
            
    # Vzdy doplnit timestamp — dashboard si pak muze ukazat "stale data"
    from datetime import datetime, timezone
    state_cache["_server_ts"] = datetime.now(timezone.utc).isoformat()

    # DB I/O obalena — i kdyby Postgres timeoutoval, ESP dostane 200
    pending = None
    try:
        con = db_conn()
        cur = con.cursor()
        cur.execute(
            "INSERT INTO packets (data, event) VALUES (%s, %s)",
            (json.dumps(data), event),
        )
        cur.execute(
            "SELECT id, cmd FROM commands WHERE executed = FALSE ORDER BY id ASC LIMIT 1"
        )
        pending = cur.fetchone()
        if pending:
            cur.execute("UPDATE commands SET executed = TRUE WHERE id = %s", (pending[0],))
        con.commit()
        cur.close()
        con.close()
    except Exception as e:
        print(f"[DB] chyba pri zapisu/cteni: {e}", flush=True)
        # Stale vratime 200, aby ESP nezacalo hardresetovat modem kvuli DB problemu

    response = {"status": "ok"}
    if pending:
        response["cmd"] = pending[1]
    return jsonify(response)
# ============================================================================
#  DASHBOARD -> SERVER (cteni dat)
# ============================================================================
@app.route("/data", methods=["GET"])
def get_data():
    """Vrati poslednich N paketu (default 100, max 10000)."""
    limit = min(int(request.args.get("limit", 100)), 10000)
    con = db_conn()
    cur = con.cursor(row_factory=psycopg.rows.dict_row)
    cur.execute(
        "SELECT timestamp, data, event FROM packets ORDER BY id DESC LIMIT %s",
        (limit,),
    )
    rows = cur.fetchall()
    cur.close()
    con.close()
    out = []
    for r in rows:
        d = dict(r["data"]) if r["data"] else {}
        d["timestamp"] = r["timestamp"].isoformat()
        if r["event"]:
            d["event"] = r["event"]
        out.append(d)
    return jsonify(out)



@app.route("/data/latest", methods=["GET"])
def get_latest():
    if not state_cache:
        return jsonify({})

    ts = state_cache.get("_server_ts")
    if ts:
        try:
            from datetime import datetime, timezone
            age = datetime.now(timezone.utc) - datetime.fromisoformat(ts)
            if age.total_seconds() > 10:
                return jsonify({})
        except Exception:
            pass

    return jsonify(state_cache)


@app.route("/data/range", methods=["GET"])
def get_range():
    """Vrati pakety mezi 'from' a 'to' (ISO timestampy). Limit 10000."""
    f = request.args.get("from")
    t = request.args.get("to")
    limit = min(int(request.args.get("limit", 10000)), 10000)
    con = db_conn()
    cur = con.cursor(row_factory=psycopg.rows.dict_row)
    if f and t:
        cur.execute(
            "SELECT timestamp, data, event FROM packets "
            "WHERE timestamp BETWEEN %s AND %s ORDER BY id DESC LIMIT %s",
            (f, t, limit),
        )
    else:
        cur.execute(
            "SELECT timestamp, data, event FROM packets ORDER BY id DESC LIMIT %s",
            (limit,),
        )
    rows = cur.fetchall()
    cur.close()
    con.close()
    out = []
    for r in rows:
        d = dict(r["data"]) if r["data"] else {}
        d["timestamp"] = r["timestamp"].isoformat()
        if r["event"]:
            d["event"] = r["event"]
        out.append(d)
    return jsonify(out)


@app.route("/stats", methods=["GET"])
def get_stats():
    con = db_conn()
    cur = con.cursor()
    cur.execute("SELECT COUNT(*), MAX(timestamp) FROM packets")
    row = cur.fetchone()
    cur.close()
    con.close()
    return jsonify({
        "total_packets": row[0],
        "last_packet":   row[1].isoformat() if row[1] else None,
        "last_nonce":    last_nonce,
        "cache_keys":    len(state_cache),
    })


# ============================================================================
#  PRIKAZY ZE DASHBOARDU -> ESP
# ============================================================================
@app.route("/command", methods=["POST"])
def send_command():
    if not check_auth(request):
        return jsonify({"error": "unauthorized"}), 401
    data = request.get_json(silent=True)
    if not data or "cmd" not in data:
        return jsonify({"status": "error", "msg": "no cmd"}), 400
    con = db_conn()
    cur = con.cursor()
    cur.execute("INSERT INTO commands (cmd) VALUES (%s)", (data["cmd"],))
    con.commit()
    cur.close()
    con.close()
    return jsonify({"status": "ok"})


# ============================================================================
#  DASHBOARD STATIC FILES
# ============================================================================
@app.route("/")
def dashboard_root():
    return send_from_directory(DASHBOARD_DIR, "dashboard.html")


@app.route("/<path:filename>")
def dashboard_file(filename):
    return send_from_directory(DASHBOARD_DIR, filename)


@app.route("/health")
def health():
    return "OK"


# ============================================================================
init_db()
if __name__ == "__main__":
    port = int(os.environ.get("PORT", 5000))
    app.run(host="0.0.0.0", port=port, debug=False)