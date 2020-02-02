//---------------------------------------------------------------------------
// Copyright (c) 2016-2020 Michael G. Brehm
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
#include <atomic>
#include <chrono>
#include <functional>
#include <list>
#include <memory>
#include <mutex>
#include <random>
#include <string>
#include <sstream>
#include <vector>

#ifdef __ANDROID__
#include <android/log.h>
#include <sys/prctl.h>
#endif

#if defined(_WINDOWS) || defined(WINAPI_FAMILY)
#include <netlistmgr.h>
#else
#include <fcntl.h>
#include <stdlib.h>
#include <unistd.h>
#include <sys/socket.h>
#include <sys/ioctl.h>
#include <net/if.h>
#include <netinet/in.h>
#endif 

#include <xbmc_addon_dll.h>
#include <xbmc_pvr_dll.h>
#include <version.h>

#include <libXBMC_addon.h>
#include <libKODI_guilib.h>
#include <libXBMC_pvr.h>

#include "database.h"
#include "dbtypes.h"
#include "devicestream.h"
#include "httpstream.h"
#include "pvrstream.h"
#include "scalar_condition.h"
#include "scheduler.h"
#include "sqlite_exception.h"
#include "string_exception.h"

#pragma warning(push, 4)

//---------------------------------------------------------------------------
// MACROS
//---------------------------------------------------------------------------

// MENUHOOK_XXXXXX
//
// Menu hook identifiers
#define MENUHOOK_RECORD_DELETERERECORD					2
#define MENUHOOK_SETTING_TRIGGERDEVICEDISCOVERY			3
#define MENUHOOK_SETTING_TRIGGERLINEUPDISCOVERY			4
#define MENUHOOK_SETTING_TRIGGERRECORDINGDISCOVERY		6
#define MENUHOOK_SETTING_TRIGGERRECORDINGRULEDISCOVERY	7
#define MENUHOOK_CHANNEL_DISABLE						9
#define MENUHOOK_CHANNEL_ADDFAVORITE					10
#define MENUHOOK_CHANNEL_REMOVEFAVORITE					11
#define MENUHOOK_SETTING_SHOWDEVICENAMES				12
#define MENUHOOK_SETTING_TRIGGERLISTINGDISCOVERY		13

//---------------------------------------------------------------------------
// FUNCTION PROTOTYPES
//---------------------------------------------------------------------------

// Backend discovery helpers
//
static void discover_devices(scalar_condition<bool> const& cancel, bool& changed);
static void discover_episodes(scalar_condition<bool> const& cancel, bool& changed);
static void discover_lineups(scalar_condition<bool> const& cancel, bool& changed);
static void discover_listings(scalar_condition<bool> const& cancel, bool& changed);
static void discover_recordingrules(scalar_condition<bool> const& cancel, bool& changed);
static void discover_recordings(scalar_condition<bool> const& cancel, bool& changed);

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
static void startup_alerts_task(scalar_condition<bool> const& cancel);
static void update_devices_task(scalar_condition<bool> const& cancel);
static void update_episodes_task(scalar_condition<bool> const& cancel);
static void update_lineups_task(scalar_condition<bool> const& cancel);
static void update_listings_task(bool force, bool checkchannels, scalar_condition<bool> const& cancel);
static void update_recordingrules_task(scalar_condition<bool> const& cancel);
static void update_recordings_task(scalar_condition<bool> const& cancel);
static void wait_for_network_task(int seconds, scalar_condition<bool> const& cancel);

// Helper Functions
//
static bool ipv4_network_available(void);
static std::string select_tuner(std::vector<std::string> const& possibilities);
static void start_discovery(void) noexcept;
static void wait_for_devices(void) noexcept;
static void wait_for_channels(void) noexcept;
static void wait_for_timers(void) noexcept;
static void wait_for_recordings(void) noexcept;

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

// tuning_protocol
//
// Defines the protocol to use when streaming directly from tuner(s)
enum tuning_protocol {

	http					= 0,
	rtpudp					= 1,
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

	// discover_recordings_after_playback
	//
	// Flag to re-discover recordings immediately after playback has stopped
	bool discover_recordings_after_playback;

	// use_backend_genre_strings
	//
	// Flag to use the backend provided genre strings instead of mapping them
	bool use_backend_genre_strings;

	// show_drm_protected_channels
	//
	// Flag indicating that DRM channels should be shown to the user
	bool show_drm_protected_channels;

	// use_channel_names_from_lineup
	//
	// Flag indicating that the channel names should come from the lineup not the EPG
	bool use_channel_names_from_lineup;

	// disable_recording_categories
	//
	// Flag indicating that the category of a recording should be ignored
	bool disable_recording_categories;

	// generate_repeat_indicators
	//
	// Flag indicating that a repeat indicator should be appended to episode names
	bool generate_repeat_indicators;

	// delete_datetime_rules_after
	//
	// Amount of time (seconds) after which an expired date/time rule is deleted
	int delete_datetime_rules_after;

	// discover_devices_interval
	//
	// Interval at which the local network device discovery will occur (seconds)
	int discover_devices_interval;

	// discover_episodes_interval
	//
	// Interval at which the recording rule episodes discovery will occur (seconds)
	int discover_episodes_interval;

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

	// use_http_device_discovery
	//
	// Flag to discover devices via HTTP instead of local network broadcast
	bool use_http_device_discovery;

	// use_direct_tuning
	//
	// Flag indicating that Live TV will be handled directly from the tuner(s)
	bool use_direct_tuning;

	// direct_tuning_protocol
	//
	// Indicates the preferred protocol to use when streaming directly from the tuner(s)
	enum tuning_protocol direct_tuning_protocol;

	// stream_read_chunk_size
	//
	// Indicates the minimum number of bytes to return from a stream read
	int stream_read_chunk_size;

	// deviceauth_stale_after
	//
	// Amount of time (seconds) after which an expired device authorization code is removed
	int deviceauth_stale_after;

	// enable_recording_edl
	//
	// Enables support recorded TV edit decision lists
	bool enable_recording_edl;

	// recording_edl_folder
	//
	// Folder containing the recorded TV edit decision list files
	std::string recording_edl_folder;

	// recording_edl_folder_2
	//
	// Additional folder containing the recorded TV edit decision list files
	std::string recording_edl_folder_2;

	// recording_edl_folder_3
	//
	// Additional folder containing the recorded TV edit decision list files
	std::string recording_edl_folder_3;

	// recording_edl_folder_is_flat
	//
	// Indicates that the specified EDL folder is flattened (no subdirectories)
	bool recording_edl_folder_is_flat;

	// recording_edl_cut_as_comskip
	//
	// Indicates that EDL CUT indicators should be replaced with COMSKIP indicators
	bool recording_edl_cut_as_comskip;

	// recording_edl_start_padding
	//
	// Indicates the number of milliseconds to add to an EDL start value
	int recording_edl_start_padding;

	// recording_edl_end_padding
	//
	// Indicates the number of milliseconds to subtract to an EDL end value
	int recording_edl_end_padding;
};

//---------------------------------------------------------------------------
// GLOBAL VARIABLES
//---------------------------------------------------------------------------

// g_addon
//
// Kodi add-on callbacks
static std::unique_ptr<ADDON::CHelper_libXBMC_addon> g_addon;

// g_capabilities (const)
//
// PVR implementation capability flags
static const PVR_ADDON_CAPABILITIES g_capabilities = {

	true,			// bSupportsEPG
	false,			// bSupportsEPGEdl
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
	true,			// bSupportsRecordingEdl
	false,			// bSupportsRecordingsRename
	false,			// bSupportsRecordingsLifetimeChange
	false,			// bSupportsDescrambleInfo
	false,			// bSupportsAsyncEPGTransfer
	0,				// iRecordingsLifetimesSize
	{ { 0, "" } },	// recordingsLifetimeValues
};

// g_connpool
//
// Global SQLite database connection pool instance
static std::shared_ptr<connectionpool> g_connpool;

// g_discovered_xxxxx
//
// Flags indicating if initial discoveries have executed
static scalar_condition<bool> g_discovered_devices{ false };
static scalar_condition<bool> g_discovered_episodes{ false };
static scalar_condition<bool> g_discovered_lineups{ false };
static scalar_condition<bool> g_discovered_listings{ false };
static scalar_condition<bool> g_discovered_recordingrules{ false };
static scalar_condition<bool> g_discovered_recordings{ false };

// g_epgmaxtime
//
// Maximum number of days to report for EPG and series timers
static std::atomic<int> g_epgmaxtime{ EPG_TIMEFRAME_UNLIMITED };

// g_gui
//
// Kodi GUI library callbacks
static std::unique_ptr<CHelper_libKODI_guilib> g_gui;

// g_pvr
//
// Kodi PVR add-on callbacks
static std::unique_ptr<CHelper_libXBMC_pvr> g_pvr;

// g_pvrstream
//
// DVR stream buffer instance
static std::unique_ptr<pvrstream> g_pvrstream;

// g_randomengine
//
// Global pseudo-random number generator engine
static std::default_random_engine g_randomengine;

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
	false,					// discover_recordings_after_playback
	false,					// use_backend_genre_strings
	false,					// show_drm_protected_channels
	false,					// use_channel_names_from_lineup
	false,					// disable_recording_categories
	false,					// generate_repeat_indicators
	86400,					// delete_datetime_rules_after			default = 1 day
	300, 					// discover_devices_interval;			default = 5 minutes
	7200,					// discover_episodes_interval			default = 2 hours
	600,					// discover_lineups_interval			default = 10 minutes
	600,					// discover_recordings_interval			default = 10 minutes
	7200,					// discover_recordingrules_interval		default = 2 hours
	false,					// use_http_device_discovery
	false,					// use_direct_tuning
	tuning_protocol::http,	// direct_tuning_protocol
	(4 KiB),				// stream_read_chunk_size
	72000,					// deviceauth_stale_after				default = 20 hours
	false,					// enable_recording_edl
	"",						// recording_edl_folder
	"",						// recording_edl_folder_2
	"",						// recording_edl_folder_3
	false,					// recording_edl_folder_is_flat
	false,					// recording_edl_cut_as_comskip
	0,						// recording_edl_start_padding
	0,						// recording_edl_end_padding
};

// g_settings_lock
//
// Synchronization object to serialize access to addon settings
static std::mutex g_settings_lock;

// g_stream_starttime
//
// Start time to report for the current stream
static time_t g_stream_starttime = 0;

// g_stream_endtime
//
// End time to report for the current stream
static time_t g_stream_endtime = 0;

// g_timertypes (const)
//
// Array of PVR_TIMER_TYPE structures to pass to Kodi
static const PVR_TIMER_TYPE g_timertypes[] ={

	// timer_type::seriesrule
	//
	// Timer type for non-EPG series rules, requires a series link or name match operation to create. Can be both edited and deleted.
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
	// Timer type for non-EPG date time only rules, requires a series link or name match operation to create. Cannot be edited but can be deleted.
	{
		// iID
		timer_type::datetimeonlyrule,

		// iAttributes
		PVR_TIMER_TYPE_IS_REPEATING | PVR_TIMER_TYPE_IS_READONLY | PVR_TIMER_TYPE_SUPPORTS_CHANNELS | PVR_TIMER_TYPE_SUPPORTS_TITLE_EPG_MATCH | 
			PVR_TIMER_TYPE_SUPPORTS_FIRST_DAY | PVR_TIMER_TYPE_SUPPORTS_START_TIME | PVR_TIMER_TYPE_SUPPORTS_START_END_MARGIN | 
			PVR_TIMER_TYPE_FORBIDS_EPG_TAG_ON_CREATE | PVR_TIMER_TYPE_SUPPORTS_READONLY_DELETE,

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
	// used for existing episode timers; these cannot be edited or deleted
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
	// used for existing date/time only episode timers; these cannot be edited or deleted
	{
		// iID
		timer_type::datetimeonlytimer,

		// iAttributes
		PVR_TIMER_TYPE_IS_READONLY | PVR_TIMER_TYPE_FORBIDS_NEW_INSTANCES | PVR_TIMER_TYPE_SUPPORTS_CHANNELS | PVR_TIMER_TYPE_SUPPORTS_START_TIME | 
			PVR_TIMER_TYPE_SUPPORTS_END_TIME,
		
		// strDescription
		"Record Once Episode",

		0, { {0, "" } }, 0,			// priorities
		0, { {0, "" } }, 0,			// lifetimes
		0, { {0, "" } }, 0,			// preventDuplicateEpisodes
		0, { {0, "" } }, 0,			// recordingGroup
		0, { {0, "" } }, 0,			// maxRecordings
	},
};

// g_userpath
//
// Set to the input PVR user path string
static std::string g_userpath;

//---------------------------------------------------------------------------
// HELPER FUNCTIONS
//---------------------------------------------------------------------------

// copy_settings (inline)
//
// Atomically creates a copy of the global addon_settings structure
inline struct addon_settings copy_settings(void)
{
	std::unique_lock<std::mutex> settings_lock(g_settings_lock);
	return g_settings;
}

// discover_devices (local)
//
// Helper function used to execute a backend device discovery operation
static void discover_devices(scalar_condition<bool> const&, bool& changed)
{
	changed = false;						// Initialize [ref] argument

	// Create a copy of the current addon settings structure
	struct addon_settings settings = copy_settings();

	log_notice(__func__, ": initiated local network device discovery (method: ", settings.use_http_device_discovery ? "http" : "broadcast", ")");

	try {

		// Pull a database connection out from the connection pool
		connectionpool::handle dbhandle(g_connpool);

		// Clear any invalid device authorization strings present in the existing discovery data
		clear_authorization_strings(dbhandle, settings.deviceauth_stale_after);

		// Discover the devices on the local network and check for changes
		auto caller = __func__;
		discover_devices(dbhandle, settings.use_http_device_discovery, changed);
		enumerate_device_names(dbhandle, [&](struct device_name const& device_name) -> void { log_notice(caller, ": discovered: ", device_name.name); });

		// Set the discovery time for the device information
		set_discovered(dbhandle, "devices", time(nullptr));

		g_discovered_devices = true;			// Set the scalar_condition flag
	}

	// Set the global scalar_condition on exception before re-throwing it
	catch(...) { g_discovered_devices = true; throw; }
}

// discover_episodes (local)
//
// Helper function used to execute a backend recording rule episode discovery operation
static void discover_episodes(scalar_condition<bool> const&, bool& changed)
{
	changed = false;						// Initialize [ref] argument

	log_notice(__func__, ": initiated recording rule episode discovery");

	try {

		// Pull a database connection out from the connection pool
		connectionpool::handle dbhandle(g_connpool);

		// This operation is only available when there is at least one DVR authorized tuner
		std::string authorization = get_authorization_strings(dbhandle, true);

		// Discover the recording rule episode information associated with all of the authorized devices
		if(authorization.length() != 0) discover_episodes(dbhandle, authorization.c_str(), changed);
		else log_notice(__func__, ": no tuners with valid DVR authorization were discovered; skipping recording rule episode discovery");

		// Set the discovery time for the episode information
		set_discovered(dbhandle, "episodes", time(nullptr));

		g_discovered_episodes = true;			// Set the scalar_condition flag
	}

	// Set the global scalar_condition on exception before re-throwing it
	catch(...) { g_discovered_episodes = true; throw; }
}

// discover_lineups (local)
//
// Helper function used to execute a backend channel lineup discovery operation
static void discover_lineups(scalar_condition<bool> const&, bool& changed)
{
	changed = false;						// Initialize [ref] argument

	log_notice(__func__, ": initiated local tuner device lineup discovery");

	try { 
		
		// Pull a database connection out from the connection pool
		connectionpool::handle dbhandle(g_connpool);

		// Execute the channel lineup discovery operation
		discover_lineups(dbhandle, changed);

		// Set the discovery time for the lineup information
		set_discovered(dbhandle, "lineups", time(nullptr));

		g_discovered_lineups = true;			// Set the scalar_condition flag
	}

	// Set the global scalar_condition on exception before re-throwing it
	catch(...) { g_discovered_lineups = true; throw; }
}

// discover_listings (local)
//
// Helper function used to execute a backend listing discovery operation
static void discover_listings(scalar_condition<bool> const&, bool& changed)
{
	changed = true;						// Initialize [ref] argument

	// Create a copy of the current addon settings structure
	struct addon_settings settings = copy_settings();

	log_notice(__func__, ": initiated listing discovery");

	try {

		// Pull a database connection out from the connection pool
		connectionpool::handle dbhandle(g_connpool);

		// This operation is only available when there is at least one DVR authorized tuner, but
		// lineup data for any unauthorized tuner(s) can also be retrieved
		if(has_dvr_authorization(dbhandle)) {

			std::string authorization = get_authorization_strings(dbhandle, false);
			if(authorization.length() != 0) discover_listings(dbhandle, authorization.c_str(), changed);
		}

		else log_notice(__func__, ": no tuners with valid DVR authorization were discovered; skipping listing discovery");

		// Set the discovery time for the listing information
		set_discovered(dbhandle, "listings", time(nullptr));

		g_discovered_listings = true;			// Set the scalar_condition flag
	}

	// Set the global scalar_condition on exception before re-throwing it
	catch(...) { g_discovered_listings = true; throw; }
}

// discover_recordingrules (local)
//
// Helper function used to execute a backend recording rule discovery operation
static void discover_recordingrules(scalar_condition<bool> const&, bool& changed)
{
	changed = false;						// Initialize [ref] argument

	// Create a copy of the current addon settings structure
	struct addon_settings settings = copy_settings();

	log_notice(__func__, ": initiated recording rule discovery");

	try {

		// Pull a database connection out from the connection pool
		connectionpool::handle dbhandle(g_connpool);

		// This operation is only available when there is at least one DVR authorized tuner
		std::string authorization = get_authorization_strings(dbhandle, true);
		if(authorization.length() != 0) {

			// Discover the recording rules associated with all authorized devices
			discover_recordingrules(dbhandle, authorization.c_str(), changed);

			// Delete all expired recording rules from the backend as part of the discovery operation
			enumerate_expired_recordingruleids(dbhandle, settings.delete_datetime_rules_after, [&](unsigned int const& recordingruleid) -> void
			{
				try { delete_recordingrule(dbhandle, authorization.c_str(), recordingruleid); changed = true; }
				catch(std::exception& ex) { handle_stdexception(__func__, ex); } 
				catch(...) { handle_generalexception(__func__); }
			});

		}

		else log_notice(__func__, ": no tuners with valid DVR authorization were discovered; skipping recording rule discovery");

		// Set the discovery time for the recordingrule information
		set_discovered(dbhandle, "recordingrules", time(nullptr));

		g_discovered_recordingrules = true;		// Set the scalar_condition flag
	}

	// Set the global scalar_condition on exception before re-throwing it
	catch(...) { g_discovered_recordingrules = true; throw; }
}

// discover_recordings (local)
//
// Helper function used to execute a backend recordings operation
static void discover_recordings(scalar_condition<bool> const&, bool& changed)
{
	changed = false;						// Initialize [ref] argument

	log_notice(__func__, ": initiated local storage device recording discovery");

	try {

		// Pull a database connection out from the connection pool
		connectionpool::handle dbhandle(g_connpool);

		// Execute the recording discovery operation
		discover_recordings(dbhandle, changed);

		// Set the discovery time for the recording information
		set_discovered(dbhandle, "recordings", time(nullptr));

		g_discovered_recordings = true;			// Set the scalar_codition flag
	}

	// Set the global scalar_condition on exception before re-throwing it
	catch(...) { g_discovered_recordings = true; throw; }
}

// handle_generalexception (local)
//
// Handler for thrown generic exceptions
static void handle_generalexception(char const* function)
{
	log_error(function, " failed due to an exception");
}

// handle_generalexception (local)
//
// Handler for thrown generic exceptions
template<typename _result>
static _result handle_generalexception(char const* function, _result result)
{
	handle_generalexception(function);
	return result;
}

// handle_stdexception (local)
//
// Handler for thrown std::exceptions
static void handle_stdexception(char const* function, std::exception const& ex)
{
	log_error(function, " failed due to an exception: ", ex.what());
}

// handle_stdexception (local)
//
// Handler for thrown std::exceptions
template<typename _result>
static _result handle_stdexception(char const* function, std::exception const& ex, _result result)
{
	handle_stdexception(function, ex);
	return result;
}

// log_debug (local)
//
// Variadic method of writing a LOG_DEBUG entry into the Kodi application log
template<typename... _args>
static void log_debug(_args&&... args)
{
	log_message(ADDON::addon_log_t::LOG_DEBUG, std::forward<_args>(args)...);
}

// log_error (local)
//
// Variadic method of writing a LOG_ERROR entry into the Kodi application log
template<typename... _args>
static void log_error(_args&&... args)
{
	log_message(ADDON::addon_log_t::LOG_ERROR, std::forward<_args>(args)...);
}

// log_info (local)
//
// Variadic method of writing a LOG_INFO entry into the Kodi application log
template<typename... _args>
static void log_info(_args&&... args)
{
	log_message(ADDON::addon_log_t::LOG_INFO, std::forward<_args>(args)...);
}

// log_message (local)
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

#if defined(_WINDOWS) || defined(WINAPI_FAMILY)
		std::string message = "ERROR: " + stream.str() + "\r\n";
		OutputDebugStringA(message.c_str());
#elif __ANDROID__
		__android_log_print(ANDROID_LOG_ERROR, VERSION_PRODUCTNAME_ANSI, "ERROR: %s\n", stream.str().c_str());
#else
		fprintf(stderr, "ERROR: %s\r\n", stream.str().c_str());
#endif

	}
}

// log_notice (local)
//
// Variadic method of writing a LOG_NOTICE entry into the Kodi application log
template<typename... _args>
static void log_notice(_args&&... args)
{
	log_message(ADDON::addon_log_t::LOG_NOTICE, std::forward<_args>(args)...);
}

// edltype_to_string (local)
//
// Converts a PVR_EDL_TYPE enumeration value into a string
static char const* const edltype_to_string(PVR_EDL_TYPE const& type)
{
	switch(type) {

		case PVR_EDL_TYPE::PVR_EDL_TYPE_CUT: return "CUT";
		case PVR_EDL_TYPE::PVR_EDL_TYPE_MUTE: return "MUTE";
		case PVR_EDL_TYPE::PVR_EDL_TYPE_SCENE: return "SCENE";
		case PVR_EDL_TYPE::PVR_EDL_TYPE_COMBREAK: return "COMBREAK";
	}

	return "<UNKNOWN>";
}

// ipv4_network_available (local)
//
// Determines if IPv4 connectivity has been established on the system
static bool ipv4_network_available(void)
{
#if defined(_WINDOWS) || defined(WINAPI_FAMILY)

  #if WINAPI_FAMILY_PARTITION(WINAPI_PARTITION_DESKTOP)

	INetworkListManager*	netlistmgr = nullptr;
	NLM_CONNECTIVITY		connectivity = NLM_CONNECTIVITY_DISCONNECTED;

	// Create an instance of the NetworkListManager object
	HRESULT hresult = CoCreateInstance(CLSID_NetworkListManager, nullptr, CLSCTX_INPROC_SERVER, IID_INetworkListManager, reinterpret_cast<void**>(&netlistmgr));
	if(FAILED(hresult)) throw string_exception(__func__, ": failed to create NetworkListManager instance (hr=", hresult, ")");

	// Interrogate the Network List Manager to determine the current network connectivity status
	hresult = netlistmgr->GetConnectivity(&connectivity);
	netlistmgr->Release();

	// If the NetworkListManager returned an error, throw an exception to that effect
	if(FAILED(hresult)) throw string_exception(__func__, ": failed to interrogate NetworkListManager connectivity state (hr=", hresult, ")");
	
	// If the status was successfully interrogated, check for the necessary IPv4 connectivity flags
	return ((connectivity & (NLM_CONNECTIVITY_IPV4_SUBNET | NLM_CONNECTIVITY_IPV4_LOCALNETWORK | NLM_CONNECTIVITY_IPV4_INTERNET)) != 0);

  #else

	// This function cannot currently be implemented on non-desktop Windows platforms
	return true;

  #endif // WINAPI_FAMILY_PARTITION(WINAPI_PARTITION_DESKTOP)

#else

	const int MAX_INTERFACES = 128;

	// Allocate a buffer to hold the interface address information
	std::unique_ptr<struct ifreq[]> ifreqs(new struct ifreq[MAX_INTERFACES]);
	memset(&ifreqs[0], 0, sizeof(struct ifreq) * MAX_INTERFACES);

	// Create a IPv4 TCP socket instance
	int sock = socket(AF_INET, SOCK_STREAM, IPPROTO_IP);
	if(sock == -1) throw string_exception(__func__, ": failed to create socket instance");

	// Initialize an ifconf structure to send into ioctl(SIOCGIFCONF)
	struct ifconf ifc;
	ifc.ifc_len = sizeof(struct ifreq) * MAX_INTERFACES;
	ifc.ifc_req = &ifreqs[0];

	// Retrieve the interface address information
	if(ioctl(sock, SIOCGIFCONF, &ifc) != 0) { close(sock); throw string_exception(__func__, ": ioctl(SIOCGIFCONF) failed"); }

	// If the returned length is equal to (or greater than) the original length, there was an overflow
	if(ifc.ifc_len >= static_cast<int>(sizeof(struct ifreq) * MAX_INTERFACES)) { close(sock); throw string_exception(__func__, ": ioctl(SIOCGIFCONF) returned more interfaces than have been allowed for"); }

	// Set up the head and tail pointers into the array of ifreq structures
	struct ifreq* current = ifc.ifc_req;
	struct ifreq* end = reinterpret_cast<struct ifreq*>(&ifc.ifc_buf[ifc.ifc_len]);

	// Iterate over each ifreq structure and determine if the interface is running
	while(current <= end) {

		struct sockaddr_in* addrin = reinterpret_cast<struct sockaddr_in*>(&current->ifr_addr);
		
		// If the interface has a valid IPv4 address, get the active flag information
		uint32_t ipaddr = ntohl(addrin->sin_addr.s_addr);
		if((ipaddr != 0) && (ioctl(sock, SIOCGIFFLAGS, current) == 0)) {

			// If the flag indicate that the interface is both UP and RUNNING, we're done
			unsigned int flags = current->ifr_flags & (IFF_LOOPBACK | IFF_POINTOPOINT | IFF_UP | IFF_RUNNING);
			if(flags == (IFF_UP | IFF_RUNNING)) { close(sock); return true; }
		}

		current++;				// Move to the next interface in the list
	}

	close(sock);				// Close the socket instance
	return false;				// No interfaces were both UP and RUNNING

#endif	// defined(_WINDOWS) || defined(WINAPI_FAMILY)
}

// openlivestream_storage_http (local)
//
// Attempts to open a live stream via HTTP from an available storage engine
static std::unique_ptr<pvrstream> openlivestream_storage_http(connectionpool::handle const& dbhandle, struct addon_settings const& settings, union channelid channelid, char const* vchannel)
{
	assert(vchannel != nullptr);
	if((vchannel == nullptr) || (*vchannel == '\0')) throw std::invalid_argument("vchannel");

	// Generate a vector<> of possible storage device URLs for the virtual channel
	std::vector<std::string> streamurls = get_storage_stream_urls(dbhandle, channelid);
	if(streamurls.size() == 0) { log_error(__func__, ": unable to generate storage engine stream url(s) for channel ", vchannel); return nullptr; }

	// Attempt to create a stream using the URLs in the order provided, there is currently no way to choose priority here
	for(auto const& streamurl : streamurls) {

		try {

			// Start the new HTTP stream using the parameters currently specified by the settings
			std::unique_ptr<pvrstream> stream = httpstream::create(streamurl.c_str(), settings.stream_read_chunk_size);
			log_notice(__func__, ": streaming channel ", vchannel, " via storage engine url ", streamurl.c_str());

			return stream;
		}

		// If the stream creation failed, log an error; do not stop enumerating the available storage devices
		catch(std::exception & ex) { log_error(__func__, ": unable to stream channel ", vchannel, " via storage engine url ", streamurl.c_str(), ": ", ex.what()); }
	}

	return nullptr;
}

// openlivestream_tuner_device (local)
//
// Attempts to open a live stream via RTP/UDP from an available tuner device
static std::unique_ptr<pvrstream> openlivestream_tuner_device(connectionpool::handle const& dbhandle, struct addon_settings const& /*settings*/, union channelid channelid, char const* vchannel)
{
	std::vector<std::string>		devices;			// vector<> of possible device tuners for the channel

	assert(vchannel != nullptr);
	if((vchannel == nullptr) || (*vchannel == '\0')) throw std::invalid_argument("vchannel");

	// Create a collection of all the tuners that can possibly stream the requested channel
	enumerate_channeltuners(dbhandle, channelid, [&](char const* item) -> void { devices.emplace_back(item); });
	if(devices.size() == 0) { log_error(__func__, ": unable to find any possible tuner devices to stream channel ", vchannel); return nullptr; }

	try {

		// Start the new RTP/UDP stream -- devicestream performs its own tuner selection based on the provided collection
		std::unique_ptr<pvrstream> stream = devicestream::create(devices, vchannel);
		log_notice(__func__, ": streaming channel ", vchannel, " via tuner device rtp/udp broadcast");

		return stream;
	}

	// If the stream creation failed, log an error and return a null unique_ptr<> back to the caller, do not throw an exception
	catch(std::exception& ex) { log_error(__func__, ": unable to stream channel ", vchannel, " via tuner device rtp/udp broadcast: ", ex.what()); }

	return nullptr;
}

// openlivestream_tuner_http (local)
//
// Attempts to open a live stream via HTTP from an available tuner device
static std::unique_ptr<pvrstream> openlivestream_tuner_http(connectionpool::handle const& dbhandle, struct addon_settings const& settings, union channelid channelid, char const* vchannel)
{
	std::vector<std::string>		devices;			// vector<> of possible device tuners for the channel

	assert(vchannel != nullptr);
	if((vchannel == nullptr) || (*vchannel == '\0')) throw std::invalid_argument("vchannel");

	// Create a collection of all the tuners that can possibly stream the requested channel
	enumerate_channeltuners(dbhandle, channelid, [&](char const* item) -> void { devices.emplace_back(item); });
	if(devices.size() == 0) { log_error(__func__, ": unable to find any possible tuner devices to stream channel ", vchannel); return nullptr; }

	// A valid tuner device has to be selected from the available options
	std::string selected = select_tuner(devices);
	if(selected.length() == 0) { log_error(__func__, ": no tuner devices are available to create the requested stream"); return nullptr; }

	// Generate the URL required to stream the channel via the tuner over HTTP
	std::string streamurl = get_tuner_stream_url(dbhandle, selected.c_str(), channelid);
	if(streamurl.length() == 0) { log_error(__func__, ": unable to generate tuner device stream url for channel ", vchannel); return nullptr; }

	try {

		// Start the new HTTP stream using the parameters currently specified by the settings
		std::unique_ptr<pvrstream> stream = httpstream::create(streamurl.c_str(), settings.stream_read_chunk_size);
		log_notice(__func__, ": streaming channel ", vchannel, " via tuner device url ", streamurl.c_str());

		return stream;
	}

	// If the stream creation failed, log an error and return a null unique_ptr<> back to the caller, do not throw an exception
	catch(std::exception& ex) { log_error(__func__, ": unable to stream channel ", vchannel, "via tuner device url ", streamurl.c_str(), ": ", ex.what()); }

	return nullptr;
}

// select_tuner (local)
//
// Selects an available tuner device from a list of possibilities
static std::string select_tuner(std::vector<std::string> const& possibilities)
{
	std::string					tunerid;			// Selected tuner identifier

	// Allocate and initialize the device selector
	struct hdhomerun_device_selector_t* selector = hdhomerun_device_selector_create(nullptr);
	if(selector == nullptr) throw string_exception(__func__, ": hdhomerun_device_selector_create() failed");

	try {

		// Add each of the possible device/tuner combinations to the selector
		for(auto const& iterator : possibilities) {

			struct hdhomerun_device_t* device = hdhomerun_device_create_from_str(iterator.c_str(), nullptr);
			if(device == nullptr) throw string_exception(__func__, ": hdhomerun_device_create_from_str() failed");

			hdhomerun_device_selector_add_device(selector, device);
		}

		// NOTE: There is an inherent race condition here with the tuner lock implementation.  When the tuner
		// is selected here it will be locked, but it cannot remain locked since the ultimate purpose here is
		// to generate an HTTP URL for the application to use.  The HTTP stream will attempt its own lock
		// and would fail if left locked after this function completes.  No way to tell it to use an existing lock.

		// Let libhdhomerun pick a free tuner for us from the available possibilities
		struct hdhomerun_device_t* selected = hdhomerun_device_selector_choose_and_lock(selector, nullptr);
		if(selected) {

			tunerid = hdhomerun_device_get_name(selected);			// DDDDDDDD-T; D=DeviceID, T=TunerID
			hdhomerun_device_tuner_lockkey_release(selected);		// Release the acquired lock
		}

		// Release the selector along with all of the generated device structures
		hdhomerun_device_selector_destroy(selector, true);
	}

	catch(...) { hdhomerun_device_selector_destroy(selector, true); }

	return tunerid;
}

// start_discovery
//
// Performs a one-time discovery startup operation
static void start_discovery(void) noexcept
{
	static std::once_flag	once;			// std::call_once flag

	try {

		// Initial discovery schedules all the individual discoveries to occur as soon as possible
		// and in the order in which they will needed by the Kodi callback functions
		std::call_once(once, []() {

			bool lineupschanged = false;			// Flag if lineups have changed

			// Create a copy of the current addon settings structure
			struct addon_settings settings = copy_settings();

			// Schedule a task to wait for the network to become available
			g_scheduler.add(std::bind(wait_for_network_task, 10, std::placeholders::_1));

			// Schedule the initial discovery tasks to execute as soon as possible
			g_scheduler.add([](scalar_condition<bool> const& cancel) -> void { bool changed; discover_devices(cancel, changed); });
			g_scheduler.add([&](scalar_condition<bool> const& cancel) -> void { discover_lineups(cancel, lineupschanged); });
			g_scheduler.add([](scalar_condition<bool> const& cancel) -> void { bool changed; discover_recordingrules(cancel, changed); });
			g_scheduler.add([](scalar_condition<bool> const& cancel) -> void { bool changed; discover_episodes(cancel, changed); });
			g_scheduler.add([](scalar_condition<bool> const& cancel) -> void { bool changed; discover_recordings(cancel, changed); });

			// Schedule the startup alert and listing update tasks to occur after the initial discovery tasks have completed
			g_scheduler.add(startup_alerts_task);
			g_scheduler.add(std::bind(update_listings_task, false, lineupschanged, std::placeholders::_1));

			// Schedule the remaining update tasks to run at the intervals specified in the addon settings
			g_scheduler.add(std::chrono::system_clock::now() + std::chrono::seconds(settings.discover_devices_interval), update_devices_task);
			g_scheduler.add(std::chrono::system_clock::now() + std::chrono::seconds(settings.discover_lineups_interval), update_lineups_task);
			g_scheduler.add(std::chrono::system_clock::now() + std::chrono::seconds(settings.discover_recordingrules_interval), update_recordingrules_task);
			g_scheduler.add(std::chrono::system_clock::now() + std::chrono::seconds(settings.discover_episodes_interval), update_episodes_task);
			g_scheduler.add(std::chrono::system_clock::now() + std::chrono::seconds(settings.discover_recordings_interval), update_recordings_task);
		});
	}

	catch(std::exception& ex) { return handle_stdexception(__func__, ex); } 
	catch(...) { return handle_generalexception(__func__); }
}

// startup_alerts_task (local)
//
// Scheduled task implementation to perform any necessary startup alerts
static void startup_alerts_task(scalar_condition<bool> const& /*cancel*/)
{
	connectionpool::handle dbhandle(g_connpool);

	// Determine how many tuner devices were discovered on the network
	int numtuners = get_tuner_count(dbhandle);

	// If there were no tuner devices detected alert the user via a notification
	if(numtuners == 0) g_addon->QueueNotification(ADDON::queue_msg_t::QUEUE_ERROR, "HDHomeRun tuner device(s) not detected");

	// If there are no DVR authorized tuner devices detected, alert the user via a message box.
	// This operation is only done one time for the installed system, don't use the database for this
	if((numtuners > 0) && (!has_dvr_authorization(dbhandle))) {

		// If for some reason the user path doesn't exist, skip this operation
		if(g_addon->DirectoryExists(g_userpath.c_str())) {

			// Check to see if the alert has already been issued on this system by checking for the flag file
			std::string alertfile = g_userpath + "/alerted-epgauth";
			if(!g_addon->FileExists(alertfile.c_str(), false)) {

				// Issue the alert about the DVR subscription requirement
				g_gui->Dialog_OK_ShowAndGetInput("DVR Service Subscription Required", 
					"Access to Electronic Program Guide (EPG) listings requires an active HDHomeRun DVR Service subscription.", "", 
					"https://www.silicondust.com/dvr-service/");

				// Write the tag file to storage to prevent the message from showing again
				g_addon->CloseFile(g_addon->OpenFileForWrite(alertfile.c_str(), true));
			}
		}
	}
}

// update_devices_task (local)
//
// Scheduled task implementation to update the HDHomeRun devices
static void update_devices_task(scalar_condition<bool> const& cancel)
{
	bool		changed = false;			// Flag if the discovery data changed

	// Create a copy of the current addon settings structure
	struct addon_settings settings = copy_settings();

	try {

		// Update the backend device discovery information
		if(cancel.test(true) == false) discover_devices(cancel, changed);

		// Changes to the device information triggers updates to the lineups and recordings
		if(changed) {

			if(cancel.test(true) == false) {

				log_notice(__func__, ": device discovery data changed -- execute lineup update now");
				g_scheduler.now(update_lineups_task, cancel);
			}

			if(cancel.test(true) == false) {

				log_notice(__func__, ": device discovery data changed -- execute recording update now");
				g_scheduler.now(update_recordings_task, cancel);
			}
		}
	}

	catch(std::exception& ex) { handle_stdexception(__func__, ex); } 
	catch(...) { handle_generalexception(__func__); }

	// Schedule the next periodic invocation of this task to occur at the caclulated interval
	if(cancel.test(true) == false) {

		log_notice(__func__, ": scheduling next device update to initiate in ", settings.discover_devices_interval, " seconds");
		g_scheduler.add(std::chrono::system_clock::now() + std::chrono::seconds(settings.discover_devices_interval), update_devices_task);
	}

	else log_notice(__func__, ": device update task was cancelled");
}

// update_episodes_task (local)
//
// Scheduled task implementation to update the episode data associated with recording rules
static void update_episodes_task(scalar_condition<bool> const& cancel)
{
	bool		changed = false;			// Flag if the discovery data changed

	// Create a copy of the current addon settings structure
	struct addon_settings settings = copy_settings();

	try {

		// Update the backend recording rule episode information
		if(cancel.test(true) == false) discover_episodes(cancel, changed);

		// Changes to the episode information affects the PVR timers
		if(changed) {

			if(cancel.test(true) == false) {

				log_notice(__func__, ": recording rule episode discovery data changed -- trigger timer update");
				g_pvr->TriggerTimerUpdate();
			}
		}
	}

	catch(std::exception& ex) { handle_stdexception(__func__, ex); } 
	catch(...) { handle_generalexception(__func__); }

	// Schedule the next periodic invocation of this task to occur at the caclulated interval
	if(cancel.test(true) == false) {

		log_notice(__func__, ": scheduling next recording rule episode update to initiate in ", settings.discover_episodes_interval, " seconds");
		g_scheduler.add(std::chrono::system_clock::now() + std::chrono::seconds(settings.discover_episodes_interval), update_episodes_task);
	}

	else log_notice(__func__, ": recording rule episode update task was cancelled");
}

// update_lineups_task (local)
//
// Scheduled task implementation to update the channel lineups
static void update_lineups_task(scalar_condition<bool> const& cancel)
{
	bool		changed = false;			// Flag if the discovery data changed

	// Create a copy of the current addon settings structure
	struct addon_settings settings = copy_settings();

	try {

		// Update the backend channel lineup information
		if(cancel.test(true) == false) discover_lineups(cancel, changed);

		// Changes to the channel lineups affects the PVR channel and channel group information,
		// and may require a listings update if new channels were added to the lineup
		if(changed) {

			if(cancel.test(true) == false) {

				log_notice(__func__, ": lineup discovery data changed -- trigger channel update");
				g_pvr->TriggerChannelUpdate();
			}

			if(cancel.test(true) == false) {

				log_notice(__func__, ": lineup discovery data changed -- trigger channel group update");
				g_pvr->TriggerChannelGroupsUpdate();
			}

			if(cancel.test(true) == false) {

				log_notice(__func__, ": lineup discovery data changed -- schedule guide listings update");
				g_scheduler.add(std::bind(update_listings_task, false, true, std::placeholders::_1));
			}
		}
	}

	catch(std::exception& ex) { handle_stdexception(__func__, ex); } 
	catch(...) { handle_generalexception(__func__); }

	// Schedule the next periodic invocation of this task to occur at the caclulated interval
	if(cancel.test(true) == false) {

		log_notice(__func__, ": scheduling next lineup update to initiate in ", settings.discover_lineups_interval, " seconds");
		g_scheduler.add(std::chrono::system_clock::now() + std::chrono::seconds(settings.discover_lineups_interval), update_lineups_task);
	}

	else log_notice(__func__, ": lineup update task was cancelled");
}

// update_listings_task (local)
//
// Scheduled task implementation to update the XMLTV listings
static void update_listings_task(bool force, bool checkchannels, scalar_condition<bool> const& cancel)
{
	time_t		lastdiscovery = 0;			// Timestamp indicating the last successful discovery
	bool		changed = false;			// Flag if the discovery data changed

	// Create a copy of the current addon settings structure
	struct addon_settings settings = copy_settings();

	// Determine the time at which this function has been called
	time_t now = time(nullptr);

	// Create a database connection to use during this function
	connectionpool::handle dbhandle(g_connpool);

	// Determine the last time the listings discovery executed successfully
	try { lastdiscovery = get_discovered(dbhandle, "listings"); }
	catch(...) { lastdiscovery = 0; }

	// Force an update if the last discovery was more than 18 hours ago
	if((!force) && (lastdiscovery <= (now - 64800))) force = true;

	// Force an update to the listings if there are lineup channels without any guide information
	if((!force) && (checkchannels) && (has_missing_guide_channels(dbhandle))) {

		force = true;
		log_notice(__func__, ": forcing update due to missing channel(s) in listing data");
	}

	// Calculate the next time the listings discovery should be executed, which is 24 hours from
	// now or the last successful discovery with a +/- 2 hour amount of randomness applied to it
	int delta = std::uniform_int_distribution<int>(-7200, 7200)(g_randomengine);
	time_t nextdiscovery = (force) ? (now + 86400 + delta) : (lastdiscovery + 86400 + delta);

	try {

		// Update the backend XMLTV listing information (changed will always be true here)
		if(cancel.test(true) == false) {

			if(force) discover_listings(cancel, changed);
			else log_notice(__func__, ": listing discovery skipped; data is less than 18 hours old");
		}

		// Trigger a channel update; the metadata (name, icon, etc) may have changed
		if(cancel.test(true) == false) {

			log_notice(__func__, ": triggering channel update");
			g_pvr->TriggerChannelUpdate();
		}

		// Enumerate all of the listings in the database and (re)send them to Kodi
		if(cancel.test(true) == false) {

			log_notice(__func__, ": execute electronic program guide update");

			enumerate_listings(connectionpool::handle(g_connpool), settings.show_drm_protected_channels, g_epgmaxtime.load(), 
				[&](struct listing const& item, bool& cancelenum) -> void {

				EPG_TAG			epgtag;						// EPG_TAG to be transferred to Kodi

				memset(&epgtag, 0, sizeof(EPG_TAG));		// Initialize the structure

				// Abort the enumeration if the cancellation scalar_condition has been set
				if(cancel.test(true) == true) { cancelenum = true; return; }

				// iUniqueBroadcastId (required)
				assert(item.broadcastid > EPG_TAG_INVALID_UID);
				epgtag.iUniqueBroadcastId = item.broadcastid;

				// iUniqueChannelId (required)
				epgtag.iUniqueChannelId = item.channelid;

				// strTitle (required)
				if(item.title == nullptr) return;
				epgtag.strTitle = item.title;

				// startTime (required)
				epgtag.startTime = static_cast<time_t>(item.starttime);

				// endTime (required)
				epgtag.endTime = static_cast<time_t>(item.endtime);

				// strPlot
				epgtag.strPlot = item.synopsis;

				// iYear
				//
				// Only report for program type "MV" (Movies)
				if(strcasecmp(item.programtype, "MV") == 0) epgtag.iYear = item.year;

				// strIconPath
				epgtag.strIconPath = item.iconurl;

				// iGenreType
				epgtag.iGenreType = (settings.use_backend_genre_strings) ? EPG_GENRE_USE_STRING : item.genretype;

				// strGenreDescription
				if(settings.use_backend_genre_strings) epgtag.strGenreDescription = item.genres;

				// firstAired
				//
				// Only report for program types "EP" (Series Episode) and "SH" (Show)
				if(((strcasecmp(item.programtype, "EP") == 0) || (strcasecmp(item.programtype, "SH") == 0)) &&
					(item.originalairdate > 0)) epgtag.firstAired = static_cast<time_t>(item.originalairdate);

				// iSeriesNumber
				epgtag.iSeriesNumber = item.seriesnumber;

				// iEpisodeNumber
				epgtag.iEpisodeNumber = item.episodenumber;

				// iEpisodePartNumber
				epgtag.iEpisodePartNumber = -1;

				// strEpisodeName
				epgtag.strEpisodeName = item.episodename;

				// iFlags
				epgtag.iFlags = EPG_TAG_FLAG_IS_SERIES;

				// strSeriesLink
				epgtag.strSeriesLink = item.seriesid;

				// iStarRating
				epgtag.iStarRating = item.starrating;

				// Transfer the EPG_TAG structure over to Kodi asynchronously
				g_pvr->EpgEventStateChange(&epgtag, EPG_EVENT_STATE::EPG_EVENT_UPDATED);
			});

			// Trigger a timer update after the guide information has been updated
			if(cancel.test(true) == false) {

				log_notice(__func__, ": triggering timer update");
				g_pvr->TriggerTimerUpdate();
			}
		}
	}

	catch(std::exception& ex) { handle_stdexception(__func__, ex); } 
	catch(...) { handle_generalexception(__func__); }

	// Schedule the next periodic invocation of this task to occur at the caclulated interval
	if(cancel.test(true) == false) {

		log_notice(__func__, ": scheduling next listing update to initiate in ", nextdiscovery - now, " seconds");
		g_scheduler.add(std::chrono::system_clock::now() + std::chrono::seconds(nextdiscovery - now), std::bind(update_listings_task, false, false, std::placeholders::_1));
	}

	else log_notice(__func__, ": listing update task was cancelled");
}

// update_recordingrules_task (local)
//
// Scheduled task implementation to update the recording rules and timers
static void update_recordingrules_task(scalar_condition<bool> const& cancel)
{
	bool		changed = false;			// Flag if the discovery data changed

	// Create a copy of the current addon settings structure
	struct addon_settings settings = copy_settings();

	try {

		// Update the backend recording rule information
		if(cancel.test(true) == false) discover_recordingrules(cancel, changed);

		// Changes to the recording rules affects the episode information and PVR timers
		if(changed) {

			// Execute a recording rule episode discovery now; task will reschedule itself
			if(cancel.test(true) == false) {

				log_notice(__func__, ": device discovery data changed -- update recording rule episode discovery now");
				g_scheduler.now(update_episodes_task, cancel);
			}

			// Trigger a PVR timer update (this may be redundant if update_episodes_task already did it)
			if(cancel.test(true) == false) {

				log_notice(__func__, ": recording rule discovery data changed -- trigger timer update");
				g_pvr->TriggerTimerUpdate();
			}
		}
	}

	catch(std::exception& ex) { handle_stdexception(__func__, ex); } 
	catch(...) { handle_generalexception(__func__); }

	// Schedule the next periodic invocation of this task to occur at the caclulated interval
	if(cancel.test(true) == false) {

		log_notice(__func__, ": scheduling next recording rule update to initiate in ", settings.discover_recordingrules_interval, " seconds");
		g_scheduler.add(std::chrono::system_clock::now() + std::chrono::seconds(settings.discover_recordingrules_interval), update_recordingrules_task);
	}

	else log_notice(__func__, ": recording rule update task was cancelled");
}

// update_recordings_task (local)
//
// Scheduled task implementation to update the storage recordings
static void update_recordings_task(scalar_condition<bool> const& cancel)
{
	bool		changed = false;			// Flag if the discovery data changed

	// Create a copy of the current addon settings structure
	struct addon_settings settings = copy_settings();

	try {

		// Update the backend recording information
		if(cancel.test(true) == false) discover_recordings(cancel, changed);

		// Changes to the recordings affects the PVR recording information
		if(changed) {

			if(cancel.test(true) == false) {

				log_notice(__func__, ": recording discovery data changed -- trigger recording update");
				g_pvr->TriggerRecordingUpdate();
			}
		}
	}

	catch(std::exception& ex) { handle_stdexception(__func__, ex); } 
	catch(...) { handle_generalexception(__func__); }

	// Schedule the next periodic invocation of this task to occur at the caclulated interval
	if(cancel.test(true) == false) {

		log_notice(__func__, ": scheduling next recording update to initiate in ", settings.discover_recordings_interval, " seconds");
		g_scheduler.add(std::chrono::system_clock::now() + std::chrono::seconds(settings.discover_recordings_interval), update_recordings_task);
	}

	else log_notice(__func__, ": recording update task was cancelled");
}

// wait_for_devices (local)
//
// Waits until the data required to produce device data has been discovered
static void wait_for_devices(void) noexcept
{
	try {

		// Ensure that the discovery operations have been started
		start_discovery();

		// DEVICES
		g_discovered_devices.wait_until_equals(true);
	}

	catch(std::exception& ex) { handle_stdexception(__func__, ex); } 
	catch(...) { handle_generalexception(__func__); }
}

// wait_for_channels (local)
//
// Waits until the data required to produce channel data has been discovered
static void wait_for_channels(void) noexcept
{
	try {
	
		// Ensure that the discovery operations have been started
		start_discovery();

		// CHANNELS -> { DEVICES + LINEUPS }
		g_discovered_devices.wait_until_equals(true);
		g_discovered_lineups.wait_until_equals(true);
	}
	
	catch(std::exception& ex) { handle_stdexception(__func__, ex); } 
	catch(...) { handle_generalexception(__func__); }
}

// wait_for_network_task (local)
//
// Scheduled task implementation to wait for the network to become available
static void wait_for_network_task(int seconds, scalar_condition<bool> const& cancel)
{
	int attempts = 0;

	// Watch for task cancellation and only retry the operation(s) up to the number of seconds specified
	while((cancel.test(true) == false) && (++attempts < seconds)) {

		if(ipv4_network_available()) { log_notice(__func__, ": IPv4 network connectivity detected"); return; }

		// Sleep for one second before trying the operation again
		log_notice(__func__, ": IPv4 network connectivity not detected; waiting for one second before trying again");
		std::this_thread::sleep_for(std::chrono::seconds(1));
	}

	// Log an error message if the wait operation was aborted due to a timeout condition
	if(attempts >= seconds) log_error(__func__, ": IPv4 network connectivity was not detected within ", seconds, " seconds; giving up");
}

// wait_for_timers (local)
//
// Waits until the data required to produce timer data has been discovered
static void wait_for_timers(void) noexcept
{
	try {

		// Ensure that the discovery operations have been started
		start_discovery();

		// TIMERS -> { DEVICES + LINEUPS + RECORDING RULES + EPISODES }
		g_discovered_devices.wait_until_equals(true);
		g_discovered_lineups.wait_until_equals(true);
		g_discovered_recordingrules.wait_until_equals(true);
		g_discovered_episodes.wait_until_equals(true);
	}

	catch(std::exception& ex) { handle_stdexception(__func__, ex); } 
	catch(...) { handle_generalexception(__func__); }
}

// wait_for_recordings (local)
//
// Waits until the data required to produce recording data has been discovered
static void wait_for_recordings(void) noexcept
{
	try {

		// Ensure that the discovery operations have been started
		start_discovery();

		// RECORDINGS -> { DEVICES + RECORDINGS }
		g_discovered_devices.wait_until_equals(true);
		g_discovered_recordings.wait_until_equals(true);
	}
	
	catch(std::exception& ex) { handle_stdexception(__func__, ex); } 
	catch(...) { handle_generalexception(__func__); }
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
	PVR_MENUHOOK			menuhook;						// For registering menu hooks
	bool					bvalue = false;					// Setting value
	int						nvalue = 0;						// Setting value
	char					strvalue[1024] = { '\0' };		// Setting value 
	scalar_condition<bool>	cancel{false};					// Dummy cancellation flag for tasks

	if((handle == nullptr) || (props == nullptr)) return ADDON_STATUS::ADDON_STATUS_PERMANENT_FAILURE;

	// Seed the pseudo-random number generator
	g_randomengine.seed(static_cast<unsigned int>(time(nullptr)));

	// Copy anything relevant from the provided parameters
	PVR_PROPERTIES* pvrprops = reinterpret_cast<PVR_PROPERTIES*>(props);
	g_epgmaxtime.store(pvrprops->iEpgMaxDays);
	g_userpath.assign(pvrprops->strUserPath);

#ifdef __ANDROID__
	// Uncomment this to allow normal crash dumps to be generated on Android
	// prctl(PR_SET_DUMPABLE, 1);
#endif

	try {

#ifdef _WINDOWS
		// On Windows, initialize winsock in case broadcast discovery is used; WSAStartup is
		// reference-counted so if it has already been called this won't hurt anything
		WSADATA wsaData;
		int wsaresult = WSAStartup(MAKEWORD(2, 2), &wsaData);
		if(wsaresult != 0) throw string_exception(__func__, ": WSAStartup failed with error code ", wsaresult);
#endif

		// Initialize libcurl using the standard default options
		if(curl_global_init(CURL_GLOBAL_DEFAULT) != CURLE_OK) throw string_exception(__func__, ": curl_global_init(CURL_GLOBAL_DEFAULT) failed");

		// Initialize SQLite
		int result = sqlite3_initialize();
		if(result != SQLITE_OK) throw sqlite_exception(result, "sqlite3_initialize() failed");

		// Create the global addon callbacks instance
		g_addon.reset(new ADDON::CHelper_libXBMC_addon());
		if(!g_addon->RegisterMe(handle)) throw string_exception(__func__, ": failed to register addon handle (CHelper_libXBMC_addon::RegisterMe)");

		// Throw a banner out to the Kodi log indicating that the add-on is being loaded
		log_notice(__func__, ": ", VERSION_PRODUCTNAME_ANSI, " v", VERSION_VERSION3_ANSI, " loading");

		try { 

			// The user data path doesn't always exist when an addon has been installed
			if(!g_addon->DirectoryExists(g_userpath.c_str())) {

				log_notice(__func__, ": user data directory ", g_userpath.c_str(), " does not exist");
				if(!g_addon->CreateDirectory(g_userpath.c_str())) throw string_exception(__func__, ": unable to create addon user data directory");
				log_notice(__func__, ": user data directory ", g_userpath.c_str(), " created");
			}

			// Load the general settings
			if(g_addon->GetSetting("pause_discovery_while_streaming", &bvalue)) g_settings.pause_discovery_while_streaming = bvalue;
			if(g_addon->GetSetting("prepend_channel_numbers", &bvalue)) g_settings.prepend_channel_numbers = bvalue;
			if(g_addon->GetSetting("use_episode_number_as_title", &bvalue)) g_settings.use_episode_number_as_title = bvalue;
			if(g_addon->GetSetting("discover_recordings_after_playback", &bvalue)) g_settings.discover_recordings_after_playback = bvalue;
			if(g_addon->GetSetting("use_backend_genre_strings", &bvalue)) g_settings.use_backend_genre_strings = bvalue;
			if(g_addon->GetSetting("show_drm_protected_channels", &bvalue)) g_settings.show_drm_protected_channels = bvalue;
			if(g_addon->GetSetting("use_channel_names_from_lineup", &bvalue)) g_settings.use_channel_names_from_lineup = bvalue;
			if(g_addon->GetSetting("disable_recording_categories", &bvalue)) g_settings.disable_recording_categories = bvalue;
			if(g_addon->GetSetting("generate_repeat_indicators", &bvalue)) g_settings.generate_repeat_indicators = bvalue;
			if(g_addon->GetSetting("delete_datetime_rules_after_v2", &nvalue)) g_settings.delete_datetime_rules_after = nvalue;

			// Load the discovery interval settings
			if(g_addon->GetSetting("discover_devices_interval_v2", &nvalue)) g_settings.discover_devices_interval = nvalue;
			if(g_addon->GetSetting("discover_lineups_interval_v2", &nvalue)) g_settings.discover_lineups_interval = nvalue;
			if(g_addon->GetSetting("discover_recordings_interval_v2", &nvalue)) g_settings.discover_recordings_interval = nvalue;
			if(g_addon->GetSetting("discover_recordingrules_interval_v2", &nvalue)) g_settings.discover_recordingrules_interval = nvalue;
			if(g_addon->GetSetting("discover_episodes_interval_v2", &nvalue)) g_settings.discover_episodes_interval = nvalue;

			// Load the Edit Decision List (EDL) settings
			if(g_addon->GetSetting("enable_recording_edl", &bvalue)) g_settings.enable_recording_edl = bvalue;
			if(g_addon->GetSetting("recording_edl_folder", strvalue)) g_settings.recording_edl_folder.assign(strvalue);
			if(g_addon->GetSetting("recording_edl_folder_2", strvalue)) g_settings.recording_edl_folder_2.assign(strvalue);
			if(g_addon->GetSetting("recording_edl_folder_3", strvalue)) g_settings.recording_edl_folder_3.assign(strvalue);
			if(g_addon->GetSetting("recording_edl_folder_is_flat", &bvalue)) g_settings.recording_edl_folder_is_flat = bvalue;
			if(g_addon->GetSetting("recording_edl_cut_as_comskip", &bvalue)) g_settings.recording_edl_cut_as_comskip = bvalue;
			if(g_addon->GetSetting("recording_edl_start_padding", &nvalue)) g_settings.recording_edl_start_padding = nvalue;
			if(g_addon->GetSetting("recording_edl_end_padding", &nvalue)) g_settings.recording_edl_end_padding = nvalue;

			// Load the advanced settings
			if(g_addon->GetSetting("use_http_device_discovery", &bvalue)) g_settings.use_http_device_discovery = bvalue;
			if(g_addon->GetSetting("use_direct_tuning", &bvalue)) g_settings.use_direct_tuning = bvalue;
			if(g_addon->GetSetting("direct_tuning_protocol", &nvalue)) g_settings.direct_tuning_protocol = static_cast<enum tuning_protocol>(nvalue);
			if(g_addon->GetSetting("stream_read_chunk_size_v2", &nvalue)) g_settings.stream_read_chunk_size = nvalue;
			if(g_addon->GetSetting("deviceauth_stale_after_v2", &nvalue)) g_settings.deviceauth_stale_after = nvalue;

			// Create the global guicallbacks instance
			g_gui.reset(new CHelper_libKODI_guilib());
			if(!g_gui->RegisterMe(handle)) throw string_exception(__func__, ": failed to register gui addon handle (CHelper_libKODI_guilib::RegisterMe)");

			try {

				// Create the global pvrcallbacks instance
				g_pvr.reset(new CHelper_libXBMC_pvr());
				if(!g_pvr->RegisterMe(handle)) throw string_exception(__func__, ": failed to register pvr addon handle (CHelper_libXBMC_pvr::RegisterMe)");
		
				try {

					// MENUHOOK_RECORD_DELETERERECORD
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

					// MENUHOOK_SETTING_TRIGGERLISTINGDISCOVERY
					//
					memset(&menuhook, 0, sizeof(PVR_MENUHOOK));
					menuhook.iHookId = MENUHOOK_SETTING_TRIGGERLISTINGDISCOVERY;
					menuhook.iLocalizedStringId = 30313;
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

					// Generate the local file system and URL-based file names for the PVR database, the file name is based on the version
					std::string databasefile = std::string(g_userpath.c_str()) + "/hdhomerundvr-v" + DATABASE_SCHEMA_VERSION + ".db";
					std::string databasefileuri = "file:///" + databasefile;

					// Create the global database connection pool instance
					try { g_connpool = std::make_shared<connectionpool>(databasefileuri.c_str(), SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE | SQLITE_OPEN_URI); }
					catch(sqlite_exception const& dbex) {

						log_error(__func__, ": unable to create/open the PVR database ", databasefile, " - ", dbex.what());
						
						// If any SQLite-specific errors were thrown during database open/create, attempt to delete and recreate the database
						log_notice(__func__, ": attempting to delete and recreate the PVR database");
						g_addon->DeleteFile(databasefile.c_str());
						g_connpool = std::make_shared<connectionpool>(databasefileuri.c_str(), SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE | SQLITE_OPEN_URI);
						log_notice(__func__, ": successfully recreated the PVR database");
					}

					// Start the task scheduler
					try { g_scheduler.start(); }
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
	// Throw a message out to the Kodi log indicating that the add-on is being unloaded
	log_notice(__func__, ": ", VERSION_PRODUCTNAME_ANSI, " v", VERSION_VERSION3_ANSI, " unloading");

	g_pvrstream.reset();					// Destroy any active stream instance
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

	// Clean up SQLite
	sqlite3_shutdown();

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

	// discover_recordings_after_playback
	//
	else if(strcmp(name, "discover_recordings_after_playback") == 0) {

		bool bvalue = *reinterpret_cast<bool const*>(value);
		if(bvalue != g_settings.discover_recordings_after_playback) {

			g_settings.discover_recordings_after_playback = bvalue;
			log_notice(__func__, ": setting discover_recordings_after_playback changed to ", (bvalue) ? "true" : "false");
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

	// use_channel_names_from_lineup
	//
	else if(strcmp(name, "use_channel_names_from_lineup") == 0) {

		bool bvalue = *reinterpret_cast<bool const*>(value);
		if(bvalue != g_settings.use_channel_names_from_lineup) {

			g_settings.use_channel_names_from_lineup = bvalue;
			log_notice(__func__, ": setting use_channel_names_from_lineup changed to ", (bvalue) ? "true" : "false", " -- trigger channel and channel group updates");
			g_pvr->TriggerChannelUpdate();
			g_pvr->TriggerChannelGroupsUpdate();
		}
	}

	// disable_recording_categories
	//
	else if(strcmp(name, "disable_recording_categories") == 0) {

		bool bvalue = *reinterpret_cast<bool const*>(value);
		if(bvalue != g_settings.disable_recording_categories) {

			g_settings.disable_recording_categories = bvalue;
			log_notice(__func__, ": setting disable_recording_categories changed to ", (bvalue) ? "true" : "false", " -- trigger recording update");
			g_pvr->TriggerRecordingUpdate();
		}
	}

	// generate_repeat_indicators
	//
	else if(strcmp(name, "generate_repeat_indicators") == 0) {

		bool bvalue = *reinterpret_cast<bool const*>(value);
		if(bvalue != g_settings.generate_repeat_indicators) {

			g_settings.generate_repeat_indicators = bvalue;
			log_notice(__func__, ": setting generate_repeat_indicators changed to ", (bvalue) ? "true" : "false", " -- trigger recording update");
			g_pvr->TriggerRecordingUpdate();
		}
	}

	// delete_datetime_rules_after
	//
	else if(strcmp(name, "delete_datetime_rules_after_v2") == 0) {

		int nvalue = *reinterpret_cast<int const*>(value);
		if(nvalue != g_settings.delete_datetime_rules_after) {

			g_settings.delete_datetime_rules_after = nvalue;
			log_notice(__func__, ": setting delete_datetime_rules_after changed to ", nvalue, " seconds -- execute recording rule update");
			g_scheduler.add(update_recordingrules_task);
		}
	}

	// discover_devices_interval
	//
	else if(strcmp(name, "discover_devices_interval_v2") == 0) {

		int nvalue = *reinterpret_cast<int const*>(value);
		if(nvalue != g_settings.discover_devices_interval) {

			// Reschedule the update_devices_task to execute at the specified interval from now
			g_settings.discover_devices_interval = nvalue;
			log_notice(__func__, ": setting discover_devices_interval changed -- rescheduling update task to initiate in ", nvalue, " seconds");
			g_scheduler.add(now + std::chrono::seconds(nvalue), update_devices_task);
		}
	}

	// discover_episodes_interval
	//
	else if(strcmp(name, "discover_episodes_interval_v2") == 0) {

		int nvalue = *reinterpret_cast<int const*>(value);
		if(nvalue != g_settings.discover_episodes_interval) {

			// Reschedule the update_episodes_task to execute at the specified interval from now
			g_settings.discover_episodes_interval = nvalue;
			log_notice(__func__, ": setting discover_episodes_interval changed -- rescheduling update task to initiate in ", nvalue, " seconds");
			g_scheduler.add(now + std::chrono::seconds(nvalue), update_episodes_task);
		}
	}

	// discover_lineups_interval
	//
	else if(strcmp(name, "discover_lineups_interval_v2") == 0) {

		int nvalue = *reinterpret_cast<int const*>(value);
		if(nvalue != g_settings.discover_lineups_interval) {

			// Reschedule the update_lineups_task to execute at the specified interval from now
			g_settings.discover_lineups_interval = nvalue;
			log_notice(__func__, ": setting discover_lineups_interval changed -- rescheduling update task to initiate in ", nvalue, " seconds");
			g_scheduler.add(now + std::chrono::seconds(nvalue), update_lineups_task);
		}
	}

	// discover_recordingrules_interval
	//
	else if(strcmp(name, "discover_recordingrules_interval_v2") == 0) {

		int nvalue = *reinterpret_cast<int const*>(value);
		if(nvalue != g_settings.discover_recordingrules_interval) {

			// Reschedule the update_recordingrules_task to execute at the specified interval from now
			g_settings.discover_recordingrules_interval = nvalue;
			log_notice(__func__, ": setting discover_recordingrules_interval changed -- rescheduling update task to initiate in ", nvalue, " seconds");
			g_scheduler.add(now + std::chrono::seconds(nvalue), update_recordingrules_task);
		}
	}

	// discover_recordings_interval
	//
	else if(strcmp(name, "discover_recordings_interval_v2") == 0) {

	int nvalue = *reinterpret_cast<int const*>(value);
	if(nvalue != g_settings.discover_recordings_interval) {

		// Reschedule the update_recordings_task to execute at the specified interval from now
		g_settings.discover_recordings_interval = nvalue;
		log_notice(__func__, ": setting discover_recordings_interval changed -- rescheduling update task to initiate in ", nvalue, " seconds");
		g_scheduler.add(now + std::chrono::seconds(nvalue), update_recordings_task);
	}
	}

	// use_http_device_discovery
	//
	else if(strcmp(name, "use_http_device_discovery") == 0) {

		bool bvalue = *reinterpret_cast<bool const*>(value);
		if(bvalue != g_settings.use_http_device_discovery) {

			g_settings.use_http_device_discovery = bvalue;
			log_notice(__func__, ": setting use_http_device_discovery changed to ", (bvalue) ? "true" : "false", " -- schedule device update");

			// Reschedule the device update task to run as soon as possible
			g_scheduler.add(update_devices_task);
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

	// direct_tuning_protocol
	//
	else if(strcmp(name, "direct_tuning_protocol") == 0) {

		int nvalue = *reinterpret_cast<int const*>(value);
		if(nvalue != static_cast<int>(g_settings.direct_tuning_protocol)) {

			g_settings.direct_tuning_protocol = static_cast<enum tuning_protocol>(nvalue);
			log_notice(__func__, ": setting direct_tuning_protocol changed to ", (g_settings.direct_tuning_protocol == tuning_protocol::http) ? "HTTP" : "RTP/UDP");
		}
	}

	// stream_read_chunk_size
	//
	else if(strcmp(name, "stream_read_chunk_size_v2") == 0) {

		int nvalue = *reinterpret_cast<int const*>(value);
		if(nvalue != g_settings.stream_read_chunk_size) {

			g_settings.stream_read_chunk_size = nvalue;
			log_notice(__func__, ": setting stream_read_chunk_size changed to ", nvalue, " bytes");
		}
	}

	// deviceauth_stale_after
	//
	else if(strcmp(name, "deviceauth_stale_after_v2") == 0) {

		int nvalue = *reinterpret_cast<int const*>(value);
		if(nvalue != g_settings.deviceauth_stale_after) {

			g_settings.deviceauth_stale_after = nvalue;
			log_notice(__func__, ": setting deviceauth_stale_after changed to ", nvalue, " seconds -- schedule device discovery");

			// Reschedule the device discovery task to run as soon as possible
			g_scheduler.add(update_devices_task);
		}
	}

	// enable_recording_edl
	//
	else if(strcmp(name, "enable_recording_edl") == 0) {

		bool bvalue = *reinterpret_cast<bool const*>(value);
		if(bvalue != g_settings.enable_recording_edl) {

			g_settings.enable_recording_edl = bvalue;
			log_notice(__func__, ": setting enable_recording_edl changed to ", (bvalue) ? "true" : "false");
		}
	}

	// recording_edl_folder
	//
	else if(strcmp(name, "recording_edl_folder") == 0) {

		if(strcmp(g_settings.recording_edl_folder.c_str(), reinterpret_cast<char const*>(value)) != 0) {

			g_settings.recording_edl_folder.assign(reinterpret_cast<char const*>(value));
			log_notice(__func__, ": setting recording_edl_folder changed to ", g_settings.recording_edl_folder.c_str());
		}
	}

	// recording_edl_folder_2
	//
	else if(strcmp(name, "recording_edl_folder_2") == 0) {

		if(strcmp(g_settings.recording_edl_folder_2.c_str(), reinterpret_cast<char const*>(value)) != 0) {

			g_settings.recording_edl_folder_2.assign(reinterpret_cast<char const*>(value));
			log_notice(__func__, ": setting recording_edl_folder_2 changed to ", g_settings.recording_edl_folder_2.c_str());
		}
	}

	// recording_edl_folder_3
	//
	else if(strcmp(name, "recording_edl_folder_3") == 0) {

		if(strcmp(g_settings.recording_edl_folder_3.c_str(), reinterpret_cast<char const*>(value)) != 0) {

			g_settings.recording_edl_folder_3.assign(reinterpret_cast<char const*>(value));
			log_notice(__func__, ": setting recording_edl_folder_3 changed to ", g_settings.recording_edl_folder_3.c_str());
		}
	}

	// recording_edl_folder_is_flat
	//
	else if(strcmp(name, "recording_edl_folder_is_flat") == 0) {

		bool bvalue = *reinterpret_cast<bool const*>(value);
		if(bvalue != g_settings.recording_edl_folder_is_flat) {

			g_settings.recording_edl_folder_is_flat = bvalue;
			log_notice(__func__, ": setting recording_edl_folder_is_flat changed to ", (bvalue) ? "true" : "false");
		}
	}

	// recording_edl_cut_as_comskip
	//
	else if(strcmp(name, "recording_edl_cut_as_comskip") == 0) {

		bool bvalue = *reinterpret_cast<bool const*>(value);
		if(bvalue != g_settings.recording_edl_cut_as_comskip) {

			g_settings.recording_edl_cut_as_comskip = bvalue;
			log_notice(__func__, ": setting recording_edl_cut_as_comskip changed to ", (bvalue) ? "true" : "false");
		}
	}

	// recording_edl_start_padding
	//
	else if(strcmp(name, "recording_edl_start_padding") == 0) {

		int nvalue = *reinterpret_cast<int const*>(value);
		if(nvalue != g_settings.recording_edl_start_padding) {

			g_settings.recording_edl_start_padding = nvalue;
			log_notice(__func__, ": setting recording_edl_start_padding changed to ", nvalue, " milliseconds");
		}
	}

	// recording_edl_end_padding
	//
	else if(strcmp(name, "recording_edl_end_padding") == 0) {

		int nvalue = *reinterpret_cast<int const*>(value);
		if(nvalue != g_settings.recording_edl_end_padding) {

			g_settings.recording_edl_end_padding = nvalue;
			log_notice(__func__, ": setting recording_edl_end_padding changed to ", nvalue, " milliseconds");
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
	return "api.hdhomerun.com";
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

PVR_ERROR GetDriveSpace(long long* total, long long* used)
{
	struct storage_space		space { 0, 0 };		// Disk space returned from database layer

	// Wait until the device information has been discovered for the first time
	wait_for_devices();

	try {
		
		// Attempt to get the available total and available space for the system, but return NOT_IMPLEMENTED
		// instead of an error code if the total value isn't available - this info wasn't always available
		space = get_available_storage_space(connectionpool::handle(g_connpool));
		if(space.total == 0) return PVR_ERROR::PVR_ERROR_NOT_IMPLEMENTED;

		// The reported values are multiplied by 1024 for some reason; accomodate the delta here
		*total = space.total / 1024;
		*used = (space.total - space.available) / 1024;	
	}

	catch(std::exception& ex) { return handle_stdexception(__func__, ex, PVR_ERROR_NOT_IMPLEMENTED); }
	catch(...) { return handle_generalexception(__func__, PVR_ERROR_NOT_IMPLEMENTED); }

	return PVR_ERROR::PVR_ERROR_NO_ERROR;
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

	// MENUHOOK_RECORD_DELETERERECORD
	//
	if((menuhook.iHookId == MENUHOOK_RECORD_DELETERERECORD) && (item.cat == PVR_MENUHOOK_RECORDING)) {

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

				if(device_name.name != nullptr) names.append(std::string(device_name.name) + "\r\n");
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

			log_notice(__func__, ": scheduling device update task");
			g_scheduler.add(update_devices_task);
		}

		catch(std::exception& ex) { return handle_stdexception(__func__, ex, PVR_ERROR::PVR_ERROR_FAILED); }
		catch(...) { return handle_generalexception(__func__, PVR_ERROR::PVR_ERROR_FAILED); }
		
		return PVR_ERROR::PVR_ERROR_NO_ERROR;
	}

	// MENUHOOK_SETTING_TRIGGERLINEUPDISCOVERY
	//
	else if(menuhook.iHookId == MENUHOOK_SETTING_TRIGGERLINEUPDISCOVERY) {

		try {

			log_notice(__func__, ": scheduling lineup update task");
			g_scheduler.add(update_lineups_task);
		}

		catch(std::exception& ex) { return handle_stdexception(__func__, ex, PVR_ERROR::PVR_ERROR_FAILED); }
		catch(...) { return handle_generalexception(__func__, PVR_ERROR::PVR_ERROR_FAILED); }
		
		return PVR_ERROR::PVR_ERROR_NO_ERROR;
	}

	// MENUHOOK_SETTING_TRIGGERLISTINGDISCOVERY
	//
	else if(menuhook.iHookId == MENUHOOK_SETTING_TRIGGERLISTINGDISCOVERY) {

		try {

			log_notice(__func__, ": scheduling listing update task (forced)");
			g_scheduler.add(std::bind(update_listings_task, true, true, std::placeholders::_1));
		}

		catch(std::exception& ex) { return handle_stdexception(__func__, ex, PVR_ERROR::PVR_ERROR_FAILED); }
		catch(...) { return handle_generalexception(__func__, PVR_ERROR::PVR_ERROR_FAILED); }
		
		return PVR_ERROR::PVR_ERROR_NO_ERROR;
	}

	// MENUHOOK_SETTING_TRIGGERRECORDINGRULEDISCOVERY
	//
	else if(menuhook.iHookId == MENUHOOK_SETTING_TRIGGERRECORDINGRULEDISCOVERY) {

		try {

			log_notice(__func__, ": scheduling recording rule update task");
			g_scheduler.add(update_recordingrules_task);
		}

		catch(std::exception& ex) { return handle_stdexception(__func__, ex, PVR_ERROR::PVR_ERROR_FAILED); }
		catch(...) { return handle_generalexception(__func__, PVR_ERROR::PVR_ERROR_FAILED); }
		
		return PVR_ERROR::PVR_ERROR_NO_ERROR;
	}

	// MENUHOOK_SETTING_TRIGGERRECORDINGDISCOVERY
	//
	else if(menuhook.iHookId == MENUHOOK_SETTING_TRIGGERRECORDINGDISCOVERY) {

		try {

			log_notice(__func__, ": scheduling recording update task");
			g_scheduler.add(update_recordings_task);
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
		
			log_notice(__func__, ": channel ", item.data.channel.strChannelName, " disabled; scheduling lineup update task");
			g_scheduler.add(update_lineups_task);
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
		
			log_notice(__func__, ": channel ", item.data.channel.strChannelName, " added as favorite; scheduling lineup update task");
			g_scheduler.add(update_lineups_task);
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
		
			log_notice(__func__, ": channel ", item.data.channel.strChannelName, " removed from favorites; scheduling lineup update task");
			g_scheduler.add(update_lineups_task);
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

PVR_ERROR GetEPGForChannel(ADDON_HANDLE handle, int channel, time_t start, time_t end)
{
	if(handle == nullptr) return PVR_ERROR::PVR_ERROR_INVALID_PARAMETERS;

	// Create a copy of the current addon settings structure
	struct addon_settings settings = copy_settings();

	// Convert the channel identifier back into a channelid
	union channelid channelid;
	channelid.value = channel;

	try {

		// Enumerate all of the listings in the database for this channel and time frame
		enumerate_listings(connectionpool::handle(g_connpool), channelid, start, end, [&](struct listing const& item, bool&) -> void {

			EPG_TAG			epgtag;						// EPG_TAG to be transferred to Kodi

			memset(&epgtag, 0, sizeof(EPG_TAG));		// Initialize the structure

			// Don't send EPG entries with start/end times outside the requested range
			if((item.starttime > end) || (item.endtime < start)) return;

			// iUniqueBroadcastId (required)
			assert(item.broadcastid > EPG_TAG_INVALID_UID);
			epgtag.iUniqueBroadcastId = item.broadcastid;

			// iUniqueChannelId (required)
			epgtag.iUniqueChannelId = item.channelid;

			// strTitle (required)
			if(item.title == nullptr) return;
			epgtag.strTitle = item.title;

			// startTime (required)
			epgtag.startTime = static_cast<time_t>(item.starttime);

			// endTime (required)
			epgtag.endTime = static_cast<time_t>(item.endtime);

			// strPlot
			epgtag.strPlot = item.synopsis;

			// iYear
			//
			// Only report for program type "MV" (Movies)
			if(strcasecmp(item.programtype, "MV") == 0) epgtag.iYear = item.year;

			// strIconPath
			epgtag.strIconPath = item.iconurl;

			// iGenreType
			epgtag.iGenreType = (settings.use_backend_genre_strings) ? EPG_GENRE_USE_STRING : item.genretype;

			// strGenreDescription
			if(settings.use_backend_genre_strings) epgtag.strGenreDescription = item.genres;

			// firstAired
			//
			// Only report for program types "EP" (Series Episode) and "SH" (Show)
			if(((strcasecmp(item.programtype, "EP") == 0) || (strcasecmp(item.programtype, "SH") == 0)) &&
				(item.originalairdate > 0)) epgtag.firstAired = static_cast<time_t>(item.originalairdate);

			// iSeriesNumber
			epgtag.iSeriesNumber = item.seriesnumber;

			// iEpisodeNumber
			epgtag.iEpisodeNumber = item.episodenumber;

			// iEpisodePartNumber
			epgtag.iEpisodePartNumber = -1;

			// strEpisodeName
			epgtag.strEpisodeName = item.episodename;

			// iFlags
			epgtag.iFlags = EPG_TAG_FLAG_IS_SERIES;

			// strSeriesLink
			epgtag.strSeriesLink = item.seriesid;

			// iStarRating
			epgtag.iStarRating = item.starrating;

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
// GetEPGTagEdl
//
// Retrieve the edit decision list (EDL) of an EPG tag on the backend
//
// Arguments:
//
//	tag			- EPG tag
//	edl			- The function has to write the EDL list into this array
//	count		- The maximum size of the EDL, out: the actual size of the EDL

PVR_ERROR GetEPGTagEdl(EPG_TAG const* /*tag*/, PVR_EDL_ENTRY /*edl*/[], int* /*count*/)
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
	return 4;		// "Favorite Channels", "HD Channels", "SD Channels" and "Demo Channels"
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
	snprintf(group.strGroupName, std::extent<decltype(group.strGroupName)>::value, "Favorite channels");
	g_pvr->TransferChannelGroup(handle, &group);

	// HD Channels
	snprintf(group.strGroupName, std::extent<decltype(group.strGroupName)>::value, "HD channels");
	g_pvr->TransferChannelGroup(handle, &group);

	// SD Channels
	snprintf(group.strGroupName, std::extent<decltype(group.strGroupName)>::value, "SD channels");
	g_pvr->TransferChannelGroup(handle, &group);

	// Demo Channels
	snprintf(group.strGroupName, std::extent<decltype(group.strGroupName)>::value, "Demo channels");
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

	// Wait until the channel information has been discovered the first time
	wait_for_channels();

	// Determine which group enumerator to use for the operation, there are only four to
	// choose from: "Favorite Channels", "HD Channels", "SD Channels" and "Demo Channels"
	std::function<void(sqlite3*, bool, enumerate_channelids_callback)> enumerator = nullptr;
	if(strcmp(group.strGroupName, "Favorite channels") == 0) enumerator = enumerate_favorite_channelids;
	else if(strcmp(group.strGroupName, "HD channels") == 0) enumerator = enumerate_hd_channelids;
	else if(strcmp(group.strGroupName, "SD channels") == 0) enumerator = enumerate_sd_channelids;
	else if(strcmp(group.strGroupName, "Demo channels") == 0) enumerator = enumerate_demo_channelids;

	// If neither enumerator was selected, there isn't any work to do here
	if(enumerator == nullptr) return PVR_ERROR::PVR_ERROR_NO_ERROR;

	// Create a copy of the current addon settings structure
	struct addon_settings settings = copy_settings();

	try {

		// Enumerate all of the channels in the specified group
		enumerator(connectionpool::handle(g_connpool), settings.show_drm_protected_channels, [&](union channelid const& item) -> void {

			PVR_CHANNEL_GROUP_MEMBER member;						// PVR_CHANNEL_GROUP_MEMORY to send
			memset(&member, 0, sizeof(PVR_CHANNEL_GROUP_MEMBER));	// Initialize the structure

			// strGroupName (required)
			snprintf(member.strGroupName, std::extent<decltype(member.strGroupName)>::value, "%s", group.strGroupName);

			// iChannelUniqueId (required)
			member.iChannelUniqueId = item.value;

			// iChannelNumber
			member.iChannelNumber = static_cast<int>(item.parts.channel);

			// iSubChannelNumber
			member.iSubChannelNumber = static_cast<int>(item.parts.subchannel);

			// Transfer the generated PVR_CHANNEL_GROUP_MEMBER structure over to Kodi
			g_pvr->TransferChannelGroupMember(handle, &member);
		});
	}

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
	// Wait until the channel information has been discovered the first time
	wait_for_channels();

	// Create a copy of the current addon settings structure
	struct addon_settings settings = copy_settings();

	try { return get_channel_count(connectionpool::handle(g_connpool), settings.show_drm_protected_channels); }
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

	// Wait until the channel information has been discovered the first time
	wait_for_channels();

	// Create a copy of the current addon settings structure
	struct addon_settings settings = copy_settings();

	try {

		// Enumerate all of the channels in the database
		enumerate_channels(connectionpool::handle(g_connpool), settings.prepend_channel_numbers, settings.show_drm_protected_channels, settings.use_channel_names_from_lineup, [&](struct channel const& item) -> void {

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

			// Transfer the PVR_CHANNEL structure over to Kodi
			g_pvr->TransferChannelEntry(handle, &channel);
		});
	}
	
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

	// Wait until the recording information has been discovered the first time
	wait_for_recordings();

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

	// Wait until the recording information has been discovered the first time
	wait_for_recordings();

	// Create a copy of the current addon settings structure
	struct addon_settings settings = copy_settings();

	try {

		// Enumerate all of the recordings in the database
		enumerate_recordings(connectionpool::handle(g_connpool), settings.use_episode_number_as_title, settings.disable_recording_categories, [&](struct recording const& item) -> void {

			PVR_RECORDING recording;							// PVR_RECORDING to be transferred to Kodi
			memset(&recording, 0, sizeof(PVR_RECORDING));		// Initialize the structure

			// Determine if the episode is a repeat.  If the program type is "EP" or "SH" and firstairing is *not* set, flag it as a repeat
			bool isrepeat = ((item.programtype != nullptr) && ((strcasecmp(item.programtype, "EP") == 0) || (strcasecmp(item.programtype, "SH") == 0)) && (item.firstairing == 0));

			// strRecordingId (required)
			if(item.recordingid == nullptr) return;
			snprintf(recording.strRecordingId, std::extent<decltype(recording.strRecordingId)>::value, "%s", item.recordingid);
			
			// strTitle (required)
			if(item.title == nullptr) return;
			snprintf(recording.strTitle, std::extent<decltype(recording.strTitle)>::value, "%s", item.title);

			// strEpisodeName
			if(item.episodename != nullptr) snprintf(recording.strEpisodeName, std::extent<decltype(recording.strEpisodeName)>::value, "%s%s", item.episodename, 
				((isrepeat) && (settings.generate_repeat_indicators)) ? " [R]" : "");

			// iSeriesNumber
			recording.iSeriesNumber = item.seriesnumber;
			 
			// iEpisodeNumber
			recording.iEpisodeNumber = item.episodenumber;

			// iYear
			//
			// Only report for program type "MV" (Movies)
			if(strcasecmp(item.programtype, "MV") == 0) recording.iYear = item.year;

			// strDirectory
			if(item.directory != nullptr) {
				
				// Special case: "movie" --> #30402
				if(strcasecmp(item.directory, "movie") == 0) 
					snprintf(recording.strDirectory, std::extent<decltype(recording.strDirectory)>::value, "%s", g_addon->GetLocalizedString(30402));

				// Special case: "sport" --> #30403
				else if(strcasecmp(item.directory, "sport") == 0)
					snprintf(recording.strDirectory, std::extent<decltype(recording.strDirectory)>::value, "%s", g_addon->GetLocalizedString(30403));

				// Special case: "special" --> #30404
				else if(strcasecmp(item.directory, "special") == 0)
					snprintf(recording.strDirectory, std::extent<decltype(recording.strDirectory)>::value, "%s", g_addon->GetLocalizedString(30404));

				// Special case: "news" --> #30405
				else if(strcasecmp(item.directory, "news") == 0)
					snprintf(recording.strDirectory, std::extent<decltype(recording.strDirectory)>::value, "%s", g_addon->GetLocalizedString(30405));

				else snprintf(recording.strDirectory, std::extent<decltype(recording.strDirectory)>::value, "%s", item.directory);
			}

			// strPlot
			if(item.plot != nullptr) snprintf(recording.strPlot, std::extent<decltype(recording.strPlot)>::value, "%s", item.plot);

			// strChannelName
			if(item.channelname != nullptr) snprintf(recording.strChannelName, std::extent<decltype(recording.strChannelName)>::value, "%s", item.channelname);

			// strThumbnailPath
			if(item.thumbnailpath != nullptr) snprintf(recording.strThumbnailPath, std::extent<decltype(recording.strThumbnailPath)>::value, "%s", item.thumbnailpath);

			// recordingTime
			recording.recordingTime = static_cast<time_t>(item.recordingtime);

			// iDuration
			recording.iDuration = item.duration;
			assert(recording.iDuration > 0);

			// iLastPlayedPosition
			//
			recording.iLastPlayedPosition = item.lastposition;

			// iChannelUid
			recording.iChannelUid = item.channelid.value;

			// channelType
			recording.channelType = PVR_RECORDING_CHANNEL_TYPE_TV;

			//  Transfer the generated PVR_RECORDING structure over to Kodi 
			g_pvr->TransferRecordingEntry(handle, &recording);
		});
	}
	
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

PVR_ERROR GetRecordingEdl(PVR_RECORDING const& recording, PVR_EDL_ENTRY edl[], int* count)
{
	std::vector<PVR_EDL_ENTRY>		entries;			// vector<> of PVR_EDL_ENTRYs

	if(count == nullptr) return PVR_ERROR::PVR_ERROR_INVALID_PARAMETERS;
	if((*count) && (edl == nullptr)) return PVR_ERROR::PVR_ERROR_INVALID_PARAMETERS;

	memset(edl, 0, sizeof(PVR_EDL_ENTRY) * (*count));		// Initialize [out] array

	try {

		// Create a copy of the current addon settings structure and check if EDL is enabled
		struct addon_settings settings = copy_settings();
		if(!settings.enable_recording_edl) return PVR_ERROR::PVR_ERROR_NOT_IMPLEMENTED;

		// Generate the base file name for the recording by combining the folder with the recording metadata
		std::string basename = get_recording_filename(connectionpool::handle(g_connpool), recording.strRecordingId, settings.recording_edl_folder_is_flat);
		if(basename.length() == 0) throw string_exception(__func__, ": unable to determine the base file name of the specified recording");

		// Remove any extension present on the base file name
		size_t extindex = basename.find_last_of('.');
		if(extindex != std::string::npos) basename = basename.substr(0, extindex);

		// Attempt to locate a matching .EDL file based on the configured directories
		std::string filename = settings.recording_edl_folder.append(basename).append(".edl");
		if(!g_addon->FileExists(filename.c_str(), false)) {

			// Check secondary EDL directory
			filename = settings.recording_edl_folder_2.append(basename).append(".edl");
			if(!g_addon->FileExists(filename.c_str(), false)) {

				// Check tertiary EDL directory
				filename = settings.recording_edl_folder_3.append(basename).append(".edl");
				if(!g_addon->FileExists(filename.c_str(), false)) {

					// If the .EDL file was not found anywhere, log a notice but return NO_ERROR back to Kodi -- this is not fatal
					log_notice(__func__, ": edit decision list for recording ", basename.c_str(), " was not found in any configured EDL file directories");
					return PVR_ERROR::PVR_ERROR_NOT_IMPLEMENTED;
				}
			}
		}

		// 2 KiB should be more than sufficient to hold a single line from the .edl file
		std::unique_ptr<char[]> line(new char[2 KiB]);

		// Attempt to open the input edit decision list file
		void* handle = g_addon->OpenFile(filename.c_str(), 0);
		if(handle != nullptr) {

			size_t linenumber = 0;
			log_notice(__func__, ": processing edit decision list file: ", filename.c_str());

			// Process each line of the file individually
			while(g_addon->ReadFileString(handle, &line[0], 2 KiB)) {

				++linenumber;									// Increment the line number

				float			start	= 0.0F;					// Starting point, in milliseconds
				float			end		= 0.0F;					// Ending point, in milliseconds
				int				type	= PVR_EDL_TYPE_CUT;		// Type of edit to be made

				// The only currently supported format for EDL is the {float|float|[int]} format, as the
				// frame rate of the recording would be required to process the {#frame|#frame|[int]} format
				if(sscanf(&line[0], "%f %f %i", &start, &end, &type) >= 2) {

					// Apply any user-specified adjustments to the start and end times accordingly
					start += (static_cast<float>(settings.recording_edl_start_padding) / 1000.0F);
					end -= (static_cast<float>(settings.recording_edl_end_padding) / 1000.0F);
						
					// Ensure the start and end times are positive and do not overlap
					start = std::min(std::max(start, 0.0F), std::max(end, 0.0F));
					end = std::max(std::max(end, 0.0F), std::max(start, 0.0F));

					// Replace CUT indicators with COMSKIP indicators if requested
					if((static_cast<PVR_EDL_TYPE>(type) == PVR_EDL_TYPE_CUT) && (g_settings.recording_edl_cut_as_comskip)) type = static_cast<int>(PVR_EDL_TYPE_COMBREAK);

					// Log the adjusted values for the entry and add a PVR_EDL_ENTRY to the vector<>
					log_notice(__func__, ": adding edit decision list entry (start=", start, "s, end=", end, "s, type=", edltype_to_string(static_cast<PVR_EDL_TYPE>(type)), ")");
					entries.emplace_back(PVR_EDL_ENTRY { static_cast<int64_t>(static_cast<double>(start) * 1000.0), static_cast<int64_t>(static_cast<double>(end) * 1000.0), static_cast<PVR_EDL_TYPE>(type)});
				}

				else log_error(__func__, ": invalid edit decision list entry detected at line #", linenumber);
			}
				
			g_addon->CloseFile(handle);
		}

		else log_error(__func__, ": unable to open edit decision list file: ", filename.c_str());

		// Copy the parsed entries, if any, from the vector<> into the output array
		*count = static_cast<int>(std::min(entries.size(), static_cast<size_t>(*count)));
		memcpy(edl, entries.data(), (*count * sizeof(PVR_EDL_ENTRY)));

		return PVR_ERROR::PVR_ERROR_NO_ERROR;
	}
	
	catch(std::exception& ex) { return handle_stdexception(__func__, ex, PVR_ERROR::PVR_ERROR_FAILED); }
	catch(...) { return handle_generalexception(__func__, PVR_ERROR::PVR_ERROR_FAILED); }

	return PVR_ERROR::PVR_ERROR_NO_ERROR;
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
	// Wait until the timer information has been discovered the first time
	wait_for_timers();

	try {
		
		// Pull a database connection out from the connection pool
		connectionpool::handle dbhandle(g_connpool);

		// Return the sum of the timer rules and the invidual timers themselves
		return get_recordingrule_count(dbhandle) + get_timer_count(dbhandle, g_epgmaxtime.load()); 
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
	assert(g_pvr);				

	if(handle == nullptr) return PVR_ERROR::PVR_ERROR_INVALID_PARAMETERS;

	// Wait until the timer information has been discovered the first time
	wait_for_timers();

	time_t now = time(nullptr);				// Get the current date/time for comparison

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
			timer.iClientChannelUid = static_cast<int>(item.channelid.value);

			// startTime
			timer.startTime = (item.type == recordingrule_type::datetimeonly) ? static_cast<time_t>(item.datetimeonly) : now;

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

			// firstDay
			if(item.type == recordingrule_type::datetimeonly) timer.firstDay = static_cast<time_t>(item.datetimeonly);

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

			// Transfer the generated PVR_TIMER structure over to Kodi
			g_pvr->TransferTimerEntry(handle, &timer);
		});

		// Enumerate all of the timers in the database
		enumerate_timers(dbhandle, g_epgmaxtime.load(), [&](struct timer const& item) -> void {

			PVR_TIMER timer;							// PVR_TIMER to be transferred to Kodi
			memset(&timer, 0, sizeof(PVR_TIMER));		// Initialize the structure

			// iClientIndex (required)
			timer.iClientIndex = item.timerid;

			// iParentClientIndex
			timer.iParentClientIndex = item.recordingruleid;

			// iClientChannelUid
			timer.iClientChannelUid = static_cast<int>(item.channelid.value);

			// startTime
			timer.startTime = static_cast<time_t>(item.starttime);

			// endTime
			timer.endTime = static_cast<time_t>(item.endtime);

			// state (required)
			if(timer.endTime < now) timer.state = PVR_TIMER_STATE_COMPLETED;
			else if((now >= timer.startTime) && (now <= timer.endTime)) timer.state = PVR_TIMER_STATE_RECORDING;
			else timer.state = PVR_TIMER_STATE_SCHEDULED;

			// iTimerType (required)
			timer.iTimerType = (item.parenttype == recordingrule_type::series) ? timer_type::seriestimer : timer_type::datetimeonlytimer;

			// strTitle (required)
			if(item.title == nullptr) return;
			snprintf(timer.strTitle, std::extent<decltype(timer.strTitle)>::value, "%s", item.title);

			// iEpgUid
			timer.iEpgUid = static_cast<unsigned int>(item.starttime);

			// strSeriesLink
			snprintf(timer.strSeriesLink, std::extent<decltype(timer.strSeriesLink)>::value, "%s", item.seriesid);

			// Transfer the generated PVR_TIMER structure over to Kodi
			g_pvr->TransferTimerEntry(handle, &timer);
		});
	}
	
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

	assert(g_pvr && g_gui);

	// Get the current time as a unix timestamp, used to set up AfterOriginalAirdateOnly
	time_t now = time(nullptr);

	// Create an initialize a new recordingrule to be passed to the database
	struct recordingrule recordingrule;
	memset(&recordingrule, 0, sizeof(struct recordingrule));

	try {

		// Pull a database connection out from the connection pool
		connectionpool::handle dbhandle(g_connpool);

		// This operation is only available when there is at least one DVR authorized tuner
		std::string authorization = get_authorization_strings(dbhandle, true);
		if(authorization.length() == 0) {

			g_gui->Dialog_OK_ShowAndGetInput("DVR Service Subscription Required", "Timer operations require an active HDHomeRun DVR Service subscription.",
				"", "https://www.silicondust.com/dvr-service/");
			return PVR_ERROR::PVR_ERROR_NO_ERROR;
		}

		// seriesrule / epgseriesrule --> recordingrule_type::series
		//
		if((timer.iTimerType == timer_type::seriesrule) || (timer.iTimerType == timer_type::epgseriesrule)) {

			// seriesrule --> execute a title match operation against the backend and let the user choose the series they want
			//
			if(timer.iTimerType == timer_type::seriesrule) {
			
				// Generate a vector of all series that are a title match with the requested EPG search string; the
				// selection dialog will be displayed even if there is only one match in order to confirm the result
				std::vector<std::tuple<std::string, std::string>> matches;
				enumerate_series(dbhandle, authorization.c_str(), timer.strEpgSearchString, [&](struct series const& item) -> void { matches.emplace_back(item.title, item.seriesid); });
				
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

				// Get the seriesid for the recording rule; if one has been specified as part of the timer request use it.
				// Otherwise search for it with a title match against the backend services
				seriesid.assign(timer.strSeriesLink);
				if(seriesid.length() == 0) seriesid = find_seriesid(dbhandle, authorization.c_str(), timer.strEpgSearchString);

				// If no match was found, the timer cannot be added; use a dialog box rather than returning an error
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

			// Get the seriesid for the recording rule; if one has been specified as part of the timer request use it.
			// Otherwise search for it first by channel and start time, falling back to a title match if necessary
			seriesid.assign(timer.strSeriesLink);
			if(seriesid.length() == 0) seriesid = find_seriesid(dbhandle, channelid, timer.startTime);
			if(seriesid.length() == 0) seriesid = find_seriesid(dbhandle, authorization.c_str(), timer.strEpgSearchString);

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
		add_recordingrule(dbhandle, authorization.c_str(), recordingrule);

		// Update the episode information for the specified series; issue a log warning if the operation fails
		try { discover_episodes_seriesid(dbhandle, authorization.c_str(), seriesid.c_str()); }
		catch(std::exception& ex) { log_notice(__func__, ": warning: unable to refresh episode information for series ", seriesid.c_str(), ": ", ex.what()); }
		catch(...) { log_notice(__func__, ": warning: unable to refresh episode information for series ", seriesid.c_str()); }

		// Force a timer update in Kodi to refresh whatever this did on the backend
		g_pvr->TriggerTimerUpdate();

		// Schedule a recording update operation for 15 seconds in the future after any new timer has been
		// added; this allows a timer that kicks off immediately to show the recording in Kodi quickly
		log_notice(__func__, ": scheduling recording update to initiate in 15 seconds");
		g_scheduler.add(std::chrono::system_clock::now() + std::chrono::seconds(15), update_recordings_task);
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
	unsigned int			recordingruleid = 0;		// Backend recording rule identifier

	assert(g_pvr && g_gui);

	try {

		// Pull a database connection out from the connection pool
		connectionpool::handle dbhandle(g_connpool);

		// This operation is only available when there is at least one DVR authorized tuner
		std::string authorization = get_authorization_strings(dbhandle, true);
		if(authorization.length() == 0) {

			g_gui->Dialog_OK_ShowAndGetInput("DVR Service Subscription Required", "Timer operations require an active HDHomeRun DVR Service subscription.",
				"", "https://www.silicondust.com/dvr-service/");
			return PVR_ERROR::PVR_ERROR_NO_ERROR;
		}

		// Determine the recording rule identifier for this timer object
		//
		// seriestimer                   --> not implemented; display message
		// datetimeonlytimer             --> use the parent recording rule identifier
		// seriesrule / datetimeonlyrule --> use the recording rule identifier
		// anything else                 --> not implemented
		//
		if(timer.iTimerType == timer_type::seriestimer) {

			std::string text = "The Timer for this episode of " + std::string(timer.strTitle) + " is a member of an active Record Series Timer Rule and cannot be deleted.";
			g_gui->Dialog_OK_ShowAndGetInput("Unable to delete Timer", text.c_str());

			return PVR_ERROR::PVR_ERROR_NO_ERROR;
		}

		else if(timer.iTimerType == timer_type::datetimeonlytimer) recordingruleid = timer.iParentClientIndex;
		else if((timer.iTimerType == timer_type::seriesrule) || (timer.iTimerType == timer_type::datetimeonlyrule)) recordingruleid = timer.iClientIndex;
		else return PVR_ERROR::PVR_ERROR_NOT_IMPLEMENTED;

		// Determine the series identifier for the recording rule before it gets deleted
		std::string seriesid = (*timer.strSeriesLink) ? timer.strSeriesLink : get_recordingrule_seriesid(dbhandle, recordingruleid);
		if(seriesid.length() == 0) throw string_exception(__func__, ": could not determine seriesid for timer");

		// Attempt to delete the recording rule from the backend and the database
		delete_recordingrule(dbhandle, authorization.c_str(), recordingruleid);

		// Update the episode information for the specified series; issue a log warning if the operation fails
		try { discover_episodes_seriesid(dbhandle, authorization.c_str(), seriesid.c_str()); }
		catch(std::exception& ex) { log_notice(__func__, ": warning: unable to refresh episode information for series ", seriesid.c_str(), ": ", ex.what()); }
		catch(...) { log_notice(__func__, ": warning: unable to refresh episode information for series ", seriesid.c_str()); }
	}

	catch(std::exception& ex) { return handle_stdexception(__func__, ex, PVR_ERROR::PVR_ERROR_FAILED); }
	catch(...) { return handle_generalexception(__func__, PVR_ERROR::PVR_ERROR_FAILED); }

	// Force a timer update in Kodi to refresh whatever this did on the backend
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
	assert(g_pvr && g_gui);

	// Get the current time as a unix timestamp, used to set up AfterOriginalAirdateOnly
	time_t now = time(nullptr);

	// Create an initialize a new recordingrule to be passed to the database
	struct recordingrule recordingrule;
	memset(&recordingrule, 0, sizeof(struct recordingrule));

	try {

		// Pull a database connection out from the connection pool
		connectionpool::handle dbhandle(g_connpool);

		// This operation is only available when there is at least one DVR authorized tuner
		std::string authorization = get_authorization_strings(dbhandle, true);
		if(authorization.length() == 0) {

			g_gui->Dialog_OK_ShowAndGetInput("DVR Service Subscription Required", "Timer operations require an active HDHomeRun DVR Service subscription.",
				"", "https://www.silicondust.com/dvr-service/");
			return PVR_ERROR::PVR_ERROR_NO_ERROR;
		}

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

		// Determine the series identifier for the recording rule before it gets modified
		std::string seriesid = (*timer.strSeriesLink) ? timer.strSeriesLink : get_recordingrule_seriesid(dbhandle, recordingrule.recordingruleid);
		if(seriesid.length() == 0) throw string_exception(__func__, ": could not determine seriesid for timer");

		// Attempt to modify the recording rule on the backend and in the database
		modify_recordingrule(dbhandle, authorization.c_str(), recordingrule);

		// Update the episode information for the specified series; issue a log warning if the operation fails
		try { discover_episodes_seriesid(dbhandle, authorization.c_str(), seriesid.c_str()); }
		catch(std::exception& ex) { log_notice(__func__, ": warning: unable to refresh episode information for series ", seriesid.c_str(), ": ", ex.what()); }
		catch(...) { log_notice(__func__, ": warning: unable to refresh episode information for series ", seriesid.c_str()); }
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
	char						vchannel[64];		// Virtual channel number

	// DRM channels are flagged with a non-zero iEncryptionSystem value to prevent streaming
	if(channel.iEncryptionSystem != 0) {
	
		std::string text = "Channel " + std::string(channel.strChannelName) + " is marked as encrypted and cannot be played";
		g_gui->Dialog_OK_ShowAndGetInput("DRM Protected Content", text.c_str());
		return false;
	}

	// Create a copy of the current addon settings structure
	struct addon_settings settings = copy_settings();

	// The only interesting thing about PVR_CHANNEL is the channel id
	union channelid channelid;
	channelid.value = channel.iUniqueId;

	// Generate a string version of the channel number to represent the virtual channel number
	if(channelid.parts.subchannel == 0) snprintf(vchannel, std::extent<decltype(vchannel)>::value, "%d", channelid.parts.channel);
	else snprintf(vchannel, std::extent<decltype(vchannel)>::value, "%d.%d", channelid.parts.channel, channelid.parts.subchannel);

	try {

		// Pull a database connection out from the connection pool
		connectionpool::handle dbhandle(g_connpool);

		// Determine if HTTP can be used from the storage engine and/or the tuner directly. Tuner HTTP can be used as a fallback
		// for a failed storage stream or if use_direct_tuning is enabled and HTTP is the preferred protocol
		bool use_storage_http = ((settings.use_direct_tuning == false) && (get_tuner_direct_channel_flag(dbhandle, channelid) == false));
		bool use_tuner_http = (use_storage_http || settings.direct_tuning_protocol == tuning_protocol::http);

		// Attempt to create the stream from the storage engine via HTTP if available
		if(use_storage_http) g_pvrstream = openlivestream_storage_http(dbhandle, settings, channelid, vchannel);
		
		// Attempt to create the stream from the tuner via HTTP if available
		if((!g_pvrstream) && (use_tuner_http)) g_pvrstream = openlivestream_tuner_http(dbhandle, settings, channelid, vchannel);
		
		// Attempt to create the stream from the tuner via RTP/UDP (always available)
		if(!g_pvrstream) g_pvrstream = openlivestream_tuner_device(dbhandle, settings, channelid, vchannel);

		// If none of the above methods generated a valid stream, there is nothing left to try
		if(!g_pvrstream) throw string_exception(__func__, ": unable to create a valid stream instance for channel ", vchannel);

		// Pause the scheduler if the user wants that functionality disabled during streaming
		if(settings.pause_discovery_while_streaming) g_scheduler.pause();

		try {

			// For live streams, set the start time to now and set the end time to time_t::max()
			g_stream_starttime = time(nullptr);
			g_stream_endtime = std::numeric_limits<time_t>::max();

			// Log some additional information about the stream for diagnostic purposes
			log_notice(__func__, ": mediatype = ", g_pvrstream->mediatype());
			log_notice(__func__, ": canseek   = ", g_pvrstream->canseek() ? "true" : "false");
			log_notice(__func__, ": length    = ", g_pvrstream->length());
			log_notice(__func__, ": realtime  = ", g_pvrstream->realtime() ? "true" : "false");
			log_notice(__func__, ": starttime = ", g_stream_starttime, " (epoch) = ", strtok(asctime(localtime(&g_stream_starttime)), "\n"), " (local)");
		}

		catch(...) { g_pvrstream.reset(); g_scheduler.resume(); throw; }

		return true;
	}

	// Queue a notification for the user when a live stream cannot be opened, don't just silently log it
	catch(std::exception& ex) { 
		
		g_addon->QueueNotification(ADDON::queue_msg_t::QUEUE_ERROR, "Live Stream creation failed (%s).", ex.what());
		return handle_stdexception(__func__, ex, false); 
	}

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
	if(!g_pvrstream) return;

	try {
		
		g_pvrstream.reset();							// Close the active stream instance
		g_scheduler.resume();							// Resume task scheduler
		g_stream_starttime = g_stream_endtime = 0;		// Reset stream time trackers

		// If the setting to refresh the recordings immediately after playback, reschedule it
		// to execute in a few seconds; this prevents doing it multiple times when changing channels
		if(copy_settings().discover_recordings_after_playback) {

			log_notice(__func__, ": playback stopped; scheduling recording update to occur in 5 seconds");
			g_scheduler.add(std::chrono::system_clock::now() + std::chrono::seconds(5), update_recordings_task);
		}
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
	if(!g_pvrstream) return -1;

	try { 
	
		// Attempt to read the requested number of bytes from the stream
		int result = static_cast<int>(g_pvrstream->read(buffer, size));

		// Live streams should always return data, log an error on any zero-length read
		if(result == 0) log_error(__func__, ": zero-length read on stream at position ", g_pvrstream->position());

		return result;
	}

	catch(std::exception& ex) {

		// Log the exception and alert the user of the failure with an error notification
		log_error(__func__, ": read operation failed with exception: ", ex.what());
		g_addon->QueueNotification(ADDON::queue_msg_t::QUEUE_ERROR, "Unable to read from stream: %s", ex.what());

		// Kodi is going to continue to call this function until it thinks the stream has ended so
		// consume whatever data is left in the stream buffer until it returns zero enough times to stop
		try { return static_cast<int>(g_pvrstream->read(buffer, size)); }
		catch(...) { return 0; }
	}

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
	try { return (g_pvrstream) ? g_pvrstream->seek(position, whence) : -1; }

	catch(std::exception& ex) {

		// Log the exception and alert the user of the failure with an error notification
		log_error(__func__, ": seek operation failed with exception: ", ex.what());
		g_addon->QueueNotification(ADDON::queue_msg_t::QUEUE_ERROR, "Unable to seek stream: %s", ex.what());

		return -1;
	}

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
	// Don't report the position for a real-time stream
	try { return (g_pvrstream && !g_pvrstream->realtime()) ? g_pvrstream->position() : -1; }
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
	try { return (g_pvrstream) ? g_pvrstream->length() : -1; }
	catch(std::exception& ex) { return handle_stdexception(__func__, ex, -1); }
	catch(...) { return handle_generalexception(__func__, -1); }
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
// Get the stream properties for a channel from the backend
//
// Arguments:
//
//	channel		- Channel to get the stream properties for
//	props		- Array of properties to be set for the stream
//	numprops	- Number of properties returned by this function

PVR_ERROR GetChannelStreamProperties(PVR_CHANNEL const* /*channel*/, PVR_NAMED_VALUE* props, unsigned int* numprops)
{
	// PVR_STREAM_PROPERTY_MIMETYPE
	snprintf(props[0].strName, std::extent<decltype(props[0].strName)>::value, PVR_STREAM_PROPERTY_MIMETYPE);
	snprintf(props[0].strValue, std::extent<decltype(props[0].strName)>::value, "video/mp2t");

	// PVR_STREAM_PROPERTY_ISREALTIMESTREAM
	snprintf(props[1].strName, std::extent<decltype(props[1].strName)>::value, PVR_STREAM_PROPERTY_ISREALTIMESTREAM);
	snprintf(props[1].strValue, std::extent<decltype(props[1].strName)>::value, "true");

	*numprops = 2;

	return PVR_ERROR::PVR_ERROR_NO_ERROR;
}

//---------------------------------------------------------------------------
// GetRecordingStreamProperties
//
// Get the stream properties for a recording from the backend
//
// Arguments:
//
//	recording	- Recording to get the stream properties for
//	props		- Array of properties to be set for the stream
//	numprops	- Number of properties returned by this function

PVR_ERROR GetRecordingStreamProperties(PVR_RECORDING const* recording, PVR_NAMED_VALUE* props, unsigned int* numprops)
{
	// Determine if the recording will be realtime or not based on the end time
	bool isrealtime = ((recording->recordingTime + recording->iDuration) > time(nullptr));

	// PVR_STREAM_PROPERTY_MIMETYPE
	snprintf(props[0].strName, std::extent<decltype(props[0].strName)>::value, PVR_STREAM_PROPERTY_MIMETYPE);
	snprintf(props[0].strValue, std::extent<decltype(props[0].strName)>::value, "video/mp2t");

	// PVR_STREAM_PROPERTY_ISREALTIMESTREAM
	snprintf(props[1].strName, std::extent<decltype(props[1].strName)>::value, PVR_STREAM_PROPERTY_ISREALTIMESTREAM);
	snprintf(props[1].strValue, std::extent<decltype(props[1].strName)>::value, (isrealtime) ? "true" : "false");

	*numprops = 2;

	return PVR_ERROR::PVR_ERROR_NO_ERROR;
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
// GetStreamReadChunkSize
//
// Obtain the chunk size to use when reading streams
//
// Arguments:
//
//	chunksize	- Set to the stream chunk size

PVR_ERROR GetStreamReadChunkSize(int* chunksize)
{
	if(chunksize == nullptr) return PVR_ERROR::PVR_ERROR_INVALID_PARAMETERS;

	assert(g_pvrstream);
	if(!g_pvrstream) return PVR_ERROR::PVR_ERROR_NOT_IMPLEMENTED;

	// Report the chunk size value reported by the stream instance
	*chunksize = static_cast<int>(g_pvrstream->chunksize());
	return PVR_ERROR::PVR_ERROR_NO_ERROR;
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

		// Generate the stream URL for the specified channel
		std::string streamurl = get_recording_stream_url(connectionpool::handle(g_connpool), recording.strRecordingId);
		if(streamurl.length() == 0) throw string_exception(__func__, ": unable to determine the URL for specified recording");

		// Pause the scheduler if the user wants that functionality disabled during streaming
		if(settings.pause_discovery_while_streaming) g_scheduler.pause();

		try {

			// Start the new recording stream using the tuning parameters currently specified by the settings
			log_notice(__func__, ": streaming recording '", recording.strTitle, "' via url ", streamurl.c_str());
			g_pvrstream = httpstream::create(streamurl.c_str(), settings.stream_read_chunk_size);

			// For recorded streams, set the start and end times based on the recording metadata
			g_stream_starttime = recording.recordingTime;
			g_stream_endtime = recording.recordingTime + recording.iDuration;

			// Log some additional information about the stream for diagnostic purposes
			log_notice(__func__, ": mediatype = ", g_pvrstream->mediatype());
			log_notice(__func__, ": canseek   = ", g_pvrstream->canseek() ? "true" : "false");
			log_notice(__func__, ": length    = ", g_pvrstream->length());
			log_notice(__func__, ": realtime  = ", g_pvrstream->realtime() ? "true" : "false");
			log_notice(__func__, ": starttime = ", g_stream_starttime, " (epoch) = ", strtok(asctime(localtime(&g_stream_starttime)), "\n"), " (local)");
			log_notice(__func__, ": endtime   = ", g_stream_endtime, " (epoch) = ", strtok(asctime(localtime(&g_stream_endtime)), "\n"), " (local)");
		}

		catch(...) { g_pvrstream.reset(); g_scheduler.resume(); throw; }

		return true;
	}

	// Queue a notification for the user when a recorded stream cannot be opened, don't just silently log it
	catch(std::exception& ex) { 
		
		g_addon->QueueNotification(ADDON::queue_msg_t::QUEUE_ERROR, "Recorded Stream creation failed (%s).", ex.what());
		return handle_stdexception(__func__, ex, false); 
	}

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
	if(!g_pvrstream) return;

	try {

		g_pvrstream.reset();							// Close the active stream instance
		g_scheduler.resume();							// Resume task scheduler
		g_stream_starttime = g_stream_endtime = 0;		// Reset stream time trackers

		// If the setting to refresh the recordings immediately after playback, reschedule it
		// to execute in a few seconds; this prevents doing it multiple times when changing channels
		if(copy_settings().discover_recordings_after_playback) {

			log_notice(__func__, ": playback stopped; scheduling recording update to occur in 5 seconds");
			g_scheduler.add(std::chrono::system_clock::now() + std::chrono::seconds(5), update_recordings_task);
		}
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
	if(!g_pvrstream) return -1;

	try { 
	
		// Attempt to read the requested number of bytes from the stream
		int result = static_cast<int>(g_pvrstream->read(buffer, size));

		// Recorded streams may be real-time if they were in progress when started, but it
		// is still normal for them to end at some point and return no data.  If no data was 
		// read from a real-time stream and the current system clock is before the expected 
		// end time of that stream, log a zero-length read error
		if(result == 0) {

			time_t now = time(nullptr);
			if((g_pvrstream->realtime()) && (now < g_stream_endtime)) log_error(__func__, ": zero-length read on stream at position ", g_pvrstream->position());
		}

		return result;
	}

	catch(std::exception& ex) {

		// Log the exception and alert the user of the failure with an error notification
		log_error(__func__, ": read operation failed with exception: ", ex.what());
		g_addon->QueueNotification(ADDON::queue_msg_t::QUEUE_ERROR, "Unable to read from stream: %s", ex.what());

		// Kodi is going to continue to call this function until it thinks the stream has ended so
		// consume whatever data is left in the stream buffer until it returns zero enough times to stop
		try { return static_cast<int>(g_pvrstream->read(buffer, size)); }
		catch(...) { return 0; }
	}

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
	try { return (g_pvrstream) ? g_pvrstream->seek(position, whence) : -1; }

	catch(std::exception& ex) {

		// Log the exception and alert the user of the failure with an error notification
		log_error(__func__, ": seek operation failed with exception: ", ex.what());
		g_addon->QueueNotification(ADDON::queue_msg_t::QUEUE_ERROR, "Unable to seek stream: %s", ex.what());

		return -1;
	}

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
	try { return (g_pvrstream) ? g_pvrstream->length() : -1; }
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
	try { return (g_pvrstream) ? g_pvrstream->canseek() : false; }
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
// FillBuffer
//
// Notify the pvr addon/demuxer that Kodi wishes to fill demux queue
//
// Arguments:
//
//	mode		- The requested filling mode

void FillBuffer(bool /*mode*/)
{
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
// IsRealTimeStream
//
// Check for real-time streaming
//
// Arguments:
//
//	NONE

bool IsRealTimeStream(void)
{
	try { return (g_pvrstream) ? g_pvrstream->realtime() : false; }
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
	if(days == g_epgmaxtime.load()) return PVR_ERROR::PVR_ERROR_NO_ERROR;

	g_epgmaxtime.store(days);

	log_notice(__func__, ": EPG time frame has changed -- schedule guide listings update");
	g_scheduler.add(std::bind(update_listings_task, false, false, std::placeholders::_1));

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
	// CAUTION: This function will be called on a different thread than the main PVR
	// callback functions -- do not attempt to manipulate any in-progress streams

	try {

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
	// CAUTION: This function will be called on a different thread than the main PVR
	// callback functions -- do not attempt to manipulate any in-progress streams

	try {

		g_scheduler.stop();					// Ensure scheduler has been stopped
		g_scheduler.clear();				// Ensure there are no pending tasks

		// Schedule a task to wait for the network to become available
		g_scheduler.add(std::bind(wait_for_network_task, 60, std::placeholders::_1));

		// Schedule the normal update tasks for everything in an appropriate order
		g_scheduler.add(update_devices_task);
		g_scheduler.add(update_lineups_task);
		g_scheduler.add(update_recordingrules_task);
		g_scheduler.add(update_episodes_task);
		g_scheduler.add(update_recordings_task);

		// A listings update may have been scheduled by update_lineups_task with a channel check set;
		// adding it again may override that task, so perform a missing channel check here as well
		g_scheduler.add(std::bind(update_listings_task, false, true, std::placeholders::_1));

		// Restart the task scheduler
		g_scheduler.start();
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

PVR_ERROR GetStreamTimes(PVR_STREAM_TIMES* times)
{
	assert(times != nullptr);
	assert(g_stream_starttime <= g_stream_endtime);

	// Block this function for non-seekable streams otherwise Kodi will allow those operations
	if((!g_pvrstream) || (!g_pvrstream->canseek())) return PVR_ERROR::PVR_ERROR_NOT_IMPLEMENTED;

	// SPECIAL CASE: If start time and end time are the same, let Kodi handle it. 
	// This can happen if the duration of a recorded stream was not reported properly (credit: timecutter)
	if(g_stream_starttime == g_stream_endtime) return PVR_ERROR::PVR_ERROR_NOT_IMPLEMENTED;

	// Set the start time to the actual start time (UTC) for live streams, otherwise zero
	// Using zero here is required to enable calls to SetRecordingLastPlayedPosition()
	times->startTime = (g_stream_endtime == std::numeric_limits<time_t>::max()) ? g_stream_starttime : 0;

	times->ptsStart = 0;							// Starting PTS gets set to zero
	times->ptsBegin = 0;							// Timeshift buffer start PTS also gets set to zero

	// Set the timeshift duration to the delta between the start time and the lesser of the 
	// current wall clock time or the known stream end time
	time_t now = time(nullptr);
	times->ptsEnd = static_cast<int64_t>(((now < g_stream_endtime) ? now : g_stream_endtime) - g_stream_starttime) * DVD_TIME_BASE;

	return PVR_ERROR::PVR_ERROR_NO_ERROR;
}

//---------------------------------------------------------------------------

#pragma warning(pop)
