//---------------------------------------------------------------------------
// Copyright (c) 2016-2021 Michael G. Brehm
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

#include <cstddef>
#include <vector>

#include "genremap.h"
#include "sqlite_exception.h"
#include "string_exception.h"

#pragma warning(push, 4)

// dbextension.cpp
//
extern "C" int sqlite3_extension_init(sqlite3 *db, char** errmsg, const sqlite3_api_routines* api);

// discover_device
//
// Information about a single HDHomeRun device discovered via broadcast
struct discover_device {

	uint32_t			deviceid;
	char const*			storageid;
	char const*			baseurl;
};

// enumerate_devices_callback
//
// Callback function passed to enumerate_devices
using enumerate_devices_callback = std::function<void(struct discover_device const& device)>;

// xmltv_channel_element
//
// Structure used to convert the pointers returned from the XMLTV callback into std::string()s to
// hold onto them for insertion into the channels table after the listings have been processed
struct xmltv_channel_element {

	std::string		id;					// Channel identifier
	std::string		number;				// Channel number
	std::string		name;				// Channel name
	std::string		altname;			// Channel alternate name
	std::string		network;			// Channel network
	std::string		iconsrc;			// Channel icon URL
};

//---------------------------------------------------------------------------
// FUNCTION PROTOTYPES
//---------------------------------------------------------------------------

static void bind_parameter(sqlite3_stmt* statement, int& paramindex, const char* value);
static void bind_parameter(sqlite3_stmt* statement, int& paramindex, uint32_t value);
static void bind_parameter(sqlite3_stmt* statement, int& paramindex, int32_t value);
static bool discover_devices_broadcast(sqlite3* instance);
static bool discover_devices_http(sqlite3* instance);
static void discover_series_recordings(sqlite3* instance, char const* seriesid);
static void enumerate_devices_broadcast(enumerate_devices_callback const& callback);
template<typename... _parameters> static int execute_non_query(sqlite3* instance, char const* sql, _parameters&&... parameters);
template<typename... _parameters> static int execute_scalar_int(sqlite3* instance, char const* sql, _parameters&&... parameters);
template<typename... _parameters> static int64_t execute_scalar_int64(sqlite3* instance, char const* sql, _parameters&&... parameters);
template<typename... _parameters> static std::string execute_scalar_string(sqlite3* instance, char const* sql, _parameters&&... parameters);

//---------------------------------------------------------------------------
// CONNECTIONPOOL IMPLEMENTATION
//---------------------------------------------------------------------------

//---------------------------------------------------------------------------
// connectionpool Constructor
//
// Arguments:
//
//	connstring		- Database connection string
//	poolsize		- Initial connection pool size
//	flags			- Database connection flags

connectionpool::connectionpool(char const* connstring, size_t poolsize, int flags) : 
	m_connstr((connstring) ? connstring : ""), m_flags(flags)
{
	sqlite3*		handle = nullptr;		// Initial database connection

	if(connstring == nullptr) throw std::invalid_argument("connstring");

	// Create and pool an initial connection to initialize the database
	handle = open_database(m_connstr.c_str(), m_flags, true);
	m_connections.push_back(handle);
	m_queue.push(handle);

	// Create and pool the requested number of additional connections
	try {

		for(size_t index = 1; index < poolsize; index++) {

			handle = open_database(m_connstr.c_str(), m_flags, false);
			m_connections.push_back(handle);
			m_queue.push(handle);
		}
	}

	catch(...) {

		// Clear the connection cache and destroy all created connections
		while(!m_queue.empty()) m_queue.pop();
		for(auto const& iterator : m_connections) close_database(iterator);

		throw;
	}
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
//	deviceauth		- Device authorization string to use
//	recordingrule	- New recording rule to be added

void add_recordingrule(sqlite3* instance, char const* deviceauth, struct recordingrule const& recordingrule)
{
	sqlite3_stmt*				statement;			// SQL statement to execute
	int							result;				// Result from SQLite function
	
	if((instance == nullptr) || (deviceauth == nullptr)) return;

	// Add the new recording rule
	auto sql = "replace into recordingrule "
		"select json_extract(value, '$.RecordingRuleID') as recordingruleid, "
		"cast(strftime('%s', 'now') as integer) as discovered, "
		"json_extract(value, '$.SeriesID') as seriesid, "
		"value as data from "
		"json_each(json_get('http://api.hdhomerun.com/api/recording_rules', 'post', 'DeviceAuth=' || ?1 || '&Cmd=add&SeriesID=' || ?2 || "
		"case when ?3 is null then '' else '&RecentOnly=' || ?3 end || "
		"case when ?4 is null then '' else '&ChannelOnly=' || decode_channel_id(?4) end || "
		"case when ?5 is null then '' else '&AfterOriginalAirdateOnly=' || strftime('%s', date(?5, 'unixepoch')) end || "
		"case when ?6 is null then '' else '&DateTimeOnly=' || ?6 end || "
		"case when ?7 is null then '' else '&StartPadding=' || ?7 end || "
		"case when ?8 is null then '' else '&EndPadding=' || ?8 end))";

	// Prepare the query
	result = sqlite3_prepare_v2(instance, sql, -1, &statement, nullptr);
	if(result != SQLITE_OK) throw sqlite_exception(result, sqlite3_errmsg(instance));

	try {

		// Bind the query parameter(s)
		result = sqlite3_bind_text(statement, 1, deviceauth, -1, SQLITE_STATIC);
		if(result == SQLITE_OK) result = sqlite3_bind_text(statement, 2, recordingrule.seriesid, -1, SQLITE_STATIC);
		if(result == SQLITE_OK) result = (recordingrule.recentonly) ? sqlite3_bind_int(statement, 3, 1) : sqlite3_bind_null(statement, 3);
		if(result == SQLITE_OK) result = (recordingrule.channelid.value != 0) ? sqlite3_bind_int(statement, 4, recordingrule.channelid.value) : sqlite3_bind_null(statement, 4);
		if(result == SQLITE_OK) result = (recordingrule.afteroriginalairdateonly != 0) ? sqlite3_bind_int64(statement, 5, recordingrule.afteroriginalairdateonly) : sqlite3_bind_null(statement, 5);
		if(result == SQLITE_OK) result = (recordingrule.datetimeonly != 0) ? sqlite3_bind_int64(statement, 6, recordingrule.datetimeonly) : sqlite3_bind_null(statement, 6);
		if(result == SQLITE_OK) result = (recordingrule.startpadding != 30) ? sqlite3_bind_int(statement, 7, recordingrule.startpadding) : sqlite3_bind_null(statement, 7);
		if(result == SQLITE_OK) result = (recordingrule.endpadding != 30) ? sqlite3_bind_int(statement, 8, recordingrule.endpadding) : sqlite3_bind_null(statement, 8);
		if(result != SQLITE_OK) throw sqlite_exception(result);

		// Execute the query - no result set is expected
		result = sqlite3_step(statement);
		if(result == SQLITE_ROW) throw string_exception(__func__, ": unexpected result set returned from non-query");
		if(result != SQLITE_DONE) throw sqlite_exception(result, sqlite3_errmsg(instance));

		sqlite3_finalize(statement);			// Finalize the SQLite statement
	}

	catch(...) { sqlite3_finalize(statement); throw; }

	// Poke the recording engine(s) after a successful rule change; don't worry about exceptions
	try_execute_non_query(instance, "select json_get(json_extract(data, '$.BaseURL') || '/recording_events.post?sync', 'post') from device "
		"where json_extract(data, '$.StorageURL') is not null");
}

//---------------------------------------------------------------------------
// bind_parameter (local)
//
// Used by execute_non_query to bind a string parameter
//
// Arguments:
//
//	statement		- SQL statement instance
//	paramindex		- Index of the parameter to bind; will be incremented
//	value			- Value to bind as the parameter

static void bind_parameter(sqlite3_stmt* statement, int& paramindex, char const* value)
{
	int					result;				// Result from binding operation

	// If a null string pointer was provided, bind it as NULL instead of TEXT
	if(value == nullptr) result = sqlite3_bind_null(statement, paramindex++);
	else result = sqlite3_bind_text(statement, paramindex++, value, -1, SQLITE_STATIC);

	if(result != SQLITE_OK) throw sqlite_exception(result);
}

//---------------------------------------------------------------------------
// bind_parameter (local)
//
// Used by execute_non_query to bind a character parameter
//
// Arguments:
//
//	statement		- SQL statement instance
//	paramindex		- Index of the parameter to bind; will be incremented
//	value			- Value to bind as the parameter

static void bind_parameter(sqlite3_stmt* statement, int& paramindex, char& value)
{
	int result = sqlite3_bind_text(statement, paramindex++, &value, 1, SQLITE_STATIC);
	if(result != SQLITE_OK) throw sqlite_exception(result);
}

//---------------------------------------------------------------------------
// bind_parameter (local)
//
// Used by execute_non_query to bind an integer parameter
//
// Arguments:
//
//	statement		- SQL statement instance
//	paramindex		- Index of the parameter to bind; will be incremented
//	value			- Value to bind as the parameter

static void bind_parameter(sqlite3_stmt* statement, int& paramindex, int32_t value)
{
	int result = sqlite3_bind_int(statement, paramindex++, value);
	if(result != SQLITE_OK) throw sqlite_exception(result);
}

//---------------------------------------------------------------------------
// bind_parameter (local)
//
// Used by execute_non_query to bind an integer parameter
//
// Arguments:
//
//	statement		- SQL statement instance
//	paramindex		- Index of the parameter to bind; will be incremented
//	value			- Value to bind as the parameter

static void bind_parameter(sqlite3_stmt* statement, int& paramindex, uint32_t value)
{
	int result = sqlite3_bind_int64(statement, paramindex++, static_cast<int64_t>(value));
	if(result != SQLITE_OK) throw sqlite_exception(result);
}

//---------------------------------------------------------------------------
// clear_authorization_strings
//
// Clears the device authorization string from all available tuners
//
// Arguments:
//
//	instance	- Database instance handle
//	expiry		- Expiration time, in seconds

void clear_authorization_strings(sqlite3* instance, int expiry)
{
	if((instance == nullptr) || (expiry <= 0)) return;

	// Remove all stale 'DeviceAuth' JSON properties from the device discovery data
	execute_non_query(instance, "update device set discovered = cast(strftime('%s', 'now') as integer), "
		"data = json_remove(data, '$.DeviceAuth') where coalesce(discovered, 0) < (cast(strftime('%s', 'now') as integer) - ?1)", expiry);
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
	if((instance == nullptr) || (recordingid == nullptr)) return;

	// Delete the specified recording from the storage device
	execute_non_query(instance, "select json_get(json_extract(data, '$.CmdURL') || '&cmd=delete&rerecord=' || ?2, 'post') "
		"from recording where recordingid like ?1 limit 1", recordingid, (rerecord) ? 1 : 0);

	// Delete the specified recording from the local database
	execute_non_query(instance, "delete from recording where recordingid like ?1", recordingid);
}

//---------------------------------------------------------------------------
// delete_recordingrule
//
// Deletes a recordingrule from the database instance
//
// Arguments:
//
//	instance			- Database instance
//	deviceauth			- Device authorization string to use
//	recordingruleid		- Recording Rule ID of the item to delete

void delete_recordingrule(sqlite3* instance, char const* deviceauth, unsigned int recordingruleid)
{
	if((instance == nullptr) || (deviceauth == nullptr)) return;

	// Delete the recording rule from the backend
	execute_non_query(instance, "select json_get('http://api.hdhomerun.com/api/recording_rules', 'post', 'DeviceAuth=' || ?1 || '&Cmd=delete&RecordingRuleID=' || ?2)",
		deviceauth, recordingruleid);

	// Delete the recording rule from the database
	execute_non_query(instance, "delete from recordingrule where recordingruleid = ?1", recordingruleid);

	// Poke the recording engine(s) after a successful rule change; don't worry about exceptions
	try_execute_non_query(instance, "select json_get(json_extract(data, '$.BaseURL') || '/recording_events.post?sync', 'post') from device "
		"where json_extract(data, '$.StorageURL') is not null");
}

//---------------------------------------------------------------------------
// discover_devices
//
// Reloads the information about the available devices
//
// Arguments:
//
//	instance		- SQLite database instance
//	usehttp			- Flag to use HTTP rather than broadcast discovery

void discover_devices(sqlite3* instance, bool usehttp)
{
	bool ignored;
	return discover_devices(instance, usehttp, ignored);
}

//---------------------------------------------------------------------------
// discover_devices
//
// Reloads the information about the available devices
//
// Arguments:
//
//	instance		- SQLite database instance
//	usehttp			- Flag to use HTTP rather than broadcast discovery
//	changed			- Flag indicating if the data has changed

void discover_devices(sqlite3* instance, bool usehttp, bool& changed)
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
		hastuners = (usehttp) ? discover_devices_http(instance) : discover_devices_broadcast(instance);

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
			if(execute_non_query(instance, "replace into device select * from discover_device where deviceid not in (select deviceid from device)") > 0) changed = true;

			// Update the JSON for every device based on the discovery data; this is not considered a change as
			// the device authorization string changes routinely.  (REPLACE INTO is easier than UPDATE in this case)
			execute_non_query(instance, "replace into device select * from discover_device");
			
			// Update all of the discovery timestamps to the current time so they are all the same post-discovery
			execute_non_query(instance, "update device set discovered = ?1", static_cast<int>(time(nullptr)));

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
// discover_devices_broadcast (local)
//
// discover_devices helper -- loads the discover_device table from UDP broadcast
//
// Arguments:
//
//	instance		- SQLite database instance

static bool discover_devices_broadcast(sqlite3* instance)
{
	bool					hastuners = false;		// Flag indicating tuners were found
	sqlite3_stmt*			statement;				// SQL statement to execute
	int						result;					// Result from SQLite function

	assert(instance != nullptr);

	// deviceid | discovered | dvrauthorized | data
	//
	// NOTE: Some devices (HDHomeRun SCRIBE) are both tuners and storage engines; UDP broadcast discovery
	// will generate two entries for those.  Avoid inserting the same DeviceID into the temp table more than once
	auto sql = "insert into discover_device select ?1, cast(strftime('%s', 'now') as integer), null, ?2 "
		"where not exists(select 1 from discover_device where deviceid like ?1)";

	result = sqlite3_prepare_v2(instance, sql, -1, &statement, nullptr);
	if(result != SQLITE_OK) throw sqlite_exception(result, sqlite3_errmsg(instance));

	try {

		// Enumerate the devices on the local network accessible via UDP broadcast and insert them
		// into the temp table using the baseurl as 'data' rather than the discovery JSON
		enumerate_devices_broadcast([&](struct discover_device const& device) -> void { 

			char			deviceid[9];			// Converted device id string

			// Convert the device identifier into a hexadecimal string
			snprintf(deviceid, std::extent<decltype(deviceid)>::value, "%08X", device.deviceid);

			// The presence or lack of a tuner device id is used as the function return value
			if(device.deviceid != 0) hastuners = true;

			// Bind the query parameter(s)
			result = sqlite3_bind_text(statement, 1, (device.deviceid != 0) ? deviceid : device.storageid, -1, SQLITE_STATIC);
			if(result == SQLITE_OK) result = sqlite3_bind_text(statement, 2, device.baseurl, -1, SQLITE_STATIC);
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
	execute_non_query(instance, "update discover_device set data = json_get(data || '/discover.json')");

	// Update the deviceid column for legacy storage devices, older versions did not return the storageid attribute during broadcast discovery
	execute_non_query(instance, "update discover_device set deviceid = coalesce(json_extract(data, '$.StorageID'), '00000000') where deviceid is null");

	// Update the DVR service authorization flag for each discovered tuner device
	execute_non_query(instance, "update discover_device set dvrauthorized = json_extract(json_get('http://api.hdhomerun.com/api/account?DeviceAuth=' || "
		"coalesce(url_encode(json_extract(data, '$.DeviceAuth')), '')), '$.DvrActive') where json_extract(data, '$.DeviceAuth') is not null");

	// Indicate if any tuner devices were detected during discovery or not
	return hastuners;
}

//---------------------------------------------------------------------------
// discover_devices_http (local)
//
// discover_devices helper -- loads the discover_device table from the HTTP API
//
// Arguments:
//
//	instance		- SQLite database instance

static bool discover_devices_http(sqlite3* instance)
{
	assert(instance != nullptr);
	
	//
	// NOTE: This had to be broken up into a multi-step query involving a temp table to avoid a SQLite bug/feature
	// wherein using a function (json_get in this case) as part of a column definition is reevaluated when
	// that column is subsequently used as part of a WHERE clause:
	//
	// [http://mailinglists.sqlite.org/cgi-bin/mailman/private/sqlite-users/2015-August/061083.html]
	//

	// Discover the devices from the HTTP API and insert them into the discover_device temp table
	execute_non_query(instance, "drop table if exists discover_device_http");
	execute_non_query(instance, "create temp table discover_device_http as select "
		"coalesce(json_extract(discovery.value, '$.DeviceID'), coalesce(json_extract(discovery.value, '$.StorageID'), '00000000')) as deviceid, "
		"cast(strftime('%s', 'now') as integer) as discovered, "
		"null as dvrauthorized, "
		"json_get(json_extract(discovery.value, '$.DiscoverURL')) as data from json_each(json_get('http://api.hdhomerun.com/discover')) as discovery");
	execute_non_query(instance, "insert into discover_device select deviceid, discovered, dvrauthorized, data from discover_device_http where data is not null and json_extract(data, '$.Legacy') is null");
	execute_non_query(instance, "drop table discover_device_http");

	// Update the DVR service authorization flag for each discovered tuner device
	execute_non_query(instance, "update discover_device set dvrauthorized = json_extract(json_get('http://api.hdhomerun.com/api/account?DeviceAuth=' || "
		"coalesce(url_encode(json_extract(data, '$.DeviceAuth')), '')), '$.DvrActive') where json_extract(data, '$.DeviceAuth') is not null");

	// Determine if any tuner devices were discovered from the HTTP discovery query
	return (execute_scalar_int(instance, "select count(deviceid) as numtuners from discover_device where json_extract(data, '$.LineupURL') is not null") > 0);
}

//---------------------------------------------------------------------------
// discover_episodes
//
// Reloads the information about episodes associated with a recording rule
//
// Arguments:
//
//	instance	- SQLite database instance
//	deviceauth	- Device authorization string to use

void discover_episodes(sqlite3* instance, char const* deviceauth)
{
	bool ignored;
	return discover_episodes(instance, deviceauth, ignored);
}

//---------------------------------------------------------------------------
// discover_episodes
//
// Reloads the information about episodes associated with a recording rule
//
// Arguments:
//
//	instance	- SQLite database instance
//	deviceauth	- Device authorization string to use
//	changed		- Flag indicating if the data has changed

void discover_episodes(sqlite3* instance, char const* deviceauth, bool& changed)
{
	changed = false;							// Initialize [out] argument

	if(instance == nullptr) throw std::invalid_argument("instance");
	if(deviceauth == nullptr) throw std::invalid_argument("deviceauth");

	// Clone the episode table schema into a temporary table
	execute_non_query(instance, "drop table if exists discover_episode");
	execute_non_query(instance, "create temp table discover_episode as select * from episode limit 0");

	try {

		// Discover the episode information for each series that has a recording rule
		execute_non_query(instance, "insert into discover_episode select key as seriesid, cast(strftime('%s', 'now') as integer) as discovered, value as data from "
			"json_each((select json_get_aggregate('http://api.hdhomerun.com/api/episodes?DeviceAuth=' || ?1 || '&SeriesID=' || entry.seriesid, entry.seriesid) "
			"from (select distinct json_extract(data, '$.SeriesID') as seriesid from recordingrule where seriesid is not null) as entry))", deviceauth);

		// Filter the resultant JSON data to only include episodes associated with a recording rule and sort that data by both the start
		// time and the channel number; the backend ordering is unreliable when a series exists on multiple channels
		execute_non_query(instance, "update discover_episode set data = (select json_group_array(entry.value) from discover_episode as self, json_each(self.data) as entry "
			"where self.seriesid = discover_episode.seriesid and json_extract(entry.value, '$.RecordingRule') = 1 "
			"and json_extract(entry.value, '$.RecordingRuleExt') not like 'DeletedDontRerecord' "
			"order by json_extract(entry.value, '$.StartTime'), json_extract(entry.value, '$.ChannelNumber'))");

		// Remove any series data that was nulled out by the previous operation (json_group_array() will actually return '[]' instead of null).
		execute_non_query(instance, "delete from discover_episode where data is null or data like '[]'");

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

			// Update all of the discovery timestamps to the current time so they are all the same post-discovery
			execute_non_query(instance, "update episode set discovered = ?1", static_cast<int>(time(nullptr)));

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
// discover_episodes_seriesid (local)
//
// Reloads the information about episodes for a specific seriesid
//
// Arguments:
//
//	instance	- SQLite database instance
//	deviceauth	- Device authorization string to use
//	seriesid	- Series identifier to be re-discovered

void discover_episodes_seriesid(sqlite3* instance, char const* deviceauth, char const* seriesid)
{
	if(instance == nullptr) throw std::invalid_argument("instance");
	if(deviceauth == nullptr) throw std::invalid_argument("deviceauth");
	if(seriesid == nullptr) throw std::invalid_argument("seriesid");

	execute_non_query(instance, "begin immediate transaction");

	try {

		// Delete any existing rows in the episode table for this series
		execute_non_query(instance, "delete from episode where seriesid like ?1", seriesid);

		// Rediscover the series episodes, filtering out entries that aren't associated with a recording rule
		// and sort by both the start time and the channel number to ensure the proper ordering
		execute_non_query(instance, "replace into episode select "
			"?2 as seriesid, "
			"cast(strftime('%s', 'now') as integer) as discovered, "
			"nullif(json_group_array(entry.value), '[]') as data "
			"from json_each(json_get('http://api.hdhomerun.com/api/episodes?DeviceAuth=' || ?1 || '&SeriesID=' || ?2)) as entry "
			"where json_extract(entry.value, '$.RecordingRule') = 1 "
			"and json_extract(entry.value, '$.RecordingRuleExt') not like 'DeletedDontRerecord' "
			"order by json_extract(entry.value, '$.StartTime'), json_extract(entry.value, '$.ChannelNumber')",
			deviceauth, seriesid);

		// If no episodes were found or none had a recording rule, the previous query may have returned null
		execute_non_query(instance, "delete from episode where data is null or data like '[]'");

		// Commit the transaction
		execute_non_query(instance, "commit transaction");
	}

	// Rollback the entire transaction on any failure above
	catch(...) { try_execute_non_query(instance, "rollback transaction"); throw; }
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

		// Discover the channel lineups for all available tuner devices; the tuner will return "[]" if there are no channels
		execute_non_query(instance, "insert into discover_lineup select deviceid, cast(strftime('%s', 'now') as integer) as discovered, "
			"json_get(json_extract(device.data, '$.LineupURL') || '?show=demo') as json from device where json_extract(device.data, '$.LineupURL') is not null");

		// This requires a multi-step operation against the lineup table; start a transaction
		execute_non_query(instance, "begin immediate transaction");

		try {

			// Delete any entries in the main lineup table that are no longer present in the data
			if(execute_non_query(instance, "delete from lineup where deviceid not in (select deviceid from discover_lineup)") > 0) changed = true;

			// Insert/replace entries in the main lineup table that are new or different
			if(execute_non_query(instance, "replace into lineup select discover_lineup.* from discover_lineup left outer join lineup using(deviceid) "
				"where coalesce(lineup.data, '') <> coalesce(discover_lineup.data, '')") > 0) changed = true;

			// Remove any lineup data that was nulled out by the previous operation
			execute_non_query(instance, "delete from lineup where data is null or data like '[]'");

			// Update all of the discovery timestamps to the current time so they are all the same post-discovery
			execute_non_query(instance, "update lineup set discovered = ?1", static_cast<int>(time(nullptr)));

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
// discover_listings
//
// Reloads the information about the available listings
//
// Arguments:
//
//	instance	- SQLite database instance
//	deviceauth	- Device authorization string to use

void discover_listings(sqlite3* instance, char const* deviceauth)
{
	bool ignored;
	return discover_listings(instance, deviceauth, ignored);
}

//---------------------------------------------------------------------------
// discover_listings
//
// Reloads the information about the available listings
//
// Arguments:
//
//	instance	- SQLite database instance
//	deviceauth	- Device authorization string to use
//	changed		- Flag indicating if the data has changed

void discover_listings(sqlite3* instance, char const* deviceauth, bool& changed)
{
	sqlite3_stmt*		statement;				// SQL statement to execute
	int					result;					// Result from SQLite function

	if((instance == nullptr) || (deviceauth == nullptr)) return;

	// This operation always causes changes to both the listing and guide tables
	changed = true;

	// As the XMLTV data is processed, a callback method passed to the virtual table
	// will provide the details about the channel elements as they are processed
	std::vector<struct xmltv_channel_element> channels;
	xmltv_onchannel_callback callback = [&](struct xmltv_channel const& channel) -> void {
	
		// The identifier and number strings are required to process the channel entry
		if((channel.id == nullptr) || (channel.number == nullptr)) return;
		channels.emplace_back(xmltv_channel_element{

			std::string(channel.id), 
			std::string(channel.number), 
			(channel.name != nullptr) ? channel.name : std::string(), 
			(channel.altname != nullptr) ? channel.altname : std::string(),
			(channel.network != nullptr) ? channel.network : std::string(),
			(channel.iconsrc != nullptr) ? channel.iconsrc : std::string() 
		});
	};

	// This is a multi-step operation, perform it in the context of a database transaction
	execute_non_query(instance, "begin immediate transaction");
	
	try {

		// Truncate both the listing and guide tables
		execute_non_query(instance, "delete from listing");
		execute_non_query(instance, "delete from guide");

		// Reload the listing table directly from the xmltv virtual table, passing in an onchannel
		// callback pointer to gather the channel information as the data is processed
		auto sql = "insert into listing select "
			"xmltv.channel as channelid, "
			"cast(coalesce(strftime('%s', xmltv_time_to_w3c(xmltv.start)), 0) as integer) as starttime, "
			"cast(coalesce(strftime('%s', xmltv_time_to_w3c(xmltv.stop)), 0) as integer) as endtime, "
			"xmltv.seriesid as seriesid, "
			"xmltv.title as title, "
			"xmltv.subtitle as episodename, "
			"xmltv.desc as synopsis, "
			"xmltv_time_to_year(xmltv.date) as year, "
			"xmltv_time_to_w3c(xmltv.date) as originalairdate, "
			"xmltv.iconsrc as iconurl, "
			"xmltv.programtype as programtype, "
			"get_primary_genre(xmltv.categories) as primarygenre, "
			"xmltv.categories as genres, "
			"xmltv.episodenum as episodenumber, "
			"cast(coalesce(xmltv.isnew, 0) as integer) as isnew, "
			"xmltv.starrating as starrating "
			"from xmltv where xmltv.uri = 'http://api.hdhomerun.com/api/xmltv?DeviceAuth=' || ?1 and onchannel = ?2";

		// Prepare the statement
		result = sqlite3_prepare_v2(instance, sql, -1, &statement, nullptr);
		if(result != SQLITE_OK) throw sqlite_exception(result, sqlite3_errmsg(instance));

		// Bind the query parameters
		result = sqlite3_bind_text(statement, 1, deviceauth, -1, SQLITE_STATIC);
		if(result == SQLITE_OK) result = sqlite3_bind_pointer(statement, 2, &callback, typeid(xmltv_onchannel_callback).name(), nullptr);
		if(result != SQLITE_OK) throw sqlite_exception(result);

		// Execute the query - no result set is expected
		result = sqlite3_step(statement);
		if(result == SQLITE_ROW) throw string_exception(__func__, ": unexpected result set returned from non-query");
		if(result != SQLITE_DONE) throw sqlite_exception(result, sqlite3_errmsg(instance));

		// Finalize the statement
		sqlite3_finalize(statement);

		// Now reload the guide table from the enumerated channel information
		sql = "insert into guide values(?1, ?2, ?3, ?4, ?5, ?6)";

		// Prepare the statement
		result = sqlite3_prepare_v2(instance, sql, -1, &statement, nullptr);
		if(result != SQLITE_OK) throw sqlite_exception(result, sqlite3_errmsg(instance));

		// Iterate over all of the enumerated channels and insert them
		for(auto const& channel : channels) {

			// (Re)bind the query parameters
			result = sqlite3_bind_text(statement, 1, channel.id.c_str(), -1, SQLITE_STATIC);
			if(result == SQLITE_OK) result = sqlite3_bind_text(statement, 2, channel.number.c_str(), -1, SQLITE_STATIC);
			if(result == SQLITE_OK) result = (channel.name.empty() ? sqlite3_bind_null(statement, 3) : 
				sqlite3_bind_text(statement, 3, channel.name.c_str(), -1, SQLITE_STATIC));
			if(result == SQLITE_OK) result = (channel.altname.empty() ? sqlite3_bind_null(statement, 4) : 
				sqlite3_bind_text(statement, 4, channel.altname.c_str(), -1, SQLITE_STATIC));
			if(result == SQLITE_OK) result = (channel.network.empty() ? sqlite3_bind_null(statement, 5) : 
				sqlite3_bind_text(statement, 5, channel.network.c_str(), -1, SQLITE_STATIC));
			if(result == SQLITE_OK) result = (channel.iconsrc.empty() ? sqlite3_bind_null(statement, 6) : 
				sqlite3_bind_text(statement, 6, channel.iconsrc.c_str(), -1, SQLITE_STATIC));
			if(result != SQLITE_OK) throw sqlite_exception(result);

			// Execute the query - no result set is expected
			result = sqlite3_step(statement);
			if(result == SQLITE_ROW) throw string_exception(__func__, ": unexpected result set returned from non-query");
			if(result != SQLITE_DONE) throw sqlite_exception(result, sqlite3_errmsg(instance));

			// Reset the prepared statement so that it can be executed again
			result = sqlite3_reset(statement);
			if(result != SQLITE_OK) throw sqlite_exception(result);
		}

		// Finalize the statement
		sqlite3_finalize(statement);
	
		// Commit the database transaction
		execute_non_query(instance, "commit transaction");
	}

	// Rollback the transaction on any exception
	catch(...) { try_execute_non_query(instance, "rollback transaction"); throw; }
}

//---------------------------------------------------------------------------
// discover_recordingrules
//
// Reloads the information about the available recording rules
//
// Arguments:
//
//	instance	- SQLite database instance
//	deviceauth	- Device authorization string to use

void discover_recordingrules(sqlite3* instance, char const* deviceauth)
{
	bool ignored;
	return discover_recordingrules(instance, deviceauth, ignored);
}

//---------------------------------------------------------------------------
// discover_recordingrules
//
// Reloads the information about the available recording rules
//
// Arguments:
//
//	instance	- SQLite database instance
//	deviceauth	- Device authorization string to use
//	changed		- Flag indicating if the data has changed

void discover_recordingrules(sqlite3* instance, char const* deviceauth, bool& changed)
{
	changed = false;							// Initialize [out] argument

	if(instance == nullptr) throw std::invalid_argument("instance");
	if(deviceauth == nullptr) throw std::invalid_argument("deviceauth");

	// Clone the recordingrule table schema into a temporary table
	execute_non_query(instance, "drop table if exists discover_recordingrule");
	execute_non_query(instance, "create temp table discover_recordingrule as select * from recordingrule limit 0");

	try {

		// Discover the information for the available recording rules
		execute_non_query(instance, "insert into discover_recordingrule select "
			"json_extract(value, '$.RecordingRuleID') as recordingruleid, "
			"cast(strftime('%s', 'now') as integer) as discovered, "
			"json_extract(value, '$.SeriesID') as seriesid, "
			"value as data from json_each(json_get('http://api.hdhomerun.com/api/recording_rules?DeviceAuth=' || ?1))", deviceauth);

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

			// Update all of the discovery timestamps to the current time so they are all the same post-discovery
			execute_non_query(instance, "update recordingrule set discovered = ?1", static_cast<int>(time(nullptr)));

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

	// Create and load a temporary table with the series-level recording information from each storage engine instance
	execute_non_query(instance, "drop table if exists discover_recording");
	execute_non_query(instance, "create temp table discover_recording as "
		"with storage(deviceid, url) as(select deviceid, json_extract(device.data, '$.StorageURL') || '?DisplayGroupID=root' from device where json_extract(device.data, '$.StorageURL') is not null) "
		"select distinct storage.deviceid as deviceid, json_extract(displaygroup.value, '$.SeriesID') as seriesid, "
		"max(cast(json_extract(displaygroup.value, '$.UpdateID') as integer)) as updateid, json_extract(displaygroup.value, '$.EpisodesURL') as episodesurl "
		"from storage, json_each(json_get(storage.url)) as displaygroup "
		"group by deviceid, seriesid, episodesurl");

	try {

		// This requires a multi-step operation against the recording table; start a transaction
		execute_non_query(instance, "begin immediate transaction");

		try {

			// Remove all stale deviceids and/or stale seriesids from the recordings table
			if(execute_non_query(instance, "delete from recording where deviceid not in(select distinct deviceid from discover_recording) or "
				"seriesid not in(select distinct seriesid from discover_recording)") > 0) changed = true;

			// Remove all seriesids with an outdated updateid from the recordings table
			if(execute_non_query(instance, "delete from recording where updateid <> (select updateid from discover_recording "
				"where deviceid like recording.deviceid and seriesid like recording.seriesid)") > 0) changed = true;

			// The update query is easier to do if the matching rows in discover_recording are removed first
			execute_non_query(instance, "delete from discover_recording where exists(select 1 from recording where "
				"recording.deviceid like discover_recording.deviceid and recording.seriesid like discover_recording.seriesid)");

			// Chase the episode URLs for each series that has been added/updated and insert them
			if(execute_non_query(instance, "insert into recording select discover_recording.deviceid as deviceid, "
				"discover_recording.seriesid as seriesid, get_recording_id(json_extract(entry.value, '$.CmdURL')) as recordingid, "
				"discover_recording.updateid as updateid, cast(strftime('%s', 'now') as integer) as discovered, "
				"entry.value as data from discover_recording, json_each(json_get(discover_recording.episodesurl)) as entry") > 0) changed = true;

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
// discover_series_recordings (local)
//
// Reloads the information about the available recordings for a single series
//
// Arguments:
//
//	instance	- SQLite database instance
//	seriesid	- Series identifier to use

static void discover_series_recordings(sqlite3* instance, char const* seriesid)
{
	if(instance == nullptr) throw std::invalid_argument("instance");
	if(seriesid == nullptr) return;

	// Create and load a temporary table with the series-level recording information from each storage engine instance
	execute_non_query(instance, "drop table if exists discover_recording_series");
	execute_non_query(instance, "create temp table discover_recording_series as "
		"with storage(deviceid, url) as(select deviceid, json_extract(device.data, '$.StorageURL') || '?DisplayGroupID=root' from device where json_extract(device.data, '$.StorageURL') is not null) "
		"select distinct storage.deviceid as deviceid, json_extract(displaygroup.value, '$.SeriesID') as seriesid, "
		"json_extract(displaygroup.value, '$.UpdateID') as updateid, json_extract(displaygroup.value, '$.EpisodesURL') as episodesurl "
		"from storage, json_each(json_get(storage.url)) as displaygroup where seriesid like ?1", seriesid);

	try {

		// This requires a multi-step operation against the recording table; start a transaction
		execute_non_query(instance, "begin immediate transaction");

		try {

			// Remove all existing rows from the recording table for the specified series
			execute_non_query(instance, "delete from recording where seriesid like ?1", seriesid);

			// Chase the episode URLs for the series and reload the information about the recordings
			execute_non_query(instance, "insert into recording select discover_recording_series.deviceid as deviceid, "
				"discover_recording_series.seriesid as seriesid, get_recording_id(json_extract(entry.value, '$.CmdURL')) as recordingid, "
				"discover_recording_series.updateid as updateid, cast(strftime('%s', 'now') as integer) as discovered, "
				"entry.value as data from discover_recording_series, json_each(json_get(discover_recording_series.episodesurl)) as entry");

			// Commit the database transaction
			execute_non_query(instance, "commit transaction");
		}

		// Rollback the transaction on any exception
		catch(...) { try_execute_non_query(instance, "rollback transaction"); throw; }

		// Drop the temporary table
		execute_non_query(instance, "drop table discover_recording_series");
	}

	// Drop the temporary table on any exception
	catch(...) { execute_non_query(instance, "drop table discover_recording_series"); throw; }
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
//	namesource		- Flag indicating how to source the channel names
//	callback		- Callback function

void enumerate_channels(sqlite3* instance, bool prependnumbers, bool showdrm, enum channel_name_source namesource, enumerate_channels_callback const& callback)
{
	sqlite3_stmt*				statement;			// SQL statement to execute
	int							result;				// Result from SQLite function
	
	if((instance == nullptr) || (callback == nullptr)) return;

	// channelid | channelname | iconurl | drm
	auto sql = "select "
		"distinct(encode_channel_id(json_extract(entry.value, '$.GuideNumber'))) as channelid, "
		"case when ?1 then json_extract(entry.value, '$.GuideNumber') || ' ' else '' end || "
		"case when ?2 = 1 then coalesce(coalesce(guide.altname, guide.name), json_extract(entry.value, '$.GuideName')) "	// channel_name_source::xmltvaltname
		"     when ?2 = 2 then coalesce(coalesce(guide.network, guide.name), json_extract(entry.value, '$.GuideName')) "	// channel_name_source::xmltvnetwork
		"     when ?2 = 3 then coalesce(json_extract(entry.value, '$.GuideName'), guide.name) "								// channel_name_source::device
		"     else coalesce(guide.name, json_extract(entry.value, '$.GuideName')) end as channelname, "						// channel_name_source::xmltv
		"guide.iconurl as iconurl, "
		"coalesce(json_extract(entry.value, '$.DRM'), 0) as drm "
		"from lineup, json_each(lineup.data) as entry left outer join guide on json_extract(entry.value, '$.GuideNumber') = guide.number "
		"where nullif(json_extract(entry.value, '$.DRM'), ?3) is null "
		"order by channelid";

	result = sqlite3_prepare_v2(instance, sql, -1, &statement, nullptr);
	if(result != SQLITE_OK) throw sqlite_exception(result, sqlite3_errmsg(instance));

	try {

		// Bind the query parameters
		result = sqlite3_bind_int(statement, 1, (prependnumbers) ? 1 : 0);
		if(result == SQLITE_OK) result = sqlite3_bind_int(statement, 2, static_cast<int>(namesource));
		if(result == SQLITE_OK) result = sqlite3_bind_int(statement, 3, (showdrm) ? 1 : 0);
		if(result != SQLITE_OK) throw sqlite_exception(result);

		// Execute the query and iterate over all returned rows
		while(sqlite3_step(statement) == SQLITE_ROW) {

			struct channel item{};
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

void enumerate_channelids(sqlite3* instance, bool showdrm, enumerate_channelids_callback const& callback)
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

			union channelid channelid{};
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

void enumerate_channeltuners(sqlite3* instance, union channelid channelid, enumerate_channeltuners_callback const& callback)
{
	sqlite3_stmt*				statement;			// SQL statement to execute
	int							result;				// Result from SQLite function
	
	if((instance == nullptr) || (callback == nullptr)) return;
	
	// tunerid
	auto sql = "with recursive tuners(deviceid, tunerid) as "
		"(select deviceid, json_extract(device.data, '$.TunerCount') - 1 from device where json_extract(device.data, '$.LineupURL') is not null "
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

void enumerate_demo_channelids(sqlite3* instance, bool showdrm, enumerate_channelids_callback const& callback)
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

			union channelid channelid{};
			channelid.value = static_cast<unsigned int>(sqlite3_column_int(statement, 0));
			
			callback(channelid);
		}
	
		sqlite3_finalize(statement);			// Finalize the SQLite statement
	}

	catch(...) { sqlite3_finalize(statement); throw; }
}

//---------------------------------------------------------------------------
// enumerate_devices_broadcast (local)
//
// Enumerates all of the HDHomeRun devices discovered via broadcast (libhdhomerun)
//
// Arguments:
//
//	callback	- Callback to be invoked for each discovered device

static void enumerate_devices_broadcast(enumerate_devices_callback const& callback)
{
	// Allocate enough heap storage to hold up to 64 enumerated devices on the network
	std::unique_ptr<struct hdhomerun_discover_device_v3_t[]> devices(new struct hdhomerun_discover_device_v3_t[64]);

	// Use the libhdhomerun broadcast discovery mechanism to find all devices on the local network
	int result = hdhomerun_discover_find_devices_custom_v3(0, HDHOMERUN_DEVICE_TYPE_WILDCARD,
		HDHOMERUN_DEVICE_ID_WILDCARD, &devices[0], 64);
	if(result == -1) throw string_exception(__func__, ": hdhomerun_discover_find_devices_custom_v3 failed");

	for(int index = 0; index < result; index++) {

		struct discover_device device;
		memset(&device, 0, sizeof(struct discover_device));

		// Only tuner and storage devices are supported
		if((devices[index].device_type != HDHOMERUN_DEVICE_TYPE_TUNER) && (devices[index].device_type != HDHOMERUN_DEVICE_TYPE_STORAGE)) continue;

		// Only non-legacy devices are supported
		if(devices[index].is_legacy) continue;

		// Only devices with a base URL string are supported
		if(strlen(devices[index].base_url) == 0) continue;

		// Convert the hdhomerun_discover_device_t structure into a discover_device for the caller
		device.deviceid = devices[index].device_id;
		device.storageid = devices[index].storage_id;
		device.baseurl = devices[index].base_url;

		callback(device);
	}
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

void enumerate_device_names(sqlite3* instance, enumerate_device_names_callback const& callback)
{
	sqlite3_stmt*				statement;			// SQL statement to execute
	int							result;				// Result from SQLite function
	
	if((instance == nullptr) || (callback == nullptr)) return;

	// name
	auto sql = "select coalesce(json_extract(data, '$.FriendlyName'), 'unknown') || ' ' || deviceid || "
		"' (version: ' || coalesce(coalesce(json_extract(data, '$.FirmwareVersion'), json_extract(data, '$.Version')), 'unknown') || ')' || "
		"case when coalesce(dvrauthorized, 0) = 1 then ' (DVR authorized)' else '' end as name from device";

	result = sqlite3_prepare_v2(instance, sql, -1, &statement, nullptr);
	if(result != SQLITE_OK) throw sqlite_exception(result, sqlite3_errmsg(instance));

	try {

		// Execute the query and iterate over all returned rows
		while(sqlite3_step(statement) == SQLITE_ROW) {

			struct device_name device_name{};
			device_name.name = reinterpret_cast<char const*>(sqlite3_column_text(statement, 0));
			
			callback(device_name);
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

void enumerate_favorite_channelids(sqlite3* instance, bool showdrm, enumerate_channelids_callback const& callback)
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

			union channelid channelid{};
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

void enumerate_expired_recordingruleids(sqlite3* instance, int expiry, enumerate_recordingruleids_callback const& callback)
{
	sqlite3_stmt*				statement;			// SQL statement to execute
	int							result;				// Result from SQLite function
	
	if((instance == nullptr) || (expiry <= 0) || (callback == nullptr)) return;

	// recordingruleid
	auto sql = "select distinct(recordingruleid) as recordingruleid from recordingrule "
		"where json_extract(data, '$.DateTimeOnly') < (cast(strftime('%s', 'now') as integer) - ?1)";

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
// enumerate_hd_channelids
//
// Enumerates the channels marked as 'HD' in the lineups
//
// Arguments:
//
//	instance	- Database instance
//	showdrm		- Flag to show DRM channels
//	callback	- Callback function

void enumerate_hd_channelids(sqlite3* instance, bool showdrm, enumerate_channelids_callback const& callback)
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

			union channelid channelid{};
			channelid.value = static_cast<unsigned int>(sqlite3_column_int(statement, 0));
			
			callback(channelid);
		}
	
		sqlite3_finalize(statement);			// Finalize the SQLite statement
	}

	catch(...) { sqlite3_finalize(statement); throw; }
}

//---------------------------------------------------------------------------
// enumerate_hevc_channelids
//
// Enumerates the channels marked as HEVC/H.265 in the lineups
//
// Arguments:
//
//	instance	- Database instance
//	showdrm		- Flag to show DRM channels
//	callback	- Callback function

void enumerate_hevc_channelids(sqlite3* instance, bool showdrm, enumerate_channelids_callback const& callback)
{
	sqlite3_stmt*				statement;			// SQL statement to execute
	int							result;				// Result from SQLite function

	if((instance == nullptr) || (callback == nullptr)) return;

	// channelid
	auto sql = "select distinct(encode_channel_id(json_extract(entry.value, '$.GuideNumber'))) as channelid "
		"from lineup, json_each(lineup.data) as entry where lower(json_extract(entry.value, '$.VideoCodec')) in ('hevc', 'h265') "
		"and nullif(json_extract(entry.value, '$.DRM'), ?1) is null";

	result = sqlite3_prepare_v2(instance, sql, -1, &statement, nullptr);
	if(result != SQLITE_OK) throw sqlite_exception(result, sqlite3_errmsg(instance));

	try {

		// Bind the query parameters
		result = sqlite3_bind_int(statement, 1, (showdrm) ? 1 : 0);
		if(result != SQLITE_OK) throw sqlite_exception(result);

		// Execute the query and iterate over all returned rows
		while(sqlite3_step(statement) == SQLITE_ROW) {

			union channelid channelid{};
			channelid.value = static_cast<unsigned int>(sqlite3_column_int(statement, 0));

			callback(channelid);
		}

		sqlite3_finalize(statement);			// Finalize the SQLite statement
	}

	catch(...) { sqlite3_finalize(statement); throw; }
}

//---------------------------------------------------------------------------
// enumerate_listings
//
// Enumerates all the available listings in the database
//
// Arguments:
//
//	instance	- Database instance
//	showdrm		- Flag if DRM channels should be enumerated
//	callback	- Callback function

void enumerate_listings(sqlite3* instance, bool showdrm, int maxdays, enumerate_listings_callback const& callback)
{
	sqlite3_stmt*			statement;				// SQL statement to execute
	int						result;					// Result from SQLite function
	bool					cancel = false;			// Cancellation flag

	if(instance == nullptr) return;

	// If the maximum number of days wasn't provided, use a month as the boundary
	if(maxdays < 0) maxdays = 31;

	// seriesid | title | broadcastid | channelid | starttime | endtime | synopsis | year | iconurl | programtype | genretype | genres | originalairdate | seriesnumber | episodenumber | episodename | isnew | starrating
	auto sql = "with allchannels(number) as "
		"(select distinct(json_extract(entry.value, '$.GuideNumber')) as number from lineup, json_each(lineup.data) as entry where coalesce(json_extract(entry.value, '$.DRM'), 0) = ?1) "
		"select listing.seriesid as seriesid, "
		"listing.title as title, "
		"fnv_hash(encode_channel_id(guide.number), listing.starttime, listing.endtime) as broadcastid, "
		"encode_channel_id(guide.number) as channelid, "
		"listing.starttime as starttime, "
		"listing.endtime as endtime, "
		"listing.synopsis as synopsis, "
		"coalesce(listing.year, 0) as year, "
		"listing.iconurl as iconurl, "
		"listing.programtype as programtype, "
		"case upper(listing.programtype) when 'MV' then 0x10 when 'SP' then 0x40 else (select case when genremap.genretype is not null then genremap.genretype else 0x30 end) end as genretype, "
		"listing.genres as genres, "
		"cast(coalesce(strftime('%s', listing.originalairdate), 0) as integer) as originalairdate, "
		"get_season_number(listing.episodenumber) as seriesnumber, "
		"get_episode_number(listing.episodenumber) as episodenumber, "
		"listing.episodename as episodename, "
		"listing.isnew as isnew, "
		"decode_star_rating(listing.starrating) as starrating "
		"from listing inner join guide on listing.channelid = guide.channelid "
		"inner join allchannels on guide.number = allchannels.number "
		"left outer join genremap on listing.primarygenre like genremap.genre "
		"where (listing.endtime < (cast(strftime('%s', 'now') as integer) + (?2 * 86400)))";

	// Prepare the statement
	result = sqlite3_prepare_v2(instance, sql, -1, &statement, nullptr);
	if(result != SQLITE_OK) throw sqlite_exception(result, sqlite3_errmsg(instance));

	try {

		// Bind the query parameters
		result = sqlite3_bind_int(statement, 1, (showdrm) ? 1 : 0);
		if(result == SQLITE_OK) result = sqlite3_bind_int(statement, 2, maxdays);
		if(result != SQLITE_OK) throw sqlite_exception(result);

		// Execute the SQL statement
		result = sqlite3_step(statement);
		if((result != SQLITE_DONE) && (result != SQLITE_ROW)) throw sqlite_exception(result, sqlite3_errmsg(instance));

		// Process each row returned from the query
		while((result == SQLITE_ROW) && (cancel == false)) {

			struct listing item{};
			item.seriesid = reinterpret_cast<char const*>(sqlite3_column_text(statement, 0));
			item.title = reinterpret_cast<char const*>(sqlite3_column_text(statement, 1));
			item.broadcastid = static_cast<unsigned int>(sqlite3_column_int(statement, 2));
			item.channelid = static_cast<unsigned int>(sqlite3_column_int(statement, 3));
			item.starttime = sqlite3_column_int64(statement, 4);
			item.endtime = sqlite3_column_int64(statement, 5);
			item.synopsis = reinterpret_cast<char const*>(sqlite3_column_text(statement, 6));
			item.year = sqlite3_column_int(statement, 7);
			item.iconurl = reinterpret_cast<char const*>(sqlite3_column_text(statement, 8));
			item.programtype = reinterpret_cast<char const*>(sqlite3_column_text(statement, 9));
			item.genretype = sqlite3_column_int(statement, 10);
			item.genres = reinterpret_cast<char const*>(sqlite3_column_text(statement, 11));
			item.originalairdate = sqlite3_column_int64(statement, 12);
			item.seriesnumber = sqlite3_column_int(statement, 13);
			item.episodenumber = sqlite3_column_int(statement, 14);
			item.episodename = reinterpret_cast<char const*>(sqlite3_column_text(statement, 15));
			item.isnew = (sqlite3_column_int(statement, 16) != 0);
			item.starrating = sqlite3_column_int(statement, 17);

			callback(item, cancel);					// Invoke caller-supplied callback
			result = sqlite3_step(statement);		// Move to the next row of data
		}

		sqlite3_finalize(statement);				// Finalize the SQLite statement
	}

	catch(...) { sqlite3_finalize(statement); throw; }
}

//---------------------------------------------------------------------------
// enumerate_listings
//
// Enumerates the available listings for a channel and time period
//
// Arguments:
//
//	instance	- Database instance
//	showdrm		- Flag if DRM channels should be enumerated
//	channelid	- Channel to be enumerated
//	starttime	- Starting time to be queried
//	endtime		- Ending time to be queried
//	callback	- Callback function

void enumerate_listings(sqlite3* instance, bool showdrm, union channelid channelid, time_t starttime, time_t endtime, enumerate_listings_callback const& callback)
{
	sqlite3_stmt*			statement;				// SQL statement to execute
	int						result;					// Result from SQLite function
	bool					cancel = false;			// Cancellation flag

	if(instance == nullptr) return;

	// seriesid | title | broadcastid | starttime | endtime | synopsis | year | iconurl | programtype | genretype | genres | originalairdate | seriesnumber | episodenumber | episodename | isnew | starrating
	auto sql = "with allchannels(number) as "
		"(select distinct(json_extract(entry.value, '$.GuideNumber')) as number from lineup, json_each(lineup.data) as entry where coalesce(json_extract(entry.value, '$.DRM'), 0) = ?1) "
		"select listing.seriesid as seriesid, "
		"listing.title as title, "
		"fnv_hash(encode_channel_id(guide.number), listing.starttime, listing.endtime) as broadcastid, "
		"listing.starttime as starttime, "
		"listing.endtime as endtime, "
		"listing.synopsis as synopsis, "
		"coalesce(listing.year, 0) as year, "
		"listing.iconurl as iconurl, "
		"listing.programtype as programtype, "
		"case upper(listing.programtype) when 'MV' then 0x10 when 'SP' then 0x40 else (select case when genremap.genretype is not null then genremap.genretype else 0x30 end) end as genretype, "
		"listing.genres as genres, "
		"cast(coalesce(strftime('%s', listing.originalairdate), 0) as integer) as originalairdate, "
		"get_season_number(listing.episodenumber) as seriesnumber, "
		"get_episode_number(listing.episodenumber) as episodenumber, "
		"listing.episodename as episodename, "
		"listing.isnew as isnew, "
		"decode_star_rating(listing.starrating) as starrating "
		"from listing inner join guide on listing.channelid = guide.channelid "
		"inner join allchannels on guide.number = allchannels.number "
		"left outer join genremap on listing.primarygenre like genremap.genre "
		"where guide.number = decode_channel_id(?2) and listing.starttime >= ?3 and listing.endtime <= ?4";

	// Prepare the statement
	result = sqlite3_prepare_v2(instance, sql, -1, &statement, nullptr);
	if(result != SQLITE_OK) throw sqlite_exception(result, sqlite3_errmsg(instance));

	try {

		// Bind the query parameters
		result = sqlite3_bind_int(statement, 1, (showdrm) ? 1 : 0);
		if(result == SQLITE_OK) result = sqlite3_bind_int(statement, 2, channelid.value);
		if(result == SQLITE_OK) result = sqlite3_bind_int(statement, 3, static_cast<int>(starttime));
		if(result == SQLITE_OK) result = sqlite3_bind_int(statement, 4, static_cast<int>(endtime));
		if(result != SQLITE_OK) throw sqlite_exception(result);

		// Execute the SQL statement
		result = sqlite3_step(statement);
		if((result != SQLITE_DONE) && (result != SQLITE_ROW)) throw sqlite_exception(result, sqlite3_errmsg(instance));

		// Process each row returned from the query
		while((result == SQLITE_ROW) && (cancel == false)) {

			struct listing item{};
			item.seriesid = reinterpret_cast<char const*>(sqlite3_column_text(statement, 0));
			item.title = reinterpret_cast<char const*>(sqlite3_column_text(statement, 1));
			item.broadcastid = static_cast<unsigned int>(sqlite3_column_int(statement, 2));
			item.channelid = channelid.value;
			item.starttime = sqlite3_column_int64(statement, 3);
			item.endtime = sqlite3_column_int64(statement, 4);
			item.synopsis = reinterpret_cast<char const*>(sqlite3_column_text(statement, 5));
			item.year = sqlite3_column_int(statement, 6);
			item.iconurl = reinterpret_cast<char const*>(sqlite3_column_text(statement, 7));
			item.programtype = reinterpret_cast<char const*>(sqlite3_column_text(statement, 8));
			item.genretype = sqlite3_column_int(statement, 9);
			item.genres = reinterpret_cast<char const*>(sqlite3_column_text(statement, 10));
			item.originalairdate = sqlite3_column_int64(statement, 11);
			item.seriesnumber = sqlite3_column_int(statement, 12);
			item.episodenumber = sqlite3_column_int(statement, 13);
			item.episodename = reinterpret_cast<char const*>(sqlite3_column_text(statement, 14));
			item.isnew = (sqlite3_column_int(statement, 15) != 0);
			item.starrating = sqlite3_column_int(statement, 16);

			callback(item, cancel);					// Invoke caller-supplied callback
			result = sqlite3_step(statement);		// Move to the next row of data
		}

		sqlite3_finalize(statement);				// Finalize the SQLite statement
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

void enumerate_recordings(sqlite3* instance, enumerate_recordings_callback const& callback)
{
	return enumerate_recordings(instance, false, false, callback);
}

//---------------------------------------------------------------------------
// enumerate_recordings
//
// Enumerates the available recordings
//
// Arguments:
//
//	instance			- Database instance
//	episodeastitle		- Flag to use the episode number in place of the recording title
//	ignorecategories	- Flag to ignore the Category attribute of the recording
//	callback			- Callback function

void enumerate_recordings(sqlite3* instance, bool episodeastitle, bool ignorecategories, enumerate_recordings_callback const& callback)
{
	sqlite3_stmt*				statement;			// SQL statement to execute
	int							result;				// Result from SQLite function
	
	if((instance == nullptr) || (callback == nullptr)) return;

	// recordingid | title | episodename | firstairing | originalairdate | programtype | seriesnumber | episodenumber | year | streamurl | directory | plot | channelname | thumbnailpath | recordingtime | duration | lastposition | channelid
	auto sql = "select recordingid, "
		"case when ?1 then coalesce(json_extract(data, '$.EpisodeNumber'), json_extract(data, '$.Title')) else json_extract(data, '$.Title') end as title, "
		"json_extract(data, '$.EpisodeTitle') as episodename, "
		"coalesce(json_extract(data, '$.FirstAiring'), 0) as firstairing, "
		"coalesce(json_extract(data, '$.OriginalAirdate'), 0) as originalairdate, "
		"substr(json_extract(data, '$.ProgramID'), 1, 2) as programtype, "
		"get_season_number(json_extract(data, '$.EpisodeNumber')) as seriesnumber, "
		"get_episode_number(json_extract(data, '$.EpisodeNumber')) as episodenumber, "
		"cast(strftime('%Y', coalesce(json_extract(data, '$.OriginalAirdate'), 0), 'unixepoch') as integer) as year, "
		"json_extract(data, '$.PlayURL') as streamurl, "
		"case when ?2 or lower(coalesce(json_extract(data, '$.Category'), 'series')) in ('series', 'news', 'audio') then replace(json_extract(data, '$.Title'), '/', '-') else json_extract(data, '$.Category') end as directory, "
		"json_extract(data, '$.Synopsis') as plot, "
		"json_extract(data, '$.ChannelName') as channelname, "
		"json_extract(data, '$.ImageURL') as thumbnailpath, "
		"coalesce(json_extract(data, '$.RecordStartTime'), 0) as recordingtime, "
		"coalesce(json_extract(data, '$.RecordEndTime'), 0) - coalesce(json_extract(data, '$.RecordStartTime'), 0) as duration, "
		"coalesce(json_extract(data, '$.Resume'), 0) as lastposition, "
		"encode_channel_id(json_extract(data, '$.ChannelNumber')) as channelid, "
		"coalesce(json_extract(data, '$.Category'), 'series') as category "
		"from recording";

	result = sqlite3_prepare_v2(instance, sql, -1, &statement, nullptr);
	if(result != SQLITE_OK) throw sqlite_exception(result, sqlite3_errmsg(instance));

	try {

		// Bind the query parameters
		result = sqlite3_bind_int(statement, 1, (episodeastitle) ? 1 : 0);
		if(result == SQLITE_OK) result = sqlite3_bind_int(statement, 2, (ignorecategories) ? 1 : 0);
		if(result != SQLITE_OK) throw sqlite_exception(result);

		// Execute the query and iterate over all returned rows
		result = sqlite3_step(statement);
		while(result == SQLITE_ROW) {

			struct recording item{};
			item.recordingid = reinterpret_cast<char const*>(sqlite3_column_text(statement, 0));
			item.title = reinterpret_cast<char const*>(sqlite3_column_text(statement, 1));
			item.episodename = reinterpret_cast<char const*>(sqlite3_column_text(statement, 2));
			item.firstairing = sqlite3_column_int(statement, 3);
			item.originalairdate = sqlite3_column_int64(statement, 4);
			item.programtype = reinterpret_cast<char const*>(sqlite3_column_text(statement, 5));
			item.seriesnumber = sqlite3_column_int(statement, 6);
			item.episodenumber = sqlite3_column_int(statement, 7);
			item.year = sqlite3_column_int(statement, 8);
			item.streamurl = reinterpret_cast<char const*>(sqlite3_column_text(statement, 9));
			item.directory = reinterpret_cast<char const*>(sqlite3_column_text(statement, 10));
			item.plot = reinterpret_cast<char const*>(sqlite3_column_text(statement, 11));
			item.channelname = reinterpret_cast<char const*>(sqlite3_column_text(statement, 12));
			item.thumbnailpath = reinterpret_cast<char const*>(sqlite3_column_text(statement, 13));
			item.recordingtime = sqlite3_column_int64(statement, 14);
			item.duration = sqlite3_column_int(statement, 15);
			item.lastposition = static_cast<uint32_t>(sqlite3_column_int64(statement, 16));
			item.channelid.value = static_cast<unsigned int>(sqlite3_column_int(statement, 17));
			item.category = reinterpret_cast<char const*>(sqlite3_column_text(statement, 18));

			callback(item);							// Invoke caller-supplied callback
			result = sqlite3_step(statement);		// Move to the next row in the result set
		}
	
		// If the final result of the query was not SQLITE_DONE, something bad happened
		if(result != SQLITE_DONE) throw sqlite_exception(result, sqlite3_errmsg(instance));
			
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

void enumerate_recordingrules(sqlite3* instance, enumerate_recordingrules_callback const& callback)
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
		"coalesce(json_extract(data, '$.StartPadding'), 0) as startpadding, "
		"coalesce(json_extract(data, '$.EndPadding'), 0) as endpadding "
		"from recordingrule left outer join guidenumbers on json_extract(data, '$.ChannelOnly') = guidenumbers.guidenumber";

	result = sqlite3_prepare_v2(instance, sql, -1, &statement, nullptr);
	if(result != SQLITE_OK) throw sqlite_exception(result, sqlite3_errmsg(instance));

	try {

		// Execute the query and iterate over all returned rows
		while(sqlite3_step(statement) == SQLITE_ROW) {

			struct recordingrule item{};
			item.recordingruleid = static_cast<unsigned int>(sqlite3_column_int(statement, 0));
			item.type = static_cast<recordingrule_type>(sqlite3_column_int(statement, 1));
			item.seriesid = reinterpret_cast<char const*>(sqlite3_column_text(statement, 2));
			item.channelid.value = static_cast<unsigned int>(sqlite3_column_int(statement, 3));
			item.recentonly = (sqlite3_column_int(statement, 4) != 0);
			item.afteroriginalairdateonly = sqlite3_column_int64(statement, 5);
			item.datetimeonly = sqlite3_column_int64(statement, 6);
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

void enumerate_sd_channelids(sqlite3* instance, bool showdrm, enumerate_channelids_callback const& callback)
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

			union channelid channelid{};
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
//	deviceauth	- Device authorization string to use
//	title		- Title on which to search
//	callback	- Callback function

void enumerate_series(sqlite3* instance, char const* deviceauth, char const* title, enumerate_series_callback const& callback)
{
	sqlite3_stmt*				statement;			// SQL statement to execute
	int							result;				// Result from SQLite function
	
	if((instance == nullptr) || (deviceauth == nullptr) || (title == nullptr) || (callback == nullptr)) return;

	// title | seriesid
	auto sql = "select json_extract(value, '$.Title') as title, "
		"json_extract(value, '$.SeriesID') as seriesid "
		"from json_each(json_get('http://api.hdhomerun.com/api/search?DeviceAuth=' || ?1 || '&Search=' || url_encode(?2))) "
		"where title like '%' || ?2 || '%'";

	result = sqlite3_prepare_v2(instance, sql, -1, &statement, nullptr);
	if(result != SQLITE_OK) throw sqlite_exception(result, sqlite3_errmsg(instance));

	try {

		// Bind the query parameter(s)
		result = sqlite3_bind_text(statement, 1, deviceauth, -1, SQLITE_STATIC);
		if(result == SQLITE_OK) result = sqlite3_bind_text(statement, 2, title, -1, SQLITE_STATIC);
		if(result != SQLITE_OK) throw sqlite_exception(result);

		// Execute the query and iterate over all returned rows
		while(sqlite3_step(statement) == SQLITE_ROW) {

			struct series item{};
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

void enumerate_timers(sqlite3* instance, int maxdays, enumerate_timers_callback const& callback)
{
	sqlite3_stmt*				statement;			// SQL statement to execute
	int							result;				// Result from SQLite function
	
	if((instance == nullptr) || (callback == nullptr)) return;

	// If the maximum number of days wasn't provided, use a month as the boundary
	if(maxdays < 0) maxdays = 31;

	// recordingruleid | parenttype | timerid | channelid | seriesid | starttime | endtime | title | synopsis | startpadding | endpadding
	auto sql = "with guidenumbers(guidenumber) as (select distinct(json_extract(value, '$.GuideNumber')) as guidenumber from lineup, json_each(lineup.data)), "
		"recorded(programid) as (select json_extract(recording.data, '$.ProgramID') from recording) "
		"select case when json_extract(recordingrule.data, '$.DateTimeOnly') is not null then recordingrule.recordingruleid else "
		"(select recordingruleid from recordingrule where json_extract(recordingrule.data, '$.DateTimeOnly') is null and recordingrule.seriesid = episode.seriesid limit 1) end as recordingruleid, "
		"case when json_extract(recordingrule.data, '$.DateTimeOnly') is not null then 1 else 0 end as parenttype, "
		"fnv_hash(json_extract(value, '$.ProgramID'), coalesce(json_extract(value, '$.OriginalAirdate'), 0), coalesce(json_extract(value, '$.EpisodeNumber'), 0)) as timerid, "
		"case when guidenumbers.guidenumber is null then -1 else encode_channel_id(json_extract(value, '$.ChannelNumber')) end as channelid, "
		"episode.seriesid as seriesid, "
		"min(coalesce(json_extract(value, '$.StartTime'), 0)) as starttime, "
		"min(coalesce(json_extract(value, '$.EndTime'), 0)) as endtime, "
		"json_extract(value, '$.Title') as title, "
		"json_extract(value, '$.Synopsis') as synopsis, "
		"coalesce(case when json_extract(recordingrule.data, '$.DateTimeOnly') is not null then json_extract(recordingrule.data, '$.StartPadding') else "
		"(select json_extract(recordingrule.data, '$.StartPadding') from recordingrule where json_extract(recordingrule.data, '$.DateTimeOnly') is null and recordingrule.seriesid = episode.seriesid limit 1) end, 0) as startpadding, "
		"coalesce(case when json_extract(recordingrule.data, '$.DateTimeOnly') is not null then json_extract(recordingrule.data, '$.EndPadding') else "
		"(select json_extract(recordingrule.data, '$.EndPadding') from recordingrule where json_extract(recordingrule.data, '$.DateTimeOnly') is null and recordingrule.seriesid = episode.seriesid limit 1) end, 0) as endpadding "
		"from episode, json_each(episode.data) "
		"left outer join recordingrule on episode.seriesid = recordingrule.seriesid and json_extract(value, '$.StartTime') = json_extract(recordingrule.data, '$.DateTimeOnly') "
		"left outer join guidenumbers on json_extract(value, '$.ChannelNumber') = guidenumbers.guidenumber "
		"group by recordingruleid, parenttype, timerid, channelid, episode.seriesid, title, synopsis, startpadding, endpadding having "
		"((json_extract(value, '$.RecordingRuleExt') is null) or (json_extract(value, '$.RecordingRuleExt') like 'RecordDuplicate') or "
		"(json_extract(value, '$.RecordingRuleExt') like 'RecordIfNotRecorded' and json_extract(value, '$.ProgramID') not in recorded)) and "
		"(starttime < (cast(strftime('%s', 'now') as integer) + (?1 * 86400)))";

	result = sqlite3_prepare_v2(instance, sql, -1, &statement, nullptr);
	if(result != SQLITE_OK) throw sqlite_exception(result, sqlite3_errmsg(instance));

	try {

		// Bind the query parameter(s)
		result = sqlite3_bind_int(statement, 1, maxdays);
		if(result != SQLITE_OK) throw sqlite_exception(result);

		// Execute the query and iterate over all returned rows
		while(sqlite3_step(statement) == SQLITE_ROW) {

			struct timer item{};
			item.recordingruleid = static_cast<unsigned int>(sqlite3_column_int(statement, 0));
			item.parenttype = static_cast<enum recordingrule_type>(sqlite3_column_int(statement, 1));
			item.timerid = static_cast<unsigned int>(sqlite3_column_int(statement, 2));
			item.channelid.value = static_cast<unsigned int>(sqlite3_column_int(statement, 3));
			item.seriesid = reinterpret_cast<char const*>(sqlite3_column_text(statement, 4));
			item.starttime = sqlite3_column_int64(statement, 5);
			item.endtime = sqlite3_column_int64(statement, 6);
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
// execute_non_query (local)
//
// Executes a database query and returns the number of rows affected
//
// Arguments:
//
//	instance		- Database instance
//	sql				- SQL query to execute
//	parameters		- Parameters to be bound to the query

template<typename... _parameters>
static int execute_non_query(sqlite3* instance, char const* sql, _parameters&&... parameters)
{
	sqlite3_stmt*				statement;			// SQL statement to execute
	int							paramindex = 1;		// Bound parameter index value

	if(instance == nullptr) throw std::invalid_argument("instance");
	if(sql == nullptr) throw std::invalid_argument("sql");

	// Suppress unreferenced local variable warning when there are no parameters to bind
	(void)paramindex;
	
	// Prepare the statement
	int result = sqlite3_prepare_v2(instance, sql, -1, &statement, nullptr);
	if(result != SQLITE_OK) throw sqlite_exception(result, sqlite3_errmsg(instance));

	try {

		// Bind the provided query parameter(s) by unpacking the parameter pack
       int unpack[] = { 0, (static_cast<void>(bind_parameter(statement, paramindex, parameters)), 0) ... };
       (void)unpack;

		// Execute the query; ignore any rows that are returned
		do result = sqlite3_step(statement);
		while(result == SQLITE_ROW);

		// The final result from sqlite3_step should be SQLITE_DONE
		if(result != SQLITE_DONE) throw sqlite_exception(result, sqlite3_errmsg(instance));

		// Finalize the statement
		sqlite3_finalize(statement);

		// Return the number of changes made by the statement
		return sqlite3_changes(instance);
	}

	catch(...) { sqlite3_finalize(statement); throw; }       
}

//---------------------------------------------------------------------------
// execute_scalar_int (local)
//
// Executes a database query and returns a scalar integer result
//
// Arguments:
//
//	instance		- Database instance
//	sql				- SQL query to execute
//	parameters		- Parameters to be bound to the query

template<typename... _parameters>
static int execute_scalar_int(sqlite3* instance, char const* sql, _parameters&&... parameters)
{
	sqlite3_stmt*				statement;			// SQL statement to execute
	int							paramindex = 1;		// Bound parameter index value
	int							value = 0;			// Result from the scalar function

	if(instance == nullptr) throw std::invalid_argument("instance");
	if(sql == nullptr) throw std::invalid_argument("sql");

	// Suppress unreferenced local variable warning when there are no parameters to bind
	(void)paramindex;

	// Prepare the statement
	int result = sqlite3_prepare_v2(instance, sql, -1, &statement, nullptr);
	if(result != SQLITE_OK) throw sqlite_exception(result, sqlite3_errmsg(instance));

	try {

		// Bind the provided query parameter(s) by unpacking the parameter pack
		int unpack[] = { 0, (static_cast<void>(bind_parameter(statement, paramindex, parameters)), 0) ... };
		(void)unpack;

		// Execute the query; only the first row returned will be used
		result = sqlite3_step(statement);

		if(result == SQLITE_ROW) value = sqlite3_column_int(statement, 0);
		else if(result != SQLITE_DONE) throw sqlite_exception(result, sqlite3_errmsg(instance));

		// Finalize the statement
		sqlite3_finalize(statement);

		// Return the resultant value from the scalar query
		return value;
	}

	catch(...) { sqlite3_finalize(statement); throw; }
}

//---------------------------------------------------------------------------
// execute_scalar_int64 (local)
//
// Executes a database query and returns a scalar integer result
//
// Arguments:
//
//	instance		- Database instance
//	sql				- SQL query to execute
//	parameters		- Parameters to be bound to the query

template<typename... _parameters>
static int64_t execute_scalar_int64(sqlite3* instance, char const* sql, _parameters&&... parameters)
{
	sqlite3_stmt*				statement;			// SQL statement to execute
	int							paramindex = 1;		// Bound parameter index value
	int64_t						value = 0;			// Result from the scalar function

	if(instance == nullptr) throw std::invalid_argument("instance");
	if(sql == nullptr) throw std::invalid_argument("sql");

	// Suppress unreferenced local variable warning when there are no parameters to bind
	(void)paramindex;

	// Prepare the statement
	int result = sqlite3_prepare_v2(instance, sql, -1, &statement, nullptr);
	if(result != SQLITE_OK) throw sqlite_exception(result, sqlite3_errmsg(instance));

	try {

		// Bind the provided query parameter(s) by unpacking the parameter pack
		int unpack[] = { 0, (static_cast<void>(bind_parameter(statement, paramindex, parameters)), 0) ... };
		(void)unpack;

		// Execute the query; only the first row returned will be used
		result = sqlite3_step(statement);

		if(result == SQLITE_ROW) value = sqlite3_column_int64(statement, 0);
		else if(result != SQLITE_DONE) throw sqlite_exception(result, sqlite3_errmsg(instance));

		// Finalize the statement
		sqlite3_finalize(statement);

		// Return the resultant value from the scalar query
		return value;
	}

	catch(...) { sqlite3_finalize(statement); throw; }
}

//---------------------------------------------------------------------------
// execute_scalar_string (local)
//
// Executes a database query and returns a scalar string result
//
// Arguments:
//
//	instance		- Database instance
//	sql				- SQL query to execute
//	parameters		- Parameters to be bound to the query

template<typename... _parameters>
static std::string execute_scalar_string(sqlite3* instance, char const* sql, _parameters&&... parameters)
{
	sqlite3_stmt*				statement;			// SQL statement to execute
	int							paramindex = 1;		// Bound parameter index value
	std::string					value;				// Result from the scalar function

	if(instance == nullptr) throw std::invalid_argument("instance");
	if(sql == nullptr) throw std::invalid_argument("sql");

	// Suppress unreferenced local variable warning when there are no parameters to bind
	(void)paramindex;

	// Prepare the statement
	int result = sqlite3_prepare_v2(instance, sql, -1, &statement, nullptr);
	if(result != SQLITE_OK) throw sqlite_exception(result, sqlite3_errmsg(instance));

	try {

		// Bind the provided query parameter(s) by unpacking the parameter pack
		int unpack[] = { 0, (static_cast<void>(bind_parameter(statement, paramindex, parameters)), 0) ... };
		(void)unpack;

		// Execute the query; only the first row returned will be used
		result = sqlite3_step(statement);

		if(result == SQLITE_ROW) {

			char const* ptr = reinterpret_cast<char const*>(sqlite3_column_text(statement, 0));
			if(ptr != nullptr) value.assign(ptr);
		}
		else if(result != SQLITE_DONE) throw sqlite_exception(result, sqlite3_errmsg(instance));

		// Finalize the statement
		sqlite3_finalize(statement);

		// Return the resultant value from the scalar query
		return value;
	}

	catch(...) { sqlite3_finalize(statement); throw; }
}

//---------------------------------------------------------------------------
// find_seriesid
//
// Retrieves the series id associated with a specific channel/time combination
//
// Arguments:
//
//	instance	- Database instance handle
//	channelid	- Channel on which to find the series
//	timestamp	- Time stamp on which to find the series

std::string find_seriesid(sqlite3* instance, union channelid channelid, time_t timestamp)
{
	if(instance == nullptr) return std::string();

	// Use the listings and channel tables to locate a seriesid based on a channel and timestamp
	return execute_scalar_string(instance, "select listing.seriesid from listing inner join guide on listing.channelid = guide.channelid "
		"where guide.number = decode_channel_id(?1) and listing.starttime >= ?2 and ?2 <= listing.endtime", channelid.value, static_cast<int>(timestamp));
}

//---------------------------------------------------------------------------
// find_seriesid
//
// Retrieves the series id associated with a title
//
// Arguments:
//
//	instance	- Database instance handle
//	deviceauth	- Device authorization string to use
//	title		- Title of the series to locate

std::string find_seriesid(sqlite3* instance, char const* deviceauth, char const* title)
{
	if((instance == nullptr) || (deviceauth == nullptr)) return std::string();

	return execute_scalar_string(instance, "select json_extract(value, '$.SeriesID') as seriesid "
		"from json_each(json_get('http://api.hdhomerun.com/api/search?DeviceAuth=' || ?1 || '&Search=' || url_encode(?2))) "
		"where json_extract(value, '$.Title') like ?2 limit 1", deviceauth, title);
}

//---------------------------------------------------------------------------
// generate_discovery_diagnostic_file
//
// Generates a zip file containing all of the discovery information for diagnostic purposes
//
// Arguments:
//
//	instance		- SQLite database instance
//	path			- Location where the diagnostic file will be written

void generate_discovery_diagnostic_file(sqlite3* instance, char const* path)
{
	if(instance == nullptr || path == nullptr) return;

	// TYPE | DEVICEID | DATA
	//
	execute_non_query(instance, "drop table if exists discovery_diagnostics");
	execute_non_query(instance, "create temp table discovery_diagnostics(type text not null, deviceid text, data text)");

	try {

		// HTTP DISCOVERY
		//
		try { execute_non_query(instance, "insert into discovery_diagnostics values('discover', null, "
			"ifnull(json_get('http://api.hdhomerun.com/discover'), 'null'))"); }
		catch(...) { /* DO NOTHING */ }

		// DEVICES
		//
		execute_non_query(instance, "insert into discovery_diagnostics select 'device', device.deviceid, device.data from device");

		// ACCOUNTS
		//
		try { execute_non_query(instance, "insert into discovery_diagnostics select 'account', device.deviceid, "
			"ifnull(json_get('http://api.hdhomerun.com/api/account?DeviceAuth=' || coalesce(url_encode(json_extract(device.data, '$.DeviceAuth')), '')), 'null') "
			"from device where json_extract(device.data, '$.DeviceAuth') is not null"); }
		catch(...) { /* DO NOTHING */ }

		// LINEUPS
		//
		try { execute_non_query(instance, "insert into discovery_diagnostics select 'lineup', device.deviceid, "
			"ifnull(json_get(json_extract(device.data, '$.LineupURL') || '?show=demo'), 'null') "
			"from device where json_extract(device.data, '$.LineupURL') is not null "); }
		catch(...) { /* DO NOTHING */ }

		// RECORDINGS
		//
		try { execute_non_query(instance, "insert into discovery_diagnostics select 'recordings', device.deviceid, "
			"ifnull(json_get(json_extract(device.data, '$.StorageURL') || '?DisplayGroupID=root'), 'null') "
			"from device where json_extract(device.data, '$.StorageURL') is not null"); }
		catch(...) { /* DO NOTHING */ }

		// RECORDED EPISODES
		//
		try { execute_non_query(instance, "insert into discovery_diagnostics "
			"select 'recordings-' || json_extract(entry.value, '$.SeriesID') as seriesid, "
			"discovery_diagnostics.deviceid, ifnull(json_get(json_extract(entry.value, '$.EpisodesURL')), 'null') "
			"from discovery_diagnostics, json_each(discovery_diagnostics.data) as entry where discovery_diagnostics.type like 'recordings' "
			"group by seriesid"); }
		catch(...) { /* DO NOTHING */ }

		// The remaining operations require DVR authorization to function
		std::string authorization = get_authorization_strings(instance, true);
		if(authorization.length() > 0) {

			// RECORDING RULES
			//
			execute_non_query(instance, "insert into discovery_diagnostics select 'recordingrules', null, "
				"ifnull(json_get('http://api.hdhomerun.com/api/recording_rules?DeviceAuth=' || ?1), 'null')", authorization.c_str());

			// RECORDING RULE EPISODES
			//
			try { execute_non_query(instance, "insert into discovery_diagnostics "
				"select 'episodes-' || json_extract(entry.value, '$.SeriesID') as seriesid, null, "
				"ifnull(json_get('http://api.hdhomerun.com/api/episodes?DeviceAuth=' || ?1 || '&SeriesID=' || json_extract(entry.value, '$.SeriesID')), 'null') "
				"from discovery_diagnostics, json_each(discovery_diagnostics.data) as entry where discovery_diagnostics.type like 'recordingrules' "
				"group by seriesid", authorization.c_str()); }
			catch(...) { /* DO NOTHING */ }
		}

		// Remove device authorization codes and e-mail addresses from the generated information
		execute_non_query(instance, "update discovery_diagnostics set data = json_remove(data, '$.DeviceAuth') where type = 'device'");
		execute_non_query(instance, "update discovery_diagnostics set data = json_remove(data, '$.AccountEmail') where type = 'account'");

		// Create the output .zip file as a temporary virtual table [hdhomerundvr-diag-yyyymmdd-hhmmss.zip]
		execute_non_query(instance, "drop table if exists temp.diagnostics_file");
		std::string zipfile = execute_scalar_string(instance, "select ?1 || '/hdhomerundvr-diag-' || strftime('%Y%m%d-%H%M%S') || '.zip'", path);
		std::string sql = "create virtual table temp.diagnostics_file using zipfile('" + zipfile + "')";
		execute_non_query(instance, sql.c_str());

		try {

			// Dump the information collected in the temporary table into the .zip file
			execute_non_query(instance, "insert into temp.diagnostics_file(name, data) "
				"select discovered.type || case when discovered.deviceid is null then '' else '-' || discovered.deviceid end || '.json', "
				"discovered.data from discovery_diagnostics as discovered");

			// Drop the temporary virtual table
			execute_non_query(instance, "drop table temp.diagnostics_file");
		}

		// Drop the temporary virtual table on any exception
		catch(...) { execute_non_query(instance, "drop table temp.diagnostics_file"); throw; }

		// Drop the temporary table
		execute_non_query(instance, "drop table discovery_diagnostics");
	}

	// Drop the temporary table on any exception
	catch(...) { execute_non_query(instance, "drop table discovery_diagnostics"); throw; }
}

//---------------------------------------------------------------------------
// get_authorization_strings
//
// Gets the device authorization string for all available tuners
//
// Arguments:
//
//	instance		- SQLite database instance
//	dvrauthorized	- Flag to only include tuner(s) with DVR authorization

std::string get_authorization_strings(sqlite3* instance, bool dvrauthorized)
{
	if(instance == nullptr) return std::string();

	return execute_scalar_string(instance, "select url_encode(group_concat(json_extract(data, '$.DeviceAuth'), '')) from device "
		"where json_extract(data, '$.DeviceAuth') is not null and coalesce(dvrauthorized, 0) in (1, ?1)", (dvrauthorized) ? 1 : 0);
}

//---------------------------------------------------------------------------
// get_available_storage_space
//
// Gets the total amount of free space on the backend
//
// Arguments:
//
//	instance	- SQLite database instance

struct storage_space get_available_storage_space(sqlite3* instance)
{
	sqlite3_stmt*				statement;				// Database query statement
	struct storage_space		space { 0, 0 };			// Returned amount of total/free space
	int							result;					// Result from SQLite function call

	if(instance == nullptr) return space;

	// Prepare a query to get the sum of all total and available storage space
	auto sql = "select sum(coalesce(json_extract(device.data, '$.TotalSpace'), 0)) as total, "
		"sum(coalesce(json_extract(device.data, '$.FreeSpace'), 0)) as available "
		"from device where json_extract(device.data, '$.StorageURL') is not null";

	result = sqlite3_prepare_v2(instance, sql, -1, &statement, nullptr);
	if(result != SQLITE_OK) throw sqlite_exception(result, sqlite3_errmsg(instance));

	try { 
		
		// Execute the scalar query
		result = sqlite3_step(statement);

		// There should be a single SQLITE_ROW returned from the initial step
		if(result == SQLITE_ROW) {

			space.total = sqlite3_column_int64(statement, 0);
			space.available = sqlite3_column_int64(statement, 1);
		}

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
	if(instance == nullptr) return 0;

	return execute_scalar_int(instance, "select count(distinct(json_extract(value, '$.GuideNumber'))) "
		"from lineup, json_each(lineup.data) where nullif(json_extract(value, '$.DRM'), ?1) is null", (showdrm) ? 1 : 0);
}

//---------------------------------------------------------------------------
// get_discovered
//
// Gets the timestamp of the last discovery for the specified type
//
// Arguments:
//
//	instance	- SQLite database instance
//	type		- Type of discovery operation to interrogate

time_t get_discovered(sqlite3* instance, char const* type)
{
	if((instance == nullptr) || (type == nullptr)) return 0;

	return static_cast<time_t>(execute_scalar_int(instance, "select discovered from discovered where type like ?1", type));
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
	if(instance == nullptr) return 0;

	return execute_scalar_int(instance, "select count(recordingid) from recording");
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
//	flatten			- Flag to omit the directory name ($.DisplayGroupTitle)

std::string get_recording_filename(sqlite3* instance, char const* recordingid, bool flatten)
{
	if((instance == nullptr) || (recordingid == nullptr)) return std::string();

	// Execute a scalar result query to generate the base file name of the recording MPG file; recordings with a 
	// category of movie are in a subdirectory named "Movies" and recordings with a category of 'sport' are in a
	// subdirectory named "Sporting Events".  All other categories use the series name for the subdirectory name
	//
	// HDHomeRun RECORD introduced a new "Filename" attribute that makes this much easier, but leave the old 
	// 'figure it out' method in place as a fallback mechanism for a while ...
	//
	// STANDARD FORMAT  : {"Movies"|"Sporting Events"|Title}/{Title} {EpisodeNumber} {OriginalAirDate} [{StartTime}]
	// FLATTENED FORMAT : {Title} {EpisodeNumber} {OriginalAirDate} [{StartTime}]

	return execute_scalar_string(instance,  "select case when ?1 then '' else case lower(coalesce(json_extract(data, '$.Category'), 'series')) "
		"when 'movie' then 'Movies' when 'sport' then 'Sporting Events' else rtrim(clean_filename(json_extract(data, '$.Title')), ' .') end || '/' end || "
		"case when json_extract(data, '$.Filename') is not null then json_extract(data, '$.Filename') else "
		"clean_filename(json_extract(data, '$.Title')) || ' ' || coalesce(json_extract(data, '$.EpisodeNumber') || ' ', '') || "
		"coalesce(strftime('%Y%m%d', datetime(json_extract(data, '$.OriginalAirdate'), 'unixepoch')) || ' ', '') || "
		"'[' || strftime('%Y%m%d-%H%M', datetime(json_extract(data, '$.StartTime'), 'unixepoch')) || ']' end as filename "
		"from recording where recordingid like ?2 limit 1", (flatten) ? 1 : 0, recordingid);
}

//---------------------------------------------------------------------------
// get_recording_lastposition
//
// Gets the last played position for a specific recording
//
// Arguments:
//
//	instance		- Database instance
//	allowdiscover	- Flag that will allow a refresh of the data via discovery
//	recordingid		- Recording identifier (command url)

uint32_t get_recording_lastposition(sqlite3* instance, bool allowdiscover, char const* recordingid)
{
	sqlite3_stmt*				statement;				// Database query statement
	uint32_t					resume = 0;				// Recording resume position
	time_t						discovered = 0;			// Recording discovery time
	std::string					seriesid;				// Recording series identifier
	int							result;					// Result from SQLite function call

	if(instance == nullptr) return 0;

	// Retrieve the resume position, discovery time, and series identifier for the recording from the database
	auto sql = "select coalesce(json_extract(data, '$.Resume'), 0) as lastposition, discovered, seriesid from recording where recordingid like ?1 limit 1";

	result = sqlite3_prepare_v2(instance, sql, -1, &statement, nullptr);
	if(result != SQLITE_OK) throw sqlite_exception(result, sqlite3_errmsg(instance));

	try {

		// Bind the query parameters
		result = sqlite3_bind_text(statement, 1, recordingid, -1, SQLITE_STATIC);
		if(result != SQLITE_OK) throw sqlite_exception(result);

		// Execute the query; there should be at most one row returned
		if(sqlite3_step(statement) == SQLITE_ROW) {

			resume = static_cast<uint32_t>(sqlite3_column_int64(statement, 0));
			discovered = sqlite3_column_int(statement, 1);

			char const* value = reinterpret_cast<char const*>(sqlite3_column_text(statement, 2));
			if(value != nullptr) seriesid.assign(value);
		}

		sqlite3_finalize(statement);
	}

	catch(...) { sqlite3_finalize(statement); throw; }

	// If the discovery value is zero (no rows returned), or discovery took place less than 30 seconds ago, use the resume value
	if((discovered == 0) || (discovered >= (time(nullptr) - 30))) return resume;

	// The discovery information is stale, perform a discovery for the series to refresh the information
	if(allowdiscover) discover_series_recordings(instance, seriesid.c_str());

	// Retrieve the updated resume position for the recording
	return static_cast<uint32_t>(execute_scalar_int64(instance, "select coalesce(json_extract(data, '$.Resume'), 0) as resume from recording where recordingid like ?1 limit 1", recordingid));
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
	if((instance == nullptr) || (recordingid == nullptr)) return std::string();

	return execute_scalar_string(instance, "select json_extract(data, '$.PlayURL') as streamurl from recording where recordingid like ?1", recordingid);
}

//---------------------------------------------------------------------------
// get_recording_time
//
// Gets the start time for a recording
//
// Arguments:
//
//	instance		- Database instance
//	recordingid		- Recording identifier (command url)

int64_t get_recording_time(sqlite3* instance, char const* recordingid)
{
	if((instance == nullptr) || (recordingid == nullptr)) return 0;

	return execute_scalar_int64(instance, "select coalesce(json_extract(data, '$.RecordStartTime'), 0) from recording where recordingid like ?1", recordingid);
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
	if(instance == nullptr) return 0;

	return execute_scalar_int(instance, "select count(recordingruleid) from recordingrule");
}

//---------------------------------------------------------------------------
// get_recordingrule_seriesid
//
// Gets the series identifier for the specified recording rule
//
// Arguments:
//
//	instance		- Database instance
//	recordingruleid	- Recording rule to get the series identifier for

std::string get_recordingrule_seriesid(sqlite3* instance, unsigned int recordingruleid)
{
	if(instance == nullptr) return std::string();

	return execute_scalar_string(instance, "select json_extract(data, '$.SeriesID') as seriesid from recordingrule where recordingruleid = ?1 limit 1", recordingruleid);
}

//---------------------------------------------------------------------------
// get_storage_stream_urls
//
// Generates a vector<> of possible storage stream URLs for the specified channel
//
// Arguments:
//
//	instance		- Database instance
//	channelid		- Channel for which to get the stream(s)

std::vector<std::string> get_storage_stream_urls(sqlite3* instance, union channelid channelid)
{
	sqlite3_stmt*				statement;				// Database query statement
	std::vector<std::string>	urls;					// Generated vector<> of URLs
	int							result;					// Result from SQLite function call

	if(instance == nullptr) return urls;

	// Retrieve a list of candidate stream URLs for each discovered storage engine device
	auto sql = "select json_extract(device.data, '$.BaseURL') || '/auto/v' || decode_channel_id(?1) || '?ClientID=' || (select clientid from client limit 1) "
		"|| '&SessionID=0x' || hex(randomblob(4)) from device where json_extract(device.data, '$.StorageURL') is not null order by json_extract(device.data, '$.StorageID') asc";

	result = sqlite3_prepare_v2(instance, sql, -1, &statement, nullptr);
	if(result != SQLITE_OK) throw sqlite_exception(result, sqlite3_errmsg(instance));

	try {

		// Bind the query parameters
		result = sqlite3_bind_int(statement, 1, channelid.value);
		if(result != SQLITE_OK) throw sqlite_exception(result);

		// Execute the SQL statement
		result = sqlite3_step(statement);
		if((result != SQLITE_DONE) && (result != SQLITE_ROW)) throw sqlite_exception(result, sqlite3_errmsg(instance));

		// Process each row returned from the query
		while(result == SQLITE_ROW) {

			char const* url = reinterpret_cast<char const*>(sqlite3_column_text(statement, 0));
			if(url != nullptr) urls.emplace_back(url);

			result = sqlite3_step(statement);		// Move to the next row of data
		}

		sqlite3_finalize(statement);				// Finalize the SQLite statement
	}

	catch(...) { sqlite3_finalize(statement); throw; }

	return urls;
}

//---------------------------------------------------------------------------
// get_timer_count
//
// Gets the number of timers in the database
//
// Arguments:
//
//	instance	- SQLite database instance
//	maxdays		- Maximum number of days worth of timers to report

int get_timer_count(sqlite3* instance, int maxdays)
{
	if(instance == nullptr) return 0;

	return execute_scalar_int(instance, "select count(seriesid) from episode, json_each(episode.data) "
		"where (json_extract(value, '$.StartTime') < (cast(strftime('%s', 'now') as integer) + (?1 * 86400)))", 
		(maxdays < 0) ? 31 : maxdays);
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
	if(instance == nullptr) return 0;

	return execute_scalar_int(instance, "select count(deviceid) from device where json_extract(device.data, '$.LineupURL') is not null");
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
	if(instance == nullptr) return false;

	return (execute_scalar_int(instance, "select coalesce((select json_extract(lineupdata.value, '$.Demo') as tuneronly "
		"from lineup, json_each(lineup.data) as lineupdata "
		"where json_extract(lineupdata.value, '$.GuideNumber') = decode_channel_id(?1) and tuneronly is not null limit 1), 0)", channelid.value) != 0);
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

	// Execute a scalar query to generate the URL by matching up the device id and channel against the lineup
	return execute_scalar_string(instance, "select replace(url_remove_query_string(json_extract(lineupdata.value, '$.URL')), 'auto', 'tuner' || ?1) as url "
		"from lineup, json_each(lineup.data) as lineupdata where lineup.deviceid = ?2 "
		"and json_extract(lineupdata.value, '$.GuideNumber') = decode_channel_id(?3)", tunerindex.c_str(), deviceid.c_str(), channelid.value);
}

//---------------------------------------------------------------------------
// has_dvr_authorization
//
// Gets a flag indicating if any devices have DVR service authorization
//
// Arguments:
//
//	instance		- SQLite database instance

bool has_dvr_authorization(sqlite3* instance)
{
	if(instance == nullptr) return false;

	return (execute_scalar_int(instance, "select exists(select deviceid from device where json_extract(data, '$.DeviceAuth') is not null "
		"and coalesce(dvrauthorized, 0) = 1)") != 0);
}

//---------------------------------------------------------------------------
// has_missing_guide_channels
//
// Gets a flag indicating if any channels are missing from the guide data
//
// Arguments:
//
//	instance		- SQLite database instance

bool has_missing_guide_channels(sqlite3* instance)
{
	if(instance == nullptr) return false;

	return (execute_scalar_int(instance, "select 1 where exists(select json_extract(entry.value, '$.GuideNumber') as channelnumber "
		"from lineup, json_each(lineup.data) as entry where channelnumber not in (select distinct(guide.number) from guide))") != 0);
}

//---------------------------------------------------------------------------
// has_storage_engine
//
// Gets a flag indicating if any devices are storage engines
//
// Arguments:
//
//	instance		- SQLite database instance

bool has_storage_engine(sqlite3* instance)
{
	if(instance == nullptr) return false;

	return (execute_scalar_int(instance, "select exists(select deviceid from device where json_extract(device.data, '$.StorageURL') is not null)") != 0);
}

//---------------------------------------------------------------------------
// modify_recordingrule
//
// Modifies an existing recording rule
//
// Arguments:
//
//	instance		- Database instance
//	deviceauth		- Device authorization string to use
//	recordingrule	- Recording rule to be modified with updated information

void modify_recordingrule(sqlite3* instance, char const* deviceauth, struct recordingrule const& recordingrule)
{
	sqlite3_stmt*				statement;			// SQL statement to execute
	int							result;				// Result from SQLite function
	
	if((instance == nullptr) || (deviceauth == nullptr)) return;

	// Update the specific recording rule with the new information provided
	auto sql = "replace into recordingrule "
		"select json_extract(value, '$.RecordingRuleID') as recordingruleid, "
		"cast(strftime('%s', 'now') as integer) as discovered, "
		"json_extract(value, '$.SeriesID') as seriesid, "
		"value as data from "
		"json_each(json_get('http://api.hdhomerun.com/api/recording_rules', 'post', 'DeviceAuth=' || ?1 || '&Cmd=change&RecordingRuleID=' || ?2 || "
		"'&RecentOnly=' || case when ?3 is null then '0' else ?3 end || "
		"'&ChannelOnly=' || case when ?4 is null then '' else decode_channel_id(?4) end || "
		"'&AfterOriginalAirdateOnly=' || case when ?5 is null then '0' else strftime('%s', date(?5, 'unixepoch')) end || "
		"'&StartPadding=' || case when ?6 is null then '' else ?6 end || "
		"'&EndPadding=' || case when ?7 is null then '' else ?7 end))";	
	
	// Prepare the query
	result = sqlite3_prepare_v2(instance, sql, -1, &statement, nullptr);
	if(result != SQLITE_OK) throw sqlite_exception(result, sqlite3_errmsg(instance));

	try {

		// Bind the non-null query parameter(s)
		result = sqlite3_bind_text(statement, 1, deviceauth, -1, SQLITE_STATIC);
		if(result == SQLITE_OK) result = sqlite3_bind_int(statement, 2, recordingrule.recordingruleid);
		if(result == SQLITE_OK) result = (recordingrule.recentonly) ? sqlite3_bind_int(statement, 3, 1) : sqlite3_bind_null(statement, 3);
		if(result == SQLITE_OK) result = (recordingrule.channelid.value != 0) ? sqlite3_bind_int(statement, 4, recordingrule.channelid.value) : sqlite3_bind_null(statement, 4);
		if(result == SQLITE_OK) result = (recordingrule.afteroriginalairdateonly != 0) ? sqlite3_bind_int64(statement, 5, recordingrule.afteroriginalairdateonly) : sqlite3_bind_null(statement, 5);
		if(result == SQLITE_OK) result = (recordingrule.startpadding != 30) ? sqlite3_bind_int(statement, 6, recordingrule.startpadding) : sqlite3_bind_null(statement, 6);
		if(result == SQLITE_OK) result = (recordingrule.endpadding != 30) ? sqlite3_bind_int(statement, 7, recordingrule.endpadding) : sqlite3_bind_null(statement, 7);
		if(result != SQLITE_OK) throw sqlite_exception(result);

		// Execute the query - no result set is expected
		result = sqlite3_step(statement);
		if(result == SQLITE_ROW) throw string_exception(__func__, ": unexpected result set returned from non-query");
		if(result != SQLITE_DONE) throw sqlite_exception(result, sqlite3_errmsg(instance));

		sqlite3_finalize(statement);			// Finalize the SQLite statement
	}

	catch(...) { sqlite3_finalize(statement); throw; }

	// Poke the recording engine(s) after a successful rule change; don't worry about exceptions
	try_execute_non_query(instance, "select json_get(json_extract(data, '$.BaseURL') || '/recording_events.post?sync', 'post') from device "
		"where json_extract(data, '$.StorageURL') is not null");
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

	// Automatically register the in-built database extension library functions and create
	// the database using the provided connection string
	int result = sqlite3_auto_extension(reinterpret_cast<void(*)()>(sqlite3_extension_init));
	if(result == SQLITE_OK) result = sqlite3_open_v2(connstring, &instance, flags, nullptr);
	if(result != SQLITE_OK) throw sqlite_exception(result);

	// set the connection to report extended error codes
	sqlite3_extended_result_codes(instance, -1);

	// set a busy_timeout handler for this connection
	sqlite3_busy_timeout(instance, 5000);
	
	try {

		// switch the database to write-ahead logging
		//
		execute_non_query(instance, "pragma journal_mode=wal");

		// Only execute schema creation steps if the database is being initialized; the caller needs
		// to ensure that this is set for only one connection otherwise locking issues can occur
		if(initialize) {

			// table: client
			//
			// clientid(pk)
			execute_non_query(instance, "create table if not exists client(clientid text primary key not null)");

			// table: device
			// 
			// deviceid(pk) | discovered | dvrauthorized | data
			execute_non_query(instance, "create table if not exists device(deviceid text primary key not null, discovered integer not null, dvrauthorized integer, data text)");

			// table: discovered
			//
			// type(pk) | discovered
			execute_non_query(instance, "create table if not exists discovered(type text primary key not null, discovered integer not null)");

			// table: episode
			//
			// seriesid(pk) | discovered | data
			execute_non_query(instance, "create table if not exists episode(seriesid text primary key not null, discovered integer not null, data text)");

			// table: genremap
			//
			// genre(pk) | genretype
			execute_non_query(instance, "create table if not exists genremap(genre text primary key not null, genretype integer)");

			// table: guide
			//
			// channelid | number | name | iconurl
			execute_non_query(instance, "create table if not exists guide(channelid text not null, number text not null, name text, altname text, network text, iconurl text)");
			execute_non_query(instance, "create index if not exists guide_channelid_number_index on guide(channelid, number)");

			// table: lineup
			//
			// deviceid(pk) | discovered | data
			execute_non_query(instance, "create table if not exists lineup(deviceid text primary key not null, discovered integer not null, data text)");

			// table: listing
			//
			// channelid | starttime | endtime | seriesid | title | episodename | synopsis | year | originalairdate | iconurl | programtype | primarygenre | genres | episodenumber | isnew | starrating
			execute_non_query(instance, "create table if not exists listing(channelid text not null, starttime integer not null, endtime integer not null, seriesid text, title text, "
				"episodename text, synopsis text, year integer, originalairdate text, iconurl text, programtype text, primarygenre text, genres text, episodenumber text, isnew integer, starrating text)");
			execute_non_query(instance, "create index if not exists listing_channelid_starttime_endtime_index on listing(channelid, starttime, endtime)");

			// table: recording
			//
			// deviceid(pk) | seriesid(pk) | recordingid(pk) | updateid | discovered | data
			execute_non_query(instance, "create table if not exists recording(deviceid text not null, seriesid text not null, recordingid text not null, updateid integer not null, "
				"discovered integer not null, data text, primary key(deviceid, seriesid, recordingid))");
			execute_non_query(instance, "create index if not exists recording_updateid_index on recording(updateid)");

			// table: recordingrule
			//
			// recordingruleid(pk) | discovered | data
			execute_non_query(instance, "create table if not exists recordingrule(recordingruleid text primary key not null, discovered integer not null, seriesid text not null, data text)");

			// (re)generate the clientid
			//
			execute_non_query(instance, "delete from client");
			execute_non_query(instance, "insert into client values(uuid())");

			// (re)build the genremap table from the static table
			//
			execute_non_query(instance, "begin immediate transaction");
			execute_non_query(instance, "delete from genremap");

			try {

				sqlite3_stmt*				statement = nullptr;

				genremap_element const*		element = &GENRE_MAPPING_TABLE[0];

				// Use a prepared query to replace the data as quickly as possible ...
				auto sql = "replace into genremap values(?1, ?2)";
				result = sqlite3_prepare_v2(instance, sql, -1, &statement, nullptr);
				if(result != SQLITE_OK) throw sqlite_exception(result, sqlite3_errmsg(instance));

				try {

					while(element->genre != nullptr) {

						// Bind the query parameters
						result = sqlite3_bind_text(statement, 1, element->genre, -1, SQLITE_STATIC);
						if(result == SQLITE_OK) result = sqlite3_bind_int(statement, 2, element->genretype);

						// Execute the statement, no results are expected
						result = sqlite3_step(statement);
						if(result != SQLITE_DONE) throw sqlite_exception(result, sqlite3_errmsg(instance));

						// Reset the statement and move to the next element to be inserted/replaced
						sqlite3_reset(statement);
						++element;
					}

					sqlite3_finalize(statement);			// Finalize the SQLite statement
				}

				catch(...) { sqlite3_finalize(statement); throw; }

				// Commit the transaction against the genremap table
				execute_non_query(instance, "commit transaction");
			}

			catch(...) { execute_non_query(instance, "rollback transaction"); throw; }
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
	char						flag;					// Visibility flag to be set

	if(instance == nullptr) throw std::invalid_argument("instance");

	// Convert the visibility into the character code to send to the tuner(s)
	switch(visibility) {

		case channel_visibility::enabled: flag = '-'; break;
		case channel_visibility::favorite: flag = '+'; break;
		case channel_visibility::disabled: flag = 'x'; break;

		default: throw std::invalid_argument("visibility");
	}

	// Generate the necessary URLs for each tuner that supports the channel
	execute_non_query(instance, "with deviceurls(url) as "
		"(select distinct(json_extract(device.data, '$.BaseURL') || '/lineup.post?favorite=' || ?1 || decode_channel_id(?2)) "
		"from lineup inner join device using(deviceid), json_each(lineup.data) as lineupdata "
		"where json_extract(lineupdata.value, '$.GuideNumber') = decode_channel_id(?2)) "
		"select json_get(url, 'post') from deviceurls", flag, channelid.value);
}

//---------------------------------------------------------------------------
// set_discovered
//
// Sets the timestamp of the last discovery for the specified type
//
// Arguments:
//
//	instance	- SQLite database instance
//	type		- Type of discovery operation to interrogate
//	discovered	- New discovery timestamp to apply for the specified type

void set_discovered(sqlite3* instance, char const* type, time_t discovered)
{
	if((instance == nullptr) || (type == nullptr)) return;

	execute_non_query(instance, "replace into discovered values(?1, ?2)", type, static_cast<int>(discovered));
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

void set_recording_lastposition(sqlite3* instance, char const* recordingid, uint32_t lastposition)
{
	if((instance == nullptr) || (recordingid == nullptr)) return;

	// Update the specified recording on the storage device
	execute_non_query(instance, "select json_get(json_extract(data, '$.CmdURL') || '&cmd=set&Resume=' || ?2, 'post') from recording "
		"where recordingid like ?1 limit 1", recordingid, lastposition);

	// Update the specified recording in the local database
	execute_non_query(instance, "update recording set data = json_set(data, '$.Resume', ?2) where recordingid like ?1", recordingid, lastposition);
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

#pragma warning(pop)
