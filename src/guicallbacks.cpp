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

#include "stdafx.h"
#include "guicallbacks.h"

#include <dlfcn.h>
#include <string>

#include "string_exception.h"

#pragma warning(push, 4)

// LIBKODI_GUILIB_MODULE
//
// Macro indicating the location of the libKODI_guilib module, which is architecture-specific
#if defined(_WINDOWS)
#define LIBKODI_GUILIB_MODULE "\\library.kodi.guilib\\libKODI_guilib.dll"
#elif defined(__x86_64__) && !defined(__ANDROID__)
#define LIBKODI_GUILIB_MODULE "/library.kodi.guilib/libKODI_guilib-x86_64-linux.so"
#elif defined(__i386__) && !defined(__ANDROID__)
#define LIBKODI_GUILIB_MODULE "/library.kodi.guilib/libKODI_guilib-i486-linux.so"
#elif defined(__arm__) && !defined(__ANDROID__)
#define LIBKODI_GUILIB_MODULE "/library.kodi.guilib/libKODI_guilib-arm.so"
#elif defined(__aarch64__) && !defined(__ANDROID__)
#define LIBKODI_GUILIB_MODULE "/library.kodi.guilib/libKODI_guilib-aarch64.so"
#elif defined(__ANDROID__) && defined(__arm__)
#define LIBKODI_GUILIB_MODULE "/libKODI_guilib-arm.so"
#elif defined(__ANDROID__) && (defined __aarch64__)
#define LIBKODI_GUILIB_MODULE "/libKODI_guilib-aarch64.so"
#elif defined(__ANDROID__) && defined(__i386__)
#define LIBKODI_GUILIB_MODULE "/libKODI_guilib-i486-linux.so"
#else
#error guicallbacks.cpp -- unsupported architecture
#endif

// GetFunctionPointer (local)
//
// Retrieves a function pointer from the specified module
template <typename _funcptr> 
static inline _funcptr GetFunctionPointer(void* module, char const* name)
{
	_funcptr ptr = reinterpret_cast<_funcptr>(dlsym(module, name));
	if(ptr == nullptr) throw string_exception(std::string("failed to get entry point for function ") + std::string(name));

	return ptr;
}

//-----------------------------------------------------------------------------
// guicallbacks Constructor
//
// Arguments:
//
//	addonhandle		- Add-on handle passed into ADDON_Create()

guicallbacks::guicallbacks(void* addonhandle) : m_hmodule(nullptr), m_handle(addonhandle), m_callbacks(nullptr)
{
	// The path to the Kodi addon folder is embedded in the handle as a UTF-8 string
	char const* addonpath = *reinterpret_cast<char const**>(addonhandle);

	// Construct the path to the guilib library based on the provided base path
	std::string guimodule = std::string(addonpath) + LIBKODI_GUILIB_MODULE;

	// Attempt to load the guilib library dynamically, it should already be in the process
	m_hmodule = dlopen(guimodule.c_str(), RTLD_LAZY);
	if(m_hmodule == nullptr) throw string_exception(std::string("failed to load dynamic guilib library ") + guimodule);

	try {

		// Acquire function pointers to all of the guilib library callbacks
		GuiDialogOkShowAndGetInputLineText = GetFunctionPointer<GuiDialogOkShowAndGetInputLineTextFunc>(m_hmodule, "GUI_dialog_ok_show_and_get_input_line_text");
		GuiDialogOkShowAndGetInputSingleText = GetFunctionPointer<GuiDialogOkShowAndGetInputSingleTextFunc>(m_hmodule, "GUI_dialog_ok_show_and_get_input_single_text");
		GuiDialogSelect = GetFunctionPointer<GuiDialogSelectFunc>(m_hmodule, "GUI_dialog_select");
		GuiDialogTextViewer = GetFunctionPointer<GuiDialogTextViewerFunc>(m_hmodule, "GUI_dialog_text_viewer");
		GuiRegisterMe = GetFunctionPointer<GuiRegisterMeFunc>(m_hmodule, "GUI_register_me");
		GuiUnRegisterMe = GetFunctionPointer<GuiUnRegisterMeFunc>(m_hmodule, "GUI_unregister_me");

		// Register with the guilib library
		m_callbacks = GuiRegisterMe(m_handle);
		if(m_callbacks == nullptr) throw string_exception("Failed to register guicallbacks handle");
	}

	// Ensure that the library is released on any constructor exceptions
	catch(std::exception&) { dlclose(m_hmodule); m_hmodule = nullptr; throw; }
}

//-----------------------------------------------------------------------------
// guicallbacks Destructor

guicallbacks::~guicallbacks()
{
	GuiUnRegisterMe(m_handle, m_callbacks);
	dlclose(m_hmodule);
}

//-----------------------------------------------------------------------------
// guicallbacks::DialogOK
//
// Shows an OK dialog box
//
// Arguments:
//
//	heading		- Dialog heading/title
//	text		- Dialog body text

void guicallbacks::DialogOK(char const* heading, char const* text) const
{
	assert((GuiDialogOkShowAndGetInputSingleText) && (m_handle) && (m_callbacks));
	GuiDialogOkShowAndGetInputSingleText(m_handle, m_callbacks, heading, text);
}
	
//-----------------------------------------------------------------------------
// guicallbacks::DialogOK
//
// Shows an OK dialog box
//
// Arguments:
//
//	heading		- Dialog heading/title
//	line0		- Dialog body text line 0
//	line1		- Dialog body text line 1
//	line2		- Dialog body text line 2

void guicallbacks::DialogOK(char const* heading, char const* line0, char const* line1, char const* line2) const
{
	assert((GuiDialogOkShowAndGetInputLineText) && (m_handle) && (m_callbacks));
	GuiDialogOkShowAndGetInputLineText(m_handle, m_callbacks, heading, line0, line1, line2);
}
	
//-----------------------------------------------------------------------------
// guicallbacks::DialogSelect
//
// Displays a select dialog box
//
// Arguments:
//
//	heading		- Dialog heading/title
//	entries		- Array of entries to be selected from
//	size		- Number of elements in the entries array
//	selected	- Index of item to be selected by default

int guicallbacks::DialogSelect(char const* heading, char const* entries[], unsigned int size, int selected)
{
	assert((GuiDialogSelect) && (m_handle) && (m_callbacks));
	return GuiDialogSelect(m_handle, m_callbacks, heading, entries, size, selected);
}
	
//-----------------------------------------------------------------------------
// guicallbacks::DialogTextViewer
//
// Displays a text viewer dialog
//
// Arguments:
//
//	heading		- Dialog heading/title
//	text		- Dialog body text

void guicallbacks::DialogTextViewer(char const* heading, char const* text) const
{
	assert((GuiDialogTextViewer) && (m_handle) && (m_callbacks));
	GuiDialogTextViewer(m_handle, m_callbacks, heading, text);
}
	
//-----------------------------------------------------------------------------

#pragma warning(pop)
