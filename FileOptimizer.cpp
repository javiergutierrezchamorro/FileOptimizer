// ---------------------------------------------------------------------------
#include "FileOptimizerPCH1.h"
#include "cppMain.h"
#include "cppAbout.h"
#include "cppOptions.h"
#include "clsUtil.h"


// ---------------------------------------------------------------------------


//---------------------------------------------------------------------------
#include <Vcl.Styles.hpp>
#include <Vcl.Themes.hpp>
USEFORM("Source\cppMain.cpp", frmMain);
USEFORM("Source\cppAbout.cpp", frmAbout);
USEFORM("Source\cppOptions.cpp", frmOptions);



//---------------------------------------------------------------------------
#pragma argsused
int WINAPI _tWinMain(HINSTANCE phInstance, HINSTANCE phPrevInstance, LPTSTR pacCmdLine, int piShow)
{
	HANDLE hMutex = nullptr;

	try
	{
		//Simple command line help
		if (_tcsnccmp(pacCmdLine, _T("/?"), 2) == 0)
		{
			clsUtil::MsgBox(nullptr, _T(
				"FileOptimizer command line syntax is:\n"
				"FileOptimizer.exe <Path|File> [/Options]\n\n"
				"<Path|File>: Specifies a path to recursively process or a single file. You can specify as much Files/Paths as you want. Paths with extended characters, or spaces, should be double quoted. Such as *.JPG \"C:\\PROYECTOS\\\" C:\\TEST.PNG\n\n"
				"[/Options]: Optionally you can specify any options, using same syntax as in the INI file, prepending them with a slash. Such as /GIFAllowLossy=true /Level=9 /PDFCustomDPI=150 /ExcludeMask=.bak\n\nSpecial options are also supported:\n\n"
				"/?: Show this help screen.\n"
				"/NOWINDOW: Run windowless, useful when using command-line.\n"
				"/SAVELANGUAGE: Save 1033.po file containing english strings.\n\n"
				"Arguments work like:\n\n"
				"C:\\PROYECTOS\\FileOptimizer\\Win32\\Release\\FileOptimizer64.exe /PNGCopyMetadata=true \"C:\\PROYECTOS\\FileOptimizer\\_Tests\\_ - copia\"\n\n"
				"Where:\n\n"
				"- /PNGCopyMetadata=true: Activate copying PNG metadata.\n"
				"- C:\\PROYECTOS\\FileOptimizer\\Win32\\Release\\FileOptimizer64.exe: Is the full path where FileOptimizer executable is.\n"
				"- \"C:\\PROYECTOS\\FileOptimizer\\_Tests\\_ - copia\\\": Is the full path of the folder to recursively process. Be aware enclosing it with double quotes as in the example, which is Windows mandatory if the folder contains spaces, or extended characters.\n\n"
				"Alternatively you can specify only one single file instead of a folder:\n\n"
				"C:\\PROYECTOS\\FileOptimizer\\Win32\\Release\\FileOptimizer64.exe \"C:\\PROYECTOS\\FileOptimizer\\_Tests\\_ - copia\\av-214_.zip\"\n\n"
				"Where:\n\n"
				"- C:\\PROYECTOS\\FileOptimizer\\Win32\\Release\\FileOptimizer64.exe: Is the full path where FileOptimizer executable is.\n"
				"- \"C:\\PROYECTOS\\FileOptimizer\\_Tests\\_ - copia\\av-214_.zip\": Is the full path of the file to process."
			), _T("Command-line help"), MB_OK | MB_ICONINFORMATION);
			return(-1);
		}

		Application->Initialize();
		Application->Name = "FileOptimizer";
		Application->Title = Application->Name;
		//Full path, because the current directory is changed while optimizing, and it depends on how the program was launched
		Application->HelpFile = ExtractFilePath(Application->ExeName) + Application->Name + ".chm";

		Application->MainFormOnTaskBar = true;

		#if defined(_DEBUG)
			ReportMemoryLeaksOnShutdown = true;
		#endif

		SetProcessWorkingSetSize(GetCurrentProcess(), (SIZE_T) -1, (SIZE_T) -1);	//GS:AGGRESSIVE
		SetMinimumBlockAlignment(mba16Byte);

		#if !defined(_WIN64)
			// Disable file system redirection on Win64 environments
			//Kernel32 is always loaded, so no need to load and free it
			//HMODULE hKernel32 = LoadLibraryEx(_T("KERNEL32.DLL"), NULL, LOAD_LIBRARY_SEARCH_SYSTEM32);
			HMODULE hKernel32 = GetModuleHandle(_T("KERNEL32.DLL"));
			if (hKernel32)
			{
				typedef BOOL (WINAPI Wow64DisableWow64FsRedirectionType)(PVOID *);
				Wow64DisableWow64FsRedirectionType *Wow64DisableWow64FsRedirectionProc = (Wow64DisableWow64FsRedirectionType *) GetProcAddress(hKernel32, "Wow64DisableWow64FsRedirection");
				if (Wow64DisableWow64FsRedirectionProc)
				{
					PVOID pOldWin64Redirect = nullptr;
					Wow64DisableWow64FsRedirectionProc(&pOldWin64Redirect);
				}
			}
		#endif

		//Drag and drop between non-elevated processes and elevated ones (UIPI) is enabled for the main window in TfrmMain::FormCreate, so there is no need to also open the process wide filter here


		if (!TfrmMain::GetOption(_T("Options"), _T("AllowMultipleInstances"), false))
		{
			//Create it directly instead of Open + Create, to avoid a race when two instances start at the same time
			SetLastError(0);
			hMutex = CreateMutex(nullptr, false, Application->Name.c_str());
			//Read it once, or the second call could already see a different error. ERROR_ACCESS_DENIED means it already exists, but it belongs to a process with higher privileges
			unsigned long lLastError = GetLastError();
			if (((hMutex) && (lLastError == ERROR_ALREADY_EXISTS)) || ((!hMutex) && (lLastError == ERROR_ACCESS_DENIED)))
			{
				if (clsUtil::MsgBox(nullptr, ("There is one instance of " + Application->Name + " still running.\r\n\r\nDo you want to open another?").c_str(), _T("Still running"), MB_YESNO | MB_ICONQUESTION) == IDNO)
				{
					if (hMutex)
					{
						CloseHandle(hMutex);
					}
					return (1);
				}
			}
		}


		//Look for it only once
		bool bNoWindow = (StrStrI(GetCommandLine(), _T("/NOWINDOW")) != nullptr);
		if (bNoWindow)
		{
			Application->ShowMainForm = false;
			Application->MainFormOnTaskBar = false;
		}
		Application->CreateForm(__classid(TfrmMain), &frmMain);
		if (bNoWindow)
		{
			frmMain->Visible = false;
		}
		Application->Run();
	}
	catch (Exception &excE)
	{
		#if defined( _DEBUG)
			Application->ShowException(&excE);
		#endif
	}
	catch (...)
	{
	}
	//Released once, whatever way the program ended
	if (hMutex)
	{
		//ReleaseMutex(hMutex);
		CloseHandle(hMutex);
	}
	return (0);
}


//---------------------------------------------------------------------------