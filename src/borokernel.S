	.set noreorder

#include "pspstub.s"

	STUB_START "BoroKernel",0x40090000,0x00040005
	STUB_FUNC  0x08E0008B,boroGetVolume
	STUB_FUNC  0x7ABB462B,boroMeBoot
	STUB_FUNC  0x795B0270,boroGetBrightness
	STUB_FUNC  0x57287D0A,boroSetBrightness
	STUB_END
