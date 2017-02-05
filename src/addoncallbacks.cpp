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
#include "addoncallbacks.h"

#include <dlfcn.h>
#include <string>

#include "string_exception.h"

#pragma warning(push, 4)

// LIBXBMC_ADDON_MODULE
//
// Macro indicating the location of the libXBMC_addon module, which is architecture-specific
#if defined(_WINDOWS)
#define LIBXBMC_ADDON_MODULE "\\library.xbmc.addon\\libXBMC_addon.dll"
#elif defined(__x86_64__)
#define LIBXBMC_ADDON_MODULE "/library.xbmc.addon/libXBMC_addon-x86_64-linux.so"
#elif defined(__i386__)
#define LIBXBMC_ADDON_MODULE "/library.xbmc.addon/libXBMC_addon-i486-linux.so"
#else
#error addoncallbacks.cpp -- unknown architecture; only Windows, Linux i686 and Linux x86_64 are supported
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
// addoncallbacks Constructor
//
// Arguments:
//
//	addonhandle		- Kodi add-on handle provided to ADDON_Create()

addoncallbacks::addoncallbacks(void* addonhandle) : m_hmodule(nullptr), m_handle(addonhandle), m_callbacks(nullptr)
{
	// The path to the Kodi addon folder is embedded in the handle as a UTF-8 string
	char const* addonpath = *reinterpret_cast<char const**>(addonhandle);

	// Construct the path to the addon library based on the provided base path
	std::string addonmodule = std::string(addonpath) + LIBXBMC_ADDON_MODULE;

	// Attempt to load the PVR addon library dynamically, it should already be in the process
	m_hmodule = dlopen(addonmodule.c_str(), RTLD_LAZY);
	if(m_hmodule == nullptr) throw string_exception(std::string("failed to load dynamic addon library ") + addonmodule);

	try {

		// Acquire function pointers to all of the addon library callbacks
		XbmcCanOpenDirectory = GetFunctionPointer<XbmcCanOpenDirectoryFunc>(m_hmodule, "XBMC_can_open_directory");
		XbmcCloseFile = GetFunctionPointer<XbmcCloseFileFunc>(m_hmodule, "XBMC_close_file");
		XbmcCreateDirectory = GetFunctionPointer<XbmcCreateDirectoryFunc>(m_hmodule, "XBMC_create_directory");
		XbmcCurlAddOption = GetFunctionPointer<XbmcCurlAddOptionFunc>(m_hmodule, "XBMC_curl_add_option");
		XbmcCurlCreate = GetFunctionPointer<XbmcCurlCreateFunc>(m_hmodule, "XBMC_curl_create");
		XbmcCurlOpen = GetFunctionPointer<XbmcCurlOpenFunc>(m_hmodule, "XBMC_curl_open");
		XbmcDeleteFile = GetFunctionPointer<XbmcDeleteFileFunc>(m_hmodule, "XBMC_delete_file");
		XbmcDirectoryExists = GetFunctionPointer<XbmcDirectoryExistsFunc>(m_hmodule, "XBMC_directory_exists");
		XbmcFileExists = GetFunctionPointer<XbmcFileExistsFunc>(m_hmodule, "XBMC_file_exists");
		XbmcFlushFile = GetFunctionPointer<XbmcFlushFileFunc>(m_hmodule, "XBMC_flush_file");
		XbmcFreeDirectory = GetFunctionPointer<XbmcFreeDirectoryFunc>(m_hmodule, "XBMC_free_directory");
		XbmcFreeString = GetFunctionPointer<XbmcFreeStringFunc>(m_hmodule, "XBMC_free_string");
		XbmcGetDirectory = GetFunctionPointer<XbmcGetDirectoryFunc>(m_hmodule, "XBMC_get_directory");
		XbmcGetDvdMenuLanguage = GetFunctionPointer<XbmcGetDvdMenuLanguageFunc>(m_hmodule, "XBMC_get_dvd_menu_language");
		XbmcGetFileChunkSize = GetFunctionPointer<XbmcGetFileChunkSizeFunc>(m_hmodule, "XBMC_get_file_chunk_size");
		XbmcGetFileDownloadSpeed = GetFunctionPointer<XbmcGetFileDownloadSpeedFunc>(m_hmodule, "XBMC_get_file_download_speed");
		XbmcGetFileLength = GetFunctionPointer<XbmcGetFileLengthFunc>(m_hmodule, "XBMC_get_file_length");
		XbmcGetFilePosition = GetFunctionPointer<XbmcGetFilePositionFunc>(m_hmodule, "XBMC_get_file_position");
		XbmcGetLocalizedString = GetFunctionPointer<XbmcGetLocalizedStringFunc>(m_hmodule, "XBMC_get_localized_string");
		XbmcGetSetting = GetFunctionPointer<XbmcGetSettingFunc>(m_hmodule, "XBMC_get_setting");
		XbmcLog = GetFunctionPointer<XbmcLogFunc>(m_hmodule, "XBMC_log");
		XbmcOpenFile = GetFunctionPointer<XbmcOpenFileFunc>(m_hmodule, "XBMC_open_file");
		XbmcOpenFileForWrite = GetFunctionPointer<XbmcOpenFileForWriteFunc>(m_hmodule, "XBMC_open_file_for_write");
		XbmcQueueNotification = GetFunctionPointer<XbmcQueueNotificationFunc>(m_hmodule, "XBMC_queue_notification");
		XbmcReadFile = GetFunctionPointer<XbmcReadFileFunc>(m_hmodule, "XBMC_read_file");
		XbmcReadFileString = GetFunctionPointer<XbmcReadFileStringFunc>(m_hmodule, "XBMC_read_file_string");
		XbmcRemoveDirectory = GetFunctionPointer<XbmcRemoveDirectoryFunc>(m_hmodule, "XBMC_remove_directory");
		XbmcRegisterMe = GetFunctionPointer<XbmcRegisterMeFunc>(m_hmodule, "XBMC_register_me");
		XbmcSeekFile = GetFunctionPointer<XbmcSeekFileFunc>(m_hmodule, "XBMC_seek_file");
		XbmcStatFile = GetFunctionPointer<XbmcStatFileFunc>(m_hmodule, "XBMC_stat_file");
		XbmcTranslateSpecial = GetFunctionPointer<XbmcTranslateSpecialFunc>(m_hmodule, "XBMC_translate_special");
		XbmcTruncateFile = GetFunctionPointer<XbmcTruncateFileFunc>(m_hmodule, "XBMC_truncate_file");
		XbmcUnRegisterMe = GetFunctionPointer<XbmcUnRegisterMeFunc>(m_hmodule, "XBMC_unregister_me");
		XbmcUnknownToUTF8 = GetFunctionPointer<XbmcUnknownToUTF8Func>(m_hmodule, "XBMC_unknown_to_utf8");
		XbmcWakeOnLan = GetFunctionPointer<XbmcWakeOnLanFunc>(m_hmodule, "XBMC_wake_on_lan");
		XbmcWriteFile = GetFunctionPointer<XbmcWriteFileFunc>(m_hmodule, "XBMC_write_file");

		// Register with the Kodi addon library
		m_callbacks = XbmcRegisterMe(m_handle);
		if(m_callbacks == nullptr) throw string_exception("Failed to register addoncallbacks handle");
	}

	// Ensure that the library is released on any constructor exceptions
	catch(std::exception&) { dlclose(m_hmodule); m_hmodule = nullptr; throw; }
}

//-----------------------------------------------------------------------------
// addoncallbacks Destructor

addoncallbacks::~addoncallbacks()
{
	XbmcUnRegisterMe(m_handle, m_callbacks);
	dlclose(m_hmodule);
}

//-----------------------------------------------------------------------------
// addoncallbacks::CloseFile
//
// Closes an open file handle
//
// Arguments:
//
//	handle		- File handle returned from an addon callback function

void addoncallbacks::CloseFile(void* handle) const
{
	assert((XbmcCloseFile) && (m_handle) && (m_callbacks));
	return XbmcCloseFile(m_handle, m_callbacks, handle);
}

//-----------------------------------------------------------------------------
// addoncallbacks::CreateDirectory
//
// Creates a directory on the local file system
//
// Arguments:
//
//	path		- Path of the directory to be created

bool addoncallbacks::CreateDirectory(char const* path) const
{
	assert((XbmcCreateDirectory) && (m_handle) && (m_callbacks));
	return XbmcCreateDirectory(m_handle, m_callbacks, path);
}

//-----------------------------------------------------------------------------
// addoncallbacks::CurlAddOption
//
// Adds an option to a CURL representation
//
// Arguments:
//
//	file	- CURL file representation
//	type	- Type of option to be added
//	name	- Name of the option
//	value	- Value of the option

bool addoncallbacks::CurlAddOption(void* file, CURLOPTIONTYPE type, char const* name, char const* value) const
{
	assert((XbmcCurlAddOption) && (m_handle) && (m_callbacks));
	return XbmcCurlAddOption(m_handle, m_callbacks, file, type, name, value);
}

//-----------------------------------------------------------------------------
// addoncallbacks::CurlCreate
//
// Creates a CURL representation
//
// Arguments:
//
//	url		- Target URL string

void* addoncallbacks::CurlCreate(char const* url) const
{
	assert((XbmcCurlCreate) && (m_handle) && (m_callbacks));
	return XbmcCurlCreate(m_handle, m_callbacks, url);
}

//-----------------------------------------------------------------------------
// addoncallbacks::CurlOpen
//
// Opens the file instance from a CURL representation
//
// Arguments:
//
//	file	- CURL representation created by CurlCreate
//	flags	- Open operation flags

bool addoncallbacks::CurlOpen(void* file, unsigned int flags) const
{
	assert((XbmcCurlOpen) && (m_handle) && (m_callbacks));
	return XbmcCurlOpen(m_handle, m_callbacks, file, flags);
}
	
//-----------------------------------------------------------------------------
// addoncallbacks::DeleteFile
//
// Deletes a file
//
// Arguments:
//
//	filename	- Name of the file to be deleted

bool addoncallbacks::DeleteFile(char const* filename) const
{
	assert((XbmcDeleteFile) && (m_handle) && (m_callbacks));
	return XbmcDeleteFile(m_handle, m_callbacks, filename);
}
	
//-----------------------------------------------------------------------------
// addoncallbacks::DirectoryExists
//
// Determines if a specific directory exists
//
// Arguments:
//
//	path		- Path of the directory to test

bool addoncallbacks::DirectoryExists(char const* path) const
{
	assert((XbmcDirectoryExists) && (m_handle) && (m_callbacks));
	return XbmcDirectoryExists(m_handle, m_callbacks, path);
}

//-----------------------------------------------------------------------------
// addoncallbacks::FreeDirectory
//
// Releases data obtained through GetDirectory()
//
// Arguments:
//
//	items		- Pointer returned from GetDirectory
//	count		- Number of items returned from GetDirectory

void addoncallbacks::FreeDirectory(VFSDirEntry* items, unsigned int count) const
{
	assert((XbmcFreeDirectory) && (m_handle) && (m_callbacks));
	return XbmcFreeDirectory(m_handle, m_callbacks, items, count);
}

//-----------------------------------------------------------------------------
// addoncallbacks::GetDirectory
//
// Gets a listing of all files within a directory
//
// Arguments:
//
//	path		- Target directory path
//	mask		- Filespec/mask on which to search
//	items		- On success, contains an array of directory items
//	count		- Number of items returned in the array

bool addoncallbacks::GetDirectory(char const* path, char const* mask, VFSDirEntry** items, unsigned int* count) const
{
	assert((XbmcGetDirectory) && (m_handle) && (m_callbacks));
	return XbmcGetDirectory(m_handle, m_callbacks, path, mask, items, count);
}
	
//-----------------------------------------------------------------------------
// addoncallbacks::GetFileChunkSize
//
// Gets the chunk size for the specified file handle
//
// Arguments:
//
//	handle			- File handle returned from an addon callback function

int addoncallbacks::GetFileChunkSize(void* handle) const
{
	assert((XbmcGetFileChunkSize) && (m_handle) && (m_callbacks));
	return XbmcGetFileChunkSize(m_handle, m_callbacks, handle);
}

//-----------------------------------------------------------------------------
// addoncallbacks::GetFileLength
//
// Gets the length of the file specified by the handle
//
// Arguments:
//
//	handle		- File handle returned from an addon callback function

int64_t addoncallbacks::GetFileLength(void* handle) const
{
	assert((XbmcGetFileLength) && (m_handle) && (m_callbacks));
	return XbmcGetFileLength(m_handle, m_callbacks, handle);
}

//-----------------------------------------------------------------------------
// addoncallbacks::GetFilePosition
//
// Gets the position within the current file
//
// Arguments:
//
//	handle			- File handle returned from an addon callback function

int64_t addoncallbacks::GetFilePosition(void* handle) const
{
	assert((XbmcGetFilePosition) && (m_handle) && (m_callbacks));
	return XbmcGetFilePosition(m_handle, m_callbacks, handle);
}

//-----------------------------------------------------------------------------
// addoncallbacks::GetFilePosition
//
// Retrieves a setting for the current addon
//
// Arguments:
//
//	name		- Name of the setting to be retrieved
//	value		- On success contains a pointer to the value

bool addoncallbacks::GetSetting(char const* name, void* value) const
{
	assert((XbmcGetSetting) && (m_handle) && (m_callbacks));
	return XbmcGetSetting(m_handle, m_callbacks, name, value);
}

//-----------------------------------------------------------------------------
// addoncallbacks::Log
//
// Writes an entry into the Kodi application log
//
// Arguments:
//
//	level		- Log level
//	message		- Log message to be written

void addoncallbacks::Log(addon_log_t level, char const* message) const
{
	assert((XbmcLog) && (m_handle) && (m_callbacks));
	return XbmcLog(m_handle, m_callbacks, level, message);
}
	
//-----------------------------------------------------------------------------
// addoncallbacks::OpenFile
//
// Open a handle to the specified file or URL
//
// Arguments:
//
//	filename		- Path to the file to be opened
//	flags			- Operation flags

void* addoncallbacks::OpenFile(char const* filename, unsigned int flags) const
{
	assert((XbmcOpenFile) && (m_handle) && (m_callbacks));
	return XbmcOpenFile(m_handle, m_callbacks, filename, flags);
}

//-----------------------------------------------------------------------------
// addoncallbacks::ReadFile
//
// Reads data from an open file handle
//
// Arguments:
//
//	handle			- File handle returned from an addon callback function
//	buffer			- Destination buffer to receive the data
//	count			- Size of the destination buffer

int addoncallbacks::ReadFile(void* handle, void* buffer, size_t count) const
{
	assert((XbmcReadFile) && (m_handle) && (m_callbacks));
	return XbmcReadFile(m_handle, m_callbacks, handle, buffer, count);
}
	
//-----------------------------------------------------------------------------
// addoncallbacks::SeekFile
//
// Sets the position within an open file handle
//
// Arguments:
//
//	file			- File handle return from a callback function
//	offset		- Offset within the file to seek, relative to whence
//	whence		- Position from which to seek

int64_t addoncallbacks::SeekFile(void* file, int64_t offset, int whence) const
{
	assert((XbmcSeekFile) && (m_handle) && (m_callbacks));
	return XbmcSeekFile(m_handle, m_callbacks, file, offset, whence);
}
	
//-----------------------------------------------------------------------------

#pragma warning(pop)
