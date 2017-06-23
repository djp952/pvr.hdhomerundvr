//-----------------------------------------------------------------------------
// Copyright (c) 2017 Michael G. Brehm
// 
// Permission is hereby granted, free of charge, to any person obtaining a copy
// of this software and associated documentation files (the "Software"), to deal
// in the Software without restriction, including without limitation the rights
// to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
// copies of the Software, and to permit persons to whom the Software is
// furnished to do so, subject to the following conditions:
// 
// The above copyright notice and this permission notice shall be included in all
// copies or substantial portions of the Software.
// 
// THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
// IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
// FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
// AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
// LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
// OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
// SOFTWARE.
//-----------------------------------------------------------------------------

#ifndef __GUICALLBACKS_H_
#define __GUICALLBACKS_H_
#pragma once

#pragma warning(push, 4)

// libKODI_guilib.h
//
#define KODI_GUILIB_API_VERSION				"5.11.0"
#define KODI_GUILIB_MIN_API_VERSION			"5.10.0"

//-----------------------------------------------------------------------------
// guicallbacks
//
// Dynamically loaded function pointers from libKODI_guilib.dll

class guicallbacks
{
public:

	// Instance Constructor
	//
	guicallbacks(void* addonhandle);

	// Destructor
	//
	~guicallbacks();

	//-----------------------------------------------------------------------
	// Member Functions

	// DialogOk
	//
	// Displays an OK dialog box
	void DialogOK(char const* heading, char const* text) const;
	void DialogOK(char const* heading, char const* line0, char const* line1, char const* line2) const;

	// DialogSelect
	//
	// Displays a select dialog box
	int DialogSelect(char const* heading, char const* entries[], unsigned int size, int selected);
	
	// DialogTextViewer
	//
	// Displays a text viewer dialog
	void DialogTextViewer(char const* heading, char const* text) const;

private:

	guicallbacks(const guicallbacks&)=delete;
	guicallbacks& operator=(const guicallbacks&)=delete;

	//-----------------------------------------------------------------------
	// Type Declarations

	using GuiDialogOkShowAndGetInputLineTextFunc	= void	(*)(void*, void*, char const*, char const*, char const*, char const*);
	using GuiDialogOkShowAndGetInputSingleTextFunc	= void	(*)(void*, void*, char const*, char const*);
	using GuiDialogSelectFunc						= int	(*)(void*, void*, char const*, char const*[], unsigned int, int);
	using GuiDialogTextViewerFunc					= void	(*)(void*, void*, char const*, char const*);
	using GuiRegisterMeFunc							= void*	(*)(void*);
	using GuiUnRegisterMeFunc						= void	(*)(void*, void*);

	//-------------------------------------------------------------------------
	// Private Fields

	GuiDialogOkShowAndGetInputLineTextFunc		GuiDialogOkShowAndGetInputLineText;
	GuiDialogOkShowAndGetInputSingleTextFunc	GuiDialogOkShowAndGetInputSingleText;
	GuiDialogSelectFunc							GuiDialogSelect;
	GuiDialogTextViewerFunc						GuiDialogTextViewer;
	GuiRegisterMeFunc							GuiRegisterMe;
	GuiUnRegisterMeFunc							GuiUnRegisterMe;

	//-------------------------------------------------------------------------
	// Member Variables

	void*				m_hmodule;			// Loaded DLL module handle
	void*				m_handle;			// Original add-on handle
	void*				m_callbacks;		// Registered callbacks handle
};

//-----------------------------------------------------------------------------

#pragma warning(pop)

#endif	// __GUICALLBACKS_H_
