#pragma once
#include <stdio.h>
#include <functional>
#include "cocos2d.h"
#include "typedef.h"

#ifndef __STDC_CONSTANT_MACROS
#define __STDC_CONSTANT_MACROS
#endif

#ifndef INT64_C
#define INT64_C(c) (c ## LL)
#define UINT64_C(c) (c ## ULL)
#endif

struct AVFormatContext;
struct AVCodecContext;
struct AVFrame;
struct AVPicture;
struct SwsContext;

USING_NS_CC;

class MediaPlayer : public Sprite
{
public:
    static MediaPlayer* create(const char* path, int width, int height);
    MediaPlayer();
    virtual ~MediaPlayer();

    bool init(const char* path, int width, int height);
    void play(bool fromBegin = false);
    void stop(void);
    void pause(void);
    void seek(double sec);
    double getCurrentTime();
    double getTotalTime();
    void draw(Renderer *renderer, const Mat4& transform, uint32_t flags);
    void playInBackground();
    bool readVideoFrames();

    void setEnableTouchEnd(bool enable);                        // 是否可以点击关闭视频
    void setVideoEndCallback(std::function<void()> func);       // 关闭视频回调

protected:
    bool initVideoContext();
    bool initAudioContext();

private:
    int _width;
    int _height;
    bool _requestStop;

    AVFormatContext *_formatCtx;
    AVCodecContext *_codecCtx;       //video
    AVCodecContext *_audioCodecCtx;  //audio
    AVFrame *_frame;
    AVPicture* _picture;
    int _videoStream;                // 视频流
    int _audioStream;                // 音频流
    SwsContext *_imgConvertCtx;

    std::string _filePath;
    double _frameRate;              // 帧率
    double _frameTime;              // 视频每帧的时间
    double _elapsed;                // 用于帧率控制
    double _totalTime;              // 整个视频的时间
    int _currentFrame;
    int _startFrame;                //开始播放的帧数
    double _readFrameTimeBegin;     //开始播放的时间点

    bool _enableTouchEnd;
    std::function<void()> _videoEndCallback;
};

