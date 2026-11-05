# Ballena EMG → App por Bluetooth (BLE)

## Conexión
- Nombre del dispositivo: **`Ballena-EMG`**
- Servicio **Nordic UART (NUS)**:

| | UUID | Dirección |
|---|---|---|
| Servicio | `6E400001-B5A3-F393-E0A9-E50E24DCCA9E` | |
| RX | `6E400002-B5A3-F393-E0A9-E50E24DCCA9E` | app → pantalla (**write**) |
| TX | `6E400003-B5A3-F393-E0A9-E50E24DCCA9E` | pantalla → app (**notify**) |

> Probar primero con la app **nRF Connect** o **Serial Bluetooth Terminal** (modo BLE) en el teléfono,
> antes de programar FlutterFlow. En FlutterFlow se usa el paquete `flutter_blue_plus` en una *Custom Action*.

## Formato
Todo es **texto**. Cada mensaje de la pantalla es **una línea JSON que termina en `\n`**.
Los mensajes largos llegan **en varios pedazos**: la app debe **juntarlos** hasta ver `\n`
y luego hacer `jsonDecode`. Cada mensaje tiene el campo `"tipo"`.

## Comandos (la app escribe en RX)
| Comando | Qué hace |
|---|---|
| `SYNC <epoch> <tz_min>` | Pone fecha/hora. Ej: `SYNC 1762360000 -360` (segundos Unix UTC, México = -360). **Mandarlo al conectar, antes de GRABAR.** |
| `ESTADO` | Responde `{"tipo":"estado",...}` |
| `GET` | Vuelve a mandar la última sesión |

## Mensajes (la pantalla manda por TX)
| `tipo` | Cuándo |
|---|---|
| `hola` | Al conectarse (incluye el estado) |
| `reloj` | Respuesta a `SYNC`: `{"tipo":"reloj","ok":true,"hora_local":"2026-11-05 10:32:05"}` |
| `estado` | Respuesta a `ESTADO` y al presionar **GRABAR**: `{"tipo":"estado","grabando":true,"listo":false,"id":3,...,"pacienteID":"PAC-101"}` |
| `sesion` | Al presionar **ENVIAR**. Mismos campos que antes (`pacienteID`, `sesionID`, `fecha`, `hora`, `duracion_s`, `repeticiones`, `pico_medio_uV`, `activacion_media_pct_mvc`, `lecturasEMG`, `SAMPLE_INTERVAL_ms`, …) **sin** `envolvente_mV`. |
| `error` | Comando desconocido o sin datos |

Si la app **no está conectada** al presionar ENVIAR, la pantalla guarda la sesión y la manda
**sola** unos 1.5 s después de que la app se conecte.

## Custom Action (Dart, flutter_blue_plus) — idea base
```dart
final svc = Guid('6E400001-B5A3-F393-E0A9-E50E24DCCA9E');
final rxU = Guid('6E400002-B5A3-F393-E0A9-E50E24DCCA9E');
final txU = Guid('6E400003-B5A3-F393-E0A9-E50E24DCCA9E');

// 1) Buscar "Ballena-EMG" y conectar
await FlutterBluePlus.startScan(withNames: ['Ballena-EMG'], timeout: Duration(seconds: 8));
final r = await FlutterBluePlus.scanResults.firstWhere((l) => l.isNotEmpty);
final dev = r.first.device;
await FlutterBluePlus.stopScan();
await dev.connect();
await dev.requestMtu(247);               // Android: pedazos más grandes
final s = (await dev.discoverServices()).firstWhere((x) => x.uuid == svc);
final rx = s.characteristics.firstWhere((c) => c.uuid == rxU);
final tx = s.characteristics.firstWhere((c) => c.uuid == txU);

// 2) Recibir: juntar pedazos hasta '\n'
String buf = '';
await tx.setNotifyValue(true);
tx.onValueReceived.listen((bytes) {
  buf += utf8.decode(bytes);
  int i;
  while ((i = buf.indexOf('\n')) >= 0) {
    final line = buf.substring(0, i);
    buf = buf.substring(i + 1);
    final msg = jsonDecode(line);
    if (msg['tipo'] == 'sesion') {
      // guardar en Firestore / App State: msg['pacienteID'], msg['fecha'], msg['lecturasEMG'], ...
    }
  }
});

// 3) Mandar la hora
final epoch = DateTime.now().millisecondsSinceEpoch ~/ 1000;
await rx.write(utf8.encode('SYNC $epoch -360'));
```
Android: permisos `BLUETOOTH_SCAN`, `BLUETOOTH_CONNECT` (y ubicación en Android ≤ 11).
Probar en un teléfono real (el modo web/preview no tiene Bluetooth).
