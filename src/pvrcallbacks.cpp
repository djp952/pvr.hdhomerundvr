//-----------------------------------------------------------------------------
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
//-----------------------------------------------------------------------------

#include "stdafx.h"
#include "pvrcallbacks.h"

#include <dlfcn.h>
#include <string>

#include "string_exception.h"

#pragma warning(push, 4)

// LIBXBMC_PVR_MODULE
//
// Macro indicating the location of the libXBMC_pvr module, which is architecture-specific
#if defined(_WINDOWS)
#define LIBXBMC_PVR_MODULE "\\library.xbmc.pvr\\libXBMC_pvr.dll"
#elif defined(__x86_64__) && !defined(__ANDROID__)
#define LIBXBMC_PVR_MODULE "/library.xbmc.pvr/libXBMC_pvr-x86_64-linux.so"
#elif defined(__i386__) && !defined(__ANDROID__)
#define LIBXBMC_PVR_MODULE "/library.xbmc.pvr/libXBMC_pvr-i486-linux.so"
#elif defined(__arm__) && !defined(__ANDROID__)
#define LIBXBMC_PVR_MODULE "/library.xbmc.pvr/libXBMC_pvr-arm.so"
#elif defined(__aarch64__) && !defined(__ANDROID__)
#define LIBXBMC_PVR_MODULE "/library.xbmc.pvr/libXBMC_pvr-aarch64.so"
#elif defined(__ANDROID__) && defined(__arm__)
#define LIBXBMC_PVR_MODULE "/libXBMC_pvr-arm.so"
#elif defined(__ANDROID__) && (defined __aarch64__)
#define LIBXBMC_PVR_MODULE "/libXBMC_pvr-aarch64.so"
#elif defined(__ANDROID__) && defined(__i386__)
#define LIBXBMC_PVR_MODULE "/libXBMC_pvr-i486-linux.so"
#else
#error pvrcallbacks.cpp -- unsupported architecture
#endif

// GetFunctionPointer (local)
//
// Retrieves a function pointer from the specified module
template <typename _funcptr> 
static inline _funcptr GetFunctionPointer(void* module, char const* name)
{
	_funcptr ptr = reinterpret_cast<_funcptr>(dlsym(module, name));
	if(ptr == nullptr) throw string_exception(std::string("failed to get entry point for function ") + std::string(name));

	return ptr;
}

//-----------------------------------------------------------------------------
// pvrcallbacks Constructor
//
// Arguments:
//
//	addonhandle		- Add-on handle passed into ADDON_Create()

pvrcallbacks::pvrcallbacks(void* addonhandle) : m_hmodule(nullptr), m_handle(addonhandle), m_callbacks(nullptr)
{
	// The path to the Kodi addon folder is embedded in the handle as a UTF-8 string
	char const* addonpath = *reinterpret_cast<char const**>(addonhandle);

	// Construct the path to the PVR addon library based on the provided base path
	std::string pvrmodule = std::string(addonpath) + LIBXBMC_PVR_MODULE;

	// Attempt to load the PVR addon library dynamically, it should already be in the process
	m_hmodule = dlopen(pvrmodule.c_str(), RTLD_LAZY);
	if(m_hmodule == nullptr) throw string_exception(std::string("failed to load dynamic pvr addon library ") + pvrmodule);

	try {

		// Acquire function pointers to all of the PVR addon library callbacks
		PvrAddMenuHook = GetFunctionPointer<PvrAddMenuHookFunc>(m_hmodule, "PVR_add_menu_hook");
		PvrAllocateDemuxPacket = GetFunctionPointer<PvrAllocateDemuxPacketFunc>(m_hmodule, "PVR_allocate_demux_packet");
		PvrConnectionStateChange = GetFunctionPointer<PvrConnectionStateChangeFunc>(m_hmodule, "PVR_connection_state_change");
		PvrEpgEventStateChange = GetFunctionPointer<PvrEpgEventStateChangeFunc>(m_hmodule, "PVR_epg_event_state_change");
		PvrFreeDemuxPacket = GetFunctionPointer<PvrFreeDemuxPacketFunc>(m_hmodule, "PVR_free_demux_packet");
		PvrRecording = GetFunctionPointer<PvrRecordingFunc>(m_hmodule, "PVR_recording");
		PvrRegisterMe = GetFunctionPointer<PvrRegisterMeFunc>(m_hmodule, "PVR_register_me");
		PvrTransferEpgEntry = GetFunctionPointer<PvrTransferEpgEntryFunc>(m_hmodule, "PVR_transfer_epg_entry");
		PvrTransferChannelEntry = GetFunctionPointer<PvrTransferChannelEntryFunc>(m_hmodule, "PVR_transfer_channel_entry");
		PvrTransferChannelGroup = GetFunctionPointer<PvrTransferChannelGroupFunc>(m_hmodule, "PVR_transfer_channel_group");
		PvrTransferChannelGroupMember = GetFunctionPointer<PvrTransferChannelGroupMemberFunc>(m_hmodule, "PVR_transfer_channel_group_member");
		PvrTransferRecordingEntry = GetFunctionPointer<PvrTransferRecordingEntryFunc>(m_hmodule, "PVR_transfer_recording_entry");
		PvrTransferTimerEntry = GetFunctionPointer<PvrTransferTimerEntryFunc>(m_hmodule, "PVR_transfer_timer_entry");
		PvrTriggerChannelGroupsUpdate = GetFunctionPointer<PvrTriggerChannelGroupsUpdateFunc>(m_hmodule, "PVR_trigger_channel_groups_update");
		PvrTriggerChannelUpdate = GetFunctionPointer<PvrTriggerChannelUpdateFunc>(m_hmodule, "PVR_trigger_channel_update");
		PvrTriggerEpgUpdate = GetFunctionPointer<PvrTriggerEpgUpdateFunc>(m_hmodule, "PVR_trigger_epg_update");
		PvrTriggerRecordingUpdate = GetFunctionPointer<PvrTriggerRecordingUpdateFunc>(m_hmodule, "PVR_trigger_recording_update");
		PvrTriggerTimerUpdate = GetFunctionPointer<PvrTriggerTimerUpdateFunc>(m_hmodule, "PVR_trigger_timer_update");
		PvrUnRegisterMe = GetFunctionPointer<PvrUnRegisterMeFunc>(m_hmodule, "PVR_unregister_me");

		// Register with the PVR addon library
		m_callbacks = PvrRegisterMe(m_handle);
		if(m_callbacks == nullptr) throw string_exception("Failed to register pvrcallbacks handle");
	}

	// Ensure that the library is released on any constructor exceptions
	catch(std::exception&) { dlclose(m_hmodule); m_hmodule = nullptr; throw; }
}

//-----------------------------------------------------------------------------
// pvrcallbacks Destructor

pvrcallbacks::~pvrcallbacks()
{
	PvrUnRegisterMe(m_handle, m_callbacks);
	dlclose(m_hmodule);
}

//-----------------------------------------------------------------------------
// pvrcallbacks::AddMenuHook
//
// Adds a PVR-specific menu hook
//
// Arguments:
//
//	menuhook		- PVR_MENUHOOK to be added

void pvrcallbacks::AddMenuHook(PVR_MENUHOOK* menuhook) const
{
	assert((PvrAddMenuHook) && (m_handle) && (m_callbacks));
	PvrAddMenuHook(m_handle, m_callbacks, menuhook);
}
	
//-----------------------------------------------------------------------------
// pvrcallbacks::ConnectionStateChange
//
// Notifies Kodi of a state change on the backend connection
//
// Arguments:
//
//	connstring		- The backend connection string
//	state			- The new state to be reported
//	message			- Optional connection state message to be reported

void pvrcallbacks::ConnectionStateChange(char const* connstring, PVR_CONNECTION_STATE state, char const* message) const
{
	assert((PvrConnectionStateChange) && (m_handle) && (m_callbacks));
	PvrConnectionStateChange(m_handle, m_callbacks, connstring, state, message);
}
	
//-----------------------------------------------------------------------------
// pvrcallbacks::EpgEventStateChange
//
// Asynchronously updates the EPG entries for a single channel
//
// Arguments:
//
//	tag			- EPG_TAG to be updated
//	channelid	- Channel identifier
//	state		- State being changed for the EPG_TAG

void pvrcallbacks::EpgEventStateChange(EPG_TAG* tag, unsigned int channelid, EPG_EVENT_STATE state) const
{
	assert((PvrEpgEventStateChange) && (m_handle) && (m_callbacks));
	PvrEpgEventStateChange(m_handle, m_callbacks, tag, channelid, state);
}
	
//-----------------------------------------------------------------------------
// pvrcallbacks::TransferChannelEntry
//
// Transfers an enumerated PVR_CHANNEL structure to Kodi
//
// Arguments:
//
//	handle		- ADDON_HANDLE passed into the GetRecordings entry point
//	channel		- Pointer to the channel to be transferred to Kodi

void pvrcallbacks::TransferChannelEntry(ADDON_HANDLE const handle, PVR_CHANNEL const* channel) const
{
	assert((PvrTransferChannelEntry) && (m_handle) && (m_callbacks));
	PvrTransferChannelEntry(m_handle, m_callbacks, handle, channel);
}
	
//-----------------------------------------------------------------------------
// pvrcallbacks::TransferChannelGroup
//
// Transfers an enumerated PVR_CHANNEL_GROUP structure to Kodi
//
// Arguments:
//
//	handle		- ADDON_HANDLE passed into the GetRecordings entry point
//	group		- Pointer to the channel group to be transferred to Kodi

void pvrcallbacks::TransferChannelGroup(ADDON_HANDLE const handle, PVR_CHANNEL_GROUP const* group) const
{
	assert((PvrTransferChannelGroup) && (m_handle) && (m_callbacks));
	PvrTransferChannelGroup(m_handle, m_callbacks, handle, group);
}
	
//-----------------------------------------------------------------------------
// pvrcallbacks::TransferChannelGroupMember
//
// Transfers an enumerated PVR_CHANNEL_GROUP_MEMBER structure to Kodi
//
// Arguments:
//
//	handle		- ADDON_HANDLE passed into the GetRecordings entry point
//	member		- Pointer to the channel group member to be transferred to Kodi

void pvrcallbacks::TransferChannelGroupMember(ADDON_HANDLE const handle, PVR_CHANNEL_GROUP_MEMBER const* member) const
{
	assert((PvrTransferChannelGroupMember) && (m_handle) && (m_callbacks));
	PvrTransferChannelGroupMember(m_handle, m_callbacks, handle, member);
}
	
//-----------------------------------------------------------------------------
// pvrcallbacks::TransferEpgEntry
//
// Transfers an enumerated EPG_TAG structure to Kodi
//
// Arguments:
//
//	handle		- ADDON_HANDLE passed into the GetRecordings entry point
//	epgtag		- Pointer to the epg entry to be transferred to Kodi

void pvrcallbacks::TransferEpgEntry(ADDON_HANDLE const handle, EPG_TAG const* epgtag) const
{
	assert((PvrTransferEpgEntry) && (m_handle) && (m_callbacks));
	PvrTransferEpgEntry(m_handle, m_callbacks, handle, epgtag);
}
	
//-----------------------------------------------------------------------------
// pvrcallbacks::TransferRecordingEntry
//
// Transfers an enumerated PVR_RECORDING structure to Kodi
//
// Arguments:
//
//	handle		- ADDON_HANDLE passed into the GetRecordings entry point
//	recording	- Pointer to the recording to be transferred to Kodi

void pvrcallbacks::TransferRecordingEntry(ADDON_HANDLE const handle, PVR_RECORDING const* recording) const
{
	assert((PvrTransferRecordingEntry) && (m_handle) && (m_callbacks));
	PvrTransferRecordingEntry(m_handle, m_callbacks, handle, recording);
}
	
//-----------------------------------------------------------------------------
// pvrcallbacks::TransferTimerEntry
//
// Transfers an enumerated PVR_RECORDING structure to Kodi
//
// Arguments:
//
//	handle		- ADDON_HANDLE passed into the GetRecordings entry point
//	recording	- Pointer to the recording to be transferred to Kodi

void pvrcallbacks::TransferTimerEntry(ADDON_HANDLE const handle, PVR_TIMER const* timer) const
{
	assert((PvrTransferTimerEntry) && (m_handle) && (m_callbacks));
	PvrTransferTimerEntry(m_handle, m_callbacks, handle, timer);
}
	

//-----------------------------------------------------------------------------
// pvrcallbacks::TriggerChannelUpdate
//
// Triggers a channel update operation
//
// Arguments:
//
//	NONE

void pvrcallbacks::TriggerChannelUpdate(void) const
{
	assert((PvrTriggerChannelUpdate) && (m_handle) && (m_callbacks));
	PvrTriggerChannelUpdate(m_handle, m_callbacks);
}

//-----------------------------------------------------------------------------
// pvrcallbacks::TriggerChannelGroupsUpdate
//
// Triggers a channel groups update operation
//
// Arguments:
//
//	NONE

void pvrcallbacks::TriggerChannelGroupsUpdate(void) const
{
	assert((PvrTriggerChannelGroupsUpdate) && (m_handle) && (m_callbacks));
	PvrTriggerChannelGroupsUpdate(m_handle, m_callbacks);
}
	
//-----------------------------------------------------------------------------
// pvrcallbacks::TriggerEpgUpdate
//
// Schedules an EPG update for the specified channel
//
// Arguments:
//
//	channelid	- Channel on which to schedule the EPG update

void pvrcallbacks::TriggerEpgUpdate(unsigned int channelid) const
{
	assert((PvrTriggerEpgUpdate) && (m_handle) && (m_callbacks));
	PvrTriggerEpgUpdate(m_handle, m_callbacks, channelid);
}
	
//-----------------------------------------------------------------------------
// pvrcallbacks::TriggerRecordingUpdate
//
// Schedules a recording update
//
// Arguments:
//
//	NONE

void pvrcallbacks::TriggerRecordingUpdate(void) const
{
	assert((PvrTriggerRecordingUpdate) && (m_handle) && (m_callbacks));
	PvrTriggerRecordingUpdate(m_handle, m_callbacks);
}
	
//-----------------------------------------------------------------------------
// pvrcallbacks::TriggerTimerUpdate
//
// Schedules a timer update
//
// Arguments:
//
//	NONE

void pvrcallbacks::TriggerTimerUpdate(void) const
{
	assert((PvrTriggerTimerUpdate) && (m_handle) && (m_callbacks));
	PvrTriggerTimerUpdate(m_handle, m_callbacks);
}
	
//-----------------------------------------------------------------------------

#pragma warning(pop)
