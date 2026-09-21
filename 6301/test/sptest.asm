; ---------------------------------------------------------------------------
; sptest.asm - does the stack pointer survive a reset, and does NMI fire?
;
; Two entry points, selected with the rig's "g" command.
;
;   g c000   plant a known stack pointer, then stop
;   g c010   touch nothing at all, then stop
;
; Run the first, then reset into the second and fire NMI. The second never
; writes S, so whatever the interrupt frame is pushed on top of is whatever
; survived the reset. The emulator traces the address of every bus cycle,
; writes included, so the push is visible even though its data is discarded.
; ---------------------------------------------------------------------------

SPMAGIC .equ  $B0B0           ; distinctive, external in every mode, and its
                              ; two bytes are equal

        .org  $C000
setsp
        lds   #SPMAGIC
hold1
        bra   hold1

        .org  $C010
nosp
        bra   nosp

        .end
