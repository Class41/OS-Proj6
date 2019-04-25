#ifndef MEM_STR_H
#define MEM_STR_H

#define PAGE_SIZE 1
#define PROC_SIZE 32
#define MEM_SIZE 256

typedef struct
{
    unsigned int swapped;
    unsigned int framePos;
} TransFrame;

typedef struct
{
    unsigned ref : 8;
    unsigned dirty : 1;
    TransFrame* callback;
    int currentPid;
} Frame;

typedef struct
{
    TransFrame frames[PROC_SIZE / PAGE_SIZE];
} ProcPageTable;

typedef struct
{
    Frame frames[MEM_SIZE / PAGE_SIZE];
} Main;

typedef struct 
{
   Main mainMemory;
   ProcPageTable procTables;
} Memory;

#endif