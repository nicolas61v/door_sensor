# sensorDoor — ESP32 + switch de puerta + PIR BISS0001 (HC-SR501)

Detecta si hay alguien **moviéndose adentro** después de que la puerta se cierra,
y activa una salida (relé / buzzer / LED) solo si el movimiento es **sostenido**.

## Cableado

| Módulo PIR (HC-SR501) | ESP32 | Nota |
|---|---|---|
| VCC | **5V / VIN** | necesita 4.5–20 V. **No** a 3V3 (no arranca su regulador). |
| OUT | GPIO 27 | entrega 3.3 V → seguro para el GPIO, sin divisor. |
| GND | GND | masa común obligatoria. |

| Otros | ESP32 | Nota |
|---|---|---|
| Switch de puerta | GPIO 14 ↔ GND | `INPUT_PULLUP`: **LOW = puerta cerrada**. |
| Salida de alarma | GPIO 2 | LED onboard, sirve para probar. Para un relé mové a 25/26/32/33 (GPIO2 es strapping pin y puede impedir el arranque si el relé lo tira a nivel bajo). |

Si el ESP32 se alimenta por USB, el pin `5V`/`VIN` sale del USB y alcanza para el PIR
(usa ~50 µA en reposo). Si le ponés un relé, alimentalo aparte: el regulador de la
placa no da para la bobina.

## Identificar los 3 pines ANTES de conectar

El módulo viene con los 3 agujeros sin header soldado. Soldá 3 pines (o 3 cables)
y **antes de energizar** confirmá cuál es cuál — hay clones con el orden invertido
(`VCC-OUT-GND` vs `GND-OUT-VCC`) y si le das vuelta la alimentación lo quemás.

Con el multímetro en **continuidad**, mirando el lado de los componentes:

1. **GND** = el pin que da continuidad con la **pata negativa del capacitor
   electrolítico** grande (la banda blanca) y con el plano de masa.
2. **VCC** = el pin que da continuidad con la **pata de entrada del regulador
   `7133-1`** (el SOT-89 chiquito de 3 patas).
3. **OUT** = el que sobra, el del medio en prácticamente todos los clones.

Si la serigrafía está legible al costado de los agujeros, alcanza con leerla.
Ante la duda, el multímetro manda.

## Dónde y cómo montarlo

- **Altura ~2–2.2 m**, apuntando ligeramente hacia abajo. El PIR detecta mucho mejor
  el movimiento **atravesando** su campo que viniendo de frente hacia él.
- **No lo apuntes a la puerta.** La hoja moviéndose, más el aire y la diferencia de
  temperatura que entra al abrirse, son la fuente Nº 1 de falsos disparos.
- **Lejos de** ventanas con sol directo, estufas, aires acondicionados, rejillas de
  ventilación y lámparas incandescentes/halógenas. El PIR ve cambios de calor, no luz
  visible: una corriente de aire caliente le pega igual que una persona.
- El **domo blanco (lente de Fresnel) tiene que quedar libre**, sin vidrio ni acrílico
  delante. El infrarrojo lejano no atraviesa el vidrio común: si lo metés en una caja
  cerrada, deja de detectar.
- Cubre ~110° en cono. Ubicalo para que quien entre **cruce** ese cono, no para que
  camine derecho hacia él.

## Ajuste del módulo (ya calibrado)

1. **Potenciómetro `TIME` al mínimo.** Es la posición calibrada: da **Tx = 0.7 s**.
   Si lo movés, hay que volver a medir con `calibracionPIR` y actualizar `PIR_TX_MS`.
2. **Potenciómetro `SENS`** a la mitad. Si hay falsos disparos, bajalo **antes** de
   tocar nada del código.
3. **Jumper:** el módulo está en **`L`** (single trigger) y el código está hecho para
   eso. Si lo pasás a `H` también funciona — el sketch confirma igual por pulso largo —
   pero no hace falta.

## Comportamiento real del módulo (medido, no supuesto)

Medido con `calibracionPIR`, `TIME` al mínimo, 36 pulsos:

| Situación | Salida del módulo |
|---|---|
| Persona moviéndose sin parar | pulso de **0.7 s cada ~3 s**, muy regular |
| Falso positivo (insecto, corriente de aire) | **un pulso suelto** de 0.7 s |
| Ceguera entre pulsos (*blocking time* del BISS0001) | **~2.3 s** |

Que los 36 pulsos midieran **exactamente lo mismo** es la prueba de que el módulo no
re-extiende la salida: está en modo `L`. Con esa señal es **imposible** tener HIGH
continuo por más de 0.7 s.

## Por qué la lógica es por tren de pulsos

El SR505 tenía salida casi "instantánea", así que pedir *"5 s de HIGH continuo"*
filtraba bien los falsos positivos.

Con este módulo esa regla **no dispararía nunca**: pide 5 s de HIGH continuo y el
hardware entrega como máximo 0.7 s. Cero disparos, siempre.

Por eso la confirmación pasó a ser **actividad sostenida en el tiempo**, que es lo que
de verdad separa a una persona de un falso positivo:

- `PULSOS_PARA_CONFIRMAR = 4` + `ACTIVIDAD_MINIMA_MS = 8000` → hacen falta **4 pulsos
  que abarquen al menos 8 s**. Una persona los junta en ~10-12 s; un pulso aislado se
  queda en 1 y se descarta.
- `PAUSA_MAXIMA_MS = 8000` → si pasan 8 s sin un pulso, el tren se corta y vuelve a
  cero. Tiene que ser **mayor que `PIR_TX_MS + PIR_BLOQUEO_MS`** (3.2 s), si no el tren
  se cortaría solo entre pulsos normales.
- `PULSO_LARGO_CONFIRMA_MS = 8000` → si el módulo alguna vez da un pulso largo (jumper
  en `H`), eso confirma directo. Funciona en los dos modos sin tocar nada.
- Estado `CALENTANDO` (60 s) al energizar → se ignoran los HIGH espurios del warm-up,
  que antes podían disparar la alarma sola al enchufar.
- `static_assert` que no deja compilar si los tiempos dejan de ser coherentes entre sí
  (por ejemplo, si el tren no llega a formarse dentro de la ventana).

## Máquina de estados

```
CALENTANDO --60s--> IDLE (puerta abierta) o ARMADO (puerta cerrada)

IDLE ---- puerta se cierra ----> ARMADO

ARMADO:  4 pulsos abarcando >= 8s  -> DISPARADO (salida ON)
         un pulso solo de >= 8s     -> DISPARADO (caso jumper en H)
         pasan 30s sin confirmar    -> salida OFF + se reinicia la ventana (sigue ARMADO)
         puerta se abre             -> no pasa nada, sigue contando

DISPARADO: salida fija en ON
         puerta se cierra            -> ARMADO (nueva ventana decide si sigue ON u OFF)
         puerta abierta 5 min seguidos -> salida OFF + IDLE (respaldo de seguridad)
```

## Compilar y cargar

```sh
arduino-cli compile --fqbn esp32:esp32:esp32 .
arduino-cli upload  --fqbn esp32:esp32:esp32 -p COM3 .    # ajustá el puerto
```

## Calibración en campo

Abrí el Monitor Serie a **115200**. El log de estado muestra el tren en vivo:

```
[t=75s] estado=ARMADO | puerta=CERRADA | pir_raw=MOVIMIENTO | salida=off
        | ventana_restante=15s | pulsos=3/4 | tren=6.4s/8s
```

**Esperá siempre a que pase el `CALENTANDO`** antes de sacar conclusiones: durante el
primer minuto el módulo tira pulsos espurios y no significan nada.

### Pruebas de aceptación

1. **Falso positivo.** Con la puerta cerrada y el sistema `ARMADO`, pasá la mano **una
   sola vez** y quedate quieto. Debe quedar en `pulsos=1/4` y, pasados 8 s, volver a
   `pulsos=0/4`. **La salida no se enciende.**
2. **Detección real.** Movete normalmente. Debe subir `pulsos=1/4 → 2/4 → 3/4 → 4/4` y
   disparar en ~10-12 s con `Actividad confirmada (4 pulsos en 10.5s)` y `salida=ON`.
3. **Reset por puerta.** Con la salida en ON, abrí y cerrá la puerta: tiene que volver a
   `ARMADO` y arrancar una ventana nueva.

### Si hay que re-calibrar

Cargá `calibracionPIR/calibracionPIR.ino`, que imprime la duración de cada pulso y el
hueco entre uno y otro.

- **Duración de un pulso aislado** → `PIR_TX_MS`.
- **Hueco mínimo entre pulsos** moviéndote sin parar → `PIR_BLOQUEO_MS`.
- Si al mover `TIME` los pulsos se hacen largos, subí `PAUSA_MAXIMA_MS` en la misma
  proporción: siempre tiene que ser mayor que `PIR_TX_MS + PIR_BLOQUEO_MS`.

### Si se dispara solo

En este orden: bajá `SENS` → alejalo de ventanas, estufas y rejillas de aire → recién
después subí `PULSOS_PARA_CONFIRMAR` o `ACTIVIDAD_MINIMA_MS`. Si subís esos valores, la
ventana tiene que seguir siendo lo bastante grande; el `static_assert` te avisa si no.

## Archivos

- `sensorDoor.ino` — sketch actual (BISS0001 / HC-SR501).
- `calibracionPIR/calibracionPIR.ino` — sketch auxiliar **solo para calibrar**: mide y
  reporta la duración de cada pulso del PIR. Cargalo, ajustá los potenciómetros y el
  jumper con los números que te da, y después volvé a cargar `sensorDoor.ino`.
- `sensorDoor.ino.sr505.bak` — versión anterior para el SR505, por si hay que volver.
"# door_sensor" 
