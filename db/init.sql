-- ============================================================
-- Schema do medidor de nível de reservatório
-- Executado automaticamente pelo PostgreSQL na PRIMEIRA subida
-- do container (diretório /docker-entrypoint-initdb.d).
--
-- Para reaplicar depois de alterar este arquivo:
--   docker compose down -v && docker compose up -d
-- (o -v apaga o volume, ou seja, apaga os dados)
-- ============================================================

-- Exibe TIMESTAMPTZ no horário local em vez de UTC. Não muda o
-- que é gravado (TIMESTAMPTZ guarda sempre o instante absoluto),
-- só como ele aparece nas consultas e no pgAdmin.
ALTER DATABASE reservatorio SET timezone = 'America/Sao_Paulo';

-- ------------------------------------------------------------
-- leituras: série temporal da telemetria.
-- Não recebe uma linha a cada 2 s — o coletor aplica amostragem
-- (uma linha a cada 30 s, ou na hora se o nível variar > 5 p.p.).
-- ------------------------------------------------------------
CREATE TABLE leituras (
    id          BIGSERIAL PRIMARY KEY,
    ts          TIMESTAMPTZ  NOT NULL DEFAULT now(),
    nivel       INTEGER,              -- percentual, 0-100
    distancia   REAL,                 -- cm, sensor ultrassônico
    temperatura REAL,                 -- °C
    bomba       TEXT                  -- último estado conhecido da bomba
);

CREATE INDEX idx_leituras_ts ON leituras (ts DESC);

-- ------------------------------------------------------------
-- eventos: toda mudança de estado publicada pelo ESP32.
-- tipo   -> de qual tópico veio ('alerta', 'bomba', 'status',
--           'confirmacao')
-- valor  -> payload cru ('NIVEL_BAIXO', 'LIGADA:AUTO', 'online'...)
-- nivel  -> nível do reservatório no instante do evento, para
--           dar contexto sem precisar de JOIN por timestamp
-- ------------------------------------------------------------
CREATE TABLE eventos (
    id     BIGSERIAL PRIMARY KEY,
    ts     TIMESTAMPTZ NOT NULL DEFAULT now(),
    tipo   TEXT        NOT NULL,
    valor  TEXT,
    nivel  INTEGER
);

CREATE INDEX idx_eventos_ts ON eventos (ts DESC);
