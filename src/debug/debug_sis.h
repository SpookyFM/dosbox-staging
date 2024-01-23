#pragma once
#include "debug.h"

const std::string SIS_AnimFrame("animframe");
const std::string SIS_OPL("opl");
const std::string SIS_Palette("palette");
const std::string SIS_Script("script");
const std::string SIS_Script_Verbose("script_verbose");

int64_t SIS_filterSegment = -1;

void SIS_Init();


// TODO: Move my special handling into this file

bool SIS_IsBreakpoint(Bitu seg, Bitu off);

void SIS_HandleSIS(Bitu seg, Bitu off);

bool SIS_ParseCommand(char* found, std::string command);

void SIS_GetCaller(uint32_t& out_seg, uint16_t& out_off, uint16_t num_levels = 1);
