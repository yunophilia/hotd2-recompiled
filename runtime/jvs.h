/* NAOMI Maple bus + JVS I/O board, high-level emulation.
 *
 * The game talks to the MIE (Maple-JVS bridge) on Maple port A with command
 * 0x86. The MIE forwards JVS packets to one I/O board configured for HOTD2:
 * 2 players, 2 coin slots, 2 light guns.
 */
#pragma once
#include "sh4ctx.h"

/* JVS switch bits, player byte 1 << 8 | byte 2 */
#define JVS_START   0x8000
#define JVS_SERVICE 0x4000
#define JVS_TRIGGER 0x0200  /* push button 1 */
#define JVS_RELOAD  0x0100  /* push button 2: aiming off-screen also reloads */

typedef struct JvsInput {
	u8 test;              /* system byte bit 7 */
	u16 buttons[2];
	u16 gun_x[2], gun_y[2];
	u8 offscreen[2];      /* gun pointed away from the screen */
	u16 coins[2];
} JvsInput;

extern JvsInput jvs_input;
extern u8 naomi_eeprom[128];

/* Process a Maple DMA command list at guest address `list`. */
void maple_dma(u32 list);
