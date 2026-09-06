# Reservatório IoT — camada de captura e persistência

Stack local que captura a telemetria publicada pelo ESP32 (rodando no Wokwi) e
grava em PostgreSQL.

```
ESP32 (Wokwi)  ──publica──>  HiveMQ Cloud (TLS 8883)
                                    │
                          bridge de saída (cliente)
                                    │
                                    v
                       Mosquitto local ──> coletor Python ──> PostgreSQL
```

O ponto central é a **bridge**: o Wokwi roda nos servidores da Wokwi e não
alcança esta máquina. Por isso o Mosquitto local é quem **conecta como cliente**
no broker remoto e espelha os tópicos para dentro. Não é preciso abrir porta no
roteador — o tráfego é todo de saída.

O espelhamento vai nos dois sentidos, mas a direção é declarada **por tópico**
em `mosquitto/mosquitto.conf`: telemetria só entra (`in`), comando só sai
(`out`). Não é um `topic # both` porque, com um broker remoto que não é
Mosquitto, uma mensagem publicada aqui sairia pela bridge e voltaria pela
assinatura dela mesma, sendo gravada duas vezes. Com a direção declarada por
tópico, nenhum é bidirecional e o eco não acontece.

## Serviços

| Serviço     | Imagem               | Porta no host | Papel                                  |
|-------------|----------------------|---------------|----------------------------------------|
| `mosquitto` | `eclipse-mosquitto:2`| 1883          | Broker local + bridge para o cluster    |
| `postgres`  | `postgres:16`        | 5432          | Persistência (volume `pgdata`)         |
| `collector` | build local          | —             | Assina o broker local, grava no banco  |
| `pgadmin`   | `dpage/pgadmin4:8.14`| 5050          | Interface web para inspecionar o banco |

## Como subir

```bash
cp .env.example .env
# confira MQTT_REMOTE_* e MQTT_BRIDGE_CLIENT_ID

# o pgAdmin monta este arquivo para entrar no banco sem pedir senha.
# Ele carrega a senha real, então não é versionado — gere a partir do .env:
cp pgadmin/pgpass.example pgadmin/pgpass
sed -i '' "s/TROQUE-PELA-SENHA-DO-ENV/$(grep '^POSTGRES_PASSWORD=' .env | cut -d= -f2)/" pgadmin/pgpass
chmod 600 pgadmin/pgpass

docker compose up -d --build
docker compose logs -f collector
```

Para derrubar mantendo os dados: `docker compose down`.
Para derrubar apagando o banco: `docker compose down -v`.

> O `db/init.sql` só roda quando o volume está vazio. Se alterar o schema,
> é preciso `docker compose down -v` para reaplicar.

## Verificando se a bridge está funcionando

**1. Log do Mosquitto** — deve aparecer a conexão com o broker remoto:

```bash
docker compose logs mosquitto | grep -i bridge
```

Procure por `Connecting bridge` seguido de `Bridge ... sending CONNECT`, sem
`Socket error` em loop.

**2. Tópico de estado da bridge** — `1` significa conectada:

```bash
docker compose exec mosquitto mosquitto_sub -h localhost \
  -t '$SYS/broker/connection/#' -v -C 1
```

Saída esperada:

```
$SYS/broker/connection/catolicasc-g4-bridge-a7f3/state 1
```

**3. Telemetria chegando de fora** — com o Wokwi rodando, as mensagens do ESP32
devem aparecer no broker *local*:

```bash
docker compose exec mosquitto mosquitto_sub -h localhost \
  -t 'catolicasc-g4/reservatorio/#' -v
```

Se aparecerem mensagens aqui, a bridge está trazendo os dados para dentro.

## Regra de amostragem

A tabela `leituras` **não** recebe uma linha a cada 2 s. O coletor grava quando:

- é a primeira leitura da sessão;
- passaram 30 s desde a última gravação (`AMOSTRAGEM_INTERVALO_S`); **ou**
- o nível variou mais de 5 pontos percentuais (`AMOSTRAGEM_DELTA_NIVEL`) —
  nesse caso grava imediatamente, para não perder o enchimento.

A tabela `eventos` recebe **toda** publicação em `alerta/nivel`, `bomba`,
`status` e `comando/bomba/confirmacao`.

## Visualizando o banco

Abra **http://localhost:5050**

| Campo | Valor |
|---|---|
| E-mail | `admin@reservatorio.com` |
| Senha | `admin123` |

Na barra lateral, expanda **Servers → Reservatorio → Databases → reservatorio
→ Schemas → public → Tables**. Clique com o botão direito em `leituras` ou
`eventos` e escolha **View/Edit Data → All Rows**.

O servidor já vem cadastrado e a senha do banco vem de um `pgpass` montado no
container, então não é preciso preencher host, porta nem senha. A primeira
subida do pgAdmin demora uns 20 segundos.

Para rodar SQL, use **Tools → Query Tool** e cole qualquer consulta da seção
seguinte.

> Isso é ferramenta de apoio, fora do caminho da captura. Se quiser subir o
> stack sem ela: `docker compose up -d postgres mosquitto collector`.

### Painel ao vivo

[painel-reservatorio.html](painel-reservatorio.html) — abra direto no navegador,
com dois cliques. Conecta por WebSocket no mesmo cluster que o ESP32 usa e mostra
nível, distância, temperatura, bomba e alerta em tempo real, além de um botão que
publica `LIGAR`/`PARAR` em `comando/bomba`.

É diferente do pgAdmin de propósito: o painel mostra o **agora** (a cada 2 s),
o banco mostra o **histórico** (amostrado a cada 30 s).

> As credenciais do cluster estão no próprio HTML, em texto claro. Serve para a
> demonstração local; não publique esse arquivo num repositório aberto.

### Views prontas

O banco já traz views para não precisar escrever SQL na apresentação. No
pgAdmin elas ficam em **Schemas → public → Views** — botão direito →
**View/Edit Data → All Rows**.

| View | O que mostra |
|---|---|
| `v_estado_atual` | Última leitura, com `ha_quanto_tempo` chegou — mostra se o ESP32 ainda publica |
| `v_historico_1h` | Série da última hora, formatada para virar gráfico |
| `v_eventos_hoje` | Eventos do dia, mais recentes primeiro |
| `v_bomba_janelas` | Cada acionamento como uma janela, com duração e origem (auto/manual) |
| `v_tempo_bomba` | Tempo total ligada e número de acionamentos |
| `v_resumo_diario` | Por dia: leituras, nível mín/máx/médio, temperatura média |

Estão em [db/views.sql](db/views.sql), aplicadas junto com o schema na primeira
subida. Como usam `CREATE OR REPLACE`, dá para editar e reaplicar sem perder
dados:

```bash
docker compose exec -T postgres psql -U iot -d reservatorio < db/views.sql
```

### Gráfico dentro do pgAdmin

O pgAdmin tem um plotador embutido, então dá para mostrar a curva de nível sem
Grafana:

1. **Tools → Query Tool**
2. Rode `SELECT * FROM v_historico_1h;`
3. Na área de resultados, abra a aba **Graph Visualiser**
4. Configure:
   - *Graph Type*: **Line Chart**
   - *X Axis*: `ts`
   - *Y Axis*: `nivel` — e marque também `bomba_ativa` para ver quando a bomba
     esteve ligada
5. Gere o gráfico

O `bomba_ativa` da view vale 0 ou 100 em vez de 0/1 justamente para caber na
mesma escala do nível: ele vira uma faixa que sobe enquanto a bomba trabalha.
O desenho esperado é um V — o nível caindo até o acionamento, e subindo depois.

**Preferindo o terminal**, o psql do container continua valendo:

```bash
docker compose exec postgres psql -U iot -d reservatorio
```

**Preferindo um app no Mac** (TablePlus, DBeaver), a porta 5432 está exposta:

| Campo | Valor |
|---|---|
| Host | `localhost` |
| Porta | `5432` |
| Database | `reservatorio` |
| Usuário | `iot` |
| Senha | `iot123` |

## Consultas úteis

No **Query Tool** do pgAdmin ou no psql.

**Última leitura**

```sql
SELECT ts, nivel, distancia, temperatura, bomba
FROM leituras
ORDER BY ts DESC
LIMIT 1;
```

**Histórico da última hora**

```sql
SELECT ts, nivel, distancia, temperatura, bomba
FROM leituras
WHERE ts > now() - INTERVAL '1 hour'
ORDER BY ts;
```

**Eventos de hoje**

```sql
SELECT ts, tipo, valor, nivel
FROM eventos
WHERE ts >= date_trunc('day', now())
ORDER BY ts;
```

**Tempo total com a bomba ligada**

Cada evento de bomba abre uma janela que termina no evento seguinte (ou agora,
se for o último). Somamos só as janelas em que a bomba estava ligada.

```sql
WITH janelas AS (
    SELECT valor,
           ts AS inicio,
           LEAD(ts, 1, now()) OVER (ORDER BY ts) AS fim
    FROM eventos
    WHERE tipo = 'bomba'
)
SELECT COALESCE(SUM(fim - inicio), INTERVAL '0') AS tempo_bomba_ligada
FROM janelas
WHERE valor LIKE 'LIGADA%';
```

## Testando sem o Wokwi rodando

O Mosquitto local entrega a qualquer assinante local o que é publicado nele,
então dá para exercitar todo o caminho coletor -> banco sem o ESP32.

**Publicar uma leitura de teste:**

```bash
docker compose exec mosquitto mosquitto_pub -h localhost \
  -t 'catolicasc-g4/reservatorio/nivel' -m '73'
```

**Confirmar que chegou no banco:**

```bash
docker compose exec postgres psql -U iot -d reservatorio \
  -c "SELECT ts, nivel, distancia, temperatura, bomba FROM leituras ORDER BY ts DESC LIMIT 1;"
```

Como `nivel` é declarado `in` na bridge, esta publicação de teste **não** sobe
para o cluster — ela fica na sua máquina.

**Testando o caminho de verdade (passando pelo cluster):**

Publique no HiveMQ Cloud em vez do broker local. É o mesmo caminho que a
telemetria do Wokwi percorre. Precisa de TLS e credenciais:

```bash
docker compose exec mosquitto mosquitto_pub \
  -h seu-cluster.s1.eu.hivemq.cloud -p 8883 \
  --cafile /etc/ssl/certs/ca-certificates.crt \
  -u esp32 -P 'SUA_SENHA' \
  -i teste-manual-01 -t 'catolicasc-g4/reservatorio/nivel' -m '41'
```

Depois rode a mesma consulta acima. Se a linha apareceu, a bridge está trazendo
os dados de fora — que é exatamente o que vai acontecer com o ESP32.

**Um ciclo completo, exercitando a amostragem e os eventos:**

```bash
P=catolicasc-g4/reservatorio
pub(){ docker compose exec -T mosquitto mosquitto_pub -h localhost -t "$P/$1" -m "$2"; }

pub temperatura 24.6
pub distancia 312.0
pub nivel 73          # primeira leitura -> grava
pub nivel 74          # +1 p.p. -> NAO grava (dentro do limiar)
pub nivel 82          # +9 p.p. -> grava na hora
pub alerta/nivel NIVEL_BAIXO
pub bomba LIGADA:AUTO
pub comando/bomba/confirmacao LIGAR:OK
```

Acompanhe com `docker compose logs -f collector`. O log diz o motivo de cada
gravação (`primeira leitura`, `variacao de 9 p.p.`, `intervalo de amostragem`).

## Broker: HiveMQ Cloud e o plano B

O projeto usa um cluster **privado** no HiveMQ Cloud, com TLS e credenciais.
Isso resolve dois problemas do broker público: ninguém mais publica no prefixo,
e não há disputa de client id com colegas de turma.

O cluster **não aceita conexão sem criptografia** — não existe porta 1883 lá.
Por isso o firmware usa `WiFiClientSecure` e a bridge recebe um `bridge_cafile`.

**Client id continua tendo que ser único.** São três clientes conectando:

| Quem | ID | Onde se define |
|---|---|---|
| ESP32 | `esp32-reserv-XXXXXX` | gerado do MAC, no `sketch.ino` |
| Bridge | `catolicasc-g4-bridge-a7f3` | `MQTT_BRIDGE_CLIENT_ID` no `.env` |
| Navegador | gerado sozinho | cliente web do HiveMQ |

**Plano B — voltar ao broker público em 30 segundos:**

```bash
cp .env.publico .env
docker compose up -d --force-recreate mosquitto collector
```

E no Wokwi, use o `sketch.ino.publico`. Vale manter isso à mão: se a internet
da sala bloquear a porta 8883, o broker público na 1883 costuma passar.

**Onde a senha aparece:** o repositório só carrega placeholders. O host e a
senha reais do cluster ficam em três lugares, todos fora do git:

| Lugar | Como preencher |
| --- | --- |
| `.env` | copie de `.env.example` e preencha `MQTT_REMOTE_HOST` / `MQTT_REMOTE_PASS` |
| `sketch.ino` | troque `BROKER` e `SENHA_MQTT` antes de subir no Wokwi |
| `painel-reservatorio.html` | troque `BROKER` e `SENHA` antes de abrir no navegador |

Os dois últimos não têm como evitar: o Wokwi e o navegador leem a credencial do
próprio arquivo. Se você compartilhar o projeto no Wokwi, a senha vai junto —
por isso use um cluster descartável e rotacione a credencial no painel do HiveMQ
quando terminar.

## Publicando um comando para a bomba

Publicado no broker local, a bridge leva até o ESP32:

```bash
docker compose exec mosquitto mosquitto_pub -h localhost \
  -t 'catolicasc-g4/reservatorio/comando/bomba' -m 'LIGAR'
```

A resposta do ESP32 chega em `comando/bomba/confirmacao` e é gravada em
`eventos` com `tipo = 'confirmacao'`.

## Trocando o broker ou o prefixo

Tudo sai do `.env`:

- `MQTT_PREFIXO` — usado pela bridge **e** pelo coletor;
- `MQTT_REMOTE_HOST` / `MQTT_REMOTE_PORT` — destino da bridge.

O `mosquitto/mosquitto.conf` é um template: os marcadores `__XXX__` são
substituídos por essas variáveis na subida do container (o Mosquitto não lê
variáveis de ambiente sozinho). Depois de mudar o `.env`:

```bash
docker compose up -d --force-recreate mosquitto collector
```

## Monitor serial: conexão e reconexão de Wi-Fi

Para uma captura limpa, compile a variante que realmente conecta no broker —
o `sketch.ino` versionado tem o broker placeholder e enche o log de tentativas
de MQTT falhando:

```bash
./compilar.sh sketch.ino.local
```

Todas as linhas do monitor serial saem carimbadas com `[data hora | uptime]`. A
data/hora vem de NTP (UTC−3), pedida logo depois do primeiro IP; enquanto o
relógio não sincronizou, o carimbo mostra `--/-- --:--:--` e só o uptime.

No boot o firmware registra a associação, o que o DHCP entregou e a qualidade do
sinal. O formato das linhas (os valores abaixo são ilustrativos):

```
[--/-- --:--:-- | up 00:00:00] ===== Reservatorio IoT -- firmware iniciado =====
[--/-- --:--:-- | up 00:00:00] Wi-Fi: associando ao SSID "Wokwi-GUEST" e pedindo endereco por DHCP...
[--/-- --:--:-- | up 00:00:02] Wi-Fi CONECTADO -- SSID=Wokwi-GUEST canal=6 RSSI=-58 dBm (boa)
[--/-- --:--:-- | up 00:00:02] DHCP entregou: IP=10.13.37.2 mascara=255.255.255.0 gateway=10.13.37.1 DNS=8.8.8.8
[05/09 15:20:31 | up 00:00:03] NTP: relogio sincronizado -- as linhas acima tinham so o uptime
```

De 10 em 10 segundos sai um batimento `ATIVO` com IP e RSSI, que serve de prova
de que a conexão continua de pé entre um evento e outro.

### Comandos digitados no monitor serial

| Comando  | O que faz                                                       |
|----------|-----------------------------------------------------------------|
| `QUEDA`  | Derruba o Wi-Fi de propósito para testar a reconexão            |
| `STATUS` | Imprime IP, RSSI, estado do MQTT, nível e contador de quedas    |
| `LIGAR`  | Liga a bomba manualmente (mesmo caminho do comando via MQTT)    |
| `PARAR`  | Para a bomba manualmente                                        |

`QUEDA` existe justamente para a evidência: provoca a perda **sem editar o
código e sem upload novo**, então tudo que acontece depois é do firmware.

```
[05/09 15:20:41 | up 00:00:13] ATIVO -- IP=10.13.37.2 RSSI=-58 dBm (boa) | MQTT=conectado | ... | quedas=0
[05/09 15:20:45 | up 00:00:17] COMANDO "QUEDA": derrubando o Wi-Fi de proposito para testar a reconexao
[05/09 15:20:45 | up 00:00:17] Wi-Fi PERDIDO (queda #1) -- desconexao provocada pelo comando QUEDA no monitor serial
[05/09 15:20:46 | up 00:00:18] sem Wi-Fi ha 1.0 s (status=DESCONECTADO) -- o ESP32 esta tentando voltar sozinho
[05/09 15:20:46 | up 00:00:18] Wi-Fi: tentativa #2 de reconexao (proxima em 1 s se esta falhar)
[05/09 15:20:48 | up 00:00:20] Wi-Fi CONECTADO -- SSID=Wokwi-GUEST canal=6 RSSI=-57 dBm (boa)
[05/09 15:20:48 | up 00:00:20] DHCP entregou: IP=10.13.37.2 ...
[05/09 15:20:48 | up 00:00:20] RECONEXAO AUTOMATICA concluida em 3.1 s apos a queda #1, sem mexer no codigo e sem upload novo
```

### Onde fica a lógica

`supervisionarWifi()`, no `sketch.ino`, chamada a cada volta do `loop()`. Ela
roda a cada 250 ms e **não bloqueia**: enquanto a rede não volta, o sensor, o
LCD e o controle da bomba seguem funcionando. A cada falha a espera até a
próxima tentativa dobra (1 s → 2 s → 4 s … teto de 30 s), para não inundar o ar
com pedidos de associação enquanto o ponto de acesso está fora.

O `WiFi.setAutoReconnect(false)` é proposital: a reconexão automática do próprio
stack do ESP32 ignora desconexões pedidas pelo software (motivo `ASSOC_LEAVE`),
que é exatamente o caso do comando `QUEDA`. Com um mecanismo só, no firmware, o
comportamento é o mesmo para queda real e queda provocada — e cada passo fica
registrado no serial.
