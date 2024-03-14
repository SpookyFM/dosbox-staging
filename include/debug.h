/*
 *  Copyright (C) 2002-2021  The DOSBox Team
 *
 *  This program is free software; you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License as published by
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

#ifndef DOSBOX_DEBUG_H
#define DOSBOX_DEBUG_H

#include "dosbox.h"

#include <deque>
#include <queue>
#include <map>

#include "paging.h"

void DEBUG_DrawScreen(void);
bool DEBUG_Breakpoint(void);
bool DEBUG_ReturnBreakpoint(void);
bool DEBUG_IntBreakpoint(uint8_t intNum);
void DEBUG_Enable(bool pressed);
void DEBUG_CheckExecuteBreakpoint(uint16_t seg, uint32_t off);
bool DEBUG_ExitLoop(void);
void DEBUG_RefreshPage(int scroll);
void DEBUG_PushStackFrame(uint16_t callerSeg, uint32_t callerOff, uint16_t calleeSeg, uint32_t calleeOff);
void DEBUG_PopStackFrame(uint16_t curSeg, uint32_t curOff, uint16_t retSeg, uint32_t retOff);

Bitu DEBUG_EnableDebugger(void);

extern Bitu cycle_count;
extern Bitu debugCallback;

extern bool useCallstack;
extern bool filterCallstack;

extern std::map<std::string, bool> debugLogEnabled;

inline bool isChannelActive(const std::string& name) {
	// TODO: Consider a safety mode
	return debugLogEnabled[name];
}



struct callstack_entry {
	bool is_call; // True if this is a call, false if this is a return
	uint16_t caller_seg;
	uint32_t caller_off;
	uint16_t callee_seg;
	uint16_t callee_off;
};




extern std::deque<callstack_entry> callstack;
extern std::deque<callstack_entry> calltrace;
extern std::deque<callstack_entry> rolling_calltrace;
extern int rolling_calltrace_length;
extern bool callstack_started;


#ifdef C_HEAVY_DEBUG
bool DEBUG_HeavyIsBreakpoint(void);
void DEBUG_HeavyWriteLogInstruction(void);
#endif

#endif
