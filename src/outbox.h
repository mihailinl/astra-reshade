#pragma once
// outbox: the link's messages to her engine, sent by a thread of their own. The link's frame runs on the game's present
// thread - or, before an Unreal game's tonemapper, on a thread recording the game's frame, inside the window the
// present thread waits for while it holds the game's D3D12 queue (pretone.cpp) - and a WebSocket send may block up to
// its timeout (a second). A push never blocks: it queues and wakes the sender.
// Portable (no Windows): the sending function is given; tests/core_tests.cpp drives it.
#include <condition_variable>
#include <cstddef>
#include <deque>
#include <functional>
#include <mutex>
#include <string>
#include <thread>
#include <utility>

class Outbox
{
public:
	/// Sends one message (false: not sent); the connection's generation now.
	using Sink = std::function<bool(const std::string &)>;
	using Generation = std::function<int()>;
	static constexpr size_t kMax = 256; // queued at most: a connection that stopped taking them drops the newest

	void start(Sink sink, Generation generation)
	{
		stop();
		m_sink = std::move(sink);
		m_generation = std::move(generation);
		m_quit = false;
		m_thread = std::thread([this] { run(); });
	}

	/// The sender ends after the send in flight (bounded by the sink's own timeout).
	void stop()
	{
		{
			std::lock_guard<std::mutex> lock(m_lock);
			m_quit = true;
			m_queue.clear();
		}
		m_wake.notify_all();
		if (m_thread.joinable())
			m_thread.join();
	}

	/// Queues `message` for the connection `generation`: sent in order, dropped if that connection is gone by then (a
	/// new one starts with its own hello) or the queue is full. Never blocks on the network.
	bool push(std::string message, int generation)
	{
		{
			std::lock_guard<std::mutex> lock(m_lock);
			if (m_quit || m_queue.size() >= kMax)
			{
				++m_dropped;
				return false;
			}
			m_queue.emplace_back(generation, std::move(message));
		}
		m_wake.notify_one();
		return true;
	}

	unsigned long long sent() const
	{
		std::lock_guard<std::mutex> lock(m_lock);
		return m_sent;
	}
	unsigned long long dropped() const
	{
		std::lock_guard<std::mutex> lock(m_lock);
		return m_dropped;
	}

	~Outbox()
	{
		stop();
	}

private:
	void run()
	{
		std::unique_lock<std::mutex> lock(m_lock);
		while (!m_quit)
		{
			m_wake.wait(lock, [this] { return m_quit || !m_queue.empty(); });
			if (m_quit)
				break;
			std::pair<int, std::string> m = std::move(m_queue.front());
			m_queue.pop_front();
			lock.unlock();
			const bool ok = m.first == m_generation() && m_sink(m.second);
			lock.lock();
			++(ok ? m_sent : m_dropped);
		}
	}

	mutable std::mutex m_lock;
	std::condition_variable m_wake;
	std::deque<std::pair<int, std::string>> m_queue;
	bool m_quit = false;
	unsigned long long m_sent = 0, m_dropped = 0;
	Sink m_sink;
	Generation m_generation;
	std::thread m_thread;
};
