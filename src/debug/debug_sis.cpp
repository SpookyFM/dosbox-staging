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


void SIS_HandleSIS(Bitu seg, Bitu off) {}
