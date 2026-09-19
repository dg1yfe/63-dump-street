; ---------------------------------------------------------------------------
; romdump.asm - HD6301V1: transmit $F000..$FFFF over the SCI, then sleep.
;
; Target   HD6301V1, EXTAL 1 MHz -> E = 250 kHz (on-chip divide by four)
; Assemble tabasm --cpu 6303 -b -fFF -l romdump.asm romdump.bin romdump.lst
;
; The SCI baud generator divides E by 16, 128, 1024 or 4096. This uses the
; fastest of the four, E/16, giving 250000 / 16 = 15625 baud exactly, so a
; 4096 byte dump takes 4096 * 10 / 15625 = 2.6 s.
;
; The program sits at $C000 in emulated external memory. $F000..$FFFF is the
; internal mask ROM this dumps; the only part of it written here is the reset
; vector at $FFFE, which in mode 0 is fetched externally for 3 or 4 cycles
; after RES rises and reverts to internal ROM after that.
; ---------------------------------------------------------------------------

; ---- internal registers ---------------------------------------------------
RMCR    .equ  $10             ; rate and mode control register (write only)
TRCSR   .equ  $11             ; transmit/receive control and status register
TDR     .equ  $13             ; transmit data register (write only)

TDRE    .equ  $20             ; TRCSR bit 5, transmit data register empty
TE      .equ  $02             ; TRCSR bit 1, transmit enable

; CC1:CC0 = 01  internal clock, P22 left as a general purpose pin
; SS1:SS0 = 00  E / 16 -> 250000 / 16 = 15625 baud
RMCRV   .equ  $04

STACK   .equ  $00FF           ; top of internal RAM ($0080..$00FF)
DUMPBEG .equ  $F000           ; first byte sent; the last one is $FFFF

        .org  $C000
start
        sei                   ; no interrupts are used anywhere
        lds   #STACK
        ldaa  #RMCRV
        staa  RMCR
        ldaa  #TE             ; transmitter on, receiver and interrupts off
        staa  TRCSR           ; setting TE sends a 10 bit preamble of ones
        ldx   #DUMPBEG

txbyte
        ldab  0,x             ; hold the byte in B, A is needed for status
txwait
        ldaa  TRCSR           ; read TRCSR with TDRE set ...
        bita  #TDRE
        beq   txwait
        stab  TDR             ; ... then write TDR, which clears TDRE
        inx                   ; INX touches Z only
        bne   txbyte          ; $FFFF + 1 = $0000, so this ends after $FFFF

halt
        slp                   ; peripherals run on, the last byte completes
        bra   halt            ; a masked interrupt would resume here

; ---- reset vector ---------------------------------------------------------
; $F000..$FFFD belongs to the internal mask ROM and is not written here.
; .msfirst is required: tabasm emits .word little endian by default.
        .msfirst
        .org  $FFFE
        .word start
        .end
