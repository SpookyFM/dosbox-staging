#pragma once
#include "debug.h"

const std::string SIS_AnimFrame("animframe");
const std::string SIS_OPL("opl");
const std::string SIS_Palette("palette");

void SIS_Init();


// TODO: Move my special handling into this file

bool SIS_IsBreakpoint(Bitu seg, Bitu off);

void SIS_HandleSIS(Bitu seg, Bitu off);

bool SIS_ParseCommand(char* found, std::string command);
