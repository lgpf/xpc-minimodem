// =============================================================
// XPC: sons do modem num piezo passivo (disco de 27 mm) entre D5 (GPIO14) e G
//
//   Padrao Brasil: tom de discagem 425 Hz, DTMF do numero XPC_DIAL_NUMBER,
//   chamando 425 Hz (1 s ligado / 4 s desligado), ocupado 425 Hz (0,25/0,25 s),
//   e o handshake gravado (handshake.h, 8 kHz / 8 bits).
//
//   Tudo e tocado por uma interrupcao de timer a 8 kHz: o modem pode ficar esperando
//   a conexao TCP enquanto o "chamando" toca. A saida usa o modulador sigma-delta do
//   ESP8266 (densidade de pulsos ~ amplitude), ligado ao GPIO14.
// =============================================================
#pragma once
#include "handshake.h"

#define XPC_DIAL_NUMBER  "3372234"
#define SND_PIN          14            // D5
#define SND_RATE         8000
#ifndef GPSD
#define GPSD  ESP8266_REG(0x368)    // registrador do sigma-delta (como no core_esp8266_sigma_delta.cpp)
#endif

enum SndSegType : uint8_t { SEG_TONE, SEG_SILENCE, SEG_PCM };

struct SndSeg {
   SndSegType type;
   uint16_t   inc1, inc2;              // passo de fase por amostra (pre-calculado; 0 = sem tom)
   uint32_t   samples;
};

static SndSeg            sndSegs[48];
static volatile uint8_t  sndCount = 0;
static volatile uint8_t  sndIndex = 0;
static volatile uint32_t sndPos = 0;
static volatile bool     sndLoop = false;     // repete a lista (chamando)
static volatile bool     sndActive = false;
static uint16_t          sndPh1 = 0, sndPh2 = 0;
static volatile uint8_t  sndLevel = 3;        // ATL: 0/1 normal, 2 alto, 3 maximo (padrao)
#define SND_PIN2         13                   // D7: saida invertida no ATL3 (piezo entre D5 e D7 = dobro)

static inline void IRAM_ATTR sndOut(uint8_t v) {
   // sigma-delta: bit 16 = liga, 15..8 = prescaler, 7..0 = densidade
   GPSD = (1 << 16) | (2 << 8) | v;
}

// onda triangular (-0x4000..0x3fff) a partir da fase: para somar dois tons e decidir o bit no ATL3
static inline int IRAM_ATTR sndTri(uint16_t ph) {
   return (ph < 0x8000 ? (int)ph : (int)(0xFFFF - ph)) - 0x4000;
}

static inline void IRAM_ATTR sndBit(bool b) {
   // ATL3: GPIO direto, 0/3,3 V cheios no D5 e o contrario no D7
   if( b ) { GPOS = (1 << SND_PIN); GPOC = (1 << SND_PIN2); }
   else    { GPOC = (1 << SND_PIN); GPOS = (1 << SND_PIN2); }
}

void IRAM_ATTR sndIsr(void) {
   if( !sndActive ) {
      if( sndLevel >= 3 ) {
         GPOC = (1 << SND_PIN) | (1 << SND_PIN2);   // repouso: os dois em 0 (sem tensao no piezo)
      } else {
         sndOut(0);
      }
      return;
   }
   SndSeg &s = sndSegs[sndIndex];
   uint8_t v = 0;
   bool bit = false;
   switch( s.type ) {
      case SEG_TONE: {
         sndPh1 += s.inc1;
         int a = (sndPh1 & 0x8000) ? 1 : 0;
         int t = sndTri(sndPh1);
         if( s.inc2 ) {
            sndPh2 += s.inc2;
            a += (sndPh2 & 0x8000) ? 1 : 0;
            t += sndTri(sndPh2);
            v = (uint8_t)(a * 127);
         } else {
            v = a ? 255 : 0;
         }
         bit = t > 0;
         break;
      }
      case SEG_PCM: {
         int p = pgm_read_byte(&handshakePCM[sndPos]);
         bit = p >= 128;
         if( sndLevel == 2 ) {
            p = 128 + (p - 128) * 3;                // alto: ganho 3x (com corte)
            p = p < 0 ? 0 : (p > 255 ? 255 : p);
         }
         v = (uint8_t) p;
         break;
      }
      default:
         break;
   }
   if( sndLevel >= 3 ) {
      if( s.type == SEG_SILENCE ) {
         GPOC = (1 << SND_PIN) | (1 << SND_PIN2);
      } else {
         sndBit(bit);
      }
   } else {
      sndOut(v);
   }
   if( ++sndPos >= s.samples ) {
      sndPos = 0;
      if( ++sndIndex >= sndCount ) {
         if( sndLoop ) {
            sndIndex = 0;
         } else {
            sndActive = false;
         }
      }
   }
}

void sndInit(void) {
   pinMode(SND_PIN, OUTPUT);
   digitalWrite(SND_PIN, LOW);
   GPC(SND_PIN) |= (1 << GPCS);                // GPIO14 sai pelo sigma-delta
   GPSD = 0;
   timer1_isr_init();
   timer1_attachInterrupt(sndIsr);
   timer1_enable(TIM_DIV16, TIM_EDGE, TIM_LOOP);
   timer1_write(80000000 / 16 / SND_RATE);     // 8 kHz
}

static void sndAdd(SndSegType t, uint16_t f1, uint16_t f2, uint32_t ms) {
   if( sndCount < sizeof(sndSegs) / sizeof(sndSegs[0]) ) {
      sndSegs[sndCount].type = t;
      sndSegs[sndCount].inc1 = (uint16_t)((uint32_t)f1 * 65536u / SND_RATE);   // sem divisao na interrupcao
      sndSegs[sndCount].inc2 = (uint16_t)((uint32_t)f2 * 65536u / SND_RATE);
      sndSegs[sndCount].samples = t == SEG_PCM ? HANDSHAKE_LEN : ms * SND_RATE / 1000;
      ++sndCount;
   }
}

// saida conforme o volume (ATL): sigma-delta (0..2) ou GPIO direto em ponte (3)
static void sndSetLevel(uint8_t level) {
   sndLevel = level > 3 ? 3 : level;
   if( sndLevel >= 3 ) {
      GPSD = 0;
      GPC(SND_PIN) &= ~(1 << GPCS);               // D5 volta a ser GPIO comum
      pinMode(SND_PIN2, OUTPUT);
      GPOC = (1 << SND_PIN) | (1 << SND_PIN2);
   } else {
      GPC(SND_PIN) |= (1 << GPCS);                // D5 pelo sigma-delta
      pinMode(SND_PIN2, OUTPUT);
      digitalWrite(SND_PIN2, LOW);
   }
}

static void sndStart(bool loop) {
   sndSetLevel(settings.volume);
   sndIndex = 0;
   sndPos = 0;
   sndLoop = loop;
   sndActive = sndCount > 0;
}

void sndStop(void) {
   sndActive = false;
   sndLoop = false;
}

bool sndBusy(void) {
   return sndActive;
}

// espera o som acabar; true = o usuario digitou algo (desiste)
bool sndWait(void) {
   while( sndActive ) {
      yield();
      if( Serial.available() ) {
         sndStop();
         return true;
      }
   }
   return false;
}

// tom de discagem + DTMF do numero
void sndDial(void) {
   static const uint16_t row[4] = { 697, 770, 852, 941 };
   static const uint16_t col[3] = { 1209, 1336, 1477 };
   sndStop();
   sndCount = 0;
   sndAdd(SEG_TONE, 425, 0, 1000);
   sndAdd(SEG_SILENCE, 0, 0, 300);
   for( const char *p = XPC_DIAL_NUMBER; *p; ++p ) {
      int d = *p == '0' ? 10 : *p - '1';      // 1..9 -> 0..8, 0 -> 10
      sndAdd(SEG_TONE, row[d / 3], col[d % 3], 80);
      sndAdd(SEG_SILENCE, 0, 0, 80);
   }
   sndAdd(SEG_SILENCE, 0, 0, 600);
   sndStart(false);
}

// chamando (repete ate sndStop)
void sndRingback(void) {
   sndStop();
   sndCount = 0;
   sndAdd(SEG_TONE, 425, 0, 1000);
   sndAdd(SEG_SILENCE, 0, 0, 4000);
   sndStart(true);
}

// ocupado
void sndBusyTone(void) {
   sndStop();
   sndCount = 0;
   for( int i = 0; i < 6; ++i ) {
      sndAdd(SEG_TONE, 425, 0, 250);
      sndAdd(SEG_SILENCE, 0, 0, 250);
   }
   sndStart(false);
}

// handshake gravado
void sndHandshake(void) {
   sndStop();
   sndCount = 0;
   sndAdd(SEG_PCM, 0, 0, 0);
   sndStart(false);
}
