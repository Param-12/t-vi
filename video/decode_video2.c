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

# include "base64.h"
# include "decode_video2.h"

pthread_cond_t buffer_full = PTHREAD_COND_INITIALIZER;
pthread_cond_t buffer_empty = PTHREAD_COND_INITIALIZER;
pthread_mutex_t mutex = PTHREAD_MUTEX_INITIALIZER;

pthread_mutex_t wait_mutex = PTHREAD_MUTEX_INITIALIZER;
pthread_cond_t wait_cond = PTHREAD_COND_INITIALIZER;

typedef struct RgbDataBuffer{

    int consumer_ptr;
    int producer_ptr;
    AVFrame* buffer[50];

}RgbDataBuffer;

typedef struct WriteArgs{

    int *decoded_cnt;
    int *rendered_cnt;
    int height;
    int width;
    RgbDataBuffer frame_buffer;
}WriteArgs;

void putFrame(AVFrame *frame, RgbDataBuffer *data){


    while(data->buffer[data->producer_ptr] != NULL){

        usleep(100);
    }

    data->buffer[data->producer_ptr] = frame;
    data->producer_ptr = (data->producer_ptr + 1) % 50;
}

void *writeLoop(void *args){

    WriteArgs *wargs = args;

    void *ptr;
    uint8_t *shm_dest_ptr;
    char path[16];
    int shm_fd;

    int size = wargs->height * wargs->width * 3;
    
    size_t row_bytes = wargs->width * 3;

    while(true){

        snprintf( path, 16, "/frame%d", *wargs->decoded_cnt % 50);

        shm_fd = shm_open(path, O_CREAT | O_RDWR, 0666);

        if (shm_fd == -1) {
            fprintf(stdout, "shm_open failed\n");
            return ptr;
        }

        ftruncate(shm_fd, size);

        ptr = mmap(0, size, PROT_READ | PROT_WRITE, MAP_SHARED, shm_fd, 0);

        if (ptr == MAP_FAILED) {
            fprintf(stdout, "shm_open failed\n");
            return ptr;
        }

        shm_dest_ptr = (uint8_t *)ptr; 

        pthread_mutex_lock(&wait_mutex);
        while (wargs->frame_buffer.buffer[wargs->frame_buffer.consumer_ptr] == NULL){

            pthread_cond_wait(&wait_cond, &wait_mutex);
        }

        pthread_mutex_unlock(&wait_mutex);

        AVFrame* rgb_frame = wargs->frame_buffer.buffer[wargs->frame_buffer.consumer_ptr];

        while (true) {

            int decoded = __atomic_load_n(wargs->decoded_cnt, __ATOMIC_ACQUIRE);
            int rendered = __atomic_load_n(wargs->rendered_cnt, __ATOMIC_ACQUIRE);

            if (decoded - rendered < 40) {
                break;
            }
        }

        for (int y = 0; y < rgb_frame->height; y++) {

            uint8_t *src_ptr = rgb_frame->data[0] + y * rgb_frame->linesize[0];
            memcpy(shm_dest_ptr, src_ptr, row_bytes);

            shm_dest_ptr += row_bytes;
        }

        munmap(ptr, rgb_frame->height * rgb_frame->width * 3);
        // free(rgb_frame);
        wargs->frame_buffer.buffer[wargs->frame_buffer.consumer_ptr] = NULL;
        wargs->frame_buffer.consumer_ptr = (wargs->frame_buffer.consumer_ptr + 1) % 50;
        close(shm_fd);
        increment(wargs->decoded_cnt);
    }
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

        usleep(500000);
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

void decode(AVCodecContext *dec_ctx, AVFrame *frame, struct SwsContext *sws_ctx, AVFrame *rgb_frame , int *rendered_cnt, int *decoded_cnt, WriteArgs *wargs){

    int ret;

    // FILE *err_file = fopen("logs/c_error.log", "w");
    FILE *a = fopen("logs/c.log", "a");

    while (true) {

        ret = avcodec_receive_frame(dec_ctx, frame);

        if (ret == AVERROR(EAGAIN) || ret == AVERROR_EOF) {
            // writerArgs->state = 2;
            break;
        }

        if (ret < 0) {
            fprintf(stdout, "Error decoding frame\n");
            return;
        }

        clock_t st = clock();

        ret = sws_scale( sws_ctx, (const uint8_t * const *)frame->data, frame->linesize, 0, frame->height, rgb_frame->data, rgb_frame->linesize);

        if (ret < 0) {
            fprintf(stdout, "Error converting frame to rgb\n");
            return;
        }

        putFrame(rgb_frame, &wargs->frame_buffer);
        pthread_cond_signal(&wait_cond);
        // writeFrame(rgb_frame, decoded_cnt, rendered_cnt);
        clock_t end = clock();

        fprintf(a, "%f ms\n", ((double)(end - st)) / CLOCKS_PER_SEC * 1000);

    }

    fclose(a);
    // fclose(err_file);
}

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

void frameLoop(char *path, int *rendered_cnt, int *decoded_cnt, int terminal_width, int terminal_height){

    const char *filename = path;

    AVFormatContext *fmt_ctx = NULL;
    AVCodecContext *dec_ctx = NULL;
    const AVCodec *decoder = NULL;
    AVStream *stream = NULL;

    AVPacket *packet = NULL;
    AVFrame *frame = NULL;
    AVFrame *rgb_frame = NULL;
    struct SwsContext *sws_ctx = NULL;

    WriteArgs wargs = {0};
    wargs.decoded_cnt = decoded_cnt;
    wargs.rendered_cnt = rendered_cnt;
    wargs.frame_buffer.consumer_ptr = 0;
    wargs.frame_buffer.producer_ptr = 0;

    pthread_t thread;

    int ret;
    int video_stream = -1;

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
    frame = av_frame_alloc();

    if (!packet || !frame) {
        fprintf(stderr, "Could not allocate packet/frame\n");

        av_packet_free(&packet);
        av_frame_free(&frame);
        avcodec_free_context(&dec_ctx);
        avformat_close_input(&fmt_ctx);

        return ;
    }

    rgb_frame = av_frame_alloc();

    rgb_frame->format = AV_PIX_FMT_RGB24;
    rgb_frame->width  = terminal_width;
    rgb_frame->height = terminal_height;
    wargs.width  = terminal_width;
    wargs.height = terminal_height;

    ret = av_frame_get_buffer(rgb_frame, 32);

    if (ret < 0){
        fprintf(stderr, "Error allocating frame buffer for rgb\n");
    }

    sws_ctx = sws_getContext( stream->codecpar->width, stream->codecpar->height, stream->codecpar->format, rgb_frame->width, rgb_frame->height, AV_PIX_FMT_RGB24, SWS_BILINEAR, NULL, NULL, NULL);

    pthread_create(&thread, NULL, writeLoop, &wargs);

    while ((ret = av_read_frame(fmt_ctx, packet)) >= 0) {

        FILE *b = fopen("logs/c2.log", "a");

        clock_t start = clock();

        if (packet->stream_index != video_stream) {
            av_packet_unref(packet);
            continue;
        }

        ret = avcodec_send_packet(dec_ctx, packet);

        if (ret < 0) {
            fprintf(stderr, "Error sending packet to decoder\n");
            break;
        }

        clock_t end = clock();

        fprintf(b, "%fms\n",(double)(end - start) / CLOCKS_PER_SEC * 1000 );

        fclose(b);

        decode(dec_ctx, frame, sws_ctx, rgb_frame, rendered_cnt, decoded_cnt, &wargs);

        av_packet_unref(packet);
    }

    pthread_join(thread, NULL);

    avcodec_send_packet(dec_ctx, NULL);

    decode(dec_ctx, frame, sws_ctx, rgb_frame, rendered_cnt, decoded_cnt, &wargs);

    av_frame_free(&frame);
    av_frame_free(&rgb_frame);
    av_packet_free(&packet);
    avcodec_free_context(&dec_ctx);
    avformat_close_input(&fmt_ctx);
    sws_freeContext(sws_ctx);
}

// int main(){
//
//     int a = 0;
//     int b = 0;
//     frameLoop("./videos/levi.mp4", &a, &b, 2000, 2000);
//
// }
