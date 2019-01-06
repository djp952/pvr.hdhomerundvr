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
#include "curlshare.h"

#include "string_exception.h"

#pragma warning(push, 4)

//-----------------------------------------------------------------------------
// curlshare Constructor
//
// Arguments:
//
//	NONE

curlshare::curlshare() : m_curlsh(nullptr)
{
	m_curlsh = curl_share_init();
	if(m_curlsh == nullptr) throw string_exception(__func__, ": curl_share_init() failed");

	// Set up the cURL share interface to share DNS and connection caches and provide the
	// required callbacks to the static lock and unlock synchronization routines
	CURLSHcode curlshresult = curl_share_setopt(m_curlsh, CURLSHOPT_SHARE, CURL_LOCK_DATA_DNS);
	if(curlshresult == CURLSHE_OK) curlshresult = curl_share_setopt(m_curlsh, CURLSHOPT_SHARE, CURL_LOCK_DATA_CONNECT);
	if(curlshresult == CURLSHE_OK) curlshresult = curl_share_setopt(m_curlsh, CURLSHOPT_LOCKFUNC, curl_lock);
	if(curlshresult == CURLSHE_OK) curlshresult = curl_share_setopt(m_curlsh, CURLSHOPT_UNLOCKFUNC, curl_unlock);
	if(curlshresult == CURLSHE_OK) curlshresult = curl_share_setopt(m_curlsh, CURLSHOPT_USERDATA, this);
	if(curlshresult != CURLSHE_OK) throw string_exception(__func__, ": curl_share_setopt() failed: ", curl_share_strerror(curlshresult));
}

//-----------------------------------------------------------------------------
// curlshare Destructor

curlshare::~curlshare()
{
	if(m_curlsh != nullptr) curl_share_cleanup(m_curlsh);
	m_curlsh = nullptr;
}

//-----------------------------------------------------------------------------
// curlshare CURLSH* conversion operator

curlshare::operator CURLSH*() const
{
	assert(m_curlsh != nullptr);
	return m_curlsh;
}

//---------------------------------------------------------------------------
// curlshare::curl_lock (static)
//
// Provides the lock callback for the shared interface
//
// Arguments:
//
//	handle		- Handle to the cURL easy interface object being referenced
//	data		- Type of data being locked
//	access		- Lock access type - shared or single
//	context		- Context pointer provided via CURLSHOPT_USERDATA

void curlshare::curl_lock(CURL* /*handle*/, curl_lock_data data, curl_lock_access /*access*/, void* context)
{
	curlshare* instance = reinterpret_cast<curlshare*>(context);
	assert(instance != nullptr);

	// The only implemented locks are for SHARE, DNS, and CONNECT
	if(data == curl_lock_data::CURL_LOCK_DATA_SHARE) instance->m_sharelock.lock();
	else if(data == curl_lock_data::CURL_LOCK_DATA_DNS) instance->m_dnslock.lock();
	else if(data == curl_lock_data::CURL_LOCK_DATA_CONNECT) instance->m_connlock.lock();
	else throw string_exception(__func__, ": invalid curl_lock_data type");
}

//---------------------------------------------------------------------------
// curlshare::curl_unlock (static)
//
// Provides the unlock callback for the shared interface
//
// Arguments:
//
//	handle		- Handle to the cURL easy interface object being referenced
//	data		- Type of data being unlocked
//	context		- Context pointer provided via CURLSHOPT_USERDATA

void curlshare::curl_unlock(CURL* /*handle*/, curl_lock_data data, void* context)
{
	curlshare* instance = reinterpret_cast<curlshare*>(context);
	assert(instance != nullptr);

	// The only implemented locks are for SHARE, DNS, and CONNECT
	if(data == curl_lock_data::CURL_LOCK_DATA_SHARE) instance->m_sharelock.unlock();
	else if(data == curl_lock_data::CURL_LOCK_DATA_DNS) instance->m_dnslock.unlock();
	else if(data == curl_lock_data::CURL_LOCK_DATA_CONNECT) instance->m_connlock.unlock();
	else throw string_exception(__func__, ": invalid curl_lock_data type");
}
	
//---------------------------------------------------------------------------

#pragma warning(pop)
