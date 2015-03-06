/*
	���ƣ�MediaPlayer2.h
	������
	���ߣ�
	���ڣ�
*/
//*
#ifndef _MEDIA_PLAYER2_H_
#define _MEDIA_PLAYER2_H_

#include "cocos2d.h"
#include "typedef.h"
#include "SDL_stdinc.h"

#ifndef __STDC_CONSTANT_MACROS
#define __STDC_CONSTANT_MACROS
#endif

#ifndef INT64_C
#define INT64_C(c) (c ## LL)
#define UINT64_C(c) (c ## ULL)
#endif

#define VIDEO_PICTURE_QUEUE_SIZE 1

struct AVFormatContext;
struct AVStream;
struct AVPacket;
struct VideoPicture;
struct AVFrame;
struct AVCodecContext;
struct PacketQueue;
struct SwsContext;
struct SDL_mutex;
struct SDL_cond;

class MediaPlayer2 : public cocos2d::Sprite
{
public:
	MediaPlayer2();
	virtual ~MediaPlayer2();

	static MediaPlayer2* create(const char* path, int width, int height);

	void play(bool fromBegin = false);
	void stop(void);
	void pause(void);
	void seek(double sec);

	double getCurrentTime();
	double getTotalTime();

	void setVideoEndCallback(std::function<void()> func);       // �ر���Ƶ�ص�

protected:
	bool init(const char* path, int width, int height);

	bool initVideoContext();
	bool initAudioContext();

	//�����߳�
	static int decode_thread_S(void* arg);

	//��Ƶ���߳�
	static int video_thread_S(void* arg);

	static double synchronize_video_S(MediaPlayer2* player, AVFrame* src_frame, double pts);
	static int queue_picture_S(MediaPlayer2* player, AVFrame* pFrame, double pts);

	//���ػ�����ƹ���
	//virtual void draw(cocos2d::Renderer *renderer, const cocos2d::Mat4& transform, uint32_t flags) override;

	//��Ƶ���߳�
	static void audio_callback(void* userdata, Uint8* stream, int len);

	static int audio_decode_frame_S(MediaPlayer2* player, uint8_t* audio_buf, int buf_size);
	static int our_avcodec_decode_audio_S(AVCodecContext* avctx, int16_t* samples, int* frame_size_ptr, AVPacket* avpkt);
	double getAudioClock(); //��ȡ��Ƶʱ���

	//��Ƶ���߳�
	static int event_thread_S(void* arg);
	static void schedule_refresh_S(MediaPlayer2* player, int delay);
	static Uint32 sdl_refresh_timer_cb(Uint32 interval, void* opaque);

	static void alloc_picture_S(void *userdata);
	static void video_refresh_timer_S(void *userdata);
	static void video_display_S(MediaPlayer2* player);

	//packet�ж�
	static void packet_queue_init_S(PacketQueue *q);
	static int packet_queue_put_S(PacketQueue *q, AVPacket *pkt);
	static int packet_queue_get_S(PacketQueue *q, AVPacket *pkt, int block);

	static int decode_interrupt_cb(void* arg);

protected:
	static MediaPlayer2* _MediaPlayer2;

	//�̶�����
	std::string _filePath;
	int _width; //��Ƶ���
	int _height;
	
	double _totalTime; // ������Ƶ��ʱ��
	double _frameRate; // ֡��
	double _frameTime; // ��Ƶÿ֡��ʱ��

	//����
	double _elapsed; //�Ѳ��ŵ�ʱ�䣬����֡�ʿ���
	bool _requestStop;
	double _readFrameTimeBegin; //��Ƶ��ʼ֡��ʱ��
	int _startFrame; //��ʼʱ��
	int _currentFrame;

	//����Ƶ���ƽṹ
	AVFormatContext *_formatCtx;

	//��Ƶ
	int _nIndex_VideoStream;
	AVStream* _videoStream;
	PacketQueue* _videoQueue;
	double _videoClock;

	AVCodecContext* _videoCodecCtx;

	VideoPicture* _pictq;  
	int _pictq_size;
	int _pictq_rindex; //������
	int _pictq_windex; //д����
	SDL_mutex* _pictq_mutex;  
	SDL_cond* _pictq_cond;  

	double _frame_timer;
	double _frame_last_delay;
	double _frame_last_pts;

	SwsContext* _imgConvertCtx; //scale
	
	//��Ƶ
	int _nIndex_AudioStream;
	AVStream* _audioStream;
	PacketQueue* _audioQueue;
	double _audioClock;

	unsigned int _audio_buf_size;  
	unsigned int _audio_buf_index;
	uint8_t _audioBuf[1024 * 1024];

	AVPacket* _audio_pkt;
	uint8_t* _audio_pkt_data;
	unsigned int _audio_pkt_size;

	//������Ļص�
	std::function<void()> _videoEndCallback;	
};

#endif //_MEDIA_PLAYER2_H_
//*/





