//-----------------------------------------------------------------------------
// Copyright (c) 2016-2022 Michael G. Brehm
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

#ifndef __SCHEDULER_H_
#define __SCHEDULER_H_
#pragma once

#include <chrono>
#include <functional>
#include <queue>
#include <string>
#include <thread>
#include <vector>

#include "scalar_condition.h"

#pragma warning(push, 4)

//---------------------------------------------------------------------------
// Class scheduler
//
// Implements a simple task scheduler

class scheduler
{
public:

	// Public Data Types
	//
	using exception_handler_t = std::function<void(std::exception const&)>;

	// Instance Constructors
	//
	scheduler();
	scheduler(exception_handler_t handler);

	// Destructor
	//
	~scheduler();

	//-----------------------------------------------------------------------
	// Member Functions

	// add
	//
	// Adds a task to the scheduler queue
	void add(std::function<void(scalar_condition<bool> const&)> task);
	void add(char const* name, std::function<void(scalar_condition<bool> const&)> task);
	void add(std::chrono::time_point<std::chrono::system_clock> due, std::function<void(scalar_condition<bool> const&)> task);
	void add(char const* name, std::chrono::time_point<std::chrono::system_clock> due, std::function<void(scalar_condition<bool> const&)> task); 
	template<class T> void add(void (T::*func)(scalar_condition<bool> const&), T* instance) { return add(nullptr, std::chrono::system_clock::now(), func, instance); }
	template<class T> void add(char const* name, void (T::*func)(scalar_condition<bool> const&), T* instance) { return add(name, std::chrono::system_clock::now(), func, instance); }
	template<class T> void add(std::chrono::time_point<std::chrono::system_clock> due, void (T::*func)(scalar_condition<bool> const&), T* instance) { return add(nullptr, due, func, instance); }
	template<class T> void add(char const* name, std::chrono::time_point<std::chrono::system_clock> due, void (T::*func)(scalar_condition<bool> const&), T* instance) { return add(name, due, std::bind(func, instance, std::placeholders::_1)); }

	// clear
	//
	// Removes all tasks from the scheduler
	void clear(void);

	// now
	//
	// Executes the specified task synchronously
	void now(std::function<void(scalar_condition<bool> const&)> task);
	void now(char const* name, std::function<void(scalar_condition<bool> const&)> task);
	void now(std::function<void(scalar_condition<bool> const&)> task, scalar_condition<bool> const& cancel);
	void now(char const* name, std::function<void(scalar_condition<bool> const&)> task, scalar_condition<bool> const& cancel);
	template<class T> void now(void (T::*func)(scalar_condition<bool> const&), T* instance) { return now(nullptr, func, instance); }
	template<class T> void now(char const* name, void (T::*func)(scalar_condition<bool> const&), T* instance) { return now(name, func, instance); }
	template<class T> void now(void (T::*func)(scalar_condition<bool> const&), T* instance, scalar_condition<bool> const& cancel) { return now(nullptr, func, instance, cancel); }
	template<class T> void now(char const* name, void (T::*func)(scalar_condition<bool> const&), T* instance, scalar_condition<bool> const& cancel) { return now(name, std::bind(func, instance, std::placeholders::_1), cancel); }

	// pause
	//
	// Pauses the scheduler; thread still runs but tasks do not
	void pause(void);

	// remove
	//
	// Removes all matching named tasks from the scheduler queue
	void remove(char const* name);

	// resume
	//
	// Resumes the scheduler from a paused state
	void resume(void);

	// start
	//
	// Starts the task scheduler
	void start(void);

	// stop
	//
	// Stops the task scheduler
	void stop(void);

private:

	scheduler(scheduler const&)=delete;
	scheduler& operator=(scheduler const&)=delete;

	// queueitem_t
	//
	// queue<> element type
	struct queueitem_t {

		// name
		//
		// name of the task (optional, can be empty)
		std::string name;

		// due
		//
		// time at which the task becomes due
		std::chrono::time_point<std::chrono::system_clock> due;

		// task
		//
		// functor to invoke to execute the task
		std::function<void(scalar_condition<bool> const&)> task;
	};

	// queueitem_greater_t
	//
	// Comparator to sort queueitem_t elements in the priority queue
	struct queueitem_greater_t
	{
		bool operator()(queueitem_t const& lhs, queueitem_t const& rhs) const
		{
			return lhs.due > rhs.due;
		}
	};

	// queue_t
	//
	// Scheduler queue data type
	using queue_t = std::priority_queue<queueitem_t, std::vector<queueitem_t>, queueitem_greater_t>;

	//-----------------------------------------------------------------------
	// Private Member Functions

	// remove
	//
	// Removes all instances of a named task from the queue
	void remove(std::unique_lock<std::mutex> const& lock, char const* name);

	//-----------------------------------------------------------------------
	// Member Variables

	exception_handler_t	const	m_handler;				// Exception handler
	queue_t						m_queue;				// Task queue
	mutable std::mutex			m_queue_lock;			// Synchronization object
	bool						m_paused = false;		// Flag to pause the work load
	std::thread					m_worker;				// Scheduler thread
	std::mutex					m_worker_lock;			// Synchronization object
	scalar_condition<bool>		m_stop{false};			// Condition to stop the thread
	std::recursive_mutex		m_task_lock;			// Synchronization object
};

//-----------------------------------------------------------------------------

#pragma warning(pop)

#endif	// __SCHEDULER_H_
