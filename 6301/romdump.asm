; ---------------------------------------------------------------------------
; romdump.asm - HD6301V1: transmit $F000..$FFFF over the SCI, then sleep.
;
; Target   HD6301V1, EXTAL 500 kHz -> E = 125 kHz (on-chip divide by four)
; Assemble tabasm --cpu 6303 -b -fFF -l romdump.asm romdump.bin romdump.lst
;
; The SCI baud generator only divides E by 16, 128, 1024 or 4096, so at
; E = 125 kHz the attainable rates are 7812.5, 976.5625, 122.07 and 30.52
; baud. 1200 is not among them; this uses 976.5625 baud, the closest.
;
; The program sits at $C000. $F000..$FFFD is left free for test bit patterns
; and is never written here; $FFFE..$FFFF holds the reset vector.
; ---------------------------------------------------------------------------

; ---- internal registers ---------------------------------------------------
RMCR    .equ  $10             ; rate and mode control register (write only)
TRCSR   .equ  $11             ; transmit/receive control and status register
TDR     .equ  $13             ; transmit data register (write only)

TDRE    .equ  $20             ; TRCSR bit 5, transmit data register empty
TE      .equ  $02             ; TRCSR bit 1, transmit enable

; CC1:CC0 = 01  internal clock, P22 left as a general purpose pin
; SS1:SS0 = 01  E / 128 -> 125000 / 128 = 976.5625 baud
RMCRV   .equ  $05

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
; $F000..$FFFD is reserved for the test bit patterns and is left untouched.
; .msfirst is required: tabasm emits .word little endian by default.
        .msfirst
        .org  $FFFE
        .word start
        .end
