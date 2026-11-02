// Ver audio.h
#include <Arduino.h>
#include <AudioFileSourcePROGMEM.h>
#include <AudioGeneratorMP3.h>
#include <AudioOutputI2S.h>
#include <AudioOutputMixer.h>

#include "audio.h"

// Sonidos (MP3 a 44100 Hz en PROGMEM; ver tools/mp3_header.py)
// Todos deben ser 44100 Hz: el mezclador no convierte frecuencias.
#include "bgm.h"
#include "correct.h"
#include "wrong2.h"
#include "bubble.h"
#include "rojo.h"
#include "amarillo.h"
#include "verde.h"
#include "azul.h"

// ---- Pines del amplificador I2S integrado (conector SPEAKER) ----------
constexpr int PIN_I2S_BCLK = 36;
constexpr int PIN_I2S_LRCK = 35;
constexpr int PIN_I2S_DOUT = 37;

// ---- Volumen -------------------------------------------------------------
// MASTER_GAIN: volumen general (0.0 a 1.0). 1.0 = maximo sin distorsion digital.
// Si la bocina (8 ohm / 1 W) vibra o "raspa" a todo volumen, bajarlo a 0.85.
constexpr float MASTER_GAIN = 1.0;
//
// Volumen de cada canal. Regla para que NO haya chasquidos ni "zzz":
// Los efectos y voces llegan a ~0.79 de pico (-2 dB), asi que solos aguantan
// hasta ~1.25 sin recortarse. Lo que suena al mismo tiempo (ganancia x pico)
// debe sumar como maximo ~1.3; si se pasa, el mezclador recorta la onda y se
// oye distorsion.
//   voz sola                : 1.2 x 0.79                        = 0.95
//   voz + efecto + musica   : 0.95 + 0.15 x 0.79 + 0.2 x 1.0     = 1.27 (picos rara vez coinciden)
//   efecto + musica         : 0.95 + 0.2                        = 1.15
// No subir VOICE_GAIN / FX_GAIN por encima de 1.25.
constexpr float MUSIC_GAIN = 1.0;           // musica de fondo sola (maximo)
constexpr float MUSIC_DUCK_GAIN = 0.2;      // musica mientras suena un efecto o una voz
constexpr float FX_GAIN = 1.2;              // correcto / incorrecto / burbuja
constexpr float FX_UNDER_VOICE_GAIN = 0.15; // efecto mientras habla la voz
constexpr float VOICE_GAIN = 1.2;           // ROJO / AMARILLO / VERDE / AZUL

// Cuanto cambia el volumen por vuelta de la tarea (~1 ms) al bajar/subir la
// musica: un cambio brusco de volumen hace "clic". 0.04 = unos 20 ms de rampa.
constexpr float GAIN_RAMP_STEP = 0.04;

// ---- Buffers -------------------------------------------------------------
// Buffer DMA del I2S: 8 x 256 muestras = ~46 ms. Cubre las pausas de la tarea
// (por ejemplo mientras la pantalla dibuja una transicion).
constexpr int I2S_DMA_BUFFERS = 8;
constexpr int I2S_DMA_BUFFER_BYTES = 1024;
// Buffer del mezclador. Con 32 muestras (el valor anterior) cada vuelta de la
// tarea solo podia mezclar ~0.7 ms de audio y, con musica + efecto a la vez,
// el I2S se quedaba sin datos y repetia audio viejo: ese era el "zzz".
// Con 512 cada vuelta puede mezclar hasta ~11 ms.
constexpr int MIXER_BUFFER_SAMPLES = 512;

struct SfxData {
  const uint8_t *data;
  uint32_t len;
};

// Mismo orden que el enum Sfx (audio.h)
static const SfxData SFX_TABLE[] = {
  {correct, sizeof(correct)},    // SFX_CORRECT
  {wrong2, sizeof(wrong2)},      // SFX_WRONG
  {bubble, sizeof(bubble)},      // SFX_BUBBLE
  {rojo, sizeof(rojo)},          // SFX_ROJO
  {amarillo, sizeof(amarillo)},  // SFX_AMARILLO
  {verde, sizeof(verde)},        // SFX_VERDE
  {azul, sizeof(azul)},          // SFX_AZUL
};

static bool isVoice(Sfx sfx) {
  return sfx >= SFX_ROJO && sfx <= SFX_AZUL;
}

enum AudioCmdType : uint8_t { CMD_SFX, CMD_MUSIC_ON, CMD_MUSIC_OFF };

struct AudioCmd {
  AudioCmdType type;
  Sfx sfx;
};

// Un canal = un decodificador MP3 que entra al mezclador con su propio volumen
struct Channel {
  AudioGeneratorMP3 *mp3;
  AudioFileSourcePROGMEM *src;
  AudioOutputMixerStub *stub;
  Sfx current;       // que esta sonando (solo efecto/voz)
  float gainNow;     // volumen actual (va hacia el objetivo poco a poco)
};

static QueueHandle_t cmdQueue = nullptr;
static bool musicRequested = false;  // solo lo toca el hilo de la UI

static AudioOutputI2S *out;
static AudioOutputMixer *mixer;
static Channel music, fx, voice;

static void initChannel(Channel &ch, float gain) {
  ch.stub = mixer->NewInput();
  ch.stub->SetGain(gain);
  ch.src = new AudioFileSourcePROGMEM();
  ch.mp3 = new AudioGeneratorMP3();
  ch.current = SFX_CORRECT;
  ch.gainNow = gain;
}

static void startChannel(Channel &ch, const uint8_t *data, uint32_t len) {
  if (ch.mp3->isRunning()) {
    ch.mp3->stop();
  }
  ch.src->close();
  ch.src->open(data, len);
  ch.mp3->begin(ch.src, ch.stub);
}

static void startSfx(Channel &ch, Sfx sfx) {
  startChannel(ch, SFX_TABLE[sfx].data, SFX_TABLE[sfx].len);
  ch.current = sfx;
}

static void startMusic() {
  startChannel(music, bgm, sizeof(bgm));
}

// Avanza un canal de efecto/voz; lo detiene cuando termina el sonido
static void serviceChannel(Channel &ch) {
  if (ch.mp3->isRunning() && !ch.mp3->loop()) {
    ch.mp3->stop();
  }
}

// Mueve el volumen de un canal hacia el objetivo, sin saltos bruscos
static void rampGain(Channel &ch, float target) {
  if (ch.gainNow == target) return;
  if (ch.gainNow < target) {
    ch.gainNow = min(target, ch.gainNow + GAIN_RAMP_STEP);
  } else {
    ch.gainNow = max(target, ch.gainNow - GAIN_RAMP_STEP);
  }
  ch.stub->SetGain(ch.gainNow);
}

static void updateGains() {
  bool voiceOn = voice.mp3->isRunning();
  bool fxOn = fx.mp3->isRunning();

  rampGain(music, (voiceOn || fxOn) ? MUSIC_DUCK_GAIN : MUSIC_GAIN);
  rampGain(fx, voiceOn ? FX_UNDER_VOICE_GAIN : FX_GAIN);
  // La voz siempre a su volumen (sin rampa: tiene que oirse completa desde el inicio)
}

static void handleCmd(const AudioCmd &cmd, bool &musicOn) {
  switch (cmd.type) {
    case CMD_MUSIC_ON:
      musicOn = true;
      break;
    case CMD_MUSIC_OFF:
      musicOn = false;
      if (music.mp3->isRunning()) {
        music.mp3->stop();
      }
      break;
    case CMD_SFX:
      if (isVoice(cmd.sfx)) {
        // La voz suena de inmediato (corta la voz anterior si la hay)
        startSfx(voice, cmd.sfx);
      } else if (cmd.sfx == SFX_BUBBLE) {
        // La burbuja no corta a "correcto"/"incorrecto"
        if (!fx.mp3->isRunning() || fx.current == SFX_BUBBLE) {
          startSfx(fx, cmd.sfx);
        }
      } else {
        // Correcto / incorrecto: de inmediato
        startSfx(fx, cmd.sfx);
      }
      break;
  }
}

static void audioTask(void *) {
  bool musicOn = false;
  AudioCmd cmd;

  for (;;) {
    while (xQueueReceive(cmdQueue, &cmd, 0) == pdTRUE) {
      handleCmd(cmd, musicOn);
    }

    // Musica en bucle: al terminar vuelve a empezar
    if (musicOn) {
      if (!music.mp3->isRunning()) {
        startMusic();
      } else if (!music.mp3->loop()) {
        music.mp3->stop();
        startMusic();
      }
    }

    serviceChannel(fx);
    serviceChannel(voice);
    updateGains();

    // Si no suena nada, el mezclador manda silencio al I2S. Asi el I2S nunca
    // se queda vacio (vacio repite el ultimo audio = ruido).
    mixer->loop();

    // Ceder el CPU un momento; el buffer DMA del I2S cubre la pausa
    vTaskDelay(1);
  }
}

void audioBegin() {
  out = new AudioOutputI2S();
  out->SetBuffers(I2S_DMA_BUFFERS, I2S_DMA_BUFFER_BYTES);
  out->SetPinout(PIN_I2S_BCLK, PIN_I2S_LRCK, PIN_I2S_DOUT);
  out->SetOutputModeMono(true);  // una sola bocina
  out->SetGain(MASTER_GAIN);

  mixer = new AudioOutputMixer(MIXER_BUFFER_SAMPLES, out);
  initChannel(music, MUSIC_GAIN);
  initChannel(fx, FX_GAIN);
  initChannel(voice, VOICE_GAIN);

  cmdQueue = xQueueCreate(16, sizeof(AudioCmd));
  // Core 0: la UI (loop de Arduino) corre en el core 1
  xTaskCreatePinnedToCore(audioTask, "audio", 8192, nullptr, 3, nullptr, 0);
}

void audioPlaySfx(Sfx sfx) {
  if (cmdQueue == nullptr) return;
  AudioCmd cmd = {CMD_SFX, sfx};
  xQueueSend(cmdQueue, &cmd, 0);
}

void audioMusic(bool on) {
  if (cmdQueue == nullptr || on == musicRequested) return;
  musicRequested = on;
  AudioCmd cmd = {on ? CMD_MUSIC_ON : CMD_MUSIC_OFF, SFX_CORRECT};
  xQueueSend(cmdQueue, &cmd, 0);
}