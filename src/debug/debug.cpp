/*
 *  Copyright (C) 2021-2023  The DOSBox Staging Team
 *  Copyright (C) 2002-2021  The DOSBox Team
 *
 *  This program is free software; you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License as published 
 *  the Free Software Foundation; either version 2 of the License, or
 *  (at your option) any later version.
 *
 *  This program is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *  GNU General Public License for more details.
 *
 *  You should have received a copy of the GNU General Public License along
 *  with this program; if not, write to the Free Software Foundation, Inc.,
 *  51 Franklin Street, Fifth Floor, Boston, MA 02110-1301, USA.
 */

#include "dosbox.h"

#if C_DEBUG

#include <string.h>
#include <list>
#include <vector>
#include <ctype.h>
#include <fstream>
#include <iomanip>
#include <string>
#include <sstream>
#include <regex>
#include <queue>
#include <chrono>
#include <iostream>
#include <cstdio>
using namespace std;

#include "debug.h"
#include "cross.h" //snprintf
#include "cpu.h"
#include "video.h"
#include "pic.h"
#include "mapper.h"
#include "cpu.h"
#include "callback.h"
#include "inout.h"
#include "mixer.h"
#include "timer.h"
#include "paging.h"
#include "string_utils.h"
#include "support.h"
#include "shell.h"
#include "programs.h"
#include "debug_inc.h"
#include "../cpu/lazyflags.h"
#include "keyboard.h"
#include "setup.h"
// Added for writing out a PPM of an off-screen buffer
#include "vga.h"
#include "std_filesystem.h"
#include "sdlmain.h"
#include <SDL2/SDL_syswm.h>
#include "SIS_OpcodeID/sis_opcode.h"

#include <map>

#include "debug_sis.h"

SDL_Window *GFX_GetSDLWindow(void);

int old_cursor_state;

// Forwards
static void DrawCode(void);
static void DEBUG_RaiseTimerIrq(void);
static void SaveMemory(uint16_t seg, uint32_t ofs1, uint32_t num);
static void SaveMemoryBin(uint16_t seg, uint32_t ofs1, uint32_t num);
static void SavePPM(uint16_t seg, uint32_t ofs1);
static void LogMCBS(void);
static void LogGDT(void);
static void LogLDT(void);
static void LogIDT(void);
static void LogPages(char* selname);
static void LogCPUInfo(void);
static void OutputVecTable(char* filename);
static void DrawVariables(void);
static void SaveCalltrace(char* filename);
static void SaveMemWrites(char* filename);

char* AnalyzeInstruction(char* inst, bool saveSelector);
uint32_t GetHexValue(char* str, char*& hex);

#if 0
class DebugPageHandler final : public PageHandler {
public:
	Bitu readb(PhysPt /*addr*/) {
	}
	Bitu readw(PhysPt /*addr*/) {
	}
	Bitu readd(PhysPt /*addr*/) {
	}
	void writeb(PhysPt /*addr*/,Bitu /*val*/) {
	}
	void writew(PhysPt /*addr*/,Bitu /*val*/) {
	}
	void writed(PhysPt /*addr*/,Bitu /*val*/) {
	}
};
#endif


class DEBUG;

DEBUG*	pDebugcom	= nullptr;
bool	exitLoop	= false;

std::regex* self_regex = nullptr;
// Saves the last address at which a regex breakpoint was triggered, for figuring out if we should skip the breakpoint
PhysPt regex_bp_addr = 0xFFFFFFFF;


// Heavy Debugging Vars for logging
#if C_HEAVY_DEBUG
static ofstream 	cpuLogFile;
static bool		cpuLog			= false;
static int		cpuLogCounter	= 0;
static int		cpuLogType		= 1;	// log detail
static bool zeroProtect = false;
bool	logHeavy	= false;
#endif

static struct  {
	uint32_t eax = 0;
	uint32_t ebx = 0;
	uint32_t ecx = 0;
	uint32_t edx = 0;
	uint32_t esi = 0;
	uint32_t edi = 0;
	uint32_t ebp = 0;
	uint32_t esp = 0;
	uint32_t eip = 0;
} oldregs = {};

static char curSelectorName[3] = { 0,0,0 };

static Segment oldsegs[6] = {};

static auto oldflags  = cpu_regs.flags;
static auto oldcpucpl = cpu.cpl;

DBGBlock dbg = {};
Bitu cycle_count = 0;

deque<callstack_entry> callstack;
deque<callstack_entry> calltrace;
deque<callstack_entry> rolling_calltrace;
int rolling_calltrace_length = 500;
bool callstack_started = false;
uint16_t script_off_skip_start = 0xFFFF;
uint16_t script_off_skip_end = 0xFFFF;

static bool debugging = false;

static void SetColor(Bitu test) {
	if (test) {
		if (has_colors()) { wattrset(dbg.win_reg,COLOR_PAIR(PAIR_BYELLOW_BLACK));}
	} else {
		if (has_colors()) { wattrset(dbg.win_reg,0);}
	}
}

#define MAXCMDLEN 254
struct SCodeViewData {
	int cursorPos          = 0;
	uint16_t firstInstSize = 0;
	uint16_t useCS         = 0;
	uint32_t useEIPlast    = 0;
	uint32_t useEIPmid     = 0;
	uint32_t useEIP        = 0;
	uint16_t cursorSeg     = 0;
	uint32_t cursorOfs     = 0;

	bool ovrMode = false;

	char inputStr[MAXCMDLEN + 1]     = {};
	char suspInputStr[MAXCMDLEN + 1] = {};

	int inputPos = 0;
} codeViewData = {};

static uint16_t dataSeg = 0;
static uint32_t dataOfs = 0;

static bool showExtend    = true;
static bool showPrintable = true;

static void ClearInputLine(void) {
	codeViewData.inputStr[0] = 0;
	codeViewData.inputPos = 0;
}

// History stuff
#define MAX_HIST_BUFFER 50
static list<string> histBuff = {};
static auto histBuffPos = histBuff.end();

/***********/
/* Helpers */
/***********/
uint32_t PhysMakeProt(uint16_t selector, uint32_t offset)
{
	Descriptor desc;
	if (cpu.gdt.GetDescriptor(selector,desc)) return desc.GetBase()+offset;
	return 0;
}

uint32_t GetAddress(uint16_t seg, uint32_t offset)
{
	if (seg==SegValue(cs)) return SegPhys(cs)+offset;
	if (cpu.pmode && !(reg_flags & FLAG_VM)) {
		Descriptor desc;
		if (cpu.gdt.GetDescriptor(seg,desc)) return PhysMakeProt(seg,offset);
	}
	return (seg<<4)+offset;
}

static char empty_sel[] = { ' ',' ',0 };

bool GetDescriptorInfo(char* selname, char* out1, char* out2)
{
	Bitu sel;
	Descriptor desc;

	if (strstr(selname,"cs") || strstr(selname,"CS")) sel = SegValue(cs);
	else if (strstr(selname,"ds") || strstr(selname,"DS")) sel = SegValue(ds);
	else if (strstr(selname,"es") || strstr(selname,"ES")) sel = SegValue(es);
	else if (strstr(selname,"fs") || strstr(selname,"FS")) sel = SegValue(fs);
	else if (strstr(selname,"gs") || strstr(selname,"GS")) sel = SegValue(gs);
	else if (strstr(selname,"ss") || strstr(selname,"SS")) sel = SegValue(ss);
	else {
		sel = GetHexValue(selname,selname);
		if (*selname==0) selname=empty_sel;
	}
	if (cpu.gdt.GetDescriptor(sel,desc)) {
		switch (desc.Type()) {
			case DESC_TASK_GATE:
				sprintf(out1,"%s: s:%08X type:%02X p",selname,desc.GetSelector(),desc.saved.gate.type);
				sprintf(out2,"    TaskGate   dpl : %01X %1X",desc.saved.gate.dpl,desc.saved.gate.p);
				return true;
			case DESC_LDT:
			case DESC_286_TSS_A:
			case DESC_286_TSS_B:
			case DESC_386_TSS_A:
			case DESC_386_TSS_B:
				sprintf(out1,"%s: b:%08X type:%02X pag",selname,desc.GetBase(),desc.saved.seg.type);
				sprintf(out2,"    l:%08X dpl : %01X %1X%1X%1X",desc.GetLimit(),desc.saved.seg.dpl,desc.saved.seg.p,desc.saved.seg.avl,desc.saved.seg.g);
				return true;
			case DESC_286_CALL_GATE:
			case DESC_386_CALL_GATE:
				sprintf(out1,"%s: s:%08X type:%02X p params: %02X",selname,desc.GetSelector(),desc.saved.gate.type,desc.saved.gate.paramcount);
				sprintf(out2,"    o:%08X dpl : %01X %1X",desc.GetOffset(),desc.saved.gate.dpl,desc.saved.gate.p);
				return true;
			case DESC_286_INT_GATE:
			case DESC_286_TRAP_GATE:
			case DESC_386_INT_GATE:
			case DESC_386_TRAP_GATE:
				sprintf(out1,"%s: s:%08X type:%02X p",selname,desc.GetSelector(),desc.saved.gate.type);
				sprintf(out2,"    o:%08X dpl : %01X %1X",desc.GetOffset(),desc.saved.gate.dpl,desc.saved.gate.p);
				return true;
		}
		sprintf(out1,"%s: b:%08X type:%02X parbg",selname,desc.GetBase(),desc.saved.seg.type);
		sprintf(out2,"    l:%08X dpl : %01X %1X%1X%1X%1X%1X",desc.GetLimit(),desc.saved.seg.dpl,desc.saved.seg.p,desc.saved.seg.avl,desc.saved.seg.r,desc.saved.seg.big,desc.saved.seg.g);
		return true;
	} else {
		strcpy(out1,"                                     ");
		strcpy(out2,"                                     ");
	}
	return false;
}

/********************/
/* DebugVar   stuff */
/********************/

class CDebugVar
{
public:
	CDebugVar(const char *vname, PhysPt address) : adr(address)
	{
		safe_strcpy(name, vname);
	}

	char*  GetName (void)                 { return name; }
	PhysPt GetAdr  (void)                 { return adr; }
	void   SetValue(bool has, uint16_t val) { hasvalue = has; value=val; }
	uint16_t GetValue(void)                 { return value; }
	bool   HasValue(void)                 { return hasvalue; }

private:
	const PhysPt adr = 0;
	char name[16]    = {};
	bool hasvalue    = false;
	uint16_t value   = 0;

public:
	static void       InsertVariable(char* name, PhysPt adr);
	static CDebugVar* FindVar       (PhysPt adr);
	static void       DeleteAll     ();
	static bool       SaveVars      (char* name);
	static bool       LoadVars      (char* name);
};

static std::vector<CDebugVar *> varList = {};

/********************/
/* Breakpoint stuff */
/********************/

bool skipFirstInstruction = false;

enum EBreakpoint { BKPNT_UNKNOWN, BKPNT_PHYSICAL, BKPNT_INTERRUPT, BKPNT_MEMORY, BKPNT_MEMORY_PROT, BKPNT_MEMORY_LINEAR, BKPNT_REG, BKPNT_RETURN, BKPNT_SO };

#define BPINT_ALL 0x100

class CBreakpoint
{
public:

	CBreakpoint(void);
	void					SetAddress		(uint16_t seg, uint32_t off)	{ location = GetAddress(seg,off); type = BKPNT_PHYSICAL; segment = seg; offset = off; }
	void					SetAddress		(PhysPt adr)				{ location = adr; type = BKPNT_PHYSICAL; }
	void					SetInt			(uint8_t _intNr, uint16_t ah, uint16_t al)	{ intNr = _intNr, ahValue = ah; alValue = al; type = BKPNT_INTERRUPT; }
	void					SetOnce			(bool _once)				{ once = _once; }
	void					SetType			(EBreakpoint _type)			{ type = _type; }
	void					SetValue		(uint8_t value)				{ ahValue = value; }
	void					SetOther		(uint8_t other)				{ alValue = other; }
	void					SetRegister		(uint8_t _reg)				{ reg = _reg; type = BKPNT_REG; }
	void SetCondition(uint8_t _targetValue)
	{
		targetValue  = _targetValue;
		hasCondition = true;
	}

	bool					IsActive		(void)						{ return active; }
	void					Activate		(bool _active);

	EBreakpoint GetType() const noexcept { return type; }
	bool GetOnce() const noexcept { return once; }
	PhysPt GetLocation() const noexcept { return location; }
	uint16_t GetSegment() const noexcept { return segment; }
	uint32_t GetOffset() const noexcept { return offset; }
	uint8_t GetIntNr() const noexcept { return intNr; }
	uint16_t GetValue() const noexcept { return ahValue; }
	uint16_t GetOther() const noexcept { return alValue; }
	uint8_t GetReg() const noexcept
	{
		return reg;
	}
	// TODO: Should be const
	bool IsConditionFulfilled(uint8_t value)  noexcept;

	// statics
	static CBreakpoint*		AddBreakpoint		(uint16_t seg, uint32_t off, bool once);
	static CBreakpoint*		AddIntBreakpoint	(uint8_t intNum, uint16_t ah, uint16_t al, bool once);
	static CBreakpoint*		AddMemBreakpoint	(uint16_t seg, uint32_t off);
	static CBreakpoint* AddConditionalMemBreakpoint(uint16_t seg, uint32_t off,
	                                                uint8_t targetValue);
	static CBreakpoint* AddReturnBreakpoint();
	static CBreakpoint*		AddRegBreakpoint	(uint8_t reg);
	static CBreakpoint* AddPixelBreakpoint(uint16_t x, uint16_t y);
	static CBreakpoint* AddBackBufferPixelBreakpoint(uint16_t x, uint16_t y);
	static CBreakpoint* AddScriptOpcodeBreakpoint(uint8_t opcode);
	static void				DeactivateBreakpoints();
	static void				ActivateBreakpoints	();
	static void				ActivateBreakpointsExceptAt(PhysPt adr);
	static bool				CheckBreakpoint		(PhysPt adr);
	static bool				CheckBreakpoint		(Bitu seg, Bitu off);
	static bool				CheckIntBreakpoint	(PhysPt adr, uint8_t intNr, uint16_t ahValue, uint16_t alValue);
	static bool CheckReturnBreakpoint();
	static bool CheckRegBreakpoint();
	static CBreakpoint*		FindPhysBreakpoint	(uint16_t seg, uint32_t off, bool once);
	static CBreakpoint*		FindOtherActiveBreakpoint(PhysPt adr, CBreakpoint* skip);
	static bool				IsBreakpoint		(uint16_t seg, uint32_t off);
	static bool				DeleteBreakpoint	(uint16_t seg, uint32_t off);
	static bool				DeleteByIndex		(uint16_t index);
	static void				DeleteAll			(void);
	static void				ShowList			(void);


private:
	EBreakpoint type = {};
	// Physical
	PhysPt location  = 0;
	uint8_t oldData  = 0;
	uint16_t segment = 0;
	uint32_t offset  = 0;
	// Conditional memoryF
	uint8_t targetValue = 0;
	bool hasCondition    = 0;
	// Int
	uint8_t intNr    = 0;
	uint16_t ahValue = 0;
	uint16_t alValue = 0;
	// Register
	uint8_t reg		 = 0;
	// Shared
	bool active = 0;
	bool once   = 0;

#	if C_HEAVY_DEBUG
	friend bool DEBUG_HeavyIsBreakpoint(void);
#	endif
};

CBreakpoint::CBreakpoint(void):
type(BKPNT_UNKNOWN),
location(0),oldData(0xCC),
segment(0),offset(0),intNr(0),ahValue(0),alValue(0),
active(false),once(false){ }

void CBreakpoint::Activate(bool _active)
{
#if !C_HEAVY_DEBUG
	if (GetType() == BKPNT_PHYSICAL) {
		if (_active) {
			// Set 0xCC and save old value
			uint8_t data = mem_readb(location);
			if (data != 0xCC) {
				oldData = data;
				mem_writeb(location,0xCC);
			} else if (!active) {
				// Another activate breakpoint is already here.
				// Find it, and copy its oldData value
				CBreakpoint *bp = FindOtherActiveBreakpoint(location, this);

				if (!bp || bp->oldData == 0xCC) {
					// This might also happen if there is a real 0xCC instruction here
					DEBUG_ShowMsg("DEBUG: Internal error while activating breakpoint.\n");
					oldData = 0xCC;
				} else
					oldData = bp->oldData;
			}
		} else {
			if (mem_readb(location) == 0xCC) {
				if (oldData == 0xCC)
					DEBUG_ShowMsg("DEBUG: Internal error while deactivating breakpoint.\n");

				// Check if we are the last active breakpoint at this location
				bool otherActive = (FindOtherActiveBreakpoint(location, this) != nullptr);

				// If so, remove 0xCC and set old value
				if (!otherActive)
					mem_writeb(location, oldData);
			}
		}
	}
#endif
	active = _active;
}

bool CBreakpoint::IsConditionFulfilled(uint8_t _value) noexcept
{
	const uint16_t oldValue = GetValue();
	bool valuesDiffer = oldValue != _value;
	if (hasCondition) {
		return valuesDiffer && (_value == targetValue);
	} else {
		return valuesDiffer;
	}
	return true;
}

// Statics
static std::list<CBreakpoint *> BPoints = {};

// TODO: Why does it work like this (reminder about declaring/defining static variables)
static bool breakOnReturn = true;



CBreakpoint* CBreakpoint::AddBreakpoint(uint16_t seg, uint32_t off, bool once)
{
	auto bp = new CBreakpoint();
	bp->SetAddress		(seg,off);
	bp->SetOnce			(once);
	BPoints.push_front	(bp);
	return bp;
}

CBreakpoint* CBreakpoint::AddIntBreakpoint(uint8_t intNum, uint16_t ah, uint16_t al, bool once)
{
	auto bp = new CBreakpoint();
	bp->SetInt			(intNum,ah,al);
	bp->SetOnce			(once);
	BPoints.push_front	(bp);
	return bp;
}

CBreakpoint* CBreakpoint::AddMemBreakpoint(uint16_t seg, uint32_t off)
{
	auto bp = new CBreakpoint();
	bp->SetAddress		(seg,off);
	bp->SetOnce			(false);
	bp->SetType			(BKPNT_MEMORY);
	BPoints.push_front	(bp);
	return bp;
}

CBreakpoint* CBreakpoint::AddConditionalMemBreakpoint(uint16_t seg, uint32_t off, uint8_t targetValue)
{
	auto bp = AddMemBreakpoint(seg, off);
	bp->SetCondition(targetValue);
	return bp;
}

CBreakpoint* CBreakpoint::AddReturnBreakpoint()
{
	auto bp = new CBreakpoint();
	bp->SetOnce(false);
	bp->SetType(BKPNT_RETURN);
	BPoints.push_front(bp);
	// TODO: Handle as a singleton or in a different way
	return bp;
}


CBreakpoint* CBreakpoint::AddRegBreakpoint(uint8_t reg)
{
	auto bp = new CBreakpoint();
	bp->SetRegister(reg);
	BPoints.push_front(bp);
	return bp;
}

CBreakpoint* CBreakpoint::AddPixelBreakpoint(uint16_t x, uint16_t y)
{
	// TODO: This is based on a lot of assumptions about the graphics mode
	return CBreakpoint::AddMemBreakpoint(0xa000, y * 320 + x);
	// return CBreakpoint::AddMemBreakpoint(0x026F, y * 320 + x);
}

CBreakpoint* CBreakpoint::AddBackBufferPixelBreakpoint(uint16_t x, uint16_t y)
{
	// Find out in which block this one lives
	uint16_t blockLength = 0x1e00 + 0x0140;
	uint16_t offset = y * 320 + x;
	uint16_t blockIndex = offset / blockLength;
	uint16_t blockOffset = offset % blockLength;
	
	std::vector<uint16_t> blocks = { 0x025F,0x0267,0x026F,0x0277, 0x027F, 0x0287, 0x028F, 0x0297 };
	uint16_t block = blocks[blockIndex];
	return CBreakpoint::AddMemBreakpoint(block, 0xc + blockOffset);
}

CBreakpoint* CBreakpoint::AddScriptOpcodeBreakpoint(uint8_t opcode)
{
	auto bp = new CBreakpoint();
	// Piggybacking on top of the opcode
	bp->SetRegister(opcode);
	bp->SetType(BKPNT_SO);
	bp->SetOnce(false);
	BPoints.push_front(bp);
	return bp;
}



void CBreakpoint::ActivateBreakpoints()
{
	// activate all breakpoints
	std::list<CBreakpoint*>::iterator i;
	for (auto &bp : BPoints)
		bp->Activate(true);
}

void CBreakpoint::DeactivateBreakpoints()
{
	// deactivate all breakpoints
	for (auto &bp : BPoints)
		bp->Activate(false);
}

void CBreakpoint::ActivateBreakpointsExceptAt(PhysPt adr)
{
	// activate all breakpoints, except those at adr
	std::list<CBreakpoint*>::iterator i;
	for (auto &bp : BPoints) {
		// Do not activate breakpoints at adr
		if (bp->GetType() == BKPNT_PHYSICAL && bp->GetLocation() == adr)
			continue;
		bp->Activate(true);
	}
	if (regex_bp_addr != adr) {
		regex_bp_addr = 0xFFFFFFFF;
	}
}

bool CBreakpoint::CheckReturnBreakpoint() {
	std::list<CBreakpoint*>::iterator i;
	for (auto& bp : BPoints) {
		// Do not activate breakpoints at adr
		if (bp->GetType() == BKPNT_RETURN)
			return true;
	}
	return false;
}

// Checks if breakpoint is valid and should stop execution
bool CBreakpoint::CheckBreakpoint(Bitu seg, Bitu off)
{

	
	// TODO: Quick and dirty implementation of jumping out of functions here



	// Quick exit if there are no breakpoints
	if (BPoints.empty()) return false;

	// Search matching breakpoint
	for (auto i = BPoints.begin(); i != BPoints.end(); ++i) {
		auto bp = (*i);

		if ((bp->GetType() == BKPNT_PHYSICAL) && bp->IsActive() &&
		    (bp->GetLocation() == GetAddress(seg, off))) {
			// Found
			if (bp->GetOnce()) {
				// delete it, if it should only be used once
				BPoints.erase(i);
				bp->Activate(false);
				delete bp;
			} else {
				// Also look for once-only breakpoints at this address
				bp = FindPhysBreakpoint(seg, off, true);
				if (bp) {
					BPoints.remove(bp);
					bp->Activate(false);
					delete bp;
				}
			}
			return true;
		}
#if C_HEAVY_DEBUG
		// Memory breakpoint support
		else if (bp->IsActive()) {
			if ((bp->GetType()==BKPNT_MEMORY) || (bp->GetType()==BKPNT_MEMORY_PROT) || (bp->GetType()==BKPNT_MEMORY_LINEAR)) {
				// Watch Protected Mode Memoryonly in pmode
				if (bp->GetType()==BKPNT_MEMORY_PROT) {
					// Check if pmode is active
					if (!cpu.pmode) return false;
					// Check if descriptor is valid
					Descriptor desc;
					if (!cpu.gdt.GetDescriptor(bp->GetSegment(),desc)) return false;
					if (desc.GetLimit()==0) return false;
				}

				Bitu address; 
				if (bp->GetType()==BKPNT_MEMORY_LINEAR) address = bp->GetOffset();
				else address = GetAddress(bp->GetSegment(),bp->GetOffset());
				uint8_t value=0;
				if (mem_readb_checked(address,&value)) return false;
				if (bp->IsConditionFulfilled(value)) {
					// TODO: Hardcoded check - make sure we are in game code
					uint16_t bpSeg = SegValue(cs);
					if (!(bpSeg == 0x01F7 || bpSeg == 0x01E7 || bpSeg == 0x01D7 || bpSeg == 0x0217)) {
						return false;
					}
				
						// Yup, memory value changed
						DEBUG_ShowMsg("DEBUG: Memory breakpoint %s: %04X:%04X - %02X -> %02X\n",
						              (bp->GetType() ==
						               BKPNT_MEMORY_PROT)
						                      ? "(Prot)"
						                      : "",
						              bp->GetSegment(),
						              bp->GetOffset(),
						              bp->GetValue(),
						              value);
							// TODO: Duplicated, as we need to change the value in any case. There should be a better place to put this update.
					        bp->SetValue(value);
						return true;
				}
				bp->SetValue(value);
			}
		}
#endif
		if (bp->GetType() == BKPNT_SO) {
			if (seg == 0x01E7 && off == 0xDB8E) {
				if (bp->GetReg() == reg_al) {
					uint16_t script_offset;
					uint16_t script_seg;
					uint16_t script_off;
					SIS_GetScriptInfos(script_offset,
					                   script_seg,
					                   script_off);
					DEBUG_ShowMsg("DEBUG: Script opcode breakpoint %.2x at offset %.4x (%.4x:%.4x)\n",
					              reg_al,
					              script_offset,
					              script_seg,
					              script_off);
					return true;
				}
			}
		}
	}
	return false;
}

bool CBreakpoint::CheckRegBreakpoint() {
	if (BPoints.empty())
		return false;

	for (auto i = BPoints.begin(); i != BPoints.end(); ++i) {
		auto bp = (*i);
		if ((bp->GetType() == BKPNT_REG) && bp->IsActive()) {
			if (bp->GetReg() == REGI_BP) {
				if (oldregs.ebp != reg_ebp) {
					return true;
				}
			}
		}
	}
	return false;
}

bool CBreakpoint::CheckIntBreakpoint([[maybe_unused]] PhysPt adr, uint8_t intNr, uint16_t ahValue, uint16_t alValue)
// Checks if interrupt breakpoint is valid and should stop execution
{
	if (BPoints.empty()) return false;

	// Search matching breakpoint
	for (auto i = BPoints.begin(); i != BPoints.end(); ++i) {
		auto bp = (*i);
		if ((bp->GetType() == BKPNT_INTERRUPT) && bp->IsActive() &&
		    (bp->GetIntNr() == intNr)) {
			if (((bp->GetValue() == BPINT_ALL) ||
			     (bp->GetValue() == ahValue)) &&
			    ((bp->GetOther() == BPINT_ALL) ||
			     (bp->GetOther() == alValue))) {
				// Ignore it once ?
				// Found
				if (bp->GetOnce()) {
					// delete it, if it should only be used once
					BPoints.erase(i);
					bp->Activate(false);
					delete bp;
				}
				return true;
			}
		}
	}
	return false;
}

void CBreakpoint::DeleteAll()
{
	for (auto &bp : BPoints) {
		bp->Activate(false);
		delete bp;
	}
	BPoints.clear();
}

bool CBreakpoint::DeleteByIndex(uint16_t index)
{
	// Request is past the end
	if (index >= BPoints.size())
		return false;

	auto it = BPoints.begin();
	std::advance(it, index);
	auto bp = *it;

	BPoints.erase(it);
	bp->Activate(false);
	delete bp;
	return true;
}

CBreakpoint* CBreakpoint::FindPhysBreakpoint(uint16_t seg, uint32_t off, bool once)
{
	if (BPoints.empty()) return nullptr;
#if !C_HEAVY_DEBUG
	PhysPt adr = GetAddress(seg, off);
#endif
	// Search for matching breakpoint
	for (auto &bp : BPoints) {
#	if C_HEAVY_DEBUG
		// Heavy debugging breakpoints are triggered by matching seg:off
		bool atLocation = bp->GetSegment() == seg && bp->GetOffset() == off;
#else
		// Normal debugging breakpoints are triggered at an address
		bool atLocation = bp->GetLocation() == adr;
#endif

		if (bp->GetType() == BKPNT_PHYSICAL && atLocation && bp->GetOnce() == once)
			return bp;
	}

	return nullptr;
}

CBreakpoint* CBreakpoint::FindOtherActiveBreakpoint(PhysPt adr, CBreakpoint* skip)
{
	for (auto &bp : BPoints)
		if (bp != skip && bp->GetType() == BKPNT_PHYSICAL && bp->GetLocation() == adr && bp->IsActive())
			return bp;
	return nullptr;
}

// is there a permanent breakpoint at address ?
bool CBreakpoint::IsBreakpoint(uint16_t seg, uint32_t off)
{
	return FindPhysBreakpoint(seg, off, false) != nullptr;
}

bool CBreakpoint::DeleteBreakpoint(uint16_t seg, uint32_t off)
{
	CBreakpoint* bp = FindPhysBreakpoint(seg, off, false);
	if (bp) {
		BPoints.remove(bp);
		delete bp;
		return true;
	}

	return false;
}


void CBreakpoint::ShowList(void)
{
	// iterate list
	int nr = 0;
	for (auto &bp : BPoints) {
		if (bp->GetType()==BKPNT_PHYSICAL) {
			DEBUG_ShowMsg("%02X. BP %04X:%04X\n",nr,bp->GetSegment(),bp->GetOffset());
		} else if (bp->GetType()==BKPNT_INTERRUPT) {
			if (bp->GetValue()==BPINT_ALL) DEBUG_ShowMsg("%02X. BPINT %02X\n",nr,bp->GetIntNr());
			else if (bp->GetOther()==BPINT_ALL) DEBUG_ShowMsg("%02X. BPINT %02X AH=%02X\n",nr,bp->GetIntNr(),bp->GetValue());
			else DEBUG_ShowMsg("%02X. BPINT %02X AH=%02X AL=%02X\n",nr,bp->GetIntNr(),bp->GetValue(),bp->GetOther());
		} else if (bp->GetType()==BKPNT_MEMORY) {
			DEBUG_ShowMsg("%02X. BPMEM %04X:%04X (%02X)\n",nr,bp->GetSegment(),bp->GetOffset(),bp->GetValue());
		} else if (bp->GetType()==BKPNT_MEMORY_PROT) {
			DEBUG_ShowMsg("%02X. BPPM %04X:%08X (%02X)\n",nr,bp->GetSegment(),bp->GetOffset(),bp->GetValue());
		} else if (bp->GetType()==BKPNT_MEMORY_LINEAR ) {
			DEBUG_ShowMsg("%02X. BPLM %08X (%02X)\n",nr,bp->GetOffset(),bp->GetValue());
		} else if (bp->GetType() == BKPNT_REG) {
			DEBUG_ShowMsg("%02X. BPREG %02X\n",
			              nr,
					// TODO: Display the name
			              bp->GetReg());
		}
		else if (bp->GetType() == BKPNT_SO) {
			DEBUG_ShowMsg("%02X. BPSO %02X\n",
			              nr,
			              bp->GetReg());
		}
		nr++;
	}
}

bool DEBUG_Breakpoint(void)
{
	/* First get the physical address and check for a set Breakpoint */
	if (!CBreakpoint::CheckBreakpoint(SegValue(cs),reg_eip)) return false;
	// Found. Breakpoint is valid
	// PhysPt where=GetAddress(SegValue(cs),reg_eip); -- "where" is unused
	CBreakpoint::DeactivateBreakpoints();	// Deactivate all breakpoints
	return true;
}

bool DEBUG_ReturnBreakpoint(void) {
	return CBreakpoint::CheckReturnBreakpoint();
}

bool DEBUG_IntBreakpoint(uint8_t intNum)
{
	/* First get the physical address and check for a set Breakpoint */
	PhysPt where=GetAddress(SegValue(cs),reg_eip);
	if (!CBreakpoint::CheckIntBreakpoint(where,intNum,reg_ah,reg_al)) return false;
	// Found. Breakpoint is valid
	CBreakpoint::DeactivateBreakpoints();	// Deactivate all breakpoints
	return true;
}

static bool StepOver()
{
	exitLoop = false;
	PhysPt start=GetAddress(SegValue(cs),reg_eip);
	char dline[200];Bitu size;
	size=DasmI386(dline, start, reg_eip, cpu.code.big);

	if (strstr(dline,"call") || strstr(dline,"int") || strstr(dline,"loop") || strstr(dline,"rep")) {
		// Don't add a temporary breakpoint if there's already one here
		if (!CBreakpoint::FindPhysBreakpoint(SegValue(cs), reg_eip+size, true))
			CBreakpoint::AddBreakpoint(SegValue(cs),reg_eip+size, true);
		debugging=false;
		DrawCode();
		return true;
	} 
	return false;
}

bool DEBUG_ExitLoop(void)
{
#if C_HEAVY_DEBUG
	DrawVariables();
#endif

	if (exitLoop) {
		exitLoop = false;
		return true;
	}
	return false;
}

/********************/
/*   Draw windows   */
/********************/

static void DrawData(void) {

	uint8_t ch;
	uint32_t add = dataOfs;
	uint32_t address;
	/* Data win */	
	for (int y=0; y<8; y++) {
		// Address
		if (add<0x10000) mvwprintw (dbg.win_data,y,0,"%04X:%04X     ",dataSeg,add);
		else mvwprintw (dbg.win_data,y,0,"%04X:%08X ",dataSeg,add);
		for (int x=0; x<16; x++) {
			address = GetAddress(dataSeg,add);
			if (mem_readb_checked(address,&ch)) ch=0;
			mvwprintw (dbg.win_data,y,14+3*x,"%02X",ch);
			if (showPrintable) {
				if (ch<32 || !isprint(*reinterpret_cast<unsigned char*>(&ch))) ch='.';
				mvwaddch (dbg.win_data,y,63+x,ch);
			} else {
#if PDCURSES
				mvwaddrawch (dbg.win_data,y,63+x,ch);
#else
				if (ch<32) ch='.';
				mvwaddch (dbg.win_data,y,63+x,ch);
#endif
			}
			add++;
		}
	}
	wrefresh(dbg.win_data);
}

static void DrawRegisters(void) {
	/* Main Registers */
	SetColor(reg_eax!=oldregs.eax);oldregs.eax=reg_eax;mvwprintw (dbg.win_reg,0,4,"%08X",reg_eax);
	SetColor(reg_ebx!=oldregs.ebx);oldregs.ebx=reg_ebx;mvwprintw (dbg.win_reg,1,4,"%08X",reg_ebx);
	SetColor(reg_ecx!=oldregs.ecx);oldregs.ecx=reg_ecx;mvwprintw (dbg.win_reg,2,4,"%08X",reg_ecx);
	SetColor(reg_edx!=oldregs.edx);oldregs.edx=reg_edx;mvwprintw (dbg.win_reg,3,4,"%08X",reg_edx);

	SetColor(reg_esi!=oldregs.esi);oldregs.esi=reg_esi;mvwprintw (dbg.win_reg,0,18,"%08X",reg_esi);
	SetColor(reg_edi!=oldregs.edi);oldregs.edi=reg_edi;mvwprintw (dbg.win_reg,1,18,"%08X",reg_edi);
	SetColor(reg_ebp!=oldregs.ebp);oldregs.ebp=reg_ebp;mvwprintw (dbg.win_reg,2,18,"%08X",reg_ebp);
	SetColor(reg_esp!=oldregs.esp);oldregs.esp=reg_esp;mvwprintw (dbg.win_reg,3,18,"%08X",reg_esp);
	SetColor(reg_eip!=oldregs.eip);oldregs.eip=reg_eip;mvwprintw (dbg.win_reg,1,42,"%08X",reg_eip);

	SetColor(SegValue(ds)!=oldsegs[ds].val);oldsegs[ds].val=SegValue(ds);mvwprintw (dbg.win_reg,0,31,"%04X",SegValue(ds));
	SetColor(SegValue(es)!=oldsegs[es].val);oldsegs[es].val=SegValue(es);mvwprintw (dbg.win_reg,0,41,"%04X",SegValue(es));
	SetColor(SegValue(fs)!=oldsegs[fs].val);oldsegs[fs].val=SegValue(fs);mvwprintw (dbg.win_reg,0,51,"%04X",SegValue(fs));
	SetColor(SegValue(gs)!=oldsegs[gs].val);oldsegs[gs].val=SegValue(gs);mvwprintw (dbg.win_reg,0,61,"%04X",SegValue(gs));
	SetColor(SegValue(ss)!=oldsegs[ss].val);oldsegs[ss].val=SegValue(ss);mvwprintw (dbg.win_reg,0,71,"%04X",SegValue(ss));
	SetColor(SegValue(cs)!=oldsegs[cs].val);oldsegs[cs].val=SegValue(cs);mvwprintw (dbg.win_reg,1,31,"%04X",SegValue(cs));

	/*Individual flags*/
	Bitu changed_flags = reg_flags ^ oldflags;
	oldflags = reg_flags;

	SetColor(changed_flags&FLAG_CF);
	mvwprintw (dbg.win_reg,1,53,"%01X",GETFLAG(CF) ? 1:0);
	SetColor(changed_flags&FLAG_ZF);
	mvwprintw (dbg.win_reg,1,56,"%01X",GETFLAG(ZF) ? 1:0);
	SetColor(changed_flags&FLAG_SF);
	mvwprintw (dbg.win_reg,1,59,"%01X",GETFLAG(SF) ? 1:0);
	SetColor(changed_flags&FLAG_OF);
	mvwprintw (dbg.win_reg,1,62,"%01X",GETFLAG(OF) ? 1:0);
	SetColor(changed_flags&FLAG_AF);
	mvwprintw (dbg.win_reg,1,65,"%01X",GETFLAG(AF) ? 1:0);
	SetColor(changed_flags&FLAG_PF);
	mvwprintw (dbg.win_reg,1,68,"%01X",GETFLAG(PF) ? 1:0);


	SetColor(changed_flags&FLAG_DF);
	mvwprintw (dbg.win_reg,1,71,"%01X",GETFLAG(DF) ? 1:0);
	SetColor(changed_flags&FLAG_IF);
	mvwprintw (dbg.win_reg,1,74,"%01X",GETFLAG(IF) ? 1:0);
	SetColor(changed_flags&FLAG_TF);
	mvwprintw (dbg.win_reg,1,77,"%01X",GETFLAG(TF) ? 1:0);

	SetColor(changed_flags&FLAG_IOPL);
	mvwprintw (dbg.win_reg,2,72,"%01X",GETFLAG(IOPL)>>12);


	SetColor(cpu.cpl ^ oldcpucpl);
	mvwprintw (dbg.win_reg,2,78,"%01" PRIXPTR ,cpu.cpl);
	oldcpucpl=cpu.cpl;

	if (cpu.pmode) {
		if (reg_flags & FLAG_VM) mvwprintw(dbg.win_reg,0,76,"VM86");
		else if (cpu.code.big) mvwprintw(dbg.win_reg,0,76,"Pr32");
		else mvwprintw(dbg.win_reg,0,76,"Pr16");
	} else
		mvwprintw(dbg.win_reg,0,76,"Real");

	// Selector info, if available
	if ((cpu.pmode) && curSelectorName[0]) {
		char out1[200], out2[200];
		GetDescriptorInfo(curSelectorName,out1,out2);
		mvwprintw(dbg.win_reg,2,28,out1);
		mvwprintw(dbg.win_reg,3,28,out2);
	}

	wattrset(dbg.win_reg,0);
	// We replace cycle counter by the current scripting location
	uint32_t script_location = mem_readw_inline(GetAddress(SegValue(ds), 0x0F8A));
	// mvwprintw(dbg.win_reg,3,60,"%" PRIuPTR "       ",cycle_count);
	mvwprintw(dbg.win_reg, 3, 60, "%.4x", script_location);
	wrefresh(dbg.win_reg);
}

static void DrawCode(void) {
	bool saveSel;
	uint32_t disEIP = codeViewData.useEIP;
	PhysPt start  = GetAddress(codeViewData.useCS,codeViewData.useEIP);
	char dline[200];Bitu size;Bitu c;
	static char line20[21] = "                    ";

	for (int i=0;i<10;i++) {
		saveSel = false;
		if (has_colors()) {
			if ((codeViewData.useCS==SegValue(cs)) && (disEIP == reg_eip)) {
				wattrset(dbg.win_code,COLOR_PAIR(PAIR_GREEN_BLACK));
				if (codeViewData.cursorPos==-1) {
					codeViewData.cursorPos = i; // Set Cursor
				}
				if (i == codeViewData.cursorPos) {
					codeViewData.cursorSeg = SegValue(cs);
					codeViewData.cursorOfs = disEIP;
				}
				saveSel = (i == codeViewData.cursorPos);
			} else if (i == codeViewData.cursorPos) {
				wattrset(dbg.win_code,COLOR_PAIR(PAIR_BLACK_GREY));
				codeViewData.cursorSeg = codeViewData.useCS;
				codeViewData.cursorOfs = disEIP;
				saveSel = true;
			} else if (CBreakpoint::IsBreakpoint(codeViewData.useCS, disEIP)) {
				wattrset(dbg.win_code,COLOR_PAIR(PAIR_GREY_RED));
			} else {
				wattrset(dbg.win_code,0);
			}
		}


		Bitu drawsize=size=DasmI386(dline, start, disEIP, cpu.code.big);
		bool toolarge = false;
		mvwprintw(dbg.win_code,i,0,"%04X:%04X  ",codeViewData.useCS,disEIP);

		if (drawsize>10) { toolarge = true; drawsize = 9; }
		for (c=0;c<drawsize;c++) {
			uint8_t value;
			if (mem_readb_checked(start+c,&value)) value=0;
			wprintw(dbg.win_code,"%02X",value);
		}
		if (toolarge) { waddstr(dbg.win_code,".."); drawsize++; }
		// Spacepad up to 20 characters
		if(drawsize && (drawsize < 11)) {
			line20[20 - drawsize*2] = 0;
			waddstr(dbg.win_code,line20);
			line20[20 - drawsize*2] = ' ';
		} else waddstr(dbg.win_code,line20);

		char empty_res[] = { 0 };
		char* res = empty_res;
		if (showExtend) res = AnalyzeInstruction(dline, saveSel);
		// Spacepad it up to 28 characters
		size_t dline_len = safe_strlen(dline);
		if (dline_len < 28) memset(dline + dline_len, ' ',28 - dline_len);
		dline[28] = 0;
		waddstr(dbg.win_code,dline);
		// Spacepad it up to 20 characters
		size_t res_len = strlen(res);
		if(res_len && (res_len < 21)) {
			waddstr(dbg.win_code,res);
			line20[20-res_len] = 0;
			waddstr(dbg.win_code,line20);
			line20[20-res_len] = ' ';
		} else 	waddstr(dbg.win_code,line20);

		start+=size;
		disEIP+=size;

		if (i==0) codeViewData.firstInstSize = size;
		if (i==4) codeViewData.useEIPmid	 = disEIP;
	}

	codeViewData.useEIPlast = disEIP;

	wattrset(dbg.win_code,0);
	if (!debugging) {
		if (has_colors()) wattrset(dbg.win_code,COLOR_PAIR(PAIR_GREEN_BLACK));
		mvwprintw(dbg.win_code,10,0,"%s","(Running)");
		wclrtoeol(dbg.win_code);
	} else {
		//TODO long lines
		char* dispPtr = codeViewData.inputStr;
		char* curPtr = &codeViewData.inputStr[codeViewData.inputPos];
		mvwprintw(dbg.win_code,10,0,"%c-> %s%c",
			(codeViewData.ovrMode?'O':'I'),dispPtr,(*curPtr?' ':'_'));
		wclrtoeol(dbg.win_code); // not correct in pdcurses if full line
		mvwchgat(dbg.win_code,10,0,3,0,(PAIR_BLACK_GREY),nullptr);
		if (*curPtr) {
			mvwchgat(dbg.win_code,10,(curPtr-dispPtr+4),1,0,(PAIR_BLACK_GREY),nullptr);
 		}
	}

	wattrset(dbg.win_code,0);
	wrefresh(dbg.win_code);
}

static void SetCodeWinStart()
{
	if ((SegValue(cs)==codeViewData.useCS) && (reg_eip>=codeViewData.useEIP) && (reg_eip<=codeViewData.useEIPlast)) {
		// in valid window - scroll ?
		if (reg_eip>=codeViewData.useEIPmid) codeViewData.useEIP += codeViewData.firstInstSize;

	} else {
		// totally out of range.
		codeViewData.useCS	= SegValue(cs);
		codeViewData.useEIP	= reg_eip;
	}
	codeViewData.cursorPos = -1;	// Recalc Cursor position
}

/********************/
/*    User input    */
/********************/

uint32_t GetHexValue(char* str, char*& hex)
{
	uint32_t	value = 0;
	uint32_t regval = 0;
	hex = str;
	while (*hex == ' ') hex++;
	if (strncmp(hex,"EAX",3) == 0) { hex+=3; regval = reg_eax; } else
	if (strncmp(hex,"EBX",3) == 0) { hex+=3; regval = reg_ebx; } else
	if (strncmp(hex,"ECX",3) == 0) { hex+=3; regval = reg_ecx; } else
	if (strncmp(hex,"EDX",3) == 0) { hex+=3; regval = reg_edx; } else
	if (strncmp(hex,"ESI",3) == 0) { hex+=3; regval = reg_esi; } else
	if (strncmp(hex,"EDI",3) == 0) { hex+=3; regval = reg_edi; } else
	if (strncmp(hex,"EBP",3) == 0) { hex+=3; regval = reg_ebp; } else
	if (strncmp(hex,"ESP",3) == 0) { hex+=3; regval = reg_esp; } else
	if (strncmp(hex,"EIP",3) == 0) { hex+=3; regval = reg_eip; } else
	if (strncmp(hex,"AX",2) == 0)  { hex+=2; regval = reg_ax; } else
	if (strncmp(hex,"BX",2) == 0)  { hex+=2; regval = reg_bx; } else
	if (strncmp(hex,"CX",2) == 0)  { hex+=2; regval = reg_cx; } else
	if (strncmp(hex,"DX",2) == 0)  { hex+=2; regval = reg_dx; } else
	if (strncmp(hex,"SI",2) == 0)  { hex+=2; regval = reg_si; } else
	if (strncmp(hex,"DI",2) == 0)  { hex+=2; regval = reg_di; } else
	if (strncmp(hex,"BP",2) == 0)  { hex+=2; regval = reg_bp; } else
	if (strncmp(hex,"SP",2) == 0)  { hex+=2; regval = reg_sp; } else
	if (strncmp(hex,"IP",2) == 0)  { hex+=2; regval = reg_ip; } else
	if (strncmp(hex,"CS",2) == 0)  { hex+=2; regval = SegValue(cs); } else
	if (strncmp(hex,"DS",2) == 0)  { hex+=2; regval = SegValue(ds); } else
	if (strncmp(hex,"ES",2) == 0)  { hex+=2; regval = SegValue(es); } else
	if (strncmp(hex,"FS",2) == 0)  { hex+=2; regval = SegValue(fs); } else
	if (strncmp(hex,"GS",2) == 0)  { hex+=2; regval = SegValue(gs); } else
	if (strncmp(hex,"SS",2) == 0)  { hex+=2; regval = SegValue(ss); }

	while (*hex) {
		if      ((*hex >= '0') && (*hex <= '9')) value = (value<<4) + *hex - '0';
		else if ((*hex >= 'A') && (*hex <= 'F')) value = (value<<4) + *hex - 'A' + 10;
		else {
			if (*hex == '+') {hex++;return regval + value + GetHexValue(hex,hex); } else
			if (*hex == '-') {hex++;return regval + value - GetHexValue(hex,hex); }
			else break; // No valid char
		}
		hex++;
	}
	return regval + value;
}

bool ChangeRegister(char* str)
{
	char* hex = str;
	while (*hex==' ') hex++;
	if (strncmp(hex,"EAX",3) == 0) { hex+=3; reg_eax = GetHexValue(hex,hex); } else
	if (strncmp(hex,"EBX",3) == 0) { hex+=3; reg_ebx = GetHexValue(hex,hex); } else
	if (strncmp(hex,"ECX",3) == 0) { hex+=3; reg_ecx = GetHexValue(hex,hex); } else
	if (strncmp(hex,"EDX",3) == 0) { hex+=3; reg_edx = GetHexValue(hex,hex); } else
	if (strncmp(hex,"ESI",3) == 0) { hex+=3; reg_esi = GetHexValue(hex,hex); } else
	if (strncmp(hex,"EDI",3) == 0) { hex+=3; reg_edi = GetHexValue(hex,hex); } else
	if (strncmp(hex,"EBP",3) == 0) { hex+=3; reg_ebp = GetHexValue(hex,hex); } else
	if (strncmp(hex,"ESP",3) == 0) { hex+=3; reg_esp = GetHexValue(hex,hex); } else
	if (strncmp(hex,"EIP",3) == 0) { hex+=3; reg_eip = GetHexValue(hex,hex); } else
	if (strncmp(hex,"AX",2) == 0)  { hex+=2; reg_ax = (uint16_t)GetHexValue(hex,hex); } else
	if (strncmp(hex,"BX",2) == 0)  { hex+=2; reg_bx = (uint16_t)GetHexValue(hex,hex); } else
	if (strncmp(hex,"CX",2) == 0)  { hex+=2; reg_cx = (uint16_t)GetHexValue(hex,hex); } else
	if (strncmp(hex,"DX",2) == 0)  { hex+=2; reg_dx = (uint16_t)GetHexValue(hex,hex); } else
	if (strncmp(hex,"SI",2) == 0)  { hex+=2; reg_si = (uint16_t)GetHexValue(hex,hex); } else
	if (strncmp(hex,"DI",2) == 0)  { hex+=2; reg_di = (uint16_t)GetHexValue(hex,hex); } else
	if (strncmp(hex,"BP",2) == 0)  { hex+=2; reg_bp = (uint16_t)GetHexValue(hex,hex); } else
	if (strncmp(hex,"SP",2) == 0)  { hex+=2; reg_sp = (uint16_t)GetHexValue(hex,hex); } else
	if (strncmp(hex,"IP",2) == 0)  { hex+=2; reg_ip = (uint16_t)GetHexValue(hex,hex); } else
	if (strncmp(hex,"CS",2) == 0)  { hex+=2; SegSet16(cs,(uint16_t)GetHexValue(hex,hex)); } else
	if (strncmp(hex,"DS",2) == 0)  { hex+=2; SegSet16(ds,(uint16_t)GetHexValue(hex,hex)); } else
	if (strncmp(hex,"ES",2) == 0)  { hex+=2; SegSet16(es,(uint16_t)GetHexValue(hex,hex)); } else
	if (strncmp(hex,"FS",2) == 0)  { hex+=2; SegSet16(fs,(uint16_t)GetHexValue(hex,hex)); } else
	if (strncmp(hex,"GS",2) == 0)  { hex+=2; SegSet16(gs,(uint16_t)GetHexValue(hex,hex)); } else
	if (strncmp(hex,"SS",2) == 0)  { hex+=2; SegSet16(ss,(uint16_t)GetHexValue(hex,hex)); } else
	if (strncmp(hex,"AF",2) == 0)  { hex+=2; SETFLAGBIT(AF,GetHexValue(hex,hex)); } else
	if (strncmp(hex,"CF",2) == 0)  { hex+=2; SETFLAGBIT(CF,GetHexValue(hex,hex)); } else
	if (strncmp(hex,"DF",2) == 0)  { hex+=2; SETFLAGBIT(DF,GetHexValue(hex,hex)); } else
	if (strncmp(hex,"IF",2) == 0)  { hex+=2; SETFLAGBIT(IF,GetHexValue(hex,hex)); } else
	if (strncmp(hex,"OF",2) == 0)  { hex+=2; SETFLAGBIT(OF,GetHexValue(hex,hex)); } else
	if (strncmp(hex,"ZF",2) == 0)  { hex+=2; SETFLAGBIT(ZF,GetHexValue(hex,hex)); } else
	if (strncmp(hex,"PF",2) == 0)  { hex+=2; SETFLAGBIT(PF,GetHexValue(hex,hex)); } else
	if (strncmp(hex,"SF",2) == 0)  { hex+=2; SETFLAGBIT(SF,GetHexValue(hex,hex)); } else
	{ return false; }
	return true;
}

extern bool mouseBreakpoint;
extern bool mouseBreakpointHit;
extern bool outsideStackWriteBreakpoint;
extern bool outsideStackWriteBreakpointHit;
extern PhysPt memReadWatch1;
extern PhysPt memReadWatch2;
extern bool memReadWatchHit1;
extern bool memReadWatchHit2;
extern PhysPt memReadOverride;
extern uint32_t memReadOverrideValue;
std::map<std::string, SIS_ChannelID> channelIDNames;
std::map<SIS_ChannelID, bool> debugLogEnabledID;

void DrawBackBuffer(uint16_t source_segment, uint16_t target_offset, uint16_t length) {
	constexpr auto palette_map = vga.dac.palette_map;
	
	for (int i = 0; i < length; i++) {
		int j = i + target_offset;
		uint16_t x = j % 320;
		uint16_t y = j / 320;

		const auto graphics_window = GFX_GetSDLWindow();
		const auto graphics_surface = SDL_GetWindowSurface(graphics_window);

		uint32_t value = mem_readb(GetAddress(source_segment, 0xc + i));
		Uint32* target_pixel = (Uint32*)((Uint8*)graphics_surface->pixels
			+ 2 * y * graphics_surface->pitch 
			+ 2 * x * graphics_surface->format->BytesPerPixel);
		*target_pixel = *(palette_map + value);

		target_pixel = (Uint32*)((Uint8*)graphics_surface->pixels
			+ 2 * y * graphics_surface->pitch
			+ 2 * x * graphics_surface->format->BytesPerPixel + graphics_surface->format->BytesPerPixel);
		*target_pixel = *(palette_map + value);

		target_pixel = (Uint32*)((Uint8*)graphics_surface->pixels
			+ 2 * y * graphics_surface->pitch + graphics_surface->pitch
			+ 2 * x * graphics_surface->format->BytesPerPixel);
		*target_pixel = *(palette_map + value);
		
		target_pixel = (Uint32*)((Uint8*)graphics_surface->pixels
			+ 2 * y * graphics_surface->pitch + graphics_surface->pitch
			+ 2 * x * graphics_surface->format->BytesPerPixel + graphics_surface->format->BytesPerPixel);
		*target_pixel = *(palette_map + value);
	}
}

bool ParseCommand(char* str) {
	char* found = str;
	// Change: We only uppercase the first word = the command itself
	for(char* idx = found;*idx != 0 && *idx != ' '; idx++)
		*idx = toupper(*idx);

	found = trim(found);
	string s_found(found);
	istringstream stream(s_found);
	string command;
	stream >> command;
	string::size_type next = s_found.find_first_not_of(' ',command.size());
	if(next == string::npos) next = command.size();
	(s_found.erase)(0,next);
	found = const_cast<char*>(s_found.c_str());

	if (command == "MEMDUMP") { // Dump memory to file
		uint16_t seg = (uint16_t)GetHexValue(found,found); found++;
		uint32_t ofs = GetHexValue(found,found); found++;
		uint32_t num = GetHexValue(found,found); found++;
		SaveMemory(seg,ofs,num);
		return true;
	}

	if (command == "MEMDUMPBIN") { // Dump memory to file binary
		uint16_t seg = (uint16_t)GetHexValue(found,found); found++;
		uint32_t ofs = GetHexValue(found,found); found++;
		uint32_t num = GetHexValue(found,found); found++;
		SaveMemoryBin(seg,ofs,num);
		return true;
	}

	if (command == "DUMPPPM") { // Dump image data from an off-screen buffer
		uint16_t seg = (uint16_t)GetHexValue(found, found);
		found++;
		SavePPM(seg, 0);
		return true;
	}

	if (command == "NEXTSEG") {
		DOS_PSP psp(dos.psp());
		uint16_t size = psp.GetSize();
		DEBUG_ShowMsg("DEBUG: PSP next_seg = %04X\n", size);
		return true;
	}

	if (command == "IV") { // Insert variable
		uint16_t seg = (uint16_t)GetHexValue(found,found); found++;
		uint32_t ofs = (uint16_t)GetHexValue(found,found); found++;
		char name[16];
		for (int i=0; i<16; i++) {
			if (found[i] && (found[i]!=' ')) name[i] = found[i];
			else { name[i] = 0; break; }
		}
		name[15] = 0;

		if(!name[0]) return false;
		DEBUG_ShowMsg("DEBUG: Created debug var %s at %04X:%04X\n",name,seg,ofs);
		CDebugVar::InsertVariable(name,GetAddress(seg,ofs));
		return true;
	}

	if (command == "SV") { // Save variables
		char name[13];
		for (int i=0; i<12; i++) {
			if (found[i] && (found[i]!=' ')) name[i] = found[i];
			else { name[i] = 0; break; }
		}
		name[12] = 0;
		if(!name[0]) return false;
		DEBUG_ShowMsg("DEBUG: Variable list save (%s) : %s.\n",name,(CDebugVar::SaveVars(name)?"ok":"failure"));
		return true;
	}

	if (command == "LV") { // load variables
		char name[13];
		for (int i=0; i<12; i++) {
			if (found[i] && (found[i]!=' ')) name[i] = found[i];
			else { name[i] = 0; break; }
		}
		name[12] = 0;
		if(!name[0]) return false;
		DEBUG_ShowMsg("DEBUG: Variable list load (%s) : %s.\n",name,(CDebugVar::LoadVars(name)?"ok":"failure"));
		return true;
	}

	if (command == "ADDLOG") {
		if(found && *found)	DEBUG_ShowMsg("NOTICE: %s\n",found);
		return true;
	}

	if (command == "SR") { // Set register value
		DEBUG_ShowMsg("DEBUG: Set Register %s.\n",(ChangeRegister(found)?"success":"failure"));
		return true;
	}

	if (command == "ADDR") {
		uint16_t seg = (uint16_t)GetHexValue(found, found);
		found++;
		uint32_t ofs = GetHexValue(found, found);
		found++;
		uint32_t address = GetAddress(seg, ofs);
		DEBUG_ShowMsg("DEBUG: Address if %04X:%04X is %08X\n", seg, ofs, address);
	}

	if (command == "SM") { // Set memory with following values
		uint16_t seg = (uint16_t)GetHexValue(found,found); found++;
		uint32_t ofs = GetHexValue(found,found); found++;
		uint16_t count = 0;
		while (*found) {
			while (*found==' ') found++;
			if (*found) {
				uint8_t value = (uint8_t)GetHexValue(found,found);
				if(*found) found++;
				mem_writeb_checked(GetAddress(seg,ofs+count),value);
				count++;
			}
		}
		DEBUG_ShowMsg("DEBUG: Memory changed.\n");
		return true;
	}

	if (command == "BP") { // Add new breakpoint
		uint16_t seg = (uint16_t)GetHexValue(found,found);found++; // skip ":"
		uint32_t ofs = GetHexValue(found,found);
		CBreakpoint::AddBreakpoint(seg,ofs,false);
		DEBUG_ShowMsg("DEBUG: Set breakpoint at %04X:%04X\n",seg,ofs);
		return true;
	}

	if (command == "BPREG") { // Add new breakpoint for register change
		// Parse the name of the register
		// TODO: Handle also the smaller parts of the registers like AH and AL
		uint8_t reg = 0;
		while (*found == ' ')
			found++;
		if (strncmp(found, "EAX", 3) == 0) {
			reg = REGI_AX;
		} else if (strncmp(found, "EBX", 3) == 0) {
			reg = REGI_BX;
		} else if (strncmp(found, "ECX", 3) == 0) {
			reg = REGI_CX;
		} else if (strncmp(found, "EDX", 3) == 0) {
			reg = REGI_DX;
		} else if (strncmp(found, "ESI", 3) == 0) {
			reg = REGI_SI;
		} else if (strncmp(found, "EDI", 3) == 0) {
			reg = REGI_DI;
		} else if (strncmp(found, "EBP", 3) == 0) {
			reg = REGI_BP;
		} else if (strncmp(found, "ESP", 3) == 0) {
			reg = REGI_SP;
		// TODO: Why is there no enum for this one?
		// } else if (strncmp(found, "EIP", 3) == 0) {
		// 	reg = REGI_IP;
		};
		CBreakpoint::AddRegBreakpoint(reg);
		return true;
	}

#if C_HEAVY_DEBUG

	if (command == "BPM") { // Add new breakpoint
		uint16_t seg = (uint16_t)GetHexValue(found,found);found++; // skip ":"
		uint32_t ofs = GetHexValue(found,found);
		CBreakpoint::AddMemBreakpoint(seg,ofs);
		DEBUG_ShowMsg("DEBUG: Set memory breakpoint at %04X:%04X\n",seg,ofs);
		return true;
	}

	if (command == "REBP") { // Add a regular expression breakpoint
		char source[51];
		for (int i = 0; i < 50; i++) {
			if (found[i] && (found[i] != ' ')) source[i] = found[i];
			else { source[i] = 0; break; }
		}
		source[50] = 0;

		if (source[0] == 0) {
			delete self_regex;
			self_regex = nullptr;
		}
		else {
			self_regex = new std::regex(source, std::regex_constants::ECMAScript | std::regex_constants::icase);
		}
		return true;
	}

	if (command == "BPMR1") { // Update the memory read breakpoint address 1
		uint16_t seg = (uint16_t)GetHexValue(found, found);
		found++; // skip ":"
		uint32_t ofs = GetHexValue(found, found);

		memReadWatch1 = GetAddress(seg, ofs);
		DEBUG_ShowMsg("DEBUG: Set memory read watch address 1 to %04X:%04X\n",
		              seg,
		              ofs);
		return true;
	}
	if (command == "BPMR2") { // Update the memory read breakpoint address 1
		uint16_t seg = (uint16_t)GetHexValue(found, found);
		found++; // skip ":"
		uint32_t ofs = GetHexValue(found, found);

		memReadWatch2 = GetAddress(seg, ofs);
		DEBUG_ShowMsg("DEBUG: Set memory read watch address 2 to %04X:%04X\n",
		              seg,
		              ofs);
		return true;
	}
	if (command == "BPMRO") { // Update the memory read override address
		uint16_t seg = (uint16_t)GetHexValue(found, found);
		found++; // skip ":"
		uint32_t ofs = GetHexValue(found, found);
		found++;
		uint32_t value = GetHexValue(found, found);

		memReadOverride = GetAddress(seg, ofs);
		memReadOverrideValue = value;
		DEBUG_ShowMsg("DEBUG: Set memory read override address to %04X:%04X, returning value %04X\n",
			seg,
			ofs,
			value);
		return true;
	}

	if (command == "CLS") {
		system("CLS");
		return true;
	}

	if (command == "CHANNEL") {
		while (*found == ' ') {
			found++;
		}
		

		if (channelIDNames.count(found) == 1) {
			SIS_ChannelID foundID     = channelIDNames[found];
			debugLogEnabledID[foundID] = !debugLogEnabledID[foundID];
			DEBUG_ShowMsg("DEBUG: Setting debug channel %s to %u\n",
			              found,
			              debugLogEnabledID[foundID]);
		} else {
			DEBUG_ShowMsg("Unknown debug log channel %s\n", found);
		}
		return true;
	}

	if (command == "CHANNELLIST") {
		DEBUG_ShowMsg("DEBUG: Log channels:\n");
		for (auto it = channelIDNames.begin(); it != channelIDNames.end(); it++) {
			std::string currentName = it->first;
			SIS_ChannelID currentID = it->second;
			DEBUG_ShowMsg("%s: %u\n",
			              currentName.c_str(),
			              debugLogEnabledID[currentID]);
		}
		return true;
	}

	

	if (command == "BPMC") { // Add a new conditional memory breakpoint
		uint16_t seg = (uint16_t)GetHexValue(found, found);
		found++; // skip ":"
		uint32_t ofs = GetHexValue(found, found);
		// TODO: Which data type would this actually have?
		found++;
		uint8_t condition = (uint16_t)GetHexValue(found, found);
		found++; // skip a whitespaceyu
		CBreakpoint::AddConditionalMemBreakpoint(seg, ofs, condition);
		DEBUG_ShowMsg("DEBUG: Set conditional memory breakpoint at %04X:%04X with target value %02X\n", seg, ofs, condition);
		return true;
	}
	if (command == "DB") {
		// Copy the pixels from the back buffer to the buffer
		// MEM_BlockCopy(GetAddress(0xA000, 0x0000), GetAddress(0x025F, 0x0000), 320 * 200);
		uint16_t offset = 0x0000;
		const uint16_t stride = 0x1e00 + 0x0140;
		DrawBackBuffer(0x025F, offset, stride); offset += stride;
		DrawBackBuffer(0x0267, offset, stride); offset += stride;
		DrawBackBuffer(0x026F, offset, stride); offset += stride;
		DrawBackBuffer(0x0277, offset, stride); offset += stride;
		DrawBackBuffer(0x027F, offset, stride); offset += stride;
		DrawBackBuffer(0x0287, offset, stride); offset += stride;
		DrawBackBuffer(0x028F, offset, stride); offset += stride;
		DrawBackBuffer(0x0297, offset, stride); offset += stride;

		/* MEM_BlockCopy(GetAddress(0xA000, offset), GetAddress(0x025F, 0xc), stride);
		offset += stride;
		MEM_BlockCopy(GetAddress(0xA000, offset), GetAddress(0x0267, 0xc), stride);
		offset += stride;
		MEM_BlockCopy(GetAddress(0xA000, offset), GetAddress(0x026F, 0xc), stride);
		offset += stride;
		MEM_BlockCopy(GetAddress(0xA000, offset), GetAddress(0x0277, 0xc), stride);
		offset += stride;
		MEM_BlockCopy(GetAddress(0xA000, offset), GetAddress(0x027F, 0xc), stride);
		offset += stride;
		MEM_BlockCopy(GetAddress(0xA000, offset), GetAddress(0x0287, 0xc), stride);
		offset += stride;
		MEM_BlockCopy(GetAddress(0xA000, offset), GetAddress(0x028F, 0xc), stride);
		offset += stride;
		MEM_BlockCopy(GetAddress(0xA000, offset), GetAddress(0x0297, 0xc), stride);
		offset += stride; */
		
		// Draw the image ourselves in the window
		const auto graphics_window = GFX_GetSDLWindow();
		const auto graphics_surface = SDL_GetWindowSurface(graphics_window);
		const uint16_t x = 50;
		const uint16_t y = 50;
		Uint32* const target_pixel = (Uint32*)((Uint8*)graphics_surface->pixels
			+ y * graphics_surface->pitch
			+ x * graphics_surface->format->BytesPerPixel);
		*target_pixel = 0x00FF0000;




		// Update the surface
		SDL_UpdateWindowSurface(graphics_window);

		// Focus
		SDL_RaiseWindow(graphics_window);

		return true;
	}
	if (command == "DI") {

		uint16_t img_width;
		uint16_t img_height;
		uint8_t* pixels;
		uint16_t seg = (uint16_t)GetHexValue(found, found);
		found++; // skip ":"
		uint32_t ofs = GetHexValue(found, found);

		SIS_ReadImageToPixels(seg, ofs, img_width, img_height, pixels);
		SIS_CopyImageToClipboard(img_width, img_height, pixels);
		delete[] pixels;
		
		return true;
	}

	if (command == "DP") {
		SIS_DumpPalette();
		return true;
	}

	if (command == "BPMOUSE") { // Hacky "mouse press read memory" breakpoint
		mouseBreakpoint = true;
		mouseBreakpointHit = false;
		return true;
	}

	if (command == "BPMW") { // Set a breakpoint on memory writes outside of the stack
		outsideStackWriteBreakpoint = !outsideStackWriteBreakpoint;
		outsideStackWriteBreakpointHit = false;
		memwrites.clear();
		return true;
	}

	if (command == "DUMPMW") { // Dump the memory writes
		char name[13];
		for (int i = 0; i < 12; i++) {
			if (found[i] && (found[i] != ' '))
				name[i] = found[i];
			else {
				name[i] = 0;
				break;
			}
		}
		name[12] = 0;
		if (!name[0])
			return false;

		SaveMemWrites(name);

		// Write the file
		return true;
	}

	if (command == "CT") { // Toggle usage of the call stack/trace functionality
		useCallstack = !useCallstack;
		if (useCallstack) {
			callstack.clear();
			calltrace.clear();
		}
		filterCallstack = false;
		DEBUG_ShowMsg("DEBUG: Toggling callstack usage\n");
		return true;
	}

	if (command == "SKIP") { // Toggle skipping a part of a script
		uint16_t start = (uint16_t)GetHexValue(found, found);
		// TODO: Skip all whitespace instead
		found++; // skip a space
		uint16_t end = GetHexValue(found, found);

		script_off_skip_start = start;
		script_off_skip_end = end;
		return true;
	}

	if (command == "CTF") { // Toggle usage of the call stack/trace functionality
		useCallstack    = !useCallstack;
		if (useCallstack) {
			callstack.clear();
			calltrace.clear();
		}
		filterCallstack = true;
		DEBUG_ShowMsg("DEBUG: Toggling filtered callstack usage\n");
		return true;
	}


	if (command == "BPR") { // Add a new "return" breakpoint
		CBreakpoint::AddReturnBreakpoint();
		// TODO: Also add to bplist

		return true;	
	}

	if (command == "ST") { // Dump the stack trace
		DEBUG_ShowMsg("Call Stack:\n");
		for (auto& current : callstack) {
			
			DEBUG_ShowMsg("%04X:%04X --> %04X:%04X\n",
			              current.caller_seg,
			              current.caller_off,
			              current.callee_seg,
			              current.callee_off);
		}
		return true;
	}

	if (command == "DUMPCT") { // Dump the call trace
		// TODO: Grab the filename
		// Call the dump function

		char name[13];
		for (int i = 0; i < 12; i++) {
			if (found[i] && (found[i] != ' '))
				name[i] = found[i];
			else {
				name[i] = 0;
				break;
			}
		}
		name[12] = 0;
		if (!name[0])
			return false;

		SaveCalltrace(name);
		return true;
	}

	if (command == "BPPIXEL") { // Add new breakpoint
		uint16_t x = (uint16_t)GetHexValue(found, found);
		// TODO: Skip all whitespace instead
		found++; // skip ":"
		uint16_t y = GetHexValue(found, found);
		CBreakpoint::AddPixelBreakpoint(x, y);
		DEBUG_ShowMsg("DEBUG: Set pixel breakpoint at %04X:%04X\n", x, y);
		return true;
	}
	if (command == "BPPIXELBB") { // Handle a back buffer pixel write
		uint16_t x = (uint16_t)GetHexValue(found, found);
		// TODO: Skip all whitespace instead
		found++; // skip ":"
		uint16_t y = GetHexValue(found, found);
		CBreakpoint::AddBackBufferPixelBreakpoint(x, y);
		DEBUG_ShowMsg("DEBUG: Set back buffer pixel breakpoint at %04X:%04X\n", x, y);
		return true;
	}

	if (command == "BPPM") { // Add new breakpoint
		uint16_t seg = (uint16_t)GetHexValue(found,found);found++; // skip ":"
		uint32_t ofs = GetHexValue(found,found);
		CBreakpoint* bp = CBreakpoint::AddMemBreakpoint(seg,ofs);
		if (bp)	{
			bp->SetType(BKPNT_MEMORY_PROT);
			DEBUG_ShowMsg("DEBUG: Set prot-mode memory breakpoint at %04X:%08X\n",seg,ofs);
		}
		return true;
	}

	if (command == "BPLM") { // Add new breakpoint
		uint32_t ofs = GetHexValue(found,found);
		CBreakpoint* bp = CBreakpoint::AddMemBreakpoint(0,ofs);
		if (bp) bp->SetType(BKPNT_MEMORY_LINEAR);
		DEBUG_ShowMsg("DEBUG: Set linear memory breakpoint at %08X\n",ofs);
		return true;
	}

#endif

	if (command == "BPINT") { // Add Interrupt Breakpoint
		uint8_t intNr	= (uint8_t)GetHexValue(found,found);
		bool all = !(*found);
		uint8_t valAH = (uint8_t)GetHexValue(found,found);
		if ((valAH==0x00) && (*found=='*' || all)) {
			CBreakpoint::AddIntBreakpoint(intNr,BPINT_ALL,BPINT_ALL,false);
			DEBUG_ShowMsg("DEBUG: Set interrupt breakpoint at INT %02X\n",intNr);
		} else {
			all = !(*found);
			uint8_t valAL = (uint8_t)GetHexValue(found,found);
			if ((valAL==0x00) && (*found=='*' || all)) {
				CBreakpoint::AddIntBreakpoint(intNr,valAH,BPINT_ALL,false);
				DEBUG_ShowMsg("DEBUG: Set interrupt breakpoint at INT %02X AH=%02X\n",intNr,valAH);
			} else {
				CBreakpoint::AddIntBreakpoint(intNr,valAH,valAL,false);
				DEBUG_ShowMsg("DEBUG: Set interrupt breakpoint at INT %02X AH=%02X AL=%02X\n",intNr,valAH,valAL);
			}
		}
		return true;
	}

	if (command == "BPLIST") {
		DEBUG_ShowMsg("Breakpoint list:\n");
		DEBUG_ShowMsg("-------------------------------------------------------------------------\n");
		CBreakpoint::ShowList();
		return true;
	}

	if (command == "BPDEL") { // Delete Breakpoints
		uint8_t bpNr	= (uint8_t)GetHexValue(found,found);
		if ((bpNr==0x00) && (*found=='*')) { // Delete all
			CBreakpoint::DeleteAll();
			DEBUG_ShowMsg("DEBUG: Breakpoints deleted.\n");
		} else {
			// delete single breakpoint
			DEBUG_ShowMsg("DEBUG: Breakpoint deletion %s.\n",(CBreakpoint::DeleteByIndex(bpNr)?"success":"failure"));
		}
		return true;
	}

	if (command == "C") { // Set code overview
		uint16_t codeSeg = (uint16_t)GetHexValue(found,found); found++;
		uint32_t codeOfs = GetHexValue(found,found);
		DEBUG_ShowMsg("DEBUG: Set code overview to %04X:%04X\n",codeSeg,codeOfs);
		codeViewData.useCS	= codeSeg;
		codeViewData.useEIP = codeOfs;
		codeViewData.cursorPos = 0;
		return true;
	}

	if (command == "D") { // Set data overview
		dataSeg = (uint16_t)GetHexValue(found,found); found++;
		dataOfs = GetHexValue(found,found);
		DEBUG_ShowMsg("DEBUG: Set data overview to %04X:%04X\n",dataSeg,dataOfs);
		return true;
	}

#if C_HEAVY_DEBUG

	if (command == "LOG") { // Create Cpu normal log file
		cpuLogType = 1;
		command = "logcode";
	}

	if (command == "LOGS") { // Create Cpu short log file
		cpuLogType = 0;
		command = "logcode";
	}

	if (command == "LOGL") { // Create Cpu long log file
		cpuLogType = 2;
		command = "logcode";
	}

	if (command == "LOGC") { // Create Cpu coverage log file
		cpuLogType = 3;
		command = "logcode";
	}

	if (command == "logcode") { //Shared code between all logs
		DEBUG_ShowMsg("DEBUG: Starting log\n");
		const std_fs::path log_cpu_txt = "LOGCPU.TXT";
		cpuLogFile.open(log_cpu_txt.string());
		if (!cpuLogFile.is_open()) {
			DEBUG_ShowMsg("DEBUG: Logfile couldn't be created.\n");
			return false;
		}
		DEBUG_ShowMsg("DEBUG: Logfile '%s' created.\n",
		              std_fs::absolute(log_cpu_txt).string().c_str());
		//Initialize log object
		cpuLogFile << hex << noshowbase << setfill('0') << uppercase;
		cpuLog = true;
		cpuLogCounter = GetHexValue(found,found);

		debugging = false;
		CBreakpoint::ActivateBreakpointsExceptAt(SegPhys(cs)+reg_eip);
		DOSBOX_SetNormalLoop();	
		return true;
	}

#endif

	if (command == "INTT") { //trace int.
		uint8_t intNr = (uint8_t)GetHexValue(found,found);
		DEBUG_ShowMsg("DEBUG: Tracing INT %02X\n",intNr);
		CPU_HW_Interrupt(intNr);
		SetCodeWinStart();
		return true;
	}

	if (command == "INT") { // start int.
		uint8_t intNr = (uint8_t)GetHexValue(found,found);
		DEBUG_ShowMsg("DEBUG: Starting INT %02X\n",intNr);
		CBreakpoint::AddBreakpoint(SegValue(cs),reg_eip, true);
		CBreakpoint::ActivateBreakpointsExceptAt(SegPhys(cs)+reg_eip-1);
		debugging = false;
		DrawCode();
		DOSBOX_SetNormalLoop();
		CPU_HW_Interrupt(intNr);
		return true;
	}

	if (command == "SELINFO") {
		while (found[0] == ' ') found++;
		char out1[200],out2[200];
		GetDescriptorInfo(found,out1,out2);
		DEBUG_ShowMsg("SelectorInfo %s:\n%s\n%s\n",found,out1,out2);
		return true;
	}

	if (command == "DOS") {
		stream >> command;
		if (command == "MCBS") LogMCBS();
		return true;
	}

	if (command == "GDT") {LogGDT(); return true;}

	if (command == "LDT") {LogLDT(); return true;}

	if (command == "IDT") {LogIDT(); return true;}

	if (command == "PAGING") {LogPages(found); return true;}

	if (command == "CPU") {LogCPUInfo(); return true;}

	if (command == "INTVEC") {
		if (found[0] != 0) {
			OutputVecTable(found);
			return true;
		}
	}

	if (command == "INTHAND") {
		if (found[0] != 0) {
			uint8_t intNr = (uint8_t)GetHexValue(found,found);
			DEBUG_ShowMsg("DEBUG: Set code overview to interrupt handler %X\n",intNr);
			codeViewData.useCS	= mem_readw(intNr*4+2);
			codeViewData.useEIP = mem_readw(intNr*4);
			codeViewData.cursorPos = 0;
			return true;
		}
	}

	if(command == "EXTEND") { //Toggle additional data.
		showExtend = !showExtend;
		return true;
	}

	if(command == "TIMERIRQ") { //Start a timer irq
		DEBUG_RaiseTimerIrq(); 
		DEBUG_ShowMsg("Debug: Timer Int started.\n");
		return true;
	}


#if C_HEAVY_DEBUG
	if (command == "HEAVYLOG") { // Create Cpu log file
		logHeavy = !logHeavy;
		DEBUG_ShowMsg("DEBUG: Heavy cpu logging %s.\n",logHeavy?"on":"off");
		return true;
	}

	if (command == "ZEROPROTECT") { //toggle zero protection
		zeroProtect = !zeroProtect;
		DEBUG_ShowMsg("DEBUG: Zero code execution protection %s.\n",zeroProtect?"on":"off");
		return true;
	}

#endif
	if (command == "HELP" || command == "?") {
		DEBUG_ShowMsg("Debugger commands (enter all values in hex or as register):\n");
		DEBUG_ShowMsg("Commands------------------------------------------------\n");
		DEBUG_ShowMsg("BP     [segment]:[offset] - Set breakpoint.\n");
		DEBUG_ShowMsg("BPINT  [intNr] *          - Set interrupt breakpoint.\n");
		DEBUG_ShowMsg("BPINT  [intNr] [ah] *     - Set interrupt breakpoint with ah.\n");
		DEBUG_ShowMsg("BPINT  [intNr] [ah] [al]  - Set interrupt breakpoint with ah and al.\n");
#if C_HEAVY_DEBUG
		DEBUG_ShowMsg("BPM    [segment]:[offset] - Set memory breakpoint (memory change).\n");
		DEBUG_ShowMsg("BPPM   [selector]:[offset]- Set pmode-memory breakpoint (memory change).\n");
		DEBUG_ShowMsg("BPLM   [linear address]   - Set linear memory breakpoint (memory change).\n");
#endif
		DEBUG_ShowMsg("BPLIST                    - List breakpoints.\n");
		DEBUG_ShowMsg("BPDEL  [bpNr] / *         - Delete breakpoint nr / all.\n");
		DEBUG_ShowMsg("C / D  [segment]:[offset] - Set code / data view address.\n");
		DEBUG_ShowMsg("DOS MCBS                  - Show Memory Control Block chain.\n");
		DEBUG_ShowMsg("INT [nr] / INTT [nr]      - Execute / Trace into interrupt.\n");
#if C_HEAVY_DEBUG
		DEBUG_ShowMsg("LOG [num]                 - Write cpu log file.\n");
		DEBUG_ShowMsg("LOGS/LOGL/LOGC [num]      - Write short/long/cs:ip-only cpu log file.\n");
		DEBUG_ShowMsg("HEAVYLOG                  - Enable/Disable automatic cpu log when DOSBox exits.\n");
		DEBUG_ShowMsg("ZEROPROTECT               - Enable/Disable zero code execution detection.\n");
#endif
		DEBUG_ShowMsg("SR [reg] [value]          - Set register value.\n");
		DEBUG_ShowMsg("SM [seg]:[off] [val] [.]..- Set memory with following values.\n");

		DEBUG_ShowMsg("IV [seg]:[off] [name]     - Create var name for memory address.\n");
		DEBUG_ShowMsg("SV [filename]             - Save var list in file.\n");
		DEBUG_ShowMsg("LV [filename]             - Load var list from file.\n");

		DEBUG_ShowMsg("ADDLOG [message]          - Add message to the log file.\n");

		DEBUG_ShowMsg("MEMDUMP [seg]:[off] [len] - Write memory to file memdump.txt.\n");
		DEBUG_ShowMsg("MEMDUMPBIN [s]:[o] [len]  - Write memory to file memdump.bin.\n");
		DEBUG_ShowMsg("SELINFO [segName]         - Show selector info.\n");

		DEBUG_ShowMsg("INTVEC [filename]         - Writes interrupt vector table to file.\n");
		DEBUG_ShowMsg("INTHAND [intNum]          - Set code view to interrupt handler.\n");

		DEBUG_ShowMsg("CPU                       - Display CPU status information.\n");
		DEBUG_ShowMsg("GDT                       - Lists descriptors of the GDT.\n");
		DEBUG_ShowMsg("LDT                       - Lists descriptors of the LDT.\n");
		DEBUG_ShowMsg("IDT                       - Lists descriptors of the IDT.\n");
		DEBUG_ShowMsg("PAGING [page]             - Display content of page table.\n");
		DEBUG_ShowMsg("EXTEND                    - Toggle additional info.\n");
		DEBUG_ShowMsg("TIMERIRQ                  - Run the system timer.\n");

		DEBUG_ShowMsg("HELP                      - Help\n");
		DEBUG_ShowMsg("Keys------------------------------------------------\n");
		DEBUG_ShowMsg("F3/F6                     - Previous command in history.\n");
		DEBUG_ShowMsg("F4/F7                     - Next command in history.\n");
		DEBUG_ShowMsg("F5                        - Run.\n");
		DEBUG_ShowMsg("F8                        - Toggle printable characters.\n");
		DEBUG_ShowMsg("F9                        - Set/Remove breakpoint.\n");
		DEBUG_ShowMsg("F10/F11                   - Step over / trace into instruction.\n");
		DEBUG_ShowMsg("ALT + D/E/S/X/B           - Set data view to DS:SI/ES:DI/SS:SP/DS:DX/ES:BX.\n");
		DEBUG_ShowMsg("Escape                    - Clear input line.");
		DEBUG_ShowMsg("Up/Down                   - Move code view cursor.\n");
		DEBUG_ShowMsg("Page Up/Down              - Scroll data view.\n");
		DEBUG_ShowMsg("Home/End                  - Scroll log messages.\n");

		return true;
	}

	return SIS_ParseCommand(found, command);
}

char* AnalyzeInstruction(char* inst, bool saveSelector) {
	static char result[256];

	char instu[256];
	char prefix[3];
	uint16_t seg;

	strcpy(instu,inst);
	upcase(instu);

	result[0] = 0;
	char* pos = strchr(instu,'[');
	if (pos) {
		// Segment prefix ?
		if (*(pos-1)==':') {
			char* segpos = pos-3;
			prefix[0] = tolower(*segpos);
			prefix[1] = tolower(*(segpos+1));
			prefix[2] = 0;
			seg = (uint16_t)GetHexValue(segpos,segpos);
		} else {
			if (strstr(pos,"SP") || strstr(pos,"BP")) {
				seg = SegValue(ss);
				strcpy(prefix,"ss");
			} else {
				seg = SegValue(ds);
				strcpy(prefix,"ds");
			}
		}

		pos++;
		uint32_t adr = GetHexValue(pos,pos);
		while (*pos!=']') {
			if (*pos=='+') {
				pos++;
				adr += GetHexValue(pos,pos);
			} else if (*pos=='-') {
				pos++;
				adr -= GetHexValue(pos,pos);
			} else
				pos++;
		}
		uint32_t address = GetAddress(seg,adr);
		if (!(get_tlb_readhandler(address)->flags & PFLAG_INIT)) {
			static char outmask[] = "%s:[%04X]=%02X";

			if (cpu.pmode) outmask[6] = '8';
				switch (DasmLastOperandSize()) {
				case 8 : {	uint8_t val = mem_readb(address);
							outmask[12] = '2';
							sprintf(result,outmask,prefix,adr,val);
						}	break;
				case 16: {	uint16_t val = mem_readw(address);
							outmask[12] = '4';
							sprintf(result,outmask,prefix,adr,val);
						}	break;
				case 32: {	uint32_t val = mem_readd(address);
							outmask[12] = '8';
							sprintf(result,outmask,prefix,adr,val);
						}	break;
			}
		} else {
			sprintf(result,"[illegal]");
		}
		// Variable found ?
		CDebugVar* var = CDebugVar::FindVar(address);
		if (var) {
			// Replace occurrence
			char* pos1 = strchr(inst,'[');
			char* pos2 = strchr(inst,']');
			if (pos1 && pos2) {
				char temp[256];
				strcpy(temp,pos2);				// save end
				pos1++; *pos1 = 0;				// cut after '['
				strcat(inst,var->GetName());	// add var name
				strcat(inst,temp);				// add end
			}
		}
		// show descriptor info, if available
		if ((cpu.pmode) && saveSelector) {
			strcpy(curSelectorName,prefix);
		}
	}
	// If it is a callback add additional info
	pos = strstr(inst,"callback");
	if (pos) {
		pos += 9;
		Bitu nr = GetHexValue(pos,pos);
		const char* descr = CALLBACK_GetDescription(nr);
		if (descr) {
			strcat(inst,"  ("); strcat(inst,descr); strcat(inst,")");
		}
	}
	// Must be a jump
	if (instu[0] == 'J')
	{
		bool jmp = false;
		switch (instu[1]) {
		case 'A' :	{	jmp = (get_CF()?false:true) && (get_ZF()?false:true); // JA
					}	break;
		case 'B' :	{	if (instu[2] == 'E') {
							jmp = (get_CF()?true:false) || (get_ZF()?true:false); // JBE
						} else {
							jmp = get_CF()?true:false; // JB
						}
					}	break;
		case 'C' :	{	if (instu[2] == 'X') {
							jmp = reg_cx == 0; // JCXZ
						} else {
							jmp = get_CF()?true:false; // JC
						}
					}	break;
		case 'E' :	{	jmp = get_ZF()?true:false; // JE
					}	break;
		case 'G' :	{	if (instu[2] == 'E') {
							jmp = (get_SF()?true:false)==(get_OF()?true:false); // JGE
						} else {
							jmp = (get_ZF()?false:true) && ((get_SF()?true:false)==(get_OF()?true:false)); // JG
						}
					}	break;
		case 'L' :	{	if (instu[2] == 'E') {
							jmp = (get_ZF()?true:false) || ((get_SF()?true:false)!=(get_OF()?true:false)); // JLE
						} else {
							jmp = (get_SF()?true:false)!=(get_OF()?true:false); // JL
						}
					}	break;
		case 'M' :	{	jmp = true; // JMP
					}	break;
		case 'N' :	{	switch (instu[2]) {
						case 'B' :	
						case 'C' :	{	jmp = get_CF()?false:true;	// JNB / JNC
									}	break;
						case 'E' :	{	jmp = get_ZF()?false:true;	// JNE
									}	break;
						case 'O' :	{	jmp = get_OF()?false:true;	// JNO
									}	break;
						case 'P' :	{	jmp = get_PF()?false:true;	// JNP
									}	break;
						case 'S' :	{	jmp = get_SF()?false:true;	// JNS
									}	break;
						case 'Z' :	{	jmp = get_ZF()?false:true;	// JNZ
									}	break;
						}
					}	break;
		case 'O' :	{	jmp = get_OF()?true:false; // JO
					}	break;
		case 'P' :	{	if (instu[2] == 'O') {
							jmp = get_PF()?false:true; // JPO
						} else {
							jmp = get_SF()?true:false; // JP / JPE
						}
					}	break;
		case 'S' :	{	jmp = get_SF()?true:false; // JS
					}	break;
		case 'Z' :	{	jmp = get_ZF()?true:false; // JZ
					}	break;
		}
		if (jmp) {
			pos = strchr(instu,'$');
			if (pos) {
				pos = strchr(instu,'+');
				if (pos) {
					strcpy(result,"(down)");
				} else {
					strcpy(result,"(up)");
				}
			}
		} else {
			sprintf(result,"(no jmp)");
		}
	}
	return result;
}


int32_t DEBUG_Run(int32_t amount,bool quickexit) {
	skipFirstInstruction = true;
	CPU_Cycles = amount;
	int32_t ret = (*cpudecoder)();
	if (quickexit) SetCodeWinStart();
	else {
		// ensure all breakpoints are activated
		CBreakpoint::ActivateBreakpoints();

		const auto graphics_window = GFX_GetSDLWindow();
		SDL_RaiseWindow(graphics_window);
		SDL_SetWindowInputFocus(graphics_window);

		DOSBOX_SetNormalLoop();
	}
	return ret;
}



uint32_t DEBUG_CheckKeys(void) {
	Bits ret=0;
	bool numberrun = false;
	bool skipDraw = false;
	int key=getch();

	if (key >='1' && key <='5' && safe_strlen(codeViewData.inputStr) == 0) {
		const int32_t v[] ={5,500,1000,5000,10000};

		ret = DEBUG_Run(v[key - '1'],true);

		/* Setup variables so we end up at the proper ret processing */
		numberrun = true;

		// Don't redraw the screen if it's going to get redrawn immediately
		// afterwards, to avoid resetting oldregs.
		if (ret == static_cast<Bits>(debugCallback))
			skipDraw = true;
		key = -1;
	}

	if (key>0 || numberrun) {
#if defined(WIN32) && PDCURSES
		switch (key) {
		case PADENTER:	key=0x0A;	break;
		case PADSLASH:	key='/';	break;
		case PADSTAR:	key='*';	break;
		case PADMINUS:	key='-';	break;
		case PADPLUS:	key='+';	break;
		case PADSTOP:	key=KEY_DC;		break;
		case PAD0:		key=KEY_IC;		break;
		case KEY_A1:	key=KEY_HOME;	break;
		case KEY_A2:	key=KEY_UP;		break;
		case KEY_A3:	key=KEY_PPAGE;	break;
		case KEY_B1:	key=KEY_LEFT;	break;
		case KEY_B3:	key=KEY_RIGHT;	break;
		case KEY_C1:	key=KEY_END;	break;
		case KEY_C2:	key=KEY_DOWN;	break;
		case KEY_C3:	key=KEY_NPAGE;	break;
		case ALT_D:
			if (ungetch('D') != ERR) key=27;
			break;
		case ALT_E:
			if (ungetch('E') != ERR) key=27;
			break;
		case ALT_X:
			if (ungetch('X') != ERR) key=27;
			break;
		case ALT_B:
			if (ungetch('B') != ERR) key=27;
			break;
		case ALT_S:
			if (ungetch('S') != ERR) key=27;
			break;
		}
#endif
		switch (toupper(key)) {
		// Temp code for seeing the memory at the source
		case 'Y':
			dataSeg = SegValue(ds);
			if (cpu.pmode && !(reg_flags & FLAG_VM))
				dataOfs = reg_esi;
			else
				dataOfs = reg_si;
			break;
		case 27:	// escape (a bit slow): Clears line. and processes alt commands.
			key=getch();
			if(key < 0) { //Purely escape Clear line
				ClearInputLine();
				break;
			}

			switch(toupper(key)) {
			case 'D' : // ALT - D: DS:SI
				dataSeg = SegValue(ds);
				if (cpu.pmode && !(reg_flags & FLAG_VM)) dataOfs = reg_esi;
				else dataOfs = reg_si;
				break;
			case 'E' : //ALT - E: es:di
				dataSeg = SegValue(es);
				if (cpu.pmode && !(reg_flags & FLAG_VM)) dataOfs = reg_edi;
				else dataOfs = reg_di;
				break;
			case 'X': //ALT - X: ds:dx
				dataSeg = SegValue(ds);
				if (cpu.pmode && !(reg_flags & FLAG_VM)) dataOfs = reg_edx;
				else dataOfs = reg_dx;
				break;
			case 'B' : //ALT -B: es:bx
				dataSeg = SegValue(es);
				if (cpu.pmode && !(reg_flags & FLAG_VM)) dataOfs = reg_ebx;
				else dataOfs = reg_bx;
				break;
			case 'S': //ALT - S: ss:sp
				dataSeg = SegValue(ss);
				if (cpu.pmode && !(reg_flags & FLAG_VM)) dataOfs = reg_esp;
				else dataOfs = reg_sp;
				break;
			default:
				break;
			}
			break;
		case KEY_PPAGE :	dataOfs -= 16;	break;
		case KEY_NPAGE :	dataOfs += 16;	break;

		case KEY_DOWN:	// down 
				if (codeViewData.cursorPos<9) codeViewData.cursorPos++;
				else codeViewData.useEIP += codeViewData.firstInstSize;
				break;
		case KEY_UP:	// up 
				if (codeViewData.cursorPos>0) codeViewData.cursorPos--;
				else {
					Bitu bytes = 0;
					char dline[200];
					Bitu size = 0;
					uint32_t newEIP = codeViewData.useEIP - 1;
					if(codeViewData.useEIP) {
						for (; bytes < 10; bytes++) {
							PhysPt start = GetAddress(codeViewData.useCS,newEIP);
							size = DasmI386(dline, start, newEIP, cpu.code.big);
							if(codeViewData.useEIP == newEIP+size) break;
							newEIP--;
						}
						if (bytes>=10) newEIP = codeViewData.useEIP - 1;
					}
					codeViewData.useEIP = newEIP;
				}
				break;
		case KEY_HOME:	// Home: scroll log page up
				DEBUG_RefreshPage(-1);
				break;
		case KEY_END:	// End: scroll log page down
				DEBUG_RefreshPage(1);
				break;
		case KEY_IC:	// Insert: toggle insert/overwrite
				codeViewData.ovrMode = !codeViewData.ovrMode;
				break;
		case KEY_LEFT:	// move to the left in command line
				if (codeViewData.inputPos > 0) codeViewData.inputPos--;
 				break;
		case KEY_RIGHT:	// move to the right in command line
				if (codeViewData.inputStr[codeViewData.inputPos]) codeViewData.inputPos++;
				break;
		case KEY_F(6):	// previous command (f1-f4 generate rubbish at my place)
		case KEY_F(3):	// previous command
				if (histBuffPos == histBuff.begin()) break;
				if (histBuffPos == histBuff.end()) {
					// copy inputStr to suspInputStr so we can restore it
					safe_strcpy(codeViewData.suspInputStr,
					            codeViewData.inputStr);
				}
				safe_strcpy(codeViewData.inputStr,
				            (--histBuffPos)->c_str());
				codeViewData.inputPos = safe_strlen(codeViewData.inputStr);
				break;
		case KEY_F(7):	// next command (f1-f4 generate rubbish at my place)
		case KEY_F(4):	// next command
				if (histBuffPos == histBuff.end()) break;
				if (++histBuffPos != histBuff.end()) {
					safe_strcpy(codeViewData.inputStr,
					            histBuffPos->c_str());
				} else {
					// copy suspInputStr back into inputStr
					safe_strcpy(codeViewData.inputStr,
					            codeViewData.suspInputStr);
				}
				codeViewData.inputPos = safe_strlen(codeViewData.inputStr);
				break;
		case KEY_F(5):	// Run Program
				debugging=false;
				DrawCode(); // update code window to show "running" status

				ret = DEBUG_Run(1,false);
				skipDraw = true; // don't update screen after this instruction
				break;
		case KEY_F(8):	// Toggle printable characters
				showPrintable = !showPrintable;
				break;
		case KEY_F(9):	// Set/Remove Breakpoint
				if (CBreakpoint::IsBreakpoint(codeViewData.cursorSeg, codeViewData.cursorOfs)) {
					if (CBreakpoint::DeleteBreakpoint(codeViewData.cursorSeg, codeViewData.cursorOfs))
						DEBUG_ShowMsg("DEBUG: Breakpoint deletion success.\n");
					else
						DEBUG_ShowMsg("DEBUG: Failed to delete breakpoint.\n");
				}
				else {
					CBreakpoint::AddBreakpoint(codeViewData.cursorSeg, codeViewData.cursorOfs, false);
					DEBUG_ShowMsg("DEBUG: Set breakpoint at %04X:%04X\n",codeViewData.cursorSeg,codeViewData.cursorOfs);
				}
				break;
		case KEY_F(10):	// Step over inst
				if (StepOver()) {
					ret = DEBUG_Run(1,false);
					skipDraw = true;
					break;
				}
				// If we aren't stepping over something, do a normal step.
				[[fallthrough]];
		case KEY_F(11):	// trace into
				exitLoop = false;
				ret = DEBUG_Run(1,true);
				break;
		case 0x0A: //Parse typed Command
				codeViewData.inputStr[MAXCMDLEN] = '\0';
				if(ParseCommand(codeViewData.inputStr)) {
					char* cmd = ltrim(codeViewData.inputStr);
					if (histBuff.empty() || *--histBuff.end()!=cmd)
						histBuff.push_back(cmd);
					if (histBuff.size() > MAX_HIST_BUFFER) histBuff.pop_front();
					histBuffPos = histBuff.end();
					ClearInputLine();
				} else {
					codeViewData.inputPos = safe_strlen(codeViewData.inputStr);
				}
				break;
		case KEY_BACKSPACE: //backspace (linux)
		case 0x7f:	// backspace in some terminal emulators (linux)
		case 0x08:	// delete 
				if (codeViewData.inputPos == 0) break;
				codeViewData.inputPos--;
				[[fallthrough]];
		case KEY_DC: // delete character
				if ((codeViewData.inputPos<0) || (codeViewData.inputPos>=MAXCMDLEN)) break;
				if (codeViewData.inputStr[codeViewData.inputPos] != 0) {
						codeViewData.inputStr[MAXCMDLEN] = '\0';
						for(char* p=&codeViewData.inputStr[codeViewData.inputPos];(*p=*(p+1));p++) {}
				}
 				break;
		default:
				if ((key>=32) && (key<127)) {
					if ((codeViewData.inputPos<0) || (codeViewData.inputPos>=MAXCMDLEN)) break;
					codeViewData.inputStr[MAXCMDLEN] = '\0';
					if (codeViewData.inputStr[codeViewData.inputPos] == 0) {
							codeViewData.inputStr[codeViewData.inputPos++] = char(key);
							codeViewData.inputStr[codeViewData.inputPos] = '\0';
					} else if (!codeViewData.ovrMode) {
						int len = (int) safe_strlen(codeViewData.inputStr);
						if (len < MAXCMDLEN) {
							for(len++;len>codeViewData.inputPos;len--)
								codeViewData.inputStr[len]=codeViewData.inputStr[len-1];
							codeViewData.inputStr[codeViewData.inputPos++] = char(key);
						}
					} else {
						codeViewData.inputStr[codeViewData.inputPos++] = char(key);
					}
				} else if (key==killchar()) {
					ClearInputLine();
				}
				break;
		}
		if (ret<0) return ret;
		if (ret>0) {
			if (GCC_UNLIKELY(ret >= CB_MAX))
				ret = 0;
			else
				ret = (*CallBack_Handlers[ret])();
			if (ret) {
				exitLoop=true;
				CPU_Cycles=CPU_CycleLeft=0;
				return ret;
			}
		}
		ret=0;
		if (!skipDraw)
			DEBUG_DrawScreen();
	}
	return ret;
}

Bitu DEBUG_Loop(void) {
//TODO Disable sound
	GFX_Events();
	// Interrupt started ? - then skip it
	uint16_t oldCS	= SegValue(cs);
	uint32_t oldEIP	= reg_eip;
	PIC_runIRQs();
	Delay(1);
	if ((oldCS!=SegValue(cs)) || (oldEIP!=reg_eip)) {
		CBreakpoint::AddBreakpoint(oldCS,oldEIP,true);
		CBreakpoint::ActivateBreakpointsExceptAt(SegPhys(cs)+reg_eip);
		debugging=false;
		DOSBOX_SetNormalLoop();
		return 0;
	}
	return DEBUG_CheckKeys();
}

#include <queue>

extern SDL_Window *pdc_window;
extern std::queue<SDL_Event> pdc_event_queue;

void DEBUG_Enable(bool pressed)
{
	if (!pressed)
		return;

	// Maybe construct the debugger's UI
	static bool was_ui_started = false;
	if (!was_ui_started) {
		DBGUI_StartUp();
		was_ui_started = (pdc_window != nullptr);
	}

	// The debugger is run in release mode so cannot use asserts
	if (!was_ui_started) {
		LOG_ERR("DEBUG: Failed to start up the debug window");
		return;
	}

	// Defocus the graphical UI and bring the debugger UI into focus
	GFX_LosingFocus();
	pdc_event_queue = {};
	SDL_RaiseWindow(pdc_window);
	SDL_SetWindowInputFocus(pdc_window);
	SetCodeWinStart();
	DEBUG_DrawScreen();

	// Maybe show help for the first time in the debugger
	static bool was_help_shown = false;
	if (!was_help_shown) {
		DEBUG_ShowMsg("***| TYPE HELP (+ENTER) TO GET AN OVERVIEW OF ALL COMMANDS |***\n");
		was_help_shown = true;
	}

	// Start the debugging loops
	debugging = true;
	DOSBOX_SetLoop(&DEBUG_Loop);

	KEYBOARD_ClrBuffer();
}

void DEBUG_DrawScreen(void) {
	DrawData();
	DrawCode();
	DrawRegisters();
	DrawVariables();
}

static void DEBUG_RaiseTimerIrq(void) {
	PIC_ActivateIRQ(0);
}

// Display the content of the MCB chain starting with the MCB at the specified segment.
static void LogMCBChain(uint16_t mcb_segment) {
	DOS_MCB mcb(mcb_segment);
	char filename[9]; // 8 characters plus a terminating NUL
	const char *psp_seg_note;
	uint16_t DOS_dataOfs = static_cast<uint16_t>(dataOfs); //Realmode addressing only
	PhysPt dataAddr = PhysicalMake(dataSeg,DOS_dataOfs);// location being viewed in the "Data Overview"

	// loop forever, breaking out of the loop once we've processed the last MCB
	while (true) {
		// verify that the type field is valid
		if (mcb.GetType()!=0x4d && mcb.GetType()!=0x5a) {
			LOG(LOG_MISC,LOG_ERROR)("MCB chain broken at %04X:0000!",mcb_segment);
			return;
		}

		mcb.GetFileName(filename);

		// some PSP segment values have special meanings
		switch (mcb.GetPSPSeg()) {
			case MCB_FREE:
				psp_seg_note = "(free)";
				break;
			case MCB_DOS:
				psp_seg_note = "(DOS)";
				break;
			default:
				psp_seg_note = "";
		}

		LOG(LOG_MISC,LOG_ERROR)("   %04X  %12u     %04X %-7s  %s",mcb_segment,mcb.GetSize() << 4,mcb.GetPSPSeg(), psp_seg_note, filename);

		// print a message if dataAddr is within this MCB's memory range
		PhysPt mcbStartAddr = PhysicalMake(mcb_segment+1,0);
		PhysPt mcbEndAddr = PhysicalMake(mcb_segment+1+mcb.GetSize(),0);
		if (dataAddr >= mcbStartAddr && dataAddr < mcbEndAddr) {
			LOG(LOG_MISC,LOG_ERROR)("   (data addr %04hX:%04X is %u bytes past this MCB)",dataSeg,DOS_dataOfs,dataAddr - mcbStartAddr);
		}

		// if we've just processed the last MCB in the chain, break out of the loop
		if (mcb.GetType()==0x5a) {
			break;
		}
		// else, move to the next MCB in the chain
		mcb_segment+=mcb.GetSize()+1;
		mcb.SetPt(mcb_segment);
	}
}

// Display the content of all Memory Control Blocks.
static void LogMCBS(void) {
	LOG(LOG_MISC,LOG_ERROR)("MCB Seg  Size (bytes)  PSP Seg (notes)  Filename");
	LOG(LOG_MISC,LOG_ERROR)("Conventional memory:");
	LogMCBChain(dos.firstMCB);

	LOG(LOG_MISC,LOG_ERROR)("Upper memory:");
	LogMCBChain(dos_infoblock.GetStartOfUMBChain());
}

static void LogGDT(void) {
	char out1[512];
	Descriptor desc;
	Bitu length = cpu.gdt.GetLimit();
	PhysPt address = cpu.gdt.GetBase();
	PhysPt max	   = address + length;
	Bitu i = 0;
	LOG(LOG_MISC,LOG_ERROR)("GDT Base:%08X Limit:%08" sBitfs(X),address,length);
	while (address<max) {
		desc.Load(address);
		sprintf(out1,"%04" sBitfs(X) ": b:%08X type: %02X parbg",(i<<3),desc.GetBase(),desc.saved.seg.type);
		LOG(LOG_MISC,LOG_ERROR)("%s",out1);
		sprintf(out1,"      l:%08X dpl : %01X  %1X%1X%1X%1X%1X",desc.GetLimit(),desc.saved.seg.dpl,desc.saved.seg.p,desc.saved.seg.avl,desc.saved.seg.r,desc.saved.seg.big,desc.saved.seg.g);
		LOG(LOG_MISC,LOG_ERROR)("%s",out1);
		address+=8; i++;
	}
}

static void LogLDT(void) {
	char out1[512];
	Descriptor desc;
	Bitu ldtSelector = cpu.gdt.SLDT();
	if (!cpu.gdt.GetDescriptor(ldtSelector,desc)) return;
	Bitu length = desc.GetLimit();
	PhysPt address = desc.GetBase();
	PhysPt max	   = address + length;
	Bitu i = 0;
	LOG(LOG_MISC,LOG_ERROR)("LDT Base:%08X Limit:%08" sBitfs(X),address,length);
	while (address<max) {
		desc.Load(address);
		sprintf(out1,"%04" sBitfs(X) ": b:%08X type: %02X parbg",(i<<3)|4,desc.GetBase(),desc.saved.seg.type);
		LOG(LOG_MISC,LOG_ERROR)("%s",out1);
		sprintf(out1,"      l:%08X dpl : %01X  %1X%1X%1X%1X%1X",desc.GetLimit(),desc.saved.seg.dpl,desc.saved.seg.p,desc.saved.seg.avl,desc.saved.seg.r,desc.saved.seg.big,desc.saved.seg.g);
		LOG(LOG_MISC,LOG_ERROR)("%s",out1);
		address+=8; i++;
	}
}

static void LogIDT(void) {
	char out1[512];
	Descriptor desc;
	uint32_t address = 0;
	while (address<256*8) {
		if (cpu.idt.GetDescriptor(address,desc)) {
			sprintf(out1,"%04X: sel:%04X off:%02X",address/8,desc.GetSelector(),desc.GetOffset());
			LOG(LOG_MISC,LOG_ERROR)("%s",out1);
		}
		address+=8;
	}
}

void LogPages(char* selname) {
	char out1[512];
	if (paging.enabled) {
		Bitu sel = GetHexValue(selname,selname);
		if ((sel==0x00) && ((*selname==0) || (*selname=='*'))) {
			for (int i=0; i<0xfffff; i++) {
				Bitu table_addr=(paging.base.page<<12)+(i >> 10)*4;
				X86PageEntry table;
				table.set(phys_readd(table_addr));
				if (table.p) {
					X86PageEntry entry;
					Bitu entry_addr = (table.base << 12) +
					                  (i & 0x3ff) * 4;
					entry.set(phys_readd(entry_addr));
					if (entry.p) {
						sprintf(out1,
						        "page %05Xxxx -> %04Xxxx  flags [uw] %x:%x::%x:%x [d=%x|a=%x]",
						        i,
						        entry.base,
						        entry.us,
						        table.us,
						        entry.wr,
						        table.wr,
						        entry.d,
						        entry.a);
						LOG(LOG_MISC, LOG_ERROR)
						("%s", out1);
					}
				}
			}
		} else {
			Bitu table_addr=(paging.base.page<<12)+(sel >> 10)*4;
			X86PageEntry table;
			table.set(phys_readd(table_addr));
			if (table.p) {
				X86PageEntry entry;
				Bitu entry_addr = (table.base << 12) +
				                  (sel & 0x3ff) * 4;
				entry.set(phys_readd(entry_addr));
				sprintf(out1,
				        "page %05" sBitfs(X) "xxx -> %04Xxxx  flags [puw] %x:%x::%x:%x::%x:%x",
				        sel,
				        entry.base,
				        entry.p,
				        table.p,
				        entry.us,
				        table.us,
				        entry.wr,
				        table.wr);
				LOG(LOG_MISC, LOG_ERROR)("%s", out1);
			} else {
				sprintf(out1,
				        "pagetable %03" sBitfs(X) " not present, flags [puw] %x::%x::%x",
				        (sel >> 10),
				        table.p,
				        table.us,
				        table.wr);
				LOG(LOG_MISC, LOG_ERROR)("%s", out1);
			}
		}
	}
}

static void LogCPUInfo(void) {
	char out1[512];
	sprintf(out1,"cr0:%08" sBitfs(X) " cr2:%08u cr3:%08u  cpl=%" sBitfs(x),cpu.cr0,paging.cr2,paging.cr3,cpu.cpl);
	LOG(LOG_MISC,LOG_ERROR)("%s",out1);
	sprintf(out1, "eflags:%08x [vm=%x iopl=%x nt=%x]", reg_flags,
	        GETFLAG(VM) >> 17, GETFLAG(IOPL) >> 12, GETFLAG(NT) >> 14);
	LOG(LOG_MISC,LOG_ERROR)("%s",out1);
	sprintf(out1,"GDT base=%08X limit=%08" sBitfs(X),cpu.gdt.GetBase(),cpu.gdt.GetLimit());
	LOG(LOG_MISC,LOG_ERROR)("%s",out1);
	sprintf(out1,"IDT base=%08X limit=%08" sBitfs(X),cpu.idt.GetBase(),cpu.idt.GetLimit());
	LOG(LOG_MISC,LOG_ERROR)("%s",out1);

	Bitu sel=CPU_STR();
	Descriptor desc;
	if (cpu.gdt.GetDescriptor(sel,desc)) {
		sprintf(out1,"TR selector=%04" sBitfs(X) ", base=%08X limit=%08X*%X",sel,desc.GetBase(),desc.GetLimit(),desc.saved.seg.g?0x4000:1);
		LOG(LOG_MISC,LOG_ERROR)("%s",out1);
	}
	sel=CPU_SLDT();
	if (cpu.gdt.GetDescriptor(sel,desc)) {
		sprintf(out1,"LDT selector=%04" sBitfs(X) ", base=%08X limit=%08X*%X",sel,desc.GetBase(),desc.GetLimit(),desc.saved.seg.g?0x4000:1);
		LOG(LOG_MISC,LOG_ERROR)("%s",out1);
	}
}

#if C_HEAVY_DEBUG
static void LogInstruction(uint16_t segValue, uint32_t eipValue, ofstream &out)
{
	static char empty[23] = { 32,32,32,32,32,32,32,32,32,32,32,32,32,32,32,32,32,32,32,32,32,32,0 };

	if (cpuLogType == 3) { //Log only cs:ip.
		out << setw(4) << SegValue(cs) << ":" << setw(8) << reg_eip << endl;
		return;
	}

	PhysPt start = GetAddress(segValue,eipValue);
	char dline[200];Bitu size;
	size = DasmI386(dline, start, reg_eip, cpu.code.big);
	char* res = empty;
	if (showExtend && (cpuLogType > 0) ) {
		res = AnalyzeInstruction(dline,false);
		if (!res || !(*res)) res = empty;
		Bitu reslen = strlen(res);
		if (reslen < 22) memset(res + reslen, ' ',22 - reslen);
		res[22] = 0;
	}
	Bitu len = safe_strlen(dline);
	if (len < 30) memset(dline + len,' ',30 - len);
	dline[30] = 0;

	// Get register values

	if(cpuLogType == 0) {
		out << setw(4) << SegValue(cs) << ":" << setw(4) << reg_eip << "  " << dline;
	} else if (cpuLogType == 1) {
		out << setw(4) << SegValue(cs) << ":" << setw(8) << reg_eip << "  " << dline << "  " << res;
	} else if (cpuLogType == 2) {
		char ibytes[200]="";	char tmpc[200];
		for (Bitu i=0; i<size; i++) {
			uint8_t value;
			if (mem_readb_checked(start+i,&value)) sprintf(tmpc,"%s","?? ");
			else sprintf(tmpc,"%02X ",value);
			strcat(ibytes,tmpc);
		}
		len = safe_strlen(ibytes);
		if (len<21) { for (Bitu i=0; i<21-len; i++) ibytes[len + i] =' '; ibytes[21]=0;} //NOTE THE BRACKETS
		out << setw(4) << SegValue(cs) << ":" << setw(8) << reg_eip << "  " << dline << "  " << res << "  " << ibytes;
	}

	out << " EAX:" << setw(8) << reg_eax << " EBX:" << setw(8) << reg_ebx
	    << " ECX:" << setw(8) << reg_ecx << " EDX:" << setw(8) << reg_edx
	    << " ESI:" << setw(8) << reg_esi << " EDI:" << setw(8) << reg_edi
	    << " EBP:" << setw(8) << reg_ebp << " ESP:" << setw(8) << reg_esp
	    << " DS:"  << setw(4) << SegValue(ds)<< " ES:"  << setw(4) << SegValue(es);

	if(cpuLogType == 0) {
		out << " SS:"  << setw(4) << SegValue(ss) << " C"  << (get_CF()>0)  << " Z"   << (get_ZF()>0)
		    << " S" << (get_SF()>0) << " O"  << (get_OF()>0) << " I"  << GETFLAGBOOL(IF);
	} else {
		out << " FS:"  << setw(4) << SegValue(fs) << " GS:"  << setw(4) << SegValue(gs)
		    << " SS:"  << setw(4) << SegValue(ss)
		    << " CF:"  << (get_CF()>0)  << " ZF:"   << (get_ZF()>0)  << " SF:"  << (get_SF()>0)
		    << " OF:"  << (get_OF()>0)  << " AF:"   << (get_AF()>0)  << " PF:"  << (get_PF()>0)
		    << " IF:"  << GETFLAGBOOL(IF);
	}
	if(cpuLogType == 2) {
		out << " TF:" << GETFLAGBOOL(TF) << " VM:" << GETFLAGBOOL(VM) <<" FLG:" << setw(8) << reg_flags
		    << " CR0:" << setw(8) << cpu.cr0;
	}
	out << endl;
}
#endif

// DEBUG.COM stuff

class DEBUG final : public Program {
public:
	DEBUG() : active(false) { pDebugcom = this; }

	~DEBUG() override { pDebugcom = nullptr; }

	bool IsActive() const { return active; }

	void Run() override
	{
		if(cmd->FindExist("/NOMOUSE",false)) {
	        	real_writed(0,0x33<<2,0);
			return;
		}

		uint16_t commandNr = 1;
		if (!cmd->FindCommand(commandNr++,temp_line)) return;
		// Get filename
		char filename[128];
		safe_strcpy(filename, temp_line.c_str());
		// Setup commandline
		char args[256+1];
		args[0]	= 0;
		bool found = cmd->FindCommand(commandNr++,temp_line);
		while (found) {
			if (safe_strlen(args)+temp_line.length()+1>256) break;
			strcat(args,temp_line.c_str());
			found = cmd->FindCommand(commandNr++,temp_line);
			if (found) strcat(args," ");
		}
		// Start new shell and execute prog
		active = true;
		// Save cpu state....
		uint16_t oldcs	= SegValue(cs);
		uint32_t oldeip	= reg_eip;
		uint16_t oldss	= SegValue(ss);
		uint32_t oldesp	= reg_esp;

		// Start shell
		DOS_Shell shell;
		if (!shell.ExecuteProgram(filename, args))
			WriteOut(MSG_Get("PROGRAM_EXECUTABLE_MISSING"), filename);

		// set old reg values
		SegSet16(ss,oldss);
		reg_esp = oldesp;
		SegSet16(cs,oldcs);
		reg_eip = oldeip;
	}

private:
	bool	active;
};

void DEBUG_CheckExecuteBreakpoint(uint16_t seg, uint32_t off)
{
	if (pDebugcom && pDebugcom->IsActive()) {
		CBreakpoint::AddBreakpoint(seg,off,true);
		CBreakpoint::ActivateBreakpointsExceptAt(SegPhys(cs)+reg_eip);
		pDebugcom = nullptr;
	}
}

Bitu DEBUG_EnableDebugger()
{
	exitLoop = true;
	DEBUG_Enable(true);
	CPU_Cycles=CPU_CycleLeft=0;
	return 0;
}

void DEBUG_ShutDown(Section * /*sec*/) {
	CBreakpoint::DeleteAll();
	CDebugVar::DeleteAll();
	curs_set(old_cursor_state);

	if (pdc_window)
		endwin();
}

Bitu debugCallback;

void DEBUG_Init(Section* sec) {

//	MSG_Add("DEBUG_CONFIGFILE_HELP","Debugger related options.\n");
	DEBUG_DrawScreen();
	/* Add some keyhandlers */
	// Florian: Changing to something easier to access on the laptop
	// MAPPER_AddHandler(DEBUG_Enable, SDL_SCANCODE_PAUSE, MMOD2, "debugger",
	//                  "Debugger");
	MAPPER_AddHandler(DEBUG_Enable, SDL_SCANCODE_B, MMOD2, "debugger",
	                  "Debugger");
	/* Reset code overview and input line */
	codeViewData = {};
	/* setup debug.com */
	PROGRAMS_MakeFile("DEBUG.COM",ProgramCreate<DEBUG>);
	PROGRAMS_MakeFile("DBXDEBUG.COM",ProgramCreate<DEBUG>);
	/* Setup callback */
	debugCallback=CALLBACK_Allocate();
	CALLBACK_Setup(debugCallback,DEBUG_EnableDebugger,CB_RETF,"debugger");
	/* shutdown function */
	sec->AddDestroyFunction(&DEBUG_ShutDown);
	

	SIS_Init();
}

// DEBUGGING VAR STUFF

void CDebugVar::InsertVariable(char *name, PhysPt adr)
{
	varList.push_back(new CDebugVar(name,adr));
}

void CDebugVar::DeleteAll()
{
	std::vector<CDebugVar*>::iterator i;
	CDebugVar* bp;
	for(i=varList.begin(); i != varList.end(); i++) {
		bp = static_cast<CDebugVar*>(*i);
		delete bp;
	}
	(varList.clear)();
}

CDebugVar *CDebugVar::FindVar(PhysPt pt)
{
	if (varList.empty()) return nullptr;

	std::vector<CDebugVar*>::size_type s = varList.size();
	CDebugVar* bp;
	for(std::vector<CDebugVar*>::size_type i = 0; i != s; i++) {
		bp = static_cast<CDebugVar*>(varList[i]);
		if (bp->GetAdr() == pt) return bp;
	}
	return nullptr;
}

bool CDebugVar::SaveVars(char *name)
{
	if (varList.size() > 65535) return false;
	const std_fs::path vars_file = name;
	FILE* f = fopen(vars_file.string().c_str(), "wb+");
	if (!f) {
		DEBUG_ShowMsg("DEBUG: Output of vars failed.\n");
		return false;
	}
	DEBUG_ShowMsg("DEBUG: vars file '%s' created.\n",
	              std_fs::absolute(vars_file).string().c_str());


	// write number of vars
	uint16_t num = (uint16_t)varList.size();
	fwrite(&num,1,sizeof(num),f);

	std::vector<CDebugVar*>::iterator i;
	CDebugVar* bp;
	for(i=varList.begin(); i != varList.end(); i++) {
		bp = static_cast<CDebugVar*>(*i);
		// name
		fwrite(bp->GetName(),1,16,f);
		// adr
		PhysPt adr = bp->GetAdr();
		fwrite(&adr,1,sizeof(adr),f);
	}
	fclose(f);
	return true;
}

bool CDebugVar::LoadVars(char *name)
{
	const std_fs::path vars_file = name;
	FILE* f = fopen(vars_file.string().c_str(), "rb");
	if (!f) {
		DEBUG_ShowMsg("DEBUG: Load of vars from %s failed.\n",
		              name);
		return false;
	}
	DEBUG_ShowMsg("DEBUG: vars file '%s' loaded.\n",
	              std_fs::absolute(vars_file).string().c_str());
	// read number of vars
	uint16_t num;
	if (fread(&num,sizeof(num),1,f) != 1) {
		fclose(f);
		return false;
	}
	for (uint16_t i=0; i<num; i++) {
		char name[16];
		// name
		if (fread(name,16,1,f) != 1) break;
		// adr
		PhysPt adr;
		if (fread(&adr,sizeof(adr),1,f) != 1) break;
		// insert
		InsertVariable(name,adr);
	}
	fclose(f);
	return true;
}

static void SaveMemory(uint16_t seg, uint32_t ofs1, uint32_t num) {
	const std_fs::path memdump_txt = "MEMDUMP.TXT";
	FILE* f = fopen(memdump_txt.string().c_str(),"wt");
	if (!f) {
		DEBUG_ShowMsg("DEBUG: Memory dump failed.\n");
		return;
	}
	DEBUG_ShowMsg("DEBUG: Memory dump file '%s' created.\n",
	              std_fs::absolute(memdump_txt).string().c_str());

	char buffer[128];
	char temp[16];

	while (num>16) {
		sprintf(buffer,"%04X:%04X   ",seg,ofs1);
		for (uint16_t x=0; x<16; x++) {
			uint8_t value;
			if (mem_readb_checked(GetAddress(seg,ofs1+x),&value)) sprintf(temp,"%s","?? ");
			else sprintf(temp,"%02X ",value);
			strcat(buffer,temp);
		}
		ofs1+=16;
		num-=16;

		fprintf(f,"%s\n",buffer);
	}
	if (num>0) {
		sprintf(buffer,"%04X:%04X   ",seg,ofs1);
		for (uint16_t x=0; x<num; x++) {
			uint8_t value;
			if (mem_readb_checked(GetAddress(seg,ofs1+x),&value)) sprintf(temp,"%s","?? ");
			else sprintf(temp,"%02X ",value);
			strcat(buffer,temp);
		}
		fprintf(f,"%s\n",buffer);
	}
	fclose(f);
	DEBUG_ShowMsg("DEBUG: Memory dump success.\n");
}

static void SaveMemoryBin(uint16_t seg, uint32_t ofs1, uint32_t num) {
	const std_fs::path memdump_bin = "MEMDUMP.BIN";
	FILE* f = fopen(memdump_bin.string().c_str(), "wb");
	if (!f) {
		DEBUG_ShowMsg("DEBUG: Memory binary dump failed.\n");
		return;
	}
	DEBUG_ShowMsg("DEBUG: Memory binary dump file '%s' created.\n",
	              std_fs::absolute(memdump_bin).string().c_str());

	for (Bitu x = 0; x < num;x++) {
		uint8_t val;
		if (mem_readb_checked(GetAddress(seg,ofs1+x),&val)) val=0;
		fwrite(&val,1,1,f);
	}

	fclose(f);
	DEBUG_ShowMsg("DEBUG: Memory dump binary success.\n");
}

static void SavePPM(uint16_t seg, uint32_t ofs1) {
	// Saves a PPM bitmap of an off-screen buffer, using the current VGA palette. Hardcoded to 320x200 @ 256 colors

	// Grab the palette
	constexpr auto palette_map = vga.dac.palette_map;

	// Save the file
	FILE* f = fopen("image.ppm", "wb");
	if (!f) {
		DEBUG_ShowMsg("DEBUG: Write PPM image failed.\n");
		return;
	}

	// Write the header
	fprintf(f, "P6\n");
	fprintf(f, "320 200\n");
	fprintf(f, "255\n");

	// TODO: Should these be Bitus?
	constexpr Bitu width = 320;
	constexpr Bitu height = 200;

	for (Bitu y = 0; y < height; y++) {
		for (Bitu x = 0; x < width; x++) {
			Bitu offset = y * width + x;
			uint8_t val;
			if (mem_readb_checked(GetAddress(seg, ofs1 + offset), &val))
				val = 0;
			fwrite(palette_map + val, 3, 1, f);
		}
		
	}

	fclose(f);
	DEBUG_ShowMsg("DEBUG: Memory dump binary success.\n");
}

static void SaveCalltrace(char* filename)
{
	// Save the file
	FILE* f = fopen(filename, "wb");
	if (!f) {
		DEBUG_ShowMsg("DEBUG: Write calltrace failed!\n");
		return;
	}

	//for (auto& current : calltrace) {
	for (auto& current : rolling_calltrace) {
		if (current.is_call) {
			fprintf(f, "CALL: ");
		}
		else {
			fprintf(f, "RET:  ");
		}
		fprintf(f,
		        "%04X:%04X --> %04X:%04X\n",
		        current.caller_seg,
		        current.caller_off,
		        current.callee_seg,
		        current.callee_off);
	}
	fclose(f);
	DEBUG_ShowMsg("DEBUG: Save calltrace complete.\n");
}

static void SaveMemWrites(char* filename) {
	// Save the file
	FILE* f = fopen(filename, "wb");
	if (!f) {
		DEBUG_ShowMsg("DEBUG: Write mem writes failed!\n");
		return;
	}


	for (auto& current : memwrites) {
		fprintf(f,
		        "%04X:%04X %01X %02X %04X\n",
		        current.caller_seg,
		        current.caller_off,
		        current.size,
		        current.value,
				current.address);
		/* current
		
		if (current.is_call) {
			fprintf(f, "CALL: ");
		} else {
			fprintf(f, "RET:  ");
		}
		fprintf(f,
		        "%04X:%04X --> %04X:%04X\n",
		        current.caller_seg,
		        current.caller_off,
		        current.callee_seg,
		        current.callee_off); */
	}
	fclose(f);
	DEBUG_ShowMsg("DEBUG: Save mem writes complete.\n");
}



static void OutputVecTable(char* filename) {
	const std_fs::path vec_table_file = filename;
	FILE* f = fopen(vec_table_file.string().c_str(), "wt");
	if (!f)
	{
		DEBUG_ShowMsg("DEBUG: Output of interrupt vector table failed.\n");
		return;
	}
	DEBUG_ShowMsg("DEBUG: Interrupt vector table file '%s' created.\n",
	              std_fs::absolute(vec_table_file).string().c_str());
	
	for (int i=0; i<256; i++)
		fprintf(f,"INT %02X:  %04X:%04X\n", i, mem_readw(i*4+2), mem_readw(i*4));

	fclose(f);
	DEBUG_ShowMsg("DEBUG: Interrupt vector table written to %s.\n",
	              vec_table_file.string().c_str());
}

#define DEBUG_VAR_BUF_LEN 16
static void DrawVariables()
{
	if (varList.empty())
		return;

	char buffer[DEBUG_VAR_BUF_LEN] = {};
	bool windowchanges = false;

	for (size_t i = 0; i != varList.size(); ++i) {
		if (i == 4*3) {
			/* too many variables */
			break;
		}

		auto dv = varList[i];
		uint16_t value;
		bool varchanges = false;
		bool has_no_value = mem_readw_checked(dv->GetAdr(),&value);
		if (has_no_value) {
			snprintf(buffer,DEBUG_VAR_BUF_LEN, "%s", "??????");
			dv->SetValue(false,0);
			varchanges = true;
		} else {
			if ( dv->HasValue() && dv->GetValue() == value) {
				; //It already had a value and it didn't change (most likely case)
			} else {
				dv->SetValue(true,value);
				snprintf(buffer,DEBUG_VAR_BUF_LEN, "0x%04x", value);
				varchanges = true;
			}
		}

		if (varchanges) {
			int y = i / 3;
			int x = (i % 3) * 26;
			mvwprintw(dbg.win_var, y, x, dv->GetName());
			mvwprintw(dbg.win_var, y,  (x + DEBUG_VAR_BUF_LEN + 1) , buffer);
			windowchanges = true; //Something has changed in this window
		}
	}

	if (windowchanges) wrefresh(dbg.win_var);
}
#undef DEBUG_VAR_BUF_LEN
// HEAVY DEBUGGING STUFF

#if C_HEAVY_DEBUG

const uint32_t LOGCPUMAX = 20000;

static uint32_t logCount = 0;

struct TLogInst {
	uint16_t s_cs;
	uint32_t eip;
	uint32_t eax;
	uint32_t ebx;
	uint32_t ecx;
	uint32_t edx;
	uint32_t esi;
	uint32_t edi;
	uint32_t ebp;
	uint32_t esp;
	uint16_t s_ds;
	uint16_t s_es;
	uint16_t s_fs;
	uint16_t s_gs;
	uint16_t s_ss;
	bool c;
	bool z;
	bool s;
	bool o;
	bool a;
	bool p;
	bool i;
	char dline[31];
	char res[23];
};

TLogInst logInst[LOGCPUMAX];

void DEBUG_HeavyLogInstruction()
{
	static char empty[23] = { 32,32,32,32,32,32,32,32,32,32,32,32,32,32,32,32,32,32,32,32,32,32,0 };

	PhysPt start = GetAddress(SegValue(cs),reg_eip);
	char dline[200];
	DasmI386(dline, start, reg_eip, cpu.code.big);
	char* res = empty;
	if (showExtend) {
		res = AnalyzeInstruction(dline,false);
		if (!res || !(*res)) res = empty;
		Bitu reslen = strlen(res);
		if (reslen < 22) memset(res + reslen, ' ',22 - reslen);
		res[22] = 0;
	}

	Bitu len = safe_strlen(dline);
	if (len < 30) memset(dline + len,' ',30 - len);
	dline[30] = 0;

	TLogInst & inst = logInst[logCount];
	strcpy(inst.dline,dline);
	inst.s_cs = SegValue(cs);
	inst.eip  = reg_eip;
	strcpy(inst.res,res);
	inst.eax  = reg_eax;
	inst.ebx  = reg_ebx;
	inst.ecx  = reg_ecx;
	inst.edx  = reg_edx;
	inst.esi  = reg_esi;
	inst.edi  = reg_edi;
	inst.ebp  = reg_ebp;
	inst.esp  = reg_esp;
	inst.s_ds = SegValue(ds);
	inst.s_es = SegValue(es);
	inst.s_fs = SegValue(fs);
	inst.s_gs = SegValue(gs);
	inst.s_ss = SegValue(ss);
	inst.c    = get_CF()>0;
	inst.z    = get_ZF()>0;
	inst.s    = get_SF()>0;
	inst.o    = get_OF()>0;
	inst.a    = get_AF()>0;
	inst.p    = get_PF()>0;
	inst.i    = GETFLAGBOOL(IF);

	if (++logCount >= LOGCPUMAX) logCount = 0;
}

void DEBUG_HeavyWriteLogInstruction()
{
	if (!logHeavy) return;
	logHeavy = false;

	DEBUG_ShowMsg("DEBUG: Creating cpu log LOGCPU_INT_CD.TXT\n");

	ofstream out("LOGCPU_INT_CD.TXT");
	if (!out.is_open()) {
		DEBUG_ShowMsg("DEBUG: Failed.\n");
		return;
	}
	out << hex << noshowbase << setfill('0') << uppercase;
	uint32_t startLog = logCount;
	do {
		// Write Instructions
		TLogInst & inst = logInst[startLog];
		out << setw(4) << inst.s_cs << ":" << setw(8) << inst.eip << "  "
		    << inst.dline << "  " << inst.res << " EAX:" << setw(8)<< inst.eax
		    << " EBX:" << setw(8) << inst.ebx << " ECX:" << setw(8) << inst.ecx
		    << " EDX:" << setw(8) << inst.edx << " ESI:" << setw(8) << inst.esi
		    << " EDI:" << setw(8) << inst.edi << " EBP:" << setw(8) << inst.ebp
		    << " ESP:" << setw(8) << inst.esp << " DS:"  << setw(4) << inst.s_ds
		    << " ES:"  << setw(4) << inst.s_es<< " FS:"  << setw(4) << inst.s_fs
		    << " GS:"  << setw(4) << inst.s_gs<< " SS:"  << setw(4) << inst.s_ss
		    << " CF:"  << inst.c  << " ZF:"   << inst.z  << " SF:"  << inst.s
		    << " OF:"  << inst.o  << " AF:"   << inst.a  << " PF:"  << inst.p
		    << " IF:"  << inst.i  << endl;

/*		fprintf(f,"%04X:%08X   %s  %s  EAX:%08X EBX:%08X ECX:%08X EDX:%08X ESI:%08X EDI:%08X EBP:%08X ESP:%08X DS:%04X ES:%04X FS:%04X GS:%04X SS:%04X CF:%01X ZF:%01X SF:%01X OF:%01X AF:%01X PF:%01X IF:%01X\n",
			logInst[startLog].s_cs,logInst[startLog].eip,logInst[startLog].dline,logInst[startLog].res,logInst[startLog].eax,logInst[startLog].ebx,logInst[startLog].ecx,logInst[startLog].edx,logInst[startLog].esi,logInst[startLog].edi,logInst[startLog].ebp,logInst[startLog].esp,
		        logInst[startLog].s_ds,logInst[startLog].s_es,logInst[startLog].s_fs,logInst[startLog].s_gs,logInst[startLog].s_ss,
		        logInst[startLog].c,logInst[startLog].z,logInst[startLog].s,logInst[startLog].o,logInst[startLog].a,logInst[startLog].p,logInst[startLog].i);*/
		if (++startLog >= LOGCPUMAX) startLog = 0;
	} while (startLog != logCount);

	out.close();
	DEBUG_ShowMsg("DEBUG: Done.\n");
}

Bitu tracePointSeg = 0x01e7;
Bitu tracePointOff = 0xdb8e;

// TODO: Hardcoded trace point functionality for a specific use case
bool DEBUG_HandleTracePoint(Bitu seg, Bitu off) {
	if (seg == tracePointSeg && off == tracePointOff) {
		uint8_t alValue = reg_al;
		return true;
	}
	return false;
}

uint16_t script_read_seg;
uint16_t script_read_off;
uint16_t script_last_read_off = 0xFFFF;
std::chrono::time_point<std::chrono::system_clock> script_last_leave;

uint8_t old_AH = 0xFF;


void DEBUG_HandleBackbufferBlit(Bitu seg, Bitu off) {
	if (!isChannelActive(SIS_ChannelID::Special)) {
		return;
	}

	if (seg == 0x01F7 && off == 0x1708) {
		uint32_t ret_seg = mem_readw_inline(GetAddress(SegValue(ss), reg_bp + 0x04));
		uint32_t ret_off = mem_readw_inline(GetAddress(SegValue(ss), reg_bp + 0x02));
		fprintf(stdout, "16E7: Results of 1480 call: %.4x %.4x - caller %.4x:%.4x\n", reg_ax, reg_dx, ret_seg, ret_off);
	}
	// We use this so that we skip repeats, we need to see a different location first.
	static bool shouldTrigger = true;
	if (seg == 0x01F7 && off == 0x040F) {
		if (shouldTrigger) {
			// This is the rep movsb
			// Check if we are copying into the front buffer
			uint16_t seg_es = SegValue(es);
			// TODO: Looks like segment 0x014f is somehow mapped to VGA memory
			if (seg_es != 0x014F) {
				return;
			}

			uint16_t x = reg_di % 0x140;
			uint16_t y = reg_di / 0x140;
			uint32_t ret_seg = mem_readw_inline(GetAddress(SegValue(ss), reg_bp + 0x04));
			uint32_t ret_off = mem_readw_inline(GetAddress(SegValue(ss), reg_bp + 0x02));

			


			fprintf(stdout, "Copying %u bytes from %.4x:%.4x to %.4x:%.4x (%u, %u) - caller %.4x:%.4x\n", reg_cx, SegValue(ds), reg_si, SegValue(es), reg_di, x, y, ret_seg, ret_off);
			shouldTrigger = false;
		}
	}
	else if (seg == 0x01F7 && (off == 0x0E74 || off == 0x0E75)) {
		if (shouldTrigger) {
			uint32_t param1 = mem_readw_inline(GetAddress(SegValue(ss), reg_bp + 0x0A));
			uint32_t param2 = mem_readw_inline(GetAddress(SegValue(ss), reg_bp + 0x06));
			uint32_t param3 = mem_readw_inline(GetAddress(SegValue(ss), reg_bp + 0x08));

			uint32_t ret_seg = mem_readw_inline(GetAddress(SegValue(ss), reg_bp + 0x04));
			uint32_t ret_off = mem_readw_inline(GetAddress(SegValue(ss), reg_bp + 0x02));


			fprintf(stdout, "Function 0C88: Copying %u bytes from %.4x:%.4x to %.4x:%.4x - caller %.4x:%.4x - params 0x0A: %.4x 0x06: %.4x 0x04: %.4x\n", reg_cx, SegValue(ds), reg_si, SegValue(es), reg_di, ret_seg, ret_off, param1, param2, param3);
			shouldTrigger = false;
		}
	}
	else {
		shouldTrigger = true;
	}
}

void DEBUG_HandleFileAccess(Bitu seg, Bitu off) {
	
	static uint64_t positions[10];

	if (!isChannelActive(SIS_ChannelID::Fileread)) {
		// TODO: Use proper variable
		return;
	}
	if (seg == 0x01E7 && off == 0x068B) {
		fprintf(stdout, "-- Starting to read an RLE image\n");
	}

	if (seg != 0x0217) {
		return;
	}

	
	if (off == 0x0B8F) {

		uint16_t entry = reg_bx;
		uint32_t handle = RealHandle(entry);
		if (handle >= DOS_FILES) {
			DOS_SetError(DOSERR_INVALID_HANDLE);
			return;
		};
		const char* name = Files[handle]->GetName();
		
		uint32_t ret_seg = mem_readw_inline(GetAddress(SegValue(ss), reg_bp + 0x04));
		uint32_t ret_off = mem_readw_inline(GetAddress(SegValue(ss), reg_bp + 0x02));

		// This is the command after the INT21
		uint16_t target_seg = SegValue(ds);
		
		uint32_t target_off = reg_dx;
		uint32_t num_byte_read = reg_ax;
		
		uint64_t old_pos = positions[entry];
		positions[entry] += num_byte_read;
		
		// Only filtering out here, since we want to be able to catch reads to other segments for keeping track of 
		// the position in the file
		if (SIS_filterSegment > -1) {
			if (SIS_filterSegment != target_seg) {
				return;
			}
		}
		fprintf(stdout, "DOS file read from file %u (%s, %.8x) bytes read %u to address: %.4x:%.4x, caller %.4x:%.4x, values: ", entry, name, old_pos, num_byte_read, target_seg, target_off, ret_seg, ret_off);
		for (int i = 0; i < num_byte_read; i++) {
			uint8_t current_byte = mem_readb_inline(GetAddress(target_seg, target_off + i));
			fprintf(stdout, "%02x", current_byte);
		}
		fprintf(stdout, "\n");
	}
	else if (off == 0x0BEC) {
		// This is where we set the opcode for seeking (potentially)
		old_AH = reg_ah;
	}
	else if (off == 0x0BEE) {
		// This is after a seek (potentially)
		if (old_AH != 0x42) {
			return;
		}
		uint16_t msb = reg_dx;
		uint16_t lsb = reg_ax;
		uint16_t entry = reg_bx;

		uint32_t ret_seg = mem_readw_inline(GetAddress(SegValue(ss), reg_bp + 0x04));
		uint32_t ret_off = mem_readw_inline(GetAddress(SegValue(ss), reg_bp + 0x02));

		uint32_t handle = RealHandle(entry);
		if (handle >= DOS_FILES) {
			DOS_SetError(DOSERR_INVALID_HANDLE);
			return;
		};
		const char* name = Files[handle]->GetName();
		
		fprintf(stdout, "DOS file seek file %u new position: %.2x%.2x (%s), caller:  %.4x:%.4x\n", entry, msb, lsb, name, ret_seg, ret_off);
		positions[entry] = ((uint64_t)msb << 16) + lsb;
	}
}
void DEBUG_HandleScript(Bitu seg, Bitu off)
{
	if (!isScriptChannelActive()) {
		return;
	}

	if (seg == 0x01E7 && off == 0xE4BF) {
		uint16_t objectIndex = mem_readw_inline(GetAddress(0x0227, 0x0F92));
		SIS_DebugScript(
		        "----- Switching execution to script for object: %.4x\n",
		        objectIndex);
		return;
	}

	if (seg == 0x01D7 && off == 0x082A) {
		SIS_DebugScript("*** Mouse press ***\n");
		return;
	}

	if (seg != 0x01E7) {
		return;
	}

	uint32_t ret_seg = mem_readw_inline(GetAddress(SegValue(ss), reg_bp + 0x04));
	uint32_t ret_off = mem_readw_inline(GetAddress(SegValue(ss), reg_bp + 0x02));

	if (off == 0xDB56) {
		std::chrono::time_point<std::chrono::system_clock> now =
		        std::chrono::system_clock::now();
		auto milliseconds = std::chrono::duration_cast<std::chrono::milliseconds>(
		                            now - script_last_leave)
		                            .count();
		uint16_t currentSceneID = mem_readw_inline(GetAddress(0x0227, 0x077C));
		uint16_t global1012 = mem_readw_inline(GetAddress(0x0227, 0x1012));
		uint16_t global1014 = mem_readw_inline(GetAddress(0x0227, 0x1014));
		if (isChannelActive(SIS_ChannelID::Script_Verbose)) {
			SIS_DebugScript("----- Scripting function entered - scene: %.2x 1012: %.2x 1014: %.2x (%u ms since last leave)\n",
			        currentSceneID, global1012, global1014, milliseconds);
		} else {
			SIS_DebugScript("----- Scripting function entered - scene: %.2x 1014: %.2x 1012: %.2x\n",
			        currentSceneID,
			        global1014,
			        global1012
			        );
		}
	} else if (off == 0xDB89) {
		// Check if we have a valid skip value
		uint32_t script_offset = mem_readw_inline(
		        GetAddress(SegValue(ds), 0x0F8A));
		if (script_off_skip_start == script_offset) {
			if (isChannelActive(SIS_ChannelID::Script_Verbose)) {
				SIS_DebugScript(
				        "----- Skipping script from %.4x:%.4x to %.4x:%.4x\n",
				        SegValue(ds),
				        script_off_skip_start,
				        SegValue(ds),
				        script_off_skip_end);
			} else if (isChannelActive(SIS_ChannelID::Script)) {
				SIS_DebugScript(
				        "----- Skipping script from %.4x to %.4x\n",
				        script_off_skip_start,
				        script_off_skip_end);
			}
			mem_writew_inline(GetAddress(SegValue(ds), 0x0F8A),
			                  script_off_skip_end);
		}
		// This is right before we start executing an opcode
		SIS_BeginBuffering();
		SIS_LastOpcodeTriggeredSkip = false;
	} else if (off == 0xE3E5) {
		SIS_DebugScript("----- Scripting function left\n");
		script_last_leave = std::chrono::system_clock::now();
	} else if (off == 0x9F17) {
		// This is the case where we read a byte from the file
		uint32_t script_offset = mem_readw_inline(
		        GetAddress(SegValue(ds), 0x0F8A));
		if (isChannelActive(SIS_ChannelID::Script_Verbose)) {
			SIS_DebugScript(
			        "Script read (byte): %.2x at location %.4x:%.4x | %.4x (%.4x:%.4x)\n",
			        reg_al,
			        SegValue(es),
			        reg_di,
			        script_offset,
			        ret_seg,
			        ret_off);
		} else if (isChannelActive(SIS_ChannelID::Script) &&
		           !SIS_ScriptIsSkipping) {
			SIS_DebugScript(
			        "Script read (byte): %.2x at location %.4x\n",
			        reg_al,
			        script_offset);
		}
		if (reg_di - script_last_read_off > 1) {
			if (isChannelActive(SIS_ChannelID::Script_Verbose)) {
				SIS_DebugScript(
				        "-- Gap of %u bytes\n",
				        reg_di - script_last_read_off);
			}
		}
		script_last_read_off = reg_di + 1;
	} else if (off == 0x9F34) {
		// First stage of capturing script read: Save the location we
		// are reading from
		script_read_seg = reg_dx;
		script_read_off = reg_ax;
	} else if (off == 0x9F40) {
		// Second stage of a script read for a pointed to value
		uint32_t script_offset = mem_readw_inline(
		        GetAddress(SegValue(ds), 0x0F8A));

		if (isChannelActive(SIS_ChannelID::Script_Verbose)) {
			SIS_DebugScript(
			        "Script read (word): %.4x at location %.4x:%.4x | %.4x (%.4x:%.4x)\n",
			        reg_ax,
			        script_read_seg,
			        script_read_off,
			        script_offset,
			        ret_seg,
			        ret_off);
		} else if (isChannelActive(SIS_ChannelID::Script_Verbose)) {
			SIS_DebugScript(
			        "Script read (word): %.4x at location %.4x\n",
			        reg_ax,
			        script_offset);
		}
		if (script_read_off - script_last_read_off > 2) {
			if (isChannelActive(SIS_Script_Verbose)) {
				SIS_DebugScript(
				        "-- Gap of %u bytes\n",
				        script_read_off - script_last_read_off);
			}
		}
		script_last_read_off = script_read_off + 2;
	} else if (off == 0xDB8E) {
		SIS_currentOpcode1 = reg_al;
		std::string opcodeInfo;
		if (reg_al != 5) {
			opcodeInfo = SIS_IdentifyScriptOpcode(reg_al, 0);
		}
		SIS_DebugScript("- First block opcode: %.2x %s\n",
		        reg_al,
		        opcodeInfo.c_str());
	} else if (off == 0xDC6B) {
		std::string opcodeInfo = SIS_IdentifyScriptOpcode(SIS_currentOpcode1,
		                                                  reg_al);
		SIS_DebugScript("- Second block opcode: %.2x %s\n",
		        reg_al,
		        opcodeInfo.c_str());
	} else if (off == 0x9F5E) {
		uint8_t opcode = SIS_GetLocalByte(-0x5);
		uint16_t value = reg_ax;
		std::string opcodeInfo = SIS_IdentifyHelperOpcode(opcode, value);
		if (isChannelActive(SIS_ChannelID::Script_Verbose)) {
			SIS_DebugScript("- 9F4D opcode: %.2x %.4x %s (%.4x:%.4x)\n",
			        opcode,
			        value,
			        opcodeInfo.c_str(),
			        ret_seg,
			        ret_off);
		} else if (isChannelActive(SIS_ChannelID::Script)) {
			SIS_DebugScript("- 9F4D opcode: %.2x %.4x %s\n",
			        opcode,
			        value,
			        opcodeInfo.c_str());
		}
	} else if (off == 0xA332) {
		if (isChannelActive(SIS_ChannelID::Script_Verbose)) {
			SIS_DebugScript("- 9F4D results: %.4x %.4x (%.4x:%.4x)\n",
			        reg_ax,
			        reg_dx,
			        ret_seg,
			        ret_off);
		} else if (isChannelActive(SIS_ChannelID::Script)) {
			SIS_DebugScript("- 9F4D results: %.4x %.4x\n", reg_ax, reg_dx);
		}
	} else if (off == 0xA3D2) {
		SIS_ScriptIsSkipping = true;
		SIS_LastOpcodeTriggeredSkip = true;
		if (isChannelActive(SIS_ChannelID::Script_Verbose)) {
			SIS_DebugScript("-- Entering A3D2\n");
		} else {
			SIS_DebugScript("-- Skipping using A3D2\n");
		}
	} else if (off == 0xA417) {
		// We are skipping bytes using A3D2
		uint32_t num_bytes = reg_ax;
		uint16_t opcode1   = mem_readb_inline(
                        GetAddress(SegValue(ss), reg_bp - 0x1));
		uint16_t skipValue = mem_readb_inline(
		        GetAddress(SegValue(ss), reg_bp - 0x4));
		if (isChannelActive(SIS_ChannelID::Script_Verbose)) {
			SIS_DebugScript(
			        "- A3D2 skipping %u bytes for opcode %.2x [%u] (%.4x:%.4x)\n",
			        num_bytes,
			        opcode1,
			        skipValue,
			        ret_seg,
			        ret_off);
		} /* else if (isChannelActive(SIS_Script)) {
		        fprintf(stdout,
		                "- A3D2 skipping %u bytes for opcode %.2x
		[%u]\n", num_bytes, opcode1, skipValue
		        );
		}*/
	} else if (off == 0xA437) {
		SIS_ScriptIsSkipping = false;
		if (isChannelActive(SIS_ChannelID::Script_Verbose)) {
			SIS_DebugScript("-- Leaving A3D2\n");
		}
	}
	else if (off == 0xE3BA) {
		// This is where we are finished executing an opcode
		SIS_EndBuffering(!SIS_LastOpcodeTriggeredSkip);
	}
}

extern void GFX_RefreshTitle(const bool is_paused = false);
extern void GFX_SetTitle(const int32_t cycles, const bool is_paused = false);

void DEBUG_HandleSpecial(Bitu seg, Bitu off) {
	// TODO: Could still do with a bit better location for this one.
	if (seg == 0x01E7 && off == 0x01D9) {
		// Update mouse position in the title
		static PhysPt xPos = GetAddress(0x0227, 0x0770);
		static PhysPt yPos = GetAddress(0x0227, 0x0772);
		sdl.title_bar.x = mem_readw_inline(xPos);
		sdl.title_bar.y = mem_readw_inline(yPos);
		GFX_RefreshTitle();
	}


	// Check if we are at the target location
	if (!(seg == 0x01E7 && off == 0x1BAA)) {
		return;
	}

	


	// Check if we are following the right object
	uint16_t ptrBP = reg_bp;
	uint32_t ptrSS = SegValue(ss);
	uint32_t objectID = mem_readw_inline(GetAddress(ptrSS, ptrBP + 0x06));
	if (objectID != 01) {
		return;
	}

	
	// At this point, we have the correct values in the registers.
	uint16_t ptrES = SegValue(es);
	uint32_t ptrDI = reg_edi;
	
	// Load the x value
	uint32_t x = mem_readw_inline(GetAddress(ptrES, ptrDI));


	// Load the y value
	uint32_t y = mem_readw_inline(GetAddress(ptrES, ptrDI + 2));

	uint32_t nonsense = x + y;
	
}



bool DEBUG_HandleRegexpBreakpoint(Bitu seg, Bitu off) {
	if (self_regex == nullptr) {
		return false;
	}
	PhysPt start = GetAddress(seg, off);
	if (start == regex_bp_addr) {
		return false;
	}
	char dline[200]; Bitu size;
	size = DasmI386(dline, start, reg_eip, cpu.code.big);

	
	if (std::regex_search(string(dline), *self_regex)) {
		regex_bp_addr = start;
		return true;
	}
	

	return false;
}

bool lastMouseTest = false;

bool DEBUG_HeavyIsBreakpoint(void) {
	static Bitu zero_count = 0;

	/* if ((SegValue(cs) == 0x01D7) && reg_eip == 0x081A && lastMouseTest == false) {
		if (reg_ax == 0x2) {
			lastMouseTest = true;
			return true;
		}
	}
	else {
		lastMouseTest = false;
	} */

	SIS_HandleSIS(SegValue(cs), reg_eip);
	if (SIS_IsBreakpoint(SegValue(cs), reg_eip)) {
		return true;
	}

	DEBUG_HandleSpecial(SegValue(cs), reg_eip);
	DEBUG_HandleScript(SegValue(cs), reg_eip);
	DEBUG_HandleFileAccess(SegValue(cs), reg_eip);
	DEBUG_HandleBackbufferBlit(SegValue(cs), reg_eip);

	DEBUG_HandleTracePoint(SegValue(cs), reg_eip);
	if (DEBUG_HandleRegexpBreakpoint(SegValue(cs), reg_eip)) {
		return true;
	}


	if (mouseBreakpointHit == true) {
		//if (reg_eip != 0x0817 && reg_eip != 0x081A && reg_eip != 0x09AF && reg_eip != 0x09B2 && reg_eip != 0x09BF && reg_eip != 0x09c3 && reg_eip != 0x09df && reg_eip != 0x09dc) {
			mouseBreakpoint    = false;
			mouseBreakpointHit = false;
			return true;
	        /*}
		else {
			mouseBreakpointHit = false;
		} */
	}
	if (outsideStackWriteBreakpointHit) {
		// outsideStackWriteBreakpoint = false;
		//outsideStackWriteBreakpointHit = false;
		// return true;
	}

	if (cpuLog) {
		if (cpuLogCounter>0) {
			LogInstruction(SegValue(cs),reg_eip,cpuLogFile);
			cpuLogCounter--;
		}
		if (cpuLogCounter<=0) {
			cpuLogFile.flush();
			cpuLogFile.close();
			DEBUG_ShowMsg("DEBUG: cpu log LOGCPU.TXT created\n");
			cpuLog = false;
			DEBUG_EnableDebugger();
			return true;
		}
	}
	// LogInstruction
	if (logHeavy) DEBUG_HeavyLogInstruction();
	if (zeroProtect) {
		uint32_t value = 0;
		if (!mem_readd_checked(SegPhys(cs)+reg_eip,&value)) {
			if (value == 0) zero_count++;
			else zero_count = 0;
		}
		if (GCC_UNLIKELY(zero_count == 10)) E_Exit("running zeroed code");
	}

	if (skipFirstInstruction) {
		skipFirstInstruction = false;
		return false;
	}
	if (BPoints.size() && CBreakpoint::CheckBreakpoint(SegValue(cs), reg_eip))
		return true;
	if (BPoints.size() && CBreakpoint::CheckRegBreakpoint())
		return true;

	return false;
}

#endif // HEAVY DEBUG

#endif // DEBUG

// SIS Debug code below

void SIS_Init()
{
	channelIDNames = std::map<std::string, SIS_ChannelID>{
	        {SIS_AnimFrame, SIS_ChannelID::AnimFrame},
	        {SIS_OPL, SIS_ChannelID::OPL},
	        {SIS_Palette, SIS_ChannelID::Palette},
	        {SIS_Script, SIS_ChannelID::Script},
	        {SIS_Script_Verbose, SIS_ChannelID::Script_Verbose},
	        {SIS_Script_Minimal, SIS_ChannelID::Script_Mínimal},
	        {SIS_Pathfinding, SIS_ChannelID::Pathfinding},
	        {SIS_Scaling, SIS_ChannelID::Scaling},
	        {SIS_RLE, SIS_ChannelID::RLE},
	        {SIS_Special, SIS_ChannelID::Special},
	        {SIS_Fileread, SIS_ChannelID::Fileread}};
}

void SIS_PushWord(uint16_t value)
{
	reg_sp -= 2;
	mem_writew_inline(GetAddress(SegValue(ss), reg_sp), value);
}

void SIS_Call(Bitu seg, Bitu off, Bitu retSeg, Bitu retOff)
{
	SIS_PushWord(retSeg);
	SIS_PushWord(retOff);
	SegSet16(cs, seg);
	reg_ip = off;
}

bool SIS_IsKeyPressed(SDL_Scancode scancode)
{
	int numkeys;
	const Uint8* state = SDL_GetKeyboardState(&numkeys);
	return state[scancode];
}

void SIS_HandleGameLoad(Bitu seg, Bitu off)
{
	static bool rightMouseInjected = false;
	// Replace with negative value to skip loading a game
	static int gameToLoad = 2;

	// TODO: Must be close to l0017_0800:
	bool lPressed = SIS_IsKeyPressed(SDL_SCANCODE_L);
	if (seg == 0x01D7 && off == 0x81A && lPressed && !rightMouseInjected) {
		rightMouseInjected = true;
		// Bit #2 is for the right mouse button
		reg_ax = 0x2;
	}

	if (seg == 0x01D7 && off == 0x09D0) {
		rightMouseInjected = false;
		uint16_t menu_mode = mem_readw_inline(GetAddress(0x0227, 0x0FD2));
		if (menu_mode != 0x04) {
			return;
		}

		SIS_PushWord(0x0002);
		// TODO: These probably don't matter that much?
		SIS_PushWord(0x0098);
		SIS_PushWord(0x003B);
		// This next one at least works, but it still fails since the
		// game is in the wrong state (not the menu state)
		CPU_CALL(false, 0x01E7, 0x747E, 0x09D0);
	}
	
}

bool SIS_IsBreakpoint(Bitu seg, Bitu off)
{



	/* static bool hitOnce = false;
	if (seg == 0x01E7 && off == 0xDB56 && !hitOnce) {
	        hitOnce = true;
	        return true;
	} */
	static Bitu memReadWatchSeg = 0x0000;
	static Bitu memReadWatchOff = 0x0000;
	if (memReadWatchHit1 || memReadWatchHit2 ) {
		if (memReadWatchSeg != seg && memReadWatchOff != off) {
			DEBUG_ShowMsg("DEBUG: Memory read breakpoint hit.\n",
			              SIS_filterSegment);
			memReadWatchHit1 = false;
			memReadWatchHit2 = false;
			memReadWatchSeg  = seg;
			memReadWatchOff  = off;
			return true;
		}
	}
	else {
		// TODO: Check with full debug if there is a better alternative for this gating
		memReadWatchSeg = 0x0000;
		memReadWatchOff = 0x0000;
	}
	static bool hitOnce = false;
	if (seg == 0x01E7 && off == 0x747E && !hitOnce) {
		hitOnce = true;
		return true;
	} 


	return false;
}

void SIS_HandleScaling(Bitu seg, Bitu off) {
	
	if (!isChannelActive(SIS_ChannelID::Scaling)) {
		return;
	}
	if (seg != 0x01F7) {
		return;
	}
	

	if (off == 0x1027) {
		// This is where we leave the function
		isChannelActive(SIS_ChannelID::Scaling);
	}
	if (off == 0x0FA6) {
				/*
		l00B7_0FA4:
		;; This is the only read that leads to a pixel being drawn
		mov	al,[si]
		or	al,al
		*/
		fprintf(stdout, "Reading value %.2x from %.4x:%.4x\n", reg_al, SegValue(ds), reg_si);
	}

	if (off == 0x0FC9) {
		/*
		l00B7_0FC9:
		;; We are copying pixels of the character's animation here
		;; This is also where a previusly blacked out pixel for hit detection is
		being written
		;; again
		mov	es:[di],al
		*/
		fprintf(stdout,
		        "Writing value %.2x to %.4x:%.4x\n",
		        reg_al,
		        SegValue(es),
		        reg_di);
	}

	

	




}

void SIS_HandlePathfinding(Bitu seg, Bitu off) {
	bool isInGameCode = seg == 0x01F7 || seg == 0x01E7 ||
	                           seg == 0x01D7 || seg == 0x0217;
	if (!isInGameCode) {
		return;
	}
	// Keep track of any changes to x and y
	static uint16_t oldX = 0xFFFF;
	static uint16_t oldY = 0xFFFF;
	// We just trace any change to find all places where this changes
	// if (seg == 0x01E7 && off == 0x1F4C && isChannelActive(SIS_Pathfinding)) {
	if (isChannelActive(SIS_ChannelID::Pathfinding)) {
		uint16_t x = mem_readw_inline(GetAddress(0x041F, 0x1448));
		uint16_t y = mem_readw_inline(GetAddress(0x041F, 0x144A));
		if (oldX != x || oldY != y) {
			fprintf(stdout, "Pathfinding: Target changed from (%u,%u) to (%u,%u) - %.4x:%.4x\n", oldX, oldY, x, y, seg, off);
			oldX = x;
			oldY = y;
		}
	}
}

void SIS_Temp_HandleSkipDrawObject(Bitu seg, Bitu off)
{
	// Enabling one of the two blocks will skip either the first object or
	// the second object
	/* if (seg == 0x01E7 && off == 0x92E5) {
	        // Skip handling the first object
	        reg_ax = 0x2;
	} */
	/* if (seg == 0x01E7 && off == 0x987C) {
	        reg_ax = 0x02;
	} */
}

void SIS_LogAnimFrame(Bitu seg, Bitu off)
{
	if (seg == 0x01F7 && off == 0x174A) {
		// fprintf(stdout,
		  //       "Results of 1480 call: %.4x:%.4x\n", reg_ax, reg_dx);
		return;
	}

	if (!(seg == 0x01E7 && off == 0x95D2)) {
		return;
	}
	uint16_t index = mem_readw_inline(GetAddress(SegValue(ss), reg_bp - 0x0A));
	if (index != 2) {
		return;
	}

	// Try to mess with what I think is the target width
	// mem_writew_inline(GetAddress(SegValue(ss), reg_sp + 0x0A), 0x40);
	// mem_writew_inline(GetAddress(SegValue(ss), reg_sp + 0x0C), 0x40);

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
	/* fprintf(stdout,
	        "Arguments for 172C call: bp-12h: %.4x, di: %.4x - %.4x %.4x %.4x %.4x %.4x %.4x %.4x %.4x %.4x %.4x %.4x %.4x \n",
	        bp12,
	        reg_di,
	        v1,
	        v2,
	        v3,
	        v4,
	        v5,
	        v6,
	        v7,
	        v8,
	        v9,
	        v10,
	        v11,
	        v12);
			*/
}

void SIS_HandleAnimFrame(Bitu seg, Bitu off)
{
	if (!isChannelActive(SIS_ChannelID::AnimFrame)) {
		return;
	}

	if (seg != 0x01F7) {
		return;
	}

	if (off == 0x1615) {
		// fprintf(stdout, "fn00B7_1480: Results of call: %.4x %.4x\n", reg_ax, reg_dx);
	}
}

void SIS_PrintMemoryRegion(Bitu startSeg, Bitu startOff, Bitu endOff) {
	int num_byte_read = endOff - startOff;
	for (int i = 0; i < num_byte_read; i++) {
		uint8_t current_byte = mem_readb_inline(GetAddress(startSeg, startOff + i));
		fprintf(stdout, "%02x", current_byte);
	}
}

static void SIS_WriteSegRead(Bitu*& reads, int numSegs, Bitu seg)
{
	for (int i = 0; i < numSegs; i++) {
		if (reads[i] == seg) {
			return;
		}
		if (reads[i] == 0x0000) {
			reads[i] = seg;
			return;
		}
	}
}

static void SIS_PrintSegReads(Bitu*& reads, int numSegs, const char* name)
{
	fprintf(stdout, "%s: ", name);
	for (int i = 0; i < numSegs; i++) {
		if (reads[i] != 0x0000) {
			fprintf(stdout, "%.4x, ", reads[i]);
		}
	}
	fprintf(stdout, "\n");
}


void SIS_HandleAnimFramePainting(Bitu seg, Bitu off)
{
	static bool entered = false;
	static bool firstReadDone = false;
	constexpr int numSegs = 10;
	static Bitu mins[numSegs];
	static Bitu maxs[numSegs];
	static int counts[numSegs];
	static Bitu segments[numSegs];

	static Bitu** segReads = new Bitu*[3];
	static bool initialized = false;
	if (!initialized) {
		for (int i = 0; i < 3; i++) {
			segReads[i] = new Bitu[numSegs];
		}
		initialized = true;
	}
	



	if (!isChannelActive(SIS_ChannelID::AnimFrame)) {
		return;
	}
	
	if (seg != 0x01F7) {
		return;
	}

	if (off == 0x0ED1) {
		/* fprintf(stdout,
		        "0x0ED1: Entered\n"); */
		entered = true;
		firstReadDone = false;
		for (int i = 0; i < numSegs; i++) {
			for (int j = 0; j < 3; j++) {
				segReads[j][i] = 0x0000;
			}
			mins[i] = 0xFFFF;
			maxs[i] = 0x0000;
			counts[i] = 0;
			segments[i] = 0xFFFF;
		}
	}
	if (!entered) {
		return;
	}
	Bitu currSeg = 0x0000;
	Bitu currOff = 0x0000;
	if (off == 0x0FC6) {
		firstReadDone = true;
		currSeg       = SegValue(ds);
		currOff       = reg_bx + reg_si;
		SIS_WriteSegRead(segReads[0], numSegs, currSeg);
		/* fprintf(stdout,
		        "0x0FC6: Reading pixel %.2x from %.4x:%.4x\n",
					reg_al, SegValue(ds), reg_bx + reg_si 
		        ); */
	} else if (off == 0x0FA6) // && !firstReadDone)
	{
		firstReadDone = true;
		/* fprintf(stdout,
		        "0x0FA6: Reading pixel %.2x from %.4x:%.4x\n",
		        reg_al,
		        SegValue(ds),
		        reg_si); */
		currSeg = SegValue(ds);
		currOff = reg_si;
		SIS_WriteSegRead(segReads[1], numSegs, currSeg);
	}
	else if (off == 0x0F99)  // && !firstReadDone)
	{
		firstReadDone = true;
		/* fprintf(stdout,
		        "0x0F99: Reading pixel %.2x from %.4x:%.4x\n",
		        reg_al,
		        SegValue(ds),
		        reg_bx);
				*/
		currSeg = SegValue(ds);
		currOff = reg_bx;
		SIS_WriteSegRead(segReads[2], numSegs, currSeg);
	} else if (off == 0x1027) {
		entered = false;
		fprintf(stdout,
			"Reading pixels between: \n");
		for (int i = 0; i < numSegs; i++) {
			if (segments[i] == 0xFFFF) {
				break;
			}
			fprintf(stdout, "%.4x:%.4x and %.4x:%.4x (%u) - values: \n", 
				segments[i],
				mins[i],
				segments[i],
				maxs[i],
				counts[i]				
				);
			SIS_PrintMemoryRegion(segments[i], mins[i], maxs[i]);
			fprintf(stdout, "\n");
			fprintf(stdout, "Locations: \n");
			SIS_PrintSegReads(segReads[0], numSegs, "0x0FC6");
			SIS_PrintSegReads(segReads[1], numSegs, "0x0FA6");
			SIS_PrintSegReads(segReads[2], numSegs, "0x0F99");

		}

		fprintf(stdout, "\n");
		return;
	}
	else {
		return;
	}

	int s = -1;
	for (int i = 0; i < numSegs; i++) {
		if (currSeg == segments[i]) {
			s = i;
			break;
		}
		if (segments[i] == 0xFFFF) {
			segments[i] = currSeg;
			s = i;
			break;
		}
	}

	if (s == -1) {
		fprintf(stdout,
			"Not enough space!\n");
		return;
	}
		
	if (mins[s] > currOff) {
		mins[s] = currOff;
	}
	if (maxs[s] < currOff) {
		maxs[s] = currOff;
	}
	counts[s]++;

	// l00B7_0FA4:
	// mov	al,[si]

	/* if (off == 0x0FC9) {
		// mov es : [di], al
		fprintf(stdout,
		        "Writing pixel %.2x to di: %.4x (%.4x:%.4x)\n",
		        reg_al,
		        reg_di,
		        SegValue(es),
		        reg_di);
	}
	*/
}

void SIS_HandleMouseCursor(Bitu seg, Bitu off) {
	//14 is the eye
	//        ? 15 is the hand 16 is the crosshair 19 is the red cursor 1A is the
	                  //watch
	// TODO: Find a nice place or implement some back-off logic
	// For now using the big "update objects" (?) function
	if (!(seg == 0x01E7 && off == 0x90A2))
	{
		return;
	}
	if (!SIS_IsKeyPressed(SDL_SCANCODE_C)) {
		return;
	}
	uint32_t address = GetAddress(0x0227, 0x0774);
	uint16_t mode = mem_readw_inline(address);
	mode++;
	// Switch around back
	if (mode > 0x16) {
		mode = 0x13;
	}

	// Write back
	mem_writew_inline(address, mode);
}

void SIS_HandleOPL(Bitu seg, Bitu off)
{
	if (seg != 0x01D7) {
		return;
	}

	// Detailed trace between where we read from the song data and the note on
	static TraceHelper traceHelper;
	static bool traceHelperInitialized = false;
	// TODO: This should be possible to do more elegantly, maybe with a lambda?
	if (!traceHelperInitialized) {
		traceHelper.AddTracePoint(0x1ABD);
		traceHelper.AddTracePoint(0x1ACE);
		traceHelper.AddTracePoint(0x1AD3);
		traceHelper.AddTracePoint(0x1ADA);
		traceHelper.AddTracePoint(0x1ADE);
		traceHelper.AddTracePoint(0x1AEF);
		traceHelper.AddTracePoint(0x1AF7);
		traceHelper.AddTracePoint(0x1B00);
		traceHelper.AddTracePoint(0x1B03);
		traceHelper.AddTracePoint(0x1B0C);
		traceHelper.AddTracePoint(0x1B19);
		traceHelper.AddTracePoint(0x1B1A);
		traceHelper.AddTracePoint(0x1B27);
		traceHelper.AddTracePoint(0x1B5F);
		traceHelper.AddTracePoint(0x1B9E);
		traceHelper.AddTracePoint(0x1BA1);
		traceHelper.AddTracePoint(0x1BA7);
		traceHelper.AddTracePoint(0x1BAA);
		traceHelper.AddTracePoint(0x1BDB);
		traceHelper.AddTracePoint(0x1BE1);
		traceHelper.AddTracePoint(0x1BE4);
		traceHelper.AddTracePoint(0x1BE9);
		traceHelper.AddTracePoint(0x1BF3);
		traceHelper.AddTracePoint(0x1BFD);
		traceHelper.AddTracePoint(0x1C09);
		traceHelper.AddTracePoint(0x1C15);
		traceHelper.AddTracePoint(0x1C1A);
		traceHelper.AddTracePoint(0x1C24);
		traceHelper.AddTracePoint(0x1C27);
		traceHelper.AddTracePoint(0x1C44);
		traceHelper.AddTracePoint(0x1C49);
		traceHelper.AddTracePoint(0x1C4C);
		traceHelper.AddTracePoint(0x1C56);
		traceHelper.AddTracePoint(0x1C5D);
		traceHelper.AddTracePoint(0x1C6B);
		traceHelper.AddTracePoint(0x1C7D);
		traceHelper.AddTracePoint(0x1C85);
		traceHelper.AddTracePoint(0x1C8B);
		traceHelper.AddTracePoint(0x1CB1);
		traceHelper.AddTracePoint(0x1CF2);
		traceHelper.AddTracePoint(0x1CFC);
		traceHelper.AddTracePoint(0x1CFF);
		traceHelper.AddTracePoint(0x1DEC);
		traceHelper.AddTracePoint(0x1DF0);
		traceHelper.AddTracePoint(0x1DF6);
		traceHelper.AddTracePoint(0x1DFA);
		traceHelper.AddTracePoint(0x1E91);
		traceHelper.AddTracePoint(0x1E94);
		traceHelper.AddTracePoint(0x1ED1);
		traceHelper.AddTracePoint(0x1F12);
		traceHelper.AddTracePoint(0x1FA9);
		traceHelper.AddTracePoint(0x200C);
		traceHelper.AddTracePoint(0x2010);
		traceHelper.AddTracePoint(0x2095);
		traceHelper.AddTracePoint(0x209B);
		traceHelper.AddTracePoint(0x20A4);
		traceHelper.AddTracePoint(0x20A7);
		traceHelper.AddTracePoint(0x20E1);
		traceHelper.AddTracePoint(0x20E6);
		traceHelper.AddTracePoint(0x20E9);
		traceHelper.AddTracePoint(0x20F3);
		traceHelper.AddTracePoint(0x20FA);
		traceHelper.AddTracePoint(0x2102);
		traceHelper.AddTracePoint(0x2109);
		traceHelper.AddTracePoint(0x210F);
		traceHelper.AddTracePoint(0x2114);
		traceHelper.AddTracePoint(0x211E);
		traceHelper.AddTracePoint(0x2128);
		traceHelper.AddTracePoint(0x2134);
		traceHelper.AddTracePoint(0x2140);
		traceHelper.AddTracePoint(0x2145);
		traceHelper.AddTracePoint(0x214F);
		traceHelper.AddTracePoint(0x2170);
		traceHelper.AddTracePoint(0x2172);
		traceHelper.AddTracePoint(0x21A3);
		traceHelper.AddTracePoint(0x21AC);
		traceHelper.AddTracePoint(0x21B5);
		traceHelper.AddTracePoint(0x21DF);
		traceHelper.AddTracePoint(0x21E8);
		traceHelper.AddTracePoint(0x21EB);
		traceHelper.AddTracePoint(0x221C);
		traceHelper.AddTracePoint(0x222D);
		traceHelper.AddTracePoint(0x2231);
		traceHelper.AddTracePoint(0x2237);
		traceHelper.AddTracePoint(0x2247);
		traceHelper.AddTracePoint(0x2255);
		traceHelper.AddTracePoint(0x2258);
		traceHelper.AddTracePoint(0x225C);
		traceHelper.AddTracePoint(0x2284);
		traceHelper.AddTracePoint(0x2289);
		traceHelper.AddTracePoint(0x228C);
		traceHelper.AddTracePoint(0x2298);
		traceHelper.AddTracePoint(0x22A2);
		traceHelper.AddTracePoint(0x22B7);
		traceHelper.AddTracePoint(0x22BF);
		traceHelper.AddTracePoint(0x22C1);
		traceHelper.AddTracePoint(0x22C5);
		traceHelper.AddTracePoint(0x22E3);
		traceHelper.AddTracePoint(0x22E8);
		traceHelper.AddTracePoint(0x22EB);
		traceHelper.AddTracePoint(0x22F7);
		traceHelper.AddTracePoint(0x2301);
		traceHelper.AddTracePoint(0x2316);
		traceHelper.AddTracePoint(0x231E);
		traceHelper.AddTracePoint(0x2327);
		traceHelper.AddTracePoint(0x2355);
		traceHelper.AddTracePoint(0x235E);
		traceHelper.AddTracePoint(0x237E);
		traceHelper.AddTracePoint(0x2387);
		traceHelper.AddTracePoint(0x238D);
		traceHelper.AddTracePoint(0x23B4);
		traceHelper.AddTracePoint(0x23D9);
		traceHelper.AddTracePoint(0x23DB);
		traceHelper.AddTracePoint(0x23E0);
		traceHelper.AddTracePoint(0x23E7);
		traceHelper.AddTracePoint(0x23E9);
		traceHelper.AddTracePoint(0x23F1);
		traceHelper.AddTracePoint(0x2416);
		traceHelper.AddTracePoint(0x241F);
		traceHelper.AddTracePoint(0x2422);
		traceHelper.AddTracePoint(0x2423);
		traceHelper.AddTracePoint(0x2425);
		traceHelper.AddTracePoint(0x242E);
		traceHelper.AddTracePoint(0x2433);
		traceHelper.AddTracePoint(0x2438);
		traceHelper.AddTracePoint(0x243F);
		traceHelper.AddTracePoint(0x2443);
		traceHelperInitialized = true;
	}
	

	traceHelper.HandleOffset(off);


	// Trace for accesses to the 2250 global
	uint32_t globalSeg;
	uint16_t globalOff;
	SIS_ReadAddress(0x0227, 0x2250, globalSeg, globalOff);
	uint8_t value = mem_readb_inline(GetAddress(globalSeg, globalOff));
	static bool globalLoaded = false;
	bool globalNowLoaded = (SegValue(es) == globalSeg && reg_di == globalOff);
	if ((globalNowLoaded && !globalLoaded) || (off == 0x1A13) || (off == 0x1B1E) || (off == 0x1B2B)) {
		fprintf(stdout, "Global [2250] loaded into ES:DI at %.4X:%.4X, value %.2X, DI: %.4X\n", seg, off, value, reg_di);
		globalLoaded = true;
	} else {
		globalLoaded = globalNowLoaded;
	}


	if (off == 0x1B77) {
		fprintf(stdout, "*** Reading at 1B77: ES:DI: %.4X:%.4X, AL: %.2X\n",
		        SegValue(es),
		        reg_di,
		        reg_al);
	}
	if (off == 0x2A84) {
		fprintf(stdout, "*** Entering 2A80\n");
		SIS_PrintCaller();
		SIS_PrintLocal("Local: ", +0x6, 2);
		SIS_PrintLocal("Local: ", +0x8, 2);
		SIS_PrintLocal("Local: ", +0xA, 2);
	}

	if (!isChannelActive(SIS_ChannelID::OPL)) {
		return;
	}

	// First iteration, write out data channel writes/reads to/from OPL
	// chip, add the caller

	/*

	fn0017_2792 proc
	enter	4h,0h
	mov	al,[bp+8h]
	mov	dx,388h
	out	dx,al
	mov	dl,[bp+6h]
	mov	al,[bp+8h]
	xor	ah,ah
	mov	di,ax
	mov	[di+229Ch],dl
	xor	ax,ax
	mov	[bp-2h],ax
	jmp	27B5h

l0017_27B2:
	inc	word ptr [bp-2h]

l0017_27B5:
	mov	dx,388h
	in	al,dx
	mov	[bp-3h],al
	cmp	word ptr [bp-2h],6h
	jnz	27B2h

l0017_27C2:
	mov	al,[bp+6h]
	mov	dx,389h
	out	dx,al
	xor	ax,ax
	mov	[bp-2h],ax
	jmp	27D3h

l0017_27D0:
	inc	word ptr [bp-2h]

l0017_27D3:
	mov	dx,388h
	in	al,dx
	mov	[bp-3h],al
	cmp	word ptr [bp-2h],24h
	jnz	27D0h

	*/

	// TODO: Consider adding entry and leave markers for this and the VGA one

	if (seg != 0x01D7) {
		return;
	}

	// Handling of setting the data variable in fn0017_294E
	if (off == 0x2976 || off == 0x29E9) {
		SIS_PrintLocal("OPL: Setting data local at %.4x.", -0x2, 2, off);
	}
	else if (off == 0x2A4C) {
		fprintf(stdout, "OPL: Setting data local at %.4x to: %.4x", off, reg_ax);
	}

	// Document the interrupt first
	if (off == 0x1AA7) {
		fprintf(stdout, "OPL: Interrupt handler 1AA7 entered\n");
		return;
	}

	if (off == 0x244C) {
		fprintf(stdout, "OPL: Interrupt handler 1AA7 left\n");
		return;
	}

	if (off == 0x1B6A) {
		fprintf(stdout, "OPL: Setting [bp-6h] opcode to %.2x\n", reg_al);
		return;
	}

	if (off == 0x1B00) {
		fprintf(stdout, "OPL: Early out at 1B00h?\n");
		return;
	}

	if (off == 0x1B0C) {
		uint16_t v1 = mem_readw_inline(GetAddress(SegValue(ds), 0x2254));
		uint16_t v2 = mem_readw_inline(GetAddress(SegValue(ds), 0x2256));
		fprintf(stdout, "OPL: Early out at 1B0Ch? %.4x %.4x\n", v1, v2);
		return;
	}
	
	if (off == 0x2416) {
		uint16_t v1 = mem_readw_inline(GetAddress(SegValue(ds), 0x2254));
		uint16_t v2 = mem_readw_inline(GetAddress(SegValue(ds), 0x2256));
		fprintf(stdout, "OPL: Loop continuation check %.4x %.4x\n", v1, v2);
		return;
	}



	if (off == 0x1B2E) {
		fprintf(stdout, "OPL: Reading %.2x from %.4x:%.4x\n", reg_al, SegValue(es), reg_di);
		return;
	}

	uint32_t ret_seg;
	uint16_t ret_off;
	SIS_GetCaller(ret_seg, ret_off);

	if (off == 0x279C) { // || off == 0x27C8)
	
	
		uint8_t value = mem_readb_inline(GetAddress(SegValue(ss), reg_bp + 0x06));
		uint8_t registerIndex = mem_readb_inline(GetAddress(SegValue(ss), reg_bp + 0x08));

		if (ret_seg == 0x01D7 && ret_off == 0x275F) {
			// Filter out the big initialization where we set all registers to 0
			return;
		}
	
			
		// Outs
		fprintf(stdout,
		        "OPL: Write %.2x to port %.2x (caller: %.4x:%.4x - %.8x) - %4.x:%.4x\n",
		        value,
				registerIndex,
		        ret_seg,
		        ret_off,
		        cycle_count,
				SegValue(ds),
				reg_si);
		std::string output = SIS_OpcodeID::IdentifyOPLWrite(registerIndex,
		                                                    value);
		fprintf(stdout, "%s\n", output.c_str());

	} /* else if (off == 0x2789 || off == 0x27D7) {
	        // Ins
	        fprintf(stdout,
	                "OPL: Read %.2x from port %.4x (caller: %.4x:%.4x -
	%.8x)\n", reg_al, reg_dx, ret_seg, ret_off, cycle_count);
	} */
}

void SIS_HandlePalette(Bitu seg, Bitu off) {
	if (!isChannelActive(SIS_ChannelID::Palette)) {
		return;
	}

	/*
	l00B7_0156:
	mov	dx,3C9h
	lodsb
	sub	al,bl
	jnc	0160h

l00B7_015E:
	xor	al,al

l00B7_0160:
	out	dx,al
	*/

	if (seg == 0x01F7 && off == 0x012F) {
		fprintf(stdout,
		        "VGA: Function entered.\n");
		return;
	}
	if (seg == 0x01F7 && off == 0x01A3) {
		fprintf(stdout, "VGA: Function left.\n");
		return;
	}

	if (!(seg == 0x01F7 && off == 0x0160)) {
		return;
	}
	uint32_t ret_seg;
	uint16_t ret_off;
	SIS_GetCaller(ret_seg, ret_off);

	fprintf(stdout,
	        "VGA: Write %.2x to port %.4x (caller: %.4x:%.4x - %.8x)\n",
	        reg_al,
	        reg_dx,
	        ret_seg,
	        ret_off,
			cycle_count);




	// First iteration, write out the index, the color, the CPU cylces and the caller
}

void SIS_HandleScaleChange(Bitu seg, Bitu off) {
	if ((seg == 0x01F7) && (off == 0x0ED5)) {
		// Inject the value
		mem_writew_inline(GetAddress(SegValue(ss), reg_bp + 0x12), 0x64 * 3);
	}
}

void SIS_HandleSIS(Bitu seg, Bitu off)
{
	// SIS_Temp_HandleSkipDrawObject(seg, off);
	SIS_LogAnimFrame(seg, off);
	SIS_HandleAnimFrame(seg, off);
	SIS_HandleAnimFramePainting(seg, off);
	// SIS_HandleGameLoad(seg, off);
	SIS_HandleMouseCursor(seg, off);
	SIS_HandlePalette(seg, off);
	// SIS_HandleOPL(seg, off);
	// SIS_HandlePathfinding(seg, off);
	// SIS_HandlePathfinding2(seg, off);
	SIS_HandlePathfinding3(seg, off);
	SIS_HandleScaling(seg, off);
	// SIS_HandleScaleChange(seg, off);
	SIS_HandleSkip(seg, off);
	// SIS_HandleInventoryIcons(seg, off);
	// SIS_HandleDrawingFunction(seg, off);
	// SIS_HandleDataLoadvFunction(seg, off);
	// SIS_HandleBlobLoading(seg, off);
	SIS_HandleRLEDecoding(seg, off);
	SIS_HandlePaletteChange(seg, off);
	SIS_HandleCharacterPos(seg, off);
	// SIS_HandleStopWalking(seg, off);
	// SIS_HandleCharacterDrawing(seg, off);
	// SIS_Handle1480(seg, off);
	// SIS_HandleBGAnimDrawing(seg, off);
	// SIS_HandleSkippedCode(seg, off);
	SIS_HandleMovementSpeedMod(seg, off);
}

void SIS_WipeMemory(Bitu seg, Bitu off, int length, uint8_t value) {
	for (int i = 0; i < length; i++) {
		mem_writeb_inline(GetAddress(seg, off + i), value);
	}
}

void SIS_WipeMemoryFromTo(Bitu seg, Bitu off, Bitu off_end, uint8_t value) {
	Bitu length = off_end - off;
	SIS_WipeMemory(seg, off, length, value);
}

void SIS_BeginBuffering() {
	/* 
	// Set the buffer for stdout
	if (setvbuf(stdout, stdout_buffer, _IOFBF, sizeof(stdout_buffer)) != 0) {
		std::cerr << "Failed to set buffer for stdout" << std::endl;
	}
	// We start buffering on each entry of the scripting function and each time we
	// end the execution of an opcode
	// We end buffering either on realizing that the opcode is not a branching one or after
	// we have finished the execution of the opcode
	// First step: Always print and make sure the script is the same output,
	// then start adding conditions
	*/

}

void SIS_EndBuffering(bool print) {
	/* if(print) {
		// Flush the buffer manually
		fflush(stdout);
	} else {
		// Optionally clear the buffer by resetting it
		std::memset(stdout_buffer, 0, sizeof(stdout_buffer));
		// Prevent any buffered content from being output
		setvbuf(stdout, nullptr, _IONBF, 0); // Set no buffering mode to
		                                     // ignore current buffer
		setvbuf(stdout, stdout_buffer, _IOFBF, sizeof(stdout_buffer)); // Re-apply
		                                                        // buffer
	}

	// Reset the buffer mode to default
	setvbuf(stdout, nullptr, _IOLBF, 0);
	fprintf(stdout, "Buffering ended, skipping detected: %.1x\n", !print);
	*/
	const char* prefix = print ? "" : "** ";
	for (std::string str : DebugStrings) {
		fprintf(stdout, "%s %s", prefix, str.c_str());
	}
	// Reset the buffer
	DebugStrings.clear();
}

void SIS_GetCaller(uint32_t& out_seg, uint16_t& out_off, uint16_t num_levels /*= 1*/)
{
	// At SS:BP, the old stack frame is saved, so we can use this to
	// walk up
	// TODO: Only works after enter has been executed
	uint32_t calleeBP = reg_bp;
	uint32_t callerBP = mem_readw_inline(
	        GetAddress(SegValue(ss), calleeBP + 0x00));

	out_off = mem_readw_inline(GetAddress(SegValue(ss), calleeBP + 0x02));
	out_seg = mem_readw_inline(GetAddress(SegValue(ss), calleeBP + 0x04));

	while (num_levels != 1) {
		// If need be, walk up again the same way we did before,
		// just that we read the caller's BP from the stack
		calleeBP = callerBP;
		callerBP = mem_readw_inline(GetAddress(SegValue(ss), calleeBP + 0x00));
		out_off = mem_readw_inline(GetAddress(SegValue(ss), calleeBP + 0x02));
		out_seg = mem_readw_inline(GetAddress(SegValue(ss), calleeBP + 0x04));
		num_levels--;
	}
}

void SIS_PrintCaller(uint16_t num_levels) {
	uint32_t seg;
	uint16_t off;
	SIS_GetCaller(seg, off, num_levels);
	fprintf(stdout, "-- Caller (%d): %.4x:%.4x\n", num_levels, seg, off);
}

void SIS_ReadAddressFromLocal(int16_t offset, uint32_t& outSeg, uint16_t& outOff)
{
	outSeg = SIS_GetLocalWord(offset + 0x2);
	outOff = SIS_GetLocalWord(offset);
}

void SIS_ReadAddress(uint32_t seg, uint16_t off, uint32_t& outSeg, uint16_t& outOff)
{
	outSeg = mem_readw_inline(GetAddress(seg, off+2));
	outOff = mem_readw_inline(GetAddress(seg, off));
}

void SIS_WriteAddress(uint32_t seg, uint16_t off, uint32_t outSeg, uint16_t outOff)
{
	mem_writew_inline(GetAddress(seg, off+2), outSeg);
	mem_writew_inline(GetAddress(seg, off), outOff);
}

void SIS_HandleSkip(Bitu seg, Bitu off) {
	if (seg != 0x01E7) {
		return;
	}
	// Read opcode and length
	// If we have the opcode: Set offset to new value, set instruction pointer to new iteration of
	// the loop

	// DB9C
	// Opcode is in al
	// Length is in mov	[bp-2h],al
	if (off == 0xDB9C) {
		uint8_t opcode = reg_al;
		uint8_t length =  mem_readb_inline(GetAddress(SegValue(ss), reg_bp + -0x02));
		if (opcode == SIS_skippedOpcode) {
			// TODO: Check if this does the right thing or maybe some state remains
			CPU_JMP(false, SegValue(cs), 0xDB73, reg_eip);
			uint16_t script_offset = mem_readw_inline(GetAddress(SegValue(ds), 0x0F8A));
			// TODO: Confirm length - inclusive or exclusive of two already read values?
			script_offset += length;
			mem_writew_inline(GetAddress(SegValue(ds), 0x0F8A), script_offset);
		}
	}
}

void SIS_HandleInventoryIcons(Bitu seg, Bitu off) {
	if (!(seg == 0x01E7 && off == 0x3AD8)) {
		return;
	}

	fprintf(stdout,
	        "Inventory Icons: %.4x %.4x %.4x %.4x\n",
	        SIS_GetLocalWord(0x6),
	        SIS_GetLocalWord(0x8),
	        SIS_GetLocalWord(0xA),
	        SIS_GetLocalWord(0xC)
		);

}

void SIS_HandleDrawingFunction(Bitu seg, Bitu off) {
	if (!(seg == 0x01F7 && off == 0x0C8C)) {
		return;
	}
	uint32_t caller_seg;
	uint16_t caller_off;
	SIS_GetCaller(caller_seg, caller_off);

	fprintf(stdout,
	        "01F7:088C: %.4x %.4x %.4x %.4x - caller %.4x:%.4x\n",
	        SIS_GetLocalWord(0x6),
	        SIS_GetLocalWord(0x8),
	        SIS_GetLocalWord(0xA),
	        SIS_GetLocalWord(0xC),
			caller_seg, caller_off);

}

void SIS_HandleDataLoadFunction(Bitu seg, Bitu off) {
	static uint16_t length;
	if (seg == 0x01E7 && off == 0x0ACC) {
		// Read the length
		length = mem_readw_inline(GetAddress(SegValue(es), reg_di + 0x04));
		return;
	}
	if (seg == 0x01E7 && off == 0x0AD5) {
		
		fprintf(stdout,
		        "01E7:0ACC: es:[di+4h]: %.4x - loaded to %.4x:%.4x\n",
		        length,
				reg_ax,
				reg_dx);
		return;
	}
	
	if (!(seg == 0x01E7 && off == 0x0A3E)) {
		return;
	}
	uint32_t caller_seg;
	uint16_t caller_off;
	SIS_GetCaller(caller_seg, caller_off);

	fprintf(stdout,
	        "01E7:0A3E: [bp-3h]: %.4x %.4x\n",
	        SIS_GetLocalWord(0x6),
	        SIS_GetLocalWord(-0x3)
	);

}

void SIS_HandleBlobLoading(Bitu seg, Bitu off) {
	if (seg == 0x01E7 && off == 0x08F0) {
		// The index of the object lives in bp+6h at this point
		uint32_t objectIndex = SIS_GetLocalWord(+0x6);
		fprintf(stdout,
		        "01E7:08A0: Object index = %.4x\n",
		       objectIndex);
		return;
	}
	if (seg == 0x01E7 && off == 0x0A3E) {
		uint32_t blobIndex = SIS_GetLocalWord(-0x3);
		fprintf(stdout, "01E7:0A3E: Blob index = %.4x\n", blobIndex);
		return;
	
	}

}

void SIS_HandleRLEDecoding(Bitu seg, Bitu off) {
	
	if (!isChannelActive(SIS_ChannelID::RLE)) {
		return;
	}

	static FILE* fp = fopen("rle_out.txt", "w");

	if (seg != 0x01E7) {
		return;
	}

	if (off == 0x06C9) {
		uint16_t numRow = SIS_GetLocalWord(-0x2);
		// TODO: No idea why the offset is so different between the deassembly
		// and the runtime
		uint16_t length = SIS_GetLocalWord(-0x0325);
		fprintf(fp, "RLE: Row %.4x with %.4x bytes of data.\n", numRow, length);
		return;
	}

	if (off == 0x06FC) {
		uint8_t value = reg_al;
		uint16_t remainingData = reg_bx;
		fprintf(fp, "RLE: Literal pixel %.2x, remaining row data %.4x bytes.\n", value, remainingData);
		return;
	}
	
	if (off == 0x0706) {
		uint8_t numReps = reg_cl;
		uint8_t value = reg_al;
		uint16_t remainingData = reg_bx;
		fprintf(fp,
		        "RLE: Encoded pixel %.2x for %.2x reps, remaining row data %.4x bytes.\n",
		        value,
				numReps,
		        remainingData);
		return;
	}
}

void SIS_HandlePaletteChange(Bitu seg, Bitu off) {
	if (!(seg == 0x01F7 && off == 0x0139)) {
		return;
	}
	uint16_t p1 = SIS_GetLocalWord(+0x0C);
	uint16_t p2 = SIS_GetLocalWord(+0x0E);
	uint32_t callerSeg;
	uint16_t callerOff;
	SIS_GetCaller(callerSeg, callerOff);

	fprintf(stdout, "Setting palette from %.4x:%.4x, caller %.4x:%.4x\n", p1, p2, callerSeg, callerOff);
}

void SIS_HandleStopWalking(Bitu seg, Bitu off) {
	if (!seg == 0x01E7) {
		return;
	}
	switch (off) {
		case 0x1966: fprintf(stdout, "Entering 1966\n"); break;
	case 0x1F40:
		{		uint16_t posX = mem_readw_inline(
		                GetAddress(SegValue(es), reg_di));
		        fprintf(stdout, "Decreasing x position %.4x\n", posX);
	}

			break;
	    case 0x2317: fprintf(stdout, "Setting [1020] to true\n"); break;
	}
	// fn0037_1966 proc: Function start
	
	// l0037_1F40: Decrease the x

	// l0037_2317: Setting 1020 to true

}

void SIS_DrawImage(Bitu seg, Bitu off) {
	
	constexpr auto palette_map = vga.dac.palette_map;
	// TODO: Get actual values

	const auto graphics_window = GFX_GetSDLWindow();
	const auto graphics_surface = SDL_GetWindowSurface(
		graphics_window);

	int length = 500;
	int width  = 50;
	for (int i = 0; i < length; i++) {
		uint16_t x = i % width;
		uint16_t y = i / width;

		

		uint32_t value = mem_readb(
			    GetAddress(seg, off + i));
		Uint32* target_pixel =
			    (Uint32*)((Uint8*)graphics_surface->pixels +
			                2 * y * graphics_surface->pitch +
			                2 * x * graphics_surface->format->BytesPerPixel);
		*target_pixel = *(palette_map + value);

		target_pixel =
			    (Uint32*)((Uint8*)graphics_surface->pixels +
			                2 * y * graphics_surface->pitch +
			                2 * x * graphics_surface->format->BytesPerPixel +
			                graphics_surface->format->BytesPerPixel);
		*target_pixel = *(palette_map + value);

		target_pixel =
			    (Uint32*)((Uint8*)graphics_surface->pixels +
			                2 * y * graphics_surface->pitch +
			                graphics_surface->pitch +
			                2 * x * graphics_surface->format->BytesPerPixel);
		*target_pixel = *(palette_map + value);

		target_pixel =
			    (Uint32*)((Uint8*)graphics_surface->pixels +
			                2 * y * graphics_surface->pitch +
			                graphics_surface->pitch +
			                2 * x * graphics_surface->format->BytesPerPixel +
			                graphics_surface->format->BytesPerPixel);
		*target_pixel = *(palette_map + value);
	}
	// Update the surface
	SDL_UpdateWindowSurface(graphics_window);

	// Focus
	SDL_RaiseWindow(graphics_window);

}

void SIS_HandleSkippedCode(Bitu seg, Bitu off) {
	static Bitu skipSeg = 0x01F7;
	static Bitu skipStartOff = 0x15EF;
	static Bitu skipEndOff   = 0x1601;
	constexpr bool checkSkipCondition = false;
	if (checkSkipCondition && seg == skipSeg && off == skipStartOff) {
		reg_eip = skipEndOff;
	}
}

void SIS_DumpPalette() {
	constexpr auto palette = vga.dac.palette_map;
	for (int i = 0; i < 256; i++) {
		uint32_t currentEntry = palette[i];
		fprintf(stdout, "Palette color %u: %.4x\n", i, currentEntry);
	}
}

std::string SIS_IdentifyScriptOpcode(uint8_t opcode, uint8_t opcode2)
{
	// TODO: Remove source code below when everything works
	return SIS_OpcodeID::IdentifyScriptOpcode(opcode, opcode2);
	// TODO: Initialize in a central space
	// TODO: Clean up based on how the function actually works
	// We handle it this way:
	// ids has all opcodes for 1-4 and 6-end
	// second_ids has those for the combination 5-1 through 5-6
	std::vector<std::string> ids;
	std::vector<std::string> second_ids;
	for (int i = 0; i < 100; i++) {
		ids.push_back("Unknown opcode");
		second_ids.push_back("Unknown opcode");
	}
	second_ids[0x1] = "Skip if two times 9F4D results differ";

	ids[0x04] = "Skip if any 9F4D result is non-zero";
	ids[0x0B] = "Load and place object in scene";
	ids[0x0D] = "Simple speech act";
	ids[0x0F] = "Wait for specific duration";
	ids[0x10] = "Move to target location";
	ids[0x11] = "Wait for object to reach target location";
	ids[0x12] = "First guess: Adjustment to pathfinding information";
	ids[0x15] = "TBC: Start a set of dialogue option";
	ids[0x16] = "Add a dialogue option";
	ids[0x18] = "Skip to end of script";

	if (opcode != 0x05) {
		return ids[opcode];
	}
	else if (opcode2 <= 0x06) {
		return second_ids[opcode2];
	}
	else {
		return ids[opcode];
	}
}

std::string SIS_IdentifyHelperOpcode(uint8_t opcode, uint16_t value)
{
	// TODO: Remove source code below when everything works
	return SIS_OpcodeID::IdentifyHelperOpcode(opcode, value);
	// Opcode is [bp-5h]
	// Value is [bp-7h]
	
	if (opcode == 0x00) {
		return "Return the constant read value";
	}

	// l0037_9F72:
	if (opcode > 0) {
		// l0037_9F78:
		if (opcode < 0xFF) {
			// TODO: This still feels off since it should not be
			// possible
			if ((value < 1) || (value > 0x800)) {
				return "Unknown opcode - value combination";
			} else {
				return "Read and return value of script variable";
			}
		}
	}
	// l0037_9FAE:
	if (opcode != 0xFF) {
		return "Unknown opcode - value combination";
	}
	// We are starting to execute opcode FFh here
	// l0037_9FB7:
	if (value == 0x1) {
		// l0037_9FBF:
		// TODO: Does the output depend on the mouse mode?
		return "TBC: Return interacted object - also true for all modes?";
		// l0037_A050:
	} else if (value == 0x4) {
		// TODO: What's the difference to 0x27?
		return "TBC: Result of running 101D on the character's position";
	} else if (value == 0x6) {
		return "Return constant 1 0";
	} 
	else if (value == 0x7) {
		return "Unknown opcode - value combination";
	} else if (value == 0x0b) {
		return "Return value of [1012h] global";
	} else if (value == 0x26) {
		return "Return value of [1014h] global";
	}
	else if (value == 0x2F) {
		return "Return current scene index in first result";
	} 
	return "Unknown opcode - value combination";
}

void SIS_CopyImageToClipboard(uint16_t width, uint16_t height, uint8_t* pixels)
{
	// COLORREF* colors = new COLORREF[width * height];

	// Create bitmap
	HBITMAP hBitmap = CreateBitmap(width, height, 1, 32, (void*)pixels);

	// delete[] colors;
	SDL_SysWMinfo wmInfo;
	SDL_VERSION(&wmInfo.version);
	const auto graphics_window = GFX_GetSDLWindow();
	SDL_GetWindowWMInfo(graphics_window, &wmInfo);
	HWND hwnd = wmInfo.info.win.window;

	if (!OpenClipboard(hwnd)) {
		DEBUG_ShowMsg("DEBUG: Error opening clipboard\n");
	} else {
		if (!EmptyClipboard()) {
			DEBUG_ShowMsg("DEBUG: Error emptying clipboard\n");
		}

		else if (!SetClipboardData(CF_BITMAP, hBitmap)) {
			DEBUG_ShowMsg("DEBUG: Error setting bitmap on clipboard\n");
		}

		// else
		//     hMem = NULL; // clipboard now owns the memory

		CloseClipboard();
	}


}

uint16_t lastFramePosX;
uint16_t lastFramePosY;
uint8_t lastFrameColor;

void SIS_HandleCharacterPos(Bitu seg, Bitu off) {
	if (seg != 0x01E7) {
		return;
	}
	if (off != 0x1B8F) {
		return;
	}

	// Reset the last frame's pixel
	mem_writeb_inline(GetAddress(0xA000, lastFramePosY * 320 + lastFramePosX), lastFrameColor);

	uint32_t protSeg;
	uint16_t protOff;
	// We shift left by 2 = *4
	SIS_ReadAddress(0x227, 0x77C + 0x1 * 4, protSeg, protOff);
	uint16_t charX = mem_readw_inline(GetAddress(protSeg, protOff + 0x0));
	uint16_t charY = mem_readw_inline(GetAddress(protSeg, protOff + 0x2));
	uint16_t charOrientation = mem_readw_inline(
	        GetAddress(protSeg, protOff + 0x6));
	// fprintf(stdout, "Player orientation: %.4x\n", charOrientation);
	
	lastFramePosX = charX;
	lastFramePosY = charY;
	lastFrameColor = mem_readb_inline(GetAddress(0xA000, charY * 320 + charX));

	mem_writeb_inline(GetAddress(0xA000, charY * 320 + charX), 0xFF); 

	// TODO: Find a good place (after character drawing)
	// Write at least one pixel for the position (maybe a cross)
	/*
		;; Note: This addresses the data of an object, e.g. the protagonist living at 03E7:078C
	mov	di,[bp-2h]
	shl	di,2h
	les	di,[di+77Ch]
	*/
}

void SIS_ChangeMapPointerToBackground(uint16_t localOffset) {
	// TODO: Handle scene changes? Probably no need, will reset during loading
	// but might stay persistently

	// Load the scene data
	uint32_t sceneSeg;
	uint16_t sceneOff;
	// TODO: Check if I maybe got the 32bit segment wrong
	SIS_ReadAddress(0x0227, 0x0778, sceneSeg, sceneOff);
	// Save the original background (if not already changed)
	if (localOffset == 0) {
		// We want to reset the background
		// TODO: Implement this edge case
	}
	if (!bgOriginalChanged) {
		bgOriginalChanged = true;
		SIS_ReadAddress(sceneSeg, sceneOff + 0x1 * 0x4, bgOriginalSeg, bgOriginalOff);
	}

	// uint32_t newSeg;
	// uint16_t newOff;

	
	// SIS_ReadAddress(sceneSeg, sceneOff + localOffset, newSeg, newOff);
	for (int y = 0; y != 240; y++) {
		// TODO: Make a pointer struct or check if there is one already
		uint32_t rowSeg;
		uint16_t rowOff;
		SIS_ReadAddress(sceneSeg, sceneOff + localOffset + y * 0x4, rowSeg, rowOff);
		uint32_t targetRowSeg;
		uint16_t targetRowOff;
		
		SIS_ReadAddress(0x0227, 0x245C + y * 0x4, targetRowSeg, targetRowOff);
		for (int x = 0; x != 320; x++)
		{
		
			uint8_t value = mem_readb_inline(
			        GetAddress(rowSeg, rowOff + x));
			// Copy to VGA memory for instant result and into the backbuffer so it does
			// not get overwritten
			mem_writeb_inline(GetAddress(0xA000, 320 * y + x), value);
			mem_writeb_inline(GetAddress(targetRowSeg, targetRowOff + x), value);
		} 

	}
	// SIS_WriteAddress(sceneSeg, sceneOff + 0x00, newSeg, newOff);
	// mem_writew_inline(GetAddress(0x0227, 0x0770), newSeg);
	// mem_writew_inline(GetAddress(0x0227, 0x0772), newOff);

	// Load the pointer from the local offset
	// Overwrite the background image pointer with the pointer

	// Current scene data access - example pathfinding map

	/*
		les	di,[0778h]

	mov	ax,es:[di+2017h]
	mov	dx,es:[di+2019h]
	*/


}

void SIS_ResetBackground() {
	// TODO: Use the saved background to reset it
	if (!bgOriginalChanged) {
		return;
	}
	
	uint32_t sceneSeg;
	uint16_t sceneOff;
	// TODO: Check if I maybe got the 32bit segment wrong
	SIS_ReadAddress(0x0227, 0x0778, sceneSeg, sceneOff);
	SIS_WriteAddress(sceneSeg, sceneOff + 0x0, bgOriginalSeg, bgOriginalOff);
}

void SIS_ReadImageToPixels(Bitu seg, Bitu off, uint16_t& width,
                           uint16_t& height, uint8_t*& pixels)
{
	width = mem_readw_inline(GetAddress(seg, off + 0x0));
	height = mem_readw_inline(GetAddress(seg, off + 0x2));
	uint16_t size_pixels = width * height * 4;
	pixels               = new uint8_t[size_pixels];

	constexpr auto palette_map = vga.dac.palette_map;
	uint32_t* cur = (uint32_t*) pixels;
	for (int i = 0; i < size_pixels / 4; i++) {
		uint8_t value = mem_readb_inline(GetAddress(seg, off + 0x04 + i));
		uint32_t result = palette_map[value];
		if (value != 0) {
			result |= 0xFF000000;
		}
		cur[i] = result;
	}
}

void SIS_HandleCharacterDrawing(Bitu seg, Bitu off) {
	if (seg != 0x01E7) {
		return;
	}
	
	switch (off) {
	case 0x92E5: fprintf(stdout, "Starting loop at 92E5\n");
		break;
	case 0x92ED: {
		uint16_t i = SIS_GetLocalWord(-0x0A);
		fprintf(stdout, "Loop iteration with counter %.4x\n", i);
	} break;
	case 0x958F: {
		uint16_t value = SIS_GetLocalWord(-0x12);

		uint32_t currentSeg;
		uint16_t currentOff;
		SIS_ReadAddress(SegValue(es), reg_di + 0x2C, currentSeg, currentOff);
		fprintf(stdout,
		        "Setting pointer for local variable %.4x to %.4x:%.4x\n",
		        value,
		        currentSeg,
		        currentOff);
	} break;
	}
}

void SIS_HandleBGAnimDrawing(Bitu seg, Bitu off) {
	if (seg != 0x01E7) {
		return;
	}

	if (off == 0x99D3) {
		SIS_PrintLocal("BG Anims: Loop entered for index", -0x0C, 2);
	}
	if (off == 0x9A18) {
		SIS_PrintLocal("BG Anims: Drawing part for index", -0x0C, 2);
	}

}

void SIS_HandleMovementSpeedMod(Bitu seg, Bitu off) {
	// TODO: Would be better to remove this one since it will only be used once
	static bool modApplied = false;
	if (modApplied) {
		return;
	}
	if (seg == 0x01E7 && off == 0x1C1D) {
		constexpr uint8_t factor = 0x32;
		mem_writeb_checked(GetAddress(0x01E7, 0x1C1F), factor);
		modApplied = true;
	}

}

void SIS_HandlePathfinding2(Bitu seg, Bitu off) {
	// Newer code for more detailed analysis of the pathfinding
	if (seg != 0x01E7) {
		return;
	}

	{
		static uint16_t index;
		static uint16_t x1;
		static uint16_t y1;
		static uint16_t x2;
		static uint16_t y2;

		// 0037:19CC - gather stack and index
		// 0037 : 19D1 - gather the result

		if (off == 0x19CC) {
			index = SIS_GetLocalWord(-0x6);
			x1    = SIS_GetStackWord(0x06);
			y1    = SIS_GetStackWord(0x04);
			x2 = SIS_GetStackWord(0x02);
			y2 = SIS_GetStackWord(0x00);
			return;
		}

		if (off == 0x19D1) {
			uint8_t result = reg_al;
			SIS_Debug("--- Calling 1196 on %u: (%u,%u) - (%u,%u) - result: %.2x\n",
				index, x1, y1, x2, y2, result);
			return;
		}

	}
	if (off == 0x119A) {
		SIS_Debug("--- Entering 1196 function\n");
		SIS_PrintCaller();
		SIS_PrintLocal("x1: ", SIS_Arg4, 2);
		SIS_PrintLocal("y1: ", SIS_Arg3, 2);
		SIS_PrintLocal("x2: ", SIS_Arg2, 2);
		SIS_PrintLocal("y2: ", SIS_Arg1, 2);
		return;
	}

	if (off == 0x196A) {
		SIS_Debug("--- Entering pathfinding function\n");
		SIS_PrintCaller();

		SIS_PrintLocal("[0776h]: ", SIS_Arg5, 2);
		SIS_PrintLocal("x1: ", SIS_Arg4, 2);
		SIS_PrintLocal("y1: ", SIS_Arg3, 2);
		SIS_PrintLocal("x2: ", SIS_Arg2, 2);
		SIS_PrintLocal("y2: ", SIS_Arg1, 2);
		return;
	}

	{
		static uint16_t index;
		static uint16_t x1;
		static uint16_t y1;
		static uint16_t x2;
		static uint16_t y2;


		if (off == 0x1A39) {
			index = SIS_GetLocalWord(-0x6);
			x1    = SIS_GetStackWord(0x06);
			y1    = SIS_GetStackWord(0x04);
			x2    = SIS_GetStackWord(0x02);
			y2    = SIS_GetStackWord(0x00);
			return;
		}
		
		if (off == 0x1A3E) {
			uint16_t result = reg_ax;
			SIS_Debug("--- Calling 1390 on %u: (%u,%u) - (%u,%u) - result: %u\n",
			          index,
			          x1,
			          y1,
			          x2,
			          y2,
			          result);
			return;
		}
		
	}
	{
		// Call: 0037:1A4F
		// Result: 1A54
		// Comparison with bp-2 0037:1A57 
		static uint16_t index;
		static uint16_t x1;
		static uint16_t y1;
		static uint16_t x2;
		static uint16_t y2;

		if (off == 0x1A4F) {
			index = SIS_GetLocalWord(-0x6);
			x1    = SIS_GetStackWord(0x06);
			y1    = SIS_GetStackWord(0x04);
			x2    = SIS_GetStackWord(0x02);
			y2    = SIS_GetStackWord(0x00);
			return;
		}

		if (off == 0x1A54) {
			uint16_t result = reg_ax;
			SIS_Debug("--- Calling 1390 on %u: (%u,%u) - (%u,%u) - result: %u\n",
			          index,
			          x1,
			          y1,
			          x2,
			          y2,
			          result);
			return;
		}

		if (off == 0x1A57) {
			uint16_t total = reg_ax;
			uint16_t comparison = SIS_GetLocalWord(-0x2);
			SIS_Debug("Checking if total %u < %u.\n",
			          total, comparison);
			return;
		}
	}

	if (off == 0x12D2) {
		// Handle x, y for both and the abolute differences
		uint16_t x1 = SIS_GetLocalWord(SIS_Arg1);
		uint16_t y1 = SIS_GetLocalWord(SIS_Arg2);
		uint16_t x2 = SIS_GetLocalWord(SIS_Arg3);
		uint16_t y2 = SIS_GetLocalWord(SIS_Arg4);

		uint32_t absDiffX = SIS_GetLocalDoubleWord(-0x9);
		uint32_t absDiffY = SIS_GetLocalDoubleWord(-0xD);

		double result = sqrt(absDiffX * absDiffX + absDiffY * absDiffY);

		SIS_Debug("Inputs are (%u,%u), (%u,%u)\n", x1, y1, x2, y2);
		SIS_Debug("Absolute difference: (%u,%u)\n", absDiffX, absDiffY);
		SIS_Debug("Result should be: %f\n", result);
		return;
	}
	
	if (off == 0x130E) {
		uint32_t lookup = (reg_bx << 16) + reg_cx;
		SIS_Debug("First lookup: %u (%.4x%.4x)\n", lookup, reg_bx, reg_cx);
		return;
	}

	if (off == 0x1323) {
		uint32_t lookup = (reg_dx << 16) + reg_ax;
		SIS_Debug("Second lookup: %u (%.4x%.4x)\n", lookup, reg_dx, reg_ax);
		return;
	}

	if (off == 0x132D) {
		uint32_t referenceValue = (SIS_GetLocalWord(-0x0F) << 16) + SIS_GetLocalWord(-0x11);
		SIS_Debug("Reference value: %u (%.8x)\n", referenceValue, referenceValue);
		return;
	}

	if (off == 0x134A) {
		uint16_t rightShifted = SIS_GetLocalWord(-0x1E);
		uint16_t leftShifted  = reg_ax;
		
		SIS_Debug("Right shifted: %u (%.4xh) - Left shifted: %u (%.4xh)\n", rightShifted, rightShifted, leftShifted, leftShifted);
		return;
	}

	if (off == 0x1359) {
		SIS_Debug("Looked up value: %u %.4x%.4xh\n", (reg_dx << 16) + reg_ax, reg_dx, reg_ax);
		return;
	}

	{
	// Length function from here on
	
		static bool lookupSmallerThanSum;
		static char* comparisonString;
		if (off == 0x1365) {
			lookupSmallerThanSum = false;
			comparisonString     = "smaller";
			return;
		}
		if (off == 0x1371) {
			lookupSmallerThanSum = true;
			comparisonString     = "larger";
			return;
		}

		if (off == 0x137B) {
			uint32_t total = SIS_GetLocalDoubleWord(-0x15);
			SIS_Debug("Looked up value was %s than the reference, new total: %u (%.8xh)\n",
			          comparisonString,
				      total,
			          total);
			return;
		}
	}
}

void SIS_HandlePathfinding3(Bitu seg, Bitu off) {
	if (seg != 0x01E7) {
		return;
	}

	if (off == 0x15AC) {
		SIS_Debug("--- Entering 15A8\n");
		SIS_PrintLocal("TODO: Figure out", SIS_Arg3, 2);
		SIS_PrintLocal("Index of point: ", SIS_Arg2, 2);
		SIS_PrintLocal("Caller BP:", SIS_Arg1, 2);
		return;
	}

	// One byte per value
	if (off == 0x15FD) {
		SIS_Debug("Values in [50C2h] array: ");
		for (int i = 0; i != 16; i++) {
			uint8_t currentValue = mem_readb_inline(
			        GetAddress(SIS_GlobalOffset, 0x50C2 + i));
			SIS_Debug("%.2X ", currentValue);
		}
		SIS_Debug("\n");
		return;
	}

	 add di, ax 0037 : 15FF cmp byte ptr es : [di + 50C2h],
	                                              0h

	if (off == 0x174B) {
		SIS_PrintLocal("Result: ", -0x2, 2);
		return;
	}

}

void SIS_Debug(const char* format, ...) {
	// Initialize a variable argument list
	va_list args;
	va_start(args, format);
	std::string result = SIS_DebugFormat(format, args);
	va_end(args);

	fprintf(stdout, result.c_str());
}

void SIS_DebugScript(const char* format, ...)
{
	// Initialize a variable argument list
	va_list args;
	va_start(args, format);
	std::string result = SIS_DebugFormat(format, args);
	va_end(args);

	DebugStrings.push_back(result);
}

std::string SIS_DebugFormat(const char* format, va_list args)
{
	int size = vsnprintf(nullptr, 0, format, args);

	if (size < 0) {
		// throw std::runtime_error("Error during formatting.");
		return std::string();
	}

	// Create a string with the required size
	std::vector<char> buffer(size + 1);
	vsnprintf(buffer.data(), buffer.size(), format, args);

	// Return the formatted string
	std::string result = std::string(buffer.data(), size);
	return result;
}

void SIS_GetScriptInfos(uint16_t& script_offset, uint16_t& seg, uint16_t& off)
{
	script_offset = mem_readw_inline(GetAddress(0x0227, 0x0F8A));
	seg           = mem_readw_inline(GetAddress(0x0227, 0x0F8C));
	off           = mem_readw_inline(GetAddress(0x0227, 0x0F8C + 0x2));
}

uint16_t SIS_GetStackWord(int16_t off)
{
	return mem_readw_inline(GetAddress(SegValue(ss), reg_sp + off));
}

uint8_t SIS_GetStackByte(int16_t off)
{
	return mem_readb_inline(GetAddress(SegValue(ss), reg_sp + off));
}

uint32_t SIS_GetLocalDoubleWord(int16_t off)
{
	return mem_readd_inline(GetAddress(SegValue(ss), reg_bp + off));
}

uint16_t SIS_GetLocalWord(int16_t off)
{
	return mem_readw_inline(GetAddress(SegValue(ss), reg_bp + off));
}

uint8_t SIS_GetLocalByte(int16_t off)
{
	return mem_readb_inline(GetAddress(SegValue(ss), reg_bp + off));
}


bool SIS_ParseCommand(char* found, std::string command)
{
	if (command == "WIPE") {
		// TODO: Expose these parameters
		// TODO: More output for my own functions
		// 0387:0238 and 0387:1f08
		// 038f:00f8 and 038f:0748
		SIS_WipeMemoryFromTo(0x0387, 0x0238, 0x1f08, 0x00);
		SIS_WipeMemoryFromTo(0x038f, 0x00f8, 0x0748, 0x00);

		return true;
	}

	if (command == "FS") {
		SIS_filterSegment = GetHexValue(found, found);
		DEBUG_ShowMsg("DEBUG: Setting file read filter segment to %04X\n", SIS_filterSegment);

		return true;
	}

	if (command == "SKIP2") {
		SIS_skippedOpcode = GetHexValue(found, found);
		DEBUG_ShowMsg("DEBUG: Setting skipped opcode to %02X\n",
		              SIS_skippedOpcode);

		return true;
	}
	
	
	if (command == "CALLER") {
		bool all       = !(*found);
		uint8_t levels = all ? 1 : (uint8_t)GetHexValue(found, found);

		// At SS:BP, the old stack frame is saved, so we can use this to
		// walk up
		uint32_t calleeBP = reg_bp;
		uint32_t callerBP = mem_readw_inline(
		        GetAddress(SegValue(ss), calleeBP + 0x00));
		uint16_t ret_off;
		uint32_t ret_seg;
		SIS_GetCaller(ret_seg, ret_off, levels);

		codeViewData.useCS     = ret_seg;
		codeViewData.useEIP    = ret_off;
		codeViewData.cursorPos = 0;

		return true;
	}

	if (command == "BPSO") { // Set a breakpoint for a given script opcode
		uint8_t opcode = (uint8_t)GetHexValue(found, found);
		CBreakpoint::AddScriptOpcodeBreakpoint(opcode);
		DEBUG_ShowMsg("DEBUG: Set script offset breakpoint for opcode %02X\n", opcode);
		return true;
	}

	if (command == "SLOC") { // Print the current location in the script
		
		return true;
	}

	if (command == "SSM") { // Set memory in the current script
		uint16_t seg = (uint16_t)GetHexValue(found, found);
		found++;
		uint32_t ofs = GetHexValue(found, found);
		found++;
		uint16_t count = 0;
		while (*found) {
			while (*found == ' ') {
				found++;
			}
			if (*found) {
				uint8_t value = (uint8_t)GetHexValue(found, found);
				if (*found) {
					found++;
				}
				mem_writeb_checked(GetAddress(seg, ofs + count),
				                   value);
				count++;
			}
		}
		DEBUG_ShowMsg("DEBUG: Memory changed.\n");
		return true;
	}

	if (command == "BGC") { // Change the background image pointer
		uint16_t localOffset = (uint16_t)GetHexValue(found, found);
		SIS_ChangeMapPointerToBackground(localOffset);
		DEBUG_ShowMsg("DEBUG: Changed background image to map with offset %.4x.\n", localOffset);

		return true;
	}

	if (command == "BGR") { // Change the background back to the original
		
		SIS_ResetBackground();
		DEBUG_ShowMsg("DEBUG: Reset background image.\n");

		return true;
	}
	if (command == "BPSV") { // Watch a script variable
		uint16_t variableIndex = (uint16_t)GetHexValue(found, found);
		
		uint16_t variableOffset = variableIndex << 2;
		uint32_t seg;
		uint16_t off;
		SIS_ReadAddress(0x0227, 0x06C6, seg, off);
		CBreakpoint::AddMemBreakpoint(seg, off + variableOffset);
		CBreakpoint::AddMemBreakpoint(seg, off + variableOffset + 2);

		DEBUG_ShowMsg("DEBUG: Watching script variable %.4x.\n",
		              variableIndex);
		
		return true;
	}
	if (command == "GIVEITEM") {
		// Give an item to the protagonist
		uint16_t objectIndex = (uint16_t)GetHexValue(found, found);

		uint32_t objSeg;
		uint16_t objOff;
		// We shift left by 2 = *4
		SIS_ReadAddress(0x227, 0x77C + objectIndex * 4, objSeg, objOff);
		mem_writew_inline(GetAddress(objSeg, objOff + 0x4), 0x1);

		DEBUG_ShowMsg("DEBUG: Giving item to player: %.4x.\n",
		              objectIndex);

		return true;

	}
	if (command == "SO") {
		// Set the orientation of the protagonist
		uint16_t orientation = (uint16_t)GetHexValue(found, found);

		uint32_t protSeg;
		uint16_t protOff;
		// We shift left by 2 = *4
		SIS_ReadAddress(0x227, 0x77C + 0x1 * 4, protSeg, protOff);
		uint16_t charX = mem_readw_inline(GetAddress(protSeg, protOff + 0x0));
		uint16_t charY = mem_readw_inline(GetAddress(protSeg, protOff + 0x2));
		mem_writew_inline(
		        GetAddress(protSeg, protOff + 0x6), orientation);
		
		DEBUG_ShowMsg("DEBUG: Setting player orientation to: %.4x.\n", orientation);

		return true;
	}

	if (command == "BLOB") {
		// Temp command for blob debugging
		
		uint16_t blobIndex = (uint16_t)GetHexValue(found, found);

		uint32_t protSeg;
		uint16_t protOff;
		// We shift left by 2 = *4
		SIS_ReadAddress(0x227, 0x77C + 0x1 * 4, protSeg, protOff);

		return true;
	}

	if (command == "ANIMDIS") {
		// Disable 1480 filtering
		SIS_1480_FilterForAddress = false;
		SIS_1480_FilterForCaller  = false;

		DEBUG_ShowMsg("DEBUG: Disabling 1480 filtering.\n");
		return true;
	}

	if (command == "ANIMCALLER") {
		// Enable 1480 filtering by caller
		SIS_1480_FilterForAddress = false;
		SIS_1480_FilterForCaller  = true;

		SIS_1480_CallerDepth = (uint16_t)GetHexValue(found, found);
		found++;
		SIS_1480_CallerSeg = (uint16_t)GetHexValue(found, found);
		found++;
		SIS_1480_CallerOffMin = (uint16_t)GetHexValue(found, found);
		found++;
		SIS_1480_CallerOffMax = (uint16_t)GetHexValue(found, found);
		found++;

		DEBUG_ShowMsg("DEBUG: Filtering 1480 for caller at depth %.2X: %.4X:%.4X-%.4X.\n",
			SIS_1480_CallerDepth, SIS_1480_CallerSeg, SIS_1480_CallerOffMin, SIS_1480_CallerOffMax);
		return true;
	}

	if (command == "ANIMADDR") {
		// Enable 1480 filtering by address
		SIS_1480_FilterForAddress = true;
		SIS_1480_FilterForCaller  = false;

		SIS_1480_AnimSeg = (uint16_t)GetHexValue(found, found);
		found++;
		SIS_1480_AnimOff = (uint16_t)GetHexValue(found, found);
		found++;
		

		DEBUG_ShowMsg("DEBUG: Filtering 1480 for address %.4X:%.4X.\n",
		              SIS_1480_AnimSeg,
		              SIS_1480_AnimOff);
		return true;
	}



		uint16_t SIS_1480_CallerSeg;
		uint16_t SIS_1480_CallerOffMin;
		uint16_t SIS_1480_CallerOffMax;
		bool SIS_1480_FilterForCaller;
		bool SIS_1480_FilterForAddress;
		uint16_t SIS_1480_AnimOff;
		uint16_t SIS_1480_AnimSeg;
	

	return false;
}

void SIS_Handle1480(Bitu seg, Bitu off) {

	// These save the currently active special animation set
	static int opcode26Seg = -1;
	static int opcode26Off = -1;
	if (seg == 0x01E7 && off == 0xCAE6) {
		opcode26Seg = reg_ax;
		opcode26Off = reg_dx;
		return;
	}

	// Handle the background animation update loop index
	if (seg == 0x01E7 && off == 0x9286) {
		SIS_PrintLocal("** Updating bg anim ", -0xC, 2);
		return;
	}

	static TraceHelper traceHelper;
	// Trace all times that we are wrapping around
	static bool traceHelperInitialized = false;
	// TODO: This should be possible to do more elegantly, maybe with a lambda?
	if (!traceHelperInitialized) {
		traceHelper.AddTracePoint(0x14BE, "Adjusting [bp-6] to 1");
		traceHelper.AddTracePoint(0x14EA, "Adjusting [bp-6] to 1");
		traceHelper.AddTracePoint(0x14F7, "Adjusting [bp-6] to 1");
		traceHelper.AddTracePoint(0x1505, "Adjusting [bp-6] to 1");
		traceHelper.AddTracePoint(0x15CB, "Adjusting [bp-6] to 1");
		traceHelperInitialized = true;
	}
	if (is1480Filtered) {
		// We can only leave the filtering if we leave the function
		if (off == 0x1615) {
			is1480Filtered = false;
		}
		return;
	}


	if (seg != 0x01F7) {
		return;
	}
	if (off == 0x1484) {
		// Figure out if we are called from the right function

		uint32_t caller_seg;
		uint16_t caller_off;

		SIS_GetCaller(caller_seg, caller_off, SIS_1480_CallerDepth);
		if (SIS_1480_FilterForCaller && caller_seg == SIS_1480_CallerSeg &&
		                              (caller_off < SIS_1480_CallerOffMin ||
		                               caller_off > SIS_1480_CallerOffMax)) {
			is1480Filtered = true;
			return;
		}

		// Filter by a specific animation set (walking to the left)
		uint32_t animSeg;
		uint16_t animOff;

		SIS_ReadAddressFromLocal(+0x12, animSeg, animOff);
		// TODO: I think I have these backwards, this should be segment
		if (SIS_1480_FilterForAddress &&(animOff != SIS_1480_AnimOff ||
		                                  animSeg != SIS_1480_AnimSeg)) {
			is1480Filtered = true;
			return;
		}

		// Print the arguments
		SIS_PrintCaller();
		SIS_PrintCaller(2);
		SIS_PrintLocal("Argument - Byte value: ", +0x6, 1);
		SIS_PrintLocal("Argument - Value between 0 and something around A4: ", +0x08, 2);
		SIS_PrintLocal("Argument - Address: ", +0x0A, 4);
		SIS_PrintLocal("Argument - Address: ", +0x0E, 4);
		SIS_PrintLocal("Argument - Address: ", +0x12, 4);
	}

	// Let the trace helper try handling it
	traceHelper.HandleOffset(off);

	if (off == 0x14AC) {
		// Print the read values
		SIS_PrintLocal("Read value 1: ", -0x22, 2);
		SIS_PrintLocal("Read value 2: ", -0x06, 2);
		SIS_PrintLocal("Read value 3: ", -0x08, 2);
		SIS_PrintLocal("Read value 4: ", -0x0A, 2);
		SIS_PrintLocal("Read value 5: ", -0x10, 2);
		SIS_PrintLocal("Read value 6 (incremented by 1): ", -0x0E, 2);
		SIS_PrintLocal("Read value 7 (via [bp-06h]): ", -0x0C, 1);
	}

	if (off == 0x1500) {
		SIS_PrintLocal("Loop at 1500h: ", -0x06, 2);
	}

	if (off == 0x1519) {
		// TODO: Could also be that we don't reach this place, right?
		SIS_PrintLocal("Read value (determines a switch): ", -0x0C, 1);
	}

	if (off == 0x1566) {
		SIS_PrintLocal("Read value (determines wrap-around): ", -0x24, 2);
	}

	if (off == 0x1587) {
		// This is the loop during which we iterate over animations?
		fprintf(stdout, "Loop: cx = %.4x\n", reg_cx);
		SIS_PrintLocal("Loop: Read value 1: ", -0x1A, 2);
		SIS_PrintLocal("Loop: Read value 2: ", -0x1C, 2);
		SIS_PrintLocal("Loop: Read value 3 (skipped a word): ", -0x16, 2);
		SIS_PrintLocal("Loop: Read value 4: ", -0x18, 2);
	}

	if (off == 0x15EF && SIS_GetLocalByte(+0x6)) {
		// Handle the writes
		SIS_PrintLocal("Write back: ", -0x22, 2);
		SIS_PrintLocal("Write back: ", -0x06, 2);
		SIS_PrintLocal("Write back: ", -0x08, 2);
		SIS_PrintLocal("Write back: ", -0x0A, 2);
		SIS_PrintLocal("Write back: ", -0x10, 2);
	}

	if (off == 0x15EF) {
		// Handle the other set of writes
		SIS_PrintLocal("Write to target: ", -0x1A, 2);
		SIS_PrintLocal("Write to target: ", -0x1C, 2);
	}

	if (off == 0x1615) {
		// Handle the result
		uint32_t caller_seg;
		uint16_t caller_off;
		SIS_GetCaller(caller_seg, caller_off);
		SIS_PrintLocal("Result - Segment: ", -0x04, 2);
		SIS_PrintLocal("Result - Offset: ", -0x02, 2);
		// if (caller_off == 0x1832) {
		// if (caller_off == 0x17F9) {
		/* if (caller_off == 0x174a) {
			// Testing out overwriting with a fixed result
			fprintf(stdout, "Overwriting results\n");
			reg_ax = 0x0015;
			reg_dx = 0x049F;
		}*/
	}
}

SIS_DeferredGetter<uint16_t>* SIS_GetLocalWordDeferred(int16_t localOff,
                                                      uint16_t seg, uint16_t off)
{
	SIS_DeferredGetter<uint16_t>* result = new SIS_DeferredGetter<uint16_t>();
	result->lambda = [localOff]() -> uint16_t {
		uint16_t lambdaResult = SIS_GetLocalWord(localOff);
		return lambdaResult;
	};
}

void SIS_PrintLocal(const char* format, int16_t offset, uint8_t numBytes, ...)
{
	// Initialize a variable argument list
	va_list args;
	va_start(args, format);

	// Determine the required size for the formatted string
	va_list args_copy;
	va_copy(args_copy, args);
	int size = vsnprintf(nullptr, 0, format, args_copy);
	va_end(args_copy);

	if (size < 0) {
		// TODO: Handle error
		va_end(args);
		// throw std::runtime_error("Error during formatting.");
		return;
	}

	// Create a string with the required size
	std::vector<char> buffer(size + 1);
	vsnprintf(buffer.data(), buffer.size(), format, args);
	va_end(args);

	// Return the formatted string
	std::string result = std::string(buffer.data(), size);
	fprintf(stdout, result.c_str());

	// Read the local
	// Also handle 4 bytes as a pointer
	uint16_t localValue;
	char* valueFormat;
	const char* offsetSign = offset >= 0 ? "+" : "-";
	uint16_t positiveOffset = offset >= 0 ? offset : -offset;
	switch (numBytes) {
	case 1: {
		localValue = SIS_GetLocalByte(offset);
		valueFormat = "[bp%s%.2x]: %.2x\n";
	}
		  break;
	case 2: {
		localValue  = SIS_GetLocalWord(offset);
		valueFormat = "[bp%s%.2x]: %.4x\n";
		  }
		  break;
	case 4: {
		          uint32_t localSeg;
				  uint16_t localOff;
		          SIS_ReadAddressFromLocal(offset, localSeg, localOff);
		                  fprintf(stdout,
		                          "[bp%s%.2x]: %.4x:%.4x\n",
								  offsetSign,
								  positiveOffset,
		                          localSeg,
		                          localOff);
	}
		return;
	}
	fprintf(stdout, valueFormat, offsetSign, positiveOffset, localValue);

}


void TraceHelper::AddTracePoint(uint16_t offset, const std::string& message) {
	tracePoints[offset] = message;
}

void TraceHelper::AddTracePoint(uint16_t offset) {
	AddTracePoint(offset, std::string());
}

void TraceHelper::HandleOffset(uint16_t offset) {
	map<uint16_t, string>::iterator it = tracePoints.find(offset);
	if (it != tracePoints.end()) {
		const std::string& traceMessage = it->second;
		fprintf(stdout, "%.4x: %s\n", offset, traceMessage.c_str());
	}
}

TraceHelper::TraceHelper() {
	

}
