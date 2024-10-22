#pragma once
#include "debug.h"

enum class SIS_ChannelID {
	AnimFrame,
	OPL,
	Palette,
	Script,
	Script_Verbose,
	Script_Mínimal,
	Pathfinding,
	Scaling,
	RLE,
	Special,
	Fileread
};

const std::string SIS_Special("special");
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
const std::string SIS_Fileread("fileread");

extern std::map<std::string, SIS_ChannelID> channelIDNames;
extern std::map<SIS_ChannelID, bool> debugLogEnabledID;

inline bool isChannelActive(const std::string& name)
{
	// TODO: Consider a safety mode
	return debugLogEnabledID[channelIDNames[name]];
}

inline bool isChannelActive(SIS_ChannelID channelID)
{
	return debugLogEnabledID[channelID];
}

inline void setIsChannelActive(SIS_ChannelID channelID, bool active) {
	debugLogEnabledID[channelID] = active;
}

class TraceHelper {
	public: 
		std::map<uint16_t, std::string> tracePoints;
		// TODO: Also the best way to a pass a std::string?
	    void AddTracePoint(uint16_t offset, const std::string& message);
	    void AddTracePoint(uint16_t offset);
	    void HandleOffset(uint16_t offset);
		// TODO: Added for ease of use
	    TraceHelper();
};

inline bool isScriptChannelActive(){
	
	return isChannelActive(SIS_ChannelID::Script) ||
	       isChannelActive(SIS_ChannelID::Script_Mínimal) ||
	       isChannelActive(SIS_ChannelID::Script_Verbose);
}

int64_t SIS_filterSegment = -1;
uint16_t SIS_skippedOpcode = -1;
uint8_t SIS_currentOpcode1;

bool SIS_ScriptIsSkipping = false;
bool SIS_LastOpcodeTriggeredSkip = false;


void SIS_Init();


// TODO: Move my special handling into this file

bool SIS_IsBreakpoint(Bitu seg, Bitu off);

void SIS_HandleAnimatedPortraits(Bitu seg, Bitu off);

void SIS_HandleScaling(Bitu seg, Bitu off);

void SIS_HandlePathfinding(Bitu seg, Bitu off);

void SIS_HandleSIS(Bitu seg, Bitu off);

bool SIS_ParseCommand(char* found, std::string command);

bool is1480Filtered = false;

uint16_t SIS_1480_CallerDepth;
uint16_t SIS_1480_CallerSeg;
uint16_t SIS_1480_CallerOffMin;
uint16_t SIS_1480_CallerOffMax;
bool SIS_1480_FilterForCaller;
bool SIS_1480_FilterForAddress;
uint16_t SIS_1480_AnimOff;
uint16_t SIS_1480_AnimSeg;
void SIS_Handle1480(Bitu seg, Bitu off);

constexpr uint16_t SIS_Arg7 = +0x12;
constexpr uint16_t SIS_Arg6 = +0x10;
constexpr uint16_t SIS_Arg5 = +0xE;
constexpr uint16_t SIS_Arg4 = +0xC;
constexpr uint16_t SIS_Arg3 = +0xA;
constexpr uint16_t SIS_Arg2 = +0x8;
constexpr uint16_t SIS_Arg1 = +0x6;



class SIS_DeferredGetterBase {
public:
	
	uint16_t seg;
	uint16_t off;

	virtual void Execute() = 0;
};

std::map<uint32_t, std::vector<SIS_DeferredGetterBase>> deferredGetters;

template <class T> 
class SIS_DeferredGetter : public SIS_DeferredGetterBase {
public:
	T result;

	operator T();

	uint16_t seg;
	uint16_t off;
	std::function<uint16_t()> lambda;

	virtual void Execute() override;
};



// Capture
// Need to add to a map of lists with address mapped to instruction
// When the address is hit, execute all functions and save the results for later
// Template class with return type as template
// Handle the function as a lambda?
// Retrieval
// Give out a copy of the object which can grab the data
SIS_DeferredGetter<uint16_t>* SIS_GetLocalWordDeferred(int16_t localOff, uint16_t seg, uint16_t off);

constexpr uint16_t SIS_GlobalOffset = 0x0227;

void SIS_PrintLocal(const char* format, int16_t offset, uint8_t numBytes,...);

 // Define a buffer for stdout
char stdout_buffer[0xFFFF];

void SIS_BeginBuffering();
void SIS_EndBuffering(bool print = false);

void SIS_GetCaller(uint32_t& out_seg, uint16_t& out_off, uint16_t num_levels = 1);
void SIS_PrintCaller(uint16_t num_levels = 1);

void SIS_ReadAddressFromLocal(int16_t offset, uint32_t& outSeg, uint16_t& outOff);
void SIS_ReadAddress(uint32_t seg, uint16_t off, uint32_t& outSeg, uint16_t& outOff);
void SIS_WriteAddress(uint32_t seg, uint16_t off, uint32_t outSeg, uint16_t outOff);

void SIS_HandleSkip(Bitu seg, Bitu off);

void SIS_GetScriptInfos(uint16_t& script_offset, uint16_t& seg, uint16_t& off);


uint16_t SIS_GetStackWord(int16_t off);
uint8_t SIS_GetStackByte(int16_t off);

uint32_t SIS_GetLocalDoubleWord(int16_t off);
uint16_t SIS_GetLocalWord(int16_t off);
uint8_t SIS_GetLocalByte(int16_t off);

void SIS_HandleInventoryIcons(Bitu seg, Bitu off);

void SIS_HandleDrawingFunction(Bitu seg, Bitu off);

void SIS_HandleDataLoadFunction(Bitu seg, Bitu off);

void SIS_HandleBlobLoading(Bitu seg, Bitu off);

void SIS_HandleRLEDecoding(Bitu seg, Bitu off);

void SIS_HandlePaletteChange(Bitu seg, Bitu off);

void SIS_HandleStopWalking(Bitu seg, Bitu off);

void SIS_DrawImage(Bitu seg, Bitu off);

void SIS_HandleSkippedCode(Bitu seg, Bitu off);

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

void SIS_HandleCharacterDrawing(Bitu seg, Bitu off);

void SIS_HandleBGAnimDrawing(Bitu seg, Bitu off);

void SIS_HandleMovementSpeedMod(Bitu seg, Bitu off);

void SIS_HandlePathfinding2(Bitu seg, Bitu off);
void SIS_HandlePathfinding3(Bitu seg, Bitu off);

bool SIS_FlagWatchPath = false;
void SIS_WatchPath(Bitu seg, Bitu off);

void SIS_PrintPath(uint16_t objectIndex = 1);

void SIS_Print15A8List(uint16_t off);

std::vector<std::string> DebugStrings;

void SIS_Debug(const char* format, ...);

void SIS_DebugScript(const char* format, ...);

std::string SIS_DebugFormat(const char* format, va_list args);

template <class T>
inline void SIS_DeferredGetter<T>::Execute()
{
	result = lambda();
}
