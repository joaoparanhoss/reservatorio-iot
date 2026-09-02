-- ============================================================
-- Views de apoio à visualização.
--
-- Ficam visíveis na árvore do pgAdmin em
--   Schemas > public > Views
-- e podem ser abertas com botão direito > View/Edit Data.
--
-- Rodam depois do init.sql na primeira subida (ordem
-- alfabética do /docker-entrypoint-initdb.d). Como usam
-- CREATE OR REPLACE, também podem ser reaplicadas a qualquer
-- momento sem perder dados:
--   docker compose exec -T postgres psql -U iot -d reservatorio < db/views.sql
-- ============================================================

-- ------------------------------------------------------------
-- Estado atual do reservatório: só a última leitura, com a
-- idade dela — útil para saber se o ESP32 ainda está publicando.
-- ------------------------------------------------------------
CREATE OR REPLACE VIEW v_estado_atual AS
SELECT ts,
       nivel,
       distancia,
       temperatura,
       bomba,
       now() - ts AS ha_quanto_tempo
FROM leituras
ORDER BY ts DESC
LIMIT 1;

-- ------------------------------------------------------------
-- Série da última hora, no formato que o Graph Visualiser do
-- pgAdmin espera: eixo X = ts, séries numéricas nas colunas.
--
-- bomba_ativa vale 0 ou 100 (em vez de 0/1) para poder ser
-- plotada no mesmo eixo do nível, virando uma faixa que mostra
-- quando a bomba estava ligada.
-- ------------------------------------------------------------
CREATE OR REPLACE VIEW v_historico_1h AS
SELECT ts,
       nivel,
       distancia,
       temperatura,
       CASE WHEN bomba LIKE 'LIGADA%' THEN 100 ELSE 0 END AS bomba_ativa
FROM leituras
WHERE ts > now() - INTERVAL '1 hour'
ORDER BY ts;

-- ------------------------------------------------------------
-- Eventos do dia, mais recentes primeiro.
-- ------------------------------------------------------------
CREATE OR REPLACE VIEW v_eventos_hoje AS
SELECT ts,
       tipo,
       valor,
       nivel
FROM eventos
WHERE ts >= date_trunc('day', now())
ORDER BY ts DESC;

-- ------------------------------------------------------------
-- Cada acionamento da bomba como uma janela com duração.
-- A janela termina no evento de bomba seguinte, ou agora, se
-- for o último.
-- ------------------------------------------------------------
CREATE OR REPLACE VIEW v_bomba_janelas AS
WITH janelas AS (
    SELECT valor,
           ts AS inicio,
           LEAD(ts, 1, now()) OVER (ORDER BY ts) AS fim
    FROM eventos
    WHERE tipo = 'bomba'
)
SELECT inicio,
       fim,
       fim - inicio AS duracao,
       CASE
           WHEN valor LIKE 'LIGADA:AUTO%'   THEN 'automatico'
           WHEN valor LIKE 'LIGADA:MANUAL%' THEN 'manual'
           ELSE 'parada'
       END AS origem,
       valor
FROM janelas
ORDER BY inicio DESC;

-- ------------------------------------------------------------
-- Tempo total com a bomba ligada, somando as janelas.
-- ------------------------------------------------------------
CREATE OR REPLACE VIEW v_tempo_bomba AS
SELECT COALESCE(SUM(duracao), INTERVAL '0') AS tempo_total_ligada,
       count(*)                             AS acionamentos
FROM v_bomba_janelas
WHERE origem <> 'parada';

-- ------------------------------------------------------------
-- Um resumo por dia, para a visão geral da coleta.
-- ------------------------------------------------------------
CREATE OR REPLACE VIEW v_resumo_diario AS
SELECT date_trunc('day', ts)::date          AS dia,
       count(*)                             AS leituras,
       min(nivel)                           AS nivel_min,
       max(nivel)                           AS nivel_max,
       round(avg(nivel), 1)                 AS nivel_medio,
       round(avg(temperatura)::numeric, 1)  AS temp_media
FROM leituras
GROUP BY 1
ORDER BY 1 DESC;
