//-----------------------------------------------------------------------------
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
//-----------------------------------------------------------------------------

#ifndef __ADDON_H_
#define __ADDON_H_
#pragma once

#include <atomic>
#include <deque>
#include <kodi/addon-instance/PVR.h>
#include <mutex>
#include <random>
#include <string>
#include <vector>

#include "database.h"
#include "pvrstream.h"
#include "pvrtypes.h"
#include "scalar_condition.h"
#include "scheduler.h"

#pragma warning(push, 4)

//---------------------------------------------------------------------------
// Class addon
//
// Implements the PVR addon instance

class ATTRIBUTE_HIDDEN addon : public kodi::addon::CAddonBase, public kodi::addon::CInstancePVRClient
{
public:

	// Instance Constructor
	//
	addon();

	// Destructor
	//
	virtual ~addon() override;

	//-------------------------------------------------------------------------
	// CAddonBase
	//-------------------------------------------------------------------------

	// Create
	//
	// Initializes a new addon class instance
	ADDON_STATUS Create(void) override;

	// SetSetting
	//
	// Notifies the addon that a setting has been changed
	ADDON_STATUS SetSetting(std::string const& settingName, kodi::CSettingValue const& settingValue) override;

	//-------------------------------------------------------------------------
	// CInstancePVRClient
	//-------------------------------------------------------------------------

	// AddTimer
	//
	// Add a timer on the backend
	PVR_ERROR AddTimer(kodi::addon::PVRTimer const& timer) override;
		
	// CallChannelMenuHook
	//
	// Call one of the channel related menu hooks
	PVR_ERROR CallChannelMenuHook(kodi::addon::PVRMenuhook const& menuhook, kodi::addon::PVRChannel const& item) override;
		
	// CallRecordingMenuHook
	//
	// Call one of the recording related menu hooks
	PVR_ERROR CallRecordingMenuHook(kodi::addon::PVRMenuhook const& menuhook, kodi::addon::PVRRecording const & item) override;

	// CallSettingsMenuHook
	//
	// Call one of the settings related menu hooks
	PVR_ERROR CallSettingsMenuHook(kodi::addon::PVRMenuhook const& menuhook) override;

	// CanPauseStream
	//
	// Check if the backend support pausing the currently playing stream
	bool CanPauseStream(void) override;

	// CanSeekStream
	//
	// Check if the backend supports seeking for the currently playing stream
	bool CanSeekStream(void) override;

	// CloseLiveStream
	//
	// Close an open live stream
	void CloseLiveStream(void) override;

	// CloseRecordedStream
	//
	// Close an open stream from a recording
	void CloseRecordedStream(void) override;

	// DeleteRecording
	//
	// Delete a recording on the backend
	PVR_ERROR DeleteRecording(kodi::addon::PVRRecording const& recording) override;
	
	// DeleteTimer
	//
	// Delete a timer on the backend
	PVR_ERROR DeleteTimer(kodi::addon::PVRTimer const& timer, bool forceDelete) override;
		
	// GetBackendHostname
	//
	// Get the hostname of the pvr backend server
	PVR_ERROR GetBackendHostname(std::string& hostname) override;
	
	// GetBackendName
	//
	// Get the name reported by the backend that will be displayed in the UI
	PVR_ERROR GetBackendName(std::string& name) override;

	// GetBackendVersion
	//
	// Get the version string reported by the backend that will be displayed in the UI
	PVR_ERROR GetBackendVersion(std::string& version) override;

	// GetCapabilities
	//
	// Get the list of features that this add-on provides
	PVR_ERROR GetCapabilities(kodi::addon::PVRCapabilities& capabilities) override;

	// GetChannelGroupsAmount
	//
	// Get the total amount of channel groups on the backend
	PVR_ERROR GetChannelGroupsAmount(int& amount) override;

	// GetChannelGroupMembers
	//
	// Request the list of all group members of a group from the backend
	PVR_ERROR GetChannelGroupMembers(kodi::addon::PVRChannelGroup const& group, kodi::addon::PVRChannelGroupMembersResultSet& results) override;
	
	// GetChannelGroups
	//
	// Request the list of all channel groups from the backend
	PVR_ERROR GetChannelGroups(bool radio, kodi::addon::PVRChannelGroupsResultSet& results) override;
		
	// GetChannels
	//
	// Request the list of all channels from the backend
	PVR_ERROR GetChannels(bool radio, kodi::addon::PVRChannelsResultSet& results) override;
		
	// GetChannelsAmount
	//
	// Gets the total amount of channels on the backend
	PVR_ERROR GetChannelsAmount(int& amount) override;

	// GetChannelStreamProperties
	//
	// Get the stream properties for a channel from the backend
	PVR_ERROR GetChannelStreamProperties(kodi::addon::PVRChannel const& channel, std::vector<kodi::addon::PVRStreamProperty>& properties) override;
		
	// GetDriveSpace
	//
	// Gets the disk space reported by the backend
	PVR_ERROR GetDriveSpace(uint64_t& total, uint64_t& used) override;

	// GetEPGForChannel
	//
	// Request the EPG for a channel from the backend
	PVR_ERROR GetEPGForChannel(int channelUid, time_t start, time_t end, kodi::addon::PVREPGTagsResultSet& results) override;
		
	// GetRecordingEdl
	//
	// Retrieve the edit decision list (EDL) of a recording on the backend
	PVR_ERROR GetRecordingEdl(kodi::addon::PVRRecording const& recording, std::vector<kodi::addon::PVREDLEntry>& edl) override;
		
	// GetRecordingLastPlayedPosition
	//
	// Retrieve the last watched position of a recording on the backend
	PVR_ERROR GetRecordingLastPlayedPosition(const kodi::addon::PVRRecording& recording, int& position) override;

	// GetRecordings
	//
	// Request the list of all recordings from the backend
	PVR_ERROR GetRecordings(bool deleted, kodi::addon::PVRRecordingsResultSet& results) override;
		
	// GetRecordingsAmount
	//
	// Gets the amount of recordings present on backend
	PVR_ERROR GetRecordingsAmount(bool deleted, int& amount) override;
		
	// GetRecordingStreamProperties
	//
	// Get the stream properties for a recording from the backend
	PVR_ERROR GetRecordingStreamProperties(kodi::addon::PVRRecording const& recording, std::vector<kodi::addon::PVRStreamProperty>& properties) override;
		
	// GetStreamReadChunkSize
	//
	// Obtain the chunk size to use when reading streams
	PVR_ERROR GetStreamReadChunkSize(int& chunksize) override;

	// GetStreamTimes
	//
	// Get stream times
	PVR_ERROR GetStreamTimes(kodi::addon::PVRStreamTimes& times) override;
		
	// GetTimers
	//
	// Request the list of all timers from the backend
	PVR_ERROR GetTimers(kodi::addon::PVRTimersResultSet& results) override;
		
	// GetTimersAmount
	//
	// Gets the total amount of timers on the backend
	PVR_ERROR GetTimersAmount(int& amount) override;

	// GetTimerTypes
	//
	// Retrieve the timer types supported by the backend
	PVR_ERROR GetTimerTypes(std::vector<kodi::addon::PVRTimerType>& types) override;

	// IsRealTimeStream
	//
	// Check for real-time streaming
	bool IsRealTimeStream(void) override;

	// LengthLiveStream
	//
	// Obtain the length of a live stream
	int64_t LengthLiveStream(void) override;

	// LengthRecordedStream
	//
	// Obtain the length of a recorded stream
	int64_t LengthRecordedStream(void) override;

	// OnSystemSleep
	//
	// Notification of system sleep power event
	PVR_ERROR OnSystemSleep(void) override;

	// OnSystemWake
	//
	// Notification of system wake power event
	PVR_ERROR OnSystemWake(void) override;

	// OpenLiveStream
	//
	// Open a live stream on the backend
	bool OpenLiveStream(kodi::addon::PVRChannel const& channel) override;
		
	// OpenRecordedStream
	//
	// Open a stream to a recording on the backend
	bool OpenRecordedStream(kodi::addon::PVRRecording const& recording) override;
		
	// ReadLiveStream
	//
	// Read from an open live stream
	int ReadLiveStream(unsigned char* buffer, unsigned int size) override;

	// ReadRecordedStream
	//
	// Read from a recording
	int ReadRecordedStream(unsigned char* buffer, unsigned int size) override;

	// SeekLiveStream
	//
	// Seek in a live stream on a backend that supports timeshifting
	int64_t SeekLiveStream(int64_t position, int whence) override;

	// SeekRecordedStream
	//
	// Seek in a recorded stream
	int64_t SeekRecordedStream(int64_t position, int whence) override;

	// SetEPGMaxFutureDays
	//
	// Tell the client the future time frame to use when notifying epg events back to Kodi
	PVR_ERROR SetEPGMaxFutureDays(int futureDays) override;

	// SetEPGMaxPastDays
	//
	// Tell the client the past time frame to use when notifying epg events back to Kodi
	PVR_ERROR SetEPGMaxPastDays(int pastDays) override;

	// SetRecordingLastPlayedPosition
	//
	// Set the last watched position of a recording on the backend
	PVR_ERROR SetRecordingLastPlayedPosition(kodi::addon::PVRRecording const& recording, int lastplayedposition) override;

	// UpdateTimer
	//
	// Update the timer information on the backend
	PVR_ERROR UpdateTimer(kodi::addon::PVRTimer const& timer) override;

private:

	addon(addon const&)=delete;
	addon& operator=(addon const&)=delete;

	//-------------------------------------------------------------------------
	// Private Member Functions

	// Destroy
	//
	// Uninitializes/unloads the addon instance
	void Destroy(void) noexcept;

	// Discovery Helpers
	//
	void discover_devices(scalar_condition<bool> const& cancel, bool& changed);
	void discover_episodes(scalar_condition<bool> const& cancel, bool& changed);
	void discover_lineups(scalar_condition<bool> const& cancel, bool& changed);
	void discover_listings(scalar_condition<bool> const& cancel, bool& changed);
	void discover_recordingrules(scalar_condition<bool> const& cancel, bool& changed);
	void discover_recordings(scalar_condition<bool> const& cancel, bool& changed);
	void start_discovery(void) noexcept;
	void wait_for_devices(void) noexcept;
	void wait_for_channels(void) noexcept;
	void wait_for_recordings(void) noexcept;
	void wait_for_timers(void) noexcept;

	// Exception Helpers
	//
	void handle_generalexception(char const* function);
	template<typename _result> _result handle_generalexception(char const* function, _result result);
	void handle_stdexception(char const* function, std::exception const& ex);
	template<typename _result> _result handle_stdexception(char const* function, std::exception const& ex, _result result);

	// Log Helpers
	//
	template<typename... _args> void log_debug(_args&&... args);
	template<typename... _args> void log_error(_args&&... args);
	template<typename... _args> void log_info(_args&&... args);
	template<typename... _args> void log_message(AddonLog level, _args&&... args);
	template<typename... _args> void log_warning(_args&&... args);

	// Network Helpers
	//
	bool ipv4_network_available(void);
	std::string select_tuner(std::vector<std::string> const& possibilities);

	// Scheduled Tasks
	//
	void startup_alerts_task(scalar_condition<bool> const& cancel);
	void update_devices_task(scalar_condition<bool> const& cancel);
	void update_episodes_task(scalar_condition<bool> const& cancel);
	void update_lineups_task(scalar_condition<bool> const& cancel);
	void update_listings_task(bool force, bool checkchannels, scalar_condition<bool> const& cancel);
	void update_recordingrules_task(scalar_condition<bool> const& cancel);
	void update_recordings_task(scalar_condition<bool> const& cancel);
	void wait_for_network_task(int seconds, scalar_condition<bool> const& cancel);

	// Scheduled Task Names
	//
	static char const* UPDATE_DEVICES_TASK;
	static char const* UPDATE_EPISODES_TASK;
	static char const* UPDATE_LINEUPS_TASK;
	static char const* UPDATE_LISTINGS_TASK;
	static char const* UPDATE_RECORDINGRULES_TASK;
	static char const* UPDATE_RECORDINGS_TASK;

	// Settings Helpers
	//
	struct settings copy_settings(void) const;

	// Stream Helpers
	//
	std::unique_ptr<pvrstream> openlivestream_storage_http(connectionpool::handle const& dbhandle,
		struct settings const& settings, union channelid channelid, char const* vchannel);
	std::unique_ptr<pvrstream> openlivestream_tuner_device(connectionpool::handle const& dbhandle,
		struct settings const& settings, union channelid channelid, char const* vchannel);
	std::unique_ptr<pvrstream> openlivestream_tuner_http(connectionpool::handle const& dbhandle,
		struct settings const& settings, union channelid channelid, char const* vchannel);

	//-------------------------------------------------------------------------
	// Member Variables

	std::shared_ptr<connectionpool>	m_connpool;						// Database connection pool
	scalar_condition<bool>			m_discovered_devices;			// Discovery flag
	scalar_condition<bool>			m_discovered_episodes;			// Discovery flag
	scalar_condition<bool>			m_discovered_lineups;			// Discovery flag
	scalar_condition<bool>			m_discovered_listings;			// Discovery flag
	scalar_condition<bool>			m_discovered_recordingrules;	// Discovery flag
	scalar_condition<bool>			m_discovered_recordings;		// Discovery flag
	std::once_flag					m_discovery_started;			// Discovery started flag
	std::atomic<int>				m_epgmaxtime;					// Maximum EPG time frame
	std::deque<std::string>			m_errorlog;						// Recent error log
	std::mutex						m_errorlog_lock;				// Synchronization object
	std::unique_ptr<pvrstream>		m_pvrstream;					// Active PVR stream instance
	std::default_random_engine		m_randomengine;					// Pseudo-random number generator 
	scheduler						m_scheduler;					// Background task scheduler
	struct settings					m_settings;						// Custom addon settings
	mutable std::mutex				m_settings_lock;				// Synchronization object
	time_t							m_stream_starttime;				// Current stream start time
	time_t							m_stream_endtime;				// Current stream end time
};

//-----------------------------------------------------------------------------

#pragma warning(pop)

#endif	// __ADDON_H_
