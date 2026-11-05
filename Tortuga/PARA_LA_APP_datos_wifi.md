# Ballena EMG → FlutterFlow: cómo recibe la app los datos de la sesión

## Flujo en la pantalla (MODO REHABILITACIÓN)
1. **PACIENTE** (botón con personas): abre una ventana con **[−] PAC-101 [+]** para elegir
   cualquier paciente, y **OK**. El ID actual se ve encima del botón.
   Cada paciente tiene su propia numeración de sesiones: `PAC-101 #1`, `PAC-101 #2`, `PAC-102 #1`…
   (el número sube al presionar ENVIAR; se guarda aunque se apague la pantalla).
2. **GRABAR**: borra lo anterior y empieza a guardar. Guarda el `pacienteID`, la **fecha** y la **hora** de ese momento.
3. El paciente hace las repeticiones (las cuenta la EMG).
4. **ENVIAR**: deja de grabar y publica los datos. Cada ENVIAR aumenta `id`.

## Conexión (Wi-Fi propio de la pantalla, sin router ni internet)
| | |
|---|---|
| Red (SSID) | `Ballena-EMG` |
| Contraseña | `ballena123` |
| Dirección | `http://192.168.4.1` |

> Android puede avisar "red sin internet": hay que elegir **"mantener conexión"**.
> En FlutterFlow, las llamadas a `http://` (sin s) deben estar permitidas en Android
> (cleartext traffic). Probar en un teléfono real, no en el modo web/preview.

## Paso importante: mandar la hora a la pantalla
La pantalla **no tiene reloj**. La app debe llamar esto al conectarse, **antes de GRABAR**:

```
GET http://192.168.4.1/sync?epoch=<segundos Unix UTC>&tz_min=-360
```
- `epoch`: hora actual en segundos Unix (también acepta milisegundos).
- `tz_min`: zona horaria en minutos (México = `-360`).
- Respuesta: `{"ok":true,"hora_local":"2026-11-05 10:32:05"}`

Si no se llama, `fecha` y `hora` llegan vacías y `hora_valida` = `false`
(en ese caso la app puede poner la fecha/hora en que recibió los datos).

## Endpoints (HTTP GET, CORS `*`)
| URL | Qué devuelve |
|---|---|
| `/sync?epoch=..&tz_min=-360` | Pone la fecha/hora |
| `/estado` | `{"grabando":false,"listo":true,"id":3,"segundos_grabados":84.2,"pacienteID":"PAC-103","hora_sincronizada":true}` |
| `/sesion.json` | Datos de la última sesión enviada (404 si no hay) |
| `/sesion.csv` | Envolvente RMS completa `t_s,rms_mV` |

**Detectar datos nuevos:** consultar `/estado` cada 1–2 s; cuando `id` cambie y `listo` = `true`, descargar `/sesion.json`.

## `/sesion.json`
```json
{
  "id": 3,
  "pacienteID": "PAC-103",
  "sesion_paciente": 2,
  "sesionID": "PAC-103 #2",
  "fecha": "2026-11-05",
  "hora": "10:32:05",
  "hora_valida": true,
  "modo": "REHABILITACION",
  "duracion_s": 84.2,
  "objetivo": 10,
  "repeticiones": 7,
  "completada": false,
  "umbral_mV": 75.0,
  "repeticiones_emg": 6,
  "repeticiones_manual": 1,
  "logro_pct": 70.0,
  "activacion_media_ms": 820,
  "pico_medio_mV": 210.4,
  "pico_medio_uV": 21040.0,
  "mvc_mV": 320.0,
  "activacion_media_pct_mvc": 65.8,
  "ganancia_total": 10.0,
  "envolvente_hz": 20,
  "envolvente_truncada": false,
  "repeticiones_detalle": [
    {"n": 1, "fuente": "EMG", "t_s": 3.20, "duracion_ms": 820, "pico_mV": 210.5, "latencia_ms": 180},
    {"n": 2, "fuente": "MANUAL", "t_s": 9.80, "duracion_ms": 0, "pico_mV": 0, "latencia_ms": -1}
  ],
  "pausas": [{"t_s": 40.10, "duracion_s": 12.3}],
  "muestrasGrabadas": 400,
  "SAMPLE_INTERVAL_ms": 100,
  "lecturasEMG": [8, 8, 9, 152, 160, 41, 9],
  "envolvente_mV": [8.1, 8.3, 9.0, 152.4]
}
```

### Equivalencia con el código del equipo
| Código del equipo | Campo en el JSON |
|---|---|
| `String pacienteID = "PAC-101"` | `pacienteID` (lo elige el botón PACIENTE) |
| `int lecturasEMG[MAX_MUESTRAS]` (400) | `lecturasEMG`: enteros en mV, **máximo 400** |
| `muestrasGrabadas` | `muestrasGrabadas` |
| `SAMPLE_INTERVAL = 100` | `SAMPLE_INTERVAL_ms`: 100 ms si la sesión dura ≤ 40 s. Si dura más, las 400 muestras se reparten en toda la sesión y este valor crece (p. ej. 84 s → 210 ms) |

- La pantalla ya guarda todo: **no hace falta** el arreglo `lecturasEMG` ni el muestreo en el código de la pantalla.
- `envolvente_mV` tiene la sesión completa a 20 Hz (hasta 30 min), por si quieren graficar más detalle.
- Los valores EMG son la envolvente RMS en mV a la entrada del ADC. µV en el electrodo = `valor × 1000 / ganancia_total`.

## Qué poner en cada pantalla de la app
| Pantalla de la app | Campo del JSON |
|---|---|
| Inicio de sesión → ID del paciente `PAC-XXX` | filtrar por `pacienteID` |
| Historial (fecha y hora de cada sesión) | `fecha` + `hora` (y `sesionID`, p. ej. "PAC-101 #2") |
| Reporte → Tiempo de la sesión | `duracion_s` |
| Reporte → Promedio de la sesión (%) | `activacion_media_pct_mvc` = fuerza promedio de las contracciones como % de la contracción máxima medida en la calibración (CAL). Si es `null` (no se calibró), usar `logro_pct` = repeticiones / objetivo |
| Reporte → Promedio acumulado (%) | lo calcula la app: promedio del campo anterior en todas las sesiones de ese `pacienteID` |
| Reporte → Promedio de voltaje (picos) | `pico_medio_uV` (µV en el electrodo) |
| Reporte → Voltaje acumulado | lo calcula la app con `pico_medio_uV` de todas las sesiones del paciente |
| Reporte → Gráfica | `lecturasEMG` (mV, cada `SAMPLE_INTERVAL_ms`) |

Notas:
- `fuente: "MANUAL"` = el fisioterapeuta sumó la repetición con el botón CONTINUAR (no hubo señal EMG).
  Los promedios de EMG solo usan las repeticiones `"EMG"`.
- La pantalla no conoce el "ID de doctor": ese dato lo agrega la app.
