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

#include "stdafx.h"
#include "database.h"

#include <algorithm>
#include <cctype>
#include <exception>
#include <stdint.h>
#include <string.h>
#include <uuid/uuid.h>
#include <xbmc_pvr_types.h>

#include "curlshare.h"
#include "hdhr.h"
#include "sqlite_exception.h"
#include "string_exception.h"

#pragma warning(push, 4)

// pvr.cpp (via version.h)
//
extern char const VERSION_PRODUCTNAME_ANSI[];
extern char const VERSION_VERSION3_ANSI[];

// Check SQLITE_THREADSAFE
//
#if (SQLITE_THREADSAFE != 2)
#error SQLITE_THREADSAFE must be defined and set to 2
#endif

// Check SQLITE_ENABLE_JSON1
//
#if (SQLITE_ENABLE_JSON1 != 1)
#error SQLITE_ENABLE_JSON1 must be defined and set to 1
#endif

// Check SQLITE_TEMP_STORE
//
#if (SQLITE_TEMP_STORE != 3)
#error SQLITE_TEMP_STORE must be defined and set to 3
#endif

//---------------------------------------------------------------------------
// FUNCTION PROTOTYPES
//---------------------------------------------------------------------------

void clean_filename(sqlite3_context* context, int argc, sqlite3_value** argv);
void decode_channel_id(sqlite3_context* context, int argc, sqlite3_value** argv);
bool discover_devices_broadcast(sqlite3* instance, bool excludestorage);
bool discover_devices_http(sqlite3* instance, bool excludestorage);
void encode_channel_id(sqlite3_context* context, int argc, sqlite3_value** argv);
void fnv_hash(sqlite3_context* context, int argc, sqlite3_value** argv);
void generate_uuid(sqlite3_context* context, int argc, sqlite3_value** argv);
void get_channel_number(sqlite3_context* context, int argc, sqlite3_value** argv);
void get_episode_number(sqlite3_context* context, int argc, sqlite3_value** argv);
void get_season_number(sqlite3_context* context, int argc, sqlite3_value** argv);
void http_request(sqlite3_context* context, int argc, sqlite3_value** argv);
void url_encode(sqlite3_context* context, int argc, sqlite3_value** argv);

//---------------------------------------------------------------------------
// TYPE DECLARATIONS
//---------------------------------------------------------------------------

// CURL_WRITEFUNCTION
//
// Function pointer for a CURL write function implementation
typedef size_t(*CURL_WRITEFUNCTION)(void const*, size_t, size_t, void*);

// sqlite_buffer
//
// A simple dynamically allocated buffer used to collect incremental data
// that can then be passed into SQLite and released with sqlite3_free
class sqlite_buffer
{
public:

	// Constructor / Destructor
	//
	sqlite_buffer() {}
	~sqlite_buffer() { if(m_data) sqlite3_free(m_data); }

	// append
	//
	// Appends data into the buffer
	size_t append(void const* data, size_t length)
	{
		if((data == nullptr) || (length == 0)) return 0;

		// sqlite3_malloc accepts a signed integer value, not a size_t
		if(length > static_cast<size_t>(std::numeric_limits<int>::max())) throw std::bad_alloc();

		// If the buffer has not been allocated, allocate a single chunk
		if(m_data == nullptr) {

			m_data = reinterpret_cast<uint8_t*>(sqlite3_malloc(static_cast<int>(length)));
			if(m_data == nullptr) throw std::bad_alloc();

			m_size = length;
			m_position = 0;
		}

		// If the buffer has been exhausted, allocate another chunk
		else {

			// sqlite3_realloc accepts a signed integer value, not a size_t
			if(m_size + length > static_cast<size_t>(std::numeric_limits<int>::max())) throw std::bad_alloc();

			uint8_t* newdata = reinterpret_cast<uint8_t*>(sqlite3_realloc(m_data, static_cast<int>(m_size + length)));
			if(newdata == nullptr) throw std::bad_alloc();

			m_data = newdata;
			m_size += length;
		}

		// Append the data to the current position in the reallocated buffer
		memcpy(&m_data[m_position], data, length);
		m_position += length;

		return length;
	}

	// detach
	//
	// Detaches the buffer pointer, presumably to hand it to SQLite.
	// Caller is responsible for calling sqlite3_free() on the pointer
	uint8_t* detach(void)
	{
		uint8_t* result = m_data;
		
		m_data = nullptr;
		m_size = m_position = 0;

		return result;
	}

	// size
	//
	// Gets the length of the data in the buffer
	size_t size(void) const { return m_position; }

private:

	sqlite_buffer(sqlite_buffer const&)=delete;
	sqlite_buffer& operator=(sqlite_buffer const&)=delete;

	//-----------------------------------------------------------------------
	// Member Variables

	uint8_t*				m_data = nullptr;			// Allocated data
	size_t					m_size = 0;					// Allocation length
	size_t					m_position = 0;				// Current position
};

//---------------------------------------------------------------------------
// GLOBAL VARIABLES
//---------------------------------------------------------------------------

// g_curlshare
//
// Global curlshare instance used with all easy interface handles generated
// by the database layer via the http_request function
static curlshare g_curlshare;

//
// CONNECTIONPOOL IMPLEMENTATION
//

//---------------------------------------------------------------------------
// connectionpool Constructor
//
// Arguments:
//
//	connstring		- Database connection string
//	flags			- Database connection flags

connectionpool::connectionpool(char const* connstring, int flags) : m_connstr((connstring) ? connstring : ""), m_flags(flags)
{
	sqlite3*		handle = nullptr;		// Initial database connection

	if(connstring == nullptr) throw std::invalid_argument("connstring");

	// Create and pool the initial connection now to give the caller an opportunity
	// to catch any exceptions during initialization of the database
	handle = open_database(m_connstr.c_str(), m_flags, true);
	m_connections.push_back(handle);
	m_queue.push(handle);
}

//---------------------------------------------------------------------------
// connectionpool Destructor

connectionpool::~connectionpool()
{
	// Close all of the connections that were created in the pool
	for(auto const& iterator : m_connections) close_database(iterator);
}

//---------------------------------------------------------------------------
// connectionpool::acquire
//
// Acquires a database connection, opening a new one if necessary
//
// Arguments:
//
//	NONE

sqlite3* connectionpool::acquire(void)
{
	sqlite3* handle = nullptr;				// Handle to return to the caller

	std::unique_lock<std::mutex> lock(m_lock);

	if(m_queue.empty()) {

		// No connections are available, open a new one using the same flags
		handle = open_database(m_connstr.c_str(), m_flags, false);
		m_connections.push_back(handle);
	}

	// At least one connection is available for reuse
	else { handle = m_queue.front(); m_queue.pop(); }

	return handle;
}

//---------------------------------------------------------------------------
// connectionpool::release
//
// Releases a database handle acquired from the pool
//
// Arguments:
//
//	handle		- Handle to be releases

void connectionpool::release(sqlite3* handle)
{
	std::unique_lock<std::mutex> lock(m_lock);

	if(handle == nullptr) throw std::invalid_argument("handle");

	m_queue.push(handle);
}

//---------------------------------------------------------------------------
// add_recordingrule
//
// Adds a new recording rule to the database
//
// Arguments:
//
//	instance		- Database instance
//	recordingrule	- New recording rule to be added

void add_recordingrule(sqlite3* instance, struct recordingrule const& recordingrule)
{
	sqlite3_stmt*				statement;			// SQL statement to execute
	int							result;				// Result from SQLite function
	
	if(instance == nullptr) return;

	// Clone the episode table schema into a temporary table to avoid the WHERE clause problem below
	execute_non_query(instance, "drop table if exists add_recordingrule_temp");
	execute_non_query(instance, "create temp table add_recordingrule_temp as select * from episode limit 0");

	try {

		// Start a database transaction
		execute_non_query(instance, "begin immediate transaction");

		try {

			// Add the new recording rule and replace/insert all updated rules for the series
			auto sql = "replace into recordingrule "
				"select json_extract(value, '$.RecordingRuleID') as recordingruleid, "
				"json_extract(value, '$.SeriesID') as seriesid, "
				"value as data "
				"from "
				"json_each((with deviceauth(code) as (select url_encode(group_concat(json_extract(data, '$.DeviceAuth'), '')) from device) "
				"select nullif(http_request('http://api.hdhomerun.com/api/recording_rules?DeviceAuth=' || coalesce(deviceauth.code, '') || '&Cmd=add&SeriesID=' || ?1 || "
				"case when ?2 is null then '' else '&RecentOnly=' || ?2 end || "
				"case when ?3 is null then '' else '&ChannelOnly=' || decode_channel_id(?3) end || "
				"case when ?4 is null then '' else '&AfterOriginalAirdateOnly=' || strftime('%s', date(?4, 'unixepoch')) end || "
				"case when ?5 is null then '' else '&DateTimeOnly=' || ?5 end || "
				"case when ?6 is null then '' else '&StartPadding=' || ?6 end || "
				"case when ?7 is null then '' else '&EndPadding=' || ?7 end), 'null') as data "
				"from deviceauth))";

			// Prepare the query
			result = sqlite3_prepare_v2(instance, sql, -1, &statement, nullptr);
			if(result != SQLITE_OK) throw sqlite_exception(result, sqlite3_errmsg(instance));

			try {

				// Bind the non-null query parameter(s)
				result = sqlite3_bind_text(statement, 1, recordingrule.seriesid, -1, SQLITE_STATIC);
				if((result == SQLITE_OK) && (recordingrule.recentonly)) result = sqlite3_bind_int(statement, 2, 1);
				if((result == SQLITE_OK) && (recordingrule.channelid.value != 0)) result = sqlite3_bind_int(statement, 3, recordingrule.channelid.value);
				if((result == SQLITE_OK) && (recordingrule.afteroriginalairdateonly != 0)) result = sqlite3_bind_int(statement, 4, static_cast<int>(recordingrule.afteroriginalairdateonly));
				if((result == SQLITE_OK) && (recordingrule.datetimeonly != 0)) result = sqlite3_bind_int(statement, 5, static_cast<int>(recordingrule.datetimeonly));
				if((result == SQLITE_OK) && (recordingrule.startpadding != 30)) result = sqlite3_bind_int(statement, 6, recordingrule.startpadding);
				if((result == SQLITE_OK) && (recordingrule.endpadding != 30))  result = sqlite3_bind_int(statement, 7, recordingrule.endpadding);
				if(result != SQLITE_OK) throw sqlite_exception(result);

				// Execute the query - no result set is expected
				result = sqlite3_step(statement);
				if(result == SQLITE_ROW) throw string_exception(__func__, ": unexpected result set returned from non-query");
				if(result != SQLITE_DONE) throw sqlite_exception(result, sqlite3_errmsg(instance));

				sqlite3_finalize(statement);			// Finalize the SQLite statement
			}

			catch(...) { sqlite3_finalize(statement); throw; }

			//
			// NOTE: This had to be broken up into a multi-step query involving a temp table to avoid a SQLite bug/feature
			// wherein using a function (http_request in this case) as part of a column definition is reevaluated when
			// that column is subsequently used as part of a WHERE clause:
			//
			// [http://mailinglists.sqlite.org/cgi-bin/mailman/private/sqlite-users/2015-August/061083.html]
			//
			
			// Update the episode data to take the new recording rule(s) into account; watch out for the web
			// services returning 'null' on the episode query -- this happens when there are no episodes
			sql = "with deviceauth(code) as (select url_encode(group_concat(json_extract(data, '$.DeviceAuth'), '')) from device) "
				"insert into add_recordingrule_temp select ?1 as seriesid, "
				"http_request('http://api.hdhomerun.com/api/episodes?DeviceAuth=' || coalesce(deviceauth.code, '') || '&SeriesID=' || ?1) as data "
				"from deviceauth";

			// Prepare the query
			result = sqlite3_prepare_v2(instance, sql, -1, &statement, nullptr);
			if(result != SQLITE_OK) throw sqlite_exception(result, sqlite3_errmsg(instance));

			try {

				// Bind the non-null query parameter(s)
				result = sqlite3_bind_text(statement, 1, recordingrule.seriesid, -1, SQLITE_STATIC);
				if(result != SQLITE_OK) throw sqlite_exception(result);

				// Execute the query - no result set is expected
				result = sqlite3_step(statement);
				if(result == SQLITE_ROW) throw string_exception(__func__, ": unexpected result set returned from non-query");
				if(result != SQLITE_DONE) throw sqlite_exception(result, sqlite3_errmsg(instance));

				sqlite3_finalize(statement);			// Finalize the SQLite statement
			}

			catch(...) { sqlite3_finalize(statement); throw; }

			// Complete the replace operation into the episode table from the temporary table that was generated above
			execute_non_query(instance, "replace into episode select seriesid, data from add_recordingrule_temp where cast(data as text) <> 'null'");

			// Commit the transaction
			execute_non_query(instance, "commit transaction");
		}

		// Rollback the entire transaction on any failure above
		catch(...) { try_execute_non_query(instance, "rollback transaction"); throw; }

		// Poke the recording engine(s) after a successful rule change; don't worry about exceptions
		try_execute_non_query(instance, "select http_request(json_extract(data, '$.BaseURL') || '/recording_events.post?sync') from device where type = 'storage'");

		// Drop the temporary table
		execute_non_query(instance, "drop table add_recordingrule_temp");
	}

	// Drop the temporary table on any exception
	catch(...) { execute_non_query(instance, "drop table add_recordingrule_temp"); throw; }
}

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
// clear_database
//
// Clears all discovery data from the database
//
// Arguments:
//
//	instance	- Database instance handle

void clear_database(sqlite3* instance)
{
	if(instance == nullptr) return;

	// Not very interesting, just delete all the data from each discovery table
	execute_non_query(instance, "delete from episode");
	execute_non_query(instance, "delete from recordingrule");
	execute_non_query(instance, "delete from guide");
	execute_non_query(instance, "delete from recording");
	execute_non_query(instance, "delete from lineup");
	execute_non_query(instance, "delete from device");
}

//---------------------------------------------------------------------------
// close_database
//
// Closes a SQLite database handle
//
// Arguments:
//
//	instance	- Database instance handle to be closed

void close_database(sqlite3* instance)
{
	if(instance) sqlite3_close(instance);
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
// delete_recording
//
// Deletes a recording from the database instance
//
// Arguments:
//
//	instance		- Database instance
//	recordingid		- Recording ID (CmdURL) of the item to delete
//	rerecord		- Flag to rerecord this episode in the future

void delete_recording(sqlite3* instance, char const* recordingid, bool rerecord)
{
	sqlite3_stmt*				statement;			// SQL statement to execute
	int							result;				// Result from SQLite function
	
	if((instance == nullptr) || (recordingid == nullptr)) return;

	// Prepare a query that will delete the specified recording from the storage device and the database
	auto sql = "with httprequest(response) as (select http_request(?1 || '&cmd=delete&rerecord=' || ?2)) "
		"replace into recording select "
		"deviceid, "
		"(select case when fullkey is null then recording.data else json_remove(recording.data, fullkey) end) as data "
		"from httprequest, recording, "
		"(select fullkey from recording, json_each(recording.data) as entry where json_extract(entry.value, '$.CmdURL') like ?1 limit 1)";

	result = sqlite3_prepare_v2(instance, sql, -1, &statement, nullptr);
	if(result != SQLITE_OK) throw sqlite_exception(result, sqlite3_errmsg(instance));

	try {

		// Bind the query parameter(s)
		result = sqlite3_bind_text(statement, 1, recordingid, -1, SQLITE_STATIC);
		if(result == SQLITE_OK) result = sqlite3_bind_int(statement, 2, (rerecord) ? 1 : 0);
		if(result != SQLITE_OK) throw sqlite_exception(result);

		// Execute the query; there shouldn't be any result set returned from it
		result = sqlite3_step(statement);
		if(result == SQLITE_ROW) throw string_exception(__func__, ": unexpected result set returned from non-query");
		if(result != SQLITE_DONE) throw sqlite_exception(result, sqlite3_errmsg(instance));

		sqlite3_finalize(statement);			// Finalize the SQLite statement
	}

	catch(...) { sqlite3_finalize(statement); throw; }
}

//---------------------------------------------------------------------------
// delete_recordingrule
//
// Deletes a recordingrule from the database instance
//
// Arguments:
//
//	instance			- Database instance
//	recordingruleid		- Recording Rule ID of the item to delete

void delete_recordingrule(sqlite3* instance, unsigned int recordingruleid)
{
	sqlite3_stmt*				statement;			// SQL statement to execute
	int							result;				// Result from SQLite function
	
	if(instance == nullptr) return;

	execute_non_query(instance, "begin immediate transaction");

	try {

		// Delete the recording rule from the backend services and the local database
		auto sql = "with deviceauth(code) as (select url_encode(group_concat(json_extract(data, '$.DeviceAuth'), '')) from device) "
			"delete from recordingrule where recordingruleid in "
			"(select case when cast(http_request('http://api.hdhomerun.com/api/recording_rules?DeviceAuth=' || coalesce(deviceauth.code, '') || "
			"'&Cmd=delete&RecordingRuleID=' || ?1) as text) = 'null' then ?1 else null end from deviceauth)";

		result = sqlite3_prepare_v2(instance, sql, -1, &statement, nullptr);
		if(result != SQLITE_OK) throw sqlite_exception(result, sqlite3_errmsg(instance));

		try {

			// Bind the query parameter(s)
			result = sqlite3_bind_int(statement, 1, recordingruleid);
			if(result != SQLITE_OK) throw sqlite_exception(result);

			// Execute the query; there shouldn't be any result set returned from it
			result = sqlite3_step(statement);
			if(result == SQLITE_ROW) throw string_exception(__func__, ": unexpected result set returned from non-query");
			if(result != SQLITE_DONE) throw sqlite_exception(result, sqlite3_errmsg(instance));

			sqlite3_finalize(statement);			// Finalize the SQLite statement
		}

		catch(...) { sqlite3_finalize(statement); throw; }

		// Remove episode data that no longer has an associated recording rule
		execute_non_query(instance, "delete from episode where seriesid not in (select json_extract(data, '$.SeriesID') from recordingrule)");

		// Commit the transaction
		execute_non_query(instance, "commit transaction");
	}

	// Rollback the entire transaction on any failure above
	catch(...) { try_execute_non_query(instance, "rollback transaction"); throw; }

	// Poke the recording engine(s) after a successful rule change; don't worry about exceptions
	try_execute_non_query(instance, "select http_request(json_extract(data, '$.BaseURL') || '/recording_events.post?sync') from device where type = 'storage'");
}

//---------------------------------------------------------------------------
// discover_devices
//
// Reloads the information about the available devices
//
// Arguments:
//
//	instance		- SQLite database instance
//	usebroadcast	- Flag to use broadcast rather than HTTP discovery
//	excludestorage	- Flag to exclude storage devices from discovery

void discover_devices(sqlite3* instance, bool usebroadcast, bool excludestorage)
{
	bool ignored;
	return discover_devices(instance, usebroadcast, excludestorage, ignored);
}

//---------------------------------------------------------------------------
// discover_devices
//
// Reloads the information about the available devices
//
// Arguments:
//
//	instance		- SQLite database instance
//	usebroadcast	- Flag to use broadcast rather than HTTP discovery
//	excludestorage	- Flag to exclude storage devices from discovery
//	changed			- Flag indicating if the data has changed

void discover_devices(sqlite3* instance, bool usebroadcast, bool excludestorage, bool& changed)
{
	bool			hastuners = false;			// Flag indicating if any tuners were detected

	changed = false;							// Initialize [out] argument

	if(instance == nullptr) throw std::invalid_argument("instance");

	// Clone the device table schema into a temporary table
	execute_non_query(instance, "drop table if exists discover_device");
	execute_non_query(instance, "create temp table discover_device as select * from device limit 0");

	try {

		// The logic required to load the temp table from broadcast differs greatly from the method
		// used to load from the HTTP API; the specific mechanisms have been broken out into helpers
		hastuners = (usebroadcast) ? discover_devices_broadcast(instance, excludestorage) : discover_devices_http(instance, excludestorage);

		// If no tuner devices were found during discovery, throw an exception to abort the device discovery.
		// The intention here is to prevent transient discovery problems from clearing out the existing devices
		// and channel lineups from Kodi -- this causes problems with the EPG when they come back again
		if(!hastuners) throw string_exception(__func__, ": no tuner devices were discovered; aborting device discovery");

		// This requires a multi-step operation against the device table; start a transaction
		execute_non_query(instance, "begin immediate transaction");

		try {

			// Delete any entries in the main device table that are no longer present on the network
			if(execute_non_query(instance, "delete from device where deviceid not in (select deviceid from discover_device)") > 0) changed = true;

			// Insert any new devices detected on the network into the main device table separately from 
			// the REPLACE INTO below to track changes on a new device being discovered
			if(execute_non_query(instance, "insert into device select * from discover_device where deviceid not in (select deviceid from device)") > 0) changed = true;

			// Update the JSON for every device based on the discovery data; this is not considered a change as
			// the device authorization string changes routinely.  (REPLACE INTO is easier than UPDATE in this case)
			execute_non_query(instance, "replace into device select * from discover_device");
			
			// Commit the database transaction
			execute_non_query(instance, "commit transaction");
		}
		
		// Rollback the transaction on any exception
		catch(...) { try_execute_non_query(instance, "rollback transaction"); throw; }

		// Drop the temporary table
		execute_non_query(instance, "drop table discover_device");
	}

	// Drop the temporary table on any exception
	catch(...) { execute_non_query(instance, "drop table discover_device"); throw; }
}

//---------------------------------------------------------------------------
// discover_devices_broadcast
//
// discover_devices helper -- loads the discover_device table from UDP broadcast
//
// Arguments:
//
//	instance		- SQLite database instance
//	excludestorage	- Flag to exclude storage devices from discovery

bool discover_devices_broadcast(sqlite3* instance, bool excludestorage)
{
	bool					hastuners = false;		// Flag indicating tuners were found
	sqlite3_stmt*			statement;				// SQL statement to execute
	int						result;					// Result from SQLite function

	assert(instance != nullptr);

	// deviceid | type | data
	auto sql = "insert into discover_device values(printf('%08X', ?1), ?2, ?3)";
	result = sqlite3_prepare_v2(instance, sql, -1, &statement, nullptr);
	if(result != SQLITE_OK) throw sqlite_exception(result, sqlite3_errmsg(instance));

	try {

		// Enumerate the devices on the local network accessible via UDP broadcast and insert them
		// into the temp table using the baseurl as 'data' rather than the discovery JSON
		enumerate_devices([&](struct discover_device const& device) -> void { 

			// The presence or lack of tuner devices is used as the function return value
			if(device.devicetype == device_type::tuner) hastuners = true;

			// If the flag to exclude storage devices has been set, ignore them
			if((excludestorage) && (device.devicetype == device_type::storage)) return;
			
			// Bind the query parameter(s)
			result = sqlite3_bind_int(statement, 1, device.deviceid);
			if(result == SQLITE_OK) result = sqlite3_bind_text(statement, 2, (device.devicetype == device_type::tuner) ? "tuner" : "storage", -1, SQLITE_STATIC);
			if(result == SQLITE_OK) result = sqlite3_bind_text(statement, 3, device.baseurl, -1, SQLITE_STATIC);
			if(result != SQLITE_OK) throw sqlite_exception(result);

			// This is a non-query, it's not expected to return any rows
			result = sqlite3_step(statement);
			if(result == SQLITE_ROW) throw string_exception(__func__, ": unexpected result set returned from non-query");
			if(result != SQLITE_DONE) throw sqlite_exception(result, sqlite3_errmsg(instance));

			// Reset the prepared statement so that it can be executed again
			result = sqlite3_reset(statement);
			if(result != SQLITE_OK) throw sqlite_exception(result);
		});

		// Finalize the statement after all devices have been processed
		sqlite3_finalize(statement);
	}
		
	catch(...) { sqlite3_finalize(statement); throw; }

	// Replace the base URL temporarily stored in the data column with the full discovery JSON
	execute_non_query(instance, "update discover_device set data = http_request(data || '/discover.json')");

	// Update the deviceid column for storage devices, the broadcast mechanism has no means to return the StorageID
	execute_non_query(instance, "update discover_device set deviceid = coalesce(json_extract(data, '$.StorageID'), '00000000') where type = 'storage'");

	// Indicate if any tuner devices were detected during discovery or not
	return hastuners;
}

//---------------------------------------------------------------------------
// discover_devices_http
//
// discover_devices helper -- loads the discover_device table from the HTTP API
//
// Arguments:
//
//	instance		- SQLite database instance
//	excludestorage	- Flag to exclude storage devices from discovery

bool discover_devices_http(sqlite3* instance, bool excludestorage)
{
	sqlite3_stmt*				statement;				// Database query statement
	int							tuners = 0;				// Number of tuners found
	int							result;					// Result from SQLite function call

	assert(instance != nullptr);
	
	//
	// NOTE: This had to be broken up into a multi-step query involving a temp table to avoid a SQLite bug/feature
	// wherein using a function (http_request in this case) as part of a column definition is reevaluated when
	// that column is subsequently used as part of a WHERE clause:
	//
	// [http://mailinglists.sqlite.org/cgi-bin/mailman/private/sqlite-users/2015-August/061083.html]
	//

	// Discover the devices from the HTTP API and insert them into the discover_device temp table
	execute_non_query(instance, "drop table if exists discover_device_http");
	execute_non_query(instance, "create temp table discover_device_http as select "
		"coalesce(json_extract(discovery.value, '$.DeviceID'), json_extract(discovery.value, '$.StorageID')) as deviceid, "
		"case when json_type(discovery.value, '$.DeviceID') is not null then 'tuner' when json_type(discovery.value, '$.StorageID') is not null then 'storage' else 'unknown' end as type, "
		"http_request(json_extract(discovery.value, '$.DiscoverURL'), null) as data from json_each(http_request('http://api.hdhomerun.com/discover')) as discovery");
	
	// Given that this is a multi-step operation, it's easier to use a different query altogether to exclude storage devices from discover_device
	if(!excludestorage) execute_non_query(instance, "insert into discover_device select deviceid, type, data from discover_device_http where data is not null and json_extract(data, '$.Legacy') is null");
	else execute_non_query(instance, "insert into discover_device select deviceid, type, data from discover_device_http where data is not null and json_extract(data, '$.StorageID') is null and json_extract(data, '$.Legacy') is null");

	execute_non_query(instance, "drop table discover_device_http");

	// Determine if any tuner devices were discovered from the HTTP discovery query
	auto sql = "select count(deviceid) as numtuners from discover_device where type = 'tuner'";

	result = sqlite3_prepare_v2(instance, sql, -1, &statement, nullptr);
	if(result != SQLITE_OK) throw sqlite_exception(result, sqlite3_errmsg(instance));

	try { 
		
		// Execute the scalar query
		result = sqlite3_step(statement);

		// There should be a single SQLITE_ROW returned from the initial step
		if(result == SQLITE_ROW) tuners = sqlite3_column_int(statement, 0);
		else if(result != SQLITE_DONE) throw sqlite_exception(result, sqlite3_errmsg(instance));

		sqlite3_finalize(statement);
		return (tuners > 0);
	}

	catch(...) { sqlite3_finalize(statement); throw; }
}

//---------------------------------------------------------------------------
// discover_episodes
//
// Reloads the information about episodes associated with a recording rule
//
// Arguments:
//
//	instance	- SQLite database instance

void discover_episodes(sqlite3* instance)
{
	bool ignored;
	return discover_episodes(instance, ignored);
}

//---------------------------------------------------------------------------
// discover_episodes
//
// Reloads the information about episodes associated with a recording rule
//
// Arguments:
//
//	instance	- SQLite database instance
//	changed		- Flag indicating if the data has changed

void discover_episodes(sqlite3* instance, bool& changed)
{
	changed = false;							// Initialize [out] argument

	if(instance == nullptr) throw std::invalid_argument("instance");

	// Clone the episode table schema into a temporary table
	execute_non_query(instance, "drop table if exists discover_episode");
	execute_non_query(instance, "create temp table discover_episode as select * from episode limit 0");

	try {

		// Discover the episode information for each series that has a recording rule
		execute_non_query(instance, "with deviceauth(code) as (select url_encode(group_concat(json_extract(data, '$.DeviceAuth'), '')) from device) "
			"insert into discover_episode select entry.seriesid as seriesid, "
			"http_request('http://api.hdhomerun.com/api/episodes?DeviceAuth=' || coalesce(deviceauth.code, '') || '&SeriesID=' || entry.seriesid) as data "
			"from deviceauth, (select distinct json_extract(data, '$.SeriesID') as seriesid from recordingrule where seriesid is not null) as entry");

		// This requires a multi-step operation against the episode table; start a transaction
		execute_non_query(instance, "begin immediate transaction");

		try {

			// Delete any entries in the main episode table that are no longer present in the data
			if(execute_non_query(instance, "delete from episode where seriesid not in (select seriesid from discover_episode)") > 0) changed = true;

			// Delete any entries in the main episode table that returned 'null' from the backend query
			if(execute_non_query(instance, "delete from episode where seriesid in (select seriesid from discover_episode where data like 'null')") > 0) changed = true;

			// Insert/replace entries in the main episode table that are new or different; watch for discovered rows with
			// data set to 'null' - this happens when there is no episode information available for the series
			if(execute_non_query(instance, "replace into episode select discover_episode.* from discover_episode left outer join episode using(seriesid) "
				"where (discover_episode.data not like 'null') and (coalesce(episode.data, '') <> coalesce(discover_episode.data, ''))") > 0) changed = true;

			// Commit the database transaction
			execute_non_query(instance, "commit transaction");
		}
		
		// Rollback the transaction on any exception
		catch(...) { try_execute_non_query(instance, "rollback transaction"); throw; }

		// Drop the temporary table
		execute_non_query(instance, "drop table discover_episode");
	}

	// Drop the temporary table on any exception
	catch(...) { execute_non_query(instance, "drop table discover_episode"); throw; }
}

//---------------------------------------------------------------------------
// discover_guide
//
// Reloads the basic electronic program guide information
//
// Arguments:
//
//	instance	- SQLite database instance

void discover_guide(sqlite3* instance)
{
	bool ignored;
	return discover_guide(instance, ignored);
}

//---------------------------------------------------------------------------
// discover_guide
//
// Reloads the basic electronic program guide information
//
// Arguments:
//
//	instance	- SQLite database instance
//	changed		- Flag indicating if the data has changed

void discover_guide(sqlite3* instance, bool& changed)
{
	changed = false;							// Initialize [out] argument

	if(instance == nullptr) throw std::invalid_argument("instance");

	// Clone the guide table schema into a temporary table
	execute_non_query(instance, "drop table if exists discover_guide");
	execute_non_query(instance, "create temp table discover_guide as select * from guide limit 0");

	try {

		// Discover the electronic program guide from the network and insert it into a temporary table
		execute_non_query(instance, "with deviceauth(code) as (select url_encode(group_concat(json_extract(data, '$.DeviceAuth'), '')) from device) "
			"insert into discover_guide select "
			"encode_channel_id(json_extract(discovery.value, '$.GuideNumber')) as channelid, "
			"json_extract(discovery.value, '$.GuideName') as channelname, "
			"json_extract(discovery.value, '$.ImageURL') as iconurl, "
			"null as data "
			"from deviceauth, json_each(http_request('http://api.hdhomerun.com/api/guide?DeviceAuth=' || coalesce(deviceauth.code, ''))) as discovery");

		// This requires a multi-step operation against the guide table; start a transaction
		execute_non_query(instance, "begin immediate transaction");

		try {

			// Delete any entries in the main guide table that are no longer present in the data
			if(execute_non_query(instance, "delete from guide where channelid not in (select channelid from discover_guide)") > 0) changed = true;

			// Insert/replace entries in the main guide table that are new or different
			if(execute_non_query(instance, "replace into guide select discover_guide.* from discover_guide left outer join guide using(channelid) "
				"where coalesce(guide.channelname, '') <> coalesce(discover_guide.channelname, '') "
				"or coalesce(guide.iconurl, '') <> coalesce(discover_guide.iconurl, '')") > 0) changed = true;

			// Commit the database transaction
			execute_non_query(instance, "commit transaction");
		}
		
		// Rollback the transaction on any exception
		catch(...) { try_execute_non_query(instance, "rollback transaction"); throw; }

		// Drop the temporary table
		execute_non_query(instance, "drop table discover_guide");
	}

	// Drop the temporary table on any exception
	catch(...) { execute_non_query(instance, "drop table discover_guide"); throw; }
}

//---------------------------------------------------------------------------
// discover_lineups
//
// Reloads the information about the available channel lineups
//
// Arguments:
//
//	instance	- SQLite database instance

void discover_lineups(sqlite3* instance)
{
	bool ignored;
	return discover_lineups(instance, ignored);
}

//---------------------------------------------------------------------------
// discover_lineups
//
// Reloads the information about the available channel lineups
//
// Arguments:
//
//	instance	- SQLite database instance
//	changed		- Flag indicating if the data has changed

void discover_lineups(sqlite3* instance, bool& changed)
{
	changed = false;							// Initialize [out] argument

	if(instance == nullptr) throw std::invalid_argument("instance");

	// Clone the lineup table schema into a temporary table
	execute_non_query(instance, "drop table if exists discover_lineup");
	execute_non_query(instance, "create temp table discover_lineup as select * from lineup limit 0");

	try {

		//
		// NOTE: This had to be broken up into a multi-step query involving a temp table to avoid a SQLite bug/feature
		// wherein using a function (http_request in this case) as part of a column definition is reevaluated when
		// that column is subsequently used as part of a WHERE clause:
		//
		// [http://mailinglists.sqlite.org/cgi-bin/mailman/private/sqlite-users/2015-August/061083.html]
		//

		// Discover the channel lineups for all available tuner devices; watch for results that return 'null'
		execute_non_query(instance, "drop table if exists discover_lineup_temp");
		execute_non_query(instance, "create temp table discover_lineup_temp as select deviceid, http_request(json_extract(device.data, '$.LineupURL') || '?show=demo') as json from device where device.type = 'tuner'");
		execute_non_query(instance, "insert into discover_lineup select deviceid, json from discover_lineup_temp where cast(json as text) <> 'null'");
		execute_non_query(instance, "drop table discover_lineup_temp");

		// This requires a multi-step operation against the lineup table; start a transaction
		execute_non_query(instance, "begin immediate transaction");

		try {

			// Delete any entries in the main lineup table that are no longer present in the data
			if(execute_non_query(instance, "delete from lineup where deviceid not in (select deviceid from discover_lineup)") > 0) changed = true;

			// Insert/replace entries in the main lineup table that are new or different
			if(execute_non_query(instance, "replace into lineup select discover_lineup.* from discover_lineup left outer join lineup using(deviceid) "
				"where coalesce(lineup.data, '') <> coalesce(discover_lineup.data, '')") > 0) changed = true;

			// Commit the database transaction
			execute_non_query(instance, "commit transaction");
		}
		
		// Rollback the transaction on any exception
		catch(...) { try_execute_non_query(instance, "rollback transaction"); throw; }

		// Drop the temporary table
		execute_non_query(instance, "drop table discover_lineup");
	}

	// Drop the temporary table on any exception
	catch(...) { execute_non_query(instance, "drop table discover_lineup"); throw; }
}

//---------------------------------------------------------------------------
// discover_recordingrules
//
// Reloads the information about the available recording rules
//
// Arguments:
//
//	instance	- SQLite database instance

void discover_recordingrules(sqlite3* instance)
{
	bool ignored;
	return discover_recordingrules(instance, ignored);
}

//---------------------------------------------------------------------------
// discover_recordingrules
//
// Reloads the information about the available recording rules
//
// Arguments:
//
//	instance	- SQLite database instance
//	changed		- Flag indicating if the data has changed

void discover_recordingrules(sqlite3* instance, bool& changed)
{
	changed = false;							// Initialize [out] argument

	if(instance == nullptr) throw std::invalid_argument("instance");

	// Clone the recordingrule table schema into a temporary table
	execute_non_query(instance, "drop table if exists discover_recordingrule");
	execute_non_query(instance, "create temp table discover_recordingrule as select * from recordingrule limit 0");

	try {

		// Discover the information for the available recording rules
		execute_non_query(instance, "with deviceauth(code) as (select url_encode(group_concat(json_extract(data, '$.DeviceAuth'), '')) from device) "
			"insert into discover_recordingrule select "
			"json_extract(value, '$.RecordingRuleID') as recordingruleid, "
			"json_extract(value, '$.SeriesID') as seriesid, "
			"value as data "
			"from deviceauth, json_each(http_request('http://api.hdhomerun.com/api/recording_rules?DeviceAuth=' || coalesce(deviceauth.code, '')))");

		// This requires a multi-step operation against the recording table; start a transaction
		execute_non_query(instance, "begin immediate transaction");

		try {

			// Delete any entries in the main recordingrule table that are no longer present in the data
			if(execute_non_query(instance, "delete from recordingrule where recordingruleid not in (select recordingruleid from discover_recordingrule)") > 0) changed = true;

			// Insert/replace entries in the main recordingrule table that are new or different
			if(execute_non_query(instance, "replace into recordingrule select discover_recordingrule.* "
				"from discover_recordingrule left outer join recordingrule using(recordingruleid) "
				"where coalesce(recordingrule.seriesid, '') <> coalesce(discover_recordingrule.seriesid, '') "
				"or coalesce(recordingrule.data, '') <> coalesce(discover_recordingrule.data, '')") > 0) changed = true;

			// Commit the database transaction
			execute_non_query(instance, "commit transaction");
		}
		
		// Rollback the transaction on any exception
		catch(...) { try_execute_non_query(instance, "rollback transaction"); throw; }

		// Drop the temporary table
		execute_non_query(instance, "drop table discover_recordingrule");
	}

	// Drop the temporary table on any exception
	catch(...) { execute_non_query(instance, "drop table discover_recordingrule"); throw; }
}

//---------------------------------------------------------------------------
// discover_recordings
//
// Reloads the information about the available recordings
//
// Arguments:
//
//	instance	- SQLite database instance

void discover_recordings(sqlite3* instance)
{
	bool ignored;
	return discover_recordings(instance, ignored);
}

//---------------------------------------------------------------------------
// discover_recordings
//
// Reloads the information about the available recordings
//
// Arguments:
//
//	instance	- SQLite database instance
//	changed		- Flag indicating if the data has changed

void discover_recordings(sqlite3* instance, bool& changed)
{
	changed = false;							// Initialize [out] argument

	if(instance == nullptr) throw std::invalid_argument("instance");

	// Clone the recording table schema into a temporary table
	execute_non_query(instance, "drop table if exists discover_recording");
	execute_non_query(instance, "create temp table discover_recording as select * from recording limit 0");

	try {

		// Discover the recording information for all available storage devices
		execute_non_query(instance, "insert into discover_recording select deviceid, http_request(json_extract(device.data, '$.StorageURL')) "
			"from device where device.type = 'storage'");

		// This requires a multi-step operation against the recording table; start a transaction
		execute_non_query(instance, "begin immediate transaction");

		try {

			// Delete any entries in the main recording table that are no longer present in the data
			if(execute_non_query(instance, "delete from recording where deviceid not in (select deviceid from discover_recording)") > 0) changed = true;

			// Insert/replace entries in the main recording table that are new or different
			if(execute_non_query(instance, "replace into recording select discover_recording.* from discover_recording left outer join recording using(deviceid) "
				"where coalesce(recording.data, '') <> coalesce(discover_recording.data, '')") > 0) changed = true;

			// Commit the database transaction
			execute_non_query(instance, "commit transaction");
		}
		
		// Rollback the transaction on any exception
		catch(...) { try_execute_non_query(instance, "rollback transaction"); throw; }

		// Drop the temporary table
		execute_non_query(instance, "drop table discover_recording");
	}

	// Drop the temporary table on any exception
	catch(...) { execute_non_query(instance, "drop table discover_recording"); throw; }
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
// enumerate_channels
//
// Enumerates the available channels
//
// Arguments:
//
//	instance		- Database instance
//	prependnumbers	- Flag to append the channel numbers
//	showdrm			- Flag to show DRM channels
//	lineupnames		- Flag to use names from the lineup not the EPG
//	callback		- Callback function

void enumerate_channels(sqlite3* instance, bool prependnumbers, bool showdrm, bool lineupnames, enumerate_channels_callback callback)
{
	sqlite3_stmt*				statement;			// SQL statement to execute
	int							result;				// Result from SQLite function
	
	if((instance == nullptr) || (callback == nullptr)) return;

	// channelid | channelname | iconurl | drm
	auto sql = "select "
		"distinct(encode_channel_id(json_extract(entry.value, '$.GuideNumber'))) as channelid, "
		"case when ?1 then json_extract(entry.value, '$.GuideNumber') || ' ' else '' end || "
		"case when (?2 or guide.channelid is null) then json_extract(entry.value, '$.GuideName') else guide.channelname end as channelname, "
		"guide.iconurl as iconurl, "
		"coalesce(json_extract(entry.value, '$.DRM'), 0) as drm "
		"from lineup, json_each(lineup.data) as entry left outer join guide on encode_channel_id(json_extract(entry.value, '$.GuideNumber')) = guide.channelid "
		"where nullif(json_extract(entry.value, '$.DRM'), ?3) is null "
		"order by channelid";

	result = sqlite3_prepare_v2(instance, sql, -1, &statement, nullptr);
	if(result != SQLITE_OK) throw sqlite_exception(result, sqlite3_errmsg(instance));

	try {

		// Bind the query parameters
		result = sqlite3_bind_int(statement, 1, (prependnumbers) ? 1 : 0);
		if(result == SQLITE_OK) result = sqlite3_bind_int(statement, 2, (lineupnames) ? 1 : 0);
		if(result == SQLITE_OK) result = sqlite3_bind_int(statement, 3, (showdrm) ? 1 : 0);
		if(result != SQLITE_OK) throw sqlite_exception(result);

		// Execute the query and iterate over all returned rows
		while(sqlite3_step(statement) == SQLITE_ROW) {

			struct channel item;
			item.channelid.value = static_cast<unsigned int>(sqlite3_column_int(statement, 0));
			item.channelname = reinterpret_cast<char const*>(sqlite3_column_text(statement, 1));
			item.iconurl = reinterpret_cast<char const*>(sqlite3_column_text(statement, 2));
			item.drm = (sqlite3_column_int(statement, 3) != 0);

			callback(item);						// Invoke caller-supplied callback
		}
	
		sqlite3_finalize(statement);			// Finalize the SQLite statement
	}

	catch(...) { sqlite3_finalize(statement); throw; }
}

//---------------------------------------------------------------------------
// enumerate_channelids
//
// Enumerates all of the channelids in the database
//
// Arguments:
//
//	instance	- Database instance
//	showdrm		- Flag to show DRM channels
//	callback	- Callback function

void enumerate_channelids(sqlite3* instance, bool showdrm, enumerate_channelids_callback callback)
{
	sqlite3_stmt*				statement;			// SQL statement to execute
	int							result;				// Result from SQLite function
	
	if((instance == nullptr) || (callback == nullptr)) return;

	// channelid
	auto sql = "select distinct(encode_channel_id(json_extract(entry.value, '$.GuideNumber'))) as channelid "
		"from lineup, json_each(lineup.data) as entry where nullif(json_extract(entry.value, '$.DRM'), ?1) is null";

	result = sqlite3_prepare_v2(instance, sql, -1, &statement, nullptr);
	if(result != SQLITE_OK) throw sqlite_exception(result, sqlite3_errmsg(instance));

	try {

		// Bind the query parameters
		result = sqlite3_bind_int(statement, 1, (showdrm) ? 1 : 0);
		if(result != SQLITE_OK) throw sqlite_exception(result);

		// Execute the query and iterate over all returned rows
		while(sqlite3_step(statement) == SQLITE_ROW) {

			union channelid channelid;
			channelid.value = static_cast<unsigned int>(sqlite3_column_int(statement, 0));
			
			callback(channelid);
		}
	
		sqlite3_finalize(statement);			// Finalize the SQLite statement
	}

	catch(...) { sqlite3_finalize(statement); throw; }
}

//---------------------------------------------------------------------------
// enumerate_channeltuners
//
// Enumerates the tuners that can tune a specific channel
//
// Arguments:
//
//	instance		- Database instance
//	channelid		- channelid on which to find tuners
//	callback		- Callback function

void enumerate_channeltuners(sqlite3* instance, union channelid channelid, enumerate_channeltuners_callback callback)
{
	sqlite3_stmt*				statement;			// SQL statement to execute
	int							result;				// Result from SQLite function
	
	if((instance == nullptr) || (callback == nullptr)) return;
	
	// tunerid
	auto sql = "with recursive tuners(deviceid, tunerid) as "
		"(select deviceid, json_extract(device.data, '$.TunerCount') - 1 from device where type = 'tuner' "
		"union all select deviceid, tunerid - 1 from tuners where tunerid > 0) "
		"select tuners.deviceid || '-' || tuners.tunerid as tunerid "
		"from tuners inner join lineup using(deviceid), json_each(lineup.data) as lineupdata "
		"where json_extract(lineupdata.value, '$.GuideNumber') = decode_channel_id(?1) order by tunerid desc";

	result = sqlite3_prepare_v2(instance, sql, -1, &statement, nullptr);
	if(result != SQLITE_OK) throw sqlite_exception(result, sqlite3_errmsg(instance));

	try {

		// Bind the query parameters
		result = sqlite3_bind_int(statement, 1, channelid.value);
		if(result != SQLITE_OK) throw sqlite_exception(result);

		// Execute the query and iterate over all returned rows
		while(sqlite3_step(statement) == SQLITE_ROW) callback(reinterpret_cast<char const*>(sqlite3_column_text(statement, 0)));
	
		sqlite3_finalize(statement);			// Finalize the SQLite statement
	}

	catch(...) { sqlite3_finalize(statement); throw; }
}

//---------------------------------------------------------------------------
// enumerate_demo_channelids
//
// Enumerates the channels marked as 'Demo' in the lineups
//
// Arguments:
//
//	instance	- Database instance
//	showdrm		- Flag to show DRM channels
//	callback	- Callback function

void enumerate_demo_channelids(sqlite3* instance, bool showdrm, enumerate_channelids_callback callback)
{
	sqlite3_stmt*				statement;			// SQL statement to execute
	int							result;				// Result from SQLite function
	
	if((instance == nullptr) || (callback == nullptr)) return;

	// channelid
	auto sql = "select distinct(encode_channel_id(json_extract(entry.value, '$.GuideNumber'))) as channelid "
		"from lineup, json_each(lineup.data) as entry where json_extract(entry.value, '$.Demo') = 1 "
		"and nullif(json_extract(entry.value, '$.DRM'), ?1) is null";

	result = sqlite3_prepare_v2(instance, sql, -1, &statement, nullptr);
	if(result != SQLITE_OK) throw sqlite_exception(result, sqlite3_errmsg(instance));

	try {

		// Bind the query parameters
		result = sqlite3_bind_int(statement, 1, (showdrm) ? 1 : 0);
		if(result != SQLITE_OK) throw sqlite_exception(result);

		// Execute the query and iterate over all returned rows
		while(sqlite3_step(statement) == SQLITE_ROW) {

			union channelid channelid;
			channelid.value = static_cast<unsigned int>(sqlite3_column_int(statement, 0));
			
			callback(channelid);
		}
	
		sqlite3_finalize(statement);			// Finalize the SQLite statement
	}

	catch(...) { sqlite3_finalize(statement); throw; }
}

//---------------------------------------------------------------------------
// enumerate_device_names
//
// Enumerates the available device names
//
// Arguments:
//
//	instance	- Database instance
//	callback	- Callback function

void enumerate_device_names(sqlite3* instance, enumerate_device_names_callback callback)
{
	sqlite3_stmt*				statement;			// SQL statement to execute
	int							result;				// Result from SQLite function
	
	if((instance == nullptr) || (callback == nullptr)) return;

	// name
	auto sql = "select coalesce(json_extract(data, '$.FriendlyName'), 'unknown') || ' ' || deviceid as name from device";

	result = sqlite3_prepare_v2(instance, sql, -1, &statement, nullptr);
	if(result != SQLITE_OK) throw sqlite_exception(result, sqlite3_errmsg(instance));

	try {

		// Execute the query and iterate over all returned rows
		while(sqlite3_step(statement) == SQLITE_ROW) {

			struct device_name device_name;
			device_name.name = reinterpret_cast<char const*>(sqlite3_column_text(statement, 0));
			
			callback(device_name);
		}
	
		sqlite3_finalize(statement);			// Finalize the SQLite statement
	}

	catch(...) { sqlite3_finalize(statement); throw; }
}

//---------------------------------------------------------------------------
// enumerate_episode_channelids
//
// Enumerates all of the channel identifiers associated with any episodes
//
// Arguments:
//
//	instance	- Database instance
//	callback	- Callback function

void enumerate_episode_channelids(sqlite3* instance, enumerate_channelids_callback callback)
{
	sqlite3_stmt*				statement;			// SQL statement to execute
	int							result;				// Result from SQLite function
	
	if((instance == nullptr) || (callback == nullptr)) return;

	// channelid
	auto sql = "select distinct(encode_channel_id(json_extract(value, '$.ChannelNumber'))) as channelid "
		"from episode, json_each(episode.data) where channelid <> 0";

	result = sqlite3_prepare_v2(instance, sql, -1, &statement, nullptr);
	if(result != SQLITE_OK) throw sqlite_exception(result, sqlite3_errmsg(instance));

	try {

		// Execute the query and iterate over all returned rows
		while(sqlite3_step(statement) == SQLITE_ROW) {

			union channelid channelid;
			channelid.value = static_cast<unsigned int>(sqlite3_column_int(statement, 0));
			
			callback(channelid);
		}
	
		sqlite3_finalize(statement);			// Finalize the SQLite statement
	}

	catch(...) { sqlite3_finalize(statement); throw; }
}

//---------------------------------------------------------------------------
// enumerate_favorite_channelids
//
// Enumerates the channels marked as 'Favorite' in the lineups
//
// Arguments:
//
//	instance	- Database instance
//	showdrm		- Flag to show DRM channels
//	callback	- Callback function

void enumerate_favorite_channelids(sqlite3* instance, bool showdrm, enumerate_channelids_callback callback)
{
	sqlite3_stmt*				statement;			// SQL statement to execute
	int							result;				// Result from SQLite function
	
	if((instance == nullptr) || (callback == nullptr)) return;

	// channelid
	auto sql = "select distinct(encode_channel_id(json_extract(entry.value, '$.GuideNumber'))) as channelid "
		"from lineup, json_each(lineup.data) as entry where json_extract(entry.value, '$.Favorite') = 1 "
		"and nullif(json_extract(entry.value, '$.DRM'), ?1) is null";

	result = sqlite3_prepare_v2(instance, sql, -1, &statement, nullptr);
	if(result != SQLITE_OK) throw sqlite_exception(result, sqlite3_errmsg(instance));

	try {

		// Bind the query parameters
		result = sqlite3_bind_int(statement, 1, (showdrm) ? 1 : 0);
		if(result != SQLITE_OK) throw sqlite_exception(result);

		// Execute the query and iterate over all returned rows
		while(sqlite3_step(statement) == SQLITE_ROW) {

			union channelid channelid;
			channelid.value = static_cast<unsigned int>(sqlite3_column_int(statement, 0));
			
			callback(channelid);
		}
	
		sqlite3_finalize(statement);			// Finalize the SQLite statement
	}

	catch(...) { sqlite3_finalize(statement); throw; }
}

//---------------------------------------------------------------------------
// enumerate_expired_recordingruleids
//
// Enumerates all recordingruleids that have expired
//
// Arguments:
//
//	instance	- Database instance
//	expiry		- Expiration time, in seconds
//	callback	- Callback function

void enumerate_expired_recordingruleids(sqlite3* instance, int expiry, enumerate_recordingruleids_callback callback)
{
	sqlite3_stmt*				statement;			// SQL statement to execute
	int							result;				// Result from SQLite function
	
	if((instance == nullptr) || (expiry <= 0) || (callback == nullptr)) return;

	// recordingruleid
	auto sql = "select distinct(recordingruleid) as recordingruleid from recordingrule "
		"where json_extract(data, '$.DateTimeOnly') < (cast(strftime('%s', 'now') as int) - ?1)";

	result = sqlite3_prepare_v2(instance, sql, -1, &statement, nullptr);
	if(result != SQLITE_OK) throw sqlite_exception(result, sqlite3_errmsg(instance));

	try {

		// Bind the query parameters
		result = sqlite3_bind_int(statement, 1, expiry);
		if(result != SQLITE_OK) throw sqlite_exception(result);

		// Execute the query and iterate over all returned rows
		while(sqlite3_step(statement) == SQLITE_ROW) callback(static_cast<unsigned int>(sqlite3_column_int(statement, 0)));
	
		sqlite3_finalize(statement);			// Finalize the SQLite statement
	}

	catch(...) { sqlite3_finalize(statement); throw; }
}

//---------------------------------------------------------------------------
// enumerate_guideentries
//
// Enumerates the available guide entries for a channel and time period
//
// Arguments:
//
//	instance	- Database instance
//	channelid	- Channel to be enumerated
//	starttime	- Starting time to be queried
//	endtime		- Ending time to be queried
//	callback	- Callback function

void enumerate_guideentries(sqlite3* instance, union channelid channelid, time_t starttime, time_t endtime, enumerate_guideentries_callback callback)
{
	sqlite3_stmt*				statement;			// SQL statement to execute
	int							result;				// Result from SQLite function

	if((instance == nullptr) || (callback == nullptr)) return;

	// Prevent asking for anything older than 4 hours in the past (14400 = (60 * 60 * 4) = 4 hours)
	time_t now = time(nullptr);
	starttime = std::max(starttime, now - 14400);

	// seriesid | title | starttime | endtime | synopsis | year | iconurl | genretype | genres | originalairdate | seriesnumber | episodenumber | episodename
	auto sql = "with deviceauth(code) as (select url_encode(group_concat(json_extract(data, '$.DeviceAuth'), '')) from device) "
		"select json_extract(entry.value, '$.SeriesID') as seriesid, "
		"json_extract(entry.value, '$.Title') as title, "
		"fnv_hash(?2, json_extract(value, '$.StartTime'), json_extract(value, '$.EndTime')) as broadcastid, "
		"json_extract(entry.value, '$.StartTime') as starttime, "
		"json_extract(entry.value, '$.EndTime') as endtime, "
		"json_extract(entry.value, '$.Synopsis') as synopsis, "
		"cast(strftime('%Y', coalesce(json_extract(entry.value, '$.OriginalAirdate'), 0), 'unixepoch') as int) as year, "
		"json_extract(entry.value, '$.ImageURL') as iconurl, "
		"coalesce((select genretype from genremap where filter like json_extract(entry.value, '$.Filter[0]')), 0) as genretype, "
		"(select group_concat(value) from json_each(json_extract(entry.value, '$.Filter'))) as genres, "
		"json_extract(entry.value, '$.OriginalAirdate') as originalairdate, "
		"get_season_number(json_extract(entry.value, '$.EpisodeNumber')) as seriesnumber, "
		"get_episode_number(json_extract(entry.value, '$.EpisodeNumber')) as episodenumber, "
		"json_extract(entry.value, '$.EpisodeTitle') as episodename "
		"from deviceauth, "
		"json_each(json_extract(nullif(http_request('http://api.hdhomerun.com/api/guide?DeviceAuth=' || coalesce(deviceauth.code, '') || '&Channel=' || decode_channel_id(?1) || '&Start=' || ?2), 'null'), '$[0].Guide')) as entry";

	result = sqlite3_prepare_v2(instance, sql, -1, &statement, nullptr);
	if(result != SQLITE_OK) throw sqlite_exception(result, sqlite3_errmsg(instance));

	try {

		while(starttime < endtime) {

			// Bind the query parameters
			result = sqlite3_bind_int(statement, 1, channelid.value);
			if(result == SQLITE_OK) result = sqlite3_bind_int(statement, 2, static_cast<int>(starttime));
			if(result != SQLITE_OK) throw sqlite_exception(result);

			// Execute the SQL statement
			result = sqlite3_step(statement);
			if((result != SQLITE_DONE) && (result != SQLITE_ROW)) throw sqlite_exception(result, sqlite3_errmsg(instance));

			// If no rows were returned from the query and the start time is still in the past,
			// fast-forward it to the current time and try again.  Otherwise stop - no more data
			if(result == SQLITE_DONE) {
				
				if(starttime < now) starttime = now;
				else break;
			}

			// Process each row returned from the query (if any)
			while(result == SQLITE_ROW) {

				struct guideentry item;
				item.seriesid = reinterpret_cast<char const*>(sqlite3_column_text(statement, 0));
				item.title = reinterpret_cast<char const*>(sqlite3_column_text(statement, 1));
				item.broadcastid = static_cast<unsigned int>(sqlite3_column_int(statement, 2));
				item.channelid = channelid.value;
				item.starttime = static_cast<unsigned int>(sqlite3_column_int(statement, 3));
				item.endtime = static_cast<unsigned int>(sqlite3_column_int(statement, 4));
				item.synopsis = reinterpret_cast<char const*>(sqlite3_column_text(statement, 5));
				item.year = sqlite3_column_int(statement, 6);
				item.iconurl = reinterpret_cast<char const*>(sqlite3_column_text(statement, 7));
				item.genretype = sqlite3_column_int(statement, 8);
				item.genres = reinterpret_cast<char const*>(sqlite3_column_text(statement, 9));
				item.originalairdate = sqlite3_column_int(statement, 10);
				item.seriesnumber = sqlite3_column_int(statement, 11);
				item.episodenumber = sqlite3_column_int(statement, 12);
				item.episodename = reinterpret_cast<char const*>(sqlite3_column_text(statement, 13));

				// Move the starttime to the last seen endtime to continue the backend queries
				if(item.endtime > starttime) starttime = item.endtime;

				callback(item);							// Invoke caller-supplied callback
				result = sqlite3_step(statement);		// Move to the next row of data
			}

			// Reset the prepared statement so that it can be executed again
			result = sqlite3_reset(statement);
			if(result != SQLITE_OK) throw sqlite_exception(result);
		};
	
		sqlite3_finalize(statement);			// Finalize the SQLite statement
	}

	catch(...) { sqlite3_finalize(statement); throw; }
}

//---------------------------------------------------------------------------
// enumerate_hd_channelids
//
// Enumerates the channels marked as 'HD' in the lineups
//
// Arguments:
//
//	instance	- Database instance
//	showdrm		- Flag to show DRM channels
//	callback	- Callback function

void enumerate_hd_channelids(sqlite3* instance, bool showdrm, enumerate_channelids_callback callback)
{
	sqlite3_stmt*				statement;			// SQL statement to execute
	int							result;				// Result from SQLite function
	
	if((instance == nullptr) || (callback == nullptr)) return;

	// channelid
	auto sql = "select distinct(encode_channel_id(json_extract(entry.value, '$.GuideNumber'))) as channelid "
		"from lineup, json_each(lineup.data) as entry where json_extract(entry.value, '$.HD') = 1 "
		"and nullif(json_extract(entry.value, '$.DRM'), ?1) is null";

	result = sqlite3_prepare_v2(instance, sql, -1, &statement, nullptr);
	if(result != SQLITE_OK) throw sqlite_exception(result, sqlite3_errmsg(instance));

	try {

		// Bind the query parameters
		result = sqlite3_bind_int(statement, 1, (showdrm) ? 1 : 0);
		if(result != SQLITE_OK) throw sqlite_exception(result);

		// Execute the query and iterate over all returned rows
		while(sqlite3_step(statement) == SQLITE_ROW) {

			union channelid channelid;
			channelid.value = static_cast<unsigned int>(sqlite3_column_int(statement, 0));
			
			callback(channelid);
		}
	
		sqlite3_finalize(statement);			// Finalize the SQLite statement
	}

	catch(...) { sqlite3_finalize(statement); throw; }
}

//---------------------------------------------------------------------------
// enumerate_recordings
//
// Enumerates the available recordings
//
// Arguments:
//
//	instance	- Database instance
//	callback	- Callback function

void enumerate_recordings(sqlite3* instance, enumerate_recordings_callback callback)
{
	return enumerate_recordings(instance, false, callback);
}

//---------------------------------------------------------------------------
// enumerate_recordings
//
// Enumerates the available recordings
//
// Arguments:
//
//	instance		- Database instance
//	episodeastitle	- Flag to use the episode number in place of the recording title
//	callback		- Callback function

void enumerate_recordings(sqlite3* instance, bool episodeastitle, enumerate_recordings_callback callback)
{
	sqlite3_stmt*				statement;			// SQL statement to execute
	int							result;				// Result from SQLite function
	
	if((instance == nullptr) || (callback == nullptr)) return;

	// recordingid | title | episodename | seriesnumber | episodenumber | year | streamurl | directory | plot | channelname | thumbnailpath | recordingtime | duration | lastposition | channelid
	auto sql = "select "
		"json_extract(value, '$.CmdURL') as recordingid, "
		"case when ?1 then coalesce(json_extract(value, '$.EpisodeNumber'), json_extract(value, '$.Title')) else json_extract(value, '$.Title') end as title, "
		"json_extract(value, '$.EpisodeTitle') as episodename, "
		"get_season_number(json_extract(value, '$.EpisodeNumber')) as seriesnumber, "
		"get_episode_number(json_extract(value, '$.EpisodeNumber')) as episodenumber, "
		"cast(strftime('%Y', coalesce(json_extract(value, '$.OriginalAirdate'), 0), 'unixepoch') as int) as year, "
		"json_extract(value, '$.PlayURL') as streamurl, "
		"case when json_extract(value, '$.DisplayGroupTitle') is null then json_extract(value, '$.Title') else json_extract(value, '$.DisplayGroupTitle') end as directory, "
		"json_extract(value, '$.Synopsis') as plot, "
		"json_extract(value, '$.ChannelName') as channelname, "
		"json_extract(value, '$.ImageURL') as thumbnailpath, "
		"coalesce(json_extract(value, '$.RecordStartTime'), 0) as recordingtime, "
		"coalesce(json_extract(value, '$.RecordEndTime'), 0) - coalesce(json_extract(value, '$.RecordStartTime'), 0) as duration, "
		"coalesce(json_extract(value, '$.Resume'), 0) as lastposition, "
		"encode_channel_id(json_extract(value, '$.ChannelNumber')) as channelid "
		"from recording, json_each(recording.data)";

	result = sqlite3_prepare_v2(instance, sql, -1, &statement, nullptr);
	if(result != SQLITE_OK) throw sqlite_exception(result, sqlite3_errmsg(instance));

	try {

		// Bind the query parameters
		result = sqlite3_bind_int(statement, 1, (episodeastitle) ? 1 : 0);
		if(result != SQLITE_OK) throw sqlite_exception(result);

		// Execute the query and iterate over all returned rows
		while(sqlite3_step(statement) == SQLITE_ROW) {

			struct recording item;
			item.recordingid = reinterpret_cast<char const*>(sqlite3_column_text(statement, 0));
			item.title = reinterpret_cast<char const*>(sqlite3_column_text(statement, 1));
			item.episodename = reinterpret_cast<char const*>(sqlite3_column_text(statement, 2));
			item.seriesnumber = sqlite3_column_int(statement, 3);
			item.episodenumber = sqlite3_column_int(statement, 4);
			item.year = sqlite3_column_int(statement, 5);
			item.streamurl = reinterpret_cast<char const*>(sqlite3_column_text(statement, 6));
			item.directory = reinterpret_cast<char const*>(sqlite3_column_text(statement, 7));
			item.plot = reinterpret_cast<char const*>(sqlite3_column_text(statement, 8));
			item.channelname = reinterpret_cast<char const*>(sqlite3_column_text(statement, 9));
			item.thumbnailpath = reinterpret_cast<char const*>(sqlite3_column_text(statement, 10));
			item.recordingtime = sqlite3_column_int(statement, 11);
			item.duration = sqlite3_column_int(statement, 12);
			item.lastposition = sqlite3_column_int(statement, 13);
			item.channelid.value = static_cast<unsigned int>(sqlite3_column_int(statement, 14));

			callback(item);						// Invoke caller-supplied callback
		}
	
		sqlite3_finalize(statement);			// Finalize the SQLite statement
	}

	catch(...) { sqlite3_finalize(statement); throw; }
}

//---------------------------------------------------------------------------
// enumerate_recordingrules
//
// Enumerates the available recording rules
//
// Arguments:
//
//	instance	- Database instance
//	callback	- Callback function

void enumerate_recordingrules(sqlite3* instance, enumerate_recordingrules_callback callback)
{
	sqlite3_stmt*				statement;			// SQL statement to execute
	int							result;				// Result from SQLite function
	
	if((instance == nullptr) || (callback == nullptr)) return;

	// recordingruleid | type | seriesid | channelid | recentonly | afteroriginalairdateonly | datetimeonly | title | synopsis | startpadding | endpadding
	auto sql = "with guidenumbers(guidenumber) as (select distinct(json_extract(value, '$.GuideNumber')) as guidenumber from lineup, json_each(lineup.data)) "
		"select recordingruleid, "
		"case when json_extract(data, '$.DateTimeOnly') is null then 0 else 1 end as type, "
		"json_extract(data, '$.SeriesID') as seriesid, "
		"case when guidenumbers.guidenumber is null then -1 else encode_channel_id(json_extract(data, '$.ChannelOnly')) end as channelid, "
		"coalesce(json_extract(data, '$.RecentOnly'), 0) as recentonly, "
		"coalesce(json_extract(data, '$.AfterOriginalAirdateOnly'), 0) as afteroriginalairdateonly, "
		"coalesce(json_extract(data, '$.DateTimeOnly'), 0) as datetimeonly, "
		"json_extract(data, '$.Title') as title, "
		"json_extract(data, '$.Synopsis') as synopsis, "
		"coalesce(json_extract(data, '$.StartPadding'), 30) as startpadding, "
		"coalesce(json_extract(data, '$.EndPadding'), 30) as endpadding "
		"from recordingrule left outer join guidenumbers on json_extract(data, '$.ChannelOnly') = guidenumbers.guidenumber";

	result = sqlite3_prepare_v2(instance, sql, -1, &statement, nullptr);
	if(result != SQLITE_OK) throw sqlite_exception(result, sqlite3_errmsg(instance));

	try {

		// Execute the query and iterate over all returned rows
		while(sqlite3_step(statement) == SQLITE_ROW) {

			struct recordingrule item;
			item.recordingruleid = static_cast<unsigned int>(sqlite3_column_int(statement, 0));
			item.type = static_cast<recordingrule_type>(sqlite3_column_int(statement, 1));
			item.seriesid = reinterpret_cast<char const*>(sqlite3_column_text(statement, 2));
			item.channelid.value = static_cast<unsigned int>(sqlite3_column_int(statement, 3));
			item.recentonly = (sqlite3_column_int(statement, 4) != 0);
			item.afteroriginalairdateonly = static_cast<unsigned int>(sqlite3_column_int(statement, 5));
			item.datetimeonly = static_cast<unsigned int>(sqlite3_column_int(statement, 6));
			item.title = reinterpret_cast<char const*>(sqlite3_column_text(statement, 7));
			item.synopsis = reinterpret_cast<char const*>(sqlite3_column_text(statement, 8));
			item.startpadding = static_cast<unsigned int>(sqlite3_column_int(statement, 9));
			item.endpadding = static_cast<unsigned int>(sqlite3_column_int(statement, 10));

			callback(item);						// Invoke caller-supplied callback
		}
	
		sqlite3_finalize(statement);			// Finalize the SQLite statement
	}

	catch(...) { sqlite3_finalize(statement); throw; }
}

//---------------------------------------------------------------------------
// enumerate_sd_channelids
//
// Enumerates the channels not marked as 'HD' in the lineups
//
// Arguments:
//
//	instance	- Database instance
//	showdrm		- Flag to show DRM channels
//	callback	- Callback function

void enumerate_sd_channelids(sqlite3* instance, bool showdrm, enumerate_channelids_callback callback)
{
	sqlite3_stmt*				statement;			// SQL statement to execute
	int							result;				// Result from SQLite function
	
	if((instance == nullptr) || (callback == nullptr)) return;

	// channelid
	auto sql = "select distinct(encode_channel_id(json_extract(entry.value, '$.GuideNumber'))) as channelid "
		"from lineup, json_each(lineup.data) as entry where json_extract(entry.value, '$.HD') is null "
		"and nullif(json_extract(entry.value, '$.DRM'), ?1) is null";

	result = sqlite3_prepare_v2(instance, sql, -1, &statement, nullptr);
	if(result != SQLITE_OK) throw sqlite_exception(result, sqlite3_errmsg(instance));

	try {

		// Bind the query parameters
		result = sqlite3_bind_int(statement, 1, (showdrm) ? 1 : 0);
		if(result != SQLITE_OK) throw sqlite_exception(result);

		// Execute the query and iterate over all returned rows
		while(sqlite3_step(statement) == SQLITE_ROW) {

			union channelid channelid;
			channelid.value = static_cast<unsigned int>(sqlite3_column_int(statement, 0));
			
			callback(channelid);
		}
	
		sqlite3_finalize(statement);			// Finalize the SQLite statement
	}

	catch(...) { sqlite3_finalize(statement); throw; }
}

//---------------------------------------------------------------------------
// enumerate_series
//
// Enumerates series based on a title matching search
//
// Arguments:
//
//	instance	- Database instance
//	title		- Title on which to search
//	callback	- Callback function

void enumerate_series(sqlite3* instance, char const* title, enumerate_series_callback callback)
{
	sqlite3_stmt*				statement;			// SQL statement to execute
	int							result;				// Result from SQLite function
	
	if((instance == nullptr) || (callback == nullptr)) return;

	// title | seriesid
	auto sql = "with deviceauth(code) as (select url_encode(group_concat(json_extract(data, '$.DeviceAuth'), '')) from device) "
		"select "
		"json_extract(value, '$.Title') as title, "
		"json_extract(value, '$.SeriesID') as seriesid "
		"from deviceauth, json_each(http_request('http://api.hdhomerun.com/api/search?DeviceAuth=' || coalesce(deviceauth.code, '') || '&Search=' || url_encode(?1))) "
		"where title like '%' || ?1 || '%'";

	result = sqlite3_prepare_v2(instance, sql, -1, &statement, nullptr);
	if(result != SQLITE_OK) throw sqlite_exception(result, sqlite3_errmsg(instance));

	try {

		// Bind the query parameter(s)
		result = sqlite3_bind_text(statement, 1, title, -1, SQLITE_STATIC);
		if(result != SQLITE_OK) throw sqlite_exception(result);

		// Execute the query and iterate over all returned rows
		while(sqlite3_step(statement) == SQLITE_ROW) {

			struct series item;
			item.title = reinterpret_cast<char const*>(sqlite3_column_text(statement, 0));
			item.seriesid = reinterpret_cast<char const*>(sqlite3_column_text(statement, 1));

			callback(item);						// Invoke caller-supplied callback
		}
	
		sqlite3_finalize(statement);			// Finalize the SQLite statement
	}

	catch(...) { sqlite3_finalize(statement); throw; }
}

//---------------------------------------------------------------------------
// enumerate_timers
//
// Enumerates all episodes that are scheduled to be recorded
//
// Arguments:
//
//	instance	- Database instance
//	maxdays		- Maximum number of days to report
//	callback	- Callback function

void enumerate_timers(sqlite3* instance, int maxdays, enumerate_timers_callback callback)
{
	sqlite3_stmt*				statement;			// SQL statement to execute
	int							result;				// Result from SQLite function
	
	if((instance == nullptr) || (callback == nullptr)) return;

	// If the maximum number of days wasn't provided, use a month as the boundary
	if(maxdays < 0) maxdays = 31;

	// recordingruleid | parenttype | timerid | channelid | starttime | endtime | title | synopsis
	auto sql = "with guidenumbers(guidenumber) as (select distinct(json_extract(value, '$.GuideNumber')) as guidenumber from lineup, json_each(lineup.data)) "
		"select case when json_extract(recordingrule.data, '$.DateTimeOnly') is not null then recordingrule.recordingruleid else "
		"(select recordingruleid from recordingrule where json_extract(recordingrule.data, '$.DateTimeOnly') is null and seriesid = episode.seriesid limit 1) end as recordingruleid, "
		"case when json_extract(recordingrule.data, '$.DateTimeOnly') is not null then 1 else 0 end as parenttype, "
		"fnv_hash(json_extract(value, '$.ProgramID'), json_extract(value, '$.StartTime'), json_extract(value, '$.ChannelNumber')) as timerid, "
		"case when guidenumbers.guidenumber is null then -1 else encode_channel_id(json_extract(value, '$.ChannelNumber')) end as channelid, "
		"json_extract(value, '$.StartTime') as starttime, "
		"json_extract(value, '$.EndTime') as endtime, "
		"json_extract(value, '$.Title') as title, "
		"json_extract(value, '$.Synopsis') as synopsis "
		"from episode, json_each(episode.data) "
		"left outer join recordingrule on episode.seriesid = recordingrule.seriesid and json_extract(value, '$.StartTime') = json_extract(recordingrule.data, '$.DateTimeOnly') "
		"left outer join guidenumbers on json_extract(value, '$.ChannelNumber') = guidenumbers.guidenumber "
		"where json_extract(value, '$.RecordingRule') = 1 and "
		"(starttime < (cast(strftime('%s', 'now') as integer) + (?1 * 86400)))";

	result = sqlite3_prepare_v2(instance, sql, -1, &statement, nullptr);
	if(result != SQLITE_OK) throw sqlite_exception(result, sqlite3_errmsg(instance));

	try {

		// Bind the query parameter(s)
		result = sqlite3_bind_int(statement, 1, maxdays);
		if(result != SQLITE_OK) throw sqlite_exception(result);

		// Execute the query and iterate over all returned rows
		while(sqlite3_step(statement) == SQLITE_ROW) {

			struct timer item;
			item.recordingruleid = static_cast<unsigned int>(sqlite3_column_int(statement, 0));
			item.parenttype = static_cast<enum recordingrule_type>(sqlite3_column_int(statement, 1));
			item.timerid = static_cast<unsigned int>(sqlite3_column_int(statement, 2));
			item.channelid.value = static_cast<unsigned int>(sqlite3_column_int(statement, 3));
			item.starttime = static_cast<unsigned int>(sqlite3_column_int(statement, 4));
			item.endtime = static_cast<unsigned int>(sqlite3_column_int(statement, 5));
			item.title = reinterpret_cast<char const*>(sqlite3_column_text(statement, 6));
			item.synopsis = reinterpret_cast<char const*>(sqlite3_column_text(statement, 7));

			callback(item);						// Invoke caller-supplied callback
		}
	
		sqlite3_finalize(statement);			// Finalize the SQLite statement
	}

	catch(...) { sqlite3_finalize(statement); throw; }
}

//---------------------------------------------------------------------------
// execute_non_query
//
// Executes a non-query against the database
//
// Arguments:
//
//	instance	- Database instance
//	sql			- SQL non-query to execute

int execute_non_query(sqlite3* instance, char const* sql)
{
	char*		errmsg = nullptr;		// Error message from SQLite
	int			result;					// Result from SQLite function call
	
	try {
	
		// Attempt to execute the statement and throw the error on failure
		result = sqlite3_exec(instance, sql, nullptr, nullptr, &errmsg);
		if(result != SQLITE_OK) throw sqlite_exception(result, errmsg);

		// Return the number of changes made by the preceeding query
		return sqlite3_changes(instance);
	}

	catch(...) { if(errmsg) sqlite3_free(errmsg); throw; }
}

//---------------------------------------------------------------------------
// find_seriesid
//
// Retrieves the series id associated with a specific channel/time combination
//
// Arguments:
//
//	instance		- Database instance handle
//	channelid		- Channel on which to find the series
//	timestamp		- Time stamp on which to find the series

std::string find_seriesid(sqlite3* instance, union channelid channelid, time_t timestamp)
{
	sqlite3_stmt*				statement;				// Database query statement
	std::string					seriesid;				// Discovered series identifier
	int							result;					// Result from SQLite function call

	if(instance == nullptr) return 0;

	// No guide data is stored locally anymore; always use the backend service to search for the seriesid.
	// Use the electronic program guide API to locate a seriesid based on a channel and timestamp
	auto sql = "with deviceauth(code) as (select url_encode(group_concat(json_extract(data, '$.DeviceAuth'), '')) from device) "
		"select json_extract(json_extract(nullif(http_request('http://api.hdhomerun.com/api/guide?DeviceAuth=' || coalesce(deviceauth.code, '') || '&Channel=' || decode_channel_id(?1) || '&Start=' || ?2), 'null'), '$[0].Guide[0]'), '$.SeriesID') "
		"from deviceauth";

	result = sqlite3_prepare_v2(instance, sql, -1, &statement, nullptr);
	if(result != SQLITE_OK) throw sqlite_exception(result, sqlite3_errmsg(instance));

	try {

		// Bind the query parameters(s)
		result = sqlite3_bind_int(statement, 1, channelid.value);
		if(result == SQLITE_OK) result = sqlite3_bind_int(statement, 2, static_cast<int>(timestamp));
		if(result != SQLITE_OK) throw sqlite_exception(result);
		
		// Execute the scalar query
		result = sqlite3_step(statement);

		// There should be a single SQLITE_ROW returned from the initial step
		if(result == SQLITE_ROW) seriesid.assign(reinterpret_cast<char const*>(sqlite3_column_text(statement, 0)));
		else if(result != SQLITE_DONE) throw sqlite_exception(result, sqlite3_errmsg(instance));

		sqlite3_finalize(statement);
		return seriesid;
	}

	catch(...) { sqlite3_finalize(statement); throw; }
}

//---------------------------------------------------------------------------
// find_seriesid
//
// Retrieves the series id associated with a title
//
// Arguments:
//
//	instance		- Database instance handle
//	title			- Title of the series to locate

std::string find_seriesid(sqlite3* instance, char const* title)
{
	sqlite3_stmt*				statement;				// Database query statement
	std::string					seriesid;				// Discovered series identifier
	int							result;					// Result from SQLite function call

	if(instance == nullptr) return 0;

	// No guide data is stored locally anymore; always use the backend service to search for the seriesid.
	// Use the search API to locate a series id based on the series title
	auto sql = "with deviceauth(code) as (select url_encode(group_concat(json_extract(data, '$.DeviceAuth'), '')) from device) "
		"select "
		"json_extract(value, '$.SeriesID') as seriesid "
		"from deviceauth, json_each(http_request('http://api.hdhomerun.com/api/search?DeviceAuth=' || coalesce(deviceauth.code, '') || '&Search=' || url_encode(?1))) "
		"where json_extract(value, '$.Title') like ?1"
		"limit 1";

	result = sqlite3_prepare_v2(instance, sql, -1, &statement, nullptr);
	if(result != SQLITE_OK) throw sqlite_exception(result, sqlite3_errmsg(instance));

	try {

		// Bind the query parameters(s)
		result = sqlite3_bind_text(statement, 1, title, -1, SQLITE_STATIC);
		if(result != SQLITE_OK) throw sqlite_exception(result);
		
		// Execute the scalar query
		result = sqlite3_step(statement);

		// There should be a single SQLITE_ROW returned from the initial step
		if(result == SQLITE_ROW) seriesid.assign(reinterpret_cast<char const*>(sqlite3_column_text(statement, 0)));
		else if(result != SQLITE_DONE) throw sqlite_exception(result, sqlite3_errmsg(instance));

		sqlite3_finalize(statement);
		return seriesid;
	}

	catch(...) { sqlite3_finalize(statement); throw; }
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
// get_available_storage_space
//
// Gets the total amount of free space on the backend
//
// Arguments:
//
//	instance	- SQLite database instance

long long get_available_storage_space(sqlite3* instance)
{
	sqlite3_stmt*				statement;				// Database query statement
	long long					space = 0;				// Returned amount of free space
	int							result;					// Result from SQLite function call

	if(instance == nullptr) return 0;

	// Prepare a query to get the sum of all available storage space
	auto sql = "select sum(coalesce(json_extract(device.data, '$.FreeSpace'), 0)) from device where device.type = 'storage'";

	result = sqlite3_prepare_v2(instance, sql, -1, &statement, nullptr);
	if(result != SQLITE_OK) throw sqlite_exception(result, sqlite3_errmsg(instance));

	try { 
		
		// Execute the scalar query
		result = sqlite3_step(statement);

		// There should be a single SQLITE_ROW returned from the initial step
		if(result == SQLITE_ROW) space = sqlite3_column_int64(statement, 0);
		else if(result != SQLITE_DONE) throw sqlite_exception(result, sqlite3_errmsg(instance));

		sqlite3_finalize(statement);
		return space;
	}

	catch(...) { sqlite3_finalize(statement); throw; }
}

//---------------------------------------------------------------------------
// get_channel_count
//
// Gets the number of available channels in the database
//
// Arguments:
//
//	instance	- SQLite database instance
//	showdrm		- Flag to show DRM channels

int get_channel_count(sqlite3* instance, bool showdrm)
{
	sqlite3_stmt*				statement;				// Database query statement
	int							channels = 0;			// Number of channels found
	int							result;					// Result from SQLite function call

	if(instance == nullptr) return 0;

	// Prepare a query to get the number of distinct channels in the lineup
	auto sql = "select count(distinct(json_extract(value, '$.GuideNumber'))) "
		"from lineup, json_each(lineup.data) where nullif(json_extract(value, '$.DRM'), ?1) is null";

	result = sqlite3_prepare_v2(instance, sql, -1, &statement, nullptr);
	if(result != SQLITE_OK) throw sqlite_exception(result, sqlite3_errmsg(instance));

	try { 

		// Bind the query parameters
		result = sqlite3_bind_int(statement, 1, (showdrm) ? 1 : 0);
		if(result != SQLITE_OK) throw sqlite_exception(result);

		// Execute the scalar query
		result = sqlite3_step(statement);

		// There should be a single SQLITE_ROW returned from the initial step
		if(result == SQLITE_ROW) channels = sqlite3_column_int(statement, 0);
		else if(result != SQLITE_DONE) throw sqlite_exception(result, sqlite3_errmsg(instance));

		sqlite3_finalize(statement);
		return channels;
	}

	catch(...) { sqlite3_finalize(statement); throw; }
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
// get_recording_count
//
// Gets the number of available recordings in the database
//
// Arguments:
//
//	instance	- SQLite database instance

int get_recording_count(sqlite3* instance)
{
	sqlite3_stmt*				statement;				// Database query statement
	int							recordings = 0;			// Number of recordings found
	int							result;					// Result from SQLite function call

	if(instance == nullptr) return 0;

	// Prepare a scalar result query to get the number of recordings
	auto sql = "select count(json_type(value, '$.ProgramID')) from recording, json_each(recording.data)";
	result = sqlite3_prepare_v2(instance, sql, -1, &statement, nullptr);
	if(result != SQLITE_OK) throw sqlite_exception(result, sqlite3_errmsg(instance));

	try { 
		
		// Execute the scalar query
		result = sqlite3_step(statement);

		// There should be a single SQLITE_ROW returned from the initial step
		if(result == SQLITE_ROW) recordings = sqlite3_column_int(statement, 0);
		else if(result != SQLITE_DONE) throw sqlite_exception(result, sqlite3_errmsg(instance));

		sqlite3_finalize(statement);
		return recordings;
	}

	catch(...) { sqlite3_finalize(statement); throw; }
}

//---------------------------------------------------------------------------
// get_recording_filename
//
// Generates the filename for a recording
//
// Arguments:
//
//	instance		- Database instance
//	recordingid		- Recording identifier (command url)

std::string get_recording_filename(sqlite3* instance, char const* recordingid)
{
	sqlite3_stmt*				statement;				// Database query statement
	std::string					filename;				// Generated file name
	int							result;					// Result from SQLite function call

	if((instance == nullptr) || (recordingid == nullptr)) return filename;

	// Prepare a scalar result query to generate the file name of the recording MPG file
	//
	// FORMAT: {DisplayGroupTitle}/{Title} {EpisodeNumber} {OriginalAirDate} [{StartTime}]
	auto sql = "select rtrim(clean_filename(json_extract(value, '$.DisplayGroupTitle')), ' .') || '/' || "
		"clean_filename(json_extract(value, '$.Title')) || ' ' || "
		"coalesce(json_extract(value, '$.EpisodeNumber') || ' ', '') || "
		"coalesce(strftime('%Y%m%d', datetime(json_extract(value, '$.OriginalAirdate'), 'unixepoch')) || ' ', '') || "
		"'[' || strftime('%Y%m%d-%H%M', datetime(json_extract(value, '$.StartTime'), 'unixepoch')) || ']' as filename "
		"from recording, json_each(recording.data) where json_extract(value, '$.CmdURL') like ?1 limit 1";

	result = sqlite3_prepare_v2(instance, sql, -1, &statement, nullptr);
	if(result != SQLITE_OK) throw sqlite_exception(result, sqlite3_errmsg(instance));

	try {

		// Bind the query parameters
		result = sqlite3_bind_text(statement, 1, recordingid, -1, SQLITE_STATIC);
		if(result != SQLITE_OK) throw sqlite_exception(result);
		
		// Execute the scalar query
		result = sqlite3_step(statement);

		// There should be a single SQLITE_ROW returned from the initial step
		if(result == SQLITE_ROW) filename.assign(reinterpret_cast<char const*>(sqlite3_column_text(statement, 0)));
		else if(result != SQLITE_DONE) throw sqlite_exception(result, sqlite3_errmsg(instance));

		sqlite3_finalize(statement);
		return filename;
	}

	catch(...) { sqlite3_finalize(statement); throw; }
}

//---------------------------------------------------------------------------
// get_recording_lastposition
//
// Gets the last played position for a specific recording
//
// Arguments:
//
//	instance		- Database instance
//	recordingid		- Recording identifier (command url)

int get_recording_lastposition(sqlite3* instance, char const* recordingid)
{
	sqlite3_stmt*				statement;				// Database query statement
	int							lastposition = 0;		// Last played position
	int							result;					// Result from SQLite function call

	if(instance == nullptr) return 0;

	// Prepare a scalar result query to get the last played position of the recording from the storage engine. Limit the amount 
	// of JSON that needs to be sifted through by specifically asking for the series that this recording belongs to
	auto sql = "with httprequest(response) as (select http_request(json_extract(device.data, '$.StorageURL') || '?SeriesID=' || "
		"(select json_extract(value, '$.SeriesID') as seriesid from recording, json_each(recording.data) where json_extract(value, '$.CmdURL') like ?1)) from device where device.type = 'storage') "
		"select coalesce(json_extract(entry.value, '$.Resume'), 0) as resume from httprequest, json_each(httprequest.response) as entry where json_extract(entry.value, '$.CmdURL') like ?1 limit 1";

	result = sqlite3_prepare_v2(instance, sql, -1, &statement, nullptr);
	if(result != SQLITE_OK) throw sqlite_exception(result, sqlite3_errmsg(instance));

	try { 
		
		// Bind the query parameters
		result = sqlite3_bind_text(statement, 1, recordingid, -1, SQLITE_STATIC);
		if(result != SQLITE_OK) throw sqlite_exception(result);

		// Execute the scalar query
		result = sqlite3_step(statement);

		// If the query returned a result, use that value otherwise leave at zero
		if(result == SQLITE_ROW) lastposition = sqlite3_column_int(statement, 0);

		sqlite3_finalize(statement);
		return lastposition;
	}

	catch(...) { sqlite3_finalize(statement); throw; }
}

//---------------------------------------------------------------------------
// get_recording_stream_url
//
// Gets the playback URL for a recording
//
// Arguments:
//
//	instance		- Database instance
//	recordingid		- Recording identifier (command url)

std::string get_recording_stream_url(sqlite3* instance, char const* recordingid)
{
	sqlite3_stmt*				statement;				// Database query statement
	std::string					streamurl;				// Generated stream URL
	int							result;					// Result from SQLite function call

	if((instance == nullptr) || (recordingid == nullptr)) return streamurl;

	// Prepare a scalar result query to generate a stream URL for the specified recording
	auto sql = "select json_extract(value, '$.PlayURL') as streamurl "
		"from recording, json_each(recording.data) where json_extract(value, '$.CmdURL') like ?1";

	result = sqlite3_prepare_v2(instance, sql, -1, &statement, nullptr);
	if(result != SQLITE_OK) throw sqlite_exception(result, sqlite3_errmsg(instance));

	try {

		// Bind the query parameters
		result = sqlite3_bind_text(statement, 1, recordingid, -1, SQLITE_STATIC);
		if(result != SQLITE_OK) throw sqlite_exception(result);
		
		// Execute the scalar query
		result = sqlite3_step(statement);

		// There should be a single SQLITE_ROW returned from the initial step
		if(result == SQLITE_ROW) streamurl.assign(reinterpret_cast<char const*>(sqlite3_column_text(statement, 0)));
		else if(result != SQLITE_DONE) throw sqlite_exception(result, sqlite3_errmsg(instance));

		sqlite3_finalize(statement);
		return streamurl;
	}

	catch(...) { sqlite3_finalize(statement); throw; }
}

//---------------------------------------------------------------------------
// get_recordingrule_count
//
// Gets the number of available recording rules in the database
//
// Arguments:
//
//	instance	- SQLite database instance

int get_recordingrule_count(sqlite3* instance)
{
	sqlite3_stmt*				statement;				// Database query statement
	int							rules = 0;				// Recording rules found in database
	int							result;					// Result from SQLite function call

	if(instance == nullptr) return 0;

	// Prepare a scalar result query to get the number of recording rules
	auto sql = "select count(recordingruleid) from recordingrule";
	result = sqlite3_prepare_v2(instance, sql, -1, &statement, nullptr);
	if(result != SQLITE_OK) throw sqlite_exception(result, sqlite3_errmsg(instance));

	try { 
		
		// Execute the scalar query
		result = sqlite3_step(statement);

		// There should be a single SQLITE_ROW returned from the initial step
		if(result == SQLITE_ROW) rules = sqlite3_column_int(statement, 0);
		else if(result != SQLITE_DONE) throw sqlite_exception(result, sqlite3_errmsg(instance));

		sqlite3_finalize(statement);
		return rules;
	}

	catch(...) { sqlite3_finalize(statement); throw; }
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
// get_stream_url
//
// Generates a stream URL for the specified channel
//
// Arguments:
//
//	instance		- Database instance
//	channelid		- Channel for which to get the stream

std::string get_stream_url(sqlite3* instance, union channelid channelid)
{
	sqlite3_stmt*				statement;				// Database query statement
	std::string					streamurl;				// Generated stream URL
	int							result;					// Result from SQLite function call

	if(instance == nullptr) return streamurl;

	// Prepare a scalar result query to generate a stream URL for the specified channel
	auto sql = "select json_extract(device.data, '$.BaseURL') || '/auto/v' || decode_channel_id(?1) || "
		"'?ClientID=' || (select clientid from client limit 1) || '&SessionID=0x' || hex(randomblob(4)) from device where type = 'storage' limit 1";

	result = sqlite3_prepare_v2(instance, sql, -1, &statement, nullptr);
	if(result != SQLITE_OK) throw sqlite_exception(result, sqlite3_errmsg(instance));

	try {

		// Bind the query parameters
		result = sqlite3_bind_int(statement, 1, channelid.value);
		if(result != SQLITE_OK) throw sqlite_exception(result);
		
		// Execute the scalar query
		result = sqlite3_step(statement);

		// There should be a single SQLITE_ROW returned from the initial step
		if(result == SQLITE_ROW) streamurl.assign(reinterpret_cast<char const*>(sqlite3_column_text(statement, 0)));
		else if(result != SQLITE_DONE) throw sqlite_exception(result, sqlite3_errmsg(instance));

		sqlite3_finalize(statement);
		return streamurl;
	}

	catch(...) { sqlite3_finalize(statement); throw; }
}

//---------------------------------------------------------------------------
// get_timer_count
//
// Gets the number of timerss in the database
//
// Arguments:
//
//	instance	- SQLite database instance
//	maxdays		- Maximum number of days worth of timers to report

int get_timer_count(sqlite3* instance, int maxdays)
{
	sqlite3_stmt*				statement;			// SQL statement to execute
	int							timers = 0;			// Number of timers found
	int							result;				// Result from SQLite function
	
	if(instance == nullptr) return 0;

	// If the maximum number of days wasn't provided, use a month as the boundary
	if(maxdays < 0) maxdays = 31;

	// Select the number of episodes set to record in the specified timeframe
	auto sql = "select count(*) from episode, json_each(episode.data) where json_extract(value, '$.RecordingRule') = 1 and "
		"(json_extract(value, '$.StartTime') < (cast(strftime('%s', 'now') as integer) + (?1 * 86400)))";

	result = sqlite3_prepare_v2(instance, sql, -1, &statement, nullptr);
	if(result != SQLITE_OK) throw sqlite_exception(result, sqlite3_errmsg(instance));

	try {

		// Bind the query parameter(s)
		result = sqlite3_bind_int(statement, 1, maxdays);
		if(result != SQLITE_OK) throw sqlite_exception(result);

		// Execute the scalar query
		result = sqlite3_step(statement);

		// There should be a single SQLITE_ROW returned from the initial step
		if(result == SQLITE_ROW) timers = sqlite3_column_int(statement, 0);
		else if(result != SQLITE_DONE) throw sqlite_exception(result, sqlite3_errmsg(instance));

		sqlite3_finalize(statement);
		return timers;
	}

	catch(...) { sqlite3_finalize(statement); throw; }
}

//---------------------------------------------------------------------------
// get_tuner_count
//
// Gets the number of tuner devices listed in the database
//
// Arguments:
//
//	instance		- Database instance

int get_tuner_count(sqlite3* instance)
{
	sqlite3_stmt*				statement;			// SQL statement to execute
	int							tuners = 0;			// Number of tuners found
	int							result;				// Result from SQLite function
	
	if(instance == nullptr) return 0;

	// Select the number of tuner devices listed in the device table
	auto sql = "select count(*) from device where type = 'tuner'";

	result = sqlite3_prepare_v2(instance, sql, -1, &statement, nullptr);
	if(result != SQLITE_OK) throw sqlite_exception(result, sqlite3_errmsg(instance));

	try {

		// Execute the scalar query
		result = sqlite3_step(statement);

		// There should be a single SQLITE_ROW returned from the initial step
		if(result == SQLITE_ROW) tuners = sqlite3_column_int(statement, 0);
		else if(result != SQLITE_DONE) throw sqlite_exception(result, sqlite3_errmsg(instance));

		sqlite3_finalize(statement);
		return tuners;
	}

	catch(...) { sqlite3_finalize(statement); throw; }
}

//---------------------------------------------------------------------------
// get_tuner_direct_channel_flag
//
// Gets a flag indicating if a channel can only be streamed directly from a tuner device
//
// Arguments:
//
//	instance		- Database instance
//	channelid		- Channel to be checked for tuner-direct only access

bool get_tuner_direct_channel_flag(sqlite3* instance, union channelid channelid)
{
	sqlite3_stmt*			statement;				// SQL statement to execute
	bool					directonly = false;		// Tuner-direct channel flag
	int						result;					// Result from SQLite function
	
	if(instance == nullptr) return 0;

	// Select a boolean flag indicating if any instances of this channel in the lineup table
	// are flagged as tuner-direct only channels
	auto sql = "select coalesce((select json_extract(lineupdata.value, '$.Demo') as tuneronly "
		"from lineup, json_each(lineup.data) as lineupdata "
		"where json_extract(lineupdata.value, '$.GuideNumber') = decode_channel_id(?1) and tuneronly is not null limit 1), 0)";

	result = sqlite3_prepare_v2(instance, sql, -1, &statement, nullptr);
	if(result != SQLITE_OK) throw sqlite_exception(result, sqlite3_errmsg(instance));

	try {

		// Bind the query parameters
		result = sqlite3_bind_int(statement, 1, channelid.value);
		if(result != SQLITE_OK) throw sqlite_exception(result);
		
		// Execute the scalar query
		result = sqlite3_step(statement);

		// There should be a single SQLITE_ROW returned from the initial step
		if(result == SQLITE_ROW) directonly = (sqlite3_column_int(statement, 0) != 0);
		else if(result != SQLITE_DONE) throw sqlite_exception(result, sqlite3_errmsg(instance));

		sqlite3_finalize(statement);
		return directonly;
	}

	catch(...) { sqlite3_finalize(statement); throw; }
}

//---------------------------------------------------------------------------
// get_tuner_stream_url
//
// Generates a stream URL for the specified channel on the specified tuner
//
// Arguments:
//
//	instance		- Database instance
//	tunerid			- Specified tuner id (XXXXXXXX-N format)
//	channelid		- Channel for which to get the stream

std::string get_tuner_stream_url(sqlite3* instance, char const* tunerid, union channelid channelid)
{
	sqlite3_stmt*				statement;				// Database query statement
	std::string					streamurl;				// Generated stream URL
	int							result;					// Result from SQLite function call

	if(instance == nullptr) throw std::invalid_argument("instance");
	if((tunerid == nullptr) || (*tunerid == '\0')) throw std::invalid_argument("tunerid");

	// Convert the provided tunerid (DDDDDDDD-T) into an std::string and find the hyphen
	std::string tuneridstr(tunerid);
	size_t hyphenpos = tuneridstr.find_first_of('-');
	if(hyphenpos == std::string::npos) throw std::invalid_argument("tunerid");

	// Break up the tunerid into deviceid and tuner index based on the hyphen position
	std::string deviceid = tuneridstr.substr(0, hyphenpos);
	std::string tunerindex = tuneridstr.substr(hyphenpos + 1);
	if((deviceid.length() == 0) || (tunerindex.length() != 1)) throw std::invalid_argument("tunerid");

	// Prepare a scalar query to generate the URL by matching up the device id and channel against the lineup
	auto sql = "select replace(json_extract(lineupdata.value, '$.URL'), 'auto', 'tuner' || ?1) as url "
		"from lineup, json_each(lineup.data) as lineupdata where lineup.deviceid = ?2 "
		"and json_extract(lineupdata.value, '$.GuideNumber') = decode_channel_id(?3)";

	result = sqlite3_prepare_v2(instance, sql, -1, &statement, nullptr);
	if(result != SQLITE_OK) throw sqlite_exception(result, sqlite3_errmsg(instance));

	try {

		// Bind the query parameters
		result = sqlite3_bind_text(statement, 1, tunerindex.c_str(), -1, SQLITE_STATIC);
		if(result == SQLITE_OK) result = sqlite3_bind_text(statement, 2, deviceid.c_str(), -1, SQLITE_STATIC);
		if(result == SQLITE_OK) result = sqlite3_bind_int(statement, 3, channelid.value);
		if(result != SQLITE_OK) throw sqlite_exception(result);
		
		// Execute the scalar query
		result = sqlite3_step(statement);

		// There should be a single SQLITE_ROW returned from the initial step()
		if(result == SQLITE_ROW) streamurl.assign(reinterpret_cast<char const*>(sqlite3_column_text(statement, 0)));
		else if(result != SQLITE_DONE) throw sqlite_exception(result, sqlite3_errmsg(instance));

		sqlite3_finalize(statement);
		return streamurl;
	}

	catch(...) { sqlite3_finalize(statement); throw; }
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
	sqlite_buffer		blob;					// Dynamically allocated blob buffer

	// useragent
	//
	// Static string to use as the User-Agent for this HTTP request
	static std::string useragent = "Kodi-PVR/" + std::string(ADDON_INSTANCE_VERSION_PVR) + " " + VERSION_PRODUCTNAME_ANSI + "/" + VERSION_VERSION3_ANSI;

	if((argc < 1) || (argc > 2) || (argv[0] == nullptr)) return sqlite3_result_error(context, "invalid argument", -1);

	// A null or zero-length URL results in a NULL result
	const char* url = reinterpret_cast<const char*>(sqlite3_value_text(argv[0]));
	if((url == nullptr) || (*url == 0)) return sqlite3_result_null(context);

	// Create a write callback for libcurl to invoke to write the data
	auto write_function = [](void const* data, size_t size, size_t count, void* userdata) -> size_t {

		try { return reinterpret_cast<sqlite_buffer*>(userdata)->append(data, (size * count)); }
		catch(...) { return 0; }
	};

	// Initialize the CURL session for the download operation
	CURL* curl = curl_easy_init();
	if(curl == nullptr) return sqlite3_result_error(context, "cannot initialize libcurl object", -1);

	// Set the CURL options and execute the web request to get the JSON string data
	CURLcode curlresult = curl_easy_setopt(curl, CURLOPT_URL, url);
	if(curlresult == CURLE_OK) curlresult = curl_easy_setopt(curl, CURLOPT_USERAGENT, useragent.c_str());
	if(curlresult == CURLE_OK) curlresult = curl_easy_setopt(curl, CURLOPT_NOSIGNAL, 1);
	if(curlresult == CURLE_OK) curlresult = curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);
	if(curlresult == CURLE_OK) curlresult = curl_easy_setopt(curl, CURLOPT_MAXREDIRS, 5L);
	if(curlresult == CURLE_OK) curlresult = curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT, 10L);
	if(curlresult == CURLE_OK) curlresult = curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, static_cast<CURL_WRITEFUNCTION>(write_function));
	if(curlresult == CURLE_OK) curlresult = curl_easy_setopt(curl, CURLOPT_WRITEDATA, reinterpret_cast<void*>(&blob));
	if(curlresult == CURLE_OK) curlresult = curl_easy_setopt(curl, CURLOPT_SHARE, static_cast<CURLSH*>(g_curlshare));
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

	// Send the resultant blob to SQLite as the result from this scalar function; detach from the sqlite_buffer
	return (cb > 0) ? sqlite3_result_blob(context, blob.detach(), static_cast<int>(cb), sqlite3_free) : sqlite3_result_null(context);
}

//---------------------------------------------------------------------------
// modify_recordingrule
//
// Modifies an existing recording rule
//
// Arguments:
//
//	instance		- Database instance
//	recordingrule	- Recording rule to be modified with updated information
//	seriesid		- On success, contains the series id for the recording rule

void modify_recordingrule(sqlite3* instance, struct recordingrule const& recordingrule, std::string& seriesid)
{
	sqlite3_stmt*				statement;			// SQL statement to execute
	int							result;				// Result from SQLite function
	
	if(instance == nullptr) return;

	execute_non_query(instance, "begin immediate transaction");

	try {

		// Update the specific recording rule with the new information provided
		auto sql = "replace into recordingrule "
			"select json_extract(value, '$.RecordingRuleID') as recordingruleid, "
			"json_extract(value, '$.SeriesID') as seriesid, "
			"value as data "
			"from "
			"json_each((with deviceauth(code) as (select url_encode(group_concat(json_extract(data, '$.DeviceAuth'), '')) from device) "
			"select http_request('http://api.hdhomerun.com/api/recording_rules?DeviceAuth=' || coalesce(deviceauth.code, '') || '&Cmd=change&RecordingRuleID=' || ?1 || "
			"'&RecentOnly=' || case when ?2 is null then '' else ?2 end || "
			"'&ChannelOnly=' || case when ?3 is null then '' else decode_channel_id(?3) end || "
			"'&AfterOriginalAirdateOnly=' || case when ?4 is null then '' else strftime('%s', date(?4, 'unixepoch')) end || "
			"'&StartPadding=' || case when ?5 is null then '30' else ?5 end || "
			"'&EndPadding=' || case when ?6 is null then '30' else ?6 end) as data "
			"from deviceauth))";

		// Prepare the query
		result = sqlite3_prepare_v2(instance, sql, -1, &statement, nullptr);
		if(result != SQLITE_OK) throw sqlite_exception(result, sqlite3_errmsg(instance));

		try {

			// Bind the non-null query parameter(s)
			result = sqlite3_bind_int(statement, 1, recordingrule.recordingruleid);
			if((result == SQLITE_OK) && (recordingrule.recentonly)) result = sqlite3_bind_int(statement, 2, 1);
			if((result == SQLITE_OK) && (recordingrule.channelid.value != 0)) result = sqlite3_bind_int(statement, 3, recordingrule.channelid.value);
			if((result == SQLITE_OK) && (recordingrule.afteroriginalairdateonly != 0)) result = sqlite3_bind_int(statement, 4, static_cast<int>(recordingrule.afteroriginalairdateonly));
			if((result == SQLITE_OK) && (recordingrule.startpadding != 30)) result = sqlite3_bind_int(statement, 5, recordingrule.startpadding);
			if((result == SQLITE_OK) && (recordingrule.endpadding != 30))  result = sqlite3_bind_int(statement, 6, recordingrule.endpadding);
			if(result != SQLITE_OK) throw sqlite_exception(result);

			// Execute the query - no result set is expected
			result = sqlite3_step(statement);
			if(result == SQLITE_ROW) throw string_exception(__func__, ": unexpected result set returned from non-query");
			if(result != SQLITE_DONE) throw sqlite_exception(result, sqlite3_errmsg(instance));

			sqlite3_finalize(statement);			// Finalize the SQLite statement
		}

		catch(...) { sqlite3_finalize(statement); throw; }

		// Update the episode data to take the modified recording rule(s) into account; watch out for the web
		// services returning 'null' on the episode query -- this happens when there are no episodes
		sql = "with deviceauth(code) as (select url_encode(group_concat(json_extract(data, '$.DeviceAuth'), '')) from device) "
			"replace into episode "
			"select recordingrule.seriesid, "
			"http_request('http://api.hdhomerun.com/api/episodes?DeviceAuth=' || coalesce(deviceauth.code, '') || '&SeriesID=' || recordingrule.seriesid) as data "
			"from recordingrule, deviceauth "
			"where recordingrule.recordingruleid = ?1 "
			"and cast(data as text) <> 'null'";

		// Prepare the query
		result = sqlite3_prepare_v2(instance, sql, -1, &statement, nullptr);
		if(result != SQLITE_OK) throw sqlite_exception(result, sqlite3_errmsg(instance));

		try {

			// Bind the non-null query parameter(s)
			result = sqlite3_bind_int(statement, 1, recordingrule.recordingruleid);
			if(result != SQLITE_OK) throw sqlite_exception(result);

			// Execute the query - no result set is expected
			result = sqlite3_step(statement);
			if(result == SQLITE_ROW) throw string_exception(__func__, ": unexpected result set returned from non-query");
			if(result != SQLITE_DONE) throw sqlite_exception(result, sqlite3_errmsg(instance));

			sqlite3_finalize(statement);			// Finalize the SQLite statement
		}

		catch(...) { sqlite3_finalize(statement); throw; }

		// Retrieve the seriesid for the recording rule for the caller
		sql = "select seriesid from recordingrule where recordingrule.recordingruleid = ?1";

		// Prepare the query
		result = sqlite3_prepare_v2(instance, sql, -1, &statement, nullptr);
		if(result != SQLITE_OK) throw sqlite_exception(result, sqlite3_errmsg(instance));

		try { 

			// Bind the non-null query parameter(s)
			result = sqlite3_bind_int(statement, 1, recordingrule.recordingruleid);
			if(result != SQLITE_OK) throw sqlite_exception(result);

			// Execute the query; one result column is expected
			result = sqlite3_step(statement);
			if(result == SQLITE_ROW) seriesid.assign(reinterpret_cast<char const*>(sqlite3_column_text(statement, 0)));
			else if(result != SQLITE_DONE) throw sqlite_exception(result, sqlite3_errmsg(instance));

			sqlite3_finalize(statement);
		}

		catch(...) { sqlite3_finalize(statement); throw; }

		// Commit the transaction
		execute_non_query(instance, "commit transaction");
	}

	// Rollback the entire transaction on any failure above
	catch(...) { try_execute_non_query(instance, "rollback transaction"); throw; }

	// Poke the recording engine(s) after a successful rule change; don't worry about exceptions
	try_execute_non_query(instance, "select http_request(json_extract(data, '$.BaseURL') || '/recording_events.post?sync') from device where type = 'storage'");
}

//---------------------------------------------------------------------------
// open_database
//
// Opens the SQLite database instance
//
// Arguments:
//
//	connstring		- Database connection string
//	flags			- Database open flags (see sqlite3_open_v2)

sqlite3* open_database(char const* connstring, int flags)
{
	return open_database(connstring, flags, false);
}

//---------------------------------------------------------------------------
// open_database
//
// Opens the SQLite database instance
//
// Arguments:
//
//	connstring		- Database connection string
//	flags			- Database open flags (see sqlite3_open_v2)
//	initialize		- Flag indicating database schema should be (re)initialized

sqlite3* open_database(char const* connstring, int flags, bool initialize)
{
	sqlite3*			instance = nullptr;			// SQLite database instance

	// Create the SQLite database using the provided connection string
	int result = sqlite3_open_v2(connstring, &instance, flags, nullptr);
	if(result != SQLITE_OK) throw sqlite_exception(result);

	// set the connection to report extended error codes
	//
	sqlite3_extended_result_codes(instance, -1);

	// set a busy_timeout handler for this connection
	//
	sqlite3_busy_timeout(instance, 5000);
	
	try {

		// switch the database to write-ahead logging
		//
		execute_non_query(instance, "pragma journal_mode=wal");

		// scalar function: clean_filename
		//
		result = sqlite3_create_function_v2(instance, "clean_filename", 1, SQLITE_UTF8 | SQLITE_DETERMINISTIC, nullptr, clean_filename, nullptr, nullptr, nullptr);
		if(result != SQLITE_OK) throw sqlite_exception(result);

		// scalar function: decode_channel_id
		//
		result = sqlite3_create_function_v2(instance, "decode_channel_id", 1, SQLITE_UTF8 | SQLITE_DETERMINISTIC, nullptr, decode_channel_id, nullptr, nullptr, nullptr);
		if(result != SQLITE_OK) throw sqlite_exception(result);

		// scalar function: encode_channel_id
		//
		result = sqlite3_create_function_v2(instance, "encode_channel_id", 1, SQLITE_UTF8 | SQLITE_DETERMINISTIC, nullptr, encode_channel_id, nullptr, nullptr, nullptr);
		if(result != SQLITE_OK) throw sqlite_exception(result);

		// scalar function: fnv_hash
		//
		result = sqlite3_create_function_v2(instance, "fnv_hash", -1, SQLITE_UTF8 | SQLITE_DETERMINISTIC, nullptr, fnv_hash, nullptr, nullptr, nullptr);
		if(result != SQLITE_OK) throw sqlite_exception(result);

		// scalar function: generate_uuid (non-deterministic)
		//
		result = sqlite3_create_function_v2(instance, "generate_uuid", 0, SQLITE_UTF8, nullptr, generate_uuid, nullptr, nullptr, nullptr);
		if(result != SQLITE_OK) throw sqlite_exception(result);

		// scalar function: get_channel_number
		//
		result = sqlite3_create_function_v2(instance, "get_channel_number", 1, SQLITE_UTF8 | SQLITE_DETERMINISTIC, nullptr, get_channel_number, nullptr, nullptr, nullptr);
		if(result != SQLITE_OK) throw sqlite_exception(result);

		// scalar function: get_episode_number
		//
		result = sqlite3_create_function_v2(instance, "get_episode_number", 1, SQLITE_UTF8 | SQLITE_DETERMINISTIC, nullptr, get_episode_number, nullptr, nullptr, nullptr);
		if(result != SQLITE_OK) throw sqlite_exception(result);

		// scalar function: get_season_number
		//
		result = sqlite3_create_function_v2(instance, "get_season_number", 1, SQLITE_UTF8 | SQLITE_DETERMINISTIC, nullptr, get_season_number, nullptr, nullptr, nullptr);
		if(result != SQLITE_OK) throw sqlite_exception(result);

		// scalar function: http_request (non-deterministic)
		//
		result = sqlite3_create_function_v2(instance, "http_request", -1, SQLITE_UTF8, nullptr, http_request, nullptr, nullptr, nullptr);
		if(result != SQLITE_OK) throw sqlite_exception(result);

		// scalar function: url_encode
		//
		result = sqlite3_create_function_v2(instance, "url_encode", 1, SQLITE_UTF8 | SQLITE_DETERMINISTIC, nullptr, url_encode, nullptr, nullptr, nullptr);
		if(result != SQLITE_OK) throw sqlite_exception(result);

		// Only execute schema creation steps if the database is being initialized; the caller needs
		// to ensure that this is set for only one connection otherwise locking issues can occur
		//
		if(initialize) {

			// table: client
			//
			// clientid(pk)
			execute_non_query(instance, "create table if not exists client(clientid text primary key not null)");

			// table: device
			// 
			// deviceid(pk) | type | data
			execute_non_query(instance, "create table if not exists device(deviceid text primary key not null, type text, data text)");

			// table: lineup
			//
			// deviceid(pk) | data
			execute_non_query(instance, "create table if not exists lineup(deviceid text primary key not null, data text)");

			// table: recording
			//
			// deviceid(pk) | data
			execute_non_query(instance, "create table if not exists recording(deviceid text primary key not null, data text)");

			// table: guide
			//
			// channelid(pk) | channelname | iconurl | data
			execute_non_query(instance, "create table if not exists guide(channelid integer primary key not null, channelname text, iconurl text, data text)");

			// table: recordingrule
			//
			// recordingruleid(pk) | data
			execute_non_query(instance, "create table if not exists recordingrule(recordingruleid text primary key not null, seriesid text not null, data text)");

			// table: episode
			//
			// seriesid(pk) | data
			execute_non_query(instance, "create table if not exists episode(seriesid text primary key not null, data text)");

			// table: genremap
			//
			// filter(pk) | genretype
			execute_non_query(instance, "create table if not exists genremap(filter text primary key not null, genretype integer)");

			// (re)generate the clientid
			//
			execute_non_query(instance, "delete from client");
			execute_non_query(instance, "insert into client values(generate_uuid())");

			// (re)build the genremap table
			//
			sqlite3_exec(instance, "replace into genremap values('Movies', 0x10)", nullptr, nullptr, nullptr);		// EPG_EVENT_CONTENTMASK_MOVIEDRAMA 
			sqlite3_exec(instance, "replace into genremap values('News', 0x20)", nullptr, nullptr, nullptr);		// EPG_EVENT_CONTENTMASK_NEWSCURRENTAFFAIRS
			sqlite3_exec(instance, "replace into genremap values('Comedy', 0x30)", nullptr, nullptr, nullptr);		// EPG_EVENT_CONTENTMASK_SHOW
			sqlite3_exec(instance, "replace into genremap values('Drama', 0x30)", nullptr, nullptr, nullptr);		// EPG_EVENT_CONTENTMASK_SHOW 
			sqlite3_exec(instance, "replace into genremap values('Game Show', 0x30)", nullptr, nullptr, nullptr);	// EPG_EVENT_CONTENTMASK_SHOW
			sqlite3_exec(instance, "replace into genremap values('Talk Show', 0x30)", nullptr, nullptr, nullptr);	// EPG_EVENT_CONTENTMASK_SHOW
			sqlite3_exec(instance, "replace into genremap values('Sports', 0x40)", nullptr, nullptr, nullptr);		// EPG_EVENT_CONTENTMASK_SPORTS
			sqlite3_exec(instance, "replace into genremap values('Kids', 0x50)", nullptr, nullptr, nullptr);		// EPG_EVENT_CONTENTMASK_CHILDRENYOUTH
			sqlite3_exec(instance, "replace into genremap values('Food', 0xA0)", nullptr, nullptr, nullptr);		// EPG_EVENT_CONTENTMASK_LEISUREHOBBIES
		}
	}

	// Close the database instance on any thrown exceptions
	catch(...) { sqlite3_close(instance); throw; }

	return instance;
}

//---------------------------------------------------------------------------
// set_channel_visibility
//
// Sets the visibility of a channel on all known tuner devices
//
// Arguments:
//
//	instance	- Database instance
//	channelid	- Channel to set the visibility
//	visibility	- New visibility of the channel

void set_channel_visibility(sqlite3* instance, union channelid channelid, enum channel_visibility visibility)
{
	sqlite3_stmt*				statement;				// Database query statement
	char						flag;					// Visibility flag to be set
	int							result;					// Result from SQLite function call

	if(instance == nullptr) throw std::invalid_argument("instance");

	// Convert the visibility into the character code to send to the tuner(s)
	switch(visibility) {

		case channel_visibility::enabled: flag = '-'; break;
		case channel_visibility::favorite: flag = '+'; break;
		case channel_visibility::disabled: flag = 'x'; break;
		default: throw std::invalid_argument("visibility");
	}

	// Prepate a query to generate the necessary URLs for each tuner that supports the channel
	auto sql = "with deviceurls(url) as "
		"(select distinct(json_extract(device.data, '$.BaseURL') || '/lineup.post?favorite=' || ?1 || decode_channel_id(?2)) "
		"from lineup inner join device using(deviceid), json_each(lineup.data) as lineupdata "
		"where json_extract(lineupdata.value, '$.GuideNumber') = decode_channel_id(?2)) "
		"select http_request(url) from deviceurls";

	result = sqlite3_prepare_v2(instance, sql, -1, &statement, nullptr);
	if(result != SQLITE_OK) throw sqlite_exception(result, sqlite3_errmsg(instance));

	try {

		// Bind the query parameters
		result = sqlite3_bind_text(statement, 1, &flag, 1, SQLITE_STATIC);
		if(result == SQLITE_OK) result = sqlite3_bind_int(statement, 2, channelid.value);
		if(result != SQLITE_OK) throw sqlite_exception(result);
		
		// Execute the query; ignore any rows that are returned
		do result = sqlite3_step(statement);
		while(result == SQLITE_ROW);

		// The final result from sqlite3_step should be SQLITE_DONE
		if(result != SQLITE_DONE) throw sqlite_exception(result, sqlite3_errmsg(instance));

		sqlite3_finalize(statement);
	}

	catch(...) { sqlite3_finalize(statement); throw; }
}

//---------------------------------------------------------------------------
// set_recording_lastposition
//
// Sets the last played position for a specific recording
//
// Arguments:
//
//	instance		- Database instance
//	recordingid		- Recording identifier (command url)
//	lastposition	- Last position to be stored

void set_recording_lastposition(sqlite3* instance, char const* recordingid, int lastposition)
{
	sqlite3_stmt*				statement;			// SQL statement to execute
	int							result;				// Result from SQLite function
	
	if((instance == nullptr) || (recordingid == nullptr)) return;

	// Prepare a query that will update the specified recording on the storage device and the local database
	auto sql = "with httprequest(response) as (select http_request(?1 || '&cmd=set&Resume=' || ?2)) "
		"replace into recording select "
		"deviceid, "
		"(select case when fullkey is null then recording.data else json_set(recording.data, fullkey || '.Resume', ?2) end) as data "
		"from httprequest, recording, "
		"(select fullkey from recording, json_each(recording.data) as entry where json_extract(entry.value, '$.CmdURL') like ?1 limit 1)";

	result = sqlite3_prepare_v2(instance, sql, -1, &statement, nullptr);
	if(result != SQLITE_OK) throw sqlite_exception(result, sqlite3_errmsg(instance));

	try {

		// Bind the query parameter(s)
		result = sqlite3_bind_text(statement, 1, recordingid, -1, SQLITE_STATIC);
		if(result == SQLITE_OK) result = sqlite3_bind_int(statement, 2, lastposition);
		if(result != SQLITE_OK) throw sqlite_exception(result);

		// Execute the query; there shouldn't be any result set returned from it
		result = sqlite3_step(statement);
		if(result == SQLITE_ROW) throw string_exception(__func__, ": unexpected result set returned from non-query");
		if(result != SQLITE_DONE) throw sqlite_exception(result, sqlite3_errmsg(instance));

		sqlite3_finalize(statement);			// Finalize the SQLite statement
	}

	catch(...) { sqlite3_finalize(statement); throw; }
}

//---------------------------------------------------------------------------
// try_execute_non_query
//
// Executes a non-query against the database and eats any exceptions
//
// Arguments:
//
//	instance	- Database instance
//	sql			- SQL non-query to execute

bool try_execute_non_query(sqlite3* instance, char const* sql)
{
	try { execute_non_query(instance, sql); }
	catch(...) { return false; }

	return true;
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

#pragma warning(pop)
