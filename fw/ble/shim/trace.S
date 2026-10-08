/* BLE experiment, diagnostics (pi32v2 asm).
 *
 * shim_tramp_event / shim_tramp_rx: the vectors for IRQ 45 (BLE_EVENT) and 29 (BLE_RX) point here;
 * each entry is noted in shim_isr_trace (ble.c): the count at +0, JieLi's handlers at +4 (45) and
 * +8 (29), then a ring of 16 {reti, sp} pairs at +32 (sp as it is after our pushes: the entry sp is
 * 32 higher). Everything touched is saved and restored (psr first, before any arithmetic), then
 * control goes to JieLi's handler through a slot popped into pc, with the register state the
 * interrupt left, as if the vector had pointed at it directly. The handlers are internal to the
 * library, so their addresses come from request_irq at run time.
 *
 * isr_soft0: a Felucca-style wrapper for the soft interrupt 0 probe (fm1_soft0_irq, ble.c). */
	.macro BT_TRAMP name, off
	.section .text.\name,"ax",@progbits
	.globl \name
\name:
	[--sp] = {rets}
	[--sp] = {psr, rets, reti}
	[--sp] = {r3-r0}
	r2 = shim_isr_trace
	r3 = [r2+0]
	r3 += 1
	[r2+0] = r3
	r3 = r3 & 15
	r3 = r3 << 3
	r3 += r2
	r0 = reti
	[r3+32] = r0
	r1 = sp
	[r3+36] = r1
	r0 = [r2+\off]
	[sp+28] = r0
	{r3-r0} = [sp++]
	{psr, rets, reti} = [sp++]
	pc = [sp++]
	.endm

	BT_TRAMP shim_tramp_event, 4
	BT_TRAMP shim_tramp_rx, 8

	.section .text.isr_soft0,"ax",@progbits
	.globl isr_soft0
isr_soft0:
	[--sp] = {psr, rets, reti}
	[--sp] = {r3-r0}
	call fm1_soft0_irq
	{r3-r0} = [sp++]
	{psr, rets, reti} = [sp++]
	csync
	rti
