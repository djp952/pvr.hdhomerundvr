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
#include <chrono>
#include <functional>
#include <list>
#include <memory>
#include <mutex>
#include <string>
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
static void discover_startup_task(bool includedevices, scalar_condition<bool> const& cancel);

// Helper Functions
//
static std::string select_tuner(std::vector<std::string> const& possibilities);

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

	// startup_discovery_task_delay
	//
	// Indicates the number of seconds to pause before initiating the startup discovery task
	int startup_discovery_task_delay;

	// stream_read_chunk_size
	//
	// Indicates the minimum number of bytes to return from a stream read
	int stream_read_chunk_size;

	// stream_ring_buffer_size
	//
	// Indicates the size of the stream ring buffer to allocate
	int stream_ring_buffer_size;

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

// g_pvrstream
//
// DVR stream buffer instance
static std::unique_ptr<pvrstream> g_pvrstream;

// g_epgenabled
//
// Flag indicating if EPG access is enabled for the process
static bool g_epgenabled = true;

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
	false,					// discover_recordings_after_playback
	false,					// use_backend_genre_strings
	false,					// show_drm_protected_channels
	false,					// use_channel_names_from_lineup
	false,					// generate_repeat_indicators
	86400,					// delete_datetime_rules_after			default = 1 day
	300, 					// discover_devices_interval;			default = 5 minutes
	7200,					// discover_episodes_interval			default = 2 hours
	3600,					// discover_guide_interval				default = 1 hour
	600,					// discover_lineups_interval			default = 10 minutes
	600,					// discover_recordings_interval			default = 10 minutes
	7200,					// discover_recordingrules_interval		default = 2 hours
	false,					// use_http_device_discovery
	false,					// use_direct_tuning
	tuning_protocol::http,	// direct_tuning_protocol
	3,						// startup_discovery_task_delay
	(4 KiB),				// stream_read_chunk_size
	(1 MiB),				// stream_ring_buffer_size
	72000,					// deviceauth_stale_after				default = 20 hours
	false,					// enable_recording_edl
	"",						// recording_edl_folder
	false,					// recording_edl_folder_is_flat
	false,					// recording_edl_cut_as_comskip
	0,						// recording_edl_start_padding
	0,						// recording_edl_end_padding
};

// g_settings_lock
//
// Synchronization object to serialize access to addon settings
static std::mutex g_settings_lock;

// g_stream_close_hack
//
// Flag to help deal with starting the stream during GetStreamProperties() -- the PVR
// always calls CloseStream() before calling OpenStream(), which stops and restarts
// the stream unnecessarily
static bool g_stream_close_hack = false;

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

// discover_devices_task
//
// Scheduled task implementation to discover the HDHomeRun devices
static void discover_devices_task(scalar_condition<bool> const& cancel)
{
	bool		changed = false;			// Flag if the discovery data changed

	assert(g_addon && g_pvr);

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

		// Re-enable access to the EPG if it had been disabled due to multiple failure(s) accessing
		// a channel EPG.  The idea here is to prevent unauthorized clients from slamming the backend
		// services for no reason -- see the GetEPGForChannel() function.
		g_epgenabled = true;
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

		// This operation is only available when there is at least one DVR authorized tuner
		std::string authorization = get_authorization_strings(dbhandle, true);
		if(authorization.length() != 0) {

			// Discover the episode data from the backend service
			discover_episodes(dbhandle, authorization.c_str(), changed);
		
			if(changed) {

				// Changes in the episode data affects the PVR timers
				log_notice(__func__, ": recording rule episode discovery data changed -- trigger timer update");
				g_pvr->TriggerTimerUpdate();
			}
		}

		else log_notice(__func__, ": no tuners with valid DVR authorization were discovered; skipping recording rule episode discovery");
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

		// Get the authorization code(s) for all available tuners
		std::string authorization = get_authorization_strings(dbhandle, false);
		if(authorization.length() != 0) {

			// Discover the updated electronic program guide data from the backend service
			discover_guide(dbhandle, authorization.c_str(), changed);
		
			if(changed) {

				// Trigger a channel update; the guide data is used to resolve channel names and icon image urls
				log_notice(__func__, ": guide channel discovery data changed -- trigger channel update");
				g_pvr->TriggerChannelUpdate();
			}
		}

		else log_notice(__func__, ": no tuners with valid authorization were discovered; skipping guide discovery");
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

		// This operation is only available when there is at least one DVR authorized tuner
		std::string authorization = get_authorization_strings(dbhandle, true);
		if(authorization.length() != 0) {

			// Discover the recording rules from the backend service
			discover_recordingrules(dbhandle, authorization.c_str(), changed);

			// Generate a vector<> of all expired recording rules to be deleted from the backend
			std::vector<unsigned int> expired;
			enumerate_expired_recordingruleids(dbhandle, settings.delete_datetime_rules_after, [&](unsigned int const& recordingruleid) -> void 
			{ 
				expired.push_back(recordingruleid); 
			});

			// Iterate over the vector<> and attempt to delete each expired rule from the backend
			for(auto const& it : expired) {

				try { delete_recordingrule(dbhandle, authorization.c_str(), it); changed = true; }
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

		else log_notice(__func__, ": no tuners with valid DVR authorization were discovered; skipping recording rule discovery");
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
static void discover_startup_task(bool includedevices, scalar_condition<bool> const& /*cancel*/)
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

		// When the PVR is first starting up, the devices and lineups are discovered synchronously
		// as part of that process.  Skip these to eliminate duplicating that work
		if(includedevices) { 

			// DISCOVER: Devices
			try { 
				
				auto caller = __func__;
				discover_devices(dbhandle, settings.use_http_device_discovery); 
				enumerate_device_names(dbhandle, [&](struct device_name const& device_name) -> void { log_notice(caller, ": discovered: ", device_name.name); });
			}

			catch(std::exception& ex) { handle_stdexception(std::string(__func__, " (devices)").c_str(), ex); }
			catch(...) { handle_generalexception(std::string(__func__, " (devices)").c_str()); }

			// DISCOVER: Lineups
			try { discover_lineups(dbhandle, lineups_changed); }
			catch(std::exception& ex) { handle_stdexception(std::string(__func__, " (lineups)").c_str(), ex); }
			catch(...) { handle_generalexception(std::string(__func__, " (lineups)").c_str()); }
		}

		// DISCOVER: Recordings
		try { discover_recordings(dbhandle, recordings_changed); }
		catch(std::exception& ex) { handle_stdexception(std::string(__func__, " (recordings)").c_str(), ex); }
		catch(...) { handle_generalexception(std::string(__func__, " (recordings)").c_str()); }

		// Get the device authorization string(s) required for accessing the backend services after the
		// local device discoveries have completed
		std::string authorization = get_authorization_strings(dbhandle, false);
		std::string dvrauthorization = get_authorization_strings(dbhandle, true);
		
		// DISCOVER: Guide Metadata
		try { 
			
			if(authorization.length() != 0) discover_guide(dbhandle, authorization.c_str(), guide_changed);
			else log_notice(__func__, ": no tuners with valid authorization were discovered; skipping guide discovery");
		}

		catch(std::exception& ex) { handle_stdexception(std::string(__func__, " (guide metadata)").c_str(), ex); }
		catch(...) { handle_generalexception(std::string(__func__, " (guide metadata)").c_str()); }

		// DISCOVER: Recording Rules
		try { 
			
			if(dvrauthorization.length() != 0) discover_recordingrules(dbhandle, dvrauthorization.c_str(), recordingrules_changed); 
			else log_notice(__func__, ": no tuners with valid DVR authorization were discovered; skipping recording rule discovery");
		}

		catch(std::exception& ex) { handle_stdexception(std::string(__func__, " (recording rules)").c_str(), ex); }
		catch(...) { handle_generalexception(std::string(__func__, " (recording rules)").c_str()); }

		// DISCOVER: Episodes
		try { 
		
			if(dvrauthorization.length() != 0) discover_episodes(dbhandle, dvrauthorization.c_str(), episodes_changed); 
			else log_notice(__func__, ": no tuners with valid DVR authorization were discovered; skipping recording rule episode discovery");
		}

		catch(std::exception& ex) { handle_stdexception(std::string(__func__, " (episodes)").c_str(), ex); }
		catch(...) { handle_generalexception(std::string(__func__, " (episodes)").c_str()); }

		// Execute any necessary triggers based on what discovery data has changed

		// TRIGGER: Channels
		if(lineups_changed || guide_changed) {
			
			log_notice(__func__, ": lineup / guide discovery data changed -- trigger channel update");
			g_pvr->TriggerChannelUpdate();
		}

		// TRIGGER: Channel Groups
		if(lineups_changed) {
			
			log_notice(__func__, ": lineup discovery data changed -- trigger channel group update");
			g_pvr->TriggerChannelGroupsUpdate();
		}

		// TRIGGER: Recordings
		if(recordings_changed) {
			
			log_notice(__func__, ": recording discovery data changed -- trigger recording update");
			g_pvr->TriggerRecordingUpdate();
		}

		// TRIGGER: Timers
		if(recordingrules_changed || episodes_changed) {
			
			log_notice(__func__, ": recording rule / recording rule episode discovery data changed -- trigger timer update");
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
	log_error(function, " failed due to an exception");
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
	log_error(function, " failed due to an exception: ", ex.what());
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

// edltype_to_string
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

// openlivestream_storage_http
//
// Attempts to open a live stream via HTTP from an available storage engine
static std::unique_ptr<pvrstream> openlivestream_storage_http(connectionpool::handle const& dbhandle, struct addon_settings const& settings, union channelid channelid, char const* vchannel)
{
	assert(vchannel != nullptr);
	if((vchannel == nullptr) || (*vchannel == '\0')) throw std::invalid_argument("vchannel");

	// Generate the URL for the virtual channel by querying the database
	std::string streamurl = get_stream_url(dbhandle, channelid);
	if(streamurl.length() == 0) { log_notice(__func__, ": unable to generate storage engine stream url for channel ", vchannel); return nullptr; }

	try {

		// Start the new HTTP stream using the parameters currently specified by the settings
		std::unique_ptr<pvrstream> stream = httpstream::create(streamurl.c_str(), settings.stream_ring_buffer_size, settings.stream_read_chunk_size);
		log_notice(__func__, ": streaming channel ", vchannel, " via storage engine url ", streamurl.c_str());

		return stream;
	}

	// If the stream creation failed, log a notice and return a null unique_ptr<> back to the caller, do not throw an exception
	catch(std::exception& ex) { log_notice(__func__, ": unable to stream channel ", vchannel, " via storage engine url ", streamurl.c_str(), ": ", ex.what()); }

	return nullptr;
}

// openlivestream_tuner_device
//
// Attempts to open a live stream via RTP/UDP from an available tuner device
static std::unique_ptr<pvrstream> openlivestream_tuner_device(connectionpool::handle const& dbhandle, struct addon_settings const& /*settings*/, union channelid channelid, char const* vchannel)
{
	std::vector<std::string>		devices;			// vector<> of possible device tuners for the channel

	assert(vchannel != nullptr);
	if((vchannel == nullptr) || (*vchannel == '\0')) throw std::invalid_argument("vchannel");

	// Create a collection of all the tuners that can possibly stream the requested channel
	enumerate_channeltuners(dbhandle, channelid, [&](char const* item) -> void { devices.emplace_back(item); });
	if(devices.size() == 0) { log_notice(__func__, ": unable to find any possible tuner devices to stream channel ", vchannel); return nullptr; }

	try {

		// Start the new RTP/UDP stream -- devicestream performs its own tuner selection based on the provided collection
		std::unique_ptr<pvrstream> stream = devicestream::create(devices, vchannel);
		log_notice(__func__, ": streaming channel ", vchannel, " via tuner device rtp/udp broadcast");

		return stream;
	}

	// If the stream creation failed, log a notice and return a null unique_ptr<> back to the caller, do not throw an exception
	catch(std::exception& ex) { log_notice(__func__, ": unable to stream channel ", vchannel, " via tuner device rtp/udp broadcast: ", ex.what()); }

	return nullptr;
}

// openlivestream_tuner_http
//
// Attempts to open a live stream via HTTP from an available tuner device
static std::unique_ptr<pvrstream> openlivestream_tuner_http(connectionpool::handle const& dbhandle, struct addon_settings const& settings, union channelid channelid, char const* vchannel)
{
	std::vector<std::string>		devices;			// vector<> of possible device tuners for the channel

	assert(vchannel != nullptr);
	if((vchannel == nullptr) || (*vchannel == '\0')) throw std::invalid_argument("vchannel");

	// Create a collection of all the tuners that can possibly stream the requested channel
	enumerate_channeltuners(dbhandle, channelid, [&](char const* item) -> void { devices.emplace_back(item); });
	if(devices.size() == 0) { log_notice(__func__, ": unable to find any possible tuner devices to stream channel ", vchannel); return nullptr; }

	// A valid tuner device has to be selected from the available options
	std::string selected = select_tuner(devices);
	if(selected.length() == 0) { log_notice(__func__, ": no tuner devices are available to create the requested stream"); return nullptr; }

	// Generate the URL required to stream the channel via the tuner over HTTP
	std::string streamurl = get_tuner_stream_url(dbhandle, selected.c_str(), channelid);
	if(streamurl.length() == 0) { log_notice(__func__, ": unable to generate tuner device stream url for channel ", vchannel); return nullptr; }

	try {

		// Start the new HTTP stream using the parameters currently specified by the settings
		std::unique_ptr<pvrstream> stream = httpstream::create(streamurl.c_str(), settings.stream_ring_buffer_size, settings.stream_read_chunk_size);
		log_notice(__func__, ": streaming channel ", vchannel, " via tuner device url ", streamurl.c_str());

		return stream;
	}

	// If the stream creation failed, log a notice and return a null unique_ptr<> back to the caller, do not throw an exception
	catch(std::exception& ex) { log_notice(__func__, ": unable to stream channel ", vchannel, "via tuner device url ", streamurl.c_str(), ": ", ex.what()); }

	return nullptr;
}

// select_tuner
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

// try_getepgforchannel
//
// Request the EPG for a channel from the backend
static bool try_getepgforchannel(ADDON_HANDLE handle, int channel, time_t start, time_t end)
{
	assert(g_pvr);
	assert(handle != nullptr);

	// Retrieve the channel identifier from the PVR_CHANNEL structure
	union channelid channelid;
	channelid.value = channel;

	try {

		// Create a copy of the current addon settings structure
		struct addon_settings settings = copy_settings();

		// Pull a database connection out from the connection pool
		connectionpool::handle dbhandle(g_connpool);

		// Get the authorization code(s) for all available tuners
		std::string authorization = get_authorization_strings(dbhandle, false);
		if(authorization.length() == 0) throw string_exception(__func__, ": no valid tuner device authorization string(s) available");

		// EPG_TAG uses pointers instead of string buffers, collect all of the string data returned 
		// from the database in a list<> to keep them valid until transferred
		std::list<std::string> epgstrings;

		// Collect all of the EPG_TAG structures locally before transferring any of them to Kodi
		std::vector<EPG_TAG> epgtags;

		// Enumerate all of the guide entries in the database for this channel and time frame
		enumerate_guideentries(dbhandle, authorization.c_str(), channelid, start, end, [&](struct guideentry const& item) -> void {

			EPG_TAG	epgtag;										// EPG_TAG to be transferred to Kodi
			memset(&epgtag, 0, sizeof(EPG_TAG));				// Initialize the structure

			// Determine if the episode is a repeat -- unlike recordings there is no firstairing field to key on, 
			// so if the start time of the program is within 24 hours of the originalairdate, consider it as a first airing
			bool isrepeat = !((item.originalairdate + 86400) >= item.starttime);

			// Don't send EPG entries with start/end times outside the requested range
			if((item.starttime > end) || (item.endtime < start)) return;

			// iUniqueBroadcastId (required)
			assert(item.broadcastid > EPG_TAG_INVALID_UID);
			epgtag.iUniqueBroadcastId = item.broadcastid;

			// iUniqueChannelId (required)
			epgtag.iUniqueChannelId = item.channelid;

			// strTitle (required)
			if(item.title == nullptr) return;
			epgtag.strTitle = epgstrings.emplace(epgstrings.end(), item.title)->c_str();

			// startTime (required)
			epgtag.startTime = item.starttime;

			// endTime (required)
			epgtag.endTime = item.endtime;

			// strPlot
			if(item.synopsis != nullptr) epgtag.strPlot = epgstrings.emplace(epgstrings.end(), item.synopsis)->c_str();

			// iYear
			epgtag.iYear = item.year;

			// strIconPath
			if(item.iconurl != nullptr) epgtag.strIconPath = epgstrings.emplace(epgstrings.end(), item.iconurl)->c_str();

			// iGenreType
			epgtag.iGenreType = (settings.use_backend_genre_strings) ? EPG_GENRE_USE_STRING : item.genretype;

			// strGenreDescription
			if((settings.use_backend_genre_strings) && (item.genres != nullptr)) epgtag.strGenreDescription = epgstrings.emplace(epgstrings.end(), item.genres)->c_str();

			// firstAired
			epgtag.firstAired = item.originalairdate;

			// iSeriesNumber
			epgtag.iSeriesNumber = item.seriesnumber;

			// iEpisodeNumber
			epgtag.iEpisodeNumber = item.episodenumber;

			// iEpisodePartNumber
			epgtag.iEpisodePartNumber = -1;

			// strEpisodeName
			if(item.episodename != nullptr) epgtag.strEpisodeName = epgstrings.emplace(epgstrings.end(), std::string(item.episodename) +
				std::string(((isrepeat) && (settings.generate_repeat_indicators)) ? " [R]" : ""))->c_str();

			// iFlags
			epgtag.iFlags = EPG_TAG_FLAG_IS_SERIES;

			// strSeriesLink
			if(item.seriesid != nullptr) epgtag.strSeriesLink = epgstrings.emplace(epgstrings.end(), item.seriesid)->c_str();

			// Move the EPG_TAG structure into the local vector<>
			epgtags.emplace_back(std::move(epgtag));
		});

		// Transfer the generated EPG_TAG structures over to Kodi
		for(auto const& it : epgtags) g_pvr->TransferEpgEntry(handle, &it);
	}
	
	catch(std::exception& ex) { return handle_stdexception(__func__, ex, false); }
	catch(...) { return handle_generalexception(__func__, false); }

	return true;
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
		// reference-counted so if it has already been called this won't hurt anything
		WSADATA wsaData;
		WSAStartup(MAKEWORD(2, 2), &wsaData);
#endif

		// Initialize libcurl using the standard default options
		if(curl_global_init(CURL_GLOBAL_DEFAULT) != CURLE_OK) throw string_exception(__func__, ": curl_global_init(CURL_GLOBAL_DEFAULT) failed");

		// Create the global addon callbacks instance
		g_addon.reset(new ADDON::CHelper_libXBMC_addon());
		if(!g_addon->RegisterMe(handle)) throw string_exception(__func__, ": failed to register addon handle (CHelper_libXBMC_addon::RegisterMe)");

		// Throw a banner out to the Kodi log indicating that the add-on is being loaded
		log_notice(__func__, ": ", VERSION_PRODUCTNAME_ANSI, " v", VERSION_VERSION3_ANSI, " loading");

		try { 

			// The user data path doesn't always exist when an addon has been installed
			if(!g_addon->DirectoryExists(pvrprops->strUserPath)) {

				log_notice(__func__, ": user data directory ", pvrprops->strUserPath, " does not exist");
				if(!g_addon->CreateDirectory(pvrprops->strUserPath)) throw string_exception(__func__, ": unable to create addon user data directory");
				log_notice(__func__, ": user data directory ", pvrprops->strUserPath, " created");
			}

			// Load the general settings
			if(g_addon->GetSetting("pause_discovery_while_streaming", &bvalue)) g_settings.pause_discovery_while_streaming = bvalue;
			if(g_addon->GetSetting("prepend_channel_numbers", &bvalue)) g_settings.prepend_channel_numbers = bvalue;
			if(g_addon->GetSetting("use_episode_number_as_title", &bvalue)) g_settings.use_episode_number_as_title = bvalue;
			if(g_addon->GetSetting("discover_recordings_after_playback", &bvalue)) g_settings.discover_recordings_after_playback = bvalue;
			if(g_addon->GetSetting("use_backend_genre_strings", &bvalue)) g_settings.use_backend_genre_strings = bvalue;
			if(g_addon->GetSetting("show_drm_protected_channels", &bvalue)) g_settings.show_drm_protected_channels = bvalue;
			if(g_addon->GetSetting("use_channel_names_from_lineup", &bvalue)) g_settings.use_channel_names_from_lineup = bvalue;
			if(g_addon->GetSetting("generate_repeat_indicators", &bvalue)) g_settings.generate_repeat_indicators = bvalue;
			if(g_addon->GetSetting("delete_datetime_rules_after_v2", &nvalue)) g_settings.delete_datetime_rules_after = nvalue;

			// Load the discovery interval settings
			if(g_addon->GetSetting("discover_devices_interval_v2", &nvalue)) g_settings.discover_devices_interval = nvalue;
			if(g_addon->GetSetting("discover_lineups_interval_v2", &nvalue)) g_settings.discover_lineups_interval = nvalue;
			if(g_addon->GetSetting("discover_guide_interval_v2", &nvalue)) g_settings.discover_guide_interval = nvalue;
			if(g_addon->GetSetting("discover_recordings_interval_v2", &nvalue)) g_settings.discover_recordings_interval = nvalue;
			if(g_addon->GetSetting("discover_recordingrules_interval_v2", &nvalue)) g_settings.discover_recordingrules_interval = nvalue;
			if(g_addon->GetSetting("discover_episodes_interval_v2", &nvalue)) g_settings.discover_episodes_interval = nvalue;

			// Load the Edit Decision List (EDL) settings
			if(g_addon->GetSetting("enable_recording_edl", &bvalue)) g_settings.enable_recording_edl = bvalue;
			if(g_addon->GetSetting("recording_edl_folder", strvalue)) g_settings.recording_edl_folder.assign(strvalue);
			if(g_addon->GetSetting("recording_edl_folder_is_flat", &bvalue)) g_settings.recording_edl_folder_is_flat = bvalue;
			if(g_addon->GetSetting("recording_edl_cut_as_comskip", &bvalue)) g_settings.recording_edl_cut_as_comskip = bvalue;
			if(g_addon->GetSetting("recording_edl_start_padding", &nvalue)) g_settings.recording_edl_start_padding = nvalue;
			if(g_addon->GetSetting("recording_edl_end_padding", &nvalue)) g_settings.recording_edl_end_padding = nvalue;

			// Load the advanced settings
			if(g_addon->GetSetting("use_http_device_discovery", &bvalue)) g_settings.use_http_device_discovery = bvalue;
			if(g_addon->GetSetting("use_direct_tuning", &bvalue)) g_settings.use_direct_tuning = bvalue;
			if(g_addon->GetSetting("direct_tuning_protocol", &nvalue)) g_settings.direct_tuning_protocol = static_cast<enum tuning_protocol>(nvalue);
			if(g_addon->GetSetting("startup_discovery_task_delay", &nvalue)) g_settings.startup_discovery_task_delay = nvalue;
			if(g_addon->GetSetting("stream_read_chunk_size_v2", &nvalue)) g_settings.stream_read_chunk_size = nvalue;
			if(g_addon->GetSetting("stream_ring_buffer_size_v2", &nvalue)) g_settings.stream_ring_buffer_size = nvalue;
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

					// Generate the local file system and URL-based file names for the PVR database, the file name is based on the version
					std::string databasefile = std::string(pvrprops->strUserPath) + "/hdhomerundvr-v" + VERSION_VERSION2_ANSI + ".db";
					std::string databasefileurl = "file:///" + databasefile;

					// Create the global database connection pool instance
					try { g_connpool = std::make_shared<connectionpool>(databasefileurl.c_str(), SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE | SQLITE_OPEN_URI); }
					catch(sqlite_exception const& dbex) {

						log_error(__func__, ": unable to create/open the PVR database ", databasefile, " - ", dbex.what());
						
						// If any SQLite-specific errors were thrown during database open/create, attempt to delete and recreate the database
						log_notice(__func__, ": attempting to delete and recreate the PVR database");
						g_addon->DeleteFile(databasefile.c_str());
						g_connpool = std::make_shared<connectionpool>(databasefileurl .c_str(), SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE | SQLITE_OPEN_URI);
						log_notice(__func__, ": successfully recreated the PVR database");
					}

					try {

						try {
						
							connectionpool::handle dbhandle(g_connpool);

							// Clear out existing tuner device authorization code(s) on startup, there is no good way to know if they are
							// still valid or not and discover_devices() purposely doesn't remove cached data if no devices are found
							clear_authorization_strings(dbhandle, g_settings.deviceauth_stale_after);

							// Kodi currently has no means to create EPG entries in the database for channels that are
							// added after the PVR manager has been started.  Synchronously execute a device and lineup
							// discovery so that the initial set of channels are immediately available to Kodi
							log_notice(__func__, ": initiating local network resource discovery (startup; method: ", g_settings.use_http_device_discovery ? "http" : "broadcast", ")");

							auto caller = __func__;
							discover_devices(dbhandle, g_settings.use_http_device_discovery);
							enumerate_device_names(dbhandle, [&](struct device_name const& device_name) -> void { log_notice(caller, ": discovered: ", device_name.name); });
							discover_lineups(dbhandle);

							// Alert the user if no tuner device(s) were found during startup or are cached in the database
							if(get_tuner_count(dbhandle) == 0) g_addon->QueueNotification(ADDON::queue_msg_t::QUEUE_ERROR, "HDHomeRun tuner device(s) not detected");
						}

						// Failure to perform the synchronous device and lineup discovery is not fatal
						catch(std::exception& ex) { handle_stdexception(__func__, ex); }
						catch(...) { handle_generalexception(__func__); }

						// To help reduce trigger 'chatter' at startup, a special optimized task was created that loads all discovery data and 
						// only triggers the PVR update(s) once per category.  Delay the launch of this initial startup discovery task for a 
						// reasonable amount of time to allow the PVR to finish its start up processing -- failure to do so may trigger a race 
						// condition that leads to a deadlock in Kodi that can occur when channel information changes while the EPGs are created.
						// Since the devices and lineups were synchronously discovered already, bind false as the first argument to skip those
						log_notice(__func__, ": delaying startup discovery task for ", g_settings.startup_discovery_task_delay, " seconds");					
						g_scheduler.add(std::chrono::system_clock::now() + std::chrono::seconds(g_settings.startup_discovery_task_delay), std::bind(discover_startup_task, false, std::placeholders::_1));
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
			log_notice(__func__, ": setting delete_datetime_rules_after changed to ", nvalue, " seconds -- schedule recording rule discovery");

			// Reschedule the recording rule discovery task to run as soon as possible
			g_scheduler.remove(discover_recordingrules_task);
			g_scheduler.add(now + std::chrono::seconds(1), discover_recordingrules_task);
		}
	}

	// discover_devices_interval
	//
	else if(strcmp(name, "discover_devices_interval_v2") == 0) {

		int nvalue = *reinterpret_cast<int const*>(value);
		if(nvalue != g_settings.discover_devices_interval) {

			// Reschedule the discover_devices_task to execute at the specified interval from now
			g_settings.discover_devices_interval = nvalue;
			g_scheduler.remove(discover_devices_task);
			g_scheduler.add(now + std::chrono::seconds(nvalue), discover_devices_task);
			log_notice(__func__, ": setting discover_devices_interval changed -- rescheduling task to initiate in ", nvalue, " seconds");
		}
	}

	// discover_episodes_interval
	//
	else if(strcmp(name, "discover_episodes_interval_v2") == 0) {

		int nvalue = *reinterpret_cast<int const*>(value);
		if(nvalue != g_settings.discover_episodes_interval) {

			// Reschedule the discover_episodes_task to execute at the specified interval from now
			g_settings.discover_episodes_interval = nvalue;
			g_scheduler.remove(discover_episodes_task);
			g_scheduler.add(now + std::chrono::seconds(nvalue), discover_episodes_task);
			log_notice(__func__, ": setting discover_episodes_interval changed -- rescheduling task to initiate in ", nvalue, " seconds");
		}
	}

	// discover_guide_interval
	//
	else if(strcmp(name, "discover_guide_interval_v2") == 0) {

		int nvalue = *reinterpret_cast<int const*>(value);
		if(nvalue != g_settings.discover_guide_interval) {

			// Reschedule the discover_guide_task to execute at the specified interval from now
			g_settings.discover_guide_interval = nvalue;
			g_scheduler.remove(discover_guide_task);
			g_scheduler.add(now + std::chrono::seconds(nvalue), discover_guide_task);
			log_notice(__func__, ": setting discover_guide_interval changed -- rescheduling task to initiate in ", nvalue, " seconds");
		}
	}

	// discover_lineups_interval
	//
	else if(strcmp(name, "discover_lineups_interval_v2") == 0) {

		int nvalue = *reinterpret_cast<int const*>(value);
		if(nvalue != g_settings.discover_lineups_interval) {

			// Reschedule the discover_lineups_task to execute at the specified interval from now
			g_settings.discover_lineups_interval = nvalue;
			g_scheduler.remove(discover_lineups_task);
			g_scheduler.add(now + std::chrono::seconds(nvalue), discover_lineups_task);
			log_notice(__func__, ": setting discover_lineups_interval changed -- rescheduling task to initiate in ", nvalue, " seconds");
		}
	}

	// discover_recordings_interval
	//
	else if(strcmp(name, "discover_recordings_interval_v2") == 0) {

		int nvalue = *reinterpret_cast<int const*>(value);
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
	else if(strcmp(name, "discover_recordingrules_interval_v2") == 0) {

		int nvalue = *reinterpret_cast<int const*>(value);
		if(nvalue != g_settings.discover_recordingrules_interval) {

			// Reschedule the discover_recordingrules_task to execute at the specified interval from now
			g_settings.discover_recordingrules_interval = nvalue;
			g_scheduler.remove(discover_recordingrules_task);
			g_scheduler.add(now + std::chrono::seconds(nvalue), discover_recordingrules_task);
			log_notice(__func__, ": setting discover_recordingrules_interval changed -- rescheduling task to initiate in ", nvalue, " seconds");
		}
	}

	// use_http_device_discovery
	//
	else if(strcmp(name, "use_http_device_discovery") == 0) {

		bool bvalue = *reinterpret_cast<bool const*>(value);
		if(bvalue != g_settings.use_http_device_discovery) {

			g_settings.use_http_device_discovery = bvalue;
			log_notice(__func__, ": setting use_http_device_discovery changed to ", (bvalue) ? "true" : "false", " -- schedule device discovery");

			// Reschedule the device discovery task to run as soon as possible
			g_scheduler.remove(discover_devices_task);
			g_scheduler.add(now + std::chrono::seconds(1), discover_devices_task);
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

	// startup_discovery_task_delay
	//
	else if(strcmp(name, "startup_discovery_task_delay") == 0) {

		int nvalue = *reinterpret_cast<int const*>(value);
		if(nvalue != g_settings.startup_discovery_task_delay) {

			g_settings.startup_discovery_task_delay = nvalue;
			log_notice(__func__, ": setting startup_discovery_task_delay changed to ", nvalue, " seconds");
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

	// stream_ring_buffer_size
	//
	else if(strcmp(name, "stream_ring_buffer_size_v2") == 0) {

		int nvalue = *reinterpret_cast<int const*>(value);
		if(nvalue != g_settings.stream_ring_buffer_size) {

			g_settings.stream_ring_buffer_size = nvalue;
			log_notice(__func__, ": setting stream_ring_buffer_size changed to ", nvalue, " bytes");
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
			g_scheduler.remove(discover_devices_task);
			g_scheduler.add(now + std::chrono::seconds(1), discover_devices_task);
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
			g_scheduler.add(now, std::bind(discover_startup_task, true, std::placeholders::_1));
	
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
// Request the EPG for a channel from the backend.  If the operation fails, this
// will re-execute a device discovery inline (and therefore possibly a lineup and
// recording discovery) in order to refresh the device authorization codes.  If the
// operation fails a second time, the function will be disabled until the next
// device discovery -- this was put in place to limit the number of times that an 
// unauthorized client can request EPG data from the backend services.
//
// Arguments:
//
//	handle		- Handle to pass to the callback method
//	channel		- The channel to get the EPG table for
//	start		- Get events after this time (UTC)
//	end			- Get events before this time (UTC)

PVR_ERROR GetEPGForChannel(ADDON_HANDLE handle, int channel, time_t start, time_t end)
{
	static std::mutex		sync;					// Synchronization object
	bool					cancel = false;			// Unused cancellation flag for discover_devices_task

	// Prevent concurrent access into this fuction by multiple threads
	std::unique_lock<std::mutex> lock(sync);

	if(handle == nullptr) return PVR_ERROR::PVR_ERROR_INVALID_PARAMETERS;

	// Check if the EPG function has been disabled due to failure(s) and if so, return no data
	if(!g_epgenabled) return PVR_ERROR::PVR_ERROR_NO_ERROR;

	// Try to get the EPG data for the channel, if successful the operation is complete
	bool result = try_getepgforchannel(handle, channel, start, end);
	if(result == true) return PVR_ERROR::PVR_ERROR_NO_ERROR;

	// If the operation failed, re-execute a device discovery in case the deviceauth code(s) are stale
	log_notice(__func__, ": failed to retrieve EPG data for channel -- execute device discovery now");
	g_scheduler.remove(discover_devices_task);
	discover_devices_task(cancel);

	// Try the operation again after the device discovery task has completed
	result = try_getepgforchannel(handle, channel, start, end);
	if(result == true) return PVR_ERROR::PVR_ERROR_NO_ERROR;

	// If the operation failed a second time, temporarily disable the EPG functionality.  This flag
	// will be cleared after the next successful device discovery completes.
	log_error(__func__, ": Multiple failures were encountered accessing EPG data; EPG functionality is temporarily disabled");
	g_epgenabled = false;
	return PVR_ERROR::PVR_ERROR_FAILED;
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
	snprintf(group.strGroupName, std::extent<decltype(group.strGroupName)>::value, "Favorite Channels");
	g_pvr->TransferChannelGroup(handle, &group);

	// HD Channels
	snprintf(group.strGroupName, std::extent<decltype(group.strGroupName)>::value, "HD Channels");
	g_pvr->TransferChannelGroup(handle, &group);

	// SD Channels
	snprintf(group.strGroupName, std::extent<decltype(group.strGroupName)>::value, "SD Channels");
	g_pvr->TransferChannelGroup(handle, &group);

	// Demo Channels
	snprintf(group.strGroupName, std::extent<decltype(group.strGroupName)>::value, "Demo Channels");
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

	// Determine which group enumerator to use for the operation, there are only four to
	// choose from: "Favorite Channels", "HD Channels", "SD Channels" and "Demo Channels"
	std::function<void(sqlite3*, bool, enumerate_channelids_callback)> enumerator = nullptr;
	if(strcmp(group.strGroupName, "Favorite Channels") == 0) enumerator = enumerate_favorite_channelids;
	else if(strcmp(group.strGroupName, "HD Channels") == 0) enumerator = enumerate_hd_channelids;
	else if(strcmp(group.strGroupName, "SD Channels") == 0) enumerator = enumerate_sd_channelids;
	else if(strcmp(group.strGroupName, "Demo Channels") == 0) enumerator = enumerate_demo_channelids;

	// If neither enumerator was selected, there isn't any work to do here
	if(enumerator == nullptr) return PVR_ERROR::PVR_ERROR_NO_ERROR;

	// Collect all of the PVR_CHANNEL_GROUP_MEMBER structures locally so that the database
	// connection isn't open any longer than necessary
	std::vector<PVR_CHANNEL_GROUP_MEMBER> members;

	// Create a copy of the current addon settings structure
	struct addon_settings settings = copy_settings();

	try {

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

			// iChannelNumber
			member.iChannelNumber = static_cast<int>(item.parts.channel);

			// iSubChannelNumber
			member.iSubChannelNumber = static_cast<int>(item.parts.subchannel);

			// Move the PVR_CHANNEL_GROUP_MEMBER into the local vector<>
			members.emplace_back(std::move(member));
		});

		// Transfer the generated PVR_CHANNEL_GROUP_MEMBER structures over to Kodi
		for(auto const& it : members) g_pvr->TransferChannelGroupMember(handle, &it);
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

	// Collect all of the PVR_CHANNEL structures locally so that the database
	// connection isn't open any longer than necessary
	std::vector<PVR_CHANNEL> channels;

	// Create a copy of the current addon settings structure
	struct addon_settings settings = copy_settings();

	try {

		// Pull a database connection out from the connection pool
		connectionpool::handle dbhandle(g_connpool);

		// Enumerate all of the channels in the database
		enumerate_channels(dbhandle, settings.prepend_channel_numbers, settings.show_drm_protected_channels, settings.use_channel_names_from_lineup, [&](struct channel const& item) -> void {

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

			// Move the PVR_CHANNEL structure into the local vector<>
			channels.emplace_back(std::move(channel));
		});

		// Transfer the generated PVR_CHANNEL structures over to Kodi 
		for(auto const& it : channels) g_pvr->TransferChannelEntry(handle, &it);
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

	// Create a copy of the current addon settings structure
	struct addon_settings settings = copy_settings();

	try {

		// Pull a database connection out from the connection pool
		connectionpool::handle dbhandle(g_connpool);

		// Enumerate all of the recordings in the database
		enumerate_recordings(dbhandle, settings.use_episode_number_as_title, [&](struct recording const& item) -> void {

			PVR_RECORDING recording;							// PVR_RECORDING to be transferred to Kodi
			memset(&recording, 0, sizeof(PVR_RECORDING));		// Initialize the structure

			// Determine if the recording is a repeat -- items marked specifically as firstairing or have a recordstarttime 
			// within 24 hours of the originalairdate can be considered as first airings
			bool isrepeat = !((item.firstairing == 1) || ((item.originalairdate + 86400) >= item.recordingtime));

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
			recording.iYear = item.year;

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
			assert(recording.iDuration > 0);

			// iLastPlayedPosition
			//
			recording.iLastPlayedPosition = item.lastposition;

			// iChannelUid
			recording.iChannelUid = item.channelid.value;

			// channelType
			recording.channelType = PVR_RECORDING_CHANNEL_TYPE_TV;

			// Move the PVR_RECORDING structure into the local vector<>
			recordings.emplace_back(std::move(recording));
		});

		// Transfer the generated PVR_RECORDING structures over to Kodi 
		for(auto const& it : recordings) g_pvr->TransferRecordingEntry(handle, &it);
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

		// Verify that the specified directory for the EDL files exists
		if(!g_addon->DirectoryExists(settings.recording_edl_folder.c_str()))
			throw string_exception(__func__, ": ", std::string("specified edit decision list file directory '") + settings.recording_edl_folder + "' cannot be accessed");

		// Pull a database connection out from the connection pool
		connectionpool::handle dbhandle(g_connpool);

		// Generate the base file name for the recording by combining the folder with the recording metadata
		std::string basename = get_recording_filename(dbhandle, recording.strRecordingId, settings.recording_edl_folder_is_flat);
		if(basename.length() == 0) throw string_exception(__func__, ": unable to determine the base file name of the specified recording");

		// Generate the full name of the .EDL file and if it exists, attempt to process it
		std::string filename = settings.recording_edl_folder.append(basename).append(".edl");
		if(g_addon->FileExists(filename.c_str(), false)) {

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
						entries.emplace_back(PVR_EDL_ENTRY { static_cast<int64_t>(start * 1000.0F), static_cast<int64_t>(end * 1000.0F), static_cast<PVR_EDL_TYPE>(type)});
					}

					else log_error(__func__, ": invalid edit decision list entry detected at line #", linenumber);
				}
				
				g_addon->CloseFile(handle);
			}

			else log_error(__func__, ": unable to open edit decision list file: ", filename.c_str());
		}

		else log_notice(__func__, ": edit decision list file not found: ", filename.c_str());

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
	assert(g_pvr);				

	if(handle == nullptr) return PVR_ERROR::PVR_ERROR_INVALID_PARAMETERS;

	time_t now = time(nullptr);				// Get the current date/time for comparison

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
			timer.iClientChannelUid = static_cast<int>(item.channelid.value);

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

			// firstDay
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

			// iEpgUid
			timer.iEpgUid = static_cast<unsigned int>(item.starttime);

			// strSeriesLink
			snprintf(timer.strSeriesLink, std::extent<decltype(timer.strSeriesLink)>::value, "%s", item.seriesid);

			// Move the PVR_TIMER structure into the local vector<>
			timers.emplace_back(std::move(timer));
		});

		// Transfer the generated PVR_TIMER structures over to Kodi
		for(auto const& it : timers) g_pvr->TransferTimerEntry(handle, &it);
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
			if(seriesid.length() == 0) seriesid = find_seriesid(dbhandle, authorization.c_str(), channelid, timer.startTime);
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
		// datetimeonlytimer             --> use the parent recording rule identifier
		// seriesrule / datetimeonlyrule --> use the recording rule identifier
		// anything else                 --> not implemented
		//
		if(timer.iTimerType == timer_type::datetimeonlytimer) recordingruleid = timer.iParentClientIndex;
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

	assert(g_addon);

	// The stream may have already been opened by a call to GetChannelStreamProperties()
	if(g_pvrstream) return true;

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

		// Default to accessing the stream via the storage engine if not prohibited by the settings or the channel itself
		if((settings.use_direct_tuning == false) && (get_tuner_direct_channel_flag(dbhandle, channelid) == false)) g_pvrstream = openlivestream_storage_http(dbhandle, settings, channelid, vchannel);

		// Fall back to accessing the stream via the tuner over HTTP if not prohibited by the settings
		if((!g_pvrstream) && (settings.direct_tuning_protocol == tuning_protocol::http)) g_pvrstream = openlivestream_tuner_http(dbhandle, settings, channelid, vchannel);

		// Fall back to accessing the stream via the tuner over RTP/UDP
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
			// don't log end time here, asctime/localtime won't work if time_t is 64-bit on this platform
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
	// Opening the stream in GetChannelStreamProperties has a side effect in that while the
	// stream is already open, Kodi will still always call CloseLiveStream() before OpenLiveStream()
	if(g_stream_close_hack) { g_stream_close_hack = false; return; }

	try {
		
		// Create a copy of the current addon settings structure
		struct addon_settings settings = copy_settings();

		// If the setting to refresh the recordings immediately after playback, reschedule it
		if(settings.discover_recordings_after_playback) {

			log_notice(__func__, ": triggering periodic recording discovery");
			g_scheduler.remove(discover_recordings_task);
			g_scheduler.add(std::chrono::system_clock::now(), discover_recordings_task);
		}
			
		// Ensure scheduler is running, may have been paused during playback
		g_scheduler.resume();

		// If the DVR stream is active, close it normally so exceptions are
		// propagated before destroying it; destructor alone won't throw
		if(g_pvrstream) g_pvrstream->close();
		g_pvrstream.reset();

		// Reset the global stream start and end time trackers
		g_stream_starttime = g_stream_endtime = 0;
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
	assert(g_addon);

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
	assert(g_addon);

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

PVR_ERROR GetChannelStreamProperties(PVR_CHANNEL const* channel, PVR_NAMED_VALUE* props, unsigned int* numprops)
{
	// This function is called before OpenLiveStream() will be called, but the required properties are
	// dynamic based on the stream metadata.  To fulfill the request, attempt to open the stream now
	if((g_pvrstream) || (OpenLiveStream(*channel) == false)) return PVR_ERROR::PVR_ERROR_NOT_IMPLEMENTED;
	assert(g_pvrstream);

	// Copy out the current state of the PVR client settings
	struct addon_settings settings = copy_settings();

	// PVR_STREAM_PROPERTY_MIMETYPE
	snprintf(props[0].strName, std::extent<decltype(props[0].strName)>::value, PVR_STREAM_PROPERTY_MIMETYPE);
	snprintf(props[0].strValue, std::extent<decltype(props[0].strName)>::value, "%s", g_pvrstream->mediatype());

	// PVR_STREAM_PROPERTY_ISREALTIMESTREAM
	snprintf(props[1].strName, std::extent<decltype(props[1].strName)>::value, PVR_STREAM_PROPERTY_ISREALTIMESTREAM);
	snprintf(props[1].strValue, std::extent<decltype(props[1].strName)>::value, (g_pvrstream->realtime() ? "true" : "false"));

	*numprops = 2;

	// Hack to prevent CloseLiveStream() from actually closing the stream since it was opened before 
	// OpenLiveStream() has technically been called by Kodi
	g_stream_close_hack = true;

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
	// This function is called before OpenRecordedStream() will be called, but the required properties are
	// dynamic based on the stream metadata.  To fulfill the request, attempt to open the stream now
	if((g_pvrstream) || (OpenRecordedStream(*recording) == false)) return PVR_ERROR::PVR_ERROR_NOT_IMPLEMENTED;
	assert(g_pvrstream);

	// Copy out the current state of the PVR client settings
	struct addon_settings settings = copy_settings();

	// PVR_STREAM_PROPERTY_MIMETYPE
	snprintf(props[0].strName, std::extent<decltype(props[0].strName)>::value, PVR_STREAM_PROPERTY_MIMETYPE);
	snprintf(props[0].strValue, std::extent<decltype(props[0].strName)>::value, "%s", g_pvrstream->mediatype());

	// PVR_STREAM_PROPERTY_ISREALTIMESTREAM
	snprintf(props[1].strName, std::extent<decltype(props[1].strName)>::value, PVR_STREAM_PROPERTY_ISREALTIMESTREAM);
	snprintf(props[1].strValue, std::extent<decltype(props[1].strName)>::value, (g_pvrstream->realtime() ? "true" : "false"));

	*numprops = 2;

	// Hack to prevent CloseRecordedStream() from actually closing the stream since it was opened before 
	// OpenRecordedStream() has technically been called by Kodi
	g_stream_close_hack = true;

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
//	properties	- The properties of the currently playing stream

PVR_ERROR GetStreamReadChunkSize(int* chunksize)
{
	if(chunksize == nullptr) return PVR_ERROR::PVR_ERROR_INVALID_PARAMETERS;

	// Create a copy of the current addon settings structure
	struct addon_settings settings = copy_settings();
	*chunksize = settings.stream_read_chunk_size;

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
	assert(g_addon);

	// The stream may have already been opened by a call to GetRecordingStreamProperties()
	if(g_pvrstream) return true;

	// Create a copy of the current addon settings structure
	struct addon_settings settings = copy_settings();

	try {

		// Pull a database connection out from the connection pool
		connectionpool::handle dbhandle(g_connpool);

		// Generate the stream URL for the specified channel
		std::string streamurl = get_recording_stream_url(dbhandle, recording.strRecordingId);
		if(streamurl.length() == 0) throw string_exception(__func__, ": unable to determine the URL for specified recording");

		// Pause the scheduler if the user wants that functionality disabled during streaming
		if(settings.pause_discovery_while_streaming) g_scheduler.pause();

		try {

			// Start the new recording stream using the tuning parameters currently specified by the settings
			log_notice(__func__, ": streaming recording '", recording.strTitle, "' via url ", streamurl.c_str());
			g_pvrstream = httpstream::create(streamurl.c_str(), settings.stream_ring_buffer_size, settings.stream_read_chunk_size);

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
	// Opening the stream in GetRecordingStreamProperties has a side effect in that while the
	// stream is already open, Kodi will still always call CloseRecordedStream() before OpenRecordedStream()
	if(g_stream_close_hack) { g_stream_close_hack = false; return; }

	try {

		// Create a copy of the current addon settings structure
		struct addon_settings settings = copy_settings();

		// If the setting to refresh the recordings immediately after playback, reschedule it
		if(settings.discover_recordings_after_playback) {

			log_notice(__func__, ": triggering periodic recording discovery");
			g_scheduler.remove(discover_recordings_task);
			g_scheduler.add(std::chrono::system_clock::now(), discover_recordings_task);
		}
			
		// Ensure scheduler is running, may have been paused during playback
		g_scheduler.resume();

		// If the DVR stream is active, close it normally so exceptions are
		// propagated before destroying it; destructor alone won't throw
		if(g_pvrstream) g_pvrstream->close();
		g_pvrstream.reset();

		// Reset the global stream start and end time trackers
		g_stream_starttime = g_stream_endtime = 0;
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
	assert(g_addon);

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
	assert(g_addon);

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

	// Create a copy of the current addon settings structure
	struct addon_settings settings = copy_settings();

	try {

		g_scheduler.stop();					// Ensure scheduler was stopped
		g_scheduler.clear();				// Ensure there are no pending tasks

		// The special discover_startup_task takes care of all discoveries in a more optimized fashion than invoking the 
		// periodic ones; use that on wakeup too.  Bind true as the first argument to discover_startup_task to indicate 
		// that devices and lineups should be discovered as well as everything else
		log_notice(__func__, ": scheduling startup discovery task (delayed ", settings.startup_discovery_task_delay, " seconds)");
		g_scheduler.add(std::chrono::system_clock::now() + std::chrono::seconds(settings.startup_discovery_task_delay), std::bind(discover_startup_task, true, std::placeholders::_1));
	
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

PVR_ERROR GetStreamTimes(PVR_STREAM_TIMES* times)
{
	assert(times != nullptr);
	assert(g_stream_starttime <= g_stream_endtime);

	// Block this function for non-seekable streams otherwise Kodi will allow those operations
	if((!g_pvrstream) || (!g_pvrstream->canseek())) return PVR_ERROR::PVR_ERROR_NOT_IMPLEMENTED;

	// SPECIAL CASE: If start time and end time are the same, and this is a fixed-length stream also 
	// let Kodi handle it - it can figure this out from the stream data. This can happen if the duration 
	// of a recorded stream was not reported properly for some reason (credit: timecutter)
	if((g_stream_starttime == g_stream_endtime) && (g_pvrstream->realtime() == false)) return PVR_ERROR::PVR_ERROR_NOT_IMPLEMENTED;

	// Set the start time to the actual start time (UTC) for realtime streams, otherwise zero
	// Using zero here is required to enable calls to SetRecordingLastPlayedPosition()
	times->startTime = (g_pvrstream->realtime()) ? g_stream_starttime : 0;

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
