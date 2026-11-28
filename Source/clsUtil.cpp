// ---------------------------------------------------------------------------------------------------------------------------------------------------------------------------
/*
 3.67. 28/11/2026. FileOptimizer. Removed the unused general purpose helpers (FindProcess, RunProcess, WriteFile, DownloadFile, Serialize, Unserialize, IsWine, and the GetIni for bool/long long/double and SetIni for double overloads): nothing in the program called them. TaskDialogCallbackProc is internal to the module now. CopyToRecycleBin builds its two PATH_MAX buffers writing only the names and the double terminators, instead of clearing 64 KB of stack on every recycled file
 3.66. 26/11/2026. FileOptimizer. GetIni no longer cuts values at the last semicolon (mask lists such as *.bak;*.tmp were truncated): a tab, a semicolon preceded by a blank, or a semicolon starting the value (Windows trims the tabs before it) begins the comment
 3.65. 26/11/2026. FileOptimizer. MapFileView heap copy uses a non throwing allocation (the null check after it could never be reached, an out of memory condition raised an exception instead of returning no view)
 3.64. 24/11/2026. FileOptimizer. CopyFile can replace the destination safely (copy next to it and swap, ReplaceFile/MoveFileEx, with the direct copy as the last option). ReadFile survives I/O errors on mapped pages
 3.63. 23/11/2026. FileOptimizer. SetIni also skips the write when the key exists with an empty value (the check required a non empty one, so every empty option rewrote the whole INI file on each save)
 3.62. 16/11/2026. FileOptimizer. MsgBox timeout travels in the dialog callback data instead of a shared static (workers could mix it), RunProcess does not let the child inherit the parent ends of its pipes (it never saw the end of its input) nor the process handles, SetRegistry creates the key when writing (the browser emulation key was never written when it did not exist), ExeVersion does not clear 4 KB on every call, GetFileVersionField bounded format
 3.61. 11/11/2026. FileOptimizer. SetIni does not rewrite the INI file when it already holds the same value (closing the program or accepting the options wrote about a hundred unchanged values, rewriting the whole file each time)
 3.60. 11/11/2026. FileOptimizer. RunProcess no longer turns long command lines into invalid \\?\ paths on 64 bit Windows, ExeVersion ignores truncated version blocks, ReadFile maps only big reads (a plain read is faster for small ones), CopyFile keeps the copy in the system cache (the next plugin reads it right away, and Windows already uses unbuffered IO by itself for big files), MapFileView/UnmapFileView use tabs
 3.59. 11/11/2026. FileOptimizer. CopyString helper guarantees NUL termination on every truncating copy (including the server controlled donation details), GetIni/GetRegistry/ExeVersion use thread local result buffers, RunProcess captures the child standard error too, DirectoryCreate treats ERROR_ALREADY_EXISTS as success, SetTaskListProgress reuses a single taskbar interface, GetIniPath and GetLogPath share the writable location test, GetFileVersionField validates the translation size, new MapFileView/UnmapFileView give direct read only memory mapped views (with a small heap copy fallback), so the per file buffer allocations and copies of the content scanners are gone
 3.58. 30/10/2026. FileOptimizer. DirectoryCreate no longer changes the String of the caller in place (shared buffer, race between workers), RunProcess always leaves room for the NUL terminator, LoadForm sets position and size at once, simplified MsgBox icons and GetFileVersionField, GetShortName does not clear a 32 KB buffer on every call
 3.57. 29/10/2026. FileOptimizer. RunProcess: main thread guards for cursor and message pump, wait based exit detection (a child legitimately returning 259 no longer loops forever), write end closed before waiting and pipe drained while waiting (no deadlocks, and the child always sees the end of its input). Added ReadPipeAvailable. WriteFile detects partial writes. ExeVersion bounds check. SetIni null comment guard
 3.56. 26/10/2026. FileOptimizer. RenameFile new name API path, ReadFile/WriteFile fall back to regular IO when mapping fails (and empty writes), guaranteed NUL terminated reads (Registry, Download), no GetIni(bool) copy, Random low bits, Unserialize bound, UNC long paths, pipe size
 3.55. 18/10/2026. FileOptimizer. MsgBox buttons and fallback, FindProcess handle leak and access, CopyToRecycleBin data loss, SetRegistry size. Avoid GetShortPathName on short paths. Faster GetFileTimestamp, GetIni and DirectoryCreate
 3.54. 15/10/2026. FileOptimizer. Optimized Crc32 (slicing-by-8), MemMem, CopyFile, FindProcess, Log, GetShortName, IsWine, IsWindows64. Removed useless Escape/Unescape calls. Fixed small leaks and uninitialized variables
 3.53. 22/08/2022. FileOptimizer. Added IsWine.
 3.52. 12/07/2022. FileOptimizer. Improved CopyFile and DeleteFile.
 3.51. 13/10/2018. FileOptimizer. Added RenameFile
 3.50. 15/12/2017. FileOptimizer. Added DeleteFile, GetShortName. Do most operations internally using them
 3.47. 22/11/2017. FileOptimizer. Added EscapeIniValue, UnescapeIniValue, EscapeIniKey, UnescapeIniKey.
 3.46. 21/11/2017. FileOptimizer. Added GetRegistryPath.
 3.45. 17/11/2017. FileOptimizer. Added DeleteRegistry, DeleteIni.
 3.44. 15/11/2017. FileOptimizer. Added SetRegistry for ints.
 3.43. 23/06/2017. FileOptimizer. Backported crc32 from Lamark.
 3.42. 12/01/2017. FileOptimizer. Allow writing comments in .INI files
 3.41. 08/08/2016. FileOptimizer. Added ShutdownWindows
 3.40. 27/12/2015. FileOptimizer. Added GetWindowsVersion since GetVersion on Windows 8.1 or later do not work unless application is manifested
 3.30. 05/05/2015. FileOptimizer. Added 64 bit version of GetIni/SetIni. Added MemMem, CopyFile
 3.25. 05/04/2015. FileOptimizer. Added Random
 3.20. 14/03/2015. FileOptimizer. Added Serialize and Unserialize
 3.10. 20/01/2015. FileOptimizer. Minor tweaks and fixes
 3.00. 23/09/2012. FileOptimizer. Added LoadForm, SaveForm, CopyToRecycleBin, SetTaskListProgress
 2.00. 09/08/2012. FileOptimizer. Added clsUtil static class wrapper, optimized SizeFile, mapped to TCHAR and merged clsPreferences common functions
 1.00. 18/08/2011. TBClamAV. Initial version
 */
// ---------------------------------------------------------------------------------------------------------------------------------------------------------------------------
#include "clsUtil.h"
#include "cppMain.h"
#include <new>


// ---------------------------------------------------------------------------------------------------------------------------------------------------------------------------
const void * __fastcall clsUtil::MemMem (const void *buf, size_t buf_len, const void *byte_sequence, size_t byte_sequence_len)
{
	const unsigned char *bf = (const unsigned char *) buf;
	const unsigned char *bs = (const unsigned char *) byte_sequence;
	const unsigned char *p  = bf;
	const unsigned char *pe;


	//Nothing to look for, or sequence is longer than buffer
	if ((byte_sequence_len == 0) || (byte_sequence_len > buf_len))
	{
		return (nullptr);
	}

	//One past the last position where the sequence can start, so memchr never scans the tail
	pe = bf + (buf_len - byte_sequence_len) + 1;
	unsigned int b = *bs & 0xFF;
	while ((p = (const unsigned char *) memchr(p, b, pe - p)) != nullptr)
	{
		//First byte already matches
		if ((memcmp(p + 1, bs + 1, byte_sequence_len - 1)) == 0)
		{
			return (p);
		}
		p++;
	}
	return (nullptr);
}



// ---------------------------------------------------------------------------------------------------------------------------------------------------------------------------
//Dialog callback of the timed message boxes. The timeout travels in the callback data of its own dialog, so message boxes shown at the same time never share it. Internal to this module
#pragma argsused
static HRESULT CALLBACK TaskDialogCallbackProc(HWND phWnd, UINT uNotification, WPARAM wParam, LPARAM lParam, LONG_PTR dwRefData)
{
	HRESULT hResult = S_OK;


	if (uNotification == TDN_CREATED)
	{
		//The range is a 16 bit value, so a percentage is used: a timeout over 65 seconds would not fit in it
		SendMessage(phWnd, TDM_SET_PROGRESS_BAR_RANGE, 0, MAKELONG(0, 100));
	}
	else if (uNotification == TDN_TIMER)
	{
		unsigned int iTimeout = (unsigned int) dwRefData;
		SendMessage(phWnd, TDM_SET_PROGRESS_BAR_POS, (WPARAM) min((unsigned long long) 100, ((unsigned long long) wParam * 100) / max(iTimeout, 1u)), 0);
		if (wParam > iTimeout)
		{
			PostMessage(phWnd, WM_CLOSE, 0, 0);
			hResult = S_FALSE;
		}
	}
	return (hResult);
}



// ---------------------------------------------------------------------------------------------------------------------------------------------------------------------------
int __fastcall clsUtil::MsgBox(HWND phWnd, const TCHAR *pacText, const TCHAR *pacTitle, unsigned int piType, unsigned int piTimeout)
{
	static HMODULE hUser32 = NULL;
	static HMODULE hComCtl32 = NULL;
	int iButton = 0;
	TASKDIALOGCONFIG udtFlags = {};
	bool bShown = false;
	unsigned int iButtons = piType & 0x0000000F;	//MB_TYPEMASK. Types share bits, so they cannot be checked one by one with a mask


	//DLLs not already loaded, so load them
	if (!hComCtl32)
	{
		hComCtl32 = LoadLibraryEx(_T("COMCTL32.DLL"), nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
	}
	if (hComCtl32)
	{
		typedef HRESULT (WINAPI TaskDialogType)(const TASKDIALOGCONFIG *pTaskConfig, int *pnButton, int *pnRadioButton, BOOL *pfVerificationFlagChecked);
		TaskDialogType *TaskDialogIndirect = (TaskDialogType *) GetProcAddress(hComCtl32, "TaskDialogIndirect");
		if (TaskDialogIndirect)
		{
			udtFlags.cbSize = sizeof(udtFlags);
			udtFlags.hwndParent = phWnd;
			udtFlags.dwFlags = TDF_ALLOW_DIALOG_CANCELLATION | TDF_CAN_BE_MINIMIZED | TDF_SIZE_TO_CONTENT;
			if (piTimeout != 0)
			{
				udtFlags.dwFlags |= TDF_CALLBACK_TIMER | TDF_SHOW_PROGRESS_BAR;
				udtFlags.pfCallback = (PFTASKDIALOGCALLBACK) TaskDialogCallbackProc;
				udtFlags.lpCallbackData = (LONG_PTR) piTimeout;
			}

			if (iButtons == MB_ABORTRETRYIGNORE)
			{
				udtFlags.dwCommonButtons = TDCBF_CANCEL_BUTTON | TDCBF_RETRY_BUTTON | TDCBF_CLOSE_BUTTON;
				udtFlags.nDefaultButton = IDRETRY;
				iButton = IDCANCEL;
			}
			else if (iButtons == MB_CANCELTRYCONTINUE)
			{
				udtFlags.dwCommonButtons = TDCBF_CANCEL_BUTTON | TDCBF_RETRY_BUTTON | TDCBF_OK_BUTTON;
				udtFlags.nDefaultButton = IDRETRY;
				iButton = IDCANCEL;
			}
			else if (iButtons == MB_OKCANCEL)
			{
				udtFlags.dwCommonButtons = TDCBF_OK_BUTTON | TDCBF_CANCEL_BUTTON;
				udtFlags.nDefaultButton = IDOK;
				iButton = IDCANCEL;
			}
			else if (iButtons == MB_RETRYCANCEL)
			{
				udtFlags.dwCommonButtons = TDCBF_RETRY_BUTTON | TDCBF_CANCEL_BUTTON;
				udtFlags.nDefaultButton = IDRETRY;
				iButton = IDCANCEL;
			}
			else if (iButtons == MB_YESNO)
			{
				udtFlags.dwCommonButtons = TDCBF_YES_BUTTON | TDCBF_NO_BUTTON;
				udtFlags.nDefaultButton = IDYES;
				iButton = IDNO;
			}
			else if (iButtons == MB_YESNOCANCEL)
			{
				udtFlags.dwCommonButtons = TDCBF_YES_BUTTON | TDCBF_NO_BUTTON | TDCBF_CANCEL_BUTTON;
				udtFlags.nDefaultButton = IDYES;
				iButton = IDCANCEL;
			}
			else
			{
				udtFlags.dwCommonButtons = TDCBF_OK_BUTTON;
				udtFlags.nDefaultButton = IDOK;
				iButton = IDOK;
			}
			//Icons share bits too, so compare the whole icon field. MB_ICONQUESTION has no icon
			switch (piType & MB_ICONMASK)
			{
				case MB_ICONEXCLAMATION:
					udtFlags.pszMainIcon = TD_WARNING_ICON;
					break;
				case MB_ICONINFORMATION:
					udtFlags.pszMainIcon = TD_INFORMATION_ICON;
					break;
				case MB_ICONSTOP:
					udtFlags.pszMainIcon = TD_ERROR_ICON;
					break;
				default:
					udtFlags.pszMainIcon = nullptr;
					break;
			}

			udtFlags.pszWindowTitle = (const TCHAR *) pacTitle;
			if (_tcslen(pacText) < 256)
			{
				udtFlags.pszMainInstruction = (const TCHAR *) pacText;
			}
			else
			{
				udtFlags.pszMainInstruction = (const TCHAR *) pacTitle;
				udtFlags.pszContent = (const TCHAR *) pacText;
			}
			bShown = SUCCEEDED((*TaskDialogIndirect)(&udtFlags, &iButton, nullptr, nullptr));
		}
	}
	//Fallback when library not loaded, TaskDialogIndirect not exists, or it failed (otherwise the message would never be shown)
	if (!bShown)
	{
		iButton = 0;
		if (piTimeout == 0)
		{
			iButton = MessageBox(phWnd, pacText, pacTitle, piType);
		}
		else
		{
			if (!hUser32)
			{
				hUser32 = LoadLibraryEx(_T("USER32.DLL"), nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
			}
			if (hUser32)
			{
				typedef int (__stdcall *MSGBOXWAPI)(IN HWND hWnd, IN LPCWSTR lpText, IN LPCWSTR lpCaption, IN UINT uType, IN WORD wLanguageId, IN DWORD dwMilliseconds);
				MSGBOXWAPI MessageBoxTimeout = (MSGBOXWAPI) GetProcAddress(hUser32, "MessageBoxTimeoutW");
				if (MessageBoxTimeout)
				{
					iButton = (*MessageBoxTimeout)(phWnd, pacText, pacTitle, (unsigned int) piType, 0, (unsigned long) piTimeout);
				}
			}
		}
	}
	return (iButton);
}



// ---------------------------------------------------------------------------------------------------------------------------------------------------------------------------
unsigned long __fastcall clsUtil::ReadPipeAvailable(HANDLE hRead, void *pvData, unsigned int piSize)
{
	unsigned long lRead = 0;


	//Check how many bytes are waiting in the pipe, so when PeekNamedPipe fails lRead is not used uninitialized. It also avoids reading when the pipe is empty, which would block until the child writes or exits
	if ((hRead != nullptr) && (piSize > 0))
	{
		unsigned long lAvailable = 0;
		if ((PeekNamedPipe(hRead, nullptr, 0, nullptr, &lAvailable, nullptr)) && (lAvailable > 0))
		{
			::ReadFile(hRead, pvData, piSize, &lRead, nullptr);
		}
	}
	return (lRead);
}



// ---------------------------------------------------------------------------------------------------------------------------------------------------------------------------
bool __fastcall clsUtil::ExistsFile(const TCHAR *pacFile)
{
	unsigned int iAttributes = GetFileAttributes(GetApiName(pacFile).c_str());
	return (iAttributes != INVALID_FILE_ATTRIBUTES);
}



// ---------------------------------------------------------------------------------------------------------------------------------------------------------------------------
bool __fastcall clsUtil::RenameFile(const TCHAR *pacNewName, const TCHAR *pacOldName)
{
	return(MoveFileEx(GetApiName(pacOldName).c_str(), GetApiName(pacNewName).c_str(), MOVEFILE_COPY_ALLOWED | MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH));
}



// ---------------------------------------------------------------------------------------------------------------------------------------------------------------------------
unsigned long long __fastcall clsUtil::SizeFile(const TCHAR *pacFile)
{
	unsigned long long lSize = 0;
	WIN32_FILE_ATTRIBUTE_DATA udtFileAttribute;


	if (GetFileAttributesEx(GetApiName(pacFile).c_str(), GetFileExInfoStandard, (void*) &udtFileAttribute))
	{
		lSize = ((unsigned long long) udtFileAttribute.nFileSizeHigh << 32) + udtFileAttribute.nFileSizeLow;
	}
	return (lSize);
}



// ---------------------------------------------------------------------------------------------------------------------------------------------------------------------------
bool __fastcall clsUtil::ReadFile(const TCHAR *pacFile, void *pvData, unsigned int *piSize, unsigned int piOffset)
{
	unsigned int iSize = 0;
	bool bRes = false;
	HANDLE hMapping = nullptr;
	//Requested size, so the reported one can always be set to what was really read, also on failure
	unsigned int iRequestedSize = *piSize;


	*piSize = 0;
	unsigned long lFlags = FILE_FLAG_SEQUENTIAL_SCAN;
	if ((iRequestedSize != 0) && (iRequestedSize <= 64 * 1024))
	{
		lFlags = FILE_ATTRIBUTE_NORMAL;
	}
	//Allow others to read and write the file meanwhile, or files in use by other programs could not be read
	HANDLE hFile = CreateFile(GetApiName(pacFile).c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING, lFlags, nullptr);
	if (hFile != INVALID_HANDLE_VALUE)
	{
		//Get the real file size, so the requested size can be clamped to it: mapping or reading past the end of the file is undefined
		LARGE_INTEGER lFileSize;
		if (GetFileSizeEx(hFile, &lFileSize))
		{
			//Nothing to read: empty file, or offset beyond the end of the file
			if (((unsigned long long) lFileSize.QuadPart) > piOffset)
			{
				unsigned long long lAvailable = ((unsigned long long) lFileSize.QuadPart) - piOffset;
				//Read the whole file
				if (iRequestedSize == 0)
				{
					//Size is an unsigned int, so clamp it to 4 GB
					iSize = (lAvailable < (unsigned long long) UINT_MAX) ? (unsigned int) lAvailable : UINT_MAX;
				}
				//Read a chunk: clamp it to what is really available
				else
				{
					iSize = (((unsigned long long) iRequestedSize) < lAvailable) ? iRequestedSize : (unsigned int) lAvailable;
				}
				if (iSize > 0)
				{
					// Use file mapped IO, only when reading from the start (the view always starts at offset 0) and for big reads: creating the mapping costs more than reading a small chunk directly
					if ((piOffset == 0) && (iSize > (64 * 1024)))
					{
						hMapping = CreateFileMapping(hFile, nullptr, PAGE_READONLY, 0, iSize, nullptr);
						if (hMapping != nullptr)
						{
							void *pacBuffer = MapViewOfFile(hMapping, FILE_MAP_READ, 0, 0, iSize);
							if (pacBuffer != nullptr)
							{
								try
								{
									memcpy(pvData, pacBuffer, iSize);
									bRes = true;
								}
								catch (...)
								{
									//I/O error while reading the mapped pages (the file was truncated by another program, a network share was lost...): the regular read below reports it
									bRes = false;
								}
								if (!UnmapViewOfFile(pacBuffer))
								{
									bRes = false;
								}
							}
						}
					}
					// Use regular IO, also when mapping is not possible (offset, network shares...), or nothing would be read
					if (!bRes)
					{
						if (piOffset != 0)
						{
							//SetFilePointerEx, so offsets over 2 GB do not overflow the signed long offset of SetFilePointer
							LARGE_INTEGER lOffset;
							lOffset.QuadPart = piOffset;
							SetFilePointerEx(hFile, lOffset, nullptr, FILE_BEGIN);
						}
						//Read the clamped size, so we never read past the end of the file
						bRes = (::ReadFile(hFile, pvData, iSize, (unsigned long *) &iSize, nullptr) != 0);
					}
				}
				*piSize = iSize;
			}
			else
			{
				*piSize = 0;
			}
		}
	}
	if (hMapping)
	{
		CloseHandle(hMapping);
	}
	if (hFile != INVALID_HANDLE_VALUE)
	{
		CloseHandle(hFile);
	}
	return (bRes);
}



// ---------------------------------------------------------------------------------------------------------------------------------------------------------------------------
//Returns a read only view of the first *piSize bytes of pacFile (of the whole file when *piSize is 0), and stores the size really available in *piSize, so the caller never reads past the end of the view. When mapping is not possible, a heap copy is returned instead (only for small views, so huge files are never allocated and copied just to scan a part of them). *pbMapped tells UnmapFileView how the view must be released
void * __fastcall clsUtil::MapFileView(const TCHAR *pacFile, unsigned int *piSize, bool *pbMapped)
{
	void *pvRes = nullptr;
	//Requested size, so the reported one can always be set to what is really available, also on failure
	unsigned int iRequestedSize = *piSize;
	unsigned int iSize = 0;


	*pbMapped = false;
	*piSize = 0;
	//Allow others to read and write the file meanwhile, or files in use by other programs could not be read
	HANDLE hFile = CreateFile(GetApiName(pacFile).c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING, FILE_FLAG_SEQUENTIAL_SCAN, nullptr);
	if (hFile != INVALID_HANDLE_VALUE)
	{
		//Get the real file size, so the view can be clamped to it: mapping or reading past the end of the file is undefined
		LARGE_INTEGER lFileSize;
		if ((GetFileSizeEx(hFile, &lFileSize)) && (lFileSize.QuadPart > 0))
		{
			//Size is an unsigned int, so clamp it to 4 GB
			unsigned long long lAvailable = ((unsigned long long) lFileSize.QuadPart > (unsigned long long) UINT_MAX) ? (unsigned long long) UINT_MAX : (unsigned long long) lFileSize.QuadPart;
			//Clamp the requested size to what is really available, so the view never goes past the end of the file
			iSize = ((iRequestedSize == 0) || (((unsigned long long) iRequestedSize) > lAvailable)) ? (unsigned int) lAvailable : iRequestedSize;
			HANDLE hMapping = CreateFileMapping(hFile, nullptr, PAGE_READONLY, 0, iSize, nullptr);
			if (hMapping != nullptr)
			{
				//The view keeps the mapping alive, so the mapping handle can be closed right here
				pvRes = MapViewOfFile(hMapping, FILE_MAP_READ, 0, 0, iSize);
				CloseHandle(hMapping);
				if (pvRes != nullptr)
				{
					*pbMapped = true;
				}
			}
			//Mapping is not possible on some file systems and network shares, so fall back to a heap copy, but only for small views: allocating and copying a huge file would defeat the purpose of mapping
			if ((pvRes == nullptr) && (iSize <= (16 * 1024 * 1024)))
			{
				//nothrow, or a failed allocation would throw instead of reaching the check below
				pvRes = new (std::nothrow) unsigned char[iSize];
				if (pvRes != nullptr)
				{
					unsigned long lRead;
					if ((::ReadFile(hFile, pvRes, iSize, &lRead, nullptr) != 0) && (lRead > 0))
					{
						//Report what was really read, in case the file changed its size meanwhile
						iSize = lRead;
					}
					else
					{
						delete[] (unsigned char *) pvRes;
						pvRes = nullptr;
					}
				}
			}
			if (pvRes == nullptr)
			{
				iSize = 0;
			}
			*piSize = iSize;
		}
		CloseHandle(hFile);
	}
	return (pvRes);
}



// ---------------------------------------------------------------------------------------------------------------------------------------------------------------------------
void __fastcall clsUtil::UnmapFileView(void *pvView, bool pbMapped)
{
	if (pvView != nullptr)
	{
		//The view was a real mapping, or a heap copy when mapping was not possible
		if (pbMapped)
		{
			UnmapViewOfFile(pvView);
		}
		else
		{
			delete[] (unsigned char *) pvView;
		}
	}
}



// ---------------------------------------------------------------------------------------------------------------------------------------------------------------------------
bool __fastcall clsUtil::GetFileTimestamp(const TCHAR *pacFile, FILETIME *pudtCreated, FILETIME *pudtAccessed, FILETIME *pudtModified)
{
	bool bRes = false;
	WIN32_FILE_ATTRIBUTE_DATA udtFileAttribute;


	//A single call, with no need to open the file (and so it also works with files in use)
	if (GetFileAttributesEx(GetApiName(pacFile).c_str(), GetFileExInfoStandard, (void *) &udtFileAttribute))
	{
		if (pudtCreated)
		{
			*pudtCreated = udtFileAttribute.ftCreationTime;
		}
		if (pudtAccessed)
		{
			*pudtAccessed = udtFileAttribute.ftLastAccessTime;
		}
		if (pudtModified)
		{
			*pudtModified = udtFileAttribute.ftLastWriteTime;
		}
		bRes = true;
	}
	return (bRes);
}



// ---------------------------------------------------------------------------------------------------------------------------------------------------------------------------
bool __fastcall clsUtil::SetFileTimestamp(const TCHAR *pacFile, const FILETIME *pudtCreated, const FILETIME *pudtAccessed, const FILETIME *pudtModified)
{
	bool bRes = false;


	//Only attributes are changed, so there is no need for write access to the data (which fails with read only files)
	HANDLE hFile = CreateFile(GetApiName(pacFile).c_str(), FILE_WRITE_ATTRIBUTES, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
	if (hFile != INVALID_HANDLE_VALUE)
	{
		bRes = SetFileTime(hFile, pudtCreated, pudtAccessed, pudtModified);
		CloseHandle(hFile);
	}
	return (bRes);
}



// ---------------------------------------------------------------------------------------------------------------------------------------------------------------------------
bool __fastcall clsUtil::DirectoryCreate(String psDirectory)
{
	bool bRes = false;


	if (!EndsStr("\\", psDirectory))
	{
		psDirectory += "\\";
	}
	//Already exists, so there is nothing to create
	unsigned int iAttributes = GetFileAttributes(GetApiName(psDirectory.c_str()).c_str());
	if ((iAttributes != INVALID_FILE_ATTRIBUTES) && ((iAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0))
	{
		return (true);
	}
	const TCHAR *acDirectory = psDirectory.c_str();
	unsigned int iDirectoryLen = (unsigned int) psDirectory.Length();

	for (unsigned int iCount = 0; iCount < iDirectoryLen; iCount++)
	{
		//If found a backslash we try to create that component (it should end with backslash). It works on a copy of the component, since changing the characters in place would also change the String of the caller, which can share the buffer and be used by other threads
		if (acDirectory[iCount] == '\\')
		{
			//A component that already exists is not a failure (it can even have been created concurrently by another worker), so only a real error makes the whole creation fail
			if (CreateDirectory(GetApiName(psDirectory.SubString(1, iCount).c_str()).c_str(), nullptr) != 0)
			{
				bRes = true;
			}
			else
			{
				bRes = (GetLastError() == ERROR_ALREADY_EXISTS);
			}
		}
	}
	return (bRes);
}



// ---------------------------------------------------------------------------------------------------------------------------------------------------------------------------
//Builds the user agent ("Name/Version") in the caller buffer, which is reused for the response afterwards. It is left empty when the executable name cannot be read
static void BuildUserAgent(void *pvData, unsigned int piSize)
{
	TCHAR *pacAgent = (TCHAR *) pvData;
	size_t iChars = piSize / sizeof(TCHAR);


	if (iChars == 0)
	{
		return;
	}
	pacAgent[0] = NULL;
	if ((iChars > 1) && (GetModuleFileName(nullptr, pacAgent, (unsigned long) (iChars - 1)) != 0))
	{
		String sAgent = Application->Name + _T("/") + clsUtil::ExeVersion(pacAgent);
		clsUtil::CopyString(pacAgent, iChars, sAgent.c_str());
	}
}



// ---------------------------------------------------------------------------------------------------------------------------------------------------------------------------
//Reads the response until its end or until the buffer is full (leaving room for the NUL), since a single InternetReadFile can return less than what is available. Returns false when reading failed before getting anything
static bool ReadInternetResponse(HINTERNET phRequest, void *pvData, unsigned int piSize, unsigned long *plTotal)
{
	unsigned char *pacData = (unsigned char *) pvData;
	unsigned long lTotal = 0;
	unsigned long lMax = (piSize > sizeof(TCHAR)) ? (unsigned long) (piSize - sizeof(TCHAR)) : 0;
	bool bRes = true;


	memset(pvData, 0, piSize);
	while (lTotal < lMax)
	{
		unsigned long lRead = 0;
		if (!InternetReadFile(phRequest, pacData + lTotal, lMax - lTotal, &lRead))
		{
			bRes = (lTotal > 0);
			break;
		}
		if (lRead == 0)
		{
			break;
		}
		lTotal += lRead;
	}
	*plTotal = lTotal;
	return (bRes);
}



// ---------------------------------------------------------------------------------------------------------------------------------------------------------------------------
bool __fastcall clsUtil::DownloadFilePost(const TCHAR *pacServer, const TCHAR *pacPage, const char *pacParameters, void *pvData, unsigned int piSize, bool pbHttps)
{
	bool bRes = false;


	//ToDo: Use UrlGetPart https://msdn.microsoft.com/en-us/library/windows/desktop/bb773781(v=vs.85).aspx
	BuildUserAgent(pvData, piSize);
	HINTERNET hInternet = InternetOpen((const TCHAR *) pvData, INTERNET_OPEN_TYPE_PRECONFIG, nullptr, nullptr, NULL);
	if (hInternet)
	{

		unsigned int iFlags = INTERNET_DEFAULT_HTTP_PORT;
		if (pbHttps)
		{
			iFlags = INTERNET_DEFAULT_HTTPS_PORT;
		}
		HINTERNET hConnect = InternetConnect(hInternet, pacServer, iFlags, nullptr, nullptr, INTERNET_SERVICE_HTTP, 0, 0);
		if (hConnect)
		{
			iFlags = INTERNET_FLAG_RELOAD | INTERNET_FLAG_NO_CACHE_WRITE | INTERNET_FLAG_NO_UI | INTERNET_FLAG_IGNORE_CERT_CN_INVALID | INTERNET_FLAG_IGNORE_CERT_DATE_INVALID;
			if (pbHttps)
			{
				iFlags |= INTERNET_FLAG_SECURE;
			}
			HINTERNET hRequest = HttpOpenRequest(hConnect, _T("POST"), pacPage, HTTP_VERSION, _T(""), nullptr, iFlags, 0);
			if (hRequest)
			{
				if (pbHttps)
				{
					unsigned int iCertificate = 0;
					InternetSetOption(hRequest, INTERNET_OPTION_SECURITY_SELECT_CLIENT_CERT, &iCertificate, sizeof(iCertificate));
				}

				TCHAR acHeaders[] = _T("Content-Type: application/x-www-form-urlencoded");
				if (HttpSendRequest(hRequest, acHeaders, _tcslen(acHeaders), (void *) pacParameters, strlen((const char *) pacParameters)))
				{
					unsigned long lRead;
					bRes = ((ReadInternetResponse(hRequest, pvData, piSize, &lRead)) && (lRead > 0));
				}
				InternetCloseHandle(hRequest);
			}
			InternetCloseHandle(hConnect);
		}
	}
	return (bRes);
}




// ---------------------------------------------------------------------------------------------------------------------------------------------------------------------------
bool __fastcall clsUtil::CopyFile(const TCHAR *pacSource, const TCHAR *pacDestination, bool pbReplace)
{
	String sSource = GetApiName(pacSource);
	String sDestination = GetApiName(pacDestination);

	//Replacing an existing file (the optimized copy over the original): it is copied next to it first, and swapped only when complete, so the original is never left half written if the program, the system or the power fails in the middle. It must be on the same volume, which is why it is not the temporary directory. ReplaceFile keeps the attributes, the permissions and the alternate streams of the original; MoveFileEx is the second option (file systems where ReplaceFile is not available), and the direct copy below is the last one (such as a folder where new files cannot be created), so it never works worse than before
	if (pbReplace)
	{
		String sTemp = sDestination + ".fo~";
		if (CopyFileEx(sSource.c_str(), sTemp.c_str(), nullptr, nullptr, nullptr, COPY_FILE_ALLOW_DECRYPTED_DESTINATION))
		{
			//REPLACEFILE_IGNORE_MERGE_ERRORS (2): a failure keeping the attributes or the permissions must not prevent the replacement
			if ((ReplaceFile(sDestination.c_str(), sTemp.c_str(), nullptr, 2, nullptr, nullptr)) || (MoveFileEx(sTemp.c_str(), sDestination.c_str(), MOVEFILE_REPLACE_EXISTING)))
			{
				return (true);
			}
			//The API one: inside this class DeleteFile is the member that would convert the path again
			::DeleteFile(sTemp.c_str());
		}
	}

	//Buffered on purpose: the copy is read again right away (by the next plugin), so keeping it in the system cache is faster, and Windows already switches to unbuffered IO by itself for big files. A second try without flags was useless once the unbuffered flag is gone
	return (CopyFileEx(sSource.c_str(), sDestination.c_str(), nullptr, nullptr, nullptr, COPY_FILE_ALLOW_DECRYPTED_DESTINATION) != 0);
}




// --------------------------------------------------------------------------------------------------
const TCHAR * __fastcall clsUtil::ExeVersion(const TCHAR *pacFile)
{
	unsigned int iVersionSize;
	TCHAR *pacVersionData;
	void *a;
	VS_FIXEDFILEINFO udtVersionInfo;
	//Multithreaded optimization: the result buffer is thread local, or concurrent workers would overwrite each other
	static __thread TCHAR acRes[2048];

	//_stprintf terminates it when it succeeds, so only the first character has to be cleared for the other paths
	acRes[0] = NULL;
	String sFile = GetApiName(pacFile);
	iVersionSize = GetFileVersionInfoSize(sFile.c_str(), nullptr);
	if (iVersionSize == 0)
	{
		return (acRes);
	}
	pacVersionData = new TCHAR[(iVersionSize + sizeof(TCHAR) - 1) / sizeof(TCHAR)];
	if (pacVersionData)
	{
		if (GetFileVersionInfo(sFile.c_str(), 0, iVersionSize, pacVersionData))
		{
			//A block smaller than the structure would leave its trailing fields uninitialized, so it is ignored
			if ((VerQueryValue(pacVersionData, _T("\\"), &a, &iVersionSize)) && (iVersionSize >= sizeof(udtVersionInfo)))
			{
				memcpy(&udtVersionInfo, a, sizeof(udtVersionInfo));
				_stprintf(acRes, _T("%d.%01d%01d.%d"), HIWORD(udtVersionInfo.dwFileVersionMS), LOWORD(udtVersionInfo.dwFileVersionMS), HIWORD(udtVersionInfo.dwFileVersionLS), LOWORD(udtVersionInfo.dwFileVersionLS));
			}
		}
		delete[] pacVersionData;
	}
	return (acRes);
}



//---------------------------------------------------------------------------
size_t __fastcall clsUtil::GetFileVersionField(const TCHAR *fn, const TCHAR *info, TCHAR *ret, size_t len)
{
	DWORD hVersion;
	String sFile = GetApiName(fn);
	DWORD vis = GetFileVersionInfoSize(sFile.c_str(), &hVersion);


	if (vis == 0)
	{
		return (0);
	}
	void *vData;
	vData = (void *) new TCHAR[(vis + sizeof(TCHAR) - 1) / sizeof(TCHAR)];

	if (!GetFileVersionInfo(sFile.c_str(), hVersion, vis, vData))
	{
		delete[] (TCHAR *) vData;
		return (0);
	}
	TCHAR vn[100];
	_tcscpy(vn, _T("\\VarFileInfo\\Translation"));

	LPVOID transblock;
	UINT vsize;

	BOOL res = VerQueryValue(vData, vn, &transblock, &vsize);
	if ((!res) || (vsize < sizeof(DWORD)))
	{
		delete[] (TCHAR *) vData;
		return (0);
	}
	// Swap the words so wsprintf will print the lang-charset in the correct format.
	*(DWORD *) transblock = MAKELONG(HIWORD(*(DWORD *) transblock), LOWORD(*(DWORD *) transblock));
	//Bounded, since info comes from the caller
	_sntprintf(vn, (sizeof(vn) / sizeof(TCHAR)) - 1, _T("\\StringFileInfo\\%08lx\\%s"), *(DWORD *) transblock, info);
	vn[(sizeof(vn) / sizeof(TCHAR)) - 1] = NULL;
	TCHAR *ver;
	res = VerQueryValue(vData, vn, (LPVOID*) &ver, &vsize);
	if (!res)
	{
		delete[] (TCHAR *) vData;
		return(0);
	}
	size_t vlen = _tcsclen(ver);
	//len must be checked, or len - 1 would underflow and copy past the destination buffer
	if ((ret != nullptr) && (len > 0))
	{
		//Copies up to len - 1 characters, and always terminates it
		lstrcpyn(ret, ver, (int) len);
	}
	delete[] (TCHAR *) vData;
	return(vlen);
}




// ---------------------------------------------------------------------------------------------------------------------------------------------------------------------------
//Check if we can write to that location, without leaving an empty file behind just for the test (OPEN_ALWAYS created it): if it already exists just open it, otherwise create it and remove it right away. Shared by GetIniPath and GetLogPath
static bool FileLocationWritable(const TCHAR *pacFile)
{
	HANDLE hFile = CreateFile(pacFile, GENERIC_WRITE, FILE_SHARE_WRITE, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
	if (hFile != INVALID_HANDLE_VALUE)
	{
		CloseHandle(hFile);
		return (true);
	}
	hFile = CreateFile(pacFile, GENERIC_WRITE, FILE_SHARE_WRITE, nullptr, CREATE_NEW, FILE_ATTRIBUTE_NORMAL, nullptr);
	if (hFile != INVALID_HANDLE_VALUE)
	{
		CloseHandle(hFile);
		::DeleteFile(pacFile);
		return (true);
	}
	return (false);
}



// ---------------------------------------------------------------------------------------------------------------------------------------------------------------------------
const TCHAR * __fastcall clsUtil::GetIniPath(bool pbAllUsers)
{
	static TCHAR acPath[PATH_MAX + 1] = _T("");
	static TCHAR acPathAllUsers[PATH_MAX + 1] = _T("");


	// Check if we already have it cached
	if ((acPath[0] == NULL) && (acPathAllUsers[0] == NULL))
	{
		TCHAR acTmp[2048] = _T("");
		TCHAR *pcExtension;


		if (GetModuleFileName(nullptr, acTmp, (sizeof(acTmp) / sizeof(TCHAR)) - 1) != 0)
		{
			//Remove the extension only if there is one, or _tcsrchr would return NULL and crash
			pcExtension = _tcsrchr(acTmp, '.');
			if (pcExtension)
			{
				*pcExtension = NULL;
			}
		}
		_tcscat(acTmp, _T(".ini"));

		bool bWritable = FileLocationWritable(acTmp);

		if (bWritable)
		{
			clsUtil::CopyString(acPath, sizeof(acPath) / sizeof(TCHAR), acTmp);
			clsUtil::CopyString(acPathAllUsers, sizeof(acPathAllUsers) / sizeof(TCHAR), acPath);
		}
		else
		{
			const TCHAR *pacUserProfile = _tgetenv(_T("USERPROFILE"));
			if (pacUserProfile)
			{
				_stprintf(acPath, _T("%s\\%s"), pacUserProfile, (Application->Name + ".ini").c_str());
			}
			else
			{
				//No user profile, so fall back to the program directory (later writes will fail silently, as with any other unwritable location)
				clsUtil::CopyString(acPath, sizeof(acPath) / sizeof(TCHAR), acTmp);
			}
			clsUtil::CopyString(acPathAllUsers, sizeof(acPathAllUsers) / sizeof(TCHAR), acTmp);
		}
		_tcscpy(acPath, GetShortName((String) acPath).c_str());
		_tcscpy(acPathAllUsers, GetShortName((String) acPathAllUsers).c_str());
	}
	if (pbAllUsers)
	{
		return (acPathAllUsers);
	}
	else
	{
		return (acPath);
	}
}


// ---------------------------------------------------------------------------------------------------------------------------------------------------------------------------
const TCHAR * __fastcall clsUtil::GetIni(const TCHAR *pacSection, const TCHAR *pacKey, const TCHAR *pacDefault)
{
	//Multithreaded optimization: the result buffer is thread local, or concurrent workers would overwrite each other
	static __thread TCHAR acRes[2048];
	TCHAR *pcSemicolon;


	//GetPrivateProfileString always terminates the string, so there is no need to clear the whole buffer
	//Read using an empty default, or a non-empty one would skip the current user file when the key is only present there
	acRes[0] = NULL;
	GetPrivateProfileString(pacSection, pacKey, _T(""), acRes, (sizeof(acRes) / sizeof(TCHAR)) - 1, GetIniPath(true));
	//If both are the same file, reading it again would give the same result
	if ((acRes[0] == NULL) && (_tcscmp(GetIniPath(true), GetIniPath(false)) != 0))
	{
		GetPrivateProfileString(pacSection, pacKey, _T(""), acRes, (sizeof(acRes) / sizeof(TCHAR)) - 1, GetIniPath(false));
	}
	//Key not found in any of them, so return the default
	if ((acRes[0] == NULL) && (pacDefault))
	{
		lstrcpyn(acRes, pacDefault, (sizeof(acRes) / sizeof(TCHAR)));
	}

	//Remove comments. SetIni writes them after tabs, so a tab ends the value, and semicolons inside it (such as in a mask list) are kept. Windows trims the blanks before the value, so a value that is only a comment starts with the semicolon. Without a tab, a semicolon preceded by a blank also starts a hand written comment
	pcSemicolon = _tcschr(acRes, '\t');
	if (!pcSemicolon)
	{
		pcSemicolon = _tcsstr(acRes, _T(" ;"));
	}
	if ((!pcSemicolon) && (acRes[0] == ';'))
	{
		pcSemicolon = acRes;
	}
	if (pcSemicolon)
	{
		*pcSemicolon = NULL;
	}

	return (acRes);
}



// ---------------------------------------------------------------------------------------------------------------------------------------------------------------------------
int __fastcall clsUtil::GetIni(const TCHAR *pacSection, const TCHAR *pacKey, int piDefault)
{
	TCHAR acDefault[32];


	_itot(piDefault, acDefault, 10);
	return (_ttoi(GetIni(pacSection, pacKey, acDefault)));
}



// ---------------------------------------------------------------------------------------------------------------------------------------------------------------------------
bool __fastcall clsUtil::SetIni(const TCHAR *pacSection, const TCHAR *pacKey, const TCHAR *pacValue, const TCHAR *pacComment)
{
	bool bRes;
	TCHAR acCurrent[2048];
	String sValue = (String) pacValue;


	//Append the comment to the value, separated by tabs
	if ((pacComment != nullptr) && (pacComment[0] != NULL))
	{
		sValue = sValue + _T("\t\t; ") + pacComment;
	}

	//Every write rewrites the whole INI file, so skip it when the file already holds exactly this value. GetPrivateProfileString trims the leading blanks, so they are also skipped in the expected value. The default is a marker that can not be equal to it, to tell a missing key from an empty one. Cache keys are almost always new, so there the reading would only add cost
	const TCHAR *pcExpected = sValue.c_str() + _tcsspn(sValue.c_str(), _T(" \t"));
	bool bCheck = (_tcscmp(pacSection, _T("Cache")) != 0);
	if (bCheck)
	{
		//The marker is returned when the key is missing, so an existing empty value (very common in the options) is also recognized as unchanged, and does not rewrite the file every time
		GetPrivateProfileString(pacSection, pacKey, _T("\x01"), acCurrent, (sizeof(acCurrent) / sizeof(TCHAR)) - 1, GetIniPath(true));
		if (_tcscmp(acCurrent, pcExpected) == 0)
		{
			return (true);
		}
	}
	//Try to save for all users
	bRes = WritePrivateProfileString(pacSection, pacKey, sValue.c_str(), GetIniPath(true));
	if (!bRes)
	{
		if (bCheck)
		{
			GetPrivateProfileString(pacSection, pacKey, _T("\x01"), acCurrent, (sizeof(acCurrent) / sizeof(TCHAR)) - 1, GetIniPath(false));
			if (_tcscmp(acCurrent, pcExpected) == 0)
			{
				return (true);
			}
		}
		//Save it for current user
		bRes = WritePrivateProfileString(pacSection, pacKey, sValue.c_str(), GetIniPath(false));
	}
	return (bRes);
}



// ---------------------------------------------------------------------------------------------------------------------------------------------------------------------------
bool __fastcall clsUtil::SetIni(const TCHAR *pacSection, const TCHAR *pacKey, bool pbValue, const TCHAR *pacComment)
{
	return (SetIni(pacSection, pacKey, (pbValue) ? _T("true") : _T("false"), pacComment));
}



// ---------------------------------------------------------------------------------------------------------------------------------------------------------------------------
bool __fastcall clsUtil::SetIni(const TCHAR *pacSection, const TCHAR *pacKey, int piValue, const TCHAR *pacComment)
{
	TCHAR acValue[32];


	_itot(piValue, acValue, 10);
	return (SetIni(pacSection, pacKey, acValue, pacComment));
}



// ---------------------------------------------------------------------------------------------------------------------------------------------------------------------------
bool __fastcall clsUtil::SetIni(const TCHAR *pacSection, const TCHAR *pacKey, long long plValue, const TCHAR *pacComment)
{
	TCHAR acValue[2048];


	_i64tot(plValue, acValue, 10);
	return (SetIni(pacSection, pacKey, acValue, pacComment));
}




// ---------------------------------------------------------------------------------------------------------------------------------------------------------------------------
bool __fastcall clsUtil::DeleteIni(const TCHAR *pacSection, const TCHAR *pacKey)
{
	bool bRes = (WritePrivateProfileString(pacSection, pacKey, nullptr, GetIniPath(true)) != 0);
	if (_tcscmp(GetIniPath(true), GetIniPath(false)) != 0)
	{
		bRes = (WritePrivateProfileString(pacSection, pacKey, nullptr, GetIniPath(false)) != 0) || bRes;
	}
	return (bRes);
}

// ---------------------------------------------------------------------------------------------------------------------------------------------------------------------------
const TCHAR * __fastcall clsUtil::GetRegistryPath(void)
{
	static TCHAR acPath[PATH_MAX + 1] = _T("");


	// Check if we already have it cached
	if (acPath[0] == NULL)
	{
		_tcscpy(acPath, _T("Software\\"));
		_tcscat(acPath, Application->Name.c_str());
	}
	return (acPath);
}



// ---------------------------------------------------------------------------------------------------------------------------------------------------------------------------
const TCHAR * __fastcall clsUtil::GetRegistry(HKEY phKey, const TCHAR *pacSubkey, const TCHAR *pacName)
{
	HKEY hKey;
	//Multithreaded optimization: the result buffer is thread local, or concurrent workers would overwrite each other
	static __thread TCHAR acRes[2048];


	acRes[0] = NULL;
	if (RegOpenKeyEx(phKey, pacSubkey, NULL, KEY_QUERY_VALUE, &hKey) == ERROR_SUCCESS)
	{
		unsigned int iSize = sizeof(acRes) - sizeof(TCHAR);
		if (RegQueryValueEx(hKey, pacName, nullptr, nullptr, (BYTE *) acRes, (LPDWORD) &iSize) == ERROR_SUCCESS)
		{
			acRes[iSize / sizeof(TCHAR)] = NULL;
		}
		else
		{
			acRes[0] = NULL;
		}
		RegCloseKey(hKey);
	}
	return (acRes);
}



// ---------------------------------------------------------------------------------------------------------------------------------------------------------------------------
int __fastcall clsUtil::GetRegistryI(HKEY phKey, const TCHAR *pacSubkey, const TCHAR *pacName)
{
	HKEY hKey;
	int iRes = -1;


	if (RegOpenKeyEx(phKey, pacSubkey, NULL, KEY_QUERY_VALUE, &hKey) == ERROR_SUCCESS)
	{
		unsigned int iSize = sizeof(iRes);
		RegQueryValueEx(hKey, pacName, nullptr, nullptr, (BYTE *) &iRes, (LPDWORD) &iSize);
		RegCloseKey(hKey);
	}
	return (iRes);
}




// ---------------------------------------------------------------------------------------------------------------------------------------------------------------------------
bool __fastcall clsUtil::SetRegistry(HKEY phKey, const TCHAR *pacSubkey, const TCHAR *pacName, const TCHAR *pacValue)
{
	bool bRes = false;
	HKEY hKey;


	//Writing creates the key when it does not exist (such as the browser emulation one), deleting only opens it
	if (((pacValue) ? (RegCreateKeyEx(phKey, pacSubkey, NULL, nullptr, REG_OPTION_NON_VOLATILE, KEY_SET_VALUE, nullptr, &hKey, nullptr)) : (RegOpenKeyEx(phKey, pacSubkey, NULL, KEY_SET_VALUE, &hKey))) == ERROR_SUCCESS)
	{
		if (pacValue)
		{
			bRes = (RegSetValueEx(hKey, pacName, NULL, REG_SZ, (const BYTE *) pacValue, (unsigned long) ((_tcslen(pacValue) + 1) * sizeof(TCHAR))) == ERROR_SUCCESS);
		}
		else
		{
			bRes = (RegDeleteValue(hKey, pacName) == ERROR_SUCCESS);
		}
		RegCloseKey(hKey);
	}
	return (bRes);
}



// ---------------------------------------------------------------------------------------------------------------------------------------------------------------------------
bool __fastcall clsUtil::SetRegistry(HKEY phKey, const TCHAR *pacSubkey, const TCHAR *pacName, unsigned int piValue)
{
	bool bRes = false;
	HKEY hKey;


	if (RegCreateKeyEx(phKey, pacSubkey, NULL, nullptr, REG_OPTION_NON_VOLATILE, KEY_SET_VALUE, nullptr, &hKey, nullptr) == ERROR_SUCCESS)
	{
		bRes = (RegSetValueEx(hKey, pacName, NULL, REG_DWORD, (BYTE *) &piValue, sizeof(piValue)) == ERROR_SUCCESS);
		RegCloseKey(hKey);
	}
	return (bRes);
}




// ---------------------------------------------------------------------------------------------------------------------------------------------------------------------------
bool __fastcall clsUtil::SetRegistry(HKEY phKey, const TCHAR *pacSubkey, const TCHAR *pacName, unsigned long long plValue)
{
	bool bRes = false;
	HKEY hKey;


	if (RegCreateKeyEx(phKey, pacSubkey, NULL, nullptr, REG_OPTION_NON_VOLATILE, KEY_SET_VALUE, nullptr, &hKey, nullptr) == ERROR_SUCCESS)
	{
		bRes = (RegSetValueEx(hKey, pacName, NULL, REG_QWORD, (BYTE *) &plValue, sizeof(plValue)) == ERROR_SUCCESS);
		RegCloseKey(hKey);
	}
	return (bRes);
}



// ---------------------------------------------------------------------------------------------------------------------------------------------------------------------------
bool __fastcall clsUtil::DeleteRegistry(HKEY phKey, const TCHAR *pacSubkey)
{
	bool bRes;


	bRes = (RegDeleteKey(phKey, pacSubkey) == ERROR_SUCCESS);

	return (bRes);
}



// --------------------------------------------------------------------------------------------------
unsigned int __fastcall clsUtil::Crc32 (const void *pacBuffer, unsigned int piLen, unsigned int piOldCrc)
{
	unsigned int iCont;
	unsigned int iCrc;
	unsigned int iSlice;
	const unsigned char *pacData = (const unsigned char *) pacBuffer;
	//Slicing-by-8 in 64 bit (more registers), slicing-by-4 in 32 bit
	#if defined(_WIN64)
		#define CRC32_SLICES 8
	#else
		#define CRC32_SLICES 4
	#endif
	static unsigned int aiTable[CRC32_SLICES][256];
	static bool bTable = false;


	//Build the tables only once, from the CRC-32 polynomial. Not thread safe, but safe in multithreaded optimization: the first call always happens on the main thread, while adding files
	if (!bTable)
	{
		//Classic byte-wise table
		for (iCont = 0; iCont < 256; iCont++)
		{
			iCrc = iCont;
			for (iSlice = 0; iSlice < 8; iSlice++)
			{
				iCrc = (iCrc >> 1) ^ ((0 - (iCrc & 1)) & 0xEDB88320);
			}
			aiTable[0][iCont] = iCrc;
		}
		//Each extra table is the previous one advanced one more byte (needs the whole first table)
		for (iCont = 0; iCont < 256; iCont++)
		{
			iCrc = aiTable[0][iCont];
			for (iSlice = 1; iSlice < CRC32_SLICES; iSlice++)
			{
				iCrc = (iCrc >> 8) ^ aiTable[0][iCrc & 0xFF];
				aiTable[iSlice][iCont] = iCrc;
			}
		}
		bTable = true;
	}

	iCrc = piOldCrc;
	//Little endian, so unaligned reads are fine in x86 and x64
	#if defined(_WIN64)
		while (piLen >= 8)
		{
			unsigned int iLow = iCrc ^ ((const unsigned int *) pacData)[0];
			unsigned int iHigh = ((const unsigned int *) pacData)[1];

			iCrc = aiTable[7][iLow & 0xFF] ^ aiTable[6][(iLow >> 8) & 0xFF] ^ aiTable[5][(iLow >> 16) & 0xFF] ^ aiTable[4][iLow >> 24] ^ aiTable[3][iHigh & 0xFF] ^ aiTable[2][(iHigh >> 8) & 0xFF] ^ aiTable[1][(iHigh >> 16) & 0xFF] ^ aiTable[0][iHigh >> 24];
			pacData += 8;
			piLen -= 8;
		}
	#else
		while (piLen >= 4)
		{
			iCrc ^= *((const unsigned int *) pacData);

			iCrc = aiTable[3][iCrc & 0xFF] ^ aiTable[2][(iCrc >> 8) & 0xFF] ^ aiTable[1][(iCrc >> 16) & 0xFF] ^ aiTable[0][iCrc >> 24];
			pacData += 4;
			piLen -= 4;
		}
	#endif
	//Remaining bytes
	for (iCont = 0; iCont < piLen; iCont++)
	{
		unsigned char cByte = (unsigned char) iCrc ^ pacData[iCont];
		iCrc = (iCrc >> 8) ^ aiTable[0][cByte];
	}
	return (iCrc ^ 0xFFFFFFFF);
}


// ---------------------------------------------------------------------------------------------------------------------------------------------------------------------------
int __fastcall clsUtil::Random(int piMin, int piMax)
{
	static unsigned int iSeed = (unsigned int) (GetTickCount() * GetCurrentProcessId());


	if (piMax <= piMin)
	{
		return (piMin);
	}
	//Compute it only when valid, or piMax < piMin would underflow before the check above
	unsigned long long lRange = ((unsigned long long) piMax - (unsigned long long) piMin) + 1;
	iSeed = (214013 * iSeed + 2531011);
	//Use the high bits of the generator: the low ones have a very short period (the lowest one just alternates), which is very noticeable with small ranges
	return ((int) ((((unsigned long long) iSeed * lRange) >> 32) + piMin));
}



// ---------------------------------------------------------------------------------------------------------------------------------------------------------------------------
const TCHAR * __fastcall clsUtil::GetLogPath(void)
{
	static TCHAR acPath[PATH_MAX + 1] = _T("");


	// Check if we already have it cached
	if (acPath[0] == NULL)
	{
		TCHAR acTmp[2048] = _T("");
		TCHAR *pcExtension;


		if (GetModuleFileName(nullptr, acTmp, (sizeof(acTmp) / sizeof(TCHAR)) - 1) != 0)
		{
			//Remove the extension only if there is one, or _tcsrchr would return NULL and crash
			pcExtension = _tcsrchr(acTmp, '.');
			if (pcExtension)
			{
				*pcExtension = NULL;
			}
		}
		_tcscat(acTmp, _T(".log"));

		bool bWritable = FileLocationWritable(acTmp);

		if (bWritable)
		{
			clsUtil::CopyString(acPath, sizeof(acPath) / sizeof(TCHAR), acTmp);
		}
		else
		{
			const TCHAR *pacUserProfile = _tgetenv(_T("USERPROFILE"));
			if (pacUserProfile)
			{
				_stprintf(acPath, _T("%s\\%s"), pacUserProfile, (Application->Name + ".log").c_str());
			}
			else
			{
				//No user profile, so fall back to the program directory (later writes will fail silently, as with any other unwritable location)
				clsUtil::CopyString(acPath, sizeof(acPath) / sizeof(TCHAR), acTmp);
			}
		}
		_tcscpy(acPath, GetShortName((String) acPath).c_str());
	}
	return (acPath);
}




// ---------------------------------------------------------------------------------------------------------------------------------------------------------------------------
void __fastcall clsUtil::Log(int piLevel, const TCHAR *pacValue, int piDesiredLevel)
{
	if ((piDesiredLevel) > piLevel)
	{
		FILE *pLog = _tfopen(GetLogPath(), _T("at"));
		if (pLog)
		{
			_ftprintf(pLog, _T("%s\n"), pacValue);
			fclose(pLog);
		}
	}
}




// ---------------------------------------------------------------------------
bool __fastcall clsUtil::LoadForm(TForm *pfrmForm)
{
	int iWindowState;


	//Interface zoom, as a percentage. The form was just created at its design size, so it is scaled before anything else, and the size restored below (saved already zoomed) is not scaled twice
	int iZoom = TfrmMain::GetOption(_T("Options"), _T("Zoom"), KI_ZOOM_DEFAULT);
	iZoom = max(KI_ZOOM_MIN, min(iZoom, KI_ZOOM_MAX));
	if (iZoom != KI_ZOOM_DEFAULT)
	{
		//Scales the form size, its controls (position, size, constraints) and all fonts by the same ratio
		pfrmForm->ScaleBy(iZoom, KI_ZOOM_DEFAULT);
	}

	//Set position and size at once, or the form would be moved and resized (firing its events) four times
	pfrmForm->SetBounds(GetIni(pfrmForm->Name.c_str(), _T("Left"), (Screen->Width - pfrmForm->Width) >> 1), GetIni(pfrmForm->Name.c_str(), _T("Top"), (Screen->Height - pfrmForm->Height) >> 1), GetIni(pfrmForm->Name.c_str(), _T("Width"), pfrmForm->Width), GetIni(pfrmForm->Name.c_str(), _T("Height"), pfrmForm->Height));
	iWindowState = GetIni(pfrmForm->Name.c_str(), _T("WindowState"), (int) pfrmForm->WindowState);
	//We do not want to reopen FileOptimizer in minimized state
	if (iWindowState == (int) wsMinimized)
	{
		iWindowState = (int) wsNormal;
	}
	pfrmForm->WindowState = (TWindowState) iWindowState;
	pfrmForm->DefaultMonitor = (TDefaultMonitor) GetIni(pfrmForm->Name.c_str(), _T("DefaultMonitor"), pfrmForm->DefaultMonitor);
	pfrmForm->MakeFullyVisible(nullptr);
	return (true);
}



// ---------------------------------------------------------------------------
bool __fastcall clsUtil::SaveForm(TForm *pfrmForm)
{
	if ((pfrmForm->WindowState != wsMaximized) && (pfrmForm->WindowState != wsMinimized))
	{
		SetIni(pfrmForm->Name.c_str(), _T("Left"), pfrmForm->Left);
		SetIni(pfrmForm->Name.c_str(), _T("Top"), pfrmForm->Top);
		SetIni(pfrmForm->Name.c_str(), _T("Width"), pfrmForm->Width);
		SetIni(pfrmForm->Name.c_str(), _T("Height"), pfrmForm->Height);
	}
	SetIni(pfrmForm->Name.c_str(), _T("WindowState"), (int) pfrmForm->WindowState);
	SetIni(pfrmForm->Name.c_str(), _T("DefaultMonitor"), (int) pfrmForm->DefaultMonitor);
	return (true);
}



// ---------------------------------------------------------------------------
bool __fastcall clsUtil::CopyToRecycleBin(const TCHAR *pacSource)
{
	int iRes = -1;
	SHFILEOPSTRUCT udtFileOp = {};
	//Not zeroed in advance: the names are built below writing only the characters used and their terminators, instead of clearing 64 KB of stack on every recycled file
	TCHAR acSource[PATH_MAX + 1];
	TCHAR acDestination[PATH_MAX + 1];


	//Only the main thread can process messages: workers would dispatch them in the wrong thread
	if (GetCurrentThreadId() == MainThreadID)
	{
		Application->ProcessMessages();
	}
	//SHFileOperation expects a list of strings ending in a double NUL, so both terminators are written explicitly
	size_t iSourceLen = _tcslen(pacSource);
	//Leave room for the two terminators
	if (iSourceLen > ((sizeof(acSource) / sizeof(TCHAR)) - 2))
	{
		iSourceLen = (sizeof(acSource) / sizeof(TCHAR)) - 2;
	}
	memcpy(acSource, pacSource, iSourceLen * sizeof(TCHAR));
	acSource[iSourceLen] = NULL;
	acSource[iSourceLen + 1] = NULL;

	//The temporary copy is the same name with a .tmp extension, always leaving room for it and its terminator
	size_t iDestinationLen = (iSourceLen > ((sizeof(acDestination) / sizeof(TCHAR)) - 5)) ? ((sizeof(acDestination) / sizeof(TCHAR)) - 5) : iSourceLen;
	memcpy(acDestination, acSource, iDestinationLen * sizeof(TCHAR));
	_tcscpy(acDestination + iDestinationLen, _T(".tmp"));

	//If it cannot be copied do not delete the original, since it could not be restored afterwards
	if (!CopyFile(acSource, acDestination))
	{
		DeleteFile(acDestination);
		return (false);
	}

	udtFileOp.wFunc = FO_DELETE;
	udtFileOp.fFlags = FOF_ALLOWUNDO | FOF_NO_UI;
	udtFileOp.pFrom = acSource;

	//Moving to the Recycle Bin only works reliably from the main thread: from the optimization workers the shell operation fails silently and nothing is moved, so run it there when needed
	if (GetCurrentThreadId() == MainThreadID)
	{
		iRes = SHFileOperation(&udtFileOp);
	}
	else
	{
		TThread::Synchronize(nullptr, [&]() { iRes = SHFileOperation(&udtFileOp); });
	}
	//SHFileOperation can return 0 even when the operation was aborted, so also check it was really completed
	if ((iRes == 0) && (!udtFileOp.fAnyOperationsAborted))
	{
		//The original was already moved to the Recycle Bin, so restore it from the temporary copy to keep optimizing it
		bool bRestored = RenameFile(acSource, acDestination);
		//Could not move it back, so copy it
		if (!bRestored)
		{
			bRestored = CopyFile(acDestination, acSource);
		}
		DeleteFile(acDestination);
		return (bRestored);
	}
	else
	{
		DeleteFile(acDestination);
		return (false);
	}
}




// ---------------------------------------------------------------------------
bool __fastcall clsUtil::SetTaskListProgress(unsigned int piCompleted, unsigned int piTotal)
{
	//The interface is created only once and reused, or every progress update (ten times per second while optimizing, or once per file in single threaded mode) would pay the object creation again
	static ITaskbarList3 *pTaskList = nullptr;
	static bool bChecked = false;


	// In 7 or newer use new TaskDialog
	if (!bChecked)
	{
		bChecked = true;
		if (GetWindowsVersion() >= 601)
		{
			ITaskbarList3 *pCreated = nullptr;
			HRESULT hRes = ::CoCreateInstance(CLSID_TaskbarList, nullptr, CLSCTX_INPROC_SERVER, IID_ITaskbarList3, (void **) &pCreated);
			if ((SUCCEEDED(hRes)) && (pCreated) && (SUCCEEDED(pCreated->HrInit())))
			{
				pTaskList = pCreated;
			}
			else if (pCreated)
			{
				pCreated->Release();
			}
		}
	}
	if (pTaskList)
	{
		bool bRes = true;
		if (Application->MainForm)
		{
			bRes = ((SUCCEEDED(pTaskList->SetProgressState(Application->MainForm->Handle, TBPF_NORMAL))) && (SUCCEEDED(pTaskList->SetProgressValue(Application->MainForm->Handle, piCompleted, piTotal))));
		}
		//The interface can get disconnected when the shell restarts, so drop it and let the next call create a fresh one
		if (!bRes)
		{
			pTaskList->Release();
			pTaskList = nullptr;
			return (false);
		}
		return (true);
	}
	return (false);
}


// ---------------------------------------------------------------------------
unsigned int __fastcall clsUtil::GetWindowsVersion(void)
{
	static unsigned int iWindowsVersion = NULL;


	//Get true Windows version, even for non manifested applications under Windows 8.1 or later
	if (iWindowsVersion == NULL)
	{
		//NTDLL is always loaded, so no need to load and free it
		HMODULE hNtDll = GetModuleHandle(_T("NTDLL.DLL"));
		if (hNtDll)
		{
			typedef NTSTATUS (WINAPI RtlGetVersionType)(RTL_OSVERSIONINFOW *pudtRtlVersionInfo);
			RtlGetVersionType *RtlGetVersionProc = (RtlGetVersionType *) GetProcAddress(hNtDll, "RtlGetVersion");
			if (RtlGetVersionProc)
			{
				RTL_OSVERSIONINFOW udtRtlVersionInfo = {};
				udtRtlVersionInfo.dwOSVersionInfoSize = sizeof(udtRtlVersionInfo);
				RtlGetVersionProc(&udtRtlVersionInfo);
				iWindowsVersion = (udtRtlVersionInfo.dwMajorVersion * 100) + udtRtlVersionInfo.dwMinorVersion;
			}
		}
		//Fallback when library not loaded or RtlGetVersion not exists
		if (iWindowsVersion == NULL)
		{
			iWindowsVersion = GetVersion();
			iWindowsVersion = (LOBYTE(LOWORD(iWindowsVersion))) * 100 + (HIBYTE(LOWORD(iWindowsVersion)));
		}
	}
	//500: Windows 2000; 501: Windows XP; 502: Windows XP x64/Windows 2003; 600: Windows Vista/2008; 601: Windows 7/Windows 2008 R2; 602: Windows 8/Windows 2012; 603: Windows 8.1; 1000: Windows 10
	return (iWindowsVersion);
}


// ---------------------------------------------------------------------------
bool __fastcall clsUtil::IsWindows64(void)
{
	static unsigned int iWindowsArchitecture = PROCESSOR_ARCHITECTURE_UNKNOWN;


	if (iWindowsArchitecture == PROCESSOR_ARCHITECTURE_UNKNOWN)
	{
		#if defined(_WIN64)
			iWindowsArchitecture = PROCESSOR_ARCHITECTURE_AMD64;
		#else
			SYSTEM_INFO udtSystemInfo;
			GetNativeSystemInfo(&udtSystemInfo);
			iWindowsArchitecture = udtSystemInfo.wProcessorArchitecture;
		#endif
	}
	return (iWindowsArchitecture == PROCESSOR_ARCHITECTURE_AMD64);
}



// ---------------------------------------------------------------------------
bool __fastcall clsUtil::ShutdownWindows(unsigned int piMode)
{
	bool bRes = false;
	HANDLE hToken;
	TOKEN_PRIVILEGES udtTokenPrivileges = {};


	//Get a token for this process
	if (OpenProcessToken(GetCurrentProcess(), TOKEN_ADJUST_PRIVILEGES | TOKEN_QUERY, &hToken))
	{
		//Get the LUID for the shutdown privilege.
		bool bLookup = (LookupPrivilegeValue(nullptr, SE_SHUTDOWN_NAME, &udtTokenPrivileges.Privileges[0].Luid) != 0);
		udtTokenPrivileges.PrivilegeCount = 1;  //One privilege to set
		udtTokenPrivileges.Privileges[0].Attributes = SE_PRIVILEGE_ENABLED;

		// Get the shutdown privilege for this process.
		//AdjustTokenPrivileges succeeds even when the privilege was not really assigned, so the last error must also be checked
		if ((bLookup) && (AdjustTokenPrivileges(hToken, false, &udtTokenPrivileges, 0, (PTOKEN_PRIVILEGES) nullptr, nullptr)) && (GetLastError() == ERROR_SUCCESS))
		{
			//Shut down the system and force all applications to close.
			unsigned int iFlags;

			//Poweroff
			if (piMode == 1)
			{
				iFlags = EWX_POWEROFF;
			}
			//Reboot
			else if (piMode == 2)
			{
				iFlags = EWX_REBOOT | EWX_FORCE;
			}
			//Logout
			else if (piMode == 3)
			{
				iFlags = EWX_LOGOFF | EWX_FORCE;
			}
			//Shutdown
			else //0
			{
				iFlags = EWX_SHUTDOWN | EWX_HYBRID_SHUTDOWN | EWX_FORCE;
			}
			if (ExitWindowsEx(iFlags, SHTDN_REASON_MAJOR_OPERATINGSYSTEM | SHTDN_REASON_MINOR_UPGRADE | SHTDN_REASON_FLAG_PLANNED))
			{
				bRes = true;
			}
		}
		CloseHandle(hToken);
	}
	return (bRes);
}



// ---------------------------------------------------------------------------------------------------------------------------------------------------------------------------
//Win32 file APIs already accept long names, so the short (8.3) name is only needed when the path is too long for them. GetShortPathName is slow (it queries the disk for every component of the path) and it was called on every file operation
String __fastcall clsUtil::GetApiName(const TCHAR *pacFile)
{
	if (_tcslen(pacFile) < (MAX_PATH - 16))
	{
		return ((String) pacFile);
	}
	return (clsUtil::GetShortName((String) pacFile));
}



// ---------------------------------------------------------------------------
String __fastcall clsUtil::GetShortName(String psLongName)
{
	if ((psLongName.IsEmpty()) || (psLongName.Length() > MAX_PATH * 2))
	{
		return(psLongName);  //Avoid very long paths
	}
	else if ((clsUtil::IsWindows64()) && (psLongName.Length() > MAX_PATH))
	{
		if (psLongName.StartsWith(_T("\\\\?\\")))
		{
			return(psLongName);
		}
		//UNC paths need their own prefix
		if (psLongName.StartsWith(_T("\\\\")))
		{
			return (_T("\\\\?\\UNC\\") + psLongName.SubString(3, MaxInt));
		}
		return (_T("\\\\?\\") + psLongName);
	}
	else
	{
		//Only used when GetShortPathName succeeds, which terminates it, so there is no need to clear it
		TCHAR acShortName[PATH_MAX];
		DWORD dwResult = GetShortPathName(psLongName.c_str(), acShortName, PATH_MAX);
		if ((dwResult == 0) || (dwResult >= PATH_MAX))
		{
			return(psLongName);
		}
		return(String(acShortName));
	}
}



// ---------------------------------------------------------------------------
bool __fastcall clsUtil::DeleteFile(const TCHAR *pacFile)
{
	return (::DeleteFile(GetApiName(pacFile).c_str()));
}



// ---------------------------------------------------------------------------------------------------------------------------------------------------------------------------
void __fastcall clsUtil::CopyString(TCHAR *pacDestination, size_t piDestinationSize, const TCHAR *pacSource)
{
	//_tcsncpy does not terminate the destination when the source does not fit, and several option buffers are filled from user or server controlled text, so always terminate
	if ((pacDestination) && (piDestinationSize > 0))
	{
		_tcsncpy(pacDestination, (pacSource != nullptr) ? pacSource : _T(""), piDestinationSize - 1);
		pacDestination[piDestinationSize - 1] = NULL;
	}
}




// ---------------------------------------------------------------------------------------------------------------------------------------------------------------------------