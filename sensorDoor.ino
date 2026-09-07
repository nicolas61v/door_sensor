// Sensor de puerta + PIR BISS0001 (modulo HC-SR501) para ESP32
//
// ---------------------------------------------------------------------------
// COMPORTAMIENTO REAL DEL MODULO (medido, no supuesto)
// ---------------------------------------------------------------------------
// Medido con calibracionPIR/calibracionPIR.ino, con el pot TIME al minimo:
//
//   - Cada pulso de salida dura SIEMPRE 0.7s, sin importar cuanto te muevas.
//   - Entre pulso y pulso hay ~2.3s en los que el modulo esta ciego
//     (blocking time del BISS0001).
//   - Con alguien moviendose sin parar salen pulsos de 0.7s cada ~3s, muy
//     regulares. Con un falso positivo (un bicho) sale UN pulso suelto.
//
// Que TODOS los pulsos midan exactamente lo mismo significa que el modulo no
// re-extiende la salida: esta en modo L (single trigger). Con esta senal es
// IMPOSIBLE tener HIGH continuo por mas de 0.7s, asi que la logica no puede
// pedir "movimiento continuo durante N segundos": no disparia nunca.
//
// Por eso la confirmacion se hace por TREN DE PULSOS sostenido en el tiempo,
// que es lo que separa de verdad a una persona de un falso positivo:
//
//   persona moviendose -> pulso cada ~3s, muchos seguidos
//   bicho / corriente  -> un pulso solo
//
// Si algun dia pasas el jumper a H y el modulo empieza a dar pulsos largos,
// tambien funciona: un unico pulso de mas de PULSO_LARGO_CONFIRMA_MS confirma
// directo. No hay que tocar nada.
//
// ---------------------------------------------------------------------------
// HARDWARE
// ---------------------------------------------------------------------------
//   - Pot "TIME" AL MINIMO (todo antihorario). Es el ajuste ya calibrado:
//     da Tx = 0.7s. Si lo movés, hay que volver a medir con calibracionPIR.
//   - Pot "SENS" a la mitad. Si hay falsos disparos, bajarlo antes que tocar
//     el codigo.
//   - Alimentacion: VCC del PIR al pin 5V/VIN del ESP32 (lleva un regulador
//     HT7133, con 3V3 no arranca). Su OUT entrega 3.3V: va directo al GPIO.
//   - Warm-up: al energizarse tira HIGH espurios ~30-60s. El codigo arranca en
//     CALENTANDO y los ignora.
//
// ---------------------------------------------------------------------------
// LOGICA
// ---------------------------------------------------------------------------
// - Puerta cerrada -> se abre/reinicia una ventana de VENTANA_DETECCION_MS (30s)
//   en la que hay que confirmar actividad para disparar.
// - Si se confirma dentro de esos 30s, la salida se activa/mantiene en ON y el
//   sistema deja de sensar hasta el proximo cierre de puerta.
// - Si NO se confirma en los 30s, la salida se pone/queda en OFF y la ventana
//   se reinicia sola (sigue sensando sin parar mientras la puerta siga cerrada).
// - Abrir la puerta nunca afecta nada por si sola (ni cancela la ventana, ni
//   apaga la salida): la salida se queda como estaba.
// - Cerrar la puerta SIEMPRE reactiva el sensado, sin importar el estado previo
//   (incluso si ya estaba DISPARADO). Es el resultado de esa nueva ventana el
//   que decide si la salida se queda en ON o pasa a OFF.
// - Respaldo de seguridad: si la puerta queda ABIERTA de forma continua por
//   DOOR_OPEN_RESET_MS (5 minutos) estando DISPARADO, se apaga sola y el
//   sistema vuelve a quedar en espera.

#include <Arduino.h>

// --- Pines (ajustar segun el cableado real) ---
const uint8_t PIN_DOOR_SWITCH = 14;  // Switch de puerta: INPUT_PULLUP, LOW = puerta cerrada
const uint8_t PIN_PIR         = 27;  // Pin OUT del modulo BISS0001/HC-SR501
const uint8_t PIN_OUTPUT      = 2;   // Salida hacia rele/buzzer/LED de alarma
                                     // OJO: GPIO2 es strapping pin y LED onboard.
                                     // Sirve para probar; para un rele conviene
                                     // mover a un GPIO libre (ej. 25, 26, 32, 33).
                                     // Ademas, si algo lo levanta en el arranque,
                                     // impide entrar en modo descarga al flashear.

// --- Caracteristicas medidas del modulo PIR ---
const bool PIR_ACTIVO_EN_ALTO = true;      // HC-SR501: OUT en HIGH = movimiento
const unsigned long PIR_TX_MS      = 700;  // duracion de cada pulso (MEDIDO)
const unsigned long PIR_BLOQUEO_MS = 2500; // ceguera entre pulsos (MEDIDO ~2.2-2.6s)

// Imprime la duracion de cada pulso del PIR. Util para re-calibrar en campo o
// para ver si el modulo cambio de comportamiento. Solo escribe en los flancos.
const bool LOG_PULSOS_PIR = true;

// --- Reglas de confirmacion ---
const uint8_t PULSOS_PARA_CONFIRMAR = 4;      // pulsos del tren para dar por buena la actividad
const unsigned long ACTIVIDAD_MINIMA_MS = 8000;  // el tren ademas debe abarcar este lapso.
                                                 // Con ESTE modulo la condicion no llega a
                                                 // actuar: el blocking time ya obliga a que
                                                 // 4 pulsos tarden >=9.6s. Queda como red de
                                                 // seguridad por si el modulo pasa a pulsar
                                                 // mas rapido (jumper en H, otra placa) y
                                                 // aparecen rafagas cortas.
const unsigned long PAUSA_MAXIMA_MS = 8000;      // hueco sin pulsos que corta el tren
const unsigned long PULSO_LARGO_CONFIRMA_MS = 8000; // un solo pulso tan largo ya confirma
                                                    // (caso jumper en H)

// --- Parametros generales ---
const unsigned long VENTANA_DETECCION_MS = 30000;  // 30s de vigilancia tras cerrar la puerta
const unsigned long DOOR_OPEN_RESET_MS   = 300000; // 5 min de puerta abierta seguida -> apaga
const unsigned long CALENTAMIENTO_MS     = 60000;  // estabilizacion del PIR al energizar
const unsigned long DEBOUNCE_MS          = 50;     // antirrebote del switch mecanico
const unsigned long LOG_PERIODO_MS       = 1000;   // frecuencia del log de estado

// Chequeos de coherencia contra el comportamiento medido del modulo.
static_assert(PAUSA_MAXIMA_MS > PIR_TX_MS + PIR_BLOQUEO_MS,
              "PAUSA_MAXIMA_MS tiene que tolerar un ciclo completo de ceguera del modulo, "
              "si no el tren se corta solo entre pulsos normales y nunca confirma.");
static_assert(PULSOS_PARA_CONFIRMAR >= 2,
              "Con un solo pulso no se distingue una persona de un bicho.");
// El tren completo tiene que entrar comodo en la ventana. Con este modulo N
// pulsos no pueden llegar mas rapido que (N-1)*(Tx+bloqueo), asi que la ventana
// tiene que cubrir eso y todavia dejar margen.
static_assert(VENTANA_DETECCION_MS >
                  (PULSOS_PARA_CONFIRMAR - 1) * (PIR_TX_MS + PIR_BLOQUEO_MS) + PAUSA_MAXIMA_MS,
              "La ventana es muy corta para juntar PULSOS_PARA_CONFIRMAR pulsos al ritmo que "
              "permite el modulo: subir la ventana o bajar la cantidad de pulsos.");
static_assert(VENTANA_DETECCION_MS > ACTIVIDAD_MINIMA_MS + PAUSA_MAXIMA_MS,
              "La ventana debe dar tiempo a formar el tren completo dentro de ella.");

// --- Estados del sistema ---
enum EstadoSistema {
  CALENTANDO, // el PIR todavia se esta estabilizando: se ignora su salida
  IDLE,       // esperando: puerta abierta, o cerrada pero aun sin armar ventana
  ARMADO,     // puerta cerrada, evaluando actividad dentro de la ventana
  DISPARADO   // actividad confirmada, salida fija en ON hasta el reset por puerta abierta
};

EstadoSistema estado = CALENTANDO;

bool puertaCerradaEstable = false;   // estado logico (con antirrebote) del switch
bool ultimaLecturaSwitch  = false;   // ultima lectura cruda leida
unsigned long tUltimoCambioSwitch = 0;

unsigned long tArranque = 0;
unsigned long tInicioArmado = 0;
unsigned long tPuertaAbiertaDesde = 0; // momento en que la puerta paso a ABIERTA
unsigned long tUltimoLog = 0;

// --- Seguimiento del tren de pulsos ---
bool pirNivel = false;               // ultimo nivel logico leido del PIR
uint8_t pulsosTren = 0;              // pulsos acumulados en el tren actual
unsigned long tPrimerPulsoTren = 0;  // inicio del tren actual
unsigned long tPulsoActualDesde = 0; // inicio del pulso en curso
unsigned long tUltimaBajada = 0;     // ultimo flanco de bajada

// Variables propias del medidor de pulsos: separadas a proposito para que el
// diagnostico no toque nada de la maquina de estados.
bool pmNivelAnterior = false;
unsigned long tPmSubida = 0;

const char* nombreEstado(EstadoSistema e) {
  switch (e) {
    case CALENTANDO: return "CALENTANDO";
    case IDLE:       return "IDLE";
    case ARMADO:     return "ARMADO";
    case DISPARADO:  return "DISPARADO";
  }
  return "?";
}

// Lectura del PIR ya normalizada: true = movimiento
bool leerPir() {
  bool nivel = (digitalRead(PIN_PIR) == HIGH);
  return PIR_ACTIVO_EN_ALTO ? nivel : !nivel;
}

// Mide y reporta la duracion de cada pulso. Corre en cualquier estado y no
// modifica nada de la maquina de estados.
void medirPulsosPir() {
  if (!LOG_PULSOS_PIR) return;

  bool pir = leerPir();
  unsigned long ahora = millis();

  if (pir && !pmNivelAnterior) {
    tPmSubida = ahora;
  } else if (!pir && pmNivelAnterior) {
    Serial.print("PIR: pulso de ");
    Serial.print((ahora - tPmSubida) / 1000.0, 1);
    Serial.println("s");
  }

  pmNivelAnterior = pir;
}

void reiniciarTren() {
  pirNivel          = false;
  pulsosTren        = 0;
  tPrimerPulsoTren  = 0;
  tPulsoActualDesde = 0;
  tUltimaBajada     = 0;
}

void armarVentana() {
  estado = ARMADO;
  tInicioArmado = millis();
  reiniciarTren();
}

void setup() {
  Serial.begin(115200);

  pinMode(PIN_DOOR_SWITCH, INPUT_PULLUP);
  pinMode(PIN_PIR, INPUT_PULLDOWN); // si el pin queda flotando (cable suelto),
                                    // por defecto lee LOW = sin movimiento
  pinMode(PIN_OUTPUT, OUTPUT);
  digitalWrite(PIN_OUTPUT, LOW);

  tArranque = millis();

  // Inicializa el estado del switch con la lectura actual para evitar un
  // falso flanco al arrancar
  ultimaLecturaSwitch  = (digitalRead(PIN_DOOR_SWITCH) == LOW);
  puertaCerradaEstable = ultimaLecturaSwitch;
  tPuertaAbiertaDesde  = millis();

  // No se arma nada todavia: el HC-SR501 tira falsos HIGH mientras calienta.
  estado = CALENTANDO;
  reiniciarTren();
  pmNivelAnterior = leerPir(); // evita reportar un pulso falso al arrancar

  Serial.print("Sistema listo. Calentando el PIR ");
  Serial.print(CALENTAMIENTO_MS / 1000);
  Serial.println("s antes de empezar a sensar...");
}

// Devuelve true si hubo un cambio de estado estable del switch (con antirrebote)
bool actualizarSwitchPuerta() {
  bool lecturaActual = (digitalRead(PIN_DOOR_SWITCH) == LOW); // LOW = cerrada

  if (lecturaActual != ultimaLecturaSwitch) {
    tUltimoCambioSwitch = millis();
    ultimaLecturaSwitch = lecturaActual;
  }

  if ((millis() - tUltimoCambioSwitch) > DEBOUNCE_MS &&
      lecturaActual != puertaCerradaEstable) {
    puertaCerradaEstable = lecturaActual;
    return true; // cambio de estado confirmado
  }

  return false;
}

// Sigue el tren de pulsos del PIR. Devuelve true cuando la actividad queda
// confirmada como movimiento real.
bool actualizarActividadPir() {
  unsigned long ahora = millis();
  bool pir = leerPir();

  if (pir && !pirNivel) {          // flanco de subida: empieza un pulso
    // Si paso demasiado tiempo desde el ultimo pulso, el tren anterior murio
    // y este pulso arranca uno nuevo.
    if (pulsosTren > 0 && (ahora - tUltimaBajada) > PAUSA_MAXIMA_MS) {
      pulsosTren = 0;
    }
    if (pulsosTren == 0) {
      tPrimerPulsoTren = ahora;
    }
    if (pulsosTren < 255) {
      pulsosTren++;
    }
    tPulsoActualDesde = ahora;

  } else if (!pir && pirNivel) {   // flanco de bajada: termina el pulso
    tUltimaBajada = ahora;
  }

  // Estando en bajo, el tren caduca si la pausa se hace demasiado larga
  if (!pir && pulsosTren > 0 && (ahora - tUltimaBajada) > PAUSA_MAXIMA_MS) {
    pulsosTren = 0;
  }

  pirNivel = pir;

  // Caso normal (modo L): varios pulsos repartidos en el tiempo.
  bool porTren = (pulsosTren >= PULSOS_PARA_CONFIRMAR) &&
                 ((ahora - tPrimerPulsoTren) >= ACTIVIDAD_MINIMA_MS);

  // Caso jumper en H: un unico pulso sostenido ya alcanza.
  bool porPulsoLargo = pir && ((ahora - tPulsoActualDesde) >= PULSO_LARGO_CONFIRMA_MS);

  return porTren || porPulsoLargo;
}

void loop() {
  medirPulsosPir();

  bool huboCambioPuerta = actualizarSwitchPuerta();

  if (huboCambioPuerta) {
    if (puertaCerradaEstable) {
      if (estado == CALENTANDO) {
        // Todavia no se confia en el PIR: se anota el cierre y nada mas.
        Serial.println("Puerta cerrada (el PIR sigue calentando, aun no se sensa).");
      } else {
        // Cerrar la puerta SIEMPRE reactiva el sensado, sin importar el estado
        // previo. La salida NO se toca aqui: la decide el resultado de esta
        // nueva ventana (ver bloque ARMADO mas abajo).
        armarVentana();
        Serial.print("Puerta cerrada. Reactivando sensado del PIR (");
        Serial.print(VENTANA_DETECCION_MS / 1000);
        Serial.println("s)...");
      }
    } else {
      // Abrir la puerta NO afecta el sensado ni la salida en ningun estado.
      Serial.println("Puerta abierta.");
      tPuertaAbiertaDesde = millis();
    }
  }

  if (estado == CALENTANDO) {
    if (millis() - tArranque >= CALENTAMIENTO_MS) {
      if (puertaCerradaEstable) {
        armarVentana();
        Serial.println("PIR estabilizado. Puerta cerrada: armando ventana de deteccion.");
      } else {
        estado = IDLE;
        Serial.println("PIR estabilizado. Puerta abierta: en espera.");
      }
    }
  } else if (estado == ARMADO) {
    if (actualizarActividadPir()) {
      estado = DISPARADO;
      digitalWrite(PIN_OUTPUT, HIGH);
      Serial.print("Actividad confirmada (");
      Serial.print(pulsosTren);
      Serial.print(" pulsos en ");
      Serial.print((millis() - tPrimerPulsoTren) / 1000.0, 1);
      Serial.println("s). Salida activada y fija en ON.");
    }

    if (estado == ARMADO && millis() - tInicioArmado >= VENTANA_DETECCION_MS) {
      // No se confirmo actividad en esta ventana: si la salida venia encendida
      // de un ciclo anterior, se apaga aqui. La puerta sigue cerrada, asi que
      // se reinicia la ventana para seguir sensando sin parar.
      digitalWrite(PIN_OUTPUT, LOW);
      tInicioArmado = millis();
      reiniciarTren();
      Serial.println("Ventana cumplida sin actividad sostenida. Salida en OFF. Reiniciando ventana.");
    }
  } else if (estado == DISPARADO) {
    // Unica salida de este estado: puerta abierta de forma continua 5 minutos.
    if (!puertaCerradaEstable && (millis() - tPuertaAbiertaDesde >= DOOR_OPEN_RESET_MS)) {
      digitalWrite(PIN_OUTPUT, LOW);
      estado = IDLE;
      Serial.println("Puerta abierta 5 minutos seguidos. Alarma apagada. Sistema en espera.");
    }
  }

  // --- Log periodico de estado, para ver en vivo por el Monitor Serie ---
  if (millis() - tUltimoLog >= LOG_PERIODO_MS) {
    tUltimoLog = millis();

    Serial.print("[t=");
    Serial.print(millis() / 1000);
    Serial.print("s] estado=");
    Serial.print(nombreEstado(estado));
    Serial.print(" | puerta=");
    Serial.print(puertaCerradaEstable ? "CERRADA" : "ABIERTA");
    Serial.print(" | pir_raw=");
    Serial.print(leerPir() ? "MOVIMIENTO" : "quieto");
    Serial.print(" | salida=");
    Serial.print(digitalRead(PIN_OUTPUT) == HIGH ? "ON" : "off");

    if (estado == CALENTANDO) {
      long faltaMs = (long)CALENTAMIENTO_MS - (long)(millis() - tArranque);
      if (faltaMs < 0) faltaMs = 0;
      Serial.print(" | calentando_falta=");
      Serial.print(faltaMs / 1000);
      Serial.print("s");
    } else if (estado == ARMADO) {
      long restanteMs = (long)VENTANA_DETECCION_MS - (long)(millis() - tInicioArmado);
      if (restanteMs < 0) restanteMs = 0;
      Serial.print(" | ventana_restante=");
      Serial.print(restanteMs / 1000);
      Serial.print("s | pulsos=");
      Serial.print(pulsosTren);
      Serial.print("/");
      Serial.print(PULSOS_PARA_CONFIRMAR);

      if (pulsosTren > 0) {
        Serial.print(" | tren=");
        Serial.print((millis() - tPrimerPulsoTren) / 1000.0, 1);
        Serial.print("s/");
        Serial.print(ACTIVIDAD_MINIMA_MS / 1000);
        Serial.print("s");
      }
    } else if (estado == DISPARADO && !puertaCerradaEstable) {
      long abiertaMs = (long)(millis() - tPuertaAbiertaDesde);
      long faltaMs = (long)DOOR_OPEN_RESET_MS - abiertaMs;
      if (faltaMs < 0) faltaMs = 0;
      Serial.print(" | puerta_abierta_hace=");
      Serial.print(abiertaMs / 1000);
      Serial.print("s | se_apaga_en=");
      Serial.print(faltaMs / 1000);
      Serial.print("s");
    }

    Serial.println();
  }
}
