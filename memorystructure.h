#ifndef MEM_STR_H
#define MEM_STR_H

#include "shared.h"

#define PAGE_SIZE 1
#define PROC_SIZE 32
#define MEM_SIZE 256

typedef struct //Frame for the translation table
{
    unsigned int swapped;
    unsigned int framePos;
} TransFrame;

typedef struct //frame for the main memory
{
    unsigned ref : 8;
    unsigned dirty : 1;
    TransFrame *callback; //used to update the swapped value of currently resident frame
    int currentPid;
} Frame;

typedef struct //poccess table frame
{
    TransFrame frames[PROC_SIZE / PAGE_SIZE];
} ProcPageTable;

typedef struct //main memory frame
{
    Frame frames[MEM_SIZE / PAGE_SIZE];
} Main;

typedef struct //total memory structure
{
    Main mainMemory;
    ProcPageTable procTables[MAX_PROCS];
} Memory;

#endif