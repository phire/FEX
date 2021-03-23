
%ifdef CONFIG
{
  "Match": "All",
  "RegData": {
    "MM7": ["0xfb53d1c000000000", "0x4002"]
  }
}
%endif
    call inner
    hlt

inner:
    ; calcuate pi + pi + pi + pi + pi
    fld dword [rel pi]
    fld dword [rel pi]
    fld dword [rel pi]
    fld dword [rel pi]
    fld dword [rel pi]
    faddp
    faddp
    faddp
    faddp

    ret

align 8
pi:     dd 0x40490fdb ; 3.14...
one:    dd 0x3f800000 ; 1.0
ptone:  dd 0x3dcccccd ; 0.1
