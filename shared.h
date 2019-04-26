#ifndef SHARED_H
#define SHARED_H

#define MAX_PROCS 19


/* Time structure */
typedef struct
{
	unsigned int seconds;
	unsigned int ns;
} Time;

typedef struct
{
	int pid; //process pid
    char status[50];
	Time unblockTime;
	int unblockOP;
} Process;

typedef struct
{
	Time sysTime;
    Process proc[MAX_PROCS]; //process table
} Shared;

#endif