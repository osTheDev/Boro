	.set noreorder

#include "pspimport.s"

# The H.264 entry points of mpeg_vsh.prx, which the PSPSDK stubs leave out.
	IMPORT_START "sceMpeg",0x00090000
	IMPORT_FUNC  "sceMpeg",0x682A619B,sceMpegInit
	IMPORT_FUNC  "sceMpeg",0x874624D6,sceMpegFinish
	IMPORT_FUNC  "sceMpeg",0xC132E22F,sceMpegQueryMemSize
	IMPORT_FUNC  "sceMpeg",0xD8C5F121,sceMpegCreate
	IMPORT_FUNC  "sceMpeg",0x606A4649,sceMpegDelete
	IMPORT_FUNC  "sceMpeg",0x167AFD9E,sceMpegInitAu
	IMPORT_FUNC  "sceMpeg",0x0E3C2E9D,sceMpegAvcDecode
	IMPORT_FUNC  "sceMpeg",0xCF3547A2,sceMpegAvcDecodeDetail2
	IMPORT_FUNC  "sceMpeg",0x11F95CF1,sceMpegGetAvcNalAu

	IMPORT_START "sceMpegbase",0x00090000
	IMPORT_FUNC  "sceMpegbase",0x91929A21,sceMpegBaseCscAvc
