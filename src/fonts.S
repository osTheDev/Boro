# Sora font files (SIL OFL 1.1, see res/fonts/OFL.txt) embedded in the EBOOT.
	.section .rodata
	.balign 4
	.global sora400_ttf, sora400_ttf_end
sora400_ttf:
	.incbin "res/fonts/Sora-400.ttf"
sora400_ttf_end:
	.balign 4
	.global sora700_ttf, sora700_ttf_end
sora700_ttf:
	.incbin "res/fonts/Sora-700.ttf"
sora700_ttf_end:
	.balign 4
	.global sora800_ttf, sora800_ttf_end
sora800_ttf:
	.incbin "res/fonts/Sora-800.ttf"
sora800_ttf_end:
