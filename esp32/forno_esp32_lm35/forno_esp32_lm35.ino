/*
 * Forno inteligente - ESP32 (FreeRTOS) com sensor LM35
 * Papel: leitura do sensor, controle em tempo real, atuadores e telemetria
 * para a Raspberry Pi.
 *
 * Ligações:
 *   LM35 (face plana para você: +Vs, saída, GND)
 *        +Vs -> 5V (pino VIN do ESP32)   saída -> GPIO 35   GND -> GND
 *        (capacitor de 100 nF entre a saída e o GND, perto do ESP32)
 *   Potenciômetro (pino do meio) -> GPIO 34   (extremos em 3V3 e GND)
 *   Botão liga/desliga           -> GPIO 4 e GND (pull-up interno)
 *   LED aquecedor (PWM)          -> GPIO 13 (resistor 330 ohm em série)
 *   Barra de temperatura (4 LEDs)-> GPIO 25, 26, 27, 14 (resistor 220-330 ohm cada)
 *   Raspberry Pi                 -> cabo USB (UART via Serial, 115200 baud)
 *
 * Protocolo (texto, uma linha por mensagem, terminada em '\n'):
 *   ESP32 -> Pi (a cada 500 ms):
 *       {"t":38.4,"alvo":60,"pwm":100,"estado":"AQUECENDO","modo":"POT"}
 *   Pi -> ESP32:
 *       SET <temp>   define o alvo pela Raspberry (modo REMOTO), dentro de
 *                    ALVO_MIN..ALVO_MAX (30..90 °C com LM35; 50..200 °C simulado)
 *       POT          volta a usar o potenciômetro como alvo
 *       START / STOP liga / desliga o forno
 *       PING         "estou vivo" (enviado a cada 1 s pela Pi)
 *
 * Segurança:
 *   - Pi silenciosa por mais de FAILSAFE_MS (depois de já ter falado) -> FALHA_COMM
 *   - Leitura do LM35 fora do plausível (fio solto, curto)            -> FALHA_SENSOR
 *   - Temperatura acima de T_MAX_SEG                                  -> SUPERAQUEC
 *   Em qualquer um desses casos o aquecedor é desligado.
 *
 * Opções de compilação:
 *   SENSOR_REAL = 1 (LM35) ou 0 (modelo térmico simulado, como antes)
 *   USE_PID     = 0 (on-off com histerese) ou 1 (PI; ganhos precisam de ajuste)
 */

#include <Arduino.h>

// ---------------- Opções ----------------
#define SENSOR_REAL 1
#define USE_PID     0

// ---------------- Pinos ----------------
#define PIN_LM35  35   // ADC1 (o ADC2 não funciona bem com Wi-Fi)
#define PIN_POT   34
#define PIN_BTN   4
#define PIN_HEAT  13
const int   PIN_BARRA[4]    = {25, 26, 27, 14};

// ---------------- Faixa de trabalho (depende do modo) ----------------
// ALVO_MIN/ALVO_MAX : faixa do potenciômetro e do comando SET
// T_MAX_SEG         : acima disso o aquecedor é desligado (SUPERAQUEC)
// LIMIAR_BARRA      : temperatura medida (°C) em que cada LED da barra acende
//                     (ordem crescente, na ordem dos pinos de PIN_BARRA)
#if SENSOR_REAL
  // LM35: mede de forma confiável até cerca de 100 °C
  const float ALVO_MIN  = 30.0f;
  const float ALVO_MAX  = 90.0f;
  const float T_MAX_SEG = 100.0f;
  const float LIMIAR_BARRA[4] = {35, 50, 65, 80};
#else
  // Simulação: faixa de um forno de verdade
  const float ALVO_MIN  = 50.0f;
  const float ALVO_MAX  = 200.0f;
  const float T_MAX_SEG = 230.0f;
  const float LIMIAR_BARRA[4] = {40, 80, 120, 160};
#endif

// ---------------- Sensor LM35 ----------------
const float LM35_OFFSET = 0.0f;   // correção de calibração (°C), ajuste comparando com uma referência
const float ALFA        = 0.3f;   // filtro exponencial (0..1): menor = mais suave

// ---------------- Modelo térmico (só usado se SENSOR_REAL = 0) ----------------
const float T_AMB = 25.0f;   // temperatura ambiente (°C)
const float TAU   = 15.0f;   // constante de tempo de resfriamento (s)
const float GANHO = 15.0f;   // aquecimento a 100% de potência (°C/s)

// ---------------- Controle ----------------
const float DT   = 0.2f;     // período da tarefa de controle (s)
const float HIST = 2.0f;     // histerese do on-off (°C)
const float KP   = 0.05f;
const float KI   = 0.01f;

// ---------------- Segurança de comunicação ----------------
const uint32_t FAILSAFE_MS = 5000;

// ---------------- Estado compartilhado ----------------
enum ModoAlvo { MODO_POT, MODO_REMOTO };

volatile float    temperatura = T_AMB;   // temperatura medida (ou simulada), °C
volatile float    alvo        = 30.0f;   // alvo efetivo
volatile float    alvoPot     = 30.0f;   // vindo do potenciômetro
volatile float    alvoRemoto  = 30.0f;   // vindo da Raspberry
volatile ModoAlvo modoAlvo    = MODO_POT;
volatile float    potencia    = 0.0f;    // saída do controle (0..1)
volatile bool     ligado      = false;
volatile bool     piVisto     = false;
volatile bool     falhaComm   = false;
volatile bool     falhaSensor = false;
volatile bool     superaquec  = false;
volatile uint32_t ultimoContatoPi = 0;

// ---------------- PWM (compatível com core 2.x e 3.x) ----------------
void pwmInit() {
#if ESP_ARDUINO_VERSION_MAJOR >= 3
  ledcAttach(PIN_HEAT, 5000, 8);
#else
  ledcSetup(0, 5000, 8);
  ledcAttachPin(PIN_HEAT, 0);
#endif
}

void pwmWrite(uint8_t duty) {
#if ESP_ARDUINO_VERSION_MAJOR >= 3
  ledcWrite(PIN_HEAT, duty);
#else
  ledcWrite(0, duty);
#endif
}

// ---------------- Leitura do LM35 ----------------
// 10 mV por °C. Média de 16 leituras para reduzir o ruído do ADC do ESP32.
float lerLM35() {
  uint32_t soma = 0;
  for (int i = 0; i < 16; i++) soma += analogReadMilliVolts(PIN_LM35);
  float mV = soma / 16.0f;
  return mV / 10.0f + LM35_OFFSET;
}

// ---------------- Tarefa: entradas locais ----------------
void tarefaEntrada(void *pv) {
  bool btnAnterior = true;  // pull-up: solto = HIGH
  for (;;) {
    int raw = analogRead(PIN_POT);                                   // 0..4095
    alvoPot = ALVO_MIN + (raw / 4095.0f) * (ALVO_MAX - ALVO_MIN);    // faixa do modo

    bool btn = digitalRead(PIN_BTN);
    if (btnAnterior && !btn) ligado = !ligado;                       // clique
    btnAnterior = btn;

    vTaskDelay(pdMS_TO_TICKS(100));
  }
}

// ---------------- Tarefa: sensor + controle (periódica) ----------------
void tarefaControle(void *pv) {
  TickType_t ultimo = xTaskGetTickCount();
  bool aquecendo = false;
  bool primeiraLeitura = true;
  float integral = 0.0f;

  for (;;) {
    // Alvo efetivo
    alvo = (modoAlvo == MODO_REMOTO) ? alvoRemoto : alvoPot;

    // Fail-safe: Pi sumiu -> desliga o aquecedor
    if (piVisto && !falhaComm && (millis() - ultimoContatoPi > FAILSAFE_MS)) {
      falhaComm = true;
      ligado = false;
    }

#if SENSOR_REAL
    // Leitura e filtro do LM35
    float leitura = lerLM35();
    if (leitura < -5.0f || leitura > 150.0f) {
      falhaSensor = true;                       // valor implausível: fio solto ou curto
    } else {
      falhaSensor = false;
      if (primeiraLeitura) { temperatura = leitura; primeiraLeitura = false; }
      else temperatura = ALFA * leitura + (1.0f - ALFA) * temperatura;
    }
#endif

    superaquec = (temperatura > T_MAX_SEG);
    bool bloqueado = falhaSensor || superaquec;

    // Controle
    float u = 0.0f;
    if (ligado && !bloqueado) {
#if USE_PID
      float erro = alvo - temperatura;
      integral += erro * DT;
      integral = constrain(integral, -100.0f, 100.0f);
      u = constrain(KP * erro + KI * integral, 0.0f, 1.0f);
#else
      if (temperatura < alvo - HIST)      aquecendo = true;
      else if (temperatura > alvo + HIST) aquecendo = false;
      u = aquecendo ? 1.0f : 0.0f;
#endif
    } else {
      integral = 0.0f;
      aquecendo = false;
    }
    potencia = u;

#if !SENSOR_REAL
    // Modelo térmico de 1ª ordem (apenas na simulação)
    float T = temperatura;
    T += DT * ((T_AMB - T) / TAU + GANHO * u);
    temperatura = T;
#endif

    // Atuadores
    pwmWrite((uint8_t)(u * 255.0f));                         // LED aquecedor
    for (int i = 0; i < 4; i++)                              // barra de temperatura
      digitalWrite(PIN_BARRA[i], temperatura >= LIMIAR_BARRA[i]);

    vTaskDelayUntil(&ultimo, pdMS_TO_TICKS(200));  // período fixo
  }
}

// ---------------- Processa um comando vindo da Raspberry ----------------
void processaComando(const char *linha) {
  ultimoContatoPi = millis();
  piVisto = true;
  falhaComm = false;

  if (strncmp(linha, "SET ", 4) == 0) {
    float v = atof(linha + 4);
    alvoRemoto = constrain(v, ALVO_MIN, ALVO_MAX);
    modoAlvo = MODO_REMOTO;
  } else if (strcmp(linha, "POT") == 0) {
    modoAlvo = MODO_POT;
  } else if (strcmp(linha, "START") == 0) {
    ligado = true;
  } else if (strcmp(linha, "STOP") == 0) {
    ligado = false;
  }
  // "PING" só renova o contato (já feito acima)
}

// ---------------- Tarefa: comunicação serial (UART) ----------------
void tarefaSerial(void *pv) {
  char buf[48];
  size_t n = 0;
  uint32_t proximoEnvio = millis();

  for (;;) {
    // 1) Recebe comandos (sem bloquear)
    while (Serial.available()) {
      char c = Serial.read();
      if (c == '\n') {
        buf[n] = '\0';
        if (n > 0) processaComando(buf);
        n = 0;
      } else if (c != '\r' && n < sizeof(buf) - 1) {
        buf[n++] = c;
      }
    }

    // 2) Envia telemetria a cada 500 ms
    if (millis() >= proximoEnvio) {
      proximoEnvio += 500;

      const char *estado;
      if (falhaSensor)                    estado = "FALHA_SENSOR";
      else if (superaquec)                estado = "SUPERAQUEC";
      else if (falhaComm && !ligado)      estado = "FALHA_COMM";
      else if (!ligado)                   estado = "OCIOSO";
      else if (temperatura < alvo - HIST) estado = "AQUECENDO";
      else                                estado = "MANTENDO";

      Serial.printf(
        "{\"t\":%.1f,\"alvo\":%.0f,\"pwm\":%.0f,\"estado\":\"%s\",\"modo\":\"%s\"}\n",
        temperatura, alvo, potencia * 100.0f, estado,
        (modoAlvo == MODO_REMOTO) ? "REMOTO" : "POT");
    }

    vTaskDelay(pdMS_TO_TICKS(10));
  }
}

// ---------------- Setup ----------------
void setup() {
  Serial.begin(115200);
  pinMode(PIN_BTN, INPUT_PULLUP);
  for (int i = 0; i < 4; i++) pinMode(PIN_BARRA[i], OUTPUT);
  analogSetPinAttenuation(PIN_LM35, ADC_11db);   // entrada até ~3,1 V
  pwmInit();

  xTaskCreate(tarefaEntrada,  "entrada",  2048, NULL, 1, NULL);
  xTaskCreate(tarefaControle, "controle", 3072, NULL, 3, NULL);  // maior prioridade
  xTaskCreate(tarefaSerial,   "serial",   4096, NULL, 2, NULL);
}

void loop() {
  vTaskDelay(portMAX_DELAY);  // tudo roda nas tarefas
}
