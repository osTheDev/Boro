	.set noreorder

#include "pspstub.s"

	STUB_START "BoroKernel",0x40090000,0x00020005
	STUB_FUNC  0x08E0008B,boroGetVolume
	STUB_FUNC  0x7ABB462B,boroMeBoot
	STUB_END
