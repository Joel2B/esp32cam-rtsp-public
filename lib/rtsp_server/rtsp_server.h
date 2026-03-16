#pragma once

#include <list>
#include <WiFiServer.h>
#include <ESPmDNS.h>
#include <OV2640.h>
#include <CRtspSession.h>
#include <arduino-timer.h>

class rtsp_server : public WiFiServer
{
public:
	rtsp_server(OV2640 &cam, unsigned long interval, int port = 554);

	void doLoop();

	size_t num_connected();
	float get_fps() const { return fps_ewma_; }
	size_t frames_sent() const { return frames_sent_; }

private:
	struct rtsp_client
	{
	public:
		rtsp_client(const WiFiClient &client,  OV2640 &cam);

		WiFiClient wifi_client;
		// Streamer for UDP/TCP based RTP transport
		std::shared_ptr<CStreamer> streamer;
		// RTSP session and state
		std::shared_ptr<CRtspSession> session;
	};

	OV2640 cam_;
	std::list<std::unique_ptr<rtsp_client>> clients_;
	uintptr_t task_;
	Timer<> timer_;
	uint32_t last_frame_ms_ = 0;
	float fps_ewma_ = 0.0f;         // promedio móvil exponencial
	size_t frames_sent_ = 0;
	static constexpr float FPS_ALPHA_ = 0.2f; // suaviza

	static bool client_handler(void *);
};