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
#include "scheduler.h"

#include "string_exception.h"

#pragma warning(push, 4)

//---------------------------------------------------------------------------
// scheduler Constructor
//
// Arguments:
//
//	NONE

scheduler::scheduler()
{
}

//---------------------------------------------------------------------------
// scheduler Constructor
//
// Arguments:
//
//	handler		- Function to invoke when an exception occurs during a task

scheduler::scheduler(scheduler::exception_handler_t handler) : m_handler(handler)
{
}

//---------------------------------------------------------------------------
// scheduler Destructor

scheduler::~scheduler()
{
	stop();
}

//---------------------------------------------------------------------------
// scheduler::add
//
// Adds a task to the scheduler queue
//
// Arguments:
//
//	due		- system_time at which the task should be executed
//	task	- task to be executed

void scheduler::add(std::chrono::time_point<std::chrono::system_clock> due, std::function<void(scalar_condition<bool> const&)> task)
{
	std::unique_lock<std::mutex> lock(m_queue_lock);

	m_queue.emplace(queueitem_t{ due, task });
}

//---------------------------------------------------------------------------
// scheduler::clear
//
// Removes all tasks from the scheduler queue
//
// Arguments:
//
//	NONE

void scheduler::clear(void)
{
	std::unique_lock<std::mutex> lock(m_queue_lock);

	while(!m_queue.empty()) m_queue.pop();
}

//---------------------------------------------------------------------------
// scheduler::pause
//
// Pauses execution of tasks; does not stop the worker thread
//
// Arguments:
//
//	NONE

void scheduler::pause(void)
{
	std::unique_lock<std::mutex> lock(m_queue_lock);
	m_paused = true;
}

//---------------------------------------------------------------------------
// scheduler::remove
//
// Removes all tasks from the scheduler queue with a specific tag
//
// Arguments:
//
//	task		- Task to be removed from the queue

void scheduler::remove(std::function<void(scalar_condition<bool> const&)> task)
{
	queue_t			newqueue;			// The new queue_t instance

	// targetptr_t
	//
	// Type returned by std::function::target<> below
	using targetptr_t = void(*const*)(scalar_condition<bool> const&);

	std::unique_lock<std::mutex> lock(m_queue_lock);

	// priority_queue<> doesn't actually allow elements to be removed, create
	// a new queue with all the elements that don't have the same target
	while(!m_queue.empty()) {

		// Pull out pointers to the left-hand and right-hand std::function targets to be compared
		targetptr_t left = m_queue.top().second.target<void(*)(scalar_condition<bool> const&)>();
		targetptr_t right = task.target<void(*)(scalar_condition<bool> const&)>();

		// If the current item does not match the task being removed, move it into the new queue<>
		if((left != nullptr) && (right != nullptr) && (*left != *right)) newqueue.push(m_queue.top());

		// Always remove the current item from the old queue<>
		m_queue.pop();
	}

	// Swap the contents of the new and original queues
	m_queue.swap(newqueue);
}

//---------------------------------------------------------------------------
// scheduler::start
//
// Starts the task scheduler
//
// Arguments:
//
//	NONE

void scheduler::start(void)
{
	std::unique_lock<std::mutex> lock(m_worker_lock);

	if(m_worker.joinable()) return;		// Already running

	// Define a scalar_condition for the worker to signal when it's running
	scalar_condition<bool> started{false};

	// Define and launch the scheduler worker thread
	m_worker = std::thread([&]() -> void {
	
		started = true;			// Indicate that the thread started

		// Poll the priority queue once per 500ms to acquire a new task or
		// break when the stop signal has been set
		while(!m_stop.wait_until_equals(true, 500)) {

			std::unique_lock<std::mutex> lock(m_queue_lock);
			if(m_queue.empty() || m_paused) continue;

			// Check if the topmost task in the queue has become due
			queueitem_t const& task = m_queue.top();
			if(task.first <= std::chrono::system_clock::now()) {

				// Make a copy of the functor and remove the task from the queue
				auto functor = task.second;
				m_queue.pop();

				// Allow other threads to manipulate the queue while the task runs
				lock.unlock();

				// Invoke the task and dispatch any exceptions that leak out to the handler
				try { functor(m_stop); }
				catch(std::exception& ex) { if(m_handler) m_handler(ex); }
				catch(...) { if(m_handler) m_handler(string_exception("unhandled exception during task execution")); }
			}
		}

		m_stop = false;					// Reset the stop flag
	});

	// Wait for the worker thread to start or die trying
	started.wait_until_equals(true);
}

//---------------------------------------------------------------------------
// scheduler::resume
//
// Resumes execution of tasks if the scheduler was paused
//
// Arguments:
//
//	NONE

void scheduler::resume(void)
{
	std::unique_lock<std::mutex> lock(m_queue_lock);
	m_paused = false;
}

//---------------------------------------------------------------------------
// scheduler::stop
//
// Stops the task scheduler
//
// Arguments:
//
//	NONE

void scheduler::stop(void)
{
	std::unique_lock<std::mutex> lock(m_worker_lock);

	if(!m_worker.joinable()) return;		// Already stopped

	// Signal the worker thread to stop and wait for it to do so
	m_stop = true;
	if(m_worker.joinable()) m_worker.join();
}

//---------------------------------------------------------------------------

#pragma warning(pop)
