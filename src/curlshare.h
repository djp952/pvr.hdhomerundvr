//-----------------------------------------------------------------------------
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
//-----------------------------------------------------------------------------

#ifndef __CURLSHARE_H_
#define __CURLSHARE_H_
#pragma once

#include <mutex>

#pragma warning(push, 4)	

//-----------------------------------------------------------------------------
// Class curlshare
//
// cURL share interface implementation; allows sharing of the DNS and connection
// cache among disparate cURL easy interface objects.  Note the use of recursive
// mutexes as the synchronization objects; cURL can and does call into the lock
// function multiple times on the same thread.

class curlshare
{
public:

	// Instance Constructor
	//
	curlshare();

	// Destructor
	//
	~curlshare();

	// CURLSH* conversion operator
	//
	operator CURLSH*() const;
	
private:

	//-----------------------------------------------------------------------
	// Private Member Functions

	// curl_lock (static)
	//
	// Provides the lock callback for the shared interface
	static void curl_lock(CURL* handle, curl_lock_data data, curl_lock_access access, void* context);

	// curl_unlock (static)
	//
	// Provides the unlock callback for the shared interface
	static void curl_unlock(CURL* handle, curl_lock_data data, void* context);

	//-------------------------------------------------------------------------
	// Member Variables

	CURLSH*							m_curlsh;		// cURL share interface handle
	mutable std::recursive_mutex	m_sharelock;	// General share synchronization object
	mutable std::recursive_mutex	m_dnslock;		// DNS share synchronization object
	mutable std::recursive_mutex	m_connlock;		// Connection share synchronization object
};

//-----------------------------------------------------------------------------

#pragma warning(pop)

#endif	// __CURLSHARE_H_
