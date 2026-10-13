.syntax unified
.thumb
.section .text.Reset_Handler,"ax",%progbits
.global Reset_Handler
.type Reset_Handler,%function
Reset_Handler:
    cpsid i
    ldr sp, =_estack
    bl giga_start
1:  b 1b
.size Reset_Handler,.-Reset_Handler

.section .text.giga_jump,"ax",%progbits
.global giga_jump
.type giga_jump,%function
giga_jump:
    cpsid i
    movs r2, #0
    msr control, r2
    msr basepri, r2
    msr faultmask, r2
    msr msp, r1
    dsb
    isb
    bx r0
.size giga_jump,.-giga_jump

#if defined(GIGA_APPLICATION) && defined(CORE_CM4)
.section .fast.personality,"ax",%progbits
.global __giga_fast_personality
.type __giga_fast_personality,%function
__giga_fast_personality:
    ldr r12, 1f
    bx r12
    .balign 4
1:  .word __gxx_personality_v0
.size __giga_fast_personality,.-__giga_fast_personality
#endif
