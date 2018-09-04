//
// Created by 조원근 on 7/25/18.
//

#include "wongeun_com_androidwithffplay_NDKAdapter.h"
#include "libavcodec/avcodec.h"
#include "libavformat/avformat.h"
#include "libswscale/swscale.h"
#include <android/native_window.h>
#include <android/native_window_jni.h>
#include <android/log.h>

#include <pthread.h>
#include <assert.h>


#define LOGV(...) __android_log_print(ANDROID_LOG_VERBOSE, "libnav", __VA_ARGS__)
#define LOGD(...) __android_log_print(ANDROID_LOG_DEBUG, "libnav", __VA_ARGS__)
#define LOGI(...) __android_log_print(ANDROID_LOG_INFO, "libnav", __VA_ARGS__)
#define LOGW(...) __android_log_print(ANDROID_LOG_WARN, "libnav", __VA_ARGS__)
#define LOGE(...) __android_log_print(ANDROID_LOG_ERROR, "libnav", __VA_ARGS__)

#define VIDEO_PICTURE_QUEUE_SIZE 3
#define SUBPICTURE_QUEUE_SIZE 16
#define SAMPLE_QUEUE_SIZE 9

char *uri;
int isPlaying = 0;

const int INIT_VIDEO = 1;
const int INIT_AUDIO = 2;
const int INIT_TIMER = 3;

typedef struct MyAVPacketList {

    AVPacket pkt;
    struct MyAVPacketList *next;
    int serial;

} MyAVPacketList;

typedef struct AudioParams {
    int freq;
    int channels;
    int64_t channel_layout;
    enum AVSampleFormat fmt;
    int frame_size;
    int bytes_per_sec;
} AudioParams;

typedef struct Clock {
    double pts;           /* clock base */
    double pts_drift;     /* clock base minus time at which we updated the clock */
    double last_updated;
    double speed;
    int serial;           /* clock is based on a packet with this serial */
    int paused;
    int *queue_serial;    /* pointer to the current packet queue serial, used for obsolete clock detection */
} Clock;

/* Common struct for handling all types of decoded data and allocated render buffers. */
typedef struct Frame {
    AVFrame *frame;
    AVSubtitle sub;

    int serial;
    double pts;           /* presentation timestamp for the frame */
    double duration;      /* estimated duration of the frame */
    int64_t pos;          /* byte position of the frame in the input file */
    int width;
    int height;
    int format;
    AVRational sar;
    int uploaded;
    int flip_v;
} Frame;

typedef struct FrameQueue {
    Frame queue[FRAME_QUEUE_SIZE];
    int rindex;
    int windex;
    int size;
    int max_size;
    int keep_last;
    int rindex_shown;
    pthread_mutex_t *mutex;
    pthread_cond_t *cond;
    PacketQueue *pktq;
} FrameQueue;

typedef struct PacketQueue {
  MyAVPacketList *first_pkt, *last_pkt;
  int nb_packets;
  int size;
  int64_t duration;
  int abort_request;
  int serial;
  pthread_mutex_t *mutex;
  pthread_cond_t *cond;
} PacketQueue;



/*typedef struct VideoPicture {
  SDL_Overlay *bmp;
  int width, height; /* source height & width
  int allocated;
} VideoPicture;*/

typedef struct VideoState {

    pthread_t      *read_tid;
    AVInputFormat  *iformat;

    int            abort_request;
    int            force_refresh;
    int            paused;
    int            last_paused;
    int            queue_attachments_req;
    int            seek_req;
    int            seek_flags;
    int64_t        seek_pos;
    int64_t        seek_rel;
    int            read_pause_return;
    AVFormatContext *ic;
    int             realtime;

    Clock           audclk;
    Clock           vidclk;
    Clock           extclk;

    FrameQueue      pictq;
    FrameQueue      subpq;
    FrameQueue      sampq;

    Decoder         auddec;
    Decoder         viddec;
    Decoder         subdec;

    int             audio_stream;
    int             av_sync_type;

    double          audio_clock;
    int             audio_clock_serial;
    double          audio_diff_cum;
    double          audio_diff_avg_coef;
    double          audio_diff_threshold;
    int             audio_diff_avg_count;
    AVStream        *audio_st;
    PacketQueue     audioq;
    int             audio_hw_buf_size;
    uint8_t         *audio_buf;
    uint8_t         *audio_buf1;
    unsigned int    audio_buf_size; /* in bytes */
    unsigned int    audio_buf1_size;
    int             audio_buf_index; /* in bytes */
    int             audio_write_buf_size;
    int             audio_volume;
    int             muted;
    struct          AudioParams audio_src;
#if CONFIG_AVFILTER
    struct          AudioParams audio_filter_src;
#endif
    struct          AudioParams audio_tgt;
    struct          SwrContext *swr_ctx;
    int             frame_drops_early;
    int             frame_drops_late;


    enum            ShowMode {
        SHOW_MODE_NONE = -1, SHOW_MODE_VIDEO = 0, SHOW_MODE_WAVES, SHOW_MODE_RDFT, SHOW_MODE_NB
    } show_mode;
    int16_t         sample_array[SAMPLE_ARRAY_SIZE];
    int             sample_array_index;
    int             last_i_start;
    RDFTContext     *rdft;
    int             rdft_bits;
    FFTSample       *rdft_data;
    int             xpos;
    double          last_vis_time;

    int             subtitle_stream;
    AVStream        *subtitle_st;
    PacketQueue     subtitleq;

    double          frame_timer;
    double          frame_last_returned_time;
    double          frame_last_filter_delay;
    int             video_stream;
    AVStream        *video_st;
    PacketQueue     videoq;
    double          max_frame_duration;      // maximum duration of a frame - above this, we consider the jump a timestamp discontinuity

    struct SwsContext *img_convert_ctx;
    struct SwsContext *sub_convert_ctx;
    int             eof;


    char *filename;
    int width, height, xleft, ytop;
    int step;

    int   pictq_size, pictq_rindex, pictq_windex;
    pthread_mutext_t* pictq_mutex;
    pthread_cond_t*   pictq_cond;

    #if CONFIG_AVFILTER
        int vfilter_idx;
        AVFilterContext *in_video_filter;   // the first filter in the video chain
        AVFilterContext *out_video_filter;  // the last filter in the video chain
        AVFilterContext *in_audio_filter;   // the first filter in the audio chain
        AVFilterContext *out_audio_filter;  // the last filter in the audio chain
        AVFilterGraph *agraph;              // audio filter graph
    #endif

    int last_video_stream, last_audio_stream, last_subtitle_stream;

    pthread_t       *video_tid;
    pthread_cond_t *continue_read_thread;

} VideoState;

static AVInputFormat *file_iformat;
static int find_stream_info = 1;
static int video_disable;
static int startup_volume = 100;
static int default_width  = 640;
static int default_height = 480;
static int genpts = 0;
static int64_t start_time = AV_NOPTS_VALUE;
static int av_sync_type = AV_SYNC_AUDIO_MASTER;
static const char *video_codec_name;
static int display_disable;
static int lowres = 0;
static int fast = 0;
static int find_stream_info = 1;
static int seek_by_bytes = -1;
static int infinite_buffer = -1;


static AVPacket flush_pkt;
static ANativeWindow* nativeWindow

static void sigterm_handler(int sig)
{
    exit(123);
}



static void do_exit(VideoState *is)
{
    if (is) {
        stream_close(is);
    }
    //window = NULL;
    uninit_opts();

#if CONFIG_AVFILTER
    av_freep(&vfilters_list);
#endif
    avformat_network_deinit();

    if (show_status)
        printf("\n");

    LOGE("do_exit\n");
    exit(0);

}

static int decode_interrupt_cb(void *ctx)
{
    VideoState *is = ctx;
    return is->abort_request;
}

static int stream_has_enough_packets(AVStream *st, int stream_id, PacketQueue *queue){
    return stream_id < 0 ||
            queue->abort_request ||
            (st->disposition & AV_DISPOSITION_ATTACHED_PIC) ||
            queue->nb_packets > MIN_FRAMES && (!queue->duration || 
                av_q2d(st->time_base) * queue->duration > 1.0);
}

static int is_realtime(AVFormatContext *s){
    if(!strcmp(s->iformat->name, "rtp")
        || !strcmp(s->iformat->name, "rtsp")
        || !strcmp(s->iformat->name, "sdp"))
        return 1;

    if(s->pb && (!strncmp(s->url, "rtp:", 4)
            ||   !strncmp(s->url, "udp:", 4)))
        return 1;

    return 0;
}

static int packet_queue_put_private(PacketQueue *q, AVPacket *pkt){
    
    MyAVPacketList *pktl;
    if(q->abort_request)
        return -1;

    pktl = av_malloc(sizeof(MyAVPacketList));

    if(!pktl)
        return -1;
    pktl->pkt = *pkt;
    pktl->next = NULL;
    if(pkt==&flush_pkt)
        q->serial++;
    pktl->serial = q->serial;

    if(!q->last_pkt)
        q->first_pkt = pktl;
    else
        q->last_pkt->next = pktl;
    q->last_pkt = pktl;
    q->nb_packets++;
    q->size+=pktl->pkt.size + sizeof(*pktl);
    q->duration += pktl->pkt.duration;

    pthread_cond_signal(q->cond);
    return 0;

}
static int packet_queue_put(PacketQueue *q, AVPacket *pkt){
    int ret;
    pthread_mutex_lock(q->mutex);
    ret = packet_queue_put_private(q,pkt);
    pthread_mutex_unlock(q->mutex);

    if(pkt != &flush_pkt && ret < 0)
        av_packet_unref(pkt);

    return ret;
}


static int packet_queue_init(PacketQueue *q) {
    memset(q, 0, sizeof(PacketQueue));

    pthread_mutex_init(q->mutex);

    if (!q->mutex) {
        LOGE("PACKET QUEUE, mutex_init error");
        return AVERROR(ENOMEM);
    }

    pthread_cond_init(q->cond);
    if (!q->cond) {
        LOGE("PACKET QUEUE, cond_init error");
        return AVERROR(ENOMEM);
    }

    q->abort_request = 1;

    return 0;
}

static int packet_queue_put_nullpacket(PacketQueue *q, int stream_index){
    AVPacket pktl, *pkt = &pktl;
    av_init_packet(pkt);
    pkt->data = NULL;
    pkt->size = 0;
    pkt->stream_index = stream_index;
    return packet_queue_put(q, pkt);
}

static void packet_queue_flush(PacketQueue *q){
    MyAVPacketList *pkt, *pkt1;

    pthread_mutex_lock(q->mutex);
    for(pkt = q->first_pkt; pkt; pkt = pkt1){
        pkt1 = pkt->next;
        av_packet_unref(&pkt->pkt);
        av_freep(&pkt);
    }

    q->last_pkt = NULL;
    q->first_pkt = NULL;
    q->nb_packets = 0;
    q->size = 0;
    q->duration = 0;
    pthread_mutex_unlock(q->mutex);

}


static void packet_queue_destroy(PacketQueue *q){
    packet_queue_flush(q);
    pthread_mutex_destroy(q->mutex);
    pthread_cond_destroy(q->cond);
}

static void packet_queue_abort(PacketQueue *q){
    pthread_mutex_lock(q->mutex);
    q->abort_request = 1;
    pthread_cond_signal(q->cond);
    pthread_mutex_unlock(q->mutex);
}

static void packet_queue_start(PacketQueue *q){
    pthread_mutex_lock(q->mutex);
    q->abort_request = 0;
    packet_queue_put_private(q, &flush_pkt);
    pthread_mutex_unlock(q->mutex);
}

/* return < 0 if aborted, 0 if no packet and > 0 if packet.  */
static int packet_queue_get(PacketQueue *q, AVPacket *pkt, int block ,int *serial){
    MyAVPacketList *pktl;
    int ret; 

    pthread_mutex_lock(q->mutex);
    for(;;){
        if(q->abort_request){
            ret = -1;
            break;
        }

        pktl = q->first_pkt;
        if(pktl){
            q->first_pkt = pktl->next;
            if(!q->first_pkt)
                q->last_pkt = NULL;
            q->nb_packets --;
            q->size -= pktl->pkt.duration;
            q->duration -= pktl->pkt.duration;
            *pkt = pktl->pkt;
            if(serial)
                *serial = pktl->serial;
            av_free(pktl);
            ret = 1;
            break;
        } else if(!block){
            ret = 0;
            break;
        } else{
            pthread_cond_wait(q->cond, q->mutex);
        }
    }

    pthread_mutex_unlock(q->mutex);
    return ret;
}

static double get_clock(Clock *c)
{
    if (*c->queue_serial != c->serial)
        return NAN;
    if (c->paused) {
        return c->pts;
    } else {
        double time = av_gettime_relative() / 1000000.0;
        return c->pts_drift + time - (time - c->last_updated) * (1.0 - c->speed);
    }
}

static void set_clock_at(Clock *c, double pts, int serial, double time)
{
    c->pts = pts;
    c->last_updated = time;
    c->pts_drift = c->pts - time;
    c->serial = serial;
}

static void set_clock(Clock *c, double pts, int serial)
{
    double time = av_gettime_relative() / 1000000.0;
    set_clock_at(c, pts, serial, time);
}

static void set_clock_speed(Clock *c, double speed)
{
    set_clock(c, get_clock(c), c->serial);
    c->speed = speed;
}

static void init_clock(Clock *c, int *queue_serial){
    c->speed = 1.0;
    c->paused = 0;
    c->queue_serial = queue_serial;
    set_clock(c, NAN, -1);
}

static void int frame_queue_init(FrameQueue *f, PacketQueue *q) {

      int i;

      memset(f, 0, sizeof(FrameQueue));
      if (!(pthread_mutex_init(f->mutex, NULL))) {
          LOGE(" pthread_mutex(): NULLs\n");
          return AVERROR(ENOMEM);
      }

      if (!(pthread_cond_init(f->cond, NULL))) {
          av_log(NULL, AV_LOG_FATAL, "SDL_CreateCond(): %s\n", SDL_GetError());
          return AVERROR(ENOMEM);
      }

       f->pktq = pktq;
       f->max_size = FFMIN(max_size, FRAME_QUEUE_SIZE);
       f->keep_last = !!keep_last;

       for (i = 0; i < f->max_size; i++)
           if (!(f->queue[i].frame = av_frame_alloc()))
               return AVERROR(ENOMEM);

       return 0;
}

static int frame_queue_nb_remaining(FrameQueue *f){
    return f->size - f->rindex_shown;
}

static void stream_seek(VideoState *is, int64_t pos, int64_t rel, int seek_by_bytes){
    if(!is->seek_req){
        is->seek_pos = pos;
        is->seek_rel = rel;
        is->seek_flags &= ~AVSEEK_FLAG_BYTE;

        if(seek_by_bytes)
            is->seek_flags |= AVSEEK_FLAG_BYTE;
        is->seek_req = 1;
        pthread_cond_signal(is->continue_read_thread);
    }
}

static void stream_toggle_pause(videoStream *is){
    if(is->paused){
        is->frame_timer += av_gettime_relative() / 1000000.0 - is->vidclk.last_updated;
        if(is->read_pause_return != AVERROR(ENOSYS)){
            is->vidclk.paused = 0;
        }
        set_clock(&is->extclk, get_clock(&is->extclk), is->extclk.serial);
    }

    set_clock(&is->extclk, get_clock(&is0>extclk), is->extclk.serial);
    is->paused = is->audclk.paused = is->vidclk.paused = is->extclk.pasued = !is->paused;
}

static void step_to_next_frame(VideoState *is){
    if(is->paused)
        stream_toggle_pause(is);
    is->step = 1;
}

static int stream_component_open(VideoState *is, int stream_index){

    AVFormatContext *ic = is->ic;
    AVCodecContext *avctx;
    AVCodec *codec;

    const char *forced_code_name = NULL;
    AVDictionary *opts = NULL;
    AVDictionaryEntry *t = NULL;
    int sample_rate, nb_channels;
    int64_t channel_layout;
    int ret = 0;
    int stream_lowers = lowers;

    if(stream_index < 0 || stream_index >= ic->nb_streams)
        return -1;

    avctx = avcodec_alloc_context3(NULL);
    if(!avctx)
        return AVERROR(ENOMEM);

    ret = avcodec_parameters_to_context(avctx, ic->streams[stream_index]->codecpar);
    if(ret<0);
        goto fail;

    avctx->pkt_timebase = ic->streams[stream_index]->time_base;

    codec = avcodec_find_decoder(avctx->codec_id);

    //switch(avctx->codec_type){
    //    case AVMEDIA_TYPE_AUDIO    : is->last_audio_stream = stream_index; forced_codec_name = audio_codec_name; break;
    //    case AVMEDIA_TYPE_SUBTITLE : is->last_subtitle_stream = stream_index; forced_codec_name = subtitle_codec_name; break;
    //    case AVMEDIA_TYPE_VIDEO    : is->last_video_stream = stream_index; forced_codec_name = video_codec_name; break;
    //}
    is->last_video_stream = stream_index;
    forced_codec_name = video_codec_name;

    if(forced_codec_name)
        codec = avcodec_find_decoder_by_name(forced_codec_name);
    if(!codec){
        LOGE("NO CODE ");
        return -1;
    }

    avctx->codec_id = codec->id;
    if(stream_lowres > codec->max_lowres){
        LOGE("The maximum value for lowres supported by the decoder is");
        stream_lowres = codec->max_lowres;
    }

    avctx->lowres = stream_lowres;

    if(fast)
        avctx->flags2 |= AV_CODEC_FLAG2_FAST;

    opts = filter_codec_opts(codec_opts, avctx->codec_id, ic, ic->streams[stream_index], codec);


    if(!av_dict_get(opts, "threads", NULL, 0))
        av_dict_set(&opts, "threads", "auto", 0);

    if(stream_lowres)
        av_dict_set_int(&opts, "lowres", stream_lowres, 0);
    if(avctx->codec_type == AVMEDIA_TYPE_VIDEO)
        av_dict_set(&opts, "refcounted_frames", "1", 0);

    if((ret = avcodec_open2(avctx, codec, &opts))<0){
        goto fail;
    }

    if((t = av_dict_get(opts, "", NULL, AV_DICT_IGNORE_SUFFIX))){
        LOGE("Option not found");
        ret = AVERROR_OPTION_NOT_FOUND;
        goto fail;
    }

    is->eof = 0;

    ic->stream[stream_index]->discard = AVDISCARD_DEFAULT;





}

/* this thread gets the stream from the disk or the network */
static int read_thread(void *arg){
    
    VideoState *is = arg;
    AVFormatContext *ic = NULL;
    int err, i, ret;
    int st_index[AVMEDIA_TYPE_NB];
    AVPacket pkt1, *pkt = &pkt1;
    int64_t stream_start_time;
    int pkt_in_play_range = 0;
    AVDictionaryEntry *t;
    pthread_mutex_t *wait_mutex;
    pthread_mutex_init(wait_mutex);
    int scan_all_pmts_set;
    int64_t pkt_ts;

    if(!wait_mutex){
        LOGE("pthread_mutex_init ERROR on read_thread()");
        ret = AVERROR(ENOMEM);
        goto fail;
    }

    memset(st_index, -1, sizeof(st_index));
    is->last_video_stream = is->video_stream = -1;
    is->last_audio_stream = is->audio_stream = -1;
    is->last_subtitle_stream = is->subtitle_stream = -1;
    is->eof = 0;

    ic = avformat_alloc_context();


    if(!ic){
        LOGE("avformat_alloc_context error");
        goto fail;
    }

    ic->interrupt_callback.callback = decode_interrupt_cb;
    ic->interrupt_callback.opaque = is;

    if(!av_dict_get(format_opts, "scan_all_ptms", NULL, AV_DICT_MATCH_CASE)){
        av_dict_set(&format_opts, "scan_all_ptms", "1", AV_DICT_DONT_OVERWRITE);
        scan_all_ptms = 1;
    }

    err =  avformat_open_input(&ic, is->filename, is->iformat, &format_opts);

    if(err<0){
        LOGE("Cannot open file on decode thread");
        ret = -1;
        goto fail;
    }

    if(scan_all_ptms){
        av_dict_set(&format_opts, "scan_all_ptms", NULL, AV_DICT_MATCH_CASE);
    }

    if((t = av_dict_get(format_opts, "", NULL, AV_DICT_IGNORE_SUFFIX))){
        LOGE("Option not found");
        ret = AVERROR_OPTION_NOT_FOUND;
        goto fail;
    }

    is->ic = ic;


    av_format_inject_global_side_data(ic);

    if(find_stream_info){
        AVDictionary **opts = setup_find_stream_info_opts(ic, codec_opts);
        int orig_nb_streams = ic->nb_streams;

        err = avformat_find_stream_info(ic, opts);

        for(i = 0; i<orig_nb_streams; i++)
            av_dict_free(&opts[i]);
        av_freep(&opts);

        if(err < 0){
            LOGE("Could not find codec parameters\n");
            ret = -1; 
            goto fail;
        }
    }


    if(ic->pb)
        ic->pb->eof_reached = 0;

    if(seek_by_bytes < 0)
        seek_by_bytes = !!(ic->iformat->flags&AVFMT_TS_DISCONT) && strcmp("ogg", ic->iformat->name);

    is->max_frame_duration = (ic->iformat->flags & AVFMT_TS_DISCONT) ? 10.0 : 3600.0;
    if (!window_title && (t = av_dict_get(ic->metadata, "title", NULL, 0)))
        window_title = av_asprintf("%s - %s", t->value, input_filename);


    //seeking request
    if(start_time != AV_NOPTS_VALUE){
        int64_t timestamp;

        timestamp = start_time;

        if(ic->start_time != AV_NOPTS_VALUE)
            timestamp += ic->start_time;

        if(avformat_seek_file(ic, -1, INT64_MIN, timestamp, INT64_MAX, 0)<0){
            LOGE("could not seek to position :");
        }
    }

    is->realtime = is_realtime(ic);

    if(show_status)
        av_dump_format(ic, 0, is->filename, 0);

    for(i=0; i<ic->nb_streams; i++){
        AVStream *st = ic->streams[i];
        enum AVMediaType type = st->codecpar->codec_type;
        st->discard = AVDISCARD_ALL;
        if(type >= 0 && wanted_stream_spec[type] && st_index[type] == -1)
            if(avformat_match_stream_specifier(ic, st, wanted_stream_spec[type]) > 0)
                st_index[type] = i;
    }

    for(i=0; i<AVMEDIA_TYPE_NB; i++){
        if(wanted_stream_spec[i] && st_index[i] == -1){
            LOGE("Stream specifier does not match any stream");
            st_index[i] = INT_MAX;
        }
    }

    if(!video_disable)
        st_index[AVMEDIA_TYPE_VIDEO] = 
            av_find_best_stream(ic, AVMEDIA_TYPE_VIDEO, 
                                st_index[AVMEDIA_TYPE_VIDEO], -1, 
                                NULL, 0);

    if(!audio_disalbe)
        st_index[AVMEDIA_TYPE_AUDIO] = 
            av_find_best_stream(ic, AVMEDIA_TYPE_AUDIO,
                                st_index[AVMEDIA_TYPE_AUDIO],
                                st_index[AVMEDIA_TYPE_VIDEO],
                                NULL, 0);

    if(!video_disable && !subtitle_disable)
        st_index[AVMEDIA_TYPE_SUBTITLE] = 
            av_find_best_stream(ic, AVMEDIA_TYPE_SUBTITLE, 
                                st_index[AVMEDIA_TYPE_SUBTITLE],
                                (st_index[AVMEDIA_TYPE_AUDIO]>=0 ?
                                    st_index[AVMEDIA_TYPE_AUDIO] : 
                                    st_index[AVMEDIA_TYPE_VIDEO]),
                                NULL, 0);
    is->show_mode = show_mode;

    if(st_index[AVMEDIA_TYPE_VIDEO] >= 0){
        AVStream *st = ic->streams[st_index[AVMEDIA_TYPE_VIDEO]];
        AVCodecParameters *codecpar = st->codecpar;
        AVRational sar = av_guess_sample_aspect_ratio(ic, st, NULL);
        if(codecpar->width){
            //HAVE_TO
            set_default_window_size(codecpar->width, codecpar->height, sar);
        }
    }


    /* open the streams */
   
    if(st_index[AVMEDIA_TYPE_AUDIO] >=0 ){

        //HAVE_TO
        stream_component_open(is, st_index[AVMEDIA_TYPE_AUDIO]);
    }

    ret = -1;
    if(st_index[AVMEDIA_TYPE_VIDEO] >= 0){
        ret = stream_component_open(is, st_index[AVMEDIA_TYPE_VIDEO]);
    }

    if(is->show_mode == SHOW_MODE_NONE)
        is->show_mode = ret >= 0? SHOW_MODE_VIDEO : SHOW_MODE_RDFT;
    if(st_index[AVMEDIA_TYPE_SUBTITLE] >= 0){
        stream_component_open(is, st_index[AVMEDIA_TYPE_SUBTITLE]);
    }


    if(is->video_stream<0 && is->audio_stream <0){
        LOGE("Failed to open file or configure filtergraph\n");
        ret = -1;
        goto fail;
    }

    if (infinite_buffer < 0 && is->realtime)
        infinite_buffer = 1;

    //main decode loop
    for(;;){
        if(is->abort_request)
            break;
        if(is->paused != is->last_paused){
            is->last_paused = is->paused;
            if(is->paused)
                is->read_pause_return = av_read_pause(ic);
            else
                av_read_play(ic);
        }

#if CONFIG_RTSP_DEMUXER || CONFIG_MMSH_PROTOCOL
        if (is->paused &&
                (!strcmp(ic->iformat->name, "rtsp") ||
                 (ic->pb && !strncmp(input_filename, "mmsh:", 5)))) {
            /* wait 10 ms to avoid trying to get another packet */
            /* XXX: horrible */
            sleep(10);
            continue;
        }
#endif
        // seek stuff goes here
        if(is->seek_req){

            int stream_index = -1;
            int64_t seek_target = is->seek_pos;
            int64_t seek_min    = is->seek_rel > 0 ? seek_target - is->seek_rel + 2: INT64_MIN;
            int64_t seek_max    = is->seek_rel < 0 ? seek_target - is->seek_rel - 2: INT64_MAX;

            if(avformat_seek_file(is->ic, -1, seek_min, seek_target, seek_max, is->seek_flags)<0){
                LOGE("error while seeking");
            } else{

                if(is->audio_stream >=0){
                    packet_queue_flush(&is->audioq);
                    packet_queue_put(&is->audioq, &flush_pkt);
                }
                if(is->subtitle_stream>=0){
                    packet_queue_flush(&is->subtitleq);
                    packet_queue_put(&is->subtitleq, &flush_pkt);
                }
                if(is->video_stream >= 0){
                    packet_queue_flush(&is->videoq);
                    pakcet_queue_put(&is->videoq, &flush_pkt);
                }
                if(is->seek_flags & AVSEEK_FLAG_BYTE){
                    set_clock(&is->extclk, NAN, 0);
                } else{
                    set_clock(&is->extclk, seek_target / (double)AV_TIME_BASE, 0);
                }


            }

            is->seek_req = 0;
            is->queue_attachments_req = 1;
            is->eof=0;

            if(is->paused){
                step_to_next_frame(is);
            }
        }

        if(is->queue_attachments_req){
            if(is->video_st && is->video_st->disposition & AV_DISPOSITION_ATTACHED_PIC){
                AVPacket copy = {0};
                if((ret = av_packet_ref(&copy, &is->video_st->attached_pic))<0)
                    goto fail;

                packet_queue_put(&is->videoq, &copy);
                packet_queue_put_nullpacket(&is->videoq, is->video_stream);
            }

            is->queue_attachments_req = 0;
        }

         /* if the queue are full, no need to read more */
        if(infinite_buffer<1 &&
            (is->audioq.size + is->videoq.size + is->subtitleq.size > MAX_VIDEOQ_SIZE
        || (stream_has_enough_packets(is->video_st, is->video_stream, &is->videoq) &&
            stream_has_enough_packets(is->audio_st, is->audio_stream, &is->audioq) &&
            stream_has_enough_packets(is->subtitle_st, is->subtitle_stream, &is->subtitleq)))){
        
            /*wait 10 ms */
            pthread_mutex_lock(wait_mutex);
            pthread_cond_timedwait(is->continue_read_thread, wait_mutex, 10);
            pthread_mutex_unlock(wait_mutex);
            continue;
        }

        if(!is->paused &&
        !is->video_st || (is->viddec.finished == is->videoq.serial && frame_queue_nb_remaining(&is->pictq) == 0)){
           if(loop != 1 && (!loop || --loop)){
               stream_seek(is, start_time != AV_NOPTS_VALUE ? start_time : 0, 0,0);
            } else if(autoexit){
               ret = AVERROR_EOF;
               goto fail;
            }
        }

        ret = av_read_frame(ic, pkt);
        
        if(ret < 0){
            if((ret==AVERROR_EOF || avio_feof(ic->pb)) && !is->eof){
                if(is->video_stream >= 0)
                    packet_queue_put_nullpacket(&is->videoq, is->video_stream);
                if(is->audio_stream >= 0)
                    packet_queue_put_nullpacket(&is->audioq, is->audio_stream);
                if(is->subtitle_stream >= 0)
                    packet_queue_put_nullpacket(&is->subtitleq, is->subtitle_stream);
                is->eof = 1;
            }

            if(ic->pb && ic->pb->error)
                break;
            pthread_mutex_lock(wait_mutex);
            pthread_cond_timedwait(is->continue_read_thread, wait_mutex, 10);
            pthread_mutex_unlock(wait_mutex);
            continue;
        } else{
            is->eof = 0;
        }

        stream_start_time = ic->streams[pkt->stream_index]->start_time;
        pkt_ts = pkt->pts == AV_NOPTS_VALUE ? pkt->dts : pkt->pts;
        pkt_in_play_range = duration == AV_NOPTS_VALUE ||
               (pkt_ts - (stream_start_time != AV_NOPTS_VALUE ? stream_start_time : 0)) *
               av_q2d(ic->streams[pkt->stream_index]->time_base) -
               (double)(start_time != AV_NOPTS_VALUE ? start_time : 0) / 1000000
               <= ((double)duration / 1000000);
        if(pkt->stream_index == is->audio_stream && pkt_in_play_range){
            packet_queue_put(&is->audioq, pkt);
        } else if(pkt->stream_index == is->video_stream && pkt_in_play_range
            && !(is->video_st->disposition & AV_DISPOSITION_ATTACHED_PIC)){
            packet_queue_put(&is->videoq, pkt);
        } else if(pkt->stream_index == is->subtitle_stream && pkt_in_play_range){
            packet_queue_put(&is->subtitleq, pkt);
        } else{
            av_packet_unref(pkt);
        }
    }
    ret = 0;
    //I have to do packet_queue_put, null put, some other methods.
fail :
    if(ic && !is->ic)
        avformat_close_input(&ic);
    if(ret!=0){
        //SDL_EVENT event;
        //event.type = FF_QUIT_EVENT;
        //event.user.data1=is;
        //SDL_PushEvent(&event);
    }
    pthread_mutex_destroy(wait_mutex);
    return 0;
}


static void stream_close(VideoState *is){
    is->abort_request = 1;
    pthread_join(is->read_tid, NULL);

    if(is->audio_stream >=0){
        stream_component_close(is, is->audio_stream);
    }
    if(is->video_stream >=0){
        stream_component_close(is, is->video_stream);
    }
    if(is->subtitle_stream>=0){
        stream_component_close(is, is->subtitle_stream);
    }

    avformat_close_input(&is->ic);

    packet_queue_destroy(&is->videoq);
    packet_queue_destroy(&is->audioq);
    packet_queue_destroy(&is->subtitleq);

    frame_queue_destroy(&is->pictq);
    frame_queue_destroy(&is->sampq);
    frame_queue_destroy(&is->subpq);
    pthread_cond_destroy(is->continue_read_thread);
    sws_freeContext(is->img_convert_ctx);
    sws_freeContext(is->sub_convert_ctx);
    av_free(is->filename);

    av_free(is);
}

static VideoState *stream_open(const char *filename) {
    VideoState *is;

    is = av_mallocz(sizeof(VideoState));

    if (!is)
        return NULL;

    is->filename = av_strdup(filename);
    if(!is->filename)
        goto fail;
    is->iformat = iformat;
    is->ytop    = 0;
    is->xleft   = 0;


    //start video display
    if(frame_queue_init(&is->pictq, &is->videoq, VIDEO_PICTURE_QUEUE_SIZE, 1)<0){
        goto fail;
    }
    if (frame_queue_init(&is->subpq, &is->subtitleq, SUBPICTURE_QUEUE_SIZE, 0) < 0)
        goto fail;
    if (frame_queue_init(&is->sampq, &is->audioq, SAMPLE_QUEUE_SIZE, 1) < 0)
        goto fail;

    if (packet_queue_init(&is->videoq) < 0 ||
        packet_queue_init(&is->audioq) < 0 ||
        packet_queue_init(&is->subtitleq) < 0)
        goto fail;

    if(!pthread_cond_init(is->continue_read_thread, NULL)){
        LOGE("Init condition error");
        goto fail;
    }

    init_clock(&is->vidclk, &is->videoq.serial);
    init_clock(&is->audclk, &is->audioq.serail);
    init_clock(&is->subtitleq, &is->subtitleq.serial);

    is->audio_clock_serial = -1;

    if (startup_volume < 0)
        LOGE("-volume < 0, setting to 0\n");
    if (startup_volume > 100)
        LOGE("-volume > 100, setting to 100\n");

    startup_volume = av_clip(startup_volume, 0, 100);
    is->audio_volume = startup_volume;
    is->muted = 0;
    is->av_sync_type = av_sync_type;

    //startup_volume = av_clip(SDL_MIX_MAXVOLUME * startup_volume / 100, 0, SDL_MIX_MAXVOLUME);

   int create_thread = pthread_create(is->read_tid, NULL, read_thread, is);

   if(!create_thread){
    LOGE("stream_open, create thread error");

fail : 
    stream_close(is);
    return NULL;
   }

   return is;
}



JNIEXPORT void JNICALL Java_wongeun_com_androidwithffplay_NDKAdapter_setDataSource(JNIEnv *env, jclass clazz, jstring _uri){
    uri = (*env)->GetStringUTFChars(env, _uri, NULL);
}

JNIEXPORT jint JNICALL Java_wongeun_com_androidwithffplay_NDKAdapter_play(JNIEnv *env, jclass clazz, jobject surface){
    LOGD("play");



    if(surface == NULL){
        return -1;
    }

    const char * file_name = uri;

    if(file_name == NULL){
        LOGE("please set the DataSource");
        return -1;
    }

    int flags;
    VideoState* is;

    init_dynload();


    av_register_all();
    avformat_network_init();

    init_opts();

    signal(SIGINT, sigterm_handler);
    signal(SIGTERM, sigterm_handler);

    if(!file_name){
        show_usage();
        LOGE("An input file must be speicified");
        exit(1);
    }

    if(display_disable){
        video_disable = 1;
    }

    av_init_packet(&flush_pkt);
    flush_pkt.data = (uint8_t *)&flush_pkt;


    if(!display_disable){
        window = ANativeWindow_fromSurface(env, surface);
        ANativeWindow_setBuffersGeometry(nativeWindow, default_width, default_height, WINDOW_FORMAT_RGBA_8888);


        if (!window || !renderer || !renderer_info.num_texture_formats) {
            LOGE("Failed to create window or renderer: %s");
            do_exit(NULL);
        }


    }

    is = stream_open(file_name, file_iformat);
    if(!is){
        LOGE("Fail to initialize VideoState!\n");
        do_eixt(NULL);
    }

    event_loop(is);


//    AVFormatContext * pFormatCtx = avformat_alloc_context();

    //if(avformat_open_input(&pFormatCtx, file_name, NULL, NULL)!=0){
      //  LOGE("Couldn't open file : %s\n", file_name);
        //return -1;
    //}

    //if(avformat_find_stream_info(pFormatCtx, NULL)<0){
      //  LOGE("Couldn't find stream information.");
        //return -1;
    //}

    int videoStream = -1, i;

    for ( i = 0; i < pFormatCtx->nb_streams; i++){
        if(pFormatCtx->streams[i]->codec->codec_type == AVMEDIA_TYPE_VIDEO && videoStream<0){
            videoStream = i;
        }
    }

    if(videoStream==-1){
        LOGE("Didn't find a video stream.");
        return -1;
    }

    AVCodecContext *pCodecCtx = pFormatCtx->streams[videoStream]->codec;

    AVCodec * pCodec = avcodec_find_decoder(pCodecCtx->codec_id);

    if(pCodec == NULL){
        LOGE("Codec not found.");
        return -1;
    }

    if(avcodec_open2(pCodecCtx, pCodec, NULL) < 0){
        LOGE("could not open codec.");
        return -1;
    }

    ANativeWindow* nativeWindow = ANativeWindow_fromSurface(env, surface);

    int videoWidth = pCodecCtx -> width;
    int videoHeight = pCodecCtx -> height;

    ANativeWindow_setBuffersGeometry(nativeWindow, videoWidth, videoHeight, WINDOW_FORMAT_RGBA_8888);
    ANativeWindow_Buffer windowBuffer;


    if(avcodec_open2(pCodecCtx, pCodec, NULL)<0){
        LOGE("could not open codec2.");
        return -1;
    }


    AVFrame* pFrame = av_frame_alloc();

    AVFrame* pFrameRGBA = av_frame_alloc();

    if(pFrameRGBA == NULL || pFrame == NULL){
        LOGE("Could not allocate video frame. ");
        return -1;
    }


    int numBytes = av_image_get_buffer_size(AV_PIX_FMT_RGBA, pCodecCtx->width, pCodecCtx->height, 1);

    uint8_t * buffer = (uint8_t *)av_malloc(numBytes*sizeof(uint8_t));

    av_image_fill_arrays(pFrameRGBA->data, pFrameRGBA->linesize, buffer, AV_PIX_FMT_RGBA,
                        pCodecCtx -> width, pCodecCtx->height, 1);

    struct SwsContext *sws_ctx = sws_getContext(pCodecCtx -> width,
                                            pCodecCtx -> height,
                                            pCodecCtx -> pix_fmt,
                                            pCodecCtx -> width,
                                            pCodecCtx -> height,
                                            AV_PIX_FMT_RGBA,
                                            SWS_BILINEAR,
                                            NULL,
                                            NULL,
                                            NULL);

    int frameFinished;
    AVPacket packet;

    isPlaying = 1;

    while(av_read_frame(pFormatCtx, &packet)>=0 && isPlaying){
    // Is this a packet from the video stream?
        if(packet.stream_index == videoStream){

            avcodec_decode_video2(pCodecCtx, pFrame, &frameFinished, &packet);

            if(frameFinished){
                ANativeWindow_lock(nativeWindow, &windowBuffer, 0);

                sws_scale(sws_ctx, (uint8_t const* const*) pFrame -> data,
                            pFrame->linesize, 0, pCodecCtx->height,
                            pFrameRGBA->data, pFrameRGBA->linesize);

                uint8_t* dst = windowBuffer.bits;
                int  dstStride = windowBuffer.stride * 4;
                uint8_t* src = (uint8_t*)(pFrameRGBA->data[0]);
                int srcStride = pFrameRGBA->linesize[0];

                int h;

                for(h=0; h<videoHeight; h++){
                    memcpy(dst + h * dstStride, src + h *srcStride, srcStride);
                }

                ANativeWindow_unlockAndPost(nativeWindow);
            }
        }

        av_packet_unref(&packet);
    }


    av_free(buffer);
    av_free(pFrameRGBA);

    av_free(pFrame);
    avcodec_close(pCodecCtx);

    avformat_close_input(&pFormatCtx);

    return 0;
}


