#include "stdafx.h"

#include <chrono>
#include <thread>
#include <utility>

#include "reliable.h"
#include "Listener.h"
#include "Connection.h"

// TODO: ping
// TODO: disconnect

using namespace sws;
using namespace std::chrono;
using namespace reliable;

static constexpr auto AGE_THRESHOLD = 1s;

Connection::Store::Store(sequence_t sequence, Packet packet)
	: creation_time_(clock::now()),
	  last_active(clock::now()),
	  sequence(sequence),
	  packet(std::move(packet))
{
}

Connection::Store::Store(Store&& other) noexcept
{
	*this = std::move(other);
}

Connection::Store& Connection::Store::operator=(Store&& other) noexcept
{
	sequence       = other.sequence;
	packet         = std::move(other.packet);
	creation_time_ = other.creation_time_;
	last_active    = other.last_active;

	return *this;
}

const Connection::clock::time_point& Connection::Store::creation_time() const
{
	return creation_time_;
}

bool Connection::Store::should_send(const clock::duration& duration) const
{
	return clock::now() - last_active > duration;
}

void Connection::Store::reset_activity()
{
	last_active = clock::now();
}

Connection::Connection(std::shared_ptr<UdpSocket> socket_, Listener* parent_, Address remote_address_)
	: socket(std::move(socket_)),
	  parent(parent_),
	  remote_address(std::move(remote_address_))
{
	for (auto& point : rtt_points)
	{
		point = 1s;
	}
}

Connection::Connection(Connection&& other) noexcept
{
	*this = std::move(other);
}

Connection& Connection::operator=(Connection&& other) noexcept
{
	parent = other.parent;
	socket = std::move(other.socket);

	inbound = std::move(other.inbound);

	remote_address = std::move(other.remote_address);

	ordered_out = std::move(other.ordered_out);
	uids_out    = std::move(other.uids_out);
	acknew_data    = std::move(other.acknew_data);

	seqs_in = std::move(other.seqs_in);
	uids_in = std::move(other.uids_in);

	rtt_points  = other.rtt_points;
	rtt_invalid = other.rtt_invalid;
	rtt_i       = other.rtt_i;
	rtt         = other.rtt;

	return *this;
}

SocketState Connection::send(Packet& packet, bool block)
{
	const auto read_pos  = packet.tell(SeekCursor::read);
	const auto write_pos = packet.tell(SeekCursor::write);

	packet.seek(SeekCursor::both, SeekType::from_start, 0);

	reliable_t type = reliable_t::none;
	manage_id  id   = manage_id::eop;

	ptrdiff_t sequence_offset = -1;
	sequence_t outbound_sequence = 0;

	do
	{
		packet >> id;

		switch (id)
		{
			case manage_id::eop:
				break;

			case manage_id::type:
				packet >> type;
				break;

			case manage_id::sequence:
			{
				sequence_offset = packet.tell(SeekCursor::read);

				sequence_t dummy_seq;
				packet >> dummy_seq;

				// to break out of the loop
				id = manage_id::eop;
				break;
			}

			default:
				throw;
		}
	} while (id != manage_id::eop);

	if (type == reliable_t::none)
	{
		if (sequence_offset != -1)
		{
			throw std::runtime_error("sequence specified in non-sequenced packet");
		}
	}
	else
	{
		if (sequence_offset == -1)
		{
			throw std::runtime_error("sequence offset was not reserved");
		}

		packet.seek(SeekCursor::write, SeekType::from_start, sequence_offset);

		switch (type)
		{
			case reliable_t::newest:
				outbound_sequence = ++faf_out;
				packet << outbound_sequence;
				break;

			case reliable_t::ack:
				outbound_sequence = ++uid_out;
				packet << outbound_sequence;
				uids_out.emplace(outbound_sequence, Store(outbound_sequence, packet));
				break;

			case reliable_t::ack_newest:
				outbound_sequence = ++acknew_out;
				packet << outbound_sequence;
				acknew_data = std::make_unique<Store>(outbound_sequence, packet);
				break;

			case reliable_t::ordered:
				outbound_sequence = ++seq_out;
				packet << outbound_sequence;
				ordered_out.emplace_back(outbound_sequence, packet);
				break;

			default:
				throw;
		}
	}

	SocketState result = socket->send_to(packet, remote_address);

	packet.seek(SeekCursor::read, SeekType::from_start, read_pos);
	packet.seek(SeekCursor::write, SeekType::from_start, write_pos);

	if (!block || result != SocketState::done)
	{
		return result;
	}

	switch (type)
	{
		case reliable_t::none:
		case reliable_t::newest:
			return result;

		case reliable_t::ack:
		{
			while (uids_out.find(outbound_sequence) != uids_out.end())
			{
				if ((result = parent->receive(true, 1)) == SocketState::error)
				{
					return result;
				}

				update();
				std::this_thread::sleep_for(1ms);
			}
			break;
		}

		case reliable_t::ack_newest:
		{
			while (acknew_data != nullptr)
			{
				if ((result = parent->receive(true, 1)) == SocketState::error)
				{
					return result;
				}

				update();
				std::this_thread::sleep_for(1ms);
			}

			break;
		}

		case reliable_t::ordered:
		{
			while (std::find_if(ordered_out.begin(), ordered_out.end(),
								[&](Store& s) { return s.sequence == outbound_sequence; }) != ordered_out.end())
			{
				if ((result = parent->receive(true, 1)) == SocketState::error)
				{
					return result;
				}

				update();
				std::this_thread::sleep_for(1ms);
			}
			break;
		}

		default: throw;
	}

	return result;
}

SocketState Connection::store_inbound(Packet& packet)
{
	SocketState result = SocketState::done;

	reliable_t reliable_type = reliable_t::none;
	manage_id id = manage_id::eop;

	sequence_t packet_sequence;

	do
	{
		packet >> id;

		if (id == manage_id::type)
		{
			if (reliable_type != reliable_t::none)
			{
				throw;
			}

			packet >> reliable_type;
			continue;
		}

		switch (id)
		{
			case manage_id::eop:
				continue;

			case manage_id::connect:
			{
				Packet p;
				p << manage_id::connected << manage_id::eop;

				socket->send_to(p, remote_address);

				id = manage_id::eop;
				result = SocketState::in_progress;
				break;
			}

			case manage_id::connected:
			case manage_id::bad_version:
				return SocketState::in_progress;

			case manage_id::sequence:
				if (reliable_type == reliable_t::none)
				{
					throw;
				}

				packet >> packet_sequence;
				break;

			case manage_id::ack:
			{
				reliable_t type = reliable_t::none;
				sequence_t sequence;

				packet >> type >> sequence;

				remove_outbound(type, sequence);
				break;
			}

			default:
				throw;
		}
	} while (id != manage_id::eop);

	if (reliable_type != reliable_t::none && reliable_type != reliable_t::newest)
	{
		Packet p;

		p << manage_id::ack << reliable_type << packet_sequence << manage_id::eop;
		socket->send_to(p, remote_address);

		if (handled(reliable_type, packet_sequence))
		{
			return SocketState::in_progress;
		}
	}

	inbound.emplace_back(std::move(packet));
	return result;
}

bool Connection::handled(reliable_t type, sequence_t sequence)
{
	switch (type)
	{
		case reliable_t::none:
			return false;

		case reliable_t::newest:
			if (sequence <= faf_in)
			{
				return true;
			}

			faf_in = sequence;
			return false;

		case reliable_t::ack:
		{
			auto it = uids_in.find(sequence);

			if (it != uids_in.end())
			{
				it->second = clock::now();
				return true;
			}

			uids_in[sequence] = clock::now();
			return false;
		}

		case reliable_t::ack_newest:
			if (sequence <= acknew_in)
			{
				return true;
			}

			acknew_in = sequence;
			return false;

		case reliable_t::ordered:
		{
			auto it = seqs_in.find(sequence);

			if (it != seqs_in.end())
			{
				it->second = clock::now();
				return true;
			}

			seqs_in[sequence] = clock::now();
			return false;
		}

		default:
			throw;
	}
}

void Connection::remove_outbound(reliable_t type, sequence_t sequence)
{
	switch (type)
	{
		case reliable_t::none:
			throw;

		case reliable_t::newest:
			return;

		case reliable_t::ack:
		{
			const auto it = uids_out.find(sequence);
			if (it != uids_out.end())
			{
				add_rtt_point(it->second.creation_time());
				uids_out.erase(it);
			}
			break;
		}

		case reliable_t::ack_newest:
			if (acknew_out == sequence && acknew_data != nullptr)
			{
				add_rtt_point(acknew_data->creation_time());
				acknew_data = nullptr;
			}
			break;

		case reliable_t::ordered:
		{
			const auto it = std::find_if(ordered_out.begin(), ordered_out.end(), [sequence](Store& s)
			{
				return s.sequence == sequence;
			});

			if (it != ordered_out.end())
			{
				add_rtt_point(it->creation_time());
				ordered_out.erase(it);
			}
			break;
		}

		default:
			throw;
	}
}

void Connection::prune()
{
	const auto now = clock::now();

	for (auto it = seqs_in.begin(); it != seqs_in.end();)
	{
		if (now - it->second >= AGE_THRESHOLD)
		{
			it = seqs_in.erase(it);
		}
		else
		{
			++it;
		}
	}

	for (auto it = uids_in.begin(); it != uids_in.end();)
	{
		if (now - it->second >= AGE_THRESHOLD)
		{
			it = uids_in.erase(it);
		}
		else
		{
			++it;
		}
	}
}

const Connection::clock::duration& Connection::round_trip_time()
{
	if (rtt_invalid)
	{
		clock::duration::rep duration {};

		for (auto& point : rtt_points)
		{
			duration += point.count();
		}

		duration /= rtt_points.size();
		rtt = clock::duration(duration);
		rtt_invalid = false;
	}

	return rtt;
}

void Connection::update()
{
	prune();

	const auto& rtt = round_trip_time();

	if (!ordered_out.empty())
	{
		auto& store = ordered_out.front();

		if (store.should_send(rtt))
		{
			add_rtt_point(store.creation_time());
			socket->send_to(store.packet, remote_address);
			store.reset_activity();
		}
	}

	for (auto& pair : uids_out)
	{
		auto& store = pair.second;

		if (store.should_send(rtt))
		{
			add_rtt_point(store.creation_time());
			socket->send_to(store.packet, remote_address);
			store.reset_activity();
		}
	}

	if (acknew_data != nullptr)
	{
		if (acknew_data->should_send(rtt))
		{
			add_rtt_point(acknew_data->creation_time());
			socket->send_to(acknew_data->packet, remote_address);
			acknew_data->reset_activity();
		}
	}
}

bool Connection::pop(Packet& packet)
{
	if (inbound.empty())
	{
		return false;
	}

	packet = std::move(inbound.front());
	inbound.pop_front();
	return true;
}

bool Connection::connected() const
{
	return connected_;
}

void Connection::add_rtt_point(const clock::time_point& point)
{
	rtt_points[rtt_i++] = clock::now() - point;
	rtt_i %= rtt_points.size();
	rtt_invalid = true;
}
