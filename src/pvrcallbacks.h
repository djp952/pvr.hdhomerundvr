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

#ifndef __PVRCALLBACKS_H_
#define __PVRCALLBACKS_H_
#pragma once

#include <xbmc_pvr_types.h>

#pragma warning(push, 4)

//-----------------------------------------------------------------------------
// pvrcallbacks
//
// Dynamically loaded function pointers from libXBMC_pvr.dll

class pvrcallbacks
{
public:

	// Instance Constructor
	//
	pvrcallbacks(void* addonhandle);

	// Destructor
	//
	~pvrcallbacks();

	//-----------------------------------------------------------------------
	// Member Functions

	// AddMenuHook
	//
	// Adds a PVR-specific menu hook
	void AddMenuHook(PVR_MENUHOOK* menuhook) const;

	// ConnectionStateChange
	//
	// Indicates that the state of the connection with the backend has changed
	void ConnectionStateChange(char const* connstring, PVR_CONNECTION_STATE state, char const* message) const;

	// EpgEventStateChange
	//
	// Asynchronously updates the EPG entries for a single channel
	void EpgEventStateChange(EPG_TAG* tag, unsigned int channelid, EPG_EVENT_STATE state) const;

	// TransferChannelEntry
	//
	// Transfers an enumerated PVR_CHANNEL structure to Kodi
	void TransferChannelEntry(ADDON_HANDLE const handle, PVR_CHANNEL const* channel) const;

	// TransferChannelGroup
	//
	// Transfers an enumerated PVR_CHANNEL_GROUP structure to Kodi
	void TransferChannelGroup(ADDON_HANDLE const handle, PVR_CHANNEL_GROUP const* group) const;

	// TransferChannelGroupMember
	//
	// Transfers an enumerated PVR_CHANNEL_GROUP_MEMBER structure to Kodi
	void TransferChannelGroupMember(ADDON_HANDLE const handle, PVR_CHANNEL_GROUP_MEMBER const* member) const;

	// TransferEpgEntry
	//
	// Transfers an enumerated EPG_TAG structure to Kodi
	void TransferEpgEntry(ADDON_HANDLE const handle, EPG_TAG const* epgtag) const;

	// TransferRecordingEntry
	//
	// Transfers an enumerated PVR_RECORDING structure to Kodi
	void TransferRecordingEntry(ADDON_HANDLE const handle, PVR_RECORDING const* recording) const;

	// TransferTimerEntry
	//
	// Transfers an enumerated PVR_TIMER structure to Kodi
	void TransferTimerEntry(ADDON_HANDLE const handle, PVR_TIMER const* timer) const;

	// TriggerChannelUpdate
	//
	// Triggers a channel update operation
	void TriggerChannelUpdate(void) const;

	// TriggerChannelGroupsUpdate
	//
	// Triggers a channel groups update operation
	void TriggerChannelGroupsUpdate(void) const;

	// TriggerEpgUpdate
	//
	// Schedules an EPG update for the specified channel
	void TriggerEpgUpdate(unsigned int channelid) const;

	// TriggerRecordingUpdate
	//
	// Triggers a recording update operation
	void TriggerRecordingUpdate(void) const;

	// TriggerTimerUpdate
	//
	// Triggers a timer update operation
	void TriggerTimerUpdate(void) const;

private:

	pvrcallbacks(const pvrcallbacks&)=delete;
	pvrcallbacks& operator=(const pvrcallbacks&)=delete;

	//-----------------------------------------------------------------------
	// Type Declarations

	using PvrAddMenuHookFunc				= void			(*)(void*, void*, PVR_MENUHOOK*);
    using PvrAllocateDemuxPacketFunc		= DemuxPacket*	(*)(void*, void*, int);
    using PvrConnectionStateChangeFunc		= void			(*)(void*, void*, char const*, PVR_CONNECTION_STATE, char const*);
    using PvrEpgEventStateChangeFunc		= void			(*)(void*, void*, EPG_TAG*, unsigned int, EPG_EVENT_STATE);
    using PvrFreeDemuxPacketFunc			= void			(*)(void*, void*, DemuxPacket*);
	using PvrRecordingFunc					= void			(*)(void*, void*, char const*, char const*, bool);
	using PvrRegisterMeFunc					= void*			(*)(void*);
	using PvrTransferChannelEntryFunc		= void			(*)(void*, void*, ADDON_HANDLE const, PVR_CHANNEL const*);
    using PvrTransferChannelGroupFunc		= void			(*)(void*, void*, ADDON_HANDLE const, PVR_CHANNEL_GROUP const*);
    using PvrTransferChannelGroupMemberFunc	= void			(*)(void*, void*, ADDON_HANDLE const, PVR_CHANNEL_GROUP_MEMBER const*);
	using PvrTransferEpgEntryFunc			= void			(*)(void*, void*, ADDON_HANDLE const, EPG_TAG const*);
	using PvrTransferRecordingEntryFunc		= void			(*)(void*, void*, ADDON_HANDLE const, PVR_RECORDING const*);
	using PvrTransferTimerEntryFunc			= void			(*)(void*, void*, ADDON_HANDLE const, PVR_TIMER const*);
	using PvrTriggerChannelGroupsUpdateFunc	= void			(*)(void*, void*);
	using PvrTriggerChannelUpdateFunc		= void			(*)(void*, void*);
    using PvrTriggerEpgUpdateFunc			= void			(*)(void*, void*, unsigned int);
	using PvrTriggerRecordingUpdateFunc		= void			(*)(void*, void*);
	using PvrTriggerTimerUpdateFunc			= void			(*)(void*, void*);
	using PvrUnRegisterMeFunc				= void			(*)(void*, void*);

	//-------------------------------------------------------------------------
	// Private Fields

	PvrAddMenuHookFunc					PvrAddMenuHook;
	PvrAllocateDemuxPacketFunc			PvrAllocateDemuxPacket;
	PvrConnectionStateChangeFunc		PvrConnectionStateChange;
	PvrEpgEventStateChangeFunc			PvrEpgEventStateChange;
	PvrFreeDemuxPacketFunc				PvrFreeDemuxPacket;
	PvrRecordingFunc					PvrRecording;
	PvrRegisterMeFunc					PvrRegisterMe;
	PvrTransferEpgEntryFunc				PvrTransferEpgEntry;
	PvrTransferChannelEntryFunc			PvrTransferChannelEntry;
	PvrTransferChannelGroupFunc			PvrTransferChannelGroup;
	PvrTransferChannelGroupMemberFunc	PvrTransferChannelGroupMember;
	PvrTransferRecordingEntryFunc		PvrTransferRecordingEntry;
	PvrTransferTimerEntryFunc			PvrTransferTimerEntry;
	PvrTriggerChannelGroupsUpdateFunc	PvrTriggerChannelGroupsUpdate;
	PvrTriggerChannelUpdateFunc			PvrTriggerChannelUpdate;
	PvrTriggerEpgUpdateFunc				PvrTriggerEpgUpdate;
	PvrTriggerRecordingUpdateFunc		PvrTriggerRecordingUpdate;
	PvrTriggerTimerUpdateFunc			PvrTriggerTimerUpdate;
	PvrUnRegisterMeFunc					PvrUnRegisterMe;

	//-------------------------------------------------------------------------
	// Member Variables

	void*				m_hmodule;			// Loaded DLL module handle
	void*				m_handle;			// Original add-on handle
	void*				m_callbacks;		// Registered callbacks handle
};

//-----------------------------------------------------------------------------

#pragma warning(pop)

#endif	// __PVRCALLBACKS_H_
