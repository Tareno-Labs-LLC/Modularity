@ gba_crt0.s - cartridge header, startup, and the IRQ handler.
@
@ This is all the code that runs before main(): set two stacks, copy initialised data from ROM
@ into RAM, zero .bss, run static constructors, call main. About 200 bytes.

    .syntax unified

@ --------------------------------------------------------------------------------------------
@ Cartridge header (0x08000000 - 0x080000BF)
@ --------------------------------------------------------------------------------------------
    .section .crt0, "ax", %progbits
    .arm
    .align 2
    .global _start
_start:
    b       _reset

    @ Nintendo logo, 156 bytes. The real BIOS refuses to boot without it; mGBA's built-in BIOS
    @ does not check. The exporter fills it for real hardware.
    .fill   156, 1, 0

    .ascii  "MODUHELLO"             @ 0xA0 title, 12 bytes
    .fill   3, 1, 0
    .ascii  "MODU"                  @ 0xAC game code
    .ascii  "00"                    @ 0xB0 maker code
    .byte   0x96                    @ 0xB2 fixed value
    .byte   0x00                    @ 0xB3 main unit
    .byte   0x00                    @ 0xB4 device type
    .fill   7, 1, 0                 @ 0xB5 reserved
    .byte   0x00                    @ 0xBC software version
    .byte   0x00                    @ 0xBD header checksum, patched by the build
    .fill   2, 1, 0                 @ 0xBE reserved

@ --------------------------------------------------------------------------------------------
@ Startup (0x080000C0)
@ --------------------------------------------------------------------------------------------
_reset:
    mov     r0, #0x92               @ IRQ mode, IRQs masked
    msr     cpsr_c, r0
    ldr     sp, =__sp_irq
    mov     r0, #0x1F               @ System mode, IRQs enabled
    msr     cpsr_c, r0
    ldr     sp, =__sp_usr

    @ Cartridge timing: 3/1 waitstates + prefetch (power-on is 4/2, no prefetch). This is the
    @ setting commercial games use, and every line of Thumb code in ROM runs faster for it.
    ldr     r0, =0x04000204         @ REG_WAITCNT
    ldr     r1, =0x4317
    strh    r1, [r0]

    ldr     r0, =__iwram_lma        @ .iwram: ARM hot code + initialised data
    ldr     r1, =__iwram_start
    ldr     r2, =__iwram_end
    bl      copy_words

    ldr     r0, =__ewram_lma
    ldr     r1, =__ewram_start
    ldr     r2, =__ewram_end
    bl      copy_words

    ldr     r0, =__bss_start
    ldr     r1, =__bss_end
    mov     r2, #0
1:  cmp     r0, r1
    strlo   r2, [r0], #4
    blo     1b

    ldr     r4, =__init_array_start
    ldr     r5, =__init_array_end
2:  cmp     r4, r5
    bhs     3f
    ldr     r0, [r4], #4
    mov     lr, pc
    bx      r0
    b       2b

3:  ldr     r0, =main
    mov     lr, pc
    bx      r0
    b       .                       @ main never returns on a console

@ r0 = source, r1 = destination, r2 = destination end (all word aligned)
copy_words:
    cmp     r1, r2
    ldrlo   r3, [r0], #4
    strlo   r3, [r1], #4
    blo     copy_words
    bx      lr

    .ltorg

@ --------------------------------------------------------------------------------------------
@ IRQ handler. The BIOS saves r0-r3, r12 and lr, then enters here in ARM state; return with
@ bx lr. Lives in IWRAM because the cartridge bus is 16-bit and slow for ARM code.
@ --------------------------------------------------------------------------------------------
    .section .iwram, "ax", %progbits
    .arm
    .align 2
    .global modu_irq_handler
modu_irq_handler:
    mov     r2, #0x04000000
    add     r2, r2, #0x200          @ r2 = &REG_IE; the word at r2 is IE | IF << 16
    ldr     r1, [r2]
    and     r0, r1, r1, lsr #16     @ r0 = IE & IF: the interrupts that fired
    strh    r0, [r2, #2]            @ acknowledge them in REG_IF
    mov     r2, #0x04000000         @ 0x03FFFFF8 mirrors 0x03007FF8, the BIOS IntrWait flags
    ldrh    r1, [r2, #-8]
    orr     r1, r1, r0
    strh    r1, [r2, #-8]
    bx      lr
