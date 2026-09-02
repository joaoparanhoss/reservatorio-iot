#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <PubSubClient.h>
#include <Wire.h>
#include <LiquidCrystal_I2C.h>
#include <OneWire.h>
#include <DallasTemperature.h>

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

// ---------- conexao ----------
void conectarMqtt() {
  if (mqtt.connected() || WiFi.status() != WL_CONNECTED) return;
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
    Serial.println("MQTT conectado");
  } else {
    backoff = min(backoff * 2, 30000UL);
  }
}

void setup() {
  Serial.begin(115200);
  pinMode(PIN_TRIG, OUTPUT); pinMode(PIN_ECHO, INPUT);
  pinMode(PIN_LED, OUTPUT);  pinMode(PIN_BUZZER, OUTPUT);
  digitalWrite(PIN_LED, LOW); digitalWrite(PIN_BUZZER, LOW);

  Wire.begin(21, 22);
  lcd.init(); lcd.backlight();
  lcd.setCursor(0, 0); lcd.print("Reservatorio");
  lcd.setCursor(0, 1); lcd.print("Iniciando...");

  sensorTemp.begin();
  WiFi.begin(SSID, SENHA);

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
    Serial.printf("nivel=%d%% dist=%.1fcm temp=%.1fC bomba=%d\n",
                  nivel, distFiltrada, temperatura, bombaLigada);
  }

  atualizarBuzzer();
  conectarMqtt();
  mqtt.loop();
}