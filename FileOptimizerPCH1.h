// ---------------------------------------------------------------------------------------------------------------------------------------------------------------------------
//#ifndef FileOptimizerPCH1H
//#define FileOptimizerPCH1H
#pragma once


// ---------------------------------------------------------------------------------------------------------------------------------------------------------------------------
#define _CRT_NON_CONFORMING_WCSTOK


// ---------------------------------------------------------------------------------------------------------------------------------------------------------------------------
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <mem.h>
#include <time.h>
#include <limits.h>
#include <vcl.h>
#include <tchar.h>

#include <System.hpp>
#include <System.Actions.hpp>
#include <System.Classes.hpp>
#include <System.ImageList.hpp>

#include <System.StrUtils.hpp>
#include <System.SysUtils.hpp>
#include <Vcl.Controls.hpp>
#include <Clipbrd.hpp>
#include <Vcl.StdCtrls.hpp>
#include <Vcl.ComCtrls.hpp>
#include <Vcl.Forms.hpp>
#include <Vcl.ExtCtrls.hpp>
#include <Vcl.Graphics.hpp>
#include <Vcl.Grids.hpp>
#include <Vcl.Menus.hpp>
#include <Vcl.Dialogs.hpp>
#include <Vcl.Samples.Spin.hpp>
#include <Vcl.ActnList.hpp>
#include <Vcl.ImgList.hpp>
#include <Vcl.ToolWin.hpp>
#include <Vcl.OleCtrls.hpp>
#include <Vcl.Styles.hpp>
#include <Vcl.Themes.hpp>


#include <Vcl.Imaging.jpeg.hpp>

#include <windows.h>
#include <FileCtrl.hpp>
#include <IniFiles.hpp>
#include <SHDocVw.hpp>
#include <tlhelp32.h>


// ---------------------------------------------------------------------------------------------------------------------------------------------------------------------------
#if !defined(_ttof)
	#define _ttof _wtof
#endif

#if !defined(_argc)
	#define _argc __argc
#endif

#if !defined(_targv)
	#define _targv __targv
#endif

#if !defined(random)
	#define random(n) (lrand() % num)
#endif

#if !defined(randomize)
	#define randomize (srand((unsigned int) time(0)))
#endif


#pragma hdrstop
