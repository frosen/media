#include "MediaPlayer2.h"

extern "C" {
//FFMPEG
#include "libavformat/avformat.h"
#include "libswscale/swscale.h"
#include "libswresample/swresample.h"

#include "SDL.h"
#include "SDL_thread.h"

}

USING_NS_CC;

#define VIDEO_DEBUG 1

#define SDL_AUDIO_BUFFER_SIZE 8192 

#define MAX_AUDIOQ_SIZE (5 * 16 * 8192)  
#define MAX_VIDEOQ_SIZE (5 * 256 * 8192)

#define FF_ALLOC_EVENT   (SDL_USEREVENT)  
#define FF_REFRESH_EVENT (SDL_USEREVENT + 1)  
#define FF_QUIT_EVENT (SDL_USEREVENT + 2)

#define AV_SYNC_THRESHOLD 0.01  
#define AV_NOSYNC_THRESHOLD 10.0

#define MYLOG(STR) log(STR)

typedef struct PacketQueue  
{  
	AVPacketList *first_pkt, *last_pkt;  
	int nb_packets;  
	int size;  
	SDL_mutex* mutex;  
	SDL_cond* cond;  
} PacketQueue;

//=============================================

typedef struct VideoPicture  
{  
	AVPicture* picture;   
	int allocated;  
	double pts;  
} VideoPicture;

//======================================================

/* These are called whenever we allocate a frame 
* buffer. We use this to store the global_pts in 
* a frame at the time it is allocated. 
*/

uint64_t global_video_pkt_pts = AV_NOPTS_VALUE; 

int our_get_buffer(struct AVCodecContext *c, AVFrame *pic)  
{  
	int ret = avcodec_default_get_buffer(c, pic);  
	uint64_t *pts = (uint64_t *)av_malloc(sizeof(uint64_t));  
	*pts = global_video_pkt_pts;  
	pic->opaque = pts;  
	return ret;  
}  
void our_release_buffer(struct AVCodecContext *c, AVFrame *pic)  
{  
	if(pic) av_freep(&pic->opaque);  
	avcodec_default_release_buffer(c, pic);  
}

//======================================================

//========================================================

MediaPlayer2* MediaPlayer2::_MediaPlayer2 = nullptr;

MediaPlayer2::MediaPlayer2():
	Sprite(),
	_formatCtx(nullptr),
	_videoStream(nullptr),
	_videoClock(0),
	_videoCodecCtx(nullptr),
	_pictq_size(0),
	_pictq_rindex(0),
	_pictq_windex(0),
	_frame_timer(0),
	_frame_last_delay(0),
	_frame_last_pts(0),
	_imgConvertCtx(nullptr),
	_audioStream(nullptr),
	_audio_buf_size(0),
	_audio_buf_index(0),
	_audio_pkt_data(nullptr),
	_audio_pkt_size(0)

{
	static AVPacket audio_pkt;
	_audio_pkt = &audio_pkt;

	static PacketQueue videoQueue;
	_videoQueue = &videoQueue;

	static PacketQueue audioQueue;
	_audioQueue = &audioQueue;

	static VideoPicture pictq[VIDEO_PICTURE_QUEUE_SIZE];
	_pictq = pictq;

	memset(_pictq, 0, sizeof(VideoPicture) * VIDEO_PICTURE_QUEUE_SIZE);
}

MediaPlayer2::~MediaPlayer2()
{
	if (_imgConvertCtx != nullptr) {
		sws_freeContext(_imgConvertCtx);
	}

	if (_videoCodecCtx) avcodec_close(_videoCodecCtx);

	if (_formatCtx) {
		avformat_close_input(&_formatCtx);
	}
}

MediaPlayer2* MediaPlayer2::create( const char* path, int width, int height )
{
	if(!_MediaPlayer2)
	{
		_MediaPlayer2 = new (std::nothrow) MediaPlayer2();
		if (_MediaPlayer2 && _MediaPlayer2->init(path, width, height)) 
		{
			_MediaPlayer2->autorelease();
		} 
		else 
		{
			CC_SAFE_DELETE(_MediaPlayer2);
			_MediaPlayer2 = nullptr;
		}
	}
	
	return _MediaPlayer2;
}

void MediaPlayer2::play( bool fromBegin /*= false*/ )
{
	_requestStop = false;
	
	if (fromBegin) _elapsed = 0;

	seek((double)_elapsed);

	//开启事件线程
	if(!SDL_CreateThread(MediaPlayer2::event_thread_S, "event", this))
	{
		log("decode_thread fails");
		return;
	}

	//开启解码线程
	if(!SDL_CreateThread(MediaPlayer2::decode_thread_S, "decode", this))
	{
		log("decode_thread fails");
		return;
	}

	//开启视频流处理线程
	if(!SDL_CreateThread(MediaPlayer2::video_thread_S, "video", this))
	{
		log("video_thread fails");
		return;
	}

	//开启音频流处理线程	
	SDL_PauseAudio(0);

	//开始刷新屏幕
	//schedule_refresh_S(this, 40);
}

void MediaPlayer2::stop( void )
{
	_requestStop = true;
	_elapsed = 0;
}

void MediaPlayer2::pause( void )
{
	_requestStop = true;
}

void MediaPlayer2::seek( double sec )
{
	_startFrame = sec / _frameTime;//(int64_t)((double)timeBase.den / timeBase.num * sec);
	_currentFrame = _startFrame;
	avformat_seek_file(_formatCtx, _nIndex_VideoStream, _startFrame, _startFrame, _startFrame, AVSEEK_FLAG_FRAME);
	avcodec_flush_buffers(_videoCodecCtx);
	_readFrameTimeBegin = (double)utils::gettime() - sec;
	_elapsed = sec;
}

double MediaPlayer2::getCurrentTime()
{
	return _elapsed;
}

double MediaPlayer2::getTotalTime()
{
	return _totalTime;
}

void MediaPlayer2::setVideoEndCallback( std::function<void()> func )
{

}

bool MediaPlayer2::init( const char* path, int width, int height )
{
	if (!Sprite::init()) return false;

	_filePath = FileUtils::getInstance()->fullPathForFilename(path);
	_width = width;
	_height = height;

	setContentSize(Size(_width, _height));
	
	// Register all formats and codecs
	av_register_all();
	
	if(avformat_open_input(&_formatCtx, _filePath.c_str(), NULL, NULL) != 0) 
	{
		log("avformat_open_input failed. %s", _filePath.c_str());
		return false;
	}

	// 获取流信息
	if(avformat_find_stream_info(_formatCtx, NULL) < 0) {
		log("avformat_find_stream_info failed.");
		return false;
	}

#if VIDEO_DEBUG
	av_dump_format(_formatCtx, 0, path, 0);
#endif

	// 查找视频流和音频流的索引
	_nIndex_VideoStream = -1;
	_nIndex_AudioStream = -1;

	log("stream count = %d", _formatCtx->nb_streams);
	for(int i = 0; i < (int)_formatCtx->nb_streams; ++i) 
	{
		if(_formatCtx->streams[i]->codec->codec_type == AVMEDIA_TYPE_VIDEO)
		{
			_nIndex_VideoStream = i;
			continue;
		}

		if (_formatCtx->streams[i]->codec->codec_type == AVMEDIA_TYPE_AUDIO) 
			_nIndex_AudioStream = i;
	}

	//初始化视频
	if (_nIndex_VideoStream != -1)
	{
		if (!initVideoContext())
		{
			log("init Video Context failed");
			return false;
		}
	}

	//初始化音频
	if (_nIndex_AudioStream != -1)
	{
		if (!initAudioContext())
		{
			log("init Audio Context failed");
			return false;
		}
	}

	_totalTime = _formatCtx->duration / 1000000;  //milli second;

	_elapsed = 0;

	//退出函数
	static const AVIOInterruptCB int_cb = {MediaPlayer2::decode_interrupt_cb, NULL};
	_formatCtx->interrupt_callback = int_cb;
	
	return true;
}

bool MediaPlayer2::initVideoContext()
{
	// Get a pointer to the codec context for the video stream
	_videoCodecCtx = _formatCtx->streams[_nIndex_VideoStream]->codec;

	// Find the decoder for the video stream
	AVCodec* pCodec = avcodec_find_decoder(_videoCodecCtx->codec_id);

	if (!pCodec || avcodec_open2(_videoCodecCtx, pCodec, NULL) != 0)
	{
		log("avcodec_find_decoder or avcodec_open2 fail");
		return false;
	}

	// 获取视频帧率
	_frameRate = av_q2d(_formatCtx->streams[_nIndex_VideoStream]->r_frame_rate);
	_frameTime = 1.0 / _frameRate;

	//初始化视频流
	_videoStream = _formatCtx->streams[_nIndex_VideoStream];

	_frame_timer = (double)utils::gettime(); //？
	_frame_last_delay = 40e-3; //0.04

	packet_queue_init_S(_videoQueue);

	_videoCodecCtx->get_buffer = our_get_buffer;  
	_videoCodecCtx->release_buffer = our_release_buffer; 

	//初始化屏幕尺寸调整
	sws_freeContext(_imgConvertCtx);
	_imgConvertCtx = sws_getContext(
		_videoCodecCtx->width,
		_videoCodecCtx->height,
		_videoCodecCtx->pix_fmt,
		_width,
		_height,
		PIX_FMT_RGB24,
		SWS_FAST_BILINEAR, nullptr, nullptr, nullptr);

	if (!_imgConvertCtx)
	{
		log("wrong _imgConvertCtx");
		return false;
	}

	_pictq_mutex = SDL_CreateMutex();
	_pictq_cond = SDL_CreateCond();
	
	return true;
}

bool MediaPlayer2::initAudioContext()
{
	// Get a pointer to the codec context for the video stream
	AVCodecContext* codecCtx = _formatCtx->streams[_nIndex_AudioStream]->codec;

	// Find the decoder for the video stream
	AVCodec* pCodec = avcodec_find_decoder(codecCtx->codec_id);

	if (!pCodec || avcodec_open2(codecCtx, pCodec, NULL) != 0)
	{
		log("avcodec_find_decoder or avcodec_open2 fail");
		return false;
	}

	SDL_SetMainReady();
	if (SDL_Init(SDL_INIT_AUDIO | SDL_INIT_TIMER) != 0) 
	{
		log("Could not initialize SDL - %s\n", SDL_GetError());
		return false;
	}

	SDL_AudioSpec wanted_spec, spec;

	// Set audio settings from codec info  
	wanted_spec.freq = codecCtx->sample_rate;  
	wanted_spec.format = AUDIO_S32SYS;  
	wanted_spec.channels = codecCtx->channels;  
	wanted_spec.silence = 0;  
	wanted_spec.samples = SDL_AUDIO_BUFFER_SIZE;  
	wanted_spec.callback = audio_callback;  
	wanted_spec.userdata = this;

	//初始化音频流
	_audioStream = _formatCtx->streams[_nIndex_AudioStream];

	_audio_buf_size = 0;
	_audio_buf_index = 0;

	memset(_audio_pkt, 0, sizeof(AVPacket));
	_audio_pkt_size = 0;

	packet_queue_init_S(_audioQueue);

	if (SDL_OpenAudio(&wanted_spec, &spec) < 0){
		log("can't open audio. %s\n", SDL_GetError());
		return false;
	}

	if (spec.channels != wanted_spec.channels) {
		log("not support channel");
		return false;
	}

	if (spec.format != AUDIO_S32SYS){
		log("SDL advised audio format %d is not supported\n", spec.format);
		return false;
	}

	return true;
}

int MediaPlayer2::decode_thread_S( void *arg )
{
	MediaPlayer2* player = (MediaPlayer2*)arg;
	AVPacket packet;

	while (!player->_requestStop) 
	{
		MYLOG("decode");
		if(player->_videoQueue->size > MAX_VIDEOQ_SIZE || player->_audioQueue->size > MAX_AUDIOQ_SIZE)  
		{  
			SDL_Delay(10);
			continue;  
		}

		if(av_read_frame(player->_formatCtx, &packet) < 0)  
		{  
			if(player->_formatCtx->pb && player->_formatCtx->pb->error)  
			{  
				/* no error; wait for user input */
				SDL_Delay(100); 
				continue;  
			}  
			else  
			{  
				break;  
			}  
		}

		// Is this a packet from the video stream?  
		if(packet.stream_index == player->_nIndex_VideoStream)  
		{  
			packet_queue_put_S(player->_videoQueue, &packet);  
		}  
		else if(packet.stream_index == player->_nIndex_AudioStream)  
		{  
			packet_queue_put_S(player->_audioQueue, &packet);  
		}  
		else  
		{  
			av_free_packet(&packet);  
		}

		double readFrameTimeEnd = utils::gettime();
		double nextFrameTime = (player->_currentFrame + 1) * player->_frameTime;
		double readFrameTimeCost = readFrameTimeEnd - player->_readFrameTimeBegin;
		double sleepTime = nextFrameTime - readFrameTimeCost;
		SDL_Delay(sleepTime * 1000);
		player->_elapsed = utils::gettime() - player->_readFrameTimeBegin;

		MYLOG("decode end");
	}

	Director::getInstance()->getScheduler()->performFunctionInCocosThread([=] 
	{
		if (player->_videoEndCallback)player->_videoEndCallback();
	});

	return 0;
}

int MediaPlayer2::video_thread_S( void *arg )
{
	MediaPlayer2* player = (MediaPlayer2*)arg;
	AVPacket packet;
	int frameFinished;
	double pts;

	//Allocate video frame
	AVFrame *pFrame = av_frame_alloc();

	while (true)
	{
		MYLOG("video");
		if(packet_queue_get_S(player->_videoQueue, &packet, 1) < 0)  
		{  			
			break; // means we quit getting packets  
		}

		pts = 0;

		global_video_pkt_pts = packet.pts;

		avcodec_decode_video2(player->_videoCodecCtx, pFrame, &frameFinished, &packet);

		if(packet.dts == AV_NOPTS_VALUE && pFrame->opaque && *(uint64_t*)pFrame->opaque != AV_NOPTS_VALUE)  
		{  
			pts = *(uint64_t*)pFrame->opaque;  
		}  
		else if(packet.dts != AV_NOPTS_VALUE)  
		{  
			pts = packet.dts;  
		}  
		else  
		{  
			pts = 0;  
		}

		pts *= av_q2d(player->_videoStream->time_base);  

		// Did we get a video frame?  
		if(frameFinished)  
		{  
			pts = synchronize_video_S(player, pFrame, pts);  
			if(queue_picture_S(player, pFrame, pts) < 0)  
			{  
				break;  
			}  
		}

		av_free_packet(&packet);

		MYLOG("video end");
	}

	av_free(pFrame);

	return 0;
}

double MediaPlayer2::synchronize_video_S( MediaPlayer2* player, AVFrame* src_frame, double pts )
{
	if(pts != 0)  
	{  		
		player->_videoClock = pts; /* if we have pts, set video clock to it */  
	}  
	else  
	{  		  
		pts = player->_videoClock; /* if we aren't given a pts, set it to the clock */
	}  

	/* update the video clock */  
	double frame_delay = av_q2d(player->_videoStream->codec->time_base);

	/* if we are repeating a frame, adjust clock accordingly */  
	frame_delay += src_frame->repeat_pict * (frame_delay * 0.5);  
	player->_videoClock += frame_delay;  
	return pts; 
}

int MediaPlayer2::queue_picture_S( MediaPlayer2* player, AVFrame* pFrame, double pts )
{
	VideoPicture* vp;   
  
    /* wait until we have space for a new pic */  
    SDL_LockMutex(player->_pictq_mutex);  
    while(player->_pictq_size >= VIDEO_PICTURE_QUEUE_SIZE && !player->_requestStop)  
    {  
        SDL_CondWait(player->_pictq_cond, player->_pictq_mutex);  
    }  
    SDL_UnlockMutex(player->_pictq_mutex);  
  
    if(player->_requestStop) return -1;  
  
    // windex is set to 0 initially
    vp = player->_pictq + player->_pictq_windex;  
  
    /* allocate or resize the buffer! */  
    if(!vp || !vp->picture)  
    {  
        SDL_Event event;
  
        vp->allocated = 0;  
        /* we have to do it in the main thread */  
        event.type = FF_ALLOC_EVENT;  
        event.user.data1 = player;  
        SDL_PushEvent(&event);  
  
        /* wait until we have a picture allocated */  
        SDL_LockMutex(player->_pictq_mutex);  
        while(!vp->allocated && !player->_requestStop)  
        {  
            SDL_CondWait(player->_pictq_cond, player->_pictq_mutex);  
        }  
        SDL_UnlockMutex(player->_pictq_mutex);

        if (player->_requestStop) return -1;  
    } 
  
    if(vp->picture)  
    {  
        sws_scale(player->_imgConvertCtx, 
			pFrame->data, pFrame->linesize, 
			0, player->_videoCodecCtx->height, 
			vp->picture->data, vp->picture->linesize); 
        vp->pts = pts;  
  
        /* now we inform our display thread that we have a pic ready */
		
        if(player->_pictq_windex == VIDEO_PICTURE_QUEUE_SIZE)  
        {  
            player->_pictq_windex = 0;  
        }
		else
		{
			++player->_pictq_windex;
		}

        SDL_LockMutex(player->_pictq_mutex);  
        player->_pictq_size++;  
        SDL_UnlockMutex(player->_pictq_mutex);  
    }

    return 0;  
}

void MediaPlayer2::audio_callback( void* userdata, Uint8* stream, int len )
{
	MYLOG("audio");
	MediaPlayer2* player = (MediaPlayer2*)userdata;  
	int len1, audio_size;   

	while(len > 0)  
	{  
		if(player->_audio_buf_index >= player->_audio_buf_size)  
		{  
			/* We have already sent all our data; get more */  
			audio_size = audio_decode_frame_S(player, player->_audioBuf, sizeof(player->_audioBuf));

			if(audio_size < 0) /* If error, output silence */  
			{  
				player->_audio_buf_size = 1024;  
				memset(player->_audioBuf, 0, player->_audio_buf_size);  
			}  
			else  
			{  
				player->_audio_buf_size = audio_size;  
			}

			player->_audio_buf_index = 0;  
		}

		len1 = player->_audio_buf_size - player->_audio_buf_index; //剩余空间

		if(len1 > len) len1 = len; //所需空间 
			  
		memcpy(stream, (uint8_t*)player->_audioBuf + player->_audio_buf_index, len1); //把取得的音频帧缓存复制到stream里

		len -= len1; //若len大于len1表示len中还有空间，则继续复制
		stream += len1;  
		player->_audio_buf_index += len1;  
	}
	MYLOG("audio end");
}

int MediaPlayer2::audio_decode_frame_S( MediaPlayer2* player, uint8_t* audio_buf, int buf_size )
{
	int len1, data_size, n;  
	AVPacket* pkt = player->_audio_pkt;   

	while (true)
	{  
		while(player->_audio_pkt_size > 0)  
		{  
			data_size = buf_size;

			len1 = our_avcodec_decode_audio_S(
				player->_audioStream->codec, 
				(int16_t*)audio_buf, 
				&data_size, 
				pkt);

			if(len1 < 0)  
			{  				
				player->_audio_pkt_size = 0; /* if error, skip frame */  
				break;  
			}

			player->_audio_pkt_data += len1;  
			player->_audio_pkt_size -= len1;

			if(data_size <= 0) continue; /* No data yet, get more frames */  

			n = 2 * player->_audioStream->codec->channels;  
			player->_audioClock += (double)data_size / (double)(n * player->_audioStream->codec->sample_rate);  

			return data_size; /* We have data, return it and come back for more later */  
		}

		if(pkt->data) av_free_packet(pkt);
			  
		if(player->_requestStop) return -1;
		  
		/* next packet */  
		if(packet_queue_get_S(player->_audioQueue, pkt, 1) < 0) return -1;

		player->_audio_pkt_data = pkt->data;  
		player->_audio_pkt_size = pkt->size;

		/* if update, update the audio clock w/pts */  
		if(pkt->pts != AV_NOPTS_VALUE)  
		{  
			player->_audioClock = av_q2d(player->_audioStream->time_base) * pkt->pts;  
		}  
	}
}

int MediaPlayer2::our_avcodec_decode_audio_S(AVCodecContext* avctx, int16_t* samples, int* frame_size_ptr, AVPacket* avpkt)
{
	static AVFrame sFrame; // = av_frame_alloc();
	int ret, got_frame = 0;

	AVFrame* frame = &sFrame;

	if ((ret = avcodec_decode_audio4(avctx, frame, &got_frame, avpkt)) > 0) 
	{
		if (got_frame) 
		{
			int plane_size;

			int data_size = av_samples_get_buffer_size(&plane_size, avctx->channels, frame->nb_samples, avctx->sample_fmt, 1);
			if (*frame_size_ptr < data_size) 
			{
				av_log(avctx, AV_LOG_ERROR, "output buffer size is too small for "
					"the current frame (%d < %d)\n", *frame_size_ptr, data_size);
				av_frame_free(&frame);
				return AVERROR(EINVAL);
			}

			SwrContext *au_convert_ctx = swr_alloc();
			au_convert_ctx = swr_alloc_set_opts(au_convert_ctx, 
				avctx->channel_layout, AV_SAMPLE_FMT_S16, avctx->sample_rate*2,
				avctx->channel_layout, avctx->sample_fmt, avctx->sample_rate,
				0, NULL);
			swr_init(au_convert_ctx);
			swr_convert(au_convert_ctx, (uint8_t **)&samples, plane_size, (const uint8_t **)frame->extended_data, plane_size);

			swr_free(&au_convert_ctx);

			*frame_size_ptr = data_size;
		}
		else 
		{
			*frame_size_ptr = 0;
		}
	}

	return ret;
}

double MediaPlayer2::getAudioClock()
{
	double pts = _audioClock; /* maintained in the audio thread */  
	int hw_buf_size = _audio_buf_size - _audio_buf_index;    
	int n = _audioStream->codec->channels * 2;

	int bytes_per_sec = _audioStream->codec->sample_rate * n;  
	if(bytes_per_sec)  
	{  
		pts -= (double)hw_buf_size / bytes_per_sec;  
	}  
	return pts;  
}

int MediaPlayer2::event_thread_S( void* arg )
{
	MediaPlayer2* player = (MediaPlayer2*)arg;
	SDL_Event event;

	for(;;)  
	{  
		SDL_WaitEvent(&event);  
		switch(event.type)  
		{  
		case FF_QUIT_EVENT:
		case SDL_QUIT:  
			player->_requestStop = 1;  
			SDL_Quit();
			break;  
		case FF_ALLOC_EVENT:  
			alloc_picture_S(event.user.data1);  
			break;  
		case FF_REFRESH_EVENT:  
			video_refresh_timer_S(event.user.data1);  
			break;  
		default:  
			break;  
		}  
	}
}

void MediaPlayer2::schedule_refresh_S( MediaPlayer2* player, int delay )
{
	SDL_AddTimer(delay, sdl_refresh_timer_cb, player);
}

Uint32 MediaPlayer2::sdl_refresh_timer_cb( Uint32 interval, void* opaque )
{
	SDL_Event event;  
	event.type = FF_REFRESH_EVENT;  
	event.user.data1 = opaque;  
	SDL_PushEvent(&event);  
	return 0; /* 0 means stop timer */
}

void MediaPlayer2::alloc_picture_S( void *userdata )
{
	MediaPlayer2* player = (MediaPlayer2*)userdata;  
	VideoPicture* vp;  

	vp = player->_pictq + player->_pictq_windex;  
	if(vp->picture) // we already have one make another, bigger/smaller  
	{  		 
		avpicture_free(vp->picture);
		delete vp->picture; 
	} 

	vp->picture = new AVPicture;
	avpicture_alloc(vp->picture, PIX_FMT_RGB24, player->_width, player->_height); 

	SDL_LockMutex(player->_pictq_mutex);  
	vp->allocated = 1;  
	SDL_CondSignal(player->_pictq_cond);  
	SDL_UnlockMutex(player->_pictq_mutex); 
}

void MediaPlayer2::video_refresh_timer_S( void *userdata )
{
	MYLOG("refresh");
	MediaPlayer2* player = (MediaPlayer2*)userdata;   
  
    if(!player->_videoStream)  
    {
		schedule_refresh_S(player, 100);
		return;
	}

    if(player->_pictq_size == 0)  
    {  
		MYLOG("no pictq");
        schedule_refresh_S(player, 10);
		return;
    }  
    
    VideoPicture* vp = player->_pictq + player->_pictq_rindex;  
  
    double delay = vp->pts - player->_frame_last_pts; /* the pts from last time */

	/* if incorrect delay, use previous one */
    if(delay <= 0 || delay >= 1.0) delay = player->_frame_last_pts; 

    /* save for next time */
    player->_frame_last_delay = delay;  
    player->_frame_last_pts = vp->pts;  
  
    /* update delay to sync to audio */  
    double ref_clock = player->getAudioClock();  
    double diff = vp->pts - ref_clock;  
  
    /* Skip or repeat the frame. Take delay into account 
    FFPlay still doesn't "know if this is the best guess." */  
    double sync_threshold = (delay > AV_SYNC_THRESHOLD) ? delay : AV_SYNC_THRESHOLD;

    if(fabs(diff) < AV_NOSYNC_THRESHOLD)  
    {  
        if(diff <= -sync_threshold)  
        {  
            delay = 0;  
        }  
        else if(diff >= sync_threshold)  
        {  
            delay = 2 * delay;  
        }  
    }

    player->_frame_timer += delay;

    /* computer the REAL delay */  
    double actual_delay = player->_frame_timer - utils::gettime();

    if(actual_delay < 0.010) actual_delay = 0.010;/* Really it should skip the picture instead */    

    schedule_refresh_S(player, (int)(actual_delay * 1000 + 0.5));

    /* show the picture! */  
    video_display_S(player);  
  
    /* update queue for next picture! */  
    if(++player->_pictq_rindex == VIDEO_PICTURE_QUEUE_SIZE)  
    {  
        player->_pictq_rindex = 0;  
    }
	else
	{
		++player->_pictq_rindex;
	}

    SDL_LockMutex(player->_pictq_mutex);  
    player->_pictq_size--;  
    SDL_CondSignal(player->_pictq_cond);  
    SDL_UnlockMutex(player->_pictq_mutex);
	MYLOG("refresh end");
}

void MediaPlayer2::video_display_S( MediaPlayer2* player )
{
	Director::getInstance()->getScheduler()->performFunctionInCocosThread([=] 
	{
		GL::bindTexture2D(player->_texture->getName());
		glTexImage2D(GL_TEXTURE_2D, 0, GL_RGB, 
			player->_width, player->_height, 
			0, GL_RGB, GL_UNSIGNED_BYTE,
			(player->_pictq + player->_pictq_rindex)->picture->data[0]);
	});
	
}

void MediaPlayer2::packet_queue_init_S(PacketQueue* q)
{
	memset(q, 0, sizeof(PacketQueue));
	q->mutex = SDL_CreateMutex();
	q->cond = SDL_CreateCond();
}

int MediaPlayer2::packet_queue_put_S(PacketQueue *q, AVPacket *pkt)
{
	AVPacketList *pkt1;
	
	pkt1 = (AVPacketList *)av_malloc(sizeof(AVPacketList));
	if (!pkt1) return -1;
		
	//
	av_copy_packet(&pkt1->pkt, pkt);
	pkt1->next = NULL;

	SDL_LockMutex(q->mutex);
	if (!q->last_pkt)
		q->first_pkt = pkt1;
	else
		q->last_pkt->next = pkt1;
	q->last_pkt = pkt1;
	++q->nb_packets;
	q->size += pkt1->pkt.size;
	SDL_CondSignal(q->cond);
	SDL_UnlockMutex(q->mutex);
	return 0;
}

int MediaPlayer2::packet_queue_get_S(PacketQueue *q, AVPacket *pkt, int block)
{
	AVPacketList *pkt1;
	int ret;
	SDL_LockMutex(q->mutex);
	while (true)
	{
		if (_MediaPlayer2->_requestStop/*全局变量*/)
		{
			ret = -1;
			break;
		}
		pkt1 = q->first_pkt;
		if (pkt1)
		{
			q->first_pkt = pkt1->next;
			if (!q->first_pkt)
				q->last_pkt = NULL;
			--q->nb_packets;
			q->size -= pkt1->pkt.size;

			av_copy_packet(pkt, &pkt1->pkt);

			av_free(pkt1);

			ret = 1;
			break;
		}
		else if (!block)
		{
			ret = 0;
			break;
		}
		else
		{
			SDL_CondWait(q->cond, q->mutex);
		}
	}
	SDL_UnlockMutex(q->mutex);
	return ret;
}

int MediaPlayer2::decode_interrupt_cb(void* arg)  
{  
	return (_MediaPlayer2 && _MediaPlayer2->_requestStop);  
} 






