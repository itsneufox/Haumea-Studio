// SPDX-License-Identifier: GPL-2.0-or-later
#include "haumea.h"
#include "materialtext.h"
#include "workspace.h"

#include <algorithm>
#include <map>
#include <math.h>
#include <stdlib.h>
#include <sys/stat.h>
#ifndef _WIN32
#include <dirent.h>
#include <unistd.h>
#endif

// imgui compiles stb_truetype with STBTT_STATIC, so each user carries its own
// private copy of the implementation.
#define STBTT_STATIC
#define STB_TRUETYPE_IMPLEMENTATION
#include "imgui/imstb_truetype.h"

bool
GetMaterialTextDimensions(int materialSize, int *width, int *height)
{
	// samp.dll 0x100A8C80. Any other code makes the client skip the texture.
	static const int widths[] = {
		32, 64, 64, 128, 128, 128, 256, 256, 256, 256, 512, 512, 512, 512
	};
	static const int heights[] = {
		32, 32, 64, 32, 64, 128, 32, 64, 128, 256, 64, 128, 256, 512
	};
	if(materialSize < 10 || materialSize > 140 || materialSize % 10 != 0)
		return false;
	int index = materialSize/10 - 1;
	if(width) *width = widths[index];
	if(height) *height = heights[index];
	return true;
}

/*
 * Font index
 */

struct MaterialFontFace
{
	std::string path;
	int index;
	std::string family;	// name ID 1, lower case
	std::string typoFamily;	// name ID 16, lower case
	std::string fullName;	// name ID 4, lower case
	std::string displayFamily;
	int weight;
	bool italic;
};

static std::vector<MaterialFontFace> gFontFaces;
static std::vector<std::string> gFontFamilies;
static bool gFontIndexBuilt;

static std::string
lowerString(const std::string &s)
{
	std::string out = s;
	for(size_t i = 0; i < out.size(); i++)
		out[i] = (char)tolower((unsigned char)out[i]);
	return out;
}

static uint16
readBE16(const uint8 *p)
{
	return (uint16)((p[0] << 8) | p[1]);
}

static uint32
readBE32(const uint8 *p)
{
	return ((uint32)p[0] << 24) | ((uint32)p[1] << 16) | ((uint32)p[2] << 8) | p[3];
}

static bool
readFileRange(FILE *f, uint32 offset, uint32 size, std::vector<uint8> &out)
{
	if(size > 4*1024*1024 || fseek(f, offset, SEEK_SET) != 0)
		return false;
	out.resize(size);
	return size == 0 || fread(&out[0], 1, size, f) == size;
}

static void
appendUtf8(std::string &out, uint32 c)
{
	if(c < 0x80)
		out += (char)c;
	else if(c < 0x800){
		out += (char)(0xC0 | (c >> 6));
		out += (char)(0x80 | (c & 0x3F));
	}else{
		out += (char)(0xE0 | (c >> 12));
		out += (char)(0x80 | ((c >> 6) & 0x3F));
		out += (char)(0x80 | (c & 0x3F));
	}
}

// Picks a name record, preferring Windows US English (what GDI matches on),
// then any Windows language, then Macintosh Roman.
static std::string
sfntName(const std::vector<uint8> &name, int nameId)
{
	if(name.size() < 6)
		return "";
	int count = readBE16(&name[2]);
	uint32 storage = readBE16(&name[4]);
	int bestScore = 0;
	std::string best;
	for(int i = 0; i < count; i++){
		uint32 rec = 6 + i*12;
		if(rec + 12 > name.size())
			break;
		int platform = readBE16(&name[rec]);
		int encoding = readBE16(&name[rec+2]);
		int language = readBE16(&name[rec+4]);
		if(readBE16(&name[rec+6]) != nameId)
			continue;
		uint32 length = readBE16(&name[rec+8]);
		uint32 offset = storage + readBE16(&name[rec+10]);
		if(offset + length > name.size())
			continue;
		int score = 0;
		if(platform == 3 && language == 0x409) score = 3;
		else if(platform == 3) score = 2;
		else if(platform == 1 && encoding == 0) score = 1;
		if(score <= bestScore)
			continue;
		std::string value;
		if(platform == 3){
			for(uint32 j = 0; j + 1 < length; j += 2)
				appendUtf8(value, readBE16(&name[offset+j]));
		}else
			value.assign((const char*)&name[offset], length);
		if(value.empty())
			continue;
		bestScore = score;
		best = value;
	}
	return best;
}

static void
indexFontFace(FILE *f, const std::string &path, int faceIndex, uint32 faceOffset)
{
	std::vector<uint8> header;
	if(!readFileRange(f, faceOffset, 12, header))
		return;
	int numTables = readBE16(&header[4]);
	if(numTables <= 0 || numTables > 512)
		return;
	std::vector<uint8> dir;
	if(!readFileRange(f, faceOffset + 12, numTables*16, dir))
		return;

	uint32 nameOffset = 0, nameSize = 0, os2Offset = 0, os2Size = 0;
	uint32 headOffset = 0;
	for(int i = 0; i < numTables; i++){
		const uint8 *rec = &dir[i*16];
		uint32 offset = readBE32(rec + 8), size = readBE32(rec + 12);
		if(memcmp(rec, "name", 4) == 0){ nameOffset = offset; nameSize = size; }
		else if(memcmp(rec, "OS/2", 4) == 0){ os2Offset = offset; os2Size = size; }
		else if(memcmp(rec, "head", 4) == 0) headOffset = offset;
	}
	std::vector<uint8> name;
	if(nameOffset == 0 || !readFileRange(f, nameOffset, nameSize, name))
		return;

	MaterialFontFace face;
	face.path = path;
	face.index = faceIndex;
	face.displayFamily = sfntName(name, 1);
	face.family = lowerString(face.displayFamily);
	face.typoFamily = lowerString(sfntName(name, 16));
	face.fullName = lowerString(sfntName(name, 4));
	face.weight = 400;
	face.italic = false;
	std::vector<uint8> data;
	if(os2Offset && os2Size >= 64 && readFileRange(f, os2Offset, 64, data)){
		face.weight = readBE16(&data[4]);
		face.italic = (readBE16(&data[62]) & 1) != 0;
	}else if(headOffset && readFileRange(f, headOffset, 54, data)){
		uint16 macStyle = readBE16(&data[44]);
		face.weight = macStyle & 1 ? 700 : 400;
		face.italic = (macStyle & 2) != 0;
	}
	if(face.family.empty())
		return;
	gFontFaces.push_back(face);
}

static void
indexFontFile(const std::string &path)
{
	FILE *f = fopen(path.c_str(), "rb");
	if(f == nil)
		return;
	std::vector<uint8> header;
	if(readFileRange(f, 0, 12, header)){
		if(memcmp(&header[0], "ttcf", 4) == 0){
			int numFonts = (int)readBE32(&header[8]);
			std::vector<uint8> offsets;
			if(numFonts > 0 && numFonts < 256 &&
			   readFileRange(f, 12, numFonts*4, offsets))
				for(int i = 0; i < numFonts; i++)
					indexFontFace(f, path, i, readBE32(&offsets[i*4]));
		}else
			indexFontFace(f, path, 0, 0);
	}
	fclose(f);
}

static bool
hasFontExtension(const char *name)
{
	const char *dot = strrchr(name, '.');
	if(dot == nil)
		return false;
	std::string ext = lowerString(dot);
	return ext == ".ttf" || ext == ".otf" || ext == ".ttc" || ext == ".otc";
}

static bool
isDirectory(const std::string &path)
{
	struct stat st;
	return stat(path.c_str(), &st) == 0 && (st.st_mode & S_IFMT) == S_IFDIR;
}

static void
scanFontDirectory(const std::string &dir, int depth, int *budget)
{
	if(depth > 6 || *budget <= 0)
		return;
#ifdef _WIN32
	WIN32_FIND_DATAA entry;
	HANDLE handle = FindFirstFileA((dir + "\\*").c_str(), &entry);
	if(handle == INVALID_HANDLE_VALUE)
		return;
	do{
		if(strcmp(entry.cFileName, ".") == 0 || strcmp(entry.cFileName, "..") == 0)
			continue;
		std::string child = dir + "\\" + entry.cFileName;
		if(entry.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)
			scanFontDirectory(child, depth+1, budget);
		else if(hasFontExtension(entry.cFileName) && (*budget)-- > 0)
			indexFontFile(child);
	}while(FindNextFileA(handle, &entry));
	FindClose(handle);
#else
	DIR *d = opendir(dir.c_str());
	if(d == nil)
		return;
	std::vector<std::string> names;
	dirent *ent;
	while((ent = readdir(d)) != nil)
		if(strcmp(ent->d_name, ".") != 0 && strcmp(ent->d_name, "..") != 0)
			names.push_back(ent->d_name);
	closedir(d);
	std::sort(names.begin(), names.end());
	for(size_t i = 0; i < names.size(); i++){
		std::string child = dir + "/" + names[i];
		if(isDirectory(child))
			scanFontDirectory(child, depth+1, budget);
		else if(hasFontExtension(names[i].c_str()) && (*budget)-- > 0)
			indexFontFile(child);
	}
#endif
}

// Font files right in dir, not in its subfolders (the GTA folder, where
// SA-MP installs gtaweap3.ttf and sampaux3.ttf beside everything else).
static void
scanFontFilesIn(const std::string &dir, int *budget)
{
#ifdef _WIN32
	WIN32_FIND_DATAA entry;
	HANDLE handle = FindFirstFileA((dir + "\\*").c_str(), &entry);
	if(handle == INVALID_HANDLE_VALUE)
		return;
	do{
		if(!(entry.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) && hasFontExtension(entry.cFileName) &&
		   (*budget)-- > 0)
			indexFontFile(dir + "\\" + entry.cFileName);
	}while(FindNextFileA(handle, &entry));
	FindClose(handle);
#else
	DIR *d = opendir(dir.c_str());
	if(d == nil)
		return;
	std::vector<std::string> names;
	while(dirent *ent = readdir(d))
		names.push_back(ent->d_name);
	closedir(d);
	std::sort(names.begin(), names.end());
	for(size_t i = 0; i < names.size(); i++){
		std::string child = dir + "/" + names[i];
		if(hasFontExtension(names[i].c_str()) && !isDirectory(child) && (*budget)-- > 0)
			indexFontFile(child);
	}
#endif
}

// The per-user fonts folder: SA-MP's fonts and the free substitutes are
// installed there from the bundle, and the user can add .ttf files.
// HAUMEA_USER_FONTS_DIR moves it (tests).
std::string
GetMaterialUserFontsDir(void)
{
	const char *custom = getenv("HAUMEA_USER_FONTS_DIR");
	if(custom && custom[0])
		return custom;
#ifdef _WIN32
	const char *appData = getenv("APPDATA");
	return appData && appData[0] ? std::string(appData) + "\\haumea\\fonts" : std::string();
#elif defined(__APPLE__)
	const char *home = getenv("HOME");
	return home && home[0] ? std::string(home) + "/Library/Application Support/Haumea/fonts" : std::string();
#else
	const char *data = getenv("XDG_DATA_HOME");
	if(data && data[0])
		return std::string(data) + "/haumea/fonts";
	const char *home = getenv("HOME");
	return home && home[0] ? std::string(home) + "/.local/share/haumea/fonts" : std::string();
#endif
}

// fonts/material beside the editor (a release), or in the source tree above
// a developer build. HAUMEA_BUNDLED_FONTS_DIR moves it (tests).
static std::string
bundledFontsDir(void)
{
	const char *custom = getenv("HAUMEA_BUNDLED_FONTS_DIR");
	if(custom && custom[0])
		return custom;
	char root[1024];
	if(!GetEditorRootDirectory(root, sizeof(root)))
		return "";
	std::string dir = root;
	for(int up = 0; up < 5; up++){
		if(isDirectory(dir + "/fonts/material"))
			return dir + "/fonts/material";
		size_t slash = dir.find_last_of("/\\");
		if(slash == std::string::npos || slash == 0)
			break;
		dir.resize(slash);
	}
	return "";
}

static long
fileSize(const std::string &path)
{
	FILE *f = fopen(path.c_str(), "rb");
	if(f == nil)
		return -1;
	fseek(f, 0, SEEK_END);
	long size = ftell(f);
	fclose(f);
	return size;
}

void
InstallBundledMaterialFonts(void)
{
	// Tests and scripts only ever write a private folder.
	if(IsAutomatedRun() && getenv("HAUMEA_USER_FONTS_DIR") == nil)
		return;
	std::string from = bundledFontsDir(), to = GetMaterialUserFontsDir();
	if(from.empty() || to.empty())
		return;
	std::vector<std::string> names;
#ifdef _WIN32
	WIN32_FIND_DATAA entry;
	HANDLE handle = FindFirstFileA((from + "\\*.ttf").c_str(), &entry);
	if(handle != INVALID_HANDLE_VALUE){
		do names.push_back(entry.cFileName); while(FindNextFileA(handle, &entry));
		FindClose(handle);
	}
#else
	if(DIR *d = opendir(from.c_str())){
		while(dirent *ent = readdir(d))
			if(hasFontExtension(ent->d_name))
				names.push_back(ent->d_name);
		closedir(d);
	}
#endif
	int installed = 0;
	for(const std::string &name : names){
		std::string src = from + "/" + name, dst = to + "/" + name;
		// New, or changed by an update.
		if(fileSize(dst) == fileSize(src))
			continue;
		EnsureParentDirectoriesForPath(dst.c_str());
		FILE *in = fopen(src.c_str(), "rb");
		FILE *out = in ? fopen(dst.c_str(), "wb") : nil;
		char buffer[65536];
		size_t n;
		while(in && out && (n = fread(buffer, 1, sizeof(buffer), in)) > 0)
			fwrite(buffer, 1, n, out);
		if(in) fclose(in);
		if(out){ fclose(out); installed++; }
	}
	if(installed > 0){
		log("Material text: installed %d bundled font(s) into %s\n", installed, to.c_str());
		gFontIndexBuilt = false;
	}
}

static void
addEnvironmentPath(std::vector<std::string> &dirs, const char *variable, const char *suffix)
{
	const char *value = getenv(variable);
	if(value && value[0])
		dirs.push_back(std::string(value) + suffix);
}

static void
collectFontDirectories(std::vector<std::string> &dirs)
{
	const char *custom = getenv("HAUMEA_FONT_DIRS");
	if(custom){
#ifdef _WIN32
		const char separator = ';';
#else
		const char separator = ':';
#endif
		std::string list = custom;
		size_t start = 0;
		while(start <= list.size()){
			size_t end = list.find(separator, start);
			if(end == std::string::npos) end = list.size();
			if(end > start)
				dirs.push_back(list.substr(start, end - start));
			start = end + 1;
		}
	}

	// The user's own fonts first: SA-MP's (installed from the bundle) and any
	// they added; then Haumea's older data folder.
	std::string user = GetMaterialUserFontsDir();
	if(!user.empty())
		dirs.push_back(user);
	char path[1024];
	if(GetHaumeaDataPath(path, sizeof(path), "fonts"))
		dirs.push_back(path);

#ifdef _WIN32
	addEnvironmentPath(dirs, "WINDIR", "\\Fonts");
	addEnvironmentPath(dirs, "LOCALAPPDATA", "\\Microsoft\\Windows\\Fonts");
#else
	// GTA usually runs from a Wine prefix on these platforms. Its Windows
	// font folder holds the same Arial/Webdings files SA-MP players see.
	if(GetGameDir()[0]){
		std::string current = GetGameDir();
		for(;;){
			size_t slash = current.find_last_of('/');
			std::string leaf = slash == std::string::npos ? current : current.substr(slash+1);
			if(lowerString(leaf) == "drive_c"){
				dirs.push_back(current + "/windows/Fonts");
				break;
			}
			if(slash == std::string::npos || slash == 0)
				break;
			current = current.substr(0, slash);
		}
	}
	addEnvironmentPath(dirs, "WINEPREFIX", "/drive_c/windows/Fonts");
	addEnvironmentPath(dirs, "HOME", "/.wine/drive_c/windows/Fonts");
	addEnvironmentPath(dirs, "HOME", "/.local/share/fonts");
	addEnvironmentPath(dirs, "HOME", "/.fonts");
	addEnvironmentPath(dirs, "HOME", "/Library/Fonts");
	dirs.push_back("/Library/Fonts");
	dirs.push_back("/System/Library/Fonts");
	dirs.push_back("/usr/local/share/fonts");
	dirs.push_back("/usr/share/fonts");
#endif
}

static bool
caseInsensitiveLess(const std::string &a, const std::string &b)
{
	return lowerString(a) < lowerString(b);
}

static void
buildFontIndex(void)
{
	if(gFontIndexBuilt)
		return;
	gFontIndexBuilt = true;
	gFontFaces.clear();
	gFontFamilies.clear();

	std::vector<std::string> dirs;
	collectFontDirectories(dirs);
	int budget = 20000;
	std::vector<std::string> scanned;
	for(size_t i = 0; i < dirs.size(); i++){
		if(!isDirectory(dirs[i]) ||
		   std::find(scanned.begin(), scanned.end(), dirs[i]) != scanned.end())
			continue;
		scanned.push_back(dirs[i]);
		scanFontDirectory(dirs[i], 0, &budget);
	}
	// SA-MP's fonts in the GTA folder itself (only its top level), and the
	// bundled ones last: they stand in where nothing else has the font.
	if(GetGameDir()[0])
		scanFontFilesIn(GetGameDir(), &budget);
	std::string bundled = bundledFontsDir();
	if(!bundled.empty() && std::find(scanned.begin(), scanned.end(), bundled) == scanned.end())
		scanFontFilesIn(bundled, &budget);

	std::vector<std::string> seen;
	for(size_t i = 0; i < gFontFaces.size(); i++){
		if(std::find(seen.begin(), seen.end(), gFontFaces[i].family) != seen.end())
			continue;
		seen.push_back(gFontFaces[i].family);
		gFontFamilies.push_back(gFontFaces[i].displayFamily);
	}
	std::sort(gFontFamilies.begin(), gFontFamilies.end(), caseInsensitiveLess);
	log("Material text: indexed %d font face(s) in %d famil%s\n",
		(int)gFontFaces.size(), (int)gFontFamilies.size(),
		gFontFamilies.size() == 1 ? "y" : "ies");
}

const std::vector<std::string>&
GetMaterialFontFamilies(void)
{
	buildFontIndex();
	return gFontFamilies;
}

static const MaterialFontFace*
findFontFamily(const std::string &wanted, int weight)
{
	const MaterialFontFace *best = nil;
	int bestScore = 0;
	for(size_t i = 0; i < gFontFaces.size(); i++){
		const MaterialFontFace &face = gFontFaces[i];
		int score;
		if(face.family == wanted || face.typoFamily == wanted)
			score = 0;
		else if(face.fullName == wanted)
			score = 50;	// GDI also accepts a face's full name
		else
			continue;
		score += abs(face.weight - weight) + (face.italic ? 1000 : 0);
		if(best == nil || score < bestScore){
			best = &face;
			bestScore = score;
		}
	}
	return best;
}

// GDI resolves a missing face through font substitution. Wine and most Linux
// systems ship metric-compatible replacements for the core Windows fonts.
static const MaterialFontFace*
resolveFont(const char *fontName, bool bold, int *status)
{
	buildFontIndex();
	int weight = bold ? 700 : 400;
	std::string wanted = lowerString(fontName && fontName[0] ? fontName : "Arial");
	const MaterialFontFace *face = findFontFamily(wanted, weight);
	if(status)
		*status = face ? MATERIAL_FONT_FOUND : MATERIAL_FONT_SUBSTITUTE;
	if(face)
		return face;

	static const char *substitutes[][4] = {
		{ "arial", "liberation sans", "arimo", nil },
		{ "times new roman", "liberation serif", "tinos", nil },
		{ "courier new", "liberation mono", "cousine", nil },
		{ "comic sans ms", "comic neue", nil, nil },
		{ "verdana", "dejavu sans", nil, nil },
		{ "tahoma", "dejavu sans", nil, nil },
	};
	for(size_t i = 0; i < sizeof(substitutes)/sizeof(substitutes[0]); i++){
		if(wanted != substitutes[i][0])
			continue;
		for(int j = 1; j < 4 && substitutes[i][j]; j++)
			if((face = findFontFamily(substitutes[i][j], weight)) != nil)
				return face;
	}
	if(status)
		*status = MATERIAL_FONT_MISSING;
	static const char *fallbacks[] = {
		"arial", "liberation sans", "arimo", "dejavu sans", "noto sans", "inter"
	};
	for(size_t i = 0; i < sizeof(fallbacks)/sizeof(fallbacks[0]); i++)
		if((face = findFontFamily(fallbacks[i], weight)) != nil)
			return face;
	return gFontFaces.empty() ? nil : &gFontFaces[0];
}

int
DescribeMaterialFont(const char *fontName, bool bold, std::string *description)
{
	int status = MATERIAL_FONT_MISSING;
	const MaterialFontFace *face = resolveFont(fontName, bold, &status);
	if(description){
		if(face == nil)
			*description = "no usable font files found";
		else{
			*description = face->displayFamily;
			if(bold && face->weight < 600)
				*description += " (synthetic bold)";
			const char *slash = strrchr(face->path.c_str(), '/');
			const char *backslash = strrchr(face->path.c_str(), '\\');
			if(backslash > slash) slash = backslash;
			*description += " - ";
			*description += slash ? slash + 1 : face->path.c_str();
		}
	}
	return status;
}

/*
 * Font loading
 */

struct LoadedMaterialFont
{
	std::vector<uint8> data;
	stbtt_fontinfo info;
	bool symbol;	// (3,0) symbol cmap: glyphs live at U+F000 + byte
	int ascent, descent;	// GDI cell metrics in font units
	bool valid;
};

static std::map<std::string, LoadedMaterialFont*> gLoadedFonts;

static uint32
findSymbolCmap(const uint8 *data, uint32 fontStart)
{
	uint32 cmap = stbtt__find_table((stbtt_uint8*)data, fontStart, "cmap");
	if(cmap == 0)
		return 0;
	int numTables = readBE16(data + cmap + 2);
	for(int i = 0; i < numTables; i++){
		const uint8 *record = data + cmap + 4 + 8*i;
		if(readBE16(record) == 3 && readBE16(record + 2) == 0)
			return cmap + readBE32(record + 4);
	}
	return 0;
}

static LoadedMaterialFont*
loadFont(const MaterialFontFace *face)
{
	if(face == nil)
		return nil;
	char key[16];
	snprintf(key, sizeof(key), "#%d", face->index);
	std::string id = face->path + key;
	std::map<std::string, LoadedMaterialFont*>::iterator it = gLoadedFonts.find(id);
	if(it != gLoadedFonts.end())
		return it->second->valid ? it->second : nil;

	LoadedMaterialFont *font = new LoadedMaterialFont;
	font->valid = false;
	font->symbol = false;
	gLoadedFonts[id] = font;

	FILE *f = fopen(face->path.c_str(), "rb");
	if(f == nil)
		return nil;
	fseek(f, 0, SEEK_END);
	long size = ftell(f);
	fseek(f, 0, SEEK_SET);
	if(size > 0 && size < 64*1024*1024){
		font->data.resize(size);
		if(fread(&font->data[0], 1, size, f) != (size_t)size)
			font->data.clear();
	}
	fclose(f);
	if(font->data.empty())
		return nil;

	uint8 *data = &font->data[0];
	int offset = stbtt_GetFontOffsetForIndex(data, face->index);
	if(offset < 0)
		return nil;
	// GDI uses a font's Windows symbol cmap (3,0) whenever it has one; that is
	// where Webdings and Wingdings keep their glyphs, at U+F000 + byte. Some
	// copies also carry a small Unicode cmap, which stb_truetype would prefer.
	// InitFont fills everything else in before rejecting a symbol-only font,
	// so the setup can be finished by hand.
	bool initialised = stbtt_InitFont(&font->info, data, offset) != 0;
	uint32 symbolMap = findSymbolCmap(data, offset);
	if(symbolMap && font->info.head){
		font->info.index_map = symbolMap;
		font->info.indexToLocFormat = readBE16(data + font->info.head + 50);
		font->symbol = true;
	}else if(!initialised)
		return nil;

	// GDI sizes a positive lfHeight by the cell height usWinAscent +
	// usWinDescent; fall back to hhea when there is no OS/2 table.
	uint32 os2 = stbtt__find_table(data, offset, "OS/2");
	if(os2 && os2 + 78 <= font->data.size() &&
	   readBE16(data + os2 + 74) + readBE16(data + os2 + 76) > 0){
		font->ascent = readBE16(data + os2 + 74);
		font->descent = readBE16(data + os2 + 76);
		// Some fonts store these signed: SA-MP's gtaweap3.ttf has usWinDescent
		// 65507, i.e. -29, which taken as unsigned shrinks every glyph to a
		// speck. Read such a value as the signed one it is.
		if(font->ascent > 0x7FFF) font->ascent = 0x10000 - font->ascent;
		if(font->descent > 0x7FFF) font->descent = 0x10000 - font->descent;
	}else{
		int ascent, descent, gap;
		stbtt_GetFontVMetrics(&font->info, &ascent, &descent, &gap);
		font->ascent = ascent;
		font->descent = -descent;
	}
	if(font->ascent + font->descent <= 0)
		return nil;
	font->valid = true;
	return font;
}

void
RescanMaterialFonts(void)
{
	for(std::map<std::string, LoadedMaterialFont*>::iterator it = gLoadedFonts.begin();
	    it != gLoadedFonts.end(); ++it)
		delete it->second;
	gLoadedFonts.clear();
	gFontIndexBuilt = false;
	buildFontIndex();
}

/*
 * Rendering
 */

// The editor stores material text as UTF-8, so the preview renders the code
// points the author typed. Stray bytes that aren't valid UTF-8 still fall
// back to Windows-1252, as DrawTextA would on the Western systems SA-MP
// scripts are typically written for.
static uint32
windows1252ToUnicode(uint8 c)
{
	static const uint16 high[32] = {
		0x20AC, 0x0081, 0x201A, 0x0192, 0x201E, 0x2026, 0x2020, 0x2021,
		0x02C6, 0x2030, 0x0160, 0x2039, 0x0152, 0x008D, 0x017D, 0x008F,
		0x0090, 0x2018, 0x2019, 0x201C, 0x201D, 0x2022, 0x2013, 0x2014,
		0x02DC, 0x2122, 0x0161, 0x203A, 0x0153, 0x009D, 0x017E, 0x0178
	};
	if(c >= 0x80 && c < 0xA0)
		return high[c - 0x80];
	return c;
}

// The editor keeps material text as UTF-8: imported scripts are decoded to it
// and the text field writes it, so preview the code points the author typed.
// A byte that does not start a well-formed sequence still falls back to
// Windows-1252, which keeps single-byte text rendering exactly as it did.
static uint32
decodeMaterialCodepoint(const char **pp)
{
	const unsigned char *p = (const unsigned char*)*pp;
	unsigned char c = *p;
	if(c < 0x80){
		*pp += 1;
		return c;
	}
	int extra;
	uint32 cp;
	if((c & 0xE0) == 0xC0){ extra = 1; cp = c & 0x1F; }
	else if((c & 0xF0) == 0xE0){ extra = 2; cp = c & 0x0F; }
	else if((c & 0xF8) == 0xF0){ extra = 3; cp = c & 0x07; }
	else{
		*pp += 1;
		return windows1252ToUnicode(c);
	}
	for(int i = 1; i <= extra; i++){
		if((p[i] & 0xC0) != 0x80){
			// Not UTF-8 after all: a Windows-1252 byte, or a sequence cut
			// short by the end of the string.
			*pp += 1;
			return windows1252ToUnicode(c);
		}
		cp = cp << 6 | (p[i] & 0x3F);
	}
	*pp += 1 + extra;
	return cp;
}

static int
glyphForCodepoint(LoadedMaterialFont *font, uint32 cp)
{
	if(font->symbol){
		int glyph = stbtt_FindGlyphIndex(&font->info, 0xF000 + cp);
		return glyph ? glyph : stbtt_FindGlyphIndex(&font->info, cp);
	}
	return stbtt_FindGlyphIndex(&font->info, cp);
}

static int
hexValue(char c)
{
	if(c >= '0' && c <= '9') return c - '0';
	if(c >= 'a' && c <= 'f') return c - 'a' + 10;
	if(c >= 'A' && c <= 'F') return c - 'A' + 10;
	return -1;
}

// samp.dll ParseBracedSixHexDigits: exactly "{RRGGBB}".
static bool
parseColorTag(const char *p, uint32 *rgb)
{
	if(p[0] != '{')
		return false;
	uint32 value = 0;
	for(int i = 1; i <= 6; i++){
		int digit = hexValue(p[i]);
		if(digit < 0)
			return false;
		value = value << 4 | digit;
	}
	if(p[7] != '}')
		return false;
	*rgb = value;
	return true;
}

struct MaterialTextGlyph
{
	uint32 character;	// UTF-8-decoded code point (1252 per stray byte)
	uint32 color;	// D3DCOLOR (ARGB)
};

static uint32
packARGB(const rw::RGBA &c)
{
	return (uint32)c.alpha << 24 | (uint32)c.red << 16 | (uint32)c.green << 8 | c.blue;
}

// Splits the text into lines of coloured glyphs. Colour tags are stripped
// before layout and recolour the following glyphs at full opacity, as the
// client's ID3DXFontHook does.
static void
parseMaterialText(const char *text, uint32 baseColor,
	std::vector<std::vector<MaterialTextGlyph> > &lines)
{
	lines.clear();
	lines.push_back(std::vector<MaterialTextGlyph>());
	uint32 color = baseColor;
	for(const char *p = text; *p; ){
		uint32 rgb;
		if(parseColorTag(p, &rgb)){
			color = 0xFF000000u | rgb;
			p += 8;
			continue;
		}
		if(*p == '\r' || *p == '\n'){
			if(p[0] == '\r' && p[1] == '\n')
				p++;
			p++;
			lines.push_back(std::vector<MaterialTextGlyph>());
			continue;
		}
		MaterialTextGlyph glyph;
		glyph.character = decodeMaterialCodepoint(&p);
		glyph.color = color;
		lines.back().push_back(glyph);
	}
}

// D3DX sprites blend with SRCALPHA/INVSRCALPHA on every channel, alpha
// included, so text over a transparent background keeps a reduced alpha.
static void
blendPixel(rw::Image *image, int x, int y, uint32 color, float coverage)
{
	if(x < 0 || y < 0 || x >= image->width || y >= image->height || coverage <= 0.0f)
		return;
	uint8 *pixel = image->pixels + y*image->stride + x*4;
	float a = ((color >> 24) & 0xFF) / 255.0f * coverage;
	float src[4] = {
		(float)((color >> 16) & 0xFF), (float)((color >> 8) & 0xFF),
		(float)(color & 0xFF), a * 255.0f
	};
	for(int i = 0; i < 4; i++){
		float value = src[i]*a + pixel[i]*(1.0f - a);
		pixel[i] = (uint8)clamp((int)(value + 0.5f), 0, 255);
	}
}

rw::Image*
RenderMaterialTextImage(const ObjectMaterialOverride &material)
{
	int width, height;
	if(!GetMaterialTextDimensions(material.materialSize, &width, &height))
		return nil;
	rw::Image *image = rw::Image::create(width, height, 32);
	if(image == nil)
		return nil;
	image->allocate();

	// IDirect3DDevice9::Clear with the background colour.
	const rw::RGBA &bg = material.backgroundColor;
	for(int y = 0; y < height; y++)
		for(int x = 0; x < width; x++){
			uint8 *pixel = image->pixels + y*image->stride + x*4;
			pixel[0] = bg.red;
			pixel[1] = bg.green;
			pixel[2] = bg.blue;
			pixel[3] = bg.alpha;
		}

	const MaterialFontFace *face = resolveFont(material.font, material.bold, nil);
	LoadedMaterialFont *font = loadFont(face);
	if(font == nil || material.text[0] == '\0' || material.fontSize <= 0)
		return image;

	bool syntheticBold = material.bold && face->weight < 600;
	float scale = (float)material.fontSize / (float)(font->ascent + font->descent);
	int ascent = (int)floorf(font->ascent*scale + 0.5f);
	int lineHeight = (int)floorf((font->ascent + font->descent)*scale + 0.5f);

	std::vector<std::vector<MaterialTextGlyph> > lines;
	parseMaterialText(material.text, packARGB(material.textColor), lines);

	// DT_CENTER is sent together with DT_VCENTER; left and right text stays
	// at the top of the texture.
	int top = 0;
	if(material.alignment == 1)
		top = (height - (int)lines.size()*lineHeight) / 2;

	std::vector<float> coverage;
	for(size_t line = 0; line < lines.size(); line++){
		const std::vector<MaterialTextGlyph> &glyphs = lines[line];
		std::vector<int> glyphIndices(glyphs.size());
		std::vector<int> advances(glyphs.size());
		int lineWidth = 0;
		for(size_t i = 0; i < glyphs.size(); i++){
			uint32 c = (glyphs[i].character == '\t') ? (uint32)' ' : glyphs[i].character;
			glyphIndices[i] = glyphForCodepoint(font, c);
			int advance, lsb;
			stbtt_GetGlyphHMetrics(&font->info, glyphIndices[i], &advance, &lsb);
			advances[i] = (int)floorf(advance*scale + 0.5f) + (syntheticBold ? 1 : 0);
			lineWidth += advances[i];
		}

		int penX = 0;
		if(material.alignment == 1)
			penX = (width - lineWidth) / 2;
		else if(material.alignment == 2)
			penX = width - lineWidth;
		int baseline = top + (int)line*lineHeight + ascent;

		for(size_t i = 0; i < glyphs.size(); penX += advances[i], i++){
			uint32 c = glyphs[i].character;
			if(c == ' ' || c == '\t')
				continue;
			int gw, gh, gx, gy;
			unsigned char *bitmap = stbtt_GetGlyphBitmap(&font->info, scale, scale,
				glyphIndices[i], &gw, &gh, &gx, &gy);
			if(bitmap == nil)
				continue;
			// GDI's fake bold smears each glyph one pixel to the right.
			int cw = gw + (syntheticBold ? 1 : 0);
			coverage.assign(cw*gh, 0.0f);
			for(int y = 0; y < gh; y++)
				for(int x = 0; x < gw; x++){
					float value = bitmap[y*gw + x] / 255.0f;
					float &dst = coverage[y*cw + x];
					dst = max(dst, value);
					if(syntheticBold){
						float &next = coverage[y*cw + x + 1];
						next = max(next, value);
					}
				}
			stbtt_FreeBitmap(bitmap, nil);
			for(int y = 0; y < gh; y++)
				for(int x = 0; x < cw; x++)
					blendPixel(image, penX + gx + x, baseline + gy + y,
						glyphs[i].color, coverage[y*cw + x]);
		}
	}
	return image;
}
