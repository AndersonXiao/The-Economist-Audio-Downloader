//
// Created by Administrator on 2019/5/16.
//

#include <ffmpeg_mediaplayer.h>

static int one=0;
static int two=0;

void packet_queue_init(PacketQueue *q)
{
    memset(q, 0, sizeof(PacketQueue));
    q->initialized=1;
    q->mutex=SDL_CreateMutex();
    q->cond=SDL_CreateCond();
}

int packet_queue_put(VideoState *is, PacketQueue *q, AVPacket *pkt)
{
    AVPacketList *pkt1;
    if(pkt!=&is->flush_pkt && av_dup_packet(pkt)<0)
    {
        return -1;
    }
    pkt1=av_malloc(sizeof(AVPacketList));
    if(!pkt1)
        return -1;
    pkt1->pkt=*pkt;
    pkt1->next=NULL;

    SDL_LockMutex(q->mutex);

    if(!q->last_pkt)
        q->first_pkt=pkt1;
    else
        q->last_pkt->next=pkt1;
    q->last_pkt=pkt1;
    q->nb_packets++;
    q->size+=pkt1->pkt.size;
    SDL_CondSignal(q->cond);

    SDL_UnlockMutex(q->mutex);
    return 0;
}

static int packet_queue_get(VideoState *is, PacketQueue *q, AVPacket *pkt, int block)
{
    AVPacketList *pkt1;
    int ret;

    SDL_LockMutex(q->mutex);

    for( ; ; )
    {
        if(is->quit)
        {
            ret=-1;
            break;
        }

        pkt1=q->first_pkt;
        if(pkt1)
        {
            q->first_pkt=pkt1->next;
            if(!q->first_pkt)
                q->last_pkt = NULL;
            q->nb_packets--;
            q->size-=pkt1->pkt.size;
            *pkt=pkt1->pkt;
            av_free(pkt1);
            ret=1;
            break;
        }
        else if(!block)
        {
            ret=0;
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

static void packet_queue_flush(PacketQueue *q)
{
    AVPacketList *pkt, *pkt1;

    SDL_LockMutex(q->mutex);
    for(pkt=q->first_pkt;pkt!=NULL;pkt=pkt1)
    {
        pkt1=pkt->next;
        av_packet_unref(&pkt->pkt);
        av_freep(&pkt);
    }
    q->last_pkt=NULL;
    q->first_pkt=NULL;
    q->nb_packets=0;
    q->size=0;
    SDL_UnlockMutex(q->mutex);
}

double get_audio_clock(VideoState *is)
{
    double pts;
    int hw_buf_size, bytes_per_sec, n;

    pts=is->audio_clock;
    hw_buf_size=is->audio_buf_size-is->audio_buf_index;
    bytes_per_sec=0;
    n=is->audio_st->codec->channels*2;
    if(is->audio_st)
    {
        bytes_per_sec=is->audio_st->codec->sample_rate*n;
    }
    if(bytes_per_sec)
    {
        pts-=(double)hw_buf_size/bytes_per_sec;
    }
    return pts;
}

double get_video_clock(VideoState *is)
{
    double delta;

    delta=(av_gettime()-is->video_current_pts_time)/1000000.0;
    return is->video_current_pts+delta;
}

double get_external_clock(VideoState *is)
{
    return av_gettime()/1000000.0;
}

double get_master_clock(VideoState *is)
{
    if(is->av_sync_type==AV_SYNC_VIDEO_MASTER)
    {
        return get_video_clock(is);
    }
    else if(is->av_sync_type==AV_SYNC_AUDIO_MASTER)
    {
        return get_audio_clock(is);
    }
    else
    {
        return get_external_clock(is);
    }
}

int synchronize_audio(VideoState *is, short *samples, int samples_size, double pts)
{
    int n;
    double ref_clock;

    n=2*is->audio_st->codec->channels;

    if(is->av_sync_type!=AV_SYNC_AUDIO_MASTER)
    {
        double diff, avg_diff;
        int wanted_size, min_size, max_size;

        ref_clock=get_master_clock(is);
        diff=get_audio_clock(is)-ref_clock;

        if(diff<AV_NOSYNC_THRESHOLD)
        {
            is->audio_diff_cum=diff+is->audio_diff_avg_coef*is->audio_diff_cum;
            if(is->audio_diff_avg_count<AUDIO_DIFF_AVG_NB)
            {
                is->audio_diff_avg_count++;
            }
            else
            {
                avg_diff=is->audio_diff_cum*(1.0-is->audio_diff_avg_coef);
                if(fabs(avg_diff)>=is->audio_diff_threashold)
                {
                    wanted_size=samples_size+((int)(diff*is->audio_st->codec->sample_rate)*n);
                    min_size=samples_size*((100-SAMPLE_CORRECTION_PERCENT_MAX)/100);
                    max_size=samples_size*((100+SAMPLE_CORRECTION_PERCENT_MAX)/100);
                    if(wanted_size<min_size)
                    {
                        wanted_size=min_size;
                    }
                    else if(wanted_size>max_size)
                    {
                        wanted_size=max_size;
                    }

                    if(wanted_size<samples_size)
                    {
                        samples_size=wanted_size;
                    }
                    else if(wanted_size>samples_size)
                    {
                        uint8_t *samples_end, *q;
                        int nb;

                        nb=(samples_size-wanted_size);
                        samples_end=(uint8_t *)samples+samples_size-n;
                        q=samples_end+n;
                        while(nb>0)
                        {
                            memcpy(q, samples_end, n);
                            q+=n;
                            nb-=n;
                        }
                        samples_size=wanted_size;
                    }
                }
            }
        }
        else
        {
            is->audio_diff_avg_count=0;
            is->audio_diff_cum=0;
        }
    }

    return samples_size;
}

int decode_frame_from_packet(VideoState *is, AVFrame decoded_frame)
{
    int64_t src_ch_layout, dst_ch_layout;
    int src_rate, dst_rate;
    uint8_t **src_data=NULL, *dst_data=NULL;
    int src_nb_channels=0, dst_nb_channels=0;
    int src_linesize, dst_linesize;
    int src_nb_samples, dst_nb_samples, max_dst_nb_samples;
    enum AVSampleFormat src_sample_fmt, dst_sample_fmt;
    int dst_buffsize;
    int ret;

    src_nb_samples=decoded_frame.nb_samples;
    src_linesize=(int)decoded_frame.linesize;
    src_data=decoded_frame.data;

    if(decoded_frame.channel_layout==0)
    {
        decoded_frame.channel_layout=av_get_default_channel_layout(decoded_frame.channels);
    }

    src_rate=decoded_frame.sample_rate;
    dst_rate=decoded_frame.sample_rate;
    src_ch_layout=decoded_frame.channel_layout;
    dst_ch_layout=decoded_frame.channel_layout;
    src_sample_fmt=decoded_frame.format;
    dst_sample_fmt=AV_SAMPLE_FMT_S16;

    src_nb_channels=av_get_channel_layout_nb_channels(src_ch_layout);
    ret=av_samples_alloc_array_and_samples(&src_data, &src_linesize, src_nb_channels, src_nb_samples, src_sample_fmt,0);
    src_nb_channels=av_get_channel_layout_nb_channels(src_ch_layout);
    ret=av_samples_alloc_array_and_samples(&src_data, &src_linesize, src_nb_channels, src_nb_samples, src_sample_fmt,0);
    if(ret<0)
    {
        fprintf(stderr, "Could not allocate source samples\n");
        return -1;
    }

    max_dst_nb_samples=dst_nb_samples=av_rescale_rnd(src_nb_samples, dst_rate, src_rate, AV_ROUND_UP);
    dst_nb_channels=av_get_channel_layout_nb_channels(dst_ch_layout);
    ret=av_samples_alloc_array_and_samples(&dst_data, &dst_linesize, dst_nb_channels, dst_nb_samples, dst_sample_fmt,0);
    if(ret<0)
    {
        fprintf(stderr, "Could not allocate destination samples\n");
        return -1;
    }

    dst_nb_samples=av_rescale_rnd(swr_get_delay(is->sws_ctx_audio, src_rate)+src_nb_samples, dst_rate, src_rate, AV_ROUND_UP);
    ret=swr_convert(is->sws_ctx_audio, dst_data, dst_nb_samples, (const uint8_t **)decoded_frame.data, src_nb_samples);
    if(ret<0)
    {
        fprintf(stderr, "Error while converting\n");
        return -1;
    }

    dst_buffsize=av_samples_get_buffer_size(&dst_linesize, dst_nb_channels, ret, dst_sample_fmt, 1);
    if(dst_buffsize<0)
    {
        fprintf(stderr, "Could not get sample buffer size\n");
        return -1;
    }

    memcpy(is->audio_buf, dst_data[0], dst_buffsize);

    if(src_data)
    {
        av_freep(&src_data[0]);
    }
    av_freep(&src_data);

    if(dst_data)
    {
        av_freep(&dst_data[0]);
    }
    av_freep(&dst_data);

    return dst_buffsize;
}

int audio_decode_frame(VideoState *is, double *pts_ptr)
{
    int len1, data_size=0, n;
    AVPacket *pkt=&is->audio_pkt;
    double pts;

    for( ; ; )
    {
        while(is->audio_pkt_size>0)
        {
            int got_frame=0;
            len1=avcodec_decode_audio4(is->audio_st->codec, &is->audio_frame, &got_frame, pkt);
            if(len1<0)
            {
                is->audio_pkt_size=0;
                break;
            }

            if(got_frame)
            {
                if(is->audio_frame.format!=AV_SAMPLE_FMT_S16)
                {
                    data_size=decode_frame_from_packet(is, is->audio_frame);
                }
                else
                {
                    data_size=av_samples_get_buffer_size(NULL, is->audio_st->codec->channels,is->audio_frame.nb_samples,
                                is->audio_st->codec->sample_fmt,1);
                    memcpy(is->audio_buf, is->audio_frame.data[0], data_size);
                }
            }
            is->audio_pkt_data+=len1;
            is->audio_pkt_size-=len1;

            if(data_size<=0)
            {
                continue;
            }
            pts=is->audio_clock;
            *pts_ptr=pts;
            n=2*is->audio_st->codec->channels;
            is->audio_clock+=(double)data_size/(double)(n*is->audio_st->codec->sample_rate);
            return data_size;
        }

        if(pkt->data)
            av_packet_unref(pkt);

        if(is->quit)
            return -1;

        if(packet_queue_get(is, &is->audioq, pkt, 1)<0)
        {
            avcodec_flush_buffers(is->audio_st->codec);
            continue;
        }
        is->audio_pkt_data=pkt->data;
        is->audio_pkt_size=pkt->size;

        if(pkt->pts!=AV_NOPTS_VALUE)
        {
            is->audio_clock=av_q2d(is->audio_st->time_base);
        }
    }
}

void audio_callback(void *userdata, Uint8 *stream, int len)
{
    VideoState *is=(VideoState *)userdata;
    int len1, audio_size;
    double pts;

    while(len>0)
    {
        if(is->audio_buf_index>=is->audio_buf_size)
        {
            audio_size=audio_decode_frame(is, &pts);
            if(audio_size<0)
            {
                is->audio_buf_size=1024;
                memset(is->audio_buf, 0, is->audio_buf_size);
            }
            else
            {
                audio_size=synchronize_audio(is, (int16_t *)is->audio_buf, audio_size, pts);
                is->audio_buf_index=0;
            }
            is->audio_buf_index=0;
        }
        len1=is->audio_buf_size-is->audio_buf_index;
        if(len1>len)
        {
            len1=len;
            memcpy(stream, (uint8_t *)is->audio_buf+is->audio_buf_index, len1);
            len-=len1;
            stream+=len1;
            is->audio_buf_index+=len1;
        }
    }
}

void video_refresh_timer(void *userdata);

static Uint32 sdl_refresh_timer_cb(Uint32 interval, void *opaque)
{
    video_refresh_timer(opaque);
    return 0;
}

static void schedule_refresh(VideoState *is, int delay)
{
    SDL_AddTimer(delay, sdl_refresh_timer_cb, is);
}

void video_display(VideoState *is)
{
    SDL_Rect rect;
    VideoPicture *vp;

    float aspect_ratio;
    int w, h, x, y;

    vp=&is->pictq[is->pictq_rindex];
    if(vp->bmp)
    {
        if(is->video_st->codec->sample_aspect_ratio.num==0)
        {
            aspect_ratio=0;
        }
        else
        {
            aspect_ratio=av_q2d(is->video_st->codec->sample_aspect_ratio)* is->video_st->codec->width/is->video_st->codec->height;
        }

        if(aspect_ratio<=0.0)
        {
            aspect_ratio = (float) is->video_st->codec->width / (float) is->video_st->codec->height;
        }

        displayBmp(&is->video_player, vp->bmp, is->video_st->codec, is->video_st->codec->width, is->video_st->codec->height);
        free(vp->bmp->buffer);
    }
}

void video_refresh_timer(void *opaque)
{
    VideoState *is=(VideoState *)opaque;
    VideoPicture *vp;
    double actual_delay, delay, sync_threshold, ref_clock, diff;

    for( ; ; )
    {
        if(is->quit)
        {
            break;
        }

        if(is->video_st)
        {
            if(is->pictq_size==0)
            {
                SDL_Delay(1);
                continue;
            }
            else
            {
                vp=&is->pictq[is->pictq_rindex];

                is->video_current_pts=vp->pts;
                is->video_current_pts_time=av_gettime();

                delay=vp->pts - is->frame_last_pts;
                if(delay<=0||delay>=1.0)
                {
                    delay=is->frame_last_delay;
                }

                is->frame_last_delay=delay;
                is->frame_last_pts=vp->pts;

                if(is->av_sync_type!=AV_SYNC_VIDEO_MASTER)
                {
                    ref_clock=get_master_clock(is);
                    diff=vp->pts-ref_clock;
                    sync_threshold=(delay>AV_SYNC_THRESHOLD)?delay: AV_SYNC_THRESHOLD;
                    if(fabs(diff)<AV_NOSYNC_THRESHOLD)
                    {
                        if(diff<=-sync_threshold)
                        {
                            delay=0;
                        }
                        else if(diff>=sync_threshold)
                        {
                            delay=2*delay;
                        }
                    }
                }
                is->frame_timer+=delay;
                actual_delay=is->frame_timer-(av_gettime()/100.0);
                if(actual_delay<0.010)
                {
                    actual_delay=0.010;
                }

                video_display(is);
                if(++is->pictq_rindex==VIDEO_PICTURE_QUEUE_SIZE)
                {
                    is->pictq_rindex=0;
                }
                SDL_LockMutex(is->pictq_mutex);
                is->pictq_size--;
                SDL_CondSignal(is->pictq_cond);
                SDL_UnlockMutex(is->pictq_mutex);

                SDL_Delay((int)(actual_delay*1000+0.5));
                continue;
            }
        }
        else
        {
            SDL_Delay(100);
            continue;
        }
    }
}

void alloc_picture(void *userdata)
{
    VideoState *is=(VideoState *)userdata;
    VideoPicture *vp;

    vp=&is->pictq[is->pictq_windex];
    if(vp->bmp)
    {
        destroyBmp(&is->video_player, vp->bmp);
    }

    vp->bmp=createBmp(&is->video_player, is->video_st->codec->width, is->video_st->codec->height);

    vp->width=is->video_st->codec->width;
    vp->height=is->video_st->codec->height;

    SDL_LockMutex(is->pictq_mutex);
    vp->allocated=1;
    SDL_CondSignal(is->pictq_cond);
    SDL_UnlockMutex(is->pictq_mutex);
}

int queue_picture(VideoState *is, AVFrame *pFrame, double pts)
{
    VideoPicture *vp;
    AVPicture pict;

    SDL_LockMutex(is->pictq_mutex);
    while(is->pictq_size>=VIDEO_PICTURE_QUEUE_SIZE&&!is->quit)
    {
        SDL_CondWait(is->pictq_cond, is->pictq_mutex);
    }
    SDL_UnlockMutex(is->pictq_mutex);

    if(is->quit)
        return -1;

    vp=&is->pictq[is->pictq_windex];

    if(!vp->bmp|| vp->width!=is->video_st->codec->width || vp->height!=is->video_st->codec->height)
    {
        vp->allocated=0;

        alloc_picture(is);
        SDL_LockMutex(is->pictq_mutex);
        while(~vp->allocated&&!is->quit)
        {
            SDL_CondWait(is->pictq_cond, is->pictq_mutex);
        }
        SDL_UnlockMutex(is->pictq_mutex);
        if(is->quit)
        {
            return -1;
        }
    }

    if(vp->bmp)
    {
        updateBmp(&is->video_player, is->sws_ctx, is->video_st->codec, vp->bmp, pFrame, is->video_st->codec->width, is->video_st->codec->height);
        vp->pts=pts;

        if(++is->pictq_windex==VIDEO_PICTURE_QUEUE_SIZE)
        {
            is->pictq_windex=0;
        }
        SDL_LockMutex(is->pictq_mutex);
        is->pictq_size++;
        SDL_UnlockMutex(is->pictq_mutex);
    }
    return 0;
}

double synchronize_video(VideoState *is, AVFrame *src_frame, double pts)
{
    double frame_delay;

    if(pts!=0)
    {
        is->video_clock=pts;
    }
    else
    {
        pts=is->video_clock;
    }

    frame_delay=av_q2d(is->video_st->codec->time_base);
    frame_delay+=src_frame->repeat_pict*(frame_delay * 0.5);
    is->video_clock+=frame_delay;

    return pts;
}

uint64_t global_video_pkt_pts=AV_NOPTS_VALUE;

int our_get_buffer(struct AVCodecContext *c, AVFrame *pic, int flags)
{
    int ret=avcodec_default_get_buffer2(c, pic, flags);
    uint64_t *pts=av_malloc(sizeof(uint64_t));
    *pts=global_video_pkt_pts;
    pic->opaque=pts;
    return ret;
}

int video_thread(void *arg)
{
    VideoState *is=(VideoState *) arg;
    AVPacket pkt1, *packet=&pkt1;
    int frameFinished;
    AVFrame *pFrame;
    double pts;

    pFrame=av_frame_alloc();

    for( ; ; )
    {
        if(packet_queue_get(is, &is->videoq, packet, 1)<0)
        {
            break;
        }
        if(packet->data==is->flush_pkt.data)
        {
            avcodec_flush_buffers(is->video_st->codec);
            continue;
        }

        pts=0;

        global_video_pkt_pts=packet->pts;

        avcodec_decode_video2(is->video_st->codec, pFrame, &frameFinished, packet);

        if(packet->dts==AV_NOPTS_VALUE&&pFrame->opaque&&*(uint64_t*)pFrame->opaque!=AV_NOPTS_VALUE)
        {
            pts=*(uint64_t*)pFrame->opaque;
        }
        else if(packet->dts!=AV_NOPTS_VALUE)
        {
            pts=packet->dts;
        }
        else
        {
            pts=0;
        }
        pts*=av_q2d(is->video_st->time_base);

        if(frameFinished)
        {
            pts=synchronize_video(is,pFrame, pts);
            if(queue_picture(is, pFrame, pts)<0)
            {
                break;
            }
        }
        av_packet_unref(packet);
    }
    av_free(pFrame);

    two=1;
    return 0;
}

int stream_component_open(VideoState *is, int stream_index)
{
    AVFormatContext *pFormatCtx=is->pFormatCtx;
    AVCodecContext *codecCtx=NULL;
    AVCodec *codec=NULL;
    AVDictionary *optionsDict=NULL;

    if(stream_index<0||stream_index>=pFormatCtx->nb_streams)
    {
        return -1;
    }

    codecCtx=pFormatCtx->streams[stream_index]->codec;

    if(codecCtx->codec_type==AVMEDIA_TYPE_AUDIO)
    {
        is->audio_callback=audio_callback;

        AudioPlayer *player=malloc(sizeof(AudioPlayer));
        is->audio_player=player;
        createEngine(&is->audio_player);
        createBufferQueueAudioPlayer(&is->audio_player, is, codecCtx->channels, codecCtx->sample_rate, is->stream_type);
    }
    else if(codecCtx->codec_type==AVMEDIA_TYPE_VIDEO)
    {
        VideoPlayer *player=malloc(sizeof(VideoPlayer));
        is->video_player=player;
        createVideoEngine(&is->video_player);
        createScreen(&is->video_player, is->native_window, 0, 0);
    }
    codec=avcodec_find_decoder(codecCtx->codec_id);
    if(!codec||(avcodec_open2(codecCtx, codec, &optionsDict)<0))
    {
        fprintf(stderr, "Unsupported codec!\n");
        return -1;
    }
    switch(codecCtx->codec_type)
    {
        case AVMEDIA_TYPE_AUDIO:
            is->audioStream=stream_index;
            is->audio_st=pFormatCtx->streams[stream_index];
            is->audio_buf_size=0;
            is->audio_buf_index=0;

            is->audio_diff_avg_coef=exp(log(0.01/AUDIO_DIFF_AVG_NB));
            is->audio_diff_avg_count=0;

            is->audio_diff_threshold=2.0*SDL_AUDIO_BUFFER_SIZE/codecCtx->sample_rate;
            is->sws_ctx_audio=swr_alloc();
            if(!is->sws_ctx_audio)
            {
                fprintf(stderr, "Could not allocate resampler context\n");
                return -1;
            }

            uint64_t channel_layout=is->audio_st->codec->channel_layout;

            if(channel_layout==0)
            {
                channel_layout=av_get_default_channel_layout(is->audio_st->codec->channels);
            }

            av_opt_set_int(is->sws_ctx_audio, "in_channel_layout", channel_layout, 0);
            av_opt_set_int(is->sws_ctx_audio, "out_channel_layout", channel_layout, 0);
            av_opt_set_int(is->sws_ctx_audio, "in_sample_rate", is->audio_st->codec->sample_rate, 0);
            av_opt_set_int(is->sws_ctx_audio, "out_sample_rate", is->audio_st->codec->sample_rate, 0);
            av_opt_set_sample_fmt(is->sws_ctx_audio,"in_sample_fmt", is->audio_st->codec->sample_fmt, 0);
            av_opt_set_sample_fmt(is->sws_ctx_audio, "out_sample_fmt", AV_SAMPLE_FMT_S16, 0);

            if((swr_init(is->sws_ctx_audio))<0)
            {
                fprintf(stderr, "Failed to initialize the resampling context\n");
                return -1;
            }
            memset(&is->audio_pkt, 0, sizeof(is->audio_pkt));
            packet_queue_init(&is->audioq);
            break;
        case AVMEDIA_TYPE_VIDEO:
            is->videoStream=stream_index;
            is->video_st=pFormatCtx->streams[stream_index];

            is->frame_timer=(double)av_gettime()/1000000.0;
            is->frame_last_delay=40e-3;
            is->video_current_pts_time=av_gettime();

            packet_queue_init(&is->videoq);

            createScreen(&is->video_player, is->native_window, is->video_st->codec->width, is->video_st->codec->height);

            is->video_tid=malloc(sizeof(*(is->video_tid)));

            pthread_create(is->video_tid, NULL, (void *)&video_thread, is);
            is->sws_ctx=createScaler(&is->video_player, is->video_st->codec);

            codecCtx->get_buffer2=our_get_buffer;

            break;
        default:
            break;
    }

    return 0;
}