﻿
#pragma  once

#include "pubcore.hpp"

class IMrenderObserver
{
public:
	virtual void onMPoint(int32_t type, double upts) = 0;
};
class IMrender
{
public:
	virtual void start(void) = 0;
	virtual void stopd(bool stop_quik = false) = 0;
	virtual void pause(bool pauseflag = false) = 0;
	virtual void	update(const char* device, void* config = nullptr) = 0;
	virtual STATUS	status(void) = 0;
	virtual int32_t Q_size(void) = 0;
	virtual int32_t cached(void) = 0;
	virtual	void onMFrame(std::shared_ptr<MRframe> avfrm) = 0;
protected:
	virtual ~IMrender() = default;
};

class IMrenderFactory
{
public:
	static std::shared_ptr<IMrender>
		createAudioMrender(const char* device, std::shared_ptr<IMrenderObserver> observer = nullptr);
	static std::shared_ptr<IMrender>
		createVideoMrender(const void* device, std::shared_ptr<IMrenderObserver> observer = nullptr);
};

/************************************************************************/
/* Audio Mrender                                                        */
/************************************************************************/
class AudioMrender : public IMrender
{
public:
	AudioMrender(const char* device, 
		std::shared_ptr<IMrenderObserver> observer = nullptr);
	~AudioMrender();
	
	void start(void)											override;
	void stopd(bool stop_quik = false)							override;
	void pause(bool pauseflag = false)							override;
	STATUS	status(void)										override;
	void	update(const char* device, void* config = nullptr)  override;
	int32_t	Q_size(void)										override;
	int32_t	cached(void)										override;
	void onMFrame(std::shared_ptr<MRframe> av_frm)				override;
private:
	void onAudioRFrame(const char* data, int32_t size,
		int32_t sample_rate, int32_t nb_channels, int32_t nb_samples, int32_t format, double upts);
	void SET_STATUS(STATUS status);
	void clearAudioRqueue(bool is_capture = false);
	bool opendAudioDevice(bool is_capture = false);
	void closeAudioDevice(bool is_capture = false);
	bool resetAudioDevice(bool is_capture = false);
private:
	// Audio 
	std::atomic<STATUS>		m_status{ E_INVALID };
	std::atomic<bool>		m_signal_quit{ true };
	double					m_curpts{ 0 };
	void*					m_config;				//配置参数
	std::string 			m_speakr{ "" };
	AVCodecPars				m_codecp;				//媒体参数
	std::atomic<bool>		m_pauseflag{ false };
	std::thread 			m_worker;
	std::queue<AVRenderFrm>	m_render_Q;
	std::mutex				m_render_Q_mutx;
	std::condition_variable	m_render_Q_cond;
	std::weak_ptr<IMrenderObserver> m_observe;
	SDL_AudioSpec 			m_desire_spec{ 0 };
	SDL_AudioSpec 			m_device_spec{ 0 };
	SDL_AudioDeviceID 		m_audio_devID{ 0 };
	SDL_Event      			m_aevent;				//线程句柄
	//SDL_Thread*     		refresh_thread = NULL;	//线程句柄
};

class VideoMrender : public IMrender
{
public:
	VideoMrender(const void* device = nullptr, 
		std::shared_ptr<IMrenderObserver> observer = nullptr);
	~VideoMrender();
	void start(void)											override;
	void stopd(bool stop_quik = false)							override;
	void pause(bool pauseflag = false)							override;
	void	update(const char *device,void* config = nullptr)	override;
	STATUS	status(void)										override;
	int32_t	Q_size(void)										override;
	int32_t	cached(void)										override;
	void onMFrame(std::shared_ptr<MRframe> av_frm)				override;
private:
	void onVideoRFrame(const char *data, int32_t size,
		int32_t width, int32_t height, int32_t format, double upts);
	void SET_STATUS(STATUS status);
	void clearVideoRqueue(bool is_capture = false);
	bool opendVideoDevice(bool is_capture = false);
	void closeVideoDevice(bool is_capture = false);
	bool resetVideoDevice(bool is_capture = false);
private:
	std::atomic<STATUS>		m_status{ E_INVALID };
	std::atomic<bool>		m_signal_quit{ true };
	double					m_curpts{ 0 };
	void*					m_config;				//配置参数
	const void* 			m_window{ nullptr };
	AVCodecPars				m_codecp;				//媒体参数
	std::thread 			m_worker;
	std::atomic<bool>		m_pauseflag{ false };
	std::queue<AVRenderFrm>	m_render_Q;
	std::mutex				m_render_Q_mutx;
	std::condition_variable	m_render_Q_cond;
	std::weak_ptr<IMrenderObserver> m_observe;
	SDL_Window*    			m_screen{ nullptr };	//窗口句柄
	SDL_Renderer*   		m_render{ nullptr };	//渲染句柄
	SDL_Texture*    		m_textur{ nullptr };	//纹理句柄
	SDL_Rect       			m_rectgl;				//矩形区域	
	SDL_Event      			m_vevent;				//事件循环
	//SDL_Thread*     		refresh_thread = NULL;	//线程句柄
};
