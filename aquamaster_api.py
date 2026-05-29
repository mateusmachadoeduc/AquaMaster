# ============================================================
# AQUA MASTER - Backend Python
# API Flask + Banco de Dados SQLite
# ============================================================
# COMO RODAR:
#   pip install flask
#   python aquamaster_api.py
#
# A API fica disponivel em: http://0.0.0.0:5000
# Para acesso externo: ngrok http 5000
# ============================================================

from flask import Flask, request, jsonify, send_from_directory
import sqlite3
import os
import re
from datetime import datetime, timedelta

app = Flask(__name__, static_folder=".")

# ── Serve o dashboard na raiz ──────────────────────────────────
@app.route("/")
def index():
    return send_from_directory(".", "dashboard.html")

DB_FILE = "aquamaster.db"

# ── CORS ──────────────────────────────────────────────────────
# Injeta headers CORS em TODAS as respostas (GET, POST, OPTIONS).
# Necessario para que o dashboard funcione em file://, Live Server
# (porta 5500) ou GitHub Pages sem erros de bloqueio.
@app.after_request
def add_cors(response):
    response.headers["Access-Control-Allow-Origin"]  = "*"
    response.headers["Access-Control-Allow-Methods"] = "GET, POST, OPTIONS"
    response.headers["Access-Control-Allow-Headers"] = "Content-Type"
    return response

@app.route("/", defaults={"path": ""}, methods=["OPTIONS"])
@app.route("/<path:path>",             methods=["OPTIONS"])
def preflight(path):
    return "", 200


# ============================================================
# BANCO DE DADOS
# ============================================================
def init_db():
    conn = sqlite3.connect(DB_FILE)
    c = conn.cursor()

    # Leituras dos sensores
    c.execute("""
        CREATE TABLE IF NOT EXISTS leituras (
            id           INTEGER PRIMARY KEY AUTOINCREMENT,
            timestamp    TEXT NOT NULL,
            temperatura  REAL,
            ph           REAL,
            bomba        INTEGER,
            estado_alim  TEXT,
            dia_atual    INTEGER,
            alimentacoes INTEGER,
            ciclos       INTEGER,
            servo_pos    INTEGER DEFAULT 0
        )
    """)

    # Log de eventos
    c.execute("""
        CREATE TABLE IF NOT EXISTS eventos (
            id        INTEGER PRIMARY KEY AUTOINCREMENT,
            timestamp TEXT NOT NULL,
            tipo      TEXT,
            descricao TEXT
        )
    """)

    # ── Tabela de configuracao / estado compartilhado ──────────
    # Usada para guardar o comando pendente no banco de dados,
    # evitando o problema de variavel global com o reloader do Flask
    # que pode subir dois processos com memorias separadas.
    c.execute("""
        CREATE TABLE IF NOT EXISTS config (
            chave TEXT PRIMARY KEY,
            valor TEXT
        )
    """)
    # Garante que a linha do comando pendente existe
    c.execute("""
        INSERT OR IGNORE INTO config (chave, valor)
        VALUES ('comando_pendente', NULL)
    """)

    # ── Migração: adiciona servo_pos se a tabela já existia sem ela ──
    try:
        c.execute("ALTER TABLE leituras ADD COLUMN servo_pos INTEGER DEFAULT 0")
        print("[DB] Migração: coluna servo_pos adicionada.")
    except sqlite3.OperationalError:
        pass  # Coluna já existe — tudo certo

    conn.commit()
    conn.close()
    print(f"[DB] Banco pronto: {os.path.abspath(DB_FILE)}")


def get_conn():
    return sqlite3.connect(DB_FILE)


def ler_comando_pendente(conn):
    """Le e LIMPA o comando pendente atomicamente."""
    c = conn.cursor()
    c.execute("SELECT valor FROM config WHERE chave = 'comando_pendente'")
    row = c.fetchone()
    cmd = row[0] if row and row[0] else None
    # Limpa imediatamente para nao reenviar
    c.execute("UPDATE config SET valor = NULL WHERE chave = 'comando_pendente'")
    conn.commit()
    return cmd


def gravar_comando_pendente(cmd):
    """Grava o comando pendente no banco."""
    conn = get_conn()
    conn.execute(
        "UPDATE config SET valor = ? WHERE chave = 'comando_pendente'",
        (cmd,)
    )
    conn.commit()
    conn.close()


def log_evento(tipo, descricao):
    conn = get_conn()
    conn.execute(
        "INSERT INTO eventos (timestamp, tipo, descricao) VALUES (?, ?, ?)",
        (datetime.now().isoformat(), tipo, descricao)
    )
    conn.commit()
    conn.close()


# ============================================================
# ENDPOINT: POST /dados  (chamado pelo ESP32 a cada 5s)
# Salva leitura + responde com comando pendente (se houver)
# ============================================================
@app.route("/dados", methods=["POST"])
def receber_dados():
    data = request.get_json(silent=True)
    if not data:
        return jsonify({"status": "erro", "msg": "JSON invalido"}), 400

    conn = get_conn()
    c = conn.cursor()

    # Salva leitura (incluindo posicao do servo)
    c.execute("""
        INSERT INTO leituras
            (timestamp, temperatura, ph, bomba, estado_alim, dia_atual, alimentacoes, ciclos, servo_pos)
        VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?)
    """, (
        datetime.now().isoformat(),
        data.get("temperatura"),
        data.get("ph"),
        1 if data.get("bomba") else 0,
        data.get("estado", "desconhecido"),
        data.get("diaAtual"),
        data.get("alimentacoes"),
        data.get("ciclos"),
        data.get("posicaoServo", 0),
    ))

    # Le e limpa comando pendente (mesma conexao = atomico)
    cmd = ler_comando_pendente(conn)
    conn.close()

    if cmd:
        print(f"[ESP32] Entregando comando: {cmd}")

    # Resposta compacta e sem espacos ao redor de ':' e ','
    # para facilitar o parse manual no ESP32
    cmd_json = f'"{cmd}"' if cmd else "null"
    return app.response_class(
        response=f'{{"status":"ok","comando":{cmd_json}}}',
        status=200,
        mimetype="application/json"
    )


# ============================================================
# ENDPOINT: GET /status  (ultima leitura para os cards)
# ============================================================
@app.route("/status", methods=["GET"])
def status_atual():
    conn = get_conn()
    c = conn.cursor()
    c.execute("SELECT * FROM leituras ORDER BY id DESC LIMIT 1")
    row = c.fetchone()
    conn.close()

    if not row:
        return jsonify({"erro": "Nenhum dado ainda"}), 404

    return jsonify({
        "id":           row[0],
        "timestamp":    row[1],
        "temperatura":  row[2],
        "ph":           row[3],
        "bomba":        bool(row[4]),
        "estado_alim":  row[5],
        "dia_atual":    row[6],
        "alimentacoes": row[7],
        "ciclos":       row[8],
        "servo_pos":    row[9] if len(row) > 9 else 0,
    })


# ============================================================
# ENDPOINT: GET /historico  (historico para os graficos)
# Query: ?limite=200&horas=24
# ============================================================
@app.route("/historico", methods=["GET"])
def historico():
    limite = request.args.get("limite", 800,  type=int)
    horas  = request.args.get("horas",  None, type=int)
    step   = max(1, min(request.args.get("step", 1, type=int), 200))

    conn = get_conn()
    c = conn.cursor()

    if horas:
        # Usa datetime Python para garantir mesmo formato ISO do timestamp salvo.
        # datetime('now','localtime') do SQLite usa espaco; isoformat() usa 'T'.
        # Comparacao de string falharia silenciosamente sem este fix.
        cutoff = (datetime.now() - timedelta(hours=horas)).isoformat()
        c.execute("""
            SELECT * FROM leituras
            WHERE timestamp >= ?
            ORDER BY id ASC
            LIMIT 50000
        """, (cutoff,))
    else:
        c.execute("SELECT * FROM leituras ORDER BY id ASC LIMIT 50000")

    rows = c.fetchall()
    conn.close()

    # Downsampling: pega 1 a cada `step` linhas para reduzir volume
    if step > 1:
        rows = rows[::step]

    # Aplica limite final (mantem os mais recentes)
    if len(rows) > limite:
        rows = rows[-limite:]

    resultado = [{
        "id":           r[0], "timestamp":    r[1],
        "temperatura":  r[2], "ph":           r[3],
        "bomba":        bool(r[4]), "estado_alim":  r[5],
        "dia_atual":    r[6], "alimentacoes": r[7], "ciclos": r[8],
        "servo_pos":    r[9] if len(r) > 9 else 0,
    } for r in rows]

    return jsonify(resultado)


# ============================================================
# ENDPOINT: GET ou POST /comando  (dashboard → ESP32)
#
# POST body JSON:  { "comando": "bomba_ligar" }
# GET query:       /comando?cmd=bomba_ligar
#
# Comandos validos:
#   bomba_ligar | bomba_desligar | bomba_auto
#   alimentar_agora | iniciar_ciclo | ir_home
# ============================================================
COMANDOS_FIXOS = [
    "bomba_ligar", "bomba_desligar", "bomba_auto",
    "alimentar_agora", "iniciar_ciclo", "ir_home",
]

def validar_comando(cmd):
    """Retorna True se o comando e valido."""
    if cmd in COMANDOS_FIXOS:
        return True

    # servo_0 ate servo_180 — movimento manual pontual
    m = re.fullmatch(r'servo_(\d{1,3})', cmd)
    if m:
        return 0 <= int(m.group(1)) <= 180

    # set_posicoes_D1_D2_D3_D4_D5_D6_D7 — atualiza array do ESP32
    m2 = re.fullmatch(
        r'set_posicoes_(\d{1,3})_(\d{1,3})_(\d{1,3})_(\d{1,3})_(\d{1,3})_(\d{1,3})_(\d{1,3})',
        cmd
    )
    if m2:
        return all(0 <= int(m2.group(i)) <= 180 for i in range(1, 8))

    return False

@app.route("/comando", methods=["GET", "POST"])
def enviar_comando():
    if request.method == "GET":
        cmd = request.args.get("cmd", "").strip()
    else:
        body = request.get_json(silent=True) or {}
        cmd  = body.get("comando", "").strip()

    if not cmd:
        return jsonify({"status": "erro", "msg": "Comando ausente"}), 400

    if not validar_comando(cmd):
        return jsonify({
            "status": "erro",
            "msg": f"Invalido. Use: {', '.join(COMANDOS_FIXOS)} ou servo_0..180",
        }), 400

    gravar_comando_pendente(cmd)
    log_evento("COMANDO", f"Agendado: {cmd}")
    print(f"[CMD] Gravado no banco: {cmd}")

    return jsonify({
        "status": "ok",
        "msg": f"Comando '{cmd}' sera entregue ao ESP32 em ate 5s",
    })


# ============================================================
# ENDPOINT: GET /eventos  (log para debug)
# ============================================================
@app.route("/eventos", methods=["GET"])
def listar_eventos():
    limite = request.args.get("limite", 50, type=int)
    conn = get_conn()
    c = conn.cursor()
    c.execute("SELECT * FROM eventos ORDER BY id DESC LIMIT ?", (limite,))
    rows = c.fetchall()
    conn.close()
    return jsonify([
        {"id": r[0], "timestamp": r[1], "tipo": r[2], "descricao": r[3]}
        for r in rows
    ])


# ============================================================
# ENDPOINT: GET /ping
# ============================================================
@app.route("/ping", methods=["GET"])
def ping():
    return jsonify({"status": "ok", "msg": "AquaMaster online"})


# ============================================================
# MAIN
# ============================================================
if __name__ == "__main__":
    print("=" * 50)
    print("  AQUA MASTER - Backend Python")
    print("=" * 50)
    init_db()
    print()
    print("[INFO] Endpoints:")
    print("  POST /dados        ← ESP32 envia dados")
    print("  GET  /status       ← leitura atual")
    print("  GET  /historico    ← historico graficos")
    print("  POST /comando      ← dashboard → ESP32")
    print("  GET  /eventos      ← log de eventos")
    print("  GET  /ping         ← verificar se esta no ar")
    print()
    print("[INFO] Acesso externo: ngrok http 5000")
    print("=" * 50)

    # use_reloader=False evita subir dois processos com memorias separadas.
    # threaded=True permite atender ESP32 e dashboard ao mesmo tempo.
    # PORT: variavel de ambiente usada pelo Render (e outros hostings).
    port = int(os.environ.get("PORT", 5000))
    app.run(host="0.0.0.0", port=port, debug=True,
            use_reloader=False, threaded=True)
