    .section .rodata.launcher_font,"a",%progbits
    .balign 4
    .global launcher_font
launcher_font:
    .incbin "assets/launcher-font.bin"
    .incbin "assets/NotoSansSC-OFL.txt"
    .byte 0
