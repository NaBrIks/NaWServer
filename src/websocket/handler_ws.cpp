// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Oleg Kozhevnikov

#include <NaWServer/websocket.h>
#include <NaWServer/response.h>
#include <cstring>
#include <new>
#include <utility>

namespace NaWServer
{
	namespace
	{
		WebSocketSendResult sendCopy(const std::shared_ptr<WebSocketSession> &session,
		    const WebSocketLimits &limits, const uint8_t *data, std::size_t size, bool binary)
		{
			if(!data && size) return WebSocketSendResult::InvalidArgument;

			if(size > limits.max_message_size) return WebSocketSendResult::TooLarge;

			return session->sendCopy(data, size, binary);
		}

		void record(WebSocketBroadcastResult &summary, WebSocketSendResult result)
		{
			if(result == WebSocketSendResult::Queued) ++summary.queued;
			else
			{
				if(!summary.rejected) summary.first_error = result;

				++summary.rejected;
			}
		}

	}

	WebSocketReservation::WebSocketReservation(WebSocketEndpoint *owner)
	  : owner_(owner)
	{
	}

	WebSocketReservation::~WebSocketReservation()
	{
		rollback();
	}

	WebSocketReservation::WebSocketReservation(WebSocketReservation &&other) noexcept
	  : owner_(std::exchange(other.owner_, nullptr)), client_(std::exchange(other.client_, nullptr)),
	    node_(std::move(other.node_)), committed_(std::exchange(other.committed_, false))
	{
	}

	WebSocketReservation &WebSocketReservation::operator=(WebSocketReservation &&other) noexcept
	{
		if(this != &other)
		{
			rollback();
			owner_ = std::exchange(other.owner_, nullptr);
			client_ = std::exchange(other.client_, nullptr);
			node_ = std::move(other.node_);
			committed_ = std::exchange(other.committed_, false);
		}

		return *this;
	}

	bool WebSocketReservation::valid() const
	{
		return owner_ && client_ && !node_.empty();
	}

	bool WebSocketReservation::committed() const
	{
		return committed_;
	}

	WebSocketEndpoint *WebSocketReservation::owner() const
	{
		return owner_;
	}

	bool WebSocketReservation::commit(WebSocketExchange &exchange)
	{
		return valid() && owner_->commit(*this, exchange);
	}

	void WebSocketReservation::rollback()
	{
		if(!owner_)
			return;

		auto *owner = std::exchange(owner_, nullptr);
		auto *client = std::exchange(client_, nullptr);
		node_ = {};
		if(client)
		{
			client->transportSession().invalidate();
			delete client;
		}

		NaWServer::detail::LockGuard<NaWServer::detail::Mutex> lock(owner->mutex_);
		--owner->reserved_clients_;
	}

	HttpError WebSocketEndpoint::validate() const
	{
		return !url_.empty() && url_.front() == '/' && url_.find('?') == std::string::npos &&
		       limits_.max_clients && limits_.max_pending_messages && limits_.max_message_size &&
		       limits_.max_pending_bytes
		    ? HttpError::Ok : HttpError::InvalidArgument;
	}

	WebSocketReservation WebSocketEndpoint::prepare(std::shared_ptr<WebSocketSession> session)
	{
		if(!session)
			return {};

		bool admitted = false;
		{
			NaWServer::detail::LockGuard<NaWServer::detail::Mutex> lock(mutex_);
			if(sessions_.size() < limits_.max_clients &&
			    reserved_clients_ < limits_.max_clients - sessions_.size())
			{
				++reserved_clients_;
				admitted = true;
			}
		}
		if(!admitted)
		{
			session->invalidate();
			return {};
		}

		WebSocketReservation reservation(this);
		reservation.client_ = new(std::nothrow) WebSocketClient(this, std::move(session));
		if(!reservation.client_)
		{
			if(session)
				session->invalidate();

			return {};
		}

		// The temporary set allocates its node now; extracting transfers it into
		// the reservation without exposing a pre-101 client to broadcasts.
		std::set<WebSocketClient *> pending;
		const auto inserted = pending.insert(reservation.client_);
		reservation.node_ = pending.extract(inserted.first);
		return reservation;
	}

	bool WebSocketEndpoint::commit(WebSocketReservation &reservation, WebSocketExchange &exchange)
	{
		if(reservation.owner_ != this || !reservation.valid() || !exchange.opening() || exchange.client())
			return false;

		auto *client = reservation.client_;
		{
			NaWServer::detail::LockGuard<NaWServer::detail::Mutex> lock(mutex_);
			// A unique live client pointer cannot collide with an active client.
			// Equal default allocators transfer this prepared node without allocation.
			sessions_.insert(std::move(reservation.node_));
			--reserved_clients_;
		}
		reservation.owner_ = nullptr;
		reservation.client_ = nullptr;
		reservation.committed_ = true;
		// A prepared transport is ready after 101. attach only publishes the client
		// to its serialized owner; producers may already queue through the live token.
		exchange.attach(client);
		if(connected_)
			connected_(client);

		return true;
	}

	void WebSocketEndpoint::disconnect(void *context)
	{
		auto *client = static_cast<WebSocketClient *>(context);
		if(!client) return;

		client->session_->invalidate();
		{
			NaWServer::detail::LockGuard<NaWServer::detail::Mutex> lock(client->owner_->mutex_);
			client->owner_->sessions_.erase(client);
		}
		delete client;
	}

	void WebSocketEndpoint::handleRequest(Request *request, Response &response)
	{
		auto *exchange = static_cast<WebSocketExchange *>(request->transportExtension(&websocket_exchange_key));
		if(!exchange)
		{
			(void)response.text(400, "WebSocket upgrade required");
			return;
		}

		if(exchange->opening())
		{
			if(auto *prepared = exchange->preparedAdmission())
			{
				if(prepared->owner() != this || !prepared->commit(*exchange))
					exchange->close();
			}
			else
			{
				auto opening_reservation = prepare(exchange->createSession(limits_));
				if(!opening_reservation.commit(*exchange))
					exchange->close();
			}

			return;
		}

		auto *client = exchange->client();
		if(!client) { exchange->close(); return; }

		const auto message = exchange->readMessage(limits_);
		if(message.status == WebSocketReadStatus::Message)
		{
			if(message.bytes.size() > limits_.max_message_size || (!message.bytes.data() && message.bytes.size()))
			{
				exchange->close();
				return;
			}

			if(message_) message_(client, message.bytes.data(), message.bytes.size(), message.binary);
		}
		else if(message.status == WebSocketReadStatus::Closed || message.status == WebSocketReadStatus::Error)
			exchange->close();
	}

	WebSocketBroadcastResult WebSocketEndpoint::send(const char *data) const
	{
		NaWServer::detail::LockGuard<NaWServer::detail::Mutex> lock(mutex_);
		WebSocketBroadcastResult summary;
		for(auto *client : sessions_)
			record(summary, data ? sendCopy(client->session_, limits_, reinterpret_cast<const uint8_t *>(data),
			    std::strlen(data), false) : WebSocketSendResult::InvalidArgument);
		return summary;
	}

	WebSocketBroadcastResult WebSocketEndpoint::send_binary(const uint8_t *data, size_t size) const
	{
		NaWServer::detail::LockGuard<NaWServer::detail::Mutex> lock(mutex_);
		WebSocketBroadcastResult summary;
		for(auto *client : sessions_)
			record(summary, sendCopy(client->session_, limits_, data, size, true));
		return summary;
	}

	WebSocketSendResult WebSocketClient::send(const char *data)
	{
		return data ? sendCopy(session_, owner_->limits_, reinterpret_cast<const uint8_t *>(data), std::strlen(data),
		           false)
		    : WebSocketSendResult::InvalidArgument;
	}

	WebSocketSendResult WebSocketClient::send_binary(const uint8_t *data, size_t size)
	{
		return sendCopy(session_, owner_->limits_, data, size, true);
	}

	WebSocketBroadcastResult WebSocketEndpoint::send_binary(const uint8_t *data, size_t size, WritePolicy policy) const
	{
		NaWServer::detail::LockGuard<NaWServer::detail::Mutex> lock(mutex_);
		WebSocketBroadcastResult result;
		for(auto *client : sessions_)
			record(result, !data && size ? WebSocketSendResult::InvalidArgument :
			    size > limits_.max_message_size ? WebSocketSendResult::TooLarge :
			    client->session_->sendCopyPolicy(data, size, true, policy));

		return result;
	}

	WebSocketSendResult WebSocketClient::send_binary(const uint8_t *data, size_t size, WritePolicy policy)
	{
		if(!data && size) return WebSocketSendResult::InvalidArgument;

		if(size > owner_->limits_.max_message_size) return WebSocketSendResult::TooLarge;

		return session_->sendCopyPolicy(data, size, true, policy);
	}

}
