#include <stdio.h>
#include <stdlib.h>
#include <inttypes.h>
#include <unistd.h> 
#include <libavformat/avformat.h>
#include <sys/mman.h>
#include <fcntl.h>
#include <libavcodec/avcodec.h>
#include <libswscale/swscale.h>
#include <time.h>
#include <pthread.h>
#include <libavutil/opt.h>
# include "base64.h"
# include "decode_video2.h"

pthread_cond_t buffer_full = PTHREAD_COND_INITIALIZER;
pthread_cond_t buffer_empty = PTHREAD_COND_INITIALIZER;
pthread_mutex_t mutex = PTHREAD_MUTEX_INITIALIZER;

pthread_mutex_t wait_mutex = PTHREAD_MUTEX_INITIALIZER;
pthread_cond_t wait_cond = PTHREAD_COND_INITIALIZER;

typedef struct SwsContextArgs{

    int srcWidth;
    int srcHeight;
    enum AVPixelFormat srcFormat;
    int dstWidth;
    int dstHeight;

}SwsContextArgs;

typedef struct WriterArgs{

    int *decoded_cnt;
    int *rendered_cnt;
    uint8_t occupiedIndices;
    AVFrame *rgb_frames[8];
}WriterArgs;

typedef struct ScaleArgs{

    int nextFrame;
    WriterArgs *wargs;
    SwsContextArgs sws_args;
    uint8_t buffer_state; // 0 -> empty, 1 -> filled
    AVFrame *frame_buffer[8];

}ScaleArgs;

int* createModeVariable(){

    int *mode = malloc(sizeof(int));

    *mode = 0;

    return mode;
}

int *deleteModeVariable(int *mode){

    free(mode);
}

void increment(int *val){

    __atomic_add_fetch(val, 1, __ATOMIC_RELEASE);
}

void incrementRenderCnt(int *val){
    
    __atomic_add_fetch(val, 1, __ATOMIC_RELEASE);
    pthread_cond_signal(&wait_cond);
}

void writeFrame(AVFrame *rgb_frame, int *decoded_cnt, int *rendered_cnt){

    char path[16];

    snprintf( path, 16, "/frame%d", *decoded_cnt % 50);

    int size = rgb_frame->height * rgb_frame->width * 3;       

    int shm_fd = shm_open(path, O_CREAT | O_RDWR, 0666);

    if (shm_fd == -1) {
        fprintf(stdout, "shm_open failed\n");
        return;
    }

    ftruncate(shm_fd, size);

    void *ptr = mmap(0, size, PROT_READ | PROT_WRITE, MAP_SHARED, shm_fd, 0);

    if (ptr == MAP_FAILED) {
        fprintf(stdout, "shm_open failed\n");
        return;
    }

    uint8_t *shm_dest_ptr = (uint8_t *)ptr; 
    size_t row_bytes = rgb_frame->width * 3;


    while (true) {

        int decoded = __atomic_load_n(decoded_cnt, __ATOMIC_ACQUIRE);
        int rendered = __atomic_load_n(rendered_cnt, __ATOMIC_ACQUIRE);

        if (decoded - rendered < 40) {
            break;
        }

        usleep(1000);
    }

    for (int y = 0; y < rgb_frame->height; y++) {

        uint8_t *src_ptr = rgb_frame->data[0] + y * rgb_frame->linesize[0];
        memcpy(shm_dest_ptr, src_ptr, row_bytes);

        shm_dest_ptr += row_bytes;
    }

    munmap(ptr, rgb_frame->height * rgb_frame->width * 3);
    close(shm_fd);
    increment(decoded_cnt);
}

void *writerThread(void *args){
    
    WriterArgs *wargs = (WriterArgs*) args;

    int index;

    for (;;){

        index = __atomic_load_n(wargs->decoded_cnt, __ATOMIC_ACQUIRE) % 8;

        while((__atomic_load_n(&wargs->occupiedIndices, __ATOMIC_ACQUIRE) & (1 << index)) == 0){

            usleep(100);
        }

        writeFrame(wargs->rgb_frames[index], wargs->decoded_cnt, wargs->rendered_cnt);
        // __atomic_and_fetch(&wargs->occupiedIndices, ~(1 << index), __ATOMIC_RELEASE);
        wargs->occupiedIndices &= ~(1 << index);
    }
}

void *scalingThread(void *args){

    ScaleArgs *sargs = (ScaleArgs*) args;

    struct SwsContext *sws_ctx = sws_getContext( sargs->sws_args.srcWidth, sargs->sws_args.srcHeight, sargs->sws_args.srcFormat, sargs->sws_args.dstWidth, sargs->sws_args.dstHeight, AV_PIX_FMT_RGB24, SWS_BILINEAR, NULL, NULL, NULL);

    int ret;
    int index;

    for (;;){

        index = __atomic_fetch_add(&(sargs->nextFrame), 1, __ATOMIC_RELEASE);

        index %= 8;

        while((__atomic_load_n(&sargs->buffer_state, __ATOMIC_ACQUIRE) & (1 << index)) == 0){

            usleep(100);
        }

        while((__atomic_load_n(&sargs->wargs->occupiedIndices, __ATOMIC_ACQUIRE) & (1  << index)) != 0){

            usleep(100);
        }

        ret = sws_scale(sws_ctx, (const uint8_t * const *)sargs->frame_buffer[index]->data, sargs->frame_buffer[index]->linesize, 0, sargs->frame_buffer[index]->height, sargs->wargs->rgb_frames[index]->data, sargs->wargs->rgb_frames[index]->linesize);

        if (ret < 0) {
            fprintf(stdout, "Error converting frame to rgb\n");
            return NULL;
        }

        __atomic_and_fetch(&sargs->buffer_state, ~(1 << index) , __ATOMIC_RELEASE);

        __atomic_or_fetch(&sargs->wargs->occupiedIndices, (1 << index), __ATOMIC_RELEASE);
        // sargs->wargs->occupiedIndices |= (1 << index);
    }

    sws_freeContext(sws_ctx);
}

void frameLoop(char *path, int *rendered_cnt, int *decoded_cnt, int terminal_width, int terminal_height){

    const char *filename = path;

    AVFormatContext *fmt_ctx = NULL;
    AVCodecContext *dec_ctx = NULL;
    const AVCodec *decoder = NULL;
    AVStream *stream = NULL;

    AVPacket *packet = NULL;

    pthread_t thread;
    pthread_t thread1;
    pthread_t thread2;
    pthread_t thread3;

    int ret;
    int video_stream = -1;

    int decodedFrames = 0;

    WriterArgs wargs = {0};
    ScaleArgs sargs = {0};

    sargs.wargs = &wargs;

    wargs.decoded_cnt = decoded_cnt;
    wargs.rendered_cnt = rendered_cnt;


    for (int i = 0; i < 8; i++){

        sargs.frame_buffer[i] = av_frame_alloc();

        wargs.rgb_frames[i] = av_frame_alloc();
        wargs.rgb_frames[i]->format = AV_PIX_FMT_RGB24;
        wargs.rgb_frames[i]->width  = terminal_width;
        wargs.rgb_frames[i]->height = terminal_height;

        ret = av_frame_get_buffer(wargs.rgb_frames[i], 32);

        if (ret < 0){
            fprintf(stderr, "Error allocating frame buffer for rgb\n");
        }
    }


    ret = avformat_open_input(&fmt_ctx, filename, NULL, NULL);

    if (ret < 0) {
        fprintf(stderr, "Could not open input file\n");
        return ;
    }

    ret = avformat_find_stream_info(fmt_ctx, NULL);

    if (ret < 0) {
        fprintf(stderr, "Could not find stream information\n");
        avformat_close_input(&fmt_ctx);
        return ;
    }

    for (unsigned int i = 0; i < fmt_ctx->nb_streams; i++) {

        if (fmt_ctx->streams[i]->codecpar->codec_type == AVMEDIA_TYPE_VIDEO) {
            stream = fmt_ctx->streams[i];
            video_stream = i;
            break;
        }
    }

    if (!stream) {
        fprintf(stderr, "No video stream found\n");
        avformat_close_input(&fmt_ctx);
        return ;
    }

    decoder = avcodec_find_decoder(stream->codecpar->codec_id);

    if (!decoder) {
        fprintf(stderr, "Could not find decoder\n");
        avformat_close_input(&fmt_ctx);
        return ;
    }

    dec_ctx = avcodec_alloc_context3(decoder);

    if (!dec_ctx) {
        fprintf(stderr, "Could not allocate decoder context\n");
        avformat_close_input(&fmt_ctx);
        return ;
    }

    ret = avcodec_parameters_to_context( dec_ctx, stream->codecpar);

    if (ret < 0) {
        fprintf(stderr, "Could not copy codec parameters\n");
        avcodec_free_context(&dec_ctx);
        avformat_close_input(&fmt_ctx);
        return ;
    }

    ret = avcodec_open2(dec_ctx, decoder, NULL);

    if (ret < 0) {
        fprintf(stderr, "Could not open decoder\n");
        avcodec_free_context(&dec_ctx);
        avformat_close_input(&fmt_ctx);
        return ;
    }

    packet = av_packet_alloc();

    if (!packet) {
        fprintf(stderr, "Could not allocate packet/frame\n");

        av_packet_free(&packet);
        avcodec_free_context(&dec_ctx);
        avformat_close_input(&fmt_ctx);

        return ;
    }

    sargs.sws_args = (SwsContextArgs){stream->codecpar->width, stream->codecpar->height, stream->codecpar->format, terminal_width, terminal_height};

    pthread_create(&thread, NULL, writerThread, &wargs);
    pthread_create(&thread1, NULL, scalingThread, &sargs);
    pthread_create(&thread2, NULL, scalingThread, &sargs);
    pthread_create(&thread3, NULL, scalingThread, &sargs);

    while ((ret = av_read_frame(fmt_ctx, packet)) >= 0) {

        if (packet->stream_index != video_stream) {
            av_packet_unref(packet);
            continue;
        }

        ret = avcodec_send_packet(dec_ctx, packet);

        if (ret < 0) {
            fprintf(stderr, "Error sending packet to decoder\n");
            break;
        }

        // decoding start

        while (1) {

            while((__atomic_load_n(&sargs.buffer_state, __ATOMIC_ACQUIRE) & (1 << (decodedFrames % 8))) != 0){

                usleep(100);
            }

            // ret = avcodec_receive_frame(dec_ctx, sargs.frame_buffer[__atomic_load_n(&sargs.decodedFrames, __ATOMIC_ACQUIRE) % 8]);
            ret = avcodec_receive_frame(dec_ctx, sargs.frame_buffer[decodedFrames % 8]);

            if (ret == AVERROR(EAGAIN) || ret == AVERROR_EOF) {
                break;
            }

            if (ret < 0) {
                fprintf(stdout, "Error decoding frame\n");
                return;
            }

            __atomic_fetch_or(&sargs.buffer_state, (1 << (decodedFrames % 8)), __ATOMIC_RELEASE);
            decodedFrames++;
        }
        
        // decoding end
        
        av_packet_unref(packet);
    }

    avcodec_send_packet(dec_ctx, NULL);

    pthread_join(thread, NULL);
    pthread_join(thread1, NULL);
    pthread_join(thread2, NULL);
    pthread_join(thread3, NULL);

    // decode(dec_ctx, frame, sws_ctx, rgb_frame, rendered_cnt, decoded_cnt);

    // Free resources

    for (int i = 0; i < 8; i++){

        av_frame_free(&sargs.frame_buffer[i]);
        av_frame_free(&wargs.rgb_frames[i]);
    }

    av_packet_free(&packet);
    avcodec_free_context(&dec_ctx);
    avformat_close_input(&fmt_ctx);
}
