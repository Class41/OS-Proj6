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

#define SHIFT_INTERVAL 100
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

const int CLOCK_ADD_INC = 250000; //How much to increment the clock by per tick

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
int CalculatePageID(int rawLine);
int CalculatePageOffset(int rawLine);
int CheckAndInsert(int pid, int pageID, int insertMode);
void DeleteProc(int pid);
void InsertPage(int pid, int pageID);
void GenerateProc(int pos);
void CleanupMemory(int pos);
void ShiftReference();
void ClearCallback(int pos);
void SetCallback(int pos, TransFrame *frame);
void SetReference(int pos);
void ClearReference(int pos);
void SetDirty(int pos);
void ClearDirty(int pos);
int GetPid(int pos);
void ClearPid(int pos);
void SetPid(int pos, int pid);
void DisplayResourcesToFile();

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

	DisplayResources(); //display resources on death

	printf("\n\n\n** STATUSES **\n"); //display status of all children on death
	for (i = 0; i < childCount; i++)
	{
		printf("%i: %i : %s\n", i, data->proc[i].pid, data->proc[i].status);
	}

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

/* calculates pageID based on the page size and rawLine provided */
int CalculatePageID(int rawLine)
{
	return (rawLine / (PAGE_SIZE * 1000));
}

/* calculates offset based on page size and rawLine provided */
int CalculatePageOffset(int rawLine)
{
	return (rawLine % (PAGE_SIZE * 1000));
}

/* Checks the state of proccess frame and returns the state. Also used to insert data into the main memory frame table */
int CheckAndInsert(int pid, int pageID, int insertMode)
{
	if (mem.procTables[pid].frames[pageID].framePos == -1) //for the case that we do not have a frame in memory for this position
	{
		if (insertMode == 1)
		{
			InsertPage(pid, pageID);
		}

		return 0;
	}
	else if (mem.procTables[pid].frames[pageID].swapped == 0) //If frame is present in memory and not swapped out
	{
		return 1;
	}
	else if (mem.procTables[pid].frames[pageID].swapped == 1) //frame was in memory, but was swapped out at some point. We need to swap it back in.
	{
		if (insertMode == 1)
		{
			InsertPage(pid, pageID);
		}

		return 2;
	}
	else
	{
		printf("\nReturning -1 - PID: %i PageID: %i", pid, pageID); //if an error happens. This generally does not appear...
		return -1;
	}
}

/* Deletes proccess from main memory with given PID */
void DeleteProc(int pid)
{
	int i;
	for (i = 0; i < MEM_SIZE / PAGE_SIZE; i++)
		if (GetPid(i) == pid)
			CleanupMemory(i);
}

/* Handles page insertion and swapping */
void InsertPage(int pid, int pageID)
{
	int i;
	Frame oldest;	  //This is here because bit-specific variables are weird and can apparently only exist in strcucts. This is my workaround.
	oldest.ref = 0xff; //set to highest possible value with 8 bits
	int oldestPos = -1;

	for (i = 0; i < MEM_SIZE / PAGE_SIZE; i++)
	{
		if (mem.mainMemory.frames[i].currentPid == -1) //if we find an empty frame, look no further
		{
			oldestPos = i;
			break;
		}

		if (mem.mainMemory.frames[i].ref < oldest.ref) //Is the current frame we are on of lower ref than the other frame we have seen?
		{
			oldest.ref = mem.mainMemory.frames[i].ref; //This is now the frame to be replaced
			oldestPos = i;
		}
	}

	if (oldestPos == -1) //just in case every frame is 0xff, which is unlikely, just replace the first one
		oldestPos = 0;

	if (mem.mainMemory.frames[oldestPos].currentPid > -1) //if the frame was already occupied
	{
		if (mem.mainMemory.frames[oldestPos].dirty == 0x1) //check for dirty bit
		{
			AddTime(&(data->sysTime), 5000); //Make swapping it out more expensive
		}

		(mem.mainMemory.frames[oldestPos].callback)->swapped = 1; //set the swapped out frame's swapped level to 1
	}

	CleanupMemory(oldestPos); //clean up any residue from frame
	SetPid(oldestPos, pid);   //set the pid to the requesting pid
	SetReference(oldestPos);  //set reference since we jusde made the frame

	mem.procTables[pid].frames[pageID].swapped = 0;			 //we are not swapped
	mem.procTables[pid].frames[pageID].framePos = oldestPos; //set new main memory frame position in the proccess table

	SetCallback(oldestPos, &(mem.procTables[pid].frames[pageID])); //set the callback of the new main memory frame to the pageframe of the proccess
}

/* Generate/Initialize proccesses */
void GenerateProc(int pos)
{
	int i;
	for (i = 0; i < PROC_SIZE / PAGE_SIZE; i++)
	{
		mem.procTables[pos].frames[i].framePos = -1;
		mem.procTables[pos].frames[i].swapped = -1;
	}
}

/* Cleans a memory frame */
void CleanupMemory(int pos)
{
	ClearReference(pos);
	ClearDirty(pos);
	ClearCallback(pos);
	ClearPid(pos);
}

/* Shifts reference bits by 1 to the right */
void ShiftReference()
{
	int i;

	for (i = 0; i < MEM_SIZE / PAGE_SIZE; i++)
	{
		mem.mainMemory.frames[i].ref = mem.mainMemory.frames[i].ref >> 1;
	}
}

/* Clears callback */
void ClearCallback(int pos)
{
	mem.mainMemory.frames[pos].callback = NULL;
}

/* Set the callback on the frame specified */
void SetCallback(int pos, TransFrame *frame)
{
	mem.mainMemory.frames[pos].callback = frame;
}

/* sets leftmost reference bit */
void SetReference(int pos)
{
	mem.mainMemory.frames[pos].ref = mem.mainMemory.frames[pos].ref | 0x80;
}

/* Reset reference bits */
void ClearReference(int pos)
{
	mem.mainMemory.frames[pos].ref = 0x0;
}

/* Sets the dirty bit to 1 */
void SetDirty(int pos)
{
	mem.mainMemory.frames[pos].dirty = 0x1;
}

/* Sets dirty bit to 0 */
void ClearDirty(int pos)
{
	mem.mainMemory.frames[pos].dirty = 0x0;
}

/* Gets the pid of a main memory frame */
int GetPid(int pos)
{
	return mem.mainMemory.frames[pos].currentPid;
}

/* Clears the pid in the main memory frame */
void ClearPid(int pos)
{
	mem.mainMemory.frames[pos].currentPid = -1;
}

/* Sets the pid in the main memory frame */
void SetPid(int pos, int pid)
{
	mem.mainMemory.frames[pos].currentPid = pid;
}

/* The miracle of resource creation is done here */
void GenerateResources()
{
	int i;

	for (i = 0; i < MEM_SIZE / PAGE_SIZE; i++) //setup base values for all memory frames
	{
		mem.mainMemory.frames[i].ref = 0x0;
		mem.mainMemory.frames[i].dirty = 0x0;
		mem.mainMemory.frames[i].callback = NULL;
		mem.mainMemory.frames[i].currentPid = -1;
	}

	int j;
	for (i = 0; i < MAX_PROCS; i++)
	{
		for (j = 0; j < PROC_SIZE / PAGE_SIZE; j++) //reset all proccess frames for all procceses
		{
			mem.procTables[i].frames[j].swapped = -1;
			mem.procTables[i].frames[j].framePos = -1;
		}
	}

	printf("\n%s: Finished generating resources!", filen);
}

/* Display the system resource tables to the screen */
void DisplayResources()
{
	int i;
	printf("\n*** Main Memory State ***");
	printf("\nAddr\t\tRef\t\tDirty\tPID");

	for (i = 0; i < MEM_SIZE / PAGE_SIZE; i++)
	{
		printf("\n[0x%-5x]\t%c%c%c%c%c%c%c%c\t%x\t%-3i", i * 1000, BYTE_TO_BINARY(mem.mainMemory.frames[i].ref), mem.mainMemory.frames[i].dirty, mem.mainMemory.frames[i].currentPid);
	}

	DisplayResourcesToFile();
}

/* Display the system resource tables to the file */
void DisplayResourcesToFile()
{
	int i;
	fprintf(o, "\n*** Main Memory State ***");
	fprintf(o, "\nAddr\t\tRef\t\tDirty\tPID");

	for (i = 0; i < MEM_SIZE / PAGE_SIZE; i++)
	{
		fprintf(o, "\n[0x%-5x]\t%c%c%c%c%c%c%c%c\t%x\t%-3i", i * 1000, BYTE_TO_BINARY(mem.mainMemory.frames[i].ref), mem.mainMemory.frames[i].dirty, mem.mainMemory.frames[i].currentPid);
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

void DoSharedWork()
{
	/* General sched data */
	int activeProcs = 0;
	int exitCount = 0;
	int status;
	int iterator;
	int requestCounter = 0;

	/* Proc toChildQueue and message toChildQueue data */
	int msgsize;

	/* Set shared memory clock value */
	data->sysTime.seconds = 0;
	data->sysTime.ns = 0;

	/* Setup time for random child spawning and deadlock running */
	Time nextExec = {0, 0};
	Time deadlockExec = {0, 0};
	/* Create queues */
	struct Queue *resQueue = createQueue(childCount); //Queue of real PIDS

	while (1)
	{
		AddTime(&(data->sysTime), CLOCK_ADD_INC); //increment clock between tasks to advance the clock a little
		pid_t pid;								  //pid temp

		fflush(stdout);

		/* Only executes when there is a proccess ready to be launched, given the time is right for exec, there is room in the proc table */
		if (activeProcs < childCount && CompareTime(&(data->sysTime), &nextExec))
		{
			pid = fork(); //the mircle of proccess creation

			if (pid < 0) //...or maybe not proccess creation if this executes
			{
				perror("Failed to fork, exiting");
				Handler(1);
			}

			if (pid == 0)
			{
				DoFork(pid); //do the fork thing with exec followup
			}

			/* Setup the next exec for proccess*/
			nextExec.seconds = data->sysTime.seconds; //capture current time
			nextExec.ns = data->sysTime.ns;

			AddTimeLong(&nextExec, abs((long)(rand() % 501) * (long)1000000)); //set new exec time to 0 - 500ms after now

			/* Setup the child proccess and its proccess block if there is a available slot in the control block */
			int pos = FindEmptyProcBlock();
			if (pos > -1)
			{
				/* Initialize the proccess table */
				data->proc[pos].pid = pid; //we stored the pid from fork call and now assign it to PID
				GenerateProc(pos);
				fprintf(o, "%s: [%i:%i] [PROC CREATE] pid: %i\n\n", filen, data->sysTime.seconds, data->sysTime.ns, pid);
				activeProcs++; //increment active execs
			}
			else
			{
				kill(pid, SIGTERM); //if child failed to find a proccess block, just kill it off
			}
		}

		fflush(stdout);

		if ((msgsize = msgrcv(toMasterQueue, &msgbuf, sizeof(msgbuf), 0, IPC_NOWAIT)) > -1) //non-blocking wait while waiting for child to respond
		{
			if (strcmp(msgbuf.mtext, "REQ") == 0) //If message recieved was a request for resource
			{
				int reqpid = msgbuf.mtype;			 //save its mtype which is the pid of process
				int procpos = FindPID(msgbuf.mtype); //find its position in proc table
				int rawLine = 0;

				msgrcv(toMasterQueue, &msgbuf, sizeof(msgbuf), reqpid, 0); //wait for child to send resource line number
				rawLine = atoi(msgbuf.mtext);

				fprintf(o, "%s: [%i:%i] [REQUEST] pid: %i proc: %i rawLine: %i\n", filen, data->sysTime.seconds, data->sysTime.ns, msgbuf.mtype, procpos, rawLine);

				switch (CheckAndInsert(procpos, CalculatePageID(rawLine), 0)) //check the current state of the frame that was requested
				{
				case 0:																 //The requested frame is not in memory or ever saved in the proccess table and therefore pagefault + queue
					data->proc[procpos].unblockTime.seconds = data->sysTime.seconds; //capture current time
					data->proc[procpos].unblockTime.ns = data->sysTime.ns;			 //capture current time

					AddTimeLong(&(data->proc[procpos].unblockTime), abs((long)(rand() % 15) * (long)1000000)); //set new exec time to 0 - 15ms from now
					data->proc[procpos].unblockOP = 0;														   //set which operation should be performed on unlocked
					data->proc[procpos].lastFrameRequested = CalculatePageID(rawLine);						   //set the last frame requested
					enqueue(resQueue, reqpid);																   //enqueue into wait queue since failed
					fprintf(o, "\t-> [%i:%i] [REQUEST] [PAGE_FAULT=NOTFOUND] pid: %i request unfulfilled...\n\n", data->sysTime.seconds, data->sysTime.ns, msgbuf.mtype);
					break;
				case 1:								   //the requested frame is in memory and is not swapped out. Return OK to child
					strcpy(msgbuf.mtext, "REQ_GRANT"); //send message that resource has been granted to child
					msgbuf.mtype = reqpid;
					AddTime(&(data->sysTime), 10);													 //increment clock between tasks to advance the clock a little
					SetReference(mem.procTables[procpos].frames[CalculatePageID(rawLine)].framePos); //set reference bit since we just referenced the frame
					msgsnd(toChildQueue, &msgbuf, sizeof(msgbuf), IPC_NOWAIT);
					fprintf(o, "\t-> [%i:%i] [REQUEST] [OK] pid: %i request fulfilled...\n\n", data->sysTime.seconds, data->sysTime.ns, msgbuf.mtype);
					break;
				case 2:																 //the request frame is in secondary storage, we must bring it back before we can read it. Queued.
					data->proc[procpos].unblockTime.seconds = data->sysTime.seconds; //capture current time
					data->proc[procpos].unblockTime.ns = data->sysTime.ns;			 //capture current time

					AddTimeLong(&(data->proc[procpos].unblockTime), abs((long)(rand() % 15) * (long)1000000)); //set new exec time to 0 - 15ms from now
					data->proc[procpos].unblockOP = 0;
					data->proc[procpos].lastFrameRequested = CalculatePageID(rawLine);

					enqueue(resQueue, reqpid);
					fprintf(o, "\t-> [%i:%i] [REQUEST] [PAGE_FAULT=SWAPPEDOUT] pid: %i request unfulfilled...\n\n", data->sysTime.seconds, data->sysTime.ns, msgbuf.mtype);
					break;
				default:
					break;
				}
			}
			else if (strcmp(msgbuf.mtext, "WRI") == 0) //if write request
			{
				int reqpid = msgbuf.mtype;			 //save pid of child
				int procpos = FindPID(msgbuf.mtype); //lookup child in proc table
				int writeRaw;

				msgrcv(toMasterQueue, &msgbuf, sizeof(msgbuf), reqpid, 0); //wait for child to send writing resource identifier
				writeRaw = atoi(msgbuf.mtext);

				fprintf(o, "%s: [%i:%i] [WRITE] pid: %i proc: %i writeLine: %i\n", filen, data->sysTime.seconds, data->sysTime.ns, msgbuf.mtype, procpos, writeRaw);

				switch (CheckAndInsert(procpos, CalculatePageID(writeRaw), 0)) //Check state of page to be written to
				{
				case 0:																						   //The page is not in memory and is not in the proc frame table, load it in, queue while waiting
					data->proc[procpos].unblockTime.seconds = data->sysTime.seconds;						   //capture current time
					data->proc[procpos].unblockTime.ns = data->sysTime.ns;									   //capture current time
					AddTimeLong(&(data->proc[procpos].unblockTime), abs((long)(rand() % 15) * (long)1000000)); //set new exec time to 0 - 1000  ms after now
					data->proc[procpos].unblockOP = 1;														   //proc to perform on unblock
					data->proc[procpos].lastFrameRequested = CalculatePageID(writeRaw);
					enqueue(resQueue, reqpid); //enqueue into wait queue since failed
					fprintf(o, "\t-> [%i:%i] [WRITE] [PAGE_FAULT=NOTFOUND] pid: %i request unfulfilled...\n\n", data->sysTime.seconds, data->sysTime.ns, msgbuf.mtype);
					break;
				case 1:								   //the page is in memory and is updated, set the dirty bit, set the reference bit, keep chugging along
					strcpy(msgbuf.mtext, "WRI_GRANT"); //send message that resource has been granted to child
					AddTime(&(data->sysTime), 5000);   //increment clock between tasks to advance the clock a little
					SetDirty(mem.procTables[procpos].frames[CalculatePageID(writeRaw)].framePos);
					SetReference(mem.procTables[procpos].frames[CalculatePageID(writeRaw)].framePos);
					msgbuf.mtype = reqpid;
					msgsnd(toChildQueue, &msgbuf, sizeof(msgbuf), IPC_NOWAIT);
					fprintf(o, "\t-> [%i:%i] [WRITE] [OK] pid: %i request fulfilled...\n\n", data->sysTime.seconds, data->sysTime.ns, msgbuf.mtype);
					break;
				case 2:																 //the frame was swapped out to secondary storage so we must load it in first before we can write to it...queue and block
					data->proc[procpos].unblockTime.seconds = data->sysTime.seconds; //capture current time
					data->proc[procpos].unblockTime.ns = data->sysTime.ns;			 //capture current time

					AddTimeLong(&(data->proc[procpos].unblockTime), abs((long)(rand() % 15) * (long)1000000)); //set new exec time to 0 - 1000  ms after now
					data->proc[procpos].unblockOP = 1;														   //op to perform on unblock
					data->proc[procpos].lastFrameRequested = CalculatePageID(writeRaw);
					enqueue(resQueue, reqpid);
					fprintf(o, "\t-> [%i:%i] [WRITE] [PAGE_FAULT=SWAPPED] pid: %i request unfulfilled...\n\n", data->sysTime.seconds, data->sysTime.ns, msgbuf.mtype);
					break;
				default:
					break;
				}
			}
			else if (strcmp(msgbuf.mtext, "TER") == 0) //if termination request
			{
				int procpos = FindPID(msgbuf.mtype); //find child in proc table
				fprintf(o, "%s: [%i:%i] [TERMINATE] pid: %i\n", filen, data->sysTime.seconds, data->sysTime.ns, msgbuf.mtype, procpos);

				fprintf(o, "\t-> [%i:%i] [TERMINATE] [PAUGE_FAULT=SWAPPED] pid: %i\n\n", data->sysTime.seconds, data->sysTime.ns, msgbuf.mtype);
			}

			//shift bits after SHIFT_INTERVAL and display table
			if ((requestCounter++) == SHIFT_INTERVAL)
			{
				ShiftReference();
				DisplayResources(); 
				requestCounter = 0;
			}
		}

		fflush(stdout);

		if ((pid = waitpid((pid_t)-1, &status, WNOHANG)) > 0) //if a PID is returned meaning the child died
		{
			if (WIFEXITED(status))
			{
				if (WEXITSTATUS(status) == 21) //21 is my custom return val
				{
					exitCount++;
					activeProcs--;

					int position = FindPID(pid);

					if (position > -1) //if we could find the child in the proccess table, set it to unset
					{
						DeleteProc(position); //delete from proccess translation table
						data->proc[position].pid = -1;
					}
				}
			}
		}

		fflush(stdout);

		if (CompareTime(&(data->sysTime), &deadlockExec)) //if it is time to check for deadlocks
		{
			deadlockExec.seconds = data->sysTime.seconds; //capture current time
			deadlockExec.ns = data->sysTime.ns;

			if (getSize(resQueue) == MAX_PROCS)
			{
				AddTime(&(data->sysTime), 250000); //LIGHT SPEED CAPTAIN
			}

			AddTimeLong(&deadlockExec, abs((long)(rand() % 1000) * (long)1000000)); //set new exec time to 0 - 1000  ms after now
		}

		fflush(stdout);

		/* Check the queues if anything can be reenstated now with requested frame */
		for (iterator = 0; iterator < getSize(resQueue); iterator++)
		{
			int cpid = dequeue(resQueue); //get realpid from the queue
			int procpos = FindPID(cpid);  //try to find the process in the table

			if (procpos < 0) //if our proccess is no longer in the table, then just skip it and remove it from the queue
			{
				continue;
			}
			else if (CompareTime(&(data->sysTime), &(data->proc[procpos].unblockTime)) == 1) //if the proccess is ready to be unblocked, that is its time is up waiting on IO
			{
				switch (data->proc[procpos].unblockOP) //check what operation we listed to perform
				{
				case 0: //if op = 0, perform a resource grant and dequeue the request
					msgbuf.mtype = cpid;
					strcpy(msgbuf.mtext, "REQ_GRANT"); //send message that resource has been granted to child
					CheckAndInsert(procpos, data->proc[procpos].lastFrameRequested, 1);
					SetReference(mem.procTables[procpos].frames[data->proc[procpos].lastFrameRequested].framePos);
					msgsnd(toChildQueue, &msgbuf, sizeof(msgbuf), IPC_NOWAIT);
					fprintf(o, "\t-> [%i:%i] [QUEUE] [REQUEST] pid: %i proc: %i request fulfilled...\n\n", data->sysTime.seconds, data->sysTime.ns, msgbuf.mtype, procpos);
					break;
				case 1: //if op = 1, perform a write grant and dequeue the request
					msgbuf.mtype = cpid;
					strcpy(msgbuf.mtext, "WRI_GRANT"); //send message that resource has been granted to child
					CheckAndInsert(procpos, data->proc[procpos].lastFrameRequested, 1);
					SetReference(mem.procTables[procpos].frames[data->proc[procpos].lastFrameRequested].framePos);
					SetDirty(mem.procTables[procpos].frames[data->proc[procpos].lastFrameRequested].framePos);
					msgsnd(toChildQueue, &msgbuf, sizeof(msgbuf), IPC_NOWAIT);
					fprintf(o, "\t-> [%i:%i] [QUEUE] [WRITE] pid: %i proc: %i request fulfilled...\n\n", data->sysTime.seconds, data->sysTime.ns, msgbuf.mtype, procpos);
					break;
				default: //here just in case
					break;
				}
			}
			else //if the child is not ready, just requeue it
			{
				enqueue(resQueue, cpid);
			}
		}

		fflush(stdout);
	}

	/* Wrap up the output file and detatch from shared memory items */
	shmctl(ipcid, IPC_RMID, NULL);
	msgctl(toChildQueue, IPC_RMID, NULL);
	msgctl(toMasterQueue, IPC_RMID, NULL);
	fflush(o);
	fclose(o);
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

	DoSharedWork(); //fattest function west of the mississippi

	return 0;
}
