#include "debug_sis.h"

void SIS_Init() {
	debugLogEnabled["special"]  = false;
	debugLogEnabled["fileread"] = false;
	debugLogEnabled[SIS_AnimFrame] = false;
}

bool SIS_IsBreakpoint(Bitu seg, Bitu off)
{
	return false;
}

void SIS_Temp_HandleSkipDrawObject(Bitu seg, Bitu off) {
	// Enabling one of the two blocks will skip either the first object or the second object
	/* if (seg == 0x01E7 && off == 0x92E5) {
		// Skip handling the first object
		reg_ax = 0x2;
	} */
	/* if (seg == 0x01E7 && off == 0x987C) {
		reg_ax = 0x02;
	} */ 
}

void SIS_HandleAnimFrame(Bitu seg, Bitu off)
{
	if (!debugLogEnabled[SIS_AnimFrame]) {
		return;
	}
		
	if (seg != 0x01F7) {
		return;
	}

	if (off == 0x1615) {
		fprintf(stdout,
		        "fn00B7_1480: Results of call: %.4x %.4x\n",
		        reg_ax,
		        reg_dx);
	}
}


void SIS_HandleSIS(Bitu seg, Bitu off) {
	// SIS_Temp_HandleSkipDrawObject(seg, off);
	SIS_HandleAnimFrame(seg, off);
}
