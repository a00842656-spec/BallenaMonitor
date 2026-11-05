# Ballena EMG → FlutterFlow: cómo recibe la app los datos de la sesión

## Flujo en la pantalla (MODO REHABILITACIÓN)
1. **PACIENTE** (botón con personas): cada toque suma 1 → `PAC-101`, `PAC-102`, …
   Mantener presionado 1 s = restar 1 (si se equivocan). El número se guarda aunque se apague.
   El ID actual se ve encima del botón.
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
  "fecha": "2026-11-05",
  "hora": "10:32:05",
  "hora_valida": true,
  "modo": "REHABILITACION",
  "duracion_s": 84.2,
  "objetivo": 10,
  "repeticiones": 7,
  "completada": false,
  "umbral_mV": 75.0,
  "activacion_media_ms": 820,
  "pico_medio_mV": 210.4,
  "ganancia_total": 10.0,
  "envolvente_hz": 20,
  "envolvente_truncada": false,
  "repeticiones_detalle": [
    {"n": 1, "t_s": 3.20, "duracion_ms": 820, "pico_mV": 210.5, "latencia_ms": 180}
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
