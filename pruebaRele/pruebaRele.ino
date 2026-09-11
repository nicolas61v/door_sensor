// PRUEBA DE LA SALIDA / RELE
//
// Sketch de un solo proposito: mover el pin de salida a mano para ver si el
// rele responde. No hay PIR, no hay puerta, no hay logica de alarma.
//
// PARA QUE SIRVE
// -------------------------------------------------------------------------
// Sintoma tipico: el LED testigo (GPIO2) prende, pero el rele no hace clic.
// Eso YA descarta el codigo y el ESP32: si el testigo prende, el GPIO26 esta
// en HIGH, porque sensorDoor.ino escribe los dos pines en la misma funcion.
// O sea, el problema esta en el rele o en su cableado. Este sketch sirve para
// encontrar cual de las 4 causas posibles es.
//
// LAS 4 CAUSAS, EN ORDEN DE PROBABILIDAD
// -------------------------------------------------------------------------
// 1) EL MODULO ES ACTIVO EN BAJO.
//    La mayoria de los modulos rele chinos (los azules con SRD-05VDC) activan
//    con LOW, no con HIGH. Mandando HIGH quedan apagados para siempre.
//    -> Se prueba con el comando 'l' de este sketch. Si con LOW hace clic,
//       encontraste el problema. Ver "COMO ARREGLARLO" abajo.
//
// 2) LA BOBINA NO TIENE 5V.
//    La bobina del rele necesita 5V y bastante corriente (~70mA). Si el VCC
//    del modulo esta en el pin 3V3 del ESP32, el LED del modulo puede prender
//    igual pero la bobina nunca llega a cerrar.
//    -> VCC del modulo al pin 5V/VIN del ESP32, igual que el PIR.
//
// 3) FALTA LA MASA COMUN.
//    Si el modulo se alimenta de una fuente aparte y su GND no esta unido al
//    GND del ESP32, la senal del GPIO no tiene referencia y no pasa nada.
//    -> Unir los dos GND, siempre.
//
// 4) 3.3V NO ALCANZAN PARA EL OPTOACOPLADOR.
//    Con el jumper JD-VCC puesto, algunos modulos necesitan que la senal IN
//    sea de 5V. El ESP32 entrega 3.3V y se queda corto.
//    -> Se nota porque NI HIGH NI LOW hacen clic, aunque la alimentacion este
//       bien. Se resuelve con un transistor (2N2222 + 1k) o un modulo que
//       acepte 3.3V.
//
// COMO ARREGLARLO SI ES ACTIVO EN BAJO (causa 1)
// -------------------------------------------------------------------------
// En sensorDoor.ino, dentro de setSalida(), invertir la linea del rele:
//
//     digitalWrite(PIN_OUTPUT, encendida ? LOW : HIGH);   // <- invertida
//     digitalWrite(PIN_LED_TESTIGO, encendida ? HIGH : LOW);
//
// El testigo se deja como esta, asi sigue prendiendo cuando la alarma esta ON.
// Ojo: con logica invertida el rele queda activado durante el arranque, hasta
// que el setup() llega a poner el pin. Si eso molesta, avisa y lo resolvemos.
//
// USO
// -------------------------------------------------------------------------
// Cargalo, abri el Monitor Serie a 115200, poni "Nueva linea" o "Ambos NL & CR"
// en el desplegable de abajo, y escribi los comandos:
//
//   a       alterna automatico: 3s ON / 3s OFF (es como arranca)
//   h       fija el pin en HIGH y lo deja ahi
//   l       fija el pin en LOW y lo deja ahi
//   t       test completo: HIGH, LOW, HIGH, LOW pausado, para escuchar
//   p26     cambia el pin de salida (p25, p27, el que quieras probar)
//   ?       muestra la ayuda y el estado actual

#include <Arduino.h>

uint8_t pinSalida = 26;              // el mismo que PIN_OUTPUT en sensorDoor.ino
const uint8_t PIN_LED_TESTIGO = 2;   // el mismo testigo que en sensorDoor.ino

const unsigned long PERIODO_ALTERNA_MS = 3000; // 3s de cada lado: da tiempo a
                                               // escuchar el clic y a que la
                                               // bobina termine de moverse

enum Modo { ALTERNA, FIJO_HIGH, FIJO_LOW, TEST };
Modo modo = ALTERNA;

bool nivelActual = false;
unsigned long tUltimoCambio = 0;

// Estado del test guiado
uint8_t pasoTest = 0;
unsigned long tPasoTest = 0;
const unsigned long PASO_TEST_MS = 4000;

String linea = "";

void escribir(bool alto) {
  nivelActual = alto;
  digitalWrite(pinSalida, alto ? HIGH : LOW);
  digitalWrite(PIN_LED_TESTIGO, alto ? HIGH : LOW);

  Serial.print("  GPIO");
  Serial.print(pinSalida);
  Serial.print(" = ");
  Serial.print(alto ? "HIGH (3.3V)" : "LOW  (0V)");
  Serial.println("   <- escucha el clic");
}

void ayuda() {
  Serial.println();
  Serial.println("=====================================================");
  Serial.println("   PRUEBA DE SALIDA / RELE");
  Serial.println("=====================================================");
  Serial.println("  a     alterna 3s ON / 3s OFF");
  Serial.println("  h     fija HIGH");
  Serial.println("  l     fija LOW");
  Serial.println("  t     test guiado (HIGH/LOW pausado)");
  Serial.println("  p26   cambia el pin de salida");
  Serial.println("  ?     esta ayuda");
  Serial.println("-----------------------------------------------------");
  Serial.print("  pin actual: GPIO");
  Serial.print(pinSalida);
  Serial.print(" | nivel: ");
  Serial.print(nivelActual ? "HIGH" : "LOW");
  Serial.print(" | modo: ");
  switch (modo) {
    case ALTERNA:   Serial.println("ALTERNA"); break;
    case FIJO_HIGH: Serial.println("FIJO EN HIGH"); break;
    case FIJO_LOW:  Serial.println("FIJO EN LOW"); break;
    case TEST:      Serial.println("TEST GUIADO"); break;
  }
  Serial.println("=====================================================");
  Serial.println();
}

void cambiarPin(uint8_t nuevo) {
  // Deja el pin viejo en reposo antes de soltarlo
  digitalWrite(pinSalida, LOW);
  pinSalida = nuevo;
  pinMode(pinSalida, OUTPUT);
  digitalWrite(pinSalida, LOW);
  nivelActual = false;

  Serial.print(">>> Ahora se prueba GPIO");
  Serial.println(pinSalida);
}

void procesarComando(String cmd) {
  cmd.trim();
  cmd.toLowerCase();
  if (cmd.length() == 0) return;

  if (cmd == "a") {
    modo = ALTERNA;
    tUltimoCambio = millis();
    Serial.println(">>> Modo ALTERNA: 3s HIGH, 3s LOW, sin parar.");

  } else if (cmd == "h") {
    modo = FIJO_HIGH;
    Serial.println(">>> Fijo en HIGH. Si el rele no hace clic aca, proba con l.");
    escribir(true);

  } else if (cmd == "l") {
    modo = FIJO_LOW;
    Serial.println(">>> Fijo en LOW. Si el rele hace clic AHORA, tu modulo es");
    Serial.println("    ACTIVO EN BAJO: hay que invertir setSalida() en");
    Serial.println("    sensorDoor.ino (ver el comentario al principio).");
    escribir(false);

  } else if (cmd == "t") {
    modo = TEST;
    pasoTest = 0;
    tPasoTest = millis() - PASO_TEST_MS; // arranca el primer paso ya
    Serial.println();
    Serial.println(">>> TEST GUIADO. Escucha el rele en cada paso.");

  } else if (cmd.startsWith("p")) {
    long n = cmd.substring(1).toInt();
    // Pines de salida validos en el ESP32. Se excluyen los de solo entrada
    // (34-39) y los de la flash (6-11), que colgarian la placa.
    if (n < 0 || n > 33 || (n >= 6 && n <= 11)) {
      Serial.println("!!! Pin no valido como salida. Usa 0-33, evitando 6-11.");
    } else {
      cambiarPin((uint8_t)n);
    }

  } else if (cmd == "?") {
    ayuda();

  } else {
    Serial.print("!!! Comando desconocido: ");
    Serial.println(cmd);
    Serial.println("    Escribi ? para ver la lista.");
  }
}

void correrTest() {
  if (millis() - tPasoTest < PASO_TEST_MS) return;
  tPasoTest = millis();

  switch (pasoTest) {
    case 0:
      Serial.println();
      Serial.println("[1/4] Mandando HIGH. Si es activo en ALTO, suena AHORA.");
      escribir(true);
      break;
    case 1:
      Serial.println();
      Serial.println("[2/4] Mandando LOW. Si es activo en BAJO, suena AHORA.");
      escribir(false);
      break;
    case 2:
      Serial.println();
      Serial.println("[3/4] HIGH otra vez, para confirmar.");
      escribir(true);
      break;
    case 3:
      Serial.println();
      Serial.println("[4/4] LOW otra vez, para confirmar.");
      escribir(false);
      break;
    default:
      Serial.println();
      Serial.println("=====================================================");
      Serial.println("   RESULTADO");
      Serial.println("-----------------------------------------------------");
      Serial.println(" Sono en HIGH  -> todo bien, el modulo es activo alto.");
      Serial.println("                 El problema esta en otro lado: revisa");
      Serial.println("                 que el cable este de verdad en GPIO26.");
      Serial.println();
      Serial.println(" Sono en LOW   -> modulo ACTIVO EN BAJO. Invertir la");
      Serial.println("                 linea del rele en setSalida().");
      Serial.println();
      Serial.println(" No sono nunca -> es alimentacion o cableado:");
      Serial.println("                 1. VCC del modulo al pin 5V/VIN (no 3V3)");
      Serial.println("                 2. GND del modulo unido al GND del ESP32");
      Serial.println("                 3. si igual no anda, 3.3V no alcanzan");
      Serial.println("                    para el opto: hace falta transistor.");
      Serial.println("=====================================================");
      Serial.println();
      modo = ALTERNA;
      tUltimoCambio = millis();
      return;
  }
  pasoTest++;
}

void setup() {
  Serial.begin(115200);

  pinMode(pinSalida, OUTPUT);
  pinMode(PIN_LED_TESTIGO, OUTPUT);
  digitalWrite(pinSalida, LOW);
  digitalWrite(PIN_LED_TESTIGO, LOW);

  delay(300);
  ayuda();
  Serial.println("Arrancando en modo ALTERNA. Escucha el rele.");
  Serial.println("Si no hace clic en ningun momento, escribi t.");
  Serial.println();

  tUltimoCambio = millis();
}

void loop() {
  // --- Lectura de comandos por el Monitor Serie ---
  while (Serial.available() > 0) {
    char c = (char)Serial.read();
    if (c == '\n' || c == '\r') {
      if (linea.length() > 0) {
        procesarComando(linea);
        linea = "";
      }
    } else if (linea.length() < 16) {
      linea += c;
    }
  }

  // --- Accion segun el modo ---
  if (modo == ALTERNA) {
    if (millis() - tUltimoCambio >= PERIODO_ALTERNA_MS) {
      tUltimoCambio = millis();
      escribir(!nivelActual);
    }
  } else if (modo == TEST) {
    correrTest();
  }
  // En FIJO_HIGH y FIJO_LOW no se hace nada: el pin ya quedo puesto.
}
