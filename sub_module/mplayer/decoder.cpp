﻿
#include "pubcore.hpp"
#include "decoder.hpp"

std::shared_ptr<IAudioDecoder> IAudioDecoder::create(std::shared_ptr<IAudioDecoderObserver> observer)
{
	return std::make_shared<AudioDecoder>(observer);
}

AudioDecoder::AudioDecoder(std::shared_ptr<IAudioDecoderObserver> observer)
{
	updateStatus(E_INVALID);
	m_observe = observer;
	m_codec_par = avcodec_parameters_alloc();
	updateStatus(E_INITRES);
}

AudioDecoder::~AudioDecoder()
{
	if (E_STOPPED != status())
		stopd(true);
	avcodec_parameters_free(&m_codec_par);
	updateStatus(E_INVALID);
}

void AudioDecoder::start(void)
{
	if (E_STARTED == status() || E_STRTING == status())
		return;
	updateStatus(E_STRTING);
	
	m_signal_quit = false;
	m_worker = std::thread([&]()
	{
		int32_t ret = 0;
		while (!m_signal_quit)
		{
			if (m_pauseflag)
			{
				std::shared_ptr<MRframe> avfrm = std::make_shared<MRframe>();
				SET_PROPERTY(avfrm->prop, P_PAUS);
				if (!m_observe.expired())
					m_observe.lock()->onAudioRFrame(avfrm);
				av_usleep(10 * 1000);
				continue;
			}

			// 1.receive and dec MPacket.
			std::shared_ptr<MPacket> avpkt = nullptr;
			{
				std::unique_lock<std::mutex>locker(m_decoder_Q_mutx);
				m_decoder_Q_cond.wait(locker, [&]() {
					return (!m_decoder_Q.empty() || m_signal_quit);
				});
				if (m_signal_quit || m_decoder_Q.empty())
					continue;
				avpkt = m_decoder_Q.front();
				m_decoder_Q.pop();
			}

			// 2.check and reset decoder.
			if (!m_codec_ctx || (m_codec_ctx && (m_codec_ctx->pix_fmt != avpkt->pars->format
				|| m_codec_ctx->width != avpkt->pars->width
				|| m_codec_ctx->height != avpkt->pars->height)))
			{
				if ((ret = avcodec_parameters_copy(m_codec_par, avpkt->pars)) < 0) {
					av_log(nullptr, AV_LOG_ERROR, "avcodec_parameters_copy failed! ret=%d\n", ret);
					break;
				}
				m_framerate = avpkt->ufps;
				if (!resetCodecer(true))
					break;
			}

			// 3.scale AVPacket timebase. <stream->codec: eg.1/25 1/44100>
			av_packet_rescale_ts(avpkt->ppkt, avpkt->sttb, m_codec_ctx->time_base);

			// 4. send packet to decoder.
			if ((ret = avcodec_send_packet(m_codec_ctx, avpkt->ppkt)) < 0)
			{
				if (ret != AVERROR_EOF) {
					av_log(nullptr, AV_LOG_ERROR, "avcodec_send_packet failed! ret=%d\n", ret);
					break;
				}
				av_usleep(10 * 1000);
				continue;
			}

			// 5.receive frame from decoder.
			while (true)
			{
				std::shared_ptr<MRframe> avfrm = std::make_shared<MRframe>();

				if ((ret = avcodec_receive_frame(m_codec_ctx, avfrm->pfrm))< 0)
				{
					if (ret != -(EAGAIN) && ret != AVERROR_EOF)
						av_log(nullptr, AV_LOG_ERROR, "Decoding failed!ret=%d\n", ret);
					break;
				}
				//debug_write_pcm(avfrm->pfrm);
				// 6.打包发送到渲染器.				
				avfrm->type = avpkt->type;
				avfrm->prop = avpkt->prop;
				avfrm->upts = avpkt->upts;
				avfrm->sttb = m_codec_ctx->time_base;// avpkt->sttb;
				if ((ret = avcodec_parameters_copy(avfrm->pars, avpkt->pars)) < 0) {
					av_log(nullptr, AV_LOG_ERROR, "avcodec_parameters_copy failed! ret=%d\n", ret);
					break;
				}
				avfrm->pfrm->pts = avfrm->pfrm->best_effort_timestamp;

				if (!m_observe.expired() && avfrm->type == AVMEDIA_TYPE_AUDIO)
					m_observe.lock()->onAudioRFrame(avfrm);
			}
		}
		SDL_Log("Audio decoder finished! ret=%d\n", ret);
	});

	updateStatus(E_STARTED);
}

void AudioDecoder::stopd(bool stop_quik)
{
	if (E_STOPPED == status() || E_STOPING == status())
		return;
	updateStatus(E_STOPING);
	m_signal_quit = true;
	if (m_worker.joinable()) m_worker.join();
	updateStatus(E_STOPPED);
}

void AudioDecoder::pause(bool pauseflag)
{
	m_pauseflag = pauseflag;
}

void AudioDecoder::update(void * config)
{
	m_config = config;
}

STATUS AudioDecoder::status(void)
{
	return m_status;
}

int32_t AudioDecoder::Q_size(void)
{
	std::lock_guard<std::mutex> locker(m_decoder_Q_mutx);
	return m_decoder_Q.size();
}

void AudioDecoder::onAudioPacket(std::shared_ptr<MPacket> av_pkt)
{
	if (!m_signal_quit)
	{
		if (CHK_PROPERTY(av_pkt->prop,  P_SEEK))
			clearCodec_Q(true);
		std::lock_guard<std::mutex> locker(m_decoder_Q_mutx);		
		if (!CHK_PROPERTY(av_pkt->prop, P_PAUS)) 
			m_decoder_Q.push(av_pkt);
		m_decoder_Q_cond.notify_all();
	}
}

void AudioDecoder::updateStatus(STATUS status)
{
	m_status = status;
}

void AudioDecoder::closeCodecer(bool is_decoder)
{
	if (is_decoder)
	{
		avcodec_free_context(&m_codec_ctx);
	}
}

bool AudioDecoder::opendCodecer(bool is_decoder)
{
	if (is_decoder)
	{
		int32_t ret = 0;
		// Open decoder base on m_codec & m_codec_ctx.
		if (!(m_codec = avcodec_find_decoder(m_codec_par->codec_id)))
		{
			av_log(nullptr, AV_LOG_ERROR, "Failed to find decoder for stream #%d\n", m_codec_par->codec_id);
			return false;
		}

		if (!(m_codec_ctx = avcodec_alloc_context3(m_codec)))
		{
			av_log(nullptr, AV_LOG_ERROR, "Failed to allocate the decoder context for stream !");
			return false;
		}

		if ((ret = avcodec_parameters_to_context(m_codec_ctx, m_codec_par)) < 0)
		{
			av_log(nullptr, AV_LOG_ERROR, "Failed to copy decoder parameters to input decoder context\n");
			return false;
		}

		// Open decoder base on m_codec & m_codec_ctx.
		if (	m_codec_ctx->codec_type == AVMEDIA_TYPE_VIDEO
			||	m_codec_ctx->codec_type == AVMEDIA_TYPE_AUDIO)
		{
			if (m_codec_ctx->codec_type == AVMEDIA_TYPE_VIDEO)
				m_codec_ctx->framerate = m_framerate;
			if ((ret = avcodec_open2(m_codec_ctx, m_codec, nullptr)) < 0) {
				av_log(nullptr, AV_LOG_ERROR, "Failed to open decoder for stream\n");
				return false;
			}
		}
	}

	return true;
}

bool AudioDecoder::resetCodecer(bool is_decoder)
{
	if (is_decoder)
	{
		closeCodecer(is_decoder);
		return opendCodecer(is_decoder);
	}
	return false;
}

void AudioDecoder::clearCodec_Q(bool is_decoder)
{
	if (is_decoder)
	{
		std::lock_guard<std::mutex> locker(m_decoder_Q_mutx);
		while (!m_decoder_Q.empty())
			m_decoder_Q.pop();
	}
	return;
}

std::shared_ptr<IVideoDecoder> IVideoDecoder::create(std::shared_ptr<IVideoDecoderObserver> observer)
{
	return std::make_shared<VideoDecoder>(observer);
}

VideoDecoder::VideoDecoder(std::shared_ptr<IVideoDecoderObserver> observer)
{
	updateStatus(E_INVALID);
	m_observe = observer;
	m_codec_par = avcodec_parameters_alloc();
	updateStatus(E_INITRES);
}

VideoDecoder::~VideoDecoder()
{
	if (E_STOPPED != status())
		stopd(true);
	avcodec_parameters_free(&m_codec_par);
	updateStatus(E_INVALID);
}

void VideoDecoder::start(void)
{
	if (E_STARTED == status() || E_STRTING == status())
		return;
	updateStatus(E_STRTING);

	m_signal_quit = false;
	m_worker = std::thread([&]()
	{
		int32_t ret = 0;
		while (!m_signal_quit)
		{
			if (m_pauseflag)
			{
				std::shared_ptr<MRframe> avfrm = std::make_shared<MRframe>();
				SET_PROPERTY(avfrm->prop, P_PAUS);
				if (!m_observe.expired())
					m_observe.lock()->onVideoRFrame(avfrm);
				av_usleep(10 * 1000);
				continue;
			}
			// 1.receive and dec MPacket.
			std::shared_ptr<MPacket> avpkt = nullptr;
			{
				std::unique_lock<std::mutex>locker(m_decoder_Q_mutx);
				m_decoder_Q_cond.wait(locker, [&]() {
					return (!m_decoder_Q.empty() || m_signal_quit);
				});
				if (m_signal_quit || m_decoder_Q.empty())
					continue;
				avpkt = m_decoder_Q.front();
				m_decoder_Q.pop();
			}
			
			// 2.check and reset decoder.
			if (  !m_codec_ctx || (m_codec_ctx && (m_codec_ctx->pix_fmt != avpkt->pars->format
				|| m_codec_ctx->width  != avpkt->pars->width
				|| m_codec_ctx->height != avpkt->pars->height)))
			{
				if ((ret = avcodec_parameters_copy(m_codec_par, avpkt->pars)) < 0) {
					av_log(nullptr, AV_LOG_ERROR, "avcodec_parameters_copy failed! ret=%d\n", ret);
					break;
				}
				m_framerate = avpkt->ufps;
				if (!resetCodecer(true))
					break;
			}

			// 3.scale AVPacket timebase. <stream->codec: eg.1/25 1/44100>
			av_packet_rescale_ts(avpkt->ppkt, avpkt->sttb, m_codec_ctx->time_base);
			
			// 4. send packet to decoder.
			if ((ret = avcodec_send_packet(m_codec_ctx, avpkt->ppkt)) < 0)
			{
				if (ret != AVERROR_EOF) {
					av_log(nullptr, AV_LOG_ERROR, "avcodec_send_packet failed! ret=%d\n", ret);
					break;
				}
				av_usleep(10 * 1000);
				continue;
			}

			// 5.receiveframe from decoder.
			while (true) 
			{
				std::shared_ptr<MRframe> avfrm = std::make_shared<MRframe>();
				
				if ((ret = avcodec_receive_frame(m_codec_ctx, avfrm->pfrm))< 0)
				{
					if (ret != -(EAGAIN) && ret != AVERROR_EOF)
						av_log(nullptr, AV_LOG_ERROR, "Decoding failed!ret=%d\n", ret);
					break;
				}
				
				// 6.打包发送到渲染器.				
				avfrm->type = avpkt->type;
				avfrm->prop = avpkt->prop;
				avfrm->upts = avpkt->upts;
				avfrm->sttb = m_codec_ctx->time_base;// avpkt->sttb;
				if ((ret = avcodec_parameters_copy(avfrm->pars, avpkt->pars)) < 0) {
					av_log(nullptr, AV_LOG_ERROR, "avcodec_parameters_copy failed! ret=%d\n", ret);
					break;
				}
				avfrm->pfrm->pts = avfrm->pfrm->best_effort_timestamp;

				if (!m_observe.expired() && avfrm->type == AVMEDIA_TYPE_VIDEO)
					m_observe.lock()->onVideoRFrame(avfrm);
			}
		}
		SDL_Log("Video decoder finished! ret=%d\n", ret);
	});

	updateStatus(E_STARTED);
}

void VideoDecoder::stopd(bool stop_quik)
{
	if (E_STOPPED == status() || E_STOPING == status())
		return;
	updateStatus(E_STOPING);
	m_signal_quit = true;
	if (m_worker.joinable()) m_worker.join();
	updateStatus(E_STOPPED);
}

void VideoDecoder::pause(bool pauseflag)
{
	m_pauseflag = pauseflag;
}

void VideoDecoder::update(void * config)
{
	m_config = config;
}

STATUS VideoDecoder::status(void)
{
	return m_status;
}

int32_t VideoDecoder::Q_size(void)
{
	std::lock_guard<std::mutex> locker(m_decoder_Q_mutx);
	return m_decoder_Q.size();
}

void VideoDecoder::onVideoPacket(std::shared_ptr<MPacket> av_pkt)
{
	if (!m_signal_quit)
	{
		if (CHK_PROPERTY(av_pkt->prop,  P_SEEK))
			clearCodec_Q(true);
		std::lock_guard<std::mutex> locker(m_decoder_Q_mutx);
		if (!CHK_PROPERTY(av_pkt->prop, P_PAUS))
			m_decoder_Q.push(av_pkt);
		m_decoder_Q_cond.notify_all();
	}
}

void VideoDecoder::updateStatus(STATUS status)
{
	m_status = status;
}

void VideoDecoder::closeCodecer(bool is_decoder)
{
	if (is_decoder)
	{
		avcodec_free_context(&m_codec_ctx);
	}
}

bool VideoDecoder::opendCodecer(bool is_decoder)
{
	if (is_decoder)
	{
		int32_t ret = 0;
		// Open decoder base on m_codec & m_codec_ctx.
		if (!(m_codec = avcodec_find_decoder(m_codec_par->codec_id)))
		{
			av_log(nullptr, AV_LOG_ERROR, "Failed to find decoder for stream #%d\n", m_codec_par->codec_id);
			return false;
		}

		if (!(m_codec_ctx = avcodec_alloc_context3(m_codec))) 
		{
			av_log(nullptr, AV_LOG_ERROR, "Failed to allocate the decoder context for stream !");
			return false;
		}

		if ((ret = avcodec_parameters_to_context(m_codec_ctx, m_codec_par)) < 0)
		{
			av_log(nullptr, AV_LOG_ERROR, "Failed to copy decoder parameters to input decoder context\n");
			return false;
		}

		// Open decoder base on m_codec & m_codec_ctx.
		if (	m_codec_ctx->codec_type == AVMEDIA_TYPE_VIDEO
			||  m_codec_ctx->codec_type == AVMEDIA_TYPE_AUDIO)
		{
			if (m_codec_ctx->codec_type == AVMEDIA_TYPE_VIDEO)
				m_codec_ctx->framerate = m_framerate;
			if ((ret = avcodec_open2(m_codec_ctx, m_codec, nullptr)) < 0) {
				av_log(nullptr, AV_LOG_ERROR, "Failed to open decoder for stream\n");
				return false;
			}
		}
	}
	return true;
}

bool VideoDecoder::resetCodecer(bool is_decoder)
{
	if (is_decoder)
	{
		closeCodecer(is_decoder);
		return opendCodecer(is_decoder);
	}
	return false;
}

void VideoDecoder::clearCodec_Q(bool is_decoder)
{
	if (is_decoder)
	{
		std::lock_guard<std::mutex> locker(m_decoder_Q_mutx);
		while (!m_decoder_Q.empty())
			m_decoder_Q.pop();
	}
	return;
}
