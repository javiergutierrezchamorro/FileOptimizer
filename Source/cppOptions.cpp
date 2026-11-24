// ---------------------------------------------------------------------------
#include "cppOptions.h"


// ---------------------------------------------------------------------------
#pragma package(smart_init)
#pragma resource "*.dfm"


// ---------------------------------------------------------------------------
TfrmOptions *frmOptions = nullptr;
//Same order as the items of their combo boxes
static const unsigned long maiProcessPriority[] = {IDLE_PRIORITY_CLASS, BELOW_NORMAL_PRIORITY_CLASS, NORMAL_PRIORITY_CLASS, ABOVE_NORMAL_PRIORITY_CLASS, HIGH_PRIORITY_CLASS, REALTIME_PRIORITY_CLASS};
static const TCHAR *const maacPDFProfile[] = {_T("none"), _T("screen"), _T("ebook"), _T("printer"), _T("prepress"), _T("Custom")};
static const TCHAR *const maacCSSTemplate[] = {_T("low"), _T("high"), _T("highest")};



//---------------------------------------------------------------------------
__fastcall TfrmOptions::TfrmOptions(TComponent* Owner): TForm(Owner)
{
}



// ---------------------------------------------------------------------------
__fastcall TfrmOptions::~TfrmOptions()
{
}



//---------------------------------------------------------------------------
#pragma argsused
void __fastcall TfrmOptions::FormCreate(TObject *Sender)
{
	clsUtil::LoadForm(this);
	clsLanguage::TranslateForm(this);

	Icon = Application->Icon;

	tabOptions->ActivePageIndex = 0;
	pagGeneral1->Hint = _("General program options affecting all extensions (1/2)");
	pagGeneral2->Hint = _("General program options affecting all extensions (2/2)");
	pagBMP->Hint = _("Extensions affected: ") + (String) KS_EXTENSION_BMP;
	pagCSS->Hint = _("Extensions affected: ") + (String) KS_EXTENSION_CSS;
	pagEXE->Hint = _("Extensions affected: ") + (String) KS_EXTENSION_EXE KS_EXTENSION_DLL;
	pagGIF->Hint = _("Extensions affected: ") + (String) KS_EXTENSION_GIF;
	pagGZ->Hint = _("Extensions affected: ") + (String) KS_EXTENSION_GZ;
	pagHTML->Hint = _("Extensions affected: ") + (String) KS_EXTENSION_HTML;
	pagJPEG->Hint = _("Extensions affected: ") + (String) KS_EXTENSION_JPG KS_EXTENSION_TIFF;
	pagJS->Hint = _("Extensions affected: ") + (String) KS_EXTENSION_JS;
	pagLUA->Hint = _("Extensions affected: ") + (String) KS_EXTENSION_LUA;
	pagMisc->Hint = _("Extensions affected: ") + (String) KS_EXTENSION_MISC;
	pagMP3->Hint = _("Extensions affected: ") + (String) KS_EXTENSION_MP3;
	pagMP4->Hint = _("Extensions affected: ") + (String) KS_EXTENSION_MP4;
	pagPCX->Hint = _("Extensions affected: ") + (String) KS_EXTENSION_PCX;
	pagPDF->Hint = _("Extensions affected: ") + (String) KS_EXTENSION_PDF;
	pagPNG->Hint = _("Extensions affected: ") + (String) KS_EXTENSION_PNG KS_EXTENSION_ICO;
	pagTGA->Hint = _("Extensions affected: ") + (String) KS_EXTENSION_TGA;
	pagTIFF->Hint = _("Extensions affected: ") + (String) KS_EXTENSION_TIFF;
	pagXML->Hint = _("Extensions affected: ") + (String) KS_EXTENSION_XML KS_EXTENSION_HTML;
	pagZIP->Hint = _("Extensions affected: ") + (String) KS_EXTENSION_ZIP;

	chkAllowDuplicates->Checked = gudtOptions.bAllowDuplicates;
	chkAllowMultipleInstances->Checked = gudtOptions.bAllowMultipleInstances;
	chkAlwaysOnTop->Checked = gudtOptions.bAlwaysOnTop;
	chkBeepWhenDone->Checked = gudtOptions.bBeepWhenDone;

	//Check for updates
	if (gudtOptions.iCheckForUpdates < 0)
	{
		cboCheckForUpdates->ItemIndex = 0;
	}
	else if (gudtOptions.iCheckForUpdates == 0)
	{
		cboCheckForUpdates->ItemIndex = 2;
	}
	else //iCheckForUpdates == 1
	{
		cboCheckForUpdates->ItemIndex = 1;
	}

	chkClearWhenComplete->Checked = gudtOptions.bClearWhenComplete;
	txtDisablePluginMask->Text = gudtOptions.acDisablePluginMask;
	txtDonator->Text = gudtOptions.acDonator;
	chkDoNotUseRecycleBin->Checked = gudtOptions.bDoNotUseRecycleBin;
	chkDoNotCreateBackups->Checked = gudtOptions.bDoNotCreateBackups;
	chkEnableCache->Checked = gudtOptions.bEnableCache;
	txtExcludeMask->Text = gudtOptions.acExcludeMask;
	cboFilenameFormat->ItemIndex = gudtOptions.iFilenameFormat;
	txtIncludeMask->Text = gudtOptions.acIncludeMask;
	chkKeepAttributes->Checked = gudtOptions.bKeepAttributes;
	spnThreads->Value = gudtOptions.iThreads;
	spnZoom->Value = gudtOptions.iZoom;
	spnLeanifyIterations->Value = gudtOptions.iLeanifyIterations;
	cboLogLevel->ItemIndex = gudtOptions.iLogLevel;
	cboOptimizationLevel->ItemIndex = gudtOptions.iLevel;

	//Anything unknown is idle, the first one
	cboProcessPriority->ItemIndex = 0;
	for (unsigned int iCount = 1; iCount < (sizeof(maiProcessPriority) / sizeof(maiProcessPriority[0])); iCount++)
	{
		if ((unsigned long) gudtOptions.iProcessPriority == maiProcessPriority[iCount])
		{
			cboProcessPriority->ItemIndex = (int) iCount;
		}
	}

	chkShowToolBar->Checked = gudtOptions.bShowToolBar;

	//Disable ads in XP
	if (clsUtil::GetWindowsVersion() >= 600)
	{
		chkHideAds->Checked = gudtOptions.bHideAds;
		chkHideAds->Enabled = true;
	}
	else
	{
		chkHideAds->Checked = true;
		chkHideAds->Enabled = false;
	}
	chkShutdownWhenDone->Checked = gudtOptions.bShutdownWhenDone;
	txtTempDirectory->Text = gudtOptions.acTempDirectory;

	chkBMPCopyMetadata->Checked = gudtOptions.bBMPCopyMetadata;
	chkCSSEnableTidy->Checked = gudtOptions.bCSSEnableTidy;

	//Anything unknown is low, the first one
	cboCSSTemplate->ItemIndex = 0;
	for (unsigned int iCount = 1; iCount < (sizeof(maacCSSTemplate) / sizeof(maacCSSTemplate[0])); iCount++)
	{
		if (_tcscmp(gudtOptions.acCSSTemplate, maacCSSTemplate[iCount]) == 0)
		{
			cboCSSTemplate->ItemIndex = (int) iCount;
		}
	}

	chkEXEDisablePETrim->Checked = gudtOptions.bEXEDisablePETrim;
	chkEXEEnableUPX->Checked = gudtOptions.bEXEEnableUPX;
	chkGIFCopyMetadata->Checked = gudtOptions.bGIFCopyMetadata;
	chkGIFAllowLossy->Checked = gudtOptions.bGIFAllowLossy;
	chkGZCopyMetadata->Checked = gudtOptions.bGZCopyMetadata;
	chkHTMLEnableTidy->Checked = gudtOptions.bHTMLEnableTidy;
	chkJPEGCopyMetadata->Checked = gudtOptions.bJPEGCopyMetadata;
	chkJPEGUseArithmeticEncoding->Checked = gudtOptions.bJPEGUseArithmeticEncoding;
	chkJPEGAllowLossy->Checked = gudtOptions.bJPEGAllowLossy;
	txtJSAdditionalExtensions->Text = gudtOptions.acJSAdditionalExtensions;
	chkJSEnableJSMin->Checked = gudtOptions.bJSEnableJSMin;
	chkLUAEnableLeanify->Checked = gudtOptions.bLUAEnableLeanify;
	chkMiscDisable->Checked = gudtOptions.bMiscDisable;
	//chkMiscCopyMetadata->Enabled = !gudtOptions.bMiscDisable;
	chkMiscCopyMetadata->Checked = gudtOptions.bMiscCopyMetadata;
	chkMP3CopyMetadata->Checked = gudtOptions.bMP3CopyMetadata;
	chkMP4CopyMetadata->Checked = gudtOptions.bMP4CopyMetadata;
	chkPCXCopyMetadata->Checked = gudtOptions.bPCXCopyMetadata;

	//Anything unknown is none, the first one
	cboPDFProfile->ItemIndex = 0;
	for (unsigned int iCount = 1; iCount < (sizeof(maacPDFProfile) / sizeof(maacPDFProfile[0])); iCount++)
	{
		if (_tcscmp(gudtOptions.acPDFProfile, maacPDFProfile[iCount]) == 0)
		{
			cboPDFProfile->ItemIndex = (int) iCount;
		}
	}

	spnPDFCustomDPI->Value = gudtOptions.iPDFCustomDPI;
	chkPDFSkipLayered->Checked = gudtOptions.bPDFSkipLayered;
	cboPDFProfileChange(NULL);
	//Setting the controls from code does not always trigger their events, so enable or disable the dependent controls explicitly
	chkCSSEnableTidyClick(NULL);
	chkMiscDisableClick(NULL);
	chkAllowDuplicatesClick(NULL);

	chkPNGCopyMetadata->Checked = gudtOptions.bPNGCopyMetadata;
	chkPNGAllowLossy->Checked = gudtOptions.bPNGAllowLossy;
	chkTGACopyMetadata->Checked = gudtOptions.bTGACopyMetadata;
	chkTIFFCopyMetadata->Checked = gudtOptions.bTIFFCopyMetadata;
	chkXMLEnableLeanify->Checked = gudtOptions.bXMLEnableLeanify;
	chkZIPCopyMetadata->Checked = gudtOptions.bZIPCopyMetadata;
	chkZIPRecurse->Checked = gudtOptions.bZIPRecurse;

	//Fill available languages
	cboLanguage->Items->Add(_("Automatic: System default"));
	cboLanguage->Items->Add(_("1033: English - United States (Built-in)"));

	//Language files are in the program directory. Do not change the current one, since relative paths (such as the ones from the command line) depend on it
	String sProgramDirectory = ExtractFilePath(Application->ExeName);

	WIN32_FIND_DATA udtFindFileData;
	HANDLE hFindFile = FindFirstFile((sProgramDirectory + "*.po").c_str(), &udtFindFileData);
	if (hFindFile != INVALID_HANDLE_VALUE)
	{
		unsigned int iIndex = 2;
		do
		{
			//1033 is the built-in language, so do not list it twice (the file exists after saving the language strings)
			if (_tcsicmp(udtFindFileData.cFileName, _T("1033.po")) == 0)
			{
				continue;
			}
			char acBuffer[2048];
			unsigned int iRead = sizeof(acBuffer) - 1;
			clsUtil::ReadFile((sProgramDirectory + udtFindFileData.cFileName).c_str(), acBuffer, &iRead);
			//ReadFile leaves the size really read (0 when it fails), but it does not terminate the text
			acBuffer[iRead] = 0;
			char *pcStart = strstr(acBuffer, "\"Language Name:");
			if (pcStart)
			{
				const char *pcEnd = strstr(pcStart + 15, "\"");
				if ((pcEnd) && (pcEnd >= (pcStart + 16)))
				{
					String sLanguage = ChangeFileExt(udtFindFileData.cFileName, "");
					//Converted straight from the buffer, there is no need to move it first
					sLanguage += ": " + UTF8ArrayToString(pcStart + 16, (size_t) (pcEnd - pcStart - 16));
					cboLanguage->Items->Add(sLanguage);
					//Selected?
					if (PosEx((String) gudtOptions.iLanguage + ": ", sLanguage) == 1)
					{
						cboLanguage->ItemIndex = (int) iIndex;
					}
					iIndex++;
				}
			}
		}
		while ((bool) FindNextFile(hFindFile, &udtFindFileData) != false);
		FindClose(hFindFile);
	}
	if (gudtOptions.iLanguage == 1033)
	{
		cboLanguage->ItemIndex = 1;
	}
	else if ((gudtOptions.iLanguage == 0) || (cboLanguage->ItemIndex < 0))
	{
		//Fall back to the system default when the language file no longer exists, or nothing would be selected
		cboLanguage->ItemIndex = 0;
	}

}



//---------------------------------------------------------------------------
#pragma argsused
void __fastcall TfrmOptions::butOKClick(TObject *Sender)
{
	if ((txtDonator->Text == "") && (chkHideAds->Checked) && (chkHideAds->Enabled))
	{
		String sCaption;
		sCaption.printf(_((TCHAR *) _T("Do you want to contribute %s development by showing ads while it is in use?\n\nThis will encourage its future maintenance and upgrades, being highly appreciated.")), Application->Name.c_str());
		chkHideAds->Checked = !(clsUtil::MsgBox(Handle, sCaption.c_str(), _((TCHAR *) _T("Support")), MB_YESNO | MB_ICONQUESTION) == ID_YES);
	}

	Hide();

	gudtOptions.bAllowDuplicates = chkAllowDuplicates->Checked;
	gudtOptions.bAllowMultipleInstances = chkAllowMultipleInstances->Checked;
	gudtOptions.bAlwaysOnTop = chkAlwaysOnTop->Checked;
	gudtOptions.bBeepWhenDone = chkBeepWhenDone->Checked;

	if (cboCheckForUpdates->ItemIndex == 0)
	{
		gudtOptions.iCheckForUpdates = -1;
	}
	else if (cboCheckForUpdates->ItemIndex == 2)
	{
		gudtOptions.iCheckForUpdates = 0;
	}
	else //1
	{
		gudtOptions.iCheckForUpdates = 1;
	}

	gudtOptions.bClearWhenComplete = chkClearWhenComplete->Checked;
	gudtOptions.bEnableCache = chkEnableCache->Checked;
	gudtOptions.bKeepAttributes = chkKeepAttributes->Checked;
	gudtOptions.bDoNotUseRecycleBin = chkDoNotUseRecycleBin->Checked;
	gudtOptions.bDoNotCreateBackups = chkDoNotCreateBackups->Checked;
	clsUtil::CopyString(gudtOptions.acDisablePluginMask, sizeof(gudtOptions.acDisablePluginMask) / sizeof(TCHAR), txtDisablePluginMask->Text.c_str());
	clsUtil::CopyString(gudtOptions.acDonator, sizeof(gudtOptions.acDonator) / sizeof(TCHAR), txtDonator->Text.c_str());
	clsUtil::CopyString(gudtOptions.acExcludeMask, sizeof(gudtOptions.acExcludeMask) / sizeof(TCHAR), txtExcludeMask->Text.c_str());
	gudtOptions.iFilenameFormat = cboFilenameFormat->ItemIndex;
	clsUtil::CopyString(gudtOptions.acIncludeMask, sizeof(gudtOptions.acIncludeMask) / sizeof(TCHAR), txtIncludeMask->Text.c_str());
	gudtOptions.iThreads = spnThreads->Value;
	gudtOptions.iZoom = max(KI_ZOOM_MIN, min((int) spnZoom->Value, KI_ZOOM_MAX));
	gudtOptions.iLeanifyIterations = spnLeanifyIterations->Value;
	gudtOptions.iLogLevel = cboLogLevel->ItemIndex;
	gudtOptions.iLevel = cboOptimizationLevel->ItemIndex;

	//Anything unknown is idle
	gudtOptions.iProcessPriority = (((unsigned int) cboProcessPriority->ItemIndex) < (sizeof(maiProcessPriority) / sizeof(maiProcessPriority[0]))) ? (int) maiProcessPriority[cboProcessPriority->ItemIndex] : IDLE_PRIORITY_CLASS;

	gudtOptions.bShutdownWhenDone = chkShutdownWhenDone->Checked;
	gudtOptions.bShowToolBar = chkShowToolBar->Checked;
	gudtOptions.bHideAds = chkHideAds->Checked;

	clsUtil::CopyString(gudtOptions.acTempDirectory, sizeof(gudtOptions.acTempDirectory) / sizeof(TCHAR), txtTempDirectory->Text.c_str());

	gudtOptions.bBMPCopyMetadata = chkBMPCopyMetadata->Checked;

	gudtOptions.bCSSEnableTidy = chkCSSEnableTidy->Checked;
	//Anything unknown is low
	_tcscpy(gudtOptions.acCSSTemplate, (((unsigned int) cboCSSTemplate->ItemIndex) < (sizeof(maacCSSTemplate) / sizeof(maacCSSTemplate[0]))) ? maacCSSTemplate[cboCSSTemplate->ItemIndex] : _T("low"));


	gudtOptions.bEXEDisablePETrim = chkEXEDisablePETrim->Checked;
	gudtOptions.bEXEEnableUPX = chkEXEEnableUPX->Checked;
	gudtOptions.bGIFCopyMetadata = chkGIFCopyMetadata->Checked;
	gudtOptions.bGIFAllowLossy = chkGIFAllowLossy->Checked;
	gudtOptions.bGZCopyMetadata = chkGZCopyMetadata->Checked;
	gudtOptions.bHTMLEnableTidy = chkHTMLEnableTidy->Checked;
	gudtOptions.bJPEGCopyMetadata = chkJPEGCopyMetadata->Checked;
	gudtOptions.bJPEGUseArithmeticEncoding = chkJPEGUseArithmeticEncoding->Checked;
	gudtOptions.bJPEGAllowLossy = chkJPEGAllowLossy->Checked;
	clsUtil::CopyString(gudtOptions.acJSAdditionalExtensions, sizeof(gudtOptions.acJSAdditionalExtensions) / sizeof(TCHAR), txtJSAdditionalExtensions->Text.c_str());
	gudtOptions.bJSEnableJSMin = chkJSEnableJSMin->Checked;
	gudtOptions.bLUAEnableLeanify = chkLUAEnableLeanify->Checked;
	gudtOptions.bMiscDisable = chkMiscDisable->Checked;
	gudtOptions.bMiscCopyMetadata = chkMiscCopyMetadata->Checked;
	gudtOptions.bMP3CopyMetadata = chkMP3CopyMetadata->Checked;
	gudtOptions.bMP4CopyMetadata = chkMP4CopyMetadata->Checked;
	gudtOptions.bPCXCopyMetadata = chkPCXCopyMetadata->Checked;

	//Anything unknown is ebook
	_tcscpy(gudtOptions.acPDFProfile, (((unsigned int) cboPDFProfile->ItemIndex) < (sizeof(maacPDFProfile) / sizeof(maacPDFProfile[0]))) ? maacPDFProfile[cboPDFProfile->ItemIndex] : _T("ebook"));

	gudtOptions.iPDFCustomDPI = spnPDFCustomDPI->Value;
	gudtOptions.bPDFSkipLayered = chkPDFSkipLayered->Checked;

	gudtOptions.bPNGCopyMetadata = chkPNGCopyMetadata->Checked;
	gudtOptions.bPNGAllowLossy = chkPNGAllowLossy->Checked;
	gudtOptions.bTGACopyMetadata = chkTGACopyMetadata->Checked;
	gudtOptions.bTIFFCopyMetadata = chkTIFFCopyMetadata->Checked;
	gudtOptions.bXMLEnableLeanify = chkXMLEnableLeanify->Checked;
	gudtOptions.bZIPCopyMetadata = chkZIPCopyMetadata->Checked;
	gudtOptions.bZIPRecurse = chkZIPRecurse->Checked;

	int iOldLanguage = gudtOptions.iLanguage;
	//Save selected language
	int iPos = PosEx(": ", cboLanguage->Text);
	if (iPos > 0)
	{
		String sLanguage = cboLanguage->Text.SubString(1, iPos - 1);
		gudtOptions.iLanguage = _ttoi(sLanguage.c_str());
	}
	else
	{
		gudtOptions.iLanguage = 0;
	}

	frmMain->SaveOptions();
	frmMain->UpdateTheme();

	if (iOldLanguage != gudtOptions.iLanguage)
	{
		String sCaption;
		sCaption.printf(_((TCHAR *) _T("Some changed settings require %s to be restarted.\nDo you want to restart it now?")), Application->Name.c_str());
		if (clsUtil::MsgBox(Handle, sCaption.c_str(), _((TCHAR *) _T("Restart")), MB_YESNO | MB_ICONQUESTION) == ID_YES)
		{
			ShellExecute(NULL, _T("open"), Application->ExeName.c_str(), _T("/AllowMultipleInstances=true"), _T(""), SW_SHOWNORMAL);
			Application->Terminate();
		}
	}
	Close();
}



//---------------------------------------------------------------------------
#pragma argsused
void __fastcall TfrmOptions::chkCSSEnableTidyClick(TObject *Sender)
{
	cboCSSTemplate->Enabled = chkCSSEnableTidy->Checked;
}



//---------------------------------------------------------------------------
#pragma argsused
void __fastcall TfrmOptions::chkMiscDisableClick(TObject *Sender)
{
	chkMiscCopyMetadata->Enabled = !chkMiscDisable->Checked;
}



//---------------------------------------------------------------------------
#pragma argsused
void __fastcall TfrmOptions::cboPDFProfileChange(TObject *Sender)
{
	spnPDFCustomDPI->Enabled = (cboPDFProfile->ItemIndex == 5);
}


//---------------------------------------------------------------------------
#pragma argsused
void __fastcall TfrmOptions::butCancelClick(TObject *Sender)
{
	Close();
}



//---------------------------------------------------------------------------
#pragma argsused
void __fastcall TfrmOptions::butRestoreDefaultsClick(TObject *Sender)
{
	Hide();

	clsUtil::DeleteFile(clsUtil::GetIniPath(false));
	clsUtil::DeleteFile(clsUtil::GetIniPath(true));
	clsUtil::DeleteRegistry(HKEY_LOCAL_MACHINE, clsUtil::GetRegistryPath());
	clsUtil::DeleteRegistry(HKEY_CURRENT_USER, clsUtil::GetRegistryPath());
	clsUtil::SetRegistry(HKEY_CURRENT_USER, _T("Software\\Microsoft\\Internet Explorer\\Main\\FeatureControl\\FEATURE_BROWSER_EMULATION"), ExtractFileName(Application->ExeName).c_str(), (TCHAR *) NULL);

	frmMain->LoadOptions();
	frmMain->UpdateTheme();
	Close();
}



//---------------------------------------------------------------------------
#pragma argsused
void __fastcall TfrmOptions::FormClose(TObject *Sender, TCloseAction &Action)
{
	clsUtil::SaveForm(this);
}



//---------------------------------------------------------------------------
void __fastcall TfrmOptions::chkAllowDuplicatesClick(TObject *Sender)
{
	spnThreads->Enabled = !chkAllowDuplicates->Checked;
}
//---------------------------------------------------------------------------