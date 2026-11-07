#pragma once
#include <NaWServer/request.h>
#include <NaWServer/sse.h>
#include <NaWServer/websocket.h>
namespace rp_demo::showcase
{
	NaWServer::SendResult info(NaWServer::Response &response);
	NaWServer::SendResult summary(NaWServer::Response &response);
	NaWServer::SendResult tasks(NaWServer::Request &request, NaWServer::Response &response);
	NaWServer::SendResult echo(NaWServer::Request &request, NaWServer::Response &response);
	NaWServer::SendResult stream(NaWServer::Response &response);
	NaWServer::SendResult command(NaWServer::Request &request, NaWServer::Response &response);
	NaWServer::SendResult openEvents(NaWServer::sse::EventSource &events, NaWServer::Request &request, NaWServer::Response &response);
	void message(NaWServer::WebSocketClient *client, const std::uint8_t *bytes, std::size_t size, bool binary);
	void poll(NaWServer::sse::EventSource &events, NaWServer::WebSocketEndpoint &socket);
}
