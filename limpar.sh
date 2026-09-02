#!/bin/bash
# Zera as tabelas antes de uma demonstração.
# RESTART IDENTITY faz os ids voltarem a 1 — sem isso eles
# continuariam de onde pararam (id 848, 849...), o que fica feio na tela.
set -e
cd "$(dirname "$0")"
docker compose exec -T postgres psql -U iot -d reservatorio \
  -c "TRUNCATE leituras, eventos RESTART IDENTITY;" \
  -c "SELECT (SELECT count(*) FROM leituras) AS leituras,
             (SELECT count(*) FROM eventos)  AS eventos;"
