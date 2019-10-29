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
#include "database.h"

#include <cstddef>

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

//---------------------------------------------------------------------------
// FUNCTION PROTOTYPES
//---------------------------------------------------------------------------

static void bind_parameter(sqlite3_stmt* statement, int& paramindex, const char* value);
static void bind_parameter(sqlite3_stmt* statement, int& paramindex, int value);
static bool discover_devices_broadcast(sqlite3* instance);
static bool discover_devices_http(sqlite3* instance);
static void discover_series_recordings(sqlite3* instance, char const* seriesid);
static void enumerate_devices_broadcast(enumerate_devices_callback callback);
template<typename... _parameters> static int execute_non_query(sqlite3* instance, char const* sql, _parameters&&... parameters);
template<typename... _parameters> static int execute_scalar_int(sqlite3* instance, char const* sql, _parameters&&... parameters);
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
		if(result == SQLITE_OK) result = (recordingrule.afteroriginalairdateonly != 0) ? sqlite3_bind_int(statement, 5, static_cast<int>(recordingrule.afteroriginalairdateonly)) : sqlite3_bind_null(statement, 5);
		if(result == SQLITE_OK) result = (recordingrule.datetimeonly != 0) ? sqlite3_bind_int(statement, 6, static_cast<int>(recordingrule.datetimeonly)) : sqlite3_bind_null(statement, 6);
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

static void bind_parameter(sqlite3_stmt* statement, int& paramindex, int value)
{
	int result = sqlite3_bind_int(statement, paramindex++, value);
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
// discover_guide
//
// Reloads the basic electronic program guide information
//
// Arguments:
//
//	instance	- SQLite database instance
//	deviceauth	- Device authorization string to use

void discover_guide(sqlite3* instance, char const* deviceauth)
{
	bool ignored;
	return discover_guide(instance, deviceauth, ignored);
}

//---------------------------------------------------------------------------
// discover_guide
//
// Reloads the basic electronic program guide information
//
// Arguments:
//
//	instance	- SQLite database instance
//	deviceauth	- Device authorization string to use
//	changed		- Flag indicating if the data has changed

void discover_guide(sqlite3* instance, char const* deviceauth, bool& changed)
{
	changed = false;							// Initialize [out] argument

	if(instance == nullptr) throw std::invalid_argument("instance");
	if(deviceauth == nullptr) throw std::invalid_argument("deviceauth");

	// Clone the guide table schema into a temporary table
	execute_non_query(instance, "drop table if exists discover_guide");
	execute_non_query(instance, "create temp table discover_guide as select * from guide limit 0");

	try {

		// Discover the electronic program guide from the network and insert it into a temporary table
		execute_non_query(instance, "insert into discover_guide select "
			"encode_channel_id(json_extract(discovery.value, '$.GuideNumber')) as channelid, "
			"cast(strftime('%s', 'now') as integer) as discovered, "
			"json_extract(discovery.value, '$.GuideName') as channelname, "
			"json_extract(discovery.value, '$.ImageURL') as iconurl "
			"from json_each(json_get('http://api.hdhomerun.com/api/guide?DeviceAuth=' || ?1)) as discovery", deviceauth);

		// This requires a multi-step operation against the guide table; start a transaction
		execute_non_query(instance, "begin immediate transaction");

		try {

			// Delete any entries in the main guide table that are no longer present in the data
			if(execute_non_query(instance, "delete from guide where channelid not in (select channelid from discover_guide)") > 0) changed = true;

			// Insert/replace entries in the main guide table that are new or different
			if(execute_non_query(instance, "replace into guide select discover_guide.* from discover_guide left outer join guide using(channelid) "
				"where coalesce(guide.channelname, '') <> coalesce(discover_guide.channelname, '') "
				"or coalesce(guide.iconurl, '') <> coalesce(discover_guide.iconurl, '')") > 0) changed = true;

			// Update all of the discovery timestamps to the current time so they are all the same post-discovery
			execute_non_query(instance, "update guide set discovered = ?1", static_cast<int>(time(nullptr)));

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

	// Clone the recording table schema into a temporary table
	execute_non_query(instance, "drop table if exists discover_recording");
	execute_non_query(instance, "create temp table discover_recording as select * from recording limit 0");

	try {

		// Discover the recording information for all available storage devices
		execute_non_query(instance, "insert into discover_recording select get_recording_id(json_extract(entry.value, '$.CmdURL')) as recordingid, "
			"cast(strftime('%s', 'now') as integer) as discovered, json_extract(entry.value, '$.SeriesID') as seriesid, entry.value as data "
			"from device, json_each(json_get(json_extract(device.data, '$.StorageURL'))) as entry where json_extract(device.data, '$.StorageURL') is not null");

		// This requires a multi-step operation against the recording table; start a transaction
		execute_non_query(instance, "begin immediate transaction");

		// Check to verify that all of the discovery times are the same; if any are different an update occurred outside of this
		// main discovery function and a change notification needs to be set since that update may have added or deleted rows
		if(execute_scalar_int(instance, "select count(distinct(discovered)) from recording") > 1) changed = true;
		
		try {

			// Delete any entries in the main recording table that are no longer present in the data
			if(execute_non_query(instance, "delete from recording where recordingid not in (select recordingid from discover_recording)") > 0) changed = true;

			// Insert/replace entries in the main recording table that are new or different
			if(execute_non_query(instance, "replace into recording select discover_recording.* from discover_recording left outer join recording using(recordingid) "
				"where coalesce(recording.data, '') <> coalesce(discover_recording.data, '')") > 0) changed = true;

			// Update all of the discovery timestamps to the current time so they are all the same post-discovery
			execute_non_query(instance, "update recording set discovered = ?1", static_cast<int>(time(nullptr)));

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

	// This is a multi-step operation; begin a database transaction
	execute_non_query(instance, "begin immediate transaction");

	try {

		// Remove all existing rows from the recording table for the specified series
		execute_non_query(instance, "delete from recording where seriesid like ?1", seriesid);
		
		// Reload all recordings for the specified series from all available storage engines
		execute_non_query(instance, "insert into recording select get_recording_id(json_extract(entry.value, '$.CmdURL')) as recordingid, "
			"cast(strftime('%s', 'now') as integer) as discovered, json_extract(entry.value, '$.SeriesID') as seriesid, entry.value as data "
			"from device, json_each(json_get(json_extract(device.data, '$.StorageURL') || '?SeriesID=' || ?1)) as entry "
			"where json_extract(device.data, '$.StorageURL') is not null", seriesid);

		// Commit the transaction
		execute_non_query(instance, "commit transaction");
	}

	// Rollback the database transaction on any thrown exception
	catch(...) { try_execute_non_query(instance, "rollback transaction"); throw; }
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
// enumerate_devices_broadcast (local)
//
// Enumerates all of the HDHomeRun devices discovered via broadcast (libhdhomerun)
//
// Arguments:
//
//	callback	- Callback to be invoked for each discovered device

static void enumerate_devices_broadcast(enumerate_devices_callback callback)
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

void enumerate_device_names(sqlite3* instance, enumerate_device_names_callback callback)
{
	sqlite3_stmt*				statement;			// SQL statement to execute
	int							result;				// Result from SQLite function
	
	if((instance == nullptr) || (callback == nullptr)) return;

	// name
	auto sql = "select coalesce(json_extract(data, '$.FriendlyName'), 'unknown') || ' ' || deviceid || "
		"case when coalesce(dvrauthorized, 0) = 1 then ' (DVR authorized)' else '' end as name from device";

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
// enumerate_guideentries
//
// Enumerates the available guide entries for a channel and time period
//
// Arguments:
//
//	instance		- Database instance
//	deviceauth		- Device authorization string to use
//	channelid		- Channel to be enumerated
//	starttime		- Starting time to be queried
//	endtime			- Ending time to be queried
//	prependnumber	- Flag to prepend the episode number to the episode name
//	callback		- Callback function

void enumerate_guideentries(sqlite3* instance, char const* deviceauth, union channelid channelid, time_t starttime, time_t endtime, bool prependnumber, enumerate_guideentries_callback callback)
{
	sqlite3_stmt*				statement;			// SQL statement to execute
	int							result;				// Result from SQLite function

	if((instance == nullptr) || (deviceauth == nullptr) || (callback == nullptr)) return;

	// Prevent asking for anything older than 4 hours in the past or more than 14 days in the future
	time_t now = time(nullptr);
	starttime = std::max(starttime, now - 14400);			// (60 * 60 * 4) = 4 hours
	endtime = std::min(endtime, now + 1209600);				// (60 * 60 * 24 * 14) = 14 days

	// Use a step value of 7.5 hours to retrieve the EPG data; the backend will return no more than 8 hours
	// of data at a time, this should prevent any holes from forming in the data
	time_t step = 27000;

	// seriesid | title | starttime | endtime | synopsis | year | iconurl | genretype | genres | originalairdate | seriesnumber | episodenumber | episodename
	auto sql = "select json_extract(entry.value, '$.SeriesID') as seriesid, "
		"json_extract(entry.value, '$.Title') as title, "
		"fnv_hash(?2, json_extract(entry.value, '$.StartTime'), json_extract(entry.value, '$.EndTime')) as broadcastid, "
		"json_extract(entry.value, '$.StartTime') as starttime, "
		"json_extract(entry.value, '$.EndTime') as endtime, "
		"json_extract(entry.value, '$.Synopsis') as synopsis, "
		"cast(strftime('%Y', coalesce(json_extract(entry.value, '$.OriginalAirdate'), 0), 'unixepoch') as integer) as year, "
		"json_extract(entry.value, '$.ImageURL') as iconurl, "
		"coalesce((select genretype from genremap where filter like json_extract(entry.value, '$.Filter[0]')), 0) as genretype, "
		"json_extract(entry.value, '$.Filter[0]') as genres, "
		"json_extract(entry.value, '$.OriginalAirdate') as originalairdate, "
		"get_season_number(json_extract(entry.value, '$.EpisodeNumber')) as seriesnumber, "
		"get_episode_number(json_extract(entry.value, '$.EpisodeNumber')) as episodenumber, "
		"case when ?2 then coalesce(json_extract(entry.value, '$.EpisodeNumber') || ' - ', '') else '' end || json_extract(entry.value, '$.EpisodeTitle') as episodename "
		"from json_each((select json_get_aggregate('http://api.hdhomerun.com/api/guide?DeviceAuth=' || ?1 || '&Channel=' || decode_channel_id(?3) || '&Start=' || starttime.value, starttime.value) "
		"from generate_series(?4, ?5, ?6) as starttime)) as entries, json_each(json_extract(entries.value, '$[0].Guide')) as entry";

	result = sqlite3_prepare_v2(instance, sql, -1, &statement, nullptr);
	if(result != SQLITE_OK) throw sqlite_exception(result, sqlite3_errmsg(instance));

	try {

		// Bind the query parameters
		result = sqlite3_bind_text(statement, 1, deviceauth, -1, SQLITE_STATIC);
		if(result == SQLITE_OK) result = sqlite3_bind_int(statement, 2, (prependnumber) ? 1 : 0);
		if(result == SQLITE_OK) result = sqlite3_bind_int(statement, 3, channelid.value);
		if(result == SQLITE_OK) result = sqlite3_bind_int(statement, 4, static_cast<int>(starttime));
		if(result == SQLITE_OK) result = sqlite3_bind_int(statement, 5, static_cast<int>(endtime));
		if(result == SQLITE_OK) result = sqlite3_bind_int(statement, 6, static_cast<int>(step));
		if(result != SQLITE_OK) throw sqlite_exception(result);

		// Execute the SQL statement
		result = sqlite3_step(statement);
		if((result != SQLITE_DONE) && (result != SQLITE_ROW)) throw sqlite_exception(result, sqlite3_errmsg(instance));

		// Process each row returned from the query
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

			callback(item);							// Invoke caller-supplied callback
			result = sqlite3_step(statement);		// Move to the next row of data
		}
	
		sqlite3_finalize(statement);				// Finalize the SQLite statement
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

void enumerate_recordings(sqlite3* instance, bool episodeastitle, bool ignorecategories, enumerate_recordings_callback callback)
{
	sqlite3_stmt*				statement;			// SQL statement to execute
	int							result;				// Result from SQLite function
	
	if((instance == nullptr) || (callback == nullptr)) return;

	// recordingid | title | episodename | firstairing | originalairdate | seriesnumber | episodenumber | year | streamurl | directory | plot | channelname | thumbnailpath | recordingtime | duration | lastposition | channelid
	auto sql = "select recordingid, "
		"case when ?1 then coalesce(json_extract(data, '$.EpisodeNumber'), json_extract(data, '$.Title')) else json_extract(data, '$.Title') end as title, "
		"json_extract(data, '$.EpisodeTitle') as episodename, "
		"coalesce(json_extract(data, '$.FirstAiring'), 0) as firstairing, "
		"coalesce(json_extract(data, '$.OriginalAirdate'), 0) as originalairdate, "
		"get_season_number(json_extract(data, '$.EpisodeNumber')) as seriesnumber, "
		"get_episode_number(json_extract(data, '$.EpisodeNumber')) as episodenumber, "
		"cast(strftime('%Y', coalesce(json_extract(data, '$.OriginalAirdate'), 0), 'unixepoch') as integer) as year, "
		"json_extract(data, '$.PlayURL') as streamurl, "
		"case when ?2 or lower(coalesce(json_extract(data, '$.Category'), 'series')) in ('series', 'news') then json_extract(data, '$.Title') else json_extract(data, '$.Category') end as directory, "
		"json_extract(data, '$.Synopsis') as plot, "
		"json_extract(data, '$.ChannelName') as channelname, "
		"json_extract(data, '$.ImageURL') as thumbnailpath, "
		"coalesce(json_extract(data, '$.RecordStartTime'), 0) as recordingtime, "
		"coalesce(json_extract(data, '$.RecordEndTime'), 0) - coalesce(json_extract(data, '$.RecordStartTime'), 0) as duration, "
		"coalesce(json_extract(data, '$.Resume'), 0) as lastposition, "
		"encode_channel_id(json_extract(data, '$.ChannelNumber')) as channelid "
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

			struct recording item;
			item.recordingid = reinterpret_cast<char const*>(sqlite3_column_text(statement, 0));
			item.title = reinterpret_cast<char const*>(sqlite3_column_text(statement, 1));
			item.episodename = reinterpret_cast<char const*>(sqlite3_column_text(statement, 2));
			item.firstairing = sqlite3_column_int(statement, 3);
			item.originalairdate = sqlite3_column_int(statement, 4);
			item.seriesnumber = sqlite3_column_int(statement, 5);
			item.episodenumber = sqlite3_column_int(statement, 6);
			item.year = sqlite3_column_int(statement, 7);
			item.streamurl = reinterpret_cast<char const*>(sqlite3_column_text(statement, 8));
			item.directory = reinterpret_cast<char const*>(sqlite3_column_text(statement, 9));
			item.plot = reinterpret_cast<char const*>(sqlite3_column_text(statement, 10));
			item.channelname = reinterpret_cast<char const*>(sqlite3_column_text(statement, 11));
			item.thumbnailpath = reinterpret_cast<char const*>(sqlite3_column_text(statement, 12));
			item.recordingtime = sqlite3_column_int(statement, 13);
			item.duration = sqlite3_column_int(statement, 14);
			item.lastposition = sqlite3_column_int(statement, 15);
			item.channelid.value = static_cast<unsigned int>(sqlite3_column_int(statement, 16));

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
//	deviceauth	- Device authorization string to use
//	title		- Title on which to search
//	callback	- Callback function

void enumerate_series(sqlite3* instance, char const* deviceauth, char const* title, enumerate_series_callback callback)
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

	// recordingruleid | parenttype | timerid | channelid | seriesid | starttime | endtime | title | synopsis
	auto sql = "with guidenumbers(guidenumber) as (select distinct(json_extract(value, '$.GuideNumber')) as guidenumber from lineup, json_each(lineup.data)) "
		"select case when json_extract(recordingrule.data, '$.DateTimeOnly') is not null then recordingrule.recordingruleid else "
		"(select recordingruleid from recordingrule where json_extract(recordingrule.data, '$.DateTimeOnly') is null and seriesid = episode.seriesid limit 1) end as recordingruleid, "
		"case when json_extract(recordingrule.data, '$.DateTimeOnly') is not null then 1 else 0 end as parenttype, "
		"fnv_hash(json_extract(value, '$.ProgramID'), json_extract(value, '$.StartTime'), json_extract(value, '$.ChannelNumber')) as timerid, "
		"case when guidenumbers.guidenumber is null then -1 else encode_channel_id(json_extract(value, '$.ChannelNumber')) end as channelid, "
		"episode.seriesid as seriesid, "
		"json_extract(value, '$.StartTime') as starttime, "
		"json_extract(value, '$.EndTime') as endtime, "
		"json_extract(value, '$.Title') as title, "
		"json_extract(value, '$.Synopsis') as synopsis "
		"from episode, json_each(episode.data) "
		"left outer join recordingrule on episode.seriesid = recordingrule.seriesid and json_extract(value, '$.StartTime') = json_extract(recordingrule.data, '$.DateTimeOnly') "
		"left outer join guidenumbers on json_extract(value, '$.ChannelNumber') = guidenumbers.guidenumber "
		"where (starttime < (cast(strftime('%s', 'now') as integer) + (?1 * 86400)))";

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
			item.seriesid = reinterpret_cast<char const*>(sqlite3_column_text(statement, 4));
			item.starttime = static_cast<unsigned int>(sqlite3_column_int(statement, 5));
			item.endtime = static_cast<unsigned int>(sqlite3_column_int(statement, 6));
			item.title = reinterpret_cast<char const*>(sqlite3_column_text(statement, 7));
			item.synopsis = reinterpret_cast<char const*>(sqlite3_column_text(statement, 8));

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
//	deviceauth	- Device authorization string to use
//	channelid	- Channel on which to find the series
//	timestamp	- Time stamp on which to find the series

std::string find_seriesid(sqlite3* instance, char const* deviceauth, union channelid channelid, time_t timestamp)
{
	if((instance == nullptr) || (deviceauth == nullptr)) return std::string();

	// Use the electronic program guide API to locate a seriesid based on a channel and timestamp
	return execute_scalar_string(instance, "select json_extract(json_extract(json_get('http://api.hdhomerun.com/api/guide?DeviceAuth=' || ?1 || "
		"'&Channel=' || decode_channel_id(?2) || '&Start=' || ?3), '$[0].Guide[0]'), '$.SeriesID')", deviceauth, channelid.value, static_cast<int>(timestamp));
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
	// STANDARD FORMAT  : {"Movies"|"Sporting Events"|Title}/{Title} {EpisodeNumber} {OriginalAirDate} [{StartTime}]
	// FLATTENED FORMAT : {Title} {EpisodeNumber} {OriginalAirDate} [{StartTime}]

	return execute_scalar_string(instance,  "select case when ?1 then '' else case lower(coalesce(json_extract(data, '$.Category'), 'series')) "
		"when 'movie' then 'Movies' when 'sport' then 'Sporting Events' else rtrim(clean_filename(json_extract(data, '$.Title')), ' .') end || '/' end || "
		"clean_filename(json_extract(data, '$.Title')) || ' ' || coalesce(json_extract(data, '$.EpisodeNumber') || ' ', '') || "
		"coalesce(strftime('%Y%m%d', datetime(json_extract(data, '$.OriginalAirdate'), 'unixepoch')) || ' ', '') || "
		"'[' || strftime('%Y%m%d-%H%M', datetime(json_extract(data, '$.StartTime'), 'unixepoch')) || ']' as filename "
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
//	recordingid		- Recording identifier (command url)

int get_recording_lastposition(sqlite3* instance, char const* recordingid)
{
	sqlite3_stmt*				statement;				// Database query statement
	int							resume = 0;				// Recording resume position
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

			resume = sqlite3_column_int(statement, 0);
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
	discover_series_recordings(instance, seriesid.c_str());

	// Retrieve the updated resume position for the recording
	return execute_scalar_int(instance, "select coalesce(json_extract(data, '$.Resume'), 0) as resume from recording where recordingid like ?1 limit 1", recordingid);
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
	if(instance == nullptr) return std::string();

	return execute_scalar_string(instance, "select json_extract(device.data, '$.BaseURL') || '/auto/v' || decode_channel_id(?1) || "
		"'?ClientID=' || (select clientid from client limit 1) || '&SessionID=0x' || hex(randomblob(4)) from device "
		"where json_extract(device.data, '$.StorageURL') is not null limit 1", channelid.value);
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
	return execute_scalar_string(instance, "select replace(json_extract(lineupdata.value, '$.URL'), 'auto', 'tuner' || ?1) as url "
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
		"'&RecentOnly=' || case when ?3 is null then '' else ?3 end || "
		"'&ChannelOnly=' || case when ?4 is null then '' else decode_channel_id(?4) end || "
		"'&AfterOriginalAirdateOnly=' || case when ?5 is null then '' else strftime('%s', date(?5, 'unixepoch')) end || "
		"'&StartPadding=' || case when ?6 is null then '30' else ?6 end || "
		"'&EndPadding=' || case when ?7 is null then '30' else ?7 end))";

	// Prepare the query
	result = sqlite3_prepare_v2(instance, sql, -1, &statement, nullptr);
	if(result != SQLITE_OK) throw sqlite_exception(result, sqlite3_errmsg(instance));

	try {

		// Bind the non-null query parameter(s)
		result = sqlite3_bind_text(statement, 1, deviceauth, -1, SQLITE_STATIC);
		if(result == SQLITE_OK) result = sqlite3_bind_int(statement, 2, recordingrule.recordingruleid);
		if(result == SQLITE_OK) result = (recordingrule.recentonly) ? sqlite3_bind_int(statement, 3, 1) : sqlite3_bind_null(statement, 3);
		if(result == SQLITE_OK) result = (recordingrule.channelid.value != 0) ? sqlite3_bind_int(statement, 4, recordingrule.channelid.value) : sqlite3_bind_null(statement, 4);
		if(result == SQLITE_OK) result = (recordingrule.afteroriginalairdateonly != 0) ? sqlite3_bind_int(statement, 5, static_cast<int>(recordingrule.afteroriginalairdateonly)) : sqlite3_bind_null(statement, 5);
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

			// table: lineup
			//
			// deviceid(pk) | discovered | data
			execute_non_query(instance, "create table if not exists lineup(deviceid text primary key not null, discovered integer not null, data text)");

			// table: recording
			//
			// recordingid(pk) | discovered | seriesid | data
			execute_non_query(instance, "create table if not exists recording(recordingid text primary key not null, discovered integer not null, seriesid text not null, data text)");
			execute_non_query(instance, "create index if not exists recording_seriesid_index on recording(seriesid)");

			// table: guide
			//
			// channelid(pk) | discovered | channelname | iconurl
			execute_non_query(instance, "create table if not exists guide(channelid integer primary key not null, discovered integer not null, channelname text, iconurl text)");

			// table: recordingrule
			//
			// recordingruleid(pk) | discovered | data
			execute_non_query(instance, "create table if not exists recordingrule(recordingruleid text primary key not null, discovered integer not null, seriesid text not null, data text)");

			// table: episode
			//
			// seriesid(pk) | discovered | data
			execute_non_query(instance, "create table if not exists episode(seriesid text primary key not null, discovered integer not null, data text)");

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
	if((instance == nullptr) || (recordingid == nullptr)) return;

	// Kodi will send a position value of -1 if the recording ended by playing it to the end
	if(lastposition < 0) lastposition = 0;

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
