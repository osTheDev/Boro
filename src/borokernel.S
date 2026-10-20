	.set noreorder

#include "pspstub.s"

	STUB_START "BoroKernel",0x40090000,0x00010005
	STUB_FUNC  0x08E0008B,boroGetVolume
	STUB_END
