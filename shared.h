#ifndef SHARED_H
#define SHARED_H

#define MAX_PROCS 19

typedef struct
{
	int pid; //process pid
    char status[50];
} Process;

/* Time structure */
typedef struct
{
	unsigned int seconds;
	unsigned int ns;
} Time;

typedef struct
{
	Time sysTime;
    Process proc[MAX_PROCS]; //process table
} Shared;

#endif