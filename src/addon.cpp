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

#include "addon.h"

#include <assert.h>
#include <chrono>
#include <functional>
#include <kodi/Filesystem.h>
#include <kodi/General.h>
#include <kodi/gui/dialogs/OK.h>
#include <kodi/gui/dialogs/FileBrowser.h>
#include <kodi/gui/dialogs/Select.h>
#include <kodi/gui/dialogs/TextViewer.h>
#include <sstream>
#include <version.h>

#ifdef __ANDROID__
#include <android/log.h>
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

#include "database.h"
#include "devicestream.h"
#include "httpstream.h"
#include "radiofilter.h"
#include "string_exception.h"
#include "sqlite_exception.h"

#pragma warning(push, 4)

// Addon Entry Points 
//
ADDONCREATOR(addon)

// Scheduled Task Names
//
char const* addon::EPG_TIMEFRAME_CHANGED_TASK	= "epg_timeframe_changed_task";
char const* addon::UPDATE_DEVICES_TASK			= "update_devices_task";
char const* addon::UPDATE_EPISODES_TASK			= "update_episodes_task";
char const* addon::UPDATE_LINEUPS_TASK			= "update_lineups_task";
char const* addon::UPDATE_LISTINGS_TASK			= "update_listings_task";
char const* addon::UPDATE_RECORDINGRULES_TASK	= "update_recordingrules_task";
char const* addon::UPDATE_RECORDINGS_TASK		= "update_recordings_task";

//---------------------------------------------------------------------------
// addon Instance Constructor
//
// Arguments:
//
//	NONE

addon::addon() : m_discovered_devices{ false }, 
	m_discovered_episodes{ false }, 
	m_discovered_lineups{ false },
	m_discovered_listings{ false }, 
	m_discovered_recordingrules{ false }, 
	m_discovered_recordings{ false },
	m_epgmaxtime{ EPG_TIMEFRAME_UNLIMITED }, 
	m_randomengine(static_cast<unsigned int>(time(nullptr))),
	m_scheduler([&](std::exception const& ex) -> void { handle_stdexception("scheduled task", ex); }),
	m_settings{},
	m_startup_complete{ false },
	m_stream_starttime(0), 
	m_stream_endtime(0) {}

//---------------------------------------------------------------------------
// addon Destructor

addon::~addon()
{
	// There is no corresponding "Destroy" method in CAddonBase, only the class
	// destructor will be invoked; to keep the implementation pieces near each
	// other, perform the tear-down in a helper function
	Destroy();
}

//---------------------------------------------------------------------------
// addon::copy_settings (private, inline)
//
// Atomically creates a copy of the member addon_settings structure
//
// Arguments:
//
//	NONE

inline struct settings addon::copy_settings(void) const
{
	std::unique_lock<std::mutex> settings_lock(m_settings_lock);
	return m_settings;
}

//---------------------------------------------------------------------------
// addon::discover_devices (private)
//
// Helper function used to execute a backend device discovery operation
//
// Arguments:
//
//	cancel		- Condition variable used to cancel the operation
//	changed		- Reference to a flag indicating that the discovery information has changed

void addon::discover_devices(scalar_condition<bool> const& /*cancel*/, bool& changed)
{
	changed = false;						// Initialize [ref] argument

	// Only produce trace-level logging if the addon is starting up or the data has changed
	bool const trace = (m_startup_complete.load() == false);

	// Create a copy of the current addon settings structure
	struct settings settings = copy_settings();

	log_info_if(trace, __func__, ": initiated local network device discovery (method: ", settings.use_http_device_discovery ? "http" : "broadcast", ")");

	try {

		// Pull a database connection out from the connection pool
		connectionpool::handle dbhandle(m_connpool);

		// Clear any invalid device authorization strings present in the existing discovery data
		clear_authorization_strings(dbhandle, settings.deviceauth_stale_after);

		// Discover the devices on the local network and check for changes
		auto caller = __func__;
		::discover_devices(dbhandle, settings.use_http_device_discovery, changed);

		// Log the device information if starting up or changes were detected
		if(trace || changed) {

			enumerate_device_names(dbhandle, [&](struct device_name const& device_name) -> void { log_info(caller, ": discovered: ", device_name.name); });
			log_warning_if(!has_storage_engine(dbhandle), __func__, ": no storage engine devices were discovered; recording discovery is disabled");
			log_warning_if(!has_dvr_authorization(dbhandle), __func__, ": no tuners with a valid DVR authorization were discovered; recording rule and electronic program guide discovery are disabled");
		}

		// Set the discovery time for the device information
		set_discovered(dbhandle, "devices", time(nullptr));

		m_discovered_devices = true;			// Set the scalar_condition flag
	}

	// Set the scalar_condition on exception before re-throwing it
	catch(...) { m_discovered_devices = true; throw; }
}

//---------------------------------------------------------------------------
// addon::discover_episodes (private)
//
// Helper function used to execute a backend recording rule episode discovery operation
//
// Arguments:
//
//	cancel		- Condition variable used to cancel the operation
//	changed		- Reference to a flag indicating that the discovery information has changed

void addon::discover_episodes(scalar_condition<bool> const&, bool& changed)
{
	changed = false;						// Initialize [ref] argument

	// Only produce trace-level logging if the addon is starting up or the data has changed
	bool const trace = (m_startup_complete.load() == false);

	log_info_if(trace, __func__, ": initiated recording rule episode discovery");

	try {

		// Pull a database connection out from the connection pool
		connectionpool::handle dbhandle(m_connpool);

		// This operation is only available when there is at least one DVR authorized tuner
		std::string authorization = get_authorization_strings(dbhandle, true);

		// Discover the recording rule episode information associated with all of the authorized devices
		if(authorization.length() != 0) ::discover_episodes(dbhandle, authorization.c_str(), changed);
		else log_info_if(trace, __func__, ": no tuners with valid DVR authorization were discovered; skipping recording rule episode discovery");

		// Set the discovery time for the episode information
		set_discovered(dbhandle, "episodes", time(nullptr));

		m_discovered_episodes = true;			// Set the scalar_condition flag
	}

	// Set the scalar_condition on exception before re-throwing it
	catch(...) { m_discovered_episodes = true; throw; }
}

//---------------------------------------------------------------------------
// addon::discover_lineups (private)
//
// Helper function used to execute a backend channel lineup discovery operation
//
// Arguments:
//
//	cancel		- Condition variable used to cancel the operation
//	changed		- Reference to a flag indicating that the discovery information has changed

void addon::discover_lineups(scalar_condition<bool> const&, bool& changed)
{
	changed = false;						// Initialize [ref] argument

	// Only produce trace-level logging if the addon is starting up or the data has changed
	bool const trace = (m_startup_complete.load() == false);

	log_info_if(trace, __func__, ": initiated local tuner device lineup discovery");

	try {

		// Pull a database connection out from the connection pool
		connectionpool::handle dbhandle(m_connpool);

		// Execute the channel lineup discovery operation
		::discover_lineups(dbhandle, changed);

		// Set the discovery time for the lineup information
		set_discovered(dbhandle, "lineups", time(nullptr));

		m_discovered_lineups = true;			// Set the scalar_condition flag
	}

	// Set the scalar_condition on exception before re-throwing it
	catch(...) { m_discovered_lineups = true; throw; }
}

//---------------------------------------------------------------------------
// addon::discover_listings (private)
//
// Helper function used to execute a backend listing discovery operation
//
// Arguments:
//
//	cancel		- Condition variable used to cancel the operation
//	changed		- Reference to a flag indicating that the discovery information has changed

void addon::discover_listings(scalar_condition<bool> const&, bool& changed)
{
	changed = true;						// Initialize [ref] argument

	// Create a copy of the current addon settings structure
	struct settings settings = copy_settings();

	// Only produce trace-level logging if the addon is starting up or the data has changed
	bool const trace = (m_startup_complete.load() == false);

	log_info_if(trace, __func__, ": initiated listing discovery");

	try {

		// Pull a database connection out from the connection pool
		connectionpool::handle dbhandle(m_connpool);

		// This operation is only available when there is at least one DVR authorized tuner, but
		// lineup data for any unauthorized tuner(s) can also be retrieved
		if(has_dvr_authorization(dbhandle)) {

			std::string authorization = get_authorization_strings(dbhandle, false);
			if(authorization.length() != 0) ::discover_listings(dbhandle, authorization.c_str(), changed);
		}

		else log_info_if(trace, __func__, ": no tuners with valid DVR authorization were discovered; skipping listing discovery");

		// Set the discovery time for the listing information
		set_discovered(dbhandle, "listings", time(nullptr));

		m_discovered_listings = true;			// Set the scalar_condition flag
	}

	// Set the scalar_condition on exception before re-throwing it
	catch(...) { m_discovered_listings = true; throw; }
}

//---------------------------------------------------------------------------
// addon::discover_mappings (private)
//
// Helper function used to execute a channel mapping discovery operation
//
// Arguments:
//
//	cancel		- Condition variable used to cancel the operation
//	changed		- Reference to a flag indicating that the discovery information has changed

void addon::discover_mappings(scalar_condition<bool> const&, bool& changed)
{
	channelranges_t		cable_mappings;			// Discovered radio mappings
	channelranges_t		ota_mappings;			// Discovered radio mappings

	changed = false;							// Initialize [ref] argument

	// Create a copy of the current addon settings structure
	struct settings settings = copy_settings();

	// Only produce trace-level logging if the addon is starting up or the data has changed
	bool const trace = (m_startup_complete.load() == false);

	log_info_if(trace, __func__, ": initiated channel mapping discovery");
	
	// Ignore the operation if the specified file doesn't exist
	if(kodi::vfs::FileExists(settings.radio_channel_mapping_file)) {

		kodi::vfs::CFile mappingfile;
		if(mappingfile.OpenFile(settings.radio_channel_mapping_file)) {

			log_info(__func__, ": processing channel mapping file: ", settings.radio_channel_mapping_file);

			std::string line;								// Current line of text from the file
			int linenumber = 0;								// Current line number within the file

			while(mappingfile.ReadLine(line)) {

				++linenumber;								// Increment the line number

				union channelid start = {};					// Starting channel identifier
				union channelid end = {};					// Ending channel identifier
				unsigned int start_channel = 0;				// Starting channel number
				unsigned int start_subchannel = 0;			// Starting subchannel number
				unsigned int end_channel = 0;				// Ending channel number
				unsigned int end_subchannel = 0;			// Ending subchannel number

				// Scan the line using the OTA range format first
				int result = sscanf(line.c_str(), "%u.%u-%u.%u", &start_channel, &start_subchannel, &end_channel, &end_subchannel);
				
				// OTA RANGE: CHANNEL.SUBCHANNEL-CHANNEL.SUBCHANNEL
				if(result == 4) {

					start.parts.channel = start_channel;
					start.parts.subchannel = start_subchannel;
					end.parts.channel = end_channel;
					end.parts.subchannel = end_channel;
					ota_mappings.push_back({ start, end });
				}

				// OTA: CHANNEL.SUBCHANNEL
				else if(result == 2) {

					start.parts.channel = start_channel;
					start.parts.subchannel = start_subchannel;
					ota_mappings.push_back({ start, start });
				}

				// CABLE
				else if(result == 1) {

					// Rescan the line to detect if this is a single channel or a range
					result = sscanf(line.c_str(), "%u-%u", &start_channel, &end_channel);

					// CABLE RANGE: CHANNEL-CHANNEL
					if(result == 2) {

						start.parts.channel = start_channel;
						end.parts.channel = end_channel;
						cable_mappings.push_back({ start, end });
					}

					// CABLE: CHANNEL
					else if(result == 1) {

						start.parts.channel = start_channel;
						cable_mappings.push_back({ start, start });
					}
				}

				// If the line failed to parse log an error with the line number for the user
				else log_error(__func__, ": invalid channel mapping entry detected at line #", linenumber);
			}

			mappingfile.Close();
		}

		else log_error(__func__, ": unable to open channel mapping file: ", settings.radio_channel_mapping_file);
	}

	std::unique_lock<std::mutex> lock(m_radiomappings_lock);

	// Check each of the new channel mapping ranges against the existing ones and swap them if different

	bool cable_changed = ((cable_mappings.size() != m_radiomappings_cable.size()) || (!std::equal(cable_mappings.begin(), cable_mappings.end(), m_radiomappings_cable.begin(),
		[](channelrange_t const& lhs, channelrange_t const& rhs) { return lhs.first.value == rhs.first.value && lhs.second.value == rhs.second.value; })));
	if(cable_changed) m_radiomappings_cable.swap(cable_mappings);

	bool ota_changed = ((ota_mappings.size() != m_radiomappings_ota.size()) || (!std::equal(ota_mappings.begin(), ota_mappings.end(), m_radiomappings_ota.begin(),
		[](channelrange_t const& lhs, channelrange_t const& rhs) { return lhs.first.value == rhs.first.value && lhs.second.value == rhs.second.value; })));
	if(ota_changed) m_radiomappings_ota.swap(ota_mappings);

	// Set the changed flag for the caller if either set of channel mappings changed
	changed = (cable_changed || ota_changed);
}

//---------------------------------------------------------------------------
// addon::discover_recordingrules (private)
//
// Helper function used to execute a backend recording rule discovery operation
//
// Arguments:
//
//	cancel		- Condition variable used to cancel the operation
//	changed		- Reference to a flag indicating that the discovery information has changed

void addon::discover_recordingrules(scalar_condition<bool> const&, bool& changed)
{
	changed = false;						// Initialize [ref] argument

	// Create a copy of the current addon settings structure
	struct settings settings = copy_settings();

	// Only produce trace-level logging if the addon is starting up or the data has changed
	bool const trace = (m_startup_complete.load() == false);

	log_info_if(trace, __func__, ": initiated recording rule discovery");

	try {

		// Pull a database connection out from the connection pool
		connectionpool::handle dbhandle(m_connpool);

		// This operation is only available when there is at least one DVR authorized tuner
		std::string authorization = get_authorization_strings(dbhandle, true);
		if(authorization.length() != 0) {

			// Discover the recording rules associated with all authorized devices
			::discover_recordingrules(dbhandle, authorization.c_str(), changed);

			// Delete all expired recording rules from the backend as part of the discovery operation
			enumerate_expired_recordingruleids(dbhandle, settings.delete_datetime_rules_after, [&](unsigned int const& recordingruleid) -> void
			{
				try { delete_recordingrule(dbhandle, authorization.c_str(), recordingruleid); changed = true; }
				catch(std::exception& ex) { handle_stdexception(__func__, ex); }
				catch(...) { handle_generalexception(__func__); }
			});

		}

		else log_info_if(trace, __func__, ": no tuners with valid DVR authorization were discovered; skipping recording rule discovery");

		// Set the discovery time for the recordingrule information
		set_discovered(dbhandle, "recordingrules", time(nullptr));

		m_discovered_recordingrules = true;		// Set the scalar_condition flag
	}

	// Set the scalar_condition on exception before re-throwing it
	catch(...) { m_discovered_recordingrules = true; throw; }
}

//---------------------------------------------------------------------------
// addon::discover_recordings (private)
//
// Helper function used to execute a backend recordings operation
//
// Arguments:
//
//	cancel		- Condition variable used to cancel the operation
//	changed		- Reference to a flag indicating that the discovery information has changed

void addon::discover_recordings(scalar_condition<bool> const&, bool& changed)
{
	changed = false;						// Initialize [ref] argument

	// Only produce trace-level logging if the addon is starting up or the data has changed
	bool const trace = (m_startup_complete.load() == false);

	log_info_if(trace, __func__, ": initiated local storage device recording discovery");

	try {

		// Pull a database connection out from the connection pool
		connectionpool::handle dbhandle(m_connpool);

		// Execute the recording discovery operation
		::discover_recordings(dbhandle, changed);

		// Set the discovery time for the recording information
		set_discovered(dbhandle, "recordings", time(nullptr));

		m_discovered_recordings = true;			// Set the scalar_codition flag
	}

	// Set the scalar_condition on exception before re-throwing it
	catch(...) { m_discovered_recordings = true; throw; }
}

//-----------------------------------------------------------------------------
// addon::epg_timeframe_changed_task (private)
//
// Scheduled task implementation to deal with an EPG timeframe change
//
// Arguments:
//
//	cancel		- Condition variable used to cancel the operation

void addon::epg_timeframe_changed_task(scalar_condition<bool> const& cancel)
{
	try {
	
		// Push all of the listing information available in the database based
		// on the updated timeframe over to Kodi
		push_listings(cancel);

		// A change in the EPG timeframe will also require an update to the timers
		// since they are filtered by that timeframe; trigger an update
		log_info(__func__, ": trigger timer update");
		TriggerTimerUpdate();
	}

	catch(std::exception& ex) { handle_stdexception(__func__, ex); }
	catch(...) { handle_generalexception(__func__); }
}

//---------------------------------------------------------------------------
// addon::handle_generalexception (private)
//
// Handler for thrown generic exceptions
//
// Arguments:
//
//	function		- Name of the function where the exception was thrown

void addon::handle_generalexception(char const* function) const
{
	log_error(function, " failed due to an exception");
}

//---------------------------------------------------------------------------
// addon::handle_generalexception (private)
//
// Handler for thrown generic exceptions
//
// Arguments:
//
//	function		- Name of the function where the exception was thrown
//	result			- Result code to return

template<typename _result>
_result addon::handle_generalexception(char const* function, _result result) const
{
	handle_generalexception(function);
	return result;
}

//---------------------------------------------------------------------------
// addon::handle_stdexception (private)
//
// Handler for thrown std::exceptions
//
// Arguments:
//
//	function		- Name of the function where the exception was thrown
//	exception		- std::exception that was thrown

void addon::handle_stdexception(char const* function, std::exception const& ex) const
{
	log_error(function, " failed due to an exception: ", ex.what());
}

//---------------------------------------------------------------------------
// addon::handle_stdexception (private)
//
// Handler for thrown std::exceptions
//
// Arguments:
//
//	function		- Name of the function where the exception was thrown
//	exception		- std::exception that was thrown
//	result			- Result code to return

template<typename _result>
_result addon::handle_stdexception(char const* function, std::exception const& ex, _result result) const
{
	handle_stdexception(function, ex);
	return result;
}

//---------------------------------------------------------------------------
// addon::ipv4_network_available (private)
//
// Determines if IPv4 connectivity has been established on the system
//
// Arguments:
//
//	NONE

bool addon::ipv4_network_available(void) const
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

//---------------------------------------------------------------------------
// addon::is_channel_radio (private)
//
// Determines if a channel has been mapped as a radio channel
//
// Arguments:
//
//	lock		- Held lock instance
//	channelid	- Channel identifier to check against the mappings

bool addon::is_channel_radio(std::unique_lock<std::mutex> const& lock, union channelid const& channelid) const
{
	assert(lock.owns_lock());
	if(!lock.owns_lock()) throw std::invalid_argument("lock");

	// CABLE
	if(channelid.parts.subchannel == 0) return std::any_of(m_radiomappings_cable.cbegin(), m_radiomappings_cable.cend(),
		[&](channelrange_t const& range) { return ((channelid.value >= range.first.value) && (channelid.value <= range.second.value)); });

	// OTA
	else return std::any_of(m_radiomappings_ota.cbegin(), m_radiomappings_ota.cend(),
		[&](channelrange_t const& range) { return ((channelid.value >= range.first.value) && (channelid.value <= range.second.value)); });
}

//---------------------------------------------------------------------------
// addon::log_debug (private)
//
// Variadic method of writing a LOG_DEBUG entry into the Kodi application log
//
// Arguments:
//
//	args	- Variadic argument list

template<typename... _args>
void addon::log_debug(_args&&... args) const
{
	log_message(AddonLog::ADDON_LOG_DEBUG, std::forward<_args>(args)...);
}

//---------------------------------------------------------------------------
// addon::log_debug_if (private)
//
// Variadic method of writing a LOG_DEBUG entry into the Kodi application log
//
// Arguments:
//
//	flag	- Flag indicating if the information should be logged
//	args	- Variadic argument list

template<typename... _args>
void addon::log_debug_if(bool flag, _args&&... args) const
{
	if(flag) log_message(AddonLog::ADDON_LOG_DEBUG, std::forward<_args>(args)...);
}

//---------------------------------------------------------------------------
// addon::log_error (private)
//
// Variadic method of writing a LOG_ERROR entry into the Kodi application log
//
// Arguments:
//
//	args	- Variadic argument list

template<typename... _args>
void addon::log_error(_args&&... args) const
{
	log_message(AddonLog::ADDON_LOG_ERROR, std::forward<_args>(args)...);
}

//---------------------------------------------------------------------------
// addon::log_error_if (private)
//
// Variadic method of writing a LOG_ERROR entry into the Kodi application log
//
// Arguments:
//
//	flag	- Flag indicating if the information should be logged
//	args	- Variadic argument list

template<typename... _args>
void addon::log_error_if(bool flag, _args&&... args) const
{
	if(flag) log_message(AddonLog::ADDON_LOG_ERROR, std::forward<_args>(args)...);
}

//---------------------------------------------------------------------------
// addon::log_info (private)
//
// Variadic method of writing a LOG_INFO entry into the Kodi application log
//
// Arguments:
//
//	args	- Variadic argument list

template<typename... _args>
void addon::log_info(_args&&... args) const
{
	log_message(AddonLog::ADDON_LOG_INFO, std::forward<_args>(args)...);
}

//---------------------------------------------------------------------------
// addon::log_info_if (private)
//
// Variadic method of writing a LOG_INFO entry into the Kodi application log
//
// Arguments:
//
//	flag	- Flag indicating if the information should be logged
//	args	- Variadic argument list

template<typename... _args>
void addon::log_info_if(bool flag, _args&&... args) const
{
	if(flag) log_message(AddonLog::ADDON_LOG_INFO, std::forward<_args>(args)...);
}

//---------------------------------------------------------------------------
// addon::log_message (private)
//
// Variadic method of writing a log entry into the Kodi application log
//
// Arguments:
//
//	args	- Variadic argument list

template<typename... _args>
void addon::log_message(AddonLog level, _args&&... args) const
{
	const size_t MAX_ERROR_LOG = 10;	// Maximum entries to store in error log

	std::ostringstream stream;
	int unpack[] = { 0, (static_cast<void>(stream << std::boolalpha << args), 0) ... };
	(void)unpack;

	kodi::Log(level, stream.str().c_str());

	// Write ADDON_LOG_ERROR level messages to an appropriate secondary log mechanism
	if(level == AddonLog::ADDON_LOG_ERROR) {

	#if defined(_WINDOWS) || defined(WINAPI_FAMILY)
		std::string message = "ERROR: " + stream.str() + "\r\n";
		OutputDebugStringA(message.c_str());
	#elif __ANDROID__
		__android_log_print(ANDROID_LOG_ERROR, VERSION_PRODUCTNAME_ANSI, "ERROR: %s\n", stream.str().c_str());
	#else
		fprintf(stderr, "ERROR: %s\r\n", stream.str().c_str());
	#endif

		// Maintain a list of the last MAX_ERROR_LOG error messages that can be exposed
		// to the user without needing to reference the Kodi log file
		std::unique_lock<std::mutex> lock(m_errorlog_lock);
		while(m_errorlog.size() >= MAX_ERROR_LOG) m_errorlog.pop_front();
		m_errorlog.push_back(stream.str());
	}
}

//---------------------------------------------------------------------------
// addon::log_warning (private)
//
// Variadic method of writing a LOG_WARNING entry into the Kodi application log
//
// Arguments:
//
//	args	- Variadic argument list

template<typename... _args>
void addon::log_warning(_args&&... args) const
{
	log_message(AddonLog::ADDON_LOG_WARNING, std::forward<_args>(args)...);
}

//---------------------------------------------------------------------------
// addon::log_warning_if (private)
//
// Variadic method of writing a LOG_WARNING entry into the Kodi application log
//
// Arguments:
//
//	flag	- Flag indicating if the information should be logged
//	args	- Variadic argument list

template<typename... _args>
void addon::log_warning_if(bool flag, _args&&... args) const
{
	if(flag) log_message(AddonLog::ADDON_LOG_WARNING, std::forward<_args>(args)...);
}

//---------------------------------------------------------------------------
// addon::openlivestream_storage_http (private)
//
// Attempts to open a live stream via HTTP from an available storage engine
//
// Arguments:
//
//	dbhandle	- Active database connection to use
//	channelid	- Channel identifier
//	vchannel	- Virtual channel number

std::unique_ptr<pvrstream> addon::openlivestream_storage_http(connectionpool::handle const& dbhandle, union channelid channelid, char const* vchannel) const
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
			std::unique_ptr<pvrstream> stream = httpstream::create(streamurl.c_str());
			log_info(__func__, ": streaming channel ", vchannel, " via storage engine url ", streamurl.c_str());

			return stream;
		}

		// If the stream creation failed, log an error; do not stop enumerating the available storage devices
		catch(std::exception & ex) { log_error(__func__, ": unable to stream channel ", vchannel, " via storage engine url ", streamurl.c_str(), ": ", ex.what()); }
	}

	return nullptr;
}

//---------------------------------------------------------------------------
// addon::openlivestream_tuner_device (private)
//
// Attempts to open a live stream via RTP/UDP from an available tuner device
//
// Arguments:
//
//	dbhandle	- Active database connection to use
//	channelid	- Channel identifier
//	vchannel	- Virtual channel number

std::unique_ptr<pvrstream> addon::openlivestream_tuner_device(connectionpool::handle const& dbhandle, union channelid channelid, char const* vchannel) const
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
		log_info(__func__, ": streaming channel ", vchannel, " via tuner device rtp/udp broadcast");

		return stream;
	}

	// If the stream creation failed, log an error and return a null unique_ptr<> back to the caller, do not throw an exception
	catch(std::exception& ex) { log_error(__func__, ": unable to stream channel ", vchannel, " via tuner device rtp/udp broadcast: ", ex.what()); }

	return nullptr;
}

//---------------------------------------------------------------------------
// addon::openlivestream_tuner_http (private)
//
// Attempts to open a live stream via HTTP from an available tuner device
//
// Arguments:
//
//	dbhandle	- Active database connection to use
//	channelid	- Channel identifier
//	vchannel	- Virtual channel number

std::unique_ptr<pvrstream> addon::openlivestream_tuner_http(connectionpool::handle const& dbhandle, union channelid channelid, char const* vchannel) const
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
		std::unique_ptr<pvrstream> stream = httpstream::create(streamurl.c_str());
		log_info(__func__, ": streaming channel ", vchannel, " via tuner device url ", streamurl.c_str());

		return stream;
	}

	// If the stream creation failed, log an error and return a null unique_ptr<> back to the caller, do not throw an exception
	catch(std::exception& ex) { log_error(__func__, ": unable to stream channel ", vchannel, "via tuner device url ", streamurl.c_str(), ": ", ex.what()); }

	return nullptr;
}

//---------------------------------------------------------------------------
// addon::push_listings (private)
//
// Pushes the current set of guide listings to Kodi asynchronously
//
// Arguments:
//
//	cancel		- Condition variable used to cancel the operation

void addon::push_listings(scalar_condition<bool> const& cancel)
{
	// Create a copy of the current addon settings structure
	struct settings settings = copy_settings();

	log_info(__func__, ": begin asynchronous electronic program guide update");

	enumerate_listings(connectionpool::handle(m_connpool), settings.show_drm_protected_channels, m_epgmaxtime.load(),
		[&](struct listing const& item, bool& cancelenum) -> void {

		kodi::addon::PVREPGTag epgtag;				// PVREPGTag to be transferred to Kodi

		// Abort the enumeration if the cancellation scalar_condition has been set
		if(cancel.test(true) == true) { cancelenum = true; return; }

		// UniqueBroadcastId (required)
		assert(item.broadcastid > EPG_TAG_INVALID_UID);
		epgtag.SetUniqueBroadcastId(item.broadcastid);

		// UniqueChannelId (required)
		epgtag.SetUniqueChannelId(item.channelid);

		// Title (required)
		if(item.title == nullptr) return;
		epgtag.SetTitle(item.title);

		// StartTime (required)
		epgtag.SetStartTime(static_cast<time_t>(item.starttime));

		// EndTime (required)
		epgtag.SetEndTime(static_cast<time_t>(item.endtime));

		// Plot
		if(item.synopsis != nullptr) epgtag.SetPlot(item.synopsis);

		// Year
		//
		// Only report for program type "MV" (Movies)
		if((item.programtype != nullptr) && (strcasecmp(item.programtype, "MV") == 0)) epgtag.SetYear(item.year);

		// IconPath
		if(item.iconurl != nullptr) epgtag.SetIconPath(item.iconurl);

		// GenreType
		epgtag.SetGenreType((settings.use_backend_genre_strings) ? EPG_GENRE_USE_STRING : item.genretype);

		// GenreDescription
		if((settings.use_backend_genre_strings) && (item.genres != nullptr)) epgtag.SetGenreDescription(item.genres);

		// FirstAired
		//
		// Only report for program types "EP" (Series Episode) and "SH" (Show)
		if((item.programtype != nullptr) && ((strcasecmp(item.programtype, "EP") == 0) || (strcasecmp(item.programtype, "SH") == 0))) {

			// Special case: don't report original air date for listings of type EPG_EVENT_CONTENTMASK_NEWSCURRENTAFFAIRS
			// unless series/episode information is available
			if((item.genretype != EPG_EVENT_CONTENTMASK_NEWSCURRENTAFFAIRS) || ((item.seriesnumber >= 1) || (item.episodenumber >= 1)))
				if(item.originalairdate != nullptr) epgtag.SetFirstAired(item.originalairdate);
		}

		// SeriesNumber
		epgtag.SetSeriesNumber(item.seriesnumber);

		// EpisodeNumber
		epgtag.SetEpisodeNumber(item.episodenumber);

		// EpisodePartNumber
		epgtag.SetEpisodePartNumber(EPG_TAG_INVALID_SERIES_EPISODE);

		// EpisodeName
		if(item.episodename != nullptr) epgtag.SetEpisodeName(item.episodename);

		// Flags
		epgtag.SetFlags(EPG_TAG_FLAG_IS_SERIES);

		// SeriesLink
		if(item.seriesid != nullptr) epgtag.SetSeriesLink(item.seriesid);

		// StarRating
		epgtag.SetStarRating(item.starrating);

		// Transfer the EPG_TAG structure over to Kodi
		EpgEventStateChange(epgtag, EPG_EVENT_STATE::EPG_EVENT_UPDATED);
	});

	// This can be a long-running operation; add a log message to indicate when it finished so that
	// it can be detected as a potential performance issue to be addressed in the future
	if(cancel.test(false) == true) log_info(__func__, ": asynchronous electronic program guide update complete");
	else log_info(__func__, ": asynchronous electronic program guide update was cancelled");
}

//---------------------------------------------------------------------------
// addon::select_tuner (private)
//
// Selects an available tuner device from a list of possibilities
//
// Arguments:
//
//	possibilities	- vector<> of possible tuners to select from

std::string addon::select_tuner(std::vector<std::string> const& possibilities) const
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

//---------------------------------------------------------------------------
// addon::start_discovery (private)
//
// Performs a one-time discovery startup operation
//
// Arguments:
//
//	NONE

void addon::start_discovery(void) noexcept
{
	using namespace std::chrono;

	try {

		// Initial discovery schedules all the individual discoveries to occur as soon as possible
		// and in the order in which they will needed by the Kodi callback functions
		std::call_once(m_discovery_started, [&]() {

			// Create a copy of the current addon settings structure
			struct settings settings = copy_settings();

			// Systems with a low precision system_clock implementation may run the tasks out of order,
			// account for this by using a base time with a unique millisecond offset during scheduling
			auto now = system_clock::now();

			// Schedule a task to wait for the network to become available
			m_scheduler.add(now, std::bind(&addon::wait_for_network_task, this, 10, std::placeholders::_1));

			// Schedule the initial discovery tasks to execute as soon as possible
			m_scheduler.add(now + milliseconds(1), [&](scalar_condition<bool> const& cancel) -> void { bool changed; discover_devices(cancel, changed); });
			m_scheduler.add(now + milliseconds(2), [&](scalar_condition<bool> const& cancel) -> void { bool changed; discover_mappings(cancel, changed); });
			m_scheduler.add(now + milliseconds(3), [&](scalar_condition<bool> const& cancel) -> void { bool changed; discover_lineups(cancel, changed); });
			m_scheduler.add(now + milliseconds(4), [&](scalar_condition<bool> const& cancel) -> void { bool changed; discover_recordings(cancel, changed); });
			m_scheduler.add(now + milliseconds(5), [&](scalar_condition<bool> const& cancel) -> void { bool changed; discover_recordingrules(cancel, changed); });
			m_scheduler.add(now + milliseconds(6), [&](scalar_condition<bool> const& cancel) -> void { bool changed; discover_episodes(cancel, changed); });

			// Schedule the startup alert and listing update tasks to occur after the initial discovery tasks have completed
			m_scheduler.add(now + milliseconds(7), &addon::startup_alerts_task, this);
			m_scheduler.add(UPDATE_LISTINGS_TASK, now + milliseconds(8), std::bind(&addon::update_listings_task, this, false, true, std::placeholders::_1));
			m_scheduler.add(now + milliseconds(9), &addon::startup_complete_task, this);

			// Schedule the remaining update tasks to run at the intervals specified in the addon settings
			m_scheduler.add(UPDATE_DEVICES_TASK, system_clock::now() + seconds(settings.discover_devices_interval), &addon::update_devices_task, this);
			m_scheduler.add(UPDATE_LINEUPS_TASK, system_clock::now() + seconds(settings.discover_lineups_interval), &addon::update_lineups_task, this);
			m_scheduler.add(UPDATE_RECORDINGRULES_TASK, system_clock::now() + seconds(settings.discover_recordingrules_interval), &addon::update_recordingrules_task, this);
			m_scheduler.add(UPDATE_EPISODES_TASK, system_clock::now() + seconds(settings.discover_episodes_interval), &addon::update_episodes_task, this);
			m_scheduler.add(UPDATE_RECORDINGS_TASK, system_clock::now() + seconds(settings.discover_recordings_interval), &addon::update_recordings_task, this);
		});
	}

	catch(std::exception& ex) { return handle_stdexception(__func__, ex); }
	catch(...) { return handle_generalexception(__func__); }
}

//---------------------------------------------------------------------------
// addon::startup_alerts_task (private)
//
// Scheduled task implementation to perform any necessary startup alerts
//
// Arguments:
//
//	cancel		- Condition variable used to cancel the operation

void addon::startup_alerts_task(scalar_condition<bool> const& /*cancel*/)
{
	connectionpool::handle dbhandle(m_connpool);

	// Determine how many tuner devices were discovered on the network
	int numtuners = get_tuner_count(dbhandle);

	// If there were no tuner devices detected alert the user via a notification
	if(numtuners == 0) kodi::QueueFormattedNotification(QueueMsg::QUEUE_ERROR, "HDHomeRun tuner device(s) not detected");

	// If there are no DVR authorized tuner devices detected, alert the user via a message box.
	// This operation is only done one time for the installed system, don't use the database for this
	if((numtuners > 0) && (!has_dvr_authorization(dbhandle))) {

		// If for some reason the user path doesn't exist, skip this operation
		if(!kodi::vfs::DirectoryExists(UserPath())) {

			// Check to see if the alert has already been issued on this system by checking for the flag file
			std::string alertfile = UserPath() + "/alerted-epgauth";
			if(!kodi::vfs::FileExists(alertfile, false)) {

				// Issue the alert about the DVR subscription requirement
				kodi::gui::dialogs::OK::ShowAndGetInput("DVR Service Subscription Required",
					"Access to Electronic Program Guide (EPG) listings requires an active HDHomeRun DVR Service subscription.", "",
					"https://www.silicondust.com/dvr-service/");

				// Write the tag file to storage to prevent the message from showing again
				kodi::vfs::CFile tagfile;
				if(tagfile.OpenFileForWrite(alertfile, true)) tagfile.Close();
			}
		}
	}
}

//---------------------------------------------------------------------------
// addon::startup_complete_task (private)
//
// Scheduled task implementation to indicate startup has completed
//
// Arguments:
//
//	cancel		- Condition variable used to cancel the operation

void addon::startup_complete_task(scalar_condition<bool> const& /*cancel*/)
{
	m_startup_complete.store(true);
}

//-----------------------------------------------------------------------------
// addon::update_devices_task (private)
//
// Scheduled task implementation to update the HDHomeRun devices
//
// Arguments:
//
//	cancel		- Condition variable used to cancel the operation

void addon::update_devices_task(scalar_condition<bool> const& cancel)
{
	bool		changed = false;			// Flag if the discovery data changed

	// Create a copy of the current addon settings structure
	struct settings settings = copy_settings();

	try {

		// Update the backend device discovery information
		if(cancel.test(true) == false) discover_devices(cancel, changed);

		// Changes to the device information triggers updates to the lineups and recordings
		if(changed) {

			if(cancel.test(true) == false) {

				log_info(__func__, ": device discovery data changed -- execute lineup update now");
				m_scheduler.now(UPDATE_LINEUPS_TASK, &addon::update_lineups_task, this, cancel);
			}

			if(cancel.test(true) == false) {

				log_info(__func__, ": device discovery data changed -- execute recording update now");
				m_scheduler.now(UPDATE_RECORDINGS_TASK, &addon::update_recordings_task, this, cancel);
			}
		}
	}

	catch(std::exception& ex) { handle_stdexception(__func__, ex); } 
	catch(...) { handle_generalexception(__func__); }

	// Schedule the next periodic invocation of this task to occur at the caclulated interval
	if(cancel.test(true) == false) m_scheduler.add(UPDATE_DEVICES_TASK, std::chrono::system_clock::now() + std::chrono::seconds(settings.discover_devices_interval), &addon::update_devices_task, this);
	else log_info(__func__, ": device update task was cancelled");
}

//-----------------------------------------------------------------------------
// addon::update_episodes_task (private)
//
// Scheduled task implementation to update the episode data associated with recording rules
//
// Arguments:
//
//	cancel		- Condition variable used to cancel the operation

void addon::update_episodes_task(scalar_condition<bool> const& cancel)
{
	bool		changed = false;			// Flag if the discovery data changed

	// Create a copy of the current addon settings structure
	struct settings settings = copy_settings();

	try {

		// Update the backend recording rule episode information
		if(cancel.test(true) == false) discover_episodes(cancel, changed);

		// Changes to the episode information affects the PVR timers
		if(changed) {

			if(cancel.test(true) == false) {

				log_info(__func__, ": recording rule episode discovery data changed -- trigger timer update");
				TriggerTimerUpdate();
			}
		}
	}

	catch(std::exception& ex) { handle_stdexception(__func__, ex); } 
	catch(...) { handle_generalexception(__func__); }

	// Schedule the next periodic invocation of this task to occur at the caclulated interval
	if(cancel.test(true) == false) m_scheduler.add(UPDATE_EPISODES_TASK, std::chrono::system_clock::now() + std::chrono::seconds(settings.discover_episodes_interval), &addon::update_episodes_task, this);
	else log_info(__func__, ": recording rule episode update task was cancelled");
}

//-----------------------------------------------------------------------------
// addon::update_lineups_task (private)
//
// Scheduled task implementation to update the channel lineups
//
// Arguments:
//
//	cancel		- Condition variable used to cancel the operation

void addon::update_lineups_task(scalar_condition<bool> const& cancel)
{
	bool		mappingschanged = false;		// Flag if the discovery data changed
	bool		lineupschanged = false;			// Flag if the discovery data changed

	// Create a copy of the current addon settings structure
	struct settings settings = copy_settings();

	try {

		// Update the channel mapping information
		if(cancel.test(true) == false) discover_mappings(cancel, mappingschanged);

		// Update the backend channel lineup information
		if(cancel.test(true) == false) discover_lineups(cancel, lineupschanged);

		// Changes to either the mappings or the lineups requires a channel group update
		if((cancel.test(true) == false) && (mappingschanged || lineupschanged)) {

			log_info(__func__, ": lineup discovery or channel mapping data changed -- trigger channel group update");
			TriggerChannelGroupsUpdate();
		}

		// Changes to the mappings require a recording update
		if((cancel.test(true) == false) && mappingschanged) {

			log_info(__func__, ": channel mapping data changed -- trigger recording update");
			TriggerRecordingUpdate();
		}

		// Changes to the lineups may require an update to the listings if new channels were added
		if((cancel.test(true) == false) && lineupschanged) {

			log_info(__func__, ": lineup discovery data changed -- schedule guide listings update");
			m_scheduler.add(UPDATE_LISTINGS_TASK, std::bind(&addon::update_listings_task, this, false, true, std::placeholders::_1));
		}
	}

	catch(std::exception& ex) { handle_stdexception(__func__, ex); } 
	catch(...) { handle_generalexception(__func__); }

	// Schedule the next periodic invocation of this task to occur at the caclulated interval
	if(cancel.test(true) == false) m_scheduler.add(UPDATE_LINEUPS_TASK, std::chrono::system_clock::now() + std::chrono::seconds(settings.discover_lineups_interval), &addon::update_lineups_task, this);
	else log_info(__func__, ": lineup update task was cancelled");
}

//-----------------------------------------------------------------------------
// addon::update_listings_task (private)
//
// Scheduled task implementation to update the XMLTV listings
//
// Arguments:
//
//	cancel		- Condition variable used to cancel the operation

void addon::update_listings_task(bool force, bool checkchannels, scalar_condition<bool> const& cancel)
{
	time_t		lastdiscovery = 0;			// Timestamp indicating the last successful discovery
	bool		changed = false;			// Flag if the discovery data changed

	// Create a copy of the current addon settings structure
	struct settings settings = copy_settings();

	// Determine the time at which this function has been called
	time_t now = time(nullptr);

	// Create a database connection to use during this function
	connectionpool::handle dbhandle(m_connpool);

	// Determine the last time the listings discovery executed successfully
	try { lastdiscovery = get_discovered(dbhandle, "listings"); }
	catch(...) { lastdiscovery = 0; }

	// Force an update if the last discovery was more than 18 hours ago
	if((!force) && (lastdiscovery <= (now - 64800))) force = true;

	// Force an update to the listings if there are lineup channels without any guide information
	if((!force) && (checkchannels) && (has_missing_guide_channels(dbhandle))) {

		force = true;
		log_info(__func__, ": forcing update due to missing channel(s) in listing data");
	}

	// Calculate the next time the listings discovery should be executed, which is 24 hours from
	// now or the last successful discovery with a +/- 2 hour amount of randomness applied to it
	int delta = std::uniform_int_distribution<int>(-7200, 7200)(m_randomengine);
	time_t nextdiscovery = (force) ? (now + 86400 + delta) : (lastdiscovery + 86400 + delta);

	try {

		// Update the backend XMLTV listing information (changed will always be true here)
		if(cancel.test(true) == false) {

			if(force) discover_listings(cancel, changed);
			else log_info(__func__, ": listing discovery skipped; data is less than 18 hours old");
		}

		// Trigger a channel update; the metadata (name, icon, etc) may have changed
		if(changed && (cancel.test(true) == false)) {

			log_info(__func__, ": triggering channel update");
			TriggerChannelUpdate();
		}

		// Push all of the updated listings in the database over to Kodi
		if(changed && (cancel.test(true) == false)) push_listings(cancel);
	}

	catch(std::exception& ex) { handle_stdexception(__func__, ex); } 
	catch(...) { handle_generalexception(__func__); }

	// Schedule the next periodic invocation of this task to occur at the caclulated interval
	if(cancel.test(true) == false) m_scheduler.add(UPDATE_LISTINGS_TASK, std::chrono::system_clock::now() + std::chrono::seconds(nextdiscovery - now), 
		std::bind(&addon::update_listings_task, this, false, false, std::placeholders::_1));
	else log_info(__func__, ": listing update task was cancelled");
}

//-----------------------------------------------------------------------------
// addon::update_recordingrules_task (private)
//
// Scheduled task implementation to update the recording rules and timers
//
// Arguments:
//
//	cancel		- Condition variable used to cancel the operation

void addon::update_recordingrules_task(scalar_condition<bool> const& cancel)
{
	bool		changed = false;			// Flag if the discovery data changed

	// Create a copy of the current addon settings structure
	struct settings settings = copy_settings();

	try {

		// Update the backend recording rule information
		if(cancel.test(true) == false) discover_recordingrules(cancel, changed);

		// Changes to the recording rules affects the episode information and PVR timers
		if(changed) {

			// Execute a recording rule episode discovery now; task will reschedule itself
			if(cancel.test(true) == false) {

				log_info(__func__, ": device discovery data changed -- update recording rule episode discovery now");
				m_scheduler.now(UPDATE_EPISODES_TASK, &addon::update_episodes_task, this, cancel);
			}

			// Trigger a PVR timer update (this may be redundant if update_episodes_task already did it)
			if(cancel.test(true) == false) {

				log_info(__func__, ": recording rule discovery data changed -- trigger timer update");
				TriggerTimerUpdate();
			}
		}
	}

	catch(std::exception& ex) { handle_stdexception(__func__, ex); } 
	catch(...) { handle_generalexception(__func__); }

	// Schedule the next periodic invocation of this task to occur at the caclulated interval
	if(cancel.test(true) == false) m_scheduler.add(UPDATE_RECORDINGRULES_TASK, std::chrono::system_clock::now() + std::chrono::seconds(settings.discover_recordingrules_interval), 
		&addon::update_recordingrules_task, this);
	else log_info(__func__, ": recording rule update task was cancelled");
}

//-----------------------------------------------------------------------------
// addon::update_recordings_task (private)
//
// Scheduled task implementation to update the storage recordings
//
// Arguments:
//
//	cancel		- Condition variable used to cancel the operation

void addon::update_recordings_task(scalar_condition<bool> const& cancel)
{
	bool		changed = false;			// Flag if the discovery data changed

	// Create a copy of the current addon settings structure
	struct settings settings = copy_settings();

	try {

		// Update the backend recording information
		if(cancel.test(true) == false) discover_recordings(cancel, changed);

		// Changes to the recordings affects the PVR recording information
		if(changed) {

			if(cancel.test(true) == false) {

				log_info(__func__, ": recording discovery data changed -- trigger recording update");
				TriggerRecordingUpdate();
			}
		}
	}

	catch(std::exception& ex) { handle_stdexception(__func__, ex); } 
	catch(...) { handle_generalexception(__func__); }

	// Schedule the next periodic invocation of this task to occur at the caclulated interval
	if(cancel.test(true) == false) m_scheduler.add(UPDATE_RECORDINGS_TASK, std::chrono::system_clock::now() + std::chrono::seconds(settings.discover_recordings_interval), 
		&addon::update_recordings_task, this);
	else log_info(__func__, ": recording update task was cancelled");
}

//---------------------------------------------------------------------------
// addon::wait_for_devices (private)
//
// Waits until the data required to produce device data has been discovered
//
// Arguments:
//
// NONE

void addon::wait_for_devices(void) noexcept
{
	try {

		// Ensure that the discovery operations have been started
		start_discovery();

		// DEVICES
		m_discovered_devices.wait_until_equals(true);
	}

	catch(std::exception& ex) { handle_stdexception(__func__, ex); }
	catch(...) { handle_generalexception(__func__); }
}

//---------------------------------------------------------------------------
// addon::wait_for_channels (private)
//
// Waits until the data required to produce channel data has been discovered
//
// Arguments:
//
// NONE

void addon::wait_for_channels(void) noexcept
{
	try {

		// Ensure that the discovery operations have been started
		start_discovery();

		// CHANNELS -> { DEVICES + LINEUPS }
		m_discovered_devices.wait_until_equals(true);
		m_discovered_lineups.wait_until_equals(true);
	}

	catch(std::exception& ex) { handle_stdexception(__func__, ex); }
	catch(...) { handle_generalexception(__func__); }
}

//---------------------------------------------------------------------------
// addon::wait_for_network_task (private)
//
// Scheduled task implementation to wait for the network to become available
//
// Arguments:
//
//	seconds		- Number of seconds to wait for the network to become available
//	cancel		- Condition variable used to cancel the operation

void addon::wait_for_network_task(int seconds, scalar_condition<bool> const& cancel)
{
	int attempts = 0;

	// Watch for task cancellation and only retry the operation(s) up to the number of seconds specified
	while((cancel.test(true) == false) && (++attempts < seconds)) {

		if(ipv4_network_available()) { log_info(__func__, ": IPv4 network connectivity detected"); return; }

		// Sleep for one second before trying the operation again
		log_info(__func__, ": IPv4 network connectivity not detected; waiting for one second before trying again");
		std::this_thread::sleep_for(std::chrono::seconds(1));
	}

	// Log an error message if the wait operation was aborted due to a timeout condition
	if(attempts >= seconds) log_error(__func__, ": IPv4 network connectivity was not detected within ", seconds, " seconds; giving up");
}

//---------------------------------------------------------------------------
// addon::wait_for_recordings (private)
//
// Waits until the data required to produce recording data has been discovered
//
// Arguments:
//
// NONE

void addon::wait_for_recordings(void) noexcept
{
	try {

		// Ensure that the discovery operations have been started
		start_discovery();

		// RECORDINGS -> { DEVICES + RECORDINGS }
		m_discovered_devices.wait_until_equals(true);
		m_discovered_recordings.wait_until_equals(true);
	}

	catch(std::exception& ex) { handle_stdexception(__func__, ex); }
	catch(...) { handle_generalexception(__func__); }
}

//---------------------------------------------------------------------------
// addon::wait_for_timers (private)
//
// Waits until the data required to produce timer data has been discovered
//
// Arguments:
//
// NONE

void addon::wait_for_timers(void) noexcept
{
	try {

		// Ensure that the discovery operations have been started
		start_discovery();

		// TIMERS -> { DEVICES + LINEUPS + RECORDINGS + RECORDING RULES + EPISODES }
		m_discovered_devices.wait_until_equals(true);
		m_discovered_lineups.wait_until_equals(true);
		m_discovered_recordings.wait_until_equals(true);
		m_discovered_recordingrules.wait_until_equals(true);
		m_discovered_episodes.wait_until_equals(true);
	}

	catch(std::exception& ex) { handle_stdexception(__func__, ex); }
	catch(...) { handle_generalexception(__func__); }
}

//---------------------------------------------------------------------------
// CADDONBASE IMPLEMENTATION
//---------------------------------------------------------------------------

//---------------------------------------------------------------------------
// addon::Create (CAddonBase)
//
// Initializes the addon instance
//
// Arguments:
//
//	NONE

ADDON_STATUS addon::Create(void)
{
	// Store the EPG maximum time frame specified during addon initialization
	m_epgmaxtime.store(EpgMaxFutureDays());

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

		// Throw a banner out to the Kodi log indicating that the add-on is being loaded
		log_info(__func__, ": ", VERSION_PRODUCTNAME_ANSI, " v", VERSION_VERSION3_ANSI, " loading");

		try {

			// The user data path doesn't always exist when an addon has been installed
			if(!kodi::vfs::DirectoryExists(UserPath())) {

				log_info(__func__, ": user data directory ", UserPath().c_str(), " does not exist");
				if(!kodi::vfs::CreateDirectory(UserPath())) throw string_exception(__func__, ": unable to create addon user data directory");
				log_info(__func__, ": user data directory ", UserPath().c_str(), " created");
			}

			// Load the general settings
			m_settings.pause_discovery_while_streaming = kodi::GetSettingBoolean("pause_discovery_while_streaming", false);
			m_settings.discover_recordings_after_playback = kodi::GetSettingBoolean("discover_recordings_after_playback", false);
			m_settings.show_drm_protected_channels = kodi::GetSettingBoolean("show_drm_protected_channels", false);
			m_settings.disable_backend_channel_logos = kodi::GetSettingBoolean("disable_backend_channel_logos", false);
			m_settings.delete_datetime_rules_after = kodi::GetSettingInt("delete_datetime_rules_after_v2", 86400);				// 1 day

			// Load the interface settings
			m_settings.prepend_channel_numbers = kodi::GetSettingBoolean("prepend_channel_numbers", false);
			m_settings.use_episode_number_as_title = kodi::GetSettingBoolean("use_episode_number_as_title", false);
			m_settings.use_backend_genre_strings = kodi::GetSettingBoolean("use_backend_genre_strings", false);
			m_settings.channel_name_source = kodi::GetSettingEnum("channel_name_source", channel_name_source::xmltv);
			m_settings.disable_recording_categories = kodi::GetSettingBoolean("disable_recording_categories", false);
			m_settings.generate_repeat_indicators = kodi::GetSettingBoolean("generate_repeat_indicators", false);
			m_settings.use_airdate_as_recordingdate = kodi::GetSettingBoolean("use_airdate_as_recordingdate", false);
			m_settings.use_actual_timer_times = kodi::GetSettingBoolean("use_actual_timer_times", false);

			// Load the discovery interval settings
			m_settings.discover_devices_interval = kodi::GetSettingInt("discover_devices_interval_v2", 300);					// 5 minutes
			m_settings.discover_episodes_interval = kodi::GetSettingInt("discover_episodes_interval_v2", 7200);					// 2 hours
			m_settings.discover_lineups_interval = kodi::GetSettingInt("discover_lineups_interval_v2", 2700);					// 45 minutes
			m_settings.discover_recordings_interval = kodi::GetSettingInt("discover_recordings_interval_v2", 600);				// 10 minutes
			m_settings.discover_recordingrules_interval = kodi::GetSettingInt("discover_recordingrules_interval_v2", 7200);		// 2 hours

			// Load the Edit Decision List (EDL) settings
			m_settings.enable_recording_edl = kodi::GetSettingBoolean("enable_recording_edl", false);
			m_settings.recording_edl_folder = kodi::GetSettingString("recording_edl_folder");
			m_settings.recording_edl_folder_2 = kodi::GetSettingString("recording_edl_folder_2");
			m_settings.recording_edl_folder_3 = kodi::GetSettingString("recording_edl_folder_3");
			m_settings.recording_edl_folder_is_flat = kodi::GetSettingBoolean("recording_edl_folder_is_flat", false);
			m_settings.recording_edl_cut_as_comskip = kodi::GetSettingBoolean("recording_edl_cut_as_comskip", false);
			m_settings.recording_edl_start_padding = kodi::GetSettingInt("recording_edl_start_padding", 0);
			m_settings.recording_edl_end_padding = kodi::GetSettingInt("recording_edl_end_padding", 0);

			// Load the Radio Channel settings
			m_settings.enable_radio_channel_mapping = kodi::GetSettingBoolean("enable_radio_channel_mapping", false);
			m_settings.radio_channel_mapping_file = kodi::GetSettingString("radio_channel_mapping_file");
			m_settings.block_radio_channel_video_streams = kodi::GetSettingBoolean("block_radio_channel_video_streams", false);

			// Load the advanced settings
			m_settings.use_http_device_discovery = kodi::GetSettingBoolean("use_http_device_discovery", false);
			m_settings.use_direct_tuning = kodi::GetSettingBoolean("use_direct_tuning", false);
			m_settings.direct_tuning_protocol = kodi::GetSettingEnum("direct_tuning_protocol", tuning_protocol::http);
			m_settings.direct_tuning_allow_drm = kodi::GetSettingBoolean("direct_tuning_allow_drm", false);
			m_settings.stream_read_chunk_size = kodi::GetSettingInt("stream_read_chunk_size_v3", 0);							// Automatic
			m_settings.deviceauth_stale_after = kodi::GetSettingInt("deviceauth_stale_after_v2", 72000);						// 20 hours

			// Log the setting values; these are for diagnostic purposes just use the raw values
			log_info(__func__, ": m_settings.block_radio_channel_video_streams  = ", m_settings.block_radio_channel_video_streams);
			log_info(__func__, ": m_settings.channel_name_source                = ", static_cast<int>(m_settings.channel_name_source));
			log_info(__func__, ": m_settings.delete_datetime_rules_after        = ", m_settings.delete_datetime_rules_after);
			log_info(__func__, ": m_settings.deviceauth_stale_after             = ", m_settings.deviceauth_stale_after);
			log_info(__func__, ": m_settings.direct_tuning_allow_drm            = ", m_settings.direct_tuning_allow_drm);
			log_info(__func__, ": m_settings.direct_tuning_protocol             = ", static_cast<int>(m_settings.direct_tuning_protocol));
			log_info(__func__, ": m_settings.disable_backend_channel_logos      = ", m_settings.disable_backend_channel_logos);
			log_info(__func__, ": m_settings.disable_recording_categories       = ", m_settings.disable_recording_categories);
			log_info(__func__, ": m_settings.discover_devices_interval          = ", m_settings.discover_devices_interval);
			log_info(__func__, ": m_settings.discover_episodes_interval         = ", m_settings.discover_episodes_interval);
			log_info(__func__, ": m_settings.discover_lineups_interval          = ", m_settings.discover_lineups_interval);
			log_info(__func__, ": m_settings.discover_recordingrules_interval   = ", m_settings.discover_recordingrules_interval);
			log_info(__func__, ": m_settings.discover_recordings_after_playback = ", m_settings.discover_recordings_after_playback);
			log_info(__func__, ": m_settings.discover_recordings_interval       = ", m_settings.discover_recordings_interval);
			log_info(__func__, ": m_settings.enable_radio_channel_mapping       = ", m_settings.enable_radio_channel_mapping);
			log_info(__func__, ": m_settings.enable_recording_edl               = ", m_settings.enable_recording_edl);
			log_info(__func__, ": m_settings.generate_repeat_indicators         = ", m_settings.generate_repeat_indicators);
			log_info(__func__, ": m_settings.pause_discovery_while_streaming    = ", m_settings.pause_discovery_while_streaming);
			log_info(__func__, ": m_settings.prepend_channel_numbers            = ", m_settings.prepend_channel_numbers);
			log_info(__func__, ": m_settings.radio_channel_mapping_file         = ", m_settings.radio_channel_mapping_file);
			log_info(__func__, ": m_settings.recording_edl_cut_as_comskip       = ", m_settings.recording_edl_cut_as_comskip);
			log_info(__func__, ": m_settings.recording_edl_end_padding          = ", m_settings.recording_edl_end_padding);
			log_info(__func__, ": m_settings.recording_edl_folder               = ", m_settings.recording_edl_folder);
			log_info(__func__, ": m_settings.recording_edl_folder_2             = ", m_settings.recording_edl_folder_2);
			log_info(__func__, ": m_settings.recording_edl_folder_3             = ", m_settings.recording_edl_folder_3);
			log_info(__func__, ": m_settings.recording_edl_folder_is_flat       = ", m_settings.recording_edl_folder_is_flat);
			log_info(__func__, ": m_settings.recording_edl_start_padding        = ", m_settings.recording_edl_start_padding);
			log_info(__func__, ": m_settings.show_drm_protected_channels        = ", m_settings.show_drm_protected_channels);
			log_info(__func__, ": m_settings.stream_read_chunk_size             = ", m_settings.stream_read_chunk_size);
			log_info(__func__, ": m_settings.use_actual_timer_times             = ", m_settings.use_actual_timer_times);
			log_info(__func__, ": m_settings.use_airdate_as_recordingdate       = ", m_settings.use_airdate_as_recordingdate);
			log_info(__func__, ": m_settings.use_backend_genre_strings          = ", m_settings.use_backend_genre_strings);
			log_info(__func__, ": m_settings.use_direct_tuning                  = ", m_settings.use_direct_tuning);
			log_info(__func__, ": m_settings.use_episode_number_as_title        = ", m_settings.use_episode_number_as_title);
			log_info(__func__, ": m_settings.use_http_discovery                 = ", m_settings.use_http_device_discovery);

			// Register the PVR_MENUHOOK_RECORDING category menu hooks
			AddMenuHook(kodi::addon::PVRMenuhook(MENUHOOK_RECORD_DELETERERECORD, 30302, PVR_MENUHOOK_RECORDING));

			// Register the PVR_MENUHOOK_SETTING category menu hooks
			AddMenuHook(kodi::addon::PVRMenuhook(MENUHOOK_SETTING_SHOWDEVICENAMES, 30312, PVR_MENUHOOK_SETTING));
			AddMenuHook(kodi::addon::PVRMenuhook(MENUHOOK_SETTING_SHOWRECENTERRORS, 30314, PVR_MENUHOOK_SETTING));
			AddMenuHook(kodi::addon::PVRMenuhook(MENUHOOK_SETTING_GENERATEDISCOVERYDIAGNOSTICS, 30315, PVR_MENUHOOK_SETTING));
			AddMenuHook(kodi::addon::PVRMenuhook(MENUHOOK_SETTING_TRIGGERDEVICEDISCOVERY, 30303, PVR_MENUHOOK_SETTING));
			AddMenuHook(kodi::addon::PVRMenuhook(MENUHOOK_SETTING_TRIGGERLINEUPDISCOVERY, 30304, PVR_MENUHOOK_SETTING));
			AddMenuHook(kodi::addon::PVRMenuhook(MENUHOOK_SETTING_TRIGGERLISTINGDISCOVERY, 30313, PVR_MENUHOOK_SETTING));
			AddMenuHook(kodi::addon::PVRMenuhook(MENUHOOK_SETTING_TRIGGERRECORDINGDISCOVERY, 30306, PVR_MENUHOOK_SETTING));
			AddMenuHook(kodi::addon::PVRMenuhook(MENUHOOK_SETTING_TRIGGERRECORDINGRULEDISCOVERY, 30307, PVR_MENUHOOK_SETTING));

			// Register the PVR_MENUHOOK_CHANNEL category menu hooks
			AddMenuHook(kodi::addon::PVRMenuhook(MENUHOOK_CHANNEL_DISABLE, 30309, PVR_MENUHOOK_CHANNEL));
			AddMenuHook(kodi::addon::PVRMenuhook(MENUHOOK_CHANNEL_ADDFAVORITE, 30310, PVR_MENUHOOK_CHANNEL));
			AddMenuHook(kodi::addon::PVRMenuhook(MENUHOOK_CHANNEL_REMOVEFAVORITE, 30311, PVR_MENUHOOK_CHANNEL));

			// Generate the local file system and URL-based file names for the PVR database, the file name is based on the version
			std::string databasefile = UserPath() + "/hdhomerundvr-v" + DATABASE_SCHEMA_VERSION + ".db";
			std::string databasefileuri = "file:///" + databasefile;

			// Create the global database connection pool instance
			try { m_connpool = std::make_shared<connectionpool>(databasefileuri.c_str(), DATABASE_CONNECTIONPOOL_SIZE, SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE | SQLITE_OPEN_URI); } 
			catch(sqlite_exception const& dbex) {

				log_error(__func__, ": unable to create/open the PVR database ", databasefile, " - ", dbex.what());

				// If any SQLite-specific errors were thrown during database open/create, attempt to delete and recreate the database
				log_info(__func__, ": attempting to delete and recreate the PVR database");
				kodi::vfs::DeleteFile(databasefile);
				m_connpool = std::make_shared<connectionpool>(databasefileuri.c_str(), DATABASE_CONNECTIONPOOL_SIZE, SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE | SQLITE_OPEN_URI);
				log_info(__func__, ": successfully recreated the PVR database");
			}

			// Attempt to start the task scheduler
			try { m_scheduler.start(); } 
			catch(...) { m_connpool.reset(); throw; }
		}

		catch(std::exception& ex) { handle_stdexception(__func__, ex); throw; } 
		catch(...) { handle_generalexception(__func__); throw; }
	}

	// Anything that escapes above can't be logged at this point, just return ADDON_STATUS_PERMANENT_FAILURE
	catch(...) { return ADDON_STATUS::ADDON_STATUS_PERMANENT_FAILURE; }

	// Throw a simple banner out to the Kodi log indicating that the add-on has been loaded
	log_info(__func__, ": ", VERSION_PRODUCTNAME_ANSI, " v", VERSION_VERSION3_ANSI, " loaded");

	return ADDON_STATUS::ADDON_STATUS_OK;
}

//---------------------------------------------------------------------------
// addon::Destroy (private)
//
// Uninitializes/unloads the addon instance
//
// Arguments:
//
//	NONE

void addon::Destroy(void) noexcept
{
	try {

		// Throw a message out to the Kodi log indicating that the add-on is being unloaded
		log_info(__func__, ": ", VERSION_PRODUCTNAME_ANSI, " v", VERSION_VERSION3_ANSI, " unloading");

		m_pvrstream.reset();					// Destroy any active stream instance
		m_scheduler.stop();						// Stop the task scheduler
		m_scheduler.clear();					// Clear all tasks from the scheduler

		// Check for more than just the global connection pool reference during shutdown,
		// there shouldn't still be any active callbacks running during ADDON_Destroy
		long poolrefs = m_connpool.use_count();
		if(poolrefs != 1) log_warning(__func__, ": m_connpool.use_count = ", m_connpool.use_count());
		m_connpool.reset();

		curl_global_cleanup();					// Clean up libcurl
		sqlite3_shutdown();						// Clean up SQLite

	#ifdef _WINDOWS
		WSACleanup();							// Release winsock reference
	#endif

		// Send a notice out to the Kodi log as late as possible and destroy the addon callbacks
		log_info(__func__, ": ", VERSION_PRODUCTNAME_ANSI, " v", VERSION_VERSION3_ANSI, " unloaded");
	}

	catch(std::exception& ex) { return handle_stdexception(__func__, ex); }
	catch(...) { return handle_generalexception(__func__); }
}

//---------------------------------------------------------------------------
// addon::SetSetting (CAddonBase)
//
// Notifies the addon that a setting has been changed
//
// Arguments:
//

ADDON_STATUS addon::SetSetting(std::string const& settingName, kodi::CSettingValue const& settingValue)
{
	std::chrono::time_point<std::chrono::system_clock> now = std::chrono::system_clock::now();
	std::unique_lock<std::mutex> settings_lock(m_settings_lock);

	// For comparison purposes
	struct settings previous = m_settings;

	// pause_discovery_while_streaming
	//
	if(settingName == "pause_discovery_while_streaming") {

		bool bvalue = settingValue.GetBoolean();
		if(bvalue != m_settings.pause_discovery_while_streaming) {

			m_settings.pause_discovery_while_streaming = bvalue;
			log_info(__func__, ": setting pause_discovery_while_streaming changed to ", bvalue);
		}
	}

	// prepend_channel_numbers
	//
	else if(settingName == "prepend_channel_numbers") {

		bool bvalue = settingValue.GetBoolean();
		if(bvalue != m_settings.prepend_channel_numbers) {

			m_settings.prepend_channel_numbers = bvalue;
			log_info(__func__, ": setting prepend_channel_numbers changed to ", bvalue, " -- trigger channel update");
			TriggerChannelUpdate();
		}
	}

	// use_episode_number_as_title
	//
	else if(settingName == "use_episode_number_as_title") {

		bool bvalue = settingValue.GetBoolean();
		if(bvalue != m_settings.use_episode_number_as_title) {

			m_settings.use_episode_number_as_title = bvalue;
			log_info(__func__, ": setting use_episode_number_as_title changed to ", bvalue, " -- trigger recording update");
			TriggerRecordingUpdate();
		}
	}

	// discover_recordings_after_playback
	//
	else if(settingName == "discover_recordings_after_playback") {

		bool bvalue = settingValue.GetBoolean();
		if(bvalue != m_settings.discover_recordings_after_playback) {

			m_settings.discover_recordings_after_playback = bvalue;
			log_info(__func__, ": setting discover_recordings_after_playback changed to ", bvalue);
		}
	}

	// use_backend_genre_strings
	//
	else if(settingName == "use_backend_genre_strings") {

		bool bvalue = settingValue.GetBoolean();
		if(bvalue != m_settings.use_backend_genre_strings) {

			m_settings.use_backend_genre_strings = bvalue;
			log_info(__func__, ": setting use_backend_genre_strings changed to ", bvalue);
		}
	}

	// show_drm_protected_channels
	//
	else if(settingName == "show_drm_protected_channels") {

		bool bvalue = settingValue.GetBoolean();
		if(bvalue != m_settings.show_drm_protected_channels) {

			m_settings.show_drm_protected_channels = bvalue;
			log_info(__func__, ": setting show_drm_protected_channels changed to ", bvalue, " -- trigger channel group update");
			TriggerChannelGroupsUpdate();
		}
	}

	// channel_name_source
	//
	else if(settingName == "channel_name_source") {

		int nvalue = settingValue.GetInt();
		if(nvalue != static_cast<int>(m_settings.channel_name_source)) {

			m_settings.channel_name_source = static_cast<enum channel_name_source>(nvalue);
			log_info(__func__, ": setting channel_name_source changed -- trigger channel update");
			TriggerChannelUpdate();
		}
	}

	// disable_recording_categories
	//
	else if(settingName == "disable_recording_categories") {

		bool bvalue = settingValue.GetBoolean();
		if(bvalue != m_settings.disable_recording_categories) {

			m_settings.disable_recording_categories = bvalue;
			log_info(__func__, ": setting disable_recording_categories changed to ", bvalue, " -- trigger recording update");
			TriggerRecordingUpdate();
		}
	}

	// generate_repeat_indicators
	//
	else if(settingName == "generate_repeat_indicators") {

		bool bvalue = settingValue.GetBoolean();
		if(bvalue != m_settings.generate_repeat_indicators) {

			m_settings.generate_repeat_indicators = bvalue;
			log_info(__func__, ": setting generate_repeat_indicators changed to ", bvalue, " -- trigger recording update");
			TriggerRecordingUpdate();
		}
	}

	// use_airdate_as_recordingdate
	//
	else if(settingName == "use_airdate_as_recordingdate") {

		bool bvalue = settingValue.GetBoolean();
		if(bvalue != m_settings.use_airdate_as_recordingdate) {

			m_settings.use_airdate_as_recordingdate = bvalue;
			log_info(__func__, ": setting use_airdate_as_recordingdate changed to ", bvalue, " -- trigger recording update");
			TriggerRecordingUpdate();
		}
	}

	// use_actual_timer_times
	//
	else if(settingName == "use_actual_timer_times") {

		bool bvalue = settingValue.GetBoolean();
		if(bvalue != m_settings.use_actual_timer_times) {

			m_settings.use_actual_timer_times = bvalue;
			log_info(__func__, ": setting use_actual_timer_times changed to ", bvalue, " -- trigger timer update");
			TriggerTimerUpdate();
		}
	}

	// disable_backend_channel_logos
	//
	else if(settingName == "disable_backend_channel_logos") {

		bool bvalue = settingValue.GetBoolean();
		if(bvalue != m_settings.disable_backend_channel_logos) {

			m_settings.disable_backend_channel_logos = bvalue;
			log_info(__func__, ": setting disable_backend_channel_logos changed to ", bvalue, " -- trigger channel update");
			TriggerChannelUpdate();
		}
	}

	// delete_datetime_rules_after
	//
	else if(settingName == "delete_datetime_rules_after_v2") {

		int nvalue = settingValue.GetInt();
		if(nvalue != m_settings.delete_datetime_rules_after) {

			m_settings.delete_datetime_rules_after = nvalue;
			log_info(__func__, ": setting delete_datetime_rules_after changed to ", nvalue, " seconds -- execute recording rule update");
			m_scheduler.add(UPDATE_RECORDINGRULES_TASK, &addon::update_recordingrules_task, this);
		}
	}

	// discover_devices_interval
	//
	else if(settingName == "discover_devices_interval_v2") {

		int nvalue = settingValue.GetInt();
		if(nvalue != m_settings.discover_devices_interval) {

			// Reschedule the update_devices_task to execute at the specified interval from now
			m_settings.discover_devices_interval = nvalue;
			log_info(__func__, ": setting discover_devices_interval changed -- rescheduling update task to initiate in ", nvalue, " seconds");
			m_scheduler.add(UPDATE_DEVICES_TASK, now + std::chrono::seconds(nvalue), &addon::update_devices_task, this);
		}
	}

	// discover_episodes_interval
	//
	else if(settingName == "discover_episodes_interval_v2") {

		int nvalue = settingValue.GetInt();
		if(nvalue != m_settings.discover_episodes_interval) {

			// Reschedule the update_episodes_task to execute at the specified interval from now
			m_settings.discover_episodes_interval = nvalue;
			log_info(__func__, ": setting discover_episodes_interval changed -- rescheduling update task to initiate in ", nvalue, " seconds");
			m_scheduler.add(UPDATE_EPISODES_TASK, now + std::chrono::seconds(nvalue), &addon::update_episodes_task, this);
		}
	}

	// discover_lineups_interval
	//
	else if(settingName == "discover_lineups_interval_v2") {

		int nvalue = settingValue.GetInt();
		if(nvalue != m_settings.discover_lineups_interval) {

			// Reschedule the update_lineups_task to execute at the specified interval from now
			m_settings.discover_lineups_interval = nvalue;
			log_info(__func__, ": setting discover_lineups_interval changed -- rescheduling update task to initiate in ", nvalue, " seconds");
			m_scheduler.add(UPDATE_LINEUPS_TASK, now + std::chrono::seconds(nvalue), &addon::update_lineups_task, this);
		}
	}

	// discover_recordingrules_interval
	//
	else if(settingName == "discover_recordingrules_interval_v2") {

		int nvalue = settingValue.GetInt();
		if(nvalue != m_settings.discover_recordingrules_interval) {

			// Reschedule the update_recordingrules_task to execute at the specified interval from now
			m_settings.discover_recordingrules_interval = nvalue;
			log_info(__func__, ": setting discover_recordingrules_interval changed -- rescheduling update task to initiate in ", nvalue, " seconds");
			m_scheduler.add(UPDATE_RECORDINGRULES_TASK, now + std::chrono::seconds(nvalue), &addon::update_recordingrules_task, this);
		}
	}

	// discover_recordings_interval
	//
	else if(settingName == "discover_recordings_interval_v2") {

		int nvalue = settingValue.GetInt();
		if(nvalue != m_settings.discover_recordings_interval) {

			// Reschedule the update_recordings_task to execute at the specified interval from now
			m_settings.discover_recordings_interval = nvalue;
			log_info(__func__, ": setting discover_recordings_interval changed -- rescheduling update task to initiate in ", nvalue, " seconds");
			m_scheduler.add(UPDATE_RECORDINGS_TASK, now + std::chrono::seconds(nvalue), &addon::update_recordings_task, this);
		}
	}

	// use_http_device_discovery
	//
	else if(settingName == "use_http_device_discovery") {

		bool bvalue = settingValue.GetBoolean();
		if(bvalue != m_settings.use_http_device_discovery) {

			m_settings.use_http_device_discovery = bvalue;
			log_info(__func__, ": setting use_http_device_discovery changed to ", bvalue, " -- schedule device update");

			// Reschedule the device update task to run as soon as possible
			m_scheduler.add(UPDATE_DEVICES_TASK, &addon::update_devices_task, this);
		}
	}

	// use_direct_tuning
	//
	else if(settingName == "use_direct_tuning") {

		bool bvalue = settingValue.GetBoolean();
		if(bvalue != m_settings.use_direct_tuning) {

			m_settings.use_direct_tuning = bvalue;
			log_info(__func__, ": setting use_direct_tuning changed to ", bvalue);
		}
	}

	// direct_tuning_protocol
	//
	else if(settingName == "direct_tuning_protocol") {

		int nvalue = settingValue.GetInt();
		if(nvalue != static_cast<int>(m_settings.direct_tuning_protocol)) {

			m_settings.direct_tuning_protocol = static_cast<enum tuning_protocol>(nvalue);
			log_info(__func__, ": setting direct_tuning_protocol changed to ", (m_settings.direct_tuning_protocol == tuning_protocol::http) ? "HTTP" : "RTP/UDP");
		}
	}

	// direct_tuning_allow_drm
	//
	else if(settingName == "direct_tuning_allow_drm") {

		bool bvalue = settingValue.GetBoolean();
		if(bvalue != m_settings.direct_tuning_allow_drm) {

			m_settings.direct_tuning_allow_drm = bvalue;
			log_info(__func__, ": setting direct_tuning_allow_drm changed to ", bvalue);
		}
	}

	// stream_read_chunk_size
	//
	else if(settingName == "stream_read_chunk_size_v3") {

		int nvalue = settingValue.GetInt();
		if(nvalue != m_settings.stream_read_chunk_size) {

			m_settings.stream_read_chunk_size = nvalue;
			if(m_settings.stream_read_chunk_size == 0) log_info(__func__, ": setting stream_read_chunk_size changed to Automatic");
			else log_info(__func__, ": setting stream_read_chunk_size changed to ", nvalue, " bytes");
		}
	}

	// deviceauth_stale_after
	//
	else if(settingName == "deviceauth_stale_after_v2") {

		int nvalue = settingValue.GetInt();
		if(nvalue != m_settings.deviceauth_stale_after) {

			m_settings.deviceauth_stale_after = nvalue;
			log_info(__func__, ": setting deviceauth_stale_after changed to ", nvalue, " seconds -- schedule device discovery");

			// Reschedule the device discovery task to run as soon as possible
			m_scheduler.add(UPDATE_DEVICES_TASK, &addon::update_devices_task, this);
		}
	}

	// enable_recording_edl
	//
	else if(settingName == "enable_recording_edl") {

		bool bvalue = settingValue.GetBoolean();
		if(bvalue != m_settings.enable_recording_edl) {

			m_settings.enable_recording_edl = bvalue;
			log_info(__func__, ": setting enable_recording_edl changed to ", bvalue);
		}
	}

	// recording_edl_folder
	//
	else if(settingName == "recording_edl_folder") {

		std::string strvalue = settingValue.GetString();
		if(strvalue != m_settings.recording_edl_folder) {

			m_settings.recording_edl_folder = strvalue;
			log_info(__func__, ": setting recording_edl_folder changed to ", strvalue);
		}
	}

	// recording_edl_folder_2
	//
	else if(settingName == "recording_edl_folder_2") {

		std::string strvalue = settingValue.GetString();
		if(strvalue != m_settings.recording_edl_folder_2) {

			m_settings.recording_edl_folder_2 = strvalue;
			log_info(__func__, ": setting recording_edl_folder_2 changed to ", strvalue);
		}
	}

	// recording_edl_folder_3
	//
	else if(settingName == "recording_edl_folder_3") {

		std::string strvalue = settingValue.GetString();
		if(strvalue != m_settings.recording_edl_folder_3) {

			m_settings.recording_edl_folder_3 = strvalue;
			log_info(__func__, ": setting recording_edl_folder_3 changed to ", strvalue);
		}
	}

	// recording_edl_folder_is_flat
	//
	else if(settingName == "recording_edl_folder_is_flat") {

		bool bvalue = settingValue.GetBoolean();
		if(bvalue != m_settings.recording_edl_folder_is_flat) {

			m_settings.recording_edl_folder_is_flat = bvalue;
			log_info(__func__, ": setting recording_edl_folder_is_flat changed to ", bvalue);
		}
	}

	// recording_edl_cut_as_comskip
	//
	else if(settingName == "recording_edl_cut_as_comskip") {

		bool bvalue = settingValue.GetBoolean();
		if(bvalue != m_settings.recording_edl_cut_as_comskip) {

			m_settings.recording_edl_cut_as_comskip = bvalue;
			log_info(__func__, ": setting recording_edl_cut_as_comskip changed to ", bvalue);
		}
	}

	// recording_edl_start_padding
	//
	else if(settingName == "recording_edl_start_padding") {

		int nvalue = settingValue.GetInt();
		if(nvalue != m_settings.recording_edl_start_padding) {

			m_settings.recording_edl_start_padding = nvalue;
			log_info(__func__, ": setting recording_edl_start_padding changed to ", nvalue, " milliseconds");
		}
	}

	// recording_edl_end_padding
	//
	else if(settingName == "recording_edl_end_padding") {

		int nvalue = settingValue.GetInt();
		if(nvalue != m_settings.recording_edl_end_padding) {

			m_settings.recording_edl_end_padding = nvalue;
			log_info(__func__, ": setting recording_edl_end_padding changed to ", nvalue, " milliseconds");
		}
	}

	// enable_radio_channel_mapping
	//
	else if(settingName == "enable_radio_channel_mapping") {

		bool bvalue = settingValue.GetBoolean();
		if(bvalue != m_settings.enable_radio_channel_mapping) {

			m_settings.enable_radio_channel_mapping = bvalue;
			log_info(__func__, ": setting enable_radio_channel_mapping changed to ", bvalue, " -- trigger channel group and recording updates");
			TriggerChannelGroupsUpdate();
			TriggerRecordingUpdate();
		}
	}

	// radio_channel_mapping_file
	//
	else if(settingName == "radio_channel_mapping_file") {

		std::string strvalue = settingValue.GetString();
		if(strvalue != m_settings.radio_channel_mapping_file) {

			m_settings.radio_channel_mapping_file = strvalue;
			log_info(__func__, ": setting radio_channel_mapping_file changed to ", strvalue, " -- schedule channel lineup update");
			m_scheduler.add(UPDATE_LINEUPS_TASK, &addon::update_lineups_task, this);
		}
	}

	// block_radio_channel_video_streams
	//
	else if(settingName == "block_radio_channel_video_streams") {

		bool bvalue = settingValue.GetBoolean();
		if(bvalue != m_settings.block_radio_channel_video_streams) {

			m_settings.block_radio_channel_video_streams = bvalue;
			log_info(__func__, ": setting block_radio_channel_video_streams changed to ", bvalue);
		}
	}

	return ADDON_STATUS::ADDON_STATUS_OK;
}

//---------------------------------------------------------------------------
// CINSTANCEPVRCLIENT IMPLEMENTATION
//---------------------------------------------------------------------------

//-----------------------------------------------------------------------------
// addon::AddTimer (CInstancePVRClient)
//
// Add a timer on the backend
//
// Arguments:
//
//	timer	- The timer to add

PVR_ERROR addon::AddTimer(kodi::addon::PVRTimer const& timer)
{
	std::string				seriesid;			// The seriesid for the timer

	// Get the current time as a unix timestamp, used to set up AfterOriginalAirdateOnly
	time_t now = time(nullptr);

	// Create an initialize a new recordingrule to be passed to the database
	struct recordingrule recordingrule {};

	try {

		// Pull a database connection out from the connection pool
		connectionpool::handle dbhandle(m_connpool);

		// This operation is only available when there is at least one DVR authorized tuner
		std::string authorization = get_authorization_strings(dbhandle, true);
		if(authorization.length() == 0) {

			kodi::gui::dialogs::OK::ShowAndGetInput("DVR Service Subscription Required", "Timer operations require an active HDHomeRun DVR Service subscription.",
				"", "https://www.silicondust.com/dvr-service/");

			return PVR_ERROR::PVR_ERROR_NO_ERROR;
		}

		// seriesrule / epgseriesrule --> recordingrule_type::series
		//
		if((timer.GetTimerType() == timer_type::seriesrule) || (timer.GetTimerType() == timer_type::epgseriesrule)) {

			// seriesrule --> execute a title match operation against the backend and let the user choose the series they want
			//
			if(timer.GetTimerType() == timer_type::seriesrule) {
			
				// Generate a vector of all series that are a title match with the requested EPG search string; the
				// selection dialog will be displayed even if there is only one match in order to confirm the result
				std::vector<std::tuple<std::string, std::string>> matches;
				enumerate_series(dbhandle, authorization.c_str(), timer.GetEPGSearchString().c_str(), [&](struct series const& item) -> void { matches.emplace_back(item.title, item.seriesid); });
				
				// No matches found; display an error message to the user and bail out
				if(matches.size() == 0) {

					kodi::gui::dialogs::OK::ShowAndGetInput("Series Search Failed", "Unable to locate a series with a title that contains:", timer.GetEPGSearchString(), "");
					return PVR_ERROR::PVR_ERROR_NO_ERROR;
				}

				// Create a vector<> of string instances to pass into the selection dialog
				std::vector<std::string> items;
				for(auto const& iterator : matches) items.emplace_back(std::get<0>(iterator));

				// Create and display the selection dialog to get the specific series the user wants
				int result = kodi::gui::dialogs::Select::Show("Select Series", items);
				if(result == -1) return PVR_ERROR::PVR_ERROR_NO_ERROR;
				
				seriesid = std::get<1>(matches[result]);
			}

			// epgseriesrule --> the title must be an exact match with a known series on the backend
			//
			else {

				// Get the seriesid for the recording rule; if one has been specified as part of the timer request use it.
				// Otherwise search for it with a title match against the backend services
				seriesid = timer.GetSeriesLink();
				if(seriesid.length() == 0) seriesid = find_seriesid(dbhandle, authorization.c_str(), timer.GetEPGSearchString().c_str());

				// If no match was found, the timer cannot be added; use a dialog box rather than returning an error
				if(seriesid.length() == 0) {
					
					kodi::gui::dialogs::OK::ShowAndGetInput("Series Search Failed", "Unable to locate a series with a title matching:", timer.GetEPGSearchString(), "");
					return PVR_ERROR::PVR_ERROR_NO_ERROR;
				}
			}

			// If the seriesid is still not set the operation cannot continue; throw an exception
			if(seriesid.length() == 0) throw string_exception(std::string("could not locate seriesid for title '") + timer.GetEPGSearchString().c_str() + "'");

			// Generate a series recording rule
			recordingrule.type = recordingrule_type::series;
			recordingrule.seriesid = seriesid.c_str();
			recordingrule.channelid.value = (timer.GetClientChannelUid() == PVR_TIMER_ANY_CHANNEL) ? 0 : timer.GetClientChannelUid();
			recordingrule.recentonly = (timer.GetPreventDuplicateEpisodes() == duplicate_prevention::recentonly);
			recordingrule.afteroriginalairdateonly = (timer.GetPreventDuplicateEpisodes() == duplicate_prevention::newonly) ? now : 0;
			recordingrule.startpadding = (timer.GetMarginStart() == 0) ? 30 : timer.GetMarginStart() * 60;
			recordingrule.endpadding = (timer.GetMarginEnd() == 0) ? 30 : timer.GetMarginEnd() * 60;
		}

		// datetimeonlyrule / epgdatetimeonlyrule --> recordingrule_type::datetimeonly
		//
		else if((timer.GetTimerType() == timer_type::datetimeonlyrule) || (timer.GetTimerType() == timer_type::epgdatetimeonlyrule)) {

			union channelid channelid{};
			channelid.value = (timer.GetClientChannelUid() == PVR_TIMER_ANY_CHANNEL) ? 0 : timer.GetClientChannelUid();

			// Get the seriesid for the recording rule; if one has been specified as part of the timer request use it.
			// Otherwise search for it first by channel and start time, falling back to a title match if necessary
			seriesid = timer.GetSeriesLink();
			if(seriesid.length() == 0) seriesid = find_seriesid(dbhandle, channelid, timer.GetStartTime());
			if(seriesid.length() == 0) seriesid = find_seriesid(dbhandle, authorization.c_str(), timer.GetEPGSearchString().c_str());

			// If no match was found, the timer cannot be added; use a dialog box rather than returning an error
			if(seriesid.length() == 0) {
					
				kodi::gui::dialogs::OK::ShowAndGetInput("Series Search Failed", "Unable to locate a series with a title matching:", timer.GetEPGSearchString(), "");
				return PVR_ERROR::PVR_ERROR_NO_ERROR;
			}

			recordingrule.type = recordingrule_type::datetimeonly;
			recordingrule.seriesid = seriesid.c_str();
			recordingrule.channelid = channelid;
			recordingrule.datetimeonly = timer.GetStartTime();
			recordingrule.startpadding = (timer.GetMarginStart() == 0) ? 30 : timer.GetMarginStart() * 60;
			recordingrule.endpadding = (timer.GetMarginEnd() == 0) ? 30 : timer.GetMarginEnd() * 60;
		}

		// any other timer type is not supported
		else return PVR_ERROR::PVR_ERROR_NOT_IMPLEMENTED;

		// Attempt to add the new recording rule to the database/backend service
		add_recordingrule(dbhandle, authorization.c_str(), recordingrule);

		// Update the episode information for the specified series; issue a log warning if the operation fails
		try { discover_episodes_seriesid(dbhandle, authorization.c_str(), seriesid.c_str()); }
		catch(std::exception& ex) { log_warning(__func__, ": unable to refresh episode information for series ", seriesid.c_str(), ": ", ex.what()); }
		catch(...) { log_warning(__func__, ": unable to refresh episode information for series ", seriesid.c_str()); }

		// Force a timer update in Kodi to refresh whatever this did on the backend
		TriggerTimerUpdate();

		// Schedule a recording update operation for 15 seconds in the future after any new timer has been
		// added; this allows a timer that kicks off immediately to show the recording in Kodi quickly
		log_info(__func__, ": scheduling recording update to initiate in 15 seconds");
		m_scheduler.add(UPDATE_RECORDINGS_TASK, std::chrono::system_clock::now() + std::chrono::seconds(15), &addon::update_recordings_task, this);
	}

	catch(std::exception& ex) { return handle_stdexception(__func__, ex, PVR_ERROR::PVR_ERROR_FAILED); }
	catch(...) { return handle_generalexception(__func__, PVR_ERROR::PVR_ERROR_FAILED); }

	return PVR_ERROR::PVR_ERROR_NO_ERROR;
}

//-----------------------------------------------------------------------------
// addon::CallChannelMenuHook (CInstancePVRClient)
//
// Call one of the channel related menu hooks
//
// Arguments:
//
//	menuhook	- The hook being invoked
//	item		- The item referenced by the hook

PVR_ERROR addon::CallChannelMenuHook(kodi::addon::PVRMenuhook const& menuhook, kodi::addon::PVRChannel const& item)
{
	try {

		union channelid channelid{};						// Opaque channel unique identifier
		channelid.value = item.GetUniqueId();				// Retrieve the opaque unique identifier

		// MENUHOOK_CHANNEL_DISABLE
		//
		if(menuhook.GetHookId() == MENUHOOK_CHANNEL_DISABLE) {

			// Set the channel visibility to disabled (red x) and kick off a lineup discovery task
			set_channel_visibility(connectionpool::handle(m_connpool), channelid, channel_visibility::disabled);
			log_info(__func__, ": channel ", item.GetChannelName().c_str(), " disabled; scheduling lineup update task");
			m_scheduler.add(UPDATE_LINEUPS_TASK, &addon::update_lineups_task, this);
		}

		// MENUHOOK_CHANNEL_ADDFAVORITE
		//
		else if(menuhook.GetHookId() == MENUHOOK_CHANNEL_ADDFAVORITE) {

			// Set the channel visibility to favorite (yellow star) and kick off a lineup discovery task
			set_channel_visibility(connectionpool::handle(m_connpool), channelid, channel_visibility::favorite);

			log_info(__func__, ": channel ", item.GetChannelName().c_str(), " added as favorite; scheduling lineup update task");
			m_scheduler.add(UPDATE_LINEUPS_TASK, &addon::update_lineups_task, this);
		}

		// MENUHOOK_CHANNEL_REMOVEFAVORITE
		//
		else if(menuhook.GetHookId() == MENUHOOK_CHANNEL_REMOVEFAVORITE) {

			// Set the channel visibility to favorite (gray star) and kick off a lineup discovery task
			set_channel_visibility(connectionpool::handle(m_connpool), channelid, channel_visibility::enabled);

			log_info(__func__, ": channel ", item.GetChannelName().c_str(), " removed from favorites; scheduling lineup update task");
			m_scheduler.add(UPDATE_LINEUPS_TASK, &addon::update_lineups_task, this);
		}

		else return PVR_ERROR::PVR_ERROR_NOT_IMPLEMENTED;
	}

	catch(std::exception& ex) { return handle_stdexception(__func__, ex, PVR_ERROR::PVR_ERROR_FAILED); }
	catch(...) { return handle_generalexception(__func__, PVR_ERROR::PVR_ERROR_FAILED); }

	return PVR_ERROR::PVR_ERROR_NO_ERROR;
}

//-----------------------------------------------------------------------------
// addon::CallRecordingMenuHook (CInstancePVRClient)
//
// Call one of the recording related menu hooks
//
// Arguments:
//
//	menuhook	- The hook being invoked
//	item		- The item referenced by the hook

PVR_ERROR addon::CallRecordingMenuHook(kodi::addon::PVRMenuhook const& menuhook, kodi::addon::PVRRecording const& item)
{
	try {

		std::string	recordingid = item.GetRecordingId();
	
		// MENUHOOK_RECORD_DELETERERECORD
		//
		if(menuhook.GetHookId() == MENUHOOK_RECORD_DELETERERECORD) {

			// Delete the recording with the re-record flag set to true and trigger an update
			delete_recording(connectionpool::handle(m_connpool), recordingid.c_str(), true);
			TriggerRecordingUpdate();
		}

		else return PVR_ERROR::PVR_ERROR_NOT_IMPLEMENTED;
	}

	catch(std::exception& ex) { return handle_stdexception(__func__, ex, PVR_ERROR::PVR_ERROR_FAILED); }
	catch(...) { return handle_generalexception(__func__, PVR_ERROR::PVR_ERROR_FAILED); }

	return PVR_ERROR::PVR_ERROR_NO_ERROR;
}

//-----------------------------------------------------------------------------
// addon::CallSettingsMenuHook (CInstancePVRClient)
//
// Call one of the settings related menu hooks
//
// Arguments:
//
//	menuhook	- The hook being invoked

PVR_ERROR addon::CallSettingsMenuHook(kodi::addon::PVRMenuhook const& menuhook)
{
	try {

		// MENUHOOK_SETTING_SHOWDEVICENAMES
		//
		if(menuhook.GetHookId() == MENUHOOK_SETTING_SHOWDEVICENAMES) {

			std::string names;				// Constructed string for the TextViewer dialog

			// Enumerate all of the device names in the database and build out the text string
			enumerate_device_names(connectionpool::handle(m_connpool), [&](struct device_name const& device_name) -> void {
				if(device_name.name != nullptr) names.append(std::string(device_name.name) + "\r\n"); });
			kodi::gui::dialogs::TextViewer::Show("Discovered HDHomeRun devices", names);
		}

		// MENUHOOK_SETTING_SHOWRECENTERRORS
		//
		else if(menuhook.GetHookId() == MENUHOOK_SETTING_SHOWRECENTERRORS) {

			std::string errors;			// Constructed string for the TextViewer dialog

			// Generate a simple list of the recent error messages, this doesn't need to be fancy.
			std::unique_lock<std::mutex> lock(m_errorlog_lock);
			for(auto iterator = m_errorlog.rbegin(); iterator != m_errorlog.rend(); iterator++) errors.append((*iterator) + "\r\n\r\n");
			if(errors.empty()) errors.assign("No recent error messages");

			kodi::gui::dialogs::TextViewer::Show("Recent error messages", errors);
		}

		// MENUHOOK_SETTING_GENERATEDISCOVERYDIAGNOSTICS
		//
		else if(menuhook.GetHookId() == MENUHOOK_SETTING_GENERATEDISCOVERYDIAGNOSTICS) {

			std::string					folderpath;				// Export folder path

			// Prompt the user to locate the folder where the .json file will be exported ...
			if(kodi::gui::dialogs::FileBrowser::ShowAndGetDirectory("local|network|removable", "Select diagnostic data export folder", folderpath, true)) {

				try {

					// The database module handles this; just have to tell it where to write the file
					generate_discovery_diagnostic_file(connectionpool::handle(m_connpool), folderpath.c_str());

					// Inform the user that the operation was successful
					kodi::gui::dialogs::OK::ShowAndGetInput("Discovery Diagnostic Data", "The discovery diagnostic data was exported successfully");
				}

				catch(std::exception& ex) {

					kodi::gui::dialogs::OK::ShowAndGetInput("Discovery Diagnostic Data", "An error occurred exporting the discovery diagnostic data:", "", ex.what());
					throw;
				}
			}
		}

		// MENUHOOK_SETTING_TRIGGERDEVICEDISCOVERY
		//
		else if(menuhook.GetHookId() == MENUHOOK_SETTING_TRIGGERDEVICEDISCOVERY) {

			log_info(__func__, ": scheduling device update task");
			m_scheduler.add(UPDATE_DEVICES_TASK, &addon::update_devices_task, this);
		}

		// MENUHOOK_SETTING_TRIGGERLINEUPDISCOVERY
		//
		else if(menuhook.GetHookId() == MENUHOOK_SETTING_TRIGGERLINEUPDISCOVERY) {

			log_info(__func__, ": scheduling lineup update task");
			m_scheduler.add(UPDATE_LINEUPS_TASK, &addon::update_lineups_task, this);
		}

		// MENUHOOK_SETTING_TRIGGERLISTINGDISCOVERY
		//
		else if(menuhook.GetHookId() == MENUHOOK_SETTING_TRIGGERLISTINGDISCOVERY) {

			log_info(__func__, ": scheduling listing update task (forced)");
			m_scheduler.add(UPDATE_LISTINGS_TASK, std::bind(&addon::update_listings_task, this, true, true, std::placeholders::_1));
		}

		// MENUHOOK_SETTING_TRIGGERRECORDINGRULEDISCOVERY
		//
		else if(menuhook.GetHookId() == MENUHOOK_SETTING_TRIGGERRECORDINGRULEDISCOVERY) {

			log_info(__func__, ": scheduling recording rule update task");
			m_scheduler.add(UPDATE_RECORDINGRULES_TASK, &addon::update_recordingrules_task, this);
		}

		// MENUHOOK_SETTING_TRIGGERRECORDINGDISCOVERY
		//
		else if(menuhook.GetHookId() == MENUHOOK_SETTING_TRIGGERRECORDINGDISCOVERY) {

			log_info(__func__, ": scheduling recording update task");
			m_scheduler.add(UPDATE_RECORDINGS_TASK, &addon::update_recordings_task, this);
		}

		else return PVR_ERROR::PVR_ERROR_NOT_IMPLEMENTED;
	}

	catch(std::exception& ex) { return handle_stdexception(__func__, ex, PVR_ERROR::PVR_ERROR_FAILED); }
	catch(...) { return handle_generalexception(__func__, PVR_ERROR::PVR_ERROR_FAILED); }

	return PVR_ERROR::PVR_ERROR_NO_ERROR;
}

//-----------------------------------------------------------------------------
// addon::CanPauseStream (CInstancePVRClient)
//
// Check if the backend support pausing the currently playing stream
//
// Arguments:
//
//	NONE

bool addon::CanPauseStream(void)
{
	return true;
}

//-----------------------------------------------------------------------------
// addon::CanSeekStream (CInstancePVRClient)
//
// Check if the backend supports seeking for the currently playing stream
//
// Arguments:
//
//	NONE

bool addon::CanSeekStream(void)
{
	try { return (m_pvrstream) ? m_pvrstream->canseek() : false; }
	catch(std::exception& ex) { return handle_stdexception(__func__, ex, false); }
	catch(...) { return handle_generalexception(__func__, false); }
}

//-----------------------------------------------------------------------------
// addon::CloseLiveStream (CInstancePVRClient)
//
// Close an open live stream
//
// Arguments:
//
//	NONE

void addon::CloseLiveStream(void)
{
	if(!m_pvrstream) return;

	try {
		
		m_pvrstream.reset();							// Close the active stream instance
		m_scheduler.resume();							// Resume task scheduler
		m_stream_starttime = m_stream_endtime = 0;		// Reset stream time trackers

		// If the setting to refresh the recordings immediately after playback, reschedule it
		// to execute in a few seconds; this prevents doing it multiple times when changing channels
		if(copy_settings().discover_recordings_after_playback) {

			log_info(__func__, ": playback stopped; scheduling recording update to occur in 5 seconds");
			m_scheduler.add(UPDATE_RECORDINGS_TASK, std::chrono::system_clock::now() + std::chrono::seconds(5), &addon::update_recordings_task, this);
		}
	}

	catch(std::exception& ex) { return handle_stdexception(__func__, ex); }
	catch(...) { return handle_generalexception(__func__); }
}

//-----------------------------------------------------------------------------
// addon::CloseRecordedStream (CInstancePVRClient)
//
// Close an open stream from a recording
//
// Arguments:
//
//	NONE

void addon::CloseRecordedStream(void)
{
	return CloseLiveStream();			// Identical implementation
}

//-----------------------------------------------------------------------------
// addon::DeleteRecording (CInstancePVRClient)
//
// Delete a recording on the backend
//
// Arguments:
//
//	recording	- Recording to be deleted

PVR_ERROR addon::DeleteRecording(const kodi::addon::PVRRecording& recording)
{
	try { delete_recording(connectionpool::handle(m_connpool), recording.GetRecordingId().c_str(), false); }
	catch(std::exception& ex) { return handle_stdexception(__func__, ex, PVR_ERROR::PVR_ERROR_FAILED); }
	catch(...) { return handle_generalexception(__func__, PVR_ERROR::PVR_ERROR_FAILED); }

	return PVR_ERROR::PVR_ERROR_NO_ERROR;
}

//-----------------------------------------------------------------------------
// addon::DeleteTimer (CInstancePVRClient)
//
// Delete a timer on the backend
//
// Arguments:
//
//	timer		- Timer instance to be deleted
//	forceDelete	- Forcibly delete a timer currently recording a program

PVR_ERROR addon::DeleteTimer(kodi::addon::PVRTimer const& timer, bool /*forceDelete*/)
{
	unsigned int	recordingruleid = 0;			// Backend recording rule identifier

	try {

		// Pull a database connection out from the connection pool
		connectionpool::handle dbhandle(m_connpool);

		// This operation is only available when there is at least one DVR authorized tuner
		std::string authorization = get_authorization_strings(dbhandle, true);
		if(authorization.length() == 0) {

			kodi::gui::dialogs::OK::ShowAndGetInput("DVR Service Subscription Required", "Timer operations require an active HDHomeRun DVR Service subscription.",
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
		if(timer.GetTimerType() == timer_type::seriestimer) {

			std::string text = "The Timer for this episode of " + timer.GetTitle() + " is a member of an active Record Series Timer Rule and cannot be deleted.";
			kodi::gui::dialogs::OK::ShowAndGetInput("Unable to delete Timer", text);

			return PVR_ERROR::PVR_ERROR_NO_ERROR;
		}

		else if(timer.GetTimerType() == timer_type::datetimeonlytimer) recordingruleid = timer.GetParentClientIndex();
		else if((timer.GetTimerType() == timer_type::seriesrule) || (timer.GetTimerType() == timer_type::datetimeonlyrule)) recordingruleid = timer.GetClientIndex();
		else return PVR_ERROR::PVR_ERROR_NOT_IMPLEMENTED;

		// Determine the series identifier for the recording rule before it gets deleted
		std::string seriesid = (!timer.GetSeriesLink().empty()) ? timer.GetSeriesLink() : get_recordingrule_seriesid(dbhandle, recordingruleid);
		if(seriesid.length() == 0) throw string_exception(__func__, ": could not determine seriesid for timer");

		// Attempt to delete the recording rule from the backend and the database
		delete_recordingrule(dbhandle, authorization.c_str(), recordingruleid);

		// Update the episode information for the specified series; issue a log warning if the operation fails
		try { discover_episodes_seriesid(dbhandle, authorization.c_str(), seriesid.c_str()); }
		catch(std::exception& ex) { log_warning(__func__, ": unable to refresh episode information for series ", seriesid.c_str(), ": ", ex.what()); }
		catch(...) { log_warning(__func__, ": unable to refresh episode information for series ", seriesid.c_str()); }
	}

	catch(std::exception& ex) { return handle_stdexception(__func__, ex, PVR_ERROR::PVR_ERROR_FAILED); }
	catch(...) { return handle_generalexception(__func__, PVR_ERROR::PVR_ERROR_FAILED); }

	// Force a timer update in Kodi to refresh whatever this did on the backend
	TriggerTimerUpdate();

	return PVR_ERROR::PVR_ERROR_NO_ERROR;	
}

//-----------------------------------------------------------------------------
// addon::GetBackendHostname (CInstancePVRClient)
//
// Get the hostname of the pvr backend server
//
// Arguments:
//
//	hostname	- Set to the hostname of the backend server

PVR_ERROR addon::GetBackendHostname(std::string& hostname)
{
	hostname.assign("api.hdhomerun.com");

	return PVR_ERROR::PVR_ERROR_NO_ERROR;
}

//-----------------------------------------------------------------------------
// addon::GetBackendName (CInstancePVRClient)
//
// Get the name reported by the backend that will be displayed in the UI
//
// Arguments:
//
//	name		- Backend name string to be initialized

PVR_ERROR addon::GetBackendName(std::string& name)
{
	name.assign(VERSION_PRODUCTNAME_ANSI);

	return PVR_ERROR::PVR_ERROR_NO_ERROR;
}

//-----------------------------------------------------------------------------
// addon::GetBackendVersion (CInstancePVRClient)
//
// Get the version string reported by the backend that will be displayed in the UI
//
// Arguments:
//
//	version		- Backend version string to be initialized

PVR_ERROR addon::GetBackendVersion(std::string& version)
{
	version.assign(VERSION_VERSION3_ANSI);

	return PVR_ERROR::PVR_ERROR_NO_ERROR;
}

//-----------------------------------------------------------------------------
// addon::GetCapabilities (CInstancePVRClient)
//
// Get the list of features that this add-on provides
//
// Arguments:
//
//	capabilities	- PVR capability attributes class

PVR_ERROR addon::GetCapabilities(kodi::addon::PVRCapabilities& capabilities)
{
	capabilities.SetSupportsEPG(true);
	capabilities.SetSupportsTV(true);
	capabilities.SetSupportsRadio(true);
	capabilities.SetSupportsRecordings(true);
	capabilities.SetSupportsTimers(true);
	capabilities.SetSupportsChannelGroups(true);
	capabilities.SetHandlesInputStream(true);
	capabilities.SetSupportsRecordingPlayCount(true);
	capabilities.SetSupportsLastPlayedPosition(true);
	capabilities.SetSupportsRecordingEdl(true);

	return PVR_ERROR::PVR_ERROR_NO_ERROR;
}

//-----------------------------------------------------------------------------
// addon::GetChannelGroupsAmount (CInstancePVRClient)
//
// Get the total amount of channel groups on the backend
//
// Arguments:
//
//	amount		- Set to the number of available channel groups

PVR_ERROR addon::GetChannelGroupsAmount(int& amount)
{
	amount = 5;		// "Favorite Channels", "HEVC Channels", "HD Channels", "SD Channels" and "Demo Channels"

	return PVR_ERROR::PVR_ERROR_NO_ERROR;		
}

//-----------------------------------------------------------------------------
// addon::GetChannelGroupMembers (CInstancePVRClient)
//
// Request the list of all group members of a group from the backend
//
// Arguments:
//
//	group		- Channel group for which to get the group members
//	results		- Channel group members result set to be loaded

PVR_ERROR addon::GetChannelGroupMembers(kodi::addon::PVRChannelGroup const& group, kodi::addon::PVRChannelGroupMembersResultSet& results)
{
	// Wait until the channel information has been discovered the first time
	wait_for_channels();

	// There are no radio channel groups
	if(group.GetIsRadio()) return PVR_ERROR::PVR_ERROR_NO_ERROR;

	// Determine which group enumerator to use for the operation, there are only five to
	// choose from: "Favorite Channels", "HEVC Channels", "HD Channels", "SD Channels" and "Demo Channels"
	std::function<void(sqlite3*, bool, enumerate_channelids_callback)> enumerator = nullptr;
	if(strcmp(group.GetGroupName().c_str(), "Favorite channels") == 0) enumerator = enumerate_favorite_channelids;
	else if(strcmp(group.GetGroupName().c_str(), "HEVC channels") == 0) enumerator = enumerate_hevc_channelids;
	else if(strcmp(group.GetGroupName().c_str(), "HD channels") == 0) enumerator = enumerate_hd_channelids;
	else if(strcmp(group.GetGroupName().c_str(), "SD channels") == 0) enumerator = enumerate_sd_channelids;
	else if(strcmp(group.GetGroupName().c_str(), "Demo channels") == 0) enumerator = enumerate_demo_channelids;

	// If neither enumerator was selected, there isn't any work to do here
	if(enumerator == nullptr) return PVR_ERROR::PVR_ERROR_NO_ERROR;

	// Create a copy of the current addon settings structure
	struct settings settings = copy_settings();

	try {

		std::unique_lock<std::mutex> lock(m_radiomappings_lock);		// Prevent updates to the mappings

		// Enumerate all of the channels in the specified group
		enumerator(connectionpool::handle(m_connpool), settings.show_drm_protected_channels, [&](union channelid const& item) -> void {

			// Determine if this channel should be mapped as a Radio channel instead of a TV channel
			bool isradiochannel = (settings.enable_radio_channel_mapping && is_channel_radio(lock, item));
			if(!isradiochannel) {

				// Create and initialize a PVRChannelGroupMember instance for the enumerated channel
				kodi::addon::PVRChannelGroupMember member;
				member.SetGroupName(group.GetGroupName());
				member.SetChannelUniqueId(item.value);
				member.SetChannelNumber(static_cast<int>(item.parts.channel));
				member.SetSubChannelNumber(static_cast<int>(item.parts.subchannel));

				results.Add(member);
			}
		});
	}

	catch(std::exception& ex) { return handle_stdexception(__func__, ex, PVR_ERROR::PVR_ERROR_FAILED); }
	catch(...) { return handle_generalexception(__func__, PVR_ERROR::PVR_ERROR_FAILED); }

	return PVR_ERROR::PVR_ERROR_NO_ERROR;
}

//-----------------------------------------------------------------------------
// addon::GetChannelGroups (CInstancePVRClient)
//
// Request the list of all channel groups from the backend
//
// Arguments:
//
//	radio		- True to get radio groups, false to get TV channel groups
//	results		- Channel groups result set to be loaded

PVR_ERROR addon::GetChannelGroups(bool radio, kodi::addon::PVRChannelGroupsResultSet& results)
{
	kodi::addon::PVRChannelGroup	favoritechannels;	// Favorite channels
	kodi::addon::PVRChannelGroup	hevcchannels;		// HEVC channels
	kodi::addon::PVRChannelGroup	hdchannels;			// HD channels
	kodi::addon::PVRChannelGroup	sdchannels;			// SD channels
	kodi::addon::PVRChannelGroup	demochannels;		// Demo channels

	// The PVR doesn't support radio channel groups
	if(radio) return PVR_ERROR::PVR_ERROR_NO_ERROR;

	favoritechannels.SetGroupName("Favorite channels");
	results.Add(favoritechannels);

	hevcchannels.SetGroupName("HEVC channels");
	results.Add(hevcchannels);

	hdchannels.SetGroupName("HD channels");
	results.Add(hdchannels);

	sdchannels.SetGroupName("SD channels");
	results.Add(sdchannels);

	demochannels.SetGroupName("Demo channels");
	results.Add(demochannels);

	return PVR_ERROR::PVR_ERROR_NO_ERROR;
}

//-----------------------------------------------------------------------------
// addon::GetChannels (CInstancePVRClient)
//
// Request the list of all channels from the backend
//
// Arguments:
//
//	radio		- True to get radio channels, false to get TV channels
//	results		- Channels result set to be loaded

PVR_ERROR addon::GetChannels(bool radio, kodi::addon::PVRChannelsResultSet& results)
{
	// Wait until the channel information has been discovered the first time
	wait_for_channels();

	// Create a copy of the current addon settings structure
	struct settings settings = copy_settings();

	try {

		std::unique_lock<std::mutex> lock(m_radiomappings_lock);		// Prevent updates to the mappings

		// Enumerate all of the channels in the database
		enumerate_channels(connectionpool::handle(m_connpool), settings.prepend_channel_numbers, settings.show_drm_protected_channels, 
			settings.channel_name_source, [&](struct channel const& item) -> void {

			// Determine if this channel should be mapped as a Radio channel instead of a TV channel
			bool isradiochannel = (settings.enable_radio_channel_mapping && is_channel_radio(lock, item.channelid));

			// Only transfer the channel to Kodi if it matches the current category (TV/Radio)
			if(isradiochannel == radio) {

				kodi::addon::PVRChannel channel;				// PVRChannel to be transferred to Kodi

				// UniqueId (required)
				channel.SetUniqueId(item.channelid.value);

				// IsRadio (required)
				channel.SetIsRadio(isradiochannel);

				// ChannelNumber
				channel.SetChannelNumber(item.channelid.parts.channel);

				// SubChannelNumber
				channel.SetSubChannelNumber(item.channelid.parts.subchannel);

				// ChannelName
				if(item.channelname != nullptr) channel.SetChannelName(item.channelname);

				// InputFormat
				channel.SetMimeType("video/mp2t");

				// EncryptionSystem
				//
				// This is used to flag a channel as DRM to prevent it from being streamed
				channel.SetEncryptionSystem((item.drm) ? std::numeric_limits<unsigned int>::max() : 0);

				// IconPath
				if((settings.disable_backend_channel_logos == false) && (item.iconurl != nullptr)) channel.SetIconPath(item.iconurl);

				results.Add(channel);
			}
		});
	}
	
	catch(std::exception& ex) { return handle_stdexception(__func__, ex, PVR_ERROR::PVR_ERROR_FAILED); }
	catch(...) { return handle_generalexception(__func__, PVR_ERROR::PVR_ERROR_FAILED); }

	return PVR_ERROR::PVR_ERROR_NO_ERROR;
}

//-----------------------------------------------------------------------------
// addon::GetChannelsAmount (CInstancePVRClient)
//
// Gets the total amount of channels on the backend
//
// Arguments:
//
//	amount		- Set to the number of available channels

PVR_ERROR addon::GetChannelsAmount(int& amount)
{
	// Wait until the channel information has been discovered the first time
	wait_for_channels();

	// Create a copy of the current PVR settings structure
	struct settings settings = copy_settings();

	try { amount = get_channel_count(connectionpool::handle(m_connpool), settings.show_drm_protected_channels); }
	catch(std::exception& ex) { return handle_stdexception(__func__, ex, PVR_ERROR::PVR_ERROR_FAILED); }
	catch(...) { return handle_generalexception(__func__, PVR_ERROR::PVR_ERROR_FAILED); }

	return PVR_ERROR::PVR_ERROR_NO_ERROR;
}

//-----------------------------------------------------------------------------
// addon::GetChannelStreamProperties (CInstancePVRClient)
//
// Get the stream properties for a channel from the backend
//
// Arguments:
//
//	channel		- channel to get the stream properties for
//	properties	- properties required to play the stream

PVR_ERROR addon::GetChannelStreamProperties(kodi::addon::PVRChannel const& channel, std::vector<kodi::addon::PVRStreamProperty>& properties)
{
	properties.emplace_back(PVR_STREAM_PROPERTY_MIMETYPE, channel.GetMimeType());
	properties.emplace_back(PVR_STREAM_PROPERTY_ISREALTIMESTREAM, "true");

	return PVR_ERROR::PVR_ERROR_NO_ERROR;
}

//-----------------------------------------------------------------------------
// addon::GetDriveSpace (CInstancePVRClient)
//
// Gets the disk space reported by the backend
//
// Arguments:
//
//	total		- The total disk space in bytes
//	used		- The used disk space in bytes

PVR_ERROR addon::GetDriveSpace(uint64_t& total, uint64_t& used)
{
	struct storage_space		space { 0, 0 };		// Disk space returned from database layer

	// Wait until the device information has been discovered for the first time
	wait_for_devices();

	try {
		
		// Attempt to get the available total and available space for the system, but return NOT_IMPLEMENTED
		// instead of an error code if the total value isn't available - this info wasn't always available
		space = get_available_storage_space(connectionpool::handle(m_connpool));
		if(space.total == 0) return PVR_ERROR::PVR_ERROR_NOT_IMPLEMENTED;

		// The reported values are multiplied by 1024 for some reason; accomodate the delta here
		total = space.total / 1024;
		used = (space.total - space.available) / 1024;	
	}

	catch(std::exception& ex) { return handle_stdexception(__func__, ex, PVR_ERROR_NOT_IMPLEMENTED); }
	catch(...) { return handle_generalexception(__func__, PVR_ERROR_NOT_IMPLEMENTED); }

	return PVR_ERROR::PVR_ERROR_NO_ERROR;
}

//-----------------------------------------------------------------------------
// addon::GetEPGForChannel (CInstancePVRClient)
//
// Request the EPG for a channel from the backend
//
// Arguments:
//
//	channelUid	- The UID of the channel to get the EPG table for
//	start		- Get events after this time (UTC)
//	end			- Get events before this time (UTC)
//	results		- List of EPG events to be transferred back to Kodi

PVR_ERROR addon::GetEPGForChannel(int channelUid, time_t start, time_t end, kodi::addon::PVREPGTagsResultSet& results)
{
	// Create a copy of the current addon settings structure
	struct settings settings = copy_settings();

	// Convert the channel identifier back into a channelid
	union channelid channelid{};
	channelid.value = channelUid;

	try {

		// Enumerate all of the listings in the database for this channel and time frame
		enumerate_listings(connectionpool::handle(m_connpool), settings.show_drm_protected_channels, channelid, start, end,
			[&](struct listing const& item, bool&) -> void {

			kodi::addon::PVREPGTag epgtag;					// PVREPGTag to be transferred to Kodi

			// Don't send EPG entries with start/end times outside the requested range
			if((item.starttime > end) || (item.endtime < start)) return;

			// UniqueBroadcastId (required)
			epgtag.SetUniqueBroadcastId(item.broadcastid);

			// UniqueChannelId (required)
			epgtag.SetUniqueChannelId(item.channelid);

			// Title (required)
			if(item.title == nullptr) return;
			epgtag.SetTitle(item.title);

			// StartTime
			epgtag.SetStartTime(static_cast<time_t>(item.starttime));

			// EndTime
			epgtag.SetEndTime(static_cast<time_t>(item.endtime));

			// Plot
			if(item.synopsis != nullptr) epgtag.SetPlot(item.synopsis);

			// Year
			//
			// Only report for program type "MV" (Movies)
			if((item.programtype != nullptr) && (strcasecmp(item.programtype, "MV") == 0)) epgtag.SetYear(item.year);

			// IconPath
			if(item.iconurl != nullptr) epgtag.SetIconPath(item.iconurl);

			// GenreType
			epgtag.SetGenreType((settings.use_backend_genre_strings) ? EPG_EVENT_CONTENTMASK::EPG_GENRE_USE_STRING : item.genretype);

			// GenreDescription
			if((settings.use_backend_genre_strings) && (item.genres != nullptr)) epgtag.SetGenreDescription(item.genres);

			// FirstAired
			//
			// Only report for program types "EP" (Series Episode) and "SH" (Show)
			if((item.programtype != nullptr) && ((strcasecmp(item.programtype, "EP") == 0) || (strcasecmp(item.programtype, "SH") == 0))) {

				// Special case: don't report original air date for listings of type EPG_EVENT_CONTENTMASK_NEWSCURRENTAFFAIRS
				// unless series/episode information is available
				if((item.genretype != EPG_EVENT_CONTENTMASK_NEWSCURRENTAFFAIRS) || ((item.seriesnumber >= 1) || (item.episodenumber >= 1))) {

					if(item.originalairdate != nullptr) epgtag.SetFirstAired(item.originalairdate);
				}
			}

			// SeriesNumber
			epgtag.SetSeriesNumber(item.seriesnumber);

			// EpisodeNumber
			epgtag.SetEpisodeNumber(item.episodenumber);

			// EpisodePartNumber
			epgtag.SetEpisodePartNumber(-1);

			// EpisodeName
			if(item.episodename != nullptr) epgtag.SetEpisodeName(item.episodename);

			// Flags
			epgtag.SetFlags(EPG_TAG_FLAG::EPG_TAG_FLAG_IS_SERIES);

			// SeriesLink
			if(item.seriesid != nullptr) epgtag.SetSeriesLink(item.seriesid);

			// StarRating
			epgtag.SetStarRating(item.starrating);

			results.Add(epgtag);
		});
	}

	catch(std::exception& ex) { return handle_stdexception(__func__, ex, PVR_ERROR::PVR_ERROR_FAILED); }
	catch(...) { return handle_generalexception(__func__, PVR_ERROR::PVR_ERROR_FAILED); }

	return PVR_ERROR::PVR_ERROR_NO_ERROR;
}

//-----------------------------------------------------------------------------
// addon::GetRecordingEdl (CInstancePVRClient)
//
// Retrieve the edit decision list (EDL) of a recording on the backend
//
// Arguments:
//
//	recording		- Recording to get the edit decision list for
//	edl				- Set to the EDL information for the recording

PVR_ERROR addon::GetRecordingEdl(kodi::addon::PVRRecording const& recording, std::vector<kodi::addon::PVREDLEntry>& edl)
{
	// edltype_to_string (local)
	//
	// Converts a PVR_EDL_TYPE enumeration value into a string
	auto edltype_to_string = [](PVR_EDL_TYPE const& type) -> char const* const {

		switch(type) {

			case PVR_EDL_TYPE::PVR_EDL_TYPE_CUT: return "CUT";
			case PVR_EDL_TYPE::PVR_EDL_TYPE_MUTE: return "MUTE";
			case PVR_EDL_TYPE::PVR_EDL_TYPE_SCENE: return "SCENE";
			case PVR_EDL_TYPE::PVR_EDL_TYPE_COMBREAK: return "COMBREAK";
		}

		return "<UNKNOWN>";
	};

	try {

		// Create a copy of the current addon settings structure and check if EDL is enabled
		struct settings settings = copy_settings();
		if(!settings.enable_recording_edl) return PVR_ERROR::PVR_ERROR_NOT_IMPLEMENTED;

		// Generate the base file name for the recording by combining the folder with the recording metadata
		std::string basename = get_recording_filename(connectionpool::handle(m_connpool), recording.GetRecordingId().c_str(), settings.recording_edl_folder_is_flat);
		if(basename.length() == 0) throw string_exception(__func__, ": unable to determine the base file name of the specified recording");

		// Remove any extension present on the base file name
		size_t extindex = basename.find_last_of('.');
		if(extindex != std::string::npos) basename = basename.substr(0, extindex);

		// Attempt to locate a matching .EDL file based on the configured directories
		std::string filename = settings.recording_edl_folder.append(basename).append(".edl");
		if(!kodi::vfs::FileExists(filename, false)) {

			// Check secondary EDL directory
			filename = settings.recording_edl_folder_2.append(basename).append(".edl");
			if(!kodi::vfs::FileExists(filename, false)) {

				// Check tertiary EDL directory
				filename = settings.recording_edl_folder_3.append(basename).append(".edl");
				if(!kodi::vfs::FileExists(filename, false)) {

					// If the .EDL file was not found anywhere, log a notice but return NO_ERROR back to Kodi -- this is not fatal
					log_info(__func__, ": edit decision list for recording ", basename.c_str(), " was not found in any configured EDL file directories");
					return PVR_ERROR::PVR_ERROR_NOT_IMPLEMENTED;
				}
			}
		}

		// Attempt to open the input edit decision list file
		kodi::vfs::CFile edlfile;
		if(edlfile.OpenFile(filename, 0)) {

			std::string		line;					// Line of text from the EDL file
			size_t			linenumber = 0;			// Current line number in the EDL file

			log_info(__func__, ": processing edit decision list file: ", filename.c_str());

			// Process each line of the file individually
			while(edlfile.ReadLine(line)) {

				++linenumber;									// Increment the line number

				float			start	= 0.0F;					// Starting point, in milliseconds
				float			end		= 0.0F;					// Ending point, in milliseconds
				int				type	= PVR_EDL_TYPE_CUT;		// Type of edit to be made

				// The only currently supported format for EDL is the {float|float|[int]} format, as the
				// frame rate of the recording would be required to process the {#frame|#frame|[int]} format
				if(sscanf(line.c_str(), "%f %f %i", &start, &end, &type) >= 2) {

					// Apply any user-specified adjustments to the start and end times accordingly
					start += (static_cast<float>(settings.recording_edl_start_padding) / 1000.0F);
					end -= (static_cast<float>(settings.recording_edl_end_padding) / 1000.0F);
						
					// Ensure the start and end times are positive and do not overlap
					start = std::min(std::max(start, 0.0F), std::max(end, 0.0F));
					end = std::max(std::max(end, 0.0F), std::max(start, 0.0F));

					// Replace CUT indicators with COMSKIP indicators if requested
					if((static_cast<PVR_EDL_TYPE>(type) == PVR_EDL_TYPE_CUT) && (settings.recording_edl_cut_as_comskip)) 
						type = static_cast<int>(PVR_EDL_TYPE_COMBREAK);

					// Log the adjusted values for the entry
					log_info(__func__, ": adding edit decision list entry (start=", start, "s, end=", end, "s, type=", 
						edltype_to_string(static_cast<PVR_EDL_TYPE>(type)), ")");

					// Add a PVREDLEntry into the results vector<>
					kodi::addon::PVREDLEntry entry;
					entry.SetStart(static_cast<int64_t>(static_cast<double>(start) * 1000.0));
					entry.SetEnd(static_cast<int64_t>(static_cast<double>(end) * 1000.0));
					entry.SetType(static_cast<PVR_EDL_TYPE>(type));

					edl.emplace_back(std::move(entry));
				}

				else log_error(__func__, ": invalid edit decision list entry detected at line #", linenumber);
			}
			
			edlfile.Close();
		}

		else log_error(__func__, ": unable to open edit decision list file: ", filename.c_str());
	}
	
	catch(std::exception& ex) { return handle_stdexception(__func__, ex, PVR_ERROR::PVR_ERROR_FAILED); }
	catch(...) { return handle_generalexception(__func__, PVR_ERROR::PVR_ERROR_FAILED); }

	return PVR_ERROR::PVR_ERROR_NO_ERROR;
}

//-----------------------------------------------------------------------------
// addon::GetRecordingLastPlayedPosition (CInstancePVRClient)
//
// Retrieve the last watched position of a recording on the backend
//
// Arguments:
//
//	recording		- Recording to get the last played position for
//	position		- Set to the last played position

PVR_ERROR addon::GetRecordingLastPlayedPosition(kodi::addon::PVRRecording const& recording, int& position)
{
	// NOTE: There is a race condition during startup with this function if Kodi asks for this information
	// while a startup task like XMLTV listing discovery is still executing which can cause SQLITE_BUSY.
	// Avoid this condition by only allowing a refresh of the information if startup has fully completed.

	try { position = get_recording_lastposition(connectionpool::handle(m_connpool), m_startup_complete.load(), recording.GetRecordingId().c_str()); }
	catch(std::exception& ex) { return handle_stdexception(__func__, ex, PVR_ERROR::PVR_ERROR_FAILED); }
	catch(...) { return handle_generalexception(__func__, PVR_ERROR::PVR_ERROR_FAILED); }

	return PVR_ERROR::PVR_ERROR_NO_ERROR;
}

//-----------------------------------------------------------------------------
// addon::GetRecordings (CInstancePVRClient)
//
// Request the list of all recordings from the backend
//
// Arguments:
//
//	deleted		- If set, return deleted recordings
//	results		- Recordings result set to be loaded

PVR_ERROR addon::GetRecordings(bool deleted, kodi::addon::PVRRecordingsResultSet& results)
{
	// The PVR doesn't support tracking deleted recordings
	if(deleted) return PVR_ERROR::PVR_ERROR_NO_ERROR;

	// Wait until the recording information has been discovered the first time
	wait_for_recordings();

	// Create a copy of the current addon settings structure
	struct settings settings = copy_settings();

	try {

		std::unique_lock<std::mutex> lock(m_radiomappings_lock);		// Prevent updates to the mappings

		// Enumerate all of the recordings in the database
		enumerate_recordings(connectionpool::handle(m_connpool), settings.use_episode_number_as_title, settings.disable_recording_categories, 
			[&](struct recording const& item) -> void {

			kodi::addon::PVRRecording	recording;			// PVRRecording to be transferred to Kodi

			// Determine if the channel should be mapped as a Radio channel instead of a TV channel
			bool isradiochannel = (settings.enable_radio_channel_mapping && is_channel_radio(lock, item.channelid));

			// Determine if the episode is a repeat.  If the program type is "EP" or "SH" and firstairing is *not* set, flag it as a repeat
			bool isrepeat = ((item.programtype != nullptr) && ((strcasecmp(item.programtype, "EP") == 0) || (strcasecmp(item.programtype, "SH") == 0)) && (item.firstairing == 0));

			// RecordingId (required)
			if(item.recordingid == nullptr) return;
			recording.SetRecordingId(item.recordingid);

			// Title (required)
			if(item.title == nullptr) return;
			recording.SetTitle(item.title);

			// EpisodeName
			if(item.episodename != nullptr) {

				char strEpisodeName[PVR_ADDON_NAME_STRING_LENGTH]{};

				snprintf(strEpisodeName, std::extent<decltype(strEpisodeName)>::value, "%s%s", item.episodename, 
					((isrepeat) && (settings.generate_repeat_indicators)) ? " [R]" : "");
				recording.SetEpisodeName(strEpisodeName);
			}

			// SeriesNumber
			recording.SetSeriesNumber(item.seriesnumber);

			// EpisodeNumber
			recording.SetEpisodeNumber(item.episodenumber);

			// Year
			//
			// Only report for program type "MV" (Movies)
			if((item.programtype != nullptr) && (strcasecmp(item.programtype, "MV") == 0)) recording.SetYear(item.year);

			// Directory
			if(item.directory != nullptr) {

				// Special cases: "movie", "sport", "special", and "news"
				if(strcasecmp(item.directory, "movie") == 0) recording.SetDirectory(kodi::GetLocalizedString(30402));
				else if(strcasecmp(item.directory, "sport") == 0) recording.SetDirectory(kodi::GetLocalizedString(30403));
				else if(strcasecmp(item.directory, "special") == 0) recording.SetDirectory(kodi::GetLocalizedString(30404));
				else if(strcasecmp(item.directory, "news") == 0) recording.SetDirectory(kodi::GetLocalizedString(30405));

				else recording.SetDirectory(item.directory);
			}

			// Plot
			if(item.plot != nullptr) recording.SetPlot(item.plot);

			// ChannelName
			if(item.channelname != nullptr) recording.SetChannelName(item.channelname);

			// ThumbnailPath
			if(item.thumbnailpath != nullptr) recording.SetThumbnailPath(item.thumbnailpath);

			// RecordingTime
			recording.SetRecordingTime(static_cast<time_t>(item.recordingtime));
			if((item.category != nullptr) && (settings.use_airdate_as_recordingdate) && (item.originalairdate > 0)) {

				// Only apply use_airdate_as_recordindate to items with a program type of "EP" or "SH"
				if((item.programtype != nullptr) && ((strcasecmp(item.programtype, "EP") == 0) || (strcasecmp(item.programtype, "SH") == 0))) {

					time_t epoch = static_cast<time_t>(item.originalairdate);
					recording.SetRecordingTime(mktime(gmtime(&epoch)));
				}
			}

			// Duration
			recording.SetDuration(item.duration);
			assert(recording.GetDuration() > 0);

			// PlayCount
			//
			recording.SetPlayCount((item.lastposition == std::numeric_limits<uint32_t>::max()) ? 1 : 0);

			// LastPlayedPosition
			//
			recording.SetLastPlayedPosition((item.lastposition == std::numeric_limits<uint32_t>::max()) ? 0 : item.lastposition);

			// ChannelUid
			recording.SetChannelUid(item.channelid.value);

			// ChannelType
			recording.SetChannelType((isradiochannel) ? PVR_RECORDING_CHANNEL_TYPE::PVR_RECORDING_CHANNEL_TYPE_RADIO :
				PVR_RECORDING_CHANNEL_TYPE::PVR_RECORDING_CHANNEL_TYPE_TV);

			// FirstAired
			//
			// Only report for program types "EP" (Series Episode) and "SH" (Show)
			if((item.programtype != nullptr) && (item.originalairdate > 0) && ((strcasecmp(item.programtype, "EP") == 0) || (strcasecmp(item.programtype, "SH") == 0))) {

				// Special case: omit for "news" category programs, these programs are EP/SH programs, but the original
				// air date will typically be set to the first broadcast date, which makes no sense for recordings
				if((item.directory != nullptr) && (strcasecmp(item.directory, "news") != 0)) {

					char strFirstAired[16]{};

					time_t epoch = static_cast<time_t>(item.originalairdate);
					strftime(strFirstAired, std::extent<decltype(strFirstAired)>::value, "%Y-%m-%d", gmtime(&epoch));
					recording.SetFirstAired(strFirstAired);
				}
			}

			results.Add(recording);
		});
	}

	catch(std::exception& ex) { return handle_stdexception(__func__, ex, PVR_ERROR::PVR_ERROR_FAILED); }
	catch(...) { return handle_generalexception(__func__, PVR_ERROR::PVR_ERROR_FAILED); }

	return PVR_ERROR::PVR_ERROR_NO_ERROR;
}

//-----------------------------------------------------------------------------
// addon::GetRecordingsAmount (CInstancePVRClient)
//
// Gets the amount of recordings present on backend
//
// Arguments:
//
//	deleted		- If set, return deleted recordings
//	amount		- Set to the amount of recordings available

PVR_ERROR addon::GetRecordingsAmount(bool deleted, int& amount)
{
	// Deleted recordings aren't supported
	if(deleted) return PVR_ERROR::PVR_ERROR_NO_ERROR;

	// Wait until the recording information has been discovered the first time
	wait_for_recordings();

	try { amount = get_recording_count(connectionpool::handle(m_connpool)); }
	catch(std::exception& ex) { return handle_stdexception(__func__, ex, PVR_ERROR::PVR_ERROR_FAILED); }
	catch(...) { return handle_generalexception(__func__, PVR_ERROR::PVR_ERROR_FAILED); }

	return PVR_ERROR::PVR_ERROR_NO_ERROR;
}

//-----------------------------------------------------------------------------
// addon::GetRecordingStreamProperties (CInstancePVRClient)
//
// Get the stream properties for a recording from the backend
//
// Arguments:
//
//	channel		- channel to get the stream properties for
//	properties	- properties required to play the stream

PVR_ERROR addon::GetRecordingStreamProperties(kodi::addon::PVRRecording const& recording, std::vector<kodi::addon::PVRStreamProperty>& properties)
{
	// Determine if the recording will be realtime or not based on the end time
	bool isrealtime = ((recording.GetRecordingTime() + recording.GetDuration()) > time(nullptr));

	properties.emplace_back(PVR_STREAM_PROPERTY_MIMETYPE, "video/mp2t");
	properties.emplace_back(PVR_STREAM_PROPERTY_ISREALTIMESTREAM, (isrealtime) ? "true" : "false");

	return PVR_ERROR::PVR_ERROR_NO_ERROR;
}

//-----------------------------------------------------------------------------
// addon::GetStreamReadChunkSize (CInstancePVRClient)
//
// Obtain the chunk size to use when reading streams
//
// Arguments:
//
//	chunksize	- Set to the stream read chunk size

PVR_ERROR addon::GetStreamReadChunkSize(int& chunksize)
{
	// Only report this as implemented if not set to 'Automatic'
	int stream_read_chunk_size = copy_settings().stream_read_chunk_size;
	if(stream_read_chunk_size == 0) return PVR_ERROR_NOT_IMPLEMENTED;

	chunksize = stream_read_chunk_size;
	return PVR_ERROR::PVR_ERROR_NO_ERROR;
}

//-----------------------------------------------------------------------------
// addon::GetStreamTimes (CInstancePVRClient)
//
// Get stream times
//
// Arguments:
//
//	times		- Stream time information to be described

PVR_ERROR addon::GetStreamTimes(kodi::addon::PVRStreamTimes& times)
{
	assert(m_stream_starttime <= m_stream_endtime);

	// Block this function for non-seekable streams otherwise Kodi will allow those operations
	if((!m_pvrstream) || (!m_pvrstream->canseek())) return PVR_ERROR::PVR_ERROR_NOT_IMPLEMENTED;

	// SPECIAL CASE: If start time and end time are the same, let Kodi handle it. 
	// This can happen if the duration of a recorded stream was not reported properly (credit: timecutter)
	if(m_stream_starttime == m_stream_endtime) return PVR_ERROR::PVR_ERROR_NOT_IMPLEMENTED;

	// Set the start time to the actual start time (UTC) for live streams, otherwise zero
	// Using zero here is required to enable calls to SetRecordingLastPlayedPosition()
	times.SetStartTime((m_stream_endtime == std::numeric_limits<time_t>::max()) ? m_stream_starttime : 0);

	times.SetPTSStart(0);						// Starting PTS gets set to zero
	times.SetPTSBegin(0);						// Timeshift buffer start PTS also gets set to zero

	// Set the timeshift duration to the delta between the start time and the lesser of the 
	// current wall clock time or the known stream end time
	time_t now = time(nullptr);
	times.SetPTSEnd(static_cast<int64_t>(((now < m_stream_endtime) ? now : m_stream_endtime) - m_stream_starttime) * STREAM_TIME_BASE);

	return PVR_ERROR::PVR_ERROR_NO_ERROR;
}

//-----------------------------------------------------------------------------
// addon::GetTimers (CInstancePVRClient)
//
// Request the list of all timers from the backend
//
// Arguments:
//
//	results		- Timers result set to be loaded

PVR_ERROR addon::GetTimers(kodi::addon::PVRTimersResultSet& results)
{
	// Wait until the timer information has been discovered the first time
	wait_for_timers();

	time_t now = time(nullptr);				// Get the current date/time for comparison

	// Create a copy of the current addon settings structure
	struct settings settings = copy_settings();

	try {

		// Pull a database connection out from the connection pool
		connectionpool::handle dbhandle(m_connpool);

		// Enumerate all of the recording rules in the database
		enumerate_recordingrules(dbhandle, [&](struct recordingrule const& item) -> void {

			kodi::addon::PVRTimer timer;				// PVRTimer to be transferred to Kodi

			// ClientIndex (required)
			timer.SetClientIndex(item.recordingruleid);

			// ClientChannelUid
			timer.SetClientChannelUid(static_cast<int>(item.channelid.value));

			// StartTime
			timer.SetStartTime((item.type == recordingrule_type::datetimeonly) ? static_cast<time_t>(item.datetimeonly) : now);

			// StartAnyTime
			timer.SetStartAnyTime((item.type == recordingrule_type::series));

			// EndAnyTime
			timer.SetEndAnyTime(true);

			// State (required)
			timer.SetState(PVR_TIMER_STATE::PVR_TIMER_STATE_SCHEDULED);

			// TimerType (required)
			timer.SetTimerType((item.type == recordingrule_type::series) ? timer_type::seriesrule : timer_type::datetimeonlyrule);

			// Title (required)
			if(item.title != nullptr) timer.SetTitle(item.title);

			// EPGSearchString
			if(item.title != nullptr) timer.SetEPGSearchString(item.title);

			// FirstDay
			if(item.type == recordingrule_type::datetimeonly) timer.SetFirstDay(static_cast<time_t>(item.datetimeonly));

			// PreventDuplicateEpisodes
			if(item.type == recordingrule_type::series) {

				if(item.afteroriginalairdateonly > 0) timer.SetPreventDuplicateEpisodes(duplicate_prevention::newonly);
				else if(item.recentonly) timer.SetPreventDuplicateEpisodes(duplicate_prevention::recentonly);
				else timer.SetPreventDuplicateEpisodes(duplicate_prevention::none);
			}

			// MarginStart
			timer.SetMarginStart(item.startpadding / 60);

			// MarginEnd
			timer.SetMarginEnd(item.endpadding / 60);

			// SeriesLink
			if(item.seriesid != nullptr) timer.SetSeriesLink(item.seriesid);

			results.Add(timer);
		});

		// Enumerate all of the timers in the database
		enumerate_timers(dbhandle, m_epgmaxtime.load(), [&](struct timer const& item) -> void {

			kodi::addon::PVRTimer timer;				// PVRTimer to be transferred to Kodi

			// ClientIndex (required)
			timer.SetClientIndex(item.timerid);

			// ParentClientIndex
			timer.SetParentClientIndex(item.recordingruleid);

			// ClientChannelUid
			timer.SetClientChannelUid(static_cast<int>(item.channelid.value));

			// StartTime
			timer.SetStartTime(static_cast<time_t>(item.starttime) - 
				((settings.use_actual_timer_times) ? static_cast<time_t>(item.startpadding) : 0));

			// EndTime
			timer.SetEndTime(static_cast<time_t>(item.endtime) + 
				((settings.use_actual_timer_times) ? static_cast<time_t>(item.endpadding) : 0));

			// State (required)
			if(timer.GetEndTime() < now) timer.SetState(PVR_TIMER_STATE::PVR_TIMER_STATE_COMPLETED);
			else if((now >= timer.GetStartTime()) && (now <= timer.GetEndTime())) timer.SetState(PVR_TIMER_STATE::PVR_TIMER_STATE_RECORDING);
			else timer.SetState(PVR_TIMER_STATE::PVR_TIMER_STATE_SCHEDULED);

			// TimerType (required)
			timer.SetTimerType((item.parenttype == recordingrule_type::series) ? timer_type::seriestimer : timer_type::datetimeonlytimer);

			// Title (required)
			if(item.title != nullptr) timer.SetTitle(item.title);

			// EPGUid
			timer.SetEPGUid(static_cast<unsigned int>(item.starttime));

			// SeriesLink
			if(item.seriesid != nullptr) timer.SetSeriesLink(item.seriesid);

			results.Add(timer);
		});
	}

	catch(std::exception& ex) { return handle_stdexception(__func__, ex, PVR_ERROR::PVR_ERROR_FAILED); }
	catch(...) { return handle_generalexception(__func__, PVR_ERROR::PVR_ERROR_FAILED); }

	return PVR_ERROR::PVR_ERROR_NO_ERROR;
}

//-----------------------------------------------------------------------------
// addon::GetTimersAmount (CInstancePVRClient)
//
// Gets the total amount of timers on the backend
//
// Arguments:
//
//	amount		- Set to the number of available timers

PVR_ERROR addon::GetTimersAmount(int& amount)
{
	// Wait until the timer information has been discovered the first time
	wait_for_timers();

	try {

		// Pull a database connection out from the connection pool
		connectionpool::handle dbhandle(m_connpool);

		// Return the sum of the timer rules and the invidual timers themselves
		amount = get_recordingrule_count(dbhandle) + get_timer_count(dbhandle, m_epgmaxtime.load());
	}

	catch(std::exception& ex) { return handle_stdexception(__func__, ex, PVR_ERROR::PVR_ERROR_FAILED); }
	catch(...) { return handle_generalexception(__func__, PVR_ERROR::PVR_ERROR_FAILED); }

	return PVR_ERROR::PVR_ERROR_NO_ERROR;
}

//-----------------------------------------------------------------------------
// addon::GetTimerTypes (CInstancePVRClient)
//
// Retrieve the timer types supported by the backend
//
// Arguments:
//
//	types		- vector<> to load with the supported timer types

PVR_ERROR addon::GetTimerTypes(std::vector<kodi::addon::PVRTimerType>& types)
{
	kodi::addon::PVRTimerType	seriesrule;				// timer_type::seriesrule
	kodi::addon::PVRTimerType	datetimeonlyrule;		// timer_type::datetimeonlyrule
	kodi::addon::PVRTimerType	epgseriesrule;			// timer_type::epgseriesrule
	kodi::addon::PVRTimerType	epgdatetimeonlyrule;	// timer_type::epgdatetimeonlyrule
	kodi::addon::PVRTimerType	seriestimer;			// timer_type::seriestimer
	kodi::addon::PVRTimerType	datetimeonlytimer;		// timer_type::datetimeonlytimer

	// Define the possible duplicate prevention values for series rules
	std::vector<kodi::addon::PVRTypeIntValue> preventDuplicates;
	preventDuplicates.emplace_back(duplicate_prevention::none, "Record all episodes");
	preventDuplicates.emplace_back(duplicate_prevention::newonly, "Record only new episodes");
	preventDuplicates.emplace_back(duplicate_prevention::recentonly, "Record only recent episodes");

	// seriesrule
	//
	// Timer type for non-EPG series rules, requires a series link or name match operation to create. Can be both edited and deleted.
	seriesrule.SetId(timer_type::seriesrule);
	seriesrule.SetAttributes(PVR_TIMER_TYPE_IS_REPEATING | PVR_TIMER_TYPE_SUPPORTS_CHANNELS | PVR_TIMER_TYPE_SUPPORTS_TITLE_EPG_MATCH |
		PVR_TIMER_TYPE_SUPPORTS_RECORD_ONLY_NEW_EPISODES | PVR_TIMER_TYPE_SUPPORTS_START_END_MARGIN | PVR_TIMER_TYPE_FORBIDS_EPG_TAG_ON_CREATE |
		PVR_TIMER_TYPE_SUPPORTS_ANY_CHANNEL);
	seriesrule.SetDescription("Record Series Rule");
	seriesrule.SetPreventDuplicateEpisodes(preventDuplicates, duplicate_prevention::none);
	types.emplace_back(std::move(seriesrule));

	// datetimeonlyrule
	//
	// Timer type for non-EPG date time only rules, requires a series link or name match operation to create. Cannot be edited but can be deleted.
	datetimeonlyrule.SetId(timer_type::datetimeonlyrule);
	datetimeonlyrule.SetAttributes(PVR_TIMER_TYPE_IS_REPEATING | PVR_TIMER_TYPE_IS_READONLY | PVR_TIMER_TYPE_SUPPORTS_CHANNELS |
		PVR_TIMER_TYPE_SUPPORTS_TITLE_EPG_MATCH | PVR_TIMER_TYPE_SUPPORTS_FIRST_DAY | PVR_TIMER_TYPE_SUPPORTS_START_TIME | 
		PVR_TIMER_TYPE_SUPPORTS_START_END_MARGIN | PVR_TIMER_TYPE_FORBIDS_EPG_TAG_ON_CREATE | PVR_TIMER_TYPE_SUPPORTS_READONLY_DELETE);
	datetimeonlyrule.SetDescription("Record Once Rule");
	types.emplace_back(std::move(datetimeonlyrule));

	// epgseriesrule
	//
	// Timer type for EPG series rules
	epgseriesrule.SetId(timer_type::epgseriesrule);
	epgseriesrule.SetAttributes(PVR_TIMER_TYPE_IS_REPEATING | PVR_TIMER_TYPE_SUPPORTS_CHANNELS | PVR_TIMER_TYPE_SUPPORTS_RECORD_ONLY_NEW_EPISODES |
		PVR_TIMER_TYPE_SUPPORTS_START_END_MARGIN | PVR_TIMER_TYPE_REQUIRES_EPG_SERIES_ON_CREATE | PVR_TIMER_TYPE_SUPPORTS_ANY_CHANNEL);
	epgseriesrule.SetDescription("Record Series");
	epgseriesrule.SetPreventDuplicateEpisodes(preventDuplicates, duplicate_prevention::none);
	types.emplace_back(std::move(epgseriesrule));

	// epgdatetimeonlyrule
	//
	// Timer type for EPG date time only rules
	epgdatetimeonlyrule.SetId(timer_type::epgdatetimeonlyrule);
	epgdatetimeonlyrule.SetAttributes(PVR_TIMER_TYPE_SUPPORTS_CHANNELS | PVR_TIMER_TYPE_SUPPORTS_START_END_MARGIN | PVR_TIMER_TYPE_REQUIRES_EPG_SERIES_ON_CREATE);
	epgdatetimeonlyrule.SetDescription("Record Once");
	types.emplace_back(std::move(epgdatetimeonlyrule));

	// seriestimer
	//
	// Used for existing episode timers; these cannot be edited or deleted
	seriestimer.SetId(timer_type::seriestimer);
	seriestimer.SetAttributes(PVR_TIMER_TYPE_IS_READONLY | PVR_TIMER_TYPE_FORBIDS_NEW_INSTANCES | PVR_TIMER_TYPE_SUPPORTS_CHANNELS | 
		PVR_TIMER_TYPE_SUPPORTS_START_TIME | PVR_TIMER_TYPE_SUPPORTS_END_TIME);
	seriestimer.SetDescription("Record Series Episode");
	types.emplace_back(std::move(seriestimer));

	// datetimeonlytimer
	//
	// Used for existing date/time only episode timers; these cannot be edited or deleted
	datetimeonlytimer.SetId(timer_type::datetimeonlytimer);
	datetimeonlytimer.SetAttributes(PVR_TIMER_TYPE_IS_READONLY | PVR_TIMER_TYPE_FORBIDS_NEW_INSTANCES | PVR_TIMER_TYPE_SUPPORTS_CHANNELS | 
		PVR_TIMER_TYPE_SUPPORTS_START_TIME | PVR_TIMER_TYPE_SUPPORTS_END_TIME);
	datetimeonlytimer.SetDescription("Record Once Episode");
	types.emplace_back(std::move(datetimeonlytimer));

	return PVR_ERROR::PVR_ERROR_NO_ERROR;
}

//-----------------------------------------------------------------------------
// addon::IsRealTimeStream (CInstancePVRClient)
//
// Check for real-time streaming
//
// Arguments:
//
//	NONE

bool addon::IsRealTimeStream(void)
{
	try { return (m_pvrstream) ? m_pvrstream->realtime() : false; }
	catch(std::exception& ex) { return handle_stdexception(__func__, ex, false); }
	catch(...) { return handle_generalexception(__func__, false); }
}

//-----------------------------------------------------------------------------
// addon::LengthLiveStream (CInstancePVRClient)
//
// Obtain the length of a live stream
//
// Arguments:
//
//	NONE

int64_t addon::LengthLiveStream(void)
{
	try { return (m_pvrstream) ? m_pvrstream->length() : -1; }
	catch(std::exception& ex) { return handle_stdexception(__func__, ex, -1); }
	catch(...) { return handle_generalexception(__func__, -1); }
}

//-----------------------------------------------------------------------------
// addon::LengthRecordedStream (CInstancePVRClient)
//
// Obtain the length of a recorded stream
//
// Arguments:
//
//	NONE

int64_t addon::LengthRecordedStream(void)
{
	return LengthLiveStream();			// Identical implementation
}

//-----------------------------------------------------------------------------
// addon::OnSystemSleep (CInstancePVRClient)
//
// Notification of system sleep power event
//
// Arguments:
//
//	NONE

PVR_ERROR addon::OnSystemSleep(void)
{
	// CAUTION: This function will be called on a different thread than the main PVR
	// callback functions -- do not attempt to manipulate any in-progress streams

	try {

		m_scheduler.stop();				// Stop the scheduler
		m_scheduler.clear();			// Clear out any pending tasks
	}

	catch(std::exception& ex) { return handle_stdexception(__func__, ex, PVR_ERROR::PVR_ERROR_FAILED); }
	catch(...) { return handle_generalexception(__func__, PVR_ERROR::PVR_ERROR_FAILED); }

	return PVR_ERROR::PVR_ERROR_NO_ERROR;
}

//-----------------------------------------------------------------------------
// addon::GetTimerTypes (CInstancePVRClient)
//
// Notification of system wake power event
//
// Arguments:
//
//	NONE

PVR_ERROR addon::OnSystemWake(void)
{
	using namespace std::chrono;

	// CAUTION: This function will be called on a different thread than the main PVR
	// callback functions -- do not attempt to manipulate any in-progress streams

	try {

		m_scheduler.stop();					// Ensure scheduler has been stopped
		m_scheduler.clear();				// Ensure there are no pending tasks

		// Systems with a low precision system_clock implementation may run the tasks out of order,
		// account for this by using a base time with a unique millisecond offset during scheduling
		auto now = system_clock::now();

		// Schedule a task to wait for the network to become available
		m_scheduler.add(now, std::bind(&addon::wait_for_network_task, this, 60, std::placeholders::_1));

		// Schedule the normal update tasks for everything in an appropriate order
		m_scheduler.add(UPDATE_DEVICES_TASK, now + milliseconds(1), &addon::update_devices_task, this);
		m_scheduler.add(UPDATE_LINEUPS_TASK, now + milliseconds(2), &addon::update_lineups_task, this);
		m_scheduler.add(UPDATE_RECORDINGS_TASK, now + milliseconds(3), &addon::update_recordings_task, this);
		m_scheduler.add(UPDATE_RECORDINGRULES_TASK, now + milliseconds(4), &addon::update_recordingrules_task, this);
		m_scheduler.add(UPDATE_EPISODES_TASK, now + milliseconds(5), &addon::update_episodes_task, this);

		// A listings update may have been scheduled by update_lineups_task with a channel check set;
		// adding it again may override that task, so perform a missing channel check here as well
		m_scheduler.add(UPDATE_LISTINGS_TASK, now + milliseconds(6), std::bind(&addon::update_listings_task, this, false, true, std::placeholders::_1));

		// Restart the task scheduler
		m_scheduler.start();
	}

	catch(std::exception& ex) { return handle_stdexception(__func__, ex, PVR_ERROR::PVR_ERROR_FAILED); }
	catch(...) { return handle_generalexception(__func__, PVR_ERROR::PVR_ERROR_FAILED); }

	return PVR_ERROR::PVR_ERROR_NO_ERROR;
}

//-----------------------------------------------------------------------------
// addon::OpenLiveStream (CInstancePVRClient)
//
// Open a live stream on the backend
//
// Arguments:
//
//	channel		- Channel of the live stream to be opened

bool addon::OpenLiveStream(kodi::addon::PVRChannel const& channel)
{
	char				vchannel[64];		// Virtual channel number

	// Create a copy of the current addon settings structure
	struct settings settings = copy_settings();

	// DRM channels are flagged with a non-zero iEncryptionSystem value to prevent playback.  This can be overriden
	// if direct-tuning is enabled to allow for channels that are improperly flagged as DRM by the tuner device(s)
	if(channel.GetEncryptionSystem() != 0) {
		
		if((!settings.use_direct_tuning) || (!settings.direct_tuning_allow_drm)) {

			std::string text = "Channel " + channel.GetChannelName() + " is flagged as DRM protected content and cannot be played";
			kodi::gui::dialogs::OK::ShowAndGetInput("DRM Protected Content", text);
			return false;
		}
	}

	// The only interesting thing about PVR_CHANNEL is the channel id
	union channelid channelid{};
	channelid.value = channel.GetUniqueId();

	// Generate a string version of the channel number to represent the virtual channel number
	if(channelid.parts.subchannel == 0) snprintf(vchannel, std::extent<decltype(vchannel)>::value, "%d", channelid.parts.channel);
	else snprintf(vchannel, std::extent<decltype(vchannel)>::value, "%d.%d", channelid.parts.channel, channelid.parts.subchannel);

	try {

		// Pull a database connection out from the connection pool
		connectionpool::handle dbhandle(m_connpool);

		// Determine if HTTP can be used from the storage engine and/or the tuner directly. Tuner HTTP can be used as a fallback
		// for a failed storage stream or if use_direct_tuning is enabled and HTTP is the preferred protocol
		bool use_storage_http = ((settings.use_direct_tuning == false) && (get_tuner_direct_channel_flag(dbhandle, channelid) == false));
		bool use_tuner_http = (use_storage_http || settings.direct_tuning_protocol == tuning_protocol::http);

		// Attempt to create the stream from the storage engine via HTTP if available
		if(use_storage_http) m_pvrstream = openlivestream_storage_http(dbhandle, channelid, vchannel);
		
		// Attempt to create the stream from the tuner via HTTP if available
		if((!m_pvrstream) && (use_tuner_http)) m_pvrstream = openlivestream_tuner_http(dbhandle, channelid, vchannel);
		
		// Attempt to create the stream from the tuner via RTP/UDP (always available)
		if(!m_pvrstream) m_pvrstream = openlivestream_tuner_device(dbhandle, channelid, vchannel);

		// If none of the above methods generated a valid stream, there is nothing left to try
		if(!m_pvrstream) throw string_exception(__func__, ": unable to create a valid stream instance for channel ", vchannel);

		// If this is a radio channel, check to see if the user wants to remove the video stream(s)
		if(channel.GetIsRadio() && m_settings.enable_radio_channel_mapping && m_settings.block_radio_channel_video_streams) {

			log_info(__func__, ": channel is marked as radio, applying MPEG-TS video stream filter");
			m_pvrstream = radiofilter::create(std::move(m_pvrstream));
		}

		// Pause the scheduler if the user wants that functionality disabled during streaming
		if(settings.pause_discovery_while_streaming) m_scheduler.pause();

		try {

			// For live streams, set the start time to now and set the end time to time_t::max()
			m_stream_starttime = time(nullptr);
			m_stream_endtime = std::numeric_limits<time_t>::max();

			// Log some additional information about the stream for diagnostic purposes
			log_info(__func__, ": mediatype = ", m_pvrstream->mediatype());
			log_info(__func__, ": canseek   = ", m_pvrstream->canseek());
			log_info(__func__, ": length    = ", m_pvrstream->length());
			log_info(__func__, ": realtime  = ", m_pvrstream->realtime());
			log_info(__func__, ": starttime = ", m_stream_starttime, " (epoch) = ", strtok(asctime(localtime(&m_stream_starttime)), "\n"), " (local)");
		}

		catch(...) { m_pvrstream.reset(); m_scheduler.resume(); throw; }

		return true;
	}

	// Queue a notification for the user when a live stream cannot be opened, don't just silently log it
	catch(std::exception& ex) { 
		
		kodi::QueueFormattedNotification(QueueMsg::QUEUE_ERROR, "Live Stream creation failed (%s).", ex.what());
		return handle_stdexception(__func__, ex, false); 
	}

	catch(...) { return handle_generalexception(__func__, false); }
}

//-----------------------------------------------------------------------------
// addon::OpenRecordedStream (CInstancePVRClient)
//
// Open a stream to a recording on the backend
//
// Arguments:
//
//	recording	- Recording stream to be opened

bool addon::OpenRecordedStream(kodi::addon::PVRRecording const& recording)
{
	// Create a copy of the current addon settings structure
	struct settings settings = copy_settings();

	try {

		// Pull a database connection out from the connection pool
		connectionpool::handle dbhandle(m_connpool);

		// Generate the stream URL for the specified channel
		std::string streamurl = get_recording_stream_url(dbhandle, recording.GetRecordingId().c_str());
		if(streamurl.length() == 0) throw string_exception(__func__, ": unable to determine the URL for specified recording");

		// Pause the scheduler if the user wants that functionality disabled during streaming
		if(settings.pause_discovery_while_streaming) m_scheduler.pause();

		try {

			// Start the new recording stream using the tuning parameters currently specified by the settings
			log_info(__func__, ": streaming recording '", recording.GetTitle().c_str(), "' via url ", streamurl.c_str());
			m_pvrstream = httpstream::create(streamurl.c_str());

			// If this is a radio channel, check to see if the user wants to remove the video stream(s)
			if(recording.GetChannelType() == PVR_RECORDING_CHANNEL_TYPE::PVR_RECORDING_CHANNEL_TYPE_RADIO && 
				m_settings.enable_radio_channel_mapping && m_settings.block_radio_channel_video_streams) {

				log_info(__func__, ": channel is marked as radio, applying MPEG-TS video stream filter");
				m_pvrstream = radiofilter::create(std::move(m_pvrstream));
			}

			// For recorded streams, set the start and end times based on the recording metadata. Don't use the
			// start time value in PVR_RECORDING; that may have been altered for display purposes
			m_stream_starttime = get_recording_time(dbhandle, recording.GetRecordingId().c_str());
			m_stream_endtime = m_stream_starttime + recording.GetDuration();

			// Log some additional information about the stream for diagnostic purposes
			log_info(__func__, ": mediatype = ", m_pvrstream->mediatype());
			log_info(__func__, ": canseek   = ", m_pvrstream->canseek());
			log_info(__func__, ": length    = ", m_pvrstream->length());
			log_info(__func__, ": realtime  = ", m_pvrstream->realtime());
			log_info(__func__, ": starttime = ", m_stream_starttime, " (epoch) = ", strtok(asctime(localtime(&m_stream_starttime)), "\n"), " (local)");
			log_info(__func__, ": endtime   = ", m_stream_endtime, " (epoch) = ", strtok(asctime(localtime(&m_stream_endtime)), "\n"), " (local)");
		}

		catch(...) { m_pvrstream.reset(); m_scheduler.resume(); throw; }
	}

	// Queue a notification for the user when a recorded stream cannot be opened, don't just silently log it
	catch(std::exception& ex) { 
		
		kodi::QueueFormattedNotification(QueueMsg::QUEUE_ERROR, "Recorded Stream creation failed (%s).", ex.what());
		return handle_stdexception(__func__, ex, false); 
	}

	catch(...) { return handle_generalexception(__func__, false); }

	return true;
}

//-----------------------------------------------------------------------------
// addon::ReadLiveStream (CInstancePVRClient)
//
// Read from an open live stream
//
// Arguments:
//
//	buffer		- The buffer to store the data in
//	size		- The number of bytes to read into the buffer

int addon::ReadLiveStream(unsigned char* buffer, unsigned int size)
{
	try { 
	
		// Attempt to read the requested number of bytes from the stream
		int result = (m_pvrstream) ? static_cast<int>(m_pvrstream->read(buffer, size)) : -1;

		// Live streams should always return data, log an error on any zero-length read
		if(result == 0) log_error(__func__, ": zero-length read on stream at position ", m_pvrstream->position());

		return result;
	}

	catch(std::exception& ex) {

		// Log the exception and alert the user of the failure with an error notification
		log_error(__func__, ": read operation failed with exception: ", ex.what());
		kodi::QueueFormattedNotification(QueueMsg::QUEUE_ERROR, "Unable to read from live stream: %s", ex.what());

		// Kodi is going to continue to call this function until it thinks the stream has ended so
		// consume whatever data is left in the stream buffer until it returns zero enough times to stop
		try { return static_cast<int>(m_pvrstream->read(buffer, size)); }
		catch(...) { return 0; }
	}

	catch(...) { return handle_generalexception(__func__, -1); }
}

//-----------------------------------------------------------------------------
// addon::ReadRecordedStream (CInstancePVRClient)
//
// Read from a recording
//
// Arguments:
//
//	buffer		- The buffer to store the data in
//	size		- The number of bytes to read into the buffer

int addon::ReadRecordedStream(unsigned char* buffer, unsigned int size)
{
	try { 
	
		// Attempt to read the requested number of bytes from the stream
		int result = (m_pvrstream) ? static_cast<int>(m_pvrstream->read(buffer, size)) : -1;

		// Recorded streams may be real-time if they were in progress when started, but it
		// is still normal for them to end at some point and return no data.  If no data was 
		// read from a real-time stream and the current system clock is before the expected 
		// end time of that stream, log a zero-length read error
		if(result == 0) {

			time_t now = time(nullptr);
			if((m_pvrstream->realtime()) && (now < m_stream_endtime)) 
				log_error(__func__, ": zero-length read on stream at position ", m_pvrstream->position());
		}

		return result;
	}

	catch(std::exception& ex) {

		// Log the exception and alert the user of the failure with an error notification
		log_error(__func__, ": read operation failed with exception: ", ex.what());
		kodi::QueueFormattedNotification(QueueMsg::QUEUE_ERROR, "Unable to read from recorded stream: %s", ex.what());

		// Kodi is going to continue to call this function until it thinks the stream has ended so
		// consume whatever data is left in the stream buffer until it returns zero enough times to stop
		try { return static_cast<int>(m_pvrstream->read(buffer, size)); }
		catch(...) { return 0; }
	}

	catch(...) { return handle_generalexception(__func__, -1); }
}

//-----------------------------------------------------------------------------
// addon::SeekLiveStream (CInstancePVRClient)
//
// Seek in a live stream on a backend that supports timeshifting
//
// Arguments:
//
//	position	- Delta within the stream to seek, relative to whence
//	whence		- Starting position from which to apply the delta

int64_t addon::SeekLiveStream(int64_t position, int whence)
{
	try { return (m_pvrstream) ? m_pvrstream->seek(position, whence) : -1; }

	catch(std::exception& ex) {

		// Log the exception and alert the user of the failure with an error notification
		log_error(__func__, ": seek operation failed with exception: ", ex.what());
		kodi::QueueFormattedNotification(QueueMsg::QUEUE_ERROR, "Unable to seek live stream: %s", ex.what());

		return -1;
	}

	catch(...) { return handle_generalexception(__func__, -1); }
}

//-----------------------------------------------------------------------------
// addon::SeekRecordedStream (CInstancePVRClient)
//
// Seek in a recorded stream
//
// Arguments:
//
//	position	- Delta within the stream to seek, relative to whence
//	whence		- Starting position from which to apply the delta

int64_t addon::SeekRecordedStream(int64_t position, int whence)
{
	try { return (m_pvrstream) ? m_pvrstream->seek(position, whence) : -1; }

	catch(std::exception& ex) {

		// Log the exception and alert the user of the failure with an error notification
		log_error(__func__, ": seek operation failed with exception: ", ex.what());
		kodi::QueueFormattedNotification(QueueMsg::QUEUE_ERROR, "Unable to seek recorded stream: %s", ex.what());

		return -1;
	}

	catch(...) { return handle_generalexception(__func__, -1); }
}

//-----------------------------------------------------------------------------
// addon::SetEPGMaxFutureDays (CInstancePVRClient)
//
// Tell the client the future time frame to use when notifying epg events back to Kodi
//
// Arguments:
//
//	days	- number of days from "now", or EPG_TIMEFRAME_UNLIMITED

PVR_ERROR addon::SetEPGMaxFutureDays(int futureDays)
{
	int epgmaxtime = m_epgmaxtime.load();
	if(futureDays == epgmaxtime) return PVR_ERROR::PVR_ERROR_NO_ERROR;

	m_epgmaxtime.store(futureDays);

	// The addon will receive this notification the instant the user has changed this setting; provide a 5-second
	// delay before actually pushing new data or triggering any updates to allow it to 'settle'
	log_info(__func__, ": EPG future days setting has been changed -- trigger guide listing and timer updates in 5 seconds");
	m_scheduler.add(EPG_TIMEFRAME_CHANGED_TASK, std::chrono::system_clock::now() + std::chrono::seconds(5), &addon::epg_timeframe_changed_task, this);

	return PVR_ERROR::PVR_ERROR_NO_ERROR;
}

//-----------------------------------------------------------------------------
// addon::SetEPGMaxPastDays (CInstancePVRClient)
//
// Tell the client the past time frame to use when notifying epg events back to Kodi
//
// Arguments:
//
//	days	- number of days before "now", or EPG_TIMEFRAME_UNLIMITED

PVR_ERROR addon::SetEPGMaxPastDays(int /*pastDays*/)
{
	// The terms of use for the EPG do not allow for past information to be retrieved
	return PVR_ERROR::PVR_ERROR_NO_ERROR;
}

//-----------------------------------------------------------------------------
// addon::SetRecordingPlayCount  (CInstancePVRClient)
//
// Set the play count of a recording on the backend
//
// Arguments:
//
//	recording			- Recording to set the last played position for
//	count				- Updated play count for the recording

PVR_ERROR addon::SetRecordingPlayCount(kodi::addon::PVRRecording const& recording, int count)
{
	try { 
		
		// Only handle a play count change to zero here, indicating the recording is being marked as unwatched, in this
		// case there will be no follow-up call to SetRecordingLastPlayedPosition
		if(count == 0) set_recording_lastposition(connectionpool::handle(m_connpool), recording.GetRecordingId().c_str(), 0);
	}

	catch(std::exception& ex) { return handle_stdexception(__func__, ex, PVR_ERROR::PVR_ERROR_FAILED); }
	catch(...) { return handle_generalexception(__func__, PVR_ERROR::PVR_ERROR_FAILED); }

	return PVR_ERROR::PVR_ERROR_NO_ERROR;
}

//-----------------------------------------------------------------------------
// addon::SetRecordingLastPlayedPosition (CInstancePVRClient)
//
// Set the last watched position of a recording on the backend
//
// Arguments:
//
//	recording			- Recording to set the last played position for
//	lastplayedposition	- Last played position, specified in seconds

PVR_ERROR addon::SetRecordingLastPlayedPosition(kodi::addon::PVRRecording const& recording, int lastplayedposition)
{
	try { 
	
		// If the last played position is -1, or if it's zero with a positive play count, mark as watched
		bool const watched = ((lastplayedposition < 0) || ((lastplayedposition == 0) && (recording.GetPlayCount() > 0)));
		set_recording_lastposition(connectionpool::handle(m_connpool), recording.GetRecordingId().c_str(), 
			watched ? std::numeric_limits<uint32_t>::max() : static_cast<uint32_t>(lastplayedposition));
	}

	catch(std::exception& ex) { return handle_stdexception(__func__, ex, PVR_ERROR::PVR_ERROR_FAILED); }
	catch(...) { return handle_generalexception(__func__, PVR_ERROR::PVR_ERROR_FAILED); }

	return PVR_ERROR::PVR_ERROR_NO_ERROR;
}

//-----------------------------------------------------------------------------
// addon::UpdateTimer (CInstancePVRClient)
//
// Update the timer information on the backend
//
// Arguments:
//
//	timer	- Timer instance to be updated

PVR_ERROR addon::UpdateTimer(kodi::addon::PVRTimer const& timer)
{
	// Get the current time as a unix timestamp, used to set up AfterOriginalAirdateOnly
	time_t now = time(nullptr);

	// Create an initialize a new recordingrule to be passed to the database
	struct recordingrule recordingrule {};

	try {

		// Pull a database connection out from the connection pool
		connectionpool::handle dbhandle(m_connpool);

		// This operation is only available when there is at least one DVR authorized tuner
		std::string authorization = get_authorization_strings(dbhandle, true);
		if(authorization.length() == 0) {

			kodi::gui::dialogs::OK::ShowAndGetInput("DVR Service Subscription Required", "Timer operations require an active HDHomeRun DVR Service subscription.",
				"", "https://www.silicondust.com/dvr-service/");
			return PVR_ERROR::PVR_ERROR_NO_ERROR;
		}

		// seriesrule / epgseriesrule --> recordingrule_type::series
		//
		if((timer.GetTimerType() == timer_type::seriesrule) || (timer.GetTimerType() == timer_type::epgseriesrule)) {

			// series rules allow editing of channel, recentonly, afteroriginalairdateonly, startpadding and endpadding
			recordingrule.recordingruleid = timer.GetClientIndex();
			recordingrule.type = recordingrule_type::series;
			recordingrule.channelid.value = (timer.GetClientChannelUid() == PVR_TIMER_ANY_CHANNEL) ? 0 : timer.GetClientChannelUid();
			recordingrule.recentonly = (timer.GetPreventDuplicateEpisodes() == duplicate_prevention::recentonly);
			recordingrule.afteroriginalairdateonly = (timer.GetPreventDuplicateEpisodes() == duplicate_prevention::newonly) ? now : 0;
			recordingrule.startpadding = (timer.GetMarginStart() == 0) ? 30 : timer.GetMarginStart() * 60;
			recordingrule.endpadding = (timer.GetMarginEnd() == 0) ? 30 : timer.GetMarginEnd() * 60;
		}

		// datetimeonlyrule / epgdatetimeonlyrule --> recordingrule_type::datetimeonly
		//
		else if((timer.GetTimerType() == timer_type::datetimeonlyrule) || (timer.GetTimerType() == timer_type::epgdatetimeonlyrule)) {

			// date/time only rules allow editing of channel, startpadding and endpadding
			recordingrule.recordingruleid = timer.GetClientIndex();
			recordingrule.type = recordingrule_type::datetimeonly;
			recordingrule.channelid.value = (timer.GetClientChannelUid() == PVR_TIMER_ANY_CHANNEL) ? 0 : timer.GetClientChannelUid();
			recordingrule.startpadding = (timer.GetMarginStart() == 0) ? 30 : timer.GetMarginStart() * 60;
			recordingrule.endpadding = (timer.GetMarginEnd() == 0) ? 30 : timer.GetMarginEnd() * 60;
		}

		// any other timer type is not supported
		else return PVR_ERROR::PVR_ERROR_NOT_IMPLEMENTED;

		// Determine the series identifier for the recording rule before it gets modified
		std::string seriesid = (!timer.GetSeriesLink().empty()) ? timer.GetSeriesLink() : get_recordingrule_seriesid(dbhandle, recordingrule.recordingruleid);
		if(seriesid.length() == 0) throw string_exception(__func__, ": could not determine seriesid for timer");

		// Attempt to modify the recording rule on the backend and in the database
		modify_recordingrule(dbhandle, authorization.c_str(), recordingrule);

		// Update the episode information for the specified series; issue a log warning if the operation fails
		try { discover_episodes_seriesid(dbhandle, authorization.c_str(), seriesid.c_str()); }
		catch(std::exception& ex) { log_warning(__func__, ": unable to refresh episode information for series ", seriesid.c_str(), ": ", ex.what()); }
		catch(...) { log_warning(__func__, ": unable to refresh episode information for series ", seriesid.c_str()); }
	}

	catch(std::exception& ex) { return handle_stdexception(__func__, ex, PVR_ERROR::PVR_ERROR_FAILED); }
	catch(...) { return handle_generalexception(__func__, PVR_ERROR::PVR_ERROR_FAILED); }

	// Force a timer update in Kodi to refresh whatever this did on the backend
	TriggerTimerUpdate();

	return PVR_ERROR::PVR_ERROR_NO_ERROR;
}

//---------------------------------------------------------------------------

#pragma warning(pop)
