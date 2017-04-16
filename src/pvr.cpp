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
#include <memory>
#include <mutex>
#include <pthread.h>
#include <string>
#include <sstream>
#include <vector>

#ifdef __ANDROID__
#include <android/log.h>
#endif

#include <version.h>
#include <xbmc_pvr_dll.h>

#include "addoncallbacks.h"
#include "database.h"
#include "hdhr.h"
#include "livestream.h"
#include "pvrcallbacks.h"
#include "scalar_condition.h"
#include "scheduler.h"
#include "string_exception.h"

#pragma warning(push, 4)				// Enable maximum compiler warnings

//---------------------------------------------------------------------------
// MACROS
//---------------------------------------------------------------------------

// LIVESTREAM_BUFFER_SIZE
//
// 4 megabytes is sufficient to store 2 seconds of content at 16Mb/s
#define LIVESTREAM_BUFFER_SIZE	(4 MiB)

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
template<typename... _args>	static void log_message(addoncallbacks::addon_log_t level, _args&&... args);
template<typename... _args> static void log_notice(_args&&... args);

// Scheduled Tasks
//
static void discover_devices_task(scalar_condition<bool> const& cancel);
static void discover_episodes_task(scalar_condition<bool> const& cancel);
static void discover_guide_task(scalar_condition<bool> const& cancel);
static void discover_lineups_task(scalar_condition<bool> const& cancel);
static void discover_recordingrules_task(scalar_condition<bool> const& cancel);
static void discover_recordings_task(scalar_condition<bool> const& cancel);

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

// guide_data
//
// Defines the level of guide data to load from the backend
enum guide_data {

	basic					= 0,
	extended				= 1,
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

	// use_extended_guide_data
	//
	// Flag to load the (somewhat) extended guide data
	enum guide_data guide_data_level;

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
};

//---------------------------------------------------------------------------
// GLOBAL VARIABLES
//---------------------------------------------------------------------------

// g_addon
//
// Kodi add-on callback implementation
static std::unique_ptr<addoncallbacks> g_addon;

// g_capabilities (const)
//
// PVR implementation capability flags
static const PVR_ADDON_CAPABILITIES g_capabilities = {

	true,		// bSupportsEPG
	true,		// bSupportsTV
	false,		// bSupportsRadio
	true,		// bSupportsRecordings
	false,		// bSupportsRecordingsUndelete
	true,		// bSupportsTimers
	true,		// bSupportsChannelGroups
	false,		// bSupportsChannelScan
	false,		// bSupportsChannelSettings
	true,		// bHandlesInputStream
	false,		// bHandlesDemuxing
	false,		// bSupportsRecordingPlayCount
	false,		// bSupportsLastPlayedPosition
	false,		// bSupportsRecordingEdl
};

// g_capabilities_directtune (const)
//
// PVR implementation capability flags when 'direct tuning' is enabled
static const PVR_ADDON_CAPABILITIES g_capabilities_directtune = {

	true,		// bSupportsEPG
	true,		// bSupportsTV
	false,		// bSupportsRadio
	true,		// bSupportsRecordings
	false,		// bSupportsRecordingsUndelete
	true,		// bSupportsTimers
	true,		// bSupportsChannelGroups
	false,		// bSupportsChannelScan
	false,		// bSupportsChannelSettings
	false,		// bHandlesInputStream
	false,		// bHandlesDemuxing
	false,		// bSupportsRecordingPlayCount
	false,		// bSupportsLastPlayedPosition
	false,		// bSupportsRecordingEdl
};

// g_connpool
//
// Global SQLite database connection pool instance
static std::shared_ptr<connectionpool> g_connpool;

// g_epgmaxtime
//
// Maximum number of days to report for EPG and series timers
static int g_epgmaxtime = EPG_TIMEFRAME_UNLIMITED;

// g_livestream
//
// Global live stream buffer instance
static livestream g_livestream(LIVESTREAM_BUFFER_SIZE);

// g_pvr
//
// Kodi PVR add-on callback implementation
static std::unique_ptr<pvrcallbacks> g_pvr;

// g_scheduler
//
// Task scheduler
static scheduler g_scheduler([](std::exception const& ex) -> void { handle_stdexception("scheduled task", ex); });

// g_settings
//
// Global addon settings instance
static addon_settings g_settings = {

	guide_data::basic,		// guide_data_level
	false,					// pause_discovery_while_streaming
	false,					// prepend_channel_numbers
	false,					// use_episode_number_as_title
	86400,					// delete_datetime_rules_after			default = 1 day
	false,					// use_broadcast_device_discovery
	300, 					// discover_devices_interval;			default = 5 minutes
	7200,					// discover_episodes_interval			default = 2 hours
	1800,					// discover_guide_interval				default = 30 minutes
	600,					// discover_lineups_interval			default = 10 minutes
	600,					// discover_recordings_interval			default = 10 minutes
	7200,					// discover_recordingrules_interval		default = 2 hours
	false,					// use_direct_tuning
};

// g_settings_lock
//
// Synchronization object to serialize access to addon settings
std::mutex g_settings_lock;

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
			PVR_TIMER_TYPE_SUPPORTS_START_END_MARGIN | PVR_TIMER_TYPE_FORBIDS_EPG_TAG_ON_CREATE,

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
		PVR_TIMER_TYPE_IS_REPEATING | PVR_TIMER_TYPE_SUPPORTS_CHANNELS | PVR_TIMER_TYPE_SUPPORTS_RECORD_ONLY_NEW_EPISODES | 
			PVR_TIMER_TYPE_SUPPORTS_START_END_MARGIN | PVR_TIMER_TYPE_REQUIRES_EPG_SERIES_ON_CREATE,

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
};

// discover_devices_task
//
// Scheduled task implementation to discover the HDHomeRun devices
static void discover_devices_task(scalar_condition<bool> const& cancel)
{
	bool		changed = false;			// Flag if the discovery data changed

	assert(g_addon && g_pvr);
	log_notice(__func__, ": initiated local network device discovery");

	// Grab copies of the required setting(s) up front
	std::unique_lock<std::mutex> settings_lock(g_settings_lock);
	int discover_devices_interval = g_settings.discover_devices_interval;
	bool use_broadcast_device_discovery = g_settings.use_broadcast_device_discovery;
	settings_lock.unlock();

	try {

		// Pull a database connection out from the connection pool
		connectionpool::handle dbhandle(g_connpool);

		// Discover the devices on the local network and check for changes
		discover_devices(dbhandle, use_broadcast_device_discovery, changed);

		if(changed) {

			// If the available devices have changed, remove any scheduled lineup and/or recording
			// discovery tasks and execute them now; they will reschedule themselves when done

			log_notice(__func__, ": device discovery data changed -- execute lineup discovery now");
			g_scheduler.remove(discover_lineups_task);
			discover_lineups_task(cancel);

			log_notice(__func__, ": device discovery data changed -- execute recording discovery now");
			g_scheduler.remove(discover_recordings_task);
			discover_recordings_task(cancel);
		}
	}

	catch(std::exception& ex) { handle_stdexception(__func__, ex); }
	catch(...) { handle_generalexception(__func__); }

	// Schedule the next periodic invocation of this discovery task
	auto due = std::chrono::system_clock::now() + std::chrono::seconds(discover_devices_interval);

	log_notice(__func__, ": scheduling next device discovery to initiate in ", discover_devices_interval, " seconds");
	g_scheduler.add(due, discover_devices_task);
}

// discover_episodes_task
//
// Scheduled task implementation to discover the episode data associated with recording rules
static void discover_episodes_task(scalar_condition<bool> const& /*cancel*/)
{
	bool		changed = false;			// Flag if the discovery data changed

	assert(g_addon && g_pvr);
	log_notice(__func__, ": initiated recording rule episode discovery");

	// Grab copies of the required setting(s) up front
	std::unique_lock<std::mutex> settings_lock(g_settings_lock);
	int discover_episodes_interval = g_settings.discover_episodes_interval;
	settings_lock.unlock();

	try {

		// Pull a database connection out from the connection pool
		connectionpool::handle dbhandle(g_connpool);

		// Discover the episode data from the backend service
		discover_episodes(dbhandle, changed);
		
		if(changed) {

			// Trigger electronic program guide updates for all channels with episode information
			log_notice(__func__, ": recording rule episode discovery data changed -- trigger electronic program guide update");
			enumerate_episode_channelids(dbhandle, [&](union channelid const& item) -> void { g_pvr->TriggerEpgUpdate(item.value); });

			// Changes in the episode data affects the PVR timers
			log_notice(__func__, ": recording rule episode discovery data changed -- trigger timer update");
			g_pvr->TriggerTimerUpdate();
		}
	}

	catch(std::exception& ex) { handle_stdexception(__func__, ex); }
	catch(...) { handle_generalexception(__func__); }

	// Schedule the next periodic invocation of this discovery task
	auto due = std::chrono::system_clock::now() + std::chrono::seconds(discover_episodes_interval);

	log_notice(__func__, ": scheduling next recording rule episode discovery to initiate in ", discover_episodes_interval, " seconds");
	g_scheduler.add(due, discover_episodes_task);
}

// discover_guide_task
//
// Scheduled task implementation to discover the electronic program guide
static void discover_guide_task(scalar_condition<bool> const& cancel)
{
	bool		changed = false;			// Flag if the discovery data changed

	assert(g_addon && g_pvr);
	log_notice(__func__, ": initiated electronic program guide discovery");

	// Grab copies of the required setting(s) up front
	std::unique_lock<std::mutex> settings_lock(g_settings_lock);
	enum guide_data guide_data_level = g_settings.guide_data_level;
	int discover_guide_interval = g_settings.discover_guide_interval;
	settings_lock.unlock();

	try {

		// Pull a database connection out from the connection pool
		connectionpool::handle dbhandle(g_connpool);

		// Discover the updated electronic program guide data from the backend service
		if(guide_data_level == guide_data::basic) discover_guide_basic(dbhandle, changed);
		else if(guide_data_level == guide_data::extended) discover_guide_extended(dbhandle, cancel, changed);
		else {

			// If the guide_data_level setting is bad, default to using the basic guide data
			log_error(__func__, ": guide_data_level setting invalid state -- defaulting to guide_data::basic");
			discover_guide_basic(dbhandle, changed);
		}
		
		if(changed) {

			// Trigger an EPG update for every channel; it will be typical that they all change
			log_notice(__func__, ": guide discovery data changed -- trigger EPG updates");
			enumerate_channelids(dbhandle, [&](union channelid const& item) -> void { g_pvr->TriggerEpgUpdate(item.value); });

			// Trigger a channel update; the guide data is used to resolve channel names and icon image urls
			log_notice(__func__, ": guide channel discovery data changed -- trigger channel update");
			g_pvr->TriggerChannelUpdate();
		}
	}

	catch(std::exception& ex) { handle_stdexception(__func__, ex); }
	catch(...) { handle_generalexception(__func__); }

	// Schedule the next periodic invocation of this discovery task
	auto due = std::chrono::system_clock::now() + std::chrono::seconds(discover_guide_interval);

	log_notice(__func__, ": scheduling next guide discovery to initiate in ", discover_guide_interval, " seconds");
	g_scheduler.add(due, discover_guide_task);
}

// discover_lineups_task
//
// Scheduled task implementation to discover the channel lineups
static void discover_lineups_task(scalar_condition<bool> const& cancel)
{
	bool		changed = false;			// Flag if the discovery data changed

	assert(g_addon && g_pvr);
	log_notice(__func__, ": initiated local tuner device lineup discovery");

	// Grab copies of the required setting(s) up front
	std::unique_lock<std::mutex> settings_lock(g_settings_lock);
	int discover_lineups_interval = g_settings.discover_lineups_interval;
	settings_lock.unlock();

	try {

		// Pull a database connection out from the connection pool
		connectionpool::handle dbhandle(g_connpool);

		// Discover the channel lineups for all available tuner devices
		discover_lineups(dbhandle, changed);
		
		if(changed) {

			// Changes in the lineup data directly affect the PVR channels and channel groups
			log_notice(__func__, ": lineup discovery data changed -- trigger channel update");
			g_pvr->TriggerChannelUpdate();

			log_notice(__func__, ": lineup discovery data changed -- trigger channel group update");
			g_pvr->TriggerChannelGroupsUpdate();

			// Changes in the lineup data indirectly affects the electronic program guide (EPG) data,
			// if new channels were added they may be able to be populated in the guide immediately
			log_notice(__func__, ": lineup discovery data changed -- execute electronic program guide discovery now");
			g_scheduler.remove(discover_guide_task);
			discover_guide_task(cancel);
		}
	}

	catch(std::exception& ex) { handle_stdexception(__func__, ex); }
	catch(...) { handle_generalexception(__func__); }

	// Schedule the next periodic invocation of this discovery task
	auto due = std::chrono::system_clock::now() + std::chrono::seconds(discover_lineups_interval);

	log_notice(__func__, ": scheduling next lineup discovery to initiate in ", discover_lineups_interval, " seconds");
	g_scheduler.add(due, discover_lineups_task);
}

// discover_recordingrules_task
//
// Scheduled task implementation to discover the recording rules and timers
static void discover_recordingrules_task(scalar_condition<bool> const& /*cancel*/)
{
	bool		changed = false;			// Flag if the discovery data changed

	assert(g_addon && g_pvr);
	log_notice(__func__, ": initiated recording rule discovery");

	// Grab copies of the required setting(s) up front
	std::unique_lock<std::mutex> settings_lock(g_settings_lock);
	int delete_datetime_rules_after = g_settings.delete_datetime_rules_after;
	int discover_recordingrules_interval = g_settings.discover_recordingrules_interval;
	settings_lock.unlock();

	try {

		// Pull a database connection out from the connection pool
		connectionpool::handle dbhandle(g_connpool);

		// Discover the recording rules from the backend service
		discover_recordingrules(dbhandle, changed);

		// Generate a vector<> of all expired recording rules to be deleted from the backend
		std::vector<unsigned int> expired;
		enumerate_expired_recordingruleids(dbhandle, delete_datetime_rules_after, 
			[&](unsigned int const& recordingruleid) -> void { expired.push_back(recordingruleid); });

		// Iterate over the vector<> and attempt to delete each expired rule from the backend
		for(auto const& it : expired) {

			try { delete_recordingrule(dbhandle, it); changed = true; }
			catch(std::exception& ex) { handle_stdexception(__func__, ex); }
			catch(...) { handle_generalexception(__func__); }
		}
		
		if(changed) {

			changed = false;

			// If the recording rules have changed, the episode information needs to be refreshed.  Don't use
			// the scheduled task, do it synchronously so that the Kodi timer update isn't triggered twice
			log_notice(__func__, ": recording rule discovery data changed -- execute episode update now");
			discover_episodes(dbhandle, changed);

			if(changed) {

				// Trigger electronic program guide updates for all channels with episode information
				log_notice(__func__, ": recording rule episode discovery data changed -- trigger electronic program guide update");
				enumerate_episode_channelids(dbhandle, [&](union channelid const& item) -> void { g_pvr->TriggerEpgUpdate(item.value); });
			}

			// Changes in the recording rule and/or episode data affects the PVR timers
			log_notice(__func__, ": recording rule discovery data changed -- trigger timer update");
			g_pvr->TriggerTimerUpdate();
		}
	}

	catch(std::exception& ex) { handle_stdexception(__func__, ex); }
	catch(...) { handle_generalexception(__func__); }

	// Schedule the next periodic invocation of this discovery task
	auto due = std::chrono::system_clock::now() + std::chrono::seconds(discover_recordingrules_interval);

	log_notice(__func__, ": scheduling next recording rule discovery to initiate in ", discover_recordingrules_interval, " seconds");
	g_scheduler.add(due, discover_recordingrules_task);
}

// discover_recordings_task
//
// Scheduled task implementation to discover the storage recordings
static void discover_recordings_task(scalar_condition<bool> const& /*cancel*/)
{
	bool		changed = false;			// Flag if the discovery data changed

	assert(g_addon && g_pvr);
	log_notice(__func__, ": initiated local storage device recording discovery");

	// Grab copies of the required setting(s) up front
	std::unique_lock<std::mutex> settings_lock(g_settings_lock);
	int discover_recordings_interval = g_settings.discover_recordings_interval;
	settings_lock.unlock();

	try {

		// Pull a database connection out from the connection pool
		connectionpool::handle dbhandle(g_connpool);

		// Discover the recordings for all available local storage devices
		discover_recordings(dbhandle, changed);
		
		if(changed) {

			// Changes in the recording data affects just the PVR recordings
			log_notice(__func__, ": recording discovery data changed -- trigger recording update");
			g_pvr->TriggerRecordingUpdate();
		}
	}

	catch(std::exception& ex) { handle_stdexception(__func__, ex); }
	catch(...) { handle_generalexception(__func__); }

	// Schedule the next periodic invocation of this discovery task
	auto due = std::chrono::system_clock::now() + std::chrono::seconds(discover_recordings_interval);

	log_notice(__func__, ": scheduling next recording discovery to initiate in ", discover_recordings_interval, " seconds");
	g_scheduler.add(due, discover_recordings_task);
}

// guidedatalevel_enum_to_guidedata
//
// Converts the guide data level enumeration value into a guide_data value
static enum guide_data guidedatalevel_enum_to_guidedata(int nvalue)
{
	switch(nvalue) {

		case 0: return guide_data::basic;
		case 1: return guide_data::extended;
	}

	return guide_data::basic;
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
	};

	return 600;						// 10 minutes = default
};
	
// log_debug
//
// Variadic method of writing a LOG_DEBUG entry into the Kodi application log
template<typename... _args>
static void log_debug(_args&&... args)
{
	log_message(addoncallbacks::addon_log_t::LOG_DEBUG, std::forward<_args>(args)...);
}

// log_error
//
// Variadic method of writing a LOG_ERROR entry into the Kodi application log
template<typename... _args>
static void log_error(_args&&... args)
{
	log_message(addoncallbacks::addon_log_t::LOG_ERROR, std::forward<_args>(args)...);
}

// log_info
//
// Variadic method of writing a LOG_INFO entry into the Kodi application log
template<typename... _args>
static void log_info(_args&&... args)
{
	log_message(addoncallbacks::addon_log_t::LOG_INFO, std::forward<_args>(args)...);
}

// log_message
//
// Variadic method of writing an entry into the Kodi application log
template<typename... _args>
static void log_message(addoncallbacks::addon_log_t level, _args&&... args)
{
	std::ostringstream stream;
	int unpack[] = {0, ( static_cast<void>(stream << args), 0 ) ... };
	(void)unpack;

	if(g_addon) g_addon->Log(level, stream.str().c_str());

	// Write LOG_ERROR level messages to an appropriate secondary log mechanism
	if(level == addoncallbacks::addon_log_t::LOG_ERROR) {

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
	log_message(addoncallbacks::addon_log_t::LOG_NOTICE, std::forward<_args>(args)...);
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

	// Copy anything relevant from the provided parameters
	PVR_PROPERTIES* pvrprops = reinterpret_cast<PVR_PROPERTIES*>(props);
	g_epgmaxtime = pvrprops->iEpgMaxDays;

	try {

#ifdef _WINDOWS
		// On Windows, initialize winsock in case broadcast discovery is used; WSAStartup is
		// reference-counted so if it's already been called this won't hurt anything
		WSADATA wsaData;
		WSAStartup(MAKEWORD(2, 2), &wsaData);
#endif

		// Initialize libcurl using the standard default options
		if(curl_global_init(CURL_GLOBAL_DEFAULT) != CURLE_OK) throw string_exception("curl_global_init(CURL_GLOBAL_DEFAULT) failed");

		// Create the global addoncallbacks instance
		g_addon.reset(new addoncallbacks(handle));

		// Throw a banner out to the Kodi log indicating that the add-on is being loaded
		log_notice(VERSION_PRODUCTNAME_ANSI, " v", VERSION_VERSION3_ANSI, " loading");

		try { 

			// The user data path doesn't always exist when an addon has been installed
			if(!g_addon->DirectoryExists(pvrprops->strUserPath)) {

				log_notice(__func__, ": user data directory ", pvrprops->strUserPath, " does not exist");
				if(!g_addon->CreateDirectory(pvrprops->strUserPath)) throw string_exception("unable to create addon user data directory");
				log_notice(__func__, ": user data directory ", pvrprops->strUserPath, " created");
			}

			// Load the general settings
			if(g_addon->GetSetting("guide_data_level", &nvalue)) g_settings.guide_data_level = guidedatalevel_enum_to_guidedata(nvalue);
			if(g_addon->GetSetting("pause_discovery_while_streaming", &bvalue)) g_settings.pause_discovery_while_streaming = bvalue;
			if(g_addon->GetSetting("prepend_channel_numbers", &bvalue)) g_settings.prepend_channel_numbers = bvalue;
			if(g_addon->GetSetting("use_episode_number_as_title", &bvalue)) g_settings.use_episode_number_as_title = bvalue;
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

			// Create the global pvrcallbacks instance
			g_pvr.reset(new pvrcallbacks(handle));
		
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

				// Create the global database connection pool instance, the file name is based on the versionb
				std::string databasefile = "file:///" + std::string(pvrprops->strUserPath) + "/hdhomerundvr-v" + VERSION_VERSION2_ANSI + ".db";
				g_connpool = std::make_shared<connectionpool>(databasefile.c_str(), SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE | SQLITE_OPEN_URI);

				try {

					// Find out what time it is now to set up the scheduler
					std::chrono::time_point<std::chrono::system_clock> now = std::chrono::system_clock::now();

					// WORKAROUND: Kodi currently has no means to create EPG entries in the database for channels
					// that are added after the PVR manager has been started.  When the database is empty at startup, 
					// synchronously execute a device and lineup discovery so that the initial set of channels are 
					// available.  This prevents the user from having to shut down and restart Kodi after installation
					// but does not address a lack of EPG entries for channels added after the initial discovery ...
					connectionpool::handle dbhandle(g_connpool);
					if(get_channel_count(dbhandle) == 0) {

						log_notice(__func__, ": no channels detected in the database -- execute device and lineup discovery now");

						discover_devices_task(cancel);				// Discover the initial set of devices
						discover_lineups_task(cancel);				// Discover the initial set of channels
					}

					else {

						// Channels are present in the database; schedule the device and lineup discoveries
						// such that they still execute first, but do so asynchronously and not slow startup
						g_scheduler.add(now + std::chrono::seconds(1), discover_devices_task);
						g_scheduler.add(now + std::chrono::seconds(2), discover_lineups_task);
					}

					// Schedule the initial runs for the remaining discoveries to execute sequentially as soon as 
					// possible; now that the database is an external file there is no compelling reason to launch
					// them synchronously -- whatever was in the database when Kodi shut down will be used at first
					g_scheduler.add(now + std::chrono::seconds(3), discover_recordings_task);
					g_scheduler.add(now + std::chrono::seconds(4), discover_guide_task);
					g_scheduler.add(now + std::chrono::seconds(5), discover_recordingrules_task);
					g_scheduler.add(now + std::chrono::seconds(6), discover_episodes_task);

					g_scheduler.start();				// <--- Start the task scheduler
				}

				// Clean up the database connection pool on exception
				catch(...) { g_connpool.reset(); throw; }
			}
			
			// Clean up the pvrcallbacks instance on exception
			catch(...) { g_pvr.reset(nullptr); throw; }
		}

		// Clean up the addoncallbacks on exception; but log the error first -- once the callbacks
		// are destroyed so is the ability to write to the Kodi log file
		catch(std::exception& ex) { handle_stdexception(__func__, ex); g_addon.reset(nullptr); throw; }
		catch(...) { handle_generalexception(__func__); g_addon.reset(nullptr); throw; }
	}

	// Anything that escapes above can't be logged at this point, just return ADDON_STATUS_PERMANENT_FAILURE
	catch(...) { return ADDON_STATUS::ADDON_STATUS_PERMANENT_FAILURE; }

	// Throw a simple banner out to the Kodi log indicating that the add-on has been loaded
	log_notice(VERSION_PRODUCTNAME_ANSI, " v", VERSION_VERSION3_ANSI, " loaded");

	return ADDON_STATUS::ADDON_STATUS_OK;
}

//---------------------------------------------------------------------------
// ADDON_Stop
//
// Instructs the addon to stop all activities
//
// Arguments:
//
//	NONE

void ADDON_Stop(void)
{
	g_livestream.stop();			// Stop the live stream
	g_scheduler.stop();				// Stop the task scheduler
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
	log_notice(VERSION_PRODUCTNAME_ANSI, " v", VERSION_VERSION3_ANSI, " unloading");

	// Stop any executing live stream data transfer as well as the task scheduler
	g_livestream.stop();
	g_scheduler.stop();

	// Destroy all the dynamically created objects
	g_connpool.reset();
	g_pvr.reset(nullptr);
	g_addon.reset(nullptr);

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
// ADDON_HasSettings
//
// Indicates if the Kodi addon has settings or not
//
// Arguments:
//
//	NONE

bool ADDON_HasSettings(void)
{
	return true;
}

//---------------------------------------------------------------------------
// ADDON_GetSettings
//
// Acquires the information about the Kodi addon settings
//
// Arguments:
//
//	settings		- Structure to receive the addon settings

unsigned int ADDON_GetSettings(ADDON_StructSetting*** /*settings*/)
{
	return 0;
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

	// guide_data_level
	//
	if(strcmp(name, "guide_data_level") == 0) {

		enum guide_data nvalue = guidedatalevel_enum_to_guidedata(*reinterpret_cast<int const*>(value));
		if(nvalue != g_settings.guide_data_level) {

			g_settings.guide_data_level = nvalue;
			log_notice(__func__, ": setting guide_data_level changed to ", (nvalue == guide_data::basic) ? "basic" : "extended");

			// If the change enabled extended data, reschedule the task to run as soon as possible
			if(g_settings.guide_data_level == guide_data::extended) {

				g_scheduler.remove(discover_guide_task);
				g_scheduler.add(now + std::chrono::seconds(1), discover_guide_task);
				log_notice(__func__, ": extended guide data enabled -- rescheduling task to initiate in 1 second");
			}
		}
	}

	// pause_discovery_while_streaming
	//
	else if(strcmp(name, "pause_discovery_while_streaming") == 0) {

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

			// This setting is only read during ADDON_Create(); notify the user to restart Kodi
			g_addon->QueueNotification(addoncallbacks::queue_msg_t::QUEUE_INFO, g_addon->GetLocalizedString(30401));
			log_notice(__func__, ": setting use_direct_tuning changed to ", (bvalue) ? "true" : "false", " -- restart required");
		}
	}

	return ADDON_STATUS_OK;
}

//---------------------------------------------------------------------------
// ADDON_FreeSettings
//
// Releases settings allocated by ADDON_GetSettings
//
// Arguments:
//
//	NONE

void ADDON_FreeSettings(void)
{
}

//---------------------------------------------------------------------------
// KODI PVR ADDON ENTRY POINTS
//---------------------------------------------------------------------------

//---------------------------------------------------------------------------
// GetPVRAPIVersion
//
// Get the XBMC_PVR_API_VERSION that was used to compile this add-on
//
// Arguments:
//
//	NONE

char const* GetPVRAPIVersion(void)
{
	return XBMC_PVR_API_VERSION;
}

//---------------------------------------------------------------------------
// GetMininumPVRAPIVersion
//
// Get the XBMC_PVR_MIN_API_VERSION that was used to compile this add-on
//
// Arguments:
//
//	NONE

char const* GetMininumPVRAPIVersion(void)
{
	return XBMC_PVR_MIN_API_VERSION;
}

//---------------------------------------------------------------------------
// GetGUIAPIVersion
//
// Get the XBMC_GUI_API_VERSION that was used to compile this add-on
//
// Arguments:
//
//	NONE

char const* GetGUIAPIVersion(void)
{
	return "";
}

//---------------------------------------------------------------------------
// GetMininumGUIAPIVersion
//
// Get the XBMC_GUI_MIN_API_VERSION that was used to compile this add-on
//
// Arguments:
//
//	NONE

char const* GetMininumGUIAPIVersion(void)
{
	return "";
}

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

	// g_settings.use_direct_tuning does not require synchronization
	if(g_settings.use_direct_tuning) *capabilities = g_capabilities_directtune;
	else *capabilities = g_capabilities;
	
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

	// MENUHOOK_SETTING_TRIGGERDEVICEDISCOVERY
	//
	else if(menuhook.iHookId == MENUHOOK_SETTING_TRIGGERDEVICEDISCOVERY) {

		log_notice(__func__, ": scheduling device discovery task to execute in 1 second");
		g_scheduler.remove(discover_devices_task);
		g_scheduler.add(now + std::chrono::seconds(1), discover_devices_task);
	}

	// MENUHOOK_SETTING_TRIGGERLINEUPDISCOVERY
	//
	else if(menuhook.iHookId == MENUHOOK_SETTING_TRIGGERLINEUPDISCOVERY) {

		log_notice(__func__, ": scheduling lineup discovery task to execute in 1 second");
		g_scheduler.remove(discover_lineups_task);
		g_scheduler.add(now + std::chrono::seconds(1), discover_lineups_task);
	}

	// MENUHOOK_SETTING_TRIGGERGUIDEDISCOVERY
	//
	else if(menuhook.iHookId == MENUHOOK_SETTING_TRIGGERGUIDEDISCOVERY) {

		log_notice(__func__, ": scheduling guide discovery task to execute in 1 second");
		g_scheduler.remove(discover_guide_task);
		g_scheduler.add(now + std::chrono::seconds(1), discover_guide_task);
	}

	// MENUHOOK_SETTING_TRIGGERRECORDINGDISCOVERY
	//
	else if(menuhook.iHookId == MENUHOOK_SETTING_TRIGGERRECORDINGDISCOVERY) {

		log_notice(__func__, ": scheduling recording discovery task to execute in 1 second");
		g_scheduler.remove(discover_recordings_task);
		g_scheduler.add(now + std::chrono::seconds(1), discover_recordings_task);
	}

	// MENUHOOK_SETTING_TRIGGERRECORDINGRULEDISCOVERY
	//
	else if(menuhook.iHookId == MENUHOOK_SETTING_TRIGGERRECORDINGRULEDISCOVERY) {

		log_notice(__func__, ": scheduling recording rule discovery task to execute in 1 second");
		g_scheduler.remove(discover_recordingrules_task);
		g_scheduler.add(now + std::chrono::seconds(1), discover_recordingrules_task);
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

PVR_ERROR GetEPGForChannel(ADDON_HANDLE handle, PVR_CHANNEL const& channel, time_t /*start*/, time_t /*end*/)
{
	assert(g_pvr);

	if(handle == nullptr) return PVR_ERROR::PVR_ERROR_INVALID_PARAMETERS;

	// Retrieve the channel identifier from the PVR_CHANNEL structure
	union channelid channelid;
	channelid.value = channel.iUniqueId;
		
	try {

		// Pull a database connection out from the connection pool
		connectionpool::handle dbhandle(g_connpool);

		// Enumerate all of the guide entries in the database for this channel and time frame
		enumerate_guideentries(dbhandle, channelid, g_epgmaxtime, [&](struct guideentry const& item) -> void {

			EPG_TAG	epgtag;										// EPG_TAG to be transferred to Kodi
			memset(&epgtag, 0, sizeof(EPG_TAG));				// Initialize the structure

			// iUniqueBroadcastId (required)
			epgtag.iUniqueBroadcastId = static_cast<unsigned int>(item.starttime);

			// strTitle (required)
			if(item.title == nullptr) return;
			epgtag.strTitle = item.title;

			// iChannelNumber (required)
			epgtag.iChannelNumber = item.channelid;

			// startTime (required)
			epgtag.startTime = item.starttime;

			// endTime (required)
			epgtag.endTime = item.endtime;

			// strPlot
			epgtag.strPlot = item.synopsis;

			// iYear
			epgtag.iYear = item.year;

			// strIconPath
			epgtag.strIconPath = item.iconurl;

			// iGenreType
			epgtag.iGenreType = item.genretype;

			// firstAired
			epgtag.firstAired = item.originalairdate;

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

			g_pvr->TransferEpgEntry(handle, &epgtag);
		});
	}
	
	catch(std::exception& ex) { return handle_stdexception(__func__, ex, PVR_ERROR::PVR_ERROR_FAILED); }
	catch(...) { return handle_generalexception(__func__, PVR_ERROR::PVR_ERROR_FAILED); }

	return PVR_ERROR::PVR_ERROR_NO_ERROR;
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
	std::function<void(sqlite3*, enumerate_channelids_callback)> enumerator = nullptr;
	if(strcmp(group.strGroupName, "Favorite Channels") == 0) enumerator = enumerate_favorite_channelids;
	else if(strcmp(group.strGroupName, "HD Channels") == 0) enumerator = enumerate_hd_channelids;
	else if(strcmp(group.strGroupName, "SD Channels") == 0) enumerator = enumerate_sd_channelids;

	// If neither enumerator was selected, there isn't any work to do here
	if(enumerator == nullptr) return PVR_ERROR::PVR_ERROR_NO_ERROR;

	try {

		// Pull a database connection out from the connection pool
		connectionpool::handle dbhandle(g_connpool);

		// Enumerate all of the channels in the specified group
		enumerator(dbhandle, [&](union channelid const& item) -> void {

			PVR_CHANNEL_GROUP_MEMBER member;						// PVR_CHANNEL_GROUP_MEMORY to send
			memset(&member, 0, sizeof(PVR_CHANNEL_GROUP_MEMBER));	// Initialize the structure

			// strGroupName (required)
			snprintf(member.strGroupName, std::extent<decltype(member.strGroupName)>::value, "%s", group.strGroupName);

			// iChannelUniqueId (required)
			member.iChannelUniqueId = item.value;

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
	try { return get_channel_count(connectionpool::handle(g_connpool)); }
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

	try {

		// Get the prepend_channel_numbers setting
		std::unique_lock<std::mutex> settings_lock(g_settings_lock);
		bool prependnumbers = g_settings.prepend_channel_numbers;
		settings_lock.unlock();

		// Pull a database connection out from the connection pool
		connectionpool::handle dbhandle(g_connpool);

		// Enumerate all of the channels in the database
		enumerate_channels(dbhandle, prependnumbers, [&](struct channel const& item) -> void {

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
			//
			// Only set if 'direct tuning' is disabled
			if(!g_settings.use_direct_tuning) snprintf(channel.strInputFormat, std::extent<decltype(channel.strInputFormat)>::value, "video/MP2T");

			// strStreamURL
			//
			// Only set if 'direct tuning' is enabled, this activates a hack in Kodi that will call GetLiveStreamURL
			// to dynamically retrieve the URL for a live TV stream (may disappear in future Kodi releases)
			if(g_settings.use_direct_tuning) snprintf(channel.strStreamURL, std::extent<decltype(channel.strStreamURL)>::value, "pvr://stream/%u", item.channelid.value);

			// strIconPath
			if(item.iconurl != nullptr) snprintf(channel.strIconPath, std::extent<decltype(channel.strIconPath)>::value, "%s", item.iconurl);

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

	try {

		// Get the use_episode_number_as_title setting
		std::unique_lock<std::mutex> settings_lock(g_settings_lock);
		bool useepisodenumber = g_settings.use_episode_number_as_title;
		settings_lock.unlock();

		// Pull a database connection out from the connection pool
		connectionpool::handle dbhandle(g_connpool);

		// Enumerate all of the recordings in the database
		enumerate_recordings(dbhandle, useepisodenumber, [&](struct recording const& item) -> void {

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

			// strStreamURL (required)
			if(item.streamurl == nullptr) return;
			snprintf(recording.strStreamURL, std::extent<decltype(recording.strStreamURL)>::value, "%s", item.streamurl);

			// strDirectory
			if(item.directory != nullptr) snprintf(recording.strDirectory, std::extent<decltype(recording.strDirectory)>::value, "%s", item.directory);

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

			// iChannelUid
			recording.iChannelUid = item.channelid.value;

			// channelType
			recording.channelType = PVR_RECORDING_CHANNEL_TYPE_TV;

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
//	lastplayedposition	- The last watched position in seconds

PVR_ERROR SetRecordingLastPlayedPosition(PVR_RECORDING const& /*recording*/, int /*lastplayedposition*/)
{
	return PVR_ERROR::PVR_ERROR_NOT_IMPLEMENTED;
}

//---------------------------------------------------------------------------
// GetRecordingLastPlayedPosition
//
// Retrieve the last watched position of a recording (in seconds) on the backend
//
// Arguments:
//
//	recording	- The recording

int GetRecordingLastPlayedPosition(PVR_RECORDING const& /*recording*/)
{
	return -1;
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

			g_pvr->TransferTimerEntry(handle, &timer);
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

			// The seriesid for the recording rule has to be located by name for a series rule
			seriesid = find_seriesid(dbhandle, timer.strEpgSearchString);
			if(seriesid.length() == 0) throw string_exception(std::string("could not locate seriesid for title '") + timer.strEpgSearchString + "'");

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
			if(seriesid.length() == 0) throw string_exception(std::string("could not locate seriesid for title '") + timer.strEpgSearchString + "'");

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

		// Adding a timer may expose new information for the EPG, refresh all affected channels
		enumerate_series_channelids(dbhandle, seriesid.c_str(), [&](union channelid const& channelid) -> void { g_pvr->TriggerEpgUpdate(channelid.value); });
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

		// Updating a timer may expose new information for the EPG, refresh all affected channels
		enumerate_series_channelids(dbhandle, seriesid.c_str(), [&](union channelid const& channelid) -> void { g_pvr->TriggerEpgUpdate(channelid.value); });
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

	// This function is not available when direct tuning is enabled
	if(g_settings.use_direct_tuning) return false;

	// If the user wants to pause discovery during live streaming, do so
	std::unique_lock<std::mutex> settings_lock(g_settings_lock);
	if(g_settings.pause_discovery_while_streaming) g_scheduler.pause();
	settings_lock.unlock();

	// Ensure that any active live stream is closed out and reset first
	g_livestream.stop();

	// The only interesting thing about PVR_CHANNEL is the channel id
	union channelid channelid;
	channelid.value = channel.iUniqueId;

	// Generate a string version of the channel number for logging purposes
	if(channelid.parts.subchannel == 0) snprintf(channelstr, std::extent<decltype(channelstr)>::value, "%d", channelid.parts.channel);
	else snprintf(channelstr, std::extent<decltype(channelstr)>::value, "%d.%d", channelid.parts.channel, channelid.parts.subchannel);

	try {

		// Pull a database connection out from the connection pool
		connectionpool::handle dbhandle(g_connpool);

		// Generate the stream URL for the specified channel
		std::string streamurl = get_stream_url(dbhandle, channelid);
		if(streamurl.length() == 0) throw string_exception("unable to determine the URL for specified channel");

		// Start the new channel live stream
		log_notice(__func__, ": streaming channel ", channelstr, " via url ", streamurl.c_str());
		g_livestream.start(streamurl.c_str());

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
	// Always unpause the scheduler, it won't hurt to do this if it's not paused
	g_scheduler.resume();

	// Close out the live stream if it's active
	try { g_livestream.stop(); }
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
	// This function is not available when direct tuning is enabled
	if(g_settings.use_direct_tuning) return -1;

	try { 

		// Attempt to read up to the specified amount of data from the live stream buffer,
		// waiting a ridiculously long time to avoid a zero-length read over poor connections
		size_t read = g_livestream.read(buffer, size, 5000);

		// Watch for reads that exceeds int::max in length, however unlikely that would be
		if(read > static_cast<size_t>(std::numeric_limits<int>::max())) 
			throw string_exception("livestream.read() returned more data than can be represented by data type int");

		// Report a zero-length read in the log associated with the stream that failed
		if(read == 0) log_error("zero-length read at position ", g_livestream.position());

		return static_cast<int>(read);
	}

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
	// This function is not available when direct tuning is enabled
	if(g_settings.use_direct_tuning) return -1;

	try {

		if(whence == SEEK_SET) { /* use position */ }
		else if(whence == SEEK_CUR) position = g_livestream.position() + position;
		else if(whence == SEEK_END) position = g_livestream.length() + position;

		// Request the live stream seek and return the actual position it ends up at
		return g_livestream.seek(position);
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
	// This function is not available when direct tuning is enabled
	if(g_settings.use_direct_tuning) return -1;

	// Return the current position within the live stream
	try { return g_livestream.position(); }

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
	// This function is not available when direct tuning is enabled
	if(g_settings.use_direct_tuning) return -1;

	// Return the length of the current live stream
	try { return g_livestream.length(); }

	catch(std::exception& ex) { return handle_stdexception(__func__, ex, -1); }
	catch(...) { return handle_generalexception(__func__, -1); }
}

//---------------------------------------------------------------------------
// SwitchChannel
//
// Switch to another channel. Only to be called when a live stream has already been opened
//
// Arguments:
//
//	channel		- The channel to switch to

bool SwitchChannel(PVR_CHANNEL const& channel)
{
	return OpenLiveStream(channel);
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
// GetLiveStreamURL
//
// Get the stream URL for a channel from the backend
//
// Arguments:
//
//	channel		- The channel to get the stream URL for

char const* GetLiveStreamURL(PVR_CHANNEL const& channel)
{
	static std::string		streamurl;			// The generated stream URL (static)
	char					channelstr[64];		// Channel number as a string

	streamurl.clear();			// streamurl is static and reused across invocations

	// This function is only enabled if the addon was started with use_direct_tuning
	if(!g_settings.use_direct_tuning) return "";

	// The only interesting thing about PVR_CHANNEL is the channel id
	union channelid channelid;
	channelid.value = channel.iUniqueId;

	// Generate a string version of the channel number for logging purposes
	if(channelid.parts.subchannel == 0) snprintf(channelstr, std::extent<decltype(channelstr)>::value, "%d", channelid.parts.channel);
	else snprintf(channelstr, std::extent<decltype(channelstr)>::value, "%d.%d", channelid.parts.channel, channelid.parts.subchannel);

	try {
		
		// The available tuners for the channel are captured into a vector<>
		std::vector<std::string> tuners;

		// Pull a database connection out from the connection pool
		connectionpool::handle dbhandle(g_connpool);

		// Create a collection of all the tuners that can possibly stream the requested channel
		enumerate_channeltuners(dbhandle, channelid, [&](char const* item) -> void { tuners.emplace_back(item); });
		if(tuners.size() == 0) throw string_exception("unable to find any possible tuners for channel ", channelstr);
		
		// Select an available tuner from all of the captured possibilities
		std::string selected = select_tuner(tuners);
		
		// NOTE: There is an inherent race condition here; the tuner is intentionally not
		// left locked by select_tuner() as the HTTP stream won't be able to use it, causing
		// the attempt to tune the stream to fail.  I originally left the tuner locked until 
		// right before returning to Kodi, but that wouldn't have solved anything and just 
		// caused an additional (albeit minor) delay in getting the URL generated ...

		// Generate the tuner-specific URL to send back to Kodi to tune the channel
		streamurl = get_tuner_stream_url(dbhandle, selected.c_str(), channelid);
		if(streamurl.length() == 0) throw string_exception("unable to generate streaming URL for channel ", channelstr);

		log_notice(__func__, ": streaming channel ", channelstr, " via url ", streamurl.c_str());
		return streamurl.c_str();
	}

	catch(std::exception& ex) { return handle_stdexception(__func__, ex, ""); }
	catch(...) { return handle_generalexception(__func__, ""); }
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

bool OpenRecordedStream(PVR_RECORDING const& /*recording*/)
{
	return false;
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

int ReadRecordedStream(unsigned char* /*buffer*/, unsigned int /*size*/)
{
	return -1;
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

long long SeekRecordedStream(long long /*position*/, int /*whence*/)
{
	return -1;
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
	return -1;
}

//---------------------------------------------------------------------------
// PositionRecordedStream
//
// Gets the total length of the stream that's currently being read
//
// Arguments:
//
//	NONE

long long LengthRecordedStream(void)
{
	return -1;
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
// GetChannelSwitchDelay
//
// Gets delay to use when using switching channels for add-ons not providing an input stream
//
// Arguments:
//
//	NONE

unsigned int GetChannelSwitchDelay(void)
{
	return 0;
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
	// NOTE: This technically doesn't work right with 'direct tune' enabled, but
	// returning false here prevents pause from being enabled.  The user that
	// direct tuning was added for preferred broken seek to not having pause
	return true;
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
	return true;
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

		g_livestream.stop();				// Stop live stream if necessary

		g_scheduler.stop();					// Stop the scheduler
		g_scheduler.clear();				// Clear out any pending tasks
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
	try {

		// Reschedule all discoveries to execute sequentially to update everything
		std::chrono::time_point<std::chrono::system_clock> now = std::chrono::system_clock::now();
		g_scheduler.add(now + std::chrono::seconds(1), discover_devices_task);
		g_scheduler.add(now + std::chrono::seconds(2), discover_lineups_task);
		g_scheduler.add(now + std::chrono::seconds(3), discover_guide_task);
		g_scheduler.add(now + std::chrono::seconds(4), discover_recordings_task);
		g_scheduler.add(now + std::chrono::seconds(5), discover_recordingrules_task);
		g_scheduler.add(now + std::chrono::seconds(6), discover_episodes_task);
	
		// Restart the scheduler
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

#pragma warning(pop)
