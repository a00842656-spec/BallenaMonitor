// Tortuga - protocolo ESP-NOW entre el control (WT32-SC01 Plus) y el XIAO ESP32-C6
//
// Este archivo debe ser IDENTICO en los dos proyectos: copialo tal cual al
// proyecto del WT32-SC01 Plus.
//
// Uso desde el control:
//   - Mientras un boton esta presionado, enviar el comando cada ~100 ms.
//   - Al soltarlo, enviar MOTOR_CMD_STOP.
//   - El XIAO detiene el motor solo si no recibe nada en MOTOR_FAILSAFE_MS.

#pragma once

#include <stdint.h>

// Canal Wi-Fi fijo para ESP-NOW. Ambos equipos deben usar el mismo.
// Si el WT32 se conecta a un router, este valor debe ser el canal del router.
constexpr uint8_t ESPNOW_CHANNEL = 1;

// Primer byte de cada mensaje, para ignorar paquetes ESP-NOW ajenos
constexpr uint8_t MOTOR_MSG_MAGIC = 0x7A;

enum MotorCmd : uint8_t {
  MOTOR_CMD_STOP = 0,
  MOTOR_CMD_FORWARD = 1,
  MOTOR_CMD_REVERSE = 2,
};

struct __attribute__((packed)) MotorMsg {
  uint8_t magic;  // siempre MOTOR_MSG_MAGIC
  uint8_t cmd;    // MotorCmd
  uint8_t speed;  // 0..255 (duty PWM)
};

// Tiempo sin mensajes tras el cual el XIAO detiene el motor
constexpr uint32_t MOTOR_FAILSAFE_MS = 300;

// Cada cuanto debe reenviar el control mientras un boton esta presionado
constexpr uint32_t MOTOR_RESEND_MS = 100;
