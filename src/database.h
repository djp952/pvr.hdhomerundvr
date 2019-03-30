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

#ifndef __DATABASE_H_
#define __DATABASE_H_
#pragma once

#include <functional>
#include <memory>
#include <mutex>
#include <queue>
#include <string>
#include <vector>

#include <sqlite3.h>

#include "dbtypes.h"
#include "scalar_condition.h"

#pragma warning(push, 4)

//---------------------------------------------------------------------------
// DATA TYPES
//---------------------------------------------------------------------------

// enumerate_channels_callback
//
// Callback function passed to enumerate_channels
using enumerate_channels_callback = std::function<void(struct channel const& channel)>;

// enumerate_channelids_callback
//
// Callback function passed to enumerate functions that return channelids
using enumerate_channelids_callback = std::function<void(union channelid const& channelid)>;

// enumerate_channeltuners_callback
//
// Callback function passed to enumerate_channeltuners
using enumerate_channeltuners_callback = std::function<void(char const* tuner)>;

// enumerate_device_names_callback
//
// Callback function passed to enumerate_device_names
using enumerate_device_names_callback = std::function<void(struct device_name const& device_name)>;

// enumerate_guideentries_callback
//
// Callback function passed to enumerate_guideentries
using enumerate_guideentries_callback = std::function<void(struct guideentry const& guideentry)>;

// enumerate_recordings_callback
//
// Callback function passed to enumerate_recordings
using enumerate_recordings_callback = std::function<void(struct recording const& recording)>;

// enumerate_recordingrules_callback
//
// Callback function passed to enumerate_recordingrules
using enumerate_recordingrules_callback = std::function<void(struct recordingrule const& recording)>;

// enumerate_recordingruleids_callback
//
// Callback function passed to enumerate functions that return recordingruleids
using enumerate_recordingruleids_callback = std::function<void(unsigned int const& recordingruleid)>;

// enumerate_series_callback
//
// Callback function passed to enumerate series information
using enumerate_series_callback = std::function<void(struct series const& series)>;

// enumerate_timers_callback
//
// Callback function passed to enumerate_timers
using enumerate_timers_callback = std::function<void(struct timer const& timer)>;

//---------------------------------------------------------------------------
// connectionpool
//
// Implements a connection pool for the SQLite database connections

class connectionpool
{
public:

	// Instance Constructor
	//
	connectionpool(char const* connstr, int flags);

	// Destructor
	//
	~connectionpool();

	//-----------------------------------------------------------------------
	// Member Functions

	// acquire
	//
	// Acquires a connection from the pool, creating a new one as necessary
	sqlite3* acquire(void);

	// release
	//
	// Releases a previously acquired connection back into the pool
	void release(sqlite3* handle);

	//-----------------------------------------------------------------------
	// Type Declarations

	// handle
	//
	// RAII class to acquire and release connections from the pool
	class handle
	{
	public:

		// Constructor / Destructor
		//
		handle(std::shared_ptr<connectionpool> const& pool) : m_pool(pool), m_handle(pool->acquire()) { }
		~handle() { m_pool->release(m_handle); }

		// sqlite3* type conversion operator
		//
		operator sqlite3*(void) const { return m_handle; }

	private:

		handle(handle const&)=delete;
		handle& operator=(handle const&)=delete;

		// m_pool
		//
		// Shared pointer to the parent connection pool
		std::shared_ptr<connectionpool> const m_pool;

		// m_handle
		//
		// SQLite handle acquired from the pool
		sqlite3* m_handle;
	};

private:

	connectionpool(connectionpool const&)=delete;
	connectionpool& operator=(connectionpool const&)=delete;

	//-----------------------------------------------------------------------
	// Member Variables
	
	std::string	const			m_connstr;			// Connection string
	int	const					m_flags;			// Connection flags
	std::vector<sqlite3*>		m_connections;		// All active connections
	std::queue<sqlite3*>		m_queue;			// Queue of unused connection
	mutable std::mutex			m_lock;				// Synchronization object
};

//---------------------------------------------------------------------------
// FUNCTION PROTOTYPES
//---------------------------------------------------------------------------

// add_recordingrule
//
// Adds a new recording rule to the database
void add_recordingrule(sqlite3* instance, struct recordingrule const& recordingrule);

// clear_authorization_strings
//
// Clears stale device authorization string from all available tuners
void clear_authorization_strings(sqlite3* instance, int expiry);

// clear_database
//
// Clears all discovery data out of the database
void clear_database(sqlite3* instance);

// close_database
//
// Creates a SQLite database instance handle
void close_database(sqlite3* instance);

// delete_recording
//
// Deletes a recording from the database instance
void delete_recording(sqlite3* instance, char const* recordingid, bool rerecord);

// delete_recordingrule
//
// Deletes a recording rule from the database instance
void delete_recordingrule(sqlite3* instance, unsigned int recordingruleid);

// discover_devices
//
// Reloads the information about the available devices
void discover_devices(sqlite3* instance, bool usebroadcast);
void discover_devices(sqlite3* instance, bool usebroadcast, bool& changed);

// discover_episodes
//
// Reloads the information about episodes associated with a recording rule
void discover_episodes(sqlite3* instance);
void discover_episodes(sqlite3* instance, bool& changed);

// discover_guide
//
// Reloads the electronic program guide data
void discover_guide(sqlite3* instance);
void discover_guide(sqlite3* instance, bool& changed);

// discover_lineups
//
// Reloads the information about the available channels
void discover_lineups(sqlite3* instance);
void discover_lineups(sqlite3* instance, bool& changed);

// discover_recordingrules
//
// Reloads the information about the available recording rules
void discover_recordingrules(sqlite3* instance);
void discover_recordingrules(sqlite3* instance, bool& changed);

// discover_recordings
//
// Reloads the information about the available recordings
void discover_recordings(sqlite3* instance);
void discover_recordings(sqlite3* instance, bool& changed);

// enumerate_channels
//
// Enumerates the available channels
void enumerate_channels(sqlite3* instance, bool prependnumbers, bool showdrm, bool lineupnames, enumerate_channels_callback callback);

// enumerate_channelids
//
// Enumerates the available channelids
void enumerate_channelids(sqlite3* instance, bool showdrm, enumerate_channelids_callback callback);

// enumerate_channeltuners
//
// Enumerates the tuners that can tune a specific channel
void enumerate_channeltuners(sqlite3* instance, union channelid channelid, enumerate_channeltuners_callback callback);

// enumerate_demo_channelids
//
// Enumerates channels marked as 'Demo' in the lineups
void enumerate_demo_channelids(sqlite3* instance, bool showdrm, enumerate_channelids_callback callback);

// enumerate_device_names
//
// Enumerates the available device names
void enumerate_device_names(sqlite3* instance, enumerate_device_names_callback callback);

// enumerate_episode_channelids
//
// Enumerates all channels associated with any series episodes
void enumerate_episode_channelids(sqlite3* instance, enumerate_channelids_callback callback);

// enumerate_expired_recordingruleids
//
// Enumerates all recordingruleids that have expired
void enumerate_expired_recordingruleids(sqlite3* instance, int expiry, enumerate_recordingruleids_callback callback);

// enumerate_favorite_channelids
//
// Enumerates channels marked as 'Favorite' in the lineups
void enumerate_favorite_channelids(sqlite3* instance, bool showdrm, enumerate_channelids_callback callback);

// enumerate_guideentries
//
// Enumerates the available guide entries for a channel and time period
void enumerate_guideentries(sqlite3* instance, union channelid channelid, time_t starttime, time_t endtime, bool prependnumber, enumerate_guideentries_callback callback);

// enumerate_hd_channelids
//
// Enumerates channels marked as 'HD' in the lineups
void enumerate_hd_channelids(sqlite3* instance, bool showdrm, enumerate_channelids_callback callback);

// enumerate_recordings
//
// Enumerates the available recordings
void enumerate_recordings(sqlite3* instance, enumerate_recordings_callback callback);
void enumerate_recordings(sqlite3* instance, bool episodeastitle, enumerate_recordings_callback callback);

// enumerate_recordingrules
//
// Enumerates the available recording rules
void enumerate_recordingrules(sqlite3* instance, enumerate_recordingrules_callback callback);

// enumerate_sd_channelids
//
// Enumerates channels not marked as 'HD' in the lineups
void enumerate_sd_channelids(sqlite3* instance, bool showdrm, enumerate_channelids_callback callback);

// enumerate_series
//
// Enumerates series based on a title matching search
void enumerate_series(sqlite3* instance, char const* title, enumerate_series_callback callback);

// enumerate_timers
//
// Enumerates the available timers
void enumerate_timers(sqlite3* instance, int maxdays, enumerate_timers_callback callback);

// find_seriesid
//
// Retrieves the series id associated with a specific channel/time combination
std::string find_seriesid(sqlite3* instance, union channelid channelid, time_t timestamp);

// find_seriesid
//
// Retrieves the series id associated with a title
std::string find_seriesid(sqlite3* instance, char const* title);

// get_authorization_strings
//
// Gets the device authorization string for all available tuners
std::string get_authorization_strings(sqlite3* instance, bool dvrauthorized);

// get_available_storage_space
//
// Gets the total amount of free space on the backend
long long get_available_storage_space(sqlite3* instance);

// get_channel_count
//
// Gets the number of available channels in the database
int get_channel_count(sqlite3* instance, bool showdrm);

// get_recording_count
//
// Gets the number of available recordings in the database
int get_recording_count(sqlite3* instance);

// get_recording_filename
//
// Generates the filename for a recording
std::string get_recording_filename(sqlite3* instance, char const* recordingid, bool flatten);

// get_recording_lastposition
//
// Gets the last played position for a specific recording
int get_recording_lastposition(sqlite3* instance, char const* recordingid);

// get_recording_stream_url
//
// Gets the playback URL for a recording
std::string get_recording_stream_url(sqlite3* instance, char const* recordingid);

// get_recordingrule_count
//
// Gets the number of available recording rules in the database
int get_recordingrule_count(sqlite3* instance);

// get_stream_url
//
// Generates a stream URL for the specified channel
std::string get_stream_url(sqlite3* instance, union channelid channelid);

// get_timer_count
//
// Gets the number of timers in the database
int get_timer_count(sqlite3* instance, int maxdays);

// get_tuner_count
//
// Gets the number of tuner devices listed in the database
int get_tuner_count(sqlite3* instance);

// get_tuner_direct_channel_flag
//
// Gets a flag indicating if a channel can only be streamed directly from a tuner device
bool get_tuner_direct_channel_flag(sqlite3* instance, union channelid channelid);

// get_tuner_stream_url
//
// Generates a stream URL for the specified channel on the specified tuner
std::string get_tuner_stream_url(sqlite3* instance, char const* tunerid, union channelid channelid);

// modify_recordingrule
//
// Modifies an existing recording rule
void modify_recordingrule(sqlite3* instance, struct recordingrule const& recordingrule, std::string& seriesid);

// open_database
//
// Opens a handle to the backend SQLite database
sqlite3* open_database(char const* connstring, int flags);
sqlite3* open_database(char const* connstring, int flags, bool initialize);

// set_channel_visibility
//
// Sets the visibility of a channel on all known tuner devices
void set_channel_visibility(sqlite3* instance, union channelid channelid, enum channel_visibility visibility);

// set_recording_lastposition
//
// Sets the last played position for a specific recording
void set_recording_lastposition(sqlite3* instance, char const* recordingid, int lastposition);

// try_execute_non_query
//
// executes a non-query against the database but eats any exceptions
bool try_execute_non_query(sqlite3* instance, char const* sql);

//---------------------------------------------------------------------------

#pragma warning(pop)

#endif	// __DATABASE_H_
