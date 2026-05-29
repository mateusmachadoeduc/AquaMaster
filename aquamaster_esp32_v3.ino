// ============================================================
// AQUA MASTER - ESP32 Modo Cliente WiFi
// Versao 3.0
// ============================================================
//
// COMO CONFIGURAR:
//   1. Altere WIFI_SSID e WIFI_PASSWORD para o seu WiFi
//   2. Descubra o IP do seu PC na rede local:
//        Windows: abra o Prompt e digite "ipconfig"
//        Mac/Linux: abra o Terminal e digite "ifconfig"
//   3. Altere BACKEND_URL com o IP encontrado
//   4. Suba o backend Python no seu PC antes de ligar o ESP32
//
// ============================================================

#include <WiFi.h>
#include <HTTPClient.h>
#include <OneWire.h>
#include <DallasTemperature.h>
#include <ESP32Servo.h>

// ------------------------------------------------------------
// *** CONFIGURACOES QUE VOCE PRECISA ALTERAR ***
// ------------------------------------------------------------

// Nome e senha do seu WiFi domestico
const char* WIFI_SSID     = "Educacao";       // <-- mude aqui
const char* WIFI_PASSWORD = "paulofreire";      // <-- mude aqui

// Endereco do backend Python no seu PC.
// Exemplo: "http://192.168.1.105:5000"
// Para encontrar o IP: Windows → Prompt → ipconfig (procure "Endereco IPv4")
const char* BACKEND_URL = "http://172.16.0.178:5000"; // <-- mude aqui

// Intervalo de envio de dados ao backend (em ms). Padrao: 5000 = 5 segundos
#define INTERVALO_ENVIO_MS  5000UL

// ------------------------------------------------------------
// PINOS DO ESP32
// ------------------------------------------------------------
#define PIN_TEMP   4    // GPIO4  - sensor de temperatura DS18B20
#define PIN_PH     32   // GPIO32 - sensor de pH PH-4502C
#define PIN_RELE   18   // GPIO18 - rele da bomba d'agua
#define PIN_SERVO  13   // GPIO13 - servo motor do alimentador

// ------------------------------------------------------------
// CONFIGURACAO DO RELE
// Se os botoes da dashboard estiverem trocados, inverta os valores.
// ------------------------------------------------------------
#define RELE_LIGA    HIGH
#define RELE_DESLIGA LOW

// ------------------------------------------------------------
// CICLO DA BOMBA (em minutos)
// ------------------------------------------------------------
int BOMBA_TEMPO_LIGA    = 15;  // bomba fica ligada por 15 min
int BOMBA_TEMPO_DESLIGA = 5;   // depois descansa por 5 min

// ------------------------------------------------------------
// CALIBRACAO DO pH — Dual-slope (tres pontos de calibracao)
//
// ESCALA DOS VALORES:
//   Os valores abaixo sao na "escala 0dB" — obtidos com o codigo
//   Lab Inventores que usa analogRead sem definir atenuacao (padrao 0dB,
//   faixa real 0-1.1V, mas a formula aplica 3.3/4095 e reporta 0-3.3V).
//   O pino PIN_PH e configurado em ADC_0db no setup() para manter essa
//   escala. NAO altere a atenuacao sem recalibrar.
//
// Como coletar os valores:
//   Modo calibracao (Serial "Calibrar Sensor PH") mostra "Tensao GPIO25"
//   — esse e o valor a usar nos comandos neutro/acido/alcalino.
//
//   tensaoNeutro   → tensao lida com eletrodo no pH 7.0
//   tensaoTampao4  → tensao lida com eletrodo no pH 4.0
//   tensaoTampao10 → tensao lida com eletrodo no pH 10.0
//
// Os slopes sao recalculados automaticamente.
// PH_OFFSET: ajuste fino opcional apos verificacao.
// ------------------------------------------------------------
float tensaoNeutro   = 2.5000;  // V — pH 7.0
float tensaoTampao4  = 3.3000;  // V — pH 4.0
float tensaoTampao10 = 2.0070;  // V — pH 10.0
float PH_OFFSET      = 0.00;    // ajuste fino opcional

// Slopes — recalculados por recalcularSlopes() (nao edite aqui)
float slopeAcido    = 0.0;   // (tensaoTampao4  - tensaoNeutro) / 3.0
float slopeAlcalino = 0.0;   // (tensaoNeutro   - tensaoTampao10) / 3.0

// ------------------------------------------------------------
// CONFIGURACAO DO ALIMENTADOR (disco com 7 compartimentos)
// ------------------------------------------------------------
// Posicoes do servo (com GRAUS_POR_DIA = 25):
//   Home  = 0 graus  (posicao de espera / recarga)
//   Dia 1 = 25 graus
//   Dia 2 = 50 graus
//   Dia 3 = 75 graus
//   Dia 4 = 100 graus
//   Dia 5 = 125 graus
//   Dia 6 = 150 graus
//   Dia 7 = 175 graus  (limite fisico: 180 graus)
// Apos o Dia 7, aguarda 10s e volta para home.
// O usuario recoloca racao e clica "Iniciar ciclo" no dashboard.
//
// *** AJUSTE FINO POR DIA ***
//   Altere cada valor do array abaixo para a posicao
//   exata de cada compartimento do seu disco (0–180 graus).
//   Eles tambem podem ser atualizados pelo dashboard sem
//   regravar o firmware (comando set_posicoes).
// ------------------------------------------------------------
#define NUM_DIAS         7
#define SERVO_HOME       0
#define SERVO_TEMPO_MS   600    // ms para o servo se mover

// Posicao do servo para cada dia (0 = Home, maximo 180)
// Dia:        1    2    3    4    5    6    7
int posicoesDias[NUM_DIAS] = { 25,  50,  75, 100, 125, 150, 175 };

// Intervalo entre alimentacoes:
//   86400000 = 24 horas (uso real)
//   60000    = 1 minuto (para testes)
#define INTERVALO_MS  86400000UL

// Tempo que o servo fica parado na posicao do Dia 7
// antes de retornar ao home (em ms).
//   10000 = 10 segundos (recomendado para garantir que a racao caia)
#define ESPERA_DIA7_MS   10000UL

// Angulo maximo permitido para controle manual pelo dashboard
// O SG90 vai ate 180 graus fisicamente.
#define SERVO_MAX_GRAUS  180

// Estados do alimentador
#define ESTADO_AGUARDANDO 0   // aguardando recarga pelo usuario
#define ESTADO_ATIVO      1   // ciclo de 7 dias em andamento
#define ESTADO_MOVENDO    2   // servo se movendo agora

// ============================================================
// OBJETOS
// ============================================================
OneWire           oneWire(PIN_TEMP);
DallasTemperature sensors(&oneWire);
Servo             servoAlimentador;

// ============================================================
// VARIAVEIS GLOBAIS
// ============================================================

// Sensores
float temperatura = 0.0;
float ph          = 0.0;

// Bomba
bool  bombaLigada       = false;
bool  modoManualBomba   = false;
bool  estadoManualBomba = false;
unsigned long tempoLigouBomba = 0;
bool          bombaEmCiclo    = true;

// Alimentador
int  estadoAlimentador    = ESTADO_ATIVO;
int  diaAtual             = 0;
int  posicaoAtual         = 0;
int  contadorAlimentacoes = 0;
int  ciclosCompletos      = 0;
unsigned long tempoUltimaAlimentacao = 0;
unsigned long tempoInicioMovimento   = 0;
bool retornarHome            = false; // flag: Dia 7 concluido, aguarda ir ao home
bool esperandoRetornoHome   = false; // flag: contagem de 10s antes de ir ao home
unsigned long tempoInicioEspera = 0; // inicio da espera de 10s apos Dia 7

// Temporizadores
unsigned long ultimaLeitura = 0;
unsigned long ultimoEnvio   = 0;

// Log serial
String logBuffer = "";

// ── Modo calibracao pH ────────────────────────────────────────
bool   modoCalibracaoPH    = false;
unsigned long tUltimaCalib = 0;   // controla frequencia da leitura
String serialInputBuffer   = "";  // buffer para leitura nao-bloqueante

// ============================================================
// FUNCAO: addLog - imprime no Serial e guarda historico
// ============================================================
void addLog(String msg) {
  Serial.println(msg);
  logBuffer = String(millis() / 1000) + "s | " + msg + "\n" + logBuffer;
  if (logBuffer.length() > 1500) {
    logBuffer = logBuffer.substring(0, 1500);
  }
}

// ============================================================
// FUNCAO: setBomba - liga ou desliga a bomba
// ============================================================
void setBomba(bool ligar) {
  digitalWrite(PIN_RELE, ligar ? RELE_LIGA : RELE_DESLIGA);
  bombaLigada = ligar;
}

// ============================================================
// FUNCAO: lerTemperatura - le o sensor DS18B20
// ============================================================
float lerTemperatura() {
  sensors.requestTemperatures();
  float t = sensors.getTempCByIndex(0);
  if (t == DEVICE_DISCONNECTED_C) return -99.0;
  return t;
}

// ============================================================
// FUNCAO: recalcularSlopes
// Recalcula os slopes acido e alcalino a partir das tres tensoes
// de calibracao. Chame sempre que alterar tensaoNeutro,
// tensaoTampao4 ou tensaoTampao10.
// ============================================================
void recalcularSlopes() {
  slopeAcido    = (tensaoTampao4  - tensaoNeutro) / 3.0;
  slopeAlcalino = (tensaoNeutro   - tensaoTampao10) / 3.0;
}

// ============================================================
// FUNCAO: lerPH - le o sensor PH-4502C (media de 10 leituras)
// Dual-slope: usa dois slopes diferentes para meios acido/alcalino,
// resultando em maior precisao em toda a faixa 4-10.
// ============================================================
float lerPH() {
  long soma = 0;
  for (int i = 0; i < 10; i++) {
    soma += analogRead(PIN_PH);
    delay(10);
  }
  float tensao = (soma / 10.0) / 4095.0 * 3.3;
  float pH;
  if (tensao >= tensaoNeutro) {
    // Meio acido (pH < 7): tensao alta
    pH = 7.0 + ((tensaoNeutro - tensao) / slopeAcido) + PH_OFFSET;
  } else {
    // Meio alcalino (pH > 7): tensao baixa
    pH = 7.0 + ((tensaoNeutro - tensao) / slopeAlcalino) + PH_OFFSET;
  }
  return constrain(pH, 0.0, 14.0);
}

// ============================================================
// FUNCAO: controlarBomba - ciclo automatico ou modo manual
// ============================================================
void controlarBomba() {
  if (modoManualBomba) {
    setBomba(estadoManualBomba);
    return;
  }
  unsigned long agora      = millis();
  unsigned long tempoLiga  = BOMBA_TEMPO_LIGA    * 60000UL;
  unsigned long tempoPausa = BOMBA_TEMPO_DESLIGA * 60000UL;

  if (bombaEmCiclo) {
    if ((agora - tempoLigouBomba) >= tempoLiga) {
      setBomba(false);
      bombaEmCiclo    = false;
      tempoLigouBomba = agora;
      addLog("[BOMBA] Pausa de " + String(BOMBA_TEMPO_DESLIGA) + " min");
    }
  } else {
    if ((agora - tempoLigouBomba) >= tempoPausa) {
      setBomba(true);
      bombaEmCiclo    = true;
      tempoLigouBomba = agora;
      addLog("[BOMBA] Ligando por " + String(BOMBA_TEMPO_LIGA) + " min");
    }
  }
}

// ============================================================
// FUNCAO: posicaoDoDia - graus do servo para o dia X
// Retorna o valor de posicoesDias[dia], nunca acima de 180.
// ============================================================
int posicaoDoDia(int dia) {
  if (dia < 0 || dia >= NUM_DIAS) return SERVO_HOME;
  return constrain(posicoesDias[dia], 0, SERVO_MAX_GRAUS);
}

// ============================================================
// FUNCAO: moverServo - move o servo para uma posicao
// ============================================================
void moverServo(int graus) {
  posicaoAtual         = graus;
  estadoAlimentador    = ESTADO_MOVENDO;
  tempoInicioMovimento = millis();
  servoAlimentador.write(graus);
}

// ============================================================
// FUNCAO: controlarAlimentador - ciclo de 7 dias
//
// SEQUENCIA COMPLETA DO CICLO (com GRAUS_POR_DIA=25):
//   Dia 1 (diaAtual=0): moverServo(25°)  → espera 24h
//   Dia 2 (diaAtual=1): moverServo(50°)  → espera 24h
//   ...
//   Dia 7 (diaAtual=6): moverServo(175°) → ESPERA 10s → moverServo(0°)
//                        → ESTADO_AGUARDANDO (aguarda recarga)
//
// ESTADOS:
//   ESTADO_MOVENDO    → servo em movimento (aguarda SERVO_TEMPO_MS)
//   ESTADO_ATIVO      → ciclo ativo, aguardando proximo intervalo
//   ESTADO_AGUARDANDO → ciclo completo, aguardando recarga manual
//
// FLAGS AUXILIARES:
//   retornarHome          → sinaliza que Dia 7 foi concluido
//   esperandoRetornoHome  → contando 10s antes de ir ao home
// ============================================================
void controlarAlimentador() {
  unsigned long agora = millis();

  // ── 1. Servo em movimento: aguarda SERVO_TEMPO_MS ─────────
  if (estadoAlimentador == ESTADO_MOVENDO) {
    if ((agora - tempoInicioMovimento) >= SERVO_TEMPO_MS) {

      if (retornarHome) {
        // Dia 7 acabou de ser atingido (servo chegou em 210°).
        // Inicia a contagem de 10 segundos ANTES de ir ao home.
        retornarHome = false;
        esperandoRetornoHome = true;
        tempoInicioEspera = agora;
        estadoAlimentador = ESTADO_ATIVO; // ativo mas bloqueado pela flag
        addLog("[ALIM] Dia 7 concluido! Aguardando " + String(ESPERA_DIA7_MS/1000) + "s antes de retornar ao home...");
        return;
      }

      // Verificar se chegou ao home apos ciclo completo
      if (posicaoAtual == SERVO_HOME && ciclosCompletos > 0) {
        estadoAlimentador = ESTADO_AGUARDANDO;
        addLog("[ALIM] Home atingido. Coloque racao e clique 'Iniciar ciclo' no dashboard.");
      } else {
        estadoAlimentador = ESTADO_ATIVO;
      }
    }
    return;
  }

  // ── 2. Aguardando recarga: nao faz nada ───────────────────
  if (estadoAlimentador == ESTADO_AGUARDANDO) return;

  // ── 3. Ciclo ativo ────────────────────────────────────────

  // 3a. Esperando os 10 segundos apos o Dia 7
  if (esperandoRetornoHome) {
    if ((agora - tempoInicioEspera) >= ESPERA_DIA7_MS) {
      esperandoRetornoHome = false;
      addLog("[ALIM] 10s concluidos. Retornando ao home (0 graus)...");
      moverServo(SERVO_HOME);
    }
    return; // fica parado neste estado ate os 10s terminarem
  }

  // 3b. Verifica se chegou a hora da proxima alimentacao
  if ((agora - tempoUltimaAlimentacao) >= INTERVALO_MS) {
    tempoUltimaAlimentacao = agora;

    int graus = posicaoDoDia(diaAtual);
    addLog("[ALIM] *** Dia " + String(diaAtual + 1) + " *** → " + String(graus) + " graus");
    moverServo(graus);
    contadorAlimentacoes++;
    diaAtual++;

    if (diaAtual >= NUM_DIAS) {
      // Todos os 7 dias foram alimentados.
      // Aguarda o servo chegar em 210° (SERVO_TEMPO_MS via ESTADO_MOVENDO),
      // depois esperandoRetornoHome aguarda 10s, depois vai ao home.
      diaAtual = 0;
      ciclosCompletos++;
      retornarHome = true;
      addLog("[ALIM] Ciclo " + String(ciclosCompletos) + " completo! Dia 7 sendo servido...");
    }
  }
}

// ============================================================
// FUNCAO: executarComando - processa comandos do backend
//
// Comandos disponiveis (enviados pelo dashboard → Python → ESP32):
//   "bomba_ligar"    - liga a bomba manualmente
//   "bomba_desligar" - desliga a bomba manualmente
//   "bomba_auto"     - volta a bomba para o ciclo automatico
//   "alimentar_agora"- dispara alimentacao imediatamente
//   "iniciar_ciclo"  - inicia novo ciclo apos recarga
//   "ir_home"        - move servo para posicao 0 (home)
// ============================================================
void executarComando(String cmd) {
  cmd.trim();
  addLog("[CMD] Executando: " + cmd);

  if (cmd == "bomba_ligar") {
    modoManualBomba   = true;
    estadoManualBomba = true;
    setBomba(true);
    addLog("[BOMBA] Manual → LIGADA");

  } else if (cmd == "bomba_desligar") {
    modoManualBomba   = true;
    estadoManualBomba = false;
    setBomba(false);
    addLog("[BOMBA] Manual → DESLIGADA");

  } else if (cmd == "bomba_auto") {
    modoManualBomba = false;
    tempoLigouBomba = millis();
    bombaEmCiclo    = true;
    setBomba(true);
    addLog("[BOMBA] Modo automatico retomado");

  } else if (cmd == "alimentar_agora") {
    if (estadoAlimentador == ESTADO_MOVENDO) {
      addLog("[ALIM] Ignorado - servo em movimento, tente em instantes");
    } else {
      // Se estava aguardando recarga, inicia o ciclo automaticamente
      if (estadoAlimentador == ESTADO_AGUARDANDO) {
        diaAtual          = 0;
        estadoAlimentador = ESTADO_ATIVO;
        addLog("[ALIM] Ciclo iniciado automaticamente pelo alimentar_agora");
      }
      // Forca a alimentacao imediata
      tempoUltimaAlimentacao = millis() - INTERVALO_MS;
      addLog("[ALIM] Alimentacao forcada pelo dashboard");
    }

  } else if (cmd == "iniciar_ciclo") {
    if (estadoAlimentador == ESTADO_AGUARDANDO) {
      diaAtual               = 0;
      estadoAlimentador      = ESTADO_ATIVO;
      tempoUltimaAlimentacao = millis();
      addLog("[ALIM] Novo ciclo iniciado pelo dashboard");
    } else {
      addLog("[ALIM] Ignorado (alimentador ja esta ativo)");
    }

  } else if (cmd == "ir_home") {
    // Cancela qualquer espera de retorno pendente
    retornarHome = false;
    esperandoRetornoHome = false;
    addLog("[ALIM] Indo para home manualmente");
    moverServo(SERVO_HOME);

  } else if (cmd.startsWith("servo_")) {
    // Controle direto do angulo: "servo_090" = mover para 90 graus
    // Usado pelo painel de ajuste fisico do dashboard
    int angulo = cmd.substring(6).toInt();
    angulo = constrain(angulo, 0, SERVO_MAX_GRAUS);
    addLog("[SERVO] Movimento manual → " + String(angulo) + " graus");
    posicaoAtual = angulo;
    servoAlimentador.write(angulo);

  } else if (cmd.startsWith("set_posicoes_")) {
    // Atualiza posicoes de todos os dias de uma vez.
    // Formato: "set_posicoes_25_50_75_100_125_150_175"
    // (7 valores separados por _ apos o prefixo)
    String resto = cmd.substring(13); // remove "set_posicoes_"
    int diasAtualizados = 0;
    for (int i = 0; i < NUM_DIAS; i++) {
      int sep   = resto.indexOf('_');
      String part = (sep == -1) ? resto : resto.substring(0, sep);
      int graus = constrain(part.toInt(), 0, SERVO_MAX_GRAUS);
      posicoesDias[i] = graus;
      diasAtualizados++;
      if (sep == -1) break;
      resto = resto.substring(sep + 1);
    }
    addLog("[DPOS] " + String(diasAtualizados) + " posicoes atualizadas:");
    for (int i = 0; i < NUM_DIAS; i++) {
      addLog("  Dia " + String(i + 1) + ": " + String(posicoesDias[i]) + " graus");
    }

  } else {
    addLog("[CMD] Comando desconhecido: " + cmd);
  }
}

// ============================================================
// FUNCAO: mostrarLeituraCalibracaoPH
// Imprime leitura detalhada do sensor no Serial.
// Chamada a cada 1 segundo durante o modo calibracao.
// ============================================================
void mostrarLeituraCalibracaoPH() {
  long soma = 0;
  for (int i = 0; i < 20; i++) {   // media de 20 amostras = mais estavel
    soma += analogRead(PIN_PH);
    delay(5);
  }
  float adcMedio = soma / 20.0;
  float tensao   = (adcMedio / 4095.0) * 3.3;
  float pH;
  if (tensao >= tensaoNeutro) {
    pH = 7.0 + ((tensaoNeutro - tensao) / slopeAcido) + PH_OFFSET;
  } else {
    pH = 7.0 + ((tensaoNeutro - tensao) / slopeAlcalino) + PH_OFFSET;
  }
  pH = constrain(pH, 0.0, 14.0);

  Serial.println(F("----------------------------------------------"));
  Serial.print(F("  ADC bruto (0-4095): ")); Serial.println((int)adcMedio);
  Serial.print(F("  Tensao GPIO25:      ")); Serial.print(tensao, 4);  Serial.println(F(" V"));
  Serial.print(F("  >> pH calculado:    ")); Serial.println(pH, 2);
  Serial.println(F("  --- Calibracao ativa ---"));
  Serial.print(F("  neutro   (pH 7.0): ")); Serial.print(tensaoNeutro,   4); Serial.println(F(" V"));
  Serial.print(F("  acido    (pH 4.0): ")); Serial.print(tensaoTampao4,  4); Serial.println(F(" V"));
  Serial.print(F("  alcalino (pH10.0): ")); Serial.print(tensaoTampao10, 4); Serial.println(F(" V"));
  Serial.print(F("  slopeAcido:        ")); Serial.println(slopeAcido,    4);
  Serial.print(F("  slopeAlcalino:     ")); Serial.println(slopeAlcalino, 4);
  Serial.print(F("  offset fino:       ")); Serial.println(PH_OFFSET, 3);
  Serial.println(F("----------------------------------------------"));
}

// ============================================================
// FUNCAO: processarComandoSerial
// Interpreta uma linha digitada no Monitor Serial.
// ============================================================
void processarComandoSerial(String linha) {
  linha.trim();

  // ── Entrar em modo calibracao ────────────────────────────
  if (linha.equalsIgnoreCase("Calibrar Sensor PH")) {
    modoCalibracaoPH = true;
    Serial.println(F(""));
    Serial.println(F("=============================================="));
    Serial.println(F("  MODO CALIBRACAO pH ATIVADO (dual-slope)"));
    Serial.println(F("=============================================="));
    Serial.println(F("  Leitura a cada 1 segundo."));
    Serial.println(F("  Mergulhe o eletrodo e anote a tensao exibida."));
    Serial.println(F(""));
    Serial.println(F("  Comandos disponiveis:"));
    Serial.println(F("    neutro X.XXXX   -> tensao medida no pH 7.0"));
    Serial.println(F("    acido X.XXXX    -> tensao medida no pH 4.0"));
    Serial.println(F("    alcalino X.XXXX -> tensao medida no pH 10.0"));
    Serial.println(F("    offset X.XX     -> ajuste fino (+/-)"));
    Serial.println(F("    Concluido       -> sai do modo calibracao"));
    Serial.println(F("=============================================="));
    Serial.println(F(""));
    mostrarLeituraCalibracaoPH();
    return;
  }

  // ── Sair do modo calibracao ──────────────────────────────
  if (linha.equalsIgnoreCase("Concluido")) {
    if (modoCalibracaoPH) {
      modoCalibracaoPH = false;
      Serial.println(F(""));
      Serial.println(F("=============================================="));
      Serial.println(F("  CALIBRACAO CONCLUIDA — Modo normal retomado."));
      Serial.println(F("  Valores em uso:"));
      Serial.print(F("    tensaoNeutro   = ")); Serial.println(tensaoNeutro,   4);
      Serial.print(F("    tensaoTampao4  = ")); Serial.println(tensaoTampao4,  4);
      Serial.print(F("    tensaoTampao10 = ")); Serial.println(tensaoTampao10, 4);
      Serial.print(F("    PH_OFFSET      = ")); Serial.println(PH_OFFSET,      3);
      Serial.print(F("    slopeAcido     = ")); Serial.println(slopeAcido,     4);
      Serial.print(F("    slopeAlcalino  = ")); Serial.println(slopeAlcalino,  4);
      Serial.println(F("  Para que esses valores persistam apos"));
      Serial.println(F("  reiniciar, copie-os para o .ino e regrave."));
      Serial.println(F("=============================================="));
      Serial.println(F(""));
    } else {
      Serial.println(F("[SERIAL] Nenhuma calibracao ativa."));
    }
    return;
  }

  // ── tensaoNeutro (pH 7.0) ────────────────────────────────
  if (linha.startsWith("neutro ") || linha.startsWith("neutro\t")) {
    float val = linha.substring(7).toFloat();
    if (val >= 0.5 && val <= 3.3) {
      tensaoNeutro = val;
      recalcularSlopes();
      Serial.print(F("[CAL] tensaoNeutro atualizada: ")); Serial.print(tensaoNeutro, 4); Serial.println(F(" V"));
      Serial.print(F("[CAL] slopeAcido:              ")); Serial.println(slopeAcido,    4);
      Serial.print(F("[CAL] slopeAlcalino:           ")); Serial.println(slopeAlcalino, 4);
    } else {
      Serial.println(F("[CAL] Valor invalido para neutro. Use entre 0.5 e 3.3 V"));
    }
    return;
  }

  // ── tensaoTampao4 (pH 4.0) ───────────────────────────────
  if (linha.startsWith("acido ") || linha.startsWith("acido\t")) {
    float val = linha.substring(6).toFloat();
    if (val >= 0.5 && val <= 3.3) {
      tensaoTampao4 = val;
      recalcularSlopes();
      Serial.print(F("[CAL] tensaoTampao4 atualizada: ")); Serial.print(tensaoTampao4, 4); Serial.println(F(" V"));
      Serial.print(F("[CAL] slopeAcido:               ")); Serial.println(slopeAcido,   4);
    } else {
      Serial.println(F("[CAL] Valor invalido para acido. Use entre 0.5 e 3.3 V"));
    }
    return;
  }

  // ── tensaoTampao10 (pH 10.0) ─────────────────────────────
  if (linha.startsWith("alcalino ") || linha.startsWith("alcalino\t")) {
    float val = linha.substring(9).toFloat();
    if (val >= 0.5 && val <= 3.3) {
      tensaoTampao10 = val;
      recalcularSlopes();
      Serial.print(F("[CAL] tensaoTampao10 atualizada: ")); Serial.print(tensaoTampao10, 4); Serial.println(F(" V"));
      Serial.print(F("[CAL] slopeAlcalino:             ")); Serial.println(slopeAlcalino,   4);
    } else {
      Serial.println(F("[CAL] Valor invalido para alcalino. Use entre 0.5 e 3.3 V"));
    }
    return;
  }

  // ── Ajuste de offset fino ─────────────────────────────────
  if (linha.startsWith("offset ") || linha.startsWith("offset\t")) {
    float val = linha.substring(7).toFloat();
    if (val >= -3.0 && val <= 3.0) {
      PH_OFFSET = val;
      Serial.print(F("[CAL] PH_OFFSET atualizado: ")); Serial.println(PH_OFFSET, 3);
    } else {
      Serial.println(F("[CAL] Valor de offset invalido. Use entre -3.0 e +3.0"));
    }
    return;
  }

  // ── Comando desconhecido ─────────────────────────────────
  if (linha.length() > 0) {
    Serial.print(F("[SERIAL] Comando desconhecido: ")); Serial.println(linha);
    if (modoCalibracaoPH) {
      Serial.println(F("[SERIAL] Durante calibracao use:"));
      Serial.println(F("  neutro X.XXXX | acido X.XXXX | alcalino X.XXXX | offset X.XX | Concluido"));
    } else {
      Serial.println(F("[SERIAL] Fora da calibracao use: Calibrar Sensor PH"));
    }
  }
}

// ============================================================
// FUNCAO: lerComandoSerial
// Leitura nao-bloqueante do Monitor Serial.
// Acumula caracteres ate receber newline, entao processa.
// ============================================================
void lerComandoSerial() {
  while (Serial.available() > 0) {
    char c = (char)Serial.read();
    if (c == '\n' || c == '\r') {
      if (serialInputBuffer.length() > 0) {
        processarComandoSerial(serialInputBuffer);
        serialInputBuffer = "";
      }
    } else {
      serialInputBuffer += c;
      if (serialInputBuffer.length() > 80) { // protecao contra overflow
        serialInputBuffer = "";
      }
    }
  }
}

// ============================================================
// FUNCAO: conectarWiFi - conecta ou reconecta ao WiFi da casa
// ============================================================
void conectarWiFi() {
  if (WiFi.status() == WL_CONNECTED) return;

  addLog("[WiFi] Conectando a: " + String(WIFI_SSID));
  WiFi.mode(WIFI_STA);
  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);

  int tentativas = 0;
  while (WiFi.status() != WL_CONNECTED && tentativas < 30) {
    delay(500);
    Serial.print(".");
    tentativas++;
  }
  Serial.println();

  if (WiFi.status() == WL_CONNECTED) {
    addLog("[WiFi] Conectado! IP do ESP32: " + WiFi.localIP().toString());
  } else {
    addLog("[WiFi] FALHOU - sistema continua funcionando offline");
    addLog("[WiFi] Bomba e alimentador continuam operando normalmente");
  }
}

// ============================================================
// FUNCAO: enviarDados - envia sensores e status ao backend
//
// Fluxo:
//   1. Monta JSON com temperatura, pH, bomba, alimentador
//   2. Faz POST para BACKEND_URL/dados
//   3. Backend salva no SQLite e retorna comando pendente (se houver)
//   4. ESP32 executa o comando recebido
//
// Formato do JSON enviado:
//   {
//     "temperatura": 25.40,
//     "ph": 7.10,
//     "bomba": true,
//     "estado": "ativo",
//     "diaAtual": 3,
//     "alimentacoes": 2,
//     "ciclos": 0
//   }
//
// Formato da resposta do backend:
//   { "status": "ok", "comando": null }          - sem comando
//   { "status": "ok", "comando": "bomba_ligar" } - com comando
// ============================================================
void enviarDados() {
  // Verifica conexao antes de tentar
  if (WiFi.status() != WL_CONNECTED) {
    conectarWiFi();
    if (WiFi.status() != WL_CONNECTED) return; // ainda sem WiFi, pula
  }

  // Monta o estado do alimentador como texto
  String estado;
  if      (estadoAlimentador == ESTADO_AGUARDANDO) estado = "aguardando";
  else if (estadoAlimentador == ESTADO_MOVENDO)    estado = "movendo";
  else                                             estado = "ativo";

  // Monta o JSON manualmente (sem biblioteca extra)
  String payload = "{";
  payload += "\"temperatura\":"  + String(temperatura, 2)               + ",";
  payload += "\"ph\":"            + String(ph, 2)                        + ",";
  payload += "\"bomba\":"         + String(bombaLigada ? "true":"false") + ",";
  payload += "\"estado\":\""      + estado                               + "\",";
  payload += "\"diaAtual\":"      + String(diaAtual + 1)                 + ",";
  payload += "\"alimentacoes\":"  + String(contadorAlimentacoes)         + ",";
  payload += "\"ciclos\":"        + String(ciclosCompletos)              + ",";
  payload += "\"posicaoServo\":"  + String(posicaoAtual);
  payload += "}";

  // Faz a requisicao HTTP POST
  HTTPClient http;
  String url = String(BACKEND_URL) + "/dados";
  http.begin(url);
  http.addHeader("Content-Type", "application/json");
  http.setTimeout(3000); // timeout de 3 segundos

  int httpCode = http.POST(payload);

  if (httpCode == 200) {
    String resposta = http.getString();

    // ── Parse robusto do campo "comando" ──────────────────────
    // O Flask pode gerar tanto  "comando":"valor"  quanto
    // "comando": "valor"  (com espaco apos os dois-pontos).
    // A estrategia:
    //   1. Encontra a chave  "comando":
    //   2. A partir dai, busca a PRIMEIRA aspas de abertura (")
    //      ignorando espacos e o "null" caso nao haja comando
    //   3. Captura tudo ate a proxima aspas de fechamento
    // Se o valor for null (sem aspas), nao encontra aspas e ignora.

    int idxChave = resposta.indexOf("\"comando\":");
    if (idxChave != -1) {
      int posApos = idxChave + 10; // posicao logo apos  "comando":
      // Avanca sobre espacos em branco
      while (posApos < resposta.length() && resposta[posApos] == ' ') {
        posApos++;
      }
      // So processa se o proximo char for aspas (valor string)
      // Se for 'n' (null) ou outro char, nao e um comando valido
      if (posApos < resposta.length() && resposta[posApos] == '"') {
        int inicio = posApos + 1;
        int fim    = resposta.indexOf("\"", inicio);
        if (fim != -1 && fim > inicio) {
          String cmd = resposta.substring(inicio, fim);
          cmd.trim();
          addLog("[HTTP] Comando recebido do backend: " + cmd);
          executarComando(cmd);
        }
      }
    }

  } else if (httpCode < 0) {
    addLog("[HTTP] Erro de conexao: " + String(httpCode) + " (backend offline?)");
  } else {
    addLog("[HTTP] Erro HTTP: " + String(httpCode));
  }

  http.end();
}

// ============================================================
// SETUP - executa uma vez quando o ESP32 liga
// ============================================================
void setup() {
  Serial.begin(115200);
  delay(500);
  addLog("========================================");
  addLog("[BOOT] AquaMaster v3.0 iniciando...");
  addLog("========================================");

  // --- Rele / Bomba ---
  digitalWrite(PIN_RELE, RELE_DESLIGA); // garante bomba desligada antes de configurar
  pinMode(PIN_RELE, OUTPUT);
  digitalWrite(PIN_RELE, RELE_DESLIGA);
  delay(300);
  addLog("[RELE] Configurado");

  // --- Servo Motor ---
  servoAlimentador.attach(PIN_SERVO);
  servoAlimentador.write(SERVO_HOME);
  posicaoAtual = SERVO_HOME;
  addLog("[SERVO] Em home (0 graus)");
  delay(500);

  // --- Sensores ---
  sensors.begin();
  addLog("[TEMP] Sensores encontrados: " + String(sensors.getDeviceCount()));
  analogReadResolution(12);
  // IMPORTANTE: o pino de pH usa 0dB (faixa real 0-1.1V).
  // Os valores de calibracao (tensaoNeutro, tensaoTampao4, tensaoTampao10)
  // foram medidos com o codigo Lab Inventores que nao define atenuacao,
  // usando o padrao 0dB do ESP32. A formula escala 0-4095 para 0-3.3V,
  // mas o ADC so ve 0-1.1V de verdade — gerando os valores "inflados" de
  // 2.5 / 3.3 / 2.007 que o usuario coletou.
  // NAO mude para ADC_11db sem refazer toda a calibracao.
  //analogSetPinAttenuation(PIN_PH, ADC_0db);
  delay(200);

  // --- Calibracao pH: calcula slopes a partir das tensoes configuradas ---
  recalcularSlopes();
  addLog("[pH] Calibracao dual-slope carregada:");
  addLog("     neutro=" + String(tensaoNeutro, 4) + "V  acido=" + String(tensaoTampao4, 4) + "V  alcalino=" + String(tensaoTampao10, 4) + "V");
  addLog("     slopeAcido=" + String(slopeAcido, 4) + "  slopeAlcalino=" + String(slopeAlcalino, 4));

  // --- WiFi ---
  conectarWiFi();

  // --- Primeira leitura dos sensores ---
  temperatura = lerTemperatura();
  ph          = lerPH();
  addLog("[SENSOR] T=" + String(temperatura, 1) + "C  pH=" + String(ph, 2));

  // --- Liga a bomba e inicia ciclo ---
  delay(800);
  setBomba(true);
  bombaEmCiclo    = true;
  tempoLigouBomba = millis();
  addLog("[BOMBA] Ligada - ciclo automatico iniciado");

  // --- Inicia ciclo do alimentador ---
  tempoUltimaAlimentacao = millis();
  estadoAlimentador      = ESTADO_ATIVO;
  diaAtual               = 0;
  addLog("[ALIM] Ciclo do alimentador iniciado");

  // --- Primeiro envio de dados ---
  ultimaLeitura = millis();
  ultimoEnvio   = millis();
  enviarDados();

  addLog("[OK] AquaMaster pronto!");
  addLog("     Backend: " + String(BACKEND_URL));
  addLog("     WiFi: "    + String(WIFI_SSID));
}

// ============================================================
// LOOP - executa continuamente
// ============================================================
void loop() {
  unsigned long agora = millis();

  // --- Sempre: verifica se o usuario digitou algo no Serial ---
  lerComandoSerial();

  // --- Modo calibracao pH: leitura detalhada a cada 1 segundo ---
  if (modoCalibracaoPH) {
    if (agora - tUltimaCalib >= 1000) {
      tUltimaCalib = agora;
      mostrarLeituraCalibracaoPH();
    }
    // Bomba e alimentador continuam funcionando por seguranca.
    // Envio ao backend fica pausado para nao poluir o Serial.
    static unsigned long tControleCalib = 0;
    if (agora - tControleCalib >= 1000) {
      tControleCalib = agora;
      controlarBomba();
      controlarAlimentador();
    }
    return; // pula o bloco normal abaixo
  }

  // --- Operacao normal ---

  // A cada 5 segundos: le sensores e envia para o backend
  if (agora - ultimaLeitura >= INTERVALO_ENVIO_MS) {
    ultimaLeitura = agora;
    temperatura   = lerTemperatura();
    ph            = lerPH();
    enviarDados(); // envia dados e recebe comandos pendentes
  }

  // A cada 1 segundo: controla bomba e alimentador
  static unsigned long tControle = 0;
  if (agora - tControle >= 1000) {
    tControle = agora;
    controlarBomba();
    controlarAlimentador();
  }
}