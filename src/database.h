//---------------------------------------------------------------------------
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

#pragma warning(push, 4)				// Enable maximum compiler warnings

//---------------------------------------------------------------------------
// DATA TYPES
//---------------------------------------------------------------------------

// channelid
//
// Unique identifier for a channel
union channelid {

	struct {
	
		// CCCCCCCCCCCCCCCCCCCC SSSSSSSSSSSS (little endian)
		//
		unsigned int	subchannel : 12;	// Subchannel number (0 - 4095)
		unsigned int	channel : 20;		// Channel number (0 - 1048575)

	} parts;
	
	unsigned int value;						// Complete channel id
};

static_assert(sizeof(channelid) == sizeof(uint32_t), "channelid structure must be same size as a uint32_t");

// channel
//
// Information about a single channel enumerated from the database
struct channel {

	union channelid		channelid;
	char const*			channelname;
	char const*			inputformat;
	char const*			iconurl;
};


// guideentry
//
// Information about a single guide entry enumerated from the database
struct guideentry {

	char const*			seriesid;
	char const*			title;
	unsigned int		channelid;
	time_t				starttime;
	time_t				endtime;
	char const*			synopsis;
	int					year;
	char const*			iconurl;
	int					genretype;
	time_t				originalairdate;
	int					seriesnumber;
	int					episodenumber;
	char const*			episodename;
};

// recording
//
// Information about a single recording enumerated from the database
struct recording {

	char const*			recordingid;
	char const*			title;
	char const*			episodename;
	int					seriesnumber;
	int					episodenumber;
	int					year;
	char const*			streamurl;
	char const*			plot;
	char const*			channelname;
	char const*			thumbnailpath;
	time_t				recordingtime;
	int					duration;
	union channelid		channelid;
};

// recordingrule_type
//
// Type of an existing recording rule
enum recordingrule_type {

	series						= 0,
	datetimeonly				= 1,
};

// recordingrule
//
// Information about a backend recording rule
struct recordingrule {

	unsigned int				recordingruleid;
	enum recordingrule_type		type;
	char const*					seriesid;
	union channelid				channelid;
	bool						recentonly;
	time_t						afteroriginalairdateonly;
	time_t						datetimeonly;
	char const*					title;
	char const*					synopsis;
	unsigned int				startpadding;
	unsigned int				endpadding;
};

// timer
//
// Information about a timer
struct timer {

	unsigned int				recordingruleid;
	enum recordingrule_type		parenttype;
	unsigned int				timerid;
	union channelid				channelid;
	time_t						starttime;
	time_t						endtime;
	char const*					title;
	char const*					synopsis;
	unsigned int				startpadding;
	unsigned int				endpadding;
};

// enumerate_channels_callback
//
// Callback function passed to enumerate_channels
using enumerate_channels_callback = std::function<void(struct channel const& channel)>;

// enumerate_channelids_callback
//
// Callback function passed to enumerate functions that return channelids
using enumerate_channelids_callback = std::function<void(union channelid const& channelid)>;

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
void discover_devices(sqlite3* instance);
void discover_devices(sqlite3* instance, bool& changed);

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
void enumerate_channels(sqlite3* instance, enumerate_channels_callback callback);
void enumerate_channels(sqlite3* instance, bool prependnumbers, enumerate_channels_callback callback);

// enumerate_channelids
//
// Enumerates the available channelids
void enumerate_channelids(sqlite3* instance, enumerate_channelids_callback callback);

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
void enumerate_favorite_channelids(sqlite3* instance, enumerate_channelids_callback callback);

// enumerate_guideentries
//
// Enumerates the available guide entries for a channel and time period
void enumerate_guideentries(sqlite3* instance, union channelid channelid, int maxdays, enumerate_guideentries_callback callback);

// enumerate_hd_channelids
//
// Enumerates channels marked as 'HD' in the lineups
void enumerate_hd_channelids(sqlite3* instance, enumerate_channelids_callback callback);

// enumerate_recordings
//
// Enumerates the available recordings
void enumerate_recordings(sqlite3* instance, enumerate_recordings_callback callback);

// enumerate_recordingrules
//
// Enumerates the available recording rules
void enumerate_recordingrules(sqlite3* instance, enumerate_recordingrules_callback callback);

// enumerate_series_channelids
//
// Enumerates channels associated with a series in the database
void enumerate_series_channelids(sqlite3* instance, char const* seriesid, enumerate_channelids_callback callback);

// enumerate_timers
//
// Enumerates the available timers
void enumerate_timers(sqlite3* instance, int maxdays, enumerate_timers_callback callback);

// execute_non_query
//
// executes a non-query against the database
int execute_non_query(sqlite3* instance, char const* sql);

// find_seriesid
//
// Retrieves the series id associated with a specific channel/time combination
std::string find_seriesid(sqlite3* instance, union channelid channelid, time_t timestamp);

// find_seriesid
//
// Retrieves the series id associated with a title
std::string find_seriesid(sqlite3* instance, char const* title);

// get_available_storage_space
//
// Gets the total amount of free space on the backend
long long get_available_storage_space(sqlite3* instance);

// get_channel_count
//
// Gets the number of available channels in the database
int get_channel_count(sqlite3* instance);

// get_recording_count
//
// Gets the number of available recordings in the database
int get_recording_count(sqlite3* instance);

// get_stream_url
//
// Generates a stream URL for the specified channel
std::string get_stream_url(sqlite3* instance, union channelid channelid);

// get_recordingrule_count
//
// Gets the number of available recording rules in the database
int get_recordingrule_count(sqlite3* instance);

// get_timer_count
//
// Gets the number of timers in the database
int get_timer_count(sqlite3* instance, int maxdays);

// modify_recordingrule
//
// Modifies an existing recording rule
void modify_recordingrule(sqlite3* instance, struct recordingrule const& recordingrule, std::string& seriesid);

// open_database
//
// Opens a handle to the backend SQLite database
sqlite3* open_database(char const* connstring, int flags);

// try_execute_non_query
//
// executes a non-query against the database but eats any exceptions
bool try_execute_non_query(sqlite3* instance, char const* sql);

//---------------------------------------------------------------------------

#pragma warning(pop)

#endif	// __DATABASE_H_
