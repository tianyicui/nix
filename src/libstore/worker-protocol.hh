#ifndef __WORKER_PROTOCOL_H
#define __WORKER_PROTOCOL_H


#define WORKER_MAGIC_1 0x6e697864
#define WORKER_MAGIC_2 0x6478696e


typedef enum {
    wopQuit = 0,
    wopIsValidPath = 1,
    wopQuerySubstitutes = 2,
} WorkerOp;


#endif /* !__WORKER_PROTOCOL_H */
