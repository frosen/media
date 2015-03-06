//
//  VideoPlayer.cpp
//  wanaka-framework
//
//  Created by XiaoYeZi on 15-1-8.
//  Copyright (c) 2015年 wanaka. All rights reserved.
//
#include <thread>
#include "MediaPlayer.h"
#include "UtilsWanakaFramework.h"

#define kEnableFFMPEG 1
#define VIDEO_DEBUG 1
#define ENABLE_AUDIO 1
#define SDL_AUDIO_BUFFER_SIZE 8192

#if kEnableFFMPEG
extern "C" {
//FFMPEG
#include "libavformat/avformat.h"
#include "libswscale/swscale.h"
#include "libswresample/swresample.h"

#if ENABLE_AUDIO == 1
//SDL
#include "SDL.h"
#include "SDL_thread.h"
#endif
}
#endif

static AVFormatContext *s_formatCtx;

#if ENABLE_AUDIO == 1
typedef struct PacketQueue
{
    AVPacketList *first_pkt, *last_pkt;
    int nb_packets;
    int size;
    SDL_mutex *mutex;
    SDL_cond *cond;
} PacketQueue;
PacketQueue audioq;
int quit = 0;

static void packet_queue_init(PacketQueue *q)
{
    memset(q, 0, sizeof(PacketQueue));
    q->mutex = SDL_CreateMutex();
    q->cond = SDL_CreateCond();
}

static int packet_queue_put(PacketQueue *q, AVPacket *pkt)
{
    AVPacketList *pkt1;
//    if (av_dup_packet(pkt) < 0)
  //  {
    //    return -1;
    //}
    pkt1 = (AVPacketList *)av_malloc(sizeof(AVPacketList));
    if (!pkt1)
        return -1;
    //
    av_copy_packet(&pkt1->pkt, pkt);
    //pkt1->pkt = *pkt;
    pkt1->next = NULL;
    SDL_LockMutex(q->mutex);
    if (!q->last_pkt)
        q->first_pkt = pkt1;
    else
        q->last_pkt->next = pkt1;
    q->last_pkt = pkt1;
    q->nb_packets++;
    q->size += pkt1->pkt.size;
    SDL_CondSignal(q->cond);
    SDL_UnlockMutex(q->mutex);
    return 0;
}

static int packet_queue_get(PacketQueue *q, AVPacket *pkt, int block)
{
    AVPacketList *pkt1;
    int ret;
    SDL_LockMutex(q->mutex);
    for (;;)
    {
        if (quit)
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
            q->nb_packets--;
            q->size -= pkt1->pkt.size;
            av_copy_packet(pkt, &pkt1->pkt);
//            *pkt = pkt1->pkt;
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

static int avcodec_decode_audio(AVCodecContext *avctx, int16_t *samples, int *frame_size_ptr, AVPacket *avpkt)
{
    static AVFrame sFrame;// = av_frame_alloc();
    int ret, got_frame = 0;

    AVFrame *frame = &sFrame;
//	if (!frame)
    //	return AVERROR(ENOMEM);

    if ((ret= avcodec_decode_audio4(avctx, frame, &got_frame, avpkt)) > 0) {
        if (got_frame) {
            int ch, plane_size;
            int planar = av_sample_fmt_is_planar(avctx->sample_fmt);
            int data_size = av_samples_get_buffer_size(&plane_size, avctx->channels, frame->nb_samples, avctx->sample_fmt, 1);
            if (*frame_size_ptr < data_size) {
                av_log(avctx, AV_LOG_ERROR, "output buffer size is too small for "
                    "the current frame (%d < %d)\n", *frame_size_ptr, data_size);
                av_frame_free(&frame);
                return AVERROR(EINVAL);
            }

            SwrContext *au_convert_ctx = swr_alloc();
            au_convert_ctx = swr_alloc_set_opts(au_convert_ctx, avctx->channel_layout, AV_SAMPLE_FMT_S16, avctx->sample_rate*2,
                avctx->channel_layout, avctx->sample_fmt, avctx->sample_rate, 0, NULL);
            swr_init(au_convert_ctx);
            swr_convert(au_convert_ctx, (uint8_t **)&samples, plane_size, (const uint8_t **)frame->extended_data, plane_size);

            swr_free(&au_convert_ctx);

            *frame_size_ptr = data_size;
        }
        else {
            *frame_size_ptr = 0;
        }
    }
    return ret;
}

int audio_decode_frame(AVCodecContext *aCodecCtx, uint8_t *audio_buf, int buf_size) {
    static AVPacket pkt;
    static uint8_t *audio_pkt_data = NULL;
    static int audio_pkt_size = 0;
    int len1, data_size;
    for (;;)
    {
        while (audio_pkt_size > 0)
        {
            data_size = buf_size;
            len1 = avcodec_decode_audio(aCodecCtx, (int16_t *)audio_buf, &data_size, &pkt);
            if (len1 < 0)
            { /* if error, skip frame */
                audio_pkt_size = 0;
                break;
            }
            audio_pkt_data += len1;
            audio_pkt_size -= len1;
            if (data_size <= 0)
            { /* No data yet, get more frames */
                continue;
            } /* We have data, return it and come back for more later */
            return data_size;
        }
        if (pkt.data)
            av_free_packet(&pkt);
        if (quit)
        {
            return -1;
        }

//		while (av_read_frame(s_formatCtx, &pkt) >= 0) {
    //		if (pkt.stream_index == 1) {
        //		break;
            //}
        //}
        if (packet_queue_get(&audioq, &pkt, 1) < 0)
        {
            return -1;
        }
        audio_pkt_data = pkt.data;
        audio_pkt_size = pkt.size;
    }
}

void audio_callback(void *userdata, Uint8 *stream, int len)
{
    AVCodecContext *aCodecCtx = (AVCodecContext *)userdata;
    int len1, audio_size;
    static uint8_t audio_buf[1024 * 1024];
    static unsigned int audio_buf_size = 0;
    static unsigned int audio_buf_index = 0;
    while (len > 0)
    {
        if (audio_buf_index >= audio_buf_size)
        { /* We have already sent all our data; get more */
            audio_size = audio_decode_frame(aCodecCtx, audio_buf, sizeof(audio_buf));
            if (audio_size < 0)
            { /* If error, output silence */
                audio_buf_size = 1024; // arbitrary?
                memset(audio_buf, 0, audio_buf_size);
            }
            else
            {
                audio_buf_size = audio_size;
            }
            audio_buf_index = 0;
        }
        len1 = audio_buf_size - audio_buf_index;
        if (len1 > len)
            len1 = len;
        //SDL_MixAudio(stream, (uint8_t *)audio_buf + audio_buf_index, len1, SDL_MIX_MAXVOLUME);
        memcpy(stream, (uint8_t *)audio_buf + audio_buf_index, len1);
        len -= len1;
        stream += len1;
        audio_buf_index += len1;
    }
}
#endif

MediaPlayer* MediaPlayer::create(const char* path, int width, int height) {
    MediaPlayer* video = new MediaPlayer();
    if (video && video->init(path, width, height)) {
        video->autorelease();
    } else {
        delete video;
        video = nullptr;
    }
    return video;
}

MediaPlayer::MediaPlayer() {
#if kEnableFFMPEG
    _formatCtx = nullptr;
    _codecCtx = nullptr;
    _frame = nullptr;
    _picture = nullptr;
    _imgConvertCtx = nullptr;
#endif

    _frameRate = 1 / 24.0;
    _elapsed = 0;
    _enableTouchEnd = false;
    _requestStop = false;
    _currentFrame = 0;
    _startFrame = 0;
}

MediaPlayer::~MediaPlayer() {
#if kEnableFFMPEG
    if (_imgConvertCtx != nullptr) {
        sws_freeContext(_imgConvertCtx);
    }

    // Free RGB picture
    if (_picture != nullptr) {
        avpicture_free(_picture);
         delete _picture;
    }

    // Free the YUV frame
    if (_frame != nullptr) {
        av_free(_frame);
    }

    // Close the codec
    if (_codecCtx) avcodec_close(_codecCtx);

    if (_formatCtx) {
        avformat_close_input(&_formatCtx);
    }
#endif
}

bool MediaPlayer::init(const char* path, int width, int height) {
#if kEnableFFMPEG
    _width = width;
    _height = height;
    _elapsed = 0;

	

    // Register all formats and codecs
    av_register_all();

    _filePath = FileUtils::getInstance()->fullPathForFilename(path);
    if(avformat_open_input(&_formatCtx, _filePath.c_str(), NULL, NULL) != 0) {
        log("avformat_open_input failed. %s", _filePath.c_str());
        return false;
    }

    s_formatCtx = _formatCtx;
    // 获取流信息
    if(avformat_find_stream_info(_formatCtx, NULL) < 0) {
        log("avformat_find_stream_info failed.");
        return false;
    }

    _totalTime = _formatCtx->duration / 1000000;  //milli second;

#if VIDEO_DEBUG
    av_dump_format(_formatCtx, 0, path, 0);
#endif

    // 查找视频流和音频流
    _videoStream = -1;
    _audioStream = -1;

    log("stream count=%d", _formatCtx->nb_streams);
    for(int i = 0; i < _formatCtx->nb_streams; i++) {
        //if(pFormatCtx->streams[i]->codec->codec_type==CODEC_TYPE_VIDEO)
        if(_videoStream == -1 && _formatCtx->streams[i]->codec->codec_type == AVMEDIA_TYPE_VIDEO) {
            _videoStream = i;
        }

        if (_audioStream == -1 && _formatCtx->streams[i]->codec->codec_type == AVMEDIA_TYPE_AUDIO) {
            _audioStream = i;
        }

        if (_videoStream != -1 && _audioStream != -1) {
            break;
        }
    }

    // Allocate video frame
    if (initVideoContext()) {
        _frame = av_frame_alloc();
    }
    else {
        return false;
    }

    //audio is optional.
    if (!initAudioContext()) {
        log("initAudioContext failed");
    }
#endif

    return true;
}

bool MediaPlayer::initVideoContext() {
    AVCodec         *pCodec = nullptr;         //for video
    //  没有视频流，无法播放
    if (_videoStream == -1) {
        log("videoStream == -1");
        return false;
    }

//    _videoStream = 0;

    // Get a pointer to the codec context for the video stream
    _codecCtx = _formatCtx->streams[_videoStream]->codec;
    // 获取视频帧率
    _frameRate = av_q2d(_formatCtx->streams[_videoStream]->r_frame_rate);

    _frameTime = 1.0 / _frameRate;

    // scale
    sws_freeContext(_imgConvertCtx);

    // Find the decoder for the video stream
    pCodec = avcodec_find_decoder(_codecCtx->codec_id);
    if (pCodec == nullptr) {
        return false;
    }
    if (avcodec_open2(_codecCtx, pCodec, NULL)) {
        return false;
    }
    // 用于渲染的一帧图片数据。注意其中的data是一个指针数组，我们取视频流用于渲染(一般是第0个流)
    _picture = new AVPicture;
    avpicture_alloc(_picture, PIX_FMT_RGB24, _width, _height);

    // 用于缩放视频到实际需求大小
    static int sws_flags = SWS_FAST_BILINEAR;
    _imgConvertCtx = sws_getContext(_codecCtx->width,
        _codecCtx->height,
        _codecCtx->pix_fmt,
        _width,
        _height,
        PIX_FMT_RGB24,
        sws_flags, nullptr, nullptr, nullptr);

    // 渲染的纹理
    Texture2D *texture = new Texture2D();
    texture->initWithData(_picture->data[_videoStream], _picture->linesize[0]*_height, kCCTexture2DPixelFormat_RGB888, _width, _height, Size(_width, _height));
    initWithTexture(texture);

    setContentSize(Size(_width, _height));

    return true;
}

bool MediaPlayer::initAudioContext() {
#if	ENABLE_AUDIO == 1
    AVCodec         *pCodec = nullptr;    //for audio
    // Get a pointer to the codec context for the audio stream
    _audioCodecCtx = nullptr;
    if (_audioStream != -1) {
        _audioCodecCtx = _formatCtx->streams[_audioStream]->codec;
    } else {
        return false;
    }

    // Find the decoder for the audio stream
    if (_audioCodecCtx != nullptr) {
        pCodec = avcodec_find_decoder(_audioCodecCtx->codec_id);
        if (pCodec != nullptr) {
            if (avcodec_open2(_audioCodecCtx, pCodec, NULL) < 0) {
                return false;
            }

            SDL_SetMainReady();
            if (SDL_Init(SDL_INIT_AUDIO | SDL_INIT_TIMER) != 0) {
                log("Could not initialize SDL - %s\n", SDL_GetError());
                return false;
            }

            //SDL_AudioSpec
            SDL_AudioSpec wanted_spec, spec;
            wanted_spec.freq = _audioCodecCtx->sample_rate;
			wanted_spec.format = AUDIO_S32SYS;
            wanted_spec.channels =  _audioCodecCtx->channels;
            wanted_spec.silence = 0;
            wanted_spec.samples = 4096;
            wanted_spec.callback = audio_callback;
            wanted_spec.userdata = _audioCodecCtx;

            packet_queue_init(&audioq);

            if (SDL_OpenAudio(&wanted_spec, &spec) < 0){
                printf("can't open audio.\n");
                return false;
            }

            if (spec.channels != wanted_spec.channels) {
                printf("not support channel");
                return false;
            }

			if (spec.format != AUDIO_S32SYS){
                fprintf(stderr, "SDL advised audio format %d is not supported\n", spec.format);
                return false;
            }
        }
    } else {
        return false;
    }
#endif
    return true;
}

double MediaPlayer::getCurrentTime() {
    return _elapsed;
}

double MediaPlayer::getTotalTime() {
    return _totalTime;
}

void MediaPlayer::play(bool fromBegin /*= false*/) {
    //_elapsed = 0;
    _requestStop = false;
    //seek(20);
    if (fromBegin) {
        _elapsed = 0;
    }
    seek((double)_elapsed);

    std::thread newThread(&MediaPlayer::playInBackground, this);
    newThread.detach();
#if ENABLE_AUDIO == 1
    SDL_PauseAudio(0);
#endif
}

void MediaPlayer::playInBackground() {
    while (!_requestStop) {
        if (readVideoFrames()) {
            double readFrameTimeEnd = UtilsWanakaFramework::getUnixTimestamp();
            double nextFrameTime = (_currentFrame + 1) * _frameTime;
            double readFrameTimeCost = readFrameTimeEnd - _readFrameTimeBegin;
            double sleepTime = nextFrameTime - readFrameTimeCost;
            std::this_thread::sleep_for(std::chrono::milliseconds((int)(sleepTime*1000)));
            _elapsed = (UtilsWanakaFramework::getUnixTimestamp() - _readFrameTimeBegin);
        } else {
            break;
        }
    }

    Director::getInstance()->getScheduler()->performFunctionInCocosThread([this] {
        if (_videoEndCallback) {
            _videoEndCallback();
        }
    });
}

void MediaPlayer::stop(void) {
    _requestStop = true;
}

void MediaPlayer::pause(void) {
    _requestStop = true;
}

void MediaPlayer::seek(double sec) {
#if kEnableFFMPEG
    _startFrame = sec / _frameTime;//(int64_t)((double)timeBase.den / timeBase.num * sec);
    _currentFrame = _startFrame;
    avformat_seek_file(_formatCtx, _videoStream, _startFrame, _startFrame, _startFrame, AVSEEK_FLAG_FRAME);
    avcodec_flush_buffers(_codecCtx);
    _readFrameTimeBegin = UtilsWanakaFramework::getUnixTimestamp() - sec;
    _elapsed=sec;
#endif
}

bool MediaPlayer::readVideoFrames() {
    int frameFinished = 0;
#if kEnableFFMPEG
    AVPacket packet;
    while(!frameFinished && av_read_frame(_formatCtx, &packet) >= 0) {
        if(packet.stream_index == _videoStream) {
            avcodec_decode_video2(_codecCtx, _frame, &frameFinished, &packet);
        }
        else if (packet.stream_index == _audioStream) {
#if ENABLE_AUDIO == 1
            packet_queue_put(&audioq, &packet);
#endif
        }
        av_free_packet(&packet);
    }
    _currentFrame = (int)av_frame_get_best_effort_timestamp(_frame);
    _startFrame = _currentFrame;
    sws_scale(_imgConvertCtx, _frame->data, _frame->linesize, 0, _codecCtx->height, _picture->data, _picture->linesize);
#endif
    return frameFinished == 1;
}


void MediaPlayer::draw(Renderer *renderer, const Mat4& transform, uint32_t flags) {
#if kEnableFFMPEG
    auto glProgram = getGLProgram();
    glProgram->use();
    glProgram->setUniformsForBuiltins(transform);
    GL::blendFunc(_blendFunc.src, _blendFunc.dst);

    if (_texture != NULL) {
        GL::bindTexture2D(_texture->getName());
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGB, _width, _height, 0, GL_RGB, GL_UNSIGNED_BYTE,_picture->data[0]);
    }
    else {
        GL::bindTexture2D(0);
    }

    GL::enableVertexAttribs( GL::VERTEX_ATTRIB_FLAG_POS_COLOR_TEX );

#define kQuadSize sizeof(_quad.bl)
    long offset = (long)&_quad;

    // vertex
    int diff = offsetof(V3F_C4B_T2F, vertices);
    glVertexAttribPointer(GLProgram::VERTEX_ATTRIB_POSITION, 3, GL_FLOAT, GL_FALSE, kQuadSize, (void*) (offset + diff));

    // texCoods
    diff = offsetof(V3F_C4B_T2F, texCoords);
    glVertexAttribPointer(GLProgram::VERTEX_ATTRIB_TEX_COORD, 2, GL_FLOAT, GL_FALSE, kQuadSize, (void*)(offset + diff));

    // color
    diff = offsetof(V3F_C4B_T2F, colors);
    glVertexAttribPointer(GLProgram::VERTEX_ATTRIB_COLOR, 4, GL_UNSIGNED_BYTE, GL_TRUE, kQuadSize, (void*)(offset + diff));

    glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);

    CHECK_GL_ERROR_DEBUG();
    CC_INCREMENT_GL_DRAWS(1);

#endif
}

void MediaPlayer::setVideoEndCallback(std::function<void(void)> func) {
    _videoEndCallback = func;
}

void MediaPlayer::setEnableTouchEnd(bool enable) {
    _enableTouchEnd = enable;
}
