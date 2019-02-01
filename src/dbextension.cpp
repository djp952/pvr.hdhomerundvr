//---------------------------------------------------------------------------
// Copyright (c) 2016-2019 Michael G. Brehm
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
//---------------------------------------------------------------------------

// string support
//
#include <string.h>

// libcurl
//
#define CURL_STATICLIB
#include <curl/curl.h>

// sqlite extension
//
#include <sqlite3ext.h>
SQLITE_EXTENSION_INIT1

#pragma warning(push, 4)

// database.cpp
//
void clean_filename(sqlite3_context* context, int argc, sqlite3_value** argv);
void decode_channel_id(sqlite3_context* context, int argc, sqlite3_value** argv);
void encode_channel_id(sqlite3_context* context, int argc, sqlite3_value** argv);
int epg_bestindex(sqlite3_vtab* vtab, sqlite3_index_info* info);
int epg_close(sqlite3_vtab_cursor* cursor);
int epg_column(sqlite3_vtab_cursor* cursor, sqlite3_context* context, int ordinal);
int epg_connect(sqlite3* instance, void* aux, int argc, const char* const* argv, sqlite3_vtab** vtab, char** err);
int epg_disconnect(sqlite3_vtab* vtab);
int epg_eof(sqlite3_vtab_cursor* cursor);
int epg_filter(sqlite3_vtab_cursor* cursor, int indexnum, char const* indexstr, int argc, sqlite3_value** argv);
int epg_next(sqlite3_vtab_cursor* cursor);
int epg_open(sqlite3_vtab* vtab, sqlite3_vtab_cursor** cursor);
int epg_rowid(sqlite3_vtab_cursor* cursor, sqlite_int64* rowid);
void fnv_hash(sqlite3_context* context, int argc, sqlite3_value** argv);
void generate_uuid(sqlite3_context* context, int argc, sqlite3_value** argv);
void get_channel_number(sqlite3_context* context, int argc, sqlite3_value** argv);
void get_episode_number(sqlite3_context* context, int argc, sqlite3_value** argv);
void get_season_number(sqlite3_context* context, int argc, sqlite3_value** argv);
void http_request(sqlite3_context* context, int argc, sqlite3_value** argv);
void url_encode(sqlite3_context* context, int argc, sqlite3_value** argv);

//---------------------------------------------------------------------------
// GLOBAL VARIABLES
//---------------------------------------------------------------------------

// g_epg_module
//
// Defines the entry points for the epg virtual table
static sqlite3_module g_epg_module = {

	0,						// iVersion
	nullptr,				// xCreate
	epg_connect,			// xConnect
	epg_bestindex,			// xBestIndex
	epg_disconnect,			// xDisconnect
	nullptr,				// xDestroy
	epg_open,				// xOpen
	epg_close,				// xClose
	epg_filter,				// xFilter
	epg_next,				// xNext
	epg_eof,				// xEof
	epg_column,				// xColumn
	epg_rowid,				// xRowid
	nullptr,				// xUpdate
	nullptr,				// xBegin
	nullptr,				// xSync
	nullptr,				// xCommit
	nullptr,				// xRollback
	nullptr,				// xFindMethod
	nullptr,				// xRename
	nullptr,				// xSavepoint
	nullptr,				// xRelease
	nullptr,				// xRollbackTo
	nullptr					// xShadowName
};

//---------------------------------------------------------------------------
// sqlite3_extension_init
//
// SQLite Extension Library entry point
//
// Arguments:
//
//	db		- SQLite database instance
//	errmsg	- On failure set to the error message (use sqlite3_malloc() to allocate)
//	api		- Pointer to the SQLite API functions

extern "C" int sqlite3_extension_init(sqlite3 *db, char** errmsg, const sqlite3_api_routines* api)
{
	SQLITE_EXTENSION_INIT2(api);

	*errmsg = nullptr;							// Initialize [out] variable

	// libcurl should be initialized prior to anything using it
	curl_global_init(CURL_GLOBAL_DEFAULT);

	// clean_filename
	//
	int result = sqlite3_create_function_v2(db, "clean_filename", 1, SQLITE_UTF8 | SQLITE_DETERMINISTIC, nullptr, clean_filename, nullptr, nullptr, nullptr);
	if(result != SQLITE_OK) { *errmsg = sqlite3_mprintf("Unable to register scalar function clean_filename"); return result; }

	// decode_channel_id
	//
	result = sqlite3_create_function_v2(db, "decode_channel_id", 1, SQLITE_UTF8 | SQLITE_DETERMINISTIC, nullptr, decode_channel_id, nullptr, nullptr, nullptr);
	if(result != SQLITE_OK) { *errmsg = sqlite3_mprintf("Unable to register scalar function decode_channel_id"); return result; }

	// encode_channel_id
	//
	result = sqlite3_create_function_v2(db, "encode_channel_id", 1, SQLITE_UTF8 | SQLITE_DETERMINISTIC, nullptr, encode_channel_id, nullptr, nullptr, nullptr);
	if(result != SQLITE_OK) { *errmsg = sqlite3_mprintf("Unable to register scalar function encode_channel_id"); return result; }

	// epg
	//
	// NOTE: Provides the address of the sqlite3_declare_vtab() function that is in the calling process.  This allows the
	// virtual table to behave properly when used via the extension module. Invoking the compiled-in version of the function
	// will attempt to access uninitialized global variables and crash.
	result = sqlite3_create_module_v2(db, "epg", &g_epg_module, reinterpret_cast<void*>(sqlite3_api->declare_vtab), nullptr);
	if(result != SQLITE_OK) { *errmsg = sqlite3_mprintf("Unable to register virtual table module epg"); return result; }

	// fnv_hash
	//
	result = sqlite3_create_function_v2(db, "fnv_hash", -1, SQLITE_UTF8 | SQLITE_DETERMINISTIC, nullptr, fnv_hash, nullptr, nullptr, nullptr);
	if(result != SQLITE_OK) { *errmsg = sqlite3_mprintf("Unable to register scalar function fnv_hash"); return result; }

	// generate_uuid (non-deterministic)
	//
	result = sqlite3_create_function_v2(db, "generate_uuid", 0, SQLITE_UTF8, nullptr, generate_uuid, nullptr, nullptr, nullptr);
	if(result != SQLITE_OK) { *errmsg = sqlite3_mprintf("Unable to register scalar function generate_uuid"); return result; }

	// get_channel_number
	//
	result = sqlite3_create_function_v2(db, "get_channel_number", 1, SQLITE_UTF8 | SQLITE_DETERMINISTIC, nullptr, get_channel_number, nullptr, nullptr, nullptr);
	if(result != SQLITE_OK) { *errmsg = sqlite3_mprintf("Unable to register scalar function get_channel_number"); return result; }

	// get_episode_number
	//
	result = sqlite3_create_function_v2(db, "get_episode_number", 1, SQLITE_UTF8 | SQLITE_DETERMINISTIC, nullptr, get_episode_number, nullptr, nullptr, nullptr);
	if(result != SQLITE_OK) { *errmsg = sqlite3_mprintf("Unable to register scalar function get_episode_number"); return result; }

	// get_season_number
	//
	result = sqlite3_create_function_v2(db, "get_season_number", 1, SQLITE_UTF8 | SQLITE_DETERMINISTIC, nullptr, get_season_number, nullptr, nullptr, nullptr);
	if(result != SQLITE_OK) { *errmsg = sqlite3_mprintf("Unable to register scalar function get_season_number"); return result; }

	// http_request (non-deterministic)
	//
	result = sqlite3_create_function_v2(db, "http_request", -1, SQLITE_UTF8, nullptr, http_request, nullptr, nullptr, nullptr);
	if(result != SQLITE_OK) { *errmsg = sqlite3_mprintf("Unable to register scalar function http_request"); return result; }

	// url_encode
	//
	result = sqlite3_create_function_v2(db, "url_encode", 1, SQLITE_UTF8 | SQLITE_DETERMINISTIC, nullptr, url_encode, nullptr, nullptr, nullptr);
	if(result != SQLITE_OK) { *errmsg = sqlite3_mprintf("Unable to register scalar function url_encode"); return result; }

	return SQLITE_OK;
}

//---------------------------------------------------------------------------

#pragma warning(pop)
