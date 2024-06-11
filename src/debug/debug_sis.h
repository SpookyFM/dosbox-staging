#pragma once
#include "debug.h"

const std::string SIS_AnimFrame("animframe");
const std::string SIS_OPL("opl");
const std::string SIS_Palette("palette");
const std::string SIS_Script("script");
const std::string SIS_Script_Verbose("script_verbose");
// TODO: Consider removing since I'm not using it
const std::string SIS_Script_Minimal("script_minimal");
const std::string SIS_Pathfinding("pathfinding");
const std::string SIS_Scaling("scaling");
const std::string SIS_RLE("rle");

inline bool isScriptChannelActive(){
	
	return isChannelActive(SIS_Script) ||
	       isChannelActive(SIS_Script_Minimal) ||
	       isChannelActive(SIS_Script_Verbose);
}

int64_t SIS_filterSegment = -1;
uint16_t SIS_skippedOpcode = -1;
uint8_t SIS_currentOpcode1;

bool SIS_ScriptIsSkipping = false;


void SIS_Init();


// TODO: Move my special handling into this file

bool SIS_IsBreakpoint(Bitu seg, Bitu off);

void SIS_HandleScaling(Bitu seg, Bitu off);

void SIS_HandlePathfinding(Bitu seg, Bitu off);

void SIS_HandleSIS(Bitu seg, Bitu off);

bool SIS_ParseCommand(char* found, std::string command);

void SIS_GetCaller(uint32_t& out_seg, uint16_t& out_off, uint16_t num_levels = 1);

void SIS_ReadAddress(uint32_t seg, uint16_t off, uint32_t& outSeg, uint16_t& outOff);
void SIS_WriteAddress(uint32_t seg, uint16_t off, uint32_t outSeg, uint16_t outOff);

void SIS_HandleSkip(Bitu seg, Bitu off);

void SIS_GetScriptInfos(uint16_t& script_offset, uint16_t& seg, uint16_t& off);

uint16_t SIS_GetLocalWord(Bitu off);
uint8_t SIS_GetLocalByte(Bitu off);

void SIS_HandleInventoryIcons(Bitu seg, Bitu off);

void SIS_HandleDrawingFunction(Bitu seg, Bitu off);

void SIS_HandleDataLoadFunction(Bitu seg, Bitu off);

void SIS_HandleBlobLoading(Bitu seg, Bitu off);

void SIS_HandleRLEDecoding(Bitu seg, Bitu off);

void SIS_HandlePaletteChange(Bitu seg, Bitu off);

void SIS_HandleStopWalking(Bitu seg, Bitu off);

void SIS_DrawImage(Bitu seg, Bitu off);

void SIS_DumpPalette();

std::string SIS_IdentifyScriptOpcode(uint8_t opcode, uint8_t opcode2);
std::string SIS_IdentifyHelperOpcode(uint8_t opcode, uint16_t value);

void SIS_CopyImageToClipboard(uint16_t width, uint16_t height, uint8_t* pixels);

void SIS_HandleCharacterPos(Bitu seg, Bitu off);

bool bgOriginalChanged;
uint32_t bgOriginalSeg;
uint16_t bgOriginalOff;

uint16_t lastChangedMapLocalOffset;
uint32_t lastChangedMapSeg;
uint16_t lastChangedMapOff;

void SIS_ChangeMapPointerToBackground(uint16_t localOffset);
void SIS_ResetBackground();

void SIS_ReadImageToPixels(Bitu seg, Bitu off, uint16_t& width, uint16_t& height, uint8_t*& pixels);
