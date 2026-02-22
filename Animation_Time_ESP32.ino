/*
  ============================================================
  ESP32 + SSD1306 OLED (U8g2) + MAX7219 8x32 (4x 8x8 chained)
  ============================================================

  ✅ OLED (SSD1306, I2C):
     - Big anime animation
     - Added comments around it for readability.

  ✅ MAX7219 8x32:
     - Shows time as:  H:MM AM  or  HH:MM PM
     - Seconds progress bar at the bottom row
     - FIXED layout: no leading zero, AM/PM has proper spacing
     - Includes flip_y support

  Wiring (as you said):
    MAX7219: VCC=5V, GND=common, DIN=GPIO23, CLK=GPIO18, CS=GPIO5
    OLED I2C: SDA=21, SCL=22, addr 0x3C

  Notes:
    - If your matrix looks upside-down, keep MX_FLIP_Y = true.
    - If it looks mirrored left-right, set MX_FLIP_X = true.
*/


// ============================================================
// ------------------------ INCLUDES --------------------------
// ============================================================

#include <U8g2lib.h>
#include <Wire.h>
#include <math.h>

#include <WiFi.h>
#include <time.h>
#include <MD_MAX72xx.h>
#include <SPI.h>


// ============================================================
// ------------------------- OLED -----------------------------
// ----------------(DRAWING/ANIMATION LOGIC)-------------------
// ============================================================

// OLED driver (128x64 SSD1306)
U8G2_SSD1306_128X64_NONAME_F_HW_I2C u8g2(U8G2_R0, U8X8_PIN_NONE);

// OLED size
static const int W = 128;
static const int H = 64;

// Yellow/blue OLED: keep drawings below 16px (top zone is yellow for me)
static const int SAFE_TOP_Y = 16;

// Eye placement (blue zone)
static const int CX = 64;
static const int CY = 40;
static const int EYE_R = 20;
static const int R_ORB = 13;

// Small helpers
static inline int ix(float v){ return (int)lroundf(v); }
static inline float clamp01(float v){ return v < 0 ? 0 : (v > 1 ? 1 : v); }
static inline float smooth01(float t){ t = clamp01(t); return t*t*(3.0f - 2.0f*t); }

// Circle hit-test (used to restrict drawing inside the eye)
bool insideCircle(int x, int y, int cx, int cy, int rr){
  int dx = x - cx;
  int dy = y - cy;
  return (dx*dx + dy*dy) <= (rr*rr);
}

// “Safe area” = only draw in blue zone (avoid yellow top area)
bool inSafe(int x, int y){
  return (x >= 0 && x < W && y >= SAFE_TOP_Y && y < H);
}

// Safe pixel drawing (prevents drawing outside bounds)
void safePixel(int x, int y){
  if(inSafe(x,y)) u8g2.drawPixel(x,y);
}

// Safe pixel drawing but only if it’s inside the eye circle
void safePixelInEye(int x, int y){
  if(inSafe(x,y) && insideCircle(x,y,CX,CY,EYE_R-1)) u8g2.drawPixel(x,y);
}

// Bresenham line (safe)
void safeLine(int x0,int y0,int x1,int y1){
  int dx = abs(x1-x0), sx = x0<x1 ? 1 : -1;
  int dy = -abs(y1-y0), sy = y0<y1 ? 1 : -1;
  int err = dx + dy;
  while(true){
    safePixel(x0,y0);
    if(x0==x1 && y0==y1) break;
    int e2 = 2*err;
    if(e2 >= dy){ err += dy; x0 += sx; }
    if(e2 <= dx){ err += dx; y0 += sy; }
  }
}

// Bresenham line, restricted inside the eye
void safeLineInEye(int x0,int y0,int x1,int y1){
  int dx = abs(x1-x0), sx = x0<x1 ? 1 : -1;
  int dy = -abs(y1-y0), sy = y0<y1 ? 1 : -1;
  int err = dx + dy;
  while(true){
    safePixelInEye(x0,y0);
    if(x0==x1 && y0==y1) break;
    int e2 = 2*err;
    if(e2 >= dy){ err += dy; x0 += sx; }
    if(e2 <= dx){ err += dx; y0 += sy; }
  }
}

// Filled disc (safe)
void safeDisc(int cx,int cy,int r){
  for(int y=-r; y<=r; y++){
    for(int x=-r; x<=r; x++){
      if(x*x + y*y <= r*r) safePixel(cx+x, cy+y);
    }
  }
}

// Filled disc inside the eye only
void safeDiscInEye(int cx,int cy,int r){
  for(int y=-r; y<=r; y++){
    for(int x=-r; x<=r; x++){
      if(x*x + y*y <= r*r) safePixelInEye(cx+x, cy+y);
    }
  }
}

// deterministic hash for stable particle / fade patterns (no flicker)
static inline uint32_t h2(int x,int y,uint32_t seed){
  uint32_t v = (uint32_t)(x*73856093u) ^ (uint32_t)(y*19349663u) ^ (seed*83492791u);
  v ^= (v >> 13);
  v *= 0x5bd1e995u;
  v ^= (v >> 15);
  return v;
}


// ============================================================
// ---------------- OLED ANIMATION STATES ---------------------
// ============================================================

enum AnimState {
  DOTS_1,
  DOTS_2,
  DOTS_3,
  UPSHIFT,
  ACCEL_TO_BREAK,
  BREAK_PARTICLES,
  RASENGAN,
  RAS_COMPRESS,
  KONOHA_DRAW,
  KONOHA_HOLD,

  KONOHA_IGNITE,
  FLAME_RING,

  JOLLY_DRAW,
  JOLLY_HOLD,
  JOLLY_DISSOLVE,

  OCEAN_FORM,
  MERRY_SAIL_IN,
  MERRY_HOLD,

  ZORO_ZCUTS,
  ZORO_HOLD,

  ZORO_AURA_CHARGE,
  ZORO_ASURA_FLASH,

  ZORO_REFORM_EYE
};

AnimState state = DOTS_1;
unsigned long stateStart = 0;
uint32_t stateSeed = 1; // stable per-state seed (prevents flicker)

// Timings (ms)
const uint16_t T_STAGE_HOLD   = 2200;
const uint16_t T_UPSHIFT      = 650;
const uint16_t T_ACCEL_BREAK  = 1100;
const uint16_t T_BREAK        = 900;

const uint16_t T_RAS          = 2600;
const uint16_t T_COMPRESS     = 850;
const uint16_t T_KONOHA_DRAW  = 1200;
const uint16_t T_KONOHA_HOLD  = 2000;

const uint16_t T_KONOHA_IGNITE = 850;
const uint16_t T_FLAME_RING    = 1350;

const uint16_t T_JOLLY_DRAW    = 1400;
const uint16_t T_JOLLY_HOLD    = 1700;
const uint16_t T_JOLLY_DISS    = 900;

const uint16_t T_OCEAN_FORM    = 900;
const uint16_t T_MERRY_IN      = 1900;
const uint16_t T_MERRY_HOLD    = 1700;

const uint16_t T_ZCUTS         = 850;
const uint16_t T_ZHOLD         = 1400;
const uint16_t T_ZAURA         = 900;
const uint16_t T_ASURA         = 520;
const uint16_t T_REFORM        = 1050;

// Motion parameters
float ang = 0.0f;
float angSpeed = 0.05f;
int dotCount = 1;


// ============================================================
// ------------------- OLED DRAW HELPERS ----------------------
// ============================================================

// Eye frame + pupil
void drawEyeBase(){
  u8g2.drawCircle(CX, CY, EYE_R);
  u8g2.drawDisc(CX, CY, 3, U8G2_DRAW_ALL);
}

// Draw a single tomoe (orb + tail)
void drawOneTomoe(float t){
  int hx = CX + ix(cosf(t) * R_ORB);
  int hy = CY + ix(sinf(t) * R_ORB);

  u8g2.drawDisc(hx, hy, 3, U8G2_DRAW_ALL);

  float tailDir = t - 1.05f;
  int tx = hx + ix(cosf(tailDir) * 5);
  int ty = hy + ix(sinf(tailDir) * 5);
  u8g2.drawDisc(tx, ty, 1, U8G2_DRAW_ALL);

  u8g2.drawPixel((hx + tx)/2, (hy + ty)/2);
}

// Draw N tomoe evenly around the eye
void drawTomoeSet(int n, float a){
  if(n <= 0) return;
  float step = 2.0f*(float)M_PI / (float)n;
  for(int k=0;k<n;k++) drawOneTomoe(a + k*step);
}


// ============================================================
// --------------------- PARTICLES (OLED) ---------------------
// ============================================================

struct Particle { float x,y,vx,vy; };
static const int NPART = 120;
Particle pt[NPART];

// Spawn shards around orbit ring
void spawnShards(){
  for(int i=0;i<NPART;i++){
    float a = (float)random(0, 628) / 100.0f;
    float rr = (float)random(R_ORB-3, R_ORB+3);
    pt[i].x = CX + cosf(a)*rr;
    pt[i].y = CY + sinf(a)*rr;

    float sp = (float)random(8, 22) / 10.0f;
    float tang = 1.7f;
    pt[i].vx = (-sinf(a))*tang + cosf(a)*sp;
    pt[i].vy = ( cosf(a))*tang + sinf(a)*sp;
  }
}

// Update shards and draw them inside eye
void updateShards(float swirl){
  for(int i=0;i<NPART;i++){
    float dx = pt[i].x - CX;
    float dy = pt[i].y - CY;
    float len = sqrtf(dx*dx + dy*dy);
    if(len < 0.001f) len = 0.001f;

    float tx = -dy/len;
    float ty =  dx/len;

    pt[i].vx += tx * swirl;
    pt[i].vy += ty * swirl;

    pt[i].x += pt[i].vx;
    pt[i].y += pt[i].vy;

    int x = ix(pt[i].x);
    int y = ix(pt[i].y);

    // Bounce on eye boundary
    if(!insideCircle(x,y,CX,CY,EYE_R-2)){
      float nx = dx/len;
      float ny = dy/len;
      float dot = pt[i].vx*nx + pt[i].vy*ny;
      pt[i].vx = pt[i].vx - 2*dot*nx;
      pt[i].vy = pt[i].vy - 2*dot*ny;

      pt[i].x = CX + nx*(EYE_R-3);
      pt[i].y = CY + ny*(EYE_R-3);
      x = ix(pt[i].x);
      y = ix(pt[i].y);
    }

    // Damping
    pt[i].vx *= 0.97f;
    pt[i].vy *= 0.97f;

    // Draw
    if(inSafe(x,y) && insideCircle(x,y,CX,CY,EYE_R-1)){
      u8g2.drawPixel(x,y);
      if((i % 11)==0) safePixelInEye(x+1,y);
    }
  }
}


// ============================================================
// ---------------------- RASENGAN (OLED) ---------------------
// ============================================================

void drawRasengan(float a){
  for(int yy=-6; yy<=6; yy++){
    for(int xx=-6; xx<=6; xx++){
      if(xx*xx + yy*yy <= 36) safePixelInEye(CX+xx, CY+yy);
    }
  }

  for(int k=0;k<7;k++){
    float base = a*1.15f + k*(2.0f*(float)M_PI/7.0f);
    for(int s=0;s<16;s++){
      float rr = 3 + s*1.05f;
      float tt = base + s*0.32f;
      int x = CX + ix(cosf(tt)*rr);
      int y = CY + ix(sinf(tt)*rr);
      safePixelInEye(x,y);
      if((s % 6)==0) safePixelInEye(x+1,y);
    }
  }

  for(int i=0;i<90;i++){
    float t = ((float)random(0, 628) / 100.0f) + sinf(a*2.0f + i)*0.03f;
    int d = random(5, EYE_R-2);
    int x = CX + ix(cosf(t)*d);
    int y = CY + ix(sinf(t)*d);
    safePixelInEye(x,y);
    if(i % 3 == 0) safePixelInEye(x + random(-1,2), y + random(-1,2));
  }
}

void drawCompressBall(float k){
  float rad = 18.0f - k*16.0f;
  int r = (int)lroundf(rad);
  if(r < 2) r = 2;

  for(int i=0;i<120;i++){
    float a = (float)random(0, 628) / 100.0f;
    float d0 = (float)random(2, 22);
    float d = d0 * (1.0f - k);
    int x = CX + ix(cosf(a)*d);
    int y = CY + ix(sinf(a)*d);
    safePixelInEye(x,y);
  }
  safeDiscInEye(CX, CY, r);
}


// ============================================================
// ------------------ KONOHA SYMBOL (OLED) --------------------
// ============================================================

void drawKonohaSymbol(float progress){
  progress = clamp01(progress);

  const int KX = 64;
  const int KY = 42;
  const int OUT_R = 19;

  const int SP_TURNS = 3;
  int totalSteps = 220;
  int steps = (int)lroundf(totalSteps * progress);

  // Spiral build
  for(int i=0;i<steps;i++){
    float tt = (float)i / (float)(totalSteps-1);
    float a = tt * (float)SP_TURNS * 2.0f*(float)M_PI;
    float rr = 2.0f + tt * (OUT_R - 2.0f);
    int x = KX + ix(cosf(a) * rr);
    int y = KY + ix(sinf(a) * rr);
    safePixel(x,y);
    if(i % 3 == 0){ safePixel(x+1,y); safePixel(x,y+1); }
  }

  // Triangle build
  if(progress > 0.55f){
    float k = clamp01((progress - 0.55f) / 0.45f);
    int ax = KX - 28, ay = KY + 14;
    int bx = KX - 10, by = KY + 14;
    int cx2 = KX - 16, cy2 = KY - 10;

    // A line is removed to get the correct overall shape
    safeLine(ax,ay, ax + (int)lroundf((bx-ax)*k), ay + (int)lroundf((by-ay)*k));
    safeLine(cx2,cy2, cx2 + (int)lroundf((ax-cx2)*k), cy2 + (int)lroundf((ay-cy2)*k));
  }

  // Tail line
  if(progress > 0.70f){
    float k = clamp01((progress - 0.70f) / 0.30f);
    int x0 = KX + 16, y0 = KY - 16;
    int x1 = KX + 28, y1 = KY - 24;
    safeLine(x0,y0, x0 + (int)lroundf((x1-x0)*k), y0 + (int)lroundf((y1-y0)*k));
  }
}


// ============================================================
// ------- KONOHA IGNITE / FLAME RING / JOLLY / OCEAN ---------
// ============================================================

void drawKonohaIgnite(float k){
  drawKonohaSymbol(1.0f);
  int burst = (int)(90 * clamp01(k));
  for(int i=0;i<burst;i++){
    float a = (float)random(0,628)/100.0f;
    float d = random(3, EYE_R-1);
    int x = CX + ix(cosf(a)*d);
    int y = CY + ix(sinf(a)*d);
    safePixelInEye(x,y);
  }
}

void drawFlameRing(float phase, float collapse){
  float baseR = (float)(EYE_R - 1);
  float r = baseR * (1.0f - clamp01(collapse));
  if(r < 2.0f) r = 2.0f;

  int spikes = 20;
  for(int i=0;i<spikes;i++){
    float a = phase*1.35f + i*(2.0f*(float)M_PI/spikes);
    float wave = sinf(a*3.0f + phase*4.0f) * 3.0f;
    float rr = r + wave;

    int x = CX + ix(cosf(a) * rr);
    int y = CY + ix(sinf(a) * rr);
    safePixelInEye(x,y);

    if(i % 3 == 0){
      int x2 = CX + ix(cosf(a)*(rr-3));
      int y2 = CY + ix(sinf(a)*(rr-3));
      safePixelInEye(x2,y2);
    }
  }
}

void semiArcInEye(int cx,int cy,int rx,int ry,int steps,int endStep){
  if(endStep < 0) endStep = 0;
  if(endStep > steps) endStep = steps;
  for(int i=0;i<=endStep;i++){
    float t = (float)i/(float)steps;
    float a = (float)M_PI * (1.0f - t);
    int x = cx + ix(cosf(a) * rx);
    int y = cy - ix(sinf(a) * ry);
    safePixelInEye(x,y);
    if((i % 9)==0) safePixelInEye(x, y+1);
  }
}

void drawJolly(float p){
  p = clamp01(p);

  const int SX = CX;
  const int SY = CY - 1;

  float pb = clamp01(p/0.30f);
  float ps = clamp01((p-0.30f)/0.40f);
  float pf = clamp01((p-0.70f)/0.30f);

  if(p > 0.01f){
    int ax0 = SX - 16, ay0 = SY + 10;
    int ax1 = SX + 16, ay1 = SY - 6;
    int bx0 = SX - 16, by0 = SY - 6;
    int bx1 = SX + 16, by1 = SY + 10;

    int xa = ax0 + (int)lroundf((ax1-ax0)*pb);
    int ya = ay0 + (int)lroundf((ay1-ay0)*pb);
    int xb = bx0 + (int)lroundf((bx1-bx0)*pb);
    int yb = by0 + (int)lroundf((by1-by0)*pb);

    safeLineInEye(ax0,ay0, xa,ya);
    safeLineInEye(bx0,by0, xb,yb);

    if(pb > 0.70f){
      auto boneEnd = [&](int x,int y){
        safeDiscInEye(x,y,2);
        safeDiscInEye(x-2,y,1);
        safeDiscInEye(x+2,y,1);
        safeDiscInEye(x,y-2,1);
        safeDiscInEye(x,y+2,1);
      };
      boneEnd(ax0,ay0); boneEnd(ax1,ay1);
      boneEnd(bx0,by0); boneEnd(bx1,by1);
    }
  }

  if(p > 0.30f){
    int steps = 110;
    int endStep = (int)lroundf(steps * ps);
    for(int i=0;i<=endStep;i++){
      float tt = (float)i/(float)steps;
      float a = tt * 2.0f*(float)M_PI;
      int x = SX + ix(cosf(a)*9);
      int y = (SY + 3) + ix(sinf(a)*9);
      safePixelInEye(x,y);
      if((i%10)==0) safePixelInEye(x, y+1);
    }
    if(ps > 0.55f){
      float kj = clamp01((ps - 0.55f)/0.45f);
      int left  = SX - 8;
      int right = SX + 8;
      int yJaw  = SY + 12;
      int xmidL = left + (int)lroundf((right-left)*kj);
      safeLineInEye(left, yJaw, xmidL, yJaw);
    }
  }

  if(ps > 0.12f){
    if(ps > 0.20f){
      for(int i=-12;i<=12;i++){
        float tt = (float)(i+12)/24.0f;
        int x = SX + i;
        int y = (SY - 6) + (int)lroundf(sinf(tt*(float)M_PI)*1.0f);
        safePixelInEye(x,y);
        if((i%3)==0) safePixelInEye(x, y+1);
      }
    }
    if(ps > 0.40f){
      float kd = clamp01((ps - 0.40f)/0.60f);
      int domeSteps = 60;
      int domeEnd = (int)lroundf(domeSteps * kd);

      semiArcInEye(SX, SY - 6, 8, 5, domeSteps, domeEnd);

      if(ps > 0.70f){
        for(int i=-9;i<=9;i++) safePixelInEye(SX+i, SY-8);
      }
      if(ps > 0.82f){
        safeLineInEye(SX-4, SY-12, SX-2, SY-11);
        safeLineInEye(SX+1, SY-12, SX+3, SY-11);
        safeLineInEye(SX+5, SY-11, SX+6, SY-10);
      }
    }
  }

  if(p > 0.70f){
    if(pf > 0.12f){
      safeDiscInEye(SX - 5, SY + 3, 3);
      safeDiscInEye(SX + 5, SY + 3, 3);
    }
    if(pf > 0.28f) safeDiscInEye(SX, SY + 7, 1);

    if(pf > 0.42f){
      float km = clamp01((pf - 0.42f)/0.58f);
      int mx0 = SX - 8, mx1 = SX + 8;
      int my0 = SY + 9, my1 = SY + 14;

      int xR = mx0 + (int)lroundf((mx1-mx0)*km);
      safeLineInEye(mx0,my0, xR,my0);
      safeLineInEye(mx0,my1, xR,my1);

      int yR = my0 + (int)lroundf((my1-my0)*km);
      safeLineInEye(mx0,my0, mx0,yR);
      safeLineInEye(mx1,my0, mx1,yR);

      if(km > 0.55f){
        for(int x=mx0+3; x<=mx1-3; x+=4) safeLineInEye(x,my0+1, x,my1-1);
        safeLineInEye(mx0+1, (my0+my1)/2, mx1-1, (my0+my1)/2);
      }
    }
  }
}

void drawJollyDissolve(float k){
  k = clamp01(k);
  if(k < 0.40f) drawJolly(1.0f);

  int n = 170;
  int live = (int)lroundf(n * (1.0f - k));
  for(int i=0;i<live;i++){
    float a = (float)random(0, 628) / 100.0f;
    float d = random(2, EYE_R-2);
    int x = CX + ix(cosf(a)*d);
    int y = CY + ix(sinf(a)*d);
    safePixelInEye(x,y);
  }
}

void drawOcean(float phase, float intensity){
  intensity = clamp01(intensity);
  if(intensity <= 0.001f) return;

  int yBase = 46;
  int amp1  = (int)lroundf(2 + 3*intensity);
  int amp2  = (int)lroundf(1 + 2*intensity);

  for(int x=0; x<W; x++){
    float fx = (float)x;

    int y1 = yBase + ix(sinf(fx*0.18f + phase*1.4f) * amp1);
    int y2 = (yBase+6) + ix(sinf(fx*0.14f + phase*1.0f + 1.2f) * amp2);
    int y3 = (yBase+11) + ix(sinf(fx*0.11f + phase*0.8f + 2.4f) * (amp2));

    safePixel(x,y1);
    if((x%4)==0 && intensity > 0.25f) safePixel(x, y1+1);

    safePixel(x,y2);
    if((x%6)==0 && intensity > 0.35f) safePixel(x, y2+1);

    safePixel(x,y3);
  }

  int spr = (int)lroundf(18 * intensity);
  for(int i=0;i<spr;i++){
    int x = random(0,W);
    int y = random(yBase-7, yBase+2);
    safePixel(x,y);
  }
}


// ============================================================
// --------------- GOING MERRY + ZORO PARTS -------------------
// ============================================================

// ---------------- GOING MERRY (CLEAN SILHOUETTE VERSION) ----------------
// Made for 128x64 OLED: bolder hull + clearer sail + simpler sheep head.
// x0,y0 = reference point

void drawGoingMerryLong(int x0, int y0){
  // Overall size tuned for your scene (ocean at ~46, ship at y0~36)
  const int L = 22;               // half-length of hull
  const int deckY = y0 + 3;       // deck line
  const int hullMidY = y0 + 6;    // hull belly
  const int keelY = y0 + 10;      // bottom

  // ---------- HULL (thicker + cleaner) ----------
  // Deck (thick)
  safeLine(x0-L, deckY, x0+L, deckY);
  safeLine(x0-L, deckY+1, x0+L, deckY+1);

  // Bottom (keel) (thick)
  safeLine(x0-L+3, keelY, x0+L-3, keelY);
  safeLine(x0-L+3, keelY+1, x0+L-3, keelY+1);

  // Bow (front) curve (right side)
  safeLine(x0+L, deckY,   x0+L+4, hullMidY);
  safeLine(x0+L+4, hullMidY, x0+L-2, keelY);
  safePixel(x0+L+3, hullMidY+1);
  safePixel(x0+L+2, hullMidY+2);

  // Stern (back) curve (left side)
  safeLine(x0-L, deckY,   x0-L-4, hullMidY);
  safeLine(x0-L-4, hullMidY, x0-L+2, keelY);
  safePixel(x0-L-3, hullMidY+1);

  // A couple deck dots so it feels like a ship
  for(int x = x0-L+4; x <= x0+L-6; x += 5) safePixel(x, deckY-1);

  // ---------- CABIN (simple block) ----------
  int cabX0 = x0 + 2;
  int cabX1 = x0 + 12;
  int cabY0 = deckY - 4;
  int cabY1 = deckY + 2;

  safeLine(cabX0, cabY0, cabX1, cabY0);
  safeLine(cabX0, cabY1, cabX1, cabY1);
  safeLine(cabX0, cabY0, cabX0, cabY1);
  safeLine(cabX1, cabY0, cabX1, cabY1);

  // cabin windows
  safeLine(cabX0+2, cabY0+2, cabX1-2, cabY0+2);

  // ---------- MAIN MAST + BOOM ----------
  int mastX = x0 - 2;
  safeLine(mastX, deckY, mastX, y0 - 15);     // mast
  safeLine(mastX, y0 - 7, mastX + 14, y0 - 7); // boom

  // ---------- MAIN SAIL (bold triangle/quad so it reads well) ----------
  // Outer sail shape
  safeLine(mastX,     y0 - 14, mastX + 12, y0 - 11);
  safeLine(mastX + 12,y0 - 11, mastX + 12, y0 + 1);
  safeLine(mastX + 12,y0 + 1,  mastX,      y0 + 1);

  // Fill-ish ribs (makes it look like cloth)
  safeLine(mastX+2,  y0-11, mastX+10, y0-10);
  safeLine(mastX+2,  y0-8,  mastX+10, y0-7);
  safeLine(mastX+2,  y0-5,  mastX+10, y0-4);

  // ---------- FLAGS (simple + readable) ----------
  safeLine(mastX, y0-15, mastX+7, y0-18);
  safeLine(mastX+7, y0-18, mastX+3, y0-15);
  safePixel(mastX+6, y0-17);

  int rearX = x0 + 15;
  safeLine(rearX, deckY, rearX, y0 - 10);
  safeLine(rearX, y0 - 10, rearX + 6, y0 - 12);
  safeLine(rearX + 6, y0 - 12, rearX + 3, y0 - 10);

  // ---------- SHEEP HEAD (bigger + cleaner) ----------
  // Making it read as "face" at a glance: big circle + 2 horns + mouth
  int faceX = x0 - L - 6;
  int faceY = deckY + 2;

  safeDisc(faceX, faceY, 3);            // head
  safePixel(faceX-4, faceY-1);          // horn L
  safePixel(faceX-3, faceY-2);
  safePixel(faceX+4, faceY-1);          // horn R
  safePixel(faceX+3, faceY-2);

  safePixel(faceX-1, faceY);            // eyes (tiny)
  safePixel(faceX+1, faceY);
  safeLine(faceX-2, faceY+2, faceX+2, faceY+2); // mouth

  // Attach head to hull
  safeLine(x0-L+1, faceY, x0-L+3, faceY);

  // ---------- SPLASH (tiny but readable) ----------
  safePixel(x0-L+6, keelY+2);
  safePixel(x0-L+14,keelY+2);
  safePixel(x0+2,   keelY+2);
  safePixel(x0+12,  keelY+2);
  safePixel(x0+20,  keelY+2);
}


// ============================================================
// ----------------- Z CUTS + AURA + FLASH --------------------
// ============================================================

void drawZCutsGated(float p, float fade, uint32_t seed){
  p = clamp01(p);
  fade = clamp01(fade);

  int dots = 18;
  for(int i=0;i<dots;i++){
    uint32_t hv = h2(i, 123, seed);
    int x = (int)(hv % (uint32_t)W);
    int y = SAFE_TOP_Y + (int)((hv >> 8) % (uint32_t)(H - SAFE_TOP_Y));
    if((h2(x,y,seed) & 255u) > (uint32_t)(fade*255.0f)) safePixel(x,y);
  }

  float s1 = clamp01(p / 0.34f);
  float s2 = clamp01((p - 0.34f) / 0.33f);
  float s3 = clamp01((p - 0.67f) / 0.33f);

  int thick = 1;

  auto gatedLine = [&](int x0,int y0,int x1,int y1,float pp){
    pp = clamp01(pp);
    int xe = x0 + (int)lroundf((x1-x0)*pp);
    int ye = y0 + (int)lroundf((y1-y0)*pp);

    int dx = abs(xe-x0), sx = x0<xe ? 1 : -1;
    int dy = -abs(ye-y0), sy = y0<ye ? 1 : -1;
    int err = dx + dy;
    int x = x0, y = y0;
    while(true){
      for(int t=-thick; t<=thick; t++){
        int ox = (abs(ye-y0) > abs(xe-x0)) ? t : 0;
        int oy = (abs(ye-y0) > abs(xe-x0)) ? 0 : t;
        int xx = x+ox, yy = y+oy;
        if(inSafe(xx,yy)){
          if((h2(xx,yy,seed) & 255u) > (uint32_t)(fade*255.0f)) u8g2.drawPixel(xx,yy);
        }
      }
      if(x==xe && y==ye) break;
      int e2 = 2*err;
      if(e2 >= dy){ err += dy; x += sx; }
      if(e2 <= dx){ err += dx; y += sy; }
    }
  };

  gatedLine(6,  22, 122, 20, s1);
  gatedLine(118,22, 10,  58, s2);
  gatedLine(6,  58, 122, 56, s3);

  if(p > 0.40f && fade < 0.65f){
    safePixel(118, 22);
    safePixel(10,  58);
  }
}

void drawZoroAura(float k){
  k = clamp01(k);

  int bolts = 6 + (int)lroundf(10 * k);

  const int xA0=6,   yA0=22, xA1=122, yA1=20;
  const int xB0=118, yB0=22, xB1=10,  yB1=58;
  const int xC0=6,   yC0=58, xC1=122, yC1=56;

  auto boltOnSegment = [&](int x0,int y0,int x1,int y1,int i, float amp){
    float t = (float)((h2(i,7,stateSeed) % 1000u) / 1000.0f);
    int px = x0 + (int)lroundf((x1-x0)*t);
    int py = y0 + (int)lroundf((y1-y0)*t);

    int dx = x1-x0;
    int dy = y1-y0;
    float len = sqrtf((float)(dx*dx + dy*dy));
    if(len < 1.0f) len = 1.0f;
    float nx = -(float)dy / len;
    float ny =  (float)dx / len;

    int j = (int)( (int32_t)(h2(i,9,stateSeed) & 15u) - 8 );
    int ox = (int)lroundf(nx * (j*amp));
    int oy = (int)lroundf(ny * (j*amp));

    int x = px + ox;
    int y = py + oy;

    int steps = 6;
    int lx = x, ly = y;
    for(int s=1;s<=steps;s++){
      float kk = (float)s/(float)steps;
      int tx = x + (int)lroundf(nx * sinf(kk*6.28f + i)*amp*2.0f);
      int ty = y + (int)lroundf(ny * sinf(kk*6.28f + i)*amp*2.0f);
      safeLine(lx,ly, tx,ty);
      lx = tx; ly = ty;
    }

    if(k > 0.35f){
      safePixel(x, y);
      safePixel(x+1, y);
      safePixel(x, y+1);
    }
  };

  float amp = 1.0f + 2.2f*k;

  for(int i=0;i<bolts;i++){
    uint32_t pick = h2(i,3,stateSeed) % 3u;
    if(pick==0) boltOnSegment(xA0,yA0,xA1,yA1,i, amp);
    else if(pick==1) boltOnSegment(xB0,yB0,xB1,yB1,i, amp);
    else boltOnSegment(xC0,yC0,xC1,yC1,i, amp);
  }

  int sparks = (int)lroundf(30 * k);
  for(int i=0;i<sparks;i++){
    uint32_t hv = h2(i,11,stateSeed);
    int x = 20 + (int)(hv % 88u);
    int y = SAFE_TOP_Y + 2 + (int)((hv >> 8) % 44u);
    safePixel(x,y);
  }
}

void drawAsuraFlash(float k){
  k = clamp01(k);

  int echo = (k < 0.33f) ? 0 : (k < 0.66f ? 1 : 2);

  const int xA0=6,   yA0=22, xA1=122, yA1=20;
  const int xB0=118, yB0=22, xB1=10,  yB1=58;
  const int xC0=6,   yC0=58, xC1=122, yC1=56;

  auto drawEcho = [&](int ox,int oy){
    safeLine(xA0+ox,yA0+oy,xA1+ox,yA1+oy);
    safeLine(xB0+ox,yB0+oy,xB1+ox,yB1+oy);
    safeLine(xC0+ox,yC0+oy,xC1+ox,yC1+oy);

    safePixel(118+ox,22+oy);
    safePixel(10+ox, 58+oy);
  };

  drawEcho(0,0);
  if(echo >= 1) drawEcho(1,-1);
  if(echo >= 2) drawEcho(-1,1);

  int n = 40 + (int)lroundf(30*k);
  for(int i=0;i<n;i++){
    float a = (float)((h2(i,17,stateSeed)%628u))/100.0f;
    float d = 10.0f + (float)((h2(i,19,stateSeed)%120u))/10.0f;
    int x = CX + ix(cosf(a)*d);
    int y = CY + ix(sinf(a)*d);
    safePixel(x,y);
  }
}

void drawReformEye(float k){
  k = clamp01(k);

  int ringSteps = 90;
  int end = (int)lroundf(ringSteps * k);

  for(int i=0;i<end;i++){
    float tt = (float)i/(float)ringSteps;
    float a = tt * 2.0f*(float)M_PI;
    int x = CX + ix(cosf(a)*EYE_R);
    int y = CY + ix(sinf(a)*EYE_R);
    safePixel(x,y);
    if((i%12)==0) safePixel(x, y+1);
  }

  if(k > 0.35f){
    int r = (int)lroundf(1 + 3*(k-0.35f)/0.65f);
    if(r < 1) r = 1;
    if(r > 3) r = 3;
    safeDisc(CX, CY, r);
  }
}

// ================= OLED TOP (YELLOW ZONE) =================
// Yellow zone is y = 0..15 on your OLED.
// We'll draw a centered title there every frame.
static void drawTopTitle() {
  const char* title = "Uzumaki D Scar";

  // Pick a readable small font (fits in 16px height).
  // If you want thinner/thicker, we can swap font later.
  u8g2.setFont(u8g2_font_6x12_tr);

  // Center horizontally
  int w = u8g2.getStrWidth(title);
  int x = (128 - w) / 2;

  // y is baseline (not top). 12 is safe for 6x12 font in yellow zone.
  int y = 12;

  u8g2.drawStr(x, y, title);
}


// ============================================================
// ------------------ OLED STATE CONTROL ----------------------
// ============================================================

void setAnimState(int s){
  state = (AnimState)s;
  stateStart = millis();
  stateSeed = (uint32_t)esp_random();
}

void resetToStart(){
  ang = 0.0f;
  angSpeed = 0.05f;
  dotCount = 1;
  setAnimState(DOTS_1);
}


// ============================================================
// =================== MAX7219 CLOCK PART =====================
// ============================================================


// ---------------- MAX7219 hardware config -------------------
#define MX_HWTYPE   MD_MAX72XX::FC16_HW  // Can also use GENERIC_HW and ICSTATION_HW
#define MX_DEVICES  4
#define MX_DIN      23
#define MX_CLK      18
#define MX_CS       5
MD_MAX72XX mx(MX_HWTYPE, MX_DIN, MX_CLK, MX_CS, MX_DEVICES);

// ---------------- Orientation fixes -------------------------
// flip_y = true means row 0 becomes row 7 (vertical flip).
// flip_x = true means column 0 becomes column 31 (mirror).
static const bool MX_FLIP_Y = true;   // keeping true because it worked for me
static const bool MX_FLIP_X = false;  // set true only if mirrored L/R

// ---------------- WiFi + NTP (India) ------------------------
static const char* WIFI_SSID = "WIFI_NAME"; // Put your wifi name
static const char* WIFI_PASS = "WIFI_PASS"; // put your wifi password
static const long  GMT_OFFSET_SEC      = 19800; // +05:30
static const int   DAYLIGHT_OFFSET_SEC = 0;
static const char* NTP1 = "pool.ntp.org";
static const char* NTP2 = "time.google.com";
static const char* NTP3 = "time.nist.gov";

// ---------------- 32 columns buffer -------------------------
// Each entry is ONE column (x=0..31). Bits represent rows (y=0..7).
uint8_t mxCol[32];


// ============================================================
// ------------------- 3x5 MINI FONT --------------------------
// ============================================================

// Digits are stored as 5 rows, each row is 3 bits wide.
static const uint8_t DIG3x5[10][5] = {
  {0b111,0b101,0b101,0b101,0b111}, //0
  {0b010,0b110,0b010,0b010,0b111}, //1
  {0b111,0b001,0b111,0b100,0b111}, //2
  {0b111,0b001,0b111,0b001,0b111}, //3
  {0b101,0b101,0b111,0b001,0b001}, //4
  {0b111,0b100,0b111,0b001,0b111}, //5
  {0b111,0b100,0b111,0b101,0b111}, //6
  {0b111,0b001,0b001,0b001,0b001}, //7
  {0b111,0b101,0b111,0b101,0b111}, //8
  {0b111,0b101,0b111,0b001,0b111}  //9
};

// Letters for AM/PM
static const uint8_t CH_A[5] = {0b010,0b101,0b111,0b101,0b101};
static const uint8_t CH_P[5] = {0b110,0b101,0b110,0b100,0b100};
static const uint8_t CH_M[5] = {0b101,0b111,0b111,0b101,0b101};


// ============================================================
// ---------------- MAX7219 DRAW PRIMITIVES -------------------
// ============================================================

// Clear the column buffer
static inline void mxClearBuf() {
  for (int i = 0; i < 32; i++) mxCol[i] = 0;
}

// Set one pixel (x=0..31, y=0..7) into the buffer
static inline void mxSetPx(int x, int y, bool on=true) {
  // Apply requested flips
  if (MX_FLIP_X) x = 31 - x;
  if (MX_FLIP_Y) y = 7  - y;

  if (x < 0 || x >= 32 || y < 0 || y >= 8) return;

  if (on) mxCol[x] |=  (1u << y);
  else    mxCol[x] &= ~(1u << y);
}

// Draw a 3x5 glyph at (x,y)
static inline void mxDraw3x5(int x, int y, const uint8_t rows5[5]) {
  for (int r = 0; r < 5; r++) {
    uint8_t bits = rows5[r];
    for (int c = 0; c < 3; c++) {
      if (bits & (1 << (2 - c))) mxSetPx(x + c, y + r, true);
    }
  }
}

// Draw a digit 0..9
static inline void mxDrawDigit(int x, int y, int d) {
  if (d < 0 || d > 9) return;
  mxDraw3x5(x, y, DIG3x5[d]);
}

// Draw the colon ":" (2 dots)
static inline void mxDrawColon(int x, int y, bool on) {
  if (!on) return;
  mxSetPx(x, y + 1, true);
  mxSetPx(x, y + 3, true);
}

// Push buffer → MAX7219 modules
static void mxFlush() {
  mx.control(MD_MAX72XX::UPDATE, MD_MAX72XX::OFF);
  for (int x = 0; x < 32; x++) mx.setColumn(x, mxCol[x]);
  mx.control(MD_MAX72XX::UPDATE, MD_MAX72XX::ON);
}


// ============================================================
// ---------------- CLOCK DISPLAY FUNCTIONS -------------------
// ============================================================

// Bottom row (row 7) is a progress bar for seconds (0..59)
static void drawSecondsBarRow7(int ss) {
  int filled = (ss * 32) / 60; // 0..31
  for (int x = 0; x < filled; x++) mxSetPx(x, 7, true);
}

/*
  FIXED CLOCK LAYOUT
  - No leading 0 for hour (so 7:57 not 07:57)
  - AM/PM on the right with clean gap
  - Colon blinks each second
  - Seconds bar stays at bottom
*/
static void mxDrawClock(int hh24, int mm, int ss) {
  bool pm = (hh24 >= 12);
  int h12 = hh24 % 12;
  if (h12 == 0) h12 = 12;

  int hT = h12 / 10;   // 0 or 1
  int hU = h12 % 10;   // 0..9
  int mT = mm / 10;
  int mU = mm % 10;

  const int y = 1;     // digits in rows 1..5

  // ---- spacing rules ----
  const int DIG_W = 3;     // 3x5 font width
  const int GAP   = 1;     // exactly 1 pixel gap everywhere
  const int COLON_W = 1;

  // AM/PM width: "A/P" (3) + gap(1) + "M"(3) = 7
  const int AMPM_W = DIG_W + GAP + DIG_W; // 7

  // Minutes width: Mt(3) + gap(1) + Mu(3) = 7
  const int MIN_W = DIG_W + GAP + DIG_W;  // 7

  // Hour width:
  //  - if 2-digit hour: Ht(3)+gap(1)+Hu(3) = 7
  //  - if 1-digit hour: Hu(3) = 3
  int HOUR_W = (hT == 0) ? DIG_W : (DIG_W + GAP + DIG_W);

  // Whole visible line width:
  // hour + gap + colon + gap + minutes + gap + ampm
  int totalW = HOUR_W + GAP + COLON_W + GAP + MIN_W + GAP + AMPM_W;

  // Center the whole thing in 32 columns
  int startX = (32 - totalW) / 2;

  mxClearBuf();
  drawSecondsBarRow7(ss);

  int x = startX;

  // ---- draw hour ----
  if (hT != 0) {
    mxDrawDigit(x, y, hT);
    x += DIG_W + GAP;          // 1 pixel gap between hour digits
  }
  mxDrawDigit(x, y, hU);
  x += DIG_W;

  // ---- gap + colon + gap (1 pixel each side) ----
  x += GAP;
  mxDrawColon(x, y, (ss % 2) == 0);
  x += COLON_W;
  x += GAP;

  // ---- draw minutes ----
  mxDrawDigit(x, y, mT);
  x += DIG_W + GAP;
  mxDrawDigit(x, y, mU);
  x += DIG_W;

  // ---- exactly 1 pixel gap before AM/PM ----
  x += GAP;

  // ---- draw AM/PM ----
  if (pm) mxDraw3x5(x, y, CH_P);
  else    mxDraw3x5(x, y, CH_A);
  x += DIG_W + GAP;
  mxDraw3x5(x, y, CH_M);

  mxFlush();
}


// ============================================================
// --------------------- NTP TIME HELPERS ---------------------
// ============================================================

static bool syncTimeNTP(uint32_t timeoutMs = 15000) {
  configTime(GMT_OFFSET_SEC, DAYLIGHT_OFFSET_SEC, NTP1, NTP2, NTP3);
  struct tm tinfo;
  uint32_t start = millis();
  while (millis() - start < timeoutMs) {
    if (getLocalTime(&tinfo, 200)) return true;
    delay(200);
  }
  return false;
}

// Initialize MAX7219 + connect WiFi + sync time
static void mxClockSetup() {
  mx.begin();
  mx.clear();

  // Brightness: 0 - 15 or 16 idk i forgot
  mx.control(MD_MAX72XX::INTENSITY, 0);
  mx.control(MD_MAX72XX::UPDATE, MD_MAX72XX::ON);

  WiFi.mode(WIFI_STA);
  WiFi.begin(WIFI_SSID, WIFI_PASS);

  // Give WiFi some time
  uint32_t t0 = millis();
  while (WiFi.status() != WL_CONNECTED && millis() - t0 < 12000) {
    delay(250);
  }

  // Even if WiFi fails, we still run clock using millis fallback
  syncTimeNTP();
}

// Draw clock continuously
static void mxClockLoop() {
  static uint32_t lastDrawMs = 0;

  // Update around ~15 fps is fine for blink + bar
  if (millis() - lastDrawMs < 60) return;
  lastDrawMs = millis();

  struct tm tinfo;
  bool ok = getLocalTime(&tinfo, 10);

  // If NTP time isn't ready yet: show 00:00 AM and seconds from millis()
  if (!ok) {
    int fake = (millis()/1000) % 60;
    mxDrawClock(0, 0, fake);
    return;
  }

  //mxDrawClock(18, 23, 45); // Use this to test the clock
  mxDrawClock(tinfo.tm_hour, tinfo.tm_min, tinfo.tm_sec);
}


// ============================================================
// ========================= SETUP ============================
// ============================================================

void setup(){
  // OLED I2C
  Wire.begin(21, 22);
  u8g2.begin();

  // MAX7219 + WiFi time
  mxClockSetup();

  // Random seed for OLED animation
  randomSeed(esp_random());
  resetToStart();
}


// ============================================================
// ========================== LOOP ============================
// ============================================================

void loop(){
  // 1) MAX7219 clock runs independently (never blocks OLED animation)
  mxClockLoop();

  // 2) OLED animation
  // ----------------------------------------------------------
  u8g2.clearBuffer();

  // Draw text in the OLED yellow zone (top band)
  drawTopTitle();

  unsigned long now = millis();
  unsigned long t = now - stateStart;

  bool showEyeFrame = (state <= RASENGAN);
  if(showEyeFrame) drawEyeBase();

  // Tomoe speed changes with dotCount
  if(state == DOTS_1 || state == DOTS_2 || state == DOTS_3){
    angSpeed = 0.05f + (dotCount-1)*0.004f;
  }

  if(state == DOTS_1){
    dotCount = 1;
    drawTomoeSet(1, ang);
    if(t >= T_STAGE_HOLD) setAnimState(UPSHIFT);
  }
  else if(state == DOTS_2){
    dotCount = 2;
    drawTomoeSet(2, ang);
    if(t >= T_STAGE_HOLD) setAnimState(UPSHIFT);
  }
  else if(state == DOTS_3){
    dotCount = 3;
    drawTomoeSet(3, ang);
    if(t >= T_STAGE_HOLD) setAnimState(ACCEL_TO_BREAK);
  }
  else if(state == UPSHIFT){
    float k = clamp01((float)t / (float)T_UPSHIFT);
    float base = 0.05f + (dotCount-1)*0.004f;
    angSpeed = base + k*k*0.25f;
    drawTomoeSet(dotCount, ang);
    if(k > 0.80f) u8g2.drawCircle(CX + random(-1,2), CY + random(-1,2), EYE_R);

    if(t >= T_UPSHIFT){
      if(dotCount == 1) setAnimState(DOTS_2);
      else setAnimState(DOTS_3);
    }
  }
  else if(state == ACCEL_TO_BREAK){
    float k = clamp01((float)t / (float)T_ACCEL_BREAK);
    angSpeed = 0.06f + k*k*0.60f;
    drawTomoeSet(3, ang);
    if(k > 0.85f) u8g2.drawCircle(CX + random(-1,2), CY + random(-1,2), EYE_R);

    if(t >= T_ACCEL_BREAK){
      spawnShards();
      setAnimState(BREAK_PARTICLES);
    }
  }
  else if(state == BREAK_PARTICLES){
    float k = clamp01((float)t / (float)T_BREAK);
    float swirl = 0.020f + k*0.055f;
    updateShards(swirl);
    if(k > 0.55f) drawRasengan(ang);
    if(t >= T_BREAK) setAnimState(RASENGAN);
  }
  else if(state == RASENGAN){
    updateShards(0.040f);
    drawRasengan(ang);
    if(t >= T_RAS) setAnimState(RAS_COMPRESS);
  }
  else if(state == RAS_COMPRESS){
    float k = clamp01((float)t / (float)T_COMPRESS);
    drawCompressBall(k);
    if(t >= T_COMPRESS) setAnimState(KONOHA_DRAW);
  }
  else if(state == KONOHA_DRAW){
    float k = clamp01((float)t / (float)T_KONOHA_DRAW);
    drawKonohaSymbol(k);
    if(t >= T_KONOHA_DRAW) setAnimState(KONOHA_HOLD);
  }
  else if(state == KONOHA_HOLD){
    drawKonohaSymbol(1.0f);
    if(t >= T_KONOHA_HOLD) setAnimState(KONOHA_IGNITE);
  }
  else if(state == KONOHA_IGNITE){
    float k = clamp01((float)t / (float)T_KONOHA_IGNITE);
    drawKonohaIgnite(k);
    if(t >= T_KONOHA_IGNITE) setAnimState(FLAME_RING);
  }
  else if(state == FLAME_RING){
    float k = clamp01((float)t / (float)T_FLAME_RING);
    drawFlameRing(ang, 0.0f);
    if(k > 0.15f){
      float c = clamp01((k - 0.15f) / 0.85f);
      drawFlameRing(ang*1.2f, c*0.92f);
    }
    if(t >= T_FLAME_RING) setAnimState(JOLLY_DRAW);
  }
  else if(state == JOLLY_DRAW){
    float k = clamp01((float)t / (float)T_JOLLY_DRAW);
    drawJolly(k);
    if(t >= T_JOLLY_DRAW) setAnimState(JOLLY_HOLD);
  }
  else if(state == JOLLY_HOLD){
    drawJolly(1.0f);
    if(t >= T_JOLLY_HOLD) setAnimState(JOLLY_DISSOLVE);
  }
  else if(state == JOLLY_DISSOLVE){
    float k = clamp01((float)t / (float)T_JOLLY_DISS);
    drawJollyDissolve(k);
    if(t >= T_JOLLY_DISS) setAnimState(OCEAN_FORM);
  }
  else if(state == OCEAN_FORM){
    float k = clamp01((float)t / (float)T_OCEAN_FORM);
    drawOcean(ang, k);
    if(t >= T_OCEAN_FORM) setAnimState(MERRY_SAIL_IN);
  }
  else if(state == MERRY_SAIL_IN){
    float k = clamp01((float)t / (float)T_MERRY_IN);
    drawOcean(ang, 1.0f);

    int xStart = 178;
    int xEnd   = 62;
    int shipX = xStart + (int)lroundf((xEnd - xStart) * smooth01(k));

    int bob = ix(sinf(ang*1.2f) * 1.5f);
    drawGoingMerryLong(shipX, 36 + bob);

    if(t >= T_MERRY_IN) setAnimState(MERRY_HOLD);
  }
  else if(state == MERRY_HOLD){
    drawOcean(ang, 1.0f);
    int bob = ix(sinf(ang*1.2f) * 1.5f);
    drawGoingMerryLong(62, 36 + bob);
    if(t >= T_MERRY_HOLD) setAnimState(ZORO_ZCUTS);
  }
  else if(state == ZORO_ZCUTS){
    float k = clamp01((float)t / (float)T_ZCUTS);
    drawOcean(ang*1.12f, 0.60f);
    drawZCutsGated(k, 0.0f, stateSeed);
    if(t >= T_ZCUTS) setAnimState(ZORO_HOLD);
  }
  else if(state == ZORO_HOLD){
    drawOcean(ang*1.10f, 0.60f);
    drawZCutsGated(1.0f, 0.0f, stateSeed);
    if(t >= T_ZHOLD) setAnimState(ZORO_AURA_CHARGE);
  }
  else if(state == ZORO_AURA_CHARGE){
    float k = clamp01((float)t / (float)T_ZAURA);

    float oceanInt = 0.60f * (1.0f - smooth01(k));
    drawOcean(ang*1.08f, oceanInt);

    drawZCutsGated(1.0f, 0.0f, stateSeed);
    drawZoroAura(k);

    if(t >= T_ZAURA) setAnimState(ZORO_ASURA_FLASH);
  }
  else if(state == ZORO_ASURA_FLASH){
    float k = clamp01((float)t / (float)T_ASURA);
    drawAsuraFlash(k);
    if(t >= T_ASURA) setAnimState(ZORO_REFORM_EYE);
  }
else if(state == ZORO_REFORM_EYE){
  float k = clamp01((float)t / (float)T_REFORM);

  float fadePhase  = clamp01((k - 0.75f) / 0.25f);

  float zFade = smooth01(fadePhase);
  drawZCutsGated(1.0f, zFade, stateSeed);

  if(fadePhase > 0.05f){
    float pull = 1.0f - fadePhase;
    int sparks = 55;
    for(int i=0;i<sparks;i++){
      uint32_t hv = h2(i, 77, stateSeed);
      float a = (float)((hv % 628u))/100.0f;
      float d = 10.0f + (float)(((hv >> 10) % 100u)) / 10.0f;
      int x = CX + ix(cosf(a)*d*pull);
      int y = CY + ix(sinf(a)*d*pull);
      safePixel(x,y);
    }
    drawReformEye(fadePhase);
  }

  if(t >= T_REFORM){
    resetToStart();
  }
}

  // Send OLED frame to screen
  u8g2.sendBuffer();

  // Advance rotation angle
  ang += angSpeed;
  if(ang > 2.0f*(float)M_PI) ang -= 2.0f*(float)M_PI;

  delay(30);
}