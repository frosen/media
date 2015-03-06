// ffmpegExe.cpp: ����Ŀ�ļ���  
#include "libavformat/avformat.h"  
#include "libswscale/swscale.h"  
//#include <windows.h>  
 
#include <stdlib.h>  
#include <stdio.h>  
#include <string.h>  
#include <math.h>  
#include "SDL.h"
#include "SDL_thread.h"
 
#ifdef main  
#undef main  
#endif  
 
#define SDL_AUDIO_BUFFER_SIZE 1024  
 
#define MAX_AUDIOQ_SIZE (5 * 16 * 1024)  
#define MAX_VIDEOQ_SIZE (5 * 256 * 1024)  
 
#define FF_ALLOC_EVENT   (SDL_USEREVENT)  
#define FF_REFRESH_EVENT (SDL_USEREVENT + 1)  
#define FF_QUIT_EVENT (SDL_USEREVENT + 2)  
#define VIDEO_PICTURE_QUEUE_SIZE 1  
#define SDL_AUDIO_BUFFER_SIZE 1024  
  
static int sws_flags = SWS_BICUBIC;  
  
typedef struct PacketQueue   
{   
    AVPacketList *first_pkt, *last_pkt;   
    int nb_packets;   
    int size;   
    SDL_mutex *mutex;   
    SDL_cond *cond;   
} PacketQueue;   
  
typedef struct VideoPicture   
{  
    SDL_Overlay *bmp;  
    int width, height; /* source height & width */  
    int allocated;  
} VideoPicture;  
  
typedef struct VideoState   
{  
    AVFormatContext *pFormatCtx;  
    int             videoStream, audioStream;  
    AVStream        *audio_st;  
    PacketQueue     audioq;  
    uint8_t         audio_buf[(AVCODEC_MAX_AUDIO_FRAME_SIZE * 3) / 2];  
    unsigned int    audio_buf_size;  
    unsigned int    audio_buf_index;  
    AVPacket        audio_pkt;  
    uint8_t         *audio_pkt_data;  
    int             audio_pkt_size;  
    AVStream        *video_st;  
    PacketQueue     videoq;  
  
    VideoPicture    pictq[VIDEO_PICTURE_QUEUE_SIZE];  
    int             pictq_size, pictq_rindex, pictq_windex;  
    SDL_mutex       *pictq_mutex;  
    SDL_cond        *pictq_cond;  
  
    SDL_Thread      *parse_tid;  
    SDL_Thread      *video_tid;  
  
    char            filename[1024];  
    int             quit;  
} VideoState;  
  
SDL_Surface     *screen;  
  
/* Since we only have one decoding thread, the Big Struct 
can be global in case we need it. */  
VideoState *global_video_state;  
  
void packet_queue_init(PacketQueue *q)  
{   
    memset(q, 0, sizeof(PacketQueue));   
    q->mutex = SDL_CreateMutex();   
    q->cond = SDL_CreateCond();   
}   
int packet_queue_put(PacketQueue *q, AVPacket *pkt)   
{   
    AVPacketList *pkt1;   
    if(av_dup_packet(pkt) < 0)   
    {   
        return -1;   
    }   
    pkt1 = (AVPacketList *)av_malloc(sizeof(AVPacketList));   
    if (!pkt1)   
        return -1;   
    pkt1->pkt = *pkt;   
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
    for(;;)   
    {   
        if(global_video_state->quit)   
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
            *pkt = pkt1->pkt;   
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
  
int audio_decode_frame(VideoState *is, uint8_t *audio_buf, int buf_size)  
{  
    int len1, data_size;  
    AVPacket *pkt = &is->audio_pkt;  
  
    for(;;)   
    {  
        while(is->audio_pkt_size > 0)   
        {  
            data_size = buf_size;  
            len1 = avcodec_decode_audio2(is->audio_st->codec,   
                (int16_t *)audio_buf, &data_size,   
                is->audio_pkt_data, is->audio_pkt_size);  
            if(len1 < 0)   
            {  
                /* if error, skip frame */  
                is->audio_pkt_size = 0;  
                break;  
            }  
            is->audio_pkt_data += len1;  
            is->audio_pkt_size -= len1;  
            if(data_size <= 0)   
            {  
                /* No data yet, get more frames */  
                continue;  
            }  
            /* We have data, return it and come back for more later */  
            return data_size;  
        }  
        if(pkt->data)  
            av_free_packet(pkt);  
  
        if(is->quit)   
        {  
            return -1;  
        }  
        /* next packet */  
        if(packet_queue_get(&is->audioq, pkt, 1) < 0)   
        {  
            return -1;  
        }  
        is->audio_pkt_data = pkt->data;  
        is->audio_pkt_size = pkt->size;  
    }  
}  
  
void audio_callback(void *userdata, Uint8 *stream, int len)  
{  
    VideoState *is = (VideoState *)userdata;  
    int len1, audio_size;  
  
    while(len > 0)   
    {  
        if(is->audio_buf_index >= is->audio_buf_size)   
        {  
            /* We have already sent all our data; get more */  
            audio_size = audio_decode_frame(is, is->audio_buf, sizeof(is->audio_buf));  
            if(audio_size < 0)   
            {  
                /* If error, output silence */  
                is->audio_buf_size = 1024;  
                memset(is->audio_buf, 0, is->audio_buf_size);  
            }   
            else   
            {  
                is->audio_buf_size = audio_size;  
            }  
            is->audio_buf_index = 0;  
        }  
        len1 = is->audio_buf_size - is->audio_buf_index;  
        if(len1 > len)  
            len1 = len;  
        memcpy(stream, (uint8_t *)is->audio_buf + is->audio_buf_index, len1);  
        len -= len1;  
        stream += len1;  
        is->audio_buf_index += len1;  
    }  
}  
  
  
static Uint32 sdl_refresh_timer_cb(Uint32 interval, void *opaque)   
{  
    //printf("sdl_refresh_timer_cb called:interval--%d/n",interval);  
    SDL_Event event;  
    event.type = FF_REFRESH_EVENT;  
    event.user.data1 = opaque;  
    SDL_PushEvent(&event);      //�ɷ�FF_REFRESH_EVENT�¼�  
    return 0; /* 0 means stop timer */  
}  
  
/* schedule a video refresh in 'delay' ms */  
static void schedule_refresh(VideoState *is, int delay)   
{  
    //printf("schedule_refresh called:delay--%d/n",delay);  
    SDL_AddTimer(delay, sdl_refresh_timer_cb, is);      //sdl_refresh_timer_cb��������ʱdelay�����ֻ�ᱻִ��һ�Σ�is��sdl_refresh_timer_cb�Ĳ���  
}  
  
void video_display(VideoState *is)   
{  
    //printf("video_display called/n");  
    SDL_Rect rect;  
    VideoPicture *vp;  
    AVPicture pict;  
    float aspect_ratio;  
    int w, h, x, y;  
    int i;  
  
    vp = &is->pictq[is->pictq_rindex];  
    if(vp->bmp)   
    {  
        if(is->video_st->codec->sample_aspect_ratio.num == 0)   
        {  
            aspect_ratio = 0;  
        }   
        else   
        {  
            aspect_ratio = av_q2d(is->video_st->codec->sample_aspect_ratio) *  
                is->video_st->codec->width / is->video_st->codec->height;  
        }  
        if(aspect_ratio <= 0.0)      //aspect_ratio ���߱�  
        {  
            aspect_ratio = (float)is->video_st->codec->width /  
                (float)is->video_st->codec->height;  
        }  
        h = screen->h;  
        w = ((int)(h * aspect_ratio)) & -3;  
        if(w > screen->w)   
        {  
            w = screen->w;  
            h = ((int)(w / aspect_ratio)) & -3;  
        }  
        x = (screen->w - w) / 2;  
        y = (screen->h - h) / 2;  
  
        rect.x = x;  
        rect.y = y;  
        rect.w = w;  
        rect.h = h;  
        SDL_DisplayYUVOverlay(vp->bmp, &rect);  
    }  
}  
  
void video_refresh_timer(void *userdata)   
{  
    VideoState *is = (VideoState *)userdata;  
    VideoPicture *vp;  
  
    if(is->video_st)   
    {  
        if(is->pictq_size == 0)   
        {  
            schedule_refresh(is, 1);  
        }   
        else   
        {  
            vp = &is->pictq[is->pictq_rindex];  
            /* Now, normally here goes a ton of code 
            about timing, etc. we're just going to 
            guess at a delay for now. You can 
            increase and decrease this value and hard code 
            the timing - but I don't suggest that ;) 
            We'll learn how to do it for real later. 
            */  
            schedule_refresh(is, 80);  
  
            /* show the picture! */  
            video_display(is);  
  
            /* update queue for next picture! */  
            if(++is->pictq_rindex == VIDEO_PICTURE_QUEUE_SIZE)   
            {  
                is->pictq_rindex = 0;  
            }  
            SDL_LockMutex(is->pictq_mutex);  
            is->pictq_size--;  
            SDL_CondSignal(is->pictq_cond);  
            SDL_UnlockMutex(is->pictq_mutex);  
        }  
    }   
    else   
    {  
        schedule_refresh(is, 100);  
    }  
}  
  
void alloc_picture(void *userdata)   
{  
    VideoState *is = (VideoState *)userdata;  
    VideoPicture *vp;  
  
    vp = &is->pictq[is->pictq_windex];  
    if(vp->bmp)   
    {  
        // we already have one make another, bigger/smaller  
        SDL_FreeYUVOverlay(vp->bmp);  
    }  
    // Allocate a place to put our YUV image on that screen  
    vp->bmp = SDL_CreateYUVOverlay(is->video_st->codec->width,  
        is->video_st->codec->height,  
        SDL_YV12_OVERLAY,  
        screen);  
    vp->width = is->video_st->codec->width;  
    vp->height = is->video_st->codec->height;  
  
    SDL_LockMutex(is->pictq_mutex);  
    vp->allocated = 1;  
    SDL_CondSignal(is->pictq_cond);  
    SDL_UnlockMutex(is->pictq_mutex);  
  
}  
  
int queue_picture(VideoState *is, AVFrame *pFrame)   
{  
    //printf("queue_picture called/n");  
    VideoPicture *vp;  
    int dst_pix_fmt;  
    AVPicture pict;  
    static struct SwsContext *img_convert_ctx;  
    if (img_convert_ctx == NULL)   
    {  
        img_convert_ctx = sws_getContext(is->video_st->codec->width, is->video_st->codec->height,  
                                         is->video_st->codec->pix_fmt,  
                                         is->video_st->codec->width, is->video_st->codec->height,  
                                         PIX_FMT_YUV420P,  
                                         sws_flags, NULL, NULL, NULL);  
        if (img_convert_ctx == NULL)   
        {  
            fprintf(stderr, "Cannot initialize the conversion context/n");  
            exit(1);  
        }  
    }  
  
    /* wait until we have space for a new pic */  
    SDL_LockMutex(is->pictq_mutex);  
    while(is->pictq_size >= VIDEO_PICTURE_QUEUE_SIZE &&  
        !is->quit)   
    {  
        SDL_CondWait(is->pictq_cond, is->pictq_mutex);  
    }  
    SDL_UnlockMutex(is->pictq_mutex);  
  
    if(is->quit)  
        return -1;  
  
    // windex is set to 0 initially  
    vp = &is->pictq[is->pictq_windex];  
      
    /* allocate or resize the buffer! */  
    if(!vp->bmp ||  
        vp->width != is->video_st->codec->width ||  
        vp->height != is->video_st->codec->height)   
    {  
        SDL_Event event;  
  
        vp->allocated = 0;  
        /* we have to do it in the main thread */  
        event.type = FF_ALLOC_EVENT;  
        event.user.data1 = is;  
        SDL_PushEvent(&event);  
  
        /* wait until we have a picture allocated */  
        SDL_LockMutex(is->pictq_mutex);  
        while(!vp->allocated && !is->quit)   
        {  
            SDL_CondWait(is->pictq_cond, is->pictq_mutex);    //û�еõ���Ϣʱ�������õ���Ϣ���������SDL_CondSignal���ʹ��  
        }  
        SDL_UnlockMutex(is->pictq_mutex);  
        if(is->quit)   
        {  
            return -1;  
        }  
    }  
    /* We have a place to put our picture on the queue */  
  
    if(vp->bmp)   
    {  
        SDL_LockYUVOverlay(vp->bmp);  
        dst_pix_fmt = PIX_FMT_YUV420P;  
        /* point pict at the queue */  
  
        pict.data[0] = vp->bmp->pixels[0];  
        pict.data[1] = vp->bmp->pixels[2];  
        pict.data[2] = vp->bmp->pixels[1];  
  
        pict.linesize[0] = vp->bmp->pitches[0];  
        pict.linesize[1] = vp->bmp->pitches[2];  
        pict.linesize[2] = vp->bmp->pitches[1];  
  
        // Convert the image into YUV format that SDL uses  
        sws_scale(img_convert_ctx, pFrame->data, pFrame->linesize,  
                    0, is->video_st->codec->height, pict.data, pict.linesize);  
        SDL_UnlockYUVOverlay(vp->bmp);  
        /* now we inform our display thread that we have a pic ready */  
        if(++is->pictq_windex == VIDEO_PICTURE_QUEUE_SIZE)   
        {  
            is->pictq_windex = 0;  
        }  
        SDL_LockMutex(is->pictq_mutex);  
        is->pictq_size++;  
        SDL_UnlockMutex(is->pictq_mutex);  
    }  
    return 0;  
}  
  
int video_thread(void *arg)   
{  
    //printf("video_thread called");  
    VideoState *is = (VideoState *)arg;  
    AVPacket pkt1, *packet = &pkt1;  
    int len1, frameFinished;  
    AVFrame *pFrame;  
  
    pFrame = avcodec_alloc_frame();  
  
    for(;;)   
    {  
        if(packet_queue_get(&is->videoq, packet, 1) < 0)   
        {  
            // means we quit getting packets  
            break;  
        }  
        // Decode video frame  
        len1 = avcodec_decode_video(is->video_st->codec, pFrame, &frameFinished,   
            packet->data, packet->size);  
  
        // Did we get a video frame?  
        if(frameFinished)   
        {  
            if(queue_picture(is, pFrame) < 0)   
            {  
                break;  
            }  
        }  
        av_free_packet(packet);  
    }  
    av_free(pFrame);  
    return 0;  
}  
  
int stream_component_open(VideoState *is, int stream_index)   
{  
    AVFormatContext *pFormatCtx = is->pFormatCtx;  
    AVCodecContext *codecCtx;  
    AVCodec *codec;  
    SDL_AudioSpec wanted_spec, spec;  
  
    if(stream_index < 0 || stream_index >= pFormatCtx->nb_streams)   
    {  
        return -1;  
    }  
  
    // Get a pointer to the codec context for the video stream  
    codecCtx = pFormatCtx->streams[stream_index]->codec;  
  
    if(codecCtx->codec_type == CODEC_TYPE_AUDIO)   
    {  
        // Set audio settings from codec info  
        wanted_spec.freq = codecCtx->sample_rate;  
        wanted_spec.format = AUDIO_S16SYS;  
        wanted_spec.channels = codecCtx->channels;  
        wanted_spec.silence = 0;  
        wanted_spec.samples = SDL_AUDIO_BUFFER_SIZE;  
        wanted_spec.callback = audio_callback;  
        wanted_spec.userdata = is;  
  
        if(SDL_OpenAudio(&wanted_spec, &spec) < 0)   
        {  
            fprintf(stderr, "SDL_OpenAudio: %s/n", SDL_GetError());  
            return -1;  
        }  
    }  
    codec = avcodec_find_decoder(codecCtx->codec_id);  
    if(!codec || (avcodec_open(codecCtx, codec) < 0))   
    {  
        fprintf(stderr, "Unsupported codec!/n");  
        return -1;  
    }  
  
    switch(codecCtx->codec_type)   
    {  
    case CODEC_TYPE_AUDIO:  
        is->audioStream = stream_index;  
        is->audio_st = pFormatCtx->streams[stream_index];  
        is->audio_buf_size = 0;  
        is->audio_buf_index = 0;  
        memset(&is->audio_pkt, 0, sizeof(is->audio_pkt));  
        packet_queue_init(&is->audioq);  
        SDL_PauseAudio(0);  
        break;  
    case CODEC_TYPE_VIDEO:  
        is->videoStream = stream_index;  
        is->video_st = pFormatCtx->streams[stream_index];  
  
        packet_queue_init(&is->videoq);  
        is->video_tid = SDL_CreateThread(video_thread, is);  
        break;  
    default:  
        break;  
    }  
}  
  
int decode_interrupt_cb(void)   
{  
    return (global_video_state && global_video_state->quit);  
}  
  
int decode_thread(void *arg)   
{  
    VideoState *is = (VideoState *)arg;  
    AVFormatContext *pFormatCtx;  
    AVPacket pkt1, *packet = &pkt1;  
  
    int video_index = -1;  
    int audio_index = -1;  
    int i;  
  
    is->videoStream=-1;  
    is->audioStream=-1;  
  
    global_video_state = is;  
    // will interrupt blocking functions if we quit!  
    url_set_interrupt_cb(decode_interrupt_cb);  
  
    // Open video file  
    if(av_open_input_file(&pFormatCtx, is->filename, NULL, 0, NULL)!=0)  
        return -1; // Couldn't open file  
  
    is->pFormatCtx = pFormatCtx;  
  
    // Retrieve stream information  
    if(av_find_stream_info(pFormatCtx)<0)  
        return -1; // Couldn't find stream information  
  
    // Dump information about file onto standard error  
    dump_format(pFormatCtx, 0, is->filename, 0);  
  
    // Find the first video stream  
  
    for(i=0; i<pFormatCtx->nb_streams; i++)  
    {  
        if(pFormatCtx->streams[i]->codec->codec_type==CODEC_TYPE_VIDEO &&  
            video_index < 0)   
        {  
                video_index=i;  
        }  
        if(pFormatCtx->streams[i]->codec->codec_type==CODEC_TYPE_AUDIO &&  
            audio_index < 0)   
        {  
                audio_index=i;  
        }  
    }  
    if(audio_index >= 0)   
    {  
        stream_component_open(is, audio_index);  
    }  
    if(video_index >= 0)   
    {  
        stream_component_open(is, video_index);  
    }     
  
    if(is->videoStream < 0 || is->audioStream < 0)   
    {  
        fprintf(stderr, "%s: could not open codecs/n", is->filename);  
        goto fail;  
    }  
  
    // main decode loop  
    for(;;)   
    {  
        if(is->quit)   
        {  
            break;  
        }  
        // seek stuff goes here  
        if(is->audioq.size > MAX_AUDIOQ_SIZE ||  
            is->videoq.size > MAX_VIDEOQ_SIZE)   
        {  
                SDL_Delay(10);  
                continue;  
        }  
        if(av_read_frame(is->pFormatCtx, packet) < 0)   
        {  
            if(pFormatCtx->pb && pFormatCtx->pb->error == 0)   
            {  
                SDL_Delay(100); /* no error; wait for user input */  
                continue;  
            }   
            else   
            {  
                break;  
            }  
        }  
        // Is this a packet from the video stream?  
        if(packet->stream_index == is->videoStream)  
        {  
            packet_queue_put(&is->videoq, packet);  
        }   
        else if(packet->stream_index == is->audioStream)  
        {  
            packet_queue_put(&is->audioq, packet);  
        }   
        else  
        {  
            av_free_packet(packet);  
        }  
    }  
    /* all done - wait for it */  
    while(!is->quit)   
    {  
        SDL_Delay(100);  
    }  
fail:  
    SDL_Event event;  
    event.type = FF_QUIT_EVENT;  
    event.user.data1 = is;  
    SDL_PushEvent(&event);  
    return 0;  
}  
  
int main(int argc, char *argv[])  
{  
    SDL_Event       event;  
    VideoState      *is;  
    is = (VideoState *)av_mallocz(sizeof(VideoState));  
    if(argc < 2)   
    {  
        fprintf(stderr, "Usage: test <file>/n");  
        exit(1);  
    }  
    // Register all formats and codecs  
    av_register_all();  
  
    if(SDL_Init(SDL_INIT_VIDEO | SDL_INIT_AUDIO | SDL_INIT_TIMER))   
    {  
        fprintf(stderr, "Could not initialize SDL - %s/n", SDL_GetError());  
        exit(1);  
    }  
  
    // Make a screen to put our video  
#ifndef __DARWIN__  
    screen = SDL_SetVideoMode(640, 480, 0, 0);  
#else  
    screen = SDL_SetVideoMode(640, 480, 24, 0);  
#endif  
    if(!screen)  
    {  
        fprintf(stderr, "SDL: could not set video mode - exiting/n");  
        exit(1);  
    }  
  
    //pstrcpy(is->filename, sizeof(is->filename), argv[1]);  
    strcpy(is->filename,argv[1]);  
    is->pictq_mutex = SDL_CreateMutex();  
    is->pictq_cond = SDL_CreateCond();  
  
    schedule_refresh(is, 40);  
    is->parse_tid = SDL_CreateThread(decode_thread, is);  
  
    if(!is->parse_tid)  
    {  
        av_free(is);  
        return -1;  
    }  
    for(;;)  
    {  
        SDL_WaitEvent(&event);  
        switch(event.type)   
        {  
        case FF_QUIT_EVENT:  
            printf("FF_QUIT_EVENT recieved");  
        case SDL_QUIT:  
            printf("SDL_QUIT recieved");  
            is->quit = 1;  
            SDL_Quit();  
            return 0;  
            break;  
        case FF_ALLOC_EVENT:  
            alloc_picture(event.user.data1);  
            break;  
        case FF_REFRESH_EVENT:  
            video_refresh_timer(event.user.data1);  
            break;  
        default:  
            break;  
        }  
    }  
    return 0;  
}  