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

void SIS_LogAnimFrame(Bitu seg, Bitu off) {
	if (!(seg == 0x01E7 && off == 0x95D2)) {
		return;
	}
	uint16_t index = mem_readw_inline(GetAddress(SegValue(ss), reg_bp - 0x0A));
	if (index != 2) {
		return;	
	}

	// push	word ptr es:[di+2Eh] ;; [bp+1Ch]
	uint16_t v1 = mem_readw_inline(GetAddress(SegValue(ss), reg_sp + 0x16));
	// push	word ptr es:[di+2Ch] ;; [bp+1Ah]
	uint16_t v2 = mem_readw_inline(GetAddress(SegValue(ss), reg_sp + 0x14));
	// mov	ax,[bp-2h]
	// shr	ax,1h
	// mov	dx,ax
	// les	di,[bp-1Ah]
	// mov	ax,es:[di]
	// sub	ax,dx
	// push	ax ;; [bp+18h] 
	uint16_t v3 = mem_readw_inline(GetAddress(SegValue(ss), reg_sp + 0x12));
	// mov	ax,es:[di+2h]
	// sub	ax,[bp-4h]
	// sub	ax,[bp-8h]
	// push	ax ;; [bp+16h]
	uint16_t v4 = mem_readw_inline(GetAddress(SegValue(ss), reg_sp + 0x10));
	// push	2h ;; [bp+14h]
	// TODO: Add the 2
	uint16_t v5 = mem_readw_inline(GetAddress(SegValue(ss), reg_sp + 0x0E));
	// push	word ptr [bp-6h] ;; [bp+12h]
	uint16_t v6 = mem_readw_inline(GetAddress(SegValue(ss), reg_sp + 0x0C));
	// push	word ptr es:[di+2h] ;; [bp+10h] 
	uint16_t v7 = mem_readw_inline(GetAddress(SegValue(ss), reg_sp + 0x0A));

	// les	di,[0778h]
	// add	di,1013h
	// push	es ;; [bp+Eh]
	uint16_t v8 = mem_readw_inline(GetAddress(SegValue(ss), reg_sp + 0x08));
	// push	di ;; [bp+Ch]
	uint16_t v9 = mem_readw_inline(GetAddress(SegValue(ss), reg_sp + 0x06));
	// les	di,[0778h]
	// add	di,53D3h
	// push	es ;; [bp+Ah]
	uint16_t v10 = mem_readw_inline(GetAddress(SegValue(ss), reg_sp + 0x04));
	// push	di ;; [bp+8h]
	uint16_t v11 = mem_readw_inline(GetAddress(SegValue(ss), reg_sp + 0x02));
	// push	word ptr [bp-14h] ;; [bp+6h] 
	uint16_t v12 = mem_readw_inline(GetAddress(SegValue(ss), reg_sp + 0x00));
	// call	far 00B7h:172Ch

	uint16_t bp12 = mem_readw_inline(GetAddress(SegValue(ss), reg_bp - 0x12));
	fprintf(stdout,
	        "Arguments for 172C call: bp-12h: %.4x, di: %.4x - %.4x %.4x %.4x %.4x %.4x %.4x %.4x %.4x %.4x %.4x %.4x %.4x \n",
	        bp12, reg_di, v1, v2, v3, v4, v5, v6, v7, v8, v9, v10, v11, v12);
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
	SIS_LogAnimFrame(seg, off);
	SIS_HandleAnimFrame(seg, off);
}
