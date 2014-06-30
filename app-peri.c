/*
 * NXCTRL BeagleBone Black Control Library
 *
 * Peripheral Driving App Program
 *
 * Copyright (C) 2014 Sungjin Chun <chunsj@gmail.com>
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 3 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 675 Mass Ave, Cambridge, MA 02139, USA.
 */

#include <unistd.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <signal.h>
#include <sys/time.h>
#include <time.h>
#include <errno.h>
#include <prussdrv.h>
#include <pruss_intc_mapping.h>
#include <NXCTRL_appEx.h>

#define FONT_WIDTH                  6
#define FONT_HEIGHT                 8
#define MENU_SEL_CHAR               ((unsigned char)16)

#define DPY_IDLE_COUNT_MAX          300
#define MIN_ACTION_DURATION         200

#define MENU_IDX_COUNT              6

#define MENU_IDX_NEXT_APP           0
#define MENU_IDX_SYSTEM_MENU        1
#define MENU_IDX_UPDATE_MENU        2
#define MENU_IDX_P8_13_PWM_MENU     3
#define MENU_IDX_P8_19_PWM_MENU     4
#define MENU_IDX_EXIT_MENU          5

#define NEXT_APP_IDX                6 // from tc.c

#define PRU_NUM                     PRU0
#define PRU_PATH                    "/usr/bin/ctrl-app.bin"

#define HCSR04_BANK                 NXCTRL_P8
#define HCSR04_MAX_CNT              1
#define HCSR04_MAX_DIST             100

#define TRIGGER_PIN                 NXCTRL_PIN11
#define ECHO_PIN                    NXCTRL_PIN15

#define PWM1_BANK                   NXCTRL_P8
#define PWM2_BANK                   NXCTRL_P8
#define PWM1_PIN                    NXCTRL_PIN13
#define PWM2_PIN                    NXCTRL_PIN19

static NXCTRL_BOOL                  MENU_BUTTON_STATE = NXCTRL_LOW;
static NXCTRL_BOOL                  EXEC_BUTTON_STATE = NXCTRL_LOW;
static unsigned int                 DPY_IDLE_COUNT = 0;
static unsigned char                MENU_IDX = MENU_IDX_SYSTEM_MENU;
static NXCTRL_BOOL                  IN_MENU = NXCTRL_FALSE;
static unsigned long long           LAST_ACTION_TIME = 0;

static float
getFetchDistance (NXCTRL_VOID) {
  int nRet;
  tpruss_intc_initdata nINTC = PRUSS_INTC_INITDATA;
  void *pPRUDataMem = NULL;
  unsigned int *pnPRUData = NULL;
  float fDist = 0.0f;

  // initialize PRU
  if ((nRet = prussdrv_init())) {
    fprintf(stderr, "prussdrv_init() failed\n");
    return 0;
  }

  // open the interrupt
  if ((nRet = prussdrv_open(PRU_EVTOUT_0))) {
    fprintf(stderr, "prussdrv_open() failed: %s\n", strerror(errno));
    return 0;
  }

  // initialize interrupt
  if ((nRet = prussdrv_pruintc_init(&nINTC))) {
    fprintf(stderr, "prussdrv_pruintc_init() failed\n");
    return 0;
  }

  prussdrv_map_prumem(PRUSS0_PRU0_DATARAM, &pPRUDataMem);
  pnPRUData = (unsigned int *)pPRUDataMem;

  // load and run the PRU program
  if ((nRet = prussdrv_exec_program(PRU_NUM, PRU_PATH))) {
    fprintf(stderr, "prussdrv_exec_program() failed\n");
    return 0;
  }

  prussdrv_pru_wait_event(PRU_EVTOUT_0);
  if (prussdrv_pru_clear_event(PRU_EVTOUT_0, PRU0_ARM_INTERRUPT))
    fprintf(stderr, "prussdrv_pru_clear_event() failed\n");
  fDist = (float)pnPRUData[0]/2.0/29.1;

  // halt and disable the PRU
  if (prussdrv_pru_disable(PRU_NUM))
    fprintf(stderr, "prussdrv_pru_disable() failed\n");

  // release the PRU clocks and disable prussdrv module
  if (prussdrv_exit())
    fprintf(stderr, "prussdrv_exit() failed\n");

  return fDist;
}

static NXCTRL_VOID
displayPeriInfo (LPNXCTRLAPP pApp) {
  register int i, n = HCSR04_MAX_CNT;
  float fs = 0;
  char rch[22];

  pApp->clearDisplay();
  pApp->setCursor(3*FONT_WIDTH, 0);
  pApp->writeSTR("PERIPHERAL DRV.\n");

  pApp->setCursor(0, FONT_HEIGHT + 8);

  for (i = 0; i < n; i++) {
    fs += getFetchDistance();
  }
  fs /= n;
  
  if (fs > HCSR04_MAX_DIST)
    sprintf(rch, "DIST(HCSR04): >%1.1fm\n", (HCSR04_MAX_DIST/100.0));
  else
    sprintf(rch, "DIST(HCSR04): %2.1fcm\n", fs);
  pApp->writeSTR(rch);

  sprintf(rch, "A0: %04d/4095\n", pApp->analogRead(NXCTRL_A0));
  pApp->writeSTR(rch);
  sprintf(rch, "A1: %04d/4095 (T)\n", pApp->analogRead(NXCTRL_A1));
  pApp->writeSTR(rch);
  sprintf(rch, "A2: %04d/4095\n", pApp->analogRead(NXCTRL_A2));
  pApp->writeSTR(rch);
  sprintf(rch, "A3: %04d/4095\n", pApp->analogRead(NXCTRL_A3));
  pApp->writeSTR(rch);

  pApp->updateDisplay();
}

static NXCTRL_VOID
runPWM1 (LPNXCTRLAPP pApp) {
  int i, j;
  int PWM_RES = 1000;
  int PULSE_CNT = 10;
  int PULSE_RES = 10;
  int PULSE_TM = 80;
  int nDelta = PWM_RES / PULSE_RES;
  
  pApp->clearDisplay();
  pApp->setCursor(0, 3*FONT_HEIGHT);
  pApp->writeSTR("    PWM ON P8:13");
  pApp->setCursor(0, 4*FONT_HEIGHT + 2);
  pApp->writeSTR("    PULSING LED");
  pApp->updateDisplay();

  for (j = 0; j < PULSE_CNT; j++) {
    for (i = 0; i < PULSE_RES; i++) {
      pApp->analogWrite(PWM1_BANK, PWM1_PIN, nDelta*(i+1));
      pApp->sleep(PULSE_TM, 0);
    }
    for (i = 0; i < PULSE_RES; i++) {
      pApp->analogWrite(PWM1_BANK, PWM1_PIN, PWM_RES - nDelta*(i+1));
      pApp->sleep(PULSE_TM, 0);
    }
  }
  pApp->analogWrite(PWM1_BANK, PWM1_PIN, 0);
  pApp->sleep(100, 0);
}

static NXCTRL_VOID
runPWM2 (LPNXCTRLAPP pApp) {
  int i;
  
  pApp->clearDisplay();
  pApp->setCursor(0, 3*FONT_HEIGHT);
  pApp->writeSTR("    PWM ON P8:19");
  pApp->setCursor(0, 4*FONT_HEIGHT + 2);
  pApp->writeSTR("    SERVO CONTRL");
  pApp->updateDisplay();

  pApp->servoWrite(PWM2_BANK, PWM2_PIN, 82);
  pApp->sleep(800, 0);
  pApp->servoWrite(PWM2_BANK, PWM2_PIN, 30);
  pApp->sleep(800, 0);
  pApp->servoWrite(PWM2_BANK, PWM2_PIN, 150);
  pApp->sleep(800, 0);
  
  pApp->servoWrite(PWM2_BANK, PWM2_PIN, 0);
  pApp->sleep(500, 0);

  for (i = 0; i <= 180; i += 2) {
    pApp->servoWrite(PWM2_BANK, PWM2_PIN, i);
    pApp->sleep(20, 0);
  }

  pApp->servoWrite(PWM2_BANK, PWM2_PIN, 0);
  pApp->sleep(800, 0);
  pApp->servoWrite(PWM2_BANK, PWM2_PIN, 180);
  pApp->sleep(800, 0);
  pApp->servoWrite(PWM2_BANK, PWM2_PIN, 82);
  pApp->sleep(800, 0);

  pApp->servoWrite(PWM2_BANK, PWM2_PIN, 0);
  pApp->sleep(500, 0);
}

static NXCTRL_BOOL
canAction (NXCTRL_VOID) {
  struct timespec tm;
  unsigned long long timeInMillis;
  extern int clock_gettime(int, struct timespec *);
  clock_gettime(_POSIX_CPUTIME, &tm);
  timeInMillis = tm.tv_sec * 1000 + tm.tv_nsec/1000000;
  if ((timeInMillis - LAST_ACTION_TIME) > MIN_ACTION_DURATION) {
    LAST_ACTION_TIME = timeInMillis;
    return NXCTRL_TRUE;
  } else
    return NXCTRL_FALSE;
}

static NXCTRL_VOID
updateMenuButtonState (LPNXCTRLAPP pApp) {
  if (MENU_BUTTON_STATE == NXCTRL_LOW) {
    if (pApp->digitalRead(MENU_BUTTON_BANK, MENU_BUTTON_PIN) == NXCTRL_HIGH) {
      MENU_BUTTON_STATE = NXCTRL_HIGH;
      DPY_IDLE_COUNT = 0;
    }
  } else {
    if (pApp->digitalRead(MENU_BUTTON_BANK, MENU_BUTTON_PIN) == NXCTRL_LOW) {
      MENU_BUTTON_STATE = NXCTRL_LOW;
      DPY_IDLE_COUNT = 0;
    }
  }
}

static NXCTRL_VOID
updateExecButtonState (LPNXCTRLAPP pApp) {
  if (EXEC_BUTTON_STATE == NXCTRL_LOW) {
    if (pApp->digitalRead(EXEC_BUTTON_BANK, EXEC_BUTTON_PIN) == NXCTRL_HIGH) {
      EXEC_BUTTON_STATE = NXCTRL_HIGH;
      DPY_IDLE_COUNT = 0;
    }
  } else {
    if (pApp->digitalRead(EXEC_BUTTON_BANK, EXEC_BUTTON_PIN) == NXCTRL_LOW) {
      EXEC_BUTTON_STATE = NXCTRL_LOW;
      DPY_IDLE_COUNT = 0;
    }
  }
}

static char *
mkMenuSTR (char *rch, const char *pszName, int nMenu) {
  sprintf(rch, "%c %s\n",
          (MENU_IDX == nMenu ? MENU_SEL_CHAR : ' '),
          pszName);
  return rch;
}

static NXCTRL_VOID
displayMenu (LPNXCTRLAPP pApp) {
  char rch[21];

  pApp->clearDisplay();

  pApp->setCursor(0, 0);
  pApp->writeSTR("PERI.DRV");
  pApp->drawLine(49, 6, 127, 6, NXCTRL_ON);
  pApp->setCursor(0, 16);

  if (MENU_IDX < 5)
    pApp->writeSTR(mkMenuSTR(rch, "SPARK CORE APP", MENU_IDX_NEXT_APP));
  pApp->writeSTR(mkMenuSTR(rch, "SYSTEM UTILS", MENU_IDX_SYSTEM_MENU));
  pApp->writeSTR(mkMenuSTR(rch, "UPDATE INFO", MENU_IDX_UPDATE_MENU));
  pApp->writeSTR(mkMenuSTR(rch, "P8:13 PWM(LED)", MENU_IDX_P8_13_PWM_MENU));
  pApp->writeSTR(mkMenuSTR(rch, "P8:19 PWM(SERVO)", MENU_IDX_P8_19_PWM_MENU));
  if (MENU_IDX >= 5)
    pApp->writeSTR(mkMenuSTR(rch, "EXIT MENU", MENU_IDX_EXIT_MENU));

  pApp->updateDisplay();
}

NXCTRL_VOID
NXCTRLAPP_init (LPNXCTRLAPP pApp) {
  pApp->pinMux(PWM1_BANK, PWM1_PIN, NXCTRL_MODE4, NXCTRL_PULLDN, NXCTRL_LOW);
  pApp->pinMux(PWM2_BANK, PWM2_PIN, NXCTRL_MODE4, NXCTRL_PULLDN, NXCTRL_LOW);
  
  MENU_BUTTON_STATE = pApp->digitalRead(MENU_BUTTON_BANK, MENU_BUTTON_PIN);
  EXEC_BUTTON_STATE = pApp->digitalRead(EXEC_BUTTON_BANK, EXEC_BUTTON_PIN);
  DPY_IDLE_COUNT = 0;
  MENU_IDX = MENU_IDX_NEXT_APP;
  IN_MENU = NXCTRL_FALSE;
  LAST_ACTION_TIME = 0;

  while (MENU_BUTTON_STATE == NXCTRL_HIGH) {
    pApp->sleep(100, 0);
    MENU_BUTTON_STATE = pApp->digitalRead(MENU_BUTTON_BANK, MENU_BUTTON_PIN);
  }

  displayPeriInfo(pApp);
}

NXCTRL_VOID
NXCTRLAPP_clean (LPNXCTRLAPP pApp) {
}

NXCTRL_VOID
NXCTRLAPP_run (LPNXCTRLAPP pApp) {
  updateMenuButtonState(pApp);
  updateExecButtonState(pApp);

  if (MENU_BUTTON_STATE != NXCTRL_HIGH && EXEC_BUTTON_STATE != NXCTRL_HIGH) {
    DPY_IDLE_COUNT++;
    if (DPY_IDLE_COUNT > DPY_IDLE_COUNT_MAX) {
      pApp->nCmd = 2;
      return;
    }
    return;
  }

  if (MENU_BUTTON_STATE == NXCTRL_ON) {
    if (IN_MENU) {
      if (canAction()) {
        MENU_IDX++;
        if (MENU_IDX >= MENU_IDX_COUNT)
          MENU_IDX = MENU_IDX_NEXT_APP;
        displayMenu(pApp);
      }
    } else {
      IN_MENU = NXCTRL_TRUE;
      displayMenu(pApp);
      canAction();
    }
  }

  if (EXEC_BUTTON_STATE == NXCTRL_ON) {
    if (IN_MENU) {
      if (canAction()) {
        switch (MENU_IDX) {
        case MENU_IDX_EXIT_MENU:
          IN_MENU = NXCTRL_FALSE;
          displayPeriInfo(pApp);
          break;
        case MENU_IDX_SYSTEM_MENU:
          pApp->nCmd = 1;
          return;
        case MENU_IDX_NEXT_APP:
          pApp->nCmd = NEXT_APP_IDX;
          return;
        case MENU_IDX_UPDATE_MENU:
          IN_MENU = NXCTRL_FALSE;
          displayPeriInfo(pApp);
          break;
        case MENU_IDX_P8_13_PWM_MENU:
          IN_MENU = NXCTRL_FALSE;
          runPWM1(pApp);
          displayPeriInfo(pApp);
          break;
        case MENU_IDX_P8_19_PWM_MENU:
          IN_MENU = NXCTRL_FALSE;
          runPWM2(pApp);
          displayPeriInfo(pApp);
          break;
        default:
          break;
        }
      }
    }
  }
}