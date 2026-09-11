.syntax unified
.cpu cortex-m33
.thumb
.section .isr_vector,"a",%progbits
.balign 512
.global vectors
vectors:
 .word 0x20030000
 .word Reset_Handler
 .rept 13
 .word Fault_Handler
 .endr
 .word SysTick_Handler
 .word Fault_Handler
 .word DMA0_IRQHandler
 .rept 15
 .word Fault_Handler
 .endr
 .word FLEXCOMM3_IRQHandler
 .rept 3
 .word Fault_Handler
 .endr
 .word FLEXCOMM7_IRQHandler
 .rept 6
 .word Fault_Handler
 .endr
 .word USB0_IRQHandler
 .rept 31
 .word Fault_Handler
 .endr
.section .text.Reset_Handler,"ax",%progbits
.global Reset_Handler
.type Reset_Handler,%function
.thumb_func
Reset_Handler:
 cpsid i
 ldr r0, =0xe000ed08
 ldr r1, =vectors
 str r1, [r0]
 dsb
 isb
 ldr r0, =0xe000e010
 movs r1, #0
 str r1, [r0]
 msr control, r1
 msr basepri, r1
 msr msplim, r1
 isb
 ldr r0, =0x4000c008
 movs r1, #0xaa
 strb r1, [r0]
 movs r1, #0x55
 strb r1, [r0]
 ldr r0, =__data_load
 ldr r1, =__data_start
 ldr r2, =__data_end
1:
 cmp r1, r2
 bhs 2f
 ldr r3, [r0], #4
 str r3, [r1], #4
 b 1b
2:
 ldr r1, =__bss_start
 ldr r2, =__bss_end
 movs r3, #0
3:
 cmp r1, r2
 bhs 4f
 str r3, [r1], #4
 b 3b
4:
 bl main
 b Fault_Handler
.size Reset_Handler, .-Reset_Handler
