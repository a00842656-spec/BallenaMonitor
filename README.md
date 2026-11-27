# PERLA · Ballena Monitor

Firmware de la **ballena** de PERLA (Plataforma Electromiográfica de Rehabilitación Lúdica Adaptativa), Equipo 6, BI2005B.
La pantalla táctil recibe la señal EMG del antebrazo, decide si hay contracción y mueve la **tortuga** por ESP-NOW.
Al terminar una sesión la sube a Firestore para la app del fisioterapeuta.

```
 Electrodos ─▶ Circuito EMG ─▶ ESP32 EMG ──ESP-NOW──▶ Pantalla WT32-SC01 Plus ──ESP-NOW──▶ Tortuga (XIAO ESP32-C6)
                (AD620,          (EMG_Emisor/:        (este sketch: UI, umbral,                (motor; manda su
                 filtros)         filtros + RMS)        juego, sonido, sesión)                   batería)
                                                                │
                                                                └─ ENVIAR ─▶ hotspot ─▶ Firestore ─▶ app (FlutterFlow)
```

## Carpetas

| Carpeta / archivo | Qué es |
|---|---|
| `BallenaMonitor.ino` | Pantalla WT32-SC01 Plus: pantallas, modos, ESP-NOW, baterías, volumen, sesión |
| `emg.*`, `emg_panel.*` | Detección de la contracción (umbral, histéresis, calibración CAL) y panel EMG en pantalla |
| `audio.*`, `*.h` de sonido | Música, efectos y voces (en PROGMEM) |
| `session_log.*`, `firestore_upload.*` | GRABAR / ENVIAR y subida a Firestore |
| `secrets.example.h` | Plantilla de `secrets.h` (hotspot de ENVIAR). `secrets.h` no se sube a GitHub |
| `ble_app.*` | (Opcional, apagado) Bluetooth directo con la app |
| `ui*.c/h`, `fonts/` | Generados por **SquareLine Studio**: no editarlos a mano |
| `motor_protocol.h` | Mensajes ESP-NOW. **Debe ser idéntico** en las tres carpetas |
| `EMG_Emisor/` | ESP32 del circuito EMG (ESP32 DevKit con CH340, entrada en GPIO35): filtros + RMS, manda la envolvente a la pantalla |
| `Tortuga/` | XIAO ESP32-C6 de la tortuga: motor y batería |
| `extras/` | `lv_conf.h` y `boards.local.txt` (partición de 12 MB) |
| `tools/` | `espejo.html` (la pantalla en la computadora, también se puede tocar con el mouse), `mp3_header.py` (sonidos → `.h`), `nube.py` (máscara de la nube) |
| `PARA_LA_APP_*.md` | Cómo recibe la app los datos (Wi-Fi propio / Bluetooth) |

## Instalar (una vez por computadora)

Se compila con **arduino-cli** (en la terminal de VS Code).

```powershell
winget install ArduinoSA.CLI
arduino-cli config init
arduino-cli config add board_manager.additional_urls https://espressif.github.io/arduino-esp32/package_esp32_index.json
arduino-cli core update-index
arduino-cli core install esp32:esp32@3.3.13
arduino-cli lib install "lvgl@8.3.11" "LovyanGFX" "ESP8266Audio"
```

- **Core ESP32 3.3.x**, no 4.x: el código usa la API de ESP-NOW de 3.3.
- **LVGL 8.3.11**, no 9.x: SquareLine exportó para 8.3. Si VS Code ofrece actualizar `lvgl`, elegir *Don't Notify*.
- Copiar `extras/lv_conf.h` a `Documents/Arduino/libraries/` (al lado de la carpeta `lvgl`).
- Copiar `extras/boards.local.txt` a `%LOCALAPPDATA%\Arduino15\packages\esp32\hardware\esp32\<versión>\` (agrega la partición `app12M`).
- Copiar `secrets.example.h` como **`secrets.h`** y poner el nombre y la contraseña del hotspot (ver *Wi-Fi para ENVIAR*).

## Compilar y subir

```powershell
arduino-cli board list      # ver el puerto (COMx)

# Pantalla (desde esta carpeta)
arduino-cli compile -b esp32:esp32:wt32-sc01-plus:PartitionScheme=app12M --build-path build -u -p COMx .

# ESP32 de la EMG
arduino-cli compile -b esp32:esp32:esp32 -u -p COMx EMG_Emisor

# Tortuga
arduino-cli compile -b esp32:esp32:XIAO_ESP32C6 -u -p COMx Tortuga
```

- La pantalla necesita **12 MB** de programa (imágenes y sonidos, ~45 %). Con la partición por defecto (3 MB) falla con *"text section exceeds available space"*.
  Por eso **no usar los botones ✓ / → de la extensión Arduino de VS Code**: compilan sin `app12M`.
- La primera compilación tarda 5–10 min; después solo recompila lo que cambió (`build/` está en `.gitignore`).
- Si falla con *"port is busy"*, cerrar el Serial Monitor.
- Cuidado con el puerto: no subir el sketch de otra placa a la pantalla. El número (COM3, COM4...) puede cambiar al reconectar: revisar siempre con `arduino-cli board list` (la pantalla sale como *ESP32 Family Device*; la EMG, con vid `0x1A86` = CH340).
- Para subir a la pantalla con la pila puesta: **apagar la pila primero** (botón rojo) y luego conectar el USB. No usar USB y pila al mismo tiempo.

**IntelliSense de VS Code (opcional).** Si aparecen errores rojos tipo *cannot open source file "lvgl.h"*, generar `.vscode/c_cpp_properties.json` a partir de `build/compile_commands.json` después de compilar una vez, y poner `"arduino.disableIntelliSenseAutoGen": true` en `.vscode/settings.json`. No afecta a la compilación.

## Conexiones de la ballena

Vista trasera de la pantalla, con el USB-C a la derecha: el pin ① de cada conector es el de la **izquierda**.

```
Pila 9 V + ─▶ botón rojo (paro) ─▶ regulador IN+        regulador OUT+ (5.0 V) ─▶ Extended IO ① +5V (rojo)
Pila 9 V − ──────────────────────▶ regulador IN−        regulador OUT−         ─▶ Extended IO ② GND (negro)

regulador IN+ ──[100 kΩ]──●──[30 kΩ]── GND
                          └──▶ Extended IO ③ GPIO10 (amarillo)   → batería de la ballena en pantalla

Bocina ─▶ conector SPK (2 pines, la polaridad no importa)
```

| Conector (izquierda → derecha) | Uso |
|---|---|
| **Extended IO** (8 pines) | ① +5V entrada, ② GND, ③ GPIO10 = divisor de la pila; ④–⑧ libres (aislar las puntas) |
| **DEBUG** (7 pines) | No usar (tiene EN y BOOT: si tocan GND la pantalla se reinicia) |
| **SPK** (2 pines) | Bocina |

- **Paro de emergencia:** el botón rojo (contacto NC) corta la pila. La pantalla se apaga y la tortuga se detiene sola en 0.3 s (no recibe órdenes). Para volver: girar el botón; la pantalla arranca en ~15 s. Por eso `ESTOP_PIN = -1`.
- Revisar con multímetro antes de conectar: salida del regulador **4.9–5.2 V**; punto medio del divisor **1.9–2.3 V** (con pila de 9 V). Si da 6–7 V, las resistencias están al revés.
- Una pila de 9 V dura poco con la pantalla (1–2 h): usar una nueva para las pruebas.

## Qué hace la pantalla

| Pantalla | Qué hay |
|---|---|
| INICIO | Música de fondo |
| CONEXIÓN BLUETOOTH | Panel de estado: Pantalla / Tortuga / EMG. Con los tres en verde pasa solo a SELECCIÓN DE MODO |
| MODO JUEGO | Color objetivo (voz + nube de colores). La contracción mueve la tortuga con **velocidad proporcional a la activación** (RMS entre el umbral y el 70 % de la MVC de CAL). ✓ / ✗ del fisioterapeuta suman puntos |
| MODO REHABILITACIÓN | Meta de repeticiones (▲▼). Cada contracción = 1 repetición y la tortuga avanza a velocidad fija. PAUSAR, CONTINUAR (+1 manual), PACIENTE (ID `PAC-###`), GRABAR / ENVIAR |
| RESUMEN DE SESIÓN | Objetivo / logrado y una **felicitación por voz** (siempre positiva; distinta si cumplió la meta) |

En todas las pantallas (creados por código, detrás del panel EMG y del panel de conexión):
- **Batería de la tortuga** y **batería de la ballena** (`--` = sin datos; rojo < 20 %).
- **Botón de volumen**: abre una ventana con volumen general y volumen de la música. Se guarda en memoria. El 100 % en pantalla es el **70 % real** (`VOL_REAL_MAX`): más arriba el amplificador calienta la pantalla.

En MODO JUEGO y REHABILITACIÓN:
- **Panel EMG** (se puede arrastrar): barra RMS, umbral `−`/`+`, **CAL** (3 s relajado + 3 s apretando → umbral = reposo + 30 % de la diferencia). Tocarlo muestra la gráfica de 5 s.
- **Flechas ▲▼** para mover la tortuga a mano (demostraciones).

Seguridad: sin datos de la EMG en 300 ms la señal se toma como cero; la tortuga se detiene sola si no recibe órdenes en 300 ms; en PAUSA no se mueve. Paro de emergencia: botón rojo que corta la pila (ver *Conexiones*). También existe un paro por software (`ESTOP_PIN`, apagado).

## Configuración rápida

| Dónde | Constante | Para qué |
|---|---|---|
| `BallenaMonitor.ino` | `turtleAddress` | MAC del XIAO (la imprime su Serial) |
| | `TURTLE_SPEED`, `JUEGO_SPEED_MIN/MAX`, `JUEGO_VELOCIDAD_VARIABLE` | Velocidades de la tortuga |
| | `BALLENA_BAT_X/Y`, `TORTUGA_BAT_X/Y`, `VOL_BTN_X/Y` | Posición de baterías y volumen (y crece hacia abajo) |
| | `RESUMEN_REACCION` | Felicitación por voz en RESUMEN |
| | `FLECHAS_ACTIVAS`, `ESTOP_PIN` | Flechas manuales; paro por software (`-1` = apagado, el paro es el botón que corta la pila) |
| | `BALLENA_BAT_PIN`, `BALLENA_BAT_DIVIDER`, `BALLENA_BAT_V_FULL/EMPTY` | Batería de la ballena medida en la pantalla: GPIO10, divisor 100k/30k, 9.0 V = 100 %, 7.0 V = 0 % |
| | `VOL_REAL_MAX` | Volumen real máximo (70 %) que corresponde al 100 % en pantalla |
| | `ESPEJO_USB` | Espejo y toque desde la computadora (`false` = apagado) |
| `emg.h` | `EMG_REMOTE` | `true` = la señal llega del ESP32 EMG por ESP-NOW |
| | `EMG_DEFAULT_THRESHOLD_MV`, `EMG_MIN_ON_MS`, `EMG_CAL_FRACTION` | Umbral inicial (30 mV), tiempo mínimo (200 ms), fracción de CAL |
| `secrets.h` (no está en GitHub) | `FIRESTORE_WIFI_SSID/PASS` | Hotspot que usa ENVIAR |
| `firestore_upload.h` | `FIRESTORE_PROJECT_ID`, `FIRESTORE_COLLECTION` | Proyecto y colección de Firestore |
| `EMG_Emisor/EMG_Emisor.ino` | `EMG_PIN` | Entrada del circuito EMG (GPIO35) |
| | `BAT_PIN` | Ya no se usa para la ballena (`-1`): la pantalla mide su propia pila |

## ESP-NOW (`motor_protocol.h`)

Canal fijo **1**. El primer byte identifica el mensaje:

| Magic | Mensaje | De → a | Periodo |
|---|---|---|---|
| `0x7A` | `MotorMsg` {cmd, speed} | Pantalla → Tortuga | cada 100 ms mientras se mueve |
| `0x7B` | `TelemetryMsg` (batería) | Tortuga → Pantalla | 1 s |
| `0x7C` | `EmgMsg` {RMS, pico, DC, flags} | ESP32 EMG → Pantalla | 20 ms |
| `0x7D` | `BallenaBatMsg` (batería) | ESP32 EMG → Pantalla | 1 s (se ignora si `BALLENA_BAT_PIN >= 0`) |

Al cambiar este archivo, copiarlo igual a `EMG_Emisor/` y `Tortuga/` y volver a subir las tres placas.

## Wi-Fi para ENVIAR (Firestore)

Al presionar **ENVIAR**, la pantalla se conecta unos segundos a un hotspot del celular, sube la sesión a Firestore (proyecto `emg-display`, colección `consultas`) y vuelve al canal de ESP-NOW. Al encender también se conecta un momento para tomar la hora.

El nombre y la contraseña del hotspot van en `secrets.h`, que **no se sube a GitHub** (está en `.gitignore`):

```powershell
copy secrets.example.h secrets.h     # luego editar secrets.h
```

- El hotspot debe ser de **2.4 GHz** (en iPhone: activar *Maximizar compatibilidad*).
- Sin `secrets.h` el programa compila igual (con un aviso), pero ENVIAR no se podrá conectar.
- Cada integrante pone su propio hotspot en su `secrets.h`.

## Espejo en la computadora

Para ver la pantalla en grande (presentaciones, videos, capturas para el reporte):
1. Conectar la pantalla a la computadora con el cable USB.
2. Abrir `tools/espejo.html` con doble clic en **Chrome o Edge** → **Conectar** → elegir el puerto (COMx).

La pantalla sigue funcionando igual y la computadora muestra lo mismo. Con **Clic = tocar** activado, hacer clic (o mantener, o arrastrar) en la imagen es igual que tocar la pantalla; desactivarlo para solo mirar. **Captura** guarda la imagen como PNG y **Serial** muestra los mensajes del Serial Monitor (con una casilla para mandar comandos, p. ej. `?`).
- Al conectar, Windows **reinicia la pantalla** una vez: esperar ~15 s hasta ver `[ESPEJO] encendido`. Conectar antes de empezar una sesión.
- Si se suelta el cable con un clic presionado, el toque se suelta solo en 0.4 s.
- Antes de subir un programa: **Desconectar** (la página usa el puerto, igual que el Serial Monitor).
- Un cambio de pantalla completa tarda ~0.5 s en aparecer en la computadora (USB 1.1); los cambios pequeños se ven al momento.
- Con electrodos puestos y la laptop conectada, la laptop debe estar **sin cargador**.
- `ESPEJO_USB = false` en `BallenaMonitor.ino` lo apaga del todo.

## Sonidos

Para agregar una voz o efecto:
```powershell
python tools/mp3_header.py mi_sonido.mp3 mi_sonido --sfx
```
Genera `mi_sonido.h` (44.1 kHz mono, sin silencio inicial, pico ~−2 dBFS). Luego agregarlo al `enum Sfx` de `audio.h` y a `SFX_TABLE` de `audio.cpp`, en el mismo orden.

## SquareLine Studio

- Proyecto: `Ballena_Screen` (SquareLine 1.6.3, LVGL 8.3.11, 480×320, 16 bits sin swap).
- *Project Settings → UI Files Export Path* = esta carpeta. Exportar **sobrescribe** `ui_*.c/h`: hacer commit antes.
- Las baterías, el volumen, las flechas y los paneles se crean en `BallenaMonitor.ino`, así que no se pierden al exportar.

### Nube de colores (MODO JUEGO)
La nube está dibujada dentro del fondo `ui_img_4_png.c`. Su relleno es una máscara aparte (`ui_img_nube.c`, ALPHA_8BIT) que se pinta con `img_recolor` en `generateRandomColor()`. Si se cambia el fondo en SquareLine, regenerar la máscara con `python tools/nube.py`.

## Problemas frecuentes

| Síntoma | Causa probable |
|---|---|
| Tortuga o EMG en ✗ | La placa está apagada o sin batería; encenderlas y reiniciar la pantalla. Si sigue, ver el Serial (`arduino-cli monitor -p COMx --config 115200`) |
| Batería de la ballena `--` | Normal si está solo con USB. Con pila: revisar el divisor (1.9–2.3 V en el cable amarillo) y `BALLENA_BAT_DIVIDER` |
| La pantalla se reinicia o se congela sola | Puntas sueltas del arnés tocando algo (aislarlas), o pila de 9 V baja |
| Espejo: no deja conectar (*Failed to open serial port*) | El puerto lo usa otra pestaña del espejo, el Serial Monitor o una subida en curso |
| Espejo: conectado pero sin imagen | Esperar 15 s al reinicio. Si sigue, desconectar el cable USB, volver a conectarlo y abrir de nuevo |
| *text section exceeds available space* | Se compiló sin `PartitionScheme=app12M` |
| Errores rojos en VS Code pero compila bien | Solo IntelliSense (ver arriba) |
| ENVIAR falla | Hotspot apagado, en 5 GHz, con otro nombre o falta `secrets.h`; el mensaje en pantalla y el Serial dicen la causa |
