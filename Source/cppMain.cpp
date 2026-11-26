// ---------------------------------------------------------------------------
#include "cppMain.h"
#include <memory>
#include <string>
#include <unordered_set>


// ---------------------------------------------------------------------------
#pragma package(smart_init)
#pragma resource "*.dfm"



// ---------------------------------------------------------------------------
TfrmMain *frmMain = nullptr;
struct udtOptions gudtOptions  = {};
SYSTEM_INFO gudtSystemInfo = {};
bool gbProcess = false;
bool gbStop = false;
//Set (per thread) when RunProcess killed a plugin that exceeded its maximum run time, so its partial output is never used
static __thread bool mbPluginTimedOut;
//Multithreaded optimization: handles of the workers still running (so closing can wait for them), and whether the form is already closing
static HANDLE mahWorkerThreads[MAXIMUM_WAIT_OBJECTS];
static int miWorkerThreads = 0;
static bool gbClosing = false;
unsigned int miStartTicks;
//Files already in the grid, to detect duplicates in constant time: a sorted list needed a linear insertion per added file, which made adding huge folders quadratic
static std::unordered_set<std::wstring> *msetFilesExist = nullptr;
//Cache section of the INI file, loaded once per batch of added files (see AddFiles), and the number of cache lookups the batch has done so far
static THashedStringList *mlstCache = NULL;
static unsigned int miCacheLookups = 0;
//Critical section guarding grid and shared state access between the main thread and the optimization worker threads
static CRITICAL_SECTION mudtCriticalSection;
//Interface zoom currently applied to the main window, so only the difference is scaled when it changes
static int miAppliedZoom = KI_ZOOM_DEFAULT;
//Guards the INI file writes of the workers. They are slow (the whole file is rewritten), so they must not hold the one above, or the interface painting would wait for them
static CRITICAL_SECTION mudtCriticalSectionIni;
//Se emitió un Navigate() sobre el navegador de anuncios: su motor (Edge/WebView2) puede seguir inicializándose de forma asíncrona
static bool mbWebAdsNavigateRequested = false;



// ---------------------------------------------------------------------------
__fastcall TfrmMain::TfrmMain(TComponent* Owner) : TForm(Owner)
{
}



// ---------------------------------------------------------------------------
__fastcall TfrmMain::~TfrmMain()
{
}



// ---------------------------------------------------------------------------
void __fastcall TfrmMain::FormCreate(TObject *Sender)
{
	TCHAR acPath[PATH_MAX + 1] = {};


	//The critical section lives for the whole process, so it can also guard the interface against the optimization workers
	InitializeCriticalSection(&mudtCriticalSection);
	InitializeCriticalSection(&mudtCriticalSectionIni);

	Caption = Application->Name;
	Icon = Application->Icon;
	lblCopyright->Hint = KS_APP_URL;

	clsUtil::GetFileVersionField(Application->ExeName.c_str(), (const TCHAR *) _T("LegalCopyright"), acPath, (sizeof(acPath) / sizeof(TCHAR)) - 1);
	lblCopyright->Caption = acPath;

	pgbProgress->Parent = stbMain;

	GetSystemInfo(&gudtSystemInfo);
	
	//LoadForm also applies the interface zoom, before restoring the window size and the column widths (saved already zoomed)
	clsUtil::LoadForm(this);
	LoadOptions();
	//Both read the same option, so UpdateTheme only has to scale later changes of it
	miAppliedZoom = gudtOptions.iZoom;

	//The first tick runs right after the configured startup delay, instead of at the next multiple of the 2 seconds interval (4 s with the default 3 s delay), so files received in the command line start being added sooner
	tmrMain->Interval = min(max(gudtOptions.iStartupDelay, 1000), 29000);

	actHelp->Enabled = clsUtil::ExistsFile(Application->HelpFile.c_str());

	UpdateTheme();
	FormResize(Sender);

	SetPriorityClass(GetCurrentProcess(), (unsigned long) gudtOptions.iProcessPriority);

	actClearExecute(Sender);
	//Load grid contents
	LoadPersistentGrid();

	// Allow to kill process childs
	HANDLE hJob = CreateJobObject(NULL, NULL);
	if (hJob)
	{
		JOBOBJECT_EXTENDED_LIMIT_INFORMATION udtJobInfo = {};
		udtJobInfo.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
		SetInformationJobObject(hJob, JobObjectExtendedLimitInformation, &udtJobInfo, sizeof(udtJobInfo));
		//Assign ourselves to the job, so plugin processes (our childs) are killed on close. Without it, the job did nothing
		AssignProcessToJobObject(hJob, GetCurrentProcess());
		// Do NOT CloseHandle(hJob): with JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE, closing the last
		// handle terminates every process in the job, including this one. The handle is
		// intentionally left open for the process lifetime (released by the OS on exit).
	}

	// Allow drop when elevated (UIPI)
	{
		HMODULE hUser32 = GetModuleHandle(_T("user32.dll"));
		if (hUser32)
		{
			typedef BOOL (WINAPI *PFNChangeWindowMessageFilterEx)(HWND, UINT, DWORD, PCHANGEFILTERSTRUCT);
			PFNChangeWindowMessageFilterEx pfnChangeFilter = (PFNChangeWindowMessageFilterEx) GetProcAddress(hUser32, "ChangeWindowMessageFilterEx");
			if (pfnChangeFilter)
			{
				pfnChangeFilter(Handle, WM_DROPFILES, MSGFLT_ALLOW, NULL);
				pfnChangeFilter(Handle, WM_COPYDATA, MSGFLT_ALLOW, NULL);
				pfnChangeFilter(Handle, 0x0049, MSGFLT_ALLOW, NULL); // WM_COPYGLOBALDATA
			}
			else
			{
				//Fallback for Windows < Vista SP1: ChangeWindowMessageFilter
				typedef BOOL (WINAPI *PFNChangeWindowMessageFilter)(UINT, DWORD);
				PFNChangeWindowMessageFilter pfnFilter = (PFNChangeWindowMessageFilter) GetProcAddress(hUser32, "ChangeWindowMessageFilter");
				if (pfnFilter)
				{
					pfnFilter(WM_DROPFILES, MSGFLT_ALLOW);
					pfnFilter(WM_COPYDATA, MSGFLT_ALLOW);
					pfnFilter(0x0049, MSGFLT_ALLOW);
				}
			}
		}
		DragAcceptFiles(Handle, true);
	}
}


// ---------------------------------------------------------------------------
#pragma argsused
void __fastcall TfrmMain::FormDestroy(TObject *Sender)
{
	//Clear the grid section before saving the settings, so the (potentially huge) grid rows are not rewritten once per option, and so stale rows do not linger forever once persistence is disabled
	if ((gudtOptions.bPersistentGrid) || (clsUtil::GetIni(_T("grdFiles"), _T("Rows"), _T(""))[0] != NULL))
	{
		ClearPersistentGrid();
	}

	clsUtil::SaveForm(this);
	clsLanguage::Save();
	SaveOptions();

	//Save grid contents as the last step, with a single profile write when the section fits
	if (gudtOptions.bPersistentGrid)
	{
		SavePersistentGrid();
	}

	//Release the list used to detect duplicates
	if (msetFilesExist)
	{
		delete msetFilesExist;
		msetFilesExist = nullptr;
	}

	//Release the ads browser last, so a failure here can no longer lose the settings
	if (webAds)
	{
		try
		{
			bool bLoaded = (webAds->LocationURL != "");
			//A Navigate() was issued but nothing loaded yet: the Edge engine (WebView2)
			//initializes and navigates asynchronously, so give it a bounded chance to
			//finish, or destroying the control races the pending initialization (AV in ntdll)
			if ((!bLoaded) && (mbWebAdsNavigateRequested))
			{
				unsigned int iStart = GetTickCount();
				while (((webAds->LocationURL == "") || (webAds->Busy)) && ((GetTickCount() - iStart) < 2000))
				{
					Application->ProcessMessages();
					Sleep(10);
				}
				bLoaded = (webAds->LocationURL != "");
			}
			//The engine is fully initialized (something was loaded): release the current
			//page while the control is still alive, waiting for it to go idle before freeing
			if (bLoaded)
			{
				webAds->Stop();
				webAds->Offline = true;
				webAds->Navigate("about:blank");
				unsigned int iStart = GetTickCount();
				while ((webAds->Busy) && ((GetTickCount() - iStart) < 1000))
				{
					Application->ProcessMessages();
					Sleep(10);
				}
				delete webAds;
			}
			//Navigate was requested but nothing loaded even after waiting: the engine is
			//most likely stuck in its asynchronous initialization. Detach instead of free:
			//it leaks the control, which is harmless in a process that is exiting, and
			//avoids the crash. The VCL will not destroy it later, hitting the same race
			else if (mbWebAdsNavigateRequested)
			{
				RemoveComponent(webAds);
			}
			//Never navigated: nothing asynchronous is pending, so freeing is safe
			else
			{
				delete webAds;
			}
		}
		catch (...)
		{
			//Never let the ads teardown break the shutdown
		}
		webAds = nullptr;
	}

	DeleteCriticalSection(&mudtCriticalSectionIni);
	DeleteCriticalSection(&mudtCriticalSection);
}


// ---------------------------------------------------------------------------
//The profile API can read and write whole sections, but its buffer size is limited, so single section accesses are only used when everything safely fits
#define KI_INI_SECTION_MAX	32760

// ---------------------------------------------------------------------------
//Loads the persistent grid from the INI file, in a single section read when it fits (querying every row key rescans the whole file once per row)
void __fastcall TfrmMain::LoadPersistentGrid(void)
{
	unsigned int iRows = (unsigned int) clsUtil::GetIni(_T("grdFiles"), _T("Rows"), 0);
	if (iRows > 0)
	{
		TCHAR *acSection = new TCHAR[KI_INI_SECTION_MAX];
		if (acSection)
		{
			//The all users INI file is tried first, then the per user one, like the rest of the profile accesses
			unsigned long lSize = GetPrivateProfileSection(_T("grdFiles"), acSection, KI_INI_SECTION_MAX, clsUtil::GetIniPath(true));
			if ((lSize == 0) && (_tcscmp(clsUtil::GetIniPath(true), clsUtil::GetIniPath(false)) != 0))
			{
				lSize = GetPrivateProfileSection(_T("grdFiles"), acSection, KI_INI_SECTION_MAX, clsUtil::GetIniPath(false));
			}
			//The return value is nSize - 2 when the section does not fit, so fall back to reading one row at a time
			if ((lSize > 0) && (lSize < (KI_INI_SECTION_MAX - 2)))
			{
				grdFiles->RowCount = (int) iRows + 1;
				//The section is a sequence of NUL terminated "key=value" entries, followed by an empty one
				for (const TCHAR *pcEntry = acSection; *pcEntry != NULL; pcEntry += _tcslen(pcEntry) + 1)
				{
					//Rows are indexed by their number, so the grid order survives any reordering of the section
					if ((_tcsncmp(pcEntry, _T("Row"), 3) == 0) && (_tcslen(pcEntry) > 3))
					{
						int iRow = _ttoi(pcEntry + 3);
						if ((iRow >= 1) && ((unsigned int) iRow <= iRows))
						{
							const TCHAR *pcValue = _tcschr(pcEntry, _T('='));
							if (pcValue != NULL)
							{
								pcValue++;
								String sRow = pcValue;
								sRow = ReplaceStr(sRow, "%3D", "=");
								sRow = ReplaceStr(sRow, "%3B", ";");
								sRow = ReplaceStr(sRow, "%0A", "\r");
								sRow = ReplaceStr(sRow, "%0D", "\n");
								sRow = ReplaceStr(sRow, "%25", "%");
								grdFiles->Rows[iRow]->DelimitedText = sRow;
							}
						}
					}
				}
			}
			else
			{
				grdFiles->RowCount = (int) iRows + 1;
				for (unsigned int iRow = 1; iRow <= iRows; iRow++)
				{
					String sRow = clsUtil::GetIni(_T("grdFiles"), ((String) "Row" + iRow).c_str(), _T(""));
					sRow = ReplaceStr(sRow, "%3D", "=");
					sRow = ReplaceStr(sRow, "%3B", ";");
					sRow = ReplaceStr(sRow, "%0A", "\r");
					sRow = ReplaceStr(sRow, "%0D", "\n");
					sRow = ReplaceStr(sRow, "%25", "%");
					grdFiles->Rows[(int) iRow]->DelimitedText = sRow;
				}
			}
			RefreshStatus();
			delete[] acSection;
		}
	}
}

// ---------------------------------------------------------------------------
//Saves the persistent grid to the INI file, in a single section write when it fits (writing every row key rewrites the whole INI file once per row)
void __fastcall TfrmMain::SavePersistentGrid(void)
{
	unsigned int iRows = (unsigned int) grdFiles->RowCount - 1;

	//Build every encoded row once: they are used by the single section write, and also by the per row fallback
	TStringList *lstRows = new TStringList();
	unsigned int iLength = 16;	//The "Rows=" entry
	for (unsigned int iRow = 1; iRow <= iRows; iRow++)
	{
		String sRow = grdFiles->Rows[(int) iRow]->DelimitedText;
		sRow = ReplaceStr(sRow, "%", "%25");
		sRow = ReplaceStr(sRow, "=", "%3D");
		sRow = ReplaceStr(sRow, ";", "%3B");
		sRow = ReplaceStr(sRow, "\r", "%0A");
		sRow = ReplaceStr(sRow, "\n", "%0D");
		lstRows->Add(sRow);
		//"Row" + the row number + "=" + the row itself + the NUL terminator
		iLength += (unsigned int) sRow.Length() + 16;
	}

	//A single section write also replaces every previous row at once, so there is nothing to purge
	if (iLength <= KI_INI_SECTION_MAX)
	{
		TCHAR *acSection = new TCHAR[iLength + 2];
		if (acSection)
		{
			//The section is a sequence of NUL terminated "key=value" entries, followed by an empty one
			TCHAR *pcEntry = acSection;
			pcEntry += _stprintf(pcEntry, _T("Rows=%u"), iRows) + 1;
			for (unsigned int iRow = 1; iRow <= iRows; iRow++)
			{
				pcEntry += _stprintf(pcEntry, _T("Row%u=%s"), iRow, lstRows->Strings[(int) iRow - 1].c_str()) + 1;
			}
			*pcEntry = NULL;

			bool bRes = (WritePrivateProfileSection(_T("grdFiles"), acSection, clsUtil::GetIniPath(true)) != 0);
			if ((!bRes) && (_tcscmp(clsUtil::GetIniPath(true), clsUtil::GetIniPath(false)) != 0))
			{
				bRes = (WritePrivateProfileSection(_T("grdFiles"), acSection, clsUtil::GetIniPath(false)) != 0);
			}
			delete[] acSection;
			if (bRes)
			{
				delete lstRows;
				return;
			}
			//The single write failed, so fall back to one row at a time
		}
	}

	//Fallback without a section size limit: every write rewrites the whole INI file, but there is no other way for huge grids
	for (unsigned int iRow = 1; iRow <= iRows; iRow++)
	{
		clsUtil::SetIni(_T("grdFiles"), ((String) "Row" + iRow).c_str(), lstRows->Strings[(int) iRow - 1].c_str(), _T(""));
	}
	//Purge unused entries
	unsigned int iOldRows = (unsigned int) clsUtil::GetIni(_T("grdFiles"), _T("Rows"), 0);
	for (unsigned int iRow = iRows + 1; iRow <= iOldRows; iRow++)
	{
		clsUtil::DeleteIni(_T("grdFiles"), ((String) "Row" + iRow).c_str());
	}
	clsUtil::SetIni(_T("grdFiles"), _T("Rows"), (int) iRows, _T(""));
	delete lstRows;
}

// ---------------------------------------------------------------------------
//Deletes the whole persistent grid section in a single write, so the INI file does not keep growing with stale rows from older sessions
void __fastcall TfrmMain::ClearPersistentGrid(void)
{
	//A null key deletes the whole section. Try the all users file first, then the per user one, like the rest of the profile accesses
	WritePrivateProfileString(_T("grdFiles"), nullptr, nullptr, clsUtil::GetIniPath(true));
	if (_tcscmp(clsUtil::GetIniPath(true), clsUtil::GetIniPath(false)) != 0)
	{
		WritePrivateProfileString(_T("grdFiles"), nullptr, nullptr, clsUtil::GetIniPath(false));
	}
}

// ---------------------------------------------------------------------------
//Copies the file name of every row to the clipboard. The list builds the whole text in one pass, instead of reallocating it on every row
void __fastcall TfrmMain::CopyFilesToClipboard(void)
{
	unsigned int iRows = (unsigned int) grdFiles->RowCount - 1;
	TStringList *lstFiles = new TStringList();
	//Ctrl-C is allowed while optimizing, so the cells are read under the critical section, like the painting does
	EnterCriticalSection(&mudtCriticalSection);
	for (unsigned int iRow = 1; iRow <= iRows; iRow++)
	{
		//The .c_str() deep copy keeps the list away from the grid Strings, whose reference count is not updated atomically
		lstFiles->Add(GetCellValue(grdFiles->Cells[KI_GRID_FILE][(int) iRow], 1).c_str());
	}
	LeaveCriticalSection(&mudtCriticalSection);
	Clipboard()->AsText = lstFiles->Text;
	delete lstFiles;
}

// ---------------------------------------------------------------------------
static const struct udtOptionsBool
{
	const TCHAR *pacKey;
	const TCHAR *pacComment;
	size_t iOffset;
	bool bDefault;
} maudtOptionsBool[] =
{
	{_T("BMPCopyMetadata"), _T("Boolean. Default: false. Copy file metadata. Else strip all unneeded information."), offsetof(udtOptions, bBMPCopyMetadata), false},
	{_T("CSSEnableTidy"), _T("Boolean. Default: false. Enable tidy. Results in smaller files, but can happen they are not editable anymore."), offsetof(udtOptions, bCSSEnableTidy), false},
	{_T("EXEDisablePETrim"), _T("Boolean. Default: false. Disable PETrim. When enabled, PETrim will not be used, resulting in less EXE corruption at the cost of larger file size."), offsetof(udtOptions, bEXEDisablePETrim), false},
	{_T("EXEEnableUPX"), _T("Boolean. Default: false. Enable UPX executable compression. When enabled, UPX will be used, resulting EXE and DLL size reduction at the cost of runtime decompression."), offsetof(udtOptions, bEXEEnableUPX), false},
	{_T("GIFCopyMetadata"), _T("Boolean. Default: false. Copy file metadata. Else strip all unneeded information."), offsetof(udtOptions, bGIFCopyMetadata), false},
	{_T("GIFAllowLossy"), _T("Boolean. Default: false. Allowing lossy optimizations will get higher files reduction at the cost of some quality loss, even if visually unnoticeable or not."), offsetof(udtOptions, bGIFAllowLossy), false},
	{_T("GZCopyMetadata"), _T("Boolean. Default: false. Copy file metadata. Else strip all unneeded information."), offsetof(udtOptions, bGZCopyMetadata), false},
	{_T("HTMLEnableTidy"), _T("Boolean. Default: false. Enable Tidy. Results in smaller files, but can happen they are not editable anymore. Note that this applies to both SVG and HTML file types."), offsetof(udtOptions, bHTMLEnableTidy), false},
	{_T("JPEGCopyMetadata"), _T("Boolean. Default: false. Copy file metadata. Else strip all unneeded information"), offsetof(udtOptions, bJPEGCopyMetadata), false},
	{_T("JPEGUseArithmeticEncoding"), _T("Boolean. Default: false. Arithmetic encoding gives additional saving reductions, but is incompatible with most programs."), offsetof(udtOptions, bJPEGUseArithmeticEncoding), false},
	{_T("JPEGAllowLossy"), _T("Boolean. Default: false. Allowing lossy optimizations will get higher files reduction at the cost of some quality loss, even if visually unnoticeable or not."), offsetof(udtOptions, bJPEGAllowLossy), false},
	{_T("JSEnableJSMin"), _T("Boolean. Default: false. Enable jsmin. Results in smaller files, but can happen they are not editable anymore."), offsetof(udtOptions, bJSEnableJSMin), false},
	{_T("LUAEnableLeanify"), _T("Boolean. Default: false. Enable Leanify. Results in smaller files, but can happen they are not editable anymore."), offsetof(udtOptions, bLUAEnableLeanify), false},
	{_T("MiscDisable"), _T("Boolean. Default: false. Disable processing of other file types. This could imply lossing some edit capabilities such as PSD/PSB where text layers will be rasterized."), offsetof(udtOptions, bMiscDisable), false},
	{_T("MiscCopyMetadata"), _T("Boolean. Default: false. Copy file metadata. Else strip all unneeded information."), offsetof(udtOptions, bMiscCopyMetadata), false},
	{_T("MP3CopyMetadata"), _T("Boolean. Default: false. Copy file metadata. Else strip all unneeded information."), offsetof(udtOptions, bMP3CopyMetadata), false},
	{_T("MP4CopyMetadata"), _T("Boolean. Default: false. Copy file metadata. Else strip all unneeded information."), offsetof(udtOptions, bMP4CopyMetadata), false},
	{_T("PCXCopyMetadata"), _T("Boolean. Default: false. Copy file metadata. Else strip all unneeded information."), offsetof(udtOptions, bPCXCopyMetadata), false},
	{_T("PDFSkipLayered"), _T("Boolean. Default: false. Skip processing of PDF files containing layered objects. Results in more compatible files with higher size."), offsetof(udtOptions, bPDFSkipLayered), false},
	{_T("PNGCopyMetadata"), _T("Boolean. Default: false. Copy file metadata. Else strip all unneeded information."), offsetof(udtOptions, bPNGCopyMetadata), false},
	{_T("PNGAllowLossy"), _T("Boolean. Default: false. Allowing lossy optimizations will get higher files reduction at the cost of some quality loss, even if visually unnoticeable or not."), offsetof(udtOptions, bPNGAllowLossy), false},
	{_T("SVGEnableSvgcleaner"), _T("Boolean. Default: false. Enable svgcleaner for SVG files. Lossless and recommended."), offsetof(udtOptions, bSVGEnableSvgcleaner), false},
	{_T("SVGEnableSvgop"), _T("Boolean. Default: false. Enable svgop (SVGO packaged). Large binary, use only if needed."), offsetof(udtOptions, bSVGEnableSvgop), false},
	{_T("TGACopyMetadata"), _T("Boolean. Default: false. Copy file metadata. Else strip all unneeded information."), offsetof(udtOptions, bTGACopyMetadata), false},
	{_T("TIFFCopyMetadata"), _T("Boolean. Default: false. Copy file metadata. Else strip all unneeded information."), offsetof(udtOptions, bTIFFCopyMetadata), false},
	{_T("WAVCopyMetadata"), _T("Boolean. Default: false. Copy file metadata. Else strip all unneeded information."), offsetof(udtOptions, bWAVCopyMetadata), false},
	{_T("WAVStripSilence"), _T("Boolean. Default: false. Strip start and end silences if any."), offsetof(udtOptions, bWAVStripSilence), false},
	{_T("WEBPAllowLossy"), _T("Boolean. Default: false. Allowing lossy optimizations will get higher files reduction at the cost of some quality loss, even if visually unnoticeable or not."), offsetof(udtOptions, bWEBPAllowLossy), false},
	{_T("XMLEnableLeanify"), _T("Boolean. Default: false. Enable Leanify. Results in smaller files, but can happen they are not editable anymore."), offsetof(udtOptions, bXMLEnableLeanify), false},
	{_T("ZIPCopyMetadata"), _T("Boolean. Default: false. Copy file metadata. Else strip all unneeded information."), offsetof(udtOptions, bZIPCopyMetadata), false},
	{_T("ZIPRecurse"), _T("Boolean. Default: false. Enable optimization inside archives (recursive optimization)."), offsetof(udtOptions, bZIPRecurse), false},
	{_T("KeepAttributes"), _T("Boolean. Default: false. Keep original readonly, system, hidden and archive attributes as well as creation and modification timestamps."), offsetof(udtOptions, bKeepAttributes), false},
	{_T("DoNotUseRecycleBin"), _T("Boolean. Default: false. When checked original files will not be backed up in the system trashcan."), offsetof(udtOptions, bDoNotUseRecycleBin), false},
	{_T("DoNotCreateBackups"), _T("Boolean. Default: true. When checked original files will not be backed up in the current folder as a .BAK files."), offsetof(udtOptions, bDoNotCreateBackups), true},
	{_T("BeepWhenDone"), _T("Boolean. Default: false. Beep the speaker when optimization completes."), offsetof(udtOptions, bBeepWhenDone), false},
	{_T("ShutdownWhenDone"), _T("Boolean. Default: false. Shutdown computer when optimization completes."), offsetof(udtOptions, bShutdownWhenDone), false},
	{_T("DisableSleep"), _T("Boolean. Default: true. Avoid system sleeping while optimization in progress."), offsetof(udtOptions, bDisableSleep), true},
	{_T("AlwaysOnTop"), _T("Boolean. Default: false. Show main window always on top."), offsetof(udtOptions, bAlwaysOnTop), false},
	{_T("ShowToolBar"), _T("Boolean. Default: false. Show icons toolbar on main window."), offsetof(udtOptions, bShowToolBar), false},
	{_T("PersistentGrid"), _T("Boolean. Default: false. Make contents of the file grid persistant across sessions."), offsetof(udtOptions, bPersistentGrid), false},
	{_T("Debug"), _T("Boolean. Default: false. Enable internal debugging mode. Temporary files will not be deleted."), offsetof(udtOptions, bDebug), false},
	{_T("AllowDuplicates"), _T("Boolean. Default: false. Allow adding same file more than once. If enabled, adding to the grid will be much faster, specially on very large grids."), offsetof(udtOptions, bAllowDuplicates), false},
	{_T("AllowMultipleInstances"), _T("Boolean. Default: false. Allow having more than one FileOptimizer instance. If not, a warning will appear."), offsetof(udtOptions, bAllowMultipleInstances), false},
	{_T("ClearWhenComplete"), _T("Boolean. Default: false. Automatically clear file list when optimization is completed."), offsetof(udtOptions, bClearWhenComplete), false},
	{_T("EnableCache"), _T("Boolean. Default: false. Enable cache of already optimized files to automatically skip them."), offsetof(udtOptions, bEnableCache), false},
	{_T("HideInstructions"), _T("Boolean. Default: false. Hide instructions panel label."), offsetof(udtOptions, bHideInstructions), false},
};

// ---------------------------------------------------------------------------
static const struct udtOptionsInt
{
	const TCHAR *pacKey;
	const TCHAR *pacComment;
	size_t iOffset;
	int iDefault;
} maudtOptionsInt[] =
{
	{_T("PDFCustomDPI"), _T("Number. Default: 150. When custom profile is choosen, it allows you to specify a custom DPI for downsampling images."), offsetof(udtOptions, iPDFCustomDPI), 150},
	{_T("Level"), _T("Number. Default: 5. Optimization level from best speed to best compression."), offsetof(udtOptions, iLevel), 5},
	{_T("Language"), _T("Number. Default: 0 (System default). Interface language/locale code in decimal."), offsetof(udtOptions, iLanguage), 0},
	{_T("ProcessPriority"), _T("Number. Default: 2 (Normal). Process priority from most conservative to best performance."), offsetof(udtOptions, iProcessPriority), NORMAL_PRIORITY_CLASS},
	{_T("CheckForUpdates"), _T("Number. Default: 1. Automatically check for program updates."), offsetof(udtOptions, iCheckForUpdates), 1},
	{_T("LogLevel"), _T("Number. Default: 0. Debugging level to output on program log."), offsetof(udtOptions, iLogLevel), 0},
	{_T("FilenameFormat"), _T("Number. Default: 0. Specify the format to display filenames in the list."), offsetof(udtOptions, iFilenameFormat), 0},
	{_T("PluginTimeout"), _T("Number. Default: 3600. Maximum seconds a plugin can run before it is considered hung and killed, so it does not block the optimization forever. A minute for every MB of the file is added, since big files take longer. 0 disables the limit."), offsetof(udtOptions, iPluginTimeout), 3600},
	{_T("StartupDelay"), _T("Number. Default: 3000. Integer from 1000ms to 29000ms specifying the delay before start adding files."), offsetof(udtOptions, iStartupDelay), 3000},
	{_T("Zoom"), _T("Number. Default: 100. Interface zoom as a percentage of the normal size, from 50 to 400. Enlarges text and controls on small or high resolution screens."), offsetof(udtOptions, iZoom), KI_ZOOM_DEFAULT},
	{_T("Threads"), _T("Number. Default: -1. If specified, number of threads to use. -1 is auto-detect. This option is not compatible with Allow Duplicates."), offsetof(udtOptions, iThreads), -1},
	{_T("LeanifyIterations"), _T("Number. Default: -1. If specified, number of trial iterations in all Leanify executions will use this value. If not, iterations are calculated depending on the Optimization level."), offsetof(udtOptions, iLeanifyIterations), -1},
	{_T("PNGWolfIterations"), _T("Number. Default: -1. If specified, number of trial iterations in all PNGWolf executions will use this value. If not, iterations are calculated depending on the Optimization level."), offsetof(udtOptions, iPNGWolfIterations), -1},
};

// ---------------------------------------------------------------------------
static const struct udtOptionsString
{
	const TCHAR *pacKey;
	const TCHAR *pacComment;
	size_t iOffset;
	size_t iSize;
	const TCHAR *pacDefault;
} maudtOptionsString[] =
{
	{_T("CSSTemplate"), _T("String. Default 'low'. Compression template, from safer and worse compression, to highest compression."), offsetof(udtOptions, acCSSTemplate), sizeof(gudtOptions.acCSSTemplate) / sizeof(TCHAR), _T("low")},
	{_T("JSAdditionalExtensions"), _T("String. Default: ''. Add extra extensions to be threated as JS/JSON comma (,) separated."), offsetof(udtOptions, acJSAdditionalExtensions), sizeof(gudtOptions.acJSAdditionalExtensions) / sizeof(TCHAR), _T("")},
	{_T("PDFProfile"), _T("String. Default 'none'. Compression profile, from less size, to best quality."), offsetof(udtOptions, acPDFProfile), sizeof(gudtOptions.acPDFProfile) / sizeof(TCHAR), _T("none")},
	{_T("IncludeMask"), _T("String. Default: ''. If not empty, only files containing this mask (substring) on name or path will be included from optimization. You can use comma (,) to specify more than one substring being included."), offsetof(udtOptions, acIncludeMask), sizeof(gudtOptions.acIncludeMask) / sizeof(TCHAR), _T("")},
	{_T("ExcludeMask"), _T("String. Default: ''. Files containing this mask (substring) on name or path will be excluded from optimization. You can use comma (,) to specify more than one substring being excluded."), offsetof(udtOptions, acExcludeMask), sizeof(gudtOptions.acExcludeMask) / sizeof(TCHAR), _T("")},
	{_T("DisablePluginMask"), _T("String. Default: ''. Allow excluding execution of certain plugins. It is case insensitive, and allows more than one item to be specified by using comma (,) as separator."), offsetof(udtOptions, acDisablePluginMask), sizeof(gudtOptions.acDisablePluginMask) / sizeof(TCHAR), _T("")},
	{_T("TempDirectory"), _T("String. Default: ''. If not empty specified directory will be used for temporary storage instead of system's %TEMP%."), offsetof(udtOptions, acTempDirectory), sizeof(gudtOptions.acTempDirectory) / sizeof(TCHAR), _T("")},
};

// ---------------------------------------------------------------------------
void __fastcall TfrmMain::LoadOptions(void)
{
	TCHAR acPath[PATH_MAX + 1] = {};


	grdFiles->ColWidths[KI_GRID_FILE] = GetOption(Name.c_str(), _T("Col0Width"), grdFiles->ColWidths[KI_GRID_FILE]);
	grdFiles->ColWidths[KI_GRID_EXTENSION] = GetOption(Name.c_str(), _T("Col1Width"), grdFiles->ColWidths[KI_GRID_EXTENSION]);
	grdFiles->ColWidths[KI_GRID_ORIGINAL] = GetOption(Name.c_str(), _T("Col2Width"), grdFiles->ColWidths[KI_GRID_ORIGINAL]);
	grdFiles->ColWidths[KI_GRID_OPTIMIZED] = GetOption(Name.c_str(), _T("Col3Width"), grdFiles->ColWidths[KI_GRID_OPTIMIZED]);
	grdFiles->ColWidths[KI_GRID_STATUS] = GetOption(Name.c_str(), _T("Col4Width"), grdFiles->ColWidths[KI_GRID_STATUS]);

	// Early fit when columns clearly overflow. The definitive layout (including filling
	// empty space on the first run) is done in FormResize, which already knows the final width.
	{
		int iTotal = grdFiles->ColWidths[KI_GRID_FILE] + grdFiles->ColWidths[KI_GRID_EXTENSION] +
			grdFiles->ColWidths[KI_GRID_ORIGINAL] + grdFiles->ColWidths[KI_GRID_OPTIMIZED] +
			grdFiles->ColWidths[KI_GRID_STATUS];
		int iAvail = grdFiles->ClientWidth;
		if (iAvail <= 0)
		{
			iAvail = grdFiles->Width;
		}
		if ((iAvail > 0) && (iTotal > iAvail + 20))
		{
			int iFileWidth = iAvail >> 1;
			int iOtherWidth = (iAvail - iFileWidth) >> 2;
			if (iOtherWidth < 40)
			{
				iOtherWidth = 40;
			}
			grdFiles->ColWidths[KI_GRID_FILE] = iFileWidth;
			grdFiles->ColWidths[KI_GRID_EXTENSION] = iOtherWidth;
			grdFiles->ColWidths[KI_GRID_ORIGINAL] = iOtherWidth;
			grdFiles->ColWidths[KI_GRID_OPTIMIZED] = iOtherWidth;
			grdFiles->ColWidths[KI_GRID_STATUS] = iOtherWidth;
		}
	}

	//Regular options come from the tables above, so the key, the default and the INI comment live in a single place
	for (unsigned int iOption = 0; iOption < (sizeof(maudtOptionsBool) / sizeof(maudtOptionsBool[0])); iOption++)
	{
		*(bool *) (((unsigned char *) &gudtOptions) + maudtOptionsBool[iOption].iOffset) = GetOption(_T("Options"), maudtOptionsBool[iOption].pacKey, maudtOptionsBool[iOption].bDefault);
	}
	for (unsigned int iOption = 0; iOption < (sizeof(maudtOptionsInt) / sizeof(maudtOptionsInt[0])); iOption++)
	{
		*(int *) (((unsigned char *) &gudtOptions) + maudtOptionsInt[iOption].iOffset) = GetOption(_T("Options"), maudtOptionsInt[iOption].pacKey, maudtOptionsInt[iOption].iDefault);
	}
	for (unsigned int iOption = 0; iOption < (sizeof(maudtOptionsString) / sizeof(maudtOptionsString[0])); iOption++)
	{
		clsUtil::CopyString((TCHAR *) (((unsigned char *) &gudtOptions) + maudtOptionsString[iOption].iOffset), maudtOptionsString[iOption].iSize, GetOption(_T("Options"), maudtOptionsString[iOption].pacKey, maudtOptionsString[iOption].pacDefault));
	}

	//Out of range levels (hand edited INI, or command line) would give invalid plugin parameters
	gudtOptions.iLevel = max(0, min(gudtOptions.iLevel, 9));

	//Out of range zoom (hand edited INI, or command line) would make the interface unusable
	gudtOptions.iZoom = max(KI_ZOOM_MIN, min(gudtOptions.iZoom, KI_ZOOM_MAX));

	//A negative timeout (hand edited INI, or command line) means no limit
	gudtOptions.iPluginTimeout = max(0, gudtOptions.iPluginTimeout);

	//Donator and donation are validated before use, so they are loaded manually
	clsUtil::CopyString(gudtOptions.acDonator, sizeof(gudtOptions.acDonator) / sizeof(TCHAR), GetOption(_T("Options"), _T("Donator"), _T("")));
	StrTrim(gudtOptions.acDonator, _T(" %\a\b\f\n\r\t\v\\\'\"\?"));
	clsUtil::CopyString(gudtOptions.acDonation, sizeof(gudtOptions.acDonation) / sizeof(TCHAR), GetOption(_T("Options"), _T("Donation"), _T("")));

	//Invalid donator, reset fields. The donation is also checked, or a valid one got from the updates server would be lost on the next run
	if ((_tcslen(gudtOptions.acDonator) < 3) && (_tcslen(gudtOptions.acDonation) < 3))
	{
		gudtOptions.acDonator[0] = NULL;
		gudtOptions.acDonation[0] = NULL;
	}

	//Show ads require Windows Vista or later because of the need for IE11 or Edge
	if (clsUtil::GetWindowsVersion() >= 600)
	{
		//Set IE compatibility to 11.01
		clsUtil::SetRegistry(HKEY_CURRENT_USER, _T("Software\\Microsoft\\Internet Explorer\\Main\\FeatureControl\\FEATURE_BROWSER_EMULATION"), ExtractFileName(Application->ExeName).c_str(), 11001U);
		
		//Set Edge profile folder to system temporary path
		if (GetTempPath((sizeof(acPath) / sizeof(TCHAR)) - 1, acPath))
		{
			SetEnvironmentVariable(_T("WEBVIEW2_USER_DATA_FOLDER"), acPath);
		}

		//Check if ad display was not set
		//Read it into acPath, not into the options buffer
		clsUtil::CopyString(acPath, sizeof(acPath) / sizeof(TCHAR), GetOption(_T("Options"), _T("HideAds"), _T("")));
		if ((acPath[0] == NULL) && (gudtOptions.acDonator[0] == NULL))
		{
			String sCaption;
			sCaption.printf(_((TCHAR *) _T("This is the first time you run %s.\n\nDo you want to support its development by showing ads while it is in use?\n\nThis will encourage its future maintenance and upgrades, being highly appreciated.\n\nYou can change this option at any time from the Options menu.")), Application->Name.c_str());
			gudtOptions.bHideAds = !(clsUtil::MsgBox(Handle, sCaption.c_str(), _((TCHAR *) _T("Support")), MB_YESNO | MB_ICONQUESTION) == ID_YES);
		}
		else
		{
			//Donators see no ads unless they explicitly enable them. FormDestroy already guards the WebView2/Edge race that previously crashed on close when ads were shown.
			gudtOptions.bHideAds = GetOption(_T("Options"), _T("HideAds"), (gudtOptions.acDonator[0] != NULL) || (gudtOptions.acDonation[0] != NULL));
		}
	}
	//Disable ads in XP
	else
	{
		gudtOptions.bHideAds = true;
	}
	if (GetModuleFileName(nullptr, acPath, (sizeof(acPath) / sizeof(TCHAR)) - 1) != 0)
	{
		clsUtil::CopyString(acPath, sizeof(acPath) / sizeof(TCHAR), clsUtil::ExeVersion(acPath));
		clsUtil::CopyString(gudtOptions.acVersion, sizeof(gudtOptions.acVersion) / sizeof(TCHAR), GetOption(_T("Options"), _T("Version"), acPath));
	}

	//Statistics
	miStartTicks = GetTickCount();
	gudtOptions.lStatTime = (unsigned long long) GetOption(_T("Statistics"), _T("Time"), 0LL);
	gudtOptions.iStatOpens = (unsigned int) GetOption(_T("Statistics"), _T("Opens"), 0);
	gudtOptions.iStatOpens++;
	gudtOptions.iStatFiles = (unsigned int) GetOption(_T("Statistics"), _T("Files"), 0);
	gudtOptions.lStatTotalBytes = (unsigned long long) GetOption(_T("Statistics"), _T("TotalBytes"), 0LL);
	gudtOptions.lStatSavedBytes = (unsigned long long) GetOption(_T("Statistics"), _T("SavedBytes"), 0LL);
	gudtOptions.iStatSession = (unsigned int) GetOption(_T("Statistics"), _T("Session"), clsUtil::Random(0, INT_MAX));
	gudtOptions.iAdsShown = (unsigned int) GetOption(_T("Statistics"), _T("AdsShown"), 0);
}


// ---------------------------------------------------------------------------
void __fastcall TfrmMain::SaveOptions(void)
{
	clsUtil::SetIni(Name.c_str(), _T("Col0Width"), grdFiles->ColWidths[KI_GRID_FILE]);
	clsUtil::SetIni(Name.c_str(), _T("Col1Width"), grdFiles->ColWidths[KI_GRID_EXTENSION]);
	clsUtil::SetIni(Name.c_str(), _T("Col2Width"), grdFiles->ColWidths[KI_GRID_ORIGINAL]);
	clsUtil::SetIni(Name.c_str(), _T("Col3Width"), grdFiles->ColWidths[KI_GRID_OPTIMIZED]);
	clsUtil::SetIni(Name.c_str(), _T("Col4Width"), grdFiles->ColWidths[KI_GRID_STATUS]);

	for (unsigned int iOption = 0; iOption < (sizeof(maudtOptionsBool) / sizeof(maudtOptionsBool[0])); iOption++)
	{
		clsUtil::SetIni(_T("Options"), maudtOptionsBool[iOption].pacKey, *(bool *) (((unsigned char *) &gudtOptions) + maudtOptionsBool[iOption].iOffset), maudtOptionsBool[iOption].pacComment);
	}
	for (unsigned int iOption = 0; iOption < (sizeof(maudtOptionsInt) / sizeof(maudtOptionsInt[0])); iOption++)
	{
		clsUtil::SetIni(_T("Options"), maudtOptionsInt[iOption].pacKey, *(int *) (((unsigned char *) &gudtOptions) + maudtOptionsInt[iOption].iOffset), maudtOptionsInt[iOption].pacComment);
	}
	for (unsigned int iOption = 0; iOption < (sizeof(maudtOptionsString) / sizeof(maudtOptionsString[0])); iOption++)
	{
		clsUtil::SetIni(_T("Options"), maudtOptionsString[iOption].pacKey, (TCHAR *) (((unsigned char *) &gudtOptions) + maudtOptionsString[iOption].iOffset), maudtOptionsString[iOption].pacComment);
	}

	//Loaded with special logic, but still saved as regular settings
	clsUtil::SetIni(_T("Options"), _T("Donator"), gudtOptions.acDonator, _T("String. Default: ''. Donator name if you have supported the project."));
	clsUtil::SetIni(_T("Options"), _T("Donation"), gudtOptions.acDonation, _T("String. Default: ''. Donation details."));
	clsUtil::SetIni(_T("Options"), _T("Version"), gudtOptions.acVersion, _T("String. Default: ''."));
	clsUtil::SetIni(_T("Options"), _T("HideAds"), gudtOptions.bHideAds, _T("Boolean. Default: false. Hide ads from being displayed."));

	//Statistics
	unsigned int iSeconds = (GetTickCount() - miStartTicks) / 1000;
	gudtOptions.lStatTime += iSeconds;
	//Count the elapsed time only once (SaveOptions is also called when closing the options dialog), or every save would add the whole session again
	miStartTicks += iSeconds * 1000;
	clsUtil::SetIni(_T("Statistics"), _T("Time"), (long long) gudtOptions.lStatTime);
	clsUtil::SetIni(_T("Statistics"), _T("Opens"), (int) gudtOptions.iStatOpens);
	clsUtil::SetIni(_T("Statistics"), _T("Files"), (int) gudtOptions.iStatFiles);
	clsUtil::SetIni(_T("Statistics"), _T("TotalBytes"), (long long) gudtOptions.lStatTotalBytes);
	clsUtil::SetIni(_T("Statistics"), _T("SavedBytes"), (long long) gudtOptions.lStatSavedBytes);
	clsUtil::SetIni(_T("Statistics"), _T("Session"), (int) gudtOptions.iStatSession);
	clsUtil::SetIni(_T("Statistics"), _T("AdsShown"), (int) gudtOptions.iAdsShown);
}


//---------------------------------------------------------------------------
#pragma argsused
void __fastcall TfrmMain::FormCloseQuery(TObject *Sender, bool &CanClose)
{
	//Ask for confirmation while it is optimizing. Only then, otherwise there is nothing to wait for. The optimization flag is enough: looking for the plugin processes (one process snapshot for each plugin) delayed closing, and between two plugins none is running, so it exited without asking
	if ((gbProcess) && (!gbStop))
	{
		String sCaption;
		sCaption.printf(_((TCHAR *) _T("Optimization is still running. Do you want to stop and exit %s?")), Application->Name.c_str());
		if (clsUtil::MsgBox(Handle, sCaption.c_str(), _((TCHAR *) _T("Exit")), MB_YESNO | MB_ICONQUESTION) == ID_NO)
		{
			CanClose = false;
			return;
		}
	}
	gbStop = true;
	CanClose = true;
	Hide();
	gbClosing = true;

	//In multithreaded optimization, wait until the workers have exited before closing: they are already aborting because gbStop is set, but the current plugin could still be running, and they would access the form while it is being destroyed
	while ((miWorkerThreads > 0) && (WaitForMultipleObjects(miWorkerThreads, mahWorkerThreads, TRUE, 50) == WAIT_TIMEOUT))
	{
		Application->ProcessMessages();
	}
	
	//Clean cache of unexisting files. Guarded because the optimization workers could be writing it concurrently
	if (gudtOptions.bEnableCache)
	{
		EnterCriticalSection(&mudtCriticalSection);
		bool bAllUsers = true;
		unsigned long long lIniSize = 0;
		TCHAR *acRes = nullptr;

		lIniSize = clsUtil::SizeFile(clsUtil::GetIniPath(true));
		if (lIniSize == 0)
		{
			bAllUsers = false;
			lIniSize = clsUtil::SizeFile(clsUtil::GetIniPath(false));
		}
		if ((lIniSize > 2 * 1024 * 1024) && (lIniSize < 0x10000000))
		{
			//One character per byte at most (ANSI files), plus room for the terminators, so the whole section always fits and is never written back truncated
			unsigned long lChars = (unsigned long) lIniSize + 4;
			acRes = new TCHAR[lChars];
			if (acRes)
			{
				memset(acRes, 0, lChars * sizeof(TCHAR));
				unsigned long lSection = GetPrivateProfileSection(_T("Cache"), acRes, lChars, clsUtil::GetIniPath(bAllUsers));
				//A real section is never bigger than the file, so a result close to the buffer size means it was truncated, and writing it back would lose entries
				if ((lSection > 0) && (lSection < (lChars - 2)))
				{
					//Compact the section in place keeping only the entries whose file still exists, and write it back once: deleting every stale entry one by one rewrites the whole INI file each time
					TCHAR *pcRead = acRes;
					TCHAR *pcWrite = acRes;
					bool bRemoved = false;
					while (*pcRead != NULL)
					{
						size_t iEntryLen = _tcslen(pcRead) + 1;
						const TCHAR *pcValue = _tcschr(pcRead, '=');
						bool bKeep = true;
						if (pcValue)
						{
							//The value is the file name followed by its size and timestamp, separated by pipes (not allowed in file names)
							String sFileName = (String) (pcValue + 1);
							int iPipe = sFileName.Pos("|");
							if (iPipe > 0)
							{
								sFileName.SetLength(iPipe - 1);
							}
							bKeep = clsUtil::ExistsFile(sFileName.c_str());
						}
						if (bKeep)
						{
							if (pcWrite != pcRead)
							{
								memmove(pcWrite, pcRead, iEntryLen * sizeof(TCHAR));
							}
							pcWrite += iEntryLen;
						}
						else
						{
							bRemoved = true;
						}
						pcRead += iEntryLen;
					}
					if (bRemoved)
					{
						//Second terminator of the section
						*pcWrite = NULL;
						WritePrivateProfileSection(_T("Cache"), acRes, clsUtil::GetIniPath(bAllUsers));
					}
				}
				delete[] acRes;
			}
		}
		LeaveCriticalSection(&mudtCriticalSection);
	}
	Application->ProcessMessages();
}



//---------------------------------------------------------------------------
#pragma argsused
void __fastcall TfrmMain::FormResize(TObject *Sender)
{
	static unsigned int iOldWidth = 0;


	//During shutdown the form and its controls are being destroyed: do not
	//recalculate column widths nor touch the critical section
	if (gbClosing)
	{
		return;
	}

	//Guard grid access against the optimization workers
	EnterCriticalSection(&mudtCriticalSection);
		
	// Recalculate column widths when the window width changes.
	// On later resizes always redistribute (existing behaviour).
	// On the first call, also redistribute when the columns overflow OR leave a large
	// empty gap (typical of the first run with no INI, or of DFM defaults in a wide window).
	// If the restored INI widths already fill the grid reasonably, they are kept.
	if (iOldWidth != (unsigned int) Width)
	{
		bool bRedistribute = (iOldWidth != 0);
		if (!bRedistribute)
		{
			int iTotal = grdFiles->ColWidths[KI_GRID_FILE] + grdFiles->ColWidths[KI_GRID_EXTENSION] +
				grdFiles->ColWidths[KI_GRID_ORIGINAL] + grdFiles->ColWidths[KI_GRID_OPTIMIZED] +
				grdFiles->ColWidths[KI_GRID_STATUS];
			int iGridW = grdFiles->Width;
			// Overflow, or more than ~20% of the width left empty
			bRedistribute = (iTotal > iGridW + 20) || (iTotal < iGridW - (iGridW / 5));
		}
		if (bRedistribute)
		{
			//Prevent flickering
			LockWindowUpdate(Handle);

			int iFileWidth = (grdFiles->Width >> 1);
			int iOtherWidth = ((grdFiles->Width - iFileWidth) >> 2);
			if (iOtherWidth < 40)
			{
				iOtherWidth = 40;
			}
			grdFiles->ColWidths[KI_GRID_FILE] = iFileWidth;
			grdFiles->ColWidths[KI_GRID_EXTENSION] = iOtherWidth;
			grdFiles->ColWidths[KI_GRID_ORIGINAL] = iOtherWidth;
			grdFiles->ColWidths[KI_GRID_OPTIMIZED] = iOtherWidth;
			grdFiles->ColWidths[KI_GRID_STATUS] = iOtherWidth;

			//Reenable form updates
			LockWindowUpdate(NULL);
		}
		iOldWidth = (unsigned int) Width;
	}
	LeaveCriticalSection(&mudtCriticalSection);
}



//---------------------------------------------------------------------------
#pragma argsused
void __fastcall TfrmMain::grdFilesDrawCell(TObject *Sender, System::LongInt ACol, System::LongInt ARow, TRect &Rect, TGridDrawState State)
{
	//Guard grid access against the optimization workers, which update cells from their own threads
	EnterCriticalSection(&mudtCriticalSection);
	TCanvas *pCanvas = grdFiles->Canvas;


	//Color even and odd rows
	if (ARow == 0)
	{
		pCanvas->Brush->Color = clMenu;
	}
	else if (ARow & 1)
	{
		pCanvas->Brush->Color = clGradientInactiveCaption;
	}
	else
	{
		pCanvas->Brush->Color = clGradientActiveCaption;
	}

	//Color file column and the others
	if ((ARow > 0) && (ACol == KI_GRID_FILE))
	{
		pCanvas->Font->Color = clHotLight;
		pCanvas->Font->Style = pCanvas->Font->Style << fsUnderline;
	}
	else
	{
		pCanvas->Font->Color = clWindowText;
		//Do not inherit the underline of the previous cell
		pCanvas->Font->Style = pCanvas->Font->Style >> fsUnderline;
	}

	//Paddings follow the interface zoom
	int iPad = MulDiv(4, miAppliedZoom, 100);
	Rect.left -= (iPad >> 1);
	Rect.top -= (iPad >> 1);
	//TextRect already fills the whole rectangle with the brush color before drawing the text, so a FillRect before it only painted every cell twice
	//The cell is read once, and only the part that each branch draws is decoded
	String sCell = grdFiles->Cells[ACol][ARow];

	// Filename column with format 3: compact the full path to the current column width using the native path ellipsis
	if ((ARow > 0) && (ACol == KI_GRID_FILE) && (gudtOptions.iFilenameFormat == 3))
	{
		// Use the real full path (after the \r separator) so the ellipsis always reflects the current width
		String sFull = GetCellValue(sCell, 1);
		TRect rText = Rect;
		rText.left  += iPad;
		rText.top   += iPad;
		rText.right -= iPad;
		// Fill background first (DrawText does not)
		pCanvas->FillRect(Rect);
		DrawText(pCanvas->Handle, sFull.c_str(), -1, &rText,
			DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX | DT_PATH_ELLIPSIS | DT_END_ELLIPSIS);
	}
	//Left aligned
	else if ((ACol == KI_GRID_FILE) || (ACol == KI_GRID_EXTENSION) || (ACol == KI_GRID_STATUS))
	{
		pCanvas->TextRect(Rect, Rect.left + iPad, Rect.top + iPad, GetCellValue(sCell, 0));
	}
	//Right aligned
	else
	{
		pCanvas->TextRect(Rect, Rect.right - pCanvas->TextWidth(sCell) - iPad, Rect.top + iPad, sCell);
	}

	LeaveCriticalSection(&mudtCriticalSection);
}


//---------------------------------------------------------------------------
#pragma argsused
void __fastcall TfrmMain::grdFilesFixedCellClick(TObject *Sender, System::LongInt ACol, System::LongInt ARow)
{
	unsigned int iRows = (unsigned int) grdFiles->RowCount;

	if (iRows > 1)
	{
		static int iSortField = -1;
		static unsigned int iSortOrder = 0;

		Screen->Cursor = crAppStart;
		Application->ProcessMessages();
		SendMessage(grdFiles->Handle, WM_SETREDRAW, 0, 0);

		//Clear selection
		#if defined (__clang__)
			grdFiles->Selection = TGridRect({-1, 0, 4, -1});
		#else
			TGridRect rctSelection = {-1, 0, 4, -1};
			grdFiles->Selection = rctSelection;
		#endif

		if (ACol == iSortField)
		{
			iSortOrder = (iSortOrder + 1) & 1;
		}
		else
		{
			iSortOrder = 0;
		}
		iSortField = ACol;

		//Do the sort using a temporary TStringList
		TStringList *lstTemp = new TStringList();
		lstTemp->CaseSensitive = true;
		lstTemp->Duplicates = System::Classes::dupAccept;
		String sValue;
		for (unsigned int iRow = 1; iRow < iRows; iRow++)
		{
			if ((iSortField == KI_GRID_ORIGINAL) || (iSortField == KI_GRID_OPTIMIZED))
			{
				//Same result as FormatFloat with 19 zeros, but much faster
				sValue = (String) ParseNumberThousand(grdFiles->Cells[iSortField][(int) iRow]);
				if (sValue.Length() < 19)
				{
					sValue = StringOfChar(L'0', 19 - sValue.Length()) + sValue;
				}
			}
			else
			{
				sValue = grdFiles->Cells[iSortField][(int) iRow];
			}
			sValue += "|" + grdFiles->Cells[KI_GRID_FILE][(int) iRow] + "|" + grdFiles->Cells[KI_GRID_EXTENSION][(int) iRow] + "|" + grdFiles->Cells[KI_GRID_ORIGINAL][(int) iRow] + "|" + grdFiles->Cells[KI_GRID_OPTIMIZED][(int) iRow] + "|" + grdFiles->Cells[KI_GRID_STATUS][(int) iRow];
			lstTemp->Add(sValue);
		}
		lstTemp->UseLocale = false;
		lstTemp->Sort();

		//Copy the TSTringList to the grid
		TStringDynArray asValue;
		TStringList *lstRow = new TStringList();
		for (unsigned int iRow = 1; iRow < iRows; iRow++)
		{
			//ToDo: Try to avoid TStringDynArray
			lstRow->Clear();
			if (iSortOrder == 0)
			{
				asValue = SplitString(lstTemp->Strings[(int) iRow - 1], "|");
			}
			else
			{
				asValue = SplitString(lstTemp->Strings[(int) (iRows - iRow - 1)], "|");
			}

			lstRow->Add(asValue[1]); //0: File
			lstRow->Add(asValue[2]); //1: Extension (Type)
			lstRow->Add(asValue[3]); //2: Original size
			lstRow->Add(asValue[4]); //3: Optimized size
			lstRow->Add(asValue[5]); //4: Status
			grdFiles->Rows[(int) iRow] = lstRow;
		}
		delete lstRow;
		delete lstTemp;

		RefreshStatus();
		SendMessage(grdFiles->Handle, WM_SETREDRAW, 1, 0);
		grdFiles->Repaint();

		Screen->Cursor = crDefault;
	}
}


//---------------------------------------------------------------------------
#pragma argsused
void __fastcall TfrmMain::grdFilesMouseMove(TObject *Sender, TShiftState Shift, int X, int Y)
{
	int iRow, iCol;
	
	
	//Guard grid access against the optimization workers, and draw the hint with the rollover cell contents
	EnterCriticalSection(&mudtCriticalSection);
	grdFiles->MouseToCell(X, Y, iCol, iRow);
	if ((iRow >= 0) && (iCol >=0))
	{
		String sHint = GetCellValue(grdFiles->Cells[iCol][iRow], 1);
		//Only when the text changes: cancelling it for every movement made the hint vanish while the mouse moved inside the same cell
		if (grdFiles->Hint != sHint)
		{
			Application->CancelHint();
			grdFiles->Hint = sHint;
		}
	}
	LeaveCriticalSection(&mudtCriticalSection);
}


//---------------------------------------------------------------------------
void __fastcall TfrmMain::grdFilesDblClick(TObject *Sender)
{
	//Double click synonymous Open selected file
	//The Open action is disabled while optimizing: do not bypass it from here, or the cell would be read while the workers update the grid
	if (!gbProcess)
	{
		actOpenExecute(Sender);
	}
}



//---------------------------------------------------------------------------
void __fastcall TfrmMain::stbMainDrawPanel(TStatusBar *StatusBar, TStatusPanel *Panel, const TRect &Rect)
{
	//Resize progress according to statusbar size
	if (Panel == StatusBar->Panels->Items[1])
	{
		//A single call, so the progress bar is moved and resized once (and not at all when nothing changed) instead of up to four times
		pgbProgress->SetBounds(Rect.left, Rect.top, Rect.right - Rect.left, Rect.bottom - Rect.top);
	}
}



//---------------------------------------------------------------------------
void __fastcall TfrmMain::FormKeyDown(TObject *Sender, WORD &Key, TShiftState Shift)
{
	//These keys modify the grid, which the optimization workers are using
	if ((gbProcess) && ((Key == VK_F5) || ((Shift.Contains(ssCtrl)) && ((Key == 'V') || (Key == 'X')))))
	{
		return;
	}
	//Ctrl-F5 reload grid
	if ((Key == VK_F5) && (Shift.Contains(ssCtrl)))
	{
		unsigned int iRows = (unsigned int) grdFiles->RowCount;
		if (iRows > 1)
		{
			Screen->Cursor = crAppStart;
			Application->ProcessMessages();
			SendMessage(grdFiles->Handle, WM_SETREDRAW, 0, 0);

			//Remove already optimized files, compacting the grid in only one pass
			unsigned int iKeptRows = 1;
			for (unsigned int iRow = 1; iRow < iRows; iRow++)
			{
				if (PosEx(_("Done"), grdFiles->Cells[KI_GRID_STATUS][(int) iRow]) == 0)
				{
					if (iKeptRows != iRow)
					{
						grdFiles->Rows[(int) iKeptRows] = grdFiles->Rows[(int) iRow];
					}
					iKeptRows++;
				}
			}
			grdFiles->RowCount = (int) iKeptRows;
		}
	}

	//F5 refresh grid. Will be triggered after CTRL-F5 too
	if (Key == VK_F5)
	{
		unsigned int iRows = (unsigned int) grdFiles->RowCount;
		Screen->Cursor = crAppStart;
		Application->ProcessMessages();
		SendMessage(grdFiles->Handle, WM_SETREDRAW, 0, 0);
		if (iRows > 1)
		{
			for (unsigned int iRow = 1; iRow < iRows; iRow++)
			{
				//grdFiles->Cells[KI_GRID_ORIGINAL][(int) iRow]->BeginUpdate();
				//grdFiles->Cells[KI_GRID_FILE][(int) iRow] = asValue[1];
				//grdFiles->Cells[KI_GRID_EXTENSION][(int) iRow] = asValue[2];
				grdFiles->Cells[KI_GRID_ORIGINAL][(int) iRow] = FormatNumberThousand(clsUtil::SizeFile(GetCellValue(grdFiles->Cells[KI_GRID_FILE][iRow], 1).c_str()));
				//grdFiles->Cells[KI_GRID_OPTIMIZED][(int) iRow] = asValue[4];
				//grdFiles->Cells[KI_GRID_STATUS][(int) iRow] = asValue[5];
				//grdFiles->Rows[(int) iRow]->EndUpdate();
			}
		}
		//Restore always, also when the grid became empty after CTRL-F5, or redraw would stay disabled and the cursor busy
		RefreshStatus();
		SendMessage(grdFiles->Handle, WM_SETREDRAW, 1, 0);
		grdFiles->Repaint();
		Screen->Cursor = crDefault;
	}
	//Make DEL Shortcut
	else if ((Key == VK_DELETE) && (!Shift.Contains(ssCtrl)) && (!Shift.Contains(ssAlt)) && (!Shift.Contains(ssShift)))
	{
		actRemoveExecute(Sender);
	}
	//CTRL-C
	else if ((Shift.Contains(ssCtrl)) && (!Shift.Contains(ssAlt)) && (Key == 'C'))
	{
		//Save grid contents
		CopyFilesToClipboard();
	}
	//CTRL-V
	else if ((Shift.Contains(ssCtrl)) && (!Shift.Contains(ssAlt)) && (Key == 'V'))
	{
		TClipboard *oClipBoard = Clipboard();
		if (oClipBoard->HasFormat(CF_TEXT))
		{
			actClearExecute(Sender);
			String sRow = oClipBoard->AsText;
			TStringDynArray asRows = SplitString(sRow, "\n");
			unsigned int iRows = asRows.Length;
			if (iRows > 0)
			{
				AddFilesInitializeExist();
				for (unsigned int iRow = 0; iRow < iRows; iRow++)
				{
					AddFiles(Trim(asRows[iRow]).c_str());
				}
				RefreshStatus();
				grdFiles->Enabled = true;
			}
		}
	}
	//CTRL-X
	else if ((Shift.Contains(ssCtrl)) && (!Shift.Contains(ssAlt)) && (Key == 'X'))
	{
		//Save grid contents, and clear it
		CopyFilesToClipboard();
		actClearExecute(Sender);
	}
	//CTRL-S
	else if ((Shift.Contains(ssCtrl)) && (Key == 'S'))
	{
		//Todo
	}
	//CTRL-L
	else if ((Shift.Contains(ssCtrl)) && (Key == 'L'))
	{
		//Todo
	}

}



//---------------------------------------------------------------------------
#pragma argsused
void __fastcall TfrmMain::actAddExecute(TObject *Sender)
{
	//Prevent adding files while optimizing, because workers would be accessing the grid rows concurrently
	if (gbProcess)
	{
		return;
	}

	//Add files
	if (dlgAddFiles->Execute())
	{
		Screen->Cursor = crAppStart;
		grdFiles->Enabled = false;  //Prevent grid modifications while adding files
		Application->ProcessMessages();

		TStrings *strFiles = dlgAddFiles->Files;
		AddFilesInitializeExist();
		for (int iCount = strFiles->Count; iCount > 0; iCount--)
		{
			AddFiles(strFiles->Strings[iCount - 1].c_str());
		}
		grdFiles->Enabled = true;
		RefreshStatus();
		Screen->Cursor = crDefault;
	}
}


//---------------------------------------------------------------------------
#pragma argsused
void __fastcall TfrmMain::actAddFolderExecute(TObject *Sender)
{
	//Prevent adding files while optimizing, because workers would be accessing the grid rows concurrently
	if (gbProcess)
	{
		return;
	}

	String sDirectory = "";

	if (SelectDirectory(_("Add folder"), _T(""), sDirectory, TSelectDirExtOpts() << sdNewFolder << sdShowEdit << sdShowShares << sdNewUI << sdValidateDir, this))
	{
		Screen->Cursor = crAppStart;
		grdFiles->Enabled = false;  //Prevent grid modifications while adding files
		Application->ProcessMessages();

		AddFilesInitializeExist();
		AddFiles(sDirectory.c_str());

		RefreshStatus();
		grdFiles->Enabled = true;
		Screen->Cursor = crDefault;
	}
}



//---------------------------------------------------------------------------
#pragma argsused
void __fastcall TfrmMain::actRemoveExecute(TObject *Sender)
{
	int iSelectedRow1 = grdFiles->Selection.Top;
	int iSelectedRow2 = grdFiles->Selection.Bottom;

	//Nothing selected (such as after sorting), so nothing to remove
	if ((iSelectedRow1 < 1) || (iSelectedRow2 < iSelectedRow1))
	{
		return;
	}

	//Prevent removing rows while optimizing, because workers would be accessing them concurrently
	if (gbProcess)
	{
		return;
	}

	SendMessage(grdFiles->Handle, WM_SETREDRAW, 0, 0);
	int iRows = grdFiles->RowCount - 1;
	int iRemoved = iSelectedRow2 - iSelectedRow1 + 1;
	//Only move rows that really exist, the last ones are discarded when reducing RowCount
	for (int iRow = iSelectedRow1; (iRow + iRemoved) <= iRows; iRow++)
	{
		grdFiles->Rows[iRow] = grdFiles->Rows[iRow + iRemoved];
	}
	grdFiles->RowCount -= iRemoved;
	RefreshStatus();
	SendMessage(grdFiles->Handle, WM_SETREDRAW, 1, 0);
	grdFiles->Repaint();
}



//---------------------------------------------------------------------------
#pragma argsused
void __fastcall TfrmMain::actClearExecute(TObject *Sender)
{
	//Prevent clearing the grid while optimizing, because workers would be accessing the rows concurrently
	if (gbProcess)
	{
		return;
	}

	SendMessage(grdFiles->Handle, WM_SETREDRAW, 0, 0);

	Caption = Application->Name;
	Application->Title = Caption;		
	stbMain->Panels->Items[0]->Text = "";
	stbMain->Hint = stbMain->Panels->Items[0]->Text;
		
	grdFiles->RowCount = 1;
	RefreshStatus();
	SendMessage(grdFiles->Handle, WM_SETREDRAW, 1, 0);
	grdFiles->Repaint();
}




//---------------------------------------------------------------------------
#pragma argsused
void __fastcall TfrmMain::actHelpExecute(TObject *Sender)
{
	ShellExecute(Handle, _T("open"), Application->HelpFile.c_str(), _T(""), _T(""), SW_SHOWNORMAL);
}



//---------------------------------------------------------------------------
#pragma argsused
void __fastcall TfrmMain::actOpenExecute(TObject *Sender)
{
	unsigned int iRow = (unsigned int) grdFiles->Row;
	unsigned int iCol = (unsigned int) grdFiles->Col;


	if ((iRow > 0) && (iCol == KI_GRID_FILE))
	{
		ShellExecute(Handle, _T("open"), GetCellValue(grdFiles->Cells[KI_GRID_FILE][(int) iRow], 1).c_str(), _T(""), _T(""), SW_SHOWNORMAL);
	}
}



//---------------------------------------------------------------------------
#pragma argsused
void __fastcall TfrmMain::actOpenFolderExecute(TObject *Sender)
{
	unsigned int iRow = (unsigned int) grdFiles->Row;
	unsigned int iCol = (unsigned int) grdFiles->Col;


	if ((iRow > 0) && (iCol == KI_GRID_FILE))
	{
		ShellExecute(Handle, _T("open"), ExtractFilePath(GetCellValue(grdFiles->Cells[KI_GRID_FILE][(int) iRow], 1)).c_str(), _T(""), _T(""), SW_SHOWNORMAL);
	}
}



//---------------------------------------------------------------------------
static unsigned long long lSavedBytes, lTotalBytes;
static unsigned int iStartTicks, iEndTicks;
static String sPluginsDirectory;
//Multithreaded optimization: next row to process, files already processed, and serial number for temporary file names
static long miNextRow;
static long miProcessedFiles;
static long miTmpFileSerial;
//Include and exclude masks split in uppercase when the optimization starts, into plain buffers without reference counting, so the concurrent workers can read them without any synchronization
static TCHAR **sppacIncludeMasks = nullptr;
static unsigned int siIncludeMasks = 0;
static TCHAR **sppacExcludeMasks = nullptr;
static unsigned int siExcludeMasks = 0;
//Same for the plugins disabled by the user
static TCHAR **sppacDisablePluginMasks = nullptr;
static unsigned int siDisablePluginMasks = 0;



//---------------------------------------------------------------------------
//Splits a comma separated mask list into uppercase plain buffers without reference counting, so the concurrent workers can read them without any synchronization. Called from the main thread only, when the optimization starts
static void SplitMaskList(const TCHAR *pacMaskList, TCHAR ***spppacMasks, unsigned int *siMasks)
{
	unsigned int iMask;


	//Free the masks of the previous optimization, in case the options changed
	if (*spppacMasks != nullptr)
	{
		for (iMask = 0; iMask < (*siMasks); iMask++)
		{
			delete[] (*spppacMasks)[iMask];
		}
		delete[] *spppacMasks;
		*spppacMasks = nullptr;
	}
	*siMasks = 0;
	if (pacMaskList[0] != NULL)
	{
		//Upper case and split with the VCL functions, and then deep copy every mask into plain buffers, so no reference counted String is ever shared with the workers
		TStringDynArray asMasks = SplitString(((String) pacMaskList).UpperCase(), _T(","));
		unsigned int iMasks = (unsigned int) asMasks.Length;
		*spppacMasks = new TCHAR *[iMasks];
		if (*spppacMasks != nullptr)
		{
			bool bCopied = true;
			for (iMask = 0; (iMask < iMasks) && (bCopied); iMask++)
			{
				size_t iLen = (size_t) asMasks[iMask].Length();
				TCHAR *pacMask = new TCHAR[iLen + 1];
				if (pacMask != nullptr)
				{
					clsUtil::CopyString(pacMask, iLen + 1, asMasks[iMask].c_str());
					(*spppacMasks)[iMask] = pacMask;
				}
				else
				{
					bCopied = false;
				}
			}
			if (bCopied)
			{
				*siMasks = iMasks;
			}
			else
			{
				//Free the partially copied masks, and continue without masks: they are optional
				while (iMask > 0)
				{
					delete[] (*spppacMasks)[--iMask];
				}
				delete[] *spppacMasks;
				*spppacMasks = nullptr;
			}
		}
	}
}



//---------------------------------------------------------------------------
void __fastcall TfrmMain::actOptimizeExecute(TObject *Sender)
{
	unsigned int iCount;
	TCHAR acTmpFile[PATH_MAX + 1] = _T("");


	//Prevent reentrancy: it could be triggered again (such as from the command line arguments) while already optimizing
	if (gbProcess)
	{
		return;
	}

	gbProcess = true;
	gbStop = false;
	//grdFiles->Enabled = false;
	
	//First the interface shows that it is optimizing, and only then the ads are refreshed (it can take a while)
	RefreshStatus();
	UpdateAds();

	if (GetModuleFileName(NULL, acTmpFile, (sizeof(acTmpFile) / sizeof(TCHAR)) - 1) != 0)
	{
		//Remove the file name, checking it first because _tcsrchr returns NULL and would crash when there is no backslash
		TCHAR *pcBackslash = _tcsrchr(acTmpFile, '\\');
		if (pcBackslash)
		{
			*pcBackslash = NULL;
		}
	}
	#if defined(_WIN64)
		_tcscat(acTmpFile, _T("\\Plugins64\\"));
	#else
		_tcscat(acTmpFile, _T("\\Plugins32\\"));
	#endif
	sPluginsDirectory = clsUtil::GetShortName((String) acTmpFile);
	SetCurrentDirectory(sPluginsDirectory.c_str());

	lSavedBytes = 0;
	lTotalBytes = 0;
	iStartTicks = GetTickCount();
	
	//Split the include and exclude masks once, so the concurrent workers do not have to split and upper case them again for every file
	SplitMaskList(gudtOptions.acIncludeMask, &sppacIncludeMasks, &siIncludeMasks);
	SplitMaskList(gudtOptions.acExcludeMask, &sppacExcludeMasks, &siExcludeMasks);
	SplitMaskList(gudtOptions.acDisablePluginMask, &sppacDisablePluginMasks, &siDisablePluginMasks);
	
	unsigned int iRows = (unsigned int) grdFiles->RowCount;


	//Number of threads to use: -1 or 0 is auto-detect (one per processor). AllowDuplicates is not compatible with multiple threads, because the same file could be optimized twice at the same time
	int iThreads = gudtOptions.iThreads;
	if (gudtOptions.bAllowDuplicates)
	{
		iThreads = 1;
	}
	else if (iThreads <= 0)
	{
		iThreads = (int) gudtSystemInfo.dwNumberOfProcessors;
	}
	//WaitForMultipleObjects does not support more than MAXIMUM_WAIT_OBJECTS handles
	if (iThreads > MAXIMUM_WAIT_OBJECTS)
	{
		iThreads = MAXIMUM_WAIT_OBJECTS;
	}
	if (iThreads < 1)
	{
		iThreads = 1;
	}
	//No point in creating more threads than files to process
	if (iThreads > (int) (iRows - 1))
	{
		iThreads = (int) (iRows - 1);
	}
	
	if (gudtOptions.bDisableSleep)
	{
		SetThreadExecutionState(ES_CONTINUOUS | ES_SYSTEM_REQUIRED | ES_AWAYMODE_REQUIRED);
	}

	//Builds in advance the static cache of known extensions (a known one does not read any file), so the workers never initialize it concurrently
	GetExtensionByContent(_T("x.png"));
	//Same for the CRC-32 tables: files loaded from the persistent grid never went through Hash while being added, so the first call could come from several workers at once
	clsUtil::Crc32(nullptr, 0);

	//Use multiple threads when there is more than one thread and more than 2 files
	if ((iThreads > 1) && (iRows > 2))
	{
		//Rows are pulled by the workers, so each file is processed exactly once. InterlockedIncrement returns the incremented value
		miNextRow = 0;
		miProcessedFiles = 0;

		TThread **aoThreads = new TThread *[iThreads];
		HANDLE *ahThreads = new HANDLE[iThreads];
		for (int iThread = 0; iThread < iThreads; iThread++)
		{
			aoThreads[iThread] = TThread::CreateAnonymousThread
			(
				[this, iRows]()
				{
					#if !defined(_WIN64)
						//File system redirection is per thread, so it must also be disabled in the workers
						typedef BOOL (WINAPI Wow64DisableWow64FsRedirectionType)(PVOID *);
						Wow64DisableWow64FsRedirectionType *Wow64DisableWow64FsRedirectionProc = (Wow64DisableWow64FsRedirectionType *) GetProcAddress(GetModuleHandle(_T("KERNEL32.DLL")), "Wow64DisableWow64FsRedirection");
						if (Wow64DisableWow64FsRedirectionProc)
						{
							PVOID pOldWin64Redirect = nullptr;
							Wow64DisableWow64FsRedirectionProc(&pOldWin64Redirect);
						}
					#endif
					try
					{
						//Process rows until none is left or the optimization is stopped
						int iRow;
						while (((iRow = InterlockedIncrement(&miNextRow)) < (int) iRows) && (!gbStop))
						{
							actOptimizeFor(nullptr, iRow);
						}
					}
					catch (...)
					{
					}
				}
			);
			//FreeOnTerminate is true by default, but we need the handle alive until waited, and delete the object ourselves
			aoThreads[iThread]->FreeOnTerminate = false;
			aoThreads[iThread]->Start();
			ahThreads[iThread] = (HANDLE) aoThreads[iThread]->Handle;
		}
		//Publish the worker handles, so closing can wait for them before the form gets destroyed
		memcpy(mahWorkerThreads, ahThreads, (size_t) iThreads * sizeof(HANDLE));
		miWorkerThreads = iThreads;

		//The main thread stays here keeping the interface responsive, so Stop and closing still work. It wakes up often because workers wait for it when they use Synchronize (such as moving files to the Recycle Bin), but the interface is only refreshed 10 times per second
		unsigned int iLastRefresh = 0;
		while (WaitForMultipleObjects(iThreads, ahThreads, TRUE, 0) == WAIT_TIMEOUT)
		{
			//Sleeps until a message arrives (Synchronize posts one, so the workers do not wait up to 10 ms for the main thread) or 10 ms pass. Not waiting on the thread handles together with the messages, because with bWaitAll it would not return for messages until every thread had finished
			MsgWaitForMultipleObjects(0, NULL, false, 10, QS_ALLINPUT);
			Application->ProcessMessages();
			//The form could be getting closed and destroyed while processing messages, so stop touching it
			if (gbClosing)
			{
				break;
			}
			if ((GetTickCount() - iLastRefresh) >= 100)
			{
				iLastRefresh = GetTickCount();
				//Read the shared counters under the critical section, so 64 bit values are never read partially updated
				EnterCriticalSection(&mudtCriticalSection);
				unsigned long long lTotalBytesNow = lTotalBytes;
				unsigned long long lSavedBytesNow = lSavedBytes;
				LeaveCriticalSection(&mudtCriticalSection);
				RefreshStatus(true, (unsigned int) miProcessedFiles, lTotalBytesNow, lSavedBytesNow);
				pgbProgress->Position = (int) miProcessedFiles;
				clsUtil::SetTaskListProgress((unsigned int) pgbProgress->Position, (unsigned int) pgbProgress->Max);
			}
		}

		for (int iThread = 0; iThread < iThreads; iThread++)
		{
			delete aoThreads[iThread];
		}
		delete[] aoThreads;
		delete[] ahThreads;
		miWorkerThreads = 0;
		iCount = (unsigned int) miProcessedFiles + 1;
		//The last refresh could be up to 100 ms old, so show the final progress
		if (!gbClosing)
		{
			pgbProgress->Position = (int) miProcessedFiles;
			clsUtil::SetTaskListProgress((unsigned int) pgbProgress->Position, (unsigned int) pgbProgress->Max);
		}
	}
	//Use regular for loop
	else
	{
		for (iCount = 1; iCount < iRows; iCount++)
		{
			if (!gbStop)
			{
				//An exception must not leave the optimization flag set forever (the interface would stay disabled), so it only skips that file, like the workers do
				try
				{
					actOptimizeFor(NULL, (int) iCount);
				}
				catch (...)
				{
				}
			}
			else
			{
				break;
			}
		}
	}

	//Restore Windows error handling, which was disabled by the plugins launched from any worker
	SetErrorMode(0);
	gbProcess = false;
	//When closing, the form has already been destroyed while processing messages, so do not touch it nor the interface anymore
	if (gbClosing)
	{
		return;
	}
	//grdFiles->Enabled = true;
	unsigned int iPercentBytes;
	if (lTotalBytes != 0)
	{
		//The percentage is the part that was saved, not the part that remains, or 0 bytes saved would show as 100%
		iPercentBytes = ((unsigned int) ((double) lSavedBytes / lTotalBytes * 100));
	}
	else
	{
		iPercentBytes = 0;
	}
	iEndTicks = GetTickCount();


	//Required indirection
	String sCaption;
	TCHAR acTime[64];
	StrFromTimeInterval(acTime, (sizeof(acTime) / sizeof(TCHAR)) - 1, (unsigned int) (iEndTicks - iStartTicks), 3);
	sCaption.printf(_((TCHAR *) _T("%s files processed. %s bytes saved (%s%%). Elapsed time %s")), FormatNumberThousand(iCount - 1).c_str(), FormatNumberThousand(lSavedBytes).c_str(), FormatNumberThousand(iPercentBytes).c_str(), acTime);

	if (Visible)
	{
		stbMain->Panels->Items[0]->Text = sCaption;
		stbMain->Hint = stbMain->Panels->Items[0]->Text;
		Caption = stbMain->Hint + " - " + Application->Name;
		Application->Title = Caption;	
		RefreshStatus(false);
	}

	if (gudtOptions.bBeepWhenDone)
	{	
		//Beep before the message, which does not return until the user closes it
		FlashWindow(Handle, false);
		MessageBeep(0xFFFFFFFF);
		clsUtil::MsgBox(Handle, sCaption.c_str(), _((TCHAR *) _T("Done")), MB_ICONINFORMATION | MB_OK);
	}

	if (gudtOptions.bDisableSleep)
	{
		SetThreadExecutionState(ES_CONTINUOUS);
	}

	//Stopping by hand must not shut the computer down
	if ((gudtOptions.bShutdownWhenDone) && (!gbStop))
	{
		if (!clsUtil::ShutdownWindows(0))
		{
			clsUtil::MsgBox(Handle, _((TCHAR *) _T("Error trying to automatically shutdown the system.")), _((TCHAR *) _T("Shutdown")), MB_OK | MB_ICONERROR);
		}
	}
	
	//Stopping by hand must not lose the files that were still pending
	if ((gudtOptions.bClearWhenComplete) && (!gbStop))
	{
		actClearExecute(Sender);
	}
}



//---------------------------------------------------------------------------
#pragma argsused
void __fastcall TfrmMain::actStopExecute(TObject *Sender)
{
	gbStop = true;
}



//---------------------------------------------------------------------------
#pragma argsused
void __fastcall TfrmMain::actOptionsExecute(TObject *Sender)
{
	frmOptions = new TfrmOptions(Application);

	//Set child window as on top, of current window already is
	if (FormStyle == fsStayOnTop)
	{
		FormStyle = fsNormal;
		frmOptions->FormStyle = fsStayOnTop;
	}
	frmOptions->PopupParent = this;
	try
	{
		frmOptions->ShowModal();
	}
	__finally
	{
		if (gudtOptions.bAlwaysOnTop)
		{
			FormStyle = fsStayOnTop;
		}
		delete frmOptions;
		frmOptions = nullptr;
	}
}



//---------------------------------------------------------------------------
#pragma argsused
void __fastcall TfrmMain::actAboutExecute(TObject *Sender)
{
	frmAbout = new TfrmAbout(Application);

	//Set child window as on top, of current window already is
	if (FormStyle == fsStayOnTop)
	{
		FormStyle = fsNormal;
		frmAbout->FormStyle = fsStayOnTop;
	}
	frmAbout->PopupParent = this;
	try
	{
		frmAbout->ShowModal();
	}
	__finally
	{
		if (gudtOptions.bAlwaysOnTop)
		{
			FormStyle = fsStayOnTop;
		}
		delete frmAbout;
		frmAbout = nullptr;
	}
}



//---------------------------------------------------------------------------
#pragma argsused
void __fastcall TfrmMain::actExitExecute(TObject *Sender)
{
	gbStop = true;
	Hide();
	Application->ProcessMessages();
	Close();
}



//---------------------------------------------------------------------------
#pragma argsused
void __fastcall TfrmMain::actInformationExecute(TObject *Sender)
{
	String sExtension;
	String sText = "";
	TCHAR acTime[64];


	//Get all supported extensions
	TStringDynArray asExtension;
	
	asExtension = SplitString((KS_EXTENSION_ALL + (ReplaceStr((String) gudtOptions.acJSAdditionalExtensions, ",", " ")).UpperCase() + " "), " ");
	unsigned int iExtensionLen = (unsigned int) asExtension.Length;

	//Sort them alphabetically
	TStringList *lstTemp = new TStringList();
	lstTemp->CaseSensitive = true;
	//Duplicates are only ignored by a sorted list, and many extensions are shared by several groups
	lstTemp->UseLocale = false;
	lstTemp->Sorted = true;
	lstTemp->Duplicates = System::Classes::dupIgnore;
	for (unsigned int iExtension = 0; iExtension < iExtensionLen; iExtension++)
	{
		sExtension = asExtension[iExtension];
		//Dont push it if empty extension
		if (sExtension.Length() > 1)
		{
			lstTemp->Add(sExtension);
		}
	}
	iExtensionLen = (unsigned int) lstTemp->Count;
	for (unsigned int iExtension = 0; iExtension < iExtensionLen; iExtension++)
	{
		sExtension = lstTemp->Strings[(int) iExtension].UpperCase();
		//Check if it is not the last extension
		if (iExtension != (iExtensionLen - 1))
		{
			sText += sExtension + ", ";
		}
		else
		{
			sText.cat_printf(_((TCHAR *) _T("and %s file formats among many others.")), sExtension.c_str());
		}
	}
	delete lstTemp;
	
	sText = Application->Name + _(" is an advanced file optimizer featuring a lossless (no quality loss) file size reduction that supports: ") + sText;

	sText += _((TCHAR *) _T("\n\nDONATOR INFORMATION\n"));
	if (gudtOptions.acDonation[0] != NULL)
	{
		sText += (String) gudtOptions.acDonation;

	}
	else
	{
		sText += _((TCHAR *) _T("Have not donated yet!"));

	}
	sText.cat_printf(_((TCHAR *) _T("\nUser since %s %s")), Application->Name.c_str(), gudtOptions.acVersion);


	StrFromTimeInterval(acTime, (sizeof(acTime) / sizeof(TCHAR)) - 1, (unsigned int) min(gudtOptions.lStatTime * 1000, (unsigned long long) UINT_MAX), 3);
	sText.cat_printf(_((TCHAR *) _T("\n\nUSAGE STATISTICS\n"
		"- Time: %s\n"
		"- Opens: %s\n"
		"- Files: %s\n"
		"- Total: %s\n"
		"- Saved: %s\n")),
		acTime, 
		FormatNumberThousand(gudtOptions.iStatOpens).c_str(),
		FormatNumberThousand(gudtOptions.iStatFiles).c_str(),
		FormatNumberThousandUnit(gudtOptions.lStatTotalBytes).c_str(),
		FormatNumberThousandUnit(gudtOptions.lStatSavedBytes).c_str());

	clsUtil::MsgBox(Handle, sText.c_str(), _((TCHAR *) _T("Information")), MB_ICONINFORMATION | MB_OK);
}



//---------------------------------------------------------------------------
#pragma argsused
void __fastcall TfrmMain::actDonateExecute(TObject *Sender)
{
	ShellExecute(Handle, _T("open"), KS_APP_DONATE_URL, _T(""), _T(""), SW_SHOWMAXIMIZED);
}


//---------------------------------------------------------------------------
#pragma argsused
void __fastcall TfrmMain::actOptimizeFor(TObject *Sender, int AIndex)
{
	int iCount = AIndex;
	unsigned int iStartTicks, iEndTicks;
	FILETIME udtFileCreated = {}, udtFileAccessed = {}, udtFileModified = {};
	String sInputFile, sFlags;
	//Thread local deep copy of the plugins directory, so concurrent workers do not update the reference count of the static shared String
	String sPluginsDirectory;
	EnterCriticalSection(&mudtCriticalSection);
	sPluginsDirectory = ::sPluginsDirectory.c_str();
	LeaveCriticalSection(&mudtCriticalSection);


	iStartTicks = GetTickCount();
	
	//Read the cell under the critical section, and deep copy it so the String is thread private, because its reference count is not updated atomically
	EnterCriticalSection(&mudtCriticalSection);
	sInputFile = GetCellValue(grdFiles->Cells[KI_GRID_FILE][iCount], 1).c_str();
	LeaveCriticalSection(&mudtCriticalSection);

	//Required indirection
	String sCaption;
	//Interface updates only on the main thread: workers would compete for them, and in multithreaded mode the main loop already refreshes the global progress. Workers do not use the caption, so they do not even build it
	if (GetCurrentThreadId() == MainThreadID)
	{
		sCaption.printf(_((TCHAR *) _T("Processing %s...")), sInputFile.c_str());
		stbMain->Panels->Items[0]->Text = sCaption;

		stbMain->Hint = stbMain->Panels->Items[0]->Text;
		//Caption = stbMain->Hint + " - " + Application->Name;
		//Application->Title = Caption;
		pgbProgress->Position = iCount;
		clsUtil::SetTaskListProgress((unsigned int) pgbProgress->Position, (unsigned int) pgbProgress->Max);
		grdFiles->Row = iCount;
		Application->ProcessMessages();
	}


	bool bExcluded = false;

	//If include mask is specified, by default all files are excluded, except the specified. The masks were already split and upper cased when the optimization started, so the file name is upper cased only once for both mask lists
	String sInputFileUpper;
	if ((siIncludeMasks > 0) || (siExcludeMasks > 0))
	{
		sInputFileUpper = sInputFile.UpperCase();
	}
	if (siIncludeMasks > 0)
	{
		bExcluded = true;
		for (unsigned int iMask = 0; iMask < siIncludeMasks; iMask++)
		{
			if ((sppacIncludeMasks[iMask][0] != NULL) && (_tcsstr(sInputFileUpper.c_str(), sppacIncludeMasks[iMask]) != nullptr))
			{
				bExcluded = false;
				break;
			}
		}
	}

	if (siExcludeMasks > 0)
	{
		for (unsigned int iMask = 0; iMask < siExcludeMasks; iMask++)
		{
			if ((sppacExcludeMasks[iMask][0] != NULL) && (_tcsstr(sInputFileUpper.c_str(), sppacExcludeMasks[iMask]) != nullptr))
			{
				bExcluded = true;
				break;
			}
		}
	}
	

	//Skip it if already in optimization cache
	if (gudtOptions.bEnableCache)
	{
		//Read the cell under the critical section, because the main thread also reads it while painting
		bool bCached = false;
		EnterCriticalSection(&mudtCriticalSection);
		bCached = (grdFiles->Cells[KI_GRID_STATUS][(int) iCount] == _("Optimized"));
		LeaveCriticalSection(&mudtCriticalSection);
		if (bCached)
		{
			bExcluded = true;
		}
	}

	//Check file still exists and is not to be excluded
	if ((clsUtil::ExistsFile(sInputFile.c_str())) && (!bExcluded))
	{
		unsigned int iFileAttributes = INVALID_FILE_ATTRIBUTES;
		//GetApiName avoids GetShortPathName for short paths, which is expensive (it queries the disk for every component of the path), and it is needed for every attribute and timestamp operation, so cache it
		String sApiName = clsUtil::GetApiName(sInputFile.c_str());
		if (gudtOptions.bKeepAttributes)
		{
			//If get timestamp fails, set to null
			if (!clsUtil::GetFileTimestamp(sApiName.c_str(), &udtFileCreated, &udtFileAccessed, &udtFileModified))
			{
				udtFileCreated.dwLowDateTime = 0;
				udtFileCreated.dwHighDateTime = 0;
				/* udtFileAccessed.dwLowDateTime = 0;
				udtFileAccessed.dwHighDateTime = 0;
				udtFileModified.dwLowDateTime = 0;
				udtFileModified.dwHighDateTime = 0; */
			}
			iFileAttributes = GetFileAttributes(sApiName.c_str());
		}
	
		if (!gudtOptions.bDoNotUseRecycleBin)
		{
			EnterCriticalSection(&mudtCriticalSection);
			grdFiles->Cells[KI_GRID_STATUS][iCount] = _("Copying to Recyclebin...");
			LeaveCriticalSection(&mudtCriticalSection);
			clsUtil::CopyToRecycleBin(sInputFile.c_str());
		}

		if (!gudtOptions.bDoNotCreateBackups)
		{
			EnterCriticalSection(&mudtCriticalSection);
			grdFiles->Cells[KI_GRID_STATUS][iCount] = _("Creating backup...");
			LeaveCriticalSection(&mudtCriticalSection);
			clsUtil::CopyFile(sInputFile.c_str(), (sInputFile + ".bak").c_str());
		}

		SetFileAttributes(sApiName.c_str(), FILE_ATTRIBUTE_NORMAL);

		int iLevel;
		//Iterations shared by the plugins that accept them, calculated once from the optimization level: 1, 1, 2, 3, 6, 9, 14, 21, 30
		String sIterations = (String) (((gudtOptions.iLevel * gudtOptions.iLevel * gudtOptions.iLevel) / 25) + 1);
		//Leanify and pngwolf allow overwriting them
		String sLeanifyIterations = (gudtOptions.iLeanifyIterations != -1) ? (String) gudtOptions.iLeanifyIterations : sIterations;
		String sPNGWolfIterations = (gudtOptions.iPNGWolfIterations != -1) ? (String) gudtOptions.iPNGWolfIterations : sIterations;
		//UPX flags from the optimization level
		String sUPXFlags;
		if (gudtOptions.iLevel < 3)
		{
			sUPXFlags = "-1 ";
		}
		else if (gudtOptions.iLevel < 5)
		{
			sUPXFlags = "-9 ";
		}
		else if (gudtOptions.iLevel < 7)
		{
			sUPXFlags = "-9 --best ";
		}
		else if (gudtOptions.iLevel < 9)
		{
			sUPXFlags = "-9 --best --lzma ";
		}
		else
		{
			sUPXFlags = "-9 --best --lzma --ultra-brute --crp-ms=999999 ";
		}
		//Each extension can correspond to more than one engine, so use if instead of else if
		// AVIF: ImageMagick. Not guarded: the static extensions cache is built in advance by actOptimizeExecute, so concurrent workers only read it, and the global lock was held (blocking the other workers and the painting of the grid) while reading the file header of the files with unknown extension
		String sExtensionByContent = " " + GetExtensionByContent(sInputFile) + " ";
		if (PosEx(sExtensionByContent, KS_EXTENSION_AVIF) > 0)
		{
			sFlags = "";
			RunPlugin((unsigned int) iCount, "ImageMagick (1/1)", (sPluginsDirectory + "magick.exe convert \"%INPUTFILE%\" -quiet -define heic:speed=1 " + sFlags + "\"%TMPOUTPUTFILE%\"").c_str(), sInputFile, "", 0, 0);
		}
		// BMP: ImageMagick, ImageWorsener
		if (PosEx(sExtensionByContent, KS_EXTENSION_BMP) > 0)
		{
			sFlags = "";
			if (!gudtOptions.bBMPCopyMetadata)
			{
				sFlags += "-strip ";
			}
			RunPlugin((unsigned int) iCount, "ImageMagick (1/2)", (sPluginsDirectory + "magick.exe convert \"%INPUTFILE%\" -quiet -compress RLE " + sFlags + "\"%TMPOUTPUTFILE%\"").c_str(), sInputFile, "", 0, 0);

			RunPlugin((unsigned int) iCount, "ImageWorsener (2/2)", (sPluginsDirectory + "imagew.exe -opt bmp:version=auto -noresize -zipcmprlevel 9 -outfmt bmp -compress \"rle\" \"%INPUTFILE%\" \"%TMPOUTPUTFILE%\"").c_str(), sInputFile, "", 0, 0);
		}
		// CSS: CSSTidy, Minify
		if (PosEx(sExtensionByContent, KS_EXTENSION_CSS) > 0)
		{
			if (gudtOptions.bCSSEnableTidy)
			{
				RunPlugin((unsigned int) iCount, "CSSTidy (1/2)", (sPluginsDirectory + "csstidy.exe \"%INPUTFILE%\" --template=" + gudtOptions.acCSSTemplate + " \"%TMPOUTPUTFILE%\"").c_str(), sInputFile, "", 0, 0);
				
				RunPlugin((unsigned int) iCount, "Minify (2/2)", (sPluginsDirectory + "minify.exe \"%INPUTFILE%\" --output \"%TMPOUTPUTFILE%\"").c_str(), sInputFile, "", 0, 0);
			}
		}
		// DLL: PETrim, strip, UPX
		//On .NET assemblies the CLR header and metadata would be damaged by the PE specific optimizers, so they are skipped
		if ((PosEx(sExtensionByContent, KS_EXTENSION_DLL) > 0) && (!IsEXEManagedNet(sInputFile.c_str())))
		{
			if (!gudtOptions.bEXEDisablePETrim)
			{
				RunPlugin((unsigned int) iCount, "PETrim (1/4)", (sPluginsDirectory + "petrim.exe \"%TMPINPUTFILE%\"").c_str(), sInputFile, "", 0, 0);
			}
			RunPlugin((unsigned int) iCount, "strip (2/4)", (sPluginsDirectory + "strip.exe --strip-all -o \"%TMPOUTPUTFILE%\" \"%INPUTFILE%\"").c_str(), sInputFile, "", 0, 0);
			RunPlugin((unsigned int) iCount, "llvm-strip (3/4)", (sPluginsDirectory + "llvm-strip.exe --strip-all -o \"%TMPOUTPUTFILE%\" \"%INPUTFILE%\"").c_str(), sInputFile, "", 0, 0);
			if (gudtOptions.bEXEEnableUPX)
			{
				RunPlugin((unsigned int) iCount, "UPX (4/4)", (sPluginsDirectory + "upx.exe --no-backup --force " + sUPXFlags + "\"%TMPINPUTFILE%\"").c_str(), sInputFile, "", 0, 0);
			}
		}
		// EXE: Leanify, PETrim, strip, UPX
		if (PosEx(sExtensionByContent, KS_EXTENSION_EXE) > 0)
		{
			//On .NET assemblies the CLR header and metadata would be damaged by the PE specific optimizers, so they are skipped
			if ((!IsEXESFX(sInputFile.c_str())) && (!IsEXEManagedNet(sInputFile.c_str())))
			{
				sFlags = "";
				//iLevel = min(gudtOptions.iLevel * 8 / 9, 8) + 1;
				sFlags += "-i " + sLeanifyIterations + " ";
				RunPlugin((unsigned int) iCount, "Leanify (1/5)", (sPluginsDirectory + "leanify.exe -q -p " + sFlags + "\"%TMPINPUTFILE%\"").c_str(), sInputFile, "", 0, 0);

				if (!gudtOptions.bEXEDisablePETrim)
				{
					RunPlugin((unsigned int) iCount, "PETrim (2/5)", (sPluginsDirectory + "petrim.exe /StripFixups:Y \"%TMPINPUTFILE%\"").c_str(), sInputFile, "", 0, 0);
				}
				RunPlugin((unsigned int) iCount, "strip (3/5)", (sPluginsDirectory + "strip.exe --strip-all -o \"%TMPOUTPUTFILE%\" \"%INPUTFILE%\"").c_str(), sInputFile, "", 0, 0);
				RunPlugin((unsigned int) iCount, "llvm-strip (4/5)", (sPluginsDirectory + "llvm-strip.exe --strip-all -o \"%TMPOUTPUTFILE%\" \"%INPUTFILE%\"").c_str(), sInputFile, "", 0, 0);
				if (gudtOptions.bEXEEnableUPX)
				{
					RunPlugin((unsigned int) iCount, "UPX (5/5)", (sPluginsDirectory + "upx.exe --no-backup --force " + sUPXFlags + "\"%TMPINPUTFILE%\"").c_str(), sInputFile, "", 0, 0);
				}
			}
		}
		// FLAC: FLAC, FLACOut
		if (PosEx(sExtensionByContent, KS_EXTENSION_FLAC) > 0)
		{
			if (!gudtOptions.bWAVCopyMetadata)
			{
				String sTmpOutputFile = ReplaceStr(sInputFile, ".flac", "-stripped.flac");
				//Prevent a bug in shntool with no lowercase extensions
				if (sTmpOutputFile == sInputFile)
				{
					sTmpOutputFile += "-stripped.flac";
				}

				RunPlugin((unsigned int) iCount, "shntool (1/4)", (sPluginsDirectory + "shntool.exe strip -q -O always \"%INPUTFILE%\"").c_str(), sInputFile, "", 0, 0);
				//Read the cell under the critical section, because the main thread also reads it while painting
				unsigned long long lOptimizedBytes, lTmpOutputSize;
				EnterCriticalSection(&mudtCriticalSection);
				lOptimizedBytes = ParseNumberThousand(grdFiles->Cells[KI_GRID_OPTIMIZED][iCount]);
				LeaveCriticalSection(&mudtCriticalSection);
				//SizeFile is expensive, so call it only once
				lTmpOutputSize = clsUtil::SizeFile(sTmpOutputFile.c_str());
				if ((lTmpOutputSize > 0) && (lTmpOutputSize < lOptimizedBytes) && (!gbStop) && (!mbPluginTimedOut))
				{
					clsUtil::CopyFile(sTmpOutputFile.c_str(), sInputFile.c_str(), true);
					EnterCriticalSection(&mudtCriticalSection);
					grdFiles->Cells[KI_GRID_OPTIMIZED][(int) iCount] = FormatNumberThousand(clsUtil::SizeFile(sInputFile.c_str()));
					LeaveCriticalSection(&mudtCriticalSection);
				}
				if (!gudtOptions.bDebug)
				{
					clsUtil::DeleteFile(sTmpOutputFile.c_str());
				}
				if (gudtOptions.bWAVStripSilence)
				{
					sTmpOutputFile = ReplaceStr(sInputFile, ".flac", "-trimmed.flac");
					//Prevent a bug in shntool with no lowercase extensions
					if (sTmpOutputFile == sInputFile)
					{
						sTmpOutputFile += "-trimmed.flac";
					}

					RunPlugin((unsigned int) iCount, "shntool (2/4)", (sPluginsDirectory + "shntool.exe trim -q -O always \"%INPUTFILE%\"").c_str(), sInputFile, "", 0, 0);
					//Read the cell under the critical section, because the main thread also reads it while painting
					EnterCriticalSection(&mudtCriticalSection);
					lOptimizedBytes = ParseNumberThousand(grdFiles->Cells[KI_GRID_OPTIMIZED][iCount]);
					LeaveCriticalSection(&mudtCriticalSection);
					//SizeFile is expensive, so call it only once
					lTmpOutputSize = clsUtil::SizeFile(sTmpOutputFile.c_str());
					if ((lTmpOutputSize > 0) && (lTmpOutputSize < lOptimizedBytes) && (!gbStop) && (!mbPluginTimedOut))
					{
						clsUtil::CopyFile(sTmpOutputFile.c_str(), sInputFile.c_str(), true);
						EnterCriticalSection(&mudtCriticalSection);
						grdFiles->Cells[KI_GRID_OPTIMIZED][(int) iCount] = FormatNumberThousand(clsUtil::SizeFile(sInputFile.c_str()));
						LeaveCriticalSection(&mudtCriticalSection);
					}
					if (!gudtOptions.bDebug)
					{			
						clsUtil::DeleteFile(sTmpOutputFile.c_str());
					}
				}
			}

			sFlags = "";
			if (gudtOptions.bMiscCopyMetadata)
			{
				sFlags += "--keep-foreign-metadata ";
			}
			if (gudtOptions.iLevel < 3)
			{
				sFlags += "-1 ";
			}
			else if (gudtOptions.iLevel < 5)
			{
				sFlags += "-8 --best ";
			}
			else if (gudtOptions.iLevel < 7)
			{
				sFlags += "-8 --best -e ";
			}
			else
			{
				sFlags += "-8 --best -ep ";
			}	
			RunPlugin((unsigned int) iCount, "FLAC (3/4)", (sPluginsDirectory + "flac.exe --force -s " + sFlags + "\"%TMPINPUTFILE%\"").c_str(), sInputFile, "", 0, 0);

			if (gudtOptions.iLevel >= 9)
			{
				RunPlugin((unsigned int) iCount, "FLACOut (4/4)", (sPluginsDirectory + "flacout.exe /q /y \"%INPUTFILE%\" \"%TMPOUTPUTFILE%\"").c_str(), sInputFile, "", 0, 0);
			}
		}
		// GIF: ImageMagick, gifsicle
		if (PosEx(sExtensionByContent, KS_EXTENSION_GIF) > 0)
		{
			sFlags = "";
			if (!gudtOptions.bGIFCopyMetadata)
			{
				sFlags += "-strip ";
			}
			//RunPlugin((unsigned int) iCount, "ImageMagick", (sPluginsDirectory + "magick.exe convert \"%INPUTFILE%\" -quiet -layers optimize -compress LZW " + sFlags + "\"%TMPOUTPUTFILE%\"").c_str(), sInputFile, "", 0, 0);
			RunPlugin((unsigned int) iCount, "ImageMagick (1/2)", (sPluginsDirectory + "magick.exe convert \"%INPUTFILE%\" -quiet -layers optimize -compress LZW " + sFlags + "\"%TMPOUTPUTFILE%\"").c_str(), sInputFile, "", 0, 0);
			
			sFlags = "";
			//iLevel = min(gudtOptions.iLevel * 3 / 9, 3);
			iLevel = 3;
			sFlags += "-O" + (String) iLevel + " ";
			if (!gudtOptions.bGIFCopyMetadata)
			{
				sFlags += "--no-comments --no-extensions --no-names ";
			}
			if (gudtOptions.bGIFAllowLossy)
			{
				sFlags += "--lossy=85 ";
			}
			RunPlugin((unsigned int) iCount, "gifsicle (2/2)", (sPluginsDirectory + "gifsicle.exe -w -j --no-conserve-memory -o \"%TMPOUTPUTFILE%\" " + sFlags + "\"%INPUTFILE%\"").c_str(), sInputFile, "", 0, 0);
		}
		// GZ: Libdeflate, Leanify, ect, advdef, zRecompress, deflopt, columbo, defluff, deflopt, columbo
		if (PosEx(sExtensionByContent, KS_EXTENSION_GZ) > 0)
		{
			if (!gudtOptions.bGZCopyMetadata)
			{
				RunPlugin((unsigned int) iCount, "libdeflate (1/10)", (sPluginsDirectory + "libdeflate.bat \"%INPUTFILE%\" \"%TMPOUTPUTFILE%\"").c_str(), sInputFile, "", 0, 0);
				
				sFlags = "";
				//iLevel = min(gudtOptions.iLevel * 8 / 9, 8) + 1;
				sFlags += "-i " + sLeanifyIterations + " ";
				RunPlugin((unsigned int) iCount, "Leanify (2/10)", (sPluginsDirectory + "leanify.exe -q -p " + sFlags + "\"%TMPINPUTFILE%\"").c_str(), sInputFile, "", 0, 0);
			}
			
			sFlags = "";
			//iLevel = min(gudtOptions.iLevel * 7 / 9, 7) + 1;
			sFlags += "-i " + sIterations + " ";
			RunPlugin((unsigned int) iCount, "advdef (3/10)", (sPluginsDirectory + "advdef.exe -z -q -4 " + sFlags + "\"%TMPINPUTFILE%\"").c_str(), sInputFile, "", 0, 0);

			RunPlugin((unsigned int) iCount, "zRecompress (4/10)", (sPluginsDirectory + "zRecompress.exe -tgz \"%TMPINPUTFILE%\"").c_str(), sInputFile, "", 0, 0);

			sFlags = "";
			if (!gudtOptions.bGZCopyMetadata)
			{
				sFlags += "-strip ";
			}
			iLevel = min(gudtOptions.iLevel * 8 / 9, 8) + 1;
			//Convert level 9 to level 90032 because it is faster and usually with higher savings
			if (iLevel == 9)
			{
				iLevel = 90032;
			}
			sFlags += "-" + (String) iLevel + " ";
			RunPlugin((unsigned int) iCount, "ECT (5/10)", (sPluginsDirectory + "ECT.exe -quiet --mt-deflate --mt-file --allfilters -gzip " + sFlags + "\"%TMPINPUTFILE%\"").c_str(), sInputFile, "", 0, 0);

			sFlags = "";
			if (gudtOptions.bGZCopyMetadata)
			{
				sFlags += "/c ";
			}
			RunPlugin((unsigned int) iCount, "DeflOpt (6/10)", (sPluginsDirectory + "deflopt.exe /a /b /s " + sFlags + "\"%TMPINPUTFILE%\"").c_str(), sInputFile, "", 0, 0);

			sFlags = "";
			if (!gudtOptions.bGZCopyMetadata)
			{
				sFlags += "--strip ";
			}
			if (gudtOptions.iLevel >= 9)
			{
				sFlags += "--max ";
			}
			RunPlugin((unsigned int) iCount, "Columbo (7/10)", (sPluginsDirectory + "columbo.exe " + sFlags + "\"%TMPINPUTFILE%\"").c_str(), sInputFile, "", 0, 0);

			RunPlugin((unsigned int) iCount, "defluff (8/10)", (sPluginsDirectory + "defluff.bat \"%INPUTFILE%\" \"%TMPOUTPUTFILE%\"").c_str(), sInputFile, "", 0, 0);

			sFlags = "";
			if (gudtOptions.bGZCopyMetadata)
			{
				sFlags += "/c ";
			}
			RunPlugin((unsigned int) iCount, "DeflOpt (9/10)", (sPluginsDirectory + "deflopt.exe /a /b /s " + sFlags + "\"%TMPINPUTFILE%\"").c_str(), sInputFile, "", 0, 0);

			sFlags = "";
			if (!gudtOptions.bGZCopyMetadata)
			{
				sFlags += "--strip ";
			}
			if (gudtOptions.iLevel >= 9)
			{
				sFlags += "--max ";
			}
			RunPlugin((unsigned int) iCount, "Columbo (10/10)", (sPluginsDirectory + "columbo.exe " + sFlags + "\"%TMPINPUTFILE%\"").c_str(), sInputFile, "", 0, 0);
		}
		// HTML / SVG: svgcleaner, svgop, tidy-html5, Minify, Leanify
		if (PosEx(sExtensionByContent, KS_EXTENSION_HTML) > 0)
		{
			const bool bIsSVG = (PosEx(_T(" .svg "), sExtensionByContent) > 0);

			if (bIsSVG)
			{
				if (gudtOptions.bSVGEnableSvgcleaner)
				{
					RunPlugin((unsigned int) iCount, "svgcleaner (1/5)", (sPluginsDirectory + "svgcleaner.exe --quiet \"%INPUTFILE%\" \"%TMPOUTPUTFILE%\"").c_str(), sInputFile, "", 0, 0);
				}
				if (gudtOptions.bSVGEnableSvgop)
				{
					RunPlugin((unsigned int) iCount, "svgop (2/5)", (sPluginsDirectory + "svgop.exe \"%INPUTFILE%\" \"%TMPOUTPUTFILE%\"").c_str(), sInputFile, "", 0, 0);
				}
			}

			if (gudtOptions.bHTMLEnableTidy)
			{
				if (bIsSVG)
				{
					RunPlugin((unsigned int) iCount, "tidy (3/5)", (sPluginsDirectory + "tidy.exe -config tidy.config -quiet -output \"%TMPOUTPUTFILE%\" \"%INPUTFILE%\" ").c_str(), sInputFile, "", 0, 0);
					RunPlugin((unsigned int) iCount, "Minify (4/5)", (sPluginsDirectory + "minify.exe \"%INPUTFILE%\" --output \"%TMPOUTPUTFILE%\"").c_str(), sInputFile, "", 0, 0);
					sFlags = "-i " + sLeanifyIterations + " ";
					RunPlugin((unsigned int) iCount, "Leanify (5/5)", (sPluginsDirectory + "leanify.exe -q -p " + sFlags + "\"%TMPINPUTFILE%\"").c_str(), sInputFile, "", 0, 0);
				}
				else
				{
					RunPlugin((unsigned int) iCount, "tidy (1/3)", (sPluginsDirectory + "tidy.exe -config tidy.config -quiet -output \"%TMPOUTPUTFILE%\" \"%INPUTFILE%\" ").c_str(), sInputFile, "", 0, 0);
					RunPlugin((unsigned int) iCount, "Minify (2/3)", (sPluginsDirectory + "minify.exe \"%INPUTFILE%\" --output \"%TMPOUTPUTFILE%\"").c_str(), sInputFile, "", 0, 0);
					sFlags = "-i " + sLeanifyIterations + " ";
					RunPlugin((unsigned int) iCount, "Leanify (3/3)", (sPluginsDirectory + "leanify.exe -q -p " + sFlags + "\"%TMPINPUTFILE%\"").c_str(), sInputFile, "", 0, 0);
				}
			}
			else if (bIsSVG && (gudtOptions.bSVGEnableSvgcleaner || gudtOptions.bSVGEnableSvgop))
			{
				// Without Tidy, still run Leanify on SVG when any SVG-specific tool is enabled
				sFlags = "-i " + sLeanifyIterations + " ";
				RunPlugin((unsigned int) iCount, "Leanify (5/5)", (sPluginsDirectory + "leanify.exe -q -p " + sFlags + "\"%TMPINPUTFILE%\"").c_str(), sInputFile, "", 0, 0);
			}
		}
		// ICO: ImageMagick, Leanify
		if (PosEx(sExtensionByContent, KS_EXTENSION_ICO) > 0)
		{
			/*
			sFlags = "";
			if (!gudtOptions.bPNGCopyMetadata)
			{
				sFlags += "-strip ";
			}
			RunPlugin((unsigned int) iCount, "ImageMagick (1/2)", (sPluginsDirectory + "magick.exe convert \"%INPUTFILE%\" -quiet -compress ZIP " + sFlags + "\"%TMPOUTPUTFILE%\"").c_str(), sInputFile, "", 0, 0);
			*/

			//Temporary disable Leanify because it removed IPTC metadata
			if (!gudtOptions.bPNGCopyMetadata)
			{
				sFlags = "";
				//iLevel = min(gudtOptions.iLevel * 8 / 9, 8) + 1;
				sFlags += "-i " + sLeanifyIterations + " ";
				RunPlugin((unsigned int) iCount, "Leanify (2/2)", (sPluginsDirectory + "leanify.exe -q -p " + sFlags + "\"%TMPINPUTFILE%\"").c_str(), sInputFile, "", 0, 0);
			}
		}
		// JPEG 2000: ImageMagick
		if (PosEx(sExtensionByContent, KS_EXTENSION_JP2) > 0)
		{
			sFlags = "";
			RunPlugin((unsigned int) iCount, "ImageMagick (1/1)", (sPluginsDirectory + "magick.exe convert \"%INPUTFILE%\" -quiet -quality 0 " + sFlags + "\"%TMPOUTPUTFILE%\"").c_str(), sInputFile, "", 0, 0);
		}
		// JPEG: pingo, cjpegli, guetzli, jpeg-recompress, jhead, Leanify, ect, jpegoptim, jpegtran, mozjpegtran
		if (PosEx(sExtensionByContent, KS_EXTENSION_JPG) > 0)
		{
			bool bIsJPEGCMYK = IsJPEGCMYK(sInputFile.c_str());
			
			sFlags = "";
			iLevel = min(gudtOptions.iLevel * 4 / 9, 4);
			sFlags += "-s" + (String) iLevel + " ";
			if (gudtOptions.bJPEGAllowLossy)
			{
				sFlags += "-quality=95 ";
			}
			else
			{
				sFlags += "-lossless ";
			}
			if (gudtOptions.bJPEGCopyMetadata)
			{
				sFlags += "-nostrip ";
			}
			RunPlugin((unsigned int) iCount, "pingo (1/11)", (sPluginsDirectory + "pingo.exe " + sFlags + "\"%TMPINPUTFILE%\"").c_str(), sInputFile, "", 0, 0);

			if ((gudtOptions.bJPEGAllowLossy) && (!gudtOptions.bJPEGCopyMetadata))
			{
				RunPlugin((unsigned int) iCount, "cjpegli (2/11)", (sPluginsDirectory + "cjpegli.exe -q 70 \"%INPUTFILE%\" \"%TMPOUTPUTFILE%\"").c_str(), sInputFile, "", 0, 0);

				RunPlugin((unsigned int) iCount, "Guetzli (3/11)", (sPluginsDirectory + "guetzli.exe --auto --quality 90 \"%INPUTFILE%\" \"%TMPOUTPUTFILE%\"").c_str(), sInputFile, "", 0, 0);
			}
			
			if (gudtOptions.bJPEGAllowLossy)
			{
				sFlags = "";
				if (!gudtOptions.bJPEGCopyMetadata)
				{
					sFlags += "--strip ";
				}
				if (gudtOptions.iLevel >= 5)
				{
					sFlags += "--accurate ";
				}
				RunPlugin((unsigned int) iCount, "jpeg-recompress (4/11)", (sPluginsDirectory + "jpeg-recompress.exe --method smallfry --quality high --min 60 --subsample disable --quiet " + sFlags + "\"%INPUTFILE%\" \"%TMPOUTPUTFILE%\"").c_str(), sInputFile, "", 0, 0);
			}
			//jhead is corrupting CMYK JPEG images
			if (!bIsJPEGCMYK)
			{
				sFlags = "";
				if (gudtOptions.bJPEGCopyMetadata)
				{
					sFlags += "-zt ";
				}
				else
				{
					sFlags += "-purejpg -di -dx -dt -zt ";
				}
				RunPlugin((unsigned int) iCount, "jhead (5/11)", (sPluginsDirectory + "jhead.exe -q -autorot " + sFlags + " \"%TMPINPUTFILE%\"").c_str(), sInputFile, "", 0, 0);
			}
			sFlags = "";
			if (gudtOptions.bJPEGCopyMetadata)
			{
				sFlags += "--keep-exif --keep-icc --jpeg-keep-all ";
			}
			if (gudtOptions.bJPEGUseArithmeticEncoding)
			{
				sFlags += "--jpeg-arithmetic ";
			}		
			//iLevel = min(gudtOptions.iLevel * 8 / 9, 8) + 1;
			sFlags += "-i " + sLeanifyIterations + " ";
			RunPlugin((unsigned int) iCount, "Leanify (6/11)", (sPluginsDirectory + "leanify.exe -q -p " + sFlags + "\"%TMPINPUTFILE%\"").c_str(), sInputFile, "", 0, 0);

			if (gudtOptions.bJPEGAllowLossy)
			{
				sFlags = "";
				if (!gudtOptions.bJPEGCopyMetadata)
				{
					sFlags += "-strip ";
				}
				//Seems to cause some loss of quality
				RunPlugin((unsigned int) iCount, "ImageMagick (7/11)", (sPluginsDirectory + "magick.exe convert \"%INPUTFILE%\" -quiet -interlace Plane -define jpeg:optimize-coding=true " + sFlags + "\"%TMPOUTPUTFILE%\"").c_str(), sInputFile, "", 0, 0);
			}

			sFlags = "";
			if (!gudtOptions.bJPEGCopyMetadata)
			{
				sFlags += "--strip-all ";
			}
			if (gudtOptions.bJPEGUseArithmeticEncoding)
			{
				sFlags += "--with-arith ";
			}
			sFlags += "-w " + (String) gudtSystemInfo.dwNumberOfProcessors + " ";
			RunPlugin((unsigned int) iCount, "jpegoptim (8/11)", (sPluginsDirectory + "jpegoptim.exe -o -q --all-progressive --nofix " + sFlags + "\"%TMPINPUTFILE%\"").c_str(), sInputFile, "", 0, 0);

			sFlags = "";
			if (gudtOptions.bJPEGUseArithmeticEncoding)
			{
				sFlags += "-arithmetic ";
			}
			if (gudtOptions.bJPEGCopyMetadata)
			{
				sFlags += "-copy all ";
			}
			else
			{
				sFlags += "-copy none ";
			}
			RunPlugin((unsigned int) iCount, "jpegtran (9/11)", (sPluginsDirectory + "jpegtran.exe -progressive -optimize " + sFlags + "\"%INPUTFILE%\" \"%TMPOUTPUTFILE%\"").c_str(), sInputFile, "", 0, 0);

			RunPlugin((unsigned int) iCount, "mozjpegtran (10/11)", (sPluginsDirectory + "mozjpegtran.exe -outfile \"%TMPOUTPUTFILE%\" -progressive -optimize -perfect " + sFlags + "\"%INPUTFILE%\"").c_str(), sInputFile, "", 0, 0);
			
			sFlags = "";
			if (!gudtOptions.bJPEGCopyMetadata)
			{
				sFlags += "-strip ";
			}
			if (!gudtOptions.bJPEGAllowLossy)
			{
				sFlags += "--strict ";
			}			
			iLevel = min(gudtOptions.iLevel * 8 / 9, 8) + 1;
			//Convert level 9 to level 90032 because it is faster and usually with higher savings
			if (iLevel == 9)
			{
				iLevel = 90032;
			}
			sFlags += "-" + (String) iLevel + " ";
			RunPlugin((unsigned int) iCount, "ECT (11/11)", (sPluginsDirectory + "ECT.exe -quiet --mt-deflate --mt-file --allfilters -progressive " + sFlags + "\"%TMPINPUTFILE%\"").c_str(), sInputFile, "", 0, 0);
		}
		
		// JPEG XL: ImageMagick, jxltran
		if (PosEx(sExtensionByContent, KS_EXTENSION_JXL) > 0)
		{
			sFlags = "";
			iLevel = min(gudtOptions.iLevel * 9 / 9, 9);
			sFlags += "jxl:effort=" + (String) iLevel + " ";
			RunPlugin((unsigned int) iCount, "ImageMagick (1/2)", (sPluginsDirectory + "magick.exe convert \"%INPUTFILE%\" -quiet " + sFlags + "\"%TMPOUTPUTFILE%\"").c_str(), sInputFile, "", 0, 0);

			//Extracting the codestream removes the container, and with it the Exif/XMP and JPEG reconstruction boxes, so it is only done when metadata is not kept
			if (!gudtOptions.bJPEGCopyMetadata)
			{
				RunPlugin((unsigned int) iCount, "jxltran (2/2)", (sPluginsDirectory + "jxltran.exe \"%INPUTFILE%\" \"%TMPOUTPUTFILE%\" --extract").c_str(), sInputFile, "", 0, 0);
			}
		}
		

		// JS: jsmin, minify
		bool bJSAdditional = ((gudtOptions.acJSAdditionalExtensions[0] != NULL) && (PosEx(sExtensionByContent, " " + ReplaceStr((String) gudtOptions.acJSAdditionalExtensions, ",", " ") + " ") > 0));
		if ((PosEx(sExtensionByContent, KS_EXTENSION_JS) > 0) || (bJSAdditional))
		{
			//If JSMin is enabled or it is a custom extension (we assume custom extensions always enable it)
			if ((gudtOptions.bJSEnableJSMin) || (bJSAdditional))
			{
				RunPlugin((unsigned int) iCount, "jsmin (1/2)", (sPluginsDirectory + "jsmin.bat \"%INPUTFILE%\" \"%TMPOUTPUTFILE%\"").c_str(), sInputFile, "", 0, 0);
				
				RunPlugin((unsigned int) iCount, "Minify (2/2)", (sPluginsDirectory + "minify.exe \"%INPUTFILE%\" --output \"%TMPOUTPUTFILE%\"").c_str(), sInputFile, "", 0, 0);
			}
		}
		// LUA: Leanify
		if (PosEx(sExtensionByContent, KS_EXTENSION_LUA) > 0)
		{
			if (gudtOptions.bLUAEnableLeanify)
			{
				sFlags = "";
				//iLevel = min(gudtOptions.iLevel * 8 / 9, 8) + 1;
				sFlags += "-i " + sLeanifyIterations + " ";
				RunPlugin((unsigned int) iCount, "Leanify (1/1)", (sPluginsDirectory + "leanify.exe -q -p " + sFlags + "\"%TMPINPUTFILE%\"").c_str(), sInputFile, "", 0, 0);
			}
		}
		// MIME: Leanify
		if (PosEx(sExtensionByContent, KS_EXTENSION_MIME) > 0)
		{
			sFlags = "";
			//iLevel = min(gudtOptions.iLevel * 8 / 9, 8) + 1;
			sFlags += "-i " + sLeanifyIterations + " ";
			RunPlugin((unsigned int) iCount, "Leanify (1/1)", (sPluginsDirectory + "leanify.exe -q -p " + sFlags + "\"%TMPINPUTFILE%\"").c_str(), sInputFile, "", 0, 0);
		}
		// MKV: ffmpeg, mkclean
		if (PosEx(sExtensionByContent, KS_EXTENSION_MKV) > 0)
		{
			sFlags = "";
			if (!gudtOptions.bMP4CopyMetadata)
			{
				sFlags += "-map_metadata -1 ";
			}			
			RunPlugin((unsigned int) iCount, "ffmpeg (1/2)", (sPluginsDirectory + "ffmpeg.exe -i \"%INPUTFILE%\" -vcodec copy -acodec copy -map 0 " + sFlags + "\"%TMPOUTPUTFILE%\"").c_str(), sInputFile, "", 0, 0);
			
			RunPlugin((unsigned int) iCount, "mkclean (2/2)", (sPluginsDirectory + "mkclean.exe --optimize --unsafe --quiet \"%INPUTFILE%\" \"%TMPOUTPUTFILE%\"").c_str(), sInputFile, "", 0, 0);
		}
		// MNG: advmng
		if (PosEx(sExtensionByContent, KS_EXTENSION_MNG) > 0)
		{
			sFlags = "";
			//iLevel = min(gudtOptions.iLevel * 7 / 9, 7) + 1;
			sFlags += "-i " + sIterations + " ";
			RunPlugin((unsigned int) iCount, "advmng (1/1)", (sPluginsDirectory + "advmng.exe -z -r -q -4 " + sFlags + "\"%TMPINPUTFILE%\"").c_str(), sInputFile, "", 0, 0);
		}
		// MP3: MP3packer
		if (PosEx(sExtensionByContent, KS_EXTENSION_MP3) > 0)
		{
			sFlags = "";
			if (!gudtOptions.bMP3CopyMetadata)
			{
				sFlags += "-t -s ";
			}
			RunPlugin((unsigned int) iCount, "MP3packer (1/1)", (sPluginsDirectory + "mp3packer.exe " + sFlags + "-z -a \"\" -A -f \"%INPUTFILE%\" \"%TMPOUTPUTFILE%\"").c_str(), sInputFile, "", 0, 0);
		}
		// MP4: ffmpeg, mp4v2
		if (PosEx(sExtensionByContent, KS_EXTENSION_MP4) > 0)
		{
			sFlags = "";
			if (!gudtOptions.bMP4CopyMetadata)
			{
				sFlags += "-map_metadata -1 ";
			}			
			RunPlugin((unsigned int) iCount, "ffmpeg (1/2)", (sPluginsDirectory + "ffmpeg.exe -i \"%INPUTFILE%\" -vcodec copy -acodec copy -map 0 " + sFlags + "\"%TMPOUTPUTFILE%\"").c_str(), sInputFile, "", 0, 0);
			
			RunPlugin((unsigned int) iCount, "mp4v2 (2/2)", (sPluginsDirectory + "mp4file.exe --optimize -q \"%TMPINPUTFILE%\"").c_str(), sInputFile, "", 0, 0);
		}
		// OBJ: strip, llvm-strip
		if (PosEx(sExtensionByContent, KS_EXTENSION_OBJ) > 0)
		{
			RunPlugin((unsigned int) iCount, "strip (1/2)", (sPluginsDirectory + "strip.exe --strip-all -o \"%TMPOUTPUTFILE%\" \"%INPUTFILE%\"").c_str(), sInputFile, "", 0, 0);
			RunPlugin((unsigned int) iCount, "llvm-strip (2/2)", (sPluginsDirectory + "llvm-strip.exe --strip-all -o \"%TMPOUTPUTFILE%\" \"%INPUTFILE%\"").c_str(), sInputFile, "", 0, 0);
		}
		// OGG: optivorbis, rehuff_theora
		if (PosEx(sExtensionByContent, KS_EXTENSION_OGG) > 0)
		{
			RunPlugin((unsigned int) iCount, "OptiVorbis (1/2)", (sPluginsDirectory + "optivorbis.exe --vendor_string_action empty --comment_fields_action delete \"%INPUTFILE%\" \"%TMPOUTPUTFILE%\"").c_str(), sInputFile, "", 0, 0);
			
			RunPlugin((unsigned int) iCount, "rehuff_theora (2/2)", (sPluginsDirectory + "rehuff_theora.exe \"%INPUTFILE%\" \"%TMPOUTPUTFILE%\"").c_str(), sInputFile, "", 0, 0);
		}
		// OGV: ffmpeg, rehuff_theora
		if (PosEx(sExtensionByContent, KS_EXTENSION_OGV) > 0)
		{
			sFlags = "";
			if (!gudtOptions.bMP4CopyMetadata)
			{
				sFlags += "-map_metadata -1 ";
			}			
			RunPlugin((unsigned int) iCount, "ffmpeg (1/2)", (sPluginsDirectory + "ffmpeg.exe -i \"%INPUTFILE%\" -vcodec copy -acodec copy -map 0 " + sFlags + "\"%TMPOUTPUTFILE%\"").c_str(), sInputFile, "", 0, 0);
			
			RunPlugin((unsigned int) iCount, "rehuff_theora (2/2)", (sPluginsDirectory + "rehuff_theora.exe \"%INPUTFILE%\" \"%TMPOUTPUTFILE%\"").c_str(), sInputFile, "", 0, 0);
		}
		// Microsoft OLE Compound Files: Document Press, Best CFBF
		if (PosEx(sExtensionByContent, KS_EXTENSION_OLE) > 0)
		{
			RunPlugin((unsigned int) iCount, "Document Press (1/2)", (sPluginsDirectory + "docprc.exe -opt \"%TMPINPUTFILE%\"").c_str(), sInputFile, "", 0, 0);
			RunPlugin((unsigned int) iCount, "Best CFBF (2/2)", (sPluginsDirectory + "bestcfbf.exe \"%INPUTFILE%\" \"%TMPOUTPUTFILE%\" -v4").c_str(), sInputFile, "", 0, 0);
		}
		// PCX: ImageMagick
		if (PosEx(sExtensionByContent, KS_EXTENSION_PCX) > 0)
		{
			sFlags = "";
			if (!gudtOptions.bPCXCopyMetadata)
			{
				sFlags += "-strip ";
			}
			RunPlugin((unsigned int) iCount, "ImageMagick (1/1)", (sPluginsDirectory + "magick.exe convert \"%INPUTFILE%\" -quiet -compress RLE " + sFlags + "\"%TMPOUTPUTFILE%\"").c_str(), sInputFile, "", 0, 0);
		}
		// PDF: mutool, ghostcript, cpdf, qpdf
		if (PosEx(sExtensionByContent, KS_EXTENSION_PDF) > 0)
		{
			bool bIsPDFLayered = IsPDFLayered(sInputFile.c_str());
			
			//Skip Ghostcript on PDF with layers, or if no downsampling is selected, because GS always downsample images.
			if ((!bIsPDFLayered) || (!gudtOptions.bPDFSkipLayered) || (_tcscmp(gudtOptions.acPDFProfile, _T("none")) == 0))
			{		
				//RunPlugin((unsigned int) iCount, "mutool (1/4)", (sPluginsDirectory + "mutool.exe clean -gggg -z \"%INPUTFILE%\" \"%TMPOUTPUTFILE%\"").c_str(), sInputFile, "", 0, 0);
				RunPlugin((unsigned int) iCount, "mutool (1/4)", (sPluginsDirectory + "mutool.exe clean -g -z \"%INPUTFILE%\" \"%TMPOUTPUTFILE%\"").c_str(), sInputFile, "", 0, 0);
				
				//Do not use Ghoscript for Adobe Illustrator (AI) files
				if (!EndsText(".ai", sInputFile))
				{
					sFlags = "";
					//Custom mode
					if (_tcscmp(gudtOptions.acPDFProfile, _T("Custom")) == 0)
					{
						sFlags += "-dPDFSETTINGS=/ebook -dDownsampleColorImages=true -dColorImageResolution=" + (String) gudtOptions.iPDFCustomDPI + " -dDownsampleGrayImages=true -dGrayImageResolution=" + (String) gudtOptions.iPDFCustomDPI + " -dDownsampleMonoImages=true -dMonoImageResolution=" + (String) gudtOptions.iPDFCustomDPI + " ";
					}
					//No downsampling
					else if (_tcscmp(gudtOptions.acPDFProfile, _T("none")) == 0)
					{
						sFlags += "-dPDFSETTINGS=/default -c \".setpdfwrite <</ColorACSImageDict>[1 1 1 1] /VSamples [1 1 1 1] /Blend 1>> /GrayACSImageDict<</QFactor>[1 1 1 1] /VSamples [1 1 1 1] /Blend 1>>>> setdistillerparams\" ";
					}				
					//Built in downsample modes: screen, ebook, printer, prepress
					else
					{
						sFlags += "-dPDFSETTINGS=/" + (String) gudtOptions.acPDFProfile + " ";
					}
					
					sFlags += "-dColorImageDownsampleType=/Bicubic -dGrayImageDownsampleType=/Bicubic -dMonoImageDownsampleType=/Bicubic -dOptimize=true -dConvertCMYKImagesToRGB=true -dColorConversionStrategy=/sRGB -dPrinted=false -q -dBATCH -dNOPAUSE -dSAFER -dDELAYSAFER -dNOPROMPT -sDEVICE=pdfwrite -dDetectDuplicateImages=true -dAutoRotatePages=/None -dCompatibilityLevel=1.4 ";
					
					String sTmpFilePdf = clsUtil::GetShortName(sInputFile + ".pdf");

					#if defined(_WIN64)
						RunPlugin((unsigned int) iCount, "Ghostcript (2/4)", (sPluginsDirectory + "gswin64c.exe " + sFlags + "-sOutputFile=\"" + sTmpFilePdf + "\" \"%INPUTFILE%\"").c_str(), sInputFile, "", 0, 0);
					#else
						RunPlugin((unsigned int) iCount, "Ghostcript (2/4)", (sPluginsDirectory + "gswin32c.exe " + sFlags + "-sOutputFile=\"" + sTmpFilePdf + "\" \"%INPUTFILE%\"").c_str(), sInputFile, "", 0, 0);
					#endif
					//If there is size reduction check it is not so high to detect corrupted encrypted PDF
					unsigned long long lTmpFilePdfSize = clsUtil::SizeFile(sTmpFilePdf.c_str());
					unsigned long long lInputFileSize = clsUtil::SizeFile(sInputFile.c_str());
					if ((lTmpFilePdfSize < lInputFileSize) && (lTmpFilePdfSize > 3000) && (lInputFileSize > 20000) && (!gbStop) && (!mbPluginTimedOut))
					{
						clsUtil::CopyFile(sTmpFilePdf.c_str(), sInputFile.c_str(), true);
					}
					if (!gudtOptions.bDebug)
					{
						clsUtil::DeleteFile(sTmpFilePdf.c_str());
					}
				}
				RunPlugin((unsigned int) iCount, "cpdf (3/4)", (sPluginsDirectory + "cpdf.exe -squeeze \"%INPUTFILE%\" -o \"%TMPOUTPUTFILE%\"").c_str(), sInputFile, "", 0, 0);
				
				sFlags = "";
				if (gudtOptions.bJPEGAllowLossy)
				{
					sFlags += "--optimize-images ";
				}
				RunPlugin((unsigned int) iCount, "qpdf (4/4)", (sPluginsDirectory + "qpdf.exe \"%INPUTFILE%\" --compress-streams=y --decode-level=generalized --recompress-flate --compression-level=9 --object-streams=generate " + sFlags + "\"%TMPOUTPUTFILE%\"").c_str(), sInputFile, "", 0, 0);
			}
		}
		// PNG: apngopt, pngquant, PngOptimizer, TruePNG, pngout, oxipng, pngwolf, Leanify, ect, pingo, advpng, deflopt, columbo, defluff, deflopt, columbo
		if (PosEx(sExtensionByContent, KS_EXTENSION_PNG) > 0)
		{
			bool bIsAPNG = IsAPNG(sInputFile.c_str());
			bool bIsPNG9Patch = EndsText(".9.png", sInputFile);

			//Android 9-patch images get broken with advpng, deflopt, columbo, oxipng, pngoptimizer, pngout, pngrewrite and truepng. Only pngwolf, defluff and leanify seem to be safe. At the moment, detect them by extension .9.png.
			if (bIsAPNG)
			{
				RunPlugin((unsigned int) iCount, "apngopt (1/18)", (sPluginsDirectory + "apngopt.exe \"%INPUTFILE%\" \"%TMPOUTPUTFILE%\"").c_str(), sInputFile, "", 0, 0);
			}

			if (!bIsPNG9Patch)
			{
				if ((gudtOptions.bPNGAllowLossy) && (!bIsAPNG))
				{
					sFlags = "";
					if (!gudtOptions.bPNGCopyMetadata)
					{
						sFlags += "--strip ";
					}
					RunPlugin((unsigned int) iCount, "pngquant (2/18)", (sPluginsDirectory + "pngquant.exe " + sFlags + "--quality=85-95 --speed 1 --ext .png --force \"%TMPINPUTFILE%\"").c_str(), sInputFile, "", 0, 0);
				}

				sFlags = "";
				if (gudtOptions.bPNGCopyMetadata)
				{
					sFlags += "-KeepPhysicalPixelDimensions ";
				}
				RunPlugin((unsigned int) iCount, "PngOptimizer (3/18)", (sPluginsDirectory + "PngOptimizer.exe " + sFlags + "-file:\"%TMPINPUTFILE%\"").c_str(), sInputFile, "", 0, 0);
			}
			
			if ((!bIsAPNG) && (!bIsPNG9Patch))
			{
				//Disable TruePNG on ICO files because it crashes
				if (PosEx(sExtensionByContent, KS_EXTENSION_ICO) == 0)
				{
					sFlags = "";
					iLevel = min(gudtOptions.iLevel * 3 / 9, 3) + 1;
					sFlags += "-o" + (String) iLevel + " ";
					if (gudtOptions.bPNGCopyMetadata)
					{
						sFlags += "-md keep all ";
					}
					else
					{
						sFlags += "-tz -md remove all -a1 -g1 ";
					}
					if (gudtOptions.bPNGAllowLossy)
					{
						sFlags += "-l ";
					}
					RunPlugin((unsigned int) iCount, "TruePNG (4/18)", (sPluginsDirectory + "truepng.exe " + sFlags + "/i0 /nc /tz /quiet /y /out \"%TMPOUTPUTFILE%\" \"%INPUTFILE%\"").c_str(), sInputFile, "", 0, 0);
				}

				//Skip PNGOut when it is a JPEG renamed to PNG
				if (PosEx(" " + GetExtensionByContent(sInputFile, true) + " ", KS_EXTENSION_JPG) == 0)
				{
					sFlags = "";
					if (gudtOptions.bPNGCopyMetadata)
					{
						sFlags += "/k1 ";
					}
					else
					{
						sFlags += "/kacTL,fcTL,fdAT ";
					}
					iLevel = max((gudtOptions.iLevel * 3 / 9) - 3, 0);
					sFlags += "/s" + (String) iLevel + " ";
					RunPlugin((unsigned int) iCount, "PNGOut (5/18)", (sPluginsDirectory + "pngout.exe /q /y /r /d0 /mincodes0 " + sFlags + "\"%INPUTFILE%\" \"%TMPOUTPUTFILE%\"").c_str(), sInputFile, "", 0, 0);
				}
			}
			
			sFlags = "";
			iLevel = min(gudtOptions.iLevel * 6 / 9, 6);
			sFlags += "-o" + (String) iLevel + " ";
			if (gudtOptions.bPNGAllowLossy)
			{
				sFlags += "--scale16 ";
			}
			if (bIsAPNG)
			{
				// For some reason -strip all -protect acTL,fcTL,fdAT is not keeping APNG chunks
				sFlags += "--strip safe ";
				RunPlugin((unsigned int) iCount, "OxiPNG (6/18)", (sPluginsDirectory + "oxipng.exe --zopfli --alpha --quiet " + sFlags + "\"%TMPINPUTFILE%\"").c_str(), sInputFile, "", 0, 0);
			}
			else
			{
				if (!gudtOptions.bPNGCopyMetadata)
				{
					sFlags += "--strip all ";
				}
				else
				{
					sFlags += "--strip safe ";
				}
				RunPlugin((unsigned int) iCount, "OxiPNG (7/18)", (sPluginsDirectory + "oxipng.exe --zopfli --alpha --quiet " + sFlags + "\"%TMPINPUTFILE%\"").c_str(), sInputFile, "", 0, 0);
			}

			if (!bIsAPNG)
			{
				sFlags = "";
				//iLevel = min(gudtOptions.iLevel * 8 / 9, 8) + 1;
				sFlags += "-i " + sLeanifyIterations + " ";
				//Temporary disable Leanify because it removed IPTC metadata
				if (!gudtOptions.bPNGCopyMetadata)
				{
					RunPlugin((unsigned int) iCount, "Leanify (8/18)", (sPluginsDirectory + "leanify.exe -q -p " + sFlags + "\"%TMPINPUTFILE%\"").c_str(), sInputFile, "", 0, 0);
				}
				
				sFlags = "";
				//iLevel = min(gudtOptions.iLevel * 7 / 9, 7) + 1;
				sFlags += "--out-deflate=zopfli,iter=" + sPNGWolfIterations + " ";
				RunPlugin((unsigned int) iCount, "pngwolf (9/18)", (sPluginsDirectory + "pngwolf.exe " + sFlags + "--in=\"%INPUTFILE%\" --out=\"%TMPOUTPUTFILE%\"").c_str(), sInputFile, "", 0, 0);

				if (!bIsPNG9Patch)
				{
					RunPlugin((unsigned int) iCount, "pngrewrite (10/18)", (sPluginsDirectory + "pngrewrite.exe \"%INPUTFILE%\" \"%TMPOUTPUTFILE%\"").c_str(), sInputFile, "", 0, 0);

					sFlags = "";
					//iLevel = min(gudtOptions.iLevel * 7 / 9, 7) + 1;
					sFlags += "-i " + sIterations + " ";
					if (!gudtOptions.bPNGCopyMetadata)
					{					
						RunPlugin((unsigned int) iCount, "advpng (11/18)", (sPluginsDirectory + "advpng.exe -z -q -4 " + sFlags + "\"%TMPINPUTFILE%\"").c_str(), sInputFile, "", 0, 0);
					}
				}
			}

			sFlags = "";
			//ECT will preserve APNG compatibility when --reuse is used and -strip is not used
			if (bIsAPNG)
			{
				sFlags += "--reuse ";
			}
			else if (!gudtOptions.bPNGCopyMetadata)
			{
				sFlags += "-strip ";
			}
			iLevel = min(gudtOptions.iLevel * 8 / 9, 8) + 1;
			//Convert level 9 to level 90032 because it is faster and usually with higher savings
			if (iLevel == 9)
			{
				iLevel = 90032;
			}
			sFlags += "-" + (String) iLevel + " ";
			RunPlugin((unsigned int) iCount, "ECT (12/18)", (sPluginsDirectory + "ECT.exe -quiet --mt-deflate --mt-file --allfilters " + sFlags + "\"%TMPINPUTFILE%\"").c_str(), sInputFile, "", 0, 0);

			sFlags = "";
			iLevel = min(gudtOptions.iLevel * 4 / 9, 4);
			sFlags += "-s" + (String) iLevel + " ";
			if (gudtOptions.bPNGCopyMetadata)
			{
				sFlags += "-nostrip ";
			}
			if (gudtOptions.bPNGAllowLossy)
			{
				sFlags += "-quality=95 ";
			}
			else
			{
				sFlags += "-lossless ";
			}
			RunPlugin((unsigned int) iCount, "pingo (13/18)", (sPluginsDirectory + "pingo.exe " + sFlags + "\"%TMPINPUTFILE%\"").c_str(), sInputFile, "", 0, 0);
			

			sFlags = "";
			if (gudtOptions.bPNGCopyMetadata)
			{
				sFlags += "/k ";
			}
			
			if ((!bIsAPNG) && (!bIsPNG9Patch))
			{	
				RunPlugin((unsigned int) iCount, "DeflOpt (14/18)", (sPluginsDirectory + "deflopt.exe /a /b /s " + sFlags + "\"%TMPINPUTFILE%\"").c_str(), sInputFile, "", 0, 0);

				sFlags = "";
				if (!gudtOptions.bPNGCopyMetadata)
				{
					sFlags += "--strip ";
				}
				if (gudtOptions.iLevel >= 9)
				{
					sFlags += "--max ";
				}
				RunPlugin((unsigned int) iCount, "Columbo (15/18)", (sPluginsDirectory + "columbo.exe " + sFlags + "\"%TMPINPUTFILE%\"").c_str(), sInputFile, "", 0, 0);
			}

			RunPlugin((unsigned int) iCount, "defluff (16/18)", (sPluginsDirectory + "defluff.bat \"%INPUTFILE%\" \"%TMPOUTPUTFILE%\"").c_str(), sInputFile, "", 0, 0);

			if ((!bIsAPNG) && (!bIsPNG9Patch))
			{
				sFlags = "";
				if (gudtOptions.bPNGCopyMetadata)
				{
					sFlags += "/k ";
				}
				RunPlugin((unsigned int) iCount, "DeflOpt (17/18)", (sPluginsDirectory + "deflopt.exe /a /b /s " + sFlags + "\"%TMPINPUTFILE%\"").c_str(), sInputFile, "", 0, 0);

				sFlags = "";
				if (!gudtOptions.bPNGCopyMetadata)
				{
					sFlags += "--strip ";
				}
				if (gudtOptions.iLevel >= 9)
				{
					sFlags += "--max ";
				}
				RunPlugin((unsigned int) iCount, "Columbo (18/18)", (sPluginsDirectory + "columbo.exe " + sFlags + "\"%TMPINPUTFILE%\"").c_str(), sInputFile, "", 0, 0);
			}
		}

		// SWF: Leanfy, flasm, zRecompress
		if (PosEx(sExtensionByContent, KS_EXTENSION_SWF) > 0)
		{
			String sTmpOutputFile = ChangeFileExt(sInputFile, ".$wf");
			RunPlugin((unsigned int) iCount, "flasm (1/5)", (sPluginsDirectory + "flasm.exe -x \"%INPUTFILE%\"").c_str(), sInputFile, "", 0, 0);
			//A flasm killed by Stop (or because it hung) leaves a partial file, which must not replace the input
			if ((!gbStop) && (!mbPluginTimedOut))
			{
				clsUtil::CopyFile(sTmpOutputFile.c_str(), sInputFile.c_str(), true);
			}
			clsUtil::DeleteFile(sTmpOutputFile.c_str());

			RunPlugin((unsigned int) iCount, "flasm (2/5)", (sPluginsDirectory + "flasm.exe -u \"%INPUTFILE%\"").c_str(), sInputFile, "", 0, 0);
			//A flasm killed by Stop (or because it hung) leaves a partial file, which must not replace the input
			if ((!gbStop) && (!mbPluginTimedOut))
			{
				clsUtil::CopyFile(sTmpOutputFile.c_str(), sInputFile.c_str(), true);
			}
			clsUtil::DeleteFile(sTmpOutputFile.c_str());

			RunPlugin((unsigned int) iCount, "flasm (3/5)", (sPluginsDirectory + "flasm.exe -z \"%INPUTFILE%\"").c_str(), sInputFile, "", 0, 0);
			//Read the cell under the critical section, because the main thread also reads it while painting
			unsigned long long lOptimizedBytes, lTmpOutputSize;
			EnterCriticalSection(&mudtCriticalSection);
			lOptimizedBytes = ParseNumberThousand(grdFiles->Cells[KI_GRID_OPTIMIZED][iCount]);
			LeaveCriticalSection(&mudtCriticalSection);
			//SizeFile is expensive, so call it only once
			lTmpOutputSize = clsUtil::SizeFile(sTmpOutputFile.c_str());
			if ((lTmpOutputSize > 0) && (lTmpOutputSize < lOptimizedBytes) && (!gbStop) && (!mbPluginTimedOut))
			{
				clsUtil::CopyFile(sTmpOutputFile.c_str(), sInputFile.c_str(), true);
				EnterCriticalSection(&mudtCriticalSection);
				grdFiles->Cells[KI_GRID_OPTIMIZED][(int) iCount] = FormatNumberThousand(clsUtil::SizeFile(sInputFile.c_str()));
				LeaveCriticalSection(&mudtCriticalSection);
			}
			clsUtil::DeleteFile(sTmpOutputFile.c_str());

			RunPlugin((unsigned int) iCount, "zRecompress (4/5)", (sPluginsDirectory + "zRecompress.exe -tswf-lzma \"%TMPINPUTFILE%\"").c_str(), sInputFile, "", 0, 0);

			sFlags = "";
			//iLevel = min(gudtOptions.iLevel * 8 / 9, 8) + 1;
			sFlags += "-i " + sLeanifyIterations + " ";
			RunPlugin((unsigned int) iCount, "Leanify (5/5)", (sPluginsDirectory + "leanify.exe -q -p " + sFlags + "\"%TMPINPUTFILE%\"").c_str(), sInputFile, "", 0, 0);
		}
		// SQLITE: sqlite
		if (PosEx(sExtensionByContent, KS_EXTENSION_SQLITE) > 0)
		{
			RunPlugin((unsigned int) iCount, "sqlite (1/1)", (sPluginsDirectory + "sqlite.bat \"%INPUTFILE%\" \"%TMPOUTPUTFILE%\"").c_str(), sInputFile, "", 0, 0);
		}
		// TAR: Leanify
		if (PosEx(sExtensionByContent, KS_EXTENSION_TAR) > 0)
		{
			sFlags = "";
			if (gudtOptions.bGZCopyMetadata)
			{
				sFlags += "--keep-exif ";
			}
			//iLevel = min(gudtOptions.iLevel * 8 / 9, 8) + 1;
			sFlags += "-i " + sLeanifyIterations + " ";
			RunPlugin((unsigned int) iCount, "Leanify (1/1)", (sPluginsDirectory + "leanify.exe -q -p " + sFlags + "\"%TMPINPUTFILE%\"").c_str(), sInputFile, "", 0, 0);
		}
		// Tencent QQ: Leanify
		if (PosEx(sExtensionByContent, KS_EXTENSION_TENCENTQQ) > 0)
		{
			if (!gudtOptions.bPNGCopyMetadata)
			{
				sFlags = "";
				//iLevel = min(gudtOptions.iLevel * 8 / 9, 8) + 1;
				sFlags += "-i " + sLeanifyIterations + " ";
				RunPlugin((unsigned int) iCount, "Leanify (1/1)", (sPluginsDirectory + "leanify.exe -q -p " + sFlags + "\"%TMPINPUTFILE%\"").c_str(), sInputFile, "", 0, 0);
			}
		}
		
		// TGA: ImageMagick
		if (PosEx(sExtensionByContent, KS_EXTENSION_TGA) > 0)
		{
			sFlags = "";
			if (!gudtOptions.bTGACopyMetadata)
			{
				sFlags += "-strip ";
			}
			RunPlugin((unsigned int) iCount, "ImageMagick (1/1)", (sPluginsDirectory + "magick.exe convert -quiet -compress RLE " + sFlags + "\"%INPUTFILE%\" \"%TMPOUTPUTFILE%\"").c_str(), sInputFile, "", 0, 0);
		}
		// TIFF: jhead, ImageMagick, jpegoptim, jpegtran, mozjpegtran, tinydng
		if (PosEx(sExtensionByContent, KS_EXTENSION_TIFF) > 0)
		{
			bool bIsJPEGCMYK = IsJPEGCMYK(sInputFile.c_str());

			//jhead is corrupting CMYK JPEG images
			if (!bIsJPEGCMYK)
			{
				sFlags = "";
				if (gudtOptions.bTIFFCopyMetadata)
				{
					sFlags += "-zt ";
				}
				else
				{
					sFlags += "-purejpg -di -dx -dt -zt ";
				}	
				RunPlugin((unsigned int) iCount, "jhead (1/6)", (sPluginsDirectory + "jhead.exe -q -autorot " + sFlags + " \"%TMPINPUTFILE%\"").c_str(), sInputFile, "", 0, 0);
			}
			// ImageMagick does not keep metadata on TIFF images so disable it
			if (!gudtOptions.bTIFFCopyMetadata)
			{
				RunPlugin((unsigned int) iCount, "ImageMagick (2/6)", (sPluginsDirectory + "magick.exe convert \"%INPUTFILE%\" -quiet -compress ZIP -strip \"%TMPOUTPUTFILE%\"").c_str(), sInputFile, "", 0, 0);
			}
			
			sFlags = "";
			if (!gudtOptions.bTIFFCopyMetadata)
			{
				sFlags += "--strip-all ";
			}
			if (gudtOptions.bJPEGUseArithmeticEncoding)
			{
				sFlags += "--with-arith ";
			}
			sFlags += "-w " + (String) gudtSystemInfo.dwNumberOfProcessors + " ";
			RunPlugin((unsigned int) iCount, "jpegoptim (3/6)", (sPluginsDirectory + "jpegoptim.exe -o -q --all-progressive --nofix " + sFlags + "\"%TMPINPUTFILE%\"").c_str(), sInputFile, "", 0, 0);

			sFlags = "";
			if (gudtOptions.bJPEGUseArithmeticEncoding)
			{
				sFlags += "-arithmetic ";
			}
			if (gudtOptions.bTIFFCopyMetadata)
			{
				sFlags += "-copy all ";
			}
			else
			{
				sFlags += "-copy none ";
			}
			RunPlugin((unsigned int) iCount, "jpegtran (4/6)", (sPluginsDirectory + "jpegtran.exe -progressive -optimize " + sFlags + "\"%INPUTFILE%\" \"%TMPOUTPUTFILE%\"").c_str(), sInputFile, "", 0, 0);

			RunPlugin((unsigned int) iCount, "mozjpegtran (5/6)", (sPluginsDirectory + "mozjpegtran.exe -outfile \"%TMPOUTPUTFILE%\" -progressive -optimize -perfect " + sFlags + "\"%INPUTFILE%\"").c_str(), sInputFile, "", 0, 0);
			
			RunPlugin((unsigned int) iCount, "tinydng (6/6)", (sPluginsDirectory + "tinydng-cli.exe --input \"%INPUTFILE%\" -l -o \"%TMPOUTPUTFILE%\"").c_str(), sInputFile, "", 0, 0);
		}
		// WAV: shntool
		if (PosEx(sExtensionByContent, KS_EXTENSION_WAV) > 0)
		{
			if (!gudtOptions.bWAVCopyMetadata)
			{
				String sTmpOutputFile = ReplaceStr(sInputFile, ".wav", "-stripped.wav");
				//Prevent a bug in shntool with no lowercase extensions
				if (sTmpOutputFile == sInputFile)
				{
					sTmpOutputFile += "-stripped.wav";
				}

				RunPlugin((unsigned int) iCount, "shntool (1/2)", (sPluginsDirectory + "shntool.exe strip -q -O always \"%INPUTFILE%\"").c_str(), sInputFile, "", 0, 0);
				//Read the cell under the critical section, because the main thread also reads it while painting
				unsigned long long lOptimizedBytes, lTmpOutputSize;
				EnterCriticalSection(&mudtCriticalSection);
				lOptimizedBytes = ParseNumberThousand(grdFiles->Cells[KI_GRID_OPTIMIZED][iCount]);
				LeaveCriticalSection(&mudtCriticalSection);
				//SizeFile is expensive, so call it only once
				lTmpOutputSize = clsUtil::SizeFile(sTmpOutputFile.c_str());
				if ((lTmpOutputSize > 0) && (lTmpOutputSize < lOptimizedBytes) && (!gbStop) && (!mbPluginTimedOut))
				{
					clsUtil::CopyFile(sTmpOutputFile.c_str(), sInputFile.c_str(), true);
					EnterCriticalSection(&mudtCriticalSection);
					grdFiles->Cells[KI_GRID_OPTIMIZED][(int) iCount] = FormatNumberThousand(clsUtil::SizeFile(sInputFile.c_str()));
					LeaveCriticalSection(&mudtCriticalSection);
				}
				if (!gudtOptions.bDebug)
				{
					clsUtil::DeleteFile(sTmpOutputFile.c_str());
				}
				if (gudtOptions.bWAVStripSilence)
				{
					sTmpOutputFile = ReplaceStr(sInputFile, ".wav", "-trimmed.wav");
					//Prevent a bug in shntool with no lowercase extensions
					if (sTmpOutputFile == sInputFile)
					{
						sTmpOutputFile += "-trimmed.wav";
					}

					RunPlugin((unsigned int) iCount, "shntool (2/2)", (sPluginsDirectory + "shntool.exe trim -q -O always \"%INPUTFILE%\"").c_str(), sInputFile, "", 0, 0);
					//Read the cell under the critical section, because the main thread also reads it while painting
					EnterCriticalSection(&mudtCriticalSection);
					lOptimizedBytes = ParseNumberThousand(grdFiles->Cells[KI_GRID_OPTIMIZED][iCount]);
					LeaveCriticalSection(&mudtCriticalSection);
					//SizeFile is expensive, so call it only once
					lTmpOutputSize = clsUtil::SizeFile(sTmpOutputFile.c_str());
					if ((lTmpOutputSize > 0) && (lTmpOutputSize < lOptimizedBytes) && (!gbStop) && (!mbPluginTimedOut))
					{
						clsUtil::CopyFile(sTmpOutputFile.c_str(), sInputFile.c_str(), true);
						EnterCriticalSection(&mudtCriticalSection);
						grdFiles->Cells[KI_GRID_OPTIMIZED][(int) iCount] = FormatNumberThousand(clsUtil::SizeFile(sInputFile.c_str()));
						LeaveCriticalSection(&mudtCriticalSection);
					}
					if (!gudtOptions.bDebug)
					{
						clsUtil::DeleteFile(sTmpOutputFile.c_str());
					}
				}
			}
		}
		// XML: Leanify
		if (PosEx(sExtensionByContent, KS_EXTENSION_XML) > 0)
		{
			if (gudtOptions.bXMLEnableLeanify)
			{
				sFlags = "";
				//iLevel = min(gudtOptions.iLevel * 8 / 9, 8) + 1;
				sFlags += "-i " + sLeanifyIterations + " ";
				RunPlugin((unsigned int) iCount, "Leanify (1/2)", (sPluginsDirectory + "leanify.exe -q -p " + sFlags + "\"%TMPINPUTFILE%\"").c_str(), sInputFile, "", 0, 0);
				
				RunPlugin((unsigned int) iCount, "Minify (2/2)", (sPluginsDirectory + "minify.exe \"%INPUTFILE%\" --output \"%TMPOUTPUTFILE%\"").c_str(), sInputFile, "", 0, 0);
			}
		}
		// WEBP: pingo, dwebp + cwebp, ImageWorsener
		if (PosEx(sExtensionByContent, KS_EXTENSION_WEBP) > 0)
		{
			sFlags = "";
			iLevel = min(gudtOptions.iLevel * 4 / 9, 4);
			sFlags += "-s" + (String) iLevel + " ";
			if (gudtOptions.bWEBPAllowLossy)
			{
				sFlags += "-quality=95 -webp ";
			}
			else
			{
				sFlags += "-lossless ";
			}
			RunPlugin((unsigned int) iCount, "pingo (1/2)", (sPluginsDirectory + "pingo.exe " + sFlags + "\"%TMPINPUTFILE%\"").c_str(), sInputFile, "", 0, 0);
					
			sFlags = "";
			if (gudtOptions.bWEBPAllowLossy)
			{
				iLevel = min(gudtOptions.iLevel * 6 / 9, 6);
				sFlags += "-m " + (String) iLevel + " ";
			}
			else
			{
				iLevel = min(gudtOptions.iLevel * 9 / 9, 9);
				sFlags += "-z " + (String) iLevel + " -lossless ";
			}
			RunPlugin((unsigned int) iCount, "cwebp (2/2)", (sPluginsDirectory + "cwebp.exe -mt -quiet " + sFlags + "\"%INPUTFILE%\" -o \"%TMPOUTPUTFILE%\"").c_str(), sInputFile, "", 0, 0);

			//RunPlugin((unsigned int) iCount, "ImageWorsener", (sPluginsDirectory + "imagew.exe -noresize -zipcmprlevel 9 \"%INPUTFILE%\" \"%TMPOUTPUTFILE%\"").c_str(), sInputFile, "", 0, 0);
		}
		// ZIP: Leanify, ect, advzip, deflopt, columbo, defluff, deflopt, columbo
		if (PosEx(sExtensionByContent, KS_EXTENSION_ZIP) > 0)
		{
			bool bIsEXESFX = IsEXESFX(sInputFile.c_str());
			
			sFlags = "";
			if (gudtOptions.bZIPCopyMetadata)
			{
				sFlags += "--keep-exif ";
			}
			//iLevel = min(gudtOptions.iLevel * 8 / 9, 8) + 1;
			sFlags += "-i " + sLeanifyIterations + " ";
			sFlags += "--zip-deflate ";

			if (!gudtOptions.bZIPRecurse)
			{
				sFlags += "-d 1 ";
			}
			if (gudtOptions.bJPEGCopyMetadata)
			{
				sFlags += "--keep-exif --keep-icc --jpeg-keep-all ";
			}
			if (gudtOptions.bJPEGUseArithmeticEncoding)
			{
				sFlags += "--jpeg-arithmetic ";
			}		
			RunPlugin((unsigned int) iCount, "Leanify (1/8)", (sPluginsDirectory + "leanify.exe -q -p " + sFlags + "\"%TMPINPUTFILE%\"").c_str(), sInputFile, "", 0, 0);

			sFlags = "";
			if (!gudtOptions.bZIPCopyMetadata)
			{
				sFlags += "-strip ";
			}
			if ((!gudtOptions.bZIPRecurse) && (PosEx(sExtensionByContent, " .zip ") > 0))
			{
				sFlags += "--disable-png --disable-jpg ";
			}
			iLevel = min(gudtOptions.iLevel * 8 / 9, 8) + 1;
			//Convert level 9 to level 90032 because it is faster and usually with higher savings
			if (iLevel == 9)
			{
				iLevel = 90032;
			}
			sFlags += "-" + (String) iLevel + " ";
			RunPlugin((unsigned int) iCount, "ECT (2/8)", (sPluginsDirectory + "ECT.exe -quiet --mt-deflate --mt-file -zip " + sFlags + "\"%TMPINPUTFILE%\"").c_str(), sInputFile, "", 0, 0);

			//AdvZip strips header on ZIP files
			if (!bIsEXESFX)
			{
				sFlags = "";
				//iLevel = min(gudtOptions.iLevel * 7 / 9, 7) + 1;
				sFlags += "-i " + sIterations + " ";
				RunPlugin((unsigned int) iCount, "advzip (3/8)", (sPluginsDirectory + "advzip.exe -z -q -4 " + sFlags + "\"%TMPINPUTFILE%\"").c_str(), sInputFile, "", 0, 0);
			}
			
			sFlags = "";
			if (gudtOptions.bZIPCopyMetadata)
			{
				sFlags += "/c ";
			}
			RunPlugin((unsigned int) iCount, "DeflOpt (4/8)", (sPluginsDirectory + "deflopt.exe /a /b /s " + sFlags + "\"%TMPINPUTFILE%\"").c_str(), sInputFile, "", 0, 0);

			sFlags = "";
			if (!gudtOptions.bZIPCopyMetadata)
			{
				sFlags += "--strip ";
			}
			if (gudtOptions.iLevel >= 9)
			{
				sFlags += "--max ";
			}
			RunPlugin((unsigned int) iCount, "Columbo (5/8)", (sPluginsDirectory + "columbo.exe " + sFlags + "\"%TMPINPUTFILE%\"").c_str(), sInputFile, "", 0, 0);

			RunPlugin((unsigned int) iCount, "defluff (6/8)", (sPluginsDirectory + "defluff.bat \"%INPUTFILE%\" \"%TMPOUTPUTFILE%\"").c_str(), sInputFile, "", 0, 0);

			sFlags = "";
			if (gudtOptions.bZIPCopyMetadata)
			{
				sFlags += "/c ";
			}
			RunPlugin((unsigned int) iCount, "DeflOpt (7/8)", (sPluginsDirectory + "deflopt.exe /a /b /s " + sFlags + "\"%TMPINPUTFILE%\"").c_str(), sInputFile, "", 0, 0);

			sFlags = "";
			if (!gudtOptions.bZIPCopyMetadata)
			{
				sFlags += "--strip ";
			}
			if (gudtOptions.iLevel >= 9)
			{
				sFlags += "--max ";
			}
			RunPlugin((unsigned int) iCount, "Columbo (8/8)", (sPluginsDirectory + "columbo.exe " + sFlags + "\"%TMPINPUTFILE%\"").c_str(), sInputFile, "", 0, 0);
		}
		// 7Z / XZ: xz.exe (always for .xz/.lz/.lzma) + m7zRepacker (high levels)
		// CreateProcess does not interpret shell redirection, so xz is invoked via cmd /c.
		if (PosEx(sExtensionByContent, KS_EXTENSION_7Z) > 0)
		{
			// .xz and .lzma are native xz formats; .lz is lzip and is left for m7zRepacker/other tools
			if ((PosEx(_T(" .xz "), sExtensionByContent) > 0) || (PosEx(_T(" .lzma "), sExtensionByContent) > 0))
			{
				RunPlugin((unsigned int) iCount, "xz (1/2)", ("cmd.exe /c \"\"" + sPluginsDirectory + "xz.exe\" -e -9 -k -f -c \"%INPUTFILE%\" > \"%TMPOUTPUTFILE%\"\"").c_str(), sInputFile, "", 0, 0);
			}

			// Very slow; only in high compression profiles (classic 7z)
			if (gudtOptions.iLevel > 7)
			{
				#if defined(_WIN64)
					RunPlugin((unsigned int) iCount, "m7zRepacker (2/2)", (sPluginsDirectory + "m7zrepacker.exe -m1 -d1024 -mem2048 \"%TMPINPUTFILE%\"").c_str(), sInputFile, "", 0, 0);
				#else
					RunPlugin((unsigned int) iCount, "m7zRepacker (2/2)", (sPluginsDirectory + "m7zrepacker.exe -m1 -d128 -mem512 \"%TMPINPUTFILE%\"").c_str(), sInputFile, "", 0, 0);
				#endif
			}
		}
		// FONT: ttfautohint (TTF/OTF/TTC only; WOFF/WOFF2 are containers ttfautohint does not read)
		if ((PosEx(_T(" .ttf "), sExtensionByContent) > 0) || (PosEx(_T(" .otf "), sExtensionByContent) > 0) || (PosEx(_T(" .ttc "), sExtensionByContent) > 0))
		{
			RunPlugin((unsigned int) iCount, "ttfautohint (1/1)", (sPluginsDirectory + "ttfautohint.exe -f none \"%INPUTFILE%\" \"%TMPOUTPUTFILE%\"").c_str(), sInputFile, "", 0, 0);
		}
		// MISC: ImageMagick
		if (PosEx(sExtensionByContent, KS_EXTENSION_MISC) > 0)
		{
			if (!gudtOptions.bMiscDisable)
			{
				sFlags = "";
				if (!gudtOptions.bMiscCopyMetadata)
				{
					sFlags += "-strip ";
				}
				RunPlugin((unsigned int) iCount, "ImageMagick (1/1)", (sPluginsDirectory + "magick.exe convert \"%INPUTFILE%\" -quiet " + sFlags + "\"%TMPOUTPUTFILE%\"").c_str(), sInputFile, "", 0, 0);
			}
		}

		if (gudtOptions.bKeepAttributes)
		{
			//Restore timestamp if we were able to get it
			if ((udtFileCreated.dwLowDateTime != 0) || (udtFileCreated.dwHighDateTime != 0))
			{
				clsUtil::SetFileTimestamp(sApiName.c_str(), &udtFileCreated, &udtFileAccessed, &udtFileModified);
			}
			if (iFileAttributes != INVALID_FILE_ATTRIBUTES)
			{
				SetFileAttributes(sApiName.c_str(), iFileAttributes);
			}
		}

		//Make sure the file was indeed processed because asuming we got gains. This is to solve Pending items being counted as 0 bytes
		//Read the cells and update the shared counters and statistics under the critical section, because workers do it concurrently, and the main thread also reads them while painting
		//Read the size cells once instead of re-parsing them for every counter: they are scanned character by character
		EnterCriticalSection(&mudtCriticalSection);
		if (grdFiles->Cells[KI_GRID_OPTIMIZED][iCount] != "")
		{
			unsigned long long lOriginalBytes = ParseNumberThousand(grdFiles->Cells[KI_GRID_ORIGINAL][iCount]);
			unsigned long long lOptimizedBytes = ParseNumberThousand(grdFiles->Cells[KI_GRID_OPTIMIZED][iCount]);
			//The file could have changed after being added, so an optimized size bigger than the original one must not wrap around the unsigned subtraction, or the totals and the statistics would be ruined
			unsigned long long lFileSavedBytes = (lOriginalBytes > lOptimizedBytes) ? (lOriginalBytes - lOptimizedBytes) : 0;

			lTotalBytes += lOriginalBytes;
			lSavedBytes += lFileSavedBytes;

			gudtOptions.lStatTotalBytes += lOriginalBytes;
			gudtOptions.lStatSavedBytes += lFileSavedBytes;
			gudtOptions.iStatFiles++;
		}
		LeaveCriticalSection(&mudtCriticalSection);
	}

	//If file was not processed, mark it as skipped because not supported extension, or skipped because user preference (do not process JS for instance)
	//Read the status cell once under the critical section, and deep copy it so the String is thread private, because its reference count is not updated atomically
	String sStatus;
	EnterCriticalSection(&mudtCriticalSection);
	sStatus = grdFiles->Cells[KI_GRID_STATUS][iCount].c_str();
	LeaveCriticalSection(&mudtCriticalSection);
	if ((sStatus == _("Pending")) || (sStatus == _("Copying to Recyclebin...")) || (sStatus == _("Creating backup...")))
	{
		EnterCriticalSection(&mudtCriticalSection);
		grdFiles->Cells[KI_GRID_STATUS][iCount] = _("Skipped");
		LeaveCriticalSection(&mudtCriticalSection);
	}
	else if (sStatus != _("Optimized"))
	{
		//Read the size cells under the critical section, because the main thread also reads them while painting
		unsigned long long lOriginalBytes, lOptimizedBytes;
		EnterCriticalSection(&mudtCriticalSection);
		lOriginalBytes = ParseNumberThousand(grdFiles->Cells[KI_GRID_ORIGINAL][iCount]);
		lOptimizedBytes = ParseNumberThousand(grdFiles->Cells[KI_GRID_OPTIMIZED][iCount]);
		LeaveCriticalSection(&mudtCriticalSection);
		unsigned int iPercentBytes = 0;
		//Prevent dividing by zero, which would give an invalid percentage
		if (lOriginalBytes > 0)
		{
			iPercentBytes = ((unsigned int) ((double) lOptimizedBytes / lOriginalBytes * 100));
		}

		//Required indirection
		
		iEndTicks = GetTickCount();
		TCHAR acTime[64];
		StrFromTimeInterval(acTime, (sizeof(acTime) / sizeof(TCHAR)) - 1, (unsigned int) (iEndTicks - iStartTicks), 3);
		
		sCaption.printf(_((TCHAR *) _T("Done (%3u%%) in %s")), iPercentBytes, acTime);
		EnterCriticalSection(&mudtCriticalSection);
		grdFiles->Cells[KI_GRID_STATUS][iCount] = sCaption;
		LeaveCriticalSection(&mudtCriticalSection);

		//Update cache. Guarded because INI file writes are not safe when done concurrently
		//A file whose optimization was interrupted by Stop is not cached, or it would be skipped as already optimized next time
		if ((gudtOptions.bEnableCache) && (!gbStop))
		{
			String sHashValue = Hash(sInputFile.c_str());
			unsigned int iHashKey = clsUtil::Crc32(sHashValue.c_str(), (unsigned int) sHashValue.Length() * sizeof(TCHAR));
			EnterCriticalSection(&mudtCriticalSectionIni);
			clsUtil::SetIni(_T("Cache"), ((String) iHashKey).c_str(), sHashValue.c_str());
			LeaveCriticalSection(&mudtCriticalSectionIni);
		}
	}
	//Count the file as processed, so the main thread can update the global progress
	InterlockedIncrement(&miProcessedFiles);
	if (GetCurrentThreadId() == MainThreadID)
	{
		RefreshStatus(true, (unsigned int) iCount, lTotalBytes, lSavedBytes);
	}
}



//---------------------------------------------------------------------------
#pragma argsused
void __fastcall TfrmMain::tmrMainTimer(TObject *Sender)
{
	//Elapsed time
	tmrMain->Tag += tmrMain->Interval;
	
	//30 seconds: Update check and enable ads
	if (tmrMain->Tag >= 30000)
	{
		tmrMain->Enabled = false;
		if (gudtOptions.iCheckForUpdates < 0)
		{
			String sCaption;
			sCaption.printf(_((TCHAR *) _T("Do you want %s to automatically check for updates?")), Application->Name.c_str());
			gudtOptions.iCheckForUpdates = (clsUtil::MsgBox(Handle, sCaption.c_str(), _((TCHAR *) _T("Check for updates")), MB_YESNO | MB_ICONQUESTION) == ID_YES);
		}
		else if ((gudtOptions.iCheckForUpdates == 1) || (gudtOptions.acDonator[0] != 0))
		{
			CheckForUpdates(true);
		}
	}
	//1 second: Process command-line arguments
	else if (tmrMain->Tag >= gudtOptions.iStartupDelay)
	{
		tmrMain->Interval = 30000;

		if (_argc > 1)
		{
			Screen->Cursor = crAppStart;
			grdFiles->Enabled = false;  //Prevent grid modifications while adding files
			//Show();	//Required because some themes do not automatically refresh
			Application->ProcessMessages();
			AddFilesInitializeExist();
			bool bAdded = false;
			for (unsigned int iCount = 1; iCount < (unsigned int) _argc; iCount++)
			{
				//Skip options starting with /
				if (_targv[iCount][0] != '/')
				{
					//Full path, because the current directory is changed while optimizing, so relative ones would not be found any more
					AddFiles(ExpandFileName(_targv[iCount]).c_str());
					bAdded = true;
				}
			}
			//Restore the interface also when only options were specified, or the grid would stay disabled
			grdFiles->Enabled = true;
			Screen->Cursor = crDefault;
			if (bAdded)
			{
				//Avoid timer re-entrancy (update check, message boxes) while optimizing
				tmrMain->Enabled = false;
				//Disable Beep when done to allow closing automatically, restoring it afterwards so the user setting is not lost
				bool bBeepWhenDone = gudtOptions.bBeepWhenDone;
				gudtOptions.bBeepWhenDone = false;
				RefreshStatus();
				actOptimizeExecute(Sender);
				gudtOptions.bBeepWhenDone = bBeepWhenDone;
				actExitExecute(Sender);
			}
		}
	}
}



//---------------------------------------------------------------------------
#pragma argsused
void __fastcall TfrmMain::lblCopyrightClick(TObject *Sender)
{
	ShellExecute(Handle, _T("open"), KS_APP_URL, _T(""), _T(""), SW_SHOWNORMAL);
}



// ---------------------------------------------------------------------------
void __fastcall TfrmMain::WMDropFiles(const TWMDropFiles &udtMessage)
{
	HDROP hDrop = (HDROP) udtMessage.Drop;
	if (hDrop)
	{
		unsigned int iFiles = DragQueryFile(hDrop, -1, NULL, 0);
		if (iFiles > 0)
		{
			Screen->Cursor = crAppStart;
			grdFiles->Enabled = false;  //Prevent grid modifications while adding files
			Application->ProcessMessages();
			AddFilesInitializeExist();
			for (unsigned int iCount = 0; iCount < iFiles; iCount++)
			{
				TCHAR acBuffer[PATH_MAX + 1];

				if (DragQueryFile(hDrop, iCount, acBuffer, (sizeof(acBuffer) / sizeof(TCHAR)) - 1))
				{
					AddFiles(acBuffer);
				}
			}
			grdFiles->Enabled = true;
			RefreshStatus();
			Screen->Cursor = crDefault;
			SetForegroundWindow(Handle); // Bring to front
			BringToFront();
		}
		DragFinish(hDrop);
	}
}



// ---------------------------------------------------------------------------
void __fastcall TfrmMain::AddFilesInitializeExist(void)
{
	//A new batch of files starts: forget the cache section of the previous one, because the optimizer could have added entries since then
	miCacheLookups = 0;
	delete mlstCache;
	mlstCache = NULL;

	if (!gudtOptions.bAllowDuplicates)
	{
		if (!msetFilesExist)
		{
			msetFilesExist = new std::unordered_set<std::wstring>();
		}
		msetFilesExist->clear();
		int iRows = grdFiles->RowCount;
		msetFilesExist->reserve((size_t) iRows + 1024);
		for (int iRow = 0; iRow < iRows; iRow++)
		{
			msetFilesExist->insert(std::wstring(grdFiles->Cells[KI_GRID_FILE][iRow].c_str()));
		}
	}
}



// ---------------------------------------------------------------------------
bool __fastcall TfrmMain::AddFilesExist(const String psFile)
{
	bool bRes = false;

	if ((!gudtOptions.bAllowDuplicates) && (msetFilesExist))
	{
		//Inserting reports whether it was really added, so it already existed when it was not
		bRes = !(msetFilesExist->insert(std::wstring(psFile.c_str())).second);
	}
	return (bRes);
}



// ---------------------------------------------------------------------------
//Starts (pbStart) or stops the marquee (back and forth) animation of the progress bar, used while adding files. The VCL wrapper has no property for it on all the compiler versions in use, so the style bit is changed directly on the control handle. PBM_SETMARQUEE comes from comctl32 v6, which is a system requirement anyway (Vista or later)
#ifndef PBS_MARQUEE
	#define PBS_MARQUEE 0x00000008
#endif
#ifndef PBM_SETMARQUEE
	#define PBM_SETMARQUEE (WM_USER + 10)
#endif
static void SetMarqueeProgressBar(TProgressBar *pgbProgress, const bool pbStart)
{
	if (pgbProgress)
	{
		const LONG lStyle = GetWindowLong(pgbProgress->Handle, GWL_STYLE);
		//The style bit is changed only when it differs, so a call with the current state does nothing
		if ((pbStart) && ((lStyle & PBS_MARQUEE) == 0))
		{
			SetWindowLong(pgbProgress->Handle, GWL_STYLE, lStyle | PBS_MARQUEE);
		}
		if ((!pbStart) && ((lStyle & PBS_MARQUEE) != 0))
		{
			SetWindowLong(pgbProgress->Handle, GWL_STYLE, lStyle & ~PBS_MARQUEE);
		}
		//Starts or stops the animation. A speed of 0 means the default 30 milliseconds per animation step
		SendMessage(pgbProgress->Handle, PBM_SETMARQUEE, (WPARAM) pbStart, (LPARAM) 0);
	}
}

// ---------------------------------------------------------------------------
// ---------------------------------------------------------------------------
//Adds one element of the file list (a file, folder or wildcard), expanding folders recursively. It only does the marquee bookkeeping around AddFilesScan, inside a try/__finally so the animation cannot be left running if something unexpected throws
void __fastcall TfrmMain::AddFiles(const TCHAR *pacFile, const WIN32_FIND_DATA *pudtKnown)
{
	//The program is being closed while adding files, so do not keep adding
	if (gbClosing)
	{
		return;
	}
	//Number of nested AddFiles calls in progress, so the marquee is started once per batch and not restarted on every recursion level
	static unsigned int siAddFilesDepth = 0;
	siAddFilesDepth++;
	//While adding files the final count is not known until the whole scan ends, so instead of the stepped bar (which can look stalled on a big folder) the progress bar animates as a marquee (back and forth), making clear the program is working. Only the outermost call touches it, or every recursion would restart it, and not while optimizing, where the workers feed it with the real progress
	if ((siAddFilesDepth == 1) && (!gbProcess))
	{
		SetMarqueeProgressBar(pgbProgress, true);
	}
	try
	{
		AddFilesScan(pacFile, pudtKnown);
	}
	__finally
	{
		//The marquee is stopped only when the outermost call ends, because the callers add one element per call
		siAddFilesDepth--;
		if (siAddFilesDepth == 0)
		{
			SetMarqueeProgressBar(pgbProgress, false);
		}
	}
}

// ---------------------------------------------------------------------------
void __fastcall TfrmMain::AddFilesScan(const TCHAR *pacFile, const WIN32_FIND_DATA *pudtKnown)
{
	WIN32_FIND_DATA udtFindFileData;
	WIN32_FILE_ATTRIBUTE_DATA udtFileAttribute;
	static unsigned int iLastRefresh = 0;

	//Keep the interface alive and showing the growing list while adding many files, even inside a huge folder, but only 10 times per second because refreshing is expensive
	if ((GetTickCount() - iLastRefresh) >= 100)
	{
		iLastRefresh = GetTickCount();
		RefreshStatus();
	}

	//When it comes from a folder listing, its attributes and size are already known, so asking for them again (one more round trip for every file, very slow on network drives) is not needed
	bool bAttributes = true;
	if (pudtKnown)
	{
		udtFileAttribute.dwFileAttributes = pudtKnown->dwFileAttributes;
		udtFileAttribute.nFileSizeHigh = pudtKnown->nFileSizeHigh;
		udtFileAttribute.nFileSizeLow = pudtKnown->nFileSizeLow;
	}
	else
	{
		bAttributes = GetFileAttributesEx(clsUtil::GetApiName(pacFile).c_str(), GetFileExInfoStandard, (void *) &udtFileAttribute);
	}
	if (bAttributes)
	{
		//If it is a directory, recurse
		if (udtFileAttribute.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)
		{
			HANDLE hFindFile = FindFirstFile(clsUtil::GetApiName((((String) pacFile) + "\\*").c_str()).c_str(), &udtFindFileData);
			if (hFindFile != INVALID_HANDLE_VALUE )
			{
				do
				{
					if ((_tcscmp(udtFindFileData.cFileName, _T(".")) != 0) && (_tcscmp(udtFindFileData.cFileName, _T("..")) != 0) && (((udtFindFileData.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) == 0) || ((udtFindFileData.dwReserved0 != IO_REPARSE_TAG_MOUNT_POINT) && (udtFindFileData.dwReserved0 != IO_REPARSE_TAG_SYMLINK))))
					{
						
					
						AddFiles((((String) pacFile) + "\\" + udtFindFileData.cFileName).c_str(), &udtFindFileData);
					}
				}
				while ((bool) FindNextFile(hFindFile, &udtFindFileData) != false);
				FindClose(hFindFile);
			}
		}
		//It it is a file, parse it
		else
		{
			String sCellFile = SetCellFileValue(pacFile);
			unsigned long long lSize = (((unsigned long long) udtFileAttribute.nFileSizeHigh) << 32) + udtFileAttribute.nFileSizeLow;

			//We will only add files with more than 0 bytes
			if (lSize > 0)
			{
				//Check if already added
				if (!gudtOptions.bAllowDuplicates)
				{
				//if (grdFiles->Cols[KI_GRID_FILE]->IndexOf(sCellFile) >= 0)
				if (AddFilesExist(sCellFile))
				{
					return;
				}
				}

				String sExtensionByContent = GetExtensionByContent(pacFile);
				if (sExtensionByContent != "")
				{
					//We store the name to show concatenated with the full name
					unsigned int iRows = (unsigned int) grdFiles->RowCount;

					String sExtension = ExtractFileExt(pacFile).LowerCase();
					if (sExtensionByContent != sExtension)
					{
						sExtension = sExtension + " (" + sExtensionByContent + ")";
					}
					String sStatus;

					//Check if it was already optimized
					if (gudtOptions.bEnableCache)
					{
						String sHashValue = Hash(pacFile);
						unsigned int iHashKey = clsUtil::Crc32(sHashValue.c_str(), (unsigned int) sHashValue.Length() * sizeof(TCHAR));
						String sHashKey = (String) iHashKey;

						//The first lookup of a batch reads the INI file directly, but when more files follow, the Cache section is loaded once into a hashed list: every direct lookup parses the INI file again, which gets very slow with big caches
						//A path with a semicolon is always looked up in the list: GetIni treats the semicolon as the start of a comment and cuts the value, so such a file was never recognized as already optimized when it was the only one added
						if (((miCacheLookups == 1) || (sHashValue.Pos(";") > 0)) && (!mlstCache))
						{
							bool bLoaded = true;
							mlstCache = new THashedStringList();
							mlstCache->CaseSensitive = true;
							//Same precedence as GetIni: the all users file first, then the current user one (when it is a different file)
							for (int iIni = 0; (iIni < 2) && (bLoaded); iIni++)
							{
								const TCHAR *pacIni = clsUtil::GetIniPath(iIni == 0);
								if ((iIni == 1) && (_tcscmp(pacIni, clsUtil::GetIniPath(true)) == 0))
								{
									break;
								}
								unsigned long long lIniSize = clsUtil::SizeFile(pacIni);
								if (lIniSize > 0)
								{
									//One character per byte at most, plus room for the terminators, so a complete section is never mistaken for a truncated one
									unsigned long lChars = (unsigned long) lIniSize + 4;
									TCHAR *acSection = (lIniSize < 0x10000000) ? new TCHAR[lChars] : nullptr;
									if (acSection)
									{
										memset(acSection, 0, lChars * sizeof(TCHAR));
										if (GetPrivateProfileSection(_T("Cache"), acSection, lChars, pacIni) < (lChars - 2))
										{
											for (const TCHAR *pcEntry = acSection; *pcEntry != NULL; pcEntry += _tcslen(pcEntry) + 1)
											{
												mlstCache->Add(pcEntry);
											}
										}
										else
										{
											bLoaded = false;
										}
										delete[] acSection;
									}
									else
									{
										bLoaded = false;
									}
								}
							}
							//Anything unexpected: forget the partial list and keep reading the INI file directly
							if (!bLoaded)
							{
								delete mlstCache;
								mlstCache = NULL;
							}
						}
						miCacheLookups++;
						//In cache, show it as already optimized
						if (((mlstCache) && (mlstCache->Values[sHashKey] == sHashValue)) || ((!mlstCache) && (_tcscmp(clsUtil::GetIni(_T("Cache"), sHashKey.c_str(), _T("")), sHashValue.c_str()) == 0)))
						{
							sStatus = _("Optimized");
						}
						else
						{
							sStatus = _("Pending");
						}
					}
					else
					{
						sStatus = _("Pending");
					}
					//Direct cell writes, without creating a row object nor a temporary list for every file. The optimized size cell is already empty in a new row
					grdFiles->RowCount = (int) iRows + 1;
					grdFiles->Cells[KI_GRID_FILE][(int) iRows] = sCellFile;
					grdFiles->Cells[KI_GRID_EXTENSION][(int) iRows] = sExtension;
					grdFiles->Cells[KI_GRID_ORIGINAL][(int) iRows] = FormatNumberThousand(lSize);
					grdFiles->Cells[KI_GRID_STATUS][(int) iRows] = sStatus;
				}
			}
		}
	}
	//Not found, but it can be a wildcard (such as *.JPG), which is expanded in the folder of the pattern
	else if (_tcspbrk(pacFile, _T("*?")) != nullptr)
	{
		HANDLE hFindFile = FindFirstFile(clsUtil::GetApiName(pacFile).c_str(), &udtFindFileData);
		if (hFindFile != INVALID_HANDLE_VALUE)
		{
			String sDirectory = ExtractFilePath(pacFile);
			do
			{
				if ((_tcscmp(udtFindFileData.cFileName, _T(".")) != 0) && (_tcscmp(udtFindFileData.cFileName, _T("..")) != 0))
				{
					AddFiles((sDirectory + udtFindFileData.cFileName).c_str(), &udtFindFileData);
				}
			}
			while ((bool) FindNextFile(hFindFile, &udtFindFileData) != false);
			FindClose(hFindFile);
		}
	}
}



// ---------------------------------------------------------------------------
int __fastcall TfrmMain::RunPlugin(unsigned int piCurrent, String psStatus, String psCommandLine, String psInputFile, String psOutputFile, int piErrorMin, int piErrorMax)
{
	String sInputFile, sOutputFile, sTmpInputFile, sTmpOutputFile, sCommandLine;
	TCHAR acTempPath[PATH_MAX + 1] = _T("");

	//Whether the last plugin of this worker was killed for taking too long is only meaningful for the plugin that is about to run (the blocks that read its output file themselves check it)
	mbPluginTimedOut = false;

	//Thread local deep copies of the shared application name and plugins directory, so concurrent workers do not update the reference count of shared Strings
	String sAppName, sPluginsDirectory;
	EnterCriticalSection(&mudtCriticalSection);
	sAppName = Application->Name.c_str();
	sPluginsDirectory = ::sPluginsDirectory.c_str();
	LeaveCriticalSection(&mudtCriticalSection);


	if (gbStop)
	{
		//Close();
		return (0);
	}

	//The masks were already split and upper cased when the optimization started, so only the command line has to be upper cased
	if (siDisablePluginMasks > 0)
	{
		String sCommandLineUpper = psCommandLine.UpperCase();
		for (unsigned int iMask = 0; iMask < siDisablePluginMasks; iMask++)
		{
			if ((sppacDisablePluginMasks[iMask][0] != NULL) && (_tcsstr(sCommandLineUpper.c_str(), sppacDisablePluginMasks[iMask]) != nullptr))
			{
				return (0);
			}
		}
	}

	sInputFile = psInputFile;
	sOutputFile = psOutputFile;
	sCommandLine = psCommandLine;

	//Avoid temporary name collisions across different instances and threads. InterlockedIncrement returns the incremented value
	int iRandom = (int) ((unsigned int) InterlockedIncrement(&miTmpFileSerial) + ((unsigned int) GetCurrentProcessId() << 16) + (unsigned int) GetCurrentThreadId());

	//Use specified option temp directory if exists
	if (gudtOptions.acTempDirectory[0] != NULL)
	{
		//Add final slash if it has not. Guarded because it mutates shared state, and workers run concurrently
		EnterCriticalSection(&mudtCriticalSection);
		if (gudtOptions.acTempDirectory[_tcslen(gudtOptions.acTempDirectory) - 1] != '\\')
		{
			_tcscat(gudtOptions.acTempDirectory, _T("\\"));
		}
		LeaveCriticalSection(&mudtCriticalSection);
		clsUtil::CopyString(acTempPath, sizeof(acTempPath) / sizeof(TCHAR), gudtOptions.acTempDirectory);
	}
	else
	{
		if (!GetTempPath((sizeof(acTempPath) / sizeof(TCHAR)) - 1, acTempPath))
		{
			memset(acTempPath, 0, sizeof(acTempPath));
		}
	}

	//Create temporary directory just in case it did not existed
	if (gudtOptions.acTempDirectory[0] != NULL)
	{
		clsUtil::DirectoryCreate(gudtOptions.acTempDirectory);
	}
	sTmpInputFile = (String) acTempPath + sAppName + "_Input_" + (String) iRandom + "_" + ExtractFileName(sInputFile);

	sTmpOutputFile = (String) acTempPath + sAppName + "_Output_" + (String) iRandom + "_" + ExtractFileName(sInputFile);

	if (!gudtOptions.bDebug)
	{
		clsUtil::DeleteFile(sTmpInputFile.c_str());
		clsUtil::DeleteFile(sTmpOutputFile.c_str());
	}

	//Required indirection
	String sCaption;
	sCaption.printf(_((TCHAR *) _T("Running %s...")), psStatus.c_str());
	EnterCriticalSection(&mudtCriticalSection);
	grdFiles->Cells[KI_GRID_STATUS][(int) piCurrent] = sCaption;
	LeaveCriticalSection(&mudtCriticalSection);

	unsigned long long lSize = clsUtil::SizeFile(sInputFile.c_str());
	unsigned long long lSizeNew = lSize;
	EnterCriticalSection(&mudtCriticalSection);
	grdFiles->Cells[KI_GRID_OPTIMIZED][(int) piCurrent] = FormatNumberThousand(lSize);
	LeaveCriticalSection(&mudtCriticalSection);

	//Only the main thread can process messages: workers would dispatch them in the wrong thread
	if (GetCurrentThreadId() == MainThreadID)
	{
		Application->ProcessMessages();
	}

	bool bTmpOutputFile = (PosEx("%TMPOUTPUTFILE%", psCommandLine) != 0);
	bool bTmpInputFile = (PosEx("%TMPINPUTFILE%", psCommandLine) != 0);
	//Copy the original file only when the plugin really works on the temporary copy: copying it for the ones that use the input file in place (Ghostscript, shntool, flasm...) was a wasted copy of the whole file
	if (bTmpInputFile)
	{
		clsUtil::CopyFile(sInputFile.c_str(), sTmpInputFile.c_str());
		//sInputFile = sTmpOutputFile;
	}

	//sCommandLine = StringReplace(sCommandLine, "%INPUTFILE%", sInputFile, TReplaceFlags() << rfReplaceAll);
	sCommandLine = ReplaceStr(sCommandLine, "%INPUTFILE%", sInputFile);

	//sCommandLine = StringReplace(sCommandLine, "%OUTPUTFILE%", sOutputFile, TReplaceFlags() << rfReplaceAll);
	sCommandLine = ReplaceStr(sCommandLine, "%OUTPUTFILE%", sOutputFile);

	//sCommandLine = StringReplace(sCommandLine, "%TMPINPUTFILE%", sTmpInputFile, TReplaceFlags() << rfReplaceAll);
	sCommandLine = ReplaceStr(sCommandLine, "%TMPINPUTFILE%", sTmpInputFile);

	//sCommandLine = StringReplace(sCommandLine, "%TMPOUTPUTFILE%", sTmpOutputFile, TReplaceFlags() << rfReplaceAll);
	sCommandLine = ReplaceStr(sCommandLine, "%TMPOUTPUTFILE%", sTmpOutputFile);

	//If the plugins path has spaces (short names not available) quote the program, otherwise CreateProcess would try to run things such as C:\Program.exe first
	if ((sPluginsDirectory.Pos(" ") > 0) && (StartsStr(sPluginsDirectory, sCommandLine)))
	{
		int iProgramEnd = PosEx(" ", sCommandLine, sPluginsDirectory.Length() + 1);
		if (iProgramEnd == 0)
		{
			iProgramEnd = sCommandLine.Length() + 1;
		}
		sCommandLine = "\"" + sCommandLine.SubString(1, iProgramEnd - 1) + "\"" + sCommandLine.SubString(iProgramEnd, MaxInt);
	}
	int iError;
	//The timestamps are only used by the log, so they are not queried when logging is disabled (two GetLocalTime calls per plugin saved)
	TDateTime dteStart, dteEnd;
	bool bLogTime = (gudtOptions.iLogLevel > 3);
	if (lSize > 0)
	{
		if (bLogTime)
		{
			dteStart = TDateTime::CurrentDateTime();
		}
		//Maximum run time, so a plugin that hangs does not block its worker forever: the configured seconds (0 = no limit) plus a minute for every MB of the file, since big files legitimately take longer
		unsigned int iTimeout = 0;
		if (gudtOptions.iPluginTimeout > 0)
		{
			unsigned long long lTimeout = (((unsigned long long) gudtOptions.iPluginTimeout) * 1000) + ((lSize >> 20) * 60000);
			iTimeout = (lTimeout < 0xFFFFFFF0ULL) ? (unsigned int) lTimeout : 0xFFFFFFF0U;
		}
		iError = (int) RunProcess(sCommandLine.c_str(), NULL, 0, true, iTimeout);
		if (bLogTime)
		{
			dteEnd = TDateTime::CurrentDateTime();
		}

		//Check exit errorlevel. A plugin killed by Stop leaves a partial output, which must never replace the input file
		if ((iError >= piErrorMin) && (iError <= piErrorMax) && (!gbStop))
		{
			//We did get a TMP output file, so if smaller, make it overwrite input file
			if (bTmpOutputFile)
			{
				lSizeNew = clsUtil::SizeFile(sTmpOutputFile.c_str());
				if ((lSizeNew >= 8) && (lSizeNew < lSize))
				{
					if (!clsUtil::CopyFile(sTmpOutputFile.c_str(), sInputFile.c_str(), true))
					{
						iError = -9998;
						//Nothing was replaced, so do not report the smaller size
						lSizeNew = lSize;
					}
				}
			}
			else if (bTmpInputFile)
			{
				lSizeNew = clsUtil::SizeFile(sTmpInputFile.c_str());
				if ((lSizeNew >= 8) && (lSizeNew < lSize))
				{
					if (!clsUtil::CopyFile(sTmpInputFile.c_str(), sInputFile.c_str(), true))
					{
						iError = -9997;
						//Nothing was replaced, so do not report the smaller size
						lSizeNew = lSize;
					}
					//sInputFile = sTmpOutputFile;
				}
			}
		}
	}
	else
	{
		if (bLogTime)
		{
			dteStart = TDateTime::CurrentDateTime();
			dteEnd = dteStart;
		}
		iError = -9999;
	}

	if (!gudtOptions.bDebug)
	{
		clsUtil::DeleteFile(sTmpInputFile.c_str());
		clsUtil::DeleteFile(sTmpOutputFile.c_str());
	}

	if ((lSizeNew <= 8) || (lSizeNew >= lSize))
	{
		lSizeNew = lSize;
	}
	else
	{
		EnterCriticalSection(&mudtCriticalSection);
		grdFiles->Cells[KI_GRID_OPTIMIZED][(int) piCurrent] = FormatNumberThousand(lSizeNew);
		LeaveCriticalSection(&mudtCriticalSection);
	}

	//Log only writes when log level is higher than 3, so do not even build such a long text otherwise. Guarded so concurrent writes do not get interleaved
	if (gudtOptions.iLogLevel > 3)
	{
		EnterCriticalSection(&mudtCriticalSection);
		clsUtil::Log(3, ("Start: " + dteStart.FormatString("yyyy/mm/dd hh:nn:ss") + "\tEnd: " + dteEnd.FormatString("yyyy/mm/dd hh:nn:ss") + "\tLevel: " + ((String) gudtOptions.iLevel) +  "\tOriginal: " + ((String) lSize) + "\tOptimized: " + ((String) lSizeNew) + "\tErrorlevel: " + ((String) iError) + "\tInput: " + sInputFile + "\tOutput: " + sOutputFile + "\tPlugin: " + psStatus + "\tCommandline: " + sCommandLine + "").c_str(), gudtOptions.iLogLevel);
		LeaveCriticalSection(&mudtCriticalSection);
	}


	return (iError);
}



// ---------------------------------------------------------------------------
void __fastcall TfrmMain::CheckForUpdates(bool pbSilent)
{
	//Owned buffers, so every exit path releases them
	std::unique_ptr<TCHAR[]> oWide(new TCHAR[KI_BUFFER_SIZE]);
	std::unique_ptr<TCHAR[]> oTemp(new TCHAR[KI_BUFFER_SIZE]);
	std::unique_ptr<char[]> oPath(new char[KI_BUFFER_SIZE]);
	std::unique_ptr<char[]> oBuffer(new char[KI_BUFFER_SIZE]);
	TCHAR *acWide = oWide.get();
	TCHAR *acTemp = oTemp.get();
	char *acPath = oPath.get();
	char *acBuffer = oBuffer.get();
	unsigned int iSize;


	acTemp[0] = NULL;
	strcpy(acPath, "ini=");
	memset(acBuffer, 0, KI_BUFFER_SIZE);
	iSize = KI_BUFFER_SIZE - 1024;
	clsUtil::ReadFile(clsUtil::GetIniPath(), (void *) acBuffer, &iSize);
	//Remove cache part
	char *pcCache = strstr(acBuffer, "[Cache]");
	if (pcCache)
	{
		*pcCache = NULL;
	}

	//ToDo: Use UrlEscape https://msdn.microsoft.com/en-us/library/windows/desktop/bb773774(v=vs.85).aspx
	strncpy(acBuffer, ((AnsiString) ReplaceStr(ReplaceStr(ReplaceStr(acBuffer, "\r\n", "%0D"), "=", "%3D"), "\t", "")).c_str(), KI_BUFFER_SIZE - 1);
	strncat(acPath, acBuffer, KI_BUFFER_SIZE - 1 - strlen(acPath));
	//strcat(acPath, "&log=");
	//iSize = 0;
	//clsUtil::ReadFile(clsUtil::GetLogPath(), (void *) acBuffer, &iSize);
	//strcat(acPath, acBuffer);

	//mbstowcs(acWide, acPath, KI_BUFFER_SIZE);
	//if (clsUtil::DownloadFile(acWide, acBuffer, KI_BUFFER_SIZE))
	if (clsUtil::DownloadFilePost(KS_APP_UPDATE_SERVER, KS_APP_UPDATE_PAGE, acPath, acBuffer, KI_BUFFER_SIZE, StartsStr("https://", KS_APP_URL)))
	{
		mbstowcs(acWide, acBuffer, KI_BUFFER_SIZE);
		//Check we only got number and punctuation digits in first characters to detect router errors returning HTML
		size_t iWideLen = min(_tcslen(acWide), 10);
		for (size_t iWide = 0; iWide < iWideLen; iWide++)
		{
			if ((!_istdigit(acWide[iWide])) && (!_istpunct(acWide[iWide]) && (acWide[iWide] != ' ')))
			{
				if (!pbSilent)
				{
					clsUtil::MsgBox(Handle, _((TCHAR *) _T("Error checking for updates.")), _((TCHAR *) _T("Check updates")), MB_OK | MB_ICONERROR);
				}
				return;
			}
		}

		gudtOptions.iAdsShown = 0;

		if (GetModuleFileName(NULL, (TCHAR *) acPath, KI_BUFFER_SIZE / sizeof(TCHAR)) != 0)
		{
			clsUtil::CopyString(acTemp, KI_BUFFER_SIZE, clsUtil::ExeVersion((TCHAR *) acPath));
		}
		_stprintf((TCHAR *) acBuffer, _T("%10s"), (TCHAR *) acTemp);

		clsUtil::CopyString(acTemp, KI_BUFFER_SIZE, acWide);
		//The server can append donation details to the version in the form "version (donator)", so they must be split before comparing, or donators would always be told a new version is available
		TCHAR *pcDonation = _tcsstr(acTemp, _T(" ("));
		if (pcDonation != NULL)
		{
			clsUtil::CopyString(gudtOptions.acDonation, sizeof(gudtOptions.acDonation) / sizeof(TCHAR), pcDonation + 2);
			StrTrim(gudtOptions.acDonation, _T(" ()"));
			*pcDonation = NULL;
		}
		else
		{
			gudtOptions.acDonator[0] = NULL;
			gudtOptions.acDonation[0] = NULL;
			if ((gudtOptions.iStatOpens > 10) && (clsUtil::Random(0, 50) == 5))
			{
				String sCaption;
				sCaption.printf(_((TCHAR *) _T("Thank you for using my program!\n\nYou have used it %s times and have optimized %s files.\n\nYou can continue using it free of charge.\n\nIf you are happy, please contribute to the active development by donating via Paypal. It is secure, safe and convenient.\n\nDonators will receive priority support and consultancy, while those cannot be guaranteed to non-donors.")), FormatNumberThousand(gudtOptions.iStatOpens).c_str(), FormatNumberThousand(gudtOptions.iStatFiles).c_str());
				clsUtil::MsgBox(Handle, sCaption.c_str(), (_("Thank you for using ") + Application->Name).c_str(), MB_OK|MB_ICONEXCLAMATION, 60000);
				actDonateExecute(NULL);
			}
		}
		_stprintf(acWide, _T("%10s"), (TCHAR *) acTemp);

		if (_tcsncmp(acWide, (TCHAR *) acBuffer, 10) > 0)
		{
			String sCaption;
			sCaption.printf(_((TCHAR *) _T("%s version %s is available.\r\nDo you want to download it now?")), Application->Name.c_str(), Trim(acWide).c_str());
			if (clsUtil::MsgBox(Handle, sCaption.c_str(), _((TCHAR *) _T("Check updates")), MB_YESNO | MB_ICONQUESTION) == ID_YES)
			{
				ShellExecute(Handle, _T("open"), KS_APP_UPDATE_URL, _T(""), _T(""), SW_SHOWNORMAL);
			}
		}
		else if (!pbSilent)
		{
			String sCaption;
			sCaption.printf(_((TCHAR *) _T("You already have latest %s? version.")), Application->Name.c_str());
			clsUtil::MsgBox(Handle, sCaption.c_str(), _((TCHAR *) _T("Check updates")), MB_OK|MB_ICONINFORMATION);
		}
	}
	else if (!pbSilent)
	{
		clsUtil::MsgBox(Handle, _((TCHAR *) _T("Error checking for updates.")), _((TCHAR *) _T("Check updates")), MB_OK | MB_ICONERROR);
	}
}




//---------------------------------------------------------------------------
String __fastcall TfrmMain::GetExtensionByContent (const String psFilename, bool pbForce)
{
	String sRes;
	unsigned char acBuffer[512 * 2] = {};
	static const String sExtensionAll = KS_EXTENSION_ALL;


	sRes = ExtractFileExt(psFilename).LowerCase();
	//Fixe ExtractFileExt returning NULL when no extension instead of empty.
	if (sRes.IsEmpty())
	{
		sRes = ".";
	}


	//If file extension is not known, get it by analyzing file contents
	String sExtensionFind = " " + sRes + " ";
	if (((PosEx(sExtensionFind, sExtensionAll) == 0) && ((gudtOptions.acJSAdditionalExtensions[0] == NULL) || (PosEx(sExtensionFind, " " + ReplaceStr((String) gudtOptions.acJSAdditionalExtensions, ",", " ") + " ") == 0))) || (pbForce))
	{
		//memset(acBuffer, 0, sizeof(acBuffer));
		bool bMapped;
		bool bFilled = false;

		//Map the file once, so the header and the footer can be copied from the same view, and only the touched pages are ever read from the disk
		unsigned long long lFileSize = clsUtil::SizeFile(psFilename.c_str());
		unsigned int iSize = 0;
		void *pvFile = clsUtil::MapFileView(psFilename.c_str(), &iSize, &bMapped);
		if (pvFile != nullptr)
		{
			const unsigned char *pacFileData = (const unsigned char *) pvFile;
			try
			{
				//Copy footer, only if file is big enough and the whole file is available in the view (otherwise the offset would be wrong)
				if ((lFileSize >= 512) && (lFileSize <= UINT_MAX) && (iSize >= (unsigned int) lFileSize))
				{
					memcpy(&acBuffer[512], pacFileData + (unsigned int) lFileSize - 512, 512);
				}
				//Copy header
				memcpy(acBuffer, pacFileData, ((iSize < 512) ? iSize : 512));
				bFilled = true;
			}
			catch (...)
			{
				//I/O error while reading the mapped pages (the file was truncated by another program, a network share was lost...): it is treated as not matching
			}
			clsUtil::UnmapFileView(pvFile, bMapped);
		}
		else
		{
			//Mapping was not possible (such as the whole view of a huge file on 32 bit): read the header and the footer with two bounded reads
			unsigned int iRead = sizeof(acBuffer) >> 1;
			if ((lFileSize >= 512) && (lFileSize <= UINT_MAX))
			{
				clsUtil::ReadFile(psFilename.c_str(), &acBuffer[512], &iRead, (unsigned int) lFileSize - 512);
			}
			iRead = sizeof(acBuffer) >> 1;
			if (clsUtil::ReadFile(psFilename.c_str(), acBuffer, &iRead))
			{
				bFilled = true;
			}
		}
		if (bFilled)
		{
			//ToDo: Optimize to use regular comparisons instead of memcmp for short comparisons.
			//Check AVIF
			if ((memcmp(&acBuffer[4], "ftyp", 4) == 0) &&
				((memcmp(&acBuffer[8], "avif", 4) == 0) || (memcmp(&acBuffer[8], "avis", 4) == 0) ||
				 (memcmp(&acBuffer[8], "avci", 4) == 0) || (memcmp(&acBuffer[8], "avcs", 4) == 0) ||
				 (memcmp(&acBuffer[8], "heic", 4) == 0) || (memcmp(&acBuffer[8], "heix", 4) == 0) ||
				 (memcmp(&acBuffer[8], "hevc", 4) == 0) || (memcmp(&acBuffer[8], "hevx", 4) == 0) ||
				 (memcmp(&acBuffer[8], "mif1", 4) == 0) || (memcmp(&acBuffer[8], "msf1", 4) == 0)))
			{
				sRes = ".avif";
			}
			//Check BMP
			else if ((memcmp(acBuffer, "BM", 2) == 0) || (memcmp(acBuffer, "BA", 2) == 0) || (memcmp(acBuffer, "CI", 2) == 0) || (memcmp(acBuffer, "CP", 2) == 0) || (memcmp(acBuffer, "IC", 2) == 0) || (memcmp(acBuffer, "PT", 2) == 0))
			{
				sRes = ".bmp";
			}
			//Check EXE
			//Check DLL
			else if ((memcmp(acBuffer, "MZ", 2) == 0) || (memcmp(acBuffer, "ZM", 2) == 0))
			{
				sRes = ".dll";
			}
			//Check FLAC
			else if (memcmp(acBuffer, "fLaC", 4) == 0)
			{
				sRes = ".flac";
			}
			//Check GIF
			else if (memcmp(acBuffer, "GIF8", 4) == 0)
			{
				sRes = ".gif";
			}
			//Check GZ
			else if (memcmp(acBuffer, "\x1F\x8B\x08", 3) == 0)
			{
				sRes = ".gz";
			}
			//Check ICO
			else if (memcmp(acBuffer, "\x00\x00\x01\x00", 4) == 0)
			{
				sRes = ".ico";
			}
			//Check JPEG 2000
			else if (memcmp(&acBuffer[4], "jP", 2) == 0)
			{
				sRes = ".jp2";
			}
			//Check JPEG
			else if (memcmp(acBuffer, "\xFF\xD8\xFF", 3) == 0)
			{
				sRes = ".jpg";
			}
			//Check JPEG XL
			else if (memcmp(acBuffer, "\x00\x00\x00\x0C\x4A\x58\x4C\x20\x0D\x0A\x87\x0A", 12) == 0)
			{
				sRes = ".jxl";
			}
			//Check MKV
			else if (memcmp(acBuffer, "\x1A\x45\xDF\xA3", 4) == 0)
			{
				sRes = ".mkv";
			}
			//ToDo: MIME
			//Check MNG
			else if (memcmp(acBuffer, "\x8A\x4D\x4E\x47\x0D\x0A\x1A\x0A", 8) == 0)
			{
				sRes = ".mng";
			}
			//Check MP3
			else if (memcmp(acBuffer, "ID3", 3) == 0)
			{
				sRes = ".mp3";
			}
			//Check MP4
			else if (memcmp(&acBuffer[4], "ftyp", 4) == 0)
			{
				sRes = ".mp4";
			}
			//Check OBJ
			else if ((memcmp(acBuffer, "\x4C\x01", 2) == 0) ||
				(acBuffer[0] == 0x80))
			{
				sRes = ".obj";
			}
			//Check OGG / Check OGV
			else if (memcmp(acBuffer, "OggS", 4) == 0)
			{
				sRes = ".ogg";
			}
			//Check OLE/OLE Beta
			else if ((memcmp(acBuffer, "\xD0\xCF\x11\xE0\xA1\xB1\x1A\xE1", 8) == 0) ||
					(memcmp(acBuffer, "\x0E\x11\xFC\x0D\xD0\xCF\x11\x0E", 8) == 0))
			{
				sRes = ".ole";
			}
			//Check PCX
			else if ((acBuffer[0] == 10) && (acBuffer[2] == 1) && (acBuffer[64] == 0) && (acBuffer[74] == 0))
			{
				sRes = ".pcx";
			}
			//Check PDF
			else if (memcmp(acBuffer, "%PDF-", 5) == 0)
			{
				sRes = ".pdf";
			}
			//Check PNG
			else if (memcmp(acBuffer, "\x89\x50\x4E\x47\x0D\x0A\x1A\x0A", 8) == 0)
			{
				sRes = ".png";
			}
			//Check SQLITE
			else if (memcmp(acBuffer, "SQLite format 3", 16) == 0)
			{
				sRes = ".sqlite";
			}
			//Check SWF
			else if ((memcmp(acBuffer, "FWS", 3) == 0) || (memcmp(acBuffer, "CWS", 3) == 0) || (memcmp(acBuffer, "ZWS", 3) == 0))
			{
				sRes = ".swf";
			}
			//Check TAR
			else if (memcmp(&acBuffer[257], "\x75\x73\x74\x61\x72", 5) == 0)
			{
				sRes = ".tar";
			}
			//Check TGA
			else if (memcmp(&acBuffer[512 * 2 - 18], "TRUEVISION", 10) == 0)
			{
				sRes = ".tga";
			}
			//Check TIFF
			else if ((memcmp(acBuffer, "\x0C\xED", 2) == 0) || (memcmp(acBuffer, "\x49\x20\x49", 3) == 0) || (memcmp(acBuffer, "\x49\x49\x2A\x00", 4) == 0) || (memcmp(acBuffer, "\x4D\x4D\x00\x2B", 4) == 0))
			{
				sRes = ".tif";
			}
			//ToDo: Tencent QQ
			//Check WAV and WEBP, both are RIFF containers (WEBP signature is at offset 8)
			else if (memcmp(acBuffer, "RIFF", 4) == 0)
			{
				if (memcmp(&acBuffer[8], "WEBP", 4) == 0)
				{
					sRes = ".webp";
				}
				else
				{
					sRes = ".wav";
				}
			}
			//Check ZIP
			else if (memcmp(acBuffer, "\x50\x4B\x03\x04", 4) == 0)
			{
				sRes = ".zip";
			}
			//Check CAB
			else if (memcmp(acBuffer, "MSCF", 4) == 0)
			{
				sRes = ".cab";
			}			
			//Check 7z
			else if (memcmp(acBuffer, "\x37\x7A\xBC\xAF\x27\x1C", 6) == 0)
			{
				sRes = ".7z";
			}
			//Unsupported extension
			else
			{
				sRes = "";
			}
		}
		else
		{
			sRes = "";
		}
	}
	return (sRes);
}




//---------------------------------------------------------------------------
String __inline TfrmMain::FormatNumberThousand (unsigned long long plNumber)
{
	//return (FloatToStrF(plNumber, (TFloatFormat) ffNumber, 18, 0));
	TCHAR acNumber[64];
	TCHAR acFormatedNumber[64];
	TCHAR *pcDecimal;


	_ui64tot(plNumber, acNumber, 10);
	GetNumberFormat(LOCALE_USER_DEFAULT, NULL, acNumber, NULL, acFormatedNumber, (sizeof(acFormatedNumber) / sizeof(TCHAR)) - 1);
	//Remove Decimals
	pcDecimal = _tcsrchr(acFormatedNumber, FormatSettings.DecimalSeparator);
	if (pcDecimal)
	{
		*pcDecimal = NULL;
	}
	return ((String) acFormatedNumber);
}


//---------------------------------------------------------------------------
String __inline TfrmMain::FormatNumberThousandUnit (unsigned long long plNumber)
{
	TCHAR acRes[64];


	StrFormatByteSize64(plNumber, acRes, (sizeof(acRes) / sizeof(TCHAR)) - 1);
	return (String(acRes));
}




//---------------------------------------------------------------------------
unsigned long long __inline TfrmMain::ParseNumberThousand (String psNumber)
{
	const TCHAR *acNumber = psNumber.c_str();
	unsigned long long lRes = 0;


	//Only digits matter, so the thousands separators are ignored
	for (; *acNumber != NULL; acNumber++)
	{
		if ((*acNumber >= '0') && (*acNumber <= '9'))
		{
			lRes = (lRes * 10) + (*acNumber - '0');
		}
	}
	return (lRes);
}



// ---------------------------------------------------------------------------------------------------------------------------------------------------------------------------
unsigned long __fastcall TfrmMain::RunProcess(const TCHAR *pacProcess, TCHAR *pacOutput, unsigned int piOutputLen, bool pbWait, unsigned int piTimeout)
{
	unsigned long lExitCode = (unsigned long) -1;
	unsigned long lRead = 0;
	//One byte of the output buffer is never used, so the captured output is always NUL terminated
	unsigned int iOutputMax = (piOutputLen > 0) ? (piOutputLen - 1) : 0;
	STARTUPINFO udtSI = {};
	PROCESS_INFORMATION udtPI = {};
	SECURITY_ATTRIBUTES udtSA;
	HANDLE hOutRead = NULL, hOutWrite = NULL;
	HANDLE hInRead = NULL, hInWrite = NULL;


	if (gbStop)
	{
		return (0);
	}

	//Only the main thread can safely change the screen cursor
	if (GetCurrentThreadId() == MainThreadID)
	{
		Screen->Cursor = crAppStart;
	}

	//CreateProcess can modify the command line while parsing, so it gets a private writable copy: the caller passes the buffer of a reference counted String. Same fix clsUtil::RunProcess got
	const size_t iCmdLen = _tcslen(pacProcess) + 1;
	TCHAR *pacCmdLine = new TCHAR[iCmdLen];
	_tcscpy(pacCmdLine, pacProcess);

	//Disable Windows error handling. Do not use GetErrorMode because it requires Vista or later.
	SetErrorMode(SEM_FAILCRITICALERRORS | SEM_NOALIGNMENTFAULTEXCEPT | SEM_NOGPFAULTERRORBOX | SEM_NOOPENFILEERRORBOX);

	udtSA.nLength = sizeof(udtSA);
	udtSA.bInheritHandle = true;
	udtSA.lpSecurityDescriptor = NULL;

	udtSI.cb = sizeof(udtSI);
	udtSI.dwFlags = STARTF_USESHOWWINDOW;
	udtSI.wShowWindow = SW_HIDE;

	//Capture the child output through a dedicated pipe. A separate stdin pipe is used so the child never sees its own stdout on stdin (the previous reuse of the output read end mixed the two streams for any plugin that both writes and reads).
	if ((pacOutput != NULL) && (iOutputMax > 0) && (CreatePipe(&hOutRead, &hOutWrite, &udtSA, piOutputLen)))
	{
		//Provide an empty stdin (EOF) via a second pipe; the write end is closed right after CreateProcess
		if (!CreatePipe(&hInRead, &hInWrite, &udtSA, 0))
		{
			hInRead = NULL;
			hInWrite = NULL;
		}
		if (hInRead != NULL)
		{
			//The ends kept by the parent must not be inherited by the child, or it would never see the end of its input
			SetHandleInformation(hOutRead, HANDLE_FLAG_INHERIT, 0);
			SetHandleInformation(hInWrite, HANDLE_FLAG_INHERIT, 0);
		}
		udtSI.dwFlags = STARTF_USESTDHANDLES | STARTF_USESHOWWINDOW;
		udtSI.hStdInput = (hInRead != NULL) ? hInRead : hOutRead;	//fallback only if stdin pipe failed
		udtSI.hStdOutput = hOutWrite;
		//The child also gets the write end as its standard error, or its error output would go to an invalid handle (and capturing it helps debugging plugin failures)
		udtSI.hStdError = hOutWrite;
		//The output buffer is zeroed in advance, and its last byte is never written
		memset(pacOutput, 0, piOutputLen);
	}

	//Handles are only inherited when the output is captured. The process and thread handles are never inheritable, or the plugins that other workers launch at the same time would keep them alive
	if (!CreateProcess(NULL, pacCmdLine, NULL, NULL, (hOutWrite != NULL), ((unsigned long) gudtOptions.iProcessPriority) | CREATE_NO_WINDOW, NULL, NULL /*(TCHAR *) pacDirectory*/, &udtSI, &udtPI))
	{
		//_stprintf(acTmp, _T("Cannot launch %s"), pacProcess);
	}

	//Close the write ends in the parent immediately: the child then sees EOF on stdin and the output pipe can signal end-of-data without deadlock
	if (hOutWrite != NULL)
	{
		CloseHandle(hOutWrite);
		hOutWrite = NULL;
	}
	if (hInWrite != NULL)
	{
		CloseHandle(hInWrite);
		hInWrite = NULL;
	}

	if ((pbWait) && (udtPI.hProcess))
	{
		bool bExited = false;
		unsigned int iWaitStart = GetTickCount();
		do
		{
			//Only the main thread can process messages: workers would dispatch them in the wrong thread
			if (GetCurrentThreadId() == MainThreadID)
			{
				Application->ProcessMessages();
			}
			if (gbStop)
			{
				//Not exit code 0, which would look like a successful run
				TerminateProcess(udtPI.hProcess, (unsigned int) -1);
			}
			else if ((piTimeout > 0) && ((GetTickCount() - iWaitStart) >= piTimeout))
			{
				//The plugin exceeded its maximum run time, so it is hung: kill it, and its worker goes on with the next file. Not exit code 0 either, and the caller is told so it never uses the partial output
				TerminateProcess(udtPI.hProcess, (unsigned int) -1);
				mbPluginTimedOut = true;
				piTimeout = 0;
			}
			//Drain the pipe while waiting, or a child writing more than the pipe capacity would block forever, deadlocking the parent too
			if (lRead < iOutputMax)
			{
				lRead += clsUtil::ReadPipeAvailable(hOutRead, (void *) (((unsigned char *) pacOutput) + lRead), iOutputMax - (unsigned int) lRead);
			}
			else
			{
				//The output buffer is full, so drain and discard the rest, or the child would block the same way
				unsigned char acDiscard[1024];
				clsUtil::ReadPipeAvailable(hOutRead, acDiscard, sizeof(acDiscard));
			}
			//Give the process a 100 ms time slice. Any result other than timeout means it exited (or the wait failed, and the loop would never end otherwise). The exit code is not checked to exit the loop: a child legitimately returning STILL_ACTIVE (259) would loop forever
			//The main thread also wakes up as soon as a message arrives (the next loop pumps it), so the interface does not wait up to 100 ms to react while the plugin runs. Workers have no messages
			unsigned long lWait = (GetCurrentThreadId() == MainThreadID) ? MsgWaitForMultipleObjects(1, &udtPI.hProcess, false, 100, QS_ALLINPUT) : WaitForSingleObject(udtPI.hProcess, 100);
			if ((lWait != WAIT_TIMEOUT) && (lWait != (WAIT_OBJECT_0 + 1)))
			{
				bExited = true;
			}
		}
		while (!bExited);
		GetExitCodeProcess(udtPI.hProcess, &lExitCode);
	}
	else if (udtPI.hProcess)
	{
		GetExitCodeProcess(udtPI.hProcess, &lExitCode);
	}

	if (hInRead != NULL)
	{
		CloseHandle(hInRead);
	}

	//Read what is left in the pipe, since the child could have exited before the last drain
	if (hOutRead != NULL)
	{
		if (lRead < iOutputMax)
		{
			clsUtil::ReadPipeAvailable(hOutRead, (void *) (((unsigned char *) pacOutput) + lRead), iOutputMax - (unsigned int) lRead);
		}
		CloseHandle(hOutRead);
	}

	if (udtPI.hProcess)
	{
		CloseHandle(udtPI.hProcess);
		CloseHandle(udtPI.hThread);
	}

	//The error mode is process wide, so it is restored when the optimization ends, not here, where other workers could be launching plugins

	//Only the main thread can safely change the screen cursor
	if (GetCurrentThreadId() == MainThreadID)
	{
		Screen->Cursor = crDefault;
	}

	delete[] pacCmdLine;
	return (lExitCode);
}



// ---------------------------------------------------------------------------------------------------------------------------------------------------------------------------
bool __fastcall TfrmMain::IsAPNG(const TCHAR *pacFile)
{
	#pragma pack(push, 1)
	unsigned char *acBuffer;
	struct
	{
		unsigned char acSignature[8];
	} udtPNGHeader;
	struct
	{
		unsigned int iLength;
		unsigned char acType[4];
	} udtPNGChunk;
	#pragma pack(pop)


	bool bRes = false;
	//The animation control chunks are always at the beginning of the file, so mapping the first megabyte is enough (allocating whole files wasted memory and time, and also truncated sizes over 4 GB). Mapping also avoids the allocation and the copy of a buffer per file, and only the scanned pages are ever read from the disk
	unsigned long long lFileSize = clsUtil::SizeFile(pacFile);
	unsigned int iSize = (lFileSize < (1 * 1024 * 1024)) ? (unsigned int) lFileSize : (1 * 1024 * 1024);
	if (iSize > sizeof(udtPNGHeader))
	{
		bool bMapped;
		acBuffer = (unsigned char *) clsUtil::MapFileView(pacFile, &iSize, &bMapped);
		if (acBuffer != nullptr)
		{
			try
			{
				if (iSize > sizeof(udtPNGHeader))
				{
					unsigned int iPos = 0;
					memcpy(&udtPNGHeader, &acBuffer[iPos], sizeof(udtPNGHeader));
					iPos += sizeof(udtPNGHeader);
					if (memcmp(udtPNGHeader.acSignature, "\x89\x50\x4E\x47\x0D\x0A\x1A\x0A", sizeof(udtPNGHeader.acSignature)) == 0)
					{
						while ((iPos + sizeof(udtPNGChunk)) <= iSize)
						{
							memcpy(&udtPNGChunk, &acBuffer[iPos], sizeof(udtPNGChunk));
							iPos += sizeof(udtPNGChunk);
							udtPNGChunk.iLength = ((udtPNGChunk.iLength >> 24) & 0xFF) | ((udtPNGChunk.iLength << 8) & 0xFF0000) | ((udtPNGChunk.iLength >> 8) & 0xFF00) | ((udtPNGChunk.iLength << 24) & 0xFF000000);
							if ((memcmp(udtPNGChunk.acType, "fcTL", sizeof(udtPNGChunk.acType)) == 0) || (memcmp(udtPNGChunk.acType, "acTL", sizeof(udtPNGChunk.acType)) == 0) || (memcmp(udtPNGChunk.acType, "fdAT", sizeof(udtPNGChunk.acType)) == 0))
							{
								bRes = true;
								break;
							}
							//Animation chunks always precede the image data, so there is no need to look any further
							if (memcmp(udtPNGChunk.acType, "IDAT", sizeof(udtPNGChunk.acType)) == 0)
							{
								break;
							}
							//Corrupted chunk length, stop looking
							if (udtPNGChunk.iLength > (iSize - iPos))
							{
								break;
							}
							iPos += udtPNGChunk.iLength + 4;
						}
					}
				}
			}
			catch (...)
			{
				//I/O error while reading the mapped pages (the file was truncated by another program, a network share was lost...): it is treated as not matching
				bRes = false;
			}
			clsUtil::UnmapFileView(acBuffer, bMapped);
		}
	}
	return (bRes);
}


// ---------------------------------------------------------------------------------------------------------------------------------------------------------------------------
bool __fastcall TfrmMain::IsEXESFX(const TCHAR *pacFile)
{
	bool bRes = false;
	unsigned char *acBuffer;

	
	//Read only 2 bytes first, to avoid reading 1 MB on files that are not executables
	unsigned char acMagic[2] = {};
	unsigned int iMagic = sizeof(acMagic);
	if ((!clsUtil::ReadFile(pacFile, acMagic, &iMagic)) || (iMagic < 2) || ((memcmp(acMagic, "MZ", 2) != 0) && (memcmp(acMagic, "ZM", 2) != 0)))
	{
		return (false);
	}

	unsigned int iSize = 1 * 1024 * 1024;
	bool bMapped;
	//Map the beginning of the file instead of allocating and copying it, so only the scanned pages are ever read from the disk. Only the available size is scanned (iSize is 0 when nothing was mapped)
	acBuffer = (unsigned char *) clsUtil::MapFileView(pacFile, &iSize, &bMapped);
	if (acBuffer != nullptr)
	{
		
		try
		{
			//Check if it is EXE
			if ((iSize >= 2) && ((memcmp(acBuffer, "MZ", 2) == 0) || (memcmp(acBuffer, "ZM", 2) == 0)))
			{
				//Check if it is an Inno Setup Installer
				if (clsUtil::MemMem((const void *) acBuffer, iSize, (const void *) "Inno Setup", 10) != NULL)
				{
					bRes = true;
				}
				//Check if it is an InstallShield Wizard
				else if (clsUtil::MemMem((const void *) acBuffer, iSize, (const void *) "InstallShield", 13) != NULL)
				{
					bRes = true;
				}
				//Check if it is an NSIS
				else if (clsUtil::MemMem((const void *) acBuffer, iSize, (const void *) "Nullsoft Install System", 23) != NULL)
				{
					bRes = true;
				}
				//Check if it is an RTPatch Updater
				else if (clsUtil::MemMem((const void *) acBuffer, iSize, (const void *) "RTPatch", 7) != NULL)
				{
					bRes = true;
				}
				//Check if it is a RAR SFX
				else if (clsUtil::MemMem((const void *) acBuffer, iSize, (const void *) "\x52\x61\x72\x21\x1A\x07", 6) != NULL)
				{
					bRes = true;
				}
				//Check if it is a ZIP SFX
				else if (clsUtil::MemMem((const void *) acBuffer, iSize, (const void *) "\x50\x4B\x03\x04", 4) != NULL)
				{
					bRes = true;
				}
				//Check if it is a 7-ZIP SFX
				else if (clsUtil::MemMem((const void *) acBuffer, iSize, (const void *) "\x37\x7A\xBC\xAF\x27\x1C", 6) != NULL)
				{
					bRes = true;
				}
				//Check if it is a SFXCAB
				else if (clsUtil::MemMem((const void *) acBuffer, iSize, (const void *) "MSCF", 4) != NULL)
				{
					bRes = true;
				}
			}
		}
		catch (...)
		{
			//I/O error while reading the mapped pages (the file was truncated by another program, a network share was lost...): it is treated as not matching
			bRes = false;
		}
		clsUtil::UnmapFileView(acBuffer, bMapped);
	}
	return (bRes);
}


// ---------------------------------------------------------------------------------------------------------------------------------------------------------------------------
bool __fastcall TfrmMain::IsEXEManagedNet(const TCHAR *pacFile)
{
	bool bRes = false; //variable that indicates if managed or not.
	unsigned long long lFileSize = clsUtil::SizeFile(pacFile);
	//GetApiName supports paths beyond MAX_PATH, and the file size allows validating the headers before using them
	HANDLE hFile = CreateFile(clsUtil::GetApiName(pacFile).c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, NULL, OPEN_EXISTING, FILE_FLAG_SEQUENTIAL_SCAN, NULL);
	if (hFile != INVALID_HANDLE_VALUE)
	{
		//succeeded
		HANDLE hOpenFileMapping = CreateFileMapping(hFile, NULL, PAGE_READONLY, 0, 0, NULL);
		if (hOpenFileMapping)
		{
			//Map the file , so it can be simply be acted on as a contiguous array of unsigned chars
			unsigned char* pacBaseAddress = (unsigned char*) MapViewOfFile(hOpenFileMapping, FILE_MAP_READ, 0, 0, 0);
			if (pacBaseAddress)
			{
				try
				{
					//having mapped the executable ,now start navigating through the sections
					//DOS header is straightforward. It is the topmost structure in the PE file
					//i.e. the one at the lowest offset into the file
					IMAGE_DOS_HEADER* udtDOSHeader = (IMAGE_DOS_HEADER*) pacBaseAddress;
					//Do not trust the headers: on a damaged executable e_lfanew can point outside the file, and dereferencing it would crash. All offsets are validated against the file size before use
					if ((lFileSize >= sizeof(IMAGE_DOS_HEADER)) && (udtDOSHeader->e_magic == IMAGE_DOS_SIGNATURE) && (udtDOSHeader->e_lfanew > 0) && (((unsigned long long) udtDOSHeader->e_lfanew + sizeof(IMAGE_NT_HEADERS)) <= lFileSize))
					{
						//the only important data in the DOS header is the e_lfanew
						//the e_lfanew points to the offset of the beginning of NT Headers data
						IMAGE_NT_HEADERS* udtNTHeaders = (IMAGE_NT_HEADERS*) ((unsigned char*) udtDOSHeader + udtDOSHeader->e_lfanew);
						//store the section header for future use. This will later be need to check to see if metadata lies within
						//the area as indicated by the section headers
						//The optional header differs between 32 bit (PE32) and 64 bit (PE32+) files whatever the architecture of this program is, so the section table is located using the real size of the optional header stored in the file header
						IMAGE_SECTION_HEADER* udtSectionHeader = IMAGE_FIRST_SECTION(udtNTHeaders);
						//Now, start parsing
						//check if it is a PE file first of all. All assemblies are PE files. The optional header and the section headers table must also fit inside the file, or walking them would read past its end
						if ((udtNTHeaders->Signature == IMAGE_NT_SIGNATURE) && (((unsigned long long) udtDOSHeader->e_lfanew + offsetof(IMAGE_NT_HEADERS, OptionalHeader) + udtNTHeaders->FileHeader.SizeOfOptionalHeader + ((unsigned long long) udtNTHeaders->FileHeader.NumberOfSections * sizeof(IMAGE_SECTION_HEADER))) <= lFileSize))
						{
							//start parsing COM table ( this is what points to the metadata and other information ). It is the 15th data directory, at a different offset in PE32 and PE32+
							unsigned int iNETHeaderTableLocation = 0;
							unsigned int iOptionalSize = udtNTHeaders->FileHeader.SizeOfOptionalHeader;
							if (udtNTHeaders->OptionalHeader.Magic == IMAGE_NT_OPTIONAL_HDR32_MAGIC)
							{
								IMAGE_OPTIONAL_HEADER32* udtOptional32 = (IMAGE_OPTIONAL_HEADER32*) &udtNTHeaders->OptionalHeader;
								if ((iOptionalSize >= (offsetof(IMAGE_OPTIONAL_HEADER32, DataDirectory) + ((IMAGE_DIRECTORY_ENTRY_COM_DESCRIPTOR + 1) * sizeof(IMAGE_DATA_DIRECTORY)))) && (udtOptional32->NumberOfRvaAndSizes > IMAGE_DIRECTORY_ENTRY_COM_DESCRIPTOR))
								{
									iNETHeaderTableLocation = udtOptional32->DataDirectory[IMAGE_DIRECTORY_ENTRY_COM_DESCRIPTOR].VirtualAddress;
								}
							}
							else if (udtNTHeaders->OptionalHeader.Magic == IMAGE_NT_OPTIONAL_HDR64_MAGIC)
							{
								IMAGE_OPTIONAL_HEADER64* udtOptional64 = (IMAGE_OPTIONAL_HEADER64*) &udtNTHeaders->OptionalHeader;
								if ((iOptionalSize >= (offsetof(IMAGE_OPTIONAL_HEADER64, DataDirectory) + ((IMAGE_DIRECTORY_ENTRY_COM_DESCRIPTOR + 1) * sizeof(IMAGE_DATA_DIRECTORY)))) && (udtOptional64->NumberOfRvaAndSizes > IMAGE_DIRECTORY_ENTRY_COM_DESCRIPTOR))
								{
									iNETHeaderTableLocation = udtOptional64->DataDirectory[IMAGE_DIRECTORY_ENTRY_COM_DESCRIPTOR].VirtualAddress;
								}
							}
							if (iNETHeaderTableLocation)
							{
								//.Net header data does exist for this module, find it's location in one of the sections
								unsigned int iActualAddressFromRVA = 0;
								unsigned int iSections = udtNTHeaders->FileHeader.NumberOfSections;
								for (unsigned int iSection = 0; iSection < iSections; iSection++, udtSectionHeader++)
								{
									unsigned int iMaxOnDisk = min(udtSectionHeader->Misc.VirtualSize, udtSectionHeader->SizeOfRawData);
									unsigned int iStartSectRVA = udtSectionHeader->VirtualAddress;
									unsigned int iEndSectRVA = iStartSectRVA + iMaxOnDisk;
									if ((iNETHeaderTableLocation >= iStartSectRVA) && (iNETHeaderTableLocation < iEndSectRVA))
									{
										iActualAddressFromRVA = (udtSectionHeader->PointerToRawData) + (iNETHeaderTableLocation - iStartSectRVA);
										break;
									}
								}
								if (iActualAddressFromRVA)
								{
									//valid address obtained. Suffices to say , this is good enough to identify this as a
									//valid managed component
									bRes = true;
								}
							}
						}
					}
				}
				catch (...)
				{
					//I/O error while reading the mapped pages (the file was truncated by another program, a network share was lost...): it is treated as not matching
					bRes = false;
				}
				//cleanup
				UnmapViewOfFile(pacBaseAddress);
			}
			//cleanup
			CloseHandle(hOpenFileMapping);
		}
		//cleanup
		CloseHandle(hFile);
	}
	return (bRes);
}


// ---------------------------------------------------------------------------------------------------------------------------------------------------------------------------
bool __fastcall TfrmMain::IsPDFLayered(const TCHAR *pacFile)
{
	bool bRes = false;
	unsigned char *acBuffer;

	
	unsigned int iSize = 512 * 1024;
	bool bMapped;
	//Map the beginning of the file instead of allocating and copying it, so only the scanned pages are ever read from the disk. Only the available size is searched (iSize is 0 when nothing was mapped)
	acBuffer = (unsigned char *) clsUtil::MapFileView(pacFile, &iSize, &bMapped);
	if (acBuffer != nullptr)
	{
		try
		{
			//Look for a OCG (Optional Content Groups)
			bRes = (clsUtil::MemMem(acBuffer, iSize, "<< /Type /OCG /Name", 19) != NULL);
		}
		catch (...)
		{
			//I/O error while reading the mapped pages (the file was truncated by another program, a network share was lost...): it is treated as not matching
			bRes = false;
		}
		clsUtil::UnmapFileView(acBuffer, bMapped);
	}
	return (bRes);
}


// ---------------------------------------------------------------------------------------------------------------------------------------------------------------------------
bool __fastcall TfrmMain::IsJPEGCMYK(const TCHAR *pacFile)
{
	bool bRes = false;
	unsigned char *acBuffer;

	
	unsigned int iSize = 512 * 1024;
	bool bMapped;
	//Map the beginning of the file instead of allocating and copying it, so only the scanned pages are ever read from the disk
	acBuffer = (unsigned char *) clsUtil::MapFileView(pacFile, &iSize, &bMapped);
	if (acBuffer != nullptr)
	{
		try
		{
			if ((iSize >= 4) && (acBuffer[0] == 0xFF) && (acBuffer[1] == 0xD8))
			{
				//Walk the segments until SOF (Start Of Frame) and check the number of components (4 = CMYK)
				unsigned int iPos = 2;
				while ((iPos + 4) <= iSize)
				{
					if (acBuffer[iPos] != 0xFF)
					{
						break;
					}
					unsigned char cMarker = acBuffer[iPos + 1];
					//Fill byte
					if (cMarker == 0xFF)
					{
						iPos++;
						continue;
					}
					//SOFn, except DHT, JPG and DAC
					if ((cMarker >= 0xC0) && (cMarker <= 0xCF) && (cMarker != 0xC4) && (cMarker != 0xC8) && (cMarker != 0xCC))
					{
						bRes = (((iPos + 10) <= iSize) && (acBuffer[iPos + 9] == 4));
						break;
					}
					//SOS or EOI
					if ((cMarker == 0xDA) || (cMarker == 0xD9))
					{
						break;
					}
					iPos += 2 + ((acBuffer[iPos + 2] << 8) | acBuffer[iPos + 3]);
				}
			}
		}
		catch (...)
		{
			//I/O error while reading the mapped pages (the file was truncated by another program, a network share was lost...): it is treated as not matching
			bRes = false;
		}
		clsUtil::UnmapFileView(acBuffer, bMapped);
	}
	return (bRes);
}



// ---------------------------------------------------------------------------------------------------------------------------------------------------------------------------
void __fastcall TfrmMain::UpdateAds(void)
{
	unsigned long lResultFlags;


	//El formulario se está cerrando: no iniciar nuevas navegaciones (el cierre bombea mensajes y podría disparar TitleChange -> UpdateAds -> Navigate de nuevo)
	if (gbClosing)
	{
		return;
	}

	//Show ads
	if ((!gudtOptions.bHideAds) && (InternetGetConnectedState(&lResultFlags, 0)))
	{
		//If was hidden, show it
		if (webAds->Height == 0)
		{
			webAds->Offline = false;
			webAds->Height = 90;
			webAds->Show();
		}

		//Only the first rows are needed, so avoid building the CommaText of the whole column. Guarded and deep copied, because the optimization workers update the grid concurrently
		TStringList *lstAds = new TStringList();
		int iAdsLength = 0;
		EnterCriticalSection(&mudtCriticalSection);
		for (int iRow = 0; (iRow < grdFiles->RowCount) && (iAdsLength < 520); iRow++)
		{
			String sAdsCell = grdFiles->Cells[KI_GRID_FILE][iRow].c_str();
			lstAds->Add(sAdsCell);
			iAdsLength += sAdsCell.Length() + 1;
		}
		LeaveCriticalSection(&mudtCriticalSection);
		String sAdsQuery = LeftStr(lstAds->CommaText, 512);
		delete lstAds;

		#if defined (_DEBUG)
			String sUrl = (String) KS_APP_ADS_URL + "?w=" + webAds->Width + "&h=" + webAds->Height + "&d=1&q=" + sAdsQuery;
		#else
			String sUrl = (String) KS_APP_ADS_URL + "?w=" + webAds->Width + "&h=" + webAds->Height + "&d=0&q=" + sAdsQuery;
		#endif
		//Hack to prevent IE memory leak due to store all previous visited pages
		if (sUrl == webAds->LocationURL)
		{
			webAds->Refresh();
		}
		else
		{
			if (webAds->ActiveEngine == TWebBrowser::TActiveEngine::IE)
			{
				OleVariant oFlags = Shdocvw::navNoHistory | Shdocvw::navNoReadFromCache | Shdocvw::navNoWriteToCache | Shdocvw::navNewWindowsManaged | Shdocvw::navTrustedForActiveX;
				mbWebAdsNavigateRequested = true;
				webAds->Navigate(sUrl, oFlags);
			}
			else
			{
				mbWebAdsNavigateRequested = true;
				webAds->Navigate(sUrl);
			}
		}
		//Trim memory working set
		SetProcessWorkingSetSize(GetCurrentProcess(), -1, -1);
	}
	else
	{
		//If was visible, hide it
		if (webAds->Height > 0)
		{
			//Do not touch the browser when nothing was loaded: navigating to "about:blank"
			//after Hide() makes the native window (Edge/WebView2) be created asynchronously,
			//and it shows as a white rectangle even when the control is hidden
			if (webAds->LocationURL != "")
			{
				webAds->Stop();
				webAds->Offline = true;
				//Release the current page while still visible (IE memory leak hack)
				webAds->Navigate("about:blank");
			}
			webAds->Hide();
			webAds->Height = 0;
		}
	}
}



//---------------------------------------------------------------------------
#pragma argsused
void __fastcall TfrmMain::webAdsTitleChange(TObject *ASender, const WideString Text)
{
	static String sLastOpenedUrl = "";


	if (!gudtOptions.bHideAds)
	{
		//Finished loading
		if (!webAds->Busy)
		{
			gudtOptions.iAdsShown++;
		}

		//URL moved from ads page
		if (PosEx((String) KS_APP_ADS_URL, webAds->LocationURL) == 0)
		{
			if ((PosEx("http://", webAds->LocationURL) != 0) || (PosEx("https://", webAds->LocationURL) != 0))
			{
				//Prevent double opening
				if (sLastOpenedUrl != webAds->LocationURL)
				{
					sLastOpenedUrl = webAds->LocationURL;
					ShellExecute(Handle, _T("open"), sLastOpenedUrl.c_str(), _T(""), _T(""), SW_SHOWNORMAL);
				}
			}
			UpdateAds();
		}
	}
}



// ---------------------------------------------------------------------------------------------------------------------------------------------------------------------------
void __fastcall TfrmMain::UpdateTheme(void)
{
	//Prevent flickering
	LockWindowUpdate(Handle);

	//Interface zoom. Only the change from the zoom already applied is scaled, so this does nothing when it did not change
	if (miAppliedZoom != gudtOptions.iZoom)
	{
		ScaleBy(gudtOptions.iZoom, miAppliedZoom);
		miAppliedZoom = gudtOptions.iZoom;
	}

	int iLightTheme = clsUtil::GetRegistryI(HKEY_CURRENT_USER, _T("Software\\Microsoft\\Windows\\CurrentVersion\\Themes\\Personalize"), _T("AppsUseLightTheme"));
	if (iLightTheme == 0)
	{
		Vcl::Themes::TStyleManager::TrySetStyle("Windows10 Dark", false);
	}
	else
	{
		Vcl::Themes::TStyleManager::TrySetStyle("Windows", false);
	}

	clsLanguage::Load((unsigned int) gudtOptions.iLanguage, true);
	clsLanguage::TranslateForm(this);

	//Change instructions depending on Recycle Bin settings
	if (gudtOptions.bHideInstructions)
	{
		lblInstructions->Visible = false;
	}
	else
	{
		lblInstructions->Visible = true;
		if (gudtOptions.bDoNotUseRecycleBin)
		{
			lblInstructions->Caption = _("Drag on the list below files you want to optimize, and when ready, click on the right button context menu to proceed. No backups will be created, but you can enable moving to Recycle Bin if you like. Double click an item to preview it.");
		}
		else
		{
			lblInstructions->Caption = _("Drag on the list below files you want to optimize, and when ready, click on the right button context menu to proceed. All processed files are copied to Recycle Bin, so you can easily restore them. You can disable moving to Recycle Bin if you like. Double click an item to preview it.");
		}
		lblInstructions->Hint = lblInstructions->Caption;
	}
	
	if (gudtOptions.bAlwaysOnTop)
	{
		FormStyle = fsStayOnTop;
	}
	else
	{
		FormStyle = fsNormal;
	}

	tooMain->Visible = gudtOptions.bShowToolBar;

	UpdateAds();
	
	//Reenable form updates
	LockWindowUpdate(NULL);
}


//---------------------------------------------------------------------------
//Follows the Windows light/dark app theme while the program is running: the VCL only applies the style that the program selects, so it does not change by itself
void __fastcall TfrmMain::WMSettingChange(TMessage &udtMessage)
{
	TForm::Dispatch(&udtMessage);
	//The color mode change is notified with this text. It is sent several times for the same change, but nothing is done when the style is already the right one
	if ((udtMessage.LParam != 0) && (_tcscmp((const TCHAR *) udtMessage.LParam, _T("ImmersiveColorSet")) == 0))
	{
		String sStyle = (clsUtil::GetRegistryI(HKEY_CURRENT_USER, _T("Software\\Microsoft\\Windows\\CurrentVersion\\Themes\\Personalize"), _T("AppsUseLightTheme")) == 0) ? "Windows10 Dark" : "Windows";
		if (Vcl::Themes::TStyleManager::ActiveStyle->Name != sStyle)
		{
			Vcl::Themes::TStyleManager::TrySetStyle(sStyle, false);
		}
	}
}



//---------------------------------------------------------------------------
void __fastcall TfrmMain::RefreshStatus(bool pbUpdateStatusBar, unsigned int piCurrent, unsigned long long plTotalBytes, unsigned long long plSavedBytes)
{
	//Prevent flickering
	//LockWindowUpdate(Handle);

	//ProcessMessages is required before changing DragAcceptFiles. Checked only once, or every refresh would rescan the whole command line
	static bool bNoWindow = (StrStrI(GetCommandLine(), _T("/NOWINDOW")) != nullptr);
	if (bNoWindow)
	{
		Hide();
	}


	Application->ProcessMessages();

	if (gbProcess)
	{
		unsigned int iPercentBytes;
		if (plTotalBytes != 0)
		{
			//iPercentBytes = ((unsigned long long) piTotalBytes - piSavedBytes) * 100 / piTotalBytes;
			//The percentage is the part that was saved, not the part that remains, or 0 bytes saved would show as 100%
			iPercentBytes = ((unsigned int) ((double) plSavedBytes / plTotalBytes * 100));

		}
		else
		{
			iPercentBytes = 0;
		}

		//Required indirection
		String sCaption;
		sCaption.printf(_((TCHAR *) _T("%s / %s files. %s bytes saved (%s%%)")), FormatNumberThousand(piCurrent).c_str(), FormatNumberThousand((unsigned long long) grdFiles->RowCount - 1).c_str(), FormatNumberThousand(plSavedBytes).c_str(), FormatNumberThousand(iPercentBytes).c_str());
		stbMain->Panels->Items[0]->Text = sCaption;

		stbMain->Hint = stbMain->Panels->Items[0]->Text;
		Caption = stbMain->Hint + " - " + Application->Name;
		Application->Title = Caption;

		actOpen->Enabled = false;
		actOpenFolder->Enabled = false;
		actOptimize->Enabled = false;
		actOptions->Enabled = false;
		actStop->Enabled = true;
		actAdd->Enabled = false;
		actAddFolder->Enabled = false;
		actClear->Enabled = false;
		actRemove->Enabled = false;

		// Prevent grid sorting while optimizing. Only change it if needed, because this is called for every file
		TGridOptions oOptionsBusy = grdFiles->Options >> goFixedHotTrack >> goFixedRowClick >> goFixedColClick;
		if (!(grdFiles->Options == oOptionsBusy))
		{
			grdFiles->Options = oOptionsBusy;
		}
		DragAcceptFiles(Handle, false);
	}
	else
	{
		actOpen->Enabled = (grdFiles->RowCount > 1);
		actOpenFolder->Enabled = (grdFiles->RowCount > 1);
		actOptimize->Enabled = (grdFiles->RowCount > 1);
		actOptions->Enabled = true;
		actStop->Enabled = false;
		actAdd->Enabled = true;
		actAddFolder->Enabled = true;
		actClear->Enabled = (grdFiles->RowCount > 1);
		actRemove->Enabled = (grdFiles->RowCount > 1);

		TGridOptions oOptionsIdle = grdFiles->Options << goFixedHotTrack << goFixedRowClick << goFixedColClick;
		if (!(grdFiles->Options == oOptionsIdle))
		{
			grdFiles->Options = oOptionsIdle;
		}
		DragAcceptFiles(Handle, true);

		if (pbUpdateStatusBar)
		{
			if (grdFiles->RowCount > 1)
			{
				//Required indirection
				String sCaption;
				sCaption.printf(_((TCHAR *) _T("0 / %s files")), FormatNumberThousand((unsigned long long) grdFiles->RowCount - 1).c_str());
				stbMain->Panels->Items[0]->Text = sCaption;

				stbMain->Hint = stbMain->Panels->Items[0]->Text;
				Caption = stbMain->Hint + " - " + Application->Name;
				Application->Title = Caption;
				grdFiles->FixedRows = 1;
				pgbProgress->Position = 0;
				pgbProgress->Max = grdFiles->RowCount - 1;
				clsUtil::SetTaskListProgress((unsigned int) pgbProgress->Position, (unsigned int) pgbProgress->Max);
			}
			else
			{
				grdFiles->Rows[0]->BeginUpdate();
				grdFiles->ColCount = 5;
				TStringList *lstRow = new TStringList();
				lstRow->Add(_("File"));
				lstRow->Add(_("Extension (Type)"));
				lstRow->Add(_("Original size"));
				lstRow->Add(_("Optimized size"));
				lstRow->Add(_("Status"));
				grdFiles->Rows[0] = lstRow;
				delete lstRow;
				grdFiles->Rows[0]->EndUpdate();

				stbMain->Panels->Items[0]->Text = "";
				stbMain->Hint = stbMain->Panels->Items[0]->Text;
				Caption = Application->Name;
				Application->Title = Caption;
				pgbProgress->Position = 0;
				clsUtil::SetTaskListProgress((unsigned int) pgbProgress->Position, (unsigned int) pgbProgress->Max);
			}
		}
		else
		{
			//Caption = Application->Name;
			//Application->Title = Caption;
		}
	}

	//Reenable form updates
	//LockWindowUpdate(NULL);
}


//---------------------------------------------------------------------------
String __fastcall TfrmMain::Hash(const String psFilename)
{
	unsigned long long lSize = 0;
	WIN32_FILE_ATTRIBUTE_DATA udtFileAttribute = {};
	String sHash;
	

	//A single call gives both the size and the last write time (they were two separate queries of the same file). Both stay 0 when it fails
	if (GetFileAttributesEx(clsUtil::GetApiName(psFilename.c_str()).c_str(), GetFileExInfoStandard, (void *) &udtFileAttribute))
	{
		lSize = (((unsigned long long) udtFileAttribute.nFileSizeHigh) << 32) + udtFileAttribute.nFileSizeLow;
	}
	sHash = psFilename + _T("|") + (String) lSize + _T("|") + (String) udtFileAttribute.ftLastWriteTime.dwHighDateTime + _T("|") + (String) udtFileAttribute.ftLastWriteTime.dwLowDateTime;
	return (sHash);
}


//---------------------------------------------------------------------------
String __inline TfrmMain::GetCellValue(String psValue, unsigned int piPos)
{
	//Decode the information in cell separating the value to show, with the value to parse
	int iSeparator = psValue.Pos("\r");
	if (iSeparator > 0)
	{
		if (piPos == 0)
		{
			psValue = psValue.SubString(1, iSeparator - 1);
		}
		else if (piPos == 1)
		{
			psValue = psValue.SubString(iSeparator + 1, MaxInt);
		}
	}
	return (psValue);
}



//---------------------------------------------------------------------------
String __inline TfrmMain::SetCellFileValue(const String psValue)
{
	String sRes;


	// 0 = full path + filename (default)
	// 1 = filename only
	// 2 = drive + ... + last folder + filename
	// 3 = full path (compacted with native path ellipsis at draw time according to column width)
	if (gudtOptions.iFilenameFormat == 1)
	{
		sRes = ExtractFileName(psValue);
	}
	else if (gudtOptions.iFilenameFormat == 2)
	{
		// Drive letter + "\...\" + last folder + "\" + filename
		// Example: C:\Users\Me\Documents\Photos\IMG_001.jpg  →  C:\...\Photos\IMG_001.jpg
		String sDrive = ExtractFileDrive(psValue);
		String sName  = ExtractFileName(psValue);
		String sPath  = ExtractFilePath(psValue);

		// Remove drive from path so we work with the directory part only
		if ((!sDrive.IsEmpty()) && (sPath.Length() >= sDrive.Length()) &&
			(sPath.SubString(1, sDrive.Length()).CompareIC(sDrive) == 0))
		{
			sPath = sPath.SubString(sDrive.Length() + 1, MaxInt);
		}

		// Trim leading/trailing backslashes
		while ((!sPath.IsEmpty()) && (sPath[1] == _T('\\')))
			sPath.Delete(1, 1);
		while ((!sPath.IsEmpty()) && (sPath[sPath.Length()] == _T('\\')))
			sPath.Delete(sPath.Length(), 1);

		if (sPath.IsEmpty())
		{
			// Root of the drive, or no path at all
			sRes = sDrive.IsEmpty() ? sName : (sDrive + _T("\\") + sName);
		}
		else
		{
			int iLast = sPath.LastDelimiter(_T("\\"));
			String sLastFolder = (iLast > 0) ? sPath.SubString(iLast + 1, MaxInt) : sPath;
			sRes = sDrive + _T("\\...\\") + sLastFolder + _T("\\") + sName;
		}
	}
	else
	{
		// Formats 0 and 3 both store the full path as the visible part.
		// Format 3 is later compacted with DT_PATH_ELLIPSIS when the cell is painted.
		sRes = psValue;
	}

	sRes += _T("\r") + psValue;
	return (sRes);
}



// ---------------------------------------------------------------------------------------------------------------------------------------------------------------------------
const TCHAR * __fastcall TfrmMain::GetOptionCommandLine(void)
{
	//GetCommandLine already returns a pointer to the full command line, so there is no need to copy it (copying it truncated long command lines)
	return (GetCommandLine());
}


// ---------------------------------------------------------------------------------------------------------------------------------------------------------------------------
const TCHAR * __fastcall TfrmMain::GetOptionArgument(const TCHAR *pacKey)
{
	TCHAR acArgument[PATH_MAX + 1];
	//Thread-local so concurrent option lookups from workers (or reentrant calls) cannot overwrite each other
	static __thread TCHAR acRes[PATH_MAX + 1];
	TCHAR *pcStart;


	//Clear previous result, or the value of a previous key would be returned when this key is not in the command line
	acRes[0] = NULL;
	acArgument[0] = '/';
	//lstrcpyn always terminates and, unlike _tcsncpy, it does not NUL pad the rest of the buffer (that cleared up to PATH_MAX characters on every option lookup)
	lstrcpyn(&acArgument[1], pacKey, (sizeof(acArgument) / sizeof(TCHAR)) - 1);
	pcStart = StrStrI(GetOptionCommandLine(), acArgument);
	//The key must be directly followed by the assignment character, or a longer option name or a path containing the same text could match, so keep looking until one does. The whole argument matched, so the character after it can be read (even if it is the terminator)
	while ((pcStart) && (pcStart[_tcslen(pacKey) + 1] != _T('=')))
	{
		pcStart = StrStrI(pcStart + 1, acArgument);
	}
	if (pcStart)
	{
		TCHAR *pcValue = pcStart + _tcslen(pacKey) + 2;
		TCHAR *pcEnd;
		//A value enclosed in double quotes can contain spaces
		if (*pcValue == '"')
		{
			pcValue++;
			pcEnd = _tcschr(pcValue, '"');
		}
		else
		{
			pcEnd = _tcsstr(pcValue, _T(" "));
		}
		if (pcEnd == NULL)
		{
			pcEnd = pcValue + _tcslen(pcValue);
		}
		size_t iLen = (size_t) (pcEnd - pcValue);
		if (iLen > (sizeof(acRes) / sizeof(TCHAR)) - 1)
		{
			iLen = (sizeof(acRes) / sizeof(TCHAR)) - 1;
		}
		_tcsncpy(acRes, pcValue, iLen);
		acRes[iLen] = NULL;
	}
	return (acRes);
}



// ---------------------------------------------------------------------------------------------------------------------------------------------------------------------------
const TCHAR * __fastcall TfrmMain::GetOption(const TCHAR *pacSection, const TCHAR *pacKey, const TCHAR *pacDefault)
{
	//Thread-local so concurrent option lookups from workers (or reentrant calls) cannot overwrite each other
	static __thread TCHAR acRes[2048];


	//memset(acRes, 0, sizeof(acRes));
	/* Get it via global registry */
	lstrcpyn(acRes, clsUtil::GetRegistry(HKEY_LOCAL_MACHINE, clsUtil::GetRegistryPath(), pacKey), sizeof(acRes) / sizeof(TCHAR));
	if (acRes[0] == NULL)
	{
		/* Get it via user registry */
		lstrcpyn(acRes, clsUtil::GetRegistry(HKEY_CURRENT_USER, clsUtil::GetRegistryPath(), pacKey), sizeof(acRes) / sizeof(TCHAR));
		if (acRes[0] == NULL)
		{
			/* Get it via command-line */
			lstrcpyn(acRes, GetOptionArgument(pacKey), sizeof(acRes) / sizeof(TCHAR));
			if (acRes[0] == NULL)
			{
				//Get it via INI file
				lstrcpyn(acRes, clsUtil::GetIni(pacSection, pacKey, pacDefault), sizeof(acRes) / sizeof(TCHAR));
			}
		}
	}
	return (acRes);
}



// ---------------------------------------------------------------------------------------------------------------------------------------------------------------------------
int __fastcall TfrmMain::GetOption(const TCHAR *pacSection, const TCHAR *pacKey, int piDefault)
{
	int iRes;
	const TCHAR *pacValue = GetOption(pacSection, pacKey, _T(""));


	if (pacValue[0] != NULL)
	{
		iRes = _ttoi(pacValue);
	}
	else
	{
		iRes = piDefault;
	}
	return (iRes);
}



// ---------------------------------------------------------------------------------------------------------------------------------------------------------------------------
long long __fastcall TfrmMain::GetOption(const TCHAR *pacSection, const TCHAR *pacKey, long long plDefault)
{
	long long lRes;
	const TCHAR *pacValue = GetOption(pacSection, pacKey, _T(""));


	if (pacValue[0] != NULL)
	{
		lRes = _ttoi64(pacValue);
	}
	else
	{
		lRes = plDefault;
	}
	return (lRes);
}



// ---------------------------------------------------------------------------------------------------------------------------------------------------------------------------
double __fastcall TfrmMain::GetOption(const TCHAR *pacSection, const TCHAR *pacKey, double pdDefault)
{
	double dRes;
	const TCHAR *pacValue = GetOption(pacSection, pacKey, _T(""));


	if (pacValue[0] != NULL)
	{
		//dRes = _ttof(acDefault);
		dRes = _ttof(pacValue);
	}
	else
	{
		dRes = pdDefault;
	}
	return (dRes);
}



// ---------------------------------------------------------------------------------------------------------------------------------------------------------------------------
bool __fastcall TfrmMain::GetOption(const TCHAR *pacSection, const TCHAR *pacKey, bool pbDefault)
{
	bool bRes;
	const TCHAR *pacValue = GetOption(pacSection, pacKey, _T(""));


	if (pacValue[0] != NULL)
	{
		bRes = ((_tcsicmp(pacValue, _T("true")) == 0) || (_tcsicmp(pacValue, _T("yes")) == 0) || (_tcsicmp(pacValue, _T("on")) == 0) || (_tcscmp(pacValue, _T("1")) == 0));
	}
	else
	{
		bRes = pbDefault;
	}
	return (bRes);
}



// ---------------------------------------------------------------------------------------------------------------------------------------------------------------------------