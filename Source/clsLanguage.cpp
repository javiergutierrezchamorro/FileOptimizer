// ---------------------------------------------------------------------------------------------------------------------------------------------------------------------------
/*
 1.01. 14/11/2026. FileOptimizer. The lock is released on any exit (a failing LoadFromFile left it locked forever), PO escapes are decoded in a single pass (the sequential replacements turned an escaped backslash followed by n into a new line), no more NOT_FOUND marker that a real translation could collide with, iterations over the controls skip every property that did not change and are not even started when there is nothing to translate
 1.00. 22/11/2017. FileOptimizer. Initial version
 */
// ---------------------------------------------------------------------------------------------------------------------------------------------------------------------------
#include "clsLanguage.h"


//Lines of the PO files, hashed so the msgid lookups are fast
static THashedStringList *mlstLanguage = nullptr;
static THashedStringList *mlst1033 = nullptr;
//Multithreaded optimization: workers translate concurrently, so the shared language lists and their Strings must be guarded. It is created before the program starts, so there is no first use race
static CRITICAL_SECTION mcsLanguage;
static struct udtLanguageLockInit
{
	udtLanguageLockInit()
	{
		InitializeCriticalSection(&mcsLanguage);
	}
} mudtLanguageLockInit;


// ---------------------------------------------------------------------------------------------------------------------------------------------------------------------------
//Holds the lock until the end of the scope, whatever way it is left (such as an exception)
class clsLanguageLock
{
	public:
		clsLanguageLock()
		{
			EnterCriticalSection(&mcsLanguage);
		}
		~clsLanguageLock()
		{
			LeaveCriticalSection(&mcsLanguage);
		}
};


// ---------------------------------------------------------------------------------------------------------------------------------------------------------------------------
//Nothing to translate to (built-in English) and not collecting texts
static bool NothingToTranslate(void)
{
	return ((!mlst1033) && ((!mlstLanguage) || (mlstLanguage->Count <= 0)));
}


// ---------------------------------------------------------------------------------------------------------------------------------------------------------------------------
//Escapes a text to be used in a PO file. Most texts have no special characters, so they are returned as they are
static String EscapePo(const String &psText)
{
	int iLength = psText.Length();
	int iSpecial = 0;
	for (int iChar = 1; iChar <= iLength; iChar++)
	{
		switch (psText[iChar])
		{
			case '\\':
			case '\n':
			case '\r':
			case '\t':
			case '\"':
				iSpecial++;
		}
	}
	if (iSpecial == 0)
	{
		return (psText);
	}
	String sRes;
	sRes.SetLength(iLength + iSpecial);
	int iOut = 0;
	for (int iChar = 1; iChar <= iLength; iChar++)
	{
		TCHAR cChar = psText[iChar];
		TCHAR cEscaped = 0;
		switch (cChar)
		{
			case '\\':
			case '\"':
				cEscaped = cChar;
				break;
			case '\n':
				cEscaped = 'n';
				break;
			case '\r':
				cEscaped = 'r';
				break;
			case '\t':
				cEscaped = 't';
				break;
		}
		if (cEscaped != 0)
		{
			sRes[++iOut] = '\\';
			sRes[++iOut] = cEscaped;
		}
		else
		{
			sRes[++iOut] = cChar;
		}
	}
	return (sRes);
}


// ---------------------------------------------------------------------------------------------------------------------------------------------------------------------------
//Decodes the escapes of a PO text in a single pass, so an escaped backslash is never mistaken for the start of another escape
static String UnescapePo(const String &psText)
{
	if (psText.Pos("\\") == 0)
	{
		return (psText);
	}
	int iLength = psText.Length();
	String sRes;
	sRes.SetLength(iLength);
	int iOut = 0;
	for (int iChar = 1; iChar <= iLength; iChar++)
	{
		TCHAR cChar = psText[iChar];
		if ((cChar == '\\') && (iChar < iLength))
		{
			TCHAR cNext = psText[++iChar];
			switch (cNext)
			{
				case 'n':
					cChar = '\n';
					break;
				case 'r':
					cChar = '\r';
					break;
				case 't':
					cChar = '\t';
					break;
				case '\\':
				case '\"':
					cChar = cNext;
					break;
				default:
					//Unknown escape, so it is kept as it is
					sRes[++iOut] = '\\';
					cChar = cNext;
					break;
			}
		}
		sRes[++iOut] = cChar;
	}
	sRes.SetLength(iOut);
	return (sRes);
}


// ---------------------------------------------------------------------------------------------------------------------------------------------------------------------------
static bool LoadPo(THashedStringList *plstLanguage, const String &psFile)
{
	if (!clsUtil::ExistsFile(psFile.c_str()))
	{
		return (false);
	}
	plstLanguage->LoadFromFile(psFile, TEncoding::UTF8);
	return (true);
}


// ---------------------------------------------------------------------------------------------------------------------------------------------------------------------------
static THashedStringList *NewLanguageList(void)
{
	THashedStringList *lstLanguage = new THashedStringList();
	lstLanguage->CaseSensitive = true;
	lstLanguage->Duplicates = System::Classes::dupAccept;
	return (lstLanguage);
}


// ---------------------------------------------------------------------------------------------------------------------------------------------------------------------------
void __fastcall clsLanguage::Load(unsigned int piLanguage, bool pbForce)
{
	clsLanguageLock oLock;


	if (pbForce)
	{
		delete mlstLanguage;
		mlstLanguage = nullptr;
	}

	//Language files are in the program directory, not in the current one (which depends on how the program was launched, and changes while optimizing)
	String sDirectory = ExtractFilePath(Application->ExeName);

	//If not yet loaded
	if (!mlstLanguage)
	{
		mlstLanguage = NewLanguageList();
		if (piLanguage != 0)
		{
			//Never load en-US because it is built-in
			if (piLanguage != 1033)
			{
				LoadPo(mlstLanguage, sDirectory + (String) (int) piLanguage + ".po");
			}
		}
		else
		{
			//Language of the current user, not the one Windows was installed with
			LANGID iLanguage = GetUserDefaultUILanguage();
			if (iLanguage != 1033)
			{
				//Try to load locale, and then the primary language
				if (!LoadPo(mlstLanguage, sDirectory + (String) (int) iLanguage + ".po"))
				{
					LoadPo(mlstLanguage, sDirectory + (String) (int) PRIMARYLANGID(iLanguage) + ".po");
				}
			}
		}
	}

	//Need to collect the texts?
	if ((!mlst1033) && (StrStrI(GetCommandLine(), _T("/SAVELANGUAGE")) != nullptr))
	{
		mlst1033 = NewLanguageList();
		LoadPo(mlst1033, sDirectory + "1033.po");
	}
}



// ---------------------------------------------------------------------------------------------------------------------------------------------------------------------------
void __fastcall clsLanguage::Save(void)
{
	clsLanguageLock oLock;


	//Need to save language?
	if (mlst1033)
	{
		try
		{
			//Same directory as the rest of language files, not the current one (which changes while optimizing)
			mlst1033->SaveToFile(ExtractFilePath(Application->ExeName) + "1033.po", TEncoding::UTF8);
		}
		catch (...)
		{
		}
	}
}



// ---------------------------------------------------------------------------
const TCHAR * __fastcall clsLanguage::Get(TCHAR *pacText)
{
	//Multithreaded optimization: workers translate concurrently, so the returned buffer must be thread local instead of shared
	static __thread TCHAR acRes[2048];


	String sRes = Get((String) pacText);
	//lstrcpyn always terminates, and unlike memset + _tcsncpy it does not clear and pad the whole buffer on every call
	lstrcpyn(acRes, sRes.c_str(), sizeof(acRes) / sizeof(TCHAR));
	return (acRes);
}



// ---------------------------------------------------------------------------
//Looks for the translation of a text. It returns false when the text is not in the list, and an empty translation means that it is there but not translated yet. It must be called with the lock held
static bool Search(const String &psText, THashedStringList *plstLanguage, String &psTranslation)
{
	if ((!plstLanguage) || (plstLanguage->Count <= 0))
	{
		return (false);
	}
	int iLine = plstLanguage->IndexOf("msgid \"" + EscapePo(psText) + "\"");
	if (iLine < 0)
	{
		return (false);
	}
	int iLines = plstLanguage->Count;
	//Skip lines not starting with msgstr, but do not read past the next entry (including plural forms) nor the end of the list
	for (iLine++; iLine < iLines; iLine++)
	{
		String sLine = plstLanguage->Strings[iLine];
		if (StartsStr("msgid \"", sLine))
		{
			break;
		}
		if (StartsStr("msgstr \"", sLine))
		{
			//The texts collected in this session end with a new line
			if (sLine.Pos("\n") > 0)
			{
				sLine = ReplaceStr(sLine, "\n", "");
			}
			//Remove first msgstr \" and last quote
			psTranslation = UnescapePo(sLine.SubString(9, sLine.Length() - 9));
			return (true);
		}
	}
	return (false);
}



// ---------------------------------------------------------------------------
String __fastcall clsLanguage::Get(String psText)
{
	//Nothing to search, so do not even take the lock
	if ((psText.IsEmpty()) || (NothingToTranslate()))
	{
		return (psText);
	}
	//Multithreaded optimization: workers translate concurrently and the lists return shared Strings whose reference count is not updated atomically, so guard the accesses
	clsLanguageLock oLock;
	String sTranslated;

	//If need to update 1033.po, add the text when it is not there yet
	if ((mlst1033) && (!Search(psText, mlst1033, sTranslated)))
	{
		if (mlst1033->Count <= 0)
		{
			//Write header
			mlst1033->Add("# \"Language ID: 1033 (0x0409)\"");
			mlst1033->Add("# \"Language Name: English - United States\"");
			mlst1033->Add("# \"Project-Id-Version: " + Application->Name + " " + (String) clsUtil::ExeVersion(Application->ExeName.c_str()) + "\"");
			mlst1033->Add("# \"POT-Creation-Date: " + (String) __DATE__ + "\"");
			mlst1033->Add("# \"MIME-Version: 1.0\"");
			mlst1033->Add("# \"Content-Type: text/plain; charset=UTF-8\"");
			mlst1033->Add("# \"Content-Transfer-Encoding: 8bit\"");
			mlst1033->Add("# \"Language: en_US\"");
			mlst1033->Add(_T("# \"Last-Translator: Javier Guti\u00e9rrez Chamorro\""));
			mlst1033->Add(_T("# \"Language-Team: Javier Guti\u00e9rrez Chamorro\""));
			mlst1033->Add("# \"Plural-Forms: nplurals=2; plural=(n != 1);\"\n");
		}
		//Write text
		mlst1033->Add("msgid \"" + EscapePo(psText) + "\"");
		mlst1033->Add("msgstr \"\"\n");
	}

	//Look on regular PO dictionary. An empty msgstr means that it is not translated yet, so keep the original text instead of showing nothing
	if ((Search(psText, mlstLanguage, sTranslated)) && (!sTranslated.IsEmpty()))
	{
		psText = sTranslated;
	}
	return (psText);
}



// ---------------------------------------------------------------------------
//Translates the caption and the hint of any control, menu item or action that has them. It returns false when the component is not of that class. Properties are only written when they changed, since writing them repaints, lays out again, and in actions also updates every linked control
template <class T>
static bool TranslateCaptionHint(TComponent *poComponent)
{
	T *oControl = dynamic_cast<T *>(poComponent);
	if (!oControl)
	{
		return (false);
	}
	String sCaption = oControl->Caption;
	String sTranslated = clsLanguage::Get(sCaption);
	if (sTranslated != sCaption)
	{
		oControl->Caption = sTranslated;
	}
	String sHint = oControl->Hint;
	sTranslated = clsLanguage::Get(sHint);
	if (sTranslated != sHint)
	{
		oControl->Hint = sTranslated;
	}
	return (true);
}



// ---------------------------------------------------------------------------
//Same for the controls that only have a hint
template <class T>
static void TranslateHint(T *poControl)
{
	String sHint = poControl->Hint;
	String sTranslated = clsLanguage::Get(sHint);
	if (sTranslated != sHint)
	{
		poControl->Hint = sTranslated;
	}
}



// ---------------------------------------------------------------------------
//Translates the text of a multiline control, without rewriting it when nothing changed. Shared by the TMemo and TRichEdit branches
static void TranslateLines(TStrings *poLines)
{
	String sText = poLines->Text;
	String sTranslated = clsLanguage::Get(sText);
	if (sTranslated != sText)
	{
		poLines->Text = sTranslated;
	}
}



// ---------------------------------------------------------------------------
static void EnumerateControls(TComponent *poComponent)
{
	//The first class that matches is the only one handled
	if ((!TranslateCaptionHint<TForm>(poComponent)) &&
		(!TranslateCaptionHint<TLabel>(poComponent)) &&
		(!TranslateCaptionHint<TCheckBox>(poComponent)) &&
		(!TranslateCaptionHint<TMenuItem>(poComponent)) &&
		(!TranslateCaptionHint<TButton>(poComponent)) &&
		(!TranslateCaptionHint<TAction>(poComponent)) &&
		(!TranslateCaptionHint<TTabSheet>(poComponent)))
	{
		if (TEdit *oEdit = dynamic_cast<TEdit *>(poComponent))
		{
			String sText = oEdit->Text;
			String sTranslated = clsLanguage::Get(sText);
			if (sTranslated != sText)
			{
				oEdit->Text = sTranslated;
			}
			TranslateHint(oEdit);
		}
		else if (TMemo *oMemo = dynamic_cast<TMemo *>(poComponent))
		{
			TranslateLines(oMemo->Lines);
			TranslateHint(oMemo);
		}
		else if (TRichEdit *oRichEdit = dynamic_cast<TRichEdit *>(poComponent))
		{
			TranslateLines(oRichEdit->Lines);
			TranslateHint(oRichEdit);
		}
		else if (TSpinEdit *oSpinEdit = dynamic_cast<TSpinEdit *>(poComponent))
		{
			TranslateHint(oSpinEdit);
		}
		else if (TComboBox *oComboBox = dynamic_cast<TComboBox *>(poComponent))
		{
			TranslateHint(oComboBox);
			for (int iItem = oComboBox->Items->Count - 1; iItem >= 0; iItem--)
			{
				String sItem = oComboBox->Items->Strings[iItem];
				String sTranslated = clsLanguage::Get(sItem);
				//Replacing an item is expensive, so only do it if it changed
				if (sTranslated != sItem)
				{
					oComboBox->Items->Strings[iItem] = sTranslated;
				}
			}
		}
		else if (TImage *oImage = dynamic_cast<TImage *>(poComponent))
		{
			TranslateHint(oImage);
		}
	}

	//Childs
	for (int iComponent = poComponent->ComponentCount - 1; iComponent >= 0; iComponent--)
	{
		EnumerateControls(poComponent->Components[iComponent]);
	}
}



// ---------------------------------------------------------------------------
void __fastcall clsLanguage::TranslateForm(TForm *pfrmForm)
{
	//English is built-in, so there is nothing to change in any control
	if (!NothingToTranslate())
	{
		EnumerateControls(pfrmForm);
	}
}
