// ---------------------------------------------------------------------------
#pragma once
#ifndef clsUtilH
#define clsUtilH
#include "FileOptimizerPCH1.h"


// ---------------------------------------------------------------------------------------------------------------------------------------------------------------------------
#if !defined(_max)
	#define max(a,b) (((a) > (b)) ? (a) : (b))
#endif
#if !defined(_min)
	#define min(a,b) (((a) < (b)) ? (a) : (b))
#endif



#if defined(PATH_MAX)
	#undef PATH_MAX
#endif
#define PATH_MAX	(MAX_PATH << 6)

//Interface zoom, as a percentage of the design size
#define KI_ZOOM_MIN			50
#define KI_ZOOM_MAX			400
#define KI_ZOOM_DEFAULT		100


// ---------------------------------------------------------------------------
class clsUtil
{
	public:
		static const void * __fastcall MemMem (const void *buf, size_t buf_len, const void *byte_sequence, size_t byte_sequence_len);
		static int __fastcall MsgBox(HWND phWnd, const TCHAR *pacText, const TCHAR *pacTitle, unsigned int piType, unsigned int piTimeout = 0);
		static unsigned long __fastcall ReadPipeAvailable(HANDLE hRead, void *pvData, unsigned int piSize);
		static bool __fastcall ExistsFile(const TCHAR *pacFile);
		static bool __fastcall RenameFile(const TCHAR *pacNewName, const TCHAR *pacOldName);
		static unsigned long long __fastcall SizeFile(const TCHAR *pacFile);
		static bool __fastcall ReadFile(const TCHAR *pacFile, void *pvData, unsigned int *piSize, unsigned int piOffset = 0);
		//Returns a read only view of the first *piSize bytes of pacFile (of the whole file when *piSize is 0), and stores the size really available in *piSize, so the caller never reads past the end of the view. When mapping is not possible, a heap copy is returned instead (only for small views, so huge files are never allocated and copied just to scan a part of them). *pbMapped tells UnmapFileView how the view must be released
		static void * __fastcall MapFileView(const TCHAR *pacFile, unsigned int *piSize, bool *pbMapped);
		static void __fastcall UnmapFileView(void *pvView, bool pbMapped);
		static bool __fastcall GetFileTimestamp(const TCHAR *pacFile, FILETIME *pudtCreated = nullptr, FILETIME *pudtAccessed  = nullptr, FILETIME *pudtModified  = nullptr);
		static bool __fastcall SetFileTimestamp(const TCHAR *pacFile, const FILETIME *pudtCreated = nullptr, const FILETIME *pudtAccessed = nullptr, const FILETIME *pudtModified = nullptr);
		static bool __fastcall DirectoryCreate(String psDirectory);
		static bool __fastcall DownloadFilePost(const TCHAR *pacServer, const TCHAR *pacPage, const char *pacParameters, void *pvData, unsigned int piSize, bool pbHttps = false);
		//pbReplace: the destination is an existing file that must never be left half written, so it is copied next to it and then swapped
		static bool __fastcall CopyFile(const TCHAR *pacSource, const TCHAR *pacDestination, bool pbReplace = false);
		static const TCHAR * __fastcall ExeVersion(const TCHAR *pacFile);
		static size_t __fastcall GetFileVersionField(const TCHAR *fn, const TCHAR *info, TCHAR *ret, size_t len);
		static const TCHAR * __fastcall GetIniPath(bool pbAllUsers = false);
		static const TCHAR * __fastcall GetIni(const TCHAR *pacSection, const TCHAR *pacKey, const TCHAR *pacDefault);
		static int __fastcall GetIni(const TCHAR *pacSection, const TCHAR *pacKey, int piDefault = 0);
		static bool __fastcall SetIni(const TCHAR *pacSection, const TCHAR *pacKey, const TCHAR *pacValue = _T(""), const TCHAR *pacComment = _T(""));
		static bool __fastcall SetIni(const TCHAR *pacSection, const TCHAR *pacKey, bool pbValue, const TCHAR *pacComment = _T(""));
		static bool __fastcall SetIni(const TCHAR *pacSection, const TCHAR *pacKey, int piValue, const TCHAR *pacComment = _T(""));
		static bool __fastcall SetIni(const TCHAR *pacSection, const TCHAR *pacKey, long long plValue, const TCHAR *pacComment = _T(""));
		static bool __fastcall DeleteIni(const TCHAR *pacSection, const TCHAR *pacKey = nullptr);
		static unsigned int __fastcall Crc32 (const void *pacBuffer, unsigned int piLen, unsigned int piOldCrc = 0xFFFFFFFF);
		static int __fastcall Random (int piMin, int piMax);
		static const TCHAR * __fastcall GetRegistryPath(void);
		static const TCHAR * __fastcall GetRegistry(HKEY phKey, const TCHAR *pacSubkey, const TCHAR *pacName);
		static int __fastcall GetRegistryI(HKEY phKey, const TCHAR *pacSubkey, const TCHAR *pacName);
		static bool __fastcall SetRegistry(HKEY phKey, const TCHAR *pacSubkey, const TCHAR *pacName, const TCHAR *pacValue);
		static bool __fastcall SetRegistry(HKEY phKey, const TCHAR *pacSubkey, const TCHAR *pacName, unsigned int piValue);
		static bool __fastcall SetRegistry(HKEY phKey, const TCHAR *pacSubkey, const TCHAR *pacName, unsigned long long plValue);
		static bool __fastcall DeleteRegistry(HKEY phKey, const TCHAR *pacSubkey);
		static const TCHAR * __fastcall GetLogPath(void);
		static void __fastcall Log(int piLevel, const TCHAR *pacValue, int piDesiredLevel);
		static bool __fastcall LoadForm(TForm *pfrmForm);
		static bool __fastcall SaveForm(TForm *pfrmForm);
		static bool __fastcall CopyToRecycleBin(const TCHAR *pacSource);
		static bool __fastcall SetTaskListProgress(unsigned int piCompleted, unsigned int piTotal);
		static unsigned int __fastcall GetWindowsVersion(void);
		static bool __fastcall IsWindows64(void);
		static bool __fastcall ShutdownWindows(unsigned int piMode = 0);
		static String __fastcall GetApiName(const TCHAR *pacFile);
		static String __fastcall GetShortName(String psLongName);
		static bool __fastcall DeleteFile(const TCHAR *pacFile);
		//piDestinationSize is in characters. _tcsncpy leaves the destination unterminated when the source is longer than the count, so always leave room for the NUL
		static void __fastcall CopyString(TCHAR *pacDestination, size_t piDestinationSize, const TCHAR *pacSource);
};

// ---------------------------------------------------------------------------------------------------------------------------------------------------------------------------
#endif