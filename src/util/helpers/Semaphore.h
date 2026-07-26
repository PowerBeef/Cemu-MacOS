#pragma once

#include <mutex>
#include <condition_variable>

class Semaphore
{
public:
	void notify() 
	{
		std::lock_guard lock(m_mutex);
		++m_count;
		m_condition.notify_one();
	}

	void wait() 
	{
		std::unique_lock lock(m_mutex);
		while (m_count == 0)
		{
			m_condition.wait(lock);
		}

		--m_count;
	}

	bool try_wait() 
	{
		std::lock_guard lock(m_mutex);
		if (m_count == 0)
			return false;

		--m_count;
		return true;
	}

	void reset()
	{
		std::lock_guard lock(m_mutex);
		m_count = 0;
	}

private:
	std::mutex m_mutex;
	std::condition_variable m_condition;
	uint64 m_count = 0;
};

class CounterSemaphore
{
public:
	void reset()
	{
		std::lock_guard lock(m_mutex);
		m_count = 0;
	}

	void increment()
	{
		std::lock_guard lock(m_mutex);
		++m_count;
		if (m_count == 1)
			m_condition.notify_all();
	}

	void decrement()
	{
		std::lock_guard lock(m_mutex);
		--m_count;
		cemu_assert_debug(m_count >= 0);
		if (m_count == 0)
			m_condition.notify_all();
	}

	// decrement only if non-zero
	// otherwise wait
	void decrementWithWait()
	{
		std::unique_lock lock(m_mutex);
		while (m_count == 0)
			m_condition.wait(lock);
		m_count--;
	}

	// decrement only if non-zero
	// otherwise wait
	// may wake up spuriously
	bool decrementWithWaitAndTimeout(uint32 ms)
	{
		std::unique_lock lock(m_mutex);
		if (m_count == 0)
		{
			m_condition.wait_for(lock, std::chrono::milliseconds(ms));
		}
		if (m_count == 0)
			return false;
		m_count--;
		return true;
	}

	void waitUntilZero()
	{
		std::unique_lock lock(m_mutex);
		while (m_count != 0)
			m_condition.wait(lock);
	}

	void waitUntilNonZero()
	{
		std::unique_lock lock(m_mutex);
		while (m_count == 0)
			m_condition.wait(lock);
	}

	// wait until non-zero or until the timeout expires, whichever comes first
	// returns true if the counter is non-zero on return
	// does not decrement, matching waitUntilNonZero
	bool waitUntilNonZeroWithTimeout(std::chrono::microseconds timeout)
	{
		std::unique_lock lock(m_mutex);
		if (m_count == 0)
			m_condition.wait_for(lock, timeout);
		return m_count != 0;
	}

	bool isZero() const
	{
		return m_count == 0;
	}

private:
	std::mutex m_mutex;
	std::condition_variable m_condition;
	sint64 m_count = 0;
};

template<typename T>
class StateSemaphore
{
public:
	StateSemaphore(T initialState) : m_state(initialState) {};

	T getValue()
	{
		std::unique_lock lock(m_mutex);
		return m_state;
	}

	bool hasState(T state)
	{
		std::unique_lock lock(m_mutex);
		return m_state == state;
	}

	void setValue(T newState)
	{
		std::unique_lock lock(m_mutex);
		m_state = newState;
		m_condition.notify_all();
	}

	void setValue(T newState, T expectedValue)
	{
		std::unique_lock lock(m_mutex);
		while (m_state != expectedValue)
			m_condition.wait(lock);
		m_state = newState;
		m_condition.notify_all();
	}

	void waitUntilValue(T state)
	{
		std::unique_lock lock(m_mutex);
		while (m_state != state)
			m_condition.wait(lock);
	}

private:
	std::mutex m_mutex;
	std::condition_variable m_condition;
	T m_state;
};