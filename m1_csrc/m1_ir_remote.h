/* See COPYING.txt for license details. */

/*
 * m1_ir_remote.h
 *
 * IR Remote — multi-button infrared remote control (load + create on-device).
 *
 * The five hardware buttons (UP / DOWN / LEFT / RIGHT / OK) each map to one IR
 * signal. Unlike the Sub-GHz Remote (a .rem manifest that points at external
 * .sub files), an IR Remote is entirely self-contained: it is a standard .ir
 * file whose five signals are named "up", "down", "left", "right" and "ok".
 * That keeps it Flipper-compatible and immune to a referenced file going away.
 *
 * M1 Project
 */

#ifndef M1_IR_REMOTE_H_
#define M1_IR_REMOTE_H_

/* Entry point (blocking). Offers Load existing / Create new, then a button pad
 * that fires the mapped signal on each press. Runs until BACK. */
void ir_remote(void);

#endif /* M1_IR_REMOTE_H_ */
