"""
Coletor MQTT -> PostgreSQL

Assina o prefixo do projeto no Mosquitto LOCAL (que por sua vez
recebe tudo do broker público através da bridge) e grava:

  - telemetria (nivel/distancia/temperatura) na tabela "leituras",
    com amostragem para não gerar uma linha a cada 2 segundos;
  - mudanças de estado (alerta/bomba/status/confirmação) na
    tabela "eventos", sempre que chegam.

SQL escrito na mão de propósito: é um projeto acadêmico e as
queries precisam ficar visíveis.
"""

import logging
import os
import time

import paho.mqtt.client as mqtt
import psycopg

# ============================================================
# Configuração (injetada pelo docker-compose a partir do .env)
# ============================================================
MQTT_HOST = os.getenv("MQTT_HOST", "mosquitto")
MQTT_PORT = int(os.getenv("MQTT_PORT", "1883"))
PREFIXO = os.getenv("MQTT_PREFIXO", "catolicasc-g4/reservatorio").strip("/")

# Regra de amostragem das leituras
INTERVALO_S = float(os.getenv("AMOSTRAGEM_INTERVALO_S", "30"))
DELTA_NIVEL = float(os.getenv("AMOSTRAGEM_DELTA_NIVEL", "5"))

DSN = (
    f"host={os.getenv('POSTGRES_HOST', 'postgres')} "
    f"port={os.getenv('POSTGRES_PORT_INTERNO', '5432')} "
    f"dbname={os.getenv('POSTGRES_DB', 'reservatorio')} "
    f"user={os.getenv('POSTGRES_USER', 'iot')} "
    f"password={os.getenv('POSTGRES_PASSWORD', 'iot123')}"
)

logging.basicConfig(
    level=logging.INFO,
    format="%(asctime)s  %(levelname)-7s %(message)s",
    datefmt="%H:%M:%S",
)
log = logging.getLogger("coletor")

# ============================================================
# Estado em memória
#
# Os callbacks do paho rodam todos na mesma thread do loop, então
# não há concorrência aqui e não precisamos de lock.
# ============================================================
estado = {"nivel": None, "distancia": None, "temperatura": None, "bomba": None}

ultima_gravacao = 0.0      # timestamp (monotônico) da última linha em "leituras"
nivel_gravado = None       # nível daquela última linha

_conn = None               # conexão psycopg reaproveitada


# ============================================================
# PostgreSQL
# ============================================================
def conectar_banco():
    """Devolve uma conexão aberta, criando de novo se a anterior caiu."""
    global _conn
    if _conn is not None and not _conn.closed:
        return _conn
    _conn = psycopg.connect(DSN, autocommit=True)
    log.info("Conectado ao PostgreSQL")
    return _conn


def executar(sql, params):
    """
    Executa um INSERT. Se a conexão tiver caído, descarta e tenta
    mais uma vez — o coletor nunca deve morrer por causa do banco.
    """
    global _conn
    for tentativa in (1, 2):
        try:
            with conectar_banco().cursor() as cur:
                cur.execute(sql, params)
            return True
        except Exception as erro:
            log.warning("Falha no INSERT (tentativa %d/2): %s", tentativa, erro)
            try:
                if _conn is not None:
                    _conn.close()
            except Exception:
                pass
            _conn = None
            time.sleep(1)
    log.error("INSERT descartado após 2 tentativas")
    return False


def esperar_banco():
    """Segura a subida do coletor até o PostgreSQL aceitar conexão."""
    while True:
        try:
            conectar_banco()
            return
        except Exception as erro:
            log.info("PostgreSQL ainda não respondeu (%s), tentando de novo...", erro)
            time.sleep(2)


# ============================================================
# Gravações
# ============================================================
def gravar_leitura(motivo):
    global ultima_gravacao, nivel_gravado

    ok = executar(
        """
        INSERT INTO leituras (nivel, distancia, temperatura, bomba)
        VALUES (%s, %s, %s, %s)
        """,
        (estado["nivel"], estado["distancia"], estado["temperatura"], estado["bomba"]),
    )
    if not ok:
        return

    ultima_gravacao = time.monotonic()
    nivel_gravado = estado["nivel"]
    log.info(
        "LEITURA  nivel=%s%%  dist=%s cm  temp=%s C  bomba=%s  (%s)",
        estado["nivel"], estado["distancia"], estado["temperatura"],
        estado["bomba"], motivo,
    )


def gravar_evento(tipo, valor):
    ok = executar(
        "INSERT INTO eventos (tipo, valor, nivel) VALUES (%s, %s, %s)",
        (tipo, valor, estado["nivel"]),
    )
    if ok:
        log.info("EVENTO   %s = %s  (nivel=%s%%)", tipo, valor, estado["nivel"])


def avaliar_amostragem():
    """
    Decide se esta telemetria vira uma linha em "leituras".

    Chamada só quando chega o tópico 'nivel', que é o dado que
    governa a regra. Grava se:
      - é a primeira leitura da sessão;
      - passaram INTERVALO_S segundos desde a última gravação; ou
      - o nível variou mais que DELTA_NIVEL pontos percentuais.
    """
    agora = time.monotonic()

    if nivel_gravado is None:
        gravar_leitura("primeira leitura")
        return

    variacao = abs(estado["nivel"] - nivel_gravado)
    if variacao > DELTA_NIVEL:
        gravar_leitura(f"variacao de {variacao} p.p.")
        return

    if agora - ultima_gravacao >= INTERVALO_S:
        gravar_leitura("intervalo de amostragem")


# ============================================================
# MQTT
# ============================================================
def on_connect(client, userdata, flags, reason_code, properties=None):
    if reason_code == 0:
        topico = f"{PREFIXO}/#"
        client.subscribe(topico, qos=0)
        log.info("Conectado ao MQTT %s:%s — assinando %s", MQTT_HOST, MQTT_PORT, topico)
    else:
        log.warning("Conexão MQTT recusada: %s", reason_code)


def on_disconnect(client, userdata, flags, reason_code, properties=None):
    # O loop_forever() do paho reconecta sozinho; aqui só registramos.
    log.warning("Desconectado do MQTT (%s) — tentando reconectar...", reason_code)


def on_message(client, userdata, msg):
    """
    Roteia a mensagem pelo sufixo do tópico (o que vem depois do
    prefixo). Payload malformado é registrado e ignorado — o
    serviço não pode cair por causa de um número quebrado.
    """
    try:
        sufixo = msg.topic[len(PREFIXO) + 1:]
        payload = msg.payload.decode("utf-8", errors="replace").strip()

        if sufixo == "nivel":
            estado["nivel"] = int(float(payload))
            avaliar_amostragem()

        elif sufixo == "distancia":
            estado["distancia"] = float(payload)

        elif sufixo == "temperatura":
            estado["temperatura"] = float(payload)

        elif sufixo == "bomba":
            estado["bomba"] = payload
            gravar_evento("bomba", payload)

        elif sufixo == "status":
            gravar_evento("status", payload)

        elif sufixo == "alerta/nivel":
            gravar_evento("alerta", payload)

        elif sufixo == "comando/bomba/confirmacao":
            gravar_evento("confirmacao", payload)

        elif sufixo == "comando/bomba":
            # Comando de entrada do ESP32: quem registra é a confirmação.
            pass

        else:
            log.debug("Tópico ignorado: %s", msg.topic)

    except ValueError:
        log.warning("Payload malformado em %s: %r", msg.topic, msg.payload[:80])
    except Exception as erro:
        log.warning("Erro ao processar %s: %s", msg.topic, erro)


def main():
    log.info("Coletor iniciando — prefixo '%s'", PREFIXO)
    log.info("Amostragem: 1 linha a cada %.0fs, ou se o nível variar > %.0f p.p.",
             INTERVALO_S, DELTA_NIVEL)

    esperar_banco()

    client = mqtt.Client(mqtt.CallbackAPIVersion.VERSION2)
    client.on_connect = on_connect
    client.on_disconnect = on_disconnect
    client.on_message = on_message
    client.reconnect_delay_set(min_delay=1, max_delay=15)

    # connect_async + loop_forever: não trava se o broker ainda
    # não subiu, e reconecta sozinho se a conexão cair.
    client.connect_async(MQTT_HOST, MQTT_PORT, keepalive=60)
    client.loop_forever(retry_first_connection=True)


if __name__ == "__main__":
    main()
