#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <PubSubClient.h>
#include <Wire.h>
#include <LiquidCrystal_I2C.h>
#include <OneWire.h>
#include <DallasTemperature.h>
#include <time.h>
#include <stdarg.h>

// ---------- pinos ----------
#define PIN_TRIG    5
#define PIN_ECHO    34
#define PIN_TEMP    4
#define PIN_LED     26
#define PIN_BUZZER  27

// ---------- calibracao ----------
const float D_VAZIO  = 400.0;  // cm do sensor ate o fundo
const float D_CHEIO  = 2.0;    // cm do sensor ate o nivel maximo
const float ALFA_EMA = 0.30;

// ---------- limiares ----------
const int NIVEL_LIGA     = 10;
const int NIVEL_DESLIGA  = 90;
const int NIVEL_BAIXO    = 15;
const int NIVEL_CRITICO  = 95;

// ---------- rede ----------
const char* SSID    = "Wokwi-GUEST";
const char* SENHA   = "";
const char* BROKER  = "seu-cluster.s1.eu.hivemq.cloud";
const int   PORTA   = 8883;
const char* USUARIO = "esp32";
const char* SENHA_MQTT = "troque-aqui";
const char* PREFIXO = "catolicasc-g4/reservatorio";

// ---------- wi-fi: reconexao e relogio ----------
// Espera entre tentativas de reconexao. Dobra a cada falha ate o teto para
// nao inundar o ar com pedidos de associacao enquanto o AP esta fora.
const unsigned long WIFI_ESPERA_INICIAL = 5000;
const unsigned long WIFI_ESPERA_MAXIMA  = 30000;
const long NTP_FUSO = -3 * 3600;   // horario de Brasilia (UTC-3)

// WiFiClientSecure no lugar de WiFiClient: o cluster exige TLS.
WiFiClientSecure net;
PubSubClient mqtt(net);
LiquidCrystal_I2C lcd(0x27, 16, 2);
OneWire fio(PIN_TEMP);
DallasTemperature sensorTemp(&fio);

// ---------- estado ----------
float buf[5]; uint8_t bufIdx = 0; bool bufCheio = false;
float distFiltrada = -1, temperatura = 25.0;
int nivel = 0;
bool bombaLigada = false, origemAuto = false, inibido = false;
String alerta = "NORMAL";
unsigned long tLeitura = 0, tLcd = 0, tPub = 0, tTemp = 0, tMqtt = 0;
unsigned long backoff = 1000;

// ---------- estado da rede ----------
bool wifiConectado = false, horaSincronizada = false;
unsigned long tWifi = 0, tTentativa = 0, tQueda = 0, tSemWifi = 0, tStatus = 0;
unsigned long esperaWifi = WIFI_ESPERA_INICIAL;
unsigned long tentativasWifi = 0, quedasWifi = 0;

// ---------- log carimbado ----------
// Toda linha sai com [data hora | uptime] na frente. E isso que transforma o
// print do monitor serial em evidencia: da para conferir a hora do log com a
// hora do relogio do computador na mesma tela.
void carimbo(char* saida, size_t n) {
  unsigned long s = millis() / 1000;
  char hora[20] = "--/-- --:--:--";
  if (horaSincronizada) {
    time_t agora = time(nullptr);
    struct tm t;
    localtime_r(&agora, &t);
    strftime(hora, sizeof(hora), "%d/%m %H:%M:%S", &t);
  }
  snprintf(saida, n, "[%s | up %02lu:%02lu:%02lu]", hora, s / 3600, (s / 60) % 60, s % 60);
}

void logSerial(const char* fmt, ...) {
  char ts[40];
  carimbo(ts, sizeof(ts));
  char msg[220];
  va_list args;
  va_start(args, fmt);
  vsnprintf(msg, sizeof(msg), fmt, args);
  va_end(args);
  Serial.print(ts); Serial.print(' '); Serial.println(msg);
}

const char* nomeStatusWifi(wl_status_t s) {
  switch (s) {
    case WL_CONNECTED:       return "CONECTADO";
    case WL_NO_SSID_AVAIL:   return "SSID_NAO_ENCONTRADO";
    case WL_CONNECT_FAILED:  return "FALHA_NA_AUTENTICACAO";
    case WL_CONNECTION_LOST: return "CONEXAO_PERDIDA";
    case WL_DISCONNECTED:    return "DESCONECTADO";
    case WL_IDLE_STATUS:     return "OCIOSO";
    default:                 return "DESCONHECIDO";
  }
}

// RSSI e potencia recebida em dBm, sempre negativa: quanto mais perto de zero,
// mais forte o sinal que chega na antena.
const char* qualidadeRssi(int rssi) {
  if (rssi >= -60) return "boa";
  if (rssi >= -70) return "aceitavel";
  if (rssi >= -80) return "fraca";
  return "critica";
}

void imprimirStatus() {
  wl_status_t s = WiFi.status();
  if (s == WL_CONNECTED) {
    int rssi = WiFi.RSSI();
    logSerial("ATIVO -- IP=%s RSSI=%d dBm (%s) | MQTT=%s | nivel=%d%% temp=%.1fC bomba=%s | quedas=%lu",
              WiFi.localIP().toString().c_str(), rssi, qualidadeRssi(rssi),
              mqtt.connected() ? "conectado" : "desconectado",
              nivel, temperatura,
              bombaLigada ? (origemAuto ? "AUTO" : "MANUAL") : "parada", quedasWifi);
  } else {
    logSerial("OFFLINE -- status=%s | tentativas=%lu | quedas=%lu",
              nomeStatusWifi(s), tentativasWifi, quedasWifi);
  }
}

void iniciarWifi() {
  WiFi.mode(WIFI_STA);
  WiFi.persistent(false);        // nao regrava as credenciais na flash a cada boot
  WiFi.setAutoReconnect(false);  // a reconexao e feita por supervisionarWifi(), logo
                                 // abaixo: um mecanismo so, e com log de cada passo
  logSerial("Wi-Fi: associando ao SSID \"%s\" e pedindo endereco por DHCP...", SSID);
  WiFi.begin(SSID, SENHA);
  tTentativa = millis();
  tentativasWifi = 1;
}

void aoConectarWifi() {
  wifiConectado = true;
  esperaWifi = WIFI_ESPERA_INICIAL;
  backoff = 1000;   // solta a espera do MQTT: com rede de volta ele tenta na hora
  tMqtt = 0;
  int rssi = WiFi.RSSI();
  logSerial("Wi-Fi CONECTADO -- SSID=%s canal=%d RSSI=%d dBm (%s)",
            WiFi.SSID().c_str(), WiFi.channel(), rssi, qualidadeRssi(rssi));
  logSerial("DHCP entregou: IP=%s mascara=%s gateway=%s DNS=%s",
            WiFi.localIP().toString().c_str(), WiFi.subnetMask().toString().c_str(),
            WiFi.gatewayIP().toString().c_str(), WiFi.dnsIP().toString().c_str());
  if (tQueda) {
    logSerial("RECONEXAO AUTOMATICA concluida em %.1f s apos a queda #%lu, "
              "sem mexer no codigo e sem upload novo",
              (millis() - tQueda) / 1000.0, quedasWifi);
    tQueda = 0;
  }
  if (!horaSincronizada) {
    configTime(NTP_FUSO, 0, "pool.ntp.org", "a.st1.ntp.br");
    logSerial("NTP: pedindo a hora certa para carimbar as proximas linhas");
  }
}

// Um caminho unico para registrar a perda, venha ela de uma queda real ou do
// comando QUEDA digitado no monitor serial.
void registrarQueda(const char* motivo) {
  if (!wifiConectado) return;
  wifiConectado = false;
  quedasWifi++;
  tQueda = millis();
  tSemWifi = 0;
  logSerial("Wi-Fi PERDIDO (queda #%lu) -- %s", quedasWifi, motivo);
  mqtt.disconnect();   // sem rede o MQTT so acumularia timeout; ele volta depois
  esperaWifi = WIFI_ESPERA_INICIAL;
  tTentativa = millis() - esperaWifi;   // primeira tentativa sai imediatamente
}

// Esta e a logica que detecta e reage a uma queda de Wi-Fi. Roda a cada 250 ms
// dentro do loop(), sem bloquear: o sensor, o LCD e a bomba continuam
// funcionando enquanto a rede nao volta.
void supervisionarWifi() {
  unsigned long agora = millis();
  if (agora - tWifi < 250) return;
  tWifi = agora;

  wl_status_t s = WiFi.status();

  if (s == WL_CONNECTED) {
    if (!wifiConectado) aoConectarWifi();
    if (!horaSincronizada && time(nullptr) > 1700000000) {
      horaSincronizada = true;
      logSerial("NTP: relogio sincronizado -- as linhas acima tinham so o uptime");
    }
    return;
  }

  if (wifiConectado) {
    char motivo[80];
    snprintf(motivo, sizeof(motivo), "queda detectada pelo firmware (status=%s)", nomeStatusWifi(s));
    registrarQueda(motivo);
  }

  // Na queda, uma linha por segundo deixa a interrupcao visivel no print.
  // No boot, a cada 2 s, para nao empurrar o DHCP para fora da tela.
  if (agora - tSemWifi >= (tQueda ? 1000UL : 2000UL)) {
    tSemWifi = agora;
    if (tQueda) logSerial("sem Wi-Fi ha %.1f s (status=%s) -- o ESP32 esta tentando voltar sozinho",
                          (agora - tQueda) / 1000.0, nomeStatusWifi(s));
    else        logSerial("ainda sem IP (status=%s) -- aguardando o Wi-Fi subir", nomeStatusWifi(s));
  }

  if (agora - tTentativa < esperaWifi) return;
  tTentativa = agora;
  tentativasWifi++;
  logSerial("Wi-Fi: tentativa #%lu de %s (proxima em %lu s se esta falhar)",
            tentativasWifi, tQueda ? "reconexao" : "conexao", esperaWifi / 1000);
  WiFi.disconnect(false, false);
  WiFi.begin(SSID, SENHA);
  esperaWifi = min(esperaWifi * 2, WIFI_ESPERA_MAXIMA);
}

// ---------- buzzer nao bloqueante ----------
const uint16_t PAD_CONCLUIDO[] = {300, 200, 300, 200, 300, 0};
const uint16_t PAD_ALTO[]      = {800, 400, 0};
const uint16_t* padrao = nullptr;
uint8_t padIdx = 0; bool padRepete = false; unsigned long padT = 0;

void tocar(const uint16_t* p, bool rep) {
  if (padrao == p) return;
  padrao = p; padIdx = 0; padRepete = rep; padT = millis();
  digitalWrite(PIN_BUZZER, HIGH);
}
void pararBuzzer() { padrao = nullptr; digitalWrite(PIN_BUZZER, LOW); }
void atualizarBuzzer() {
  if (!padrao) return;
  if (millis() - padT < padrao[padIdx]) return;
  padT = millis(); padIdx++;
  if (padrao[padIdx] == 0) {
    if (padRepete) { padIdx = 0; digitalWrite(PIN_BUZZER, HIGH); }
    else pararBuzzer();
    return;
  }
  digitalWrite(PIN_BUZZER, (padIdx % 2 == 0) ? HIGH : LOW);
}

// ---------- medicao ----------
float lerDistancia() {
  float v = 331.4 + 0.606 * temperatura;      // m/s
  float cmPorUs = v / 10000.0;
  digitalWrite(PIN_TRIG, LOW);  delayMicroseconds(4);
  digitalWrite(PIN_TRIG, HIGH); delayMicroseconds(10);
  digitalWrite(PIN_TRIG, LOW);
  unsigned long dur = pulseIn(PIN_ECHO, HIGH, 30000UL);
  if (dur == 0) return -1.0;
  return (dur * cmPorUs) / 2.0;
}

float mediana() {
  float t[5]; uint8_t n = bufCheio ? 5 : bufIdx;
  if (n == 0) return -1.0;
  for (uint8_t i = 0; i < n; i++) t[i] = buf[i];
  for (uint8_t i = 1; i < n; i++) {
    float k = t[i]; int j = i - 1;
    while (j >= 0 && t[j] > k) { t[j + 1] = t[j]; j--; }
    t[j + 1] = k;
  }
  return t[n / 2];
}

void medir() {
  float d = lerDistancia();
  if (d < 0 || d > 450) return;
  buf[bufIdx] = d;
  bufIdx = (bufIdx + 1) % 5;
  if (bufIdx == 0) bufCheio = true;
  float m = mediana();
  if (m < 0) return;
  distFiltrada = (distFiltrada < 0) ? m : (ALFA_EMA * m + (1 - ALFA_EMA) * distFiltrada);
  float pct = (D_VAZIO - distFiltrada) / (D_VAZIO - D_CHEIO) * 100.0;
  nivel = constrain((int)round(pct), 0, 100);
}

// ---------- publicacao ----------
void pub(const char* sufixo, const char* payload, bool retained = false) {
  if (!mqtt.connected()) return;
  char topico[100];
  snprintf(topico, sizeof(topico), "%s/%s", PREFIXO, sufixo);
  mqtt.publish(topico, payload, retained);
}

void publicarBomba() {
  const char* v = !bombaLigada ? "DESLIGADA" : (origemAuto ? "LIGADA:AUTO" : "LIGADA:MANUAL");
  pub("bomba", v, true);
}

void definirAlerta(String novo) {
  if (novo == alerta) return;
  alerta = novo;
  pub("alerta/nivel", alerta.c_str());
}

// ---------- logica da bomba ----------
void ligar(bool automatico) {
  if (bombaLigada) return;
  bombaLigada = true; origemAuto = automatico;
  digitalWrite(PIN_LED, HIGH);
  publicarBomba();
  if (automatico) definirAlerta("ACIONAMENTO_AUTOMATICO");
}

void parar(bool manual) {
  if (!bombaLigada) return;
  bombaLigada = false;
  digitalWrite(PIN_LED, LOW);
  if (manual && nivel <= NIVEL_LIGA) inibido = true;
  publicarBomba();
  if (!manual) { tocar(PAD_CONCLUIDO, false); definirAlerta("ENCHIMENTO_CONCLUIDO"); }
}

void controlar() {
  if (nivel > NIVEL_BAIXO) inibido = false;
  if (!bombaLigada && nivel <= NIVEL_LIGA && !inibido) ligar(true);
  else if (bombaLigada && nivel >= NIVEL_DESLIGA) parar(false);

  if (nivel >= NIVEL_CRITICO) { definirAlerta("CRITICO_ALTO"); tocar(PAD_ALTO, true); }
  else {
    if (padRepete) pararBuzzer();
    if (nivel < NIVEL_BAIXO) definirAlerta("NIVEL_BAIXO");
    else if (alerta == "CRITICO_ALTO" || alerta == "NIVEL_BAIXO") definirAlerta("NORMAL");
  }
}

// ---------- comandos ----------
void aplicarComando(String c) {
  c.trim(); c.toUpperCase();
  if (c == "LIGAR")      { ligar(false); pub("comando/bomba/confirmacao", "LIGAR:OK"); }
  else if (c == "PARAR") { parar(true);  pub("comando/bomba/confirmacao", "PARAR:OK"); }
  else                     pub("comando/bomba/confirmacao", "ERRO:comando_invalido");
}

void aoReceber(char* topico, byte* payload, unsigned int len) {
  String msg; for (unsigned int i = 0; i < len; i++) msg += (char)payload[i];
  aplicarComando(msg);
}

// ---------- comandos digitados no monitor serial ----------
// QUEDA existe para a evidencia de reconexao: derruba o Wi-Fi de proposito sem
// editar o codigo e sem upload novo, entao o que acontece depois e do firmware.
void processarComandoSerial(String c) {
  c.trim(); c.toUpperCase();
  if (c == "QUEDA") {
    logSerial("COMANDO \"QUEDA\": derrubando o Wi-Fi de proposito para testar a reconexao");
    registrarQueda("desconexao provocada pelo comando QUEDA no monitor serial");
    WiFi.disconnect(false, false);   // so desassocia; nao apaga as credenciais
  } else if (c == "STATUS") {
    imprimirStatus();
  } else if (c == "LIGAR" || c == "PARAR") {
    aplicarComando(c);
    logSerial("COMANDO \"%s\" aplicado pelo monitor serial", c.c_str());
  } else {
    logSerial("comando \"%s\" desconhecido -- use QUEDA, STATUS, LIGAR ou PARAR", c.c_str());
  }
}

void lerSerial() {
  static String linha;
  while (Serial.available()) {
    char c = (char)Serial.read();
    if (c == '\n' || c == '\r') {
      if (linha.length()) {
        Serial.println();
        processarComandoSerial(linha);
        linha = "";
      }
    } else if (c == 8 || c == 127) {          // backspace
      if (linha.length()) {
        linha.remove(linha.length() - 1);
        Serial.print("\b \b");
      }
    } else if (linha.length() < 32) {
      linha += c;
      Serial.write(c);   // eco: o terminal do Wokwi no VS Code nao ecoa sozinho,
                         // entao sem isto o comando digitado nao sai no print
    }
  }
}

// ---------- conexao ----------
void conectarMqtt() {
  // Usa o estado que supervisionarWifi() publica, e nao WiFi.status() direto:
  // assim a linha do MQTT nunca aparece antes das linhas de Wi-Fi e DHCP.
  if (mqtt.connected() || !wifiConectado) return;
  if (millis() - tMqtt < backoff) return;
  tMqtt = millis();
  char id[40], lwt[110];
  snprintf(id, sizeof(id), "esp32-reserv-%06X", (uint32_t)ESP.getEfuseMac());
  snprintf(lwt, sizeof(lwt), "%s/status", PREFIXO);
  if (mqtt.connect(id, USUARIO, SENHA_MQTT, lwt, 1, true, "offline")) {
    backoff = 1000;
    pub("status", "online", true);
    publicarBomba();
    char sub[110];
    snprintf(sub, sizeof(sub), "%s/comando/bomba", PREFIXO);
    mqtt.subscribe(sub, 1);
    logSerial("MQTT conectado ao broker %s:%d como %s", BROKER, PORTA, id);
  } else {
    backoff = min(backoff * 2, 30000UL);
    logSerial("MQTT: falha ao conectar em %s:%d (rc=%d) -- nova tentativa em %lu s",
              BROKER, PORTA, mqtt.state(), backoff / 1000);
  }
}

void setup() {
  Serial.begin(115200);
  delay(300);   // da tempo do monitor serial engatar antes do cabecalho
  Serial.println();
  logSerial("===== Reservatorio IoT -- firmware iniciado =====");
  logSerial("Dispositivo esp32-reserv-%06X | topico base: %s",
            (uint32_t)ESP.getEfuseMac(), PREFIXO);
  logSerial("Comandos do monitor serial: QUEDA | STATUS | LIGAR | PARAR");
  pinMode(PIN_TRIG, OUTPUT); pinMode(PIN_ECHO, INPUT);
  pinMode(PIN_LED, OUTPUT);  pinMode(PIN_BUZZER, OUTPUT);
  digitalWrite(PIN_LED, LOW); digitalWrite(PIN_BUZZER, LOW);

  Wire.begin(21, 22);
  lcd.init(); lcd.backlight();
  lcd.setCursor(0, 0); lcd.print("Reservatorio");
  lcd.setCursor(0, 1); lcd.print("Iniciando...");

  sensorTemp.begin();
  iniciarWifi();

  // Aceita o certificado do broker sem validar a cadeia. Suficiente
  // para o trabalho e evita embutir o CA raiz no firmware; em
  // producao o certo seria net.setCACert(...) com o CA do cluster.
  net.setInsecure();

  mqtt.setServer(BROKER, PORTA);
  mqtt.setCallback(aoReceber);
}

void loop() {
  unsigned long agora = millis();

  if (agora - tTemp > 3000) {
    tTemp = agora;
    sensorTemp.requestTemperatures();
    float t = sensorTemp.getTempCByIndex(0);
    if (t > -50 && t < 90) temperatura = t;
  }

  if (agora - tLeitura > 100) { tLeitura = agora; medir(); controlar(); }

  if (agora - tLcd > 500) {
    tLcd = agora;
    char l1[17], l2[17];
    snprintf(l1, sizeof(l1), "Nivel: %3d%%    ", nivel);
    snprintf(l2, sizeof(l2), "Bomba: %-9s", bombaLigada ? (origemAuto ? "AUTO" : "MANUAL") : "PARADA");
    lcd.setCursor(0, 0); lcd.print(l1);
    lcd.setCursor(0, 1); lcd.print(l2);
  }

  if (agora - tPub > 2000 && mqtt.connected()) {
    tPub = agora;
    char v[12];
    snprintf(v, sizeof(v), "%d", nivel);            pub("nivel", v);
    snprintf(v, sizeof(v), "%.1f", distFiltrada);   pub("distancia", v);
    snprintf(v, sizeof(v), "%.1f", temperatura);    pub("temperatura", v);
  }

  // Batimento de vida no serial: prova que a conexao segue de pe entre um
  // evento e outro, mesmo quando nada mais esta acontecendo.
  if (agora - tStatus > 5000) {
    tStatus = agora;
    if (wifiConectado) imprimirStatus();
  }

  atualizarBuzzer();
  supervisionarWifi();
  lerSerial();
  conectarMqtt();
  mqtt.loop();
}