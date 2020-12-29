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
//	task	- task to be executed

void scheduler::add(std::function<void(scalar_condition<bool> const&)> task)
{
	return add(nullptr, std::chrono::system_clock::now(), task);
}

//---------------------------------------------------------------------------
// scheduler::add
//
// Adds a task to the scheduler queue; removes any matching named tasks
//
// Arguments:
//
//	name	- name to assign to the task
//	task	- task to be executed

void scheduler::add(char const* name, std::function<void(scalar_condition<bool> const&)> task)
{
	return add(name, std::chrono::system_clock::now(), task);
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
	return add(nullptr, due, task);
}

//---------------------------------------------------------------------------
// scheduler::add
//
// Adds a task to the scheduler queue; removes any matching named tasks
//
// Arguments:
//
//	name	- name to assign to the task
//	due		- system_time at which the task should be executed
//	task	- task to be executed

void scheduler::add(char const* name, std::chrono::time_point<std::chrono::system_clock> due, std::function<void(scalar_condition<bool> const&)> task)
{
	std::unique_lock<std::mutex> queuelock(m_queue_lock);

	// Remove any existing instances of a named task from the queue
	if((name != nullptr) && (*name != 0)) remove(queuelock, name);

	// Add the new task to the scheduler priority queue
	m_queue.emplace(queueitem_t{ (name != nullptr) ? name : std::string(), due, task });
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
	std::unique_lock<std::mutex> queuelock(m_queue_lock);

	while(!m_queue.empty()) m_queue.pop();
}

//---------------------------------------------------------------------------
// scheduler::now
//
// Executes the specified task synchronously
//
// Arguments:
//
//	task	- Task to be executed synchronously

void scheduler::now(std::function<void(scalar_condition<bool> const&)> task)
{
	return now(nullptr, task, scalar_condition<bool>{ false });
}

//---------------------------------------------------------------------------
// scheduler::now
//
// Executes the specified task synchronously; removes any matching named tasks
//
// Arguments:
//
//	name	- name to assign to the task
//	task	- Task to be executed synchronously

void scheduler::now(char const* name, std::function<void(scalar_condition<bool> const&)> task)
{
	return now(name, task, scalar_condition<bool>{ false });
}

//---------------------------------------------------------------------------
// scheduler::now
//
// Executes the specified task synchronously
//
// Arguments:
//
//	task	- Task to be executed synchronously
//	cancel	- Task cancellation condition variable 

void scheduler::now(std::function<void(scalar_condition<bool> const&)> task, scalar_condition<bool> const& cancel)
{
	return now(nullptr, task, cancel);
}

//---------------------------------------------------------------------------
// scheduler::now
//
// Executes the specified task synchronously; removes any matching named tasks
//
// Arguments:
//
//	name	- name to assign to the task
//	task	- Task to be executed synchronously
//	cancel	- Task cancellation condition variable 

void scheduler::now(char const* name, std::function<void(scalar_condition<bool> const&)> task, scalar_condition<bool> const& cancel)
{
	std::unique_lock<std::mutex> queuelock(m_queue_lock);

	// Remove any existing instances of a named task from the queue
	if((name != nullptr) && (*name != 0)) remove(queuelock, name);

	// Acquire the task mutex to prevent race condition with main worker thread
	std::unique_lock<std::recursive_mutex> tasklock(m_task_lock);

	// Release the queue lock and execute the task synchronously
	queuelock.unlock();
	task(cancel);
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
	std::unique_lock<std::mutex> queuelock(m_queue_lock);
	m_paused = true;
}

//---------------------------------------------------------------------------
// scheduler::remove
//
// Removes all matching named tasks from the scheduler queue
//
// Arguments:
//
//	name	- name assigned to the task(s)

void scheduler::remove(char const* name)
{
	std::unique_lock<std::mutex> queuelock(m_queue_lock);
	if((name != nullptr) && (*name != 0)) remove(queuelock, name);
}

//---------------------------------------------------------------------------
// scheduler::remove (private)
//
// Removes all tasks from the scheduler queue with a specific tag
//
// Arguments:
//
//	lock	- Held lock instance
//	name	- name assigned to the task(s)

void scheduler::remove(std::unique_lock<std::mutex> const& lock, char const* name)
{
	queue_t			newqueue;			// The new queue_t instance

	assert(lock.owns_lock());
	if(!lock.owns_lock()) throw std::invalid_argument("lock");

	// This function does nothing if the task is unnamed
	if((name == nullptr) || (*name == 0)) return;

	// priority_queue<> doesn't actually allow elements to be removed, create
	// a new queue with all the elements that don't have the same task name
	while(!m_queue.empty()) {

		// Get a reference to the topmost item in the priority_queue<>
		queueitem_t const& top = m_queue.top();

		// Preserve the existing item if the task name doesn't match
		if(top.name.compare(name) != 0) newqueue.push(top);

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
	std::unique_lock<std::mutex> workerlock(m_worker_lock);

	if(m_worker.joinable()) return;		// Already running
	m_stop = false;						// Reset the stop signal

	// Define a scalar_condition for the worker to signal when it's running
	scalar_condition<bool> started{false};

	// Define and launch the scheduler worker thread
	m_worker = std::thread([&]() -> void {

	#if defined(_WINDOWS) || defined(WINAPI_FAMILY)
		// On Windows, set the scheduler thread to run with a BELOW_NORMAL priority
		SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_BELOW_NORMAL);
	#endif

		started = true;			// Indicate that the thread started

		// Poll the priority queue once per 250ms to check for new tasks
		while(m_stop.wait_until_equals(true, 250) == false) {

			// Process all tasks from the top of the queue that have become due before waiting again
			std::unique_lock<std::mutex> queuelock(m_queue_lock);
			while((m_stop.test(false) == true) && (!m_queue.empty()) && (!m_paused) && (m_queue.top().due <= std::chrono::system_clock::now())) {

				// Make a copy of the functor and remove the task from the queue
				auto functor = m_queue.top().task;
				m_queue.pop();

				// Acquire the task mutex to prevent race condition with now()
				std::unique_lock<std::recursive_mutex> tasklock(m_task_lock);

				// Allow other threads to manipulate the queue while the task runs
				queuelock.unlock();

				// Invoke the task and dispatch any exceptions that leak out to the handler
				try { functor(m_stop); } 
				catch(std::exception& ex) { if(m_handler) m_handler(ex); } 
				catch(...) { if(m_handler) m_handler(string_exception(__func__, ": unhandled exception during task execution")); }

				// Reacquire the queue lock after the task has completed
				queuelock.lock();
			}
		}
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
	std::unique_lock<std::mutex> queuelock(m_queue_lock);
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
	std::unique_lock<std::mutex> workerlock(m_worker_lock);

	if(!m_worker.joinable()) return;		// Already stopped

	// Signal the worker thread to stop and wait for it to do so
	m_stop = true;
	if(m_worker.joinable()) m_worker.join();
}

//---------------------------------------------------------------------------

#pragma warning(pop)
