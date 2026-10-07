/* Cooperative context switch for the BT shim (pi32v2). A C call may clobber r0-r3; r4-r15 and
 * rets are the callee's to keep, so a switch saves exactly those on the old stack.
 *
 *   void shim_ctx_switch(uint32_t *save_sp, uint32_t new_sp)
 *
 * A new task's stack holds 13 copies of shim_task_entry: popped as {pc, r15-r4}, whatever order
 * the core uses, the pc lands on shim_task_entry (which reads its task from shim_cur). */
	.section .text.shim_ctx_switch,"ax",@progbits
	.globl shim_ctx_switch
shim_ctx_switch:
	[--sp] = {rets, r15-r4}
	r2 = sp
	[r0] = r2
	sp = r1
	{pc, r15-r4} = [sp++]
