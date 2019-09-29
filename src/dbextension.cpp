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

int generate_series_bestindex(sqlite3_vtab* vtab, sqlite3_index_info* info);
int generate_series_close(sqlite3_vtab_cursor* cursor);
int generate_series_column(sqlite3_vtab_cursor* cursor, sqlite3_context* context, int ordinal);
int generate_series_connect(sqlite3* instance, void* aux, int argc, const char* const* argv, sqlite3_vtab** vtab, char** err);
int generate_series_disconnect(sqlite3_vtab* vtab);
int generate_series_eof(sqlite3_vtab_cursor* cursor);
int generate_series_filter(sqlite3_vtab_cursor* cursor, int indexnum, char const* indexstr, int argc, sqlite3_value** argv);
int generate_series_next(sqlite3_vtab_cursor* cursor);
int generate_series_open(sqlite3_vtab* vtab, sqlite3_vtab_cursor** cursor);
int generate_series_rowid(sqlite3_vtab_cursor* cursor, sqlite_int64* rowid);

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

// generate_series_vtab_columns
//
// Constants indicating the generate_series virtual table column ordinals
enum generate_series_vtab_columns {

	value = 0,			// value integer
	start,				// start integer hidden
	stop,				// stop integer hidden
	step,				// step integer hidden
};

// generate_series_vtab
//
// Subclassed version of sqlite3_vtab for the generate_series virtual table
struct generate_series_vtab : public sqlite3_vtab
{
	// Instance Constructor
	//
	generate_series_vtab() { memset(static_cast<sqlite3_vtab*>(this), 0, sizeof(sqlite3_vtab)); }
};

// generate_series_vtab_cursor
//
// Subclassed version of sqlite3_vtab_cursor for the generate_series virtual table
struct generate_series_vtab_cursor : public sqlite3_vtab_cursor
{
	// Instance Constructor
	//
	generate_series_vtab_cursor() { memset(static_cast<sqlite3_vtab_cursor*>(this), 0, sizeof(sqlite3_vtab_cursor)); }

	// Fields
	//
	bool				desc = false;			// descending flag
	sqlite3_int64		rowid = 0;				// rowid
	sqlite3_int64		value = 0;				// current value
	sqlite3_int64		minvalue = 0;			// minimum value
	sqlite3_int64		maxvalue = 0;			// maximum value
	sqlite3_int64		step = 0;				// increment
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

// g_generate_series_module
//
// Defines the entry points for the generate_series virtual table
static sqlite3_module g_generate_series_module = {

	0,							// iVersion
	nullptr,					// xCreate
	generate_series_connect,	// xConnect
	generate_series_bestindex,	// xBestIndex
	generate_series_disconnect,	// xDisconnect
	nullptr,					// xDestroy
	generate_series_open,		// xOpen
	generate_series_close,		// xClose
	generate_series_filter,		// xFilter
	generate_series_next,		// xNext
	generate_series_eof,		// xEof
	generate_series_column,		// xColumn
	generate_series_rowid,		// xRowid
	nullptr,					// xUpdate
	nullptr,					// xBegin
	nullptr,					// xSync
	nullptr,					// xCommit
	nullptr,					// xRollback
	nullptr,					// xFindMethod
	nullptr,					// xRename
	nullptr,					// xSavepoint
	nullptr,					// xRelease
	nullptr,					// xRollbackTo
	nullptr						// xShadowName
};

// g_useragent
//
// Static string to use as the User-Agent for database driven HTTP requests
static std::string g_useragent = "Kodi-PVR/" + std::string(XBMC_PVR_API_VERSION) + " " + VERSION_PRODUCTNAME_ANSI + "/" + VERSION_VERSION3_ANSI;

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
// generate_series_bestindex
//
// Determines the best index to use when querying the virtual table
//
// Arguments:
//
//	vtab	- Virtual Table instance
//	info	- Selected index information to populate

int generate_series_bestindex(sqlite3_vtab* /*vtab*/, sqlite3_index_info* info)
{
	int			indexmask = 0;				// The query plan bitmask
	int			unusablemask = 0;			// Mask of unusable constraints
	int			numargs = 0;				// Number of arguments that xFilter() expects
	int			usableconstraints[3];		// Usable constraints on start, stop, and step

	// Notes from series.c (sqlite-src/ext/misc)
	//
	// SQLite will invoke this method one or more times while planning a query that uses the generate_series 
	// virtual table.  This routine needs to create a query plan for each invocation and compute an estimated 
	// cost for that plan. In this implementation bitmask is used to represent the query plan; idxStr is unused.
	//
	// The query plan is represented by bits in bitmask:
	//
	//  (1)  start = $value   -- constraint exists
	//  (2)  stop =  $value   -- constraint exists
	//  (4)  step =  $value   -- constraint exists
	//  (8)  output in descending order
		
	// This implementation assumes that the start, stop, and step columns are the last three columns in the virtual table
	assert(static_cast<int>(generate_series_vtab_columns::stop) == static_cast<int>(generate_series_vtab_columns::start) + 1);
	assert(static_cast<int>(generate_series_vtab_columns::step) == static_cast<int>(generate_series_vtab_columns::start) + 2);

	// Initialize the usable constraints array
	usableconstraints[0] = usableconstraints[1] = usableconstraints[2] = -1;

	// Iterate over the provided constraints to determine which are usable
	auto constraint = info->aConstraint;
	for(int index = 0; index < info->nConstraint; index++, constraint++) {

		int		column;			// 0 for start, 1 for stop, 2 for step */
		int		bitmask;		// bitmask for those columns

		if(constraint->iColumn < static_cast<int>(generate_series_vtab_columns::start)) continue;
		column = constraint->iColumn - static_cast<int>(generate_series_vtab_columns::start);
		assert(column >= 0 && column <= 2);

		bitmask = 1 << column;
		if(constraint->usable == 0) { 
			
			unusablemask |= bitmask; 
			continue; 
		}
		else if(constraint->op == SQLITE_INDEX_CONSTRAINT_EQ) {

			indexmask |= bitmask;
			usableconstraints[column] = index;
		}
	}

	// Set up the array of usable constraints for xFilter() to consume
	for(int index = 0; index < 3; index++) {

		int usableindex = usableconstraints[index];
		if(usableindex >= 0) {

			info->aConstraintUsage[usableindex].argvIndex = ++numargs;
			info->aConstraintUsage[usableindex].omit = 1;
		}
	}

	// The start, stop, and step columns are inputs. Therefore if there are unusable constraints 
	// on any of start, stop, or step then this plan is unusable
	if((unusablemask & ~indexmask) != 0) return SQLITE_CONSTRAINT;

	// Both start= and stop= boundaries are available.  This is the the preferred case
	if((indexmask & 3) == 3) {

		info->estimatedCost = static_cast<double>(2 - ((indexmask & 4) != 0) ? 1 : 0);
		info->estimatedRows = 1000;
		if(info->nOrderBy == 1) {

			if(info->aOrderBy[0].desc) indexmask |= 8;
			info->orderByConsumed = 1;
		}
	}

	// If either boundary is missing, we have to generate a huge span of numbers. Make this case very 
	// expensive so that the query planner will work hard to avoid it
	else info->estimatedRows = 2147483647;
	
	info->idxNum = indexmask;

	return SQLITE_OK;
}

//---------------------------------------------------------------------------
// generate_series_close
//
// Closes and deallocates a virtual table cursor instance
//
// Arguments:
//
//	cursor		- Cursor instance allocated by xOpen

int generate_series_close(sqlite3_vtab_cursor* cursor)
{
	if(cursor != nullptr) delete reinterpret_cast<generate_series_vtab_cursor*>(cursor);

	return SQLITE_OK;
}

//---------------------------------------------------------------------------
// generate_series_column
//
// Accesses the data in the specified column of the current cursor row
//
// Arguments:
//
//	cursor		- Virtual table cursor instance
//	context		- Result context object
//	ordinal		- Ordinal of the column being accessed

int generate_series_column(sqlite3_vtab_cursor* cursor, sqlite3_context* context, int ordinal)
{
	// Cast the provided generic cursor instance back into an generate_series_vtab_cursor instance
	generate_series_vtab_cursor* seriescursor = reinterpret_cast<generate_series_vtab_cursor*>(cursor);
	assert(seriescursor != nullptr);

	// Return the current value corresponding to the column that has been requested
	switch(ordinal) {

		case generate_series_vtab_columns::value: sqlite3_result_int64(context, seriescursor->value); break;
		case generate_series_vtab_columns::start: sqlite3_result_int64(context, seriescursor->minvalue); break;
		case generate_series_vtab_columns::stop: sqlite3_result_int64(context, seriescursor->maxvalue); break;
		case generate_series_vtab_columns::step: sqlite3_result_int64(context, seriescursor->step); break;

		// Invalid ordinal or invalid row when accessing the value column yields null
		default: sqlite3_result_null(context);
	}

	return SQLITE_OK;
}

//---------------------------------------------------------------------------
// generate_series_connect
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

int generate_series_connect(sqlite3* instance, void* /*aux*/, int /*argc*/, const char* const* /*argv*/, sqlite3_vtab** vtab, char** err)
{
	// Declare the schema for the virtual table, use hidden columns for all of the filter criteria
	int result = sqlite3_declare_vtab(instance, "create table generate_series(value integer, start integer hidden, stop integer hidden, step integer hidden)");
	if(result != SQLITE_OK) return result;

	// Allocate and initialize the custom virtual table class
	try { *vtab = static_cast<sqlite3_vtab*>(new generate_series_vtab()); } 
	catch(std::exception const& ex) { *err = sqlite3_mprintf("%s", ex.what()); return SQLITE_ERROR; } 
	catch(...) { return SQLITE_ERROR; }

	return (*vtab == nullptr) ? SQLITE_NOMEM : SQLITE_OK;
}

//---------------------------------------------------------------------------
// generate_series_disconnect
//
// Disconnects from the virtual table
//
// Arguments:
//
//	vtab		- Virtual table instance allocated by xConnect

int generate_series_disconnect(sqlite3_vtab* vtab)
{
	if(vtab != nullptr) delete reinterpret_cast<generate_series_vtab*>(vtab);

	return SQLITE_OK;
}

//---------------------------------------------------------------------------
// generate_series_eof
//
// Determines if the specified cursor has moved beyond the last row of data
//
// Arguments:
//
//	cursor		- Virtual table cursor instance

int generate_series_eof(sqlite3_vtab_cursor* cursor)
{
	// Cast the provided generic cursor instance back into an generate_series_vtab_cursor instance
	generate_series_vtab_cursor* seriescursor = reinterpret_cast<generate_series_vtab_cursor*>(cursor);
	assert(seriescursor != nullptr);

	// Return 1 if the current values exceeds the min/max value for the cursor
	return (seriescursor->desc) ? seriescursor->value < seriescursor->minvalue : seriescursor->value > seriescursor->maxvalue;
}

//---------------------------------------------------------------------------
// generate_series_filter
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

int generate_series_filter(sqlite3_vtab_cursor* cursor, int indexnum, char const* /*indexstr*/, int argc, sqlite3_value** argv)
{
	int				argindex = 0;					// Index into the provided argv[] array

	// Cast the provided generic cursor instance back into a generate_series_vtab_cursor instance
	generate_series_vtab_cursor* seriescursor = reinterpret_cast<generate_series_vtab_cursor*>(cursor);
	assert(seriescursor != nullptr);

	// Notes from series.c (sqlite-src/ext/misc):
	//
	// This method is called to "rewind" the series_cursor object back to the first row of output. This method 
	// is always called at least once prior to any call to xColumn() or xRowid() or xEof()
	//
	// The query plan selected by generate_series_bestindex is passed in the indexnum parameter (indexstr is not 
	// used in this implementation).  indexnum is a bitmask showing which constraints are available:
	//
	// 1 : start = VALUE
	// 2 : stop = VALUE
	// 4 : step = VALUE
	// 
	// Also, if bit 8 is set, that means that the series should be output in descending order rather than in ascending order
	//
	// This routine should initialize the cursor and position it so that it is pointing at the first row, or pointing off 
	// the end of the table (so that xEof() will return true) if the table is empty

	// 1: minvalue
	seriescursor->minvalue = (indexnum & 1) ? sqlite3_value_int64(argv[argindex++]) : 0;
	
	// 2: maxvalue
	seriescursor->maxvalue = (indexnum & 2) ? sqlite3_value_int64(argv[argindex++]) : std::numeric_limits<sqlite3_int64>::max();
	
	// 4: step
	seriescursor->step = (indexnum & 4) ? sqlite3_value_int64(argv[argindex++]) : 1;
	if(seriescursor->step < 1) seriescursor->step = 1;
	
	// If any of the constraints have a NULL value, return no rows
	for(int index = 0; index < argc; index++) {

		if(sqlite3_value_type(argv[index]) == SQLITE_NULL) {

			seriescursor->minvalue = 1;
			seriescursor->maxvalue = 0;
			break;
		}
	}

	// 8: desc
	seriescursor->desc = ((indexnum & 8) == 8);

	// Set the initial value, taking into account the descending flag
	seriescursor->value = (seriescursor->desc) ? seriescursor->maxvalue : seriescursor->minvalue;
	if((seriescursor->desc) && (seriescursor->step > 0)) seriescursor->value -= (seriescursor->maxvalue - seriescursor->minvalue) % seriescursor->step;

	// Set the initial rowid value
	seriescursor->rowid = 1;

	return SQLITE_OK;
}

//---------------------------------------------------------------------------
// generate_series_open
//
// Creates and intializes a new virtual table cursor instance
//
// Arguments:
//
//	vtab		- Virtual table instance
//	cursor		- On success contains the allocated virtual table cursor instance

int generate_series_open(sqlite3_vtab* /*vtab*/, sqlite3_vtab_cursor** cursor)
{
	// Allocate and initialize the custom virtual table cursor class
	try { *cursor = static_cast<sqlite3_vtab_cursor*>(new generate_series_vtab_cursor()); } 
	catch(...) { return SQLITE_ERROR; }

	return (*cursor == nullptr) ? SQLITE_NOMEM : SQLITE_OK;
}

//---------------------------------------------------------------------------
// generate_series_next
//
// Advances the virtual table cursor to the next row
//
// Arguments:
//
//	cursor		- Virtual table cusror instance

int generate_series_next(sqlite3_vtab_cursor* cursor)
{
	// Cast the provided generic cursor instance back into an generate_series_vtab_cursor instance
	generate_series_vtab_cursor* seriescursor = reinterpret_cast<generate_series_vtab_cursor*>(cursor);
	assert(seriescursor != nullptr);

	// Check if the operation is ascending or descending and increment/decrement the value accordingly
	seriescursor->value = (seriescursor->desc) ? seriescursor->value - seriescursor->step : seriescursor->value + seriescursor->step;

	// Increment the rowid for the virtual table
	seriescursor->rowid++;

	return SQLITE_OK;
}

//---------------------------------------------------------------------------
// generate_series_rowid
//
// Retrieves the ROWID for the current virtual table cursor row
//
// Arguments:
//
//	cursor		- Virtual table cursor instance
//	rowid		- On success contains the ROWID for the current row

int generate_series_rowid(sqlite3_vtab_cursor* cursor, sqlite_int64* rowid)
{
	// Cast the provided generic cursor instance back into an generate_series_vtab_cursor instance
	generate_series_vtab_cursor* seriescursor = reinterpret_cast<generate_series_vtab_cursor*>(cursor);
	assert(seriescursor != nullptr);

	// Return the current ROWID for the cursor instance
	*rowid = seriescursor->rowid;

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

	// generate_series virtual table
	//
	result = sqlite3_create_module_v2(db, "generate_series", &g_generate_series_module, nullptr, nullptr);
	if(result != SQLITE_OK) { *errmsg = sqlite3_mprintf("Unable to register virtual table module generate_series"); return result; }

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
