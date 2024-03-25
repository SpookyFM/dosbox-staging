#pragma once
#include "debug.h"

const std::string SIS_AnimFrame("animframe");
const std::string SIS_OPL("opl");
const std::string SIS_Palette("palette");
const std::string SIS_Script("script");
const std::string SIS_Script_Verbose("script_verbose");
const std::string SIS_Script_Minimal("script_minimal");
const std::string SIS_Pathfinding("pathfinding");
const std::string SIS_Scaling("scaling");

inline bool isScriptChannelActive(){
	
	return isChannelActive(SIS_Script) ||
	       isChannelActive(SIS_Script_Minimal) ||
	       isChannelActive(SIS_Script_Verbose);
}

int64_t SIS_filterSegment = -1;
uint16_t SIS_skippedOpcode = -1;
uint8_t SIS_currentOpcode1;


void SIS_Init();


// TODO: Move my special handling into this file

bool SIS_IsBreakpoint(Bitu seg, Bitu off);

void SIS_HandleScaling(Bitu seg, Bitu off);

void SIS_HandlePathfinding(Bitu seg, Bitu off);

void SIS_HandleSIS(Bitu seg, Bitu off);

bool SIS_ParseCommand(char* found, std::string command);

void SIS_GetCaller(uint32_t& out_seg, uint16_t& out_off, uint16_t num_levels = 1);

void SIS_HandleSkip(Bitu seg, Bitu off);

void SIS_GetScriptInfos(uint16_t& script_offset, uint16_t& seg, uint16_t& off);

uint16_t SIS_GetLocalWord(Bitu off);
uint8_t SIS_GetLocalByte(Bitu off);

void SIS_HandleInventoryIcons(Bitu seg, Bitu off);

void SIS_HandleDrawingFunction(Bitu seg, Bitu off);

void SIS_DrawImage(Bitu seg, Bitu off);

std::string SIS_IdentifyScriptOpcode(uint8_t opcode, uint8_t opcode2);
