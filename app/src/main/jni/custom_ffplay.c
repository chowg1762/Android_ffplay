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

#define LOGV(...) __android_log_print(ANDROID_LOG_VERBOSE, "libnav", __VA_ARGS__)
#define LOGD(...) __android_log_print(ANDROID_LOG_DEBUG, "libnav", __VA_ARGS__)
#define LOGI(...) __android_log_print(ANDROID_LOG_INFO, "libnav", __VA_ARGS__)
#define LOGW(...) __android_log_print(ANDROID_LOG_WARN, "libnav", __VA_ARGS__)
#define LOGE(...) __android_log_print(ANDROID_LOG_ERROR, "libnav", __VA_ARGS__)

char *uri;
int isPlaying = 0;

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

    av_register_all();

    AVFormatContext * pFormatCtx = avformat_alloc_context();

    if(avformat_open_input(&pFormatCtx, file_name, NULL, NULL)!=0){
        LOGE("Couldn't open file : %s\n", file_name);
        return -1;
    }

    if(avformat_find_stream_info(pFormatCtx, NULL)<0){
        LOGE("Couldn't find stream information.");
        return -1;
    }

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

JNIEXPORT void JNICALL Java_wongeun_com_androidwithffplay_NDKAdapter_setPlayingState(JNIEnv *env, jclass clazz, jint playing){
    isPlaying = playing;
}