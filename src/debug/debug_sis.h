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
	Fileread,
	FrameUpdate
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
const std::string SIS_FrameUpdate("frame_update");

extern std::map<std::string, SIS_ChannelID> channelIDNames;
extern std::map<SIS_ChannelID, bool> debugLogEnabledID;

// Addresses for the font
uint16_t SIS_FontAddresses[0x502 / 2][2];
bool SIS_FontInitialized = false;

uint32_t SIS_FontAddressesASCII[0x502 / 2];

void SIS_DrawString(const std::string& s, uint16_t x, uint16_t y);

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

// Break if the first read in the Adlib timer function is read from this offset
// Default value of 0 will never break
uint16_t SIS_AdlibLoopBPOffset = 0;


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
void SIS_Handle1480Short(Bitu seg, Bitu off);

void SIS_HandleFont(Bitu seg, Bitu off);
bool SIS_GetFontCharacterData(uint8_t c, uint16_t& w, uint16_t& h, uint32_t& data);

void HandleMovementSpeed(Bitu seg, Bitu off);

std::string SIS_ProtagonistDebugText;
void SIS_HandleProtagonistDebugText(Bitu seg, Bitu off);

void SIS_HandleOPLWrite(Bitu seg, Bitu off);


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
void SIS_PrintLocalShort(int16_t offset, uint8_t numBytes);

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

void SIS_HandleBlobLoading2(Bitu seg, Bitu off);


bool injectFunction = false;
uint16_t giveItemObjectID;
void SIS_HandleFunctionInjection(Bitu seg, Bitu off);

void SIS_HandleScalingCalculation(Bitu seg, Bitu off);

void SIS_HandleRLEDecoding(Bitu seg, Bitu off);

void SIS_HandlePaletteChange(Bitu seg, Bitu off);

void SIS_HandleStopWalking(Bitu seg, Bitu off);

void SIS_DrawImage(Bitu seg, Bitu off);

void SIS_HandleSkippedCode(Bitu seg, Bitu off);

void SIS_DumpPalette();

void SIS_HandleUI(Bitu seg, Bitu off);

void SIS_HandleInventoryScrolling(Bitu seg, Bitu off);

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

void SIS_DrawHorizontalLine(uint16_t x1, uint16_t x2, uint16_t y, uint8_t value = 0xFF);
void SIS_DrawVerticalLine(uint16_t x, uint16_t y1, uint16_t y2, uint8_t value = 0xFF);
inline void SIS_SetPixel(uint16_t x, uint16_t y, uint8_t value)
{
	mem_writeb_inline(GetAddress(0xA000, y * 320 + x), value);
};

uint16_t SIS_InitialSceneOverride = 0x06;
constexpr uint8_t SIS_UseSound    = 0;
void SIS_HandleInitialSceneOverride(Bitu seg, Bitu off);

void SIS_HandleAdlibSeek(Bitu seg, Bitu off);
void SIS_HandleAdlibSeekShort(Bitu seg, Bitu off);

void SIS_HandleInventoryRedraw(Bitu seg, Bitu off);


struct MemWatchConfig {
	Bitu seg;
	Bitu startOff;
	Bitu endOff;
};

bool SIS_MemWatchesInitialized = false;
uint16_t* SIS_MemWatches   = nullptr;
uint16_t SIS_MemWatchIndex = 0;

void SIS_StartMemWatches();
void SIS_FinishMemWatches();

std::string SIS_StringFormat(const char* format, ...);

// TODO: Consider start and end of function tracking for also getting it if 
// globals change in a called function
void SIS_HandleMemWatch(Bitu seg, Bitu off,
                       uint32_t varAddress, uint8_t varSize, const char* varName);

void SIS_HandleLocalsWatch(Bitu seg, Bitu off,
						 int16_t varOffset, uint8_t varSize);

void SIS_HandleGlobalsWatch(Bitu seg, Bitu off,
                           Bitu globalSeg, Bitu globalOff, uint8_t globalSize);



void SIS_HandleAdlib(Bitu seg, Bitu off);
void SIS_LogEntry(Bitu seg, Bitu off, Bitu targetSeg, Bitu targetOff,
                  std::string msg = "");


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
