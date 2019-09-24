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

#include "stdafx.h"

#include <algorithm>
#include <list>
#include <memory>
#include <rapidjson/document.h>
#include <rapidjson/stringbuffer.h>
#include <rapidjson/writer.h>
#include <sqlite3ext.h>
#include <string>
#include <uuid/uuid.h>
#include <xbmc_pvr_types.h>
#include <vector>
#include <version.h>

#include "curlshare.h"
#include "dbtypes.h"
#include "http_exception.h"
#include "string_exception.h"

SQLITE_EXTENSION_INIT1

#pragma warning(push, 4)

//---------------------------------------------------------------------------
// FUNCTION PROTOTYPES
//---------------------------------------------------------------------------

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

static void http_request(sqlite3_context* context, sqlite3_value* urlvalue, sqlite3_value* postvalue, sqlite3_value* defaultvalue);

//---------------------------------------------------------------------------
// TYPE DECLARATIONS
//---------------------------------------------------------------------------

// byte_string
//
// Alias for an std::basic_string<> of bytes
typedef std::basic_string<uint8_t> byte_string;

// curl_writefunction
//
// Function pointer for a CURL write function implementation
typedef size_t(*curl_writefunction)(void const*, size_t, size_t, void*);

// epg_vtab_columns
//
// Constants indicating the epg virtual table column ordinals
enum epg_vtab_columns {

	value		= 0,		// value text
	deviceauth,				// deviceauth text hidden
	channel,				// channel integer hidden
	starttime,				// starttime integer hidden
	endtime,				// endtime integer hidden
};

// epg_vtab
//
// Subclassed version of sqlite3_vtab for the epg virtual table
struct epg_vtab : public sqlite3_vtab 
{
	// Instance Constructor
	//
	epg_vtab() { memset(static_cast<sqlite3_vtab*>(this), 0, sizeof(sqlite3_vtab)); }
};

// epg_vtab_cursor
//
// Subclassed version of sqlite3_vtab_cursor for the epg virtual table
struct epg_vtab_cursor : public sqlite3_vtab_cursor
{
	// Instance Constructor
	//
	epg_vtab_cursor() { memset(static_cast<sqlite3_vtab_cursor*>(this), 0, sizeof(sqlite3_vtab_cursor)); }

	// Fields
	//
	std::string			deviceauth;			// deviceauth string
	std::string			channel;			// channel number
	time_t				starttime = 0;		// start time
	time_t				endtime = 0;		// end time
	size_t				currentrow = 0;		// current row

	std::vector<byte_string> rows;			// returned rows
};

// json_get_aggregate_state
//
// Used as the state object for the json_get_aggregate function
typedef std::vector<std::tuple<std::string, std::string>> json_get_aggregate_state;

//---------------------------------------------------------------------------
// GLOBAL VARIABLES
//---------------------------------------------------------------------------

// g_curlshare
//
// Global curlshare instance to share resources among all cURL connections
static curlshare g_curlshare;

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

// g_useragent
//
// Static string to use as the User-Agent for database driven HTTP requests
static std::string g_useragent = "Kodi-PVR/" + std::string(ADDON_INSTANCE_VERSION_PVR) + " " + VERSION_PRODUCTNAME_ANSI + "/" + VERSION_VERSION3_ANSI;

//---------------------------------------------------------------------------
// clean_filename
//
// SQLite scalar function to clean invalid chars from a file name
//
// Arguments:
//
//	context		- SQLite context object
//	argc		- Number of supplied arguments
//	argv		- Argument values

void clean_filename(sqlite3_context* context, int argc, sqlite3_value** argv)
{
	if((argc != 1) || (argv[0] == nullptr)) return sqlite3_result_error(context, "invalid argument", -1);

	// Null or zero-length input string results in a zero-length output string
	const char* str = reinterpret_cast<const char*>(sqlite3_value_text(argv[0]));
	if((str == nullptr) || (*str == 0)) return sqlite3_result_text(context, "", -1, SQLITE_STATIC);

	// isinvalid_char
	//
	// Returns -1 if the specified character is invalid for a filename on Windows or Unix
	auto isinvalid_char = [](char const& ch) -> bool { 

		// Exclude characters with a value between 0 and 31 (inclusive) as well as various
		// specific additional characters: [",<,>,|,:,*,?,\,/]
		return (((static_cast<int>(ch) >= 0) && (static_cast<int>(ch) <= 31)) ||
			(ch == '"') || (ch == '<') || (ch == '>') || (ch == '|') || (ch == ':') ||
			(ch == '*') || (ch == '?') || (ch == '\\') || (ch == '/'));
	};

	std::string output(str);
	output.erase(std::remove_if(output.begin(), output.end(), isinvalid_char), output.end());

	// Return the generated string as a transient value (needs to be copied)
	return sqlite3_result_text(context, output.c_str(), -1, SQLITE_TRANSIENT);
}

//---------------------------------------------------------------------------
// decode_channel_id
//
// SQLite scalar function to reverse encode_channel_id
//
// Arguments:
//
//	context		- SQLite context object
//	argc		- Number of supplied arguments
//	argv		- Argument values

void decode_channel_id(sqlite3_context* context, int argc, sqlite3_value** argv)
{
	union channelid			channelid;			// Encoded channel identifier

	if((argc != 1) || (argv[0] == nullptr)) return sqlite3_result_error(context, "invalid arguments", -1);
	
	// Null input results in "0"
	if(sqlite3_value_type(argv[0]) == SQLITE_NULL) return sqlite3_result_text(context, "0", -1, SQLITE_STATIC);

	// Convert the input encoded channelid back into a string
	channelid.value = static_cast<unsigned int>(sqlite3_value_int(argv[0]));
	char* result = (channelid.parts.subchannel > 0) ? sqlite3_mprintf("%u.%u", channelid.parts.channel, channelid.parts.subchannel) :
		sqlite3_mprintf("%u", channelid.parts.channel);

	// Return the converted string as the scalar result from this function
	return sqlite3_result_text(context, result, -1, sqlite3_free);
}

//---------------------------------------------------------------------------
// encode_channel_id
//
// SQLite scalar function to generate a channel identifier
//
// Arguments:
//
//	context		- SQLite context object
//	argc		- Number of supplied arguments
//	argv		- Argument values

void encode_channel_id(sqlite3_context* context, int argc, sqlite3_value** argv)
{
	int			channel = 0;			// Parsed channel number
	int			subchannel = 0;			// Parsed subchannel number

	if((argc != 1) || (argv[0] == nullptr)) return sqlite3_result_error(context, "invalid arguments", -1);
	
	// Null or zero-length input string results in 0
	const char* str = reinterpret_cast<const char*>(sqlite3_value_text(argv[0]));
	if((str == nullptr) || (*str == 0)) return sqlite3_result_int(context, 0);

	// The input format must be %d.%d or %d
	if((sscanf(str, "%d.%d", &channel, &subchannel) == 2) || (sscanf(str, "%d", &channel) == 1)) {

		// Construct the channel identifier by setting the bit field components
		union channelid channelid;
		channelid.parts.channel = channel;
		channelid.parts.subchannel = subchannel;

		return sqlite3_result_int(context, channelid.value);
	}

	// Could not parse the channel number into channel/subchannel components
	return sqlite3_result_int(context, 0);
}

//---------------------------------------------------------------------------
// epg_bestindex
//
// Determines the best index to use when querying the virtual table
//
// Arguments:
//
//	vtab	- Virtual Table instance
//	info	- Selected index information to populate

int epg_bestindex(sqlite3_vtab* /*vtab*/, sqlite3_index_info* info)
{
	// usable_constraint_index (local)
	//
	// Finds the first usable constraint for the specified column ordinal
	auto usable_constraint_index = [](sqlite3_index_info* info, int ordinal) -> int {

		// The constraints aren't necessarily in the order specified by the table, loop to find it
		for(int index = 0; index < info->nConstraint; index++) {

			auto constraint = &info->aConstraint[index];
			if(constraint->iColumn == ordinal) return ((constraint->usable) && (constraint->op == SQLITE_INDEX_CONSTRAINT_EQ)) ? index : -1;
		}

		return -1;
	};

	// Ensure that valid constraints have been specified for all of the input columns and set them as
	// the input arguments for xFilter() in the proper order
	for(int ordinal = epg_vtab_columns::deviceauth; ordinal <= epg_vtab_columns::endtime; ordinal++) {

		// Find the index of the first usable constraint for this ordinal, if none are found abort
		int index = usable_constraint_index(info, ordinal);
		if(index < 0) return SQLITE_CONSTRAINT;

		// Set the constraint value to be passed into xFilter() as an argument, ensuring that the
		// argument ordering matches what xFilter() will be expecting
		info->aConstraintUsage[index].argvIndex = ordinal;
		info->aConstraintUsage[index].omit = 1;
	}

	// There is only one viable index to be selected, set the cost to 1.0
	info->estimatedCost = 1.0;

	return SQLITE_OK;
}

//---------------------------------------------------------------------------
// epg_close
//
// Closes and deallocates a virtual table cursor instance
//
// Arguments:
//
//	cursor		- Cursor instance allocated by xOpen

int epg_close(sqlite3_vtab_cursor* cursor)
{
	if(cursor != nullptr) delete reinterpret_cast<epg_vtab_cursor*>(cursor);

	return SQLITE_OK;
}

//---------------------------------------------------------------------------
// epg_column
//
// Accesses the data in the specified column of the current cursor row
//
// Arguments:
//
//	cursor		- Virtual table cursor instance
//	context		- Result context object
//	ordinal		- Ordinal of the column being accessed

int epg_column(sqlite3_vtab_cursor* cursor, sqlite3_context* context, int ordinal)
{
	// Cast the provided generic cursor instance back into an epg_vtab_cursor instance
	epg_vtab_cursor* epgcursor = reinterpret_cast<epg_vtab_cursor*>(cursor);
	assert(epgcursor != nullptr);

	// Accessing the value column requires a valid reference to the current row data
	if((ordinal == epg_vtab_columns::value) && (epgcursor->currentrow < epgcursor->rows.size())) {

		auto const& value = epgcursor->rows[epgcursor->currentrow];

		// Watch out for zero-length results - convert into NULL
		if(value.size() == 0) sqlite3_result_null(context);
		else sqlite3_result_text(context, reinterpret_cast<char const*>(value.data()), static_cast<int>(value.size()), SQLITE_TRANSIENT);
	}

	// The remaining columns are static in nature and can be accessed from any row
	else switch(ordinal) {

		case epg_vtab_columns::deviceauth: sqlite3_result_text(context, epgcursor->deviceauth.c_str(), -1, SQLITE_TRANSIENT); break;
		case epg_vtab_columns::channel: sqlite3_result_text(context, epgcursor->channel.c_str(), -1, SQLITE_TRANSIENT); break;
		case epg_vtab_columns::starttime: sqlite3_result_int(context, static_cast<int>(epgcursor->starttime)); break;
		case epg_vtab_columns::endtime: sqlite3_result_int(context, static_cast<int>(epgcursor->endtime)); break;

		// Invalid ordinal or invalid row when accessing the value column yields null
		default: sqlite3_result_null(context);
	}

	return SQLITE_OK;
}

//---------------------------------------------------------------------------
// epg_connect
//
// Connects to the specified virtual table
//
// Arguments:
//
//	instance	- SQLite database instance handle
//	aux			- Client data pointer provided to sqlite3_create_module[_v2]()
//	argc		- Number of provided metadata strings
//	argv		- Metadata strings
//	vtab		- On success contains the allocated virtual table instance
//	err			- On error contains a string-based error message

int epg_connect(sqlite3* instance, void* /*aux*/, int /*argc*/, const char* const* /*argv*/, sqlite3_vtab** vtab, char** err)
{
	// Declare the schema for the virtual table, use hidden columns for all of the filter criteria
	int result = sqlite3_declare_vtab(instance, "create table epg(value text, deviceauth text hidden, channel text hidden, starttime integer hidden, endtime integer hidden)");
	if(result != SQLITE_OK) return result;

	// Allocate and initialize the custom virtual table class
	try { *vtab = static_cast<sqlite3_vtab*>(new epg_vtab()); }
	catch(std::exception const& ex) { *err = sqlite3_mprintf("%s", ex.what()); return SQLITE_ERROR; }
	catch(...) { return SQLITE_ERROR; }

	return (*vtab == nullptr) ? SQLITE_NOMEM : SQLITE_OK;
}

//---------------------------------------------------------------------------
// epg_disconnect
//
// Disconnects from the EPG virtual table
//
// Arguments:
//
//	vtab		- Virtual table instance allocated by xConnect

int epg_disconnect(sqlite3_vtab* vtab)
{
	if(vtab != nullptr) delete reinterpret_cast<epg_vtab*>(vtab);

	return SQLITE_OK;
}

//---------------------------------------------------------------------------
// epg_eof
//
// Determines if the specified cursor has moved beyond the last row of data
//
// Arguments:
//
//	cursor		- Virtual table cursor instance

int epg_eof(sqlite3_vtab_cursor* cursor)
{
	// Cast the provided generic cursor instance back into an epg_vtab_cursor instance
	epg_vtab_cursor* epgcursor = reinterpret_cast<epg_vtab_cursor*>(cursor);
	assert(epgcursor != nullptr);

	// Return 1 if the current row points beyond the number of rows available
	return (epgcursor->currentrow >= epgcursor->rows.size()) ? 1 : 0;
}

//---------------------------------------------------------------------------
// epg_filter
//
// Executes a search of the virtual table
//
// Arguments:
//
//	cursor		- Virtual table cursor instance
//	indexnum	- Virtual table index number from xBestIndex()
//	indexstr	- Virtual table index string from xBestIndex()
//	argc		- Number of arguments assigned by xBestIndex()
//	argv		- Argument data assigned by xBestIndex()

int epg_filter(sqlite3_vtab_cursor* cursor, int /*indexnum*/, char const* /*indexstr*/, int argc, sqlite3_value** argv)
{
	// Cast the provided generic cursor instance back into an epg_vtab_cursor instance
	epg_vtab_cursor* epgcursor = reinterpret_cast<epg_vtab_cursor*>(cursor);
	assert(epgcursor != nullptr);

	// write_function (local)
	//
	// cURL write callback to append data to the provided byte_string instance
	auto write_function = [](void const* data, size_t size, size_t count, void* userdata) -> size_t {

		assert((size * count) > 0);

		try { if((size * count) > 0) reinterpret_cast<byte_string*>(userdata)->append(reinterpret_cast<uint8_t const*>(data), size * count); }
		catch(...) { return 0; }
		
		return size * count;
	};

	try {

		// All four arguments must have been specified by xBestIndex
		if(argc != 4) throw string_exception(__func__, ": invalid argument count provided by xBestIndex");

		// Assign the deviceauth string to the epg_vtab_cursor instance; must be present
		char const* deviceauth = reinterpret_cast<char const*>(sqlite3_value_text(argv[0]));
		if(deviceauth != nullptr) epgcursor->deviceauth.assign(deviceauth);
		if(epgcursor->deviceauth.length() == 0) throw string_exception(__func__, ": null or zero-length deviceauth string");

		// Assign the channel string to the epg_vtab_cursor instance; must be present
		char const* channel = reinterpret_cast<char const*>(sqlite3_value_text(argv[1]));
		if(channel != nullptr) epgcursor->channel.assign(channel);
		if(epgcursor->channel.length() == 0) throw string_exception(__func__, ": null or zero-length channel string");

		// Assign the start and end time values to the cursor instance
		epgcursor->starttime = sqlite3_value_int(argv[2]);
		epgcursor->endtime = sqlite3_value_int(argv[3]);

		// Use local variables to track starttime and endtime as the queries are generated
		time_t starttime = epgcursor->starttime;
		time_t endtime = epgcursor->endtime;

		// Initialize a cURL multiple interface session to handle the data transfers
		CURLM* curlm = curl_multi_init();
		if(curlm == nullptr) throw string_exception(__func__, ": curl_multi_init() failed");

		try {

			// Disable pipelining/multiplexing on the multi interface object. It doesn't make an appreciable
			// performance difference here and may have been the root cause of a lot of weird problems
			CURLMcode curlmresult = curl_multi_setopt(curlm, CURLMOPT_PIPELINING, CURLPIPE_NOTHING);
			if(curlmresult != CURLM_OK) throw string_exception(__func__, ": curl_multi_setopt(CURLMOPT_PIPELINING) failed: ", curl_multi_strerror(curlmresult));

			// Create a list<> to hold the individual transfer information (no iterator invalidation)
			std::list<std::pair<CURL*, byte_string>> transfers;

			try {

				// Create all of the required individual transfer objects necessary to satisfy the EPG request.  The backend will
				// return no more than 8 hours of data per request, so break it up into 7.5 hour chunks to avoid any holes
				while(starttime < endtime) {

					// Create and initialize the cURL easy interface handle for this transfer operation
					CURL* curl = curl_easy_init();
					if(curl == nullptr) throw string_exception(__func__, ": curl_easy_init() failed");

					// Generate the URL required to execute this transfer operation
					auto url = sqlite3_mprintf("http://api.hdhomerun.com/api/guide?DeviceAuth=%s&Channel=%s&Start=%d", epgcursor->deviceauth.c_str(), epgcursor->channel.c_str(), starttime);

				#if defined(_WINDOWS) && defined(_DEBUG)
					// Dump the target URL to the debugger on Windows _DEBUG builds to watch for URL duplication
					char debugurl[256];
					snprintf(debugurl, std::extent<decltype(debugurl)>::value, "%s: %s\r\n", __func__, url);
					OutputDebugStringA(debugurl);
				#endif

					// Create the transfer instance to track this operation in the list<>
					auto transfer = transfers.emplace(transfers.end(), std::make_pair(curl, byte_string()));

					// Set the CURL options and execute the web request to get the JSON string data
					CURLcode curlresult = curl_easy_setopt(curl, CURLOPT_URL, url);
					if(curlresult == CURLE_OK) curlresult = curl_easy_setopt(curl, CURLOPT_USERAGENT, g_useragent.c_str());
					if(curlresult == CURLE_OK) curlresult = curl_easy_setopt(curl, CURLOPT_NOSIGNAL, 1);
					if(curlresult == CURLE_OK) curlresult = curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);
					if(curlresult == CURLE_OK) curlresult = curl_easy_setopt(curl, CURLOPT_MAXREDIRS, 5L);
					if(curlresult == CURLE_OK) curlresult = curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT, 10L);
					if(curlresult == CURLE_OK) curlresult = curl_easy_setopt(curl, CURLOPT_IPRESOLVE, CURL_IPRESOLVE_V4);
					if(curlresult == CURLE_OK) curlresult = curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, static_cast<curl_writefunction>(write_function));
					if(curlresult == CURLE_OK) curlresult = curl_easy_setopt(curl, CURLOPT_WRITEDATA, reinterpret_cast<void*>(&transfer->second));
					if(curlresult == CURLE_OK) curlresult = curl_easy_setopt(curl, CURLOPT_SHARE, static_cast<CURLSH*>(g_curlshare));

					// Release the URL string after cURL initializations are complete
					sqlite3_free(url);

					// Verify that initialization of the cURL easy interface handle was completed successfully
					if(curlresult != CURLE_OK) throw string_exception(__func__, ": curl_easy_setopt() failed: ", curl_easy_strerror(curlresult));

					// Loop to the next required transfer instance; use a value of (27000 = 7.5 hours) to set 30 minutes of overlap
					starttime += 27000;
				}

				// Add all of the generated cURL easy interface objects to the cURL multi interface object
				for(auto const& transfer : transfers) {

					curlmresult = curl_multi_add_handle(curlm, transfer.first);
					if(curlmresult != CURLM_OK) throw string_exception(__func__, ": curl_multi_add_handle() failed: ", curl_multi_strerror(curlmresult));
				}

				// Execute the transfer operation(s) until they have all completed
				int numfds = 0;
				curlmresult = curl_multi_perform(curlm, &numfds);
				while((curlmresult == CURLM_OK) && (numfds > 0)) {

					curlmresult = curl_multi_wait(curlm, nullptr, 0, 500, &numfds);
					if(curlmresult == CURLM_OK) curlmresult = curl_multi_perform(curlm, &numfds);
				}

				// After the transfer operation(s) have completed, verify the HTTP status of each one
				// and abort the operation if any of them did not return HTTP 200: OK
				for(auto& transfer : transfers) {

					long responsecode = 200;			// Assume HTTP 200: OK

					// The response code will come back as zero if there was no response from the host,
					// otherwise it should be a standard HTTP response code
					curl_easy_getinfo(transfer.first, CURLINFO_RESPONSE_CODE, &responsecode);

					if(responsecode == 0) throw string_exception(__func__, ": no response from host");
					else if((responsecode < 200) || (responsecode > 299)) throw http_exception(responsecode);

					// Ignore transfers that returned no data or begin with the string "null"
					if((transfer.second.size() > 0) && (strcasecmp(reinterpret_cast<const char*>(transfer.second.c_str()), "null") != 0)) {

						// Validate the JSON document returned from the query has no parse error(s)
						rapidjson::Document json;
						json.Parse(reinterpret_cast<const char*>(transfer.second.data()), transfer.second.size());

						// Ignore any transfers that returned invalid JSON data (for now, ultimately I would like
						// to add a retry operation here for the individual bad transfers)
						if(!json.HasParseError()) epgcursor->rows.emplace_back(std::move(transfer.second));
					}
				}

				// Clean up and destroy the generated cURL easy interface handles
				for(auto& iterator : transfers) {
					
					curl_multi_remove_handle(curlm, iterator.first);  
					curl_easy_cleanup(iterator.first);
				}
			}

			// Clean up any destroy any created cURL easy interface handles on exception
			catch(...) { 
				
				for(auto& iterator : transfers) {
					
					curl_multi_remove_handle(curlm, iterator.first); 
					curl_easy_cleanup(iterator.first); 
				}
				
				throw; 
			}

			// Clean up and destroy the cURL multi interface handle
			curl_multi_cleanup(curlm);
		}

		// Clean up and destroy the multi handle on exception
		catch(...) { curl_multi_cleanup(curlm); throw; }
	}
	
	catch(std::exception const& ex) { epgcursor->pVtab->zErrMsg = sqlite3_mprintf("%s", ex.what()); return SQLITE_ERROR; }
	catch(...) { return SQLITE_ERROR; }

	return SQLITE_OK;
}

//---------------------------------------------------------------------------
// epg_next
//
// Advances the virtual table cursor to the next row
//
// Arguments:
//
//	cursor		- Virtual table cusror instance

int epg_next(sqlite3_vtab_cursor* cursor)
{
	// Cast the provided generic cursor instance back into an epg_vtab_cursor instance
	epg_vtab_cursor* epgcursor = reinterpret_cast<epg_vtab_cursor*>(cursor);
	assert(epgcursor != nullptr);

	// The only way this can fail is if xNext() was called too many times, which shouldn't
	// happen unless there is a bug so send back SQLITE_INTERNAL if it does happen
	return ((++epgcursor->currentrow) <= epgcursor->rows.size()) ? SQLITE_OK : SQLITE_INTERNAL;
}

//---------------------------------------------------------------------------
// epg_open
//
// Creates and intializes a new virtual table cursor instance
//
// Arguments:
//
//	vtab		- Virtual table instance
//	cursor		- On success contains the allocated virtual table cursor instance

int epg_open(sqlite3_vtab* /*vtab*/, sqlite3_vtab_cursor** cursor)
{
	// Allocate and initialize the custom virtual table cursor class
	try { *cursor = static_cast<sqlite3_vtab_cursor*>(new epg_vtab_cursor()); }
	catch(...) { return SQLITE_ERROR; }

	return (*cursor == nullptr) ? SQLITE_NOMEM : SQLITE_OK;
}

//---------------------------------------------------------------------------
// epg_rowid
//
// Retrieves the ROWID for the current virtual table cursor row
//
// Arguments:
//
//	cursor		- Virtual table cursor instance
//	rowid		- On success contains the ROWID for the current row

int epg_rowid(sqlite3_vtab_cursor* cursor, sqlite_int64* rowid)
{
	// Cast the provided generic cursor instance back into an epg_vtab_cursor instance
	epg_vtab_cursor* epgcursor = reinterpret_cast<epg_vtab_cursor*>(cursor);
	assert(epgcursor != nullptr);

	// Use the current row index as the ROWID for the cursor
	*rowid = static_cast<sqlite_int64>(epgcursor->currentrow);

	return SQLITE_OK;
}

//---------------------------------------------------------------------------
// fnv_hash
//
// SQLite scalar function to generate an FNV-1a hash code from multiple values
//
// Arguments:
//
//	context		- SQLite context object
//	argc		- Number of supplied arguments
//	argv		- Argument values

void fnv_hash(sqlite3_context* context, int argc, sqlite3_value** argv)
{
	// 32-bit FNV-1a primes (http://www.isthe.com/chongo/tech/comp/fnv/index.html#FNV-source) 
	const int fnv_offset_basis = 2166136261U;
	const int fnv_prime = 16777619U;

	if(argc == 0) return sqlite3_result_int(context, 0);

	// Calcuate the FNV-1a hash for each argument passed into the function
	int hash = fnv_offset_basis;
	for(int index = 0; index < argc; index++) {

		int type = sqlite3_value_type(argv[index]);

		// SQLITE_NULL - Ignore this value
		if(type == SQLITE_NULL) continue;

		// Treat SQLITE_INTEGER values as integers
		else if(type == SQLITE_INTEGER) {
			
			hash ^= sqlite3_value_int(argv[index]);
			hash *= fnv_prime;
		}

		// Treat everything else as a blob, per documentation SQLite will cast
		// SQLITE_FLOAT and SQLITE_TEXT into blobs directly without conversion
		else {

			uint8_t const* blob = reinterpret_cast<uint8_t const*>(sqlite3_value_blob(argv[index]));
			if(blob == nullptr) continue;

			// Hash each byte of the BLOB individually
			for(int offset = 0; offset < sqlite3_value_bytes(argv[index]); offset++) {
				
				hash ^= blob[offset];
				hash *= fnv_prime;
			}
		}
	}

	return sqlite3_result_int(context, hash);
}

//---------------------------------------------------------------------------
// generate_uuid
//
// SQLite scalar function to generate a UUID
//
// Arguments:
//
//	context		- SQLite context object
//	argc		- Number of supplied arguments
//	argv		- Argument values

void generate_uuid(sqlite3_context* context, int argc, sqlite3_value** /*argv*/)
{
	uuid_t		uuid;						// Generated UUID
	char		uuidstr[40];				// UUID string representation

	if(argc != 0) return sqlite3_result_error(context, "invalid argument", -1);

	// Create and convert a new UUID to provide as the string result
	uuid_generate(uuid);
	uuid_unparse(uuid, uuidstr);

	sqlite3_result_text(context, uuidstr, -1, SQLITE_TRANSIENT);
}

//---------------------------------------------------------------------------
// get_channel_number
//
// SQLite scalar function to read the channel number from a string
//
// Arguments:
//
//	context		- SQLite context object
//	argc		- Number of supplied arguments
//	argv		- Argument values

void get_channel_number(sqlite3_context* context, int argc, sqlite3_value** argv)
{
	int				channel = 0;				// Parsed channel number
	int				subchannel = 0;				// Parsed subchannel number

	if((argc != 1) || (argv[0] == nullptr)) return sqlite3_result_error(context, "invalid argument", -1);
	
	// Null or zero-length input string results in 0
	const char* str = reinterpret_cast<const char*>(sqlite3_value_text(argv[0]));
	if((str == nullptr) || (*str == 0)) return sqlite3_result_int(context, 0);

	// The input format must be %d.%d or %d
	if((sscanf(str, "%d.%d", &channel, &subchannel) == 2) || (sscanf(str, "%d", &channel) == 1)) return sqlite3_result_int(context, channel);
	else return sqlite3_result_int(context, 0);
}

//---------------------------------------------------------------------------
// get_episode_number
//
// SQLite scalar function to read the episode number from a string
//
// Arguments:
//
//	context		- SQLite context object
//	argc		- Number of supplied arguments
//	argv		- Argument values

void get_episode_number(sqlite3_context* context, int argc, sqlite3_value** argv)
{
	int				season = -1;				// Parsed season number
	int				episode = -1;				// Parsed episode number

	if((argc != 1) || (argv[0] == nullptr)) return sqlite3_result_error(context, "invalid argument", -1);
	
	// Null or zero-length input string results in -1
	const char* str = reinterpret_cast<const char*>(sqlite3_value_text(argv[0]));
	if((str == nullptr) || (*str == 0)) return sqlite3_result_int(context, -1);

	if(sscanf(str, "S%dE%d", &season, &episode) == 2) return sqlite3_result_int(context, episode);
	else if(sscanf(str, "%d-%d", &season, &episode) == 2) return sqlite3_result_int(context, episode);
	else if(sscanf(str, "EP%d", &episode) == 1) return sqlite3_result_int(context, episode);
	else if(sscanf(str, "%d", &episode) == 1) return sqlite3_result_int(context, episode);

	return sqlite3_result_int(context, -1);
}

//---------------------------------------------------------------------------
// get_recording_id
//
// SQLite scalar function to read the recording ID from a command URL
//
// Arguments:
//
//	context		- SQLite context object
//	argc		- Number of supplied arguments
//	argv		- Argument values

void get_recording_id(sqlite3_context* context, int argc, sqlite3_value** argv)
{
	if((argc != 1) || (argv[0] == nullptr)) return sqlite3_result_error(context, "invalid argument", -1);

	// This function accepts the command URL for the recording so that the identifier can be extracted
	const char* url = reinterpret_cast<const char*>(sqlite3_value_text(argv[0]));
	if((url == nullptr) || (*url == 0)) return sqlite3_result_error(context, "url argument is null or zero-length", -1);

	// Allocate a CURLU instance to parse the command URL components
	CURLU* curlu = curl_url();
	if(curlu == nullptr) return sqlite3_result_error(context, "insufficient memory to allocate CURLU instance", -1);

	// Apply the command URL to the CURLU instance
	CURLUcode curluresult = curl_url_set(curlu, CURLUPart::CURLUPART_URL, url, 0);
	if(curluresult == CURLUE_OK) {

		// We are interested in the query string portion of the CmdURL
		char* querystring = nullptr;
		curluresult = curl_url_get(curlu, CURLUPART_QUERY, &querystring, 0);
		if(curluresult == CURLUE_OK) {

			// The query string must start with "id=", use the rest as-is.  This will be OK for now, but
			// a more robust solution would be parsing the entire query string and selecting just the id key/value
			if(strncasecmp(querystring, "id=", 3) == 0) sqlite3_result_text(context, &querystring[3], -1, SQLITE_TRANSIENT);
			else sqlite3_result_error(context, "unable to extract recording id from specified url", -1);

			curl_free(querystring);			// Release the allocated query string
		}

		else sqlite3_result_error(context, "unable to extract query string from specified url", -1);
	}

	else sqlite3_result_error(context, "unable to parse supplied url", -1);

	curl_url_cleanup(curlu);				// Release the CURLU instance
}

//---------------------------------------------------------------------------
// get_season_number
//
// SQLite scalar function to read the season number from a string
//
// Arguments:
//
//	context		- SQLite context object
//	argc		- Number of supplied arguments
//	argv		- Argument values

void get_season_number(sqlite3_context* context, int argc, sqlite3_value** argv)
{
	int				season = -1;				// Parsed season number
	int				episode = -1;				// Parsed episode number

	if((argc != 1) || (argv[0] == nullptr)) return sqlite3_result_error(context, "invalid argument", -1);
	
	// Null or zero-length input string results in -1
	const char* str = reinterpret_cast<const char*>(sqlite3_value_text(argv[0]));
	if((str == nullptr) || (*str == 0)) return sqlite3_result_int(context, -1);

	if(sscanf(str, "S%dE%d", &season, &episode) == 2) return sqlite3_result_int(context, season);
	else if(sscanf(str, "%d-%d", &season, &episode) == 2) return sqlite3_result_int(context, season);

	return sqlite3_result_int(context, -1);
}

//---------------------------------------------------------------------------
// http_get
//
// SQLite scalar function to execute an HTTP GET request
//
// Arguments:
//
//	context		- SQLite context object
//	argc		- Number of supplied arguments
//	argv		- Argument values

void http_get(sqlite3_context* context, int argc, sqlite3_value** argv)
{
	// http_get requires at least the URL argument to be specified, with an optional second parameter
	// indicating a default value to return in the event of an HTTP error
	if((argc < 1) || (argc > 2) || (argv[0] == nullptr)) return sqlite3_result_error(context, "invalid argument", -1);

	// Invoke http_request without specifying anything for the POST data argument
	return http_request(context, argv[0], nullptr, ((argc >= 2) && (argv[1] != nullptr)) ? argv[1] : nullptr);
}

//---------------------------------------------------------------------------
// http_post
//
// SQLite scalar function to execute an HTTP POST request
//
// Arguments:
//
//	context		- SQLite context object
//	argc		- Number of supplied arguments
//	argv		- Argument values

void http_post(sqlite3_context* context, int argc, sqlite3_value** argv)
{
	// http_post requires at least the URL and post data arguments to be specified, with an optional third parameter
	// indicating a default value to return in the event of an HTTP error
	if((argc < 2) || (argc > 3) || (argv[0] == nullptr) || (argv[1] == nullptr)) return sqlite3_result_error(context, "invalid argument", -1);

	// Invoke http_request specifying the POST data argument and the optional default value
	return http_request(context, argv[0], argv[1], ((argc >= 3) && (argv[2] != nullptr)) ? argv[2] : nullptr);
}

//---------------------------------------------------------------------------
// http_request
//
// Helper function used by http_get and http_post to execute an HTTP request
//
// Arguments:
//
//	context			- SQLite context object
//	urlvalue		- URL function argument
//	postvalue		- POSTFIELDS function argument
//	defaultvalue	- DEFAULTVALUE function argument

static void http_request(sqlite3_context* context, sqlite3_value* urlvalue, sqlite3_value* postvalue, sqlite3_value* defaultvalue)
{
	bool				post = false;			// Flag indicating an HTTP POST operation
	std::string			postfields;				// HTTP post fields (optional)
	long				responsecode = 200;		// HTTP response code
	byte_string			blob;					// Dynamically allocated blob buffer

	assert(urlvalue != nullptr);

	// A null or zero-length URL results in a NULL result
	const char* url = reinterpret_cast<const char*>(sqlite3_value_text(urlvalue));
	if((url == nullptr) || (*url == '\0')) return sqlite3_result_null(context);

	// If a POST argument was specified, switch the operation into HTTP POST mode
	if((post = (postvalue != nullptr))) {

		const char* postdata = reinterpret_cast<const char*>(sqlite3_value_text(postvalue));
		if(postdata != nullptr) postfields.assign(postdata);
	}

	// Create a write callback for libcurl to invoke to write the data
	auto write_function = [](void const* data, size_t size, size_t count, void* userdata) -> size_t {

		assert((size * count) > 0);

		try { if((size * count) > 0) reinterpret_cast<byte_string*>(userdata)->append(reinterpret_cast<uint8_t const*>(data), size * count); }
		catch(...) { return 0; }
		
		return size * count;
	};

#if defined(_WINDOWS) && defined(_DEBUG)
	// Dump the target URL to the debugger on Windows _DEBUG builds to watch for URL duplication
	char debugurl[256];
	snprintf(debugurl, std::extent<decltype(debugurl)>::value, "%s (%s): %s%s%s%s\r\n", __func__, (post) ? "post" : "get", url, (post) ? " [" : "", (post) ? postfields.c_str() : "", (post) ? "]" : "");
	OutputDebugStringA(debugurl);
#endif

	// Initialize the CURL session for the download operation
	CURL* curl = curl_easy_init();
	if(curl == nullptr) return sqlite3_result_error(context, "cannot initialize libcurl object", -1);

	// Set the CURL options and execute the web request, switching to POST if indicated
	CURLcode curlresult = curl_easy_setopt(curl, CURLOPT_URL, url);
	if((post) && (curlresult == CURLE_OK)) curlresult = curl_easy_setopt(curl, CURLOPT_POSTFIELDS, postfields.c_str());
	if(curlresult == CURLE_OK) curlresult = curl_easy_setopt(curl, CURLOPT_USERAGENT, g_useragent.c_str());
	if(curlresult == CURLE_OK) curlresult = curl_easy_setopt(curl, CURLOPT_NOSIGNAL, 1);
	if(curlresult == CURLE_OK) curlresult = curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);
	if(curlresult == CURLE_OK) curlresult = curl_easy_setopt(curl, CURLOPT_MAXREDIRS, 5L);
	if(curlresult == CURLE_OK) curlresult = curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT, 10L);
	if(curlresult == CURLE_OK) curlresult = curl_easy_setopt(curl, CURLOPT_IPRESOLVE, CURL_IPRESOLVE_V4);
	if(curlresult == CURLE_OK) curlresult = curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, static_cast<curl_writefunction>(write_function));
	if(curlresult == CURLE_OK) curlresult = curl_easy_setopt(curl, CURLOPT_WRITEDATA, reinterpret_cast<void*>(&blob));
	if(curlresult == CURLE_OK) curlresult = curl_easy_setopt(curl, CURLOPT_SHARE, static_cast<CURLSH*>(g_curlshare));
	if(curlresult == CURLE_OK) curlresult = curl_easy_perform(curl);
	if(curlresult == CURLE_OK) curlresult = curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &responsecode);
	curl_easy_cleanup(curl);

	// Check if any of the above operations failed and return an error condition
	if(curlresult != CURLE_OK) {

		// If a default result was provided, use it rather than returning an error result
		if(defaultvalue != nullptr) return sqlite3_result_value(context, defaultvalue);
	
		// Use sqlite3_mprintf to generate the formatted error message
		auto message = sqlite3_mprintf("http %s request on [%s] failed: %s", (post) ? "post" : "get", url, curl_easy_strerror(curlresult));
		sqlite3_result_error(context, message, -1);
		return sqlite3_free(reinterpret_cast<void*>(message));
	}

	// Check the HTTP response code and return an error condition if unsuccessful
	if((responsecode < 200) || (responsecode > 299)) {
	
		// If a default result was provided, use it rather than returning an error result
		if(defaultvalue != nullptr) return sqlite3_result_value(context, defaultvalue);
	
		// Use sqlite3_mprintf to generate the formatted error message
		auto message = sqlite3_mprintf("http %s request on url [%s] failed with http response code %ld", (post) ? "post" : "get", url, responsecode);
		sqlite3_result_error(context, message, -1);
		return sqlite3_free(reinterpret_cast<void*>(message));
	}

	// Watch for data that exceeds int::max, sqlite3_result_blob does not accept a size_t for the length
	size_t cb = blob.size();
	if(cb > static_cast<size_t>(std::numeric_limits<int>::max())) 
		return sqlite3_result_error(context, "blob data exceeds std::numeric_limits<int>::max() in length", -1);

	// Send the resultant blob to SQLite as the result from this scalar function
	return (cb > 0) ? sqlite3_result_blob(context, blob.data(), static_cast<int>(blob.size()), SQLITE_TRANSIENT) : sqlite3_result_null(context);
}

//---------------------------------------------------------------------------
// json_get_aggregate_final
//
// SQLite aggregate function to generate a JSON array of multiple JSON documents
//
// Arguments:
//
//	context		- SQLite context object

void json_get_aggregate_final(sqlite3_context* context)
{
	rapidjson::Document			document;			// Resultant JSON document

	// Retrieve the json_get_aggregate_state pointer from the aggregate context; if it does not exist return NULL
	json_get_aggregate_state** statepp = reinterpret_cast<json_get_aggregate_state**>(sqlite3_aggregate_context(context, sizeof(json_get_aggregate_state*)));
	json_get_aggregate_state* statep = (statepp == nullptr) ? nullptr : *statepp;
	if(statep == nullptr) { sqlite3_aggregate_context(context, 0); return sqlite3_result_null(context); }

	// write_function (local)
	//
	// cURL write callback to append data to the provided byte_string instance
	auto write_function = [](void const* data, size_t size, size_t count, void* userdata) -> size_t {

		assert((size * count) > 0);

		try { if((size * count) > 0) reinterpret_cast<byte_string*>(userdata)->append(reinterpret_cast<uint8_t const*>(data), size * count); } 
		catch(...) { return 0; }

		return size * count;
	};

	// Wrap the json_get_aggregate_state pointer in a unique_ptr to automatically release it when it falls out of scope
	std::unique_ptr<json_get_aggregate_state, std::function<void(json_get_aggregate_state*)>> state(statep, [&](json_get_aggregate_state* statep) -> void {

		if(statep) delete statep;					// Release the json_get_aggregate_state
		sqlite3_aggregate_context(context, 0);		// Clear the aggregate context
	});

	// Initialize a cURL multiple interface session to handle the data transfers
	std::unique_ptr<CURLM, std::function<void(CURLM*)>> curlm(curl_multi_init(), [](CURLM* curlm) -> void { curl_multi_cleanup(curlm); });
	if(!curlm) throw string_exception(__func__, ": curl_multi_init() failed");

	document.SetObject();

	try {

		// Disable pipelining/multiplexing on the multi interface object. It doesn't make an appreciable
		// performance difference here and may have been the root cause of a lot of weird problems
		CURLMcode curlmresult = curl_multi_setopt(curlm.get(), CURLMOPT_PIPELINING, CURLPIPE_NOTHING);
		if(curlmresult != CURLM_OK) throw string_exception(__func__, ": curl_multi_setopt(CURLMOPT_PIPELINING) failed: ", curl_multi_strerror(curlmresult));

		// Create a list<> to hold the individual transfer information (no iterator invalidation)
		std::list<std::tuple<CURL*, byte_string, std::string>> transfers;

		try {

			for(auto const& iterator : *state) {

				// Create and initialize the cURL easy interface handle for this transfer operation
				CURL* curl = curl_easy_init();
				if(curl == nullptr) throw string_exception(__func__, ": curl_easy_init() failed");

			#if defined(_WINDOWS) && defined(_DEBUG)
				// Dump the target URL to the debugger on Windows _DEBUG builds to watch for URL duplication
				char debugurl[256];
				snprintf(debugurl, std::extent<decltype(debugurl)>::value, "%s: %s\r\n", __func__, std::get<0>(iterator).c_str());
				OutputDebugStringA(debugurl);
			#endif

				// Create the transfer instance to track this operation in the list<>
				auto transfer = transfers.emplace(transfers.end(), std::make_tuple(curl, byte_string(), std::get<1>(iterator)));

				// Set the CURL options and execute the web request to get the JSON string data
				CURLcode curlresult = curl_easy_setopt(curl, CURLOPT_URL, std::get<0>(iterator).c_str());
				if(curlresult == CURLE_OK) curlresult = curl_easy_setopt(curl, CURLOPT_USERAGENT, g_useragent.c_str());
				if(curlresult == CURLE_OK) curlresult = curl_easy_setopt(curl, CURLOPT_NOSIGNAL, 1);
				if(curlresult == CURLE_OK) curlresult = curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);
				if(curlresult == CURLE_OK) curlresult = curl_easy_setopt(curl, CURLOPT_MAXREDIRS, 5L);
				if(curlresult == CURLE_OK) curlresult = curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT, 10L);
				if(curlresult == CURLE_OK) curlresult = curl_easy_setopt(curl, CURLOPT_IPRESOLVE, CURL_IPRESOLVE_V4);
				if(curlresult == CURLE_OK) curlresult = curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, static_cast<curl_writefunction>(write_function));
				if(curlresult == CURLE_OK) curlresult = curl_easy_setopt(curl, CURLOPT_WRITEDATA, reinterpret_cast<void*>(&std::get<1>(*transfer)));
				if(curlresult == CURLE_OK) curlresult = curl_easy_setopt(curl, CURLOPT_SHARE, static_cast<CURLSH*>(g_curlshare));

				// Verify that initialization of the cURL easy interface handle was completed successfully
				if(curlresult != CURLE_OK) throw string_exception(__func__, ": curl_easy_setopt() failed: ", curl_easy_strerror(curlresult));
			}

			// Add all of the generated cURL easy interface objects to the cURL multi interface object
			for(auto const& transfer : transfers) {

				curlmresult = curl_multi_add_handle(curlm.get(), std::get<0>(transfer));
				if(curlmresult != CURLM_OK) throw string_exception(__func__, ": curl_multi_add_handle() failed: ", curl_multi_strerror(curlmresult));
			}

			// Execute the transfer operation(s) until they have all completed
			int numfds = 0;
			curlmresult = curl_multi_perform(curlm.get(), &numfds);
			while((curlmresult == CURLM_OK) && (numfds > 0)) {

				curlmresult = curl_multi_wait(curlm.get(), nullptr, 0, 500, &numfds);
				if(curlmresult == CURLM_OK) curlmresult = curl_multi_perform(curlm.get(), &numfds);
			}

			// After the transfer operation(s) have completed, verify the HTTP status of each one
			// and abort the operation if any of them did not return HTTP 200: OK
			for(auto& transfer : transfers) {

				long responsecode = 200;			// Assume HTTP 200: OK

				// The response code will come back as zero if there was no response from the host,
				// otherwise it should be a standard HTTP response code
				curl_easy_getinfo(std::get<0>(transfer), CURLINFO_RESPONSE_CODE, &responsecode);

				if(responsecode == 0) throw string_exception(__func__, ": no response from host");
				else if((responsecode < 200) || (responsecode > 299)) throw http_exception(responsecode);

				// Ignore transfers that returned no data or begin with the string "null"
				if((std::get<1>(transfer).size() > 0) && (strcasecmp(reinterpret_cast<const char*>(std::get<1>(transfer).c_str()), "null") != 0)) {

					// Convert the data returned from the query into a JSON document object
					rapidjson::Document json;
					json.Parse(reinterpret_cast<const char*>(std::get<1>(transfer).data()), std::get<1>(transfer).size());

					// If the JSON parsed successfully, add the entire document as a new array element in the final document
					if(!json.HasParseError()) document.AddMember(rapidjson::GenericStringRef<char>(std::get<2>(transfer).c_str()), 
						rapidjson::Value(json, document.GetAllocator()), document.GetAllocator());
				}
			}

			// Clean up and destroy the generated cURL easy interface handles
			for(auto& transfer : transfers) {

				curl_multi_remove_handle(curlm.get(), std::get<0>(transfer));
				curl_easy_cleanup(std::get<0>(transfer));
			}
		}

		// Clean up any destroy any created cURL easy interface handles on exception
		catch(...) {

			for(auto& transfer : transfers) {

				curl_multi_remove_handle(curlm.get(), std::get<0>(transfer));
				curl_easy_cleanup(std::get<0>(transfer));
			}

			throw;
		}

		//
		// TODO: This can be more efficient by providing a custom rapidjson output stream that uses 
		// sqlite3_malloc() internally and detaches that pointer for sqlite3_result_text, using 
		// sqlite3_free as the destructor function instead of SQLITE_TRANSIENT
		//

		// Convert the final document back into JSON and return it to the caller
		rapidjson::StringBuffer sb;
		rapidjson::Writer<rapidjson::StringBuffer> writer(sb);
		document.Accept(writer);
		sqlite3_result_text(context, sb.GetString(), -1, SQLITE_TRANSIENT);
	}

	catch(std::exception const& ex) { return sqlite3_result_error(context, ex.what(), -1); }
	catch(...) { return sqlite3_result_error_code(context, SQLITE_ERROR); }
}

//---------------------------------------------------------------------------
// json_get_aggregate_step
//
// SQLite aggregate function to generate a JSON array of multiple JSON documents
//
// Arguments:
//
//	context		- SQLite context object
//	argc		- Number of supplied arguments
//	argv		- Argument values

void json_get_aggregate_step(sqlite3_context* context, int argc, sqlite3_value** argv)
{
	if(argc != 2) return sqlite3_result_error(context, "invalid argument", -1);
	if(argv[0] == nullptr) return sqlite3_result_error(context, "invalid argument", -1);

	// Use a json_get_aggregate_state to hold the aggregate state as it's calculated
	json_get_aggregate_state** statepp = reinterpret_cast<json_get_aggregate_state**>(sqlite3_aggregate_context(context, sizeof(json_get_aggregate_state*)));
	if(statepp == nullptr) return sqlite3_result_error_nomem(context);
	
	// If the string_vector hasn't been allocated yet, allocate it now
	json_get_aggregate_state* statep = *statepp;
	if(statep == nullptr) *statepp = statep = new json_get_aggregate_state();
	if(statep == nullptr) return sqlite3_result_error_nomem(context);

	// There are two arguments to this function, the first is the URL to query for the JSON
	// and the second is the key name to assign to the resultant JSON array element
	char const* url = reinterpret_cast<char const*>(sqlite3_value_text(argv[0]));
	char const* key = reinterpret_cast<char const*>(sqlite3_value_text(argv[1]));

	// The URL string must be non-null, but the key can be null or blank if the caller doesn't care
	if(url != nullptr) statep->emplace_back(std::make_tuple(url, (key != nullptr) ? key : ""));
	else return sqlite3_result_error(context, "invalid argument", -1);
}

//---------------------------------------------------------------------------
// url_encode
//
// SQLite scalar function to encode a string with URL escape sequences
//
// Arguments:
//
//	context		- SQLite context object
//	argc		- Number of supplied arguments
//	argv		- Argument values

void url_encode(sqlite3_context* context, int argc, sqlite3_value** argv)
{
	if((argc != 1) || (argv[0] == nullptr)) return sqlite3_result_error(context, "invalid argument", -1);

	// A null or zero-length string results in a NULL result
	const char* input = reinterpret_cast<const char*>(sqlite3_value_text(argv[0]));
	if((input == nullptr) || (*input == 0)) return sqlite3_result_null(context);

	// Initialize the CURL session for the string operation
	CURL* curl = curl_easy_init();
	if(curl == nullptr) return sqlite3_result_error(context, "cannot initialize libcurl object", -1);

	// Use libcurl to encode any required escape sequences
	sqlite3_result_text(context, curl_easy_escape(curl, input, 0), -1, curl_free);

	curl_easy_cleanup(curl);
}

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

	// clean_filename function
	//
	int result = sqlite3_create_function_v2(db, "clean_filename", 1, SQLITE_UTF8 | SQLITE_DETERMINISTIC, nullptr, clean_filename, nullptr, nullptr, nullptr);
	if(result != SQLITE_OK) { *errmsg = sqlite3_mprintf("Unable to register scalar function clean_filename"); return result; }

	// decode_channel_id function
	//
	result = sqlite3_create_function_v2(db, "decode_channel_id", 1, SQLITE_UTF8 | SQLITE_DETERMINISTIC, nullptr, decode_channel_id, nullptr, nullptr, nullptr);
	if(result != SQLITE_OK) { *errmsg = sqlite3_mprintf("Unable to register scalar function decode_channel_id"); return result; }

	// encode_channel_id function
	//
	result = sqlite3_create_function_v2(db, "encode_channel_id", 1, SQLITE_UTF8 | SQLITE_DETERMINISTIC, nullptr, encode_channel_id, nullptr, nullptr, nullptr);
	if(result != SQLITE_OK) { *errmsg = sqlite3_mprintf("Unable to register scalar function encode_channel_id"); return result; }

	// epg virtual table function
	//
	result = sqlite3_create_module_v2(db, "epg", &g_epg_module, nullptr, nullptr);
	if(result != SQLITE_OK) { *errmsg = sqlite3_mprintf("Unable to register virtual table module epg"); return result; }

	// fnv_hash function
	//
	result = sqlite3_create_function_v2(db, "fnv_hash", -1, SQLITE_UTF8 | SQLITE_DETERMINISTIC, nullptr, fnv_hash, nullptr, nullptr, nullptr);
	if(result != SQLITE_OK) { *errmsg = sqlite3_mprintf("Unable to register scalar function fnv_hash"); return result; }

	// generate_uuid (non-deterministic)
	//
	result = sqlite3_create_function_v2(db, "generate_uuid", 0, SQLITE_UTF8, nullptr, generate_uuid, nullptr, nullptr, nullptr);
	if(result != SQLITE_OK) { *errmsg = sqlite3_mprintf("Unable to register scalar function generate_uuid"); return result; }

	// get_channel_number function
	//
	result = sqlite3_create_function_v2(db, "get_channel_number", 1, SQLITE_UTF8 | SQLITE_DETERMINISTIC, nullptr, get_channel_number, nullptr, nullptr, nullptr);
	if(result != SQLITE_OK) { *errmsg = sqlite3_mprintf("Unable to register scalar function get_channel_number"); return result; }

	// get_episode_number function
	//
	result = sqlite3_create_function_v2(db, "get_episode_number", 1, SQLITE_UTF8 | SQLITE_DETERMINISTIC, nullptr, get_episode_number, nullptr, nullptr, nullptr);
	if(result != SQLITE_OK) { *errmsg = sqlite3_mprintf("Unable to register scalar function get_episode_number"); return result; }

	// get_recording_id function
	//
	result = sqlite3_create_function_v2(db, "get_recording_id", 1, SQLITE_UTF8 | SQLITE_DETERMINISTIC, nullptr, get_recording_id, nullptr, nullptr, nullptr);
	if(result != SQLITE_OK) { *errmsg = sqlite3_mprintf("Unable to register scalar function get_recording_id"); return result; }

	// get_season_number function
	//
	result = sqlite3_create_function_v2(db, "get_season_number", 1, SQLITE_UTF8 | SQLITE_DETERMINISTIC, nullptr, get_season_number, nullptr, nullptr, nullptr);
	if(result != SQLITE_OK) { *errmsg = sqlite3_mprintf("Unable to register scalar function get_season_number"); return result; }

	// http_get (non-deterministic)
	//
	result = sqlite3_create_function_v2(db, "http_get", -1, SQLITE_UTF8, nullptr, http_get, nullptr, nullptr, nullptr);
	if(result != SQLITE_OK) { *errmsg = sqlite3_mprintf("Unable to register scalar function http_get"); return result; }

	// http_post (non-deterministic)
	//
	result = sqlite3_create_function_v2(db, "http_post", -1, SQLITE_UTF8, nullptr, http_post, nullptr, nullptr, nullptr);
	if(result != SQLITE_OK) { *errmsg = sqlite3_mprintf("Unable to register scalar function http_post"); return result; }

	// json_get_aggregate (non-deterministic)
	//
	result = sqlite3_create_function_v2(db, "json_get_aggregate", 2, SQLITE_UTF8, nullptr, nullptr, json_get_aggregate_step, json_get_aggregate_final, nullptr);
	if(result != SQLITE_OK) { *errmsg = sqlite3_mprintf("Unable to register aggregate function json_get_aggregate"); return result; }

	// url_encode function
	//
	result = sqlite3_create_function_v2(db, "url_encode", 1, SQLITE_UTF8 | SQLITE_DETERMINISTIC, nullptr, url_encode, nullptr, nullptr, nullptr);
	if(result != SQLITE_OK) { *errmsg = sqlite3_mprintf("Unable to register scalar function url_encode"); return result; }

	return SQLITE_OK;
}

//---------------------------------------------------------------------------

#pragma warning(pop)
