#include "../buddy.h"
#include <string.h>



// ─── SLEEP ───  ~12s cycle, 6 poses
static void doSleep(uint32_t t) {
  static const char* const PUDDLE[5]  = { "            ", "            ", "   .----.   ", "  ( -- -- ) ", "  `~------~`" };
  static const char* const BREATH[5]  = { "            ", "   .----.   ", "  ( -- -- ) ", "  (        )", "   `------` " };
  static const char* const DEEP[5]    = { "            ", "  .------.  ", " ( -- -- ) ", " (         )", "  `~------~`" };
  static const char* const DRIP[5]    = { "            ", "            ", "   .----.   ", "  ( -- -- ) ", "  `--.----` " };
  static const char* const MELT[5]    = { "            ", "            ", "   .----.   ", "  ( __ __ ) ", " `~~~~~~~~~`" };
  static const char* const SNORE[5]   = { "            ", "  .------.  ", " ( __ __ )  ", " (    o    )", "  `~------~`" };

  const char* const* P[6] = { PUDDLE, BREATH, DEEP, DRIP, MELT, SNORE };
  static const uint8_t SEQ[] = {
    0,1,2,1,0,1,2,1,
    0,4,4,0,
    1,2,5,2,1,
    3,3,0,0,
    1,2,1,0
  };
  uint8_t beat = (t / 5) % sizeof(SEQ);
  buddy_print_sprite(P[SEQ[beat]], 5, 0, 0x07F0, 0);

  // Z particles drift up-right + a slow drip falling
  int p1 = (t)     % 10;
  int p2 = (t + 4) % 10;
  int p3 = (t + 7) % 10;
  buddy_set_color(BUDDY_DIM);
  buddy_set_cursor(BUDDY_X_CENTER + 20 + p1, BUDDY_Y_OVERLAY + 18 - p1 * 2);
  buddy_print("z");
  buddy_set_color(BUDDY_WHITE);
  buddy_set_cursor(BUDDY_X_CENTER + 26 + p2, BUDDY_Y_OVERLAY + 14 - p2);
  buddy_print("Z");
  buddy_set_color(BUDDY_DIM);
  buddy_set_cursor(BUDDY_X_CENTER + 16 + p3 / 2, BUDDY_Y_OVERLAY + 10 - p3 / 2);
  buddy_print("z");

  // slow droplet falling under the puddle
  int dphase = (t / 2) % 12;
  buddy_set_color(0x07F0);
  buddy_set_cursor(BUDDY_X_CENTER - 6, BUDDY_Y_BASE + 26 + dphase);
  buddy_print(dphase < 8 ? "." : " ");
}

// ─── IDLE ───  ~14s cycle, 10 poses
static void doIdle(uint32_t t) {
  static const char* const SMALL[5]   = { "            ", "    .--.    ", "   (o  o)   ", "   (    )   ", "    `--`    " };
  static const char* const MED[5]     = { "            ", "   .----.   ", "  ( o  o )  ", "  (      )  ", "   `----`   " };
  static const char* const BIG[5]     = { "            ", "  .------.  ", " ( o    o ) ", " (        ) ", "  `------`  " };
  static const char* const LOOK_L[5]  = { "            ", "   .----.   ", "  (o   o )  ", "  (      )  ", "   `----`   " };
  static const char* const LOOK_R[5]  = { "            ", "   .----.   ", "  ( o   o)  ", "  (      )  ", "   `----`   " };
  static const char* const BLINK[5]   = { "            ", "   .----.   ", "  ( -  - )  ", "  (      )  ", "   `----`   " };
  static const char* const WIGGLE_L[5]= { "            ", "  .----.    ", " ( o  o )   ", " (      )   ", "  `----`    " };
  static const char* const WIGGLE_R[5]= { "            ", "    .----.  ", "   ( o  o ) ", "   (      ) ", "    `----`  " };
  static const char* const JIGGLE[5]  = { "            ", "  .~~~~~~.  ", " ( o    o ) ", " (        ) ", "  `~~~~~~`  " };
  static const char* const DRIP_S[5]  = { "            ", "   .----.   ", "  ( o  o )  ", "  (      )  ", "  `--.--.`  " };

  const char* const* P[10] = { SMALL, MED, BIG, LOOK_L, LOOK_R, BLINK, WIGGLE_L, WIGGLE_R, JIGGLE, DRIP_S };
  static const uint8_t SEQ[] = {
    1,2,1,0,1,2, 1,3,1,4,1,5,
    2,2,8,8,2,
    6,7,6,7, 1,1,
    2,9,9,1,
    1,3,4,3,4,1,5,1,
    0,0,2,2
  };
  uint8_t beat = (t / 5) % sizeof(SEQ);
  buddy_print_sprite(P[SEQ[beat]], 5, 0, 0x07F0, 0);
}

// ─── BUSY ───  ~10s cycle, 6 poses + dot ticker
static void doBusy(uint32_t t) {
  static const char* const FOCUS_A[5] = { "            ", "   .----.   ", "  ( v  v )  ", "  (   --  ) ", "   `----`   " };
  static const char* const FOCUS_B[5] = { "            ", "   .----.   ", "  ( v  v )  ", "  (   __  ) ", "   `----`   " };
  static const char* const CHURN[5]   = { "            ", "  .~----~.  ", " ( v    v ) ", " (   oo   )", "  `~----~`  " };
  static const char* const THINK[5]   = { "      ?     ", "   .----.   ", "  ( ^  ^ )  ", "  (   ..  ) ", "   `----`   " };
  static const char* const PROCESS[5] = { "      *     ", "  .------.  ", " ( O    O ) ", " (   ==   )", "  `------`  " };
  static const char* const DRIP_W[5]  = { "            ", "   .----.   ", "  ( v  v )  ", "  (   --  ) ", "  `--.----` " };

  const char* const* P[6] = { FOCUS_A, FOCUS_B, CHURN, THINK, PROCESS, DRIP_W };
  static const uint8_t SEQ[] = {
    0,1,0,1,0,1, 2,2, 0,1,0,1, 3,3, 4,4, 0,1,5,5,2
  };
  uint8_t beat = (t / 5) % sizeof(SEQ);
  buddy_print_sprite(P[SEQ[beat]], 5, 0, 0x07F0, 0);

  static const char* const DOTS[] = { ".  ", ".. ", "...", " ..", "  .", "   " };
  buddy_set_color(BUDDY_WHITE);
  buddy_set_cursor(BUDDY_X_CENTER + 22, BUDDY_Y_OVERLAY + 14);
  buddy_print(DOTS[t % 6]);

  // tiny bubble rising inside the slime
  int b = (t / 2) % 8;
  buddy_set_color(BUDDY_CYAN);
  buddy_set_cursor(BUDDY_X_CENTER - 2, BUDDY_Y_OVERLAY + 18 - b);
  buddy_print(b < 6 ? "o" : " ");
}

// ─── ATTENTION ───  ~8s cycle, 6 poses + ! pulse
static void doAttention(uint32_t t) {
  static const char* const TALL[5]    = { "    .--.    ", "   (    )   ", "  ( O  O )  ", "  (   !   ) ", "  `------`  " };
  static const char* const PEEK_L[5]  = { "    .--.    ", "   (    )   ", " ( O  O  )  ", " (   !    ) ", " `------`   " };
  static const char* const PEEK_R[5]  = { "    .--.    ", "   (    )   ", "  ( O  O )  ", "   (   !  ) ", "   `------` " };
  static const char* const STRETCH[5] = { "     ||     ", "    /  \\    ", "  ( O  O )  ", "  (   !   ) ", "  `------`  " };
  static const char* const TENSE[5]   = { "    .--.    ", "  /(    )\\  ", " /( O  O )\\ ", " (   !!   ) ", " /`------`\\ " };
  static const char* const SHRINK[5]  = { "            ", "    .--.    ", "   (O  O)   ", "   (  !  )  ", "    `--`    " };

  const char* const* P[7] = { TALL, PEEK_L, TALL, PEEK_R, STRETCH, TENSE, SHRINK };
  static const uint8_t SEQ[] = {
    0,4,0,1,0,2,0,3, 4,4,0,1,2,0, 5,3, 0,0,6,0
  };
  uint8_t beat = (t / 5) % sizeof(SEQ);
  uint8_t pose = SEQ[beat];
  int xOff = (pose == 4) ? ((t & 1) ? 1 : -1) : 0;
  buddy_print_sprite(P[pose], 5, 0, 0x07F0, xOff);

  if ((t / 2) & 1) {
    buddy_set_color(BUDDY_YEL);
    buddy_set_cursor(BUDDY_X_CENTER - 8, BUDDY_Y_OVERLAY - 4);
    buddy_print("!");
  }
  if ((t / 3) & 1) {
    buddy_set_color(BUDDY_RED);
    buddy_set_cursor(BUDDY_X_CENTER + 8, BUDDY_Y_OVERLAY);
    buddy_print("!");
  }
  if ((t / 4) & 1) {
    buddy_set_color(BUDDY_YEL);
    buddy_set_cursor(BUDDY_X_CENTER, BUDDY_Y_OVERLAY - 8);
    buddy_print("!");
  }
}

// ─── CELEBRATE ───  ~5.6s cycle, 6 poses + droplet/confetti rain
static void doCelebrate(uint32_t t) {
  static const char* const SQUASH[5]  = { "            ", "            ", "  .--------.", " ( ^      ^)", " `~~------~`" };
  static const char* const LAUNCH[5]  = { "            ", "   .----.   ", "  ( ^  ^ )  ", " /(  ww  )\\ ", "  `------`  " };
  static const char* const AIRBORNE[5]= { "    .--.    ", "   ( ^^ )   ", "   (  WW)   ", "    `--`    ", "    : :     " };
  static const char* const SPLAT_L[5] = { "            ", "            ", " .---------.", "( ^      ^ )", " `~~~------`" };
  static const char* const SPLAT_R[5] = { "            ", "            ", ".---------. ", "( ^      ^ )", "`------~~~` " };
  static const char* const POSE[5]    = { "    \\__/    ", "   .----.   ", "  ( *  * )  ", " /(  WW  )\\ ", "  `------`  " };

  const char* const* P[6] = { SQUASH, LAUNCH, AIRBORNE, SPLAT_L, SPLAT_R, POSE };
  static const uint8_t SEQ[] = { 0,1,2,1,0, 3,4,3,4, 0,1,2,1,0, 5,5 };
  static const int8_t Y_SHIFT[] = { 0,-2,-7,-2,0, 0,0,0,0, 0,-2,-7,-2,0, 0,0 };
  uint8_t beat = (t / 3) % sizeof(SEQ);
  buddy_print_sprite(P[SEQ[beat]], 5, Y_SHIFT[beat], 0x07F0, 0);

  static const uint16_t cols[] = { BUDDY_YEL, BUDDY_HEART, BUDDY_CYAN, 0x07F0, BUDDY_GREEN };
  for (int i = 0; i < 6; i++) {
    int phase = (t * 2 + i * 11) % 22;
    int x = BUDDY_X_CENTER - 36 + i * 14;
    int y = BUDDY_Y_OVERLAY - 6 + phase;
    if (y > BUDDY_Y_BASE + 20 || y < 0) continue;
    buddy_set_color(cols[i % 5]);
    buddy_set_cursor(x, y);
    // alternating splat-droplets and stars
    const char* glyph = (i + (int)(t/2)) & 1 ? "*" : "o";
    buddy_print(glyph);
  }
}

// ─── DIZZY ───  ~5.6s cycle, 5 poses + orbiting stars
static void doDizzy(uint32_t t) {
  static const char* const LEAN_L[5]  = { "            ", "  .----.    ", " ( @  @ )   ", " (  ~~  )   ", "  `----`    " };
  static const char* const LEAN_R[5]  = { "            ", "    .----.  ", "   ( @  @ ) ", "   (  ~~  ) ", "    `----`  " };
  static const char* const WOBBLE[5]  = { "            ", "  .~----~.  ", " ( x    @ ) ", " (   vv   )", "  `~----~`  " };
  static const char* const WOBBLE2[5] = { "            ", "  .~----~.  ", " ( @    x ) ", " (   vv   )", "  `~----~`  " };
  static const char* const SPLAT[5]   = { "            ", "            ", " .---------.", "( @      @ )", " `--._.--._`" };

  const char* const* P[5] = { LEAN_L, LEAN_R, WOBBLE, WOBBLE2, SPLAT };
  static const uint8_t SEQ[] = { 0,1,0,1, 2,3, 0,1,0,1, 4,4, 2,3 };
  static const int8_t X_SHIFT[] = { -3,3,-3,3, 0,0, -3,3,-3,3, 0,0, 0,0 };
  uint8_t beat = (t / 4) % sizeof(SEQ);
  buddy_print_sprite(P[SEQ[beat]], 5, 0, 0x07F0, X_SHIFT[beat]);

  static const int8_t OX[] = { 0, 5, 7, 5, 0, -5, -7, -5 };
  static const int8_t OY[] = { -5, -3, 0, 3, 5, 3, 0, -3 };
  uint8_t p1 = t % 8;
  uint8_t p2 = (t + 4) % 8;
  uint8_t p3 = (t + 2) % 8;
  buddy_set_color(BUDDY_CYAN);
  buddy_set_cursor(BUDDY_X_CENTER + OX[p1] - 2, BUDDY_Y_OVERLAY + 6 + OY[p1]);
  buddy_print("*");
  buddy_set_color(BUDDY_YEL);
  buddy_set_cursor(BUDDY_X_CENTER + OX[p2] - 2, BUDDY_Y_OVERLAY + 6 + OY[p2]);
  buddy_print("*");
  buddy_set_color(BUDDY_WHITE);
  buddy_set_cursor(BUDDY_X_CENTER + OX[p3] - 2, BUDDY_Y_OVERLAY + 6 + OY[p3]);
  buddy_print("o");
}

// ─── HEART ───  ~10s cycle, 5 poses + rising heart stream
static void doHeart(uint32_t t) {
  static const char* const DREAMY[5]  = { "            ", "   .----.   ", "  ( ^  ^ )  ", "  (   ww  ) ", "   `----`   " };
  static const char* const BLUSH[5]   = { "            ", "   .----.   ", "  (#^  ^#)  ", "  (   ww  ) ", "   `----`   " };
  static const char* const HEART_E[5] = { "            ", "  .------.  ", " ( <3  <3 ) ", " (    v   ) ", "  `------`  " };
  static const char* const MELT_H[5]  = { "            ", "  .~~~~~~.  ", " ( @    @ ) ", " (   ww   )", "  `~------`" };
  static const char* const SIGH[5]    = { "            ", "   .----.   ", "  ( -  - )  ", "  (   ^^  ) ", "   `----`   " };

  const char* const* P[5] = { DREAMY, BLUSH, HEART_E, MELT_H, SIGH };
  static const uint8_t SEQ[] = {
    0,0,1,0, 2,2,0, 1,0,4, 0,0,3,3, 0,1,0,2, 1,0
  };
  static const int8_t Y_BOB[] = { 0,-1,0,-1, 0,-1,0, -1,0,0, -1,0,0,0, -1,0,-1,0, -1,0 };
  uint8_t beat = (t / 5) % sizeof(SEQ);
  buddy_print_sprite(P[SEQ[beat]], 5, Y_BOB[beat], 0x07F0, 0);

  buddy_set_color(BUDDY_HEART);
  for (int i = 0; i < 5; i++) {
    int phase = (t + i * 4) % 16;
    int y = BUDDY_Y_OVERLAY + 16 - phase;
    if (y < -2 || y > BUDDY_Y_BASE) continue;
    int x = BUDDY_X_CENTER - 20 + i * 8 + ((phase / 3) & 1) * 2 - 1;
    buddy_set_cursor(x, y);
    buddy_print("v");
  }
}


const species_t BLOB_SPECIES = {
  "blob",
  0x07F0,
  { doSleep, doIdle, doBusy, doAttention,
    doCelebrate, doDizzy, doHeart }
};
