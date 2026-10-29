// SPDX-License-Identifier: GPL-2.0-or-later
#include "haumea.h"
#include "workspace.h"
#include "textdraws.h"
#include "radar.h"
#include "icons.h"
#include "shortcuts.h"
#include "i18n.h"
#include "imgui/imgui_internal.h"

#include <ctype.h>
#include <string.h>
#include <climits>
#include <cmath>
#include <algorithm>
#include <functional>
#include <map>
#ifdef _WIN32
#include <windows.h>
#else
#include <dirent.h>
#endif

// TextDraw mode: the textdraw file on a 640x448 canvas in place of the world,
// drawn with the game's own font textures (models/fonts.txd) and widths
// (data/fonts.dat). Map editing is off while it is on.

static int gSelected = -1;
static bool gMode;
enum { TD_BACKGROUND_PLAIN, TD_BACKGROUND_STILL };
static int gBackground = TD_BACKGROUND_STILL;
static unsigned int gCanvasColour = 0x3C4450FF;
static bool gStillPending;	// take the still frame after this frame's world
static int gStillWait;	// frames the world streams in before it is taken, after a move
static rw::Texture *gStillFrame;
static std::vector<std::string> gRecent;
static const int MAX_RECENT_TEXTDRAW_FILES = 8;

// The canvas: the player's screen shape, zoom and pan, the grid and snapping.
enum { TD_SCREEN_WINDOW, TD_SCREEN_4_3, TD_SCREEN_16_10, TD_SCREEN_16_9, TD_SCREEN_21_9, NUM_TD_SCREENS };
static const float gScreenAspects[NUM_TD_SCREENS] = { 0.0f, 4.0f / 3.0f, 16.0f / 10.0f, 16.0f / 9.0f, 21.0f / 9.0f };
static const char *gScreenNames[NUM_TD_SCREENS] = { N_("Window"), "4:3", "16:10", "16:9", "21:9" };
static int gScreen = TD_SCREEN_WINDOW;
static float gZoom = 1.0f, gPanX, gPanY;	// the pan in pixels, from the centred canvas
static bool gSnap = true, gGrid, gCompare, gRulers = true, gGuidesLocked, gPixelExact;
static float gGridSize = 10.0f;
static float gNudge = 1.0f;	// the arrow keys' step; Shift ten times it
static bool gClickThroughLocked = true;
// Selection: gSel in file order; gSelected is the one the panel edits.
static std::vector<int> gSel;
// A row picked in Layers takes precedence over overlapping canvas hit boxes.
static bool gLayerSelection;
// Double-clicking a group enters it; its members can then be edited alone.
static std::string gIsolatedGroup;
// The artboard on the canvas, and boards made but still empty.
static std::string gBoard;
static std::vector<std::string> gEmptyBoards;
// Colours the user saved, shared by every colour field (and kept in the settings).
static std::vector<unsigned int> gSavedColours;
static std::vector<std::string> gFavSprites;	// "txd:texture", starred in the sprite picker
// The eyedropper: a colour field asks, the next click on the canvas answers
// with the reference image's colour there.
static ImGuiID gPickFor;
static bool gPicking, gPicked;
static unsigned int gPickedColour;

// CHud's layout on the 640x448 screen, under the textdraws as in game:
// gta-reversed's Hud.cpp, and gta_sa.exe 1.0 US for what it doesn't have
// (DrawPlayerInfo 0x58EAF0, DrawWanted 0x58D9A0). As SA-MP shows it: max
// health 100, so GetYPosBasedOnHealth moves the bars, money and stars up.
enum {
	HUD_RADAR = 1 << 0, HUD_CLOCK = 1 << 1, HUD_MONEY = 1 << 2, HUD_WEAPON = 1 << 3, HUD_AMMO = 1 << 4,
	HUD_HEALTH = 1 << 5, HUD_ARMOUR = 1 << 6, HUD_BREATH = 1 << 7, HUD_WANTED = 1 << 8,
	HUD_ZONE = 1 << 9, HUD_VEHICLE = 1 << 10,
	// SA-MP's defaults: no clock (TogglePlayerClock), zone names or breath bar.
	HUD_DEFAULT = HUD_RADAR | HUD_MONEY | HUD_WEAPON | HUD_AMMO | HUD_HEALTH | HUD_ARMOUR | HUD_WANTED,
};
struct HudSettings {
	bool show;
	int parts;
	int hour, minute;
	int money;
	int health, armour, breath;	// percent
	int wanted;
	int weapon;	// in gHudWeapons
	int ammo, clip;	// the total, and what is in the clip
	char zone[64], vehicle[64];
	// SA-MP's chat, which is sized in the player's pixels, not the 640x448 screen.
	bool chat;
	int resolution;	// in gHudResolutions
	int fontSize;	// /fontsize, -3 to 5
	int pageSize;	// /pagesize, 10 to 20 lines
	bool timestamps;	// /timestamp
	char messages[2048];	// one per line, {RRGGBB} colours
	// ThirteenAG's GTA SA Widescreen Fix, which many players have: the HUD
	// unstretched and smaller, the radar round. Its ini's values.
	bool wsfix;
	float hudWidthScale, hudHeightScale;	// HudWidthScale, HudHeightScale (0.8)
	float radarWidth, radarHeight;	// RadarWidth, RadarHeight (85.5, 78)
	bool proportionalIcon;	// ProportionalWeaponIcon: the icon's width scaled as its height
	bool roundOutlines;	// newer builds draw text edges as a ring that grows with the screen
	// SA-MP 0.3.7-R3 and newer: /hudscalefix (on unless nohudscalefix=1 in sa-mp.cfg)
	// makes the radar round on screens 16:10 and wider.
	bool hudScaleFix;
};
static const struct { int w, h; } gHudResolutions[] = {
	{ 800, 600 }, { 1024, 768 }, { 1280, 720 }, { 1366, 768 }, { 1600, 900 }, { 1920, 1080 }, { 2560, 1440 },
};
static const int NUM_HUD_RESOLUTIONS = IM_ARRAYSIZE(gHudResolutions);
static const HudSettings HUD_DEFAULTS = { false, HUD_DEFAULT, 12, 0, 1500, 100, 50, 100, 5, 1, 120, 17, "Ganton", "Infernus",
	true, 5, 0, 10, false,
	"{A9C4E4}SA-MP 0.3.7-R5 Started\n{A9C4E4}Connecting to 127.0.0.1:7777...\n"
	"{A9C4E4}Connected. Joining the game...\n{A9C4E4}Connected to {B9C9BF}Haumea Server\n"
	"{FFFFFF}Welcome to the server!\n{FFFF00}Player_Name: {FFFFFF}hello everyone",
	false, 0.8f, 0.8f, 85.5f, 78.0f, true, false, true };
static HudSettings gHud = HUD_DEFAULTS;

// The weapon's model, its icon is <model>icon in <model>.txd; its clip as
// CWeaponInfo has it at poor skill (1 and below or 1000 and above: the total only).
static const struct { const char *name, *model; int clip; bool ammo; } gHudWeapons[] = {
	{ N_("Fist"), nil, 0, false },
	{ N_("Baseball bat"), "bat", 0, false },
	{ N_("Knife"), "knifecur", 0, false },
	{ N_("Colt 45"), "colt45", 17, true },
	{ N_("Silenced pistol"), "silenced", 17, true },
	{ N_("Desert Eagle"), "desert_eagle", 7, true },
	{ N_("Shotgun"), "chromegun", 1, true },
	{ N_("Sawn-off shotgun"), "sawnoff", 2, true },
	{ N_("Combat shotgun"), "shotgspa", 7, true },
	{ N_("Micro SMG"), "micro_uzi", 50, true },
	{ N_("MP5"), "mp5lng", 30, true },
	{ N_("Tec-9"), "tec9", 50, true },
	{ N_("AK-47"), "ak47", 30, true },
	{ N_("M4"), "m4", 50, true },
	{ N_("Country rifle"), "cuntgun", 1, true },
	{ N_("Sniper rifle"), "sniper", 1, true },
	{ N_("Rocket launcher"), "rocketla", 1, true },
	{ N_("Minigun"), "minigun", 500, true },
	{ N_("Grenade"), "grenade", 1, true },
	{ N_("Spraycan"), "spraycan", 500, true },
	{ N_("Camera"), "camera", 36, true },
	{ N_("Parachute"), "gun_para", 0, false },
};
static const int NUM_HUD_WEAPONS = IM_ARRAYSIZE(gHudWeapons);

static void
releaseStillFrame(void)
{
	if(gStillFrame){
		gStillFrame->destroy();
		gStillFrame = nil;
	}
}

bool
InTextDrawMode(void)
{
	return gMode;
}

bool
TextDrawModeHidesWorld(void)
{
	return gMode && !gStillPending && gStillWait <= 0;
}

void
EnterTextDrawMode(void)
{
	if(gMode)
		return;
	gMode = true;
	// The world as it is now, behind the canvas.
	releaseStillFrame();
	gStillPending = gBackground == TD_BACKGROUND_STILL;
}

void
ExitTextDrawMode(void)
{
	gMode = false;
	gStillPending = false;
	gLayerSelection = false;
	gIsolatedGroup.clear();
	releaseStillFrame();
}

void
TextDrawsAfterWorldRender(void)
{
	// After a camera preset: the world is drawn (under the canvas) for a
	// while so its models stream in, then the still frame is taken.
	if(gStillWait > 0 && --gStillWait == 0)
		gStillPending = true;
	if(!gStillPending)
		return;
	gStillPending = false;
	if(Scene.camera == nil || Scene.camera->frameBuffer == nil)
		return;
#ifdef RW_GL3
	glPixelStorei(GL_PACK_ALIGNMENT, 1);	// rows of width*3 bytes, as librw reads them
#endif
	rw::Image *image = Scene.camera->frameBuffer->toImage();
	if(image == nil)
		return;
	rw::Raster *raster = rw::Raster::createFromImage(image);
	image->destroy();
	if(raster == nil)
		return;
	gStillFrame = rw::Texture::create(raster);
	gStillFrame->setFilter(rw::Texture::LINEAR);
}

const std::vector<std::string>&
GetRecentTextDrawFiles(void)
{
	return gRecent;
}

void
AddRecentTextDrawFile(const char *path)
{
	if(path == nil || path[0] == '\0')
		return;
	gRecent.erase(std::remove(gRecent.begin(), gRecent.end(), std::string(path)), gRecent.end());
	gRecent.insert(gRecent.begin(), path);
	if(gRecent.size() > (size_t)MAX_RECENT_TEXTDRAW_FILES)
		gRecent.resize(MAX_RECENT_TEXTDRAW_FILES);
}

bool
LoadTextDrawSetting(const char *key, const char *value)
{
	if(strcmp(key, "textdraw_code_target") == 0){
		int target = TEXTDRAW_CODE_OPENMP;
		if(sscanf(value, "%d", &target) == 1 &&
		   (target == TEXTDRAW_CODE_SAMP || target == TEXTDRAW_CODE_OPENMP))
			SetTextDrawCodeTarget((TextDrawCodeTarget)target);
	}else if(strcmp(key, "textdraw_background") == 0)
		gBackground = atoi(value) == TD_BACKGROUND_PLAIN ? TD_BACKGROUND_PLAIN : TD_BACKGROUND_STILL;
	else if(strcmp(key, "textdraw_canvas_colour") == 0)
		gCanvasColour = (unsigned int)strtoul(value, nil, 16);
	else if(strcmp(key, "textdraw_saved_colours") == 0){
		gSavedColours.clear();
		for(const char *p = value; *p; ){
			char *end;
			unsigned long c = strtoul(p, &end, 16);
			if(end == p)
				break;
			gSavedColours.push_back((unsigned int)c);
			p = end;
		}
	}else if(strcmp(key, "textdraw_favourite_sprites") == 0){
		gFavSprites.clear();
		std::string v = value;
		for(size_t at = 0; at < v.size(); ){
			size_t end = v.find(' ', at);
			if(end == std::string::npos) end = v.size();
			if(end > at)
				gFavSprites.push_back(v.substr(at, end - at));
			at = end + 1;
		}
	}else if(strcmp(key, "textdraw_canvas") == 0){
		int snap = 1, grid = 0, through = 1, compare = 0, rulers = 1, locked = 0, exact = 0;
		sscanf(value, "%d %d %d %f %f %d %d %d %d %d", &gScreen, &snap, &grid, &gGridSize, &gNudge, &through, &compare,
			&rulers, &locked, &exact);
		gPixelExact = exact != 0;
		gRulers = rulers != 0;
		gGuidesLocked = locked != 0;
		gScreen = std::max(0, std::min(gScreen, NUM_TD_SCREENS - 1));
		gSnap = snap != 0;
		gGrid = grid != 0;
		gClickThroughLocked = through != 0;
		gCompare = compare != 0;
		gGridSize = std::max(1.0f, std::min(gGridSize, 200.0f));
		gNudge = std::max(0.01f, std::min(gNudge, 100.0f));
	}
	else if(strcmp(key, "textdraw_hud") == 0){
		int show = 0;
		sscanf(value, "%d %x %d %d %d %d %d %d %d %d %d %d", &show, (unsigned int*)&gHud.parts,
			&gHud.hour, &gHud.minute, &gHud.money, &gHud.health, &gHud.armour, &gHud.breath,
			&gHud.wanted, &gHud.weapon, &gHud.ammo, &gHud.clip);
		gHud.show = show != 0;
		if(gHud.weapon < 0 || gHud.weapon >= NUM_HUD_WEAPONS)
			gHud.weapon = 0;
	}else if(strcmp(key, "textdraw_hud_zone") == 0)
		snprintf(gHud.zone, sizeof(gHud.zone), "%s", value);
	else if(strcmp(key, "textdraw_hud_vehicle") == 0)
		snprintf(gHud.vehicle, sizeof(gHud.vehicle), "%s", value);
	else if(strcmp(key, "textdraw_hud_chat") == 0){
		int chat = 1, timestamps = 0;
		sscanf(value, "%d %d %d %d %d", &chat, &gHud.resolution, &gHud.fontSize, &gHud.pageSize, &timestamps);
		gHud.chat = chat != 0;
		gHud.timestamps = timestamps != 0;
		gHud.resolution = std::max(0, std::min(gHud.resolution, NUM_HUD_RESOLUTIONS - 1));
		gHud.fontSize = std::max(-3, std::min(gHud.fontSize, 5));
		gHud.pageSize = std::max(10, std::min(gHud.pageSize, 20));
	}else if(strcmp(key, "textdraw_hud_wsfix") == 0){
		int on = 0;
		sscanf(value, "%d %f %f %f %f", &on, &gHud.hudWidthScale, &gHud.hudHeightScale, &gHud.radarWidth, &gHud.radarHeight);
		gHud.wsfix = on != 0;
		gHud.hudWidthScale = std::max(0.25f, std::min(gHud.hudWidthScale, 4.0f));
		gHud.hudHeightScale = std::max(0.25f, std::min(gHud.hudHeightScale, 4.0f));
	}else if(strcmp(key, "textdraw_hud_samp") == 0){
		int fix = 1, icon = 1, round = 0;
		sscanf(value, "%d %d %d", &fix, &icon, &round);
		gHud.hudScaleFix = fix != 0;
		gHud.proportionalIcon = icon != 0;
		gHud.roundOutlines = round != 0;
	}else if(strcmp(key, "textdraw_hud_messages") == 0){
		// \n between messages, \\ for a backslash.
		size_t n = 0;
		for(const char *p = value; *p && n + 1 < sizeof(gHud.messages); p++){
			if(*p == '\\' && (p[1] == 'n' || p[1] == '\\')){
				gHud.messages[n++] = p[1] == 'n' ? '\n' : '\\';
				p++;
			}else if(*p != '\r' && *p != '\n')
				gHud.messages[n++] = *p;
		}
		gHud.messages[n] = '\0';
	}else
		return false;
	return true;
}

void
SaveTextDrawSettings(FILE *f)
{
	fprintf(f, "textdraw_code_target %d\n", (int)GetTextDrawCodeTarget());
	fprintf(f, "textdraw_background %d\n", gBackground);
	fprintf(f, "textdraw_canvas_colour %08X\n", gCanvasColour);
	fputs("textdraw_favourite_sprites", f);
	for(const std::string &fav : gFavSprites)
		fprintf(f, " %s", fav.c_str());
	fputc('\n', f);
	fputs("textdraw_saved_colours", f);
	for(unsigned int c : gSavedColours)
		fprintf(f, " %08X", c);
	fputc('\n', f);
	fprintf(f, "textdraw_canvas %d %d %d %g %g %d %d %d %d %d\n", gScreen, gSnap ? 1 : 0, gGrid ? 1 : 0, gGridSize, gNudge,
		gClickThroughLocked ? 1 : 0, gCompare ? 1 : 0, gRulers ? 1 : 0, gGuidesLocked ? 1 : 0, gPixelExact ? 1 : 0);
	fprintf(f, "textdraw_hud %d %x %d %d %d %d %d %d %d %d %d %d\n", gHud.show ? 1 : 0, (unsigned int)gHud.parts,
		gHud.hour, gHud.minute, gHud.money, gHud.health, gHud.armour, gHud.breath,
		gHud.wanted, gHud.weapon, gHud.ammo, gHud.clip);
	fprintf(f, "textdraw_hud_zone %s\n", gHud.zone);
	fprintf(f, "textdraw_hud_vehicle %s\n", gHud.vehicle);
	fprintf(f, "textdraw_hud_chat %d %d %d %d %d\n", gHud.chat ? 1 : 0, gHud.resolution, gHud.fontSize,
		gHud.pageSize, gHud.timestamps ? 1 : 0);
	fprintf(f, "textdraw_hud_samp %d %d %d\n", gHud.hudScaleFix ? 1 : 0, gHud.proportionalIcon ? 1 : 0,
		gHud.roundOutlines ? 1 : 0);
	fprintf(f, "textdraw_hud_wsfix %d %.3f %.3f %.1f %.1f\n", gHud.wsfix ? 1 : 0, gHud.hudWidthScale,
		gHud.hudHeightScale, gHud.radarWidth, gHud.radarHeight);
	fputs("textdraw_hud_messages ", f);
	for(const char *p = gHud.messages; *p; p++)
		if(*p == '\n') fputs("\\n", f);
		else if(*p == '\\') fputs("\\\\", f);
		else fputc(*p, f);
	fputc('\n', f);
}

// ---- Game fonts ----

// font2 holds Beckett (0) above Bank Gothic (2), font1 Aharoni (1) above
// Pricedown (3). fonts.dat's first table is font2's, the second font1's.
static rw::Texture *gFontTex[2];
static int gPropWidths[2][208];
static int gUnpropWidth[2] = { 27, 27 };
static bool gFontsLoaded;

static void
loadFonts(void)
{
	if(gFontsLoaded)
		return;
	gFontsLoaded = true;
	int slot = AddTxdSlot("~td_fonts");
	LoadTxd(slot, "MODELS/FONTS.TXD");
	gFontTex[0] = FindTextureInTxd(slot, "font2");
	gFontTex[1] = FindTextureInTxd(slot, "font1");
	for(int f = 0; f < 2; f++)
		for(int i = 0; i < 208; i++)
			gPropWidths[f][i] = 27;
	FILE *file = fopen(GamePath("data/fonts.dat").c_str(), "r");
	if(file == nil)
		return;
	char line[512];
	int font = -1, count = 0;
	enum { NONE, PROP, UNPROP, OTHER } section = NONE;
	while(fgets(line, sizeof(line), file)){
		char *hash = strchr(line, '#');
		if(hash) *hash = '\0';
		if(strstr(line, "[FONT_ID]")){ section = OTHER; font++; count = 0; continue; }
		if(strstr(line, "[PROP]")){ section = PROP; continue; }
		if(strstr(line, "[UNPROP]")){ section = UNPROP; continue; }
		if(strchr(line, '[')){ section = OTHER; continue; }
		if(font < 0 || font > 1)
			continue;
		char *p = line;
		while(*p){
			while(*p && !isdigit((unsigned char)*p)) p++;
			if(!*p) break;
			int v = (int)strtol(p, &p, 10);
			if(section == PROP && count < 208)
				gPropWidths[font][count++] = v;
			else if(section == UNPROP)
				gUnpropWidth[font] = v;
		}
	}
	fclose(file);
}

// The glyph a character uses: its index in the 16-wide grid.
static int
glyphIndex(int font, unsigned char c)
{
	int i = c - 0x20;
	if(i < 0 || i >= 208)
		return 0;
	if(font == TD_FONT_BANK_GOTHIC || font == TD_FONT_PRICEDOWN){
		// These fonts sit in the lower rows and only have digits and capitals
		// (CFont::FindSubFontCharacter, 0x7192C0), Pricedown a few signs too.
		if(font == TD_FONT_PRICEDOWN){
			if(i == 1) return 208;	// !
			if(i == 4) return 93;	// $
			if(i == 7) return 206;	// '
			if(i == 8 || i == 9) return i + 86;	// ( )
			if(i == 14) return 207;	// .
		}
		if(i >= 16 && i <= 25) return i + 128;	// 0-9
		if(i == 26) return 154;	// :
		if(i >= 33 && i <= 58) return i + 122;	// A-Z
		if(i >= 65 && i <= 90) return i + 90;	// a-z, drawn as capitals
		return 0;
	}
	return i;
}

static int
fontTexture(int font)
{
	return font == TD_FONT_AHARONI || font == TD_FONT_PRICEDOWN ? 1 : 0;
}

static float
charAdvance(const TextDraw &td, unsigned char c)
{
	int t = fontTexture(td.font);
	int width = td.proportional ? gPropWidths[t][glyphIndex(td.font, c)] : gUnpropWidth[t];
	return (width + td.outline) * td.letterX;
}

// ---- Layout ----

struct Glyph { float x, y; int index; ImU32 colour; unsigned int rgba; int line; };

static ImU32
rgbaToImU32(unsigned int c)
{
	return IM_COL32(c >> 24 & 0xFF, c >> 16 & 0xFF, c >> 8 & 0xFF, c & 0xFF);
}

// A token's letter applied to the colour, as CFont::ParseToken (0x718F00)
// does: the colour letters set a HUD colour keeping the alpha, ~h~ multiplies
// by 1.5. Only these letters are remembered as the line's tag.
static bool
applyColourToken(char code, unsigned int *colour)
{
	unsigned int alpha = *colour & 0xFF;
	switch(code){
	case 'R': case 'r': *colour = 0xB4191D00 | alpha; return true;
	case 'G': case 'g': *colour = 0x36682C00 | alpha; return true;
	case 'B': case 'b': *colour = 0x323C7F00 | alpha; return true;
	case 'Y': case 'y': *colour = 0xE2C06300 | alpha; return true;
	case 'P': case 'p': *colour = 0xA86EFC00 | alpha; return true;
	case 'W': case 'w': case 'S': case 's': *colour = 0xE1E1E100 | alpha; return true;
	case 'l': *colour = 0x00000000 | alpha; return true;	// lower case only
	case 'H': case 'h':{
		unsigned int out = alpha;
		for(int shift = 8; shift <= 24; shift += 8)
			out |= (unsigned int)std::min(255.0f, ((*colour >> shift) & 0xFF) * 1.5f) << shift;
		*colour = out;
		return true;
	}
	}
	return false;
}

// The token at s[i] ('~'): its letter, and where the text goes on. The game
// reads only the first letter and skips to the closing '~' (~red~ is ~r~);
// an unclosed one hides the rest.
static char
readToken(const std::string &s, size_t i, size_t *next)
{
	size_t end = s.find('~', i + 1);
	*next = end == std::string::npos ? s.size() : end + 1;
	char code = i + 1 < s.size() ? s[i + 1] : '\0';
	// ~k~~ACTION~ is a key name, put in before any token is read
	// (CMessages::InsertPlayerControlKeysInString): its action isn't a colour.
	if((code == 'k' || code == 'K') && *next < s.size() && s[*next] == '~'){
		end = s.find('~', *next + 1);
		*next = end == std::string::npos ? s.size() : end + 1;
	}
	return code;
}

// How many continuation bytes this one starts, 0 when it is an ASCII byte or a
// stray one. The game's font knows nothing of UTF-8 and gives every byte a cell
// of its own, which is why a canvas that draws a character as a character
// counts it once instead (three cells make a box three times too wide).
static size_t
utf8ContinuationBytes(const std::string &s, size_t i)
{
	unsigned char lead = (unsigned char)s[i];
	size_t len = lead >= 0xF0 ? 4 : lead >= 0xE0 ? 3 : lead >= 0xC2 ? 2 : 1;
	if(len == 1)
		return 0;
	if(i + len > s.size())
		return 0;
	for(size_t k = 1; k < len; k++)
		if(((unsigned char)s[i + k] & 0xC0) != 0x80)
			return 0;
	return len - 1;
}

// SA-MP sets CFont::SetScale(letterX, letterY * 0.5) in 640x448 units
// (DrawTextDrawRuntimeText, 0x100B31A0): glyphs are 32 * x by 20 * y * 0.5
// (CFont::PrintChar) and lines 18 * y * 0.5 apart (ProcessCurrentString).
static const float TD_GLYPH_HEIGHT = 10.0f, TD_LINE_HEIGHT = 9.0f;	// per unit of letterY

static float
lineHeight(const TextDraw &td)
{
	return TD_LINE_HEIGHT * td.letterY;
}

// Glyphs in canvas units, and the text's bounds. Like CFont::ProcessCurrentString
// (0x71A220): a word that doesn't fit starts a new line, and every line starts
// in the textdraw's colour plus the last colour token before it, which the game
// copies to the line's start ("~%c~"). So ~y~a~n~b is yellow twice, but
// ~w~~h~a~n~b's second line is TextDrawColor highlighted, not white.
static void
layoutText(const TextDraw &td, std::vector<Glyph> &glyphs, float *minX, float *maxX, int *numLines)
{
	glyphs.clear();
	struct Line { std::vector<Glyph> glyphs; float width; };
	std::vector<Line> lines(1);
	lines[0].width = 0.0f;
	float wrap = td.alignment == 2 ? td.sizeY : td.alignment == 3 ? td.x - td.sizeX : td.sizeX - td.x;
	unsigned int colour = td.colour;
	char tag = '\0';
	auto newLine = [&](){
		lines.push_back(Line());
		lines.back().width = 0.0f;
		colour = td.colour;
		if(tag)
			applyColourToken(tag, &colour);
	};
	const std::string &s = td.text;
	bool inWord = false;
	for(size_t i = 0; i < s.size(); i++){
		char c = s[i];
		if(c == '~'){
			size_t next;
			char code = readToken(s, i, &next);
			if(code == 'n' || code == 'N'){
				newLine();
				inWord = false;
			}else if(applyColourToken(code, &colour))
				tag = code;
			i = next - 1;
			continue;
		}
		if(c == '\n'){
			newLine();
			inWord = false;
			continue;
		}
		// A character in another script takes one cell here, not one per byte.
		size_t extra = utf8ContinuationBytes(s, i);
		float advance = charAdvance(td, (unsigned char)c);
		if(c == ' ')
			inWord = false;
		else if(!inWord){
			// Word wrap at the text size: the whole word moves, like the client.
			inWord = true;
			float word = 0.0f;
			for(size_t k = i; k < s.size() && s[k] != ' ' && s[k] != '\n'; k++){
				if(s[k] == '~'){
					size_t next;
					char code = readToken(s, k, &next);
					if(code == 'n' || code == 'N')
						break;
					k = next - 1;
					continue;
				}
				word += charAdvance(td, (unsigned char)s[k]);
				k += utf8ContinuationBytes(s, k);
			}
			if(wrap > 0.0f && !lines.back().glyphs.empty() && lines.back().width + word > wrap)
				newLine();
		}
		Line *line = &lines.back();
		Glyph g;
		g.x = line->width;
		// '_' is the usual invisible character in textdraws, and a character in
		// another script has no glyph in the game's atlas at all.
		g.index = extra || c == '_' ? 0 : glyphIndex(td.font, (unsigned char)c);
		g.colour = rgbaToImU32(colour);
		g.rgba = colour;
		line->glyphs.push_back(g);
		line->width += advance;
		i += extra;
	}
	*minX = 1e9f;
	*maxX = -1e9f;
	for(size_t l = 0; l < lines.size(); l++){
		float start = td.alignment == 2 ? td.x - lines[l].width * 0.5f :
			td.alignment == 3 ? td.x - lines[l].width : td.x;
		*minX = std::min(*minX, start);
		*maxX = std::max(*maxX, start + lines[l].width);
		for(size_t g = 0; g < lines[l].glyphs.size(); g++){
			Glyph glyph = lines[l].glyphs[g];
			glyph.x += start;
			glyph.y = td.y + l * lineHeight(td);
			glyph.line = (int)l;
			glyphs.push_back(glyph);
		}
	}
	*numLines = (int)lines.size();
}

std::string
DescribeTextDrawColours(const TextDraw &td)
{
	std::vector<Glyph> glyphs;
	float minX, maxX;
	int lines;
	layoutText(td, glyphs, &minX, &maxX, &lines);
	std::string out;
	char buf[32];
	for(int l = 0; l < lines; l++){
		snprintf(buf, sizeof(buf), "%sline %d:", l ? "\n" : "", l + 1);
		out += buf;
		unsigned int last = 0;
		bool any = false;
		for(const Glyph &g : glyphs)
			if(g.line == l && (!any || g.rgba != last)){
				snprintf(buf, sizeof(buf), " %08X", g.rgba);
				out += buf;
				last = g.rgba;
				any = true;
			}
	}
	return out;
}

// The area a textdraw covers on the canvas: its box, sprite or text.
static void
textDrawBounds(const TextDraw &td, float *x0, float *y0, float *x1, float *y1)
{
	if(td.font == TD_FONT_SPRITE || td.font == TD_FONT_PREVIEW || td.font == TD_FONT_REFERENCE){
		*x0 = td.x; *y0 = td.y;
		*x1 = td.x + td.sizeX; *y1 = td.y + td.sizeY;
		return;
	}
	std::vector<Glyph> glyphs;
	float minX, maxX;
	int lines;
	layoutText(td, glyphs, &minX, &maxX, &lines);
	*y0 = td.y;
	*y1 = td.y + lines * lineHeight(td);
	if(td.useBox){
		if(td.alignment == 2){ *x0 = td.x - td.sizeY * 0.5f; *x1 = td.x + td.sizeY * 0.5f; }
		else if(td.alignment == 3){ *x0 = td.sizeX; *x1 = td.x; }
		else{ *x0 = td.x; *x1 = td.sizeX; }
		if(*x1 < *x0) std::swap(*x0, *x1);
		if(*y1 < *y0) std::swap(*y0, *y1);	// a negative letter height: the box grows up
		*x0 -= 2.0f; *x1 += 2.0f; *y0 -= 2.0f; *y1 += 2.0f;
	}else{
		*x0 = minX;
		*x1 = std::max(maxX, minX + 2.0f);
	}
}

// ---- Sprites ----

static std::map<std::string, int> gSpriteTxds;	// lower-case name -> slot

static std::string
lowered(const std::string &s)
{
	std::string out = s;
	for(size_t i = 0; i < out.size(); i++)
		out[i] = (char)tolower((unsigned char)out[i]);
	return out;
}

static bool
fileExists(const std::string &path)
{
	FILE *f = fopen(path.c_str(), "rb");
	if(f) fclose(f);
	return f != nil;
}

// The client looks sprite TXDs up in models/txd, then models and SA-MP's folder.
// SA-MP 0.3.DL (and open.mp): "mdl-2018:name" is a texture in the TXD of the
// custom model AddSimpleModel added as -2018, which players download. The
// open map's custom models give it here. -2 when the name isn't one.
static int
customSpriteTxdSlot(const std::string &name)
{
	if(name.size() < 5 || rw::strncmp_ci(name.c_str(), "mdl-", 4) != 0)
		return -2;
	int id = atoi(name.c_str() + 4);
	int editorId = EditorModelId(-id);
	if(id < 1000 || id > 30000 || !IsCustomModel(editorId))
		return -1;
	ObjectDef *obj = GetObjectDef(editorId);
	if(obj == nil || obj->m_txdSlot < 0)
		return -1;
	if(!IsTxdLoaded(obj->m_txdSlot))
		LoadTxd(obj->m_txdSlot);
	return obj->m_txdSlot;
}

static int
spriteTxdSlot(const std::string &name)
{
	int custom = customSpriteTxdSlot(name);
	if(custom != -2)
		return custom;	// not kept: the map, and its custom models, can change
	std::string key = lowered(name);
	std::map<std::string, int>::iterator it = gSpriteTxds.find(key);
	if(it != gSpriteTxds.end())
		return it->second;
	int slot = -1;
	static const char *dirs[] = { "models/txd/", "models/", "SAMP/" };
	for(int d = 0; d < 3 && slot < 0; d++){
		std::string path = GamePath((std::string(dirs[d]) + name + ".txd").c_str());
		if(!fileExists(path))
			continue;
		slot = AddTxdSlot(("~td_" + key).c_str());
		LoadTxd(slot, path.c_str());
	}
	gSpriteTxds[key] = slot;
	return slot;
}

static rw::Texture*
spriteTexture(const std::string &text, const char *serverTxd = nil)
{
	size_t colon = text.find(':');
	if(colon == std::string::npos)
		return nil;
	std::string dictionary = text.substr(0, colon);
	int slot = -1;
	if(serverTxd && serverTxd[0] && rw::strncmp_ci(dictionary.c_str(), "mdl-", 4) == 0)
		slot = LoadServerTextureTxd(serverTxd);
	if(slot < 0) slot = spriteTxdSlot(dictionary);
	return slot >= 0 ? FindTextureInTxd(slot, text.substr(colon + 1).c_str()) : nil;
}

bool
TextDrawSpriteTextureAvailable(const TextDraw &td)
{
	rw::Texture *texture = td.font == TD_FONT_SPRITE ? spriteTexture(td.text, td.serverTxd.c_str()) : nil;
	return texture && texture->raster;
}

static void
listSpriteTxds(std::vector<std::string> &out)
{
	static std::vector<std::string> cache;
	if(cache.empty()){
		static const char *dirs[] = { "models/txd", "SAMP" };
		for(int d = 0; d < 2; d++){
			std::string dir = GamePath(dirs[d]);
#ifdef _WIN32
			WIN32_FIND_DATAA fd;
			HANDLE h = FindFirstFileA((dir + "/*.txd").c_str(), &fd);
			if(h != INVALID_HANDLE_VALUE){
				do cache.push_back(std::string(fd.cFileName).substr(0, strlen(fd.cFileName) - 4));
				while(FindNextFileA(h, &fd));
				FindClose(h);
			}
#else
			DIR *dp = opendir(dir.c_str());
			if(dp == nil)
				continue;
			struct dirent *e;
			while((e = readdir(dp)) != nil){
				size_t len = strlen(e->d_name);
				if(len > 4 && rw::strcmp_ci(e->d_name + len - 4, ".txd") == 0)
					cache.push_back(std::string(e->d_name, len - 4));
			}
			closedir(dp);
#endif
		}
		std::sort(cache.begin(), cache.end(), [](const std::string &a, const std::string &b){
			return lowered(a) < lowered(b);
		});
	}
	out = cache;
}

// ---- Drawing ----

struct Canvas {
	float x, y, sx, sy;	// screen origin and units per canvas unit
	float areaY;	// the top of the free area, above the canvas when it is letterboxed
	ImVec2 at(float cx, float cy) const { return ImVec2(x + cx * sx, y + cy * sy); }
};

// The current frame's reserved bars are authoritative when a workspace changes.
// Viewport::WorkPos still describes the preceding frame during a mode switch.
static void
textDrawWorkspaceArea(ImVec2 *pos, ImVec2 *size)
{
	ImGuiViewportP *viewport = (ImGuiViewportP*)ImGui::GetMainViewport();
	ImRect work = viewport->GetBuildWorkRect();
#ifdef IMGUI_HAS_DOCK
	ImGuiDockNode *central = ImGui::DockBuilderGetCentralNode(ImHashStr("HaumeaTextDrawDockSpace"));
	if(central && central->Size.x > 32.0f && central->Size.y > 32.0f){
		ImRect center(central->Pos, ImVec2(central->Pos.x + central->Size.x, central->Pos.y + central->Size.y));
		center.ClipWith(work);
		if(center.GetWidth() > 32.0f && center.GetHeight() > 32.0f)
			work = center;
	}
#endif
	*pos = work.Min;
	*size = work.GetSize();
}

// The free area between the docked panels, beneath the workspace bar.
static void
canvasArea(ImVec2 *pos, ImVec2 *size)
{
	textDrawWorkspaceArea(pos, size);
}

// The screen fitted in a rectangle: its shape is the window's, as a
// player's screen would be, or the one picked.
static Canvas
fitCanvas(ImVec2 pos, ImVec2 size, float aspect)
{
	float w = size.x, h = size.x / aspect;
	if(h > size.y){
		h = size.y;
		w = h * aspect;
	}
	Canvas c;
	c.x = pos.x + (size.x - w) * 0.5f;
	c.y = pos.y + (size.y - h) * 0.5f;
	c.sx = w / 640.0f;
	c.sy = h / 448.0f;
	c.areaY = pos.y;
	return c;
}

static float
screenAspect(int screen)
{
	if(screen > TD_SCREEN_WINDOW && screen < NUM_TD_SCREENS)
		return gScreenAspects[screen];
	ImGuiViewport *vp = ImGui::GetMainViewport();
	return vp->Size.x / std::max(1.0f, vp->Size.y);
}

// The canvas as edited: fitted with a margin, then zoomed and panned.
static Canvas
getCanvas(void)
{
	ImVec2 pos, size;
	canvasArea(&pos, &size);
	float margin = ImGui::GetFontSize();
	Canvas c = fitCanvas(ImVec2(pos.x + margin, pos.y + margin),
		ImVec2(size.x - margin * 2.0f, size.y - margin * 2.0f), screenAspect(gScreen));
	float cx = pos.x + size.x * 0.5f, cy = pos.y + size.y * 0.5f;
	c.sx *= gZoom;
	c.sy *= gZoom;
	c.x = cx - 320.0f * c.sx + gPanX;
	c.y = cy - 224.0f * c.sy + gPanY;
	c.areaY = pos.y;
	return c;
}

float
TextDrawCanvasZoom(void)
{
	return gZoom;
}

// For UI scripts: a canvas x (or y) on the screen; ruler, the middle of the ruler strip.
float
TextDrawCanvasToScreen(float at, bool vertical, bool ruler)
{
	Canvas c = getCanvas();
	if(ruler){
		ImVec2 pos, size;
		canvasArea(&pos, &size);
		return (vertical ? pos.y : pos.x) + ImGui::GetFontSize() * 0.65f;
	}
	return vertical ? c.y + at * c.sy : c.x + at * c.sx;
}

// ---- Model previews ----

struct PreviewEntry {
	int model;
	float rx, ry, rz, zoom;
	int colour1, colour2;
	unsigned int background;
	rw::Raster *color, *depth;
	rw::Texture *texture;
	bool ready;
	bool failed;	// it couldn't be drawn: shown as its background, not tried again
	int lastFrame;
};
static PreviewEntry gPreviews[16];
static const int PREVIEW_SIZE = 256;

static bool
previewMatches(const PreviewEntry &e, const TextDraw &td)
{
	return e.texture && e.model == td.previewModel && e.rx == td.previewRotX && e.ry == td.previewRotY &&
		e.rz == td.previewRotZ && e.zoom == td.previewZoom && e.colour1 == td.previewColour1 &&
		e.colour2 == td.previewColour2 && e.background == td.backgroundColour;
}

// The texture of a preview, or nil until it has been rendered.
static rw::Texture*
previewTexture(const TextDraw &td)
{
	int frame = ImGui::GetFrameCount();
	for(int i = 0; i < 16; i++)
		if(previewMatches(gPreviews[i], td)){
			gPreviews[i].lastFrame = frame;
			return gPreviews[i].ready && !gPreviews[i].failed ? gPreviews[i].texture : nil;
		}
	// Take the least recently used entry and render it next frame.
	PreviewEntry *e = &gPreviews[0];
	for(int i = 1; i < 16; i++)
		if(gPreviews[i].lastFrame < e->lastFrame)
			e = &gPreviews[i];
	if(e->texture == nil){
		e->color = rw::Raster::create(PREVIEW_SIZE, PREVIEW_SIZE, 32, rw::Raster::C8888|rw::Raster::CAMERATEXTURE);
		e->depth = rw::Raster::create(PREVIEW_SIZE, PREVIEW_SIZE, 0, rw::Raster::ZBUFFER);
		if(e->color == nil || e->depth == nil)
			return nil;
		e->texture = rw::Texture::create(e->color);
		e->texture->setFilter(rw::Texture::LINEAR);
	}
	e->model = td.previewModel;
	e->rx = td.previewRotX; e->ry = td.previewRotY; e->rz = td.previewRotZ;
	e->zoom = td.previewZoom;
	e->colour1 = td.previewColour1; e->colour2 = td.previewColour2;
	e->background = td.backgroundColour;
	e->ready = false;
	e->failed = false;
	e->lastFrame = frame;
	return nil;
}

// ---- Pixel-exact canvas ----

// The screen drawn at the player's resolution, then scaled onto the canvas:
// where a box edge meets an edge inside a picture, the pixels come out as a
// player at that resolution sees them. Last frame's draw list is drawn here,
// before the world.
static ImDrawList *gExactList;
static rw::Camera *gExactCamera;
static rw::Raster *gExactColor, *gExactDepth;
static rw::Texture *gExactTexture;
static bool gExactReady;

static bool
exactTarget(int w, int h)
{
	if(gExactCamera == nil){
		gExactCamera = rw::Camera::create();
		rw::Frame *frame = rw::Frame::create();
		if(gExactCamera == nil || frame == nil)
			return false;
		gExactCamera->setFrame(frame);
		gExactCamera->setNearPlane(0.1f);
		gExactCamera->setFarPlane(10.0f);
	}
	if(gExactColor && gExactColor->width == w && gExactColor->height == h)
		return true;
	if(gExactTexture){
		gExactTexture->raster = nil;
		gExactTexture->destroy();
		gExactTexture = nil;
	}
	if(gExactColor) gExactColor->destroy();
	if(gExactDepth) gExactDepth->destroy();
	// No alpha channel: blending translucent textdraws in lowers the alpha,
	// and the picture, drawn over the editor's background, came out darker.
	gExactColor = rw::Raster::create(w, h, 32, rw::Raster::C888|rw::Raster::CAMERATEXTURE);
	gExactDepth = rw::Raster::create(w, h, 0, rw::Raster::ZBUFFER);
	gExactReady = false;
	if(gExactColor == nil || gExactDepth == nil)
		return false;
	gExactTexture = rw::Texture::create(gExactColor);
	gExactTexture->setFilter(rw::Texture::NEAREST);	// zoomed in, its pixels show
	return true;
}

static void
renderExactScreen(int w, int h)
{
	if(gExactList == nil || gExactList->VtxBuffer.Size == 0 || !exactTarget(w, h))
		return;
	gExactCamera->frameBuffer = gExactColor;
	gExactCamera->zBuffer = gExactDepth;
	rw::RGBA black = { 0, 0, 0, 255 };
	gExactCamera->clear(&black, rw::Camera::CLEARIMAGE|rw::Camera::CLEARZ);
	gExactCamera->beginUpdate();
	static std::vector<rw::RWDEVICE::Im2DVertex> verts;
	const ImDrawList *list = gExactList;
	verts.resize(list->VtxBuffer.Size);
	float recipZ = 1.0f / gExactCamera->nearPlane;
	for(int i = 0; i < list->VtxBuffer.Size; i++){
		const ImDrawVert &v = list->VtxBuffer[i];
		verts[i].setScreenX(v.pos.x);
		verts[i].setScreenY(v.pos.y);
		verts[i].setScreenZ(rw::im2d::GetNearZ());
		verts[i].setCameraZ(gExactCamera->nearPlane);
		verts[i].setRecipCameraZ(recipZ);
		verts[i].setColor(v.col & 0xFF, v.col >> 8 & 0xFF, v.col >> 16 & 0xFF, v.col >> 24 & 0xFF);
		verts[i].setU(v.uv.x, recipZ);
		verts[i].setV(v.uv.y, recipZ);
	}
	int vertexAlpha = rw::GetRenderState(rw::VERTEXALPHA);
	int srcBlend = rw::GetRenderState(rw::SRCBLEND), dstBlend = rw::GetRenderState(rw::DESTBLEND);
	int ztest = rw::GetRenderState(rw::ZTESTENABLE), cull = rw::GetRenderState(rw::CULLMODE);
	void *raster = rw::GetRenderStatePtr(rw::TEXTURERASTER);
	int filter = rw::GetRenderState(rw::TEXTUREFILTER);
	int addrU = rw::GetRenderState(rw::TEXTUREADDRESSU), addrV = rw::GetRenderState(rw::TEXTUREADDRESSV);
	int alphaTest = rw::GetRenderState(rw::ALPHATESTFUNC);
	// No alpha test, as for the interface: it cut the faint edges of soft
	// sprites (lamp_shad_64's glow) away.
	rw::SetRenderState(rw::ALPHATESTFUNC, rw::ALPHAALWAYS);
	rw::SetRenderState(rw::VERTEXALPHA, 1);
	rw::SetRenderState(rw::SRCBLEND, rw::BLENDSRCALPHA);
	rw::SetRenderState(rw::DESTBLEND, rw::BLENDINVSRCALPHA);
	rw::SetRenderState(rw::ZTESTENABLE, 0);
	rw::SetRenderState(rw::CULLMODE, rw::CULLNONE);
	for(int i = 0; i < list->CmdBuffer.Size; i++){
		const ImDrawCmd &cmd = list->CmdBuffer[i];
		if(cmd.UserCallback || cmd.ElemCount == 0)
			continue;
		rw::Texture *tex = (rw::Texture*)cmd.GetTexID();
		if(tex && tex->raster){
			rw::SetRenderStatePtr(rw::TEXTURERASTER, tex->raster);
			rw::SetRenderState(rw::TEXTUREADDRESSU, tex->getAddressU());
			rw::SetRenderState(rw::TEXTUREADDRESSV, tex->getAddressV());
			rw::SetRenderState(rw::TEXTUREFILTER, tex->getFilter());
		}else
			rw::SetRenderStatePtr(rw::TEXTURERASTER, nil);
		rw::im2d::RenderIndexedPrimitive(rw::PRIMTYPETRILIST, verts.data() + cmd.VtxOffset,
			list->VtxBuffer.Size - cmd.VtxOffset, list->IdxBuffer.Data + cmd.IdxOffset, cmd.ElemCount);
	}
	rw::SetRenderState(rw::VERTEXALPHA, vertexAlpha);
	rw::SetRenderState(rw::SRCBLEND, srcBlend);
	rw::SetRenderState(rw::DESTBLEND, dstBlend);
	rw::SetRenderState(rw::ZTESTENABLE, ztest);
	rw::SetRenderState(rw::CULLMODE, cull);
	rw::SetRenderStatePtr(rw::TEXTURERASTER, raster);
	rw::SetRenderState(rw::TEXTUREFILTER, filter);
	rw::SetRenderState(rw::TEXTUREADDRESSU, addrU);
	rw::SetRenderState(rw::TEXTUREADDRESSV, addrV);
	rw::SetRenderState(rw::ALPHATESTFUNC, alphaTest);
	gExactCamera->endUpdate();
	gExactReady = true;
}

void
TextDrawsBeforeWorldRender(void)
{
	if(gMode && gPixelExact){
		int r = std::max(0, std::min(gHud.resolution, NUM_HUD_RESOLUTIONS - 1));
		renderExactScreen(gHudResolutions[r].w, gHudResolutions[r].h);
	}
	for(int i = 0; i < 16; i++){
		PreviewEntry &e = gPreviews[i];
		if(e.texture == nil || e.ready)
			continue;
		rw::RGBA bg = { (uint8)(e.background >> 24), (uint8)(e.background >> 16),
			(uint8)(e.background >> 8), (uint8)e.background };
		rw::V3d rot = { e.rx, e.ry, e.rz };
		// A model that can't be drawn stays failed while it is asked for: it
		// used to be asked for, loaded and drawn again every frame.
		e.failed = !RenderModelPreviewToRaster(e.model, rot, e.zoom, e.colour1, e.colour2, bg, e.color, e.depth);
		e.ready = true;
		return;	// one per frame
	}
}

// ---- Reference images ----

struct RefImage { rw::Image *image; rw::Texture *texture; };
static std::map<std::string, RefImage> gRefImages;	// by file, nil when it can't be read

// PNG, BMP or TGA, read once and kept as 32-bit for the eyedropper.
static const RefImage*
refImage(const std::string &path)
{
	std::map<std::string, RefImage>::iterator it = gRefImages.find(path);
	if(it != gRefImages.end())
		return it->second.texture ? &it->second : nil;
	RefImage ref = { nil, nil };
	std::string low = lowered(path);
	auto ends = [&](const char *ext){ return low.size() > 4 && low.compare(low.size() - 4, 4, ext) == 0; };
	rw::Image *image = ends(".png") ? rw::readPNG(path.c_str()) : ends(".bmp") ? rw::readBMP(path.c_str()) :
		ends(".tga") ? rw::readTGA(path.c_str()) : nil;
	if(image){
		image->unpalettize();
		image->convertTo32();
		rw::Raster *raster = rw::Raster::createFromImage(image);
		if(raster){
			ref.image = image;
			ref.texture = rw::Texture::create(raster);
			ref.texture->setFilter(rw::Texture::LINEAR);
		}else
			image->destroy();
	}
	if(ref.texture == nil && !path.empty())
		log("TextDraws: could not read the reference image %s (PNG, BMP or TGA)\n", path.c_str());
	gRefImages[path] = ref;
	return ref.texture ? &gRefImages[path] : nil;
}

// The image's colour at a point of the canvas, or false when it isn't over it.
static bool
refImageColour(const TextDraw &td, float cx, float cy, unsigned int *rgba)
{
	const RefImage *ref = refImage(td.text);
	if(ref == nil || td.sizeX == 0.0f || td.sizeY == 0.0f)
		return false;
	float u = (cx - td.x) / td.sizeX, v = (cy - td.y) / td.sizeY;	// a negative size mirrors it
	if(u < 0.0f || u >= 1.0f || v < 0.0f || v >= 1.0f)
		return false;
	rw::Image *im = ref->image;
	int px = std::min(im->width - 1, (int)(u * im->width)), py = std::min(im->height - 1, (int)(v * im->height));
	const uint8 *p = im->pixels + py * im->stride + px * 4;
	*rgba = (unsigned int)p[0] << 24 | (unsigned int)p[1] << 16 | (unsigned int)p[2] << 8 | 0xFF;
	return true;
}

static void
drawGlyphs(ImDrawList *dl, const Canvas &cv, const TextDraw &td, const std::vector<Glyph> &glyphs,
	float dx, float dy, ImU32 overrideColour, bool useOverride)
{
	rw::Texture *tex = gFontTex[fontTexture(td.font)];
	if(tex == nil)
		return;
	float w = 32.0f * td.letterX, h = TD_GLYPH_HEIGHT * td.letterY;
	for(size_t i = 0; i < glyphs.size(); i++){
		const Glyph &g = glyphs[i];
		if(g.index == 0)
			continue;
		float u = (g.index % 16) / 16.0f, v = (g.index / 16) / 12.8f;
		dl->AddImage((ImTextureID)tex, cv.at(g.x + dx, g.y + dy), cv.at(g.x + dx + w, g.y + dy + h),
			ImVec2(u, v), ImVec2(u + 1.0f / 16.0f, v + 1.0f / 12.8f), useOverride ? overrideColour : g.colour);
	}
}

// ---- Text in another script ----

// The game's textdraw atlas holds Latin glyphs only (glyphIndex above), so a
// string in another script lands on arbitrary cells and every byte takes one
// cell of its own. Haumea previews those with its own UI font instead:
// style.cpp merges a face that covers the script, and the atlas loads glyphs on
// demand at the size it is asked for, so this is sharp at any zoom. It stands
// in for a client with a CJK font patch (wm_sachs and the like) - the catalog
// and the map file need no change, the client does. Text the game can draw
// keeps the faithful path; only a multibyte UTF-8 sequence takes this one.
static bool
textInOtherScript(const std::string &s)
{
	for(size_t i = 0; i + 1 < s.size(); i++){
		unsigned char lead = (unsigned char)s[i], next = (unsigned char)s[i + 1];
		// A lead byte. 0x80..0xBF on its own is a Windows-1252 character,
		// which the atlas does carry, so that stays on the game's path.
		if(lead >= 0xC2 && lead <= 0xF4 && (next & 0xC0) == 0x80)
			return true;
	}
	return false;
}

// CFont's glyphs are 10 * letterY units tall (TD_GLYPH_HEIGHT above); a font's
// em box is taller than its capitals, so the size is chosen to keep the height
// close. Only the height is matched: the advance comes from the font, so a
// string is laid out like the font does it, not like the client does.
static const float TD_OTHER_SCRIPT_EM = 0.8f;

static void
drawOtherScriptText(ImDrawList *dl, const Canvas &cv, const TextDraw &td)
{
	ImFont *font = ImGui::GetFont();
	if(font == nil || td.letterY <= 0.0f || cv.sx <= 0.0f || cv.sy <= 0.0f)
		return;
	float size = TD_GLYPH_HEIGHT * td.letterY * cv.sy / TD_OTHER_SCRIPT_EM;
	if(size < 1.0f)
		size = 1.0f;	// a thumbnail keeps its text, however small it gets
	// The wrap width as the game reads it (layoutText above), in this canvas's
	// pixels: only the right and left alignments wrap at TextDrawTextSize.
	float wrap = (td.alignment == 2 ? td.sizeY : td.alignment == 3 ? td.x - td.sizeX : td.sizeX - td.x) * cv.sx;

	// One item per character, holding its UTF-8 bytes, so a wrap can move a
	// whole word the way the client moves it.
	struct Item { std::string utf8; unsigned int colour; };
	struct Line { std::vector<Item> items; float width; };
	std::vector<Line> lines(1);
	lines[0].width = 0.0f;
	std::vector<Item> word;
	float wordWidth = 0.0f;
	unsigned int colour = td.colour;
	auto measure = [&](const std::string &t){
		return font->CalcTextSizeA(size, 1e9f, 0.0f, t.c_str()).x;
	};
	auto newLine = [&](bool resetColour){
		// Like ProcessCurrentString, a new line starts in the textdraw's colour
		// again; a wrap keeps the colour the text is already in.
		lines.push_back(Line());
		lines.back().width = 0.0f;
		if(resetColour)
			colour = td.colour;
	};
	auto flushWord = [&](){
		if(word.empty())
			return;
		if(wrap > 0.0f && !lines.back().items.empty() && lines.back().width + wordWidth > wrap)
			newLine(false);
		for(const Item &item : word){
			lines.back().items.push_back(item);
			lines.back().width += measure(item.utf8);
		}
		word.clear();
		wordWidth = 0.0f;
	};
	const std::string &s = td.text;
	for(size_t i = 0; i < s.size(); ){
		char c = s[i];
		if(c == '~'){
			size_t next;
			char code = readToken(s, i, &next);
			if(code == 'n' || code == 'N'){
				flushWord();
				newLine(true);
			}else
				applyColourToken(code, &colour);
			i = next;
			continue;
		}
		if(c == '\n'){
			flushWord();
			newLine(true);
			i++;
			continue;
		}
		// One item for the whole character, so it is never cut in half.
		size_t len = 1 + utf8ContinuationBytes(s, i);
		// '_' is the usual invisible character in textdraws.
		bool blank = c == ' ' || c == '_';
		Item item;
		item.utf8 = blank ? " " : s.substr(i, len);
		item.colour = colour;
		i += len;
		if(blank){
			flushWord();
			lines.back().items.push_back(item);
			lines.back().width += measure(item.utf8);
		}else{
			word.push_back(item);
			wordWidth += measure(item.utf8);
		}
	}
	flushWord();
	while(lines.size() > 1 && lines.back().items.empty())
		lines.pop_back();

	ImU32 back = rgbaToImU32(td.backgroundColour);
	float step = lineHeight(td);
	auto draw = [&](float ox, float oy, ImU32 overrideColour, bool useOverride){
		for(size_t l = 0; l < lines.size(); l++){
			float x = td.alignment == 2 ? td.x - lines[l].width * 0.5f / cv.sx :
				td.alignment == 3 ? td.x - lines[l].width / cv.sx : td.x;
			float y = td.y + l * step;
			// A run of one colour goes to the list in one call.
			for(size_t i = 0; i < lines[l].items.size(); ){
				unsigned int runColour = lines[l].items[i].colour;
				std::string run;
				while(i < lines[l].items.size() && lines[l].items[i].colour == runColour)
					run += lines[l].items[i++].utf8;
				dl->AddText(font, size, cv.at(x + ox, y + oy),
					useOverride ? overrideColour : rgbaToImU32(runColour), run.c_str());
				x += measure(run) / cv.sx;
			}
		}
	};
	if(td.outline > 0){
		// CFont's edge all the way round, as far as the outline is wide.
		float o = (float)td.outline;
		for(int k = 0; k < 8; k++){
			float a = k * 3.14159265f / 4.0f;
			draw(cosf(a) * o, sinf(a) * o, back, true);
		}
	}else if(td.shadow > 0)
		draw((float)td.shadow, (float)td.shadow, back, true);
	draw(0.0f, 0.0f, 0, false);
}

static void
drawTextDraw(ImDrawList *dl, const Canvas &cv, const TextDraw &td)
{
	float x0, y0, x1, y1;
	textDrawBounds(td, &x0, &y0, &x1, &y1);
	if(td.font == TD_FONT_REFERENCE){
		// Drawn in its colour: the alpha is how much of it shows.
		const RefImage *ref = refImage(td.text);
		ImVec2 uv0(x1 < x0 ? 1.0f : 0.0f, y1 < y0 ? 1.0f : 0.0f), uv1(1.0f - uv0.x, 1.0f - uv0.y);
		if(x1 < x0) std::swap(x0, x1);
		if(y1 < y0) std::swap(y0, y1);
		if(ref)
			dl->AddImage((ImTextureID)ref->texture, cv.at(x0, y0), cv.at(x1, y1), uv0, uv1, rgbaToImU32(td.colour));
		else{
			dl->AddRect(cv.at(x0, y0), cv.at(x1, y1), IM_COL32(255, 80, 80, 200));
			dl->AddText(cv.at(x0 + 2.0f, y0 + 2.0f), IM_COL32(255, 120, 120, 255), td.text.empty() ? "?" : td.text.c_str());
		}
		return;
	}
	if(td.font == TD_FONT_SPRITE){
		// A negative width or height mirrors it, as in game (e.g. LD_DRV:tvcorn's four corners).
		rw::Texture *tex = spriteTexture(td.text, td.serverTxd.c_str());
		ImVec2 uv0(x1 < x0 ? 1.0f : 0.0f, y1 < y0 ? 1.0f : 0.0f), uv1(1.0f - uv0.x, 1.0f - uv0.y);
		if(x1 < x0) std::swap(x0, x1);
		if(y1 < y0) std::swap(y0, y1);
		if(tex)
			dl->AddImage((ImTextureID)tex, cv.at(x0, y0), cv.at(x1, y1), uv0, uv1, rgbaToImU32(td.colour));
		else
			dl->AddRect(cv.at(x0, y0), cv.at(x1, y1), IM_COL32(255, 80, 80, 200));
		return;
	}
	if(td.font == TD_FONT_PREVIEW){
		// Rendered with its own background, rotation and zoom, then drawn in
		// TextDrawColor like any textdraw texture (DrawTextDrawRuntimeTexture).
		// A negative size mirrors it, as it does a sprite. The rendered
		// picture is upside down and the wrong way round (the preview camera
		// is turned half round), so u and v run from 1 unless mirrored.
		rw::Texture *tex = previewTexture(td);
		ImVec2 uv0(x1 < x0 ? 0.0f : 1.0f, y1 < y0 ? 0.0f : 1.0f), uv1(1.0f - uv0.x, 1.0f - uv0.y);
		if(x1 < x0) std::swap(x0, x1);
		if(y1 < y0) std::swap(y0, y1);
		if(tex)
			dl->AddImage((ImTextureID)tex, cv.at(x0, y0), cv.at(x1, y1), uv0, uv1, rgbaToImU32(td.colour));
		else
			dl->AddRectFilled(cv.at(x0, y0), cv.at(x1, y1), rgbaToImU32(td.backgroundColour));
		return;
	}
	if(td.useBox)
		dl->AddRectFilled(cv.at(x0, y0), cv.at(x1, y1), rgbaToImU32(td.boxColour));
	if(textInOtherScript(td.text)){
		drawOtherScriptText(dl, cv, td);
		return;
	}
	std::vector<Glyph> glyphs;
	float minX, maxX;
	int lines;
	layoutText(td, glyphs, &minX, &maxX, &lines);
	ImU32 back = rgbaToImU32(td.backgroundColour);
	if(td.outline > 0){
		float o = (float)td.outline;
		if(gHud.wsfix && gHud.roundOutlines){
			// The newer Widescreen Fix: a ring of 16, as far as the edge in
			// 640 x 448 units, kept round on wide screens.
			float square = (4.0f / 3.0f) / (640.0f * cv.sx / (448.0f * cv.sy));
			for(int k = 0; k < 16; k++){
				float a = k * 3.14159265f / 8.0f;
				drawGlyphs(dl, cv, td, glyphs, cosf(a) * o * square, sinf(a) * o, back, true);
			}
		}else{
			// CFont's edge is in the player's pixels, so it is thinner on
			// bigger screens: in 640 x 448 units at the HUD's resolution.
			int r = std::max(0, std::min(gHud.resolution, NUM_HUD_RESOLUTIONS - 1));
			float ex = o * 640.0f / gHudResolutions[r].w, ey = o * 448.0f / gHudResolutions[r].h;
			for(int oy = -1; oy <= 1; oy++)
				for(int ox = -1; ox <= 1; ox++)
					if(ox || oy)
						drawGlyphs(dl, cv, td, glyphs, ox * ex, oy * ey, back, true);
		}
	}else if(td.shadow > 0)
		drawGlyphs(dl, cv, td, glyphs, (float)td.shadow, (float)td.shadow, back, true);
	drawGlyphs(dl, cv, td, glyphs, 0.0f, 0.0f, 0, false);
}

// ---- The game's HUD ----

// CHudColours.
static const unsigned int HUD_RED = 0xB4191DFF, HUD_GREEN = 0x36682CFF, HUD_LIGHT_BLUE = 0xACCBF1FF,
	HUD_LIGHT_GRAY = 0xE1E1E1FF, HUD_GOLD = 0x906210FF, HUD_BLACK = 0x000000FF;

static rw::Texture*
hudWeaponIcon(int weapon)
{
	static rw::Texture *icons[NUM_HUD_WEAPONS];
	static bool looked[NUM_HUD_WEAPONS];
	if(weapon < 0 || weapon >= NUM_HUD_WEAPONS)
		return nil;
	if(gHudWeapons[weapon].model == nil)
		return spriteTexture("hud:fist");
	if(!looked[weapon]){
		looked[weapon] = true;
		int slot = FindTxdSlot(gHudWeapons[weapon].model);
		if(slot >= 0){
			if(!IsTxdLoaded(slot))
				LoadTxd(slot);
			icons[weapon] = FindTextureInTxd(slot, (std::string(gHudWeapons[weapon].model) + "icon").c_str());
		}
	}
	return icons[weapon];
}

// CFont text as a textdraw draws it: the same fonts and edges. The HUD sets
// CFont's scale itself, a textdraw's letter height is halved first.
// fromBottom: y is where the line ends (CFont::PrintStringFromBottom).
static void
hudText(ImDrawList *dl, const Canvas &cv, const char *text, float x, float y, int font, float sx, float sy,
	int alignment, bool proportional, int edge, unsigned int colour, bool fromBottom = false)
{
	TextDraw td;
	InitTextDraw(&td);
	td.text = text;
	td.font = font;
	td.letterX = sx; td.letterY = sy * 2.0f;
	td.x = x; td.y = fromBottom ? y - lineHeight(td) : y;
	td.alignment = alignment;
	td.sizeY = 640.0f;	// CFont::SetCentreSize, for centred text
	td.proportional = proportional;
	td.shadow = 0;
	td.outline = edge;
	td.colour = colour;
	td.backgroundColour = HUD_BLACK;
	drawTextDraw(dl, cv, td);
}

// CSprite2d::DrawBarChart with its black border.
static void
hudBar(ImDrawList *dl, const Canvas &cv, float x, float y, float w, float h, float bx, float by, int percent,
	unsigned int colour)
{
	float fill = x + w * std::min(100, std::max(0, percent)) / 100.0f;
	unsigned int dark = ((colour >> 25 & 0x7F) << 24) | ((colour >> 17 & 0x7F) << 16) | ((colour >> 9 & 0x7F) << 8) | 0xFF;
	dl->AddRectFilled(cv.at(x, y), cv.at(fill, y + h), rgbaToImU32(colour));
	dl->AddRectFilled(cv.at(fill, y), cv.at(x + w, y + h), rgbaToImU32(dark));
	ImU32 black = IM_COL32(0, 0, 0, 255);
	dl->AddRectFilled(cv.at(x, y), cv.at(x + w, y + by), black);
	dl->AddRectFilled(cv.at(x, y + h - by), cv.at(x + w, y + h), black);
	dl->AddRectFilled(cv.at(x, y), cv.at(x + bx, y + h), black);
	dl->AddRectFilled(cv.at(x + w - bx, y), cv.at(x + w, y + h), black);
}

// CChatWindow: Arial bold at GetFontSize's size for the screen width
// (14 under 1024, 16 under 1400, 18, 20 from 1600, two per /fontsize step),
// outlined in black, from 45, 10 in the player's pixels. White unless a
// {RRGGBB} changes it.
static void
drawChat(ImDrawList *dl, const Canvas &cv)
{
	int r = std::max(0, std::min(gHud.resolution, NUM_HUD_RESOLUTIONS - 1));
	int w = gHudResolutions[r].w;
	float size = (float)(w < 1024 ? 14 : w < 1400 ? 16 : w < 1600 ? 18 : 20) + 2.0f * gHud.fontSize;
	float px = cv.sy * 448.0f / gHudResolutions[r].h;	// screen pixels per player pixel
	ImFont *font = ImGui::GetFont();
	float fontSize = size * px;
	float x0 = cv.x + 45.0f * px, y = cv.y + 10.0f * px;
	std::vector<std::string> lines;
	for(const char *p = gHud.messages; *p; ){
		const char *e = strchr(p, '\n');
		lines.push_back(std::string(p, e ? e - p : strlen(p)));
		p = e ? e + 1 : p + strlen(p);
	}
	// The last pageSize lines, at the bottom of the page as new ones arrive.
	size_t first = lines.size() > (size_t)gHud.pageSize ? lines.size() - gHud.pageSize : 0;
	y += (gHud.pageSize - (int)(lines.size() - first)) * fontSize;
	for(size_t i = first; i < lines.size(); i++, y += fontSize){
		float x = x0;
		ImU32 colour = IM_COL32_WHITE;
		std::string text = gHud.timestamps ? "[12:00:00] " + lines[i] : lines[i];
		size_t at = 0;
		while(at < text.size()){
			size_t open = text.find('{', at);
			size_t end = open;
			// A run of text up to the next {RRGGBB}.
			while(open != std::string::npos){
				bool hex = open + 7 < text.size() && text[open + 7] == '}';
				for(int k = 1; hex && k <= 6; k++)
					hex = isxdigit((unsigned char)text[open + k]) != 0;
				if(hex)
					break;
				open = text.find('{', open + 1);
			}
			end = open == std::string::npos ? text.size() : open;
			if(end > at){
				const char *b = text.c_str() + at, *e = text.c_str() + end;
				for(int oy = -1; oy <= 1; oy++)
					for(int ox = -1; ox <= 1; ox++)
						if(ox || oy)
							dl->AddText(font, fontSize, ImVec2(x + ox * px, y + oy * px), IM_COL32(0, 0, 0, 255), b, e);
				dl->AddText(font, fontSize, ImVec2(x, y), colour, b, e);
				dl->AddText(font, fontSize, ImVec2(x + 0.5f * px, y), colour, b, e);	// Arial bold
				x += font->CalcTextSizeA(fontSize, FLT_MAX, 0.0f, b, e).x + 0.5f * px;
			}
			if(open == std::string::npos)
				break;
			unsigned int rgb = (unsigned int)strtoul(text.substr(open + 1, 6).c_str(), nil, 16);
			colour = IM_COL32(rgb >> 16 & 0xFF, rgb >> 8 & 0xFF, rgb & 0xFF, 255);
			at = open + 8;
		}
	}
}

// Where the HUD's 640x448 numbers land. The game stretches them to the
// screen. The Widescreen Fix makes them square ((4/3) / aspect across) and
// scales them by HudWidthScale and HudHeightScale, keeping each part against
// its edge (its Frontend.ixx); the radar is only made square.
struct HudLayout {
	float kx, ky;
	float R(float x) const { return 640.0f - kx * (640.0f - x); }	// from the right edge
	float T(float y) const { return y * ky; }	// from the top
	float B(float y) const { return 448.0f - ky * (448.0f - y); }	// from the bottom
};

static void
drawHud(ImDrawList *dl, const Canvas &cv)
{
	const int p = gHud.parts;
	char buf[64];
	float square = (4.0f / 3.0f) / (640.0f * cv.sx / (448.0f * cv.sy));
	HudLayout l = { 1.0f, 1.0f };
	if(gHud.wsfix){
		l.kx = square * gHud.hudWidthScale;
		l.ky = gHud.hudHeightScale;
	}
	if(p & HUD_RADAR){
		// CHud::DrawRadar: 40 from the left, 104 above the bottom, 94 x 76.
		float aspect = 640.0f * cv.sx / (448.0f * cv.sy);
		float kx = gHud.wsfix ? square : 1.0f, ky = 1.0f;
		float w = (gHud.wsfix ? gHud.radarWidth : 94.0f) * kx, h = gHud.wsfix ? gHud.radarHeight : 76.0f;
		if(gHud.hudScaleFix && !gHud.wsfix && aspect >= 1.6f){
			// SA-MP's ProcessHudScaleFix, round CHud::DrawRadar: 96 x 82, the
			// x scale 0.00222 / aspect of the width and y 0.00242 of the height.
			kx = 0.00222f * 640.0f / aspect;
			ky = 0.00242f * 448.0f;
			w = 96.0f * kx;
			h = 82.0f * ky;
		}
		ImVec2 centre = cv.at(40.0f * kx + w * 0.5f, 448.0f - 104.0f * ky + h * 0.5f);
		DrawRadarOnCanvas(dl, centre, ImVec2(w * 0.5f * cv.sx, h * 0.5f * cv.sy), cv.sy);
	}
	if(p & HUD_WEAPON){
		// 32 from the right and 111 (0.1734 of the width) for the icon, 47 x 58.
		rw::Texture *icon = hudWeaponIcon(gHud.weapon);
		// The Widescreen Fix's ProportionalWeaponIcon sizes its width as its height.
		float right = gHud.wsfix && gHud.proportionalIcon ? l.R(497.0f) + 47.0f * l.ky * (640.0f / 448.0f) /
			(640.0f * cv.sx / (448.0f * cv.sy)) : l.R(544.0f);
		if(icon)
			dl->AddImage((ImTextureID)icon, cv.at(l.R(497.0f), l.T(20.0f)), cv.at(right, l.T(78.0f)));
	}
	if((p & HUD_AMMO) && gHud.weapon >= 0 && gHud.weapon < NUM_HUD_WEAPONS && gHudWeapons[gHud.weapon].ammo){
		// CHud::DrawAmmo: "total-clip", or the total for one-shot clips.
		int clip = gHudWeapons[gHud.weapon].clip;
		int inClip = std::min(gHud.clip, gHud.ammo);
		if(clip <= 1 || clip >= 1000)
			snprintf(buf, sizeof(buf), "%d", gHud.ammo);
		else
			snprintf(buf, sizeof(buf), "%d-%d", std::min(gHud.ammo - inClip, 9999), inClip);
		hudText(dl, cv, buf, l.R(520.5f), l.T(63.0f), TD_FONT_AHARONI, 0.3f * l.kx, 0.7f * l.ky, 2, true, 1, HUD_LIGHT_BLUE);
	}
	if(p & HUD_CLOCK){
		snprintf(buf, sizeof(buf), "%02d:%02d", gHud.hour, gHud.minute);
		hudText(dl, cv, buf, l.R(608.0f), l.T(22.0f), TD_FONT_PRICEDOWN, 0.55f * l.kx, 1.1f * l.ky, 3, false, 2,
			HUD_LIGHT_GRAY);
	}
	if(p & HUD_HEALTH){	// 141 from the right, 77 - 10 down, 109 wide at max health 100 / 176
		float w = 109.0f * 100.0f / 176.0f;
		hudBar(dl, cv, l.R(499.0f + 109.0f - w), l.T(67.0f), w * l.kx, 9.0f * l.ky, 2.0f * l.kx, 2.0f * l.ky,
			gHud.health, HUD_RED);
	}
	if((p & HUD_ARMOUR) && gHud.armour > 1)	// 94 from the right, 48 - 3 down
		hudBar(dl, cv, l.R(546.0f), l.T(45.0f), 62.0f * l.kx, 9.0f * l.ky, 2.0f * l.kx, 2.0f * l.ky,
			gHud.armour, HUD_LIGHT_GRAY);
	if(p & HUD_BREATH)	// under water: 62 - 6 down
		hudBar(dl, cv, l.R(546.0f), l.T(56.0f), 62.0f * l.kx, 9.0f * l.ky, 2.0f * l.kx, 2.0f * l.ky,
			gHud.breath, HUD_LIGHT_BLUE);
	if(p & HUD_MONEY){
		if(gHud.money < 0)
			snprintf(buf, sizeof(buf), "-$%07d", -gHud.money);
		else
			snprintf(buf, sizeof(buf), "$%08d", gHud.money);
		hudText(dl, cv, buf, l.R(608.0f), l.T(77.0f), TD_FONT_PRICEDOWN, 0.55f * l.kx, 1.1f * l.ky, 3, false, 2,
			gHud.money < 0 ? HUD_RED : HUD_GREEN);
	}
	if((p & HUD_WANTED) && gHud.wanted > 0)	// six stars (']'), the first on the right, 18 apart
		for(int i = 0; i < 6; i++)
			hudText(dl, cv, "]", l.R(611.0f - 18.0f * i), l.T(102.0f), TD_FONT_BECKETT, 0.605f * l.kx, 1.21f * l.ky,
				3, true, 1, i < gHud.wanted ? HUD_GOLD : HUD_BLACK);
	if((p & HUD_VEHICLE) && gHud.vehicle[0])
		hudText(dl, cv, gHud.vehicle, l.R(608.0f), l.B(344.0f), TD_FONT_BANK_GOTHIC, 1.0f * l.kx, 1.5f * l.ky,
			3, true, 2, HUD_GREEN);
	if((p & HUD_ZONE) && gHud.zone[0]){
		// The line ends 104 - 76 above the bottom.
		hudText(dl, cv, gHud.zone, l.R(608.0f), l.B(420.0f), TD_FONT_BECKETT, 1.2f * l.kx, 1.9f * l.ky, 3, true, 2,
			HUD_LIGHT_BLUE, true);
	}
	if(gHud.chat)
		drawChat(dl, cv);
}

static void
hudSettings(void)
{
	bool changed = ImGui::Checkbox(L("Game HUD"), &gHud.show);
	ImGui::SetItemTooltip("%s", _("The game's HUD under the textdraws, where SA-MP shows it, to check they don't cover it."));
	ImGui::SameLine();
	if(ImGui::SmallButton(ICON_FA_GEAR "##hud_settings"))
		ImGui::OpenPopup("##hud_popup");
	ImGui::SetItemTooltip("%s", _("What the HUD shows"));
	if(ImGui::BeginPopup("##hud_popup")){
		static const struct { int bit; const char *label; } parts[] = {
			{ HUD_RADAR, N_("Radar") }, { HUD_WEAPON, N_("Weapon") }, { HUD_AMMO, N_("Ammo") },
			{ HUD_HEALTH, N_("Health") }, { HUD_ARMOUR, N_("Armour") }, { HUD_BREATH, N_("Breath") },
			{ HUD_MONEY, N_("Money") }, { HUD_CLOCK, N_("Clock") }, { HUD_WANTED, N_("Wanted level") },
			{ HUD_VEHICLE, N_("Vehicle name") }, { HUD_ZONE, N_("Zone name") },
		};
		ImGui::SeparatorText(_("Show"));
		for(int i = 0; i < IM_ARRAYSIZE(parts); i++){
			if(i % 3)
				ImGui::SameLine(ImGui::GetFontSize() * 8.0f * (i % 3));
			changed |= ImGui::CheckboxFlags(_(parts[i].label), &gHud.parts, parts[i].bit);
		}
		ImGui::SetItemTooltip("%s", _("SA-MP shows zone names with EnableZoneNames, the clock with TogglePlayerClock."));
		ImGui::SeparatorText(_("Values"));
		float w = ImGui::GetFontSize() * 10.0f;
		ImGui::PushItemWidth(w);
		changed |= ImGui::SliderInt(L("Health"), &gHud.health, 0, 100, "%d%%");
		changed |= ImGui::SliderInt(L("Armour"), &gHud.armour, 0, 100, "%d%%");
		changed |= ImGui::SliderInt(L("Breath"), &gHud.breath, 0, 100, "%d%%");
		changed |= ImGui::InputInt(L("Money"), &gHud.money, 100, 10000);
		gHud.money = std::max(-9999999, std::min(gHud.money, 99999999));
		int time[2] = { gHud.hour, gHud.minute };
		if(ImGui::DragInt2(L("Time"), time, 0.2f, 0, 59, "%02d")){
			gHud.hour = std::max(0, std::min(time[0], 23));
			gHud.minute = std::max(0, std::min(time[1], 59));
			changed = true;
		}
		changed |= ImGui::SliderInt(L("Wanted level"), &gHud.wanted, 0, 6);
		const char *weapons[NUM_HUD_WEAPONS];
		for(int i = 0; i < NUM_HUD_WEAPONS; i++)
			weapons[i] = _(gHudWeapons[i].name);
		changed |= ImGui::Combo(L("Weapon"), &gHud.weapon, weapons, NUM_HUD_WEAPONS);
		int ammo[2] = { gHud.ammo, gHud.clip };
		if(ImGui::InputInt2(L("Ammo, in clip"), ammo)){
			gHud.ammo = std::max(0, ammo[0]);
			gHud.clip = std::max(0, ammo[1]);
			changed = true;
		}
		ImGui::InputText(L("Vehicle name"), gHud.vehicle, sizeof(gHud.vehicle));
		changed |= ImGui::IsItemDeactivatedAfterEdit();
		ImGui::InputText(L("Zone name"), gHud.zone, sizeof(gHud.zone));
		changed |= ImGui::IsItemDeactivatedAfterEdit();
		ImGui::PopItemWidth();

		ImGui::SeparatorText(_("Widescreen Fix"));
		changed |= ImGui::Checkbox(L("As with the Widescreen Fix"), &gHud.wsfix);
		ImGui::SetItemTooltip("%s", _("ThirteenAG's GTA SA Widescreen Fix: the HUD unstretched and smaller, the radar round.\n"
			"Many players have it; check your textdraws both ways."));
		if(gHud.wsfix){
			ImGui::PushItemWidth(w);
			float scale[2] = { gHud.hudWidthScale, gHud.hudHeightScale };
			if(ImGui::DragFloat2(L("HudWidthScale, HudHeightScale"), scale, 0.01f, 0.25f, 4.0f, "%.2f")){
				gHud.hudWidthScale = scale[0];
				gHud.hudHeightScale = scale[1];
				changed = true;
			}
			float radar[2] = { gHud.radarWidth, gHud.radarHeight };
			if(ImGui::DragFloat2(L("RadarWidth, RadarHeight"), radar, 0.5f, 10.0f, 300.0f, "%.1f")){
				gHud.radarWidth = radar[0];
				gHud.radarHeight = radar[1];
				changed = true;
			}
			ImGui::SetItemTooltip("%s", _("As in its GTASA.WidescreenFix.ini (0.8, 0.8, 85.5 and 78 by default)"));
			ImGui::PopItemWidth();
			changed |= ImGui::Checkbox(L("ProportionalWeaponIcon"), &gHud.proportionalIcon);
			ImGui::SetItemTooltip("%s", _("The weapon icon keeps its shape (1 in the ini it ships with). Older builds\n"
				"and 0 squeeze it to the HUD's width."));
			changed |= ImGui::Checkbox(L("Round text outlines"), &gHud.roundOutlines);
			ImGui::SetItemTooltip("%s", _("Newer builds draw text outlines as a soft ring that grows with the\n"
				"screen (ReplaceTextShadowWithOutline). Off: the game's own sharp edge, a few\n"
				"pixels wide, as older builds and the plain game draw it. Textdraws too."));
		}
		changed |= ImGui::Checkbox(L("/hudscalefix"), &gHud.hudScaleFix);
		ImGui::SetItemTooltip("%s", _("SA-MP 0.3.7-R3 and newer: on 16:10 and wider screens the radar is made round\n"
			"(96 x 82, sized by the screen's height). On unless the player turned it off\n"
			"(nohudscalefix=1 in sa-mp.cfg). Not with the Widescreen Fix, which sizes the radar itself."));

		ImGui::SeparatorText(_("SA-MP chat"));
		changed |= ImGui::Checkbox(L("Chat"), &gHud.chat);
		ImGui::SameLine();
		changed |= ImGui::Checkbox(L("Timestamps"), &gHud.timestamps);
		ImGui::PushItemWidth(w);
		char res[NUM_HUD_RESOLUTIONS][24];
		const char *resNames[NUM_HUD_RESOLUTIONS];
		for(int i = 0; i < NUM_HUD_RESOLUTIONS; i++){
			snprintf(res[i], sizeof(res[i]), "%d x %d", gHudResolutions[i].w, gHudResolutions[i].h);
			resNames[i] = res[i];
		}
		changed |= ImGui::Combo(L("Player's screen"), &gHud.resolution, resNames, NUM_HUD_RESOLUTIONS);
		ImGui::SetItemTooltip("%s", _("The chat and text outlines are sized in the player's pixels: on a bigger\n"
			"screen the chat takes less of it and outlines are thinner. Textdraws too."));
		changed |= ImGui::SliderInt(L("/fontsize"), &gHud.fontSize, -3, 5);
		changed |= ImGui::SliderInt(L("/pagesize"), &gHud.pageSize, 10, 20);
		ImGui::PopItemWidth();
		ImGui::InputTextMultiline(L("Messages"), gHud.messages, sizeof(gHud.messages),
			ImVec2(ImGui::GetFontSize() * 22.0f, ImGui::GetFontSize() * 6.0f));
		changed |= ImGui::IsItemDeactivatedAfterEdit();
		ImGui::SetItemTooltip("%s", _("One message per line; {RRGGBB} changes the colour."));

		if(ImGui::Button(L("Reset"))){
			bool show = gHud.show;
			gHud = HUD_DEFAULTS;
			gHud.show = show;
			changed = true;
		}
		ImGui::SetItemTooltip("%s", _("SA-MP's defaults"));
		ImGui::EndPopup();
	}
	if(changed)
		SaveEditorSettings();
}

// ---- Selection and editing ----

static bool
onBoard(const TextDraw &td)
{
	return td.board == gBoard;
}

// Drawn on the canvas: on this board and not hidden.
static bool
shown(const TextDraw &td)
{
	return onBoard(td) && !td.hidden;
}

static bool
isSelected(int i)
{
	return std::binary_search(gSel.begin(), gSel.end(), i);
}

static void
selectOnly(int i)
{
	gLayerSelection = false;
	if(i < 0)
		gIsolatedGroup.clear();
	gSel.clear();
	if(i >= 0)
		gSel.push_back(i);
	gSelected = i;
}

static void
addToSelection(int i)
{
	gLayerSelection = false;
	std::vector<int>::iterator it = std::lower_bound(gSel.begin(), gSel.end(), i);
	if(it == gSel.end() || *it != i)
		gSel.insert(it, i);
	gSelected = i;
}

static void
toggleSelected(int i)
{
	gLayerSelection = false;
	std::vector<int>::iterator it = std::lower_bound(gSel.begin(), gSel.end(), i);
	if(it != gSel.end() && *it == i){
		gSel.erase(it);
		if(gSelected == i)
			gSelected = gSel.empty() ? -1 : gSel.back();
	}else
		addToSelection(i);
}

// After an undo, a delete or a board switch: only what exists and is on the board.
static void
validateSelection(void)
{
	std::vector<TextDraw> &tds = GetTextDraws();
	std::vector<int> keep;
	for(int i : gSel)
		if(i >= 0 && i < (int)tds.size() && onBoard(tds[i]))
			keep.push_back(i);
	gSel.swap(keep);
	if(!isSelected(gSelected))
		gSelected = gSel.empty() ? -1 : gSel.back();
	if(!gIsolatedGroup.empty()){
		bool exists = false;
		for(const TextDraw &td : tds)
			if(onBoard(td) && td.group == gIsolatedGroup){
				exists = true;
				break;
			}
		if(!exists)
			gIsolatedGroup.clear();
	}
}

void
SelectTextDraw(int index)
{
	EnterTextDrawMode();
	std::vector<TextDraw> &tds = GetTextDraws();
	if(index >= 0 && index < (int)tds.size())
		gBoard = tds[index].board;
	selectOnly(index);
}

// Canvas-unit bounds, smallest corner first (mirrored sprites have them the other way).
static void
boundsOf(const TextDraw &td, float *x0, float *y0, float *x1, float *y1)
{
	textDrawBounds(td, x0, y0, x1, y1);
	if(*x1 < *x0) std::swap(*x0, *x1);
	if(*y1 < *y0) std::swap(*y0, *y1);
}

// Moved as a whole: a text's wrap or box edge (TextSize's x, a position on
// the screen for left and right alignment) moves with it.
struct ShapeSettings;
static bool parseShapeSpec(const std::string &spec, ShapeSettings *sh);
static std::string shapeSpec(const ShapeSettings &sh);
static void moveShapeSpec(std::string &spec, float dx, float dy);

static void
moveBy(TextDraw &td, float dx, float dy)
{
	td.x += dx;
	td.y += dy;
	if(!td.shape.empty())
		moveShapeSpec(td.shape, dx, dy);
	bool isText = td.font < TD_FONT_SPRITE;
	if(isText && (td.alignment == 1 || td.alignment == 3))
		td.sizeX += dx;
}

static void
deleteSelected(void)
{
	std::vector<TextDraw> &tds = GetTextDraws();
	for(int k = (int)gSel.size() - 1; k >= 0; k--)
		tds.erase(tds.begin() + gSel[k]);
	selectOnly(-1);
	TextDrawsChanged();
}

// A group name not in use: the name's words with the first free number
// from `from` ("Rounded box 1" gives "Rounded box 2" when 1 is taken).
static std::string
uniqueGroupName(std::string base, int from = 1)
{
	size_t space = base.find_last_of(' ');
	if(space != std::string::npos && base.find_first_not_of("0123456789", space + 1) == std::string::npos)
		base.resize(space);
	const std::vector<TextDraw> &tds = GetTextDraws();
	for(int n = from; ; n++){
		std::string name = base + " " + std::to_string(n);
		bool used = false;
		for(const TextDraw &td : tds)
			used |= td.group == name;
		if(!used)
			return name;
	}
}

// Copies after the last one, offset unless in place; the copies end up selected.
static void
appendCopies(const std::vector<TextDraw> &copies, float offset)
{
	std::vector<TextDraw> &tds = GetTextDraws();
	gSel.clear();
	// A copied group is a group of its own: "Rounded box 1" copies as "Rounded box 2".
	std::map<std::string, std::string> renamed;
	for(TextDraw td : copies){
		if(!td.group.empty()){
			if(renamed.find(td.group) == renamed.end()){
				std::string name = uniqueGroupName(td.group);
				// Not one an earlier copied group was given either.
				for(int n = 2; ; n++){
					bool taken = false;
					for(const auto &r : renamed)
						taken |= r.second == name;
					if(!taken)
						break;
					name = uniqueGroupName(td.group, n);
				}
				renamed[td.group] = name;
			}
			td.group = renamed[td.group];
		}
		td.name[0] = '\0';
		td.board = gBoard;
		moveBy(td, offset, offset);
		tds.push_back(td);
		gSel.push_back((int)tds.size() - 1);
	}
	gSelected = gSel.empty() ? -1 : gSel.back();
	TextDrawsChanged();
}

static std::vector<TextDraw>
selectedCopies(void)
{
	std::vector<TextDraw> out;
	std::vector<TextDraw> &tds = GetTextDraws();
	for(int i : gSel)
		out.push_back(tds[i]);
	return out;
}

static std::vector<TextDraw> gClipboard;

// One step towards the top (later in the file) or the bottom, past the
// textdraws that aren't selected.
static void
restackSelected(bool up)
{
	std::vector<TextDraw> &tds = GetTextDraws();
	bool moved = false;
	if(up){
		for(int k = (int)gSel.size() - 1; k >= 0; k--){
			int i = gSel[k];
			if(i + 1 < (int)tds.size() && !isSelected(i + 1)){
				std::swap(tds[i], tds[i + 1]);
				gSel[k] = i + 1;
				if(gSelected == i) gSelected = i + 1;
				moved = true;
			}
		}
	}else{
		for(size_t k = 0; k < gSel.size(); k++){
			int i = gSel[k];
			if(i > 0 && !isSelected(i - 1)){
				std::swap(tds[i], tds[i - 1]);
				gSel[k] = i - 1;
				if(gSelected == i) gSelected = i - 1;
				moved = true;
			}
		}
	}
	if(moved)
		TextDrawsChanged();
}

// The selection moved to before file index `at` (tds.size() for the end),
// in its order; it stays selected.
static void
moveSelectionTo(int at)
{
	std::vector<TextDraw> &tds = GetTextDraws();
	if(gSel.empty())
		return;
	std::vector<TextDraw> moving, rest;
	int primary = -1, before = 0;
	for(int i = 0; i < (int)tds.size(); i++){
		if(isSelected(i)){
			if(i == gSelected) primary = (int)moving.size();
			moving.push_back(tds[i]);
		}else{
			if(i < at) before++;
			rest.push_back(tds[i]);
		}
	}
	rest.insert(rest.begin() + before, moving.begin(), moving.end());
	tds.swap(rest);
	gSel.clear();
	for(int k = 0; k < (int)moving.size(); k++)
		gSel.push_back(before + k);
	gSelected = primary >= 0 ? before + primary : gSel.back();
	TextDrawsChanged();
}

// A group is one name on its textdraws, kept together in the file.
static void
groupSelected(const char *name)
{
	std::vector<TextDraw> &tds = GetTextDraws();
	gIsolatedGroup.clear();
	for(int i : gSel)
		tds[i].group = name;
	if(!gSel.empty())
		moveSelectionTo(gSel.front());
	TextDrawsChanged();
}

static void
selectGroup(const std::string &group, bool add)
{
	std::vector<TextDraw> &tds = GetTextDraws();
	if(!add)
		selectOnly(-1);
	for(int i = 0; i < (int)tds.size(); i++)
		if(onBoard(tds[i]) && tds[i].group == group)
			addToSelection(i);
}

// Shift adds a group; Ctrl toggles all of its members together.
static void
toggleGroupSelection(const std::string &group, int key)
{
	std::vector<TextDraw> &tds = GetTextDraws();
	bool any = false, all = true;
	for(int i = 0; i < (int)tds.size(); i++)
		if(onBoard(tds[i]) && tds[i].group == group){
			any = true;
			all &= isSelected(i);
		}
	if(!any)
		return;
	if(all){
		gSel.erase(std::remove_if(gSel.begin(), gSel.end(), [&](int i){ return onBoard(tds[i]) && tds[i].group == group; }), gSel.end());
		if(!isSelected(gSelected))
			gSelected = gSel.empty() ? -1 : gSel.back();
	}else{
		selectGroup(group, true);
		if(isSelected(key))
			gSelected = key;
	}
}

static std::string
newGroupName(void)
{
	std::vector<TextDraw> &tds = GetTextDraws();
	for(int n = 1; ; n++){
		std::string name = std::string(_("Group")) + " " + std::to_string(n);
		bool used = false;
		for(const TextDraw &td : tds)
			used |= td.group == name;
		if(!used)
			return name;
	}
}

// ---- Shapes: what they are made of ----

// Shapes made of several textdraws, as Textdraw Studio builds them: boxes
// of LD_SPAC:white, circles of LD_BEAT:chit, a ring of LD_BEAT:cring and
// arrows of LD_BEAT's. They are added as a group, and each part keeps the
// shape's settings, so the shape can be changed and resized after.
enum { SHAPE_ROUNDED_BOX, SHAPE_CIRCLE, SHAPE_BAR, SHAPE_RING, SHAPE_PILL, SHAPE_OUTLINE,
	SHAPE_CIRCLE_OUTLINE, SHAPE_GRADIENT, SHAPE_TRIANGLE, SHAPE_ARROW, SHAPE_PLUS, NUM_SHAPES };
static const char *gShapeKeys[NUM_SHAPES] = { "rounded", "circle", "bar", "ring", "pill", "outline",
	"circleoutline", "gradient", "triangle", "arrow", "plus" };
static const char *gShapeNames[NUM_SHAPES] = { N_("Rounded box"), N_("Circle"), N_("Progress bar"),
	N_("Progress ring"), N_("Pill"), N_("Outline"), N_("Circle outline"), N_("Gradient"), N_("Triangle"),
	N_("Arrow"), N_("Plus") };
// The order the list offers them in.
static const int gShapeOrder[NUM_SHAPES] = { SHAPE_ROUNDED_BOX, SHAPE_PILL, SHAPE_OUTLINE, SHAPE_CIRCLE,
	SHAPE_CIRCLE_OUTLINE, SHAPE_TRIANGLE, SHAPE_ARROW, SHAPE_PLUS, SHAPE_GRADIENT, SHAPE_BAR, SHAPE_RING };
enum { DIR_RIGHT, DIR_LEFT, DIR_UP, DIR_DOWN };

struct ShapeSettings {
	int kind;
	float x, y, w, h;	// the box it fills
	float radius;	// the rounded box's corners
	float thickness;	// a border or line, the ring's dots, the gap between a bar's segments
	int segments;	// the ring's dots, a bar's segments, a gradient's bands, a triangle's steps
	int value;	// percent, for the bar and the ring
	int dir;	// where a bar fills to, a gradient runs, a triangle or an arrow points
	unsigned int colour;
	unsigned int back;	// the empty part, the gradient's end, the border, an outline's inside
	float shadow;	// a drop shadow this far down and right, 0 for none
	unsigned int shadowColour;
};
static ShapeSettings gShape = { SHAPE_ROUNDED_BOX, 0.0f, 0.0f, 160.0f, 90.0f, 12.0f, 0.0f, 24, 65, DIR_RIGHT,
	0x2B2F36FF, 0x000000AA, 0.0f, 0x00000080 };

// Shapes whose sprites overlap: a see-through colour would show the
// overlaps darker, so they are always opaque.
static bool
shapeOpaque(const ShapeSettings &sh)
{
	return sh.kind == SHAPE_ROUNDED_BOX || sh.kind == SHAPE_PILL || (sh.kind == SHAPE_CIRCLE && sh.thickness > 0.0f);
}

// A new kind's own settings, keeping the place, the size and the colour.
static void
shapeDefaults(ShapeSettings &sh, int kind)
{
	sh.kind = kind;
	sh.dir = kind == SHAPE_GRADIENT ? DIR_DOWN : kind == SHAPE_TRIANGLE || kind == SHAPE_ARROW ? DIR_UP : DIR_RIGHT;
	switch(kind){
	case SHAPE_ROUNDED_BOX: case SHAPE_PILL: case SHAPE_CIRCLE:
		sh.thickness = 0.0f; sh.back = 0xFFFFFFFF; break;
	case SHAPE_OUTLINE: sh.thickness = 2.0f; sh.back = 0x00000000; break;
	case SHAPE_PLUS: sh.thickness = std::max(1.0f, std::min(sh.w, sh.h) * 0.25f); break;
	case SHAPE_RING: sh.thickness = 10.0f; sh.segments = 24; sh.back = 0x000000AA; break;
	case SHAPE_BAR: sh.thickness = 2.0f; sh.segments = 1; sh.back = 0x000000AA; break;
	case SHAPE_GRADIENT: sh.segments = 16; sh.back = 0x00000000; break;
	case SHAPE_TRIANGLE: sh.segments = 12; break;
	}
}

static std::string
shapeSpec(const ShapeSettings &sh)
{
	char buf[256];
	snprintf(buf, sizeof(buf), "%s %.2f %.2f %.2f %.2f %.2f %.2f %d %d %d %08X %08X %.2f %08X",
		gShapeKeys[sh.kind], sh.x, sh.y, sh.w, sh.h, sh.radius, sh.thickness, sh.segments, sh.value, sh.dir,
		sh.colour, sh.back, sh.shadow, sh.shadowColour);
	return buf;
}

static bool
parseShapeSpec(const std::string &spec, ShapeSettings *sh)
{
	char key[32];
	ShapeSettings s = gShape;
	if(sscanf(spec.c_str(), "%31s %f %f %f %f %f %f %d %d %d %x %x %f %x", key, &s.x, &s.y, &s.w, &s.h,
	   &s.radius, &s.thickness, &s.segments, &s.value, &s.dir, &s.colour, &s.back, &s.shadow, &s.shadowColour) != 14)
		return false;
	for(int k = 0; k < NUM_SHAPES; k++)
		if(strcmp(key, gShapeKeys[k]) == 0){
			s.kind = k;
			*sh = s;
			return true;
		}
	return false;
}

// A shape's part moved: so is the place its settings give.
static void
moveShapeSpec(std::string &spec, float dx, float dy)
{
	ShapeSettings sh;
	if(parseShapeSpec(spec, &sh)){
		sh.x += dx;
		sh.y += dy;
		spec = shapeSpec(sh);
	}
}

static TextDraw
shapeSprite(const char *sprite, float x, float y, float w, float h, unsigned int colour)
{
	TextDraw td;
	InitTextDraw(&td);
	td.font = TD_FONT_SPRITE;
	td.text = sprite;
	td.x = x; td.y = y;
	td.sizeX = w; td.sizeY = h;
	td.colour = colour;
	td.board = gBoard;
	return td;
}

// LD_BEAT:chit's circle doesn't fill its texture: these are its margins.
static const float CHIT_LEFT = 0.164f, CHIT_RIGHT = 0.164f, CHIT_TOP = 0.158f, CHIT_BOTTOM = 0.17f;
// LD_BEAT:cring's ring, 10 of its 64 pixels in from each side.
static const float CRING_MARGIN = 10.0f / 64.0f;

static unsigned int
mixColour(unsigned int a, unsigned int b, float t)
{
	unsigned int out = 0;
	for(int s = 0; s < 32; s += 8){
		float ca = (float)(a >> s & 0xFF), cb = (float)(b >> s & 0xFF);
		out |= (unsigned int)(ca + (cb - ca) * t + 0.5f) << s;
	}
	return out;
}

// The shape's sprites, in drawing order.
static void
buildShape(const ShapeSettings &sh, std::vector<TextDraw> &parts)
{
	const char *white = "LD_SPAC:white";
	if(shapeOpaque(sh) && ((sh.colour | sh.back | sh.shadowColour) & 0xFF) != 0xFF){
		ShapeSettings solid = sh;
		solid.colour |= 0xFF;
		solid.back |= 0xFF;
		solid.shadowColour |= 0xFF;
		buildShape(solid, parts);
		return;
	}
	float x = sh.x, y = sh.y, w = std::max(1.0f, sh.w), h = std::max(1.0f, sh.h);
	float t = std::max(0.0f, std::min(sh.thickness, std::min(w, h) * 0.5f));
	auto box = [&](float bx, float by, float bw, float bh, unsigned int c){
		if(bw > 0.01f && bh > 0.01f)
			parts.push_back(shapeSprite(white, bx, by, bw, bh, c));
	};
	// An ellipse filling the box: the chit, grown by its margins.
	auto ellipse = [&](float bx, float by, float bw, float bh, unsigned int c){
		if(bw <= 0.01f || bh <= 0.01f)
			return;
		float sw = bw / (1.0f - CHIT_LEFT - CHIT_RIGHT), sh2 = bh / (1.0f - CHIT_TOP - CHIT_BOTTOM);
		parts.push_back(shapeSprite("LD_BEAT:chit", bx - sw * CHIT_LEFT, by - sh2 * CHIT_TOP, sw, sh2, c));
	};
	// A middle column and two side strips, so translucent colours only
	// double up where the corners' circles are.
	auto rounded = [&](float bx, float by, float bw, float bh, float r, unsigned int c){
		r = std::max(0.0f, std::min(r, std::min(bw, bh) * 0.5f));
		box(bx + r, by, bw - 2.0f * r, bh, c);
		box(bx, by + r, r, bh - 2.0f * r, c);
		box(bx + bw - r, by + r, r, bh - 2.0f * r, c);
		if(r > 0.0f){
			ellipse(bx, by, 2.0f * r, 2.0f * r, c);
			ellipse(bx + bw - 2.0f * r, by, 2.0f * r, 2.0f * r, c);
			ellipse(bx, by + bh - 2.0f * r, 2.0f * r, 2.0f * r, c);
			ellipse(bx + bw - 2.0f * r, by + bh - 2.0f * r, 2.0f * r, 2.0f * r, c);
		}
	};
	switch(sh.kind){
	case SHAPE_ROUNDED_BOX:
	case SHAPE_PILL: {
		float r = sh.kind == SHAPE_PILL ? std::min(w, h) * 0.5f : sh.radius;
		if(t > 0.0f){
			rounded(x, y, w, h, r, sh.back);
			rounded(x + t, y + t, w - 2.0f * t, h - 2.0f * t, std::max(0.0f, r - t), sh.colour);
		}else
			rounded(x, y, w, h, r, sh.colour);
		break;
	}
	case SHAPE_CIRCLE:
		if(t > 0.0f){
			ellipse(x, y, w, h, sh.back);
			ellipse(x + t, y + t, w - 2.0f * t, h - 2.0f * t, sh.colour);
		}else
			ellipse(x, y, w, h, sh.colour);
		break;
	case SHAPE_OUTLINE:
		t = std::max(t, 0.5f);
		if(sh.back & 0xFF)
			box(x + t, y + t, w - 2.0f * t, h - 2.0f * t, sh.back);
		box(x, y, w, t, sh.colour);
		box(x, y + h - t, w, t, sh.colour);
		box(x, y + t, t, h - 2.0f * t, sh.colour);
		box(x + w - t, y + t, t, h - 2.0f * t, sh.colour);
		break;
	case SHAPE_CIRCLE_OUTLINE: {
		float sw = w / (1.0f - 2.0f * CRING_MARGIN), sh2 = h / (1.0f - 2.0f * CRING_MARGIN);
		parts.push_back(shapeSprite("LD_BEAT:cring", x - sw * CRING_MARGIN, y - sh2 * CRING_MARGIN, sw, sh2, sh.colour));
		break;
	}
	case SHAPE_PLUS:
		t = std::max(t, 0.5f);
		box(x + (w - t) * 0.5f, y, t, h, sh.colour);
		box(x, y + (h - t) * 0.5f, (w - t) * 0.5f, t, sh.colour);
		box(x + (w + t) * 0.5f, y + (h - t) * 0.5f, (w - t) * 0.5f, t, sh.colour);
		break;
	case SHAPE_ARROW: {
		static const char *arrows[] = { "LD_BEAT:right", "LD_BEAT:left", "LD_BEAT:up", "LD_BEAT:down" };
		parts.push_back(shapeSprite(arrows[sh.dir & 3], x, y, w, h, sh.colour));
		break;
	}
	case SHAPE_TRIANGLE: {
		// Steps of boxes, widest at the base; the point is the way it points.
		int n = std::max(1, std::min(sh.segments, 64));
		bool across = sh.dir == DIR_LEFT || sh.dir == DIR_RIGHT;
		float len = across ? w : h, wide = across ? h : w;
		for(int k = 0; k < n; k++){
			float a = len * k / n, b = len * (k + 1) / n;
			float span = wide * (k + 1) / n;
			// k counts from the point.
			float from = sh.dir == DIR_UP || sh.dir == DIR_LEFT ? a : len - b;
			if(across)
				box(x + from, y + (h - span) * 0.5f, b - a, span, sh.colour);
			else
				box(x + (w - span) * 0.5f, y + from, span, b - a, sh.colour);
		}
		break;
	}
	case SHAPE_GRADIENT: {
		// Bands from the colour to the end colour, the way it runs.
		int n = std::max(2, std::min(sh.segments, 128));
		bool across = sh.dir == DIR_LEFT || sh.dir == DIR_RIGHT;
		bool reversed = sh.dir == DIR_LEFT || sh.dir == DIR_UP;
		float len = across ? w : h;
		for(int k = 0; k < n; k++){
			float a = len * k / n, b = len * (k + 1) / n;
			unsigned int c = mixColour(sh.colour, sh.back, (float)(reversed ? n - 1 - k : k) / (n - 1));
			if(across) box(x + a, y, b - a, h, c);
			else box(x, y + a, w, b - a, c);
		}
		break;
	}
	case SHAPE_BAR: {
		// The empty part, then the filled one from where it fills from.
		float v = std::max(0, std::min(sh.value, 100)) / 100.0f;
		bool across = sh.dir == DIR_LEFT || sh.dir == DIR_RIGHT;
		bool reversed = sh.dir == DIR_LEFT || sh.dir == DIR_UP;
		float len = across ? w : h;
		auto piece = [&](float a, float b, unsigned int c){	// along the bar, from where it fills from
			if(reversed){ float f = len - b; b = len - a; a = f; }
			if(across) box(x + a, y, b - a, h, c);
			else box(x, y + a, w, b - a, c);
		};
		int n = std::max(1, std::min(sh.segments, 100));
		if(n == 1){
			piece(0.0f, len, sh.back);
			piece(0.0f, len * v, sh.colour);
		}else{
			float gap = std::min(t, len / n * 0.9f), seg = (len - gap * (n - 1)) / n;
			int lit = (int)(n * v + 0.5f);
			for(int k = 0; k < n; k++)
				piece(k * (seg + gap), k * (seg + gap) + seg, k < lit ? sh.colour : sh.back);
		}
		break;
	}
	case SHAPE_RING: {
		// Dots round an ellipse from the top, clockwise; the first value% lit.
		int n = std::max(3, std::min(sh.segments, 120));
		float d = std::max(1.0f, sh.thickness);
		float rx = (w - d) * 0.5f, ry = (h - d) * 0.5f;
		int lit = (int)(n * std::max(0, std::min(sh.value, 100)) / 100.0f + 0.5f);
		for(int k = 0; k < n; k++){
			float a = (float)k / n * 2.0f * 3.14159265f;
			float cx = x + w * 0.5f + sinf(a) * rx, cy = y + h * 0.5f - cosf(a) * ry;
			ellipse(cx - d * 0.5f, cy - d * 0.5f, d, d, k < lit ? sh.colour : sh.back);
		}
		break;
	}
	}
	if(sh.shadow != 0.0f && !parts.empty()){
		// The same sprites again, under it, offset and in the shadow's colour.
		size_t n = parts.size();
		parts.insert(parts.begin(), parts.begin(), parts.begin() + n);
		for(size_t k = 0; k < n; k++){
			parts[k].x += sh.shadow;
			parts[k].y += sh.shadow;
			parts[k].colour = sh.shadowColour;
		}
	}
}

// The group's shape settings, at the place the group is now; false when it isn't one.
static bool
groupShape(const std::string &group, ShapeSettings *sh)
{
	std::vector<TextDraw> &tds = GetTextDraws();
	for(int i = 0; i < (int)tds.size(); i++)
		if(onBoard(tds[i]) && tds[i].group == group){
			if(tds[i].shape.empty() || !parseShapeSpec(tds[i].shape, sh))
				return false;
			std::vector<TextDraw> parts;
			buildShape(*sh, parts);
			if(!parts.empty()){
				sh->x += tds[i].x - parts[0].x;
				sh->y += tds[i].y - parts[0].y;
			}
			return true;
		}
	return false;
}

// The shape the selection is, when it is all of one shape's group.
static bool
selectedShape(ShapeSettings *sh, std::string *group)
{
	std::vector<TextDraw> &tds = GetTextDraws();
	if(gSelected < 0 || gSelected >= (int)tds.size())
		return false;
	const TextDraw &key = tds[gSelected];
	if(key.shape.empty() || key.group.empty())
		return false;
	int count = 0;
	for(int i = 0; i < (int)tds.size(); i++)
		if(onBoard(tds[i]) && tds[i].group == key.group){
			if(!isSelected(i))
				return false;
			count++;
		}
	if(count != (int)gSel.size() || !groupShape(key.group, sh))
		return false;
	if(group)
		*group = key.group;
	return true;
}

// The group's sprites made again from the settings, where the group was in
// the stack; they stay selected.
static void
replaceShape(const std::string &group, const ShapeSettings &sh)
{
	std::vector<TextDraw> &tds = GetTextDraws();
	std::vector<TextDraw> parts;
	buildShape(sh, parts);
	int first = -1;
	TextDraw like;
	for(int i = (int)tds.size() - 1; i >= 0; i--)
		if(onBoard(tds[i]) && tds[i].group == group){
			first = i;
			like = tds[i];
			tds.erase(tds.begin() + i);
		}
	if(first < 0)
		return;
	std::string spec = shapeSpec(sh);
	gSel.clear();
	for(size_t k = 0; k < parts.size(); k++){
		TextDraw td = parts[k];
		td.group = group;
		td.board = like.board;
		td.locked = like.locked;
		td.hidden = like.hidden;
		td.perPlayer = like.perPlayer;
		td.shape = spec;
		tds.insert(tds.begin() + first + k, td);
		gSel.push_back(first + (int)k);
	}
	gSelected = gSel.empty() ? -1 : gSel.front();
	TextDrawsChanged();
}

// ---- Artboards ----

// The boards in order: the file's, then those made but still empty.
static void
listBoards(std::vector<std::string> &out)
{
	TextDrawBoards(GetTextDraws(), out);
	if(out.empty())
		out.push_back(std::string());
	for(const std::string &b : gEmptyBoards)
		if(std::find(out.begin(), out.end(), b) == out.end())
			out.push_back(b);
	if(std::find(out.begin(), out.end(), gBoard) == out.end())
		out.push_back(gBoard);
}

static const char*
boardLabel(const std::string &board)
{
	return board.empty() ? _("Main") : board.c_str();
}

static void
switchBoard(const std::string &board)
{
	gBoard = board;
	gLayerSelection = false;
	gIsolatedGroup.clear();
	selectOnly(-1);
}

static void
renameBoard(const std::string &from, const std::string &to)
{
	if(from == to || to.empty())
		return;
	for(TextDraw &td : GetTextDraws())
		if(td.board == from)
			td.board = to;
	std::replace(gEmptyBoards.begin(), gEmptyBoards.end(), from, to);
	if(gBoard == from)
		gBoard = to;
	TextDrawsChanged();
}

static std::string
newBoardName(const char *base)
{
	std::vector<std::string> boards;
	listBoards(boards);
	for(int n = 1; ; n++){
		std::string name = std::string(base) + (n > 1 ? " " + std::to_string(n) : "");
		if(std::find(boards.begin(), boards.end(), name) == boards.end())
			return name;
	}
}

static void
duplicateBoard(const std::string &board)
{
	std::vector<TextDraw> &tds = GetTextDraws();
	std::string copy = newBoardName((std::string(boardLabel(board)) + " " + _("copy")).c_str());
	size_t n = tds.size();
	for(size_t i = 0; i < n; i++)
		if(tds[i].board == board){
			TextDraw td = tds[i];
			td.name[0] = '\0';
			td.board = copy;
			tds.push_back(td);
		}
	gEmptyBoards.push_back(copy);
	switchBoard(copy);
	TextDrawsChanged();
}

static void
deleteBoard(const std::string &board)
{
	std::vector<TextDraw> &tds = GetTextDraws();
	tds.erase(std::remove_if(tds.begin(), tds.end(), [&](const TextDraw &td){ return td.board == board; }), tds.end());
	gEmptyBoards.erase(std::remove(gEmptyBoards.begin(), gEmptyBoards.end(), board), gEmptyBoards.end());
	std::vector<std::string> boards;
	listBoards(boards);
	boards.erase(std::remove(boards.begin(), boards.end(), board), boards.end());
	switchBoard(boards.empty() ? std::string() : boards.front());
	TextDrawsChanged();
}

// ---- The canvas ----

// Dragging the selection: where it started, and each one as it was.
static bool gDragging, gDragMoved;
static ImVec2 gDragStart;
static std::vector<std::pair<int, TextDraw>> gDragFrom;
// The box drawn on empty canvas to select what it touches.
static bool gMarquee, gMarqueeAdd;
static ImVec2 gMarqueeStart;
static bool gPanning;

static void
startDrag(void)
{
	std::vector<TextDraw> &tds = GetTextDraws();
	gDragging = true;
	gDragMoved = false;
	gDragStart = ImGui::GetIO().MousePos;
	gDragFrom.clear();
	for(int i : gSel)
		if(!tds[i].locked)
			gDragFrom.push_back(std::make_pair(i, tds[i]));
}

// The textdraws under a point, the top one first.
static void
textDrawsAt(const Canvas &cv, ImVec2 p, std::vector<int> &out)
{
	std::vector<TextDraw> &tds = GetTextDraws();
	out.clear();
	for(int i = (int)tds.size() - 1; i >= 0; i--){
		if(!shown(tds[i]))
			continue;
		float x0, y0, x1, y1;
		boundsOf(tds[i], &x0, &y0, &x1, &y1);
		ImVec2 a = cv.at(x0, y0), b = cv.at(x1, y1);
		b.x = std::max(b.x, a.x + 6.0f);
		b.y = std::max(b.y, a.y + 6.0f);
		if(p.x >= a.x && p.x <= b.x && p.y >= a.y && p.y <= b.y)
			out.push_back(i);
	}
}

// Prefer the layer the user picked in the list when several hit boxes overlap.
// Locked layers still click through when that canvas option is on.
static int
layerHit(const std::vector<int> &under)
{
	std::vector<TextDraw> &tds = GetTextDraws();
	if(!gLayerSelection)
		return -1;
	auto canPick = [&](int i){ return !tds[i].locked || !gClickThroughLocked; };
	if(gSelected >= 0 && std::find(under.begin(), under.end(), gSelected) != under.end() && canPick(gSelected))
		return gSelected;
	for(int i : under)
		if(isSelected(i) && canPick(i))
			return i;
	return -1;
}

static int
topCanvasHit(const std::vector<int> &under)
{
	std::vector<TextDraw> &tds = GetTextDraws();
	for(int i : under)
		if(!tds[i].locked || !gClickThroughLocked)
			return i;
	return -1;
}

// Snaps the moved selection's edges and centre to the screen's, the grid's
// and the other textdraws'. Returns the correction; guides are drawn.
static float
snapAxis(const float edges[3], const std::vector<float> &targets, float reach, float *guide)
{
	float best = reach, fix = 0.0f;
	for(float t : targets)
		for(int e = 0; e < 3; e++)
			if(fabsf(t - edges[e]) < best){
				best = fabsf(t - edges[e]);
				fix = t - edges[e];
				*guide = t;
			}
	return fix;
}

// ---- Resizing, rulers and guides ----

static int gGuideDrag = -1;	// the guide being dragged, in GetTextDrawGuides()

// Where moved and resized edges snap to: the screen's edges and centre,
// the textdraws that aren't selected, the grid and the board's guides.
static void
snapTargets(std::vector<float> &xs, std::vector<float> &ys)
{
	std::vector<TextDraw> &tds = GetTextDraws();
	xs = { 0.0f, 320.0f, 640.0f };
	ys = { 0.0f, 224.0f, 448.0f };
	for(int i = 0; i < (int)tds.size(); i++){
		if(isSelected(i) || !shown(tds[i]))
			continue;
		float x0, y0, x1, y1;
		boundsOf(tds[i], &x0, &y0, &x1, &y1);
		xs.push_back(x0); xs.push_back((x0 + x1) * 0.5f); xs.push_back(x1);
		ys.push_back(y0); ys.push_back((y0 + y1) * 0.5f); ys.push_back(y1);
	}
	if(gGrid)
		for(float g = 0.0f; g <= 640.0f; g += gGridSize){
			xs.push_back(g);
			if(g <= 448.0f) ys.push_back(g);
		}
	if(gRulers)
		for(const TextDrawGuide &g : GetTextDrawGuides())
			if(g.board == gBoard)
				(g.vertical ? xs : ys).push_back(g.at);
}

static void
drawSnapGuides(ImDrawList *dl, const Canvas &cv, float gx, float gy)
{
	ImU32 guide = IM_COL32(255, 70, 200, 220);
	if(gx > -1e8f)
		dl->AddLine(cv.at(gx, 0.0f), cv.at(gx, 448.0f), guide);
	if(gy > -1e8f)
		dl->AddLine(cv.at(0.0f, gy), cv.at(640.0f, gy), guide);
}

// The textdraw as it was in box o, fitted to box n (x0 y0 x1 y1). Sprites
// and models mirror when the box is turned inside out; text and images don't.
static TextDraw
scaledTextDraw(const TextDraw &from, const float o[4], const float n[4])
{
	TextDraw td = from;
	float sx = (n[2] - n[0]) / std::max(0.01f, o[2] - o[0]);
	float sy = (n[3] - n[1]) / std::max(0.01f, o[3] - o[1]);
	if(td.font >= TD_FONT_SPRITE){
		td.x = n[0] + (from.x - o[0]) * sx;
		td.y = n[1] + (from.y - o[1]) * sy;
		td.sizeX = from.sizeX * sx;
		td.sizeY = from.sizeY * sy;
		if(td.font == TD_FONT_REFERENCE){
			if(td.sizeX < 0.0f){ td.x += td.sizeX; td.sizeX = -td.sizeX; }
			if(td.sizeY < 0.0f){ td.y += td.sizeY; td.sizeY = -td.sizeY; }
		}
		return td;
	}
	// Text keeps reading the right way, in the box made the right way round;
	// a box turned upside down grows up from its place.
	float ax = fabsf(sx), ay = fabsf(sy);
	float nx0 = std::min(n[0], n[2]), ny0 = std::min(n[1], n[3]);
	td.x = nx0 + (from.x - o[0]) * ax;
	td.letterX = std::max(0.01f, from.letterX * ax);
	if(td.alignment == 2)
		td.sizeY = from.sizeY * ax;	// a centred one's width
	else
		td.sizeX = nx0 + (from.sizeX - o[0]) * ax;	// its box's other edge, or where it wraps
	if(td.useBox){
		td.y = n[1] + (from.y - o[1]) * sy;
		td.letterY = from.letterY * sy;
	}else{
		td.y = ny0 + (from.y - o[1]) * ay;
		td.letterY = from.letterY * ay;
	}
	return td;
}

// The selection as it was when a resize started.
static float gBoxFrom[4];
static bool gScaleShape;
static ShapeSettings gShapeFrom;
static std::string gShapeGroup;
static std::vector<std::pair<int, TextDraw>> gScaleFrom;
static int gHandle = -1;	// the handle being dragged: 0 top left, clockwise to 7 left
static ImVec2 gHandleStart;

// The box the handles go round: a shape's own, or what the unlocked
// selected ones cover. False when nothing in it can be resized.
static bool
selectionScaleBox(float b[4])
{
	std::vector<TextDraw> &tds = GetTextDraws();
	ShapeSettings sh;
	if(selectedShape(&sh, nil)){
		if(tds[gSelected].locked)
			return false;
		b[0] = sh.x; b[1] = sh.y; b[2] = sh.x + sh.w; b[3] = sh.y + sh.h;
		return true;
	}
	bool any = false;
	b[0] = b[1] = 1e9f;
	b[2] = b[3] = -1e9f;
	for(int i : gSel){
		if(tds[i].locked || !shown(tds[i]))
			continue;
		float x0, y0, x1, y1;
		boundsOf(tds[i], &x0, &y0, &x1, &y1);
		b[0] = std::min(b[0], x0); b[1] = std::min(b[1], y0);
		b[2] = std::max(b[2], x1); b[3] = std::max(b[3], y1);
		any = true;
	}
	return any;
}

// Takes the selection's box and what is in it, for a resize.
static bool
beginScale(void)
{
	std::vector<TextDraw> &tds = GetTextDraws();
	if(!selectionScaleBox(gBoxFrom))
		return false;
	gScaleFrom.clear();
	gScaleShape = selectedShape(&gShapeFrom, &gShapeGroup);
	if(!gScaleShape)
		for(int i : gSel)
			if(!tds[i].locked && shown(tds[i]))
				gScaleFrom.push_back(std::make_pair(i, tds[i]));
	return true;
}

// Fits what beginScale took to box n.
static void
scaleSelection(const float n[4])
{
	if(gScaleShape){
		ShapeSettings sh = gShapeFrom;
		sh.x = std::min(n[0], n[2]);
		sh.y = std::min(n[1], n[3]);
		sh.w = std::max(1.0f, fabsf(n[2] - n[0]));
		sh.h = std::max(1.0f, fabsf(n[3] - n[1]));
		replaceShape(gShapeGroup, sh);
		return;
	}
	std::vector<TextDraw> &tds = GetTextDraws();
	for(const auto &f : gScaleFrom)
		tds[f.first] = scaledTextDraw(f.second, gBoxFrom, n);
	TextDrawsChanged();
}

static void
handlePos(int h, const float b[4], float *x, float *y)
{
	static const float fx[8] = { 0.0f, 0.5f, 1.0f, 1.0f, 1.0f, 0.5f, 0.0f, 0.0f };
	static const float fy[8] = { 0.0f, 0.0f, 0.0f, 0.5f, 1.0f, 1.0f, 1.0f, 0.5f };
	*x = b[0] + (b[2] - b[0]) * fx[h];
	*y = b[1] + (b[3] - b[1]) * fy[h];
}

static ImGuiMouseCursor
handleCursor(int h)
{
	static const ImGuiMouseCursor cursors[8] = { ImGuiMouseCursor_ResizeNWSE, ImGuiMouseCursor_ResizeNS,
		ImGuiMouseCursor_ResizeNESW, ImGuiMouseCursor_ResizeEW, ImGuiMouseCursor_ResizeNWSE,
		ImGuiMouseCursor_ResizeNS, ImGuiMouseCursor_ResizeNESW, ImGuiMouseCursor_ResizeEW };
	return cursors[h & 7];
}

// Dragging a handle: its edges follow the mouse and snap; Shift keeps the
// proportions, Alt sizes from the centre, Ctrl doesn't snap.
static void
resizeSelection(const Canvas &cv, ImDrawList *dl)
{
	ImGuiIO &io = ImGui::GetIO();
	const float *o = gBoxFrom;
	float dx = (io.MousePos.x - gHandleStart.x) / cv.sx, dy = (io.MousePos.y - gHandleStart.y) / cv.sy;
	int h = gHandle;
	bool left = h == 0 || h == 6 || h == 7, right = h >= 2 && h <= 4;
	bool top = h <= 2, bottom = h >= 4 && h <= 6;
	float n[4] = { o[0], o[1], o[2], o[3] };
	if(left) n[0] += dx;
	if(right) n[2] += dx;
	if(top) n[1] += dy;
	if(bottom) n[3] += dy;
	if(gSnap && !io.KeyCtrl){
		std::vector<float> xs, ys;
		snapTargets(xs, ys);
		float gx = -1e9f, gy = -1e9f;
		int ex = left ? 0 : right ? 2 : -1, ey = top ? 1 : bottom ? 3 : -1;
		if(ex >= 0){
			float e[3] = { n[ex], n[ex], n[ex] };
			n[ex] += snapAxis(e, xs, 6.0f / cv.sx, &gx);
		}
		if(ey >= 0){
			float e[3] = { n[ey], n[ey], n[ey] };
			n[ey] += snapAxis(e, ys, 6.0f / cv.sy, &gy);
		}
		drawSnapGuides(dl, cv, gx, gy);
	}
	if(io.KeyAlt){
		if(left) n[2] = o[2] - (n[0] - o[0]);
		if(right) n[0] = o[0] - (n[2] - o[2]);
		if(top) n[3] = o[3] - (n[1] - o[1]);
		if(bottom) n[1] = o[1] - (n[3] - o[3]);
	}
	if(io.KeyShift){
		float sx = (n[2] - n[0]) / std::max(0.01f, o[2] - o[0]);
		float sy = (n[3] - n[1]) / std::max(0.01f, o[3] - o[1]);
		bool across = left || right, down = top || bottom;
		float s = across && down ? std::max(fabsf(sx), fabsf(sy)) : across ? fabsf(sx) : fabsf(sy);
		float gxs = across && sx < 0.0f ? -1.0f : 1.0f, gys = down && sy < 0.0f ? -1.0f : 1.0f;
		// From the opposite edge, or the centre (with Alt, and across a side handle).
		float cx = (o[0] + o[2]) * 0.5f, cy = (o[1] + o[3]) * 0.5f;
		float ax = io.KeyAlt || !across ? cx : left ? o[2] : o[0];
		float ay = io.KeyAlt || !down ? cy : top ? o[3] : o[1];
		n[0] = ax + (o[0] - ax) * s * gxs; n[2] = ax + (o[2] - ax) * s * gxs;
		n[1] = ay + (o[1] - ay) * s * gys; n[3] = ay + (o[3] - ay) * s * gys;
	}
	scaleSelection(n);
	ImGui::SetTooltip("%.1f x %.1f", fabsf(n[2] - n[0]), fabsf(n[3] - n[1]));
}

static float
rulerSize(void)
{
	return gRulers ? ImGui::GetFontSize() * 1.3f : 0.0f;
}

static bool
rulerTickRange(float firstPx, float lastPx, float origin, float scale, float minor, int *first, int *last)
{
	if(!std::isfinite(firstPx) || !std::isfinite(lastPx) || !std::isfinite(origin) ||
	   !std::isfinite(scale) || scale <= 0.0f || !std::isfinite(minor) || minor <= 0.0f || lastPx < firstPx)
		return false;
	double spacing = (double)scale * minor;
	double lo = std::ceil(((double)firstPx - origin) / spacing);
	double hi = std::floor(((double)lastPx - origin) / spacing);
	if(!std::isfinite(lo) || !std::isfinite(hi) || hi < lo || lo < INT_MIN || lo > INT_MAX ||
	   hi < INT_MIN || hi > INT_MAX || hi - lo >= 4096.0)
		return false;
	*first = (int)lo;
	*last = (int)hi;
	return true;
}

// The rulers along the top and the left, in canvas units: the selection's
// extent and the mouse are marked on them, and so are the guides.
static void
drawRulers(ImDrawList *dl, const Canvas &cv, ImVec2 a, ImVec2 b, const float *sel)
{
	float rs = rulerSize();
	ImU32 bg = IM_COL32(30, 32, 38, 245), tick = IM_COL32(150, 155, 165, 255);
	ImU32 text = IM_COL32(205, 210, 220, 255), accent = IM_COL32(255, 220, 60, 160);
	ImU32 guide = IM_COL32(0, 200, 255, 255);
	dl->AddRectFilled(a, ImVec2(b.x, a.y + rs), bg);
	dl->AddRectFilled(ImVec2(a.x, a.y + rs), ImVec2(a.x + rs, b.y), bg);
	dl->AddLine(ImVec2(a.x + rs, a.y + rs), ImVec2(b.x, a.y + rs), tick);
	dl->AddLine(ImVec2(a.x + rs, a.y + rs), ImVec2(a.x + rs, b.y), tick);
	// A minimized or unusually narrow viewport can leave the fitted canvas
	// with a zero or negative scale. Ruler tick loops require positive scales.
	if(!std::isfinite(cv.sx) || cv.sx <= 0.0f || !std::isfinite(cv.sy) || cv.sy <= 0.0f)
		return;
	// Numbers at least 50 pixels apart, four small ticks between.
	static const float steps[] = { 1.0f, 2.0f, 5.0f, 10.0f, 20.0f, 25.0f, 50.0f, 100.0f, 200.0f };
	float step = 500.0f;
	for(float s : steps)
		if(s * cv.sx >= 50.0f){
			step = s;
			break;
		}
	float minor = step / 5.0f, fs = ImGui::GetFontSize() * 0.72f;
	char num[16];
	if(sel){
		dl->AddRectFilled(ImVec2(cv.at(sel[0], 0).x, a.y + rs - 4), ImVec2(cv.at(sel[2], 0).x, a.y + rs), accent);
		dl->AddRectFilled(ImVec2(a.x + rs - 4, cv.at(0, sel[1]).y), ImVec2(a.x + rs, cv.at(0, sel[3]).y), accent);
	}
	dl->PushClipRect(ImVec2(a.x + rs, a.y), ImVec2(b.x, a.y + rs), true);
	int first, last;
	if(rulerTickRange(a.x + rs, b.x, cv.x, cv.sx, minor, &first, &last)){
		for(int k = first; ; k++){
			float u = k * minor, px = cv.x + u * cv.sx;
			bool major = k % 5 == 0;
			dl->AddLine(ImVec2(px, a.y + rs * (major ? 0.25f : 0.65f)), ImVec2(px, a.y + rs), tick);
			if(major){
				snprintf(num, sizeof(num), "%g", u);
				dl->AddText(ImGui::GetFont(), fs, ImVec2(px + 2.0f, a.y), text, num);
			}
			if(k == last)
				break;
		}
	}
	dl->PopClipRect();
	dl->PushClipRect(ImVec2(a.x, a.y + rs), ImVec2(a.x + rs, b.y), true);
	if(rulerTickRange(a.y + rs, b.y, cv.y, cv.sy, minor, &first, &last)){
		for(int k = first; ; k++){
			float u = k * minor, py = cv.y + u * cv.sy;
			bool major = k % 5 == 0;
			dl->AddLine(ImVec2(a.x + rs * (major ? 0.25f : 0.65f), py), ImVec2(a.x + rs, py), tick);
			if(major){
				snprintf(num, sizeof(num), "%g", u);
				dl->AddText(ImGui::GetFont(), fs, ImVec2(a.x + 1.0f, py + 1.0f), text, num);
			}
			if(k == last)
				break;
		}
	}
	dl->PopClipRect();
	for(const TextDrawGuide &g : GetTextDrawGuides()){
		if(g.board != gBoard)
			continue;
		if(g.vertical){
			float px = cv.at(g.at, 0).x;
			if(px > a.x + rs)
				dl->AddTriangleFilled(ImVec2(px - 4, a.y + rs - 6), ImVec2(px + 4, a.y + rs - 6), ImVec2(px, a.y + rs), guide);
		}else{
			float py = cv.at(0, g.at).y;
			if(py > a.y + rs)
				dl->AddTriangleFilled(ImVec2(a.x + rs - 6, py - 4), ImVec2(a.x + rs - 6, py + 4), ImVec2(a.x + rs, py), guide);
		}
	}
	ImVec2 m = ImGui::GetIO().MousePos;
	if(m.x > a.x + rs && m.x < b.x && m.y > a.y && m.y < b.y){
		dl->AddLine(ImVec2(m.x, a.y), ImVec2(m.x, a.y + rs), IM_COL32(255, 255, 255, 200));
		dl->AddLine(ImVec2(a.x, m.y), ImVec2(a.x + rs, m.y), IM_COL32(255, 255, 255, 200));
	}
	dl->AddRectFilled(a, ImVec2(a.x + rs, a.y + rs), bg);
}

// Alt held: how far the selection is from what the mouse is over, or from
// the screen's edges.
static void
drawDistances(ImDrawList *dl, const Canvas &cv, const float A[4], const float B[4])
{
	ImU32 c = IM_COL32(255, 80, 80, 255);
	auto label = [&](ImVec2 p, float v){
		char t[16];
		snprintf(t, sizeof(t), "%.1f", v);
		ImVec2 ts = ImGui::CalcTextSize(t);
		ImVec2 q(p.x - ts.x * 0.5f, p.y - ts.y * 0.5f);
		dl->AddRectFilled(ImVec2(q.x - 3, q.y - 1), ImVec2(q.x + ts.x + 3, q.y + ts.y + 1), c, 3.0f);
		dl->AddText(q, IM_COL32(255, 255, 255, 255), t);
	};
	auto across = [&](float x0, float x1, float y){
		if(x1 - x0 < 0.05f)
			return;
		ImVec2 p = cv.at(x0, y), q = cv.at(x1, y);
		dl->AddLine(p, q, c, 1.5f);
		dl->AddLine(ImVec2(p.x, p.y - 4), ImVec2(p.x, p.y + 4), c);
		dl->AddLine(ImVec2(q.x, q.y - 4), ImVec2(q.x, q.y + 4), c);
		label(ImVec2((p.x + q.x) * 0.5f, p.y), x1 - x0);
	};
	auto down = [&](float y0, float y1, float x){
		if(y1 - y0 < 0.05f)
			return;
		ImVec2 p = cv.at(x, y0), q = cv.at(x, y1);
		dl->AddLine(p, q, c, 1.5f);
		dl->AddLine(ImVec2(p.x - 4, p.y), ImVec2(p.x + 4, p.y), c);
		dl->AddLine(ImVec2(q.x - 4, q.y), ImVec2(q.x + 4, q.y), c);
		label(ImVec2(p.x, (p.y + q.y) * 0.5f), y1 - y0);
	};
	dl->AddRect(cv.at(B[0], B[1]), cv.at(B[2], B[3]), c);
	auto inside = [](const float *p, const float *q){	// p inside q
		return p[0] >= q[0] && p[1] >= q[1] && p[2] <= q[2] && p[3] <= q[3];
	};
	const float *in = inside(A, B) ? A : inside(B, A) ? B : nil;
	if(in){
		const float *out = in == A ? B : A;
		float mx = (in[0] + in[2]) * 0.5f, my = (in[1] + in[3]) * 0.5f;
		across(out[0], in[0], my);
		across(in[2], out[2], my);
		down(out[1], in[1], mx);
		down(in[3], out[3], mx);
		return;
	}
	float my = std::max(A[1], B[1]) < std::min(A[3], B[3]) ? (std::max(A[1], B[1]) + std::min(A[3], B[3])) * 0.5f :
		(A[1] + A[3]) * 0.5f;
	float mx = std::max(A[0], B[0]) < std::min(A[2], B[2]) ? (std::max(A[0], B[0]) + std::min(A[2], B[2])) * 0.5f :
		(A[0] + A[2]) * 0.5f;
	if(B[0] >= A[2]) across(A[2], B[0], my);
	else if(A[0] >= B[2]) across(B[2], A[0], my);
	if(B[1] >= A[3]) down(A[3], B[1], mx);
	else if(A[1] >= B[3]) down(B[3], A[1], mx);
}

// ---- Arranging ----

// What align, spacing, sizes and flips work on: a selected group as one
// (a shape by its own box), or a textdraw on its own.
struct ArrangeUnit {
	std::string group;
	std::vector<int> members;
	float box[4];
	bool shape;
	ShapeSettings sh;
};

static void
selectionUnits(std::vector<ArrangeUnit> &out)
{
	std::vector<TextDraw> &tds = GetTextDraws();
	out.clear();
	for(int i : gSel){
		if(!shown(tds[i]))
			continue;
		ArrangeUnit *u = nil;
		if(!tds[i].group.empty())
			for(ArrangeUnit &v : out)
				if(v.group == tds[i].group)
					u = &v;
		if(u == nil){
			out.push_back(ArrangeUnit());
			u = &out.back();
			u->group = tds[i].group;
			u->box[0] = u->box[1] = 1e9f;
			u->box[2] = u->box[3] = -1e9f;
			u->shape = false;
		}
		u->members.push_back(i);
		float x0, y0, x1, y1;
		boundsOf(tds[i], &x0, &y0, &x1, &y1);
		u->box[0] = std::min(u->box[0], x0); u->box[1] = std::min(u->box[1], y0);
		u->box[2] = std::max(u->box[2], x1); u->box[3] = std::max(u->box[3], y1);
	}
	for(ArrangeUnit &u : out){
		if(u.group.empty() || !groupShape(u.group, &u.sh))
			continue;
		int count = 0;
		for(const TextDraw &td : tds)
			count += onBoard(td) && td.group == u.group;
		if(count != (int)u.members.size())
			continue;	// only part of the shape is selected
		u.shape = true;
		u.box[0] = u.sh.x; u.box[1] = u.sh.y;
		u.box[2] = u.sh.x + u.sh.w; u.box[3] = u.sh.y + u.sh.h;
	}
}

static int
keyUnit(const std::vector<ArrangeUnit> &units)
{
	for(size_t k = 0; k < units.size(); k++)
		for(int i : units[k].members)
			if(i == gSelected)
				return (int)k;
	return -1;
}

static void
moveUnit(const ArrangeUnit &u, float dx, float dy)
{
	std::vector<TextDraw> &tds = GetTextDraws();
	for(int i : u.members)
		if(!tds[i].locked)
			moveBy(tds[i], dx, dy);
}

static bool
unitLocked(const ArrangeUnit &u)
{
	for(int i : u.members)
		if(GetTextDraws()[i].locked)
			return true;
	return false;
}

// Puts the unit in box n: a shape is made again, the rest are scaled.
// Shapes can change how many sprites they have, so this goes from the
// last unit to the first and the selection is made again after.
static void
fitUnit(const ArrangeUnit &u, const float n[4], int flip)
{
	std::vector<TextDraw> &tds = GetTextDraws();
	if(u.shape){
		ShapeSettings sh = u.sh;
		sh.x = std::min(n[0], n[2]); sh.y = std::min(n[1], n[3]);
		sh.w = std::max(1.0f, fabsf(n[2] - n[0])); sh.h = std::max(1.0f, fabsf(n[3] - n[1]));
		if(flip == 1 && (sh.dir == DIR_LEFT || sh.dir == DIR_RIGHT)) sh.dir ^= 1;
		if(flip == 2 && (sh.dir == DIR_UP || sh.dir == DIR_DOWN)) sh.dir ^= 1;
		replaceShape(u.group, sh);
		return;
	}
	for(int i : u.members)
		if(!tds[i].locked)
			tds[i] = scaledTextDraw(tds[i], u.box, n);
}

static void
reselectUnits(const std::vector<ArrangeUnit> &units, const std::vector<int> &plain, int key)
{
	std::vector<TextDraw> &tds = GetTextDraws();
	selectOnly(-1);
	for(int i : plain)
		if(i >= 0 && i < (int)tds.size())
			addToSelection(i);
	for(const ArrangeUnit &u : units)
		if(!u.group.empty())
			for(int i = 0; i < (int)tds.size(); i++)
				if(onBoard(tds[i]) && tds[i].group == u.group)
					addToSelection(i);
	if(key >= 0 && key < (int)tds.size() && isSelected(key))
		gSelected = key;
}

// Each unit to its new box, last first; what was selected stays so.
static void
fitUnits(std::vector<ArrangeUnit> &units, const std::vector<std::vector<float>> &boxes, int flip)
{
	std::vector<TextDraw> &tds = GetTextDraws();
	std::vector<size_t> order(units.size());
	for(size_t k = 0; k < order.size(); k++)
		order[k] = k;
	std::sort(order.begin(), order.end(), [&](size_t a, size_t b){ return units[a].members[0] > units[b].members[0]; });
	std::vector<int> plain;
	for(const ArrangeUnit &u : units)
		if(u.group.empty())
			plain.push_back(u.members[0]);
	int key = gSelected;
	for(size_t k : order){
		ArrangeUnit &u = units[k];
		if(boxes[k].empty() || unitLocked(u))
			continue;
		size_t before = tds.size();
		fitUnit(u, boxes[k].data(), flip);
		int grew = (int)tds.size() - (int)before;
		if(grew != 0){
			for(int &i : plain)
				if(i > u.members[0]) i += grew;
			if(key > u.members[0]) key += grew;
		}
	}
	reselectUnits(units, plain, key);
	TextDrawsChanged();
}

enum { ALIGN_LEFT, ALIGN_HCENTRE, ALIGN_RIGHT, ALIGN_TOP, ALIGN_VCENTRE, ALIGN_BOTTOM };
enum { ALIGN_TO_SELECTION, ALIGN_TO_KEY, ALIGN_TO_SCREEN };
static int gAlignTo = ALIGN_TO_SELECTION;

// To the selection's box, to the key one (the one last clicked, which
// stays) or to the screen. One alone goes to the screen.
static void
alignSelected(int how)
{
	std::vector<ArrangeUnit> units;
	selectionUnits(units);
	if(units.empty())
		return;
	int key = gAlignTo == ALIGN_TO_KEY ? keyUnit(units) : -1;
	float T[4] = { 1e9f, 1e9f, -1e9f, -1e9f };
	if(gAlignTo == ALIGN_TO_SCREEN || units.size() == 1){
		T[0] = 0.0f; T[1] = 0.0f; T[2] = 640.0f; T[3] = 448.0f;
	}else if(key >= 0)
		memcpy(T, units[key].box, sizeof(T));
	else
		for(const ArrangeUnit &u : units){
			T[0] = std::min(T[0], u.box[0]); T[1] = std::min(T[1], u.box[1]);
			T[2] = std::max(T[2], u.box[2]); T[3] = std::max(T[3], u.box[3]);
		}
	for(int k = 0; k < (int)units.size(); k++){
		if(k == key && units.size() > 1)
			continue;
		const float *b = units[k].box;
		float dx = 0.0f, dy = 0.0f;
		switch(how){
		case ALIGN_LEFT: dx = T[0] - b[0]; break;
		case ALIGN_HCENTRE: dx = (T[0] + T[2]) * 0.5f - (b[0] + b[2]) * 0.5f; break;
		case ALIGN_RIGHT: dx = T[2] - b[2]; break;
		case ALIGN_TOP: dy = T[1] - b[1]; break;
		case ALIGN_VCENTRE: dy = (T[1] + T[3]) * 0.5f - (b[1] + b[3]) * 0.5f; break;
		case ALIGN_BOTTOM: dy = T[3] - b[3]; break;
		}
		moveUnit(units[k], dx, dy);
	}
	TextDrawsChanged();
}

// Three or more with equal gaps, the outer ones staying; or, with a gap
// given, two or more that far apart from the first.
static void
distributeSelected(bool across, float gap)
{
	std::vector<ArrangeUnit> units;
	selectionUnits(units);
	if(units.size() < (gap >= 0.0f ? 2u : 3u))
		return;
	int a = across ? 0 : 1, b = across ? 2 : 3;
	std::sort(units.begin(), units.end(), [&](const ArrangeUnit &p, const ArrangeUnit &q){ return p.box[a] < q.box[a]; });
	if(gap < 0.0f){
		float total = 0.0f;
		for(const ArrangeUnit &u : units)
			total += u.box[b] - u.box[a];
		gap = (units.back().box[b] - units.front().box[a] - total) / (float)(units.size() - 1);
	}
	float at = units.front().box[a];
	for(const ArrangeUnit &u : units){
		float d = at - u.box[a];
		moveUnit(u, across ? d : 0.0f, across ? 0.0f : d);
		at += u.box[b] - u.box[a] + gap;
	}
	TextDrawsChanged();
}

// The key one's width or height for all, from their left or top edge.
static void
matchSize(bool width)
{
	std::vector<ArrangeUnit> units;
	selectionUnits(units);
	int key = keyUnit(units);
	if(key < 0 || units.size() < 2)
		return;
	const float *K = units[key].box;
	std::vector<std::vector<float>> boxes(units.size());
	for(size_t k = 0; k < units.size(); k++){
		if((int)k == key)
			continue;
		const float *b = units[k].box;
		boxes[k] = { b[0], b[1], width ? b[0] + K[2] - K[0] : b[2], width ? b[3] : b[1] + K[3] - K[1] };
	}
	fitUnits(units, boxes, 0);
}

// Mirrored across the selection's middle: places swap, sprites mirror,
// shapes point the other way; text still reads the right way.
static void
flipSelected(bool across)
{
	std::vector<ArrangeUnit> units;
	selectionUnits(units);
	if(units.empty())
		return;
	float S[4] = { 1e9f, 1e9f, -1e9f, -1e9f };
	for(const ArrangeUnit &u : units){
		S[0] = std::min(S[0], u.box[0]); S[1] = std::min(S[1], u.box[1]);
		S[2] = std::max(S[2], u.box[2]); S[3] = std::max(S[3], u.box[3]);
	}
	std::vector<std::vector<float>> boxes(units.size());
	for(size_t k = 0; k < units.size(); k++){
		const float *b = units[k].box;
		if(across)
			boxes[k] = { S[0] + S[2] - b[0], b[1], S[0] + S[2] - b[2], b[3] };
		else
			boxes[k] = { b[0], S[1] + S[3] - b[1], b[2], S[1] + S[3] - b[3] };
	}
	fitUnits(units, boxes, across ? 1 : 2);
}

static void
dragSelection(const Canvas &cv, ImDrawList *dl)
{
	std::vector<TextDraw> &tds = GetTextDraws();
	ImGuiIO &io = ImGui::GetIO();
	ImVec2 d(io.MousePos.x - gDragStart.x, io.MousePos.y - gDragStart.y);
	if(!gDragMoved && d.x * d.x + d.y * d.y < 9.0f)
		return;
	gDragMoved = true;
	// Shift keeps it to the way it has moved most.
	if(io.KeyShift){
		if(fabsf(d.x) > fabsf(d.y)) d.y = 0.0f;
		else d.x = 0.0f;
	}
	float dx = d.x / cv.sx, dy = d.y / cv.sy;
	if(gSnap && !io.KeyCtrl && !gDragFrom.empty()){
		float X0 = 1e9f, Y0 = 1e9f, X1 = -1e9f, Y1 = -1e9f;
		for(const auto &f : gDragFrom){
			float x0, y0, x1, y1;
			boundsOf(f.second, &x0, &y0, &x1, &y1);
			X0 = std::min(X0, x0); Y0 = std::min(Y0, y0);
			X1 = std::max(X1, x1); Y1 = std::max(Y1, y1);
		}
		std::vector<float> xs, ys;
		snapTargets(xs, ys);
		float ex[3] = { X0 + dx, (X0 + X1) * 0.5f + dx, X1 + dx };
		float ey[3] = { Y0 + dy, (Y0 + Y1) * 0.5f + dy, Y1 + dy };
		float gx = -1e9f, gy = -1e9f;
		if(!io.KeyShift || d.x != 0.0f) dx += snapAxis(ex, xs, 6.0f / cv.sx, &gx);
		if(!io.KeyShift || d.y != 0.0f) dy += snapAxis(ey, ys, 6.0f / cv.sy, &gy);
		drawSnapGuides(dl, cv, gx, gy);
	}
	for(const auto &f : gDragFrom){
		TextDraw td = f.second;
		moveBy(td, dx, dy);
		tds[f.first] = td;
	}
	TextDrawsChanged();
	if(gSelected >= 0)
		ImGui::SetTooltip("%.1f, %.1f", tds[gSelected].x, tds[gSelected].y);
}

static const float MAX_ZOOM = 16.0f;

static void
drawGrid(ImDrawList *dl, const Canvas &cv)
{
	if(!gGrid || gGridSize * cv.sx < 4.0f)
		return;
	ImU32 line = IM_COL32(255, 255, 255, 28);
	for(float x = gGridSize; x < 640.0f; x += gGridSize)
		dl->AddLine(cv.at(x, 0.0f), cv.at(x, 448.0f), line);
	for(float y = gGridSize; y < 448.0f; y += gGridSize)
		dl->AddLine(cv.at(0.0f, y), cv.at(640.0f, y), line);
	dl->AddLine(cv.at(320.0f, 0.0f), cv.at(320.0f, 448.0f), IM_COL32(255, 255, 255, 50));
	dl->AddLine(cv.at(0.0f, 224.0f), cv.at(640.0f, 224.0f), IM_COL32(255, 255, 255, 50));
}

// The screen as a player sees it: background, HUD, the map's textdraws, the board's.
static void
drawScreen(ImDrawList *dl, const Canvas &cv)
{
	if(gBackground == TD_BACKGROUND_STILL && gStillFrame)
		dl->AddImage((ImTextureID)gStillFrame, cv.at(0, 0), cv.at(640, 448));
	else
		dl->AddRectFilled(cv.at(0, 0), cv.at(640, 448), rgbaToImU32(gCanvasColour));
	if(gHud.show)
		drawHud(dl, cv);
	const std::vector<TextDraw> &mapTds = GetMapTextDraws();
	for(size_t i = 0; i < mapTds.size(); i++)
		drawTextDraw(dl, cv, mapTds[i]);
	const std::vector<TextDraw> &tds = GetTextDraws();
	for(size_t i = 0; i < tds.size(); i++)
		if(shown(tds[i]))
			drawTextDraw(dl, cv, tds[i]);
}

// A regular window: comparisons can be moved, resized or docked beside the canvas.
static void
drawCompare(void)
{
	ImVec2 pos, size;
	canvasArea(&pos, &size);
	ImGui::SetNextWindowSize(ImVec2(560 * GetUiScale(), 210 * GetUiScale()), ImGuiCond_FirstUseEver);
	ImGui::SetNextWindowPos(ImVec2(pos.x + 20, pos.y + std::max(0.0f, size.y - 230 * GetUiScale())), ImGuiCond_FirstUseEver);
	bool open = gCompare;
	if(ImGui::Begin(L("Screen previews###td_compare"), &open)){
		static const int shapes[] = { TD_SCREEN_4_3, TD_SCREEN_16_9, TD_SCREEN_21_9 };
		float available = ImGui::GetContentRegionAvail().x;
		int columns = available >= ImGui::GetFontSize() * 30.0f ? 3 : 1;
		float width = (available - ImGui::GetStyle().ItemSpacing.x * (columns - 1)) / columns;
		for(int k = 0; k < IM_ARRAYSIZE(shapes); k++){
			if(k % columns) ImGui::SameLine();
			ImGui::BeginGroup();
			ImGui::TextUnformatted(gScreenNames[shapes[k]]);
			ImVec2 p = ImGui::GetCursorScreenPos();
			float h = width / gScreenAspects[shapes[k]];
			Canvas cv = fitCanvas(p, ImVec2(width, h), gScreenAspects[shapes[k]]);
			ImDrawList *dl = ImGui::GetWindowDrawList();
			dl->PushClipRect(p, ImVec2(p.x + width, p.y + h), true);
			drawScreen(dl, cv);
			dl->PopClipRect();
			ImGui::Dummy(ImVec2(width, h));
			ImGui::EndGroup();
		}
	}
	ImGui::End();
	if(open != gCompare){ gCompare = open; SaveEditorSettings(); }
}

// The canvas: the still frame or a plain colour where the world was, the
// map's read-only textdraws, then the board's, which are edited here.
void
DrawTextDrawsOverlay(void)
{
	if(!gMode)
		return;
	std::vector<TextDraw> &tds = GetTextDraws();
	ImGuiIO &io = ImGui::GetIO();
	loadFonts();
	validateSelection();
	Canvas cv = getCanvas();
	ImVec2 areaPos, areaSize;
	canvasArea(&areaPos, &areaSize);
	ImVec2 areaEnd(areaPos.x + areaSize.x, areaPos.y + areaSize.y);
	ImDrawList *dl = ImGui::GetBackgroundDrawList();
	ImGuiViewport *vp = ImGui::GetMainViewport();
	dl->AddRectFilled(vp->Pos, ImVec2(vp->Pos.x + vp->Size.x, vp->Pos.y + vp->Size.y), IM_COL32(24, 26, 30, 255));
	dl->PushClipRect(areaPos, areaEnd, true);
	if(gPixelExact){
		// Drawn at the player's resolution, shown next frame (see renderExactScreen).
		int r = std::max(0, std::min(gHud.resolution, NUM_HUD_RESOLUTIONS - 1));
		float w = (float)gHudResolutions[r].w, h = (float)gHudResolutions[r].h;
		if(gExactList == nil)
			gExactList = IM_NEW(ImDrawList)(ImGui::GetDrawListSharedData());
		gExactList->_ResetForNewFrame();
		gExactList->PushClipRect(ImVec2(0.0f, 0.0f), ImVec2(w, h));
		gExactList->PushTexture(ImGui::GetIO().Fonts->TexRef);
		Canvas player = { 0.0f, 0.0f, w / 640.0f, h / 448.0f, 0.0f };
		drawScreen(gExactList, player);
		if(gExactReady && gExactTexture)
			dl->AddImage((ImTextureID)gExactTexture, cv.at(0, 0), cv.at(640, 448));
		else
			drawScreen(dl, cv);
	}else
		drawScreen(dl, cv);
	const std::vector<TextDraw> &mapTds = GetMapTextDraws();
	for(size_t i = 0; i < mapTds.size(); i++){
		float x0, y0, x1, y1;
		boundsOf(mapTds[i], &x0, &y0, &x1, &y1);
		dl->AddRect(cv.at(x0, y0), cv.at(x1, y1), IM_COL32(160, 160, 170, 120));
	}
	drawGrid(dl, cv);
	if(gRulers)
		for(const TextDrawGuide &gd : GetTextDrawGuides())
			if(gd.board == gBoard){
				if(gd.vertical) dl->AddLine(ImVec2(cv.at(gd.at, 0).x, areaPos.y), ImVec2(cv.at(gd.at, 0).x, areaEnd.y), IM_COL32(0, 200, 255, 170));
				else dl->AddLine(ImVec2(areaPos.x, cv.at(0, gd.at).y), ImVec2(areaEnd.x, cv.at(0, gd.at).y), IM_COL32(0, 200, 255, 170));
			}
	// The screen's edge, so textdraws can be placed against it.
	dl->AddRect(cv.at(0, 0), cv.at(640, 448), IM_COL32(255, 255, 255, 60));

	// Invisible windows over the textdraws take the clicks and drags; locked
	// ones let them through to what is under them (unless that is turned off).
	ImGuiWindowFlags flags = ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoBackground |
		ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoFocusOnAppearing |
		ImGuiWindowFlags_NoNav | ImGuiWindowFlags_NoBringToFrontOnFocus | ImGuiWindowFlags_NoScrollbar;
#ifdef IMGUI_HAS_DOCK
	flags |= ImGuiWindowFlags_NoDocking;
#endif
	int hovered = -1;
	bool consumedPickClick = false;
	if(gPicking){
		// The eyedropper: the top image layer under the click gives its colour.
		ImGui::SetMouseCursor(ImGuiMouseCursor_Hand);
		ImVec2 m = io.MousePos;
		bool inside = m.x >= areaPos.x && m.y >= areaPos.y && m.x < areaEnd.x && m.y < areaEnd.y;
		float cx = (m.x - cv.x) / cv.sx, cy = (m.y - cv.y) / cv.sy;
		unsigned int under = 0;
		bool over = false;
		for(int i = (int)tds.size() - 1; i >= 0 && !over; i--)
			if(shown(tds[i]) && tds[i].font == TD_FONT_REFERENCE)
				over = refImageColour(tds[i], cx, cy, &under);
		if(inside && !io.WantCaptureMouse){
			if(over){
				ImGui::BeginTooltip();
				ImGui::ColorButton("##under", ImVec4((under >> 24 & 0xFF) / 255.0f, (under >> 16 & 0xFF) / 255.0f,
					(under >> 8 & 0xFF) / 255.0f, 1.0f));
				ImGui::SameLine();
				ImGui::Text("0x%08X", under);
				ImGui::EndTooltip();
			}
			if(ImGui::IsMouseClicked(0)){
				if(over){
					gPickedColour = under & 0xFFFFFF00u;
					gPicked = true;
					gPicking = false;
					consumedPickClick = true;
				}else
					Toast(TOAST_SELECTION, "%s", _("Click on an image layer to take its colour"));
			}
		}
		if(ImGui::IsKeyPressed(ImGuiKey_Escape))
			gPicking = false;
	}
	// Before the textdraws take the click: a resize handle, a guide or a ruler.
	ImGuiContext *ctx = ImGui::GetCurrentContext();
	ImVec2 m0 = io.MousePos;
	float rs = rulerSize();
	bool overCanvas = !gPicking && !consumedPickClick && m0.x >= areaPos.x && m0.y >= areaPos.y &&
		m0.x < areaEnd.x && m0.y < areaEnd.y &&
		(ctx->HoveredWindow == nil || strncmp(ctx->HoveredWindow->Name, "##td", 4) == 0);
	bool onRuler = gRulers && overCanvas && (m0.x < areaPos.x + rs || m0.y < areaPos.y + rs);
	bool grabbed = gHandle >= 0 || gGuideDrag >= 0;
	float box[4];
	bool hasBox = !gSel.empty() && !gDragging && selectionScaleBox(box);
	bool smallBox = hasBox && (fabsf(box[2] - box[0]) * cv.sx < 24.0f || fabsf(box[3] - box[1]) * cv.sy < 24.0f);
	int hoverHandle = -1;
	if(hasBox && !grabbed && overCanvas && !onRuler)
		for(int h = 0; h < 8; h++){
			if(smallBox && (h & 1))
				continue;
			float hx, hy;
			handlePos(h, box, &hx, &hy);
			ImVec2 p = cv.at(hx, hy);
			if(fabsf(m0.x - p.x) <= 6.0f && fabsf(m0.y - p.y) <= 6.0f)
				hoverHandle = h;
		}
	if(hoverHandle >= 0){
		ImGui::SetMouseCursor(handleCursor(hoverHandle));
		if(ImGui::IsMouseClicked(0) && beginScale()){
			gHandle = hoverHandle;
			gHandleStart = m0;
			grabbed = true;
		}
	}
	std::vector<TextDrawGuide> &guides = GetTextDrawGuides();
	if(gRulers && !gGuidesLocked && !grabbed && hoverHandle < 0 && overCanvas && !onRuler){
		int hoverGuide = -1;
		for(int k = 0; k < (int)guides.size(); k++)
			if(guides[k].board == gBoard &&
			   (guides[k].vertical ? fabsf(cv.at(guides[k].at, 0).x - m0.x) : fabsf(cv.at(0, guides[k].at).y - m0.y)) <= 3.0f)
				hoverGuide = k;
		if(hoverGuide >= 0){
			ImGui::SetMouseCursor(guides[hoverGuide].vertical ? ImGuiMouseCursor_ResizeEW : ImGuiMouseCursor_ResizeNS);
			ImGui::SetTooltip("%s", _("Drag it onto a ruler to remove it"));
			if(ImGui::IsMouseClicked(0)){
				gGuideDrag = hoverGuide;
				grabbed = true;
			}
		}
	}
	if(onRuler && !grabbed && ImGui::IsMouseClicked(0)){
		// A guide's marker takes that guide; anywhere else on a ruler makes
		// one: across from the top ruler, down from the left one.
		grabbed = true;
		bool top = m0.y < areaPos.y + rs, side = m0.x < areaPos.x + rs;
		for(int k = 0; k < (int)guides.size(); k++){
			const TextDrawGuide &gd = guides[k];
			if(!gGuidesLocked && gd.board == gBoard && ((top && !side && gd.vertical && fabsf(cv.at(gd.at, 0).x - m0.x) <= 5.0f) ||
			   (side && !top && !gd.vertical && fabsf(cv.at(0, gd.at).y - m0.y) <= 5.0f)))
				gGuideDrag = k;
		}
		if(gGuideDrag < 0 && top != side){
			TextDrawGuide gd = { gBoard, side, side ? (m0.x - cv.x) / cv.sx : (m0.y - cv.y) / cv.sy };
			guides.push_back(gd);
			gGuideDrag = (int)guides.size() - 1;
		}
	}
	for(int i = (int)tds.size() - 1; i >= 0 && !gPicking && !consumedPickClick; i--){
		TextDraw &td = tds[i];
		if(!shown(td) || (td.locked && gClickThroughLocked))
			continue;
		float x0, y0, x1, y1;
		boundsOf(td, &x0, &y0, &x1, &y1);
		ImVec2 p0 = cv.at(x0, y0), p1 = cv.at(x1, y1);
		if(p1.x - p0.x < 6.0f) p1.x = p0.x + 6.0f;
		if(p1.y - p0.y < 6.0f) p1.y = p0.y + 6.0f;
		// Only the part inside the area, so it never covers the panels.
		p0.x = std::max(p0.x, areaPos.x + rs); p0.y = std::max(p0.y, areaPos.y + rs);
		p1.x = std::min(p1.x, areaEnd.x); p1.y = std::min(p1.y, areaEnd.y);
		if(p1.x - p0.x < 1.0f || p1.y - p0.y < 1.0f)
			continue;
		char id[32];
		snprintf(id, sizeof(id), "##tdhit%d", i);
		ImGui::SetNextWindowPos(p0);
		ImGui::SetNextWindowSize(ImVec2(p1.x - p0.x, p1.y - p0.y));
		ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0, 0));
		ImGui::PushStyleVar(ImGuiStyleVar_WindowMinSize, ImVec2(1, 1));
		ImGui::Begin(id, nil, flags);
		ImGui::InvisibleButton("hit", ImVec2(p1.x - p0.x, p1.y - p0.y));
		if(ImGui::IsItemActivated() && !grabbed){
			// Use the canvas stack order, even if another invisible hit window
			// happened to receive this click. A row picked in Layers wins when
			// that selected layer is under the pointer.
			std::vector<int> under;
			textDrawsAt(cv, io.MousePos, under);
			int picked = topCanvasHit(under);
			int preferred = io.KeyAlt ? -1 : layerHit(under);
			if(preferred >= 0)
				picked = preferred;
			if(io.KeyAlt){
				// Alt+click: the next one down under the cursor, for stacks.
				std::vector<int>::iterator it = std::find(under.begin(), under.end(), gSelected);
				picked = under.empty() ? i : it == under.end() || it + 1 == under.end() ? under.front() : *(it + 1);
			}
			if(picked < 0)
				picked = i;
			gLayerSelection = false;
			TextDraw &target = tds[picked];
			bool enterGroup = !target.group.empty() && target.group != gIsolatedGroup &&
				ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left);
			if(enterGroup){
				gIsolatedGroup = target.group;
				selectOnly(picked);
			}else{
				if(!gIsolatedGroup.empty() && target.group != gIsolatedGroup)
					gIsolatedGroup.clear();
				if(!target.group.empty() && target.group != gIsolatedGroup){
					if(io.KeyCtrl)
						toggleGroupSelection(target.group, picked);
					else{
						selectGroup(target.group, io.KeyShift);
						gSelected = picked;
					}
				}else if(io.KeyShift || io.KeyCtrl)
					toggleSelected(picked);
				else if(!isSelected(picked))
					selectOnly(picked);
				else
					gSelected = picked;
			}
			if(!enterGroup && isSelected(picked))
				startDrag();
		}
		if(ImGui::IsItemHovered() && !gDragging){
			hovered = i;
			const char *name = td.name[0] ? td.name : _("textdraw");
			ImGui::SetItemTooltip("%s%s%s", name, td.group.empty() ? "" : "  -  ", td.group.c_str());
		}
		ImGui::End();
		ImGui::PopStyleVar(2);
	}
	if(gDragging){
		if(ImGui::IsMouseDown(0))
			dragSelection(cv, dl);
		else
			gDragging = false;
	}

	// Outlines: what the mouse is over, the selection, the one being edited.
	if(hovered >= 0 && !isSelected(hovered)){
		float x0, y0, x1, y1;
		boundsOf(tds[hovered], &x0, &y0, &x1, &y1);
		dl->AddRect(cv.at(x0, y0), cv.at(x1, y1), IM_COL32(120, 190, 255, 200), 0.0f, 0, 1.0f);
	}
	for(int i : gSel){
		if(!shown(tds[i]))
			continue;
		float x0, y0, x1, y1;
		boundsOf(tds[i], &x0, &y0, &x1, &y1);
		ImU32 c = tds[i].locked ? IM_COL32(200, 200, 200, 200) : i == gSelected ? IM_COL32(255, 220, 60, 255) :
			IM_COL32(255, 220, 60, 150);
		dl->AddRect(cv.at(x0, y0), cv.at(x1, y1), c, 0.0f, 0, i == gSelected ? 1.5f : 1.0f);
	}

	// Resizing: by the handles round the selection's box.
	if(gHandle >= 0){
		if(ImGui::IsMouseDown(0)){
			ImGui::SetMouseCursor(handleCursor(gHandle));
			resizeSelection(cv, dl);
		}else
			gHandle = -1;
	}
	if(!gSel.empty() && !gDragging && selectionScaleBox(box)){
		ImVec2 p0 = cv.at(box[0], box[1]), p1 = cv.at(box[2], box[3]);
		if(gSel.size() > 1)
			dl->AddRect(p0, p1, IM_COL32(255, 220, 60, 110));
		smallBox = fabsf(p1.x - p0.x) < 24.0f || fabsf(p1.y - p0.y) < 24.0f;
		for(int h = 0; h < 8; h++){
			if(smallBox && (h & 1))
				continue;
			float hx, hy;
			handlePos(h, box, &hx, &hy);
			ImVec2 p = cv.at(hx, hy);
			dl->AddRectFilled(ImVec2(p.x - 4, p.y - 4), ImVec2(p.x + 4, p.y + 4), IM_COL32(255, 220, 60, 255));
			dl->AddRect(ImVec2(p.x - 4, p.y - 4), ImVec2(p.x + 4, p.y + 4), IM_COL32(20, 20, 20, 255));
		}
		// The bottom right one is also an item, for UI scripts to find (the
		// click itself is taken above, by where the mouse is).
		ImGui::SetNextWindowPos(ImVec2(p1.x - 6, p1.y - 6));
		ImGui::SetNextWindowSize(ImVec2(12, 12));
		ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0, 0));
		ImGui::PushStyleVar(ImGuiStyleVar_WindowMinSize, ImVec2(1, 1));
		ImGui::Begin("##tdresize", nil, flags);
		ImGui::InvisibleButton("resize", ImVec2(12, 12));
		ImGui::End();
		ImGui::PopStyleVar(2);
	}

	// Guides: dragged, or dropped on a ruler (or off the canvas) to go.
	if(gGuideDrag >= 0 && gGuideDrag < (int)guides.size()){
		TextDrawGuide &gd = guides[gGuideDrag];
		ImGui::SetMouseCursor(gd.vertical ? ImGuiMouseCursor_ResizeEW : ImGuiMouseCursor_ResizeNS);
		if(ImGui::IsMouseDown(0)){
			float at = gd.vertical ? (m0.x - cv.x) / cv.sx : (m0.y - cv.y) / cv.sy;
			at = roundf(at * 2.0f) * 0.5f;
			if(gSnap && !io.KeyCtrl){
				gd.at = -1e9f;	// not to itself
				std::vector<float> xs, ys;
				snapTargets(xs, ys);
				float e[3] = { at, at, at }, unused;
				at += snapAxis(e, gd.vertical ? xs : ys, 6.0f / (gd.vertical ? cv.sx : cv.sy), &unused);
			}
			gd.at = at;
			ImGui::SetTooltip("%s %.1f", gd.vertical ? "x" : "y", at);
		}else{
			bool off = m0.x < areaPos.x + rs || m0.y < areaPos.y + rs || m0.x >= areaEnd.x || m0.y >= areaEnd.y;
			if(off)
				guides.erase(guides.begin() + gGuideDrag);
			gGuideDrag = -1;
			TextDrawGuidesChanged();
		}
	}else
		gGuideDrag = -1;

	// Alt: how far the selection is from what the mouse is over, or the screen's edges.
	if(io.KeyAlt && !gSel.empty() && gHandle < 0 && !gDragging){
		float A[4] = { 1e9f, 1e9f, -1e9f, -1e9f }, B[4] = { 0.0f, 0.0f, 640.0f, 448.0f };
		for(int i : gSel){
			float x0, y0, x1, y1;
			boundsOf(tds[i], &x0, &y0, &x1, &y1);
			A[0] = std::min(A[0], x0); A[1] = std::min(A[1], y0);
			A[2] = std::max(A[2], x1); A[3] = std::max(A[3], y1);
		}
		if(hovered >= 0 && !isSelected(hovered))
			boundsOf(tds[hovered], &B[0], &B[1], &B[2], &B[3]);
		drawDistances(dl, cv, A, B);
	}

	// On empty canvas: drag a box to select, click to clear; the wheel zooms
	// round the cursor, the middle button pans.
	ImVec2 m = io.MousePos;
	bool inArea = m.x >= areaPos.x && m.y >= areaPos.y && m.x < areaEnd.x && m.y < areaEnd.y;
	if(inArea && !io.WantCaptureMouse && ImGui::IsMouseClicked(0) && !gPicking && !consumedPickClick && !grabbed){
		gMarquee = true;
		gMarqueeAdd = io.KeyShift || io.KeyCtrl;
		gMarqueeStart = m;
		gLayerSelection = false;
		gIsolatedGroup.clear();
	}
	// Over the textdraws too, which take the mouse for clicks and drags;
	// not over the panels.
	if(inArea && (ctx->HoveredWindow == nil || strncmp(ctx->HoveredWindow->Name, "##td", 4) == 0)){
		if(io.MouseWheel != 0.0f && !io.KeyCtrl){
			float cxU = (m.x - cv.x) / cv.sx, cyU = (m.y - cv.y) / cv.sy;
			gZoom = std::max(0.25f, std::min(gZoom * powf(1.15f, io.MouseWheel), MAX_ZOOM));
			Canvas after = getCanvas();
			gPanX += m.x - (after.x + cxU * after.sx);
			gPanY += m.y - (after.y + cyU * after.sy);
		}
		if(ImGui::IsMouseClicked(2))
			gPanning = true;
	}
	if(gPanning){
		if(ImGui::IsMouseDown(2)){
			gPanX += io.MouseDelta.x;
			gPanY += io.MouseDelta.y;
			ImGui::SetMouseCursor(ImGuiMouseCursor_ResizeAll);
		}else
			gPanning = false;
	}
	if(gMarquee){
		ImVec2 a(std::min(gMarqueeStart.x, m.x), std::min(gMarqueeStart.y, m.y));
		ImVec2 b(std::max(gMarqueeStart.x, m.x), std::max(gMarqueeStart.y, m.y));
		bool boxed = b.x - a.x > 3.0f || b.y - a.y > 3.0f;
		if(ImGui::IsMouseDown(0)){
			if(boxed){
				dl->AddRectFilled(a, b, IM_COL32(120, 190, 255, 40));
				dl->AddRect(a, b, IM_COL32(120, 190, 255, 200));
			}
		}else{
			gMarquee = false;
			if(!gMarqueeAdd)
				selectOnly(-1);
			if(boxed)
				for(int i = 0; i < (int)tds.size(); i++){
					if(!shown(tds[i]))
						continue;
					float x0, y0, x1, y1;
					boundsOf(tds[i], &x0, &y0, &x1, &y1);
					ImVec2 p = cv.at(x0, y0), q = cv.at(x1, y1);
					if(p.x <= b.x && q.x >= a.x && p.y <= b.y && q.y >= a.y)
						addToSelection(i);
				}
		}
	}

	// Arrow keys nudge the selection while no field has the keyboard.
	ImGuiContext *g = ImGui::GetCurrentContext();
	bool canvasHasKeys = !io.WantTextInput && !ImGui::IsAnyItemActive() &&
		(g->NavWindow == nil || strncmp(g->NavWindow->Name, "##td", 4) == 0);
	if(canvasHasKeys && !gSel.empty()){
		float step = gNudge * (io.KeyShift ? 10.0f : 1.0f);
		float dx = 0.0f, dy = 0.0f;
		if(ImGui::IsKeyPressed(ImGuiKey_LeftArrow)) dx -= step;
		if(ImGui::IsKeyPressed(ImGuiKey_RightArrow)) dx += step;
		if(ImGui::IsKeyPressed(ImGuiKey_UpArrow)) dy -= step;
		if(ImGui::IsKeyPressed(ImGuiKey_DownArrow)) dy += step;
		if((dx != 0.0f || dy != 0.0f) && io.KeyAlt){
			// Alt: wider or taller instead, by the right and bottom edges.
			if(beginScale()){
				float n[4] = { gBoxFrom[0], gBoxFrom[1], gBoxFrom[2] + dx, gBoxFrom[3] + dy };
				if(n[2] - n[0] >= 1.0f && n[3] - n[1] >= 1.0f)
					scaleSelection(n);
			}
		}else if(dx != 0.0f || dy != 0.0f){
			for(int i : gSel)
				if(!tds[i].locked)
					moveBy(tds[i], dx, dy);
			TextDrawsChanged();
		}
	}
	if(canvasHasKeys && io.KeyCtrl && ImGui::IsKeyPressed(ImGuiKey_A, false)){
		selectOnly(-1);
		for(int i = 0; i < (int)tds.size(); i++)
			if(shown(tds[i]))
				addToSelection(i);
	}
	if(gRulers){
		float ext[4];
		bool any = false;
		for(int i : gSel){
			float x0, y0, x1, y1;
			boundsOf(tds[i], &x0, &y0, &x1, &y1);
			if(!any){ ext[0] = x0; ext[1] = y0; ext[2] = x1; ext[3] = y1; any = true; }
			ext[0] = std::min(ext[0], x0); ext[1] = std::min(ext[1], y0);
			ext[2] = std::max(ext[2], x1); ext[3] = std::max(ext[3], y1);
		}
		drawRulers(dl, cv, areaPos, areaEnd, any ? ext : nil);
	}
	dl->PopClipRect();
	if(gCompare)
		drawCompare();
}

// ---- Window ----

// A colour typed the ways scripts write them: 0xRRGGBBAA, #RRGGBB[AA],
// {RRGGBB}, rgb(r, g, b), rgba(r, g, b, a), "r g b [a]" or "r, g, b[, a]",
// or a decimal like -1. A missing alpha is opaque.
static bool
parseColourText(const char *text, unsigned int *out)
{
	std::string s = text;
	size_t a = s.find_first_not_of(" \t"), b = s.find_last_not_of(" \t");
	if(a == std::string::npos)
		return false;
	s = s.substr(a, b - a + 1);
	std::string low = s;
	for(char &c : low)
		c = (char)tolower((unsigned char)c);
	auto hex = [&](std::string h) -> bool {
		if(h.size() != 6 && h.size() != 8)
			return false;
		for(char c : h)
			if(!isxdigit((unsigned char)c))
				return false;
		unsigned int v = (unsigned int)strtoul(h.c_str(), nil, 16);
		*out = h.size() == 6 ? v << 8 | 0xFF : v;
		return true;
	};
	if(low.compare(0, 2, "0x") == 0)
		return hex(low.substr(2));
	if(low[0] == '#')
		return hex(low.substr(1));
	if(low[0] == '{' && low.back() == '}')
		return hex(low.substr(1, low.size() - 2));
	// Numbers: rgb(...), rgba(...), or three or four of them.
	std::string nums = low;
	if(nums.compare(0, 5, "rgba(") == 0) nums = nums.substr(5);
	else if(nums.compare(0, 4, "rgb(") == 0) nums = nums.substr(4);
	for(char &c : nums)
		if(c == ',' || c == '(' || c == ')')
			c = ' ';
	std::vector<long> v;
	char *p = &nums[0], *end;
	while(true){
		long n = strtol(p, &end, 10);
		if(end == p)
			break;
		v.push_back(n);
		p = end;
	}
	while(*p == ' ') p++;
	if(*p != '\0')
		return hex(low);
	if(v.size() == 1){	// a decimal colour, as Pawn prints them (-1 is white)
		*out = (unsigned int)v[0];
		return true;
	}
	if(v.size() != 3 && v.size() != 4)
		return false;
	for(long n : v)
		if(n < 0 || n > 255)
			return false;
	*out = (unsigned int)v[0] << 24 | (unsigned int)v[1] << 16 | (unsigned int)v[2] << 8 |
		(unsigned int)(v.size() == 4 ? v[3] : 255);
	return true;
}

static bool
colourEdit(const char *label, unsigned int *rgba)
{
	bool changed = false;
	float c[4] = { (*rgba >> 24 & 0xFF) / 255.0f, (*rgba >> 16 & 0xFF) / 255.0f,
		(*rgba >> 8 & 0xFF) / 255.0f, (*rgba & 0xFF) / 255.0f };
	ImGui::TextUnformatted(label, ImGui::FindRenderedTextEnd(label));
	ImGui::PushID(label);
	ImGuiID id = ImGui::GetID("##edit");
	if(gPicked && gPickFor == id){
		*rgba = gPickedColour | (*rgba & 0xFF);	// the image's colour, this field's alpha
		gPicked = false;
		gPicking = false;
		changed = true;
	}
	float w = ImGui::GetContentRegionAvail().x - ImGui::GetFrameHeight() - ImGui::GetStyle().ItemInnerSpacing.x;
	ImGui::SetNextItemWidth(std::max(ImGui::GetFontSize() * 4.0f, w));
	if(ImGui::ColorEdit4("##edit", c, ImGuiColorEditFlags_AlphaBar | ImGuiColorEditFlags_AlphaPreviewHalf)){
		*rgba = (unsigned int)(c[0] * 255.0f + 0.5f) << 24 | (unsigned int)(c[1] * 255.0f + 0.5f) << 16 |
			(unsigned int)(c[2] * 255.0f + 0.5f) << 8 | (unsigned int)(c[3] * 255.0f + 0.5f);
		changed = true;
	}
	ImGui::SameLine(0.0f, ImGui::GetStyle().ItemInnerSpacing.x);
	if(ImGui::Button(ICON_FA_PAINTBRUSH "##palette", ImVec2(ImGui::GetFrameHeight(), 0)))
		ImGui::OpenPopup("##colours");
	ImGui::SetItemTooltip("%s", _("Saved colours, and a colour typed as 0xRRGGBBAA, #RRGGBB, rgba(...) or numbers"));
	if(ImGui::BeginPopup("##colours")){
		static char typed[64];
		if(ImGui::IsWindowAppearing())
			snprintf(typed, sizeof(typed), "0x%08X", *rgba);
		ImGui::SetNextItemWidth(ImGui::GetFontSize() * 12.0f);
		bool enter = ImGui::InputText("##typed", typed, sizeof(typed), ImGuiInputTextFlags_EnterReturnsTrue);
		unsigned int parsed;
		bool ok = parseColourText(typed, &parsed);
		ImGui::SameLine();
		ImGui::BeginDisabled(!ok);
		if(ImGui::Button(L("Use")) || (enter && ok)){
			*rgba = parsed;
			changed = true;
		}
		ImGui::EndDisabled();
		if(!ok && typed[0])
			ImGui::TextDisabled("%s", _("Not a colour"));
		if(ImGui::Button(LI(ICON_FA_DROPLET, "Pick from an image"))){
			gPicking = true;
			gPickFor = id;
			ImGui::CloseCurrentPopup();
		}
		ImGui::SetItemTooltip("%s", _("Then click on an image layer on the canvas (Esc stops)."));
		ImGui::SeparatorText(_("Saved colours"));
		int perRow = 8, remove = -1;
		for(int i = 0; i < (int)gSavedColours.size(); i++){
			unsigned int s = gSavedColours[i];
			ImVec4 v((s >> 24 & 0xFF) / 255.0f, (s >> 16 & 0xFF) / 255.0f, (s >> 8 & 0xFF) / 255.0f, (s & 0xFF) / 255.0f);
			if(i % perRow)
				ImGui::SameLine();
			ImGui::PushID(i);
			if(ImGui::ColorButton("##saved", v, ImGuiColorEditFlags_AlphaPreviewHalf)){
				*rgba = s;
				changed = true;
			}
			if(ImGui::BeginPopupContextItem()){
				if(ImGui::MenuItem(L("Remove")))
					remove = i;
				ImGui::EndPopup();
			}
			ImGui::SetItemTooltip("0x%08X", s);
			ImGui::PopID();
		}
		if(gSavedColours.empty())
			ImGui::TextDisabled("%s", _("None yet."));
		if(remove >= 0){
			gSavedColours.erase(gSavedColours.begin() + remove);
			SaveEditorSettings();
		}
		if(ImGui::Button(LI(ICON_FA_PLUS, "Save this colour")) &&
		   std::find(gSavedColours.begin(), gSavedColours.end(), *rgba) == gSavedColours.end()){
			gSavedColours.push_back(*rgba);
			SaveEditorSettings();
		}
		ImGui::EndPopup();
	}
	ImGui::PopID();
	return changed;
}

static void
addTextDraw(int font)
{
	TextDraw td;
	InitTextDraw(&td);
	td.x = 280.0f;
	td.y = 200.0f;
	td.font = font;
	if(font == TD_FONT_REFERENCE){
		td.text = "";
		td.x = 160.0f; td.y = 112.0f;
		td.sizeX = 320.0f; td.sizeY = 224.0f;
		td.colour = 0xFFFFFF99;
		td.locked = false;
	}else if(font == TD_FONT_SPRITE){
		td.text = "LD_BEAT:chit";
		td.sizeX = td.sizeY = 32.0f;
		td.colour = 0xFFFFFFFF;
	}else if(font == TD_FONT_PREVIEW){
		td.text = "_";
		td.sizeX = td.sizeY = 80.0f;
		td.previewModel = 411;
		td.backgroundColour = 0x00000080;
	}else if(font < 0){
		// A box: an empty text with a background.
		td.font = TD_FONT_AHARONI;
		td.text = "_";
		td.useBox = true;
		td.sizeX = td.x + 120.0f;
		td.letterY = 4.0f;
		td.boxColour = 0x00000099;
	}else
		td.text = "Text";
	td.board = gBoard;
	GetTextDraws().push_back(td);
	selectOnly((int)GetTextDraws().size() - 1);
	TextDrawsChanged();
}

// An image layer's file: read again, and fitted in its box keeping the
// picture's shape, centred where the box was.
static void
setImageFile(TextDraw &td, const char *path)
{
	td.text = path;
	gRefImages.erase(td.text);
	const RefImage *ref = refImage(td.text);
	if(ref == nil || ref->image->width <= 0 || ref->image->height <= 0)
		return;
	float aspect = (float)ref->image->width / ref->image->height;
	float cx = td.x + td.sizeX * 0.5f, cy = td.y + td.sizeY * 0.5f;
	float w = fabsf(td.sizeX), h = fabsf(td.sizeY);
	if(w / h > aspect) w = h * aspect;
	else h = w / aspect;
	td.sizeX = w; td.sizeY = h;
	td.x = cx - w * 0.5f; td.y = cy - h * 0.5f;
}

// A picture dropped on the window in TextDraw mode: a new image layer.
bool
AddTextDrawImageLayer(const char *path)
{
	if(!gMode || path == nil)
		return false;
	addTextDraw(TD_FONT_REFERENCE);
	setImageFile(GetTextDraws()[gSelected], path);
	TextDrawsChanged();
	return true;
}

// A grid's width with room for its scrollbar whether it shows or not, so
// the scrollbar coming and going doesn't change the columns every frame.
static float
gridWidth(void)
{
	float w = ImGui::GetContentRegionAvail().x;
	if(!ImGui::GetCurrentWindow()->ScrollbarY)
		w -= ImGui::GetStyle().ScrollbarSize;
	return w;
}

static bool
isFavSprite(const std::string &sprite)
{
	for(const std::string &f : gFavSprites)
		if(lowered(f) == lowered(sprite))
			return true;
	return false;
}

static void
toggleFavSprite(const std::string &sprite)
{
	for(size_t i = 0; i < gFavSprites.size(); i++)
		if(lowered(gFavSprites[i]) == lowered(sprite)){
			gFavSprites.erase(gFavSprites.begin() + i);
			SaveEditorSettings();
			return;
		}
	gFavSprites.push_back(sprite);
	SaveEditorSettings();
}

// A favourite may refer to a dynamically registered server model. Reuse the
// document's TXD binding so choosing it does not lose the preview source.
static std::string
favouriteServerTxd(const std::string &sprite)
{
	size_t colon = sprite.find(':');
	if(colon == std::string::npos || rw::strncmp_ci(sprite.c_str(), "mdl-", 4) != 0) return "";
	std::string dictionary = sprite.substr(0, colon);
	for(const TextDraw &td : GetTextDraws())
		if(!td.serverTxd.empty() && rw::strcmp_ci(td.text.substr(0, td.text.find(':')).c_str(), dictionary.c_str()) == 0)
			return td.serverTxd;
	return ServerTexturePathForModel(-atoi(dictionary.c_str()+4));
}

// TXDs from the server's configured artwork folder and the installed game.
static void
spritePicker(TextDraw &td)
{
	static std::string txdName, serverFile, lastText, lastFile, lastVariable;
	static int modelId;
	static bool initialized;
	static const std::string FAVOURITES = "\x01" "favourites";
	static std::string listedRoot;
	static std::vector<std::string> serverFiles;
	std::string root = GetCustomModelsDir() ? GetCustomModelsDir() : "";
	if(listedRoot != root){
		listedRoot = root;
		ListServerTextureFiles(serverFiles);
		initialized = false;
	}
	if(!initialized || lastVariable != td.name || lastText != td.text || lastFile != td.serverTxd){
		initialized = true;
		size_t colon = td.text.find(':');
		txdName = colon == std::string::npos ? "LD_BEAT" : td.text.substr(0, colon);
		modelId = rw::strncmp_ci(txdName.c_str(), "mdl-", 4) == 0 ? -atoi(txdName.c_str()+4) : 0;
		serverFile = td.serverTxd.empty() ? ServerTexturePathForModel(modelId) : td.serverTxd;
	}
	std::vector<std::string> txds;
	listSpriteTxds(txds);
	std::string shown = txdName == FAVOURITES ? std::string(ICON_FA_STAR " ") + _("Favourites") :
		!serverFile.empty() ? serverFile : txdName;
	if(ImGui::BeginCombo(L("Sprite TXD"), shown.c_str(), ImGuiComboFlags_HeightLarge)){
		static char txdFilter[128];
		if(ImGui::IsWindowAppearing()){
			txdFilter[0] = '\0';
			ImGui::SetKeyboardFocusHere();
		}
		ImGui::SetNextItemWidth(-FLT_MIN);
		ImGui::InputTextWithHint("##txd_filter", _("Search TXDs"), txdFilter, sizeof(txdFilter));
		if(ImGui::Selectable((std::string(ICON_FA_STAR " ") + _("Favourites")).c_str(), txdName == FAVOURITES)){
			txdName = FAVOURITES; serverFile.clear(); modelId = 0;
		}
		std::string want = lowered(txdFilter);
		ImGui::SeparatorText(_("Server TXDs"));
		if(ImGui::SmallButton(L("Refresh"))) ListServerTextureFiles(serverFiles);
		if(root.empty()) ImGui::TextWrapped(_("Open a server workspace to browse its textures."));
		else ImGui::SetItemTooltip("%s", root.c_str());
		for(const std::string &file : serverFiles){
			if(!want.empty() && lowered(file).find(want) == std::string::npos) continue;
			if(ImGui::Selectable(file.c_str(), file == serverFile)){
				serverFile = file;
				modelId = ServerTextureModelId(file);
				txdName = modelId ? "mdl" + std::to_string(modelId) : "";
			}
		}
		if(!root.empty() && serverFiles.empty()) ImGui::TextDisabled(_("No server TXDs found."));
		// Keep the ID entries too, for projects already using mdl-ID references.
		std::vector<int> customs;
		ListCustomSimpleModels(customs);
		if(!customs.empty()) ImGui::SeparatorText(_("Custom models (0.3.DL)"));
		for(int id : customs){
			char key[32], label[160];
			snprintf(key, sizeof(key), "mdl%d", id);
			ObjectDef *obj = GetObjectDef(EditorModelId(id));
			const char *file = obj && obj->m_txdSlot >= 0 ? GetCustomModelTxdName(obj->m_txdSlot) : "";
			snprintf(label, sizeof(label), "%s  (%s)", key, file ? file : "");
			if(want.empty() || lowered(label).find(want) != std::string::npos)
				if(ImGui::Selectable(label, rw::strcmp_ci(key, txdName.c_str()) == 0)){
					txdName = key; modelId = id; serverFile = ServerTexturePathForModel(id);
				}
		}
		ImGui::SeparatorText(_("The game's"));
		for(const std::string &name : txds)
			if(want.empty() || lowered(name).find(want) != std::string::npos)
				if(ImGui::Selectable(name.c_str(), rw::strcmp_ci(name.c_str(), txdName.c_str()) == 0)){
					txdName = name; serverFile.clear(); modelId = 0;
				}
		ImGui::EndCombo();
	}
	ImGui::SetItemTooltip("%s", root.empty() ? _("Open a server workspace to browse its textures.") : root.c_str());
	bool server = !serverFile.empty() || rw::strncmp_ci(txdName.c_str(), "mdl-", 4) == 0;
	bool validModel = modelId <= -1000 && modelId >= -30000;
	if(server){
		ImGui::SetNextItemWidth(ImGui::GetFontSize()*10.0f);
		if(ImGui::InputInt(L("Model ID"), &modelId, 0, 0)){
			validModel = modelId <= -1000 && modelId >= -30000;
			if(validModel){
				txdName = "mdl" + std::to_string(modelId);
				if(!serverFile.empty() && td.serverTxd == serverFile && td.text.find(':') != std::string::npos){
					td.text = txdName + td.text.substr(td.text.find(':'));
					TextDrawsChanged();
				}
			}
		}
		ImGui::SetItemTooltip(_("Use the model ID registered by AddSimpleModel on your server (-1000 to -30000)."));
		UiBadge("0.3.DL", _("A custom model's texture: players on 0.3.DL and open.mp download it;\n"
			"0.3.7 players see nothing there."));
		if(!validModel) ImGui::TextWrapped(_("Enter the registered model ID before choosing a texture."));
		if(serverFile.empty()) ImGui::TextWrapped(_("Choose the server TXD for this model ID."));
	}
	static char filter[64];
	ImGui::SetNextItemWidth(-FLT_MIN);
	ImGui::InputTextWithHint("##sprite_filter", _("Search sprites in it"), filter, sizeof(filter));
	std::string want = lowered(filter);
	std::vector<std::pair<std::string, rw::Texture*>> sprites;
	if(txdName == FAVOURITES){
		for(const std::string &f : gFavSprites)
			sprites.push_back(std::make_pair(f, spriteTexture(f, favouriteServerTxd(f).c_str())));
	}else{
		int slot = serverFile.empty() ? spriteTxdSlot(txdName) : LoadServerTextureTxd(serverFile);
		int count = slot >= 0 ? GetTxdTextureCount(slot) : 0;
		for(int i = 0; i < count; i++)
			if(rw::Texture *tex = GetTxdTexture(slot, i)) sprites.push_back(std::make_pair(
				txdName.empty() ? tex->name : txdName + ":" + tex->name, tex));
		if(!serverFile.empty() && slot < 0) ImGui::TextWrapped(_("The selected server TXD could not be loaded."));
	}
	ImGui::BeginChild("sprites", ImVec2(0, ImGui::GetFontSize()*9.0f), ImGuiChildFlags_Borders | ImGuiChildFlags_ResizeY);
	float cell = ImGui::GetFontSize()*3.0f;
	int perRow = std::max(1, (int)(gridWidth()/(cell+4.0f))), shownCount = 0;
	for(size_t i = 0; i < sprites.size(); i++){
		const std::string &name = sprites[i].first;
		if(!want.empty() && lowered(name).find(want) == std::string::npos) continue;
		if(shownCount++ % perRow) ImGui::SameLine();
		ImGui::PushID((int)i);
		bool fav = isFavSprite(name);
		ImVec2 at = ImGui::GetCursorScreenPos();
		ImGui::BeginDisabled(server && !validModel);
		bool picked = sprites[i].second ? ImGui::ImageButton("##s", (ImTextureID)sprites[i].second, ImVec2(cell, cell)) :
			ImGui::Button("?##s", ImVec2(cell + ImGui::GetStyle().FramePadding.x*2.0f, cell + ImGui::GetStyle().FramePadding.y*2.0f));
		if(picked){
			std::string source = txdName == FAVOURITES ? favouriteServerTxd(name) : serverFile;
			td.text = name;
			td.serverTxd = source;
			TextDrawsChanged();
		}
		if(fav) ImGui::GetWindowDrawList()->AddText(ImVec2(at.x+2.0f, at.y+1.0f), IM_COL32(255, 210, 60, 255), ICON_FA_STAR);
		if(ImGui::BeginPopupContextItem()){
			if(ImGui::MenuItem(fav ? L("Remove from favourites") : L("Add to favourites"))) toggleFavSprite(name);
			ImGui::EndPopup();
		}
		ImGui::EndDisabled();
		ImGui::SetItemTooltip("%s%s", name.c_str(), fav ? "" : _("\nRight-click to keep it in Favourites"));
		ImGui::PopID();
	}
	if(shownCount == 0) ImGui::TextDisabled("%s", txdName == FAVOURITES ?
		_("No favourites yet: right-click a sprite to add it.") : _("No sprites match."));
	ImGui::EndChild();
	lastText = td.text; lastFile = td.serverTxd; lastVariable = td.name;
}

// Models for a model preview: objects, vehicles or skins by name or ID,
// with thumbnails as the object browser draws them.
static bool
modelBrowser(int *model)
{
	bool picked = false;
	if(ImGui::Button(LI(ICON_FA_MAGNIFYING_GLASS, "Browse models...")))
		ImGui::OpenPopup("##models");
	ImGui::SetNextWindowSize(ImVec2(ImGui::GetFontSize() * 26.0f, ImGui::GetFontSize() * 24.0f), ImGuiCond_Appearing);
	if(ImGui::BeginPopup("##models")){
		static char search[64];
		static int kind;	// all, vehicles, skins, objects
		static int page;
		const char *kinds[] = { _("All"), _("Vehicles"), _("Skins"), _("Objects") };
		ImGui::SetNextItemWidth(ImGui::GetFontSize() * 7.0f);
		if(ImGui::Combo("##kind", &kind, kinds, IM_ARRAYSIZE(kinds)))
			page = 0;
		ImGui::SameLine();
		ImGui::SetNextItemWidth(-FLT_MIN);
		if(ImGui::IsWindowAppearing())
			ImGui::SetKeyboardFocusHere();
		if(ImGui::InputTextWithHint("##search", _("Name or ID"), search, sizeof(search)))
			page = 0;
		std::string want = lowered(search);
		std::vector<int> ids;
		for(int id = 0; id < NUMOBJECTDEFS && ids.size() < 4000; id++){
			bool vehicle = id >= 400 && id <= 611, skin = id <= 311;
			if((kind == 1 && !vehicle) || (kind == 2 && !skin) || (kind == 3 && (vehicle || skin)))
				continue;
			ObjectDef *obj = GetObjectDef(id);
			if(obj == nil)
				continue;
			if(!want.empty() && lowered(obj->m_name).find(want) == std::string::npos &&
			   std::to_string(id).find(want) != 0)
				continue;
			ids.push_back(id);
		}
		const int perPage = 60;
		int pages = std::max(1, ((int)ids.size() + perPage - 1) / perPage);
		page = std::max(0, std::min(page, pages - 1));
		ImGui::BeginChild("##grid", ImVec2(0, ImGui::GetFontSize() * 18.0f), ImGuiChildFlags_Borders);
		float cell = ImGui::GetFontSize() * 4.0f;
		int perRow = std::max(1, (int)(gridWidth() / (cell + 8.0f)));
		for(int k = page * perPage, n = 0; k < (int)ids.size() && k < (page + 1) * perPage; k++, n++){
			int id = ids[k];
			if(n % perRow)
				ImGui::SameLine();
			ImGui::PushID(id);
			rw::Texture *thumb = GetObjectThumbnailTexture(id);
			bool clicked = thumb ? ImGui::ImageButton("##m", (ImTextureID)thumb, ImVec2(cell, cell)) :
				ImGui::Button(std::to_string(id).c_str(), ImVec2(cell + ImGui::GetStyle().FramePadding.x * 2.0f,
					cell + ImGui::GetStyle().FramePadding.y * 2.0f));
			if(clicked){
				*model = id;
				picked = true;
				ImGui::CloseCurrentPopup();
			}
			ImGui::SetItemTooltip("%d  %s", id, GetObjectDef(id)->m_name);
			ImGui::PopID();
		}
		if(ids.empty())
			ImGui::TextDisabled("%s", _("No models match."));
		ImGui::EndChild();
		if(ImGui::SmallButton("<##page") && page > 0) page--;
		ImGui::SameLine();
		ImGui::Text(_("Page %d of %d (%d models)"), page + 1, pages, (int)ids.size());
		ImGui::SameLine();
		if(ImGui::SmallButton(">##page") && page + 1 < pages) page++;
		ImGui::EndPopup();
	}
	return picked;
}

// Stack labels above fields so they stay readable in a narrow dock.
static const char *propertyLabel(const char *label)
{
	ImGui::TextUnformatted(label, ImGui::FindRenderedTextEnd(label));
	ImGui::SetNextItemWidth(-FLT_MIN);
	static std::string id;
	const char *hash = strstr(label, "###");
	id = hash ? hash : std::string("###") + label;
	return id.c_str();
}

static void
flowNext(float width, float gap = -1.0f)
{
	ImGui::SameLine(0.0f, gap < 0.0f ? ImGui::GetStyle().ItemSpacing.x : gap);
	if(ImGui::GetContentRegionAvail().x < width)
		ImGui::NewLine();
}

// The panel's settings for what is selected. With several, a change goes to
// all of them: the edited one's value is shown, positions move by the same step.
static void
properties(void)
{
	std::vector<TextDraw> &tds = GetTextDraws();
	TextDraw &td = tds[gSelected];
	const bool many = gSel.size() > 1;
	bool changed = false;
	// Sets the field on every selected textdraw (image layers only move).
	auto all = [&](const std::function<void(TextDraw &)> &set){
		for(int i : gSel)
			if(tds[i].font != TD_FONT_REFERENCE)
				set(tds[i]);
		changed = true;
	};
	bool isText = td.font < TD_FONT_SPRITE;
	if(td.font == TD_FONT_REFERENCE && !many){
		// Not a textdraw: its file, its place and how much of it shows.
		char path[1024];
		snprintf(path, sizeof(path), "%s", td.text.c_str());
		if(ImGui::InputTextWithHint(propertyLabel(L("Image file")), _("full path to a PNG, BMP or TGA"), path, sizeof(path),
			ImGuiInputTextFlags_EnterReturnsTrue)){
			setImageFile(td, path);
			changed = true;
		}
		ImGui::SetItemTooltip("%s", _("Enter loads it. Only the editor shows it: the Pawn has it as a comment.\n"
			"Or drop a picture on the window to add it as a new image layer."));
		if(ImGui::Button(LI(ICON_FA_FOLDER_OPEN, "Browse...##image")) &&
		   PickFileDialog(path, sizeof(path), _("Choose an image"), _("Images"), "png bmp tga")){
			setImageFile(td, path);
			changed = true;
		}
		if(const RefImage *ref = refImage(td.text)){
			ImGui::TextDisabled(_("%d x %d pixels"), ref->image->width, ref->image->height);
			ImGui::SameLine();
			if(ImGui::SmallButton(L("Fill the screen"))){
				td.x = 0.0f; td.y = 0.0f; td.sizeX = 640.0f; td.sizeY = 448.0f;
				changed = true;
			}
		}
		float pos[2] = { td.x, td.y }, size[2] = { td.sizeX, td.sizeY };
		if(ImGui::DragFloat2(propertyLabel(L("Position")), pos, 0.5f, -640.0f, 1280.0f, "%.1f")){
			td.x = pos[0]; td.y = pos[1];
			changed = true;
		}
		if(ImGui::DragFloat2(propertyLabel(L("Size")), size, 0.5f, -1280.0f, 1280.0f, "%.1f")){
			td.sizeX = size[0]; td.sizeY = size[1];
			changed = true;
		}
		int opacity = (int)((td.colour & 0xFF) * 100 / 255);
		if(ImGui::SliderInt(propertyLabel(L("Opacity")), &opacity, 0, 100, "%d%%")){
			td.colour = (td.colour & 0xFFFFFF00u) | (unsigned int)(opacity * 255 / 100);
			changed = true;
		}
		if(changed)
			TextDrawsChanged();
		return;
	}
	if(many){
		ImGui::TextColored(ImVec4(1.0f, 0.86f, 0.35f, 1.0f), _("%d selected: changes go to all of them."), (int)gSel.size());
		ImGui::SetItemTooltip("%s", _("The fields show the one clicked last."));
	}

	if(ImGui::BeginTabBar("##td_props")){
		if(ImGui::BeginTabItem(L("Content"))){
			static const char *fonts[] = { N_("0 Beckett (gothic)"), N_("1 Aharoni"), N_("2 Bank Gothic (capitals)"),
				N_("3 Pricedown (capitals)"), N_("4 Sprite"), N_("5 Model preview") };
			const char *fontNames[IM_ARRAYSIZE(fonts)];
			for(int i = 0; i < IM_ARRAYSIZE(fonts); i++)
				fontNames[i] = _(fonts[i]);
			int font = td.font;
			if(isText && ImGui::Combo(propertyLabel(L("Font")), &font, fontNames, 4))
				all([&](TextDraw &t){ t.font = font; });
			if(td.font == TD_FONT_SPRITE && !many){
				char buf[256];
				snprintf(buf, sizeof(buf), "%s", td.text.c_str());
				if(ImGui::InputText(propertyLabel(L("Sprite")), buf, sizeof(buf))){
					std::string before = td.text.substr(0, td.text.find(':'));
					td.text = buf;
					if(before != td.text.substr(0, td.text.find(':'))) td.serverTxd.clear();
					changed = true;
				}
				if(ImGui::Button(L("Browse sprites..."), ImVec2(-FLT_MIN, 0)))
					ImGui::OpenPopup("##sprite_browser");
				ImGui::SetNextWindowSize(ImVec2(ImGui::GetFontSize()*30, ImGui::GetFontSize()*24), ImGuiCond_Appearing);
				if(ImGui::BeginPopup("##sprite_browser")){
					spritePicker(td);
					ImGui::EndPopup();
				}
			}else if(isText){
				char buf[1024];
				snprintf(buf, sizeof(buf), "%s", td.text.c_str());
				if(ImGui::InputTextMultiline(propertyLabel(L("Text")), buf, sizeof(buf), ImVec2(-FLT_MIN, ImGui::GetFontSize() * 3.5f))){
					std::string text = buf;
					all([&](TextDraw &t){
						if(t.font < TD_FONT_SPRITE)
							t.text = text;
					});
				}
				ImGui::SetItemTooltip("%s", _("~n~ new line, ~r~ ~g~ ~b~ ~y~ ~p~ ~w~ ~l~ colours, ~h~ lighter, _ blank"));
			}else if(td.font == TD_FONT_PREVIEW){
				int model = td.previewModel;
				if(ImGui::InputInt(propertyLabel(L("Model")), &model))
					all([&](TextDraw &t){ t.previewModel = model; });
				if(modelBrowser(&model))
					all([&](TextDraw &t){ t.previewModel = model; });
				float rot[3] = { td.previewRotX, td.previewRotY, td.previewRotZ };
				if(ImGui::DragFloat3(propertyLabel(L("Rotation")), rot, 1.0f, -360.0f, 360.0f, "%.1f"))
					all([&](TextDraw &t){ t.previewRotX = rot[0]; t.previewRotY = rot[1]; t.previewRotZ = rot[2]; });
				float zoom = td.previewZoom;
				if(ImGui::DragFloat(propertyLabel(L("Zoom")), &zoom, 0.01f, 0.1f, 10.0f, "%.2f"))
					all([&](TextDraw &t){ t.previewZoom = zoom; });
				int cols[2] = { td.previewColour1, td.previewColour2 };
				if(ImGui::InputInt2(propertyLabel(L("Vehicle colours")), cols))
					all([&](TextDraw &t){ t.previewColour1 = cols[0]; t.previewColour2 = cols[1]; });
			}
			unsigned int colour = td.colour;
			if(colourEdit(td.font == TD_FONT_SPRITE ? L("Tint") : L("Colour"), &colour))
				all([&](TextDraw &t){ t.colour = colour; });
			if(ImGui::CollapsingHeader(L("Code settings"))){
				if(!many){
					ImGui::InputText(propertyLabel(L("Variable")), td.name, sizeof(td.name));
					if(ImGui::IsItemDeactivatedAfterEdit()) changed = true;
					ImGui::SetItemTooltip("%s", _("The Pawn variable. Empty or not a plain name makes one up."));
				}
				int scope = td.perPlayer ? 1 : 0;
				const char *scopes[] = { _("To everyone (TextDrawCreate)"), _("Per player (CreatePlayerTextDraw)") };
				if(ImGui::Combo(propertyLabel(L("Shown")), &scope, scopes, IM_ARRAYSIZE(scopes)))
					all([&](TextDraw &t){ t.perPlayer = scope == 1; });
				if(ImGui::Combo(propertyLabel(L("Type")), &font, fontNames, IM_ARRAYSIZE(fonts)))
					all([&](TextDraw &t){ t.font = font; });
				bool selectable = td.selectable;
				if(ImGui::Checkbox(L("Selectable"), &selectable))
					all([&](TextDraw &t){ t.selectable = selectable; });
				ImGui::SetItemTooltip("%s", _("Players can click it while SelectTextDraw is on (OnPlayerClickTextDraw)."));
			}
			ImGui::EndTabItem();
		}
		if(ImGui::BeginTabItem(L("Layout"))){
			float pos[2] = { td.x, td.y };
			if(ImGui::DragFloat2(propertyLabel(L("Position")), pos, 0.5f, -100.0f, 740.0f, "%.2f")){
				float dx = pos[0] - td.x, dy = pos[1] - td.y;
				for(int i : gSel)
					moveBy(tds[i], dx, dy);
				changed = true;
			}
			ImGui::SetItemTooltip("%s", _("On the 640x448 canvas every resolution is scaled to."));
			if(!many){
				float size[2] = { td.sizeX, td.sizeY };
				if(ImGui::DragFloat2(propertyLabel(L("Text size")), size, 0.5f, -1280.0f, 1280.0f, "%.2f")){
					td.sizeX = size[0]; td.sizeY = size[1];
					changed = true;
				}
				ImGui::SetItemTooltip("%s", td.font >= TD_FONT_SPRITE ? _("Width and height. A negative one mirrors a sprite.") :
					_("Left: the box's right edge. Centre: width and height are swapped (the second is the width).\n"
					"Right: the box's left edge. Text wraps there."));
			}
			if(isText){
				float letter[2] = { td.letterX, td.letterY };
				if(ImGui::DragFloat2(propertyLabel(L("Letter size")), letter, 0.005f, td.useBox ? -50.0f : 0.0f, 50.0f, "%.3f"))
					all([&](TextDraw &t){ t.letterX = letter[0]; t.letterY = letter[1]; });
				int align = td.alignment - 1;
				const char *aligns[] = { _("Left"), _("Centre"), _("Right") };
				if(ImGui::Combo(propertyLabel(L("Alignment")), &align, aligns, IM_ARRAYSIZE(aligns)))
					all([&](TextDraw &t){ t.alignment = align + 1; });
				bool proportional = td.proportional;
				if(ImGui::Checkbox(L("Proportional"), &proportional))
					all([&](TextDraw &t){ t.proportional = proportional; });
				ImGui::SetItemTooltip("%s", _("Letters take their own width instead of a fixed one."));
			}
			ImGui::EndTabItem();
		}
		if(ImGui::BeginTabItem(L("Style"))){
			if(isText){
				bool box = td.useBox;
				if(ImGui::Checkbox(L("Box"), &box))
					all([&](TextDraw &t){ t.useBox = box; });
				if(td.useBox){
					unsigned int boxColour = td.boxColour;
					if(colourEdit(L("Box colour"), &boxColour))
						all([&](TextDraw &t){ t.boxColour = boxColour; });
				}
				int shadow = td.shadow, outline = td.outline;
				if(ImGui::SliderInt(propertyLabel(L("Shadow")), &shadow, 0, 10))
					all([&](TextDraw &t){ t.shadow = shadow; });
				if(ImGui::SliderInt(propertyLabel(L("Outline")), &outline, 0, 10))
					all([&](TextDraw &t){ t.outline = outline; });
			}
			unsigned int back = td.backgroundColour;
			if(colourEdit(td.font == TD_FONT_PREVIEW ? L("Background") : L("Shadow/outline colour"), &back))
				all([&](TextDraw &t){ t.backgroundColour = back; });
			ImGui::EndTabItem();
		}
		ImGui::EndTabBar();
	}
	if(changed)
		TextDrawsChanged();
}

// ---- The file ----

static void (*gAfterClose)(void);	// runs once unsaved textdraws are dealt with
static bool gOpenUnsavedPrompt;
static bool gShowFileDialog, gFileDialogSave, gFileDialogFocus;
static char gFileDialogPath[1024];
static bool gOpenMovePrompt;
static char gMovePath[1024];

static void
afterFileChange(const char *toast)
{
	gSelected = -1;
	AddRecentTextDrawFile(GetTextDrawFilePath());
	SaveEditorSettings();
	if(toast)
		Toast(TOAST_SAVE, toast, GetTextDrawFileName());
}

static void
openFile(const char *path)
{
	std::string error;
	if(!OpenTextDrawFile(path, &error)){
		ToastFor(TOAST_SAVE, 5.0f, _("Could not open %s"), path);
		gRecent.erase(std::remove(gRecent.begin(), gRecent.end(), std::string(path)), gRecent.end());
		return;
	}
	afterFileChange(_("Opened %s"));
}

static bool
saveFile(const char *path)
{
	std::string error;
	if(!SaveTextDrawFile(path, &error)){
		ToastFor(TOAST_SAVE, 5.0f, _("Could not save the textdraws: %s"), error.c_str());
		return false;
	}
	afterFileChange(_("Saved %s"));
	if(gAfterClose){
		void (*then)(void) = gAfterClose;
		gAfterClose = nil;
		then();
	}
	return true;
}

// The file's folder, else the map's, else the workspace's gamemodes.
static std::string
suggestedPath(void)
{
	if(GetTextDrawFilePath()[0])
		return GetTextDrawFilePath();
	if(GetPawnMapPath()[0])
		return SuggestedTextDrawFilePath(GetPawnMapPath());
	std::string dir = WorkspaceDefaultDir("gamemodes");
	return (dir.empty() ? std::string(".") : dir) + "/textdraws.pwn";
}

static void
showFileDialog(bool save)
{
	gShowFileDialog = true;
	gFileDialogSave = save;
	gFileDialogFocus = true;
	snprintf(gFileDialogPath, sizeof(gFileDialogPath), "%s", save ? suggestedPath().c_str() : "");
}

static void showOpenDialog(void) { showFileDialog(false); }

static void
newFile(void)
{
	NewTextDrawFile();
	afterFileChange(nil);
	Toast(TOAST_SAVE, "%s", _("New textdraws"));
}

static void
saveOrAsk(void)
{
	if(GetTextDrawFilePath()[0])
		saveFile(nil);
	else
		showFileDialog(true);
}

void
RequestTextDrawFileClose(void (*then)(void))
{
	if(!TextDrawFileHasUnsavedChanges()){
		if(then)
			then();
		return;
	}
	gAfterClose = then;
	gOpenUnsavedPrompt = true;
}

void
OfferMoveMapTextDraws(const char *mapPath)
{
	if(GetMapTextDraws().empty())
		return;
	snprintf(gMovePath, sizeof(gMovePath), "%s", SuggestedTextDrawFilePath(mapPath).c_str());
	gOpenMovePrompt = true;
}

static void
moveMapTextDraws(void)
{
	std::string error;
	int moved = MoveMapTextDrawsToFile(GetTextDrawFilePath()[0] ? nil : gMovePath, &error);
	if(moved < 0){
		ToastFor(TOAST_SAVE, 6.0f, _("Could not move the textdraws: %s"), error.c_str());
		return;
	}
	afterFileChange(nil);
	ToastFor(TOAST_SAVE, 6.0f, _("Textdraws moved: %d, into %s. Save the map to take them out of it."),
		moved, GetTextDrawFileName());
}

static void
drawPopups(void)
{
	if(gOpenUnsavedPrompt){
		ImGui::OpenPopup(L("Unsaved textdraws"));
		gOpenUnsavedPrompt = false;
	}
	if(ImGui::BeginPopupModal(L("Unsaved textdraws"), nil, ImGuiWindowFlags_AlwaysAutoResize)){
		ImGui::Text(_("Save changes to %s?"), GetTextDrawFileName());
		ImGui::TextDisabled("%s", _("Your textdraw changes are lost if you don't save them."));
		ImGui::Spacing();
		if(ImGui::Button(L("Save"), ImVec2(110, 0))){
			ImGui::CloseCurrentPopup();
			saveOrAsk();	// continues once the file is written
		}
		ImGui::SameLine();
		if(ImGui::Button(L("Don't Save"), ImVec2(110, 0))){
			ImGui::CloseCurrentPopup();
			void (*then)(void) = gAfterClose;
			gAfterClose = nil;
			if(then)
				then();
		}
		ImGui::SameLine();
		if(ImGui::Button(L("Cancel"), ImVec2(110, 0)) || ImGui::IsKeyPressed(ImGuiKey_Escape)){
			gAfterClose = nil;
			ImGui::CloseCurrentPopup();
		}
		ImGui::EndPopup();
	}

	if(gOpenMovePrompt){
		ImGui::OpenPopup(L("Move textdraws"));
		gOpenMovePrompt = false;
	}
	if(ImGui::BeginPopupModal(L("Move textdraws"), nil, ImGuiWindowFlags_AlwaysAutoResize)){
		int count = (int)GetMapTextDraws().size();
		ImGui::Text(_("Textdraws in this map: %d."), count);
		ImGui::TextDisabled("%s", _("Textdraws now have their own file, which Test in game includes.\n"
			"Left in the map they are read-only."));
		ImGui::Spacing();
		if(GetTextDrawFilePath()[0])
			ImGui::Text(_("They go into %s."), GetTextDrawFileName());
		else{
			ImGui::SetNextItemWidth(ImGui::GetFontSize() * 30.0f);
			ImGui::InputText(L("File"), gMovePath, sizeof(gMovePath));
		}
		ImGui::Spacing();
		if(ImGui::Button(L("Move textdraws to a separate file"))){
			ImGui::CloseCurrentPopup();
			moveMapTextDraws();
		}
		ImGui::SameLine();
		if(ImGui::Button(L("Keep them read-only")) || ImGui::IsKeyPressed(ImGuiKey_Escape))
			ImGui::CloseCurrentPopup();
		ImGui::SetItemTooltip("%s", _("They stay in the map and are saved with it. TextDraws can move them later."));
		ImGui::EndPopup();
	}

	if(gShowFileDialog){
		// ###tdfile: one window for both, with the ID it has always had.
		char title[128];
		snprintf(title, sizeof(title), "%s###tdfile", gFileDialogSave ? _("Save TextDraws As") : _("Open TextDraws"));
		ImGui::SetNextWindowPos(ImGui::GetMainViewport()->GetCenter(), ImGuiCond_Appearing, ImVec2(0.5f, 0.5f));
		if(gFileDialogFocus){
			ImGui::SetNextWindowFocus();
			gFileDialogFocus = false;
		}
		if(ImGui::Begin(title, &gShowFileDialog, ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoCollapse |
			ImGuiWindowFlags_NoDocking)){
			ImGui::TextDisabled("%s", _("A .pwn or .inc file with only textdraws; your gamemode includes it."));
			ImGui::SetNextItemWidth(ImGui::GetFontSize() * 30.0f);
			ImGui::InputText(L("Path"), gFileDialogPath, sizeof(gFileDialogPath));
			ImGui::SameLine();
			if(ImGui::Button(L("Browse..."))){
				char picked[1024];
				if(PickPawnFile(picked, sizeof(picked), gFileDialogSave, gFileDialogPath[0] ? gFileDialogPath : nil))
					snprintf(gFileDialogPath, sizeof(gFileDialogPath), "%s", picked);
			}
			if(ImGui::Button(gFileDialogSave ? L("Save") : L("Open"), ImVec2(110, 0)) && gFileDialogPath[0]){
				gShowFileDialog = false;
				if(gFileDialogSave)
					saveFile(gFileDialogPath);
				else
					openFile(gFileDialogPath);
			}
			ImGui::SameLine();
			if(ImGui::Button(L("Cancel"), ImVec2(110, 0))){
				gShowFileDialog = false;
				gAfterClose = nil;
			}
		}
		ImGui::End();
	}
}


// ---- Shapes ----

// The shape's settings, for a new one or the selected one. Returns true
// when one changed.
static bool
shapeFields(ShapeSettings &sh, bool placed)
{
	bool changed = false;
	const char *names[NUM_SHAPES];
	int at = 0;
	for(int k = 0; k < NUM_SHAPES; k++){
		names[k] = _(gShapeNames[gShapeOrder[k]]);
		if(gShapeOrder[k] == sh.kind)
			at = k;
	}
	ImGui::PushItemWidth(-FLT_MIN);
	if(ImGui::Combo(propertyLabel(L("Shape")), &at, names, NUM_SHAPES)){
		shapeDefaults(sh, gShapeOrder[at]);
		changed = true;
	}
	if(placed){
		float pos[2] = { sh.x, sh.y };
		if(ImGui::DragFloat2(propertyLabel(L("Position")), pos, 0.5f, -640.0f, 1280.0f, "%.1f")){
			sh.x = pos[0]; sh.y = pos[1];
			changed = true;
		}
	}
	float size[2] = { sh.w, sh.h };
	if(ImGui::DragFloat2(propertyLabel(L("Size")), size, 0.5f, 1.0f, 640.0f, "%.1f")){
		sh.w = std::max(1.0f, size[0]);
		sh.h = std::max(1.0f, size[1]);
		changed = true;
	}
	ImGui::SetItemTooltip("%s", _("Or drag its handles on the canvas: Shift keeps the shape's proportions,\n"
		"Alt sizes it from the centre, Ctrl stops snapping."));
	int k = sh.kind;
	if(k == SHAPE_ROUNDED_BOX)
		changed |= ImGui::DragFloat(propertyLabel(L("Corner radius")), &sh.radius, 0.25f, 0.0f, 200.0f, "%.1f");
	if(k == SHAPE_ROUNDED_BOX || k == SHAPE_PILL || k == SHAPE_CIRCLE)
		changed |= ImGui::DragFloat(propertyLabel(L("Border")), &sh.thickness, 0.1f, 0.0f, 100.0f, "%.1f");
	if(k == SHAPE_OUTLINE)
		changed |= ImGui::DragFloat(propertyLabel(L("Line width")), &sh.thickness, 0.1f, 0.5f, 100.0f, "%.1f");
	if(k == SHAPE_PLUS)
		changed |= ImGui::DragFloat(propertyLabel(L("Arm width")), &sh.thickness, 0.1f, 0.5f, 200.0f, "%.1f");
	if(k == SHAPE_RING){
		changed |= ImGui::DragFloat(propertyLabel(L("Dot size")), &sh.thickness, 0.25f, 1.0f, 60.0f, "%.1f");
		changed |= ImGui::SliderInt(propertyLabel(L("Dots")), &sh.segments, 3, 120);
		ImGui::SetItemTooltip("%s", _("Each dot is a textdraw: SA-MP allows 2048 global and 256 per player."));
	}
	if(k == SHAPE_BAR){
		changed |= ImGui::SliderInt(propertyLabel(L("Segments")), &sh.segments, 1, 40);
		ImGui::SetItemTooltip("%s", _("1 for a solid bar. Each segment is a textdraw."));
		if(sh.segments > 1)
			changed |= ImGui::DragFloat(propertyLabel(L("Gap")), &sh.thickness, 0.1f, 0.0f, 50.0f, "%.1f");
	}
	if(k == SHAPE_GRADIENT){
		changed |= ImGui::SliderInt(propertyLabel(L("Bands")), &sh.segments, 2, 128);
		ImGui::SetItemTooltip("%s", _("More bands blend more smoothly. Each band is a textdraw."));
	}
	if(k == SHAPE_TRIANGLE){
		changed |= ImGui::SliderInt(propertyLabel(L("Steps")), &sh.segments, 1, 64);
		ImGui::SetItemTooltip("%s", _("Textdraws can't be turned, so a triangle is boxes in steps.\n"
			"More steps make a smoother edge. Each step is a textdraw."));
	}
	if(k == SHAPE_BAR || k == SHAPE_GRADIENT || k == SHAPE_TRIANGLE || k == SHAPE_ARROW){
		const char *dirs[4];
		if(k == SHAPE_BAR){
			dirs[0] = _("Fills to the right"); dirs[1] = _("Fills to the left");
			dirs[2] = _("Fills up"); dirs[3] = _("Fills down");
		}else if(k == SHAPE_GRADIENT){
			dirs[0] = _("Left to right"); dirs[1] = _("Right to left");
			dirs[2] = _("Bottom to top"); dirs[3] = _("Top to bottom");
		}else{
			dirs[0] = _("Points right"); dirs[1] = _("Points left");
			dirs[2] = _("Points up"); dirs[3] = _("Points down");
		}
		changed |= ImGui::Combo(propertyLabel(L("Direction")), &sh.dir, dirs, 4);
	}
	if(k == SHAPE_BAR || k == SHAPE_RING)
		changed |= ImGui::SliderInt(propertyLabel(L("Value")), &sh.value, 0, 100, "%d%%");
	ImGui::PopItemWidth();
	changed |= colourEdit(k == SHAPE_GRADIENT ? L("From") : L("Colour"), &sh.colour);
	if(shapeOpaque(sh))
		ImGui::SetItemTooltip("%s", _("Always opaque: its sprites overlap, and see-through colours would\n"
			"show the overlaps darker."));
	if(k == SHAPE_BAR || k == SHAPE_RING)
		changed |= colourEdit(L("Empty part"), &sh.back);
	if(k == SHAPE_GRADIENT)
		changed |= colourEdit(L("To"), &sh.back);
	if(k == SHAPE_OUTLINE){
		changed |= colourEdit(L("Inside"), &sh.back);
		ImGui::SetItemTooltip("%s", _("Fully transparent leaves it out."));
	}
	if((k == SHAPE_ROUNDED_BOX || k == SHAPE_PILL || k == SHAPE_CIRCLE) && sh.thickness > 0.0f)
		changed |= colourEdit(L("Border colour"), &sh.back);
	ImGui::SetNextItemWidth(-FLT_MIN);
	changed |= ImGui::DragFloat(propertyLabel(L("Shadow")), &sh.shadow, 0.1f, -20.0f, 20.0f, "%.1f");
	ImGui::SetItemTooltip("%s", _("A copy of the shape under it, this far down and right. 0 for none."));
	if(sh.shadow != 0.0f)
		changed |= colourEdit(L("Shadow colour"), &sh.shadowColour);
	return changed;
}

static void
addShape(void)
{
	ShapeSettings sh = gShape;
	sh.x = 320.0f - sh.w * 0.5f;
	sh.y = 224.0f - sh.h * 0.5f;
	std::vector<TextDraw> parts;
	buildShape(sh, parts);
	if(parts.empty())
		return;
	std::string group = uniqueGroupName(_(gShapeNames[sh.kind]));
	std::string spec = shapeSpec(sh);
	std::vector<TextDraw> &tds = GetTextDraws();
	selectOnly(-1);
	for(TextDraw &td : parts){
		td.group = group;
		td.shape = spec;
		tds.push_back(td);
		addToSelection((int)tds.size() - 1);
	}
	gSelected = gSel.front();
	TextDrawsChanged();
}

static void
shapeButton(float width)
{
	if(ImGui::Button(LI(ICON_FA_PLUS, "Shape##add"), ImVec2(width, ImGui::GetFrameHeight()*1.25f)))
		ImGui::OpenPopup("##shape");
	ImGui::SetItemTooltip("%s", _("Boxes, circles, outlines, triangles, arrows, gradients, bars and rings, made\n"
		"of sprite textdraws and added as a group. Select it later to change it."));
	if(ImGui::BeginPopup("##shape")){
		shapeFields(gShape, false);
		if(ImGui::Button(LI(ICON_FA_PLUS, "Add the shape"))){
			addShape();
			ImGui::CloseCurrentPopup();
		}
		ImGui::EndPopup();
	}
}

// With a whole shape selected: its settings, changed in place.
static bool
shapePanel(void)
{
	ShapeSettings sh;
	std::string group;
	if(!selectedShape(&sh, &group))
		return false;
	ImGui::SeparatorText(_(gShapeNames[sh.kind]));
	if(shapeFields(sh, true))
		replaceShape(group, sh);
	if(ImGui::SmallButton(L("Edit the parts on their own"))){
		std::vector<TextDraw> &tds = GetTextDraws();
		for(int i : gSel)
			tds[i].shape.clear();
		TextDrawsChanged();
	}
	ImGui::SetItemTooltip("%s", _("Makes it plain sprites, to fine-tune one by one. It is no longer\n"
		"changed as a shape, so changes here would undo that tuning."));
	return true;
}

// ---- Mode ----

// An icon button with a tooltip; label is "ICON###English id".
static bool
iconButton(const char *label, const char *tooltip, bool active = false)
{
	if(active)
		ImGui::PushStyleColor(ImGuiCol_Button, ImGui::GetStyleColorVec4(ImGuiCol_ButtonActive));
	bool pressed = ImGui::Button(label);
	if(active)
		ImGui::PopStyleColor();
	ImGui::SetItemTooltip("%s", tooltip);
	return pressed;
}

// The artboards as tabs: click to open one, right-click to rename,
// duplicate or delete it, + to add one.
static void
boardTabs(void)
{
	std::vector<std::string> boards;
	listBoards(boards);
	static std::string renaming;
	static char renameBuf[64];
	bool openRename = false;
	static std::string shownBoard = "\x01";	// forces the tab bar onto gBoard after a switch elsewhere
	if(ImGui::BeginTabBar("##boards", ImGuiTabBarFlags_FittingPolicyScroll | ImGuiTabBarFlags_NoTooltip)){
		for(size_t b = 0; b < boards.size(); b++){
			char label[96];
			snprintf(label, sizeof(label), "%s###board%s", boardLabel(boards[b]), boards[b].c_str());
			ImGuiTabItemFlags flags = boards[b] == gBoard && shownBoard != gBoard ? ImGuiTabItemFlags_SetSelected : 0;
			bool open = ImGui::BeginTabItem(label, nil, flags);
			if(ImGui::BeginPopupContextItem()){
				if(ImGui::MenuItem(L("Rename..."))){
					renaming = boards[b];
					snprintf(renameBuf, sizeof(renameBuf), "%s", boardLabel(boards[b]));
					openRename = true;
				}
				if(ImGui::MenuItem(L("Duplicate")))
					duplicateBoard(boards[b]);
				ImGui::BeginDisabled(boards.size() < 2);
				if(ImGui::MenuItem(L("Delete")))
					deleteBoard(boards[b]);
				ImGui::EndDisabled();
				ImGui::EndPopup();
			}
			ImGui::SetItemTooltip("%s", _("An artboard: a screen of its own, shown and hidden in Pawn with\n"
				"<prefix>_Show<Board>(playerid) and _Hide<Board>(playerid). Right-click for more."));
			if(open){
				if(shownBoard == gBoard && boards[b] != gBoard)
					switchBoard(boards[b]);
				ImGui::EndTabItem();
			}
		}
		shownBoard = gBoard;
		if(ImGui::TabItemButton(ICON_FA_PLUS "###add_board", ImGuiTabItemFlags_Trailing)){
			std::string name = newBoardName(_("Board"));
			gEmptyBoards.push_back(name);
			switchBoard(name);
		}
		ImGui::SetItemTooltip("%s", _("New artboard"));
		ImGui::EndTabBar();
	}
	if(openRename)
		ImGui::OpenPopup("##rename_board");
	if(ImGui::BeginPopup("##rename_board")){
		if(ImGui::IsWindowAppearing())
			ImGui::SetKeyboardFocusHere();
		bool enter = ImGui::InputText("##name", renameBuf, sizeof(renameBuf), ImGuiInputTextFlags_EnterReturnsTrue);
		ImGui::SameLine();
		if((ImGui::Button(L("Rename")) || enter) && renameBuf[0]){
			std::string to = renameBuf;
			if(to == _("Main") && renaming.empty())
				to.clear();
			renameBoard(renaming, to);
			ImGui::CloseCurrentPopup();
		}
		ImGui::EndPopup();
	}
}


// Places to take the still frame at, Textdraw Studio's: a player's spot and
// facing angle; the camera stands behind and above them.
static const struct { const char *name; float x, y, z, angle; } gStillPlaces[] = {
	{ N_("Airport"), 1923.5066f, -2547.8245f, 14.0469f, 90.0f },
	{ N_("Road"), 1714.6931f, -1340.4772f, 13.3828f, 180.1136f },
	{ N_("Grove Street"), 2450.7434f, -1658.5571f, 13.3047f, 258.7504f },
	{ N_("Santa Maria Pier"), 370.1010f, -2032.7593f, 7.6719f, 0.7464f },
	{ N_("Rooftop"), 1547.9363f, -1352.2819f, 329.4693f, 15.2985f },
	{ N_("Beach"), 482.6832f, -1857.9556f, 3.6218f, 248.6140f },
};

static void
takeStillAt(int place, int hour)
{
	if(place >= 0){
		const auto &p = gStillPlaces[place];
		float a = p.angle * 3.14159265f / 180.0f;
		rw::V3d forward = { -sinf(a), cosf(a), 0.0f };	// SA-MP's facing angle, 0 is north
		// Set as Namaka's camera poses are (setTarget would pan the position too).
		rw::V3d up = { 0.0f, 0.0f, 1.0f };
		TheCamera.m_position = { p.x - forward.x * 4.0f, p.y - forward.y * 4.0f, p.z + 1.4f };
		TheCamera.m_target = { p.x + forward.x * 6.0f, p.y + forward.y * 6.0f, p.z + 0.6f };
		TheCamera.m_at = rw::normalize(rw::sub(TheCamera.m_target, TheCamera.m_position));
		TheCamera.m_up = up;
		TheCamera.m_localup = up;
		// The places are all outside: in an interior there would be nothing
		// round the camera (it looked like being under the ground).
		currentArea = 0;
	}
	if(hour >= 0){
		currentHour = hour;
		currentMinute = 0;
	}
	gBackground = TD_BACKGROUND_STILL;
	releaseStillFrame();
	gStillWait = 90;	// a second and a half of streaming
}

static void
stillFramePlaces(void)
{
	if(ImGui::Button(LI(ICON_FA_LOCATION_DOT, "Place")))
		ImGui::OpenPopup("##still_places");
	ImGui::SetItemTooltip("%s", _("Take the still frame somewhere else or at another hour.\n"
		"The map's own camera moves there too."));
	if(ImGui::BeginPopup("##still_places")){
		static int hour = 12;
		ImGui::SetNextItemWidth(ImGui::GetFontSize() * 8.0f);
		ImGui::SliderInt(L("Hour"), &hour, 0, 23, "%02d:00");
		if(ImGui::IsItemDeactivatedAfterEdit())
			takeStillAt(-1, hour);	// here, at the new hour
		ImGui::SeparatorText(_("Places"));
		for(int i = 0; i < IM_ARRAYSIZE(gStillPlaces); i++)
			if(ImGui::MenuItem(_(gStillPlaces[i].name)))
				takeStillAt(i, hour);
		ImGui::Separator();
		if(ImGui::MenuItem(L("Here, at this hour")))
			takeStillAt(-1, hour);
		ImGui::EndPopup();
	}
	if(gStillWait > 0){
		ImGui::SameLine();
		ImGui::TextDisabled("%s", _("Taking it..."));
	}
}

// Above the canvas: the mode and file, the artboards, then how the canvas shows.
// ---- Guides ----

static void
addGuide(bool vertical, float at)
{
	std::vector<TextDrawGuide> &guides = GetTextDrawGuides();
	for(const TextDrawGuide &g : guides)
		if(g.board == gBoard && g.vertical == vertical && fabsf(g.at - at) < 0.01f)
			return;	// already one there
	TextDrawGuide g = { gBoard, vertical, at };
	guides.push_back(g);
}

static void
clearGuides(void)
{
	std::vector<TextDrawGuide> &guides = GetTextDrawGuides();
	for(int k = (int)guides.size() - 1; k >= 0; k--)
		if(guides[k].board == gBoard)
			guides.erase(guides.begin() + k);
}

// Photoshop's New Guide Layout: columns and rows (a width or height of 0
// shares the room out), each with a gutter, inside margins.
struct GuideLayout {
	bool columns, rows, margins, clear;
	int numColumns, numRows;
	float columnWidth, columnGutter, rowHeight, rowGutter;
	float top, left, bottom, right;
};
static GuideLayout gLayout = { true, false, false, true, 4, 3, 0.0f, 10.0f, 0.0f, 10.0f, 20.0f, 20.0f, 20.0f, 20.0f };

// Edges along one side: from a to b, n cells of size (0: fill) apart by gutter.
static void
layoutGuides(bool vertical, float a, float b, int n, float size, float gutter)
{
	n = std::max(1, n);
	if(size <= 0.0f)
		size = (b - a - gutter * (n - 1)) / n;
	for(int k = 0; k < n; k++){
		float from = a + k * (size + gutter);
		addGuide(vertical, from);
		addGuide(vertical, from + size);
	}
}

static void
applyGuideLayout(const GuideLayout &l)
{
	if(l.clear)
		clearGuides();
	float x0 = 0.0f, y0 = 0.0f, x1 = 640.0f, y1 = 448.0f;
	if(l.margins){
		x0 = l.left; x1 = 640.0f - l.right;
		y0 = l.top; y1 = 448.0f - l.bottom;
		addGuide(true, x0); addGuide(true, x1);
		addGuide(false, y0); addGuide(false, y1);
	}
	if(l.columns)
		layoutGuides(true, x0, x1, l.numColumns, l.columnWidth, l.columnGutter);
	if(l.rows)
		layoutGuides(false, y0, y1, l.numRows, l.rowHeight, l.rowGutter);
	TextDrawGuidesChanged();
}

static void
guidesButton(void)
{
	if(ImGui::SmallButton(ICON_FA_GEAR "##guides"))
		ImGui::OpenPopup("##guides_popup");
	ImGui::SetItemTooltip("%s", _("Guides: new ones at exact places, presets and layouts of columns and rows,\n"
		"as in Photoshop's New Guide Layout."));
	if(!ImGui::BeginPopup("##guides_popup"))
		return;
	std::vector<TextDrawGuide> &guides = GetTextDrawGuides();
	float w = ImGui::GetFontSize() * 6.0f;
	bool changed = false;

	ImGui::SeparatorText(_("New guide"));
	static int vertical = 1;
	static float at = 320.0f;
	ImGui::RadioButton(L("Vertical"), &vertical, 1);
	ImGui::SameLine();
	ImGui::RadioButton(L("Horizontal"), &vertical, 0);
	ImGui::SetNextItemWidth(w);
	ImGui::InputFloat(L("Position"), &at, 0.0f, 0.0f, "%.1f");
	ImGui::SameLine();
	if(ImGui::Button(LI(ICON_FA_PLUS, "Add##guide"))){
		addGuide(vertical != 0, at);
		changed = true;
	}
	ImGui::SetItemTooltip("%s", _("In canvas units: 0 to 640 across, 0 to 448 down."));

	ImGui::SeparatorText(_("Presets"));
	struct Preset { const char *name; int columns, rows; float margin; bool centre; };
	static const Preset presets[] = {
		{ N_("Centre lines"), 0, 0, 0.0f, true },
		{ N_("Thirds"), 3, 3, 0.0f, false },
		{ N_("Safe area (5%)"), 0, 0, -1.0f, false },
		{ N_("2 columns"), 2, 0, 20.0f, false },
		{ N_("3 columns"), 3, 0, 20.0f, false },
		{ N_("4 columns"), 4, 0, 20.0f, false },
		{ N_("6 columns"), 6, 0, 20.0f, false },
		{ N_("12 columns"), 12, 0, 20.0f, false },
		{ N_("3 x 3 grid"), 3, 3, 20.0f, false },
	};
	for(int k = 0; k < IM_ARRAYSIZE(presets); k++){
		const Preset &pr = presets[k];
		if(k % 3)
			ImGui::SameLine();
		if(ImGui::Button(_(pr.name), ImVec2(ImGui::GetFontSize() * 8.0f, 0.0f))){
			if(gLayout.clear)
				clearGuides();
			if(pr.centre){
				addGuide(true, 320.0f);
				addGuide(false, 224.0f);
			}else if(pr.margin < 0.0f){
				addGuide(true, 32.0f); addGuide(true, 608.0f);
				addGuide(false, 22.4f); addGuide(false, 425.6f);
			}else{
				GuideLayout l = gLayout;
				l.clear = false;
				l.columns = pr.columns > 0; l.numColumns = pr.columns;
				l.rows = pr.rows > 0; l.numRows = pr.rows;
				l.columnWidth = l.rowHeight = 0.0f;
				l.columnGutter = l.rowGutter = pr.margin > 0.0f ? 10.0f : 0.0f;
				l.margins = pr.margin > 0.0f;
				l.top = l.left = l.bottom = l.right = pr.margin;
				applyGuideLayout(l);
			}
			changed = true;
		}
	}

	ImGui::SeparatorText(_("New guide layout"));
	ImGui::PushItemWidth(w);
	ImGui::Checkbox(L("Columns"), &gLayout.columns);
	ImGui::BeginDisabled(!gLayout.columns);
	ImGui::InputInt(L("Number##columns"), &gLayout.numColumns, 1, 1);
	ImGui::InputFloat(L("Width##columns"), &gLayout.columnWidth, 0.0f, 0.0f, "%.1f");
	ImGui::SetItemTooltip("%s", _("0 shares the room out between them."));
	ImGui::InputFloat(L("Gutter##columns"), &gLayout.columnGutter, 0.0f, 0.0f, "%.1f");
	ImGui::EndDisabled();
	ImGui::Checkbox(L("Rows"), &gLayout.rows);
	ImGui::BeginDisabled(!gLayout.rows);
	ImGui::InputInt(L("Number##rows"), &gLayout.numRows, 1, 1);
	ImGui::InputFloat(L("Height##rows"), &gLayout.rowHeight, 0.0f, 0.0f, "%.1f");
	ImGui::SetItemTooltip("%s", _("0 shares the room out between them."));
	ImGui::InputFloat(L("Gutter##rows"), &gLayout.rowGutter, 0.0f, 0.0f, "%.1f");
	ImGui::EndDisabled();
	ImGui::Checkbox(L("Margins"), &gLayout.margins);
	ImGui::BeginDisabled(!gLayout.margins);
	float tb[2] = { gLayout.top, gLayout.bottom }, lr[2] = { gLayout.left, gLayout.right };
	ImGui::SetNextItemWidth(w * 1.5f);
	if(ImGui::InputFloat2(L("Top, bottom"), tb, "%.1f")){ gLayout.top = tb[0]; gLayout.bottom = tb[1]; }
	ImGui::SetNextItemWidth(w * 1.5f);
	if(ImGui::InputFloat2(L("Left, right"), lr, "%.1f")){ gLayout.left = lr[0]; gLayout.right = lr[1]; }
	ImGui::EndDisabled();
	ImGui::PopItemWidth();
	gLayout.numColumns = std::max(1, std::min(gLayout.numColumns, 64));
	gLayout.numRows = std::max(1, std::min(gLayout.numRows, 64));
	ImGui::Checkbox(L("Clear existing guides"), &gLayout.clear);
	ImGui::SetItemTooltip("%s", _("For the layout and the presets: the board's guides go first."));
	if(ImGui::Button(L("Make the layout"))){
		applyGuideLayout(gLayout);
		changed = true;
	}

	// This board's guides, to type exact places.
	int count = 0;
	for(const TextDrawGuide &g : guides)
		count += g.board == gBoard;
	char title[64];
	snprintf(title, sizeof(title), _("Guides on this board: %d"), count);
	ImGui::SeparatorText(title);
	if(ImGui::Checkbox(L("Lock guides"), &gGuidesLocked))
		SaveEditorSettings();
	ImGui::SetItemTooltip("%s", _("They can't be dragged on the canvas. New ones can still be made."));
	ImGui::SameLine();
	ImGui::BeginDisabled(count == 0);
	if(ImGui::Button(L("Clear guides"))){
		clearGuides();
		changed = true;
	}
	ImGui::EndDisabled();
	if(count > 0){
		ImGui::BeginChild("##guide_list", ImVec2(ImGui::GetFontSize() * 10.0f,
			std::min(count, 8) * ImGui::GetFrameHeightWithSpacing() + 4.0f));
		int remove = -1;
		for(int k = 0; k < (int)guides.size(); k++){
			TextDrawGuide &g = guides[k];
			if(g.board != gBoard)
				continue;
			ImGui::PushID(k);
			ImGui::AlignTextToFramePadding();
			ImGui::TextUnformatted(g.vertical ? "x" : "y");
			ImGui::SameLine();
			ImGui::SetNextItemWidth(w);
			if(ImGui::InputFloat("##at", &g.at, 0.0f, 0.0f, "%.1f"))
				changed = true;
			ImGui::SameLine();
			if(ImGui::SmallButton(ICON_FA_TRASH "##remove"))
				remove = k;
			ImGui::PopID();
		}
		ImGui::EndChild();
		if(remove >= 0){
			guides.erase(guides.begin() + remove);
			changed = true;
		}
	}
	if(changed)
		TextDrawGuidesChanged();
	ImGui::EndPopup();
}

// Shares the workspace bar so the canvas gets all the remaining height.
void
DrawTextDrawsToolbar(void)
{
	float controls = ImGui::GetFrameHeight() * 3.0f + ImGui::GetFontSize() * 10.0f +
		ImGui::GetStyle().ItemSpacing.x * 6.0f;
	float boards = ImGui::GetContentRegionAvail().x - controls;
	if(boards > ImGui::GetFontSize() * 5.0f){
		ImGui::BeginChild("##boards_area", ImVec2(boards, ImGui::GetFrameHeight()), ImGuiChildFlags_None,
			ImGuiWindowFlags_NoScrollbar);
		boardTabs();
		ImGui::EndChild();
	}else{
		if(ImGui::SmallButton(ICON_FA_LAYER_GROUP "##boards_menu"))
			ImGui::OpenPopup("##boards_popup");
		ImGui::SetItemTooltip("%s", boardLabel(gBoard));
		ImGui::SetNextWindowSize(ImVec2(ImGui::GetFontSize() * 16.0f, 0), ImGuiCond_Appearing);
		if(ImGui::BeginPopup("##boards_popup")){
			boardTabs();
			ImGui::EndPopup();
		}
	}

	if(ImGui::SmallButton("-##zoom")) gZoom = std::max(0.25f, gZoom / 1.25f);
	ImGui::Text("%.0f%%", gZoom * 100.0f);
	ImGui::SetItemTooltip("%s", _("The wheel zooms round the cursor, the middle button pans.\nCtrl+wheel sizes the interface instead."));
	if(ImGui::SmallButton("+##zoom")) gZoom = std::min(MAX_ZOOM, gZoom * 1.25f);
	if(ImGui::SmallButton(L("Fit"))){ gZoom = 1.0f; gPanX = gPanY = 0.0f; }
	if(ImGui::Button(LI(ICON_FA_GEAR, "Canvas")))
		ImGui::OpenPopup("##canvas_settings");
	ImGui::SetNextWindowSize(ImVec2(ImGui::GetFontSize() * 24.0f, 0), ImGuiCond_Appearing);
	if(ImGui::BeginPopup("##canvas_settings")){
		ImGui::SetNextItemWidth(-FLT_MIN);
		const char *screens[NUM_TD_SCREENS];
		for(int i = 0; i < NUM_TD_SCREENS; i++)
			screens[i] = i == TD_SCREEN_WINDOW ? _(gScreenNames[i]) : gScreenNames[i];
		if(ImGui::Combo("##screen", &gScreen, screens, NUM_TD_SCREENS)){
			gZoom = 1.0f; gPanX = gPanY = 0.0f;
			SaveEditorSettings();
		}
		ImGui::SetItemTooltip("%s", _("The player's screen shape. Textdraws stretch with it; the chat doesn't."));
		if(ImGui::Checkbox(L("Compare"), &gCompare))
			SaveEditorSettings();
		ImGui::SetItemTooltip("%s", _("Open movable previews at 4:3, 16:9 and 21:9. Dock them beside the canvas to compare."));
		if(ImGui::Checkbox(L("Pixel-exact"), &gPixelExact))
			SaveEditorSettings();
		ImGui::SetItemTooltip("%s", _("Draws the screen at the player's resolution and scales that to the canvas, so\n"
			"seams and gaps between textdraws show as they do in game. Zoom in to see\n"
			"single pixels."));
		if(gPixelExact){
			char res[NUM_HUD_RESOLUTIONS][24];
			const char *resNames[NUM_HUD_RESOLUTIONS];
			for(int i = 0; i < NUM_HUD_RESOLUTIONS; i++){
				snprintf(res[i], sizeof(res[i]), "%d x %d", gHudResolutions[i].w, gHudResolutions[i].h);
				resNames[i] = res[i];
			}
			ImGui::SetNextItemWidth(-FLT_MIN);
			if(ImGui::Combo("##exact_res", &gHud.resolution, resNames, NUM_HUD_RESOLUTIONS))
				SaveEditorSettings();
			ImGui::SetItemTooltip("%s", _("The player's screen (the same one the chat and outlines use)."));
		}
		int background = gBackground;
		ImGui::SetNextItemWidth(-FLT_MIN);
		const char *backgrounds[] = { _("Plain"), _("Still frame") };
		if(ImGui::Combo("##background", &background, backgrounds, IM_ARRAYSIZE(backgrounds))){
			gBackground = background;
			if(gBackground == TD_BACKGROUND_STILL && gStillFrame == nil)
				gStillPending = true;	// taken from the world on the next frame
			SaveEditorSettings();
		}
		ImGui::SetItemTooltip("%s", _("Still frame: the world as the camera saw it when TextDraw mode started."));
		if(gBackground == TD_BACKGROUND_STILL){
			stillFramePlaces();
		}
		if(gBackground == TD_BACKGROUND_PLAIN){
			float c[4] = { (gCanvasColour >> 24 & 0xFF) / 255.0f, (gCanvasColour >> 16 & 0xFF) / 255.0f,
				(gCanvasColour >> 8 & 0xFF) / 255.0f, 1.0f };
			if(ImGui::ColorEdit3("##canvas", c, ImGuiColorEditFlags_NoInputs)){
				gCanvasColour = (unsigned int)(c[0] * 255.0f + 0.5f) << 24 | (unsigned int)(c[1] * 255.0f + 0.5f) << 16 |
					(unsigned int)(c[2] * 255.0f + 0.5f) << 8 | 0xFF;
				SaveEditorSettings();
			}
		}
		hudSettings();
		if(ImGui::Checkbox(L("Grid"), &gGrid))
			SaveEditorSettings();
		if(gGrid){
			ImGui::SetNextItemWidth(ImGui::GetFontSize() * 3.0f);
			if(ImGui::DragFloat("##grid", &gGridSize, 0.5f, 1.0f, 200.0f, "%.0f"))
				gGridSize = std::max(1.0f, gGridSize);
			if(ImGui::IsItemDeactivatedAfterEdit())
				SaveEditorSettings();
			ImGui::SetItemTooltip("%s", _("Grid spacing, in canvas units (640 x 448)"));
		}
		if(ImGui::Checkbox(L("Snap"), &gSnap))
			SaveEditorSettings();
		ImGui::SetItemTooltip("%s", _("Dragged textdraws snap to the screen's edges and centre, to the others,\n"
			"to the grid and to guides. Hold Ctrl while dragging to place freely."));
		if(ImGui::Checkbox(L("Rulers"), &gRulers))
			SaveEditorSettings();
		ImGui::SetItemTooltip("%s", _("Rulers along the canvas, in canvas units. Drag from one to make a guide,\n"
			"and drag a guide back onto a ruler to remove it. Guides are saved with the file."));
		if(gRulers){
			guidesButton();
		}
		ImGui::SetNextItemWidth(ImGui::GetFontSize() * 3.0f);
		if(ImGui::DragFloat("##nudge", &gNudge, 0.05f, 0.01f, 100.0f, ICON_FA_ARROWS_UP_DOWN_LEFT_RIGHT " %.2f"))
			gNudge = std::max(0.01f, gNudge);
		if(ImGui::IsItemDeactivatedAfterEdit())
			SaveEditorSettings();
		ImGui::SetItemTooltip("%s", _("How far the arrow keys move the selection; Shift moves ten times as far."));
		if(ImGui::Checkbox(L("Click through locked"), &gClickThroughLocked))
			SaveEditorSettings();
		ImGui::SetItemTooltip("%s", _("Clicks on a locked textdraw go to what is under it.\n"
			"Alt+click picks the next one down under the cursor."));
		ImGui::EndPopup();
	}
}

bool
TextDrawModeShortcut(int action)
{
	if(!gMode)
		return false;
	std::vector<TextDraw> &tds = GetTextDraws();
	switch(action){
	case SC_UNDO:
		if(UndoTextDraws())
			Toast(TOAST_UNDO_REDO, "%s", _("Undo"));
		return true;
	case SC_REDO:
		if(RedoTextDraws())
			Toast(TOAST_UNDO_REDO, "%s", _("Redo"));
		return true;
	case SC_NEW_MAP: RequestTextDrawFileClose(newFile); return true;
	case SC_OPEN_MAP: RequestTextDrawFileClose(showOpenDialog); return true;
	case SC_SAVE_MAP: saveOrAsk(); return true;
	case SC_SAVE_MAP_AS: showFileDialog(true); return true;
	case SC_CANCEL:
		if(!gIsolatedGroup.empty()){
			std::string group = gIsolatedGroup;
			gIsolatedGroup.clear();
			selectGroup(group, false);
		}else if(!gSel.empty())
			selectOnly(-1);
		else
			ExitTextDrawMode();
		return true;
	case SC_DELETE:
		if(!gSel.empty())
			deleteSelected();
		return true;
	case SC_DUPLICATE:
		if(!gSel.empty())
			appendCopies(selectedCopies(), 10.0f);
		return true;
	case SC_COPY:
		if(!gSel.empty()){
			gClipboard = selectedCopies();
			Toast(TOAST_COPY_PASTE, _("Copied %d textdraws"), (int)gClipboard.size());
		}
		return true;
	case SC_CUT:
		if(!gSel.empty()){
			gClipboard = selectedCopies();
			deleteSelected();
		}
		return true;
	case SC_PASTE:
		if(!gClipboard.empty())
			appendCopies(gClipboard, 10.0f);
		return true;
	case SC_PASTE_IN_PLACE:
		if(!gClipboard.empty())
			appendCopies(gClipboard, 0.0f);
		return true;
	case SC_GROUP_SELECTION:
		if(!gSel.empty())
			groupSelected(newGroupName().c_str());
		return true;
	case SC_HIDE_SELECTION:
		for(int i : gSel)
			tds[i].hidden = true;
		if(!gSel.empty())
			TextDrawsChanged();
		selectOnly(-1);
		return true;
	case SC_SHOW_ALL:
		for(TextDraw &td : tds)
			if(onBoard(td))
				td.hidden = false;
		TextDrawsChanged();
		return true;
	// The rest works as usual: testing, windows, the interface size.
	case SC_TEST_IN_GAME:
	case SC_UI_SCALE_UP: case SC_UI_SCALE_DOWN: case SC_UI_SCALE_RESET:
	case SC_SCROLL_UP: case SC_SCROLL_DOWN:
		return false;
	}
	return action < SC_WINDOW_BROWSER;	// map editing is off
}

static void
fileBar(void)
{
	const char *icons[] = { ICON_FA_FILE "###New", ICON_FA_FOLDER_OPEN "###Open...",
		ICON_FA_FLOPPY_DISK "###Save", ICON_FA_ROTATE_LEFT "###Undo", ICON_FA_ROTATE_RIGHT "###Redo",
		"...###File options" };
	const char *tips[] = { _("New textdraw file"), _("Open a textdraw file"), ShortcutLabel(SC_SAVE_MAP),
		ShortcutLabel(SC_UNDO), ShortcutLabel(SC_REDO), _("File options") };
	float gap = ImGui::GetStyle().ItemSpacing.x;
	float width = std::max(1.0f, (ImGui::GetContentRegionAvail().x - gap * 5.0f) / 6.0f);
	for(int i = 0; i < 6; i++){
		if(i) ImGui::SameLine();
		if(ImGui::Button(icons[i], ImVec2(width, 0))){
			switch(i){
			case 0: RequestTextDrawFileClose(newFile); break;
			case 1: RequestTextDrawFileClose(showOpenDialog); break;
			case 2: saveOrAsk(); break;
			case 3: UndoTextDraws(); break;
			case 4: RedoTextDraws(); break;
			case 5: ImGui::OpenPopup("##file_options"); break;
			}
		}
		ImGui::SetItemTooltip("%s", tips[i]);
	}
	if(ImGui::BeginPopup("##file_options")){
		if(ImGui::MenuItem(L("Save As..."))) showFileDialog(true);
		if(ImGui::BeginMenu(L("Recent textdraw files"), !gRecent.empty())){
			static std::string chosen;
			for(size_t i = 0; i < gRecent.size(); i++){
				ImGui::PushID((int)i);
				const char *name = gRecent[i].c_str() + gRecent[i].find_last_of("/\\") + 1;
				if(ImGui::MenuItem(name)){
					chosen = gRecent[i];
					RequestTextDrawFileClose([]{ openFile(chosen.c_str()); });
				}
				ImGui::SetItemTooltip("%s", gRecent[i].c_str());
				ImGui::PopID();
			}
			if(ImGui::MenuItem(L("Clear list"))){ gRecent.clear(); SaveEditorSettings(); }
			ImGui::EndMenu();
		}
		ImGui::SeparatorText(_("Code target"));
		TextDrawCodeTarget target = GetTextDrawCodeTarget();
		if(ImGui::MenuItem("SA-MP 0.3.7", nil, target == TEXTDRAW_CODE_SAMP) && target != TEXTDRAW_CODE_SAMP){
			SetTextDrawCodeTarget(TEXTDRAW_CODE_SAMP); TextDrawCodeTargetChanged(); SaveEditorSettings();
		}
		if(ImGui::MenuItem("open.mp", nil, target == TEXTDRAW_CODE_OPENMP) && target != TEXTDRAW_CODE_OPENMP){
			SetTextDrawCodeTarget(TEXTDRAW_CODE_OPENMP); TextDrawCodeTargetChanged(); SaveEditorSettings();
		}
		ImGui::SetItemTooltip("%s", _("Generated Pawn code targets only the selected server."));
		ImGui::EndPopup();
	}
	ImGui::TextWrapped("%s%s", GetTextDrawFileName(), TextDrawFileHasUnsavedChanges() ? " *" : "");
	if(GetTextDrawFilePath()[0])
		ImGui::SetItemTooltip(_("%s\nInclude it in your gamemode and call %s_CreateTextDraws()\n"
			"from OnGameModeInit and %s_ShowTextDraws(playerid)."), GetTextDrawFilePath(),
			TextDrawFilePrefix(GetTextDrawFilePath()).c_str(), TextDrawFilePrefix(GetTextDrawFilePath()).c_str());
}

// What a row in the layers shows: its kind and a bit of its text.
static std::string
layerLabel(const TextDraw &td)
{
	if(td.font == TD_FONT_REFERENCE){
		const char *file = td.text.c_str() + (td.text.find_last_of("/\\") == std::string::npos ? 0 :
			td.text.find_last_of("/\\") + 1);
		return std::string(ICON_FA_FILE " ") + _("image") + "  " + file;
	}
	const char *kind = td.font == TD_FONT_SPRITE ? _("sprite") : td.font == TD_FONT_PREVIEW ? _("model") :
		td.useBox ? _("box") : _("text");
	std::string preview = td.name[0] ? std::string(td.name) : td.text.substr(0, 28);
	std::replace(preview.begin(), preview.end(), '\n', ' ');
	return std::string(td.perPlayer ? ICON_FA_USER " " : ICON_FA_GLOBE " ") + kind + "  " + preview;
}

// Small flat toggles at the start of a row: shown and locked.
static bool
rowToggle(const char *on, const char *off, bool value, const char *id, const char *tooltip)
{
	ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0, 0, 0, 0));
	ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(2.0f, 0.0f));
	if(!value)
		ImGui::PushStyleColor(ImGuiCol_Text, ImGui::GetStyleColorVec4(ImGuiCol_TextDisabled));
	char label[48];
	snprintf(label, sizeof(label), "%s##%s", value ? on : off, id);
	bool pressed = ImGui::SmallButton(label);
	if(!value)
		ImGui::PopStyleColor();
	ImGui::PopStyleVar();
	ImGui::PopStyleColor();
	ImGui::SetItemTooltip("%s", tooltip);
	return pressed;
}

static void
rowMenu(int i)
{
	std::vector<TextDraw> &tds = GetTextDraws();
	if(!isSelected(i)){
		if(!tds[i].group.empty() && tds[i].group != gIsolatedGroup){
			selectGroup(tds[i].group, false);
			gSelected = i;
		}else
			selectOnly(i);
	}
	gLayerSelection = true;
	if(ImGui::MenuItem(L("Duplicate"), ShortcutLabel(SC_DUPLICATE)))
		appendCopies(selectedCopies(), 10.0f);
	if(ImGui::MenuItem(L("Delete"), ShortcutLabel(SC_DELETE)))
		deleteSelected();
	ImGui::Separator();
	if(ImGui::MenuItem(L("Group"), ShortcutLabel(SC_GROUP_SELECTION)))
		groupSelected(newGroupName().c_str());
	if(ImGui::MenuItem(L("Ungroup"))){
		for(int k : gSel)
			tds[k].group.clear();
		TextDrawsChanged();
	}
	std::vector<std::string> boards;
	listBoards(boards);
	if(boards.size() > 1 && ImGui::BeginMenu(L("Move to artboard"))){
		for(const std::string &b : boards)
			if(b != gBoard && ImGui::MenuItem(boardLabel(b))){
				for(int k : gSel)
					tds[k].board = b;
				selectOnly(-1);
				TextDrawsChanged();
			}
		ImGui::EndMenu();
	}
}

// The board's textdraws, the top one first: groups fold, the eye hides one
// in the editor, the lock keeps it in place; drag a row to restack.
static void
layers(void)
{
	std::vector<TextDraw> &tds = GetTextDraws();
	ImGuiIO &io = ImGui::GetIO();
	static std::vector<std::string> folded;
	ImGui::BeginChild("list", ImVec2(0, ImGui::GetFontSize() * 8.0f), ImGuiChildFlags_ResizeY);
	std::string openGroup;	// the group the rows above belong to
	bool any = false;
	int dropAt = -1;
	for(int i = (int)tds.size() - 1; i >= 0; i--){
		TextDraw &td = tds[i];
		if(!onBoard(td))
			continue;
		any = true;
		ImGui::PushID(i);
		if(!td.group.empty() && td.group != openGroup){
			// A group's header: its toggles act on every member.
			openGroup = td.group;
			bool isFolded = std::find(folded.begin(), folded.end(), td.group) != folded.end();
			bool allHidden = true, allLocked = true;
			for(const TextDraw &t : tds)
				if(onBoard(t) && t.group == td.group){
					allHidden &= t.hidden;
					allLocked &= t.locked;
				}
			if(rowToggle(ICON_FA_EYE, ICON_FA_EYE_SLASH, !allHidden, "geye", _("Show or hide the group in the editor"))){
				for(TextDraw &t : tds)
					if(onBoard(t) && t.group == td.group)
						t.hidden = !allHidden;
				TextDrawsChanged();
			}
			ImGui::SameLine();
			if(rowToggle(ICON_FA_LOCK, ICON_FA_LOCK, allLocked, "glock", _("Lock or unlock the group"))){
				for(TextDraw &t : tds)
					if(onBoard(t) && t.group == td.group)
						t.locked = !allLocked;
				TextDrawsChanged();
			}
			ImGui::SameLine();
			char header[96];
			snprintf(header, sizeof(header), "%s " ICON_FA_LAYER_GROUP " %s##group", isFolded ? ">" : "v", td.group.c_str());
			bool all = true;
			for(int k = 0; k < (int)tds.size(); k++)
				if(onBoard(tds[k]) && tds[k].group == td.group)
					all &= isSelected(k);
			if(ImGui::Selectable(header, all, ImGuiSelectableFlags_AllowOverlap | ImGuiSelectableFlags_AllowDoubleClick,
			ImVec2(std::max(1.0f, ImGui::GetContentRegionAvail().x - ImGui::GetStyle().ItemSpacing.x * 0.5f), 0))){
				if(io.MousePos.x < ImGui::GetItemRectMin().x + ImGui::GetFontSize()){
					if(isFolded) folded.erase(std::find(folded.begin(), folded.end(), td.group));
					else folded.push_back(td.group);
				}else{
					gIsolatedGroup.clear();
					selectGroup(td.group, io.KeyShift || io.KeyCtrl);
					gLayerSelection = true;
				}
			}
			ImGui::SetItemTooltip("%s", _("Click selects the group. Click the arrow to fold it; right-click to rename.\nDouble-click a member to edit it alone; Esc selects the whole group."));
			if(ImGui::BeginPopupContextItem("##group_menu")){
				static char name[64];
				if(ImGui::IsWindowAppearing())
					snprintf(name, sizeof(name), "%s", td.group.c_str());
				ImGui::SetNextItemWidth(ImGui::GetFontSize() * 10.0f);
				if(ImGui::InputText("##rename", name, sizeof(name), ImGuiInputTextFlags_EnterReturnsTrue) && name[0]){
					std::string from = td.group;
					for(TextDraw &t : tds)
						if(t.group == from)
							t.group = name;
					if(gIsolatedGroup == from)
						gIsolatedGroup = name;
					std::replace(folded.begin(), folded.end(), from, std::string(name));
					TextDrawsChanged();
					ImGui::CloseCurrentPopup();
				}
				if(ImGui::MenuItem(L("Ungroup"))){
					std::string from = td.group;
					for(TextDraw &t : tds)
						if(t.group == from)
							t.group.clear();
					if(gIsolatedGroup == from)
						gIsolatedGroup.clear();
					TextDrawsChanged();
				}
				ImGui::EndPopup();
			}
			if(isFolded){
				ImGui::PopID();
				while(i > 0 && (!onBoard(tds[i - 1]) || tds[i - 1].group == openGroup))
					i--;
				continue;
			}
		}else if(td.group.empty())
			openGroup.clear();

		if(!td.group.empty())
			ImGui::Indent(ImGui::GetFontSize());
		if(rowToggle(ICON_FA_EYE, ICON_FA_EYE_SLASH, !td.hidden, "eye", _("Shown in the editor (it is in the file either way)"))){
			td.hidden = !td.hidden;
			TextDrawsChanged();
		}
		ImGui::SameLine();
		if(rowToggle(ICON_FA_LOCK, ICON_FA_LOCK, td.locked, "lock", _("Locked: not dragged on the canvas"))){
			td.locked = !td.locked;
			TextDrawsChanged();
		}
		ImGui::SameLine();
		std::string label = layerLabel(td) + "##td" + std::to_string(i);
		if(ImGui::Selectable(label.c_str(), isSelected(i), ImGuiSelectableFlags_AllowOverlap | ImGuiSelectableFlags_AllowDoubleClick,
			ImVec2(std::max(1.0f, ImGui::GetContentRegionAvail().x - ImGui::GetStyle().ItemSpacing.x * 0.5f), 0))){
			bool doubleClick = ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left);
			if(!td.group.empty() && doubleClick && td.group != gIsolatedGroup){
				gIsolatedGroup = td.group;
				selectOnly(i);
			}else if(!td.group.empty() && td.group == gIsolatedGroup){
				if(io.KeyCtrl)
					toggleSelected(i);
				else if(io.KeyShift && gSelected >= 0){
					int a = std::min(gSelected, i), b = std::max(gSelected, i);
					for(int k = a; k <= b; k++)
						if(onBoard(tds[k]) && tds[k].group == gIsolatedGroup)
							addToSelection(k);
					gSelected = i;
				}else
					selectOnly(i);
			}else if(!td.group.empty()){
				gIsolatedGroup.clear();
				if(io.KeyCtrl)
					toggleGroupSelection(td.group, i);
				else if(io.KeyShift){
					selectGroup(td.group, true);
					gSelected = i;
				}else{
					selectGroup(td.group, false);
					gSelected = i;
				}
			}else{
				gIsolatedGroup.clear();
				if(io.KeyCtrl)
					toggleSelected(i);
				else if(io.KeyShift && gSelected >= 0){
					int a = std::min(gSelected, i), b = std::max(gSelected, i);
					for(int k = a; k <= b; k++)
						if(onBoard(tds[k]))
							addToSelection(k);
					gSelected = i;
				}else
					selectOnly(i);
			}
			gLayerSelection = true;
		}
		if(ImGui::BeginPopupContextItem("##row_menu")){
			rowMenu(i);
			ImGui::EndPopup();
		}
		if(ImGui::BeginDragDropSource()){
			if(!isSelected(i))
				selectOnly(i);
			ImGui::SetDragDropPayload("HAUMEA_TD_ROW", &i, sizeof(i));
			ImGui::Text(_("%d textdraws"), (int)gSel.size());
			ImGui::EndDragDropSource();
		}
		if(ImGui::BeginDragDropTarget()){
			if(ImGui::AcceptDragDropPayload("HAUMEA_TD_ROW"))
				dropAt = i + 1;	// above this row: after it in the file
			ImGui::EndDragDropTarget();
		}
		if(!td.group.empty())
			ImGui::Unindent(ImGui::GetFontSize());
		ImGui::PopID();
	}
	if(!any)
		ImGui::TextDisabled("%s", _("No textdraws on this board yet."));
	ImGui::EndChild();
	if(dropAt >= 0)
		moveSelectionTo(dropAt);
}

// Pictures for the arrange buttons; the first six are the ALIGN_ ones.
enum { GLYPH_SPACE_ACROSS = 6, GLYPH_SPACE_DOWN, GLYPH_SAME_WIDTH, GLYPH_SAME_HEIGHT, GLYPH_FLIP_ACROSS, GLYPH_FLIP_DOWN };
static bool
arrangeButton(const char *id, int glyph, const char *tooltip)
{
	float s = ImGui::GetFrameHeight();
	bool pressed = ImGui::InvisibleButton(id, ImVec2(s, s));
	ImDrawList *dl = ImGui::GetWindowDrawList();
	ImVec2 p = ImGui::GetItemRectMin();
	bool off = ImGui::GetItemFlags() & ImGuiItemFlags_Disabled;
	ImU32 bg = ImGui::GetColorU32(ImGui::IsItemActive() ? ImGuiCol_ButtonActive :
		ImGui::IsItemHovered() ? ImGuiCol_ButtonHovered : ImGuiCol_Button);
	dl->AddRectFilled(p, ImVec2(p.x + s, p.y + s), bg, ImGui::GetStyle().FrameRounding);
	ImU32 fg = ImGui::GetColorU32(off ? ImGuiCol_TextDisabled : ImGuiCol_Text);
	ImU32 edge = off ? fg : IM_COL32(255, 200, 60, 255);
	float u = s / 16.0f;
	auto P = [&](float x, float y){ return ImVec2(p.x + x * u, p.y + y * u); };
	auto R = [&](float x0, float y0, float x1, float y1){ dl->AddRectFilled(P(x0, y0), P(x1, y1), fg); };
	auto Ln = [&](float x0, float y0, float x1, float y1){ dl->AddLine(P(x0, y0), P(x1, y1), edge, std::max(1.0f, u)); };
	switch(glyph){
	case ALIGN_LEFT: Ln(3, 2, 3, 14); R(4, 4, 13, 7); R(4, 9, 10, 12); break;
	case ALIGN_HCENTRE: R(3, 4, 13, 7); R(5, 9, 11, 12); Ln(8, 2, 8, 14); break;
	case ALIGN_RIGHT: Ln(13, 2, 13, 14); R(3, 4, 12, 7); R(6, 9, 12, 12); break;
	case ALIGN_TOP: Ln(2, 3, 14, 3); R(4, 4, 7, 13); R(9, 4, 12, 10); break;
	case ALIGN_VCENTRE: R(4, 3, 7, 13); R(9, 5, 12, 11); Ln(2, 8, 14, 8); break;
	case ALIGN_BOTTOM: Ln(2, 13, 14, 13); R(4, 3, 7, 12); R(9, 6, 12, 12); break;
	case GLYPH_SPACE_ACROSS: R(2, 4, 4, 12); R(7, 4, 9, 12); R(12, 4, 14, 12); break;
	case GLYPH_SPACE_DOWN: R(4, 2, 12, 4); R(4, 7, 12, 9); R(4, 12, 12, 14); break;
	case GLYPH_SAME_WIDTH:
		Ln(2, 3, 2, 13); Ln(14, 3, 14, 13); Ln(4, 8, 12, 8);
		dl->AddTriangleFilled(P(3, 8), P(6, 5), P(6, 11), fg);
		dl->AddTriangleFilled(P(13, 8), P(10, 11), P(10, 5), fg);
		break;
	case GLYPH_SAME_HEIGHT:
		Ln(3, 2, 13, 2); Ln(3, 14, 13, 14); Ln(8, 4, 8, 12);
		dl->AddTriangleFilled(P(8, 3), P(11, 6), P(5, 6), fg);
		dl->AddTriangleFilled(P(8, 13), P(5, 10), P(11, 10), fg);
		break;
	case GLYPH_FLIP_ACROSS:
		dl->AddTriangleFilled(P(7, 3), P(7, 13), P(2, 13), fg);
		dl->AddTriangle(P(9, 3), P(14, 13), P(9, 13), fg);
		Ln(8, 1, 8, 15);
		break;
	case GLYPH_FLIP_DOWN:
		dl->AddTriangleFilled(P(3, 7), P(13, 7), P(13, 2), fg);
		dl->AddTriangle(P(3, 9), P(13, 14), P(13, 9), fg);
		Ln(1, 8, 15, 8);
		break;
	}
	ImGui::SetItemTooltip("%s", tooltip);
	return pressed;
}

// What to do with the selection: copies, order, lock, groups, arranging.
static void
selectionActions(void)
{
	std::vector<TextDraw> &tds = GetTextDraws();
	if(gSel.empty())
		return;
	if(ImGui::Button(LI(ICON_FA_COPY, "Duplicate")))
		appendCopies(selectedCopies(), 10.0f);
	ImGui::SetItemTooltip("%s", ShortcutLabel(SC_DUPLICATE));
	flowNext(ImGui::GetFontSize() * 6.0f);
	if(ImGui::Button(LI(ICON_FA_TRASH, "Delete")))
		deleteSelected();
	ImGui::SetItemTooltip("%s", ShortcutLabel(SC_DELETE));
	if(gSel.empty())
		return;
	if(!ImGui::CollapsingHeader(L("Arrange")))
		return;
	if(ImGui::Button(L("Up")))
		restackSelected(true);
	ImGui::SetItemTooltip("%s", _("Drawn later, so over the others."));
	flowNext(ImGui::GetFontSize() * 6.0f);
	if(ImGui::Button(L("Down")))
		restackSelected(false);
	ImGui::SetItemTooltip("%s", _("Drawn earlier, so under the others."));
	bool locked = true;
	for(int i : gSel)
		locked &= tds[i].locked;
	if(ImGui::Checkbox(LI(ICON_FA_LOCK, "Locked"), &locked)){
		for(int i : gSel)
			tds[i].locked = locked;
		TextDrawsChanged();
	}
	ImGui::SetItemTooltip("%s", _("Can't be dragged or resized on the canvas. The fields here still change it."));
	flowNext(ImGui::GetFontSize() * 6.0f);
	if(ImGui::Button(LI(ICON_FA_LAYER_GROUP, "Group")))
		groupSelected(newGroupName().c_str());
	ImGui::SetItemTooltip("%s", _("Gives them one group name and keeps them together in the list.\n"
		"A group hides, locks and is selected as one."));

	// Arranging, always at hand: pictures, with what they do in their tooltips.
	std::vector<ArrangeUnit> units;
	selectionUnits(units);
	static const char *alignTips[6] = { N_("Left edges"), N_("Centres across"), N_("Right edges"),
		N_("Top edges"), N_("Middles"), N_("Bottom edges") };
	float sp = ImGui::GetStyle().ItemSpacing.x;
	for(int k = 0; k < 6; k++){
		if(k)
			flowNext(ImGui::GetFrameHeight(), k == 3 ? sp : sp * 0.5f);
		char id[16];
		snprintf(id, sizeof(id), "##align%d", k);
		if(arrangeButton(id, k, _(alignTips[k])))
			alignSelected(k);
	}
	const char *tos[] = { _("To the selection"), _("To the key one"), _("To the screen") };
	ImGui::SetNextItemWidth(-FLT_MIN);
	ImGui::Combo("##alignto", &gAlignTo, tos, IM_ARRAYSIZE(tos));
	ImGui::SetItemTooltip("%s", _("What aligning lines them up with. The key one is the one clicked last\n"
		"(outlined thicker), and it stays put. One on its own aligns to the screen.\n"
		"A group moves as one, and a shape aligns by its own edges."));
	static bool fixedGap;
	static float gap = 8.0f;
	ImGui::BeginDisabled(units.size() < (fixedGap ? 2u : 3u));
	if(arrangeButton("##spaceacross", GLYPH_SPACE_ACROSS, _("Space evenly across")))
		distributeSelected(true, fixedGap ? gap : -1.0f);
	flowNext(ImGui::GetFrameHeight(), sp * 0.5f);
	if(arrangeButton("##spacedown", GLYPH_SPACE_DOWN, _("Space evenly down")))
		distributeSelected(false, fixedGap ? gap : -1.0f);
	ImGui::EndDisabled();
	flowNext(ImGui::GetFontSize() * 6.0f);
	ImGui::Checkbox(L("Gap"), &fixedGap);
	ImGui::SetItemTooltip("%s", _("Off: the outer ones stay and the gaps between come out equal.\n"
		"On: each is this far from the one before, from the first."));
	if(fixedGap){
		flowNext(ImGui::GetFontSize() * 6.0f);
		ImGui::SetNextItemWidth(ImGui::GetFontSize() * 3.5f);
		ImGui::DragFloat("##gap", &gap, 0.25f, 0.0f, 640.0f, "%.1f");
	}
	flowNext(ImGui::GetFrameHeight(), sp);
	ImGui::BeginDisabled(units.size() < 2);
	if(arrangeButton("##samewidth", GLYPH_SAME_WIDTH, _("Same width as the key one")))
		matchSize(true);
	flowNext(ImGui::GetFrameHeight(), sp * 0.5f);
	if(arrangeButton("##sameheight", GLYPH_SAME_HEIGHT, _("Same height as the key one")))
		matchSize(false);
	ImGui::EndDisabled();
	flowNext(ImGui::GetFrameHeight(), sp);
	if(arrangeButton("##flipacross", GLYPH_FLIP_ACROSS, _("Flip across: sprites mirror, shapes point the other way")))
		flipSelected(true);
	flowNext(ImGui::GetFrameHeight(), sp * 0.5f);
	if(arrangeButton("##flipdown", GLYPH_FLIP_DOWN, _("Flip upside down")))
		flipSelected(false);
}

void
DrawTextDrawsWindow(void)
{
	drawPopups();
	if(!gMode)
		return;
	UpdateTextDrawsUndo();
	validateSelection();
	ImGui::SetNextWindowSize(ImVec2(ImGui::GetFontSize() * 24.0f, ImGui::GetFontSize() * 38.0f), ImGuiCond_FirstUseEver);
	// ###textdraws, not LI(): the ID the window has always had, so saved layouts stay.
	char title[64];
	snprintf(title, sizeof(title), ICON_FA_FONT " %s###textdraws", _("TextDraws"));
	if(!ImGui::Begin(title, nil)){
		ImGui::End();
		return;
	}
	fileBar();

	const std::vector<TextDraw> &mapTds = GetMapTextDraws();
	if(!mapTds.empty()){
		char inMap[128];
		snprintf(inMap, sizeof(inMap), _("%d in the map (read-only)"), (int)mapTds.size());
		ImGui::TextColored(ImVec4(1.0f, 0.78f, 0.25f, 1.0f), ICON_FA_TRIANGLE_EXCLAMATION " %s", inMap);
		ImGui::SetItemTooltip("%s", _("From a map made before textdraws had their own file. They are saved\n"
			"back with the map as they are; move them here to edit them."));
		ImGui::SameLine();
		if(ImGui::SmallButton(L("Move to a separate file...")))
			OfferMoveMapTextDraws(GetPawnMapPath());
	}

	ImGui::Spacing();
	float cell = (ImGui::GetContentRegionAvail().x - ImGui::GetStyle().ItemSpacing.x * 2.0f) / 3.0f;
	const char *addNames[] = { N_("Text"), N_("Box"), N_("Sprite"), N_("Model"), N_("Image") };
	const char *addIcons[] = { ICON_FA_FONT, ICON_FA_SQUARE, ICON_FA_VECTOR_SQUARE, ICON_FA_CUBE, ICON_FA_FILE };
	const int addKinds[] = { TD_FONT_AHARONI, -1, TD_FONT_SPRITE, TD_FONT_PREVIEW, TD_FONT_REFERENCE };
	for(int i = 0; i < 5; i++){
		if(i % 3) ImGui::SameLine();
		char label[160];
		snprintf(label, sizeof(label), "%s %s###add_%d", addIcons[i], _(addNames[i]), i);
		if(ImGui::Button(label, ImVec2(cell, ImGui::GetFrameHeight()*1.25f)))
			addTextDraw(addKinds[i]);
	}
	ImGui::SetItemTooltip("%s", _("A picture to build on: a mock-up or a screenshot (PNG, BMP or TGA).\n"
		"It is a layer like the others, only the editor shows it; the eyedropper picks its colours."));
	ImGui::SameLine();
	shapeButton(cell);
	ImGui::Spacing();

	char layersTitle[96];
	snprintf(layersTitle, sizeof(layersTitle), _("Layers - %s"), boardLabel(gBoard));
	ImGui::SeparatorText(layersTitle);
	layers();
	selectionActions();

	if(gSelected >= 0){
		ImGui::Spacing();
		ImGui::BeginChild("##td_inspector", ImVec2(0, 0), ImGuiChildFlags_None);
		if(!shapePanel() || ImGui::CollapsingHeader(L("The sprites it is made of")))
			properties();
		ImGui::EndChild();
	}else{
		ImGui::Spacing();
		ImGui::TextWrapped("%s", _("Click a textdraw, or drag a box round several. Alt+click picks the one under it."));
	}
	ImGui::End();
}
