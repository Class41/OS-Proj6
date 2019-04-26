#include <stdio.h>
#include <unistd.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <sys/ipc.h>
#include <sys/shm.h>
#include <errno.h>
#include <signal.h>
#include <sys/time.h>
#include <sys/types.h>
#include <sys/msg.h>
#include "queue.h"
#include "shared.h"
#include "string.h"
#include "memorystructure.h"

/*
*	Author: Vasyl Onufriyev
*	Project 5: Resource Managment
*	Date: 4/14/19
*	Purpose: Launch user processes, allocate resourced or deny them depending on a shared memory table
*/

#define BYTE_TO_BINARY_PATTERN "%c%c%c%c%c%c%c%c" //https://stackoverflow.com/questions/111928/is-there-a-printf-converter-to-print-in-binary-format
#define BYTE_TO_BINARY(byte)       \
	(byte & 0x80 ? '1' : '0'),     \
		(byte & 0x40 ? '1' : '0'), \
		(byte & 0x20 ? '1' : '0'), \
		(byte & 0x10 ? '1' : '0'), \
		(byte & 0x08 ? '1' : '0'), \
		(byte & 0x04 ? '1' : '0'), \
		(byte & 0x02 ? '1' : '0'), \
		(byte & 0x01 ? '1' : '0')

int ipcid;	//inter proccess shared memory
Shared *data; //shared memory data
Memory mem;
int toChildQueue;	//queue for communicating to child from master
int toMasterQueue;   //queue for communicating from child to master
char *filen;		 //name of this executable
int childCount = 19; //Max children concurrent

FILE *o; //output log file pointer

const int CLOCK_ADD_INC = 5000000; //How much to increment the clock by per tick

/* Create prototypes for used functions*/
void Handler(int signal);
void DoFork(int value);
void ShmAttatch();
void TimerHandler(int sig);
int SetupInterrupt();
int SetupTimer();
void DoSharedWork();
int FindEmptyProcBlock();
void SweepProcBlocks();
void AddTimeLong(Time *time, long amount);
void AddTime(Time *time, int amount);
int FindPID(int pid);
void QueueAttatch();
void GenerateResources();
void DisplayResources();

/* Message queue standard message buffer */
struct
{
	long mtype;
	char mtext[100];
} msgbuf;

/* Add time to given time structure, max 2.147billion ns */
void AddTime(Time *time, int amount)
{
	int newnano = time->ns + amount;
	while (newnano >= 1000000000) //nano = 10^9, so keep dividing until we get to something less and increment seconds
	{
		newnano -= 1000000000;
		(time->seconds)++;
	}
	time->ns = newnano; //since newnano is now < 1 billion, it is less than second. Assign it to ns
}

/* Add more than 2.147 billion nanoseconds to the time */
void AddTimeLong(Time *time, long amount)
{
	long newnano = time->ns + amount;
	while (newnano >= 1000000000) //nano = 10^9, so keep dividing until we get to something less and increment seconds
	{
		newnano -= 1000000000;
		(time->seconds)++;
	}
	time->ns = (int)newnano; //since newnano is now < 1 billion, it is less than second. Assign it to ns
}

/* My new time comparison function which uses epoch math instead of comparing nano/secs which sometimes causes issues*/
int CompareTime(Time *time1, Time *time2)
{
	long time1Epoch = ((long)(time1->seconds) * (long)1000000000) + (long)(time1->ns);
	long time2Epoch = ((long)(time2->seconds) * (long)1000000000) + (long)(time2->ns);

	if (time1Epoch > time2Epoch)
		return 1;
	else
		return 0;
}

/* handle ctrl-c and timer hit */
void Handler(int signal)
{
	int i;

	for (i = 0; i < childCount; i++) //loop thorough the proccess table and issue a termination signal to all unkilled proccess/children
		if (data->proc[i].pid != -1)
			kill(data->proc[i].pid, SIGTERM);

	fflush(o);							  //flush out the output file
	fclose(o);							  //close output file
	shmctl(ipcid, IPC_RMID, NULL);		  //free shared mem
	msgctl(toChildQueue, IPC_RMID, NULL); //free queues
	msgctl(toMasterQueue, IPC_RMID, NULL);

	printf("\n\n%s: Termination signal caught. Killed processes and killing self now...goodbye...\n\n", filen);

	kill(getpid(), SIGTERM); //kill self
}

/* Perform a forking call to launch a user proccess */
void DoFork(int value) //do fun fork stuff here. I know, very useful comment.
{
	char *forkarg[] = {//null terminated args set
					   "./user",
					   NULL}; //null terminated parameter array of chars

	execv(forkarg[0], forkarg); //exec
	Handler(1);
}

/* Attaches to shared memory */
void ShmAttatch() //attach to shared memory
{
	key_t shmkey = ftok("shmshare", 312); //shared mem key

	if (shmkey == -1) //check if the input file exists
	{
		fflush(stdout);
		perror("Error: Ftok failed");
		return;
	}

	ipcid = shmget(shmkey, sizeof(Shared), 0600 | IPC_CREAT); //get shared mem

	if (ipcid == -1) //check if the input file exists
	{
		fflush(stdout);
		perror("Error: failed to get shared memory");
		return;
	}

	data = (Shared *)shmat(ipcid, (void *)0, 0); //attach to shared mem

	if (data == (void *)-1) //check if the input file exists
	{
		fflush(stdout);
		perror("Error: Failed to attach to shared memory");
		return;
	}
}

/* Handle the timer hitting x seconds*/
void TimerHandler(int sig)
{
	Handler(sig);
}

/* Setup interrupt handling */
int SetupInterrupt()
{
	struct sigaction act;
	act.sa_handler = TimerHandler;
	act.sa_flags = 0;
	return (sigemptyset(&act.sa_mask) || sigaction(SIGPROF, &act, NULL));
}

/* setup interrupt handling from the timer */
int SetupTimer()
{
	struct itimerval value;
	value.it_interval.tv_sec = 2;
	value.it_interval.tv_usec = 0;
	value.it_value = value.it_interval;
	return (setitimer(ITIMER_PROF, &value, NULL));
}

/* Find the next empty proccess block. Returns proccess block position if one is available or -1 if one is not */
int FindEmptyProcBlock()
{
	int i;
	for (i = 0; i < childCount; i++)
	{
		if (data->proc[i].pid == -1)
			return i; //return proccess table position of empty
	}

	return -1; //error: no proccess slot available
}

/* Sets all proccess blocks to the initial value of -1 for algorithm reasons */
void SweepProcBlocks()
{
	int i;
	for (i = 0; i < MAX_PROCS; i++)
		data->proc[i].pid = -1;
}

int CalculatePageID(int rawLine)
{
	return (rawLine / (PAGE_SIZE * 1000));
}

int CalculatePageOffset(int rawLine)
{
	return (rawLine % (PAGE_SIZE * 1000));
}

int CheckAndInsert(int pid, int pageID)
{
	if (mem.procTables[pid].frames[pageID].framePos == -1)
	{
		InsertPage(pid, pageID);
		return 0;
	}
	if (mem.procTables[pid].frames[pageID].framePos > -1 && mem.procTables[pid].frames[pageID].swapped == 0)
	{
		return 1;
	}
	else if (mem.procTables[pid].frames[pageID].framePos > -1 && mem.procTables[pid].frames[pageID].swapped == 1)
	{
		InsertPage(pid, pageID);
		return 2;
	}
}

void InsertPage(int pid, int pageID)
{
	int i;
	Frame oldest;
	oldest.ref = 0xff;

	int oldestPos = -1;

	for (i = 0; i < MEM_SIZE / PAGE_SIZE; i++)
	{
		if (mem.mainMemory.frames[i].currentPid < 0)
		{
			oldestPos = i;
			break;
		}

		if (mem.mainMemory.frames[i].ref < oldest.ref)
		{
			oldest.ref = mem.mainMemory.frames[i].ref;
			oldestPos = i;
		}
	}

	if (oldestPos == -1)
		oldestPos = 0;

	if (mem.mainMemory.frames[oldestPos].currentPid > -1)
	{
		printf("\nSet swapped");
		(mem.mainMemory.frames[oldestPos].callback)->swapped = 1;
	}

	CleanupMemory(oldestPos);
	SetPid(oldestPos, pid);
	SetReference(oldestPos);

	mem.procTables[pid].frames[pageID].swapped = 0;
	mem.procTables[pid].frames[pageID].framePos = oldestPos;

	SetCallback(oldestPos, &(mem.procTables[pid].frames[pageID]));
}

void GenerateProc(int pos)
{
	int i;
	for (i = 0; i < PROC_SIZE / PAGE_SIZE; i++)
	{
		mem.procTables[pos].frames[i].framePos = -1;
		mem.procTables[pos].frames[i].swapped = -1;
	}
}

void CleanupMemory(int pos)
{
	ClearReference(pos);
	ClearDirty(pos);
	ClearCallback(pos);
	ClearPid(pos);
}

void ShiftReference()
{
	int i;

	for (i = 0; i < MEM_SIZE / PAGE_SIZE; i++)
	{
		mem.mainMemory.frames[i].ref = mem.mainMemory.frames[i].ref >> 1;
	}
}

void ClearCallback(int pos)
{
	mem.mainMemory.frames[pos].callback = NULL;
}

void SetCallback(int pos, TransFrame *frame)
{
	mem.mainMemory.frames[pos].callback = frame;
}

void SetReference(int pos)
{
	mem.mainMemory.frames[pos].ref = mem.mainMemory.frames[pos].ref | 0x80;
}

void ClearReference(int pos)
{
	mem.mainMemory.frames[pos].ref = 0x0;
}

void SetDirty(int pos)
{
	mem.mainMemory.frames[pos].dirty = 0x1;
}

void ClearDirty(int pos)
{
	mem.mainMemory.frames[pos].dirty = 0x0;
}

int GetPid(int pos)
{
	return mem.mainMemory.frames[pos].currentPid;
}

void ClearPid(int pos, int pid)
{
	mem.mainMemory.frames[pos].currentPid = -1;
}

void SetPid(int pos, int pid)
{
	mem.mainMemory.frames[pos].currentPid = pid;
}

/* The miracle of resource creation is done here */
void GenerateResources()
{
	int i;

	for (i = 0; i < MEM_SIZE / PAGE_SIZE; i++)
	{
		mem.mainMemory.frames[i].ref = 0x0;
		mem.mainMemory.frames[i].dirty = 0x0;
		mem.mainMemory.frames[i].callback = NULL;
		mem.mainMemory.frames[i].currentPid = -1;
	}

	int j;
	for (i = 0; i < MAX_PROCS; i++)
	{
		for (j = 0; j < PROC_SIZE / PAGE_SIZE; j++)
		{
			mem.procTables[i].frames[j].swapped = -1;
			mem.procTables[i].frames[j].framePos = -1;
		}
	}

	printf("\n%s: Finished generating resources!", filen);
}

/* Display the system resource tables to the file */
void DisplayResources()
{
	int i;
	printf("\n*** Main Memory State ***");
	printf("\nAddr\t\tRef\t\tDirty\tPID");

	for (i = 0; i < MEM_SIZE / PAGE_SIZE; i++)
	{
		printf("\n[0x%5x]\t%c%c%c%c%c%c%c%c\t%x\t%5i", i * 1000, BYTE_TO_BINARY(mem.mainMemory.frames[i].ref), mem.mainMemory.frames[i].dirty, mem.mainMemory.frames[i].currentPid);
	}
}

/* Find the proccess block with the given pid and return the position in the array */
int FindPID(int pid)
{
	int i;
	for (i = 0; i < childCount; i++)
		if (data->proc[i].pid == pid)
			return i;
	return -1;
}

/* Attach to queues incoming/outgoing */
void QueueAttatch()
{
	key_t shmkey = ftok("shmsharemsg", 766);

	if (shmkey == -1) //check if the input file exists
	{
		fflush(stdout);
		perror("./oss: Error: Ftok failed");
		return;
	}

	toChildQueue = msgget(shmkey, 0600 | IPC_CREAT); //attach to child queue

	if (toChildQueue == -1)
	{
		fflush(stdout);
		perror("./oss: Error: toChildQueue creation failed");
		return;
	}

	shmkey = ftok("shmsharemsg2", 767);

	if (shmkey == -1) //check if the input file exists
	{
		fflush(stdout);
		perror("./oss: Error: Ftok failed");
		return;
	}

	toMasterQueue = msgget(shmkey, 0600 | IPC_CREAT); //attach to master queue

	if (toMasterQueue == -1)
	{
		fflush(stdout);
		perror("./oss: Error: toMasterQueue creation failed");
		return;
	}
}

/* Program entry point */
int main(int argc, int **argv)
{
	//alias for file name
	filen = argv[0];					  //shorthand for filename
	srand(time(NULL) ^ (getpid() << 16)); //set random seed, doesn't really seem all that random tho...

	if (SetupInterrupt() == -1) //Handler for SIGPROF failed
	{
		perror("./oss: Failed to setup Handler for SIGPROF");
		return 1;
	}
	if (SetupTimer() == -1) //timer failed
	{
		perror("./oss: Failed to setup ITIMER_PROF interval timer");
		return 1;
	}

	int optionItem;
	while ((optionItem = getopt(argc, argv, "hn:")) != -1) //read option list
	{
		switch (optionItem)
		{
		case 'h': //show help menu
			printf("\t%s Help Menu\n\
		\t-h : show help dialog \n\
		\t-v : enable verbose mode. Default: off \n\
		\t-n [count] : max proccesses at the same time. Default: 19\n\n",
				   filen);
			return;
		case 'n': //max # of children
			childCount = atoi(optarg);
			if (childCount > 19 || childCount < 0) //if 0  > n > 20
			{
				printf("%s: Max -n is 19. Must be > 0 Aborting.\n", argv[0]);
				return -1;
			}

			printf("\n%s: Info: set max concurrent children to: %s", argv[0], optarg);
			break;
		case '?': //an error has occoured reading arguments
			printf("\n%s: Error: Invalid Argument or Arguments missing. Use -h to see usage.", argv[0]);
			return;
		}
	}

	o = fopen("output.log", "w"); //open output file

	if (o == NULL) //check if file was opened
	{
		perror("oss: Failed to open output file: ");
		return 1;
	}

	ShmAttatch();	  //attach to shared mem
	QueueAttatch();	//attach to queues
	SweepProcBlocks(); //reset all proc blocks
	GenerateResources();
	signal(SIGINT, Handler); //setup handler for CTRL-C

	int i, j;
	for (i = 0; i < PROC_SIZE / PAGE_SIZE; i++)
	{
		CheckAndInsert(1, i);

		printf("\n\n**Proc Data**");
		for (j = 0; j < PROC_SIZE / PAGE_SIZE; j++)
		{
			printf("\n%i: Swapped? %i FramePos? %i", j, mem.procTables[1].frames[j].swapped, mem.procTables[1].frames[j].framePos);
		}
	}

	for (i = 0; i < 1000; i++)
	{
		CheckAndInsert(rand() % 20, CalculatePageID(rand() % 32000));

		((rand() % 2) == 0) ? ShiftReference() : printf("");
		SetReference(rand() % (MEM_SIZE / PAGE_SIZE));
	}

	DisplayResources();

	for (i = 0; i < PROC_SIZE / PAGE_SIZE; i++)
	{
		CheckAndInsert(1, i);

		printf("\n\n**Proc Data**");
		for (j = 0; j < PROC_SIZE / PAGE_SIZE; j++)
		{
			printf("\n%i: Swapped? %i FramePos? %i", j, mem.procTables[1].frames[j].swapped, mem.procTables[1].frames[j].framePos);
		}
	}
		DisplayResources();

	printf("\n\n**Proc Data**");
	for (j = 0; j < PROC_SIZE / PAGE_SIZE; j++)
	{
		printf("\n%i: Swapped? %i FramePos? %i", j, mem.procTables[1].frames[j].swapped, mem.procTables[1].frames[j].framePos);
	}

	shmctl(ipcid, IPC_RMID, NULL);		  //free shared mem
	msgctl(toChildQueue, IPC_RMID, NULL); //free queues
	msgctl(toMasterQueue, IPC_RMID, NULL);

	printf("\n\n%s: Termination signal caught. Killed processes and killing self now...goodbye...\n\n", filen);

	kill(getpid(), SIGTERM); //kill self

	//DoSharedWork();			 //fattest function west of the mississippi

	return 0;
}
