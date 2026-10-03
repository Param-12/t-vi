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

typedef struct Node{

    AVFrame *rgb_frame;
    struct Node* next;
    struct Node* prev;

}Node;

typedef struct WorkerQueue{

    bool finished;
    int height;
    int width;
    int *decoded_cnt;
    int *rendered_cnt;
    Node* front;
    Node* back;
    AVFrame *rgb_frame;
    bool consume;

}WorkerQueue;

Node *createNode(AVFrame *rgb_frame){

    Node *node = malloc(sizeof(Node));

    node->rgb_frame = av_frame_alloc();

    node->rgb_frame = rgb_frame;
    node->next = NULL;
    node->prev = NULL;

    return node;
}

void popFrame(WorkerQueue *wq){

    if (wq->front == NULL || wq->front->next == NULL){
        fprintf(stderr, "access of null head in the worker queue\n");
        exit(1);
    }

    // if (wq->front == wq->back){
    //     fprintf(stderr, "invalid pop opertion : both front and back point to the same node\n");
    //     exit(1);
    // }

    Node* temp = wq->front;
    wq->front = wq->front->next;
    wq->front->prev = NULL;

    av_frame_free(&temp->rgb_frame);
    free(temp);
}

void insertFrame(Node *node, WorkerQueue *wq){

    if (wq->back == NULL && wq->front == NULL){

        wq->back = node;
        wq->front = node;
        return;
    }

    wq->back->next = node;
    node->prev = wq->back;
    wq->back = node;
}

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

    WorkerQueue *wq = args;

    void *ptr;
    uint8_t *shm_dest_ptr;
    char path[16];
    int shm_fd;

    int size = wq->height * wq->width * 3;
    
    size_t row_bytes = wq->width * 3;

    while(!wq->finished){

        snprintf( path, 16, "/frame%d", *wq->decoded_cnt % 50);

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

        while (!wq->consume){

            int *p = 0;

            if (wq->finished){

                munmap(ptr, wq->height * wq->width * 3);
                close(shm_fd);
                return p;
            }

            usleep(50000);
        }

        while (true) {

            int decoded = __atomic_load_n(wq->decoded_cnt, __ATOMIC_ACQUIRE);
            int rendered = __atomic_load_n(wq->rendered_cnt, __ATOMIC_ACQUIRE);

            if (decoded - rendered < 40) {
                break;
            }

            usleep(500000);
        }
        for (int y = 0; y < wq->front->rgb_frame->height; y++) {

            uint8_t *src_ptr = wq->rgb_frame->data[0] + y * wq->rgb_frame->linesize[0];
            memcpy(shm_dest_ptr, src_ptr, row_bytes);

            shm_dest_ptr += row_bytes;
        }

        munmap(ptr, wq->rgb_frame->height * wq->rgb_frame->width * 3);
        close(shm_fd);
        increment(wq->decoded_cnt);
        popFrame(wq);
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

void decode2(AVCodecContext *dec_ctx, AVFrame *frame, struct SwsContext *sws_ctx, int terminal_height, int terminal_width , int *rendered_cnt, int *decoded_cnt, WorkerQueue *wq){

    int ret;

    FILE *a = fopen("logs/c.log", "a");

    while (true) {

        ret = avcodec_receive_frame(dec_ctx, frame);

        if (ret == AVERROR(EAGAIN) || ret == AVERROR_EOF) {
            break;
        }

        if (ret < 0) {
            fprintf(stdout, "Error decoding frame\n");
            return;
        }

        clock_t st = clock();

        Node* node = malloc(sizeof(Node));
        node->next = NULL;
        node->prev = NULL;

        node->rgb_frame = av_frame_alloc();
        node->rgb_frame->format = AV_PIX_FMT_RGB24;
        node->rgb_frame->height = terminal_height;
        node->rgb_frame->width = terminal_width;

        ret = av_frame_get_buffer(node->rgb_frame, 32);

        if (ret < 0){
            fprintf(stderr, "Error allocating frame buffer for rgb\n");
        }

        ret = sws_scale( sws_ctx, (const uint8_t * const *)frame->data, frame->linesize, 0, frame->height, node->rgb_frame->data, node->rgb_frame->linesize);

        if (ret < 0) {
            fprintf(stdout, "Error converting frame to rgb\n");
            return;
        }

        insertFrame(node, wq);
        clock_t end = clock();

        fprintf(a, "%f ms\n", ((double)(end - st)) / CLOCKS_PER_SEC * 1000);
    }

    fclose(a);
}

void decode(AVCodecContext *dec_ctx, AVFrame *frame, struct SwsContext *sws_ctx, AVFrame *rgb_frame , int *rendered_cnt, int *decoded_cnt){

    int ret;

    // FILE *err_file = fopen("logs/c_error.log", "w");
    FILE *a = fopen("logs/c.log", "a");

    while (true) {

        clock_t t1 = clock();

        ret = avcodec_receive_frame(dec_ctx, frame);

        if (ret == AVERROR(EAGAIN) || ret == AVERROR_EOF) {
            // writerArgs->state = 2;
            break;
        }

        if (ret < 0) {
            fprintf(stdout, "Error decoding frame\n");
            return;
        }

        clock_t t2 = clock();

        ret = sws_scale( sws_ctx, (const uint8_t * const *)frame->data, frame->linesize, 0, frame->height, rgb_frame->data, rgb_frame->linesize);

        if (ret < 0) {
            fprintf(stdout, "Error converting frame to rgb\n");
            return;
        }

        clock_t t3 = clock();

        writeFrame(rgb_frame, decoded_cnt, rendered_cnt);

        clock_t t4 = clock();

        fprintf(a, "recieve time : %f ms | scale time : %f ms | write time : %f ms\n", ((double)(t2 - t1)) / CLOCKS_PER_SEC * 1000, ((double)(t3 - t2)) / CLOCKS_PER_SEC * 1000, ((double)(t4 - t3)) / CLOCKS_PER_SEC * 1000);
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

void frameLoop2(char *path, int *rendered_cnt, int *decoded_cnt, int terminal_width, int terminal_height){

    const char *filename = path;

    AVFormatContext *fmt_ctx = NULL;
    AVCodecContext *dec_ctx = NULL;
    const AVCodec *decoder = NULL;
    AVStream *stream = NULL;

    AVPacket *packet = NULL;
    AVFrame *frame = NULL;
    struct SwsContext *sws_ctx = NULL;

    pthread_t thread;

    WorkerQueue wq = {0};

    Node* front = malloc(sizeof(Node));

    wq.decoded_cnt = decoded_cnt;
    wq.rendered_cnt = rendered_cnt;

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

    wq.height = terminal_height;
    wq.width = terminal_width;

    sws_ctx = sws_getContext( stream->codecpar->width, stream->codecpar->height, stream->codecpar->format, terminal_width, terminal_height, AV_PIX_FMT_RGB24, SWS_BILINEAR, NULL, NULL, NULL);

    pthread_create(&thread, NULL, writeLoop, &wq);

    while ((ret = av_read_frame(fmt_ctx, packet)) >= 0) {

        // FILE *b = fopen("logs/c2.log", "a");

        // clock_t start = clock();

        if (packet->stream_index != video_stream) {
            av_packet_unref(packet);
            continue;
        }

        ret = avcodec_send_packet(dec_ctx, packet);

        if (ret < 0) {
            fprintf(stderr, "Error sending packet to decoder\n");
            break;
        }

        // clock_t end = clock();

        // fprintf(b, "%fms\n",(double)(end - start) / CLOCKS_PER_SEC * 1000 );

        // fclose(b);

        decode2(dec_ctx, frame, sws_ctx, terminal_height, terminal_width, rendered_cnt, decoded_cnt, &wq);

        av_packet_unref(packet);
    }

    wq.finished = true;

    // Node* terminatingNode = malloc(sizeof(Node));
    // terminatingNode->rgb_frame = av_frame_alloc();
    // insertFrame(terminatingNode, &wq);

    pthread_join(thread, NULL);

    avcodec_send_packet(dec_ctx, NULL);

    decode2(dec_ctx, frame, sws_ctx, terminal_height, terminal_width, rendered_cnt, decoded_cnt, &wq);

    av_frame_free(&frame);
    av_packet_free(&packet);
    avcodec_free_context(&dec_ctx);
    avformat_close_input(&fmt_ctx);
    sws_freeContext(sws_ctx);
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

    FILE *b = fopen("logs/c2.log", "a");

    clock_t start = clock();

    rgb_frame = av_frame_alloc();

    rgb_frame->format = AV_PIX_FMT_RGB24;
    rgb_frame->width  = terminal_width;
    rgb_frame->height = terminal_height;

    ret = av_frame_get_buffer(rgb_frame, 32);

    if (ret < 0){
        fprintf(stderr, "Error allocating frame buffer for rgb\n");
    }

    clock_t end = clock();

    fprintf(b, "%fms\n",(double)(end - start) / CLOCKS_PER_SEC * 1000 );

    fclose(b);

    sws_ctx = sws_getContext( stream->codecpar->width, stream->codecpar->height, stream->codecpar->format, rgb_frame->width, rgb_frame->height, AV_PIX_FMT_RGB24, SWS_BILINEAR, NULL, NULL, NULL);

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

        decode(dec_ctx, frame, sws_ctx, rgb_frame, rendered_cnt, decoded_cnt);

        av_packet_unref(packet);
    }

    avcodec_send_packet(dec_ctx, NULL);

    decode(dec_ctx, frame, sws_ctx, rgb_frame, rendered_cnt, decoded_cnt);

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
//     frameLoop2("./videos/levi.mp4", &a, &b, 2000, 2000);
//
// }
