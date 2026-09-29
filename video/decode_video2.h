# include <stdio.h>

int *createModeVariable();

int *deleteModeVariable(int *mode);

void increment(int *val);

void incrementRenderCnt(int *val);

void frameLoop(char *path, int *rendered_cnt, int *decoded_cnt, int terminal_width, int terminal_height);

