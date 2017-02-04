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

#ifndef __ADDONCALLBACKS_H_
#define __ADDONCALLBACKS_H_
#pragma once

#include <sys/types.h>
#include <sys/stat.h>
#include <stdint.h>
#include <kodi_vfs_types.h>

#pragma warning(push, 4)

//-----------------------------------------------------------------------------
// addoncallbacks
//
// Dynamically loaded function pointers from libXBMC_addon.dll

class addoncallbacks
{
public:

	// Instance Constructor
	//
	addoncallbacks(void* addonhandle);

	// Destructor
	//
	~addoncallbacks();

	//-----------------------------------------------------------------------
	// Public Type Declarations

	// addon_log_t
	//
	// from libXMBC_addon.h
	typedef enum addon_log
	{
		LOG_DEBUG,
		LOG_INFO,
		LOG_NOTICE,
		LOG_ERROR

	} addon_log_t;

	// queue_msg_t
	//
	// from libXMBC_addon.h
	typedef enum queue_msg
	{
		QUEUE_INFO,
		QUEUE_WARNING,
		QUEUE_ERROR

	} queue_msg_t;

	// CURLOPTIONTYPE
	//
	// from IFileTypes.h
	enum CURLOPTIONTYPE
	{
		CURL_OPTION_OPTION,
		CURL_OPTION_PROTOCOL,
		CURL_OPTION_CREDENTIALS,
		CURL_OPTION_HEADER
	};

	//-----------------------------------------------------------------------
	// Member Functions

	// CloseFile
	//
	// Closes an open file handle
	void CloseFile(void* handle) const;

	// CreateDirectory
	//
	// Creates a directory on the local file system
	bool CreateDirectory(char const* path) const;

	// CurlAddOption
	//
	// Adds an option to a CURL file representation
	bool CurlAddOption(void* file, CURLOPTIONTYPE type, char const* name, char const* value) const;

	// CurlCreate
	//
	// Creates a CURL representation of a file
	void* CurlCreate(char const* url) const;

	// CurlOpen
	//
	// Opens the file instance from a CURL file representation
	bool CurlOpen(void* file, unsigned int flags) const;

	// DeleteFile
	//
	// Deletes a file
	bool DeleteFile(char const* filename) const;

	// DirectoryExists
	//
	// Determines if a specific directory exists
	bool DirectoryExists(char const* path) const;

	// FreeDirectory
	//
	// Releases data obtained through GetDirectory()
	void FreeDirectory(VFSDirEntry* items, unsigned int count) const;

	// GetDirectory
	//
	// Gets a listing of all files within a directory
	bool GetDirectory(char const* path, char const* mask, VFSDirEntry** items, unsigned int* count) const;

	// GetFileChunkSize
	//
	// Gets the chunks size for the specified file handle
	int GetFileChunkSize(void* handle) const;

	// GetFileLength
	//
	// Gets the length of the file specified by the handle
	int64_t GetFileLength(void* handle) const;

	// GetFilePosition
	//
	// Gets the position within the specified file handle
	int64_t GetFilePosition(void* handle) const;

	// GetSetting
	//
	// Gets a setting for the current add-on
	bool GetSetting(char const* name, void* value) const;

	// Log
	//
	// Writes an entry into the Kodi application log
	void Log(addon_log_t level, char const* message) const;

	// OpenFile
	//
	// Open a handle to the specified file or URL
	void* OpenFile(char const* filename, unsigned int flags) const;

	// ReadFile
	//
	// Reads data from an open file handle
	int ReadFile(void* handle, void* buffer, size_t count) const;

	// SeekFile
	//
	// Sets the position within an open file handle
	int64_t SeekFile(void* file, int64_t offset, int whence) const;

private:

	addoncallbacks(const addoncallbacks&)=delete;
	addoncallbacks& operator=(const addoncallbacks&)=delete;

	//-----------------------------------------------------------------------
	// Type Declarations

	using XbmcCanOpenDirectoryFunc		= bool		(*)(void*, void*, char const*);
	using XbmcCloseFileFunc				= void		(*)(void*, void*, void*);
	using XbmcCreateDirectoryFunc		= bool		(*)(void*, void*, char const*);
	using XbmcCurlAddOptionFunc			= bool		(*)(void*, void*, void*, CURLOPTIONTYPE, char const*, char const*);
	using XbmcCurlCreateFunc			= void*		(*)(void*, void*, char const*);
	using XbmcCurlOpenFunc				= bool		(*)(void*, void*, void*, unsigned int);
	using XbmcDeleteFileFunc			= bool		(*)(void*, void*, char const*);
	using XbmcDirectoryExistsFunc		= bool		(*)(void*, void*, char const*);
	using XbmcFileExistsFunc			= bool		(*)(void*, void*, char const*, bool);
	using XbmcFlushFileFunc				= void		(*)(void*, void*, void*);
	using XbmcFreeDirectoryFunc			= void		(*)(void*, void*, VFSDirEntry*, unsigned int);
	using XbmcFreeStringFunc			= void		(*)(void*, void*, char*);
	using XbmcGetDirectoryFunc			= bool		(*)(void*, void*, char const*, char const*, VFSDirEntry**, unsigned int*);
	using XbmcGetDvdMenuLanguageFunc	= char*		(*)(void*, void*);
	using XbmcGetFileChunkSizeFunc		= int		(*)(void*, void*, void*);
	using XbmcGetFileDownloadSpeedFunc	= double	(*)(void*, void*, void*);
	using XbmcGetFileLengthFunc			= int64_t	(*)(void*, void*, void*);
	using XbmcGetFilePositionFunc		= int64_t	(*)(void*, void*, void*);
	using XbmcGetLocalizedStringFunc	= char*		(*)(void*, void*, int);
	using XbmcGetSettingFunc			= bool		(*)(void*, void*, char const*, void*);
	using XbmcLogFunc					= void		(*)(void*, void*, const addon_log_t, char const*);
	using XbmcOpenFileFunc				= void*		(*)(void*, void*, char const*, unsigned int);
	using XbmcOpenFileForWriteFunc		= void*		(*)(void*, void*, char const*, bool);
	using XbmcQueueNotificationFunc		= void		(*)(void*, void*, const queue_msg_t, char const*);
	using XbmcReadFileFunc				= intptr_t	(*)(void*, void*, void*, void*, size_t);
	using XbmcReadFileStringFunc		= bool		(*)(void*, void*, void*, char*, int);
	using XbmcRemoveDirectoryFunc		= bool		(*)(void*, void*, char const*);
	using XbmcRegisterMeFunc			= void*		(*)(void*);
	using XbmcSeekFileFunc				= int64_t	(*)(void*, void*, void*, int64_t, int);
	using XbmcStatFileFunc				= int		(*)(void*, void*, char const*, struct __stat64*);
	using XbmcTranslateSpecialFunc		= char*		(*)(void*, void*, char const*);
	using XbmcTruncateFileFunc			= int		(*)(void*, void*, void*, int64_t);
	using XbmcUnRegisterMeFunc			= void		(*)(void*, void*);
	using XbmcUnknownToUTF8Func			= char*		(*)(void*, void*, char const*);
	using XbmcWakeOnLanFunc				= bool		(*)(void*, void*, char const*);
	using XbmcWriteFileFunc				= intptr_t	(*)(void*, void*, void*, void const*, size_t);

	//-------------------------------------------------------------------------
	// Private Fields

	XbmcCanOpenDirectoryFunc		XbmcCanOpenDirectory;
	XbmcCloseFileFunc				XbmcCloseFile;
	XbmcCreateDirectoryFunc			XbmcCreateDirectory;
	XbmcCurlAddOptionFunc			XbmcCurlAddOption;
	XbmcCurlCreateFunc				XbmcCurlCreate;
	XbmcCurlOpenFunc				XbmcCurlOpen;
	XbmcDeleteFileFunc				XbmcDeleteFile;
	XbmcDirectoryExistsFunc			XbmcDirectoryExists;
	XbmcFileExistsFunc				XbmcFileExists;
	XbmcFlushFileFunc				XbmcFlushFile;
	XbmcFreeDirectoryFunc			XbmcFreeDirectory;
	XbmcFreeStringFunc				XbmcFreeString;
	XbmcGetDirectoryFunc			XbmcGetDirectory;
	XbmcGetDvdMenuLanguageFunc		XbmcGetDvdMenuLanguage;
	XbmcGetFileChunkSizeFunc		XbmcGetFileChunkSize;
	XbmcGetFileDownloadSpeedFunc	XbmcGetFileDownloadSpeed;
	XbmcGetFileLengthFunc			XbmcGetFileLength;
	XbmcGetFilePositionFunc			XbmcGetFilePosition;
	XbmcGetLocalizedStringFunc		XbmcGetLocalizedString;
	XbmcGetSettingFunc				XbmcGetSetting;
	XbmcLogFunc						XbmcLog;
	XbmcOpenFileFunc				XbmcOpenFile;
	XbmcOpenFileForWriteFunc		XbmcOpenFileForWrite;
	XbmcQueueNotificationFunc		XbmcQueueNotification;
	XbmcReadFileFunc				XbmcReadFile;
	XbmcReadFileStringFunc			XbmcReadFileString;
	XbmcRemoveDirectoryFunc			XbmcRemoveDirectory;
	XbmcRegisterMeFunc				XbmcRegisterMe;
	XbmcSeekFileFunc				XbmcSeekFile;
	XbmcStatFileFunc				XbmcStatFile;
	XbmcTranslateSpecialFunc		XbmcTranslateSpecial;
	XbmcTruncateFileFunc			XbmcTruncateFile;
	XbmcUnRegisterMeFunc			XbmcUnRegisterMe;
	XbmcUnknownToUTF8Func			XbmcUnknownToUTF8;
	XbmcWakeOnLanFunc				XbmcWakeOnLan;
	XbmcWriteFileFunc				XbmcWriteFile;

	//-------------------------------------------------------------------------
	// Member Variables

	void*				m_hmodule;			// Loaded DLL module handle
	void*				m_handle;			// Original add-on handle
	void*				m_callbacks;		// Registered callbacks handle
};

//-----------------------------------------------------------------------------

#pragma warning(pop)

#endif	// __ADDONCALLBACKS_H_
