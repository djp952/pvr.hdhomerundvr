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

#include "stdafx.h"

#include <algorithm>
#include <chrono>
#include <functional>
#include <list>
#include <memory>
#include <mutex>
#include <pthread.h>
#include <string>
#include <strings.h>
#include <sstream>
#include <vector>

#ifdef __ANDROID__
#include <android/log.h>
#include <sys/prctl.h>
#endif

#include <xbmc_addon_dll.h>
#include <xbmc_pvr_dll.h>
#include <version.h>

#include <libXBMC_addon.h>
#include <libKODI_guilib.h>
#include <libXBMC_pvr.h>

#include "database.h"
#include "dvrstream.h"
#include "hdhr.h"
#include "scalar_condition.h"
#include "scheduler.h"
#include "string_exception.h"

#pragma warning(push, 4)				// Enable maximum compiler warnings

//---------------------------------------------------------------------------
// MACROS
//---------------------------------------------------------------------------

// MENUHOOK_XXXXXX
//
// Menu hook identifiers
#define MENUHOOK_RECORD_DELETENORERECORD				1
#define MENUHOOK_RECORD_DELETERERECORD					2
#define MENUHOOK_SETTING_TRIGGERDEVICEDISCOVERY			3
#define MENUHOOK_SETTING_TRIGGERLINEUPDISCOVERY			4
#define MENUHOOK_SETTING_TRIGGERGUIDEDISCOVERY			5
#define MENUHOOK_SETTING_TRIGGERRECORDINGDISCOVERY		6
#define MENUHOOK_SETTING_TRIGGERRECORDINGRULEDISCOVERY	7
#define MENUHOOK_SETTING_RESETDATABASE					8
#define MENUHOOK_CHANNEL_DISABLE						9
#define MENUHOOK_CHANNEL_ADDFAVORITE					10
#define MENUHOOK_CHANNEL_REMOVEFAVORITE					11
#define MENUHOOK_SETTING_SHOWDEVICENAMES				12

//---------------------------------------------------------------------------
// FUNCTION PROTOTYPES
//---------------------------------------------------------------------------

// Exception helpers
//
static void handle_generalexception(char const* function);
template<typename _result> static _result handle_generalexception(char const* function, _result result);
static void handle_stdexception(char const* function, std::exception const& ex);
template<typename _result> static _result handle_stdexception(char const* function, std::exception const& ex, _result result);

// Log helpers
//
template<typename... _args> static void log_debug(_args&&... args);
template<typename... _args> static void log_error(_args&&... args);
template<typename... _args> static void log_info(_args&&... args);
template<typename... _args>	static void log_message(ADDON::addon_log_t level, _args&&... args);
template<typename... _args> static void log_notice(_args&&... args);

// Scheduled Tasks
//
static void discover_devices_task(scalar_condition<bool> const& cancel);
static void discover_episodes_task(scalar_condition<bool> const& cancel);
static void discover_guide_task(scalar_condition<bool> const& cancel);
static void discover_lineups_task(scalar_condition<bool> const& cancel);
static void discover_recordingrules_task(scalar_condition<bool> const& cancel);
static void discover_recordings_task(scalar_condition<bool> const& cancel);
static void discover_startup_task(scalar_condition<bool> const& cancel);

//---------------------------------------------------------------------------
// TYPE DECLARATIONS
//---------------------------------------------------------------------------

// duplicate_prevention
//
// Defines the identifiers for series duplicate prevention values
enum duplicate_prevention {

	none					= 0,
	newonly					= 1,
	recentonly				= 2,
};

// timer_type
//
// Defines the identifiers for the various timer types (1-based)
enum timer_type {

	seriesrule				= 1,
	datetimeonlyrule		= 2,
	epgseriesrule			= 3,
	epgdatetimeonlyrule		= 4,
	seriestimer				= 5,
	datetimeonlytimer		= 6,
};

// addon_settings
//
// Defines all of the configurable addon settings
struct addon_settings {

	// pause_discovery_while_streaming
	//
	// Flag to pause the discovery activities while a live stream is active
	bool pause_discovery_while_streaming;

	// prepend_channel_numbers
	//
	// Flag to include the channel number in the channel name
	bool prepend_channel_numbers;

	// use_episode_number_as_title
	//
	// Flag to include the episode number in recording titles
	bool use_episode_number_as_title;

	// use_backend_genre_strings
	//
	// Flag to use the backend provided genre strings instead of mapping them
	bool use_backend_genre_strings;

	// show_drm_protected_channels
	//
	// Flag indicating that DRM channels should be shown to the user
	bool show_drm_protected_channels;

	// delete_datetime_rules_after
	//
	// Amount of time (seconds) after which an expired date/time rule is deleted
	int delete_datetime_rules_after;

	// use_broadcast_device_discovery
	//
	// Flag to discover devices via local network broadcast instead of HTTP
	bool use_broadcast_device_discovery;

	// discover_devices_interval
	//
	// Interval at which the local network device discovery will occur (seconds)
	int discover_devices_interval;

	// discover_episodes_interval
	//
	// Interval at which the recording rule episodes discovery will occur (seconds)
	int discover_episodes_interval;

	// discover_guide_interval
	//
	// Interval at which the electronic program guide discovery will occur (seconds)
	int discover_guide_interval;

	// discover_lineups_interval
	//
	// Interval at which the local tuner device lineup discovery will occur (seconds)
	int discover_lineups_interval;

	// discover_recordings_interval
	//
	// Interval at which the local storage device recording discovery will occur (seconds)
	int discover_recordings_interval;

	// discover_recordingrules_interval
	//
	// Interval at which the recording rule discovery will occur (seconds)
	int discover_recordingrules_interval;

	// use_direct_tuning
	//
	// Flag indicating that Live TV will be handled directly from the tuner(s)
	bool use_direct_tuning;

	// startup_discovery_task_delay
	//
	// Indicates the number of seconds to pause before initiating the startup discovery task
	int startup_discovery_task_delay;

	// stream_read_minimum_byte_count
	//
	// Indicates the minimum number of bytes to return from a stream read
	int stream_read_minimum_byte_count;

	// stream_read_timeout
	//
	// Indicates the stream read timeout value (milliseconds)
	int stream_read_timeout;

	// stream_ring_buffer_size
	//
	// Indicates the size of the stream ring buffer to allocate
	int stream_ring_buffer_size;
};

//---------------------------------------------------------------------------
// GLOBAL VARIABLES
//---------------------------------------------------------------------------

// g_addon
//
// Kodi add-on callbacks
static std::unique_ptr<ADDON::CHelper_libXBMC_addon> g_addon;

// g_addon_lock
//
// Synchronization object to serialize access to addon callbacks
static std::mutex g_addon_lock;

// g_addon_refs
//
// Reference count for calls to ADDON_Create/ADDON_Destroy
static int g_addon_refs = 0;

// g_capabilities (const)
//
// PVR implementation capability flags
static const PVR_ADDON_CAPABILITIES g_capabilities = {

	true,			// bSupportsEPG
	true,			// bSupportsTV
	false,			// bSupportsRadio
	true,			// bSupportsRecordings
	false,			// bSupportsRecordingsUndelete
	true,			// bSupportsTimers
	true,			// bSupportsChannelGroups
	false,			// bSupportsChannelScan
	false,			// bSupportsChannelSettings
	true,			// bHandlesInputStream
	false,			// bHandlesDemuxing
	false,			// bSupportsRecordingPlayCount
	true,			// bSupportsLastPlayedPosition
	false,			// bSupportsRecordingEdl
	false,			// bSupportsRecordingsRename
	false,			// bSupportsRecordingsLifetimeChange
	false,			// bSupportsDescrambleInfo
	0,				// iRecordingsLifetimesSize
	{ { 0, "" } }	// recordingsLifetimeValues
};

// g_connpool
//
// Global SQLite database connection pool instance
static std::shared_ptr<connectionpool> g_connpool;

// g_dvrstream
//
// DVR stream buffer instance
static std::unique_ptr<dvrstream> g_dvrstream;

// g_epgmaxtime
//
// Maximum number of days to report for EPG and series timers
static int g_epgmaxtime = EPG_TIMEFRAME_UNLIMITED;

// g_gui
//
// Kodi GUI library callbacks
static std::unique_ptr<CHelper_libKODI_guilib> g_gui;

// g_pvr
//
// Kodi PVR add-on callbacks
static std::unique_ptr<CHelper_libXBMC_pvr> g_pvr;

// g_scheduler
//
// Task scheduler
static scheduler g_scheduler([](std::exception const& ex) -> void { handle_stdexception("scheduled task", ex); });

// g_settings
//
// Global addon settings instance
static addon_settings g_settings = {

	false,					// pause_discovery_while_streaming
	false,					// prepend_channel_numbers
	false,					// use_episode_number_as_title
	false,					// use_backend_genre_strings
	false,					// show_drm_protected_channels
	86400,					// delete_datetime_rules_after			default = 1 day
	false,					// use_broadcast_device_discovery
	300, 					// discover_devices_interval;			default = 5 minutes
	7200,					// discover_episodes_interval			default = 2 hours
	3600,					// discover_guide_interval				default = 1 hour
	600,					// discover_lineups_interval			default = 10 minutes
	600,					// discover_recordings_interval			default = 10 minutes
	7200,					// discover_recordingrules_interval		default = 2 hours
	false,					// use_direct_tuning
	3,						// startup_discovery_task_delay
	(1 KiB),				// stream_read_minimum_byte_count
	2500,					// stream_read_timeout
	(4 MiB),				// stream_ring_buffer_size
};

// g_settings_lock
//
// Synchronization object to serialize access to addon settings
static std::mutex g_settings_lock;

// g_timertypes (const)
//
// Array of PVR_TIMER_TYPE structures to pass to Kodi
static const PVR_TIMER_TYPE g_timertypes[] ={

	// timer_type::seriesrule
	//
	// Timer type for non-EPG series rules, requires a series name match operation to create. Also used when editing
	// an existing recording rule as the EPG/seriesid information will not be available
	{
		// iID
		timer_type::seriesrule,

		// iAttributes
		PVR_TIMER_TYPE_IS_REPEATING | PVR_TIMER_TYPE_SUPPORTS_CHANNELS | PVR_TIMER_TYPE_SUPPORTS_TITLE_EPG_MATCH | PVR_TIMER_TYPE_SUPPORTS_RECORD_ONLY_NEW_EPISODES | 
			PVR_TIMER_TYPE_SUPPORTS_START_END_MARGIN | PVR_TIMER_TYPE_FORBIDS_EPG_TAG_ON_CREATE | PVR_TIMER_TYPE_SUPPORTS_ANY_CHANNEL,

		// strDescription
		"Record Series Rule",

		0, { { 0, "" } }, 0,		// priorities
		0, { { 0, "" } }, 0,		// lifetimes

		// preventDuplicateEpisodes
		3, {
			{ duplicate_prevention::none, "Record all episodes" },
			{ duplicate_prevention::newonly, "Record only new episodes" },
			{ duplicate_prevention::recentonly, "Record only recent episodes" }
		}, 0,

		0, { { 0, "" } }, 0,		// recordingGroup
		0, { { 0, "" } }, 0,		// maxRecordings
	},

	// timer_type::datetimeonlyrule
	//
	// Timer type for non-EPG date time only rules, requires a series name match operation to create. Also used when editing
	// an existing recording rule as the EPG/seriesid information will not be available
	//
	// TODO: Made read-only since there is no way to get it to display the proper date selector.  Making it one-shot or manual
	// rather than repeating removes it from the Timer Rules area and causes other problems.  If Kodi allowed the date selector
	// to be displayed I think that would suffice, and wouldn't be that difficult or disruptive to the Kodi code.  For now, the
	// PVR_TIMER_TYPE_SUPPORTS_FIRST_DAY flag was added to show the date of the recording.  Unfortunately, this also means that
	// the timer rule cannot be deleted, which sucks.
	{
		// iID
		timer_type::datetimeonlyrule,

		// iAttributes
		PVR_TIMER_TYPE_IS_REPEATING | PVR_TIMER_TYPE_IS_READONLY | PVR_TIMER_TYPE_SUPPORTS_CHANNELS | PVR_TIMER_TYPE_SUPPORTS_TITLE_EPG_MATCH | 
			PVR_TIMER_TYPE_SUPPORTS_FIRST_DAY | PVR_TIMER_TYPE_SUPPORTS_START_TIME | PVR_TIMER_TYPE_SUPPORTS_START_END_MARGIN | 
			PVR_TIMER_TYPE_FORBIDS_EPG_TAG_ON_CREATE,

		// strDescription
		"Record Once Rule",

		0, { { 0, "" } }, 0,		// priorities
		0, { { 0, "" } }, 0,		// lifetimes
		0, { { 0, "" } }, 0,		// preventDuplicateEpisodes
		0, { { 0, "" } }, 0,		// recordingGroup
		0, { { 0, "" } }, 0,		// maxRecordings
	},	
	
	// timer_type::epgseriesrule
	//
	// Timer type for EPG series rules
	{
		// iID
		timer_type::epgseriesrule,

		// iAttributes
		//
		// todo: PVR_TIMER_TYPE_REQUIRES_EPG_SERIESLINK_ON_CREATE can be set here, but seems to have bugs right now, after Kodi
		// is stopped and restarted, the cached EPG data prevents adding a new timer if this is set
		PVR_TIMER_TYPE_IS_REPEATING | PVR_TIMER_TYPE_SUPPORTS_CHANNELS | PVR_TIMER_TYPE_SUPPORTS_RECORD_ONLY_NEW_EPISODES | 
			PVR_TIMER_TYPE_SUPPORTS_START_END_MARGIN | PVR_TIMER_TYPE_REQUIRES_EPG_SERIES_ON_CREATE | PVR_TIMER_TYPE_SUPPORTS_ANY_CHANNEL,

		// strDescription
		"Record Series",

		0, { { 0, "" } }, 0,		// priorities
		0, { { 0, "" } }, 0,		// lifetimes

		// preventDuplicateEpisodes
		3, {
			{ duplicate_prevention::none, "Record all episodes" },
			{ duplicate_prevention::newonly, "Record only new episodes" },
			{ duplicate_prevention::recentonly, "Record only recent episodes" }
		}, 0,

		0, { { 0, "" } }, 0,		// recordingGroup
		0, { { 0, "" } }, 0,		// maxRecordings
	},

	// timer_type::epgdatetimeonlyrule
	//
	// Timer type for EPG date time only rules
	{
		// iID
		timer_type::epgdatetimeonlyrule,

		// iAttributes
		//
		// todo: PVR_TIMER_TYPE_REQUIRES_EPG_SERIESLINK_ON_CREATE can be set here, but seems to have bugs right now, after Kodi
		// is stopped and restarted, the cached EPG data prevents adding a new timer if this is set
		PVR_TIMER_TYPE_SUPPORTS_CHANNELS | PVR_TIMER_TYPE_SUPPORTS_START_END_MARGIN | PVR_TIMER_TYPE_REQUIRES_EPG_SERIES_ON_CREATE,

		// strDescription
		"Record Once",

		0, { { 0, "" } }, 0,		// priorities
		0, { { 0, "" } }, 0,		// lifetimes
		0, { { 0, "" } }, 0,		// preventDuplicateEpisodes
		0, { { 0, "" } }, 0,		// recordingGroup
		0, { { 0, "" } }, 0,		// maxRecordings
	},	
	
	// timer_type::seriestimer
	//
	// used for existing episode timers; these cannot be edited or deleted by the end user
	{
		// iID
		timer_type::seriestimer,

		// iAttributes
		PVR_TIMER_TYPE_IS_READONLY | PVR_TIMER_TYPE_FORBIDS_NEW_INSTANCES | PVR_TIMER_TYPE_SUPPORTS_CHANNELS | PVR_TIMER_TYPE_SUPPORTS_START_TIME | 
			PVR_TIMER_TYPE_SUPPORTS_END_TIME,
		
		// strDescription
		"Record Series Episode",

		0, { {0, "" } }, 0,			// priorities
		0, { {0, "" } }, 0,			// lifetimes
		0, { {0, "" } }, 0,			// preventDuplicateEpisodes
		0, { {0, "" } }, 0,			// recordingGroup
		0, { {0, "" } }, 0,			// maxRecordings
	},

	// timer_type::datetimeonlytimer
	//
	// used for existing date/time only episode timers; these cannot be edited by the user, but allows the
	// timer and it's associated parent rule to be deleted successfully via the live TV interface
	{
		// iID
		timer_type::datetimeonlytimer,

		// iAttributes
		PVR_TIMER_TYPE_FORBIDS_NEW_INSTANCES | PVR_TIMER_TYPE_SUPPORTS_CHANNELS | PVR_TIMER_TYPE_SUPPORTS_START_TIME | PVR_TIMER_TYPE_SUPPORTS_END_TIME,
		
		// strDescription
		"Record Once Episode",

		0, { {0, "" } }, 0,			// priorities
		0, { {0, "" } }, 0,			// lifetimes
		0, { {0, "" } }, 0,			// preventDuplicateEpisodes
		0, { {0, "" } }, 0,			// recordingGroup
		0, { {0, "" } }, 0,			// maxRecordings
	},
};

//---------------------------------------------------------------------------
// HELPER FUNCTIONS
//---------------------------------------------------------------------------

// copy_settings
//
// Atomically creates a copy of the global addon_settings structure
inline struct addon_settings copy_settings(void)
{
	std::unique_lock<std::mutex> settings_lock(g_settings_lock);
	return g_settings;
}

// delete_expired_enum_to_seconds
//
// Converts the delete expired rules interval enumeration values into a number of seconds
static int delete_expired_enum_to_seconds(int nvalue)
{
	switch(nvalue) {

		case 0: return -1;			// Never
		case 1: return 21600;		// 6 hours
		case 2: return 43200;		// 12 hours
		case 3: return 86400;		// 1 day
		case 4: return 172800;		// 2 days
	};

	return -1;						// Never = default
}

// discover_devices_task
//
// Scheduled task implementation to discover the HDHomeRun devices
static void discover_devices_task(scalar_condition<bool> const& cancel)
{
	bool		changed = false;			// Flag if the discovery data changed

	assert(g_addon && g_pvr);
	log_notice(__func__, ": initiated local network device discovery");

	// Create a copy of the current addon settings structure
	struct addon_settings settings = copy_settings();

	try {

		// Pull a database connection out from the connection pool
		connectionpool::handle dbhandle(g_connpool);

		// Discover the devices on the local network and check for changes
		discover_devices(dbhandle, settings.use_broadcast_device_discovery, changed);

		if(changed) {

			// Execute a lineup discovery now; task will reschedule itself
			log_notice(__func__, ": device discovery data changed -- execute lineup discovery now");
			g_scheduler.remove(discover_lineups_task);
			discover_lineups_task(cancel);

			// Execute a recording discovery now; task will reschedule itself
			log_notice(__func__, ": device discovery data changed -- execute recording discovery now");
			g_scheduler.remove(discover_recordings_task);
			discover_recordings_task(cancel);
		}
	}

	catch(std::exception& ex) { handle_stdexception(__func__, ex); }
	catch(...) { handle_generalexception(__func__); }

	// Schedule the next periodic invocation of this discovery task
	log_notice(__func__, ": scheduling next device discovery to initiate in ", settings.discover_devices_interval, " seconds");
	g_scheduler.add(std::chrono::system_clock::now() + std::chrono::seconds(settings.discover_devices_interval), discover_devices_task);
}

// discover_episodes_task
//
// Scheduled task implementation to discover the episode data associated with recording rules
static void discover_episodes_task(scalar_condition<bool> const& /*cancel*/)
{
	bool		changed = false;			// Flag if the discovery data changed

	assert(g_addon && g_pvr);
	log_notice(__func__, ": initiated recording rule episode discovery");

	// Create a copy of the current addon settings structure
	struct addon_settings settings = copy_settings();

	try {

		// Pull a database connection out from the connection pool
		connectionpool::handle dbhandle(g_connpool);

		// Discover the episode data from the backend service
		discover_episodes(dbhandle, changed);
		
		if(changed) {

			// Changes in the episode data affects the PVR timers
			log_notice(__func__, ": recording rule episode discovery data changed -- trigger timer update");
			g_pvr->TriggerTimerUpdate();
		}
	}

	catch(std::exception& ex) { handle_stdexception(__func__, ex); }
	catch(...) { handle_generalexception(__func__); }

	// Schedule the next periodic invocation of this discovery task
	log_notice(__func__, ": scheduling next recording rule episode discovery to initiate in ", settings.discover_episodes_interval, " seconds");
	g_scheduler.add(std::chrono::system_clock::now() + std::chrono::seconds(settings.discover_episodes_interval), discover_episodes_task);
}

// discover_guide_task
//
// Scheduled task implementation to discover the electronic program guide
static void discover_guide_task(scalar_condition<bool> const& /*cancel*/)
{
	bool		changed = false;			// Flag if the discovery data changed

	assert(g_addon && g_pvr);
	log_notice(__func__, ": initiated guide discovery");

	// Create a copy of the current addon settings structure
	struct addon_settings settings = copy_settings();

	try {

		// Pull a database connection out from the connection pool
		connectionpool::handle dbhandle(g_connpool);

		// Discover the updated electronic program guide data from the backend service
		discover_guide(dbhandle, changed);
		
		if(changed) {

			// Trigger a channel update; the guide data is used to resolve channel names and icon image urls
			log_notice(__func__, ": guide channel discovery data changed -- trigger channel update");
			g_pvr->TriggerChannelUpdate();
		}
	}

	catch(std::exception& ex) { handle_stdexception(__func__, ex); }
	catch(...) { handle_generalexception(__func__); }

	// Schedule the next periodic invocation of this discovery task
	log_notice(__func__, ": scheduling next guide discovery to initiate in ", settings.discover_guide_interval, " seconds");
	g_scheduler.add(std::chrono::system_clock::now() + std::chrono::seconds(settings.discover_guide_interval), discover_guide_task);
}

// discover_lineups_task
//
// Scheduled task implementation to discover the channel lineups
static void discover_lineups_task(scalar_condition<bool> const& /*cancel*/)
{
	bool		changed = false;			// Flag if the discovery data changed

	assert(g_addon && g_pvr);
	log_notice(__func__, ": initiated local tuner device lineup discovery");

	// Create a copy of the current addon settings structure
	struct addon_settings settings = copy_settings();

	try {

		// Pull a database connection out from the connection pool
		connectionpool::handle dbhandle(g_connpool);

		// Discover the channel lineups for all available tuner devices
		discover_lineups(dbhandle, changed);
		
		if(changed) {

			// Trigger a PVR channel update
			log_notice(__func__, ": lineup discovery data changed -- trigger channel update");
			g_pvr->TriggerChannelUpdate();

			// Trigger a PVR channel group update
			log_notice(__func__, ": lineup discovery data changed -- trigger channel group update");
			g_pvr->TriggerChannelGroupsUpdate();

			// NOTE: Don't bother with an EPG update here, Kodi will not create EPGs for new channels
			// added via a trigger so it's pointless.  New channels will show the original lineup
			// channel name until the next periodic EPG update then the guide name will replace it
		}
	}

	catch(std::exception& ex) { handle_stdexception(__func__, ex); }
	catch(...) { handle_generalexception(__func__); }

	// Schedule the next periodic invocation of this discovery task
	log_notice(__func__, ": scheduling next lineup discovery to initiate in ", settings.discover_lineups_interval, " seconds");
	g_scheduler.add(std::chrono::system_clock::now() + std::chrono::seconds(settings.discover_lineups_interval), discover_lineups_task);
}

// discover_recordingrules_task
//
// Scheduled task implementation to discover the recording rules and timers
static void discover_recordingrules_task(scalar_condition<bool> const& cancel)
{
	bool		changed = false;			// Flag if the discovery data changed

	assert(g_addon && g_pvr);
	log_notice(__func__, ": initiated recording rule discovery");

	// Create a copy of the current addon settings structure
	struct addon_settings settings = copy_settings();

	try {

		// Pull a database connection out from the connection pool
		connectionpool::handle dbhandle(g_connpool);

		// Discover the recording rules from the backend service
		discover_recordingrules(dbhandle, changed);

		// Generate a vector<> of all expired recording rules to be deleted from the backend
		std::vector<unsigned int> expired;
		enumerate_expired_recordingruleids(dbhandle, settings.delete_datetime_rules_after, [&](unsigned int const& recordingruleid) -> void 
		{ 
			expired.push_back(recordingruleid); 
		});

		// Iterate over the vector<> and attempt to delete each expired rule from the backend
		for(auto const& it : expired) {

			try { delete_recordingrule(dbhandle, it); changed = true; }
			catch(std::exception& ex) { handle_stdexception(__func__, ex); }
			catch(...) { handle_generalexception(__func__); }
		}
		
		if(changed) {

			// Trigger a PVR timer update (if the subsequent episode discovery changes it may trigger again)
			log_notice(__func__, ": recording rule discovery data changed -- trigger timer update");
			g_pvr->TriggerTimerUpdate();

			// Execute a recording rule episode discovery now; task will reschedule itself
			log_notice(__func__, ": device discovery data changed -- execute recording rule episode discovery now");
			g_scheduler.remove(discover_episodes_task);
			discover_episodes_task(cancel);
		}
	}

	catch(std::exception& ex) { handle_stdexception(__func__, ex); }
	catch(...) { handle_generalexception(__func__); }

	// Schedule the next periodic invocation of this discovery task
	log_notice(__func__, ": scheduling next recording rule discovery to initiate in ", settings.discover_recordingrules_interval, " seconds");
	g_scheduler.add(std::chrono::system_clock::now() + std::chrono::seconds(settings.discover_recordingrules_interval), discover_recordingrules_task);
}

// discover_recordings_task
//
// Scheduled task implementation to discover the storage recordings
static void discover_recordings_task(scalar_condition<bool> const& /*cancel*/)
{
	bool		changed = false;			// Flag if the discovery data changed

	assert(g_addon && g_pvr);
	log_notice(__func__, ": initiated local storage device recording discovery");

	// Create a copy of the current addon settings structure
	struct addon_settings settings = copy_settings();

	try {

		// Pull a database connection out from the connection pool
		connectionpool::handle dbhandle(g_connpool);

		// Discover the recordings for all available local storage devices
		discover_recordings(dbhandle, changed);
		
		if(changed) {

			// Trigger a recordings update
			log_notice(__func__, ": recording discovery data changed -- trigger recording update");
			g_pvr->TriggerRecordingUpdate();
		}
	}

	catch(std::exception& ex) { handle_stdexception(__func__, ex); }
	catch(...) { handle_generalexception(__func__); }

	// Schedule the next periodic invocation of this discovery task
	log_notice(__func__, ": scheduling next recording discovery to initiate in ", settings.discover_recordings_interval, " seconds");
	g_scheduler.add(std::chrono::system_clock::now() + std::chrono::seconds(settings.discover_recordings_interval), discover_recordings_task);
}

// discover_startup_task
//
// Scheduled task implementation to discover all data during PVR startup
static void discover_startup_task(scalar_condition<bool> const& /*cancel*/)
{
	bool				lineups_changed = false;			// Flag if lineups have changed
	bool				recordings_changed = false;			// Flag if recordings have changed
	bool				guide_changed = false;				// Flag if guide data has changed
	bool				recordingrules_changed = false;		// Flag if recording rules have changed
	bool				episodes_changed = false;			// Flag if episode data has changed

	assert(g_addon && g_pvr);
	log_notice(__func__, ": initiated startup discovery task");

	// Create a copy of the current addon settings structure
	struct addon_settings settings = copy_settings();

	try {

		// Pull a database connection out from the connection pool
		connectionpool::handle dbhandle(g_connpool);

		// Discover all of the local device and backend service data
		discover_devices(dbhandle, settings.use_broadcast_device_discovery);
		discover_lineups(dbhandle, lineups_changed);
		discover_recordings(dbhandle, recordings_changed);
		discover_guide(dbhandle, guide_changed);
		discover_recordingrules(dbhandle, recordingrules_changed);
		discover_episodes(dbhandle, episodes_changed);

		// TRIGGER: Channels
		if(lineups_changed || guide_changed) {
			
			log_notice(__func__, ": discovery data changed -- trigger channel update");
			g_pvr->TriggerChannelUpdate();
		}

		// TRIGGER: Channel Groups
		if(lineups_changed) {
			
			log_notice(__func__, ": discovery data changed -- trigger channel group update");
			g_pvr->TriggerChannelGroupsUpdate();
		}

		// TRIGGER: Recordings
		if(recordings_changed) {
			
			log_notice(__func__, ": discovery data changed -- trigger recording update");
			g_pvr->TriggerRecordingUpdate();
		}

		// TRIGGER: Timers
		if(recordingrules_changed || episodes_changed) {
			
			log_notice(__func__, ": discovery data changed -- trigger timer update");
			g_pvr->TriggerTimerUpdate();
		}

		// Schedule the standard periodic updates to occur at the specified intervals
		std::chrono::time_point<std::chrono::system_clock> now = std::chrono::system_clock::now();

		log_notice(__func__, ": scheduling periodic device discovery to initiate in ", settings.discover_devices_interval, " seconds");
		g_scheduler.add(now + std::chrono::seconds(settings.discover_devices_interval), discover_devices_task);

		log_notice(__func__, ": scheduling periodic lineup discovery to initiate in ", settings.discover_lineups_interval, " seconds");
		g_scheduler.add(now + std::chrono::seconds(settings.discover_lineups_interval), discover_lineups_task);

		log_notice(__func__, ": scheduling periodic recording discovery to initiate in ", settings.discover_recordings_interval, " seconds");
		g_scheduler.add(now + std::chrono::seconds(settings.discover_recordings_interval), discover_recordings_task);

		log_notice(__func__, ": scheduling periodic guide discovery to initiate in ", settings.discover_guide_interval, " seconds");
		g_scheduler.add(now + std::chrono::seconds(settings.discover_guide_interval), discover_guide_task);

		log_notice(__func__, ": scheduling periodic recording rule discovery to initiate in ", settings.discover_recordingrules_interval, " seconds");
		g_scheduler.add(now + std::chrono::seconds(settings.discover_recordingrules_interval), discover_recordingrules_task);

		log_notice(__func__, ": scheduling periodic recording rule episode discovery to initiate in ", settings.discover_episodes_interval, " seconds");
		g_scheduler.add(now + std::chrono::seconds(settings.discover_episodes_interval), discover_episodes_task);	
	}

	catch(std::exception& ex) { handle_stdexception(__func__, ex); }
	catch(...) { handle_generalexception(__func__); }
}

// handle_generalexception
//
// Handler for thrown generic exceptions
static void handle_generalexception(char const* function)
{
	log_error(function, " failed due to an unhandled exception");
}

// handle_generalexception
//
// Handler for thrown generic exceptions
template<typename _result>
static _result handle_generalexception(char const* function, _result result)
{
	handle_generalexception(function);
	return result;
}

// handle_stdexception
//
// Handler for thrown std::exceptions
static void handle_stdexception(char const* function, std::exception const& ex)
{
	log_error(function, " failed due to an unhandled exception: ", ex.what());
}

// handle_stdexception
//
// Handler for thrown std::exceptions
template<typename _result>
static _result handle_stdexception(char const* function, std::exception const& ex, _result result)
{
	handle_stdexception(function, ex);
	return result;
}

// interval_enum_to_seconds
//
// Converts the discovery interval enumeration values into a number of seconds
static int interval_enum_to_seconds(int nvalue)
{	
	switch(nvalue) {

		case 0: return 300;			// 5 minutes
		case 1: return 600;			// 10 minutes
		case 2: return 900;			// 15 minutes
		case 3: return 1800;		// 30 minutes
		case 4: return 2700;		// 45 minutes
		case 5: return 3600;		// 1 hour
		case 6: return 7200;		// 2 hours
		case 7: return 14400;		// 4 hours
		
		// 30 seconds and 1 minute were added after the fact, for compatibility
		// with existing settings they were put at the end.  Local network
		// discoveries can be executed more quickly if the user prefers that

		case 8: return 30;			// 30 seconds
		case 9: return 60;			// 1 minute
	};

	return 600;						// 10 minutes = default
}
	
// log_debug
//
// Variadic method of writing a LOG_DEBUG entry into the Kodi application log
template<typename... _args>
static void log_debug(_args&&... args)
{
	log_message(ADDON::addon_log_t::LOG_DEBUG, std::forward<_args>(args)...);
}

// log_error
//
// Variadic method of writing a LOG_ERROR entry into the Kodi application log
template<typename... _args>
static void log_error(_args&&... args)
{
	log_message(ADDON::addon_log_t::LOG_ERROR, std::forward<_args>(args)...);
}

// log_info
//
// Variadic method of writing a LOG_INFO entry into the Kodi application log
template<typename... _args>
static void log_info(_args&&... args)
{
	log_message(ADDON::addon_log_t::LOG_INFO, std::forward<_args>(args)...);
}

// log_message
//
// Variadic method of writing an entry into the Kodi application log
template<typename... _args>
static void log_message(ADDON::addon_log_t level, _args&&... args)
{
	std::ostringstream stream;
	int unpack[] = {0, ( static_cast<void>(stream << args), 0 ) ... };
	(void)unpack;

	if(g_addon) g_addon->Log(level, stream.str().c_str());

	// Write LOG_ERROR level messages to an appropriate secondary log mechanism
	if(level == ADDON::addon_log_t::LOG_ERROR) {

#ifdef _WINDOWS
		std::string message = "ERROR: " + stream.str() + "\r\n";
		OutputDebugStringA(message.c_str());
#elif __ANDROID__
		__android_log_print(ANDROID_LOG_ERROR, VERSION_PRODUCTNAME_ANSI, "ERROR: %s\n", stream.str().c_str());
#else
		fprintf(stderr, "ERROR: %s\r\n", stream.str().c_str());
#endif

	}
}

// log_notice
//
// Variadic method of writing a LOG_NOTICE entry into the Kodi application log
template<typename... _args>
static void log_notice(_args&&... args)
{
	log_message(ADDON::addon_log_t::LOG_NOTICE, std::forward<_args>(args)...);
}

// mincount_enum_to_bytes
//
// Converts the minimum read count enumeration values into a number of bytes
static int mincount_enum_to_bytes(int nvalue)
{	
	switch(nvalue) {

		case 0: return 0;			// None
		case 1: return (1 KiB);		// 1 Kilobyte
		case 2: return (2 KiB);		// 2 Kilobytes
		case 3: return (4 KiB);		// 4 Kilobytes
		case 4: return (8 KiB);		// 8 Kilobytes
		case 5: return (16 KiB);	// 16 Kilobytes
	};

	return (1 KiB);					// 1 Kilobyte = default
}

// ringbuffersize_enum_to_bytes
//
// Converts the ring buffer size enumeration values into a number of bytes
static int ringbuffersize_enum_to_bytes(int nvalue)
{	
	switch(nvalue) {

		case 0: return (1 MiB);		// 1 Megabyte
		case 1: return (2 MiB);		// 2 Megabytes
		case 2: return (4 MiB);		// 4 Megabytes
		case 3: return (8 MiB);		// 8 Megabytes
		case 4: return (16 MiB);	// 16 Megabytes
	};

	return (4 MiB);					// 4 Megabytes = default
}

//---------------------------------------------------------------------------
// KODI ADDON ENTRY POINTS
//---------------------------------------------------------------------------

//---------------------------------------------------------------------------
// ADDON_Create
//
// Creates and initializes the Kodi addon instance
//
// Arguments:
//
//	handle			- Kodi add-on handle
//	props			- Add-on specific properties structure (PVR_PROPERTIES)

ADDON_STATUS ADDON_Create(void* handle, void* props)
{
	PVR_MENUHOOK			menuhook;			// For registering menu hooks
	bool					bvalue = false;		// Setting value
	int						nvalue = 0;			// Setting value
	scalar_condition<bool>	cancel{false};		// Dummy cancellation flag for tasks

	if((handle == nullptr) || (props == nullptr)) return ADDON_STATUS::ADDON_STATUS_PERMANENT_FAILURE;

	// Kodi 18 "Leia" allows ADDON_Create to be called multiple times from multiple threads
	// when the addon is being installed; work around this with a mutex and a reference counter
	std::unique_lock<std::mutex> lock(g_addon_lock);
	if((++g_addon_refs) > 1) {

		assert(g_addon);
		log_notice(__func__, ": warning: bypassing addon initialization (refs = ", g_addon_refs, ")");
		return ADDON_STATUS::ADDON_STATUS_OK;
	}

	// Copy anything relevant from the provided parameters
	PVR_PROPERTIES* pvrprops = reinterpret_cast<PVR_PROPERTIES*>(props);
	g_epgmaxtime = pvrprops->iEpgMaxDays;

#ifdef __ANDROID__
	// Uncomment this to allow normal crash dumps to be generated on Android
	// prctl(PR_SET_DUMPABLE, 1);
#endif

	try {

#ifdef _WINDOWS
		// On Windows, initialize winsock in case broadcast discovery is used; WSAStartup is
		// reference-counted so if it's already been called this won't hurt anything
		WSADATA wsaData;
		WSAStartup(MAKEWORD(2, 2), &wsaData);
#endif

		// Initialize libcurl using the standard default options
		if(curl_global_init(CURL_GLOBAL_DEFAULT) != CURLE_OK) throw string_exception("curl_global_init(CURL_GLOBAL_DEFAULT) failed");

		// Create the global addon callbacks instance
		g_addon.reset(new ADDON::CHelper_libXBMC_addon());
		if(!g_addon->RegisterMe(handle)) throw string_exception("Failed to register addon handle (CHelper_libXBMC_addon::RegisterMe)");

		// Throw a banner out to the Kodi log indicating that the add-on is being loaded
		log_notice(__func__, ": ", VERSION_PRODUCTNAME_ANSI, " v", VERSION_VERSION3_ANSI, " loading");

		try { 

			// The user data path doesn't always exist when an addon has been installed
			if(!g_addon->DirectoryExists(pvrprops->strUserPath)) {

				log_notice(__func__, ": user data directory ", pvrprops->strUserPath, " does not exist");
				if(!g_addon->CreateDirectory(pvrprops->strUserPath)) throw string_exception("unable to create addon user data directory");
				log_notice(__func__, ": user data directory ", pvrprops->strUserPath, " created");
			}

			// Load the general settings
			if(g_addon->GetSetting("pause_discovery_while_streaming", &bvalue)) g_settings.pause_discovery_while_streaming = bvalue;
			if(g_addon->GetSetting("prepend_channel_numbers", &bvalue)) g_settings.prepend_channel_numbers = bvalue;
			if(g_addon->GetSetting("use_episode_number_as_title", &bvalue)) g_settings.use_episode_number_as_title = bvalue;
			if(g_addon->GetSetting("use_backend_genre_strings", &bvalue)) g_settings.use_backend_genre_strings = bvalue;
			if(g_addon->GetSetting("show_drm_protected_channels", &bvalue)) g_settings.show_drm_protected_channels = bvalue;
			if(g_addon->GetSetting("delete_datetime_rules_after", &nvalue)) g_settings.delete_datetime_rules_after = delete_expired_enum_to_seconds(nvalue);

			// Load the discovery interval settings
			if(g_addon->GetSetting("use_broadcast_device_discovery", &bvalue)) g_settings.use_broadcast_device_discovery = bvalue;
			if(g_addon->GetSetting("discover_devices_interval", &nvalue)) g_settings.discover_devices_interval = interval_enum_to_seconds(nvalue);
			if(g_addon->GetSetting("discover_lineups_interval", &nvalue)) g_settings.discover_lineups_interval = interval_enum_to_seconds(nvalue);
			if(g_addon->GetSetting("discover_guide_interval", &nvalue)) g_settings.discover_guide_interval = interval_enum_to_seconds(nvalue);
			if(g_addon->GetSetting("discover_recordings_interval", &nvalue)) g_settings.discover_recordings_interval = interval_enum_to_seconds(nvalue);
			if(g_addon->GetSetting("discover_recordingrules_interval", &nvalue)) g_settings.discover_recordingrules_interval = interval_enum_to_seconds(nvalue);
			if(g_addon->GetSetting("discover_episodes_interval", &nvalue)) g_settings.discover_episodes_interval = interval_enum_to_seconds(nvalue);

			// Load the advanced settings
			if(g_addon->GetSetting("use_direct_tuning", &bvalue)) g_settings.use_direct_tuning = bvalue;
			if(g_addon->GetSetting("startup_discovery_task_delay", &nvalue)) g_settings.startup_discovery_task_delay = nvalue;
			if(g_addon->GetSetting("stream_read_minimum_byte_count", &nvalue)) g_settings.stream_read_minimum_byte_count = mincount_enum_to_bytes(nvalue);
			if(g_addon->GetSetting("stream_read_timeout", &nvalue)) g_settings.stream_read_timeout = nvalue;
			if(g_addon->GetSetting("stream_ring_buffer_size", &nvalue)) g_settings.stream_ring_buffer_size = ringbuffersize_enum_to_bytes(nvalue);

			// Create the global guicallbacks instance
			g_gui.reset(new CHelper_libKODI_guilib());
			if(!g_gui->RegisterMe(handle)) throw string_exception("Failed to register gui addon handle (CHelper_libKODI_guilib::RegisterMe)");

			try {

				// Create the global pvrcallbacks instance
				g_pvr.reset(new CHelper_libXBMC_pvr());
				if(!g_pvr->RegisterMe(handle)) throw string_exception("Failed to register pvr addon handle (CHelper_libXBMC_pvr::RegisterMe)");
		
				try {

					// PVR_MENUHOOK_TIMER
					//
					memset(&menuhook, 0, sizeof(PVR_MENUHOOK));
					menuhook.iHookId = MENUHOOK_RECORD_DELETENORERECORD;
					menuhook.iLocalizedStringId = 30301;
					menuhook.category = PVR_MENUHOOK_RECORDING;
					g_pvr->AddMenuHook(&menuhook);

					// PVR_MENUHOOK_RECORDING
					//
					memset(&menuhook, 0, sizeof(PVR_MENUHOOK));
					menuhook.iHookId = MENUHOOK_RECORD_DELETERERECORD;
					menuhook.iLocalizedStringId = 30302;
					menuhook.category = PVR_MENUHOOK_RECORDING;
					g_pvr->AddMenuHook(&menuhook);

					// MENUHOOK_SETTING_SHOWDEVICENAMES
					//
					memset(&menuhook, 0, sizeof(PVR_MENUHOOK));
					menuhook.iHookId = MENUHOOK_SETTING_SHOWDEVICENAMES;
					menuhook.iLocalizedStringId = 30312;
					menuhook.category = PVR_MENUHOOK_SETTING;
					g_pvr->AddMenuHook(&menuhook);

					// MENUHOOK_SETTING_TRIGGERDEVICEDISCOVERY
					//
					memset(&menuhook, 0, sizeof(PVR_MENUHOOK));
					menuhook.iHookId = MENUHOOK_SETTING_TRIGGERDEVICEDISCOVERY;
					menuhook.iLocalizedStringId = 30303;
					menuhook.category = PVR_MENUHOOK_SETTING;
					g_pvr->AddMenuHook(&menuhook);

					// MENUHOOK_SETTING_TRIGGERLINEUPDISCOVERY
					//
					memset(&menuhook, 0, sizeof(PVR_MENUHOOK));
					menuhook.iHookId = MENUHOOK_SETTING_TRIGGERLINEUPDISCOVERY;
					menuhook.iLocalizedStringId = 30304;
					menuhook.category = PVR_MENUHOOK_SETTING;
					g_pvr->AddMenuHook(&menuhook);

					// MENUHOOK_SETTING_TRIGGERGUIDEDISCOVERY
					//
					memset(&menuhook, 0, sizeof(PVR_MENUHOOK));
					menuhook.iHookId = MENUHOOK_SETTING_TRIGGERGUIDEDISCOVERY;
					menuhook.iLocalizedStringId = 30305;
					menuhook.category = PVR_MENUHOOK_SETTING;
					g_pvr->AddMenuHook(&menuhook);

					// MENUHOOK_SETTING_TRIGGERRECORDINGDISCOVERY
					//
					memset(&menuhook, 0, sizeof(PVR_MENUHOOK));
					menuhook.iHookId = MENUHOOK_SETTING_TRIGGERRECORDINGDISCOVERY;
					menuhook.iLocalizedStringId = 30306;
					menuhook.category = PVR_MENUHOOK_SETTING;
					g_pvr->AddMenuHook(&menuhook);

					// MENUHOOK_SETTING_TRIGGERRECORDINGRULEDISCOVERY
					//
					memset(&menuhook, 0, sizeof(PVR_MENUHOOK));
					menuhook.iHookId = MENUHOOK_SETTING_TRIGGERRECORDINGRULEDISCOVERY;
					menuhook.iLocalizedStringId = 30307;
					menuhook.category = PVR_MENUHOOK_SETTING;
					g_pvr->AddMenuHook(&menuhook);

					// MENUHOOK_SETTING_RESETDATABASE
					//
					memset(&menuhook, 0, sizeof(PVR_MENUHOOK));
					menuhook.iHookId = MENUHOOK_SETTING_RESETDATABASE;
					menuhook.iLocalizedStringId = 30308;
					menuhook.category = PVR_MENUHOOK_SETTING;
					g_pvr->AddMenuHook(&menuhook);

					// MENUHOOK_CHANNEL_DISABLE
					//
					memset(&menuhook, 0, sizeof(PVR_MENUHOOK));
					menuhook.iHookId = MENUHOOK_CHANNEL_DISABLE;
					menuhook.iLocalizedStringId = 30309;
					menuhook.category = PVR_MENUHOOK_CHANNEL;
					g_pvr->AddMenuHook(&menuhook);

					// MENUHOOK_CHANNEL_ADDFAVORITE
					//
					memset(&menuhook, 0, sizeof(PVR_MENUHOOK));
					menuhook.iHookId = MENUHOOK_CHANNEL_ADDFAVORITE;
					menuhook.iLocalizedStringId = 30310;
					menuhook.category = PVR_MENUHOOK_CHANNEL;
					g_pvr->AddMenuHook(&menuhook);

					// MENUHOOK_CHANNEL_REMOVEFAVORITE
					//
					memset(&menuhook, 0, sizeof(PVR_MENUHOOK));
					menuhook.iHookId = MENUHOOK_CHANNEL_REMOVEFAVORITE;
					menuhook.iLocalizedStringId = 30311;
					menuhook.category = PVR_MENUHOOK_CHANNEL;
					g_pvr->AddMenuHook(&menuhook);

					// Create the global database connection pool instance, the file name is based on the versionb
					std::string databasefile = "file:///" + std::string(pvrprops->strUserPath) + "/hdhomerundvr-v" + VERSION_VERSION2_ANSI + ".db";
					g_connpool = std::make_shared<connectionpool>(databasefile.c_str(), SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE | SQLITE_OPEN_URI);

					try {

						try {
						
							// Kodi currently has no means to create EPG entries in the database for channels that are
							// added after the PVR manager has been started.  Synchronously execute a device and lineup
							// discovery so that the initial set of channels are immediately available to Kodi
							connectionpool::handle dbhandle(g_connpool);

							log_notice(__func__, ": initiating local network resource discovery (startup)");
							discover_devices(dbhandle, g_settings.use_broadcast_device_discovery);
							discover_lineups(dbhandle);
						}

						// Failure to perform the synchronous device and lineup discovery is not fatal
						catch(std::exception& ex) { handle_stdexception(__func__, ex); }
						catch(...) { handle_generalexception(__func__); }

						// To help reduce trigger 'chatter' at startup, a special optimized task was created that loads
						// all discovery data and only triggers the PVR update(s) once per category.  Delay the launch 
						// of this initial startup discovery task for a reasonable amount of time to allow the PVR to 
						// finish it's start up processing -- failure to do so may trigger a race condition that leads
						// to a deadlock in Kodi that can occur when channel information changes while the EPGs are created
						log_notice(__func__, ": delaying startup discovery task for ", g_settings.startup_discovery_task_delay, " seconds");					
						g_scheduler.add(std::chrono::system_clock::now() + std::chrono::seconds(g_settings.startup_discovery_task_delay), discover_startup_task);
						g_scheduler.start();
					}

					// Clean up the database connection pool on exception
					catch(...) { g_connpool.reset(); throw; }
				}
			
				// Clean up the pvr callbacks instance on exception
				catch(...) { g_pvr.reset(); throw; }
			}
			
			// Clean up the gui callbacks instance on exception
			catch(...) { g_gui.reset(); throw; }
		}

		// Clean up the addon callbacks on exception; but log the error first -- once the callbacks
		// are destroyed so is the ability to write to the Kodi log file
		catch(std::exception& ex) { handle_stdexception(__func__, ex); g_addon.reset(); throw; }
		catch(...) { handle_generalexception(__func__); g_addon.reset(); throw; }
	}

	// Anything that escapes above can't be logged at this point, just return ADDON_STATUS_PERMANENT_FAILURE
	catch(...) { return ADDON_STATUS::ADDON_STATUS_PERMANENT_FAILURE; }

	// Throw a simple banner out to the Kodi log indicating that the add-on has been loaded
	log_notice(__func__, ": ", VERSION_PRODUCTNAME_ANSI, " v", VERSION_VERSION3_ANSI, " loaded");

	return ADDON_STATUS::ADDON_STATUS_OK;
}

//---------------------------------------------------------------------------
// ADDON_Destroy
//
// Destroys the Kodi addon instance
//
// Arguments:
//
//	NONE

void ADDON_Destroy(void)
{
	// Kodi 18 "Leia" allows ADDON_Destroy to be called multiple times from multiple threads
	// when the addon is being installed; work around this with a mutex and a reference counter
	std::unique_lock<std::mutex> lock(g_addon_lock);
	if((--g_addon_refs) > 0) return log_notice(__func__, ": warning: bypassing addon termination (refs = ", g_addon_refs, ")");

	assert(g_addon_refs == 0);				// Verify this is only happening one time

	// Throw a message out to the Kodi log indicating that the add-on is being unloaded
	log_notice(__func__, ": ", VERSION_PRODUCTNAME_ANSI, " v", VERSION_VERSION3_ANSI, " unloading");

	g_dvrstream.reset();					// Destroy any active stream instance
	g_scheduler.stop();						// Stop the task scheduler
	g_scheduler.clear();					// Clear all tasks from the scheduler

	// Check for more than just the global connection pool reference during shutdown,
	// there shouldn't still be any active callbacks running during ADDON_Destroy
	long poolrefs = g_connpool.use_count();
	if(poolrefs != 1) log_notice(__func__, ": warning: g_connpool.use_count = ", g_connpool.use_count());
	g_connpool.reset();

	// Destroy the PVR and GUI callback instances
	g_pvr.reset();
	g_gui.reset();

	// Send a notice out to the Kodi log as late as possible and destroy the addon callbacks
	log_notice(__func__, ": ", VERSION_PRODUCTNAME_ANSI, " v", VERSION_VERSION3_ANSI, " unloaded");
	g_addon.reset();

	// Clean up libcurl
	curl_global_cleanup();

#ifdef _WINDOWS
	WSACleanup();			// Release winsock reference added in ADDON_Create
#endif
}

//---------------------------------------------------------------------------
// ADDON_GetStatus
//
// Gets the current status of the Kodi addon
//
// Arguments:
//
//	NONE

ADDON_STATUS ADDON_GetStatus(void)
{
	return ADDON_STATUS::ADDON_STATUS_OK;
}

//---------------------------------------------------------------------------
// ADDON_SetSetting
//
// Changes the value of a named Kodi addon setting
//
// Arguments:
//
//	name		- Name of the setting to change
//	value		- New value of the setting to apply

ADDON_STATUS ADDON_SetSetting(char const* name, void const* value)
{
	std::chrono::time_point<std::chrono::system_clock> now = std::chrono::system_clock::now();
	std::unique_lock<std::mutex> settings_lock(g_settings_lock);

	// pause_discovery_while_streaming
	//
	if(strcmp(name, "pause_discovery_while_streaming") == 0) {

		bool bvalue = *reinterpret_cast<bool const*>(value);
		if(bvalue != g_settings.pause_discovery_while_streaming) {

			g_settings.pause_discovery_while_streaming = bvalue;
			log_notice(__func__, ": setting pause_discovery_while_streaming changed to ", (bvalue) ? "true" : "false");
		}
	}

	// prepend_channel_numbers
	//
	else if(strcmp(name, "prepend_channel_numbers") == 0) {

		bool bvalue = *reinterpret_cast<bool const*>(value);
		if(bvalue != g_settings.prepend_channel_numbers) {

			g_settings.prepend_channel_numbers = bvalue;
			log_notice(__func__, ": setting prepend_channel_numbers changed to ", (bvalue) ? "true" : "false", " -- trigger channel update");
			g_pvr->TriggerChannelUpdate();
		}
	}

	// use_episode_number_as_title
	//
	else if(strcmp(name, "use_episode_number_as_title") == 0) {

		bool bvalue = *reinterpret_cast<bool const*>(value);
		if(bvalue != g_settings.use_episode_number_as_title) {

			g_settings.use_episode_number_as_title = bvalue;
			log_notice(__func__, ": setting use_episode_number_as_title changed to ", (bvalue) ? "true" : "false", " -- trigger recording update");
			g_pvr->TriggerRecordingUpdate();
		}
	}

	// use_backend_genre_strings
	//
	else if(strcmp(name, "use_backend_genre_strings") == 0) {

		bool bvalue = *reinterpret_cast<bool const*>(value);
		if(bvalue != g_settings.use_backend_genre_strings) {

			g_settings.use_backend_genre_strings = bvalue;
			log_notice(__func__, ": setting use_backend_genre_strings changed to ", (bvalue) ? "true" : "false");
		}
	}

	// show_drm_protected_channels
	//
	else if(strcmp(name, "show_drm_protected_channels") == 0) {

		bool bvalue = *reinterpret_cast<bool const*>(value);
		if(bvalue != g_settings.show_drm_protected_channels) {

			g_settings.show_drm_protected_channels = bvalue;
			log_notice(__func__, ": setting show_drm_protected_channels changed to ", (bvalue) ? "true" : "false", " -- trigger channel and channel group updates");
			g_pvr->TriggerChannelUpdate();
			g_pvr->TriggerChannelGroupsUpdate();
		}
	}

	// delete_datetime_rules_after
	//
	else if(strcmp(name, "delete_datetime_rules_after") == 0) {

		int nvalue = delete_expired_enum_to_seconds(*reinterpret_cast<int const*>(value));
		if(nvalue != g_settings.delete_datetime_rules_after) {

			g_settings.delete_datetime_rules_after = nvalue;
			log_notice(__func__, ": setting delete_datetime_rules_after changed to ", nvalue, " seconds");
		}
	}

	// use_broadcast_device_discovery
	//
	else if(strcmp(name, "use_broadcast_device_discovery") == 0) {

		bool bvalue = *reinterpret_cast<bool const*>(value);
		if(bvalue != g_settings.use_broadcast_device_discovery) {

			g_settings.use_broadcast_device_discovery = bvalue;
			log_notice(__func__, ": setting use_broadcast_device_discovery changed to ", (bvalue) ? "true" : "false", " -- schedule device discovery");

			// Reschedule the device discovery task to run as soon as possible
			g_scheduler.remove(discover_devices_task);
			g_scheduler.add(now + std::chrono::seconds(1), discover_devices_task);
		}
	}

	// discover_devices_interval
	//
	else if(strcmp(name, "discover_devices_interval") == 0) {

		int nvalue = interval_enum_to_seconds(*reinterpret_cast<int const*>(value));
		if(nvalue != g_settings.discover_devices_interval) {

			// Reschedule the discover_devices_task to execute at the specified interval from now
			g_settings.discover_devices_interval = nvalue;
			g_scheduler.remove(discover_devices_task);
			g_scheduler.add(now + std::chrono::seconds(nvalue), discover_devices_task);
			log_notice(__func__, ": setting discover_devices_interval changed -- rescheduling task to initiate in ", nvalue, " seconds");
		}
	}

	// discover_lineups_interval
	//
	else if(strcmp(name, "discover_lineups_interval") == 0) {

		int nvalue = interval_enum_to_seconds(*reinterpret_cast<int const*>(value));
		if(nvalue != g_settings.discover_lineups_interval) {

			// Reschedule the discover_lineups_task to execute at the specified interval from now
			g_settings.discover_lineups_interval = nvalue;
			g_scheduler.remove(discover_lineups_task);
			g_scheduler.add(now + std::chrono::seconds(nvalue), discover_lineups_task);
			log_notice(__func__, ": setting discover_lineups_interval changed -- rescheduling task to initiate in ", nvalue, " seconds");
		}
	}

	// discover_guide_interval
	//
	else if(strcmp(name, "discover_guide_interval") == 0) {

		int nvalue = interval_enum_to_seconds(*reinterpret_cast<int const*>(value));
		if(nvalue != g_settings.discover_guide_interval) {

			// Reschedule the discover_guide_task to execute at the specified interval from now
			g_settings.discover_guide_interval = nvalue;
			g_scheduler.remove(discover_guide_task);
			g_scheduler.add(now + std::chrono::seconds(nvalue), discover_guide_task);
			log_notice(__func__, ": setting discover_guide_interval changed -- rescheduling task to initiate in ", nvalue, " seconds");
		}
	}

	// discover_recordings_interval
	//
	else if(strcmp(name, "discover_recordings_interval") == 0) {

		int nvalue = interval_enum_to_seconds(*reinterpret_cast<int const*>(value));
		if(nvalue != g_settings.discover_recordings_interval) {

			// Reschedule the discover_recordings_task to execute at the specified interval from now
			g_settings.discover_recordings_interval = nvalue;
			g_scheduler.remove(discover_recordings_task);
			g_scheduler.add(now + std::chrono::seconds(nvalue), discover_recordings_task);
			log_notice(__func__, ": setting discover_recordings_interval changed -- rescheduling task to initiate in ", nvalue, " seconds");
		}
	}

	// discover_recordingrules_interval
	//
	else if(strcmp(name, "discover_recordingrules_interval") == 0) {

		int nvalue = interval_enum_to_seconds(*reinterpret_cast<int const*>(value));
		if(nvalue != g_settings.discover_recordingrules_interval) {

			// Reschedule the discover_recordingrules_task to execute at the specified interval from now
			g_settings.discover_recordingrules_interval = nvalue;
			g_scheduler.remove(discover_recordingrules_task);
			g_scheduler.add(now + std::chrono::seconds(nvalue), discover_recordingrules_task);
			log_notice(__func__, ": setting discover_recordingrules_interval changed -- rescheduling task to initiate in ", nvalue, " seconds");
		}
	}

	// discover_episodes_interval
	//
	else if(strcmp(name, "discover_episodes_interval") == 0) {

		int nvalue = interval_enum_to_seconds(*reinterpret_cast<int const*>(value));
		if(nvalue != g_settings.discover_episodes_interval) {

			// Reschedule the discover_episodes_task to execute at the specified interval from now
			g_settings.discover_episodes_interval = nvalue;
			g_scheduler.remove(discover_episodes_task);
			g_scheduler.add(now + std::chrono::seconds(nvalue), discover_episodes_task);
			log_notice(__func__, ": setting discover_episodes_interval changed -- rescheduling task to initiate in ", nvalue, " seconds");
		}
	}

	// use_direct_tuning
	//
	else if(strcmp(name, "use_direct_tuning") == 0) {

		bool bvalue = *reinterpret_cast<bool const*>(value);
		if(bvalue != g_settings.use_direct_tuning) {

			g_settings.use_direct_tuning = bvalue;
			log_notice(__func__, ": setting use_direct_tuning changed to ", (bvalue) ? "true" : "false");
		}
	}

	// startup_discovery_task_delay
	//
	else if(strcmp(name, "startup_discovery_task_delay") == 0) {

		int nvalue = *reinterpret_cast<int const*>(value);
		if(nvalue != g_settings.startup_discovery_task_delay) {

			g_settings.startup_discovery_task_delay = nvalue;
			log_notice(__func__, ": setting startup_discovery_task_delay changed to ", nvalue, " seconds");
		}
	}

	// stream_read_minimum_byte_count
	//
	else if(strcmp(name, "stream_read_minimum_byte_count") == 0) {

		int nvalue = mincount_enum_to_bytes(*reinterpret_cast<int const*>(value));
		if(nvalue != g_settings.stream_read_minimum_byte_count) {

			g_settings.stream_read_minimum_byte_count = nvalue;
			log_notice(__func__, ": setting stream_read_minimum_byte_count changed to ", nvalue, " bytes");
		}
	}

	// stream_read_timeout
	//
	else if(strcmp(name, "stream_read_timeout") == 0) {

		int nvalue = *reinterpret_cast<int const*>(value);
		if(nvalue != g_settings.stream_read_timeout) {

			g_settings.stream_read_timeout = nvalue;
			log_notice(__func__, ": setting stream_read_timeout changed to ", nvalue, " milliseconds");
		}
	}

	// stream_ring_buffer_size
	//
	else if(strcmp(name, "stream_ring_buffer_size") == 0) {

		int nvalue = ringbuffersize_enum_to_bytes(*reinterpret_cast<int const*>(value));
		if(nvalue != g_settings.stream_ring_buffer_size) {

			g_settings.stream_ring_buffer_size = nvalue;
			log_notice(__func__, ": setting stream_ring_buffer_size changed to ", nvalue, " bytes");
		}
	}

	return ADDON_STATUS_OK;
}

//---------------------------------------------------------------------------
// KODI PVR ADDON ENTRY POINTS
//---------------------------------------------------------------------------

//---------------------------------------------------------------------------
// GetAddonCapabilities
//
// Get the list of features that this add-on provides
//
// Arguments:
//
//	capabilities	- Capabilities structure to fill out

PVR_ERROR GetAddonCapabilities(PVR_ADDON_CAPABILITIES *capabilities)
{
	if(capabilities == nullptr) return PVR_ERROR::PVR_ERROR_INVALID_PARAMETERS;

	*capabilities = g_capabilities;
	return PVR_ERROR::PVR_ERROR_NO_ERROR;
}

//---------------------------------------------------------------------------
// GetBackendName
//
// Get the name reported by the backend that will be displayed in the UI
//
// Arguments:
//
//	NONE

char const* GetBackendName(void)
{
	return VERSION_PRODUCTNAME_ANSI;
}

//---------------------------------------------------------------------------
// GetBackendVersion
//
// Get the version string reported by the backend that will be displayed in the UI
//
// Arguments:
//
//	NONE

char const* GetBackendVersion(void)
{
	return VERSION_VERSION3_ANSI;
}

//---------------------------------------------------------------------------
// GetConnectionString
//
// Get the connection string reported by the backend that will be displayed in the UI
//
// Arguments:
//
//	NONE

char const* GetConnectionString(void)
{
	return "my.hdhomerun.com";
}

//---------------------------------------------------------------------------
// GetDriveSpace
//
// Get the disk space reported by the backend (if supported)
//
// Arguments:
//
//	total		- The total disk space in bytes
//	used		- The used disk space in bytes

PVR_ERROR GetDriveSpace(long long* /*total*/, long long* /*used*/)
{
	// The HDHomeRun Storage engine reports free space, but not total space which isn't
	// handled well by Kodi.  Disable this for now, but there is a routine available in
	// the database layer already to get the free space -- get_available_storage_space()
	//
	// Prior implementation:
	//
	// *used = 0;
	// *total = (get_available_storage_space(g_db) >> 10);

	return PVR_ERROR::PVR_ERROR_NOT_IMPLEMENTED;
}

//---------------------------------------------------------------------------
// CallMenuHook
//
// Call one of the menu hooks (if supported)
//
// Arguments:
//
//	menuhook	- The hook to call
//	item		- The selected item for which the hook is called

PVR_ERROR CallMenuHook(PVR_MENUHOOK const& menuhook, PVR_MENUHOOK_DATA const& item)
{
	assert(g_pvr);

	// Get the current time to reschedule tasks as requested
	std::chrono::time_point<std::chrono::system_clock> now = std::chrono::system_clock::now();

	// MENUHOOK_RECORD_DELETENORERECORD
	//
	if((menuhook.iHookId == MENUHOOK_RECORD_DELETENORERECORD) && (item.cat == PVR_MENUHOOK_RECORDING)) {

		// This is a standard deletion; you need at least 2 hooks to get the menu to appear otherwise the
		// user will only see the text "Client actions" in the context menu
		try { delete_recording(connectionpool::handle(g_connpool), item.data.recording.strRecordingId, false); }
		catch(std::exception& ex) { return handle_stdexception(__func__, ex, PVR_ERROR::PVR_ERROR_FAILED); }
		catch(...) { return handle_generalexception(__func__, PVR_ERROR::PVR_ERROR_FAILED); }

		g_pvr->TriggerRecordingUpdate();
		return PVR_ERROR::PVR_ERROR_NO_ERROR;
	}

	// MENUHOOK_RECORD_DELETERERECORD
	//
	else if((menuhook.iHookId == MENUHOOK_RECORD_DELETERERECORD) && (item.cat == PVR_MENUHOOK_RECORDING)) {

		// Delete the recording with the re-record flag set to true
		try { delete_recording(connectionpool::handle(g_connpool), item.data.recording.strRecordingId, true); }
		catch(std::exception& ex) { return handle_stdexception(__func__, ex, PVR_ERROR::PVR_ERROR_FAILED); }
		catch(...) { return handle_generalexception(__func__, PVR_ERROR::PVR_ERROR_FAILED); }

		g_pvr->TriggerRecordingUpdate();
		return PVR_ERROR::PVR_ERROR_NO_ERROR;
	}

	// MENUHOOK_SETTING_SHOWDEVICENAMES
	//
	else if(menuhook.iHookId == MENUHOOK_SETTING_SHOWDEVICENAMES) {

		try {

			// Enumerate all of the device names in the database and build out the text string
			std::string names;
			enumerate_device_names(connectionpool::handle(g_connpool), [&](struct device_name const& device_name) -> void { 

				names.append(std::string(device_name.name) + "\r\n");
			});

			g_gui->Dialog_TextViewer("Discovered Devices", names.c_str());
		}

		catch(std::exception& ex) { return handle_stdexception(__func__, ex, PVR_ERROR::PVR_ERROR_FAILED); }
		catch(...) { return handle_generalexception(__func__, PVR_ERROR::PVR_ERROR_FAILED); }
		
		return PVR_ERROR::PVR_ERROR_NO_ERROR;
	}

	// MENUHOOK_SETTING_TRIGGERDEVICEDISCOVERY
	//
	else if(menuhook.iHookId == MENUHOOK_SETTING_TRIGGERDEVICEDISCOVERY) {

		try {

			log_notice(__func__, ": scheduling device discovery task");
			g_scheduler.remove(discover_devices_task);
			g_scheduler.add(now, discover_devices_task);
		}

		catch(std::exception& ex) { return handle_stdexception(__func__, ex, PVR_ERROR::PVR_ERROR_FAILED); }
		catch(...) { return handle_generalexception(__func__, PVR_ERROR::PVR_ERROR_FAILED); }
		
		return PVR_ERROR::PVR_ERROR_NO_ERROR;
	}

	// MENUHOOK_SETTING_TRIGGERLINEUPDISCOVERY
	//
	else if(menuhook.iHookId == MENUHOOK_SETTING_TRIGGERLINEUPDISCOVERY) {

		try {

			log_notice(__func__, ": scheduling lineup discovery task");
			g_scheduler.remove(discover_lineups_task);
			g_scheduler.add(now, discover_lineups_task);
		}

		catch(std::exception& ex) { return handle_stdexception(__func__, ex, PVR_ERROR::PVR_ERROR_FAILED); }
		catch(...) { return handle_generalexception(__func__, PVR_ERROR::PVR_ERROR_FAILED); }
		
		return PVR_ERROR::PVR_ERROR_NO_ERROR;
	}

	// MENUHOOK_SETTING_TRIGGERGUIDEDISCOVERY
	//
	else if(menuhook.iHookId == MENUHOOK_SETTING_TRIGGERGUIDEDISCOVERY) {

		try {

			log_notice(__func__, ": scheduling guide discovery task");
			g_scheduler.remove(discover_guide_task);
			g_scheduler.add(now, discover_guide_task);
		}

		catch(std::exception& ex) { return handle_stdexception(__func__, ex, PVR_ERROR::PVR_ERROR_FAILED); }
		catch(...) { return handle_generalexception(__func__, PVR_ERROR::PVR_ERROR_FAILED); }
		
		return PVR_ERROR::PVR_ERROR_NO_ERROR;
	}

	// MENUHOOK_SETTING_TRIGGERRECORDINGDISCOVERY
	//
	else if(menuhook.iHookId == MENUHOOK_SETTING_TRIGGERRECORDINGDISCOVERY) {

		try {

			log_notice(__func__, ": scheduling recording discovery task");
			g_scheduler.remove(discover_recordings_task);
			g_scheduler.add(now, discover_recordings_task);
		}

		catch(std::exception& ex) { return handle_stdexception(__func__, ex, PVR_ERROR::PVR_ERROR_FAILED); }
		catch(...) { return handle_generalexception(__func__, PVR_ERROR::PVR_ERROR_FAILED); }
		
		return PVR_ERROR::PVR_ERROR_NO_ERROR;
	}

	// MENUHOOK_SETTING_TRIGGERRECORDINGRULEDISCOVERY
	//
	else if(menuhook.iHookId == MENUHOOK_SETTING_TRIGGERRECORDINGRULEDISCOVERY) {

		try {

			log_notice(__func__, ": scheduling recording rule discovery task");
			g_scheduler.remove(discover_recordingrules_task);
			g_scheduler.add(now, discover_recordingrules_task);
		}

		catch(std::exception& ex) { return handle_stdexception(__func__, ex, PVR_ERROR::PVR_ERROR_FAILED); }
		catch(...) { return handle_generalexception(__func__, PVR_ERROR::PVR_ERROR_FAILED); }
		
		return PVR_ERROR::PVR_ERROR_NO_ERROR;
	}

	// MENUHOOK_SETTING_RESETDATABASE
	//
	else if(menuhook.iHookId == MENUHOOK_SETTING_RESETDATABASE) {

		scalar_condition<bool>	cancel{false};		// Dummy cancellation flag for tasks

		try {

			log_notice(__func__, ": clearing database and rescheduling all discovery tasks");

			g_scheduler.stop();					// Stop the task scheduler
			g_scheduler.clear();				// Clear all pending tasks

			// Clear the database using an automatically scoped connection
			clear_database(connectionpool::handle(g_connpool));

			// Schedule a startup discovery to occur and reload the entire database from scratch;
			// the startup task is more efficient with the callbacks to Kodi than the periodic ones
			log_notice(__func__, ": scheduling startup discovery task");
			g_scheduler.add(now, discover_startup_task);
	
			g_scheduler.start();				// Restart the task scheduler
		}

		catch(std::exception& ex) { return handle_stdexception(__func__, ex, PVR_ERROR::PVR_ERROR_FAILED); }
		catch(...) { return handle_generalexception(__func__, PVR_ERROR::PVR_ERROR_FAILED); }
		
		return PVR_ERROR::PVR_ERROR_NO_ERROR;
	}

	// MENUHOOK_CHANNEL_DISABLE
	//
	else if((menuhook.iHookId == MENUHOOK_CHANNEL_DISABLE) && (item.cat == PVR_MENUHOOK_CAT::PVR_MENUHOOK_CHANNEL)) {

		try { 
			
			union channelid channelid;
			channelid.value = item.data.channel.iUniqueId;

			// Set the channel visibility to disabled (red x) and kick off a lineup discovery task
			set_channel_visibility(connectionpool::handle(g_connpool), channelid, channel_visibility::disabled);
		
			log_notice(__func__, ": channel ", item.data.channel.strChannelName, " disabled; scheduling lineup discovery task");
			g_scheduler.remove(discover_lineups_task);
			g_scheduler.add(now, discover_lineups_task);
		}

		catch(std::exception& ex) { return handle_stdexception(__func__, ex, PVR_ERROR::PVR_ERROR_FAILED); }
		catch(...) { return handle_generalexception(__func__, PVR_ERROR::PVR_ERROR_FAILED); }

		return PVR_ERROR::PVR_ERROR_NO_ERROR;
	}

	// MENUHOOK_CHANNEL_ADDFAVORITE
	//
	else if((menuhook.iHookId == MENUHOOK_CHANNEL_ADDFAVORITE) && (item.cat == PVR_MENUHOOK_CAT::PVR_MENUHOOK_CHANNEL)) {

		try { 
			
			union channelid channelid;
			channelid.value = item.data.channel.iUniqueId;

			// Set the channel visibility to favorite (yellow star) and kick off a lineup discovery task
			set_channel_visibility(connectionpool::handle(g_connpool), channelid, channel_visibility::favorite);
		
			log_notice(__func__, ": channel ", item.data.channel.strChannelName, " added as favorite; scheduling lineup discovery task");
			g_scheduler.remove(discover_lineups_task);
			g_scheduler.add(now, discover_lineups_task);
		}

		catch(std::exception& ex) { return handle_stdexception(__func__, ex, PVR_ERROR::PVR_ERROR_FAILED); }
		catch(...) { return handle_generalexception(__func__, PVR_ERROR::PVR_ERROR_FAILED); }

		return PVR_ERROR::PVR_ERROR_NO_ERROR;
	}

	// MENUHOOK_CHANNEL_REMOVEFAVORITE
	//
	else if((menuhook.iHookId == MENUHOOK_CHANNEL_REMOVEFAVORITE) && (item.cat == PVR_MENUHOOK_CAT::PVR_MENUHOOK_CHANNEL)) {

		try { 
			
			union channelid channelid;
			channelid.value = item.data.channel.iUniqueId;

			// Set the channel visibility to favorite (gray star) and kick off a lineup discovery task
			set_channel_visibility(connectionpool::handle(g_connpool), channelid, channel_visibility::enabled);
		
			log_notice(__func__, ": channel ", item.data.channel.strChannelName, " removed from favorites; scheduling lineup discovery task");
			g_scheduler.remove(discover_lineups_task);
			g_scheduler.add(now, discover_lineups_task);
		}

		catch(std::exception& ex) { return handle_stdexception(__func__, ex, PVR_ERROR::PVR_ERROR_FAILED); }
		catch(...) { return handle_generalexception(__func__, PVR_ERROR::PVR_ERROR_FAILED); }

		return PVR_ERROR::PVR_ERROR_NO_ERROR;
	}

	return PVR_ERROR::PVR_ERROR_NOT_IMPLEMENTED;
}

//---------------------------------------------------------------------------
// GetEPGForChannel
//
// Request the EPG for a channel from the backend
//
// Arguments:
//
//	handle		- Handle to pass to the callback method
//	channel		- The channel to get the EPG table for
//	start		- Get events after this time (UTC)
//	end			- Get events before this time (UTC)

PVR_ERROR GetEPGForChannel(ADDON_HANDLE handle, PVR_CHANNEL const& channel, time_t start, time_t end)
{
	assert(g_pvr);

	if(handle == nullptr) return PVR_ERROR::PVR_ERROR_INVALID_PARAMETERS;

	// Retrieve the channel identifier from the PVR_CHANNEL structure
	union channelid channelid;
	channelid.value = channel.iUniqueId;

	//
	// NOTE: This does not cache all of the enumerated guide data locally before
	// transferring it over to Kodi, it's done realtime to reduce the heap footprint
	// and allow Kodi to process the entries as they are generated
	//

	try {

		// Create a copy of the current addon settings structure
		struct addon_settings settings = copy_settings();

		// Pull a database connection out from the connection pool
		connectionpool::handle dbhandle(g_connpool);

		// Enumerate all of the guide entries in the database for this channel and time frame
		enumerate_guideentries(dbhandle, channelid, start, end, [&](struct guideentry const& item) -> void {

			EPG_TAG	epgtag;										// EPG_TAG to be transferred to Kodi
			memset(&epgtag, 0, sizeof(EPG_TAG));				// Initialize the structure

			// iUniqueBroadcastId (required)
			epgtag.iUniqueBroadcastId = static_cast<unsigned int>(item.starttime);

			// iUniqueChannelId (required)
			epgtag.iUniqueChannelId = item.channelid;

			// strTitle (required)
			if(item.title == nullptr) return;
			epgtag.strTitle = item.title;

			// startTime (required)
			epgtag.startTime = item.starttime;

			// endTime (required)
			epgtag.endTime = item.endtime;

			// strPlot
			if(item.synopsis != nullptr) epgtag.strPlot = item.synopsis;

			// iYear
			epgtag.iYear = item.year;

			// strIconPath
			if(item.iconurl != nullptr) epgtag.strIconPath = item.iconurl;

			// iGenreType
			epgtag.iGenreType = (settings.use_backend_genre_strings) ? EPG_GENRE_USE_STRING : item.genretype;

			// strGenreDescription
			if(settings.use_backend_genre_strings) epgtag.strGenreDescription = item.genres;

			// firstAired
			epgtag.firstAired = item.originalairdate;

			// iSeriesNumber
			epgtag.iSeriesNumber = item.seriesnumber;

			// iEpisodeNumber
			epgtag.iEpisodeNumber = item.episodenumber;

			// iEpisodePartNumber
			epgtag.iEpisodePartNumber = -1;

			// strEpisodeName
			if(item.episodename != nullptr) epgtag.strEpisodeName = item.episodename;

			// iFlags
			epgtag.iFlags = EPG_TAG_FLAG_IS_SERIES;

			// strSeriesLink
			epgtag.strSeriesLink = item.seriesid;

			// Transfer the EPG_TAG structure over to Kodi
			g_pvr->TransferEpgEntry(handle, &epgtag);
		});
	}
	
	catch(std::exception& ex) { return handle_stdexception(__func__, ex, PVR_ERROR::PVR_ERROR_FAILED); }
	catch(...) { return handle_generalexception(__func__, PVR_ERROR::PVR_ERROR_FAILED); }

	return PVR_ERROR::PVR_ERROR_NO_ERROR;
}

//---------------------------------------------------------------------------
// IsEPGTagRecordable
//
// Check if the given EPG tag can be recorded
//
// Arguments:
//
//	tag			- EPG tag to be checked
//	recordable	- Flag indicating if the EPG tag can be recorded

PVR_ERROR IsEPGTagRecordable(EPG_TAG const* /*tag*/, bool* /*recordable*/)
{
	return PVR_ERROR_NOT_IMPLEMENTED;
}

//---------------------------------------------------------------------------
// IsEPGTagPlayable
//
// Check if the given EPG tag can be played
//
// Arguments:
//
//	tag			- EPG tag to be checked
//	playable	- Flag indicating if the EPG tag can be played

PVR_ERROR IsEPGTagPlayable(EPG_TAG const* /*tag*/, bool* /*playable*/)
{
	return PVR_ERROR_NOT_IMPLEMENTED;
}

//---------------------------------------------------------------------------
// GetEPGTagStreamProperties
//
// Get the stream properties for an epg tag from the backend
//
// Arguments:
//
//	tag			- EPG tag for which to retrieve the properties
//	props		- Array in which to set the stream properties
//	numprops	- Number of properties returned by this function

PVR_ERROR GetEPGTagStreamProperties(EPG_TAG const* /*tag*/, PVR_NAMED_VALUE* /*props*/, unsigned int* /*numprops*/)
{
	return PVR_ERROR_NOT_IMPLEMENTED;
}

//---------------------------------------------------------------------------
// GetChannelGroupsAmount
//
// Get the total amount of channel groups on the backend if it supports channel groups
//
// Arguments:
//
//	NONE

int GetChannelGroupsAmount(void)
{
	return 3;		// "Favorite Channels", "HD Channels" and "SD Channels"
}

//---------------------------------------------------------------------------
// GetChannelGroups
//
// Request the list of all channel groups from the backend if it supports channel groups
//
// Arguments:
//
//	handle		- Handle to pass to the callack method
//	radio		- True to get radio groups, false to get TV channel groups

PVR_ERROR GetChannelGroups(ADDON_HANDLE handle, bool radio)
{
	assert(g_pvr);

	if(handle == nullptr) return PVR_ERROR::PVR_ERROR_INVALID_PARAMETERS;

	// The PVR doesn't support radio channel groups
	if(radio) return PVR_ERROR::PVR_ERROR_NO_ERROR;

	PVR_CHANNEL_GROUP group;
	memset(&group, 0, sizeof(PVR_CHANNEL_GROUP));

	// Favorite Channels
	snprintf(group.strGroupName, std::extent<decltype(group.strGroupName)>::value, "Favorite Channels");
	g_pvr->TransferChannelGroup(handle, &group);

	// HD Channels
	snprintf(group.strGroupName, std::extent<decltype(group.strGroupName)>::value, "HD Channels");
	g_pvr->TransferChannelGroup(handle, &group);

	// SD Channels
	snprintf(group.strGroupName, std::extent<decltype(group.strGroupName)>::value, "SD Channels");
	g_pvr->TransferChannelGroup(handle, &group);

	return PVR_ERROR::PVR_ERROR_NO_ERROR;
}

//---------------------------------------------------------------------------
// GetChannelGroupMembers
//
// Request the list of all channel groups from the backend if it supports channel groups
//
// Arguments:
//
//	handle		- Handle to pass to the callack method
//	group		- The group to get the members for

PVR_ERROR GetChannelGroupMembers(ADDON_HANDLE handle, PVR_CHANNEL_GROUP const& group)
{
	assert(g_pvr);

	if(handle == nullptr) return PVR_ERROR::PVR_ERROR_INVALID_PARAMETERS;

	// Determine which group enumerator to use for the operation, there are only
	// three to choose from: "Favorite Channels", "HD Channels" and "SD Channels"
	std::function<void(sqlite3*, bool, enumerate_channelids_callback)> enumerator = nullptr;
	if(strcmp(group.strGroupName, "Favorite Channels") == 0) enumerator = enumerate_favorite_channelids;
	else if(strcmp(group.strGroupName, "HD Channels") == 0) enumerator = enumerate_hd_channelids;
	else if(strcmp(group.strGroupName, "SD Channels") == 0) enumerator = enumerate_sd_channelids;

	// If neither enumerator was selected, there isn't any work to do here
	if(enumerator == nullptr) return PVR_ERROR::PVR_ERROR_NO_ERROR;

	// Collect all of the PVR_CHANNEL_GROUP_MEMBER structures locally so that the database
	// connection isn't open any longer than necessary
	std::vector<PVR_CHANNEL_GROUP_MEMBER> members;

	try {

		// Create a copy of the current addon settings structure
		struct addon_settings settings = copy_settings();

		// Pull a database connection out from the connection pool
		connectionpool::handle dbhandle(g_connpool);

		// Enumerate all of the channels in the specified group
		enumerator(dbhandle, settings.show_drm_protected_channels, [&](union channelid const& item) -> void {

			PVR_CHANNEL_GROUP_MEMBER member;						// PVR_CHANNEL_GROUP_MEMORY to send
			memset(&member, 0, sizeof(PVR_CHANNEL_GROUP_MEMBER));	// Initialize the structure

			// strGroupName (required)
			snprintf(member.strGroupName, std::extent<decltype(member.strGroupName)>::value, "%s", group.strGroupName);

			// iChannelUniqueId (required)
			member.iChannelUniqueId = item.value;

			// Copy the PVR_CHANNEL_GROUP_MEMBER into the local vector<>
			members.push_back(std::move(member));
		});
	}

	catch(std::exception& ex) { return handle_stdexception(__func__, ex, PVR_ERROR::PVR_ERROR_FAILED); }
	catch(...) { return handle_generalexception(__func__, PVR_ERROR::PVR_ERROR_FAILED); }

	// Transfer all of the PVR_CHANNEL_GROUP_MEMBER structures over to Kodi
	try { for(auto const& it : members) g_pvr->TransferChannelGroupMember(handle, &it); }
	catch(std::exception& ex) { return handle_stdexception(__func__, ex, PVR_ERROR::PVR_ERROR_FAILED); }
	catch(...) { return handle_generalexception(__func__, PVR_ERROR::PVR_ERROR_FAILED); }

	return PVR_ERROR::PVR_ERROR_NO_ERROR;
}

//---------------------------------------------------------------------------
// OpenDialogChannelScan
//
// Show the channel scan dialog if this backend supports it
//
// Arguments:
//
//	NONE

PVR_ERROR OpenDialogChannelScan(void)
{
	return PVR_ERROR::PVR_ERROR_NOT_IMPLEMENTED;
}

//---------------------------------------------------------------------------
// GetChannelsAmount
//
// The total amount of channels on the backend, or -1 on error
//
// Arguments:
//
//	NONE

int GetChannelsAmount(void)
{
	try { return get_channel_count(connectionpool::handle(g_connpool), copy_settings().show_drm_protected_channels); }
	catch(std::exception& ex) { return handle_stdexception(__func__, ex, -1); }
	catch(...) { return handle_generalexception(__func__, -1); }
}

//---------------------------------------------------------------------------
// GetChannels
//
// Request the list of all channels from the backend
//
// Arguments:
//
//	handle		- Handle to pass to the callback method
//	radio		- True to get radio channels, false to get TV channels

PVR_ERROR GetChannels(ADDON_HANDLE handle, bool radio)
{
	assert(g_pvr);		

	if(handle == nullptr) return PVR_ERROR::PVR_ERROR_INVALID_PARAMETERS;

	// The PVR doesn't support radio channels
	if(radio) return PVR_ERROR::PVR_ERROR_NO_ERROR;

	// Collect all of the PVR_CHANNEL structures locally so that the database
	// connection isn't open any longer than necessary
	std::vector<PVR_CHANNEL> channels;

	try {

		// Create a copy of the current addon settings structure
		struct addon_settings settings = copy_settings();

		// Pull a database connection out from the connection pool
		connectionpool::handle dbhandle(g_connpool);

		// Enumerate all of the channels in the database
		enumerate_channels(dbhandle, settings.prepend_channel_numbers, settings.show_drm_protected_channels, [&](struct channel const& item) -> void {

			PVR_CHANNEL channel;								// PVR_CHANNEL to be transferred to Kodi
			memset(&channel, 0, sizeof(PVR_CHANNEL));			// Initialize the structure

			// iUniqueId (required)
			channel.iUniqueId = item.channelid.value;

			// bIsRadio (required)
			channel.bIsRadio = false;

			// iChannelNumber
			channel.iChannelNumber = item.channelid.parts.channel;

			// iSubChannelNumber
			channel.iSubChannelNumber = item.channelid.parts.subchannel;

			// strChannelName
			if(item.channelname != nullptr) snprintf(channel.strChannelName, std::extent<decltype(channel.strChannelName)>::value, "%s", item.channelname);

			// strInputFormat
			snprintf(channel.strInputFormat, std::extent<decltype(channel.strInputFormat)>::value, "video/mp2t");

			// iEncryptionSystem
			//
			// This is used to flag a channel as DRM to prevent it from being streamed
			channel.iEncryptionSystem = (item.drm) ? std::numeric_limits<unsigned int>::max() : 0;

			// strIconPath
			if(item.iconurl != nullptr) snprintf(channel.strIconPath, std::extent<decltype(channel.strIconPath)>::value, "%s", item.iconurl);

			// Copy the PVR_CHANNEL structure into the local vector<>
			channels.push_back(channel);
		});
	}
	
	catch(std::exception& ex) { return handle_stdexception(__func__, ex, PVR_ERROR::PVR_ERROR_FAILED); }
	catch(...) { return handle_generalexception(__func__, PVR_ERROR::PVR_ERROR_FAILED); }

	// Transfer all of the PVR_CHANNEL structures over to Kodi
	try { for(auto const& it : channels) g_pvr->TransferChannelEntry(handle, &it); }
	catch(std::exception& ex) { return handle_stdexception(__func__, ex, PVR_ERROR::PVR_ERROR_FAILED); }
	catch(...) { return handle_generalexception(__func__, PVR_ERROR::PVR_ERROR_FAILED); }

	return PVR_ERROR::PVR_ERROR_NO_ERROR;
}

//---------------------------------------------------------------------------
// DeleteChannel
//
// Delete a channel from the backend
//
// Arguments:
//
//	channel		- The channel to delete

PVR_ERROR DeleteChannel(PVR_CHANNEL const& /*channel*/)
{
	return PVR_ERROR::PVR_ERROR_NOT_IMPLEMENTED;
}

//---------------------------------------------------------------------------
// RenameChannel
//
// Rename a channel on the backend
//
// Arguments:
//
//	channel		- The channel to rename, containing the new channel name

PVR_ERROR RenameChannel(PVR_CHANNEL const& /*channel*/)
{
	return PVR_ERROR::PVR_ERROR_NOT_IMPLEMENTED;
}

//---------------------------------------------------------------------------
// MoveChannel
//
// Move a channel to another channel number on the backend
//
// Arguments:
//
//	channel		- The channel to move, containing the new channel number

PVR_ERROR MoveChannel(PVR_CHANNEL const& /*channel*/)
{
	return PVR_ERROR::PVR_ERROR_NOT_IMPLEMENTED;
}

//---------------------------------------------------------------------------
// OpenDialogChannelSettings
//
// Show the channel settings dialog, if supported by the backend
//
// Arguments:
//
//	channel		- The channel to show the dialog for

PVR_ERROR OpenDialogChannelSettings(PVR_CHANNEL const& /*channel*/)
{
	return PVR_ERROR::PVR_ERROR_NOT_IMPLEMENTED;
}

//---------------------------------------------------------------------------
// OpenDialogChannelAdd
//
// Show the dialog to add a channel on the backend, if supported by the backend
//
// Arguments:
//
//	channel		- The channel to add

PVR_ERROR OpenDialogChannelAdd(PVR_CHANNEL const& /*channel*/)
{
	return PVR_ERROR::PVR_ERROR_NOT_IMPLEMENTED;
}

//---------------------------------------------------------------------------
// GetRecordingsAmount
//
// The total amount of recordings on the backend or -1 on error
//
// Arguments:
//
//	deleted		- if set return deleted recording

int GetRecordingsAmount(bool deleted)
{
	if(deleted) return 0;			// Deleted recordings aren't supported

	try { return get_recording_count(connectionpool::handle(g_connpool)); }
	catch(std::exception& ex) { return handle_stdexception(__func__, ex, -1); }
	catch(...) { return handle_generalexception(__func__, -1); }
}

//---------------------------------------------------------------------------
// GetRecordings
//
// Request the list of all recordings from the backend, if supported
//
// Arguments:
//
//	handle		- Handle to pass to the callback method
//	deleted		- If set return deleted recording

PVR_ERROR GetRecordings(ADDON_HANDLE handle, bool deleted)
{
	assert(g_pvr);				

	if(handle == nullptr) return PVR_ERROR::PVR_ERROR_INVALID_PARAMETERS;

	// The PVR doesn't support tracking deleted recordings
	if(deleted) return PVR_ERROR::PVR_ERROR_NO_ERROR;

	// Collect all of the PVR_RECORDING structures locally so that the database
	// connection isn't open any longer than necessary
	std::vector<PVR_RECORDING> recordings;

	try {

		// Create a copy of the current addon settings structure
		struct addon_settings settings = copy_settings();

		// Pull a database connection out from the connection pool
		connectionpool::handle dbhandle(g_connpool);

		// Enumerate all of the recordings in the database
		enumerate_recordings(dbhandle, settings.use_episode_number_as_title, [&](struct recording const& item) -> void {

			PVR_RECORDING recording;							// PVR_RECORDING to be transferred to Kodi
			memset(&recording, 0, sizeof(PVR_RECORDING));		// Initialize the structure

			// strRecordingId (required)
			if(item.recordingid == nullptr) return;
			snprintf(recording.strRecordingId, std::extent<decltype(recording.strRecordingId)>::value, "%s", item.recordingid);
			
			// strTitle (required)
			if(item.title == nullptr) return;
			snprintf(recording.strTitle, std::extent<decltype(recording.strTitle)>::value, "%s", item.title);

			// strEpisodeName
			if(item.episodename != nullptr) snprintf(recording.strEpisodeName, std::extent<decltype(recording.strEpisodeName)>::value, "%s", item.episodename);

			// iSeriesNumber
			recording.iSeriesNumber = item.seriesnumber;
			 
			// iEpisodeNumber
			recording.iEpisodeNumber = item.episodenumber;

			// iYear
			recording.iYear = item.year;

			// strDirectory
			if(item.directory != nullptr) {
				
				// Special case: "movie" --> #30402
				if(strcasecmp(item.directory, "movie") == 0) 
					snprintf(recording.strDirectory, std::extent<decltype(recording.strDirectory)>::value, "%s", g_addon->GetLocalizedString(30402));

				// Special case: "sport" --> #30403
				else if(strcasecmp(item.directory, "sport") == 0)
					snprintf(recording.strDirectory, std::extent<decltype(recording.strDirectory)>::value, "%s", g_addon->GetLocalizedString(30403));

				else snprintf(recording.strDirectory, std::extent<decltype(recording.strDirectory)>::value, "%s", item.directory);
			}

			// strPlot
			if(item.plot != nullptr) snprintf(recording.strPlot, std::extent<decltype(recording.strPlot)>::value, "%s", item.plot);

			// strChannelName
			if(item.channelname != nullptr) snprintf(recording.strChannelName, std::extent<decltype(recording.strChannelName)>::value, "%s", item.channelname);

			// strThumbnailPath
			if(item.thumbnailpath != nullptr) snprintf(recording.strThumbnailPath, std::extent<decltype(recording.strThumbnailPath)>::value, "%s", item.thumbnailpath);

			// recordingTime
			recording.recordingTime = item.recordingtime;

			// iDuration
			recording.iDuration = item.duration;

			// iLastPlayedPosition
			//
			recording.iLastPlayedPosition = item.lastposition;

			// iChannelUid
			recording.iChannelUid = item.channelid.value;

			// channelType
			recording.channelType = PVR_RECORDING_CHANNEL_TYPE_TV;

			// Copy the PVR_RECORDING structure into the local vector<>
			recordings.push_back(recording);
		});
	}
	
	catch(std::exception& ex) { return handle_stdexception(__func__, ex, PVR_ERROR::PVR_ERROR_FAILED); }
	catch(...) { return handle_generalexception(__func__, PVR_ERROR::PVR_ERROR_FAILED); }

	// Transfer all of the PVR_RECORDING structures over to Kodi
	try { for(auto const& it : recordings) g_pvr->TransferRecordingEntry(handle, &it); }
	catch(std::exception& ex) { return handle_stdexception(__func__, ex, PVR_ERROR::PVR_ERROR_FAILED); }
	catch(...) { return handle_generalexception(__func__, PVR_ERROR::PVR_ERROR_FAILED); }

	return PVR_ERROR::PVR_ERROR_NO_ERROR;
}

//---------------------------------------------------------------------------
// DeleteRecording
//
// Delete a recording on the backend
//
// Arguments:
//
//	recording	- The recording to delete

PVR_ERROR DeleteRecording(PVR_RECORDING const& recording)
{
	try { delete_recording(connectionpool::handle(g_connpool), recording.strRecordingId, false); }
	catch(std::exception& ex) { return handle_stdexception(__func__, ex, PVR_ERROR::PVR_ERROR_FAILED); }
	catch(...) { return handle_generalexception(__func__, PVR_ERROR::PVR_ERROR_FAILED); }

	return PVR_ERROR::PVR_ERROR_NO_ERROR;
}

//---------------------------------------------------------------------------
// UndeleteRecording
//
// Undelete a recording on the backend
//
// Arguments:
//
//	recording	- The recording to undelete

PVR_ERROR UndeleteRecording(PVR_RECORDING const& /*recording*/)
{
	return PVR_ERROR::PVR_ERROR_NOT_IMPLEMENTED;
}

//---------------------------------------------------------------------------
// DeleteAllRecordingsFromTrash
//
// Delete all recordings permanent which in the deleted folder on the backend
//
// Arguments:
//
//	NONE

PVR_ERROR DeleteAllRecordingsFromTrash()
{
	return PVR_ERROR::PVR_ERROR_NOT_IMPLEMENTED;
}

//---------------------------------------------------------------------------
// RenameRecording
//
// Rename a recording on the backend
//
// Arguments:
//
//	recording	- The recording to rename, containing the new name

PVR_ERROR RenameRecording(PVR_RECORDING const& /*recording*/)
{
	return PVR_ERROR::PVR_ERROR_NOT_IMPLEMENTED;
}

//---------------------------------------------------------------------------
// SetRecordingLifetime
//
// Set the lifetime of a recording on the backend
//
// Arguments:
//
//	recording	- The recording to change the lifetime for

PVR_ERROR SetRecordingLifetime(PVR_RECORDING const* /*recording*/)
{
	return PVR_ERROR::PVR_ERROR_NOT_IMPLEMENTED;
}

//---------------------------------------------------------------------------
// SetRecordingPlayCount
//
// Set the play count of a recording on the backend
//
// Arguments:
//
//	recording	- The recording to change the play count
//	playcount	- Play count

PVR_ERROR SetRecordingPlayCount(PVR_RECORDING const& /*recording*/, int /*playcount*/)
{
	return PVR_ERROR::PVR_ERROR_NOT_IMPLEMENTED;
}

//---------------------------------------------------------------------------
// SetRecordingLastPlayedPosition
//
// Set the last watched position of a recording on the backend
//
// Arguments:
//
//	recording			- The recording
//	lastposition		- The last watched position in seconds

PVR_ERROR SetRecordingLastPlayedPosition(PVR_RECORDING const& recording, int lastposition)
{
	try {
		
		set_recording_lastposition(connectionpool::handle(g_connpool), recording.strRecordingId, lastposition);
		return PVR_ERROR::PVR_ERROR_NO_ERROR;
	}

	catch(std::exception& ex) { return handle_stdexception(__func__, ex, PVR_ERROR::PVR_ERROR_FAILED); }
	catch(...) { return handle_generalexception(__func__, PVR_ERROR::PVR_ERROR_FAILED); }
}

//---------------------------------------------------------------------------
// GetRecordingLastPlayedPosition
//
// Retrieve the last watched position of a recording (in seconds) on the backend
//
// Arguments:
//
//	recording	- The recording

int GetRecordingLastPlayedPosition(PVR_RECORDING const& recording)
{
	try { return get_recording_lastposition(connectionpool::handle(g_connpool), recording.strRecordingId); }
	catch(std::exception& ex) { return handle_stdexception(__func__, ex, -1); }
	catch(...) { return handle_generalexception(__func__, -1); }
}

//---------------------------------------------------------------------------
// GetRecordingEdl
//
// Retrieve the edit decision list (EDL) of a recording on the backend
//
// Arguments:
//
//	recording	- The recording
//	edl			- The function has to write the EDL list into this array
//	count		- in: The maximum size of the EDL, out: the actual size of the EDL

PVR_ERROR GetRecordingEdl(PVR_RECORDING const& /*recording*/, PVR_EDL_ENTRY edl[], int* count)
{
	if(count == nullptr) return PVR_ERROR::PVR_ERROR_INVALID_PARAMETERS;
	if((*count) && (edl == nullptr)) return PVR_ERROR::PVR_ERROR_INVALID_PARAMETERS;

	*count = 0;

	return PVR_ERROR::PVR_ERROR_NOT_IMPLEMENTED;
}

//---------------------------------------------------------------------------
// GetTimerTypes
//
// Retrieve the timer types supported by the backend
//
// Arguments:
//
//	types		- The function has to write the definition of the supported timer types into this array
//	count		- in: The maximum size of the list; out: the actual size of the list

PVR_ERROR GetTimerTypes(PVR_TIMER_TYPE types[], int* count)
{
	if(count == nullptr) return PVR_ERROR::PVR_ERROR_INVALID_PARAMETERS;
	if((*count) && (types == nullptr)) return PVR_ERROR::PVR_ERROR_INVALID_PARAMETERS;

	// Only copy up to the maximum size of the array provided by the caller
	*count = std::min(*count, static_cast<int>(std::extent<decltype(g_timertypes)>::value));
	for(int index = 0; index < *count; index++) types[index] = g_timertypes[index];

	return PVR_ERROR::PVR_ERROR_NO_ERROR;
}

//---------------------------------------------------------------------------
// GetTimersAmount
//
// Gets the total amount of timers on the backend or -1 on error
//
// Arguments:
//
//	NONE

int GetTimersAmount(void)
{
	try { 
		
		// Pull a database connection out from the connection pool
		connectionpool::handle dbhandle(g_connpool);

		// Return the sum of the timer rules and the invidual timers themselves
		return get_recordingrule_count(dbhandle) + get_timer_count(dbhandle, g_epgmaxtime); 
	}

	catch(std::exception& ex) { return handle_stdexception(__func__, ex, -1); }
	catch(...) { return handle_generalexception(__func__, -1); }
}

//---------------------------------------------------------------------------
// GetTimers
//
// Request the list of all timers from the backend if supported
//
// Arguments:
//
//	handle		- Handle to pass to the callback method

PVR_ERROR GetTimers(ADDON_HANDLE handle)
{
	time_t					now;			// Current date/time as seconds from epoch

	assert(g_pvr);				

	if(handle == nullptr) return PVR_ERROR::PVR_ERROR_INVALID_PARAMETERS;

	time(&now);								// Get the current date/time for comparison

	// Collect all of the PVR_TIMER structures locally so that the database
	// connection isn't open any longer than necessary
	std::vector<PVR_TIMER> timers;

	try {

		// Pull a database connection out from the connection pool
		connectionpool::handle dbhandle(g_connpool);

		// Enumerate all of the recording rules in the database
		enumerate_recordingrules(dbhandle, [&](struct recordingrule const& item) -> void {

			PVR_TIMER timer;							// PVR_TIMER to be transferred to Kodi
			memset(&timer, 0, sizeof(PVR_TIMER));		// Initialize the structure

			// iClientIndex (required)
			timer.iClientIndex = item.recordingruleid;

			// iClientChannelUid
			timer.iClientChannelUid = (item.channelid.value) ? static_cast<int>(item.channelid.value) : PVR_TIMER_ANY_CHANNEL;

			// startTime
			timer.startTime = (item.type == recordingrule_type::datetimeonly) ? item.datetimeonly : now;

			// bStartAnyTime
			timer.bStartAnyTime = (item.type == recordingrule_type::series);

			// bEndAnyTime
			timer.bEndAnyTime = true;

			// state (required)
			timer.state = PVR_TIMER_STATE_SCHEDULED;

			// iTimerType (required)
			timer.iTimerType = (item.type == recordingrule_type::series) ? timer_type::seriesrule : timer_type::datetimeonlyrule;

			// strTitle (required)
			if(item.title == nullptr) return;
			snprintf(timer.strTitle, std::extent<decltype(timer.strTitle)>::value, "%s", item.title);

			// strEpgSearchString
			snprintf(timer.strEpgSearchString, std::extent<decltype(timer.strEpgSearchString)>::value, "%s", item.title);

			// strSummary
			if(item.synopsis != nullptr) snprintf(timer.strSummary, std::extent<decltype(timer.strSummary)>::value, "%s", item.synopsis);

			// firstDay
			// TODO: This is a hack for datetimeonly rules so that they can show the date.  See comments above.
			if(item.type == recordingrule_type::datetimeonly) timer.firstDay = item.datetimeonly;

			// iPreventDuplicateEpisodes
			if(item.type == recordingrule_type::series) {

				if(item.afteroriginalairdateonly > 0) timer.iPreventDuplicateEpisodes = duplicate_prevention::newonly;
				else if(item.recentonly) timer.iPreventDuplicateEpisodes = duplicate_prevention::recentonly;
				else timer.iPreventDuplicateEpisodes = duplicate_prevention::none;
			}

			// iMarginStart
			timer.iMarginStart = (item.startpadding / 60);

			// iMarginEnd
			timer.iMarginEnd = (item.endpadding / 60);

			// strSeriesLink
			snprintf(timer.strSeriesLink, std::extent<decltype(timer.strSeriesLink)>::value, "%s", item.seriesid);

			// Copy the PVR_TIMER structure into the local vector<>
			timers.push_back(timer);
		});

		// Enumerate all of the timers in the database
		enumerate_timers(dbhandle, g_epgmaxtime, [&](struct timer const& item) -> void {

			PVR_TIMER timer;							// PVR_TIMER to be transferred to Kodi
			memset(&timer, 0, sizeof(PVR_TIMER));		// Initialize the structure

			// iClientIndex (required)
			timer.iClientIndex = item.timerid;

			// iParentClientIndex
			timer.iParentClientIndex = item.recordingruleid;

			// iClientChannelUid
			timer.iClientChannelUid = static_cast<int>(item.channelid.value);

			// startTime
			timer.startTime = item.starttime;

			// endTime
			timer.endTime = item.endtime;

			// state (required)
			if(timer.endTime < now) timer.state = PVR_TIMER_STATE_COMPLETED;
			else if((now >= timer.startTime) && (now <= timer.endTime)) timer.state = PVR_TIMER_STATE_RECORDING;
			else timer.state = PVR_TIMER_STATE_SCHEDULED;

			// iTimerType (required)
			timer.iTimerType = (item.parenttype == recordingrule_type::series) ? timer_type::seriestimer : timer_type::datetimeonlytimer;

			// strTitle (required)
			if(item.title == nullptr) return;
			snprintf(timer.strTitle, std::extent<decltype(timer.strTitle)>::value, "%s", item.title);

			// strSummary
			if(item.synopsis != nullptr) snprintf(timer.strSummary, std::extent<decltype(timer.strSummary)>::value, "%s", item.synopsis);

			// iEpgUid
			timer.iEpgUid = static_cast<unsigned int>(item.starttime);

			// Copy the PVR_TIMER structure into the local vector<>
			timers.push_back(timer);
		});
	}
	
	catch(std::exception& ex) { return handle_stdexception(__func__, ex, PVR_ERROR::PVR_ERROR_FAILED); }
	catch(...) { return handle_generalexception(__func__, PVR_ERROR::PVR_ERROR_FAILED); }

	// Transfer all of the PVR_TIMER structures over to Kodi
	try { for(auto const& it : timers) g_pvr->TransferTimerEntry(handle, &it); }
	catch(std::exception& ex) { return handle_stdexception(__func__, ex, PVR_ERROR::PVR_ERROR_FAILED); }
	catch(...) { return handle_generalexception(__func__, PVR_ERROR::PVR_ERROR_FAILED); }

	return PVR_ERROR::PVR_ERROR_NO_ERROR;
}

//---------------------------------------------------------------------------
// AddTimer
//
// Add a timer on the backend
//
// Arguments:
//
//	timer	- The timer to add

PVR_ERROR AddTimer(PVR_TIMER const& timer)
{
	std::string				seriesid;			// The seriesid for the timer
	time_t					now;				// The current date/time

	assert(g_pvr);

	// Get the current time as a unix timestamp, used to set up AfterOriginalAirdateOnly
	time(&now);

	// Create an initialize a new recordingrule to be passed to the database
	struct recordingrule recordingrule;
	memset(&recordingrule, 0, sizeof(struct recordingrule));

	try {

		// Pull a database connection out from the connection pool
		connectionpool::handle dbhandle(g_connpool);

		// todo: can use strSeriesLink here for EPG timers instead of searching, once that works properly in Kodi.
		// Right now the strSeriesLink information seems to disappear after Kodi is stopped and restarted

		// seriesrule / epgseriesrule --> recordingrule_type::series
		//
		if((timer.iTimerType == timer_type::seriesrule) || (timer.iTimerType == timer_type::epgseriesrule)) {

			// seriesrule --> execute a title match operation against the backend and let the user choose the series they want
			//
			if(timer.iTimerType == timer_type::seriesrule) {
			
				// Generate a vector of all series that are a title match with the requested EPG search string; the
				// selection dialog will be displayed even if there is only one match in order to confirm the result
				std::vector<std::tuple<std::string, std::string>> matches;
				enumerate_series(dbhandle, timer.strEpgSearchString, [&](struct series const& item) -> void { matches.emplace_back(item.title, item.seriesid); });
				
				// No matches found; display an error message to the user and bail out
				if(matches.size() == 0) {

					g_gui->Dialog_OK_ShowAndGetInput("Series Search Failed", "Unable to locate a series with a title that contains:", timer.strEpgSearchString, "");
					return PVR_ERROR::PVR_ERROR_NO_ERROR;
				}

				// Create a vector<> of c-style string pointers to pass into the selection dialog
				std::vector<char const*> items;
				for(auto const& iterator : matches) items.emplace_back(std::get<0>(iterator).c_str());

				// Create and display the selection dialog to get the specific series the user wants
				int result = g_gui->Dialog_Select("Select Series", items.data(), static_cast<unsigned int>(items.size()), 0);
				if(result == -1) return PVR_ERROR::PVR_ERROR_NO_ERROR;
				
				seriesid = std::get<1>(matches[result]);
			}

			// epgseriesrule --> the title must be an exact match with a known series on the backend
			//
			else {

				// Perform an exact-match search against the backend to locate the seriesid
				seriesid = find_seriesid(dbhandle, timer.strEpgSearchString);
				if(seriesid.length() == 0) {
					
					g_gui->Dialog_OK_ShowAndGetInput("Series Search Failed", "Unable to locate a series with a title matching:", timer.strEpgSearchString, "");
					return PVR_ERROR::PVR_ERROR_NO_ERROR;
				}
			}

			// If the seriesid is still not set the operation cannot continue; throw an exception
			if(seriesid.length() == 0) throw string_exception(std::string("could not locate seriesid for title '") + timer.strEpgSearchString + "'");

			// Generate a series recording rule
			recordingrule.type = recordingrule_type::series;
			recordingrule.seriesid = seriesid.c_str();
			recordingrule.channelid.value = (timer.iClientChannelUid == PVR_TIMER_ANY_CHANNEL) ? 0 : timer.iClientChannelUid;
			recordingrule.recentonly = (timer.iPreventDuplicateEpisodes == duplicate_prevention::recentonly);
			recordingrule.afteroriginalairdateonly = (timer.iPreventDuplicateEpisodes == duplicate_prevention::newonly) ? now : 0;
			recordingrule.startpadding = (timer.iMarginStart == 0) ? 30 : timer.iMarginStart * 60;
			recordingrule.endpadding = (timer.iMarginEnd == 0) ? 30 : timer.iMarginEnd * 60;
		}

		// datetimeonlyrule / epgdatetimeonlyrule --> recordingrule_type::datetimeonly
		//
		else if((timer.iTimerType == timer_type::datetimeonlyrule) || (timer.iTimerType == timer_type::epgdatetimeonlyrule)) {

			union channelid channelid;
			channelid.value = (timer.iClientChannelUid == PVR_TIMER_ANY_CHANNEL) ? 0 : timer.iClientChannelUid;

			// Try to find the seriesid for the recording rule by the channel and starttime first, then do a title match
			seriesid = find_seriesid(dbhandle, channelid, timer.startTime);
			if(seriesid.length() == 0) seriesid = find_seriesid(dbhandle, timer.strEpgSearchString);

			// If no match was found, the timer cannot be added; use a dialog box rather than returning an error
			if(seriesid.length() == 0) {
					
				g_gui->Dialog_OK_ShowAndGetInput("Series Search Failed", "Unable to locate a series with a title matching:", timer.strEpgSearchString, "");
				return PVR_ERROR::PVR_ERROR_NO_ERROR;
			}

			recordingrule.type = recordingrule_type::datetimeonly;
			recordingrule.seriesid = seriesid.c_str();
			recordingrule.channelid = channelid;
			recordingrule.datetimeonly = timer.startTime;
			recordingrule.startpadding = (timer.iMarginStart == 0) ? 30 : timer.iMarginStart * 60;
			recordingrule.endpadding = (timer.iMarginEnd == 0) ? 30 : timer.iMarginEnd * 60;
		}

		// any other timer type is not supported
		else return PVR_ERROR::PVR_ERROR_NOT_IMPLEMENTED;

		// Attempt to add the new recording rule to the database/backend service
		add_recordingrule(dbhandle, recordingrule);

		// Force a timer update in Kodi to refresh whatever this did on the backend
		g_pvr->TriggerTimerUpdate();
	}

	catch(std::exception& ex) { return handle_stdexception(__func__, ex, PVR_ERROR::PVR_ERROR_FAILED); }
	catch(...) { return handle_generalexception(__func__, PVR_ERROR::PVR_ERROR_FAILED); }

	return PVR_ERROR::PVR_ERROR_NO_ERROR;
}

//---------------------------------------------------------------------------
// DeleteTimer
//
// Delete a timer on the backend
//
// Arguments:
//
//	timer	- The timer to delete
//	force	- Set to true to delete a timer that is currently recording a program

PVR_ERROR DeleteTimer(PVR_TIMER const& timer, bool /*force*/)
{
	assert(g_pvr);

	try {

		// Pull a database connection out from the connection pool
		connectionpool::handle dbhandle(g_connpool);

		// datetimeonlytimer --> delete the parent rule
		//
		if(timer.iTimerType == timer_type::datetimeonlytimer) delete_recordingrule(dbhandle, timer.iParentClientIndex);

		// seriesrule / datetimeonlyrule --> delete the rule
		//
		else if((timer.iTimerType == timer_type::seriesrule) || (timer.iTimerType == timer_type::datetimeonlyrule))
			delete_recordingrule(dbhandle, timer.iClientIndex);

		// anything else --> not implemented
		//
		else return PVR_ERROR::PVR_ERROR_NOT_IMPLEMENTED;
	}

	catch(std::exception& ex) { return handle_stdexception(__func__, ex, PVR_ERROR::PVR_ERROR_FAILED); }
	catch(...) { return handle_generalexception(__func__, PVR_ERROR::PVR_ERROR_FAILED); }

	// Force a timer update in Kodi to refresh whatever this did on the backend; there
	// shouldn't be a need to update the EPG here as no new series would have been added
	g_pvr->TriggerTimerUpdate();

	return PVR_ERROR::PVR_ERROR_NO_ERROR;	
}

//---------------------------------------------------------------------------
// UpdateTimer
//
// Update the timer information on the backend
//
// Arguments:
//
//	timer	- The timer to update

PVR_ERROR UpdateTimer(PVR_TIMER const& timer)
{
	std::string				seriesid;			// The rule series id
	time_t					now;				// The current date/time

	assert(g_pvr);

	// Get the current time as a unix timestamp, used to set up AfterOriginalAirdateOnly
	time(&now);

	// Create an initialize a new recordingrule to be passed to the database
	struct recordingrule recordingrule;
	memset(&recordingrule, 0, sizeof(struct recordingrule));

	try {

		// Pull a database connection out from the connection pool
		connectionpool::handle dbhandle(g_connpool);

		// seriesrule / epgseriesrule --> recordingrule_type::series
		//
		if((timer.iTimerType == timer_type::seriesrule) || (timer.iTimerType == timer_type::epgseriesrule)) {

			// series rules allow editing of channel, recentonly, afteroriginalairdateonly, startpadding and endpadding
			recordingrule.recordingruleid = timer.iClientIndex;
			recordingrule.type = recordingrule_type::series;
			recordingrule.channelid.value = (timer.iClientChannelUid == PVR_TIMER_ANY_CHANNEL) ? 0 : timer.iClientChannelUid;
			recordingrule.recentonly = (timer.iPreventDuplicateEpisodes == duplicate_prevention::recentonly);
			recordingrule.afteroriginalairdateonly = (timer.iPreventDuplicateEpisodes == duplicate_prevention::newonly) ? now : 0;
			recordingrule.startpadding = (timer.iMarginStart == 0) ? 30 : timer.iMarginStart * 60;
			recordingrule.endpadding = (timer.iMarginEnd == 0) ? 30 : timer.iMarginEnd * 60;
		}

		// datetimeonlyrule / epgdatetimeonlyrule --> recordingrule_type::datetimeonly
		//
		else if((timer.iTimerType == timer_type::datetimeonlyrule) || (timer.iTimerType == timer_type::epgdatetimeonlyrule)) {

			// date/time only rules allow editing of channel, startpadding and endpadding
			recordingrule.recordingruleid = timer.iClientIndex;
			recordingrule.type = recordingrule_type::datetimeonly;
			recordingrule.channelid.value = (timer.iClientChannelUid == PVR_TIMER_ANY_CHANNEL) ? 0 : timer.iClientChannelUid;
			recordingrule.startpadding = (timer.iMarginStart == 0) ? 30 : timer.iMarginStart * 60;
			recordingrule.endpadding = (timer.iMarginEnd == 0) ? 30 : timer.iMarginEnd * 60;
		}

		// any other timer type is not supported
		else return PVR_ERROR::PVR_ERROR_NOT_IMPLEMENTED;

		// Attempt to modify the recording rule in the database/backend service
		modify_recordingrule(dbhandle, recordingrule, seriesid);
	}

	catch(std::exception& ex) { return handle_stdexception(__func__, ex, PVR_ERROR::PVR_ERROR_FAILED); }
	catch(...) { return handle_generalexception(__func__, PVR_ERROR::PVR_ERROR_FAILED); }

	// Force a timer update in Kodi to refresh whatever this did on the backend
	g_pvr->TriggerTimerUpdate();

	return PVR_ERROR::PVR_ERROR_NO_ERROR;
}

//---------------------------------------------------------------------------
// OpenLiveStream
//
// Open a live stream on the backend
//
// Arguments:
//
//	channel		- The channel to stream

bool OpenLiveStream(PVR_CHANNEL const& channel)
{
	char			channelstr[64];			// Channel number as a string
	std::string		streamurl;				// Generated stream URL

	// DRM channels are flagged with a non-zero iEncryptionSystem value to prevent streaming
	if(channel.iEncryptionSystem != 0) {
	
		std::string text = "Channel " + std::string(channel.strChannelName) + " is marked as encrypted and cannot be played";
		g_gui->Dialog_OK_ShowAndGetInput("DRM Protected Content", text.c_str());
		return false;
	}

	// Create a copy of the current addon settings structure
	struct addon_settings settings = copy_settings();

	// If the user wants to pause discovery during live streaming, do so
	if(settings.pause_discovery_while_streaming) g_scheduler.pause();

	// The only interesting thing about PVR_CHANNEL is the channel id
	union channelid channelid;
	channelid.value = channel.iUniqueId;

	// Generate a string version of the channel number for logging purposes
	if(channelid.parts.subchannel == 0) snprintf(channelstr, std::extent<decltype(channelstr)>::value, "%d", channelid.parts.channel);
	else snprintf(channelstr, std::extent<decltype(channelstr)>::value, "%d.%d", channelid.parts.channel, channelid.parts.subchannel);

	try {

		// Pull a database connection out from the connection pool
		connectionpool::handle dbhandle(g_connpool);

		// If direct tuning is disabled, first attempt to generate the stream URL for the specified 
		// channel from the storage engine; if that fails we can fall back to using a tuner directly
		if(settings.use_direct_tuning == false) {
			
			streamurl = get_stream_url(dbhandle, channelid);
			if(streamurl.length() == 0) log_notice(__func__, ": unable to generate storage engine stream URL for channel ", 
				channelstr, " - falling back to a tuner-direct stream");
		}

		// In direct-tuning mode or upon a failure to generate the stream URL for the storage engine
		// a tuner device must be instead be selected to stream the content
		if((settings.use_direct_tuning == true) || (streamurl.length() == 0)) {

			// The available tuners for the channel are captured into a vector<>
			std::vector<std::string> tuners;

			// Create a collection of all the tuners that can possibly stream the requested channel
			enumerate_channeltuners(dbhandle, channelid, [&](char const* item) -> void { tuners.emplace_back(item); });
			if(tuners.size() == 0) throw string_exception("unable to find any possible tuners for channel ", channelstr);
		
			// Select an available tuner from the possibilities and generate the stream URL
			std::string selected = select_tuner(tuners);
			streamurl = get_tuner_stream_url(dbhandle, selected.c_str(), channelid);
		}

		// If none of the above methods yielded a valid URL, we're done here
		if(streamurl.length() == 0) throw string_exception("unable to generate a valid stream URL for channel ", channelstr);

		// Stop and destroy any existing stream instance before opening the new one
		g_dvrstream.reset();

		// Start the new channel stream using the tuning parameters currently specified by the settings
		log_notice(__func__, ": streaming channel ", channelstr, " via url ", streamurl.c_str());
		g_dvrstream = dvrstream::create(streamurl.c_str(), settings.stream_ring_buffer_size, settings.stream_read_minimum_byte_count, settings.stream_read_timeout);

		return true;
	}

	catch(std::exception& ex) { return handle_stdexception(__func__, ex, false); }
	catch(...) { return handle_generalexception(__func__, false); }
}

//---------------------------------------------------------------------------
// CloseLiveStream
//
// Closes the live stream
//
// Arguments:
//
//	NONE

void CloseLiveStream(void)
{
	// Ensure scheduler is running again, it may have been paused
	g_scheduler.resume();

	try {
		
		// If the DVR stream is active, close it normally so exceptions are
		// propagated before destroying it; destructor alone won't throw
		if(g_dvrstream) g_dvrstream->close();
		g_dvrstream.reset();
	}

	catch(std::exception& ex) { return handle_stdexception(__func__, ex); }
	catch(...) { return handle_generalexception(__func__); }
}

//---------------------------------------------------------------------------
// ReadLiveStream
//
// Read from an open live stream
//
// Arguments:
//
//	buffer		- The buffer to store the data in
//	size		- The number of bytes to read into the buffer

int ReadLiveStream(unsigned char* buffer, unsigned int size)
{
	try { return (g_dvrstream) ? static_cast<int>(g_dvrstream->read(buffer, size)) : -1; }
	catch(std::exception& ex) { return handle_stdexception(__func__, ex, -1); }
	catch(...) { return handle_generalexception(__func__, -1); }
}

//---------------------------------------------------------------------------
// SeekLiveStream
//
// Seek in a live stream on a backend that supports timeshifting
//
// Arguments:
//
//	position	- Delta within the stream to seek, relative to whence
//	whence		- Starting position from which to apply the delta

long long SeekLiveStream(long long position, int whence)
{
	if(!g_dvrstream) return -1;				// No active dvrstream instance

	try {

		// Perform the stream seek operation; throw exception on overflow
		unsigned long long result = g_dvrstream->seek(position, whence);
		if(result > static_cast<unsigned long long>(std::numeric_limits<long long>::max())) 
			throw string_exception("seek result exceeds std::numeric_limits<long long>::max()");

		return static_cast<long long>(result);
	}

	catch(std::exception& ex) { return handle_stdexception(__func__, ex, -1); }
	catch(...) { return handle_generalexception(__func__, -1); }
}

//---------------------------------------------------------------------------
// PositionLiveStream
//
// Gets the position in the stream that's currently being read
//
// Arguments:
//
//	NONE

long long PositionLiveStream(void)
{
	try { return (g_dvrstream) ? g_dvrstream->position() : -1; }
	catch(std::exception& ex) { return handle_stdexception(__func__, ex, -1); }
	catch(...) { return handle_generalexception(__func__, -1); }
}

//---------------------------------------------------------------------------
// LengthLiveStream
//
// The total length of the stream that's currently being read
//
// Arguments:
//
//	NONE

long long LengthLiveStream(void)
{
	// Don't implement this function; all live streams are realtime and
	// reporting any length here messes things up when seeking
	return -1;
}

//---------------------------------------------------------------------------
// SignalStatus
//
// Get the signal status of the stream that's currently open
//
// Arguments:
//
//	status		- The signal status

PVR_ERROR SignalStatus(PVR_SIGNAL_STATUS& /*status*/)
{
	return PVR_ERROR::PVR_ERROR_NOT_IMPLEMENTED;
}

//---------------------------------------------------------------------------
// GetDescrambleInfo
//
// Get the descramble information of the stream that's currently open
//
// Arguments:
//
//	descrambleinfo		- Descramble information

PVR_ERROR GetDescrambleInfo(PVR_DESCRAMBLE_INFO* /*descrambleinfo*/)
{
	return PVR_ERROR::PVR_ERROR_NOT_IMPLEMENTED;
}

//---------------------------------------------------------------------------
// GetChannelStreamProperties
//
// Gets the properties for channel streams when the input stream is not handled
//
// Arguments:
//
//	channel		- Channel to get the stream properties for
//	props		- Array of properties to be set for the stream
//	numprops	- Number of properties returned by this function

PVR_ERROR GetChannelStreamProperties(PVR_CHANNEL const* /*channel*/, PVR_NAMED_VALUE* /*props*/, unsigned int* /*numprops*/)
{
	return PVR_ERROR_NOT_IMPLEMENTED;
}

//---------------------------------------------------------------------------
// GetRecordingStreamProperties
//
// Gets the properties for recording streams when the input stream is not handled
//
// Arguments:
//
//	recording	- Recording to get the stream properties for
//	props		- Array of properties to be set for the stream
//	numprops	- Number of properties returned by this function

PVR_ERROR GetRecordingStreamProperties(PVR_RECORDING const* /*recording*/, PVR_NAMED_VALUE* /*props*/, unsigned int* /*numprops*/)
{
	return PVR_ERROR_NOT_IMPLEMENTED;
}

//---------------------------------------------------------------------------
// GetStreamProperties
//
// Get the stream properties of the stream that's currently being read
//
// Arguments:
//
//	properties	- The properties of the currently playing stream

PVR_ERROR GetStreamProperties(PVR_STREAM_PROPERTIES* properties)
{
	if(properties == nullptr) return PVR_ERROR::PVR_ERROR_INVALID_PARAMETERS;

	return PVR_ERROR::PVR_ERROR_NOT_IMPLEMENTED;
}

//---------------------------------------------------------------------------
// OpenRecordedStream
//
// Open a stream to a recording on the backend
//
// Arguments:
//
//	recording	- The recording to open

bool OpenRecordedStream(PVR_RECORDING const& recording)
{
	// Create a copy of the current addon settings structure
	struct addon_settings settings = copy_settings();

	try {

		// Pull a database connection out from the connection pool
		connectionpool::handle dbhandle(g_connpool);

		// Generate the stream URL for the specified channel
		std::string streamurl = get_recording_stream_url(dbhandle, recording.strRecordingId);
		if(streamurl.length() == 0) throw string_exception("unable to determine the URL for specified recording");

		// Stop and destroy any existing stream instance before opening the new one
		g_dvrstream.reset();

		// Start the new recording stream using the tuning parameters currently specified by the settings
		log_notice(__func__, ": streaming recording ", recording.strTitle, " via url ", streamurl.c_str());
		g_dvrstream = dvrstream::create(streamurl.c_str(), settings.stream_ring_buffer_size, settings.stream_read_minimum_byte_count, settings.stream_read_timeout);

		return true;
	}

	catch(std::exception& ex) { return handle_stdexception(__func__, ex, false); }
	catch(...) { return handle_generalexception(__func__, false); }
}

//---------------------------------------------------------------------------
// CloseRecordedStream
//
// Close an open stream from a recording
//
// Arguments:
//
//	NONE

void CloseRecordedStream(void)
{
	// Ensure scheduler is running again, it may have been paused
	g_scheduler.resume();

	try {
		
		// If the DVR stream is active, close it normally so exceptions are
		// propagated before destroying it; destructor alone won't throw
		if(g_dvrstream) g_dvrstream->close();
		g_dvrstream.reset();
	}

	catch(std::exception& ex) { return handle_stdexception(__func__, ex); }
	catch(...) { return handle_generalexception(__func__); }
}

//---------------------------------------------------------------------------
// ReadRecordedStream
//
// Read from a recording
//
// Arguments:
//
//	buffer		- The buffer to store the data in
//	size		- The number of bytes to read into the buffer

int ReadRecordedStream(unsigned char* buffer, unsigned int size)
{
	try { return (g_dvrstream) ? static_cast<int>(g_dvrstream->read(buffer, size)) : -1; }
	catch(std::exception& ex) { return handle_stdexception(__func__, ex, -1); }
	catch(...) { return handle_generalexception(__func__, -1); }
}

//---------------------------------------------------------------------------
// SeekRecordedStream
//
// Seek in a recorded stream
//
// Arguments:
//
//	position	- Delta within the stream to seek, relative to whence
//	whence		- Starting position from which to apply the delta

long long SeekRecordedStream(long long position, int whence)
{
	if(!g_dvrstream) return -1;				// No active dvrstream instance

	try {

		// Perform the stream seek operation; throw exception on overflow
		unsigned long long result = g_dvrstream->seek(position, whence);
		if(result > static_cast<unsigned long long>(std::numeric_limits<long long>::max())) 
			throw string_exception("seek result exceeds std::numeric_limits<long long>::max()");

		return static_cast<long long>(result);
	}

	catch(std::exception& ex) { return handle_stdexception(__func__, ex, -1); }
	catch(...) { return handle_generalexception(__func__, -1); }
}

//---------------------------------------------------------------------------
// PositionRecordedStream
//
// Gets the position in the stream that's currently being read
//
// Arguments:
//
//	NONE

long long PositionRecordedStream(void)
{
	try { return (g_dvrstream) ? g_dvrstream->position() : -1; }
	catch(std::exception& ex) { return handle_stdexception(__func__, ex, -1); }
	catch(...) { return handle_generalexception(__func__, -1); }
}

//---------------------------------------------------------------------------
// LengthRecordedStream
//
// Gets the total length of the stream that's currently being read
//
// Arguments:
//
//	NONE

long long LengthRecordedStream(void)
{
	if(!g_dvrstream) return -1;

	// Recorded stream can actually be realtime if the recording is played while
	// it's still in progress; do not report a length back to Kodi in this case
	try { return (g_dvrstream->realtime() ? -1 : g_dvrstream->length()); }
	catch(std::exception& ex) { return handle_stdexception(__func__, ex, -1); }
	catch(...) { return handle_generalexception(__func__, -1); }
}

//---------------------------------------------------------------------------
// DemuxReset
//
// Reset the demultiplexer in the add-on
//
// Arguments:
//
//	NONE

void DemuxReset(void)
{
}

//---------------------------------------------------------------------------
// DemuxAbort
//
// Abort the demultiplexer thread in the add-on
//
// Arguments:
//
//	NONE

void DemuxAbort(void)
{
}

//---------------------------------------------------------------------------
// DemuxFlush
//
// Flush all data that's currently in the demultiplexer buffer in the add-on
//
// Arguments:
//
//	NONE

void DemuxFlush(void)
{
}

//---------------------------------------------------------------------------
// DemuxRead
//
// Read the next packet from the demultiplexer, if there is one
//
// Arguments:
//
//	NONE

DemuxPacket* DemuxRead(void)
{
	return nullptr;
}

//---------------------------------------------------------------------------
// CanPauseStream
//
// Check if the backend support pausing the currently playing stream
//
// Arguments:
//
//	NONE

bool CanPauseStream(void)
{
	return true;
}

//---------------------------------------------------------------------------
// CanSeekStream
//
// Check if the backend supports seeking for the currently playing stream
//
// Arguments:
//
//	NONE

bool CanSeekStream(void)
{
	try { return (g_dvrstream) ? g_dvrstream->canseek() : false; }
	catch(std::exception& ex) { return handle_stdexception(__func__, ex, false); }
	catch(...) { return handle_generalexception(__func__, false); }
}

//---------------------------------------------------------------------------
// PauseStream
//
// Notify the pvr addon that XBMC (un)paused the currently playing stream
//
// Arguments:
//
//	paused		- Paused/unpaused flag

void PauseStream(bool /*paused*/)
{
}

//---------------------------------------------------------------------------
// SeekTime
//
// Notify the pvr addon/demuxer that XBMC wishes to seek the stream by time
//
// Arguments:
//
//	time		- The absolute time since stream start
//	backwards	- True to seek to keyframe BEFORE time, else AFTER
//	startpts	- Can be updated to point to where display should start

bool SeekTime(double /*time*/, bool /*backwards*/, double* startpts)
{
	if(startpts == nullptr) return false;

	return false;
}

//---------------------------------------------------------------------------
// SetSpeed
//
// Notify the pvr addon/demuxer that XBMC wishes to change playback speed
//
// Arguments:
//
//	speed		- The requested playback speed

void SetSpeed(int /*speed*/)
{
}

//---------------------------------------------------------------------------
// GetPlayingTime
//
// Get actual playing time from addon. With timeshift enabled this is different to live
//
// Arguments:
//
//	NONE

time_t GetPlayingTime(void)
{
	return 0;
}

//---------------------------------------------------------------------------
// GetBufferTimeStart
//
// Get time of oldest packet in timeshift buffer (UTC)
//
// Arguments:
//
//	NONE

time_t GetBufferTimeStart(void)
{
	return 0;
}

//---------------------------------------------------------------------------
// GetBufferTimeEnd
//
// Get time of latest packet in timeshift buffer (UTC)
//
// Arguments:
//
//	NONE

time_t GetBufferTimeEnd(void)
{
	return 0;
}

//---------------------------------------------------------------------------
// GetBackendHostname
//
// Get the hostname of the pvr backend server
//
// Arguments:
//
//	NONE

char const* GetBackendHostname(void)
{
	return "";
}

//---------------------------------------------------------------------------
// IsTimeshifting
//
// Check if timeshift is active
//
// Arguments:
//
//	NONE

bool IsTimeshifting(void)
{
	// Detection of time-shifting is not currently supported
	return false;
}

//---------------------------------------------------------------------------
// IsRealTimeStream
//
// Check for real-time streaming
//
// Arguments:
//
//	NONE

bool IsRealTimeStream(void)
{
	try { return (g_dvrstream) ? g_dvrstream->realtime() : false; }
	catch(std::exception& ex) { return handle_stdexception(__func__, ex, false); }
	catch(...) { return handle_generalexception(__func__, false); }
}

//---------------------------------------------------------------------------
// SetEPGTimeFrame
//
// Tell the client the time frame to use when notifying epg events back to Kodi
//
// Arguments:
//
//	days	- number of days from "now". EPG_TIMEFRAME_UNLIMITED means that Kodi is interested in all epg events

PVR_ERROR SetEPGTimeFrame(int days)
{
	g_epgmaxtime = days;
	return PVR_ERROR::PVR_ERROR_NO_ERROR;
}

//---------------------------------------------------------------------------
// OnSystemSleep
//
// Notification of system sleep power event
//
// Arguments:
//
//	NONE

void OnSystemSleep()
{
	try {

		g_dvrstream.reset();			// Destroy any active stream instance
		g_scheduler.stop();				// Stop the scheduler
		g_scheduler.clear();			// Clear out any pending tasks
	}

	catch(std::exception& ex) { return handle_stdexception(__func__, ex); }
	catch(...) { return handle_generalexception(__func__); }
}

//---------------------------------------------------------------------------
// OnSystemWake
//
// Notification of system wake power event
//
// Arguments:
//
//	NONE

void OnSystemWake()
{
	// Create a copy of the current addon settings structure
	struct addon_settings settings = copy_settings();

	try {

		g_scheduler.stop();					// Ensure scheduler was stopped
		g_scheduler.clear();				// Ensure there are no pending tasks

		// The special discover_startup_task takes care of all discoveries in a more optimized
		// fashion than invoking the periodic ones; use that on wakeup too
		log_notice(__func__, ": scheduling startup discovery task (delayed ", settings.startup_discovery_task_delay, " seconds)");
		g_scheduler.add(std::chrono::system_clock::now() + std::chrono::seconds(settings.startup_discovery_task_delay), discover_startup_task);
	
		g_scheduler.start();				// Restart the scheduler
	}

	catch(std::exception& ex) { return handle_stdexception(__func__, ex); }
	catch(...) { return handle_generalexception(__func__); }
}

//---------------------------------------------------------------------------
// OnPowerSavingActivated
//
// Notification of system power saving activation event
//
// Arguments:
//
//	NONE

void OnPowerSavingActivated()
{
}

//---------------------------------------------------------------------------
// OnPowerSavingDeactivated
//
// Notification of system power saving deactivation event
//
// Arguments:
//
//	NONE

void OnPowerSavingDeactivated()
{
}

//---------------------------------------------------------------------------
// GetStreamTimes
//
// Temporary function to be removed in later PVR API version

PVR_ERROR GetStreamTimes(PVR_STREAM_TIMES* /*times*/)
{
	return PVR_ERROR_NOT_IMPLEMENTED;
}

//---------------------------------------------------------------------------

#pragma warning(pop)
