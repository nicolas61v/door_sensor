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
// La logica NO intenta distinguir una persona de un falso positivo: basta un
// pulso, el primero que llegue, para disparar. Lo unico que importa de lo
// medido es el ciclo pulso+ceguera (~3.2s), porque marca cada cuanto puede el
// modulo avisar y por lo tanto cuanto tiene que durar como minimo la ventana.
//
// ---------------------------------------------------------------------------
// HARDWARE
// ---------------------------------------------------------------------------
//   - Pot "TIME" AL MINIMO (todo antihorario). Es el ajuste ya calibrado:
//     da Tx = 0.7s. Si lo movés, hay que volver a medir con calibracionPIR.
//   - Pot "SENS" a la mitad. Como ahora ALCANZA UN SOLO PULSO para disparar,
//     este pot es la unica defensa contra falsos positivos: si hay disparos
//     por bichos o corrientes de aire, bajarlo antes que tocar el codigo.
//   - Alimentacion: VCC del PIR al pin 5V/VIN del ESP32 (lleva un regulador
//     HT7133, con 3V3 no arranca). Su OUT entrega 3.3V: va directo al GPIO.
//   - LED testigo en GPIO2: en la mayoria de los DevKit ya viene el LED
//     onboard ahi, asi que no hay que cablear nada. Si tu placa no lo trae,
//     LED + resistencia de 220-330 ohm de GPIO2 a GND.
//   - Warm-up: al energizarse tira HIGH espurios. El codigo arranca en
//     CALENTANDO durante CALENTAMIENTO_MS y los ignora.
//
// ---------------------------------------------------------------------------
// LOGICA
// ---------------------------------------------------------------------------
// El sensor NO esta escuchando todo el tiempo: solo sensa durante la ventana de
// un minuto que abre el cierre de la puerta. Fuera de esa ventana el PIR se
// ignora por completo.
//
// - Cerrar la puerta -> se abre una ventana de VENTANA_DETECCION_MS (1 min) en
//   la que hay que detectar movimiento para disparar.
// - BASTA UN SOLO PULSO del PIR: apenas detecta algo, dispara. No se exige tren
//   de pulsos ni duracion minima. Es a proposito: se prioriza no perderse
//   ninguna deteccion por encima de filtrar falsos positivos.
// - Si detecta dentro de ese minuto, la salida se activa/mantiene en ON y el
//   sistema deja de sensar.
// - Si NO detecta nada en el minuto, la salida se pone/queda en OFF y el sistema
//   tambien deja de sensar. La ventana NO se reinicia sola: aunque la puerta
//   siga cerrada, el PIR queda ignorado.
// - En los dos casos el sistema queda dormido hasta el proximo ciclo de puerta:
//   hay que abrirla y volver a cerrarla para que se arme una ventana nueva.
// - Abrir la puerta nunca afecta nada por si sola (ni arma la ventana, ni
//   cancela una en curso, ni apaga la salida): la salida se queda como estaba y
//   el sensado recien arranca cuando la puerta se vuelve a cerrar.
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
const uint8_t PIN_OUTPUT      = 26;  // Salida hacia rele/buzzer/LED de alarma.
                                     // GPIO26 es un pin libre, sin funcion de
                                     // arranque. Antes esto estaba en GPIO2, que
                                     // es strapping pin y comparte el LED
                                     // onboard: con un rele colgado ahi, su
                                     // pull-up puede impedir entrar en modo
                                     // descarga al flashear. NO volver a GPIO2.

const uint8_t PIN_LED_TESTIGO = 2;   // LED testigo: espeja SIEMPRE el estado de
                                     // PIN_OUTPUT para poder ver a simple vista
                                     // cuando se activa la salida.
                                     // GPIO2 es strapping pin, pero aca no hay
                                     // problema: es el LED onboard de la placa
                                     // (o un LED con su resistencia), no un rele.
                                     // Lo que rompia el flasheo era el pull-up
                                     // del rele, no el LED. Aun asi, NO colgar
                                     // el rele de este pin.

// --- Caracteristicas medidas del modulo PIR ---
const bool PIR_ACTIVO_EN_ALTO = true;      // HC-SR501: OUT en HIGH = movimiento
const unsigned long PIR_TX_MS      = 700;  // duracion de cada pulso (MEDIDO)
const unsigned long PIR_BLOQUEO_MS = 2500; // ceguera entre pulsos (MEDIDO ~2.2-2.6s)

// Imprime la duracion de cada pulso del PIR. Util para re-calibrar en campo o
// para ver si el modulo cambio de comportamiento. Solo escribe en los flancos.
const bool LOG_PULSOS_PIR = true;

// --- Parametros generales ---
const unsigned long VENTANA_DETECCION_MS = 60000;  // 1 min de vigilancia tras cerrar la puerta
const unsigned long DOOR_OPEN_RESET_MS   = 300000; // 5 min de puerta abierta seguida -> apaga
const unsigned long CALENTAMIENTO_MS     = 10000;  // estabilizacion del PIR al energizar.
                                                   // La hoja del HC-SR501 pide 30-60s, pero
                                                   // con 60s el sistema quedaba mudo el primer
                                                   // minuto tras cada flasheo/reset. Si al
                                                   // arrancar aparecen disparos espurios,
                                                   // subir esto es lo primero a probar.
const unsigned long DEBOUNCE_MS          = 50;     // antirrebote del switch mecanico
const unsigned long LOG_PERIODO_MS       = 1000;   // frecuencia del log de estado

// Chequeo de coherencia contra el comportamiento medido del modulo: la ventana
// tiene que cubrir de sobra un ciclo completo (pulso + ceguera). Si fuera mas
// corta podria abrirse y cerrarse entera mientras el PIR esta ciego, y perderse
// a alguien que si se estaba moviendo.
static_assert(VENTANA_DETECCION_MS > 2 * (PIR_TX_MS + PIR_BLOQUEO_MS),
              "La ventana apenas cubre un ciclo de ceguera del modulo: subirla.");

// --- Estados del sistema ---
enum EstadoSistema {
  CALENTANDO, // el PIR todavia se esta estabilizando: se ignora su salida
  IDLE,       // dormido: el PIR se ignora. Se sale de aqui solo al cerrar la puerta
  ARMADO,     // puerta cerrada, esperando movimiento dentro de la ventana
  DISPARADO   // movimiento detectado, salida fija en ON hasta el reset por puerta abierta
};

EstadoSistema estado = CALENTANDO;

bool puertaCerradaEstable = false;   // estado logico (con antirrebote) del switch
bool ultimaLecturaSwitch  = false;   // ultima lectura cruda leida
unsigned long tUltimoCambioSwitch = 0;

unsigned long tArranque = 0;
unsigned long tInicioArmado = 0;
unsigned long tPuertaAbiertaDesde = 0; // momento en que la puerta paso a ABIERTA
unsigned long tUltimoLog = 0;

// --- Seguimiento del PIR ---
bool pirNivel = false;               // ultimo nivel logico leido del PIR

// Variables propias del medidor de pulsos: separadas a proposito para que el
// diagnostico no toque nada de la maquina de estados.
bool pmNivelAnterior = false;
unsigned long tPmSubida = 0;

// Unico lugar donde se toca la salida. Escribe los dos pines juntos para que
// el LED testigo no pueda quedar nunca desfasado de la salida real.
void setSalida(bool encendida) {
  digitalWrite(PIN_OUTPUT, encendida ? HIGH : LOW);
  digitalWrite(PIN_LED_TESTIGO, encendida ? HIGH : LOW);
}

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

// Arranca el seguimiento del PIR tomando su nivel actual como punto de partida.
// Si el modulo esta en medio de un pulso justo cuando se arma la ventana, ese
// pulso viejo no cuenta: solo dispara un flanco de subida nuevo.
void reiniciarDeteccion() {
  pirNivel = leerPir();
}

void armarVentana() {
  estado = ARMADO;
  tInicioArmado = millis();
  reiniciarDeteccion();
}

void setup() {
  Serial.begin(115200);

  pinMode(PIN_DOOR_SWITCH, INPUT_PULLUP);
  pinMode(PIN_PIR, INPUT_PULLDOWN); // si el pin queda flotando (cable suelto),
                                    // por defecto lee LOW = sin movimiento
  pinMode(PIN_OUTPUT, OUTPUT);
  pinMode(PIN_LED_TESTIGO, OUTPUT);
  setSalida(false);

  tArranque = millis();

  // Inicializa el estado del switch con la lectura actual para evitar un
  // falso flanco al arrancar
  ultimaLecturaSwitch  = (digitalRead(PIN_DOOR_SWITCH) == LOW);
  puertaCerradaEstable = ultimaLecturaSwitch;
  tPuertaAbiertaDesde  = millis();

  // No se arma nada todavia: el HC-SR501 tira falsos HIGH mientras calienta.
  estado = CALENTANDO;
  reiniciarDeteccion();
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

// Devuelve true en el flanco de subida del PIR: un solo pulso ya basta para dar
// la deteccion por buena.
bool actualizarActividadPir() {
  bool pir = leerPir();
  bool flancoDeSubida = (pir && !pirNivel);
  pirNivel = pir;
  return flancoDeSubida;
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
      setSalida(true);
      Serial.print("Movimiento detectado a los ");
      Serial.print((millis() - tInicioArmado) / 1000.0, 1);
      Serial.println("s de la ventana. Salida activada y fija en ON.");
    }

    if (estado == ARMADO && millis() - tInicioArmado >= VENTANA_DETECCION_MS) {
      // Se cumplio el minuto sin movimiento: la salida se apaga (por si venia
      // encendida de un ciclo anterior) y el sistema DEJA DE SENSAR. La ventana
      // NO se reinicia sola: solo la reactiva el proximo cierre de puerta.
      setSalida(false);
      estado = IDLE;
      Serial.println("Ventana cumplida sin movimiento. Salida en OFF. En espera hasta el proximo cierre de puerta.");
    }
  } else if (estado == DISPARADO) {
    // Unica salida de este estado: puerta abierta de forma continua 5 minutos.
    if (!puertaCerradaEstable && (millis() - tPuertaAbiertaDesde >= DOOR_OPEN_RESET_MS)) {
      setSalida(false);
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
    Serial.print(" | testigo_gpio2=");
    Serial.print(digitalRead(PIN_LED_TESTIGO) == HIGH ? "ON" : "off");

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
      Serial.print("s");
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
