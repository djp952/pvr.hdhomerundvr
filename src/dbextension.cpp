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
	if(sqlite3_value_type(argv[0]) == SQLITE_NULL) return sqlite3_result_text(context, "0", -1, nullptr);

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
	static curlshare curlshare;				// Static curlshare instance used only with this function

	// Cast the provided generic cursor instance back into an epg_vtab_cursor instance
	epg_vtab_cursor* epgcursor = reinterpret_cast<epg_vtab_cursor*>(cursor);
	assert(epgcursor != nullptr);

	// write_function (local)
	//
	// cURL write callback to append data to the provided byte_string instance
	auto write_function = [](void const* data, size_t size, size_t count, void* userdata) -> size_t {

		try { reinterpret_cast<byte_string*>(userdata)->append(reinterpret_cast<uint8_t const*>(data), size * count); }
		catch(...) { return 0; }
		
		return size * count;
	};

	try {

		// All four arguments must have been specified by xBestIndex
		if(argc != 4) throw string_exception(__func__, ": invalid argument count provided by xBestIndex");

		// Assign the deviceauth string to the epg_vtab_cursor instance; must be present
		epgcursor->deviceauth.assign(reinterpret_cast<char const*>(sqlite3_value_text(argv[0])));
		if(epgcursor->deviceauth.length() == 0) throw string_exception(__func__, ": null or zero-length deviceauth string");

		// Assign the channel string to the epg_vtab_cursor instance; must be present
		epgcursor->channel.assign(reinterpret_cast<char const*>(sqlite3_value_text(argv[1])));
		if(epgcursor->channel.length() == 0) throw string_exception(__func__, ": null or zero-length channel string");

		// Assign the start and end time values to the cursor instance
		epgcursor->starttime = sqlite3_value_int(argv[2]);
		epgcursor->endtime = sqlite3_value_int(argv[3]);

		// Use local variables to track starttime and endtime as the queries are generated
		time_t starttime = epgcursor->starttime;
		time_t endtime = epgcursor->endtime;

		// Initialize a cURL multiple interface session to handle the pipelined/multiplexed data transfers
		CURLM* curlm = curl_multi_init();
		if(curlm == nullptr) throw string_exception(__func__, "curl_multi_init() failed");

		try {

			// Create a list<> to hold the individual transfer information (no iterator invalidation)
			std::list<std::pair<CURL*, byte_string>> transfers;

			try {

				// Create all of the required individual transfer objects necessary to satisfy the EPG request.  The backend will
				// return no more than 8 hours of data per request, so break it up into 7.5 hour chunks to avoid any holes
				while(starttime < endtime) {

					// Create and initialize the cURL easy interface handle for this transfer operation
					CURL* curl = curl_easy_init();
					if(curl == nullptr) throw string_exception(__func__, "curl_easy_init() failed");

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
					if(curlresult == CURLE_OK) curlresult = curl_easy_setopt(curl, CURLOPT_SHARE, static_cast<CURLSH*>(curlshare));

					// Release the URL string after cURL initializations are complete
					sqlite3_free(url);

					// Verify that initialization of the cURL easy interface handle was completed successfully
					if(curlresult != CURLE_OK) throw string_exception(__func__, ": curl_easy_setopt() failed: ", curl_easy_strerror(curlresult));

					// Loop to the next required transfer instance; use a value of (27000 = 7.5 hours) to set 30 minutes of overlap
					starttime += 27000;
				}

				// Add all of the generated cURL easy interface objects to the cURL multi interface object
				for(auto const& transfer : transfers) {

					CURLMcode curlmresult = curl_multi_add_handle(curlm, transfer.first);
					if(curlmresult != CURLM_OK) throw string_exception(__func__, ": curl_multi_add_handle() failed: ", curl_multi_strerror(curlmresult));
				}

				// Execute the transfer operation(s) until they have all completed
				int numfds = 0;
				CURLMcode curlmresult = curl_multi_perform(curlm, &numfds);
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

					if(responsecode == 0) throw string_exception("no response from host");
					else if((responsecode < 200) || (responsecode > 299)) throw http_exception(responsecode);

					// HTTP 200: OK was received, add the transferred data into the cursor object as a new row
					epgcursor->rows.emplace_back(std::move(transfer.second));
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

		// Clean up and destroy the multi handle on exception.  Also reset the cURL
		// share interface to ensure a new DNS lookup and connection next time through
		catch(...) { curl_multi_cleanup(curlm); curlshare.reset(); throw; }
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
// http_request
//
// SQLite scalar function to load data from a URL as a blob
//
// Arguments:
//
//	context		- SQLite context object
//	argc		- Number of supplied arguments
//	argv		- Argument values

void http_request(sqlite3_context* context, int argc, sqlite3_value** argv)
{
	long				responsecode = 200;		// HTTP response code
	byte_string			blob;					// Dynamically allocated blob buffer
	static curlshare	curlshare;				// Static curlshare instance used only with this function

	if((argc < 1) || (argc > 2) || (argv[0] == nullptr)) return sqlite3_result_error(context, "invalid argument", -1);

	// A null or zero-length URL results in a NULL result
	const char* url = reinterpret_cast<const char*>(sqlite3_value_text(argv[0]));
	if((url == nullptr) || (*url == 0)) return sqlite3_result_null(context);

	// Create a write callback for libcurl to invoke to write the data
	auto write_function = [](void const* data, size_t size, size_t count, void* userdata) -> size_t {

		try { reinterpret_cast<byte_string*>(userdata)->append(reinterpret_cast<uint8_t const*>(data), size * count); }
		catch(...) { return 0; }
		
		return size * count;
	};

#if defined(_WINDOWS) && defined(_DEBUG)
	// Dump the target URL to the debugger on Windows _DEBUG builds to watch for URL duplication
	char debugurl[256];
	snprintf(debugurl, std::extent<decltype(debugurl)>::value, "%s: %s\r\n", __func__, url);
	OutputDebugStringA(debugurl);
#endif

	// Initialize the CURL session for the download operation
	CURL* curl = curl_easy_init();
	if(curl == nullptr) return sqlite3_result_error(context, "cannot initialize libcurl object", -1);

	// Set the CURL options and execute the web request to get the JSON string data
	CURLcode curlresult = curl_easy_setopt(curl, CURLOPT_URL, url);
	if(curlresult == CURLE_OK) curlresult = curl_easy_setopt(curl, CURLOPT_USERAGENT, g_useragent.c_str());
	if(curlresult == CURLE_OK) curlresult = curl_easy_setopt(curl, CURLOPT_NOSIGNAL, 1);
	if(curlresult == CURLE_OK) curlresult = curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);
	if(curlresult == CURLE_OK) curlresult = curl_easy_setopt(curl, CURLOPT_MAXREDIRS, 5L);
	if(curlresult == CURLE_OK) curlresult = curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT, 10L);
	if(curlresult == CURLE_OK) curlresult = curl_easy_setopt(curl, CURLOPT_IPRESOLVE, CURL_IPRESOLVE_V4);
	if(curlresult == CURLE_OK) curlresult = curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, static_cast<curl_writefunction>(write_function));
	if(curlresult == CURLE_OK) curlresult = curl_easy_setopt(curl, CURLOPT_WRITEDATA, reinterpret_cast<void*>(&blob));
	if(curlresult == CURLE_OK) curlresult = curl_easy_setopt(curl, CURLOPT_SHARE, static_cast<CURLSH*>(curlshare));
	if(curlresult == CURLE_OK) curlresult = curl_easy_perform(curl);
	if(curlresult == CURLE_OK) curlresult = curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &responsecode);
	curl_easy_cleanup(curl);

	// Check if any of the above operations failed and return an error condition
	if(curlresult != CURLE_OK) {

		// If a default result was provided, use it rather than returning an error result
		if(argc >= 2) return sqlite3_result_value(context, argv[1]);
	
		// Use sqlite3_mprintf to generate the formatted error message
		auto message = sqlite3_mprintf("http request on [%s] failed: %s", url, curl_easy_strerror(curlresult));
		sqlite3_result_error(context, message, -1);
		return sqlite3_free(reinterpret_cast<void*>(message));
	}

	// Check the HTTP response code and return an error condition if unsuccessful
	if((responsecode < 200) || (responsecode > 299)) {
	
		// If a default result was provided, use it rather than returning an error result
		if(argc >= 2) return sqlite3_result_value(context, argv[1]);
	
		// Use sqlite3_mprintf to generate the formatted error message
		auto message = sqlite3_mprintf("http request on url [%s] failed with http response code %ld", url, responsecode);
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

	// get_season_number function
	//
	result = sqlite3_create_function_v2(db, "get_season_number", 1, SQLITE_UTF8 | SQLITE_DETERMINISTIC, nullptr, get_season_number, nullptr, nullptr, nullptr);
	if(result != SQLITE_OK) { *errmsg = sqlite3_mprintf("Unable to register scalar function get_season_number"); return result; }

	// http_request (non-deterministic)
	//
	result = sqlite3_create_function_v2(db, "http_request", -1, SQLITE_UTF8, nullptr, http_request, nullptr, nullptr, nullptr);
	if(result != SQLITE_OK) { *errmsg = sqlite3_mprintf("Unable to register scalar function http_request"); return result; }

	// url_encode function
	//
	result = sqlite3_create_function_v2(db, "url_encode", 1, SQLITE_UTF8 | SQLITE_DETERMINISTIC, nullptr, url_encode, nullptr, nullptr, nullptr);
	if(result != SQLITE_OK) { *errmsg = sqlite3_mprintf("Unable to register scalar function url_encode"); return result; }

	return SQLITE_OK;
}

//---------------------------------------------------------------------------

#pragma warning(pop)
