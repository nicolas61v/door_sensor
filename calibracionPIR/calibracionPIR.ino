// CALIBRACION del modulo PIR BISS0001 / HC-SR501
//
// Sketch de un solo proposito: medir cuanto dura cada pulso del PIR.
// No controla nada, no tiene alarma, no tiene puerta. Solo mide.
//
// Cargalo, abri el Monitor Serie a 115200 y segui los pasos que imprime.
// Cuando termines de calibrar, volve a cargar sensorDoor.ino con los valores
// que hayas medido.

#include <Arduino.h>

const uint8_t PIN_PIR = 27;   // mismo pin que en sensorDoor.ino
const unsigned long CALENTAMIENTO_MS = 60000;

bool nivelAnterior = false;
unsigned long tSubida = 0;
unsigned long tBajada = 0;
bool huboPulsoPrevio = false;
unsigned long tUltimoAviso = 0;
unsigned long pulsoNro = 0;

void setup() {
  Serial.begin(115200);
  pinMode(PIN_PIR, INPUT_PULLDOWN);
  delay(300);

  Serial.println();
  Serial.println("=====================================================");
  Serial.println("   CALIBRACION PIR BISS0001 / HC-SR501");
  Serial.println("=====================================================");
  Serial.println("PASO 1: identificar los potenciometros.");
  Serial.println("  Gira UNO a fondo, pasa la mano, mira si cambia la");
  Serial.println("  duracion del pulso. Si cambia, ese es TIME (Tx).");
  Serial.println();
  Serial.println("PASO 2: TIME al MINIMO.");
  Serial.println("  Gira TIME a fondo para un lado, pasa la mano UNA vez,");
  Serial.println("  anota. Gira a fondo para el otro lado, repeti.");
  Serial.println("  Quedate con la posicion que de el pulso MAS CORTO.");
  Serial.println();
  Serial.println("PASO 3: verificar el jumper.");
  Serial.println("  Movete SIN PARAR 30s delante del sensor:");
  Serial.println("   - UN pulso largo de ~30s  -> jumper en H. CORRECTO.");
  Serial.println("   - VARIOS pulsos cortos    -> jumper en L. MOVELO.");
  Serial.println("=====================================================");
  Serial.print("Calentando ");
  Serial.print(CALENTAMIENTO_MS / 1000);
  Serial.println("s. Los pulsos de este rato NO valen, ignoralos.");
  Serial.println();

  nivelAnterior = (digitalRead(PIN_PIR) == HIGH);
}

void loop() {
  bool pir = (digitalRead(PIN_PIR) == HIGH);
  unsigned long ahora = millis();
  bool calentando = (ahora < CALENTAMIENTO_MS);

  if (pir && !nivelAnterior) {
    tSubida = ahora;
    Serial.print("  > sube");
    if (huboPulsoPrevio) {
      Serial.print("   (estuvo quieto ");
      Serial.print((ahora - tBajada) / 1000.0, 1);
      Serial.print("s desde el pulso anterior)");
    }
    Serial.println();

  } else if (!pir && nivelAnterior) {
    tBajada = ahora;
    huboPulsoPrevio = true;
    pulsoNro++;

    float durS = (ahora - tSubida) / 1000.0;
    Serial.print("PULSO #");
    Serial.print(pulsoNro);
    Serial.print(" = ");
    Serial.print(durS, 1);
    Serial.print(" s");
    if (calentando) {
      Serial.print("   <-- CALENTANDO, ignorar");
    } else {
      Serial.print("   -> PIR_TX_MS = ");
      Serial.print((unsigned long)(durS * 1000.0));
      Serial.print("  (si fue UNA sola deteccion)");
    }
    Serial.println();
  }

  // Aviso una vez cuando termina el calentamiento
  if (!calentando && tUltimoAviso == 0) {
    tUltimoAviso = ahora;
    Serial.println();
    Serial.println(">>> Calentamiento terminado. Desde aca las mediciones valen. <<<");
    Serial.println();
    pulsoNro = 0;
  }

  nivelAnterior = pir;
}
