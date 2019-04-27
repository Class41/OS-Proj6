# README

***

Author: Vasyl Onufriyev
Date Finished: 4-26-19

***

## Purpose

Oss will spawn of children proccesses then those children will attempt to secure a frame in memory. Oss will either check and see that the requested frame exists in memory or doesn't. 

if the frame exists: oss simply returns an OK to the child signaling that it may proceed and the data it requested is now in the frame.

if the frame does not exist:

1. If the data is resident in the proccess translation table but is swapped: find a place to put the data, swap out whatever is there, then install the requested frame. 

2. Nonresident in the translation table or in the main memory frames: find a spot, fill it in/swap out if something is there.

in either of these cases, we pay attention to the reference bits, which will determine what frame we end up replacing. Also, we keep track of the dirty bit which will tell us how much more expensive the operation will be.

I have it output the table to the console as well as to the file so it is easier for you to see the progress of the program as it runs.

WARNINGS:

Sometimes the random number generator...isn't quite random. Keep an eye out for that

!!! Important !!!

When you launch oss, it may take longer than 2 seconds. This is is because of I/O. Give it about 15 seconds you will get a termination prompt.

Statistics are printed TO THE CONSOLE after program termination. This is in an effort to make things easier to see.
## How to Run
```
$ make
$ ./oss [options]
```

### Options:

```
-h -> show help menu
-n -> how many children should exist at any given time. Max 19/Default 19
```

Output file:

output.log
