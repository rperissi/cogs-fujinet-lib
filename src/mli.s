;
; mli.s - one ProDOS 8 MLI call from C.
;
; Copyright (C) 2026 Rob Perissi
;
; This program is free software: you can redistribute it and/or modify
; it under the terms of the GNU General Public License as published by
; the Free Software Foundation, either version 3 of the License, or
; (at your option) any later version. See LICENSE.
;
;   uint8_t __fastcall__ fn_mli(uint8_t call, void *params);
;
; Returns the MLI error code, 0 on success. Main RAM on purpose: the MLI
; switches the language card itself and puts it back the way it found it.

        .export         _fn_mli
        .import         popa

        .code

.proc   _fn_mli
        sta     param
        stx     param+1
        jsr     popa
        sta     call
        jsr     $BF00
call:   .byte   0
param:  .addr   0
        ldx     #0
        rts
.endproc
