//---------------------------------------------------------------------------
// Copyright (c) 2018 Michael G. Brehm
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

// FUNCTION PROTOTYPES
//
void clean_filename(sqlite3_context* context, int argc, sqlite3_value** argv);
void decode_channel_id(sqlite3_context* context, int argc, sqlite3_value** argv);
void encode_channel_id(sqlite3_context* context, int argc, sqlite3_value** argv);
void fnv_hash(sqlite3_context* context, int argc, sqlite3_value** argv);
void generate_uuid(sqlite3_context* context, int argc, sqlite3_value** argv);
void get_channel_number(sqlite3_context* context, int argc, sqlite3_value** argv);
void get_episode_number(sqlite3_context* context, int argc, sqlite3_value** argv);
void get_season_number(sqlite3_context* context, int argc, sqlite3_value** argv);
void http_request(sqlite3_context* context, int argc, sqlite3_value** argv);
void url_encode(sqlite3_context* context, int argc, sqlite3_value** argv);

#pragma warning(push, 4)

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
	if(result != SQLITE_OK) {
	
		*errmsg = reinterpret_cast<char*>(sqlite3_malloc(512));
		snprintf(*errmsg, 512, "Unable to register scalar function clean_filename");
		return result;
	}

	// decode_channel_id
	//
	result = sqlite3_create_function_v2(db, "decode_channel_id", 1, SQLITE_UTF8 | SQLITE_DETERMINISTIC, nullptr, decode_channel_id, nullptr, nullptr, nullptr);
	if(result != SQLITE_OK) {
	
		*errmsg = reinterpret_cast<char*>(sqlite3_malloc(512));
		snprintf(*errmsg, 512, "Unable to register scalar function decode_channel_id");
		return result;
	}

	// encode_channel_id
	//
	result = sqlite3_create_function_v2(db, "encode_channel_id", 1, SQLITE_UTF8 | SQLITE_DETERMINISTIC, nullptr, encode_channel_id, nullptr, nullptr, nullptr);
	if(result != SQLITE_OK) {
	
		*errmsg = reinterpret_cast<char*>(sqlite3_malloc(512));
		snprintf(*errmsg, 512, "Unable to register scalar function encode_channel_id");
		return result;
	}

	// fnv_hash
	//
	result = sqlite3_create_function_v2(db, "fnv_hash", -1, SQLITE_UTF8 | SQLITE_DETERMINISTIC, nullptr, fnv_hash, nullptr, nullptr, nullptr);
	if(result != SQLITE_OK) {
	
		*errmsg = reinterpret_cast<char*>(sqlite3_malloc(512));
		snprintf(*errmsg, 512, "Unable to register scalar function fnv_hash");
		return result;
	}

	// generate_uuid (non-deterministic)
	//
	result = sqlite3_create_function_v2(db, "generate_uuid", 0, SQLITE_UTF8, nullptr, generate_uuid, nullptr, nullptr, nullptr);
	if(result != SQLITE_OK) {
	
		*errmsg = reinterpret_cast<char*>(sqlite3_malloc(512));
		snprintf(*errmsg, 512, "Unable to register scalar function generate_uuid");
		return result;
	}

	// get_channel_number
	//
	result = sqlite3_create_function_v2(db, "get_channel_number", 1, SQLITE_UTF8 | SQLITE_DETERMINISTIC, nullptr, get_channel_number, nullptr, nullptr, nullptr);
	if(result != SQLITE_OK) {
	
		*errmsg = reinterpret_cast<char*>(sqlite3_malloc(512));
		snprintf(*errmsg, 512, "Unable to register scalar function get_channel_number");
		return result;
	}

	// get_episode_number
	//
	result = sqlite3_create_function_v2(db, "get_episode_number", 1, SQLITE_UTF8 | SQLITE_DETERMINISTIC, nullptr, get_episode_number, nullptr, nullptr, nullptr);
	if(result != SQLITE_OK) {
	
		*errmsg = reinterpret_cast<char*>(sqlite3_malloc(512));
		snprintf(*errmsg, 512, "Unable to register scalar function get_episode_number");
		return result;
	}

	// get_season_number
	//
	result = sqlite3_create_function_v2(db, "get_season_number", 1, SQLITE_UTF8 | SQLITE_DETERMINISTIC, nullptr, get_season_number, nullptr, nullptr, nullptr);
	if(result != SQLITE_OK) {
	
		*errmsg = reinterpret_cast<char*>(sqlite3_malloc(512));
		snprintf(*errmsg, 512, "Unable to register scalar function get_season_number");
		return result;
	}

	// http_request (non-deterministic)
	//
	result = sqlite3_create_function_v2(db, "http_request", -1, SQLITE_UTF8, nullptr, http_request, nullptr, nullptr, nullptr);
	if(result != SQLITE_OK) {
	
		*errmsg = reinterpret_cast<char*>(sqlite3_malloc(512));
		snprintf(*errmsg, 512, "Unable to register scalar function http_request");
		return result;
	}

	// url_encode
	//
	result = sqlite3_create_function_v2(db, "url_encode", 1, SQLITE_UTF8 | SQLITE_DETERMINISTIC, nullptr, url_encode, nullptr, nullptr, nullptr);
	if(result != SQLITE_OK) {
	
		*errmsg = reinterpret_cast<char*>(sqlite3_malloc(512));
		snprintf(*errmsg, 512, "Unable to register scalar function url_encode");
		return result;
	}

	return SQLITE_OK;
}

//---------------------------------------------------------------------------

#pragma warning(pop)
