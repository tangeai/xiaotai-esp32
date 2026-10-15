.section .rodata
.global _binary_audio_opuspkt_start
.global _binary_audio_opuspkt_end
_binary_audio_opuspkt_start:
.rept 50
.short 3
.byte 0xf8,0xff,0xfe
.endr
_binary_audio_opuspkt_end:
.section .note.GNU-stack,"",@progbits
