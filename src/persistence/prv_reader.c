#include <stdio.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <stdint.h>
#include <ctype.h>
#include <errno.h> 
#include <inttypes.h>
#include <omp.h>

#define TRACE "/home/orudyy/apps/NPB3.4-MZ/NPB3.4-MZ-MPI/extrae/bt-mz.2x2-+A.x.prv"
#define TRACE_HUGE "/home/orudyy/apps/OpenFoam-Ashee/traces/rhoPimplExtrae5TimeSteps.01.FoA.prv"
#define MAXBUF  32768  // The minimum size in bytes one line contain
#define MIN_ELEM    524288 // The minimum size of one temporal array of *uint64_t 32MB
#define MIN_BYTES_LINE  16
#define STATE_RECORD_ELEM 7
#define EVENT_RECORD_ELEM 7
#define COMM_RECORD_ELEM 14
#define MASTER_THREAD 0

long long read_size;
long long chunk_size;
long long min_elem = MIN_ELEM;

typedef struct Chunk {
    char **lines;
    size_t nlines;
} Chunk;


typedef enum {
    STATE_RECORD    =   1,
    EVENT_RECORD    =   2,
    COMM_RECORD     =   3,
    INVALID_RECORD  =   -1,
} prv_states;

typedef struct recordArray {
    uint64_t **array;
    size_t rows;
} recordArray;

typedef struct ChunkSet {
    Chunk **chunk_array;
    size_t nchunks;
} ChunkSet;

size_t split(ChunkSet *chunk_p, const char *file, const size_t size, const size_t chunksize, const size_t displ) {
    FILE *fp;
    int retcode;
    size_t readB = 0, readcounter = 0;

    if ( (fp = fopen(file, "r")) == NULL) {
        retcode = errno;
        perror("split(...):fopen");
        exit(retcode);
    }
    // Move the file pointer acording tot the displacement
    if ( fseek(fp, displ, SEEK_SET) != 0) {
        retcode = errno;
        perror("split(...):fseek");
        exit(retcode);
    }

    char *line_buf = NULL;
    char **aux_lines_p = calloc(chunksize/MIN_BYTES_LINE, sizeof(char *));
    char ** lines_p = NULL;
    char *chunk_buf =malloc(chunksize+MAXBUF);
    chunk_p->chunk_array = calloc(size/chunksize + 1, sizeof(Chunk *)); chunk_p->nchunks = 0;
    size_t numchunks = 0;

    while( readB < size && 0 < (readcounter = fread(chunk_buf, 1, chunksize, fp))) {
        // Check if chunk is aligned to line
        if (chunk_buf[readcounter-1] != '\n' || chunk_buf[readcounter-1] != '\0') {
            size_t back = 0;
            int flag = 0;
            for (; back < readcounter && !flag; back++) {
                if (chunk_buf[(readcounter-back)-1] == '\n') {
                    readcounter -= back;
                    fseek(fp, -back, SEEK_CUR); // Move the file pointer to the start of the line
                    flag = 1;
                }
            }
        }
        readB += readcounter;

        size_t line_start_pos = 0;
        size_t numlines, i, lenght_line;
        numlines = i = lenght_line = 0;
        for (; i < readcounter; i++, lenght_line++) {
            // Check whether we've found one line
            if (chunk_buf[i] == '\n') {
                line_buf = malloc(lenght_line);  // Allocate memory to contain the line
                memcpy(line_buf, chunk_buf+line_start_pos, lenght_line);
                line_buf[lenght_line-1] = '\0';   // Change the last character '\n' for a null character ('\0') to mark end of string
                aux_lines_p[numlines] = line_buf;
                numlines++;
                line_start_pos = i+1;   // The next line starts at the next position
                lenght_line = 0; // Reset line length counter
            }
        }

        // We have processed a chunk
        Chunk *lines_p = malloc(sizeof(Chunk));
        lines_p->lines = calloc(numlines, sizeof(char *));
        lines_p->nlines = numlines;
        memcpy(lines_p->lines, aux_lines_p, numlines*sizeof(char *));
        (chunk_p->chunk_array)[numchunks] = lines_p;
        numchunks++;
    }
    // Realloc the chunk_pointer to its real size (free the last position if it's the case)
    chunk_p->chunk_array = realloc(chunk_p->chunk_array, sizeof(char **)*numchunks);
    chunk_p->nchunks = numchunks;
    free(aux_lines_p);
    free(chunk_buf);
    return readB;
}


prv_states get_record_type(char *line) {
    char record = line[0];
    if ('0' <= record && '9' >= record)
        return (prv_states)atoi(&record);
    else return INVALID_RECORD;
}

uint64_t *parse_state(char *line) {
    char *next, *rest = line;
    uint64_t *state = calloc(STATE_RECORD_ELEM, sizeof(uint64_t));
    size_t i = 0;
    //We discard the record type:
    strtok_r(rest, ":", &rest);
    while ((next = strtok_r(rest, ":", &rest))) {
        state[i] = (uint64_t) atoll(next);
        i++;
    }
    return state;
}

// Not constant amount of events in each line.
// Thus, this routine returns a pointer with *nevents rows
void parse_event(char *line, uint64_t **events, size_t *nevents) {
    char *next, *nnext, *rest = line;
    uint64_t *event = calloc(EVENT_RECORD_ELEM, sizeof(uint64_t));
    //We discard the record type:
    strtok_r(rest, ":", &rest);
    for (size_t i = 0; i < EVENT_RECORD_ELEM; i++) {
        next = strtok_r(rest, ":", &rest);
        event[i] = (uint64_t) atoll(next);
    }
    events[(*nevents)] = event;
    (*nevents)++;
    while ((next = strtok_r(rest, ":", &rest)) && (nnext = strtok_r(rest, ":", &rest))) {
        event = calloc(EVENT_RECORD_ELEM, sizeof(uint64_t));
        event = (uint64_t *)memcpy(event, events[(*nevents)-1], 5*sizeof(uint64_t));
        event[EVENT_RECORD_ELEM-2] = (uint64_t) atoll(next);
        event[EVENT_RECORD_ELEM-1] = (uint64_t) atoll(nnext);
        events[(*nevents)] = event;
        (*nevents)++;
    }
}

uint64_t *parse_comm(char *line) {
    char *next, *rest = line;
    uint64_t *comm = calloc(COMM_RECORD_ELEM, sizeof(uint64_t));
    size_t i = 0;
    //We discard the record type:
    strtok_r(rest, ":", &rest);
    while ((next = strtok_r(rest, ":", &rest))) {
        comm[i] = (uint64_t) atoll(next);
        i++;
    }
    return comm;
}

void parse_prv_records(ChunkSet * chunkS, recordArray *state_array, recordArray *event_array, recordArray *comm_array) {
    int nthreads = omp_get_max_threads();
    printf("Max. number of threads: %d\n", nthreads);
    uint64_t *thread_data[nthreads*3];
    size_t nrows[nthreads*3];
    for (int i = 0; i < nthreads; i++) {
        thread_data[i*3] = malloc(min_elem*sizeof(uint64_t *));
        nrows[i*3] = 0;
        thread_data[i*3+1] = malloc(min_elem*sizeof(uint64_t *));
        nrows[i*3+1] = 0;
        thread_data[i*3+2] = malloc(min_elem*sizeof(uint64_t *));
        nrows[i*3+2] = 0;
    }
    #pragma omp parallel shared(thread_data, nthreads, chunkS) 
    {
        int thread_id = omp_get_thread_num();
        nthreads = omp_get_num_threads();
        printf("I am thread %d of %d in total\n", thread_id, nthreads);

        size_t nstates = 0, nevents = 0, ncomms = 0;
        size_t *auxnstates, auxnevents, auxncomms;
        auxnstates = nrows+thread_id*3;
        auxnevents = nrows+thread_id*3+1;
        auxncomms = nrows+thread_id*3+2;
        uint64_t **states = thread_data+thread_id*3;
        uint64_t **events = thread_data+thread_id*3+1;
        uint64_t **comms = thread_data+thread_id*3+2;

        size_t nchunks = chunkS->nchunks;
        #pragma omp for schedule (static), reduction(+: nstates, nevents, ncomms)
        for (int i = 0; i < nchunks; i++) {
            char **lines = (chunkS->chunk_array[i])->lines;
            size_t nlines = (chunkS->chunk_array[i])->nlines;
            for (int j = 0; j < nlines; j++) {
                switch(get_record_type(lines[j])) {
                    case STATE_RECORD   :
                    states[nstates] = parse_state(lines[j]);
                    nstates++;
                    break;

                    case EVENT_RECORD   :
                    parse_event(lines[j], events, &nevents);
                    break;
            
                    case COMM_RECORD    :
                    comms[ncomms] = parse_comm(lines[j]);
                    ncomms++;
                    break;

                    default :
                    printf("WARNING: Invalid/Not supported record type in the trace\n");
                    break;
                }
            }
            *auxnstates = nstates;
            *auxnevents = nevents;
            *auxncomms = ncomms;
        }

        if (thread_id = MASTER_THREAD) {
            state_array->array = realloc(thread_data[MASTER_THREAD], nstates*STATE_RECORD_ELEM*sizeof(uint64_t));
            state_array->rows = nstates;
            event_array->array = realloc(thread_data[MASTER_THREAD+1], nevents*EVENT_RECORD_ELEM*sizeof(uint64_t));
            event_array->rows = nevents;
            comm_array->array = realloc(thread_data[MASTER_THREAD+2], ncomms*COMM_RECORD_ELEM*sizeof(uint64_t));
            comm_array->rows = ncomms;
            
            size_t st_dspl, ev_dspl, cmm_dspl;
            st_dspl = nrows[MASTER_THREAD*3];
            ev_dspl = nrows[MASTER_THREAD*3+1];
            cmm_dspl = nrows[MASTER_THREAD*3+2];
            for (int i = 1; i < nthreads; i++) {
                memcpy(state_array->array, thread_data[i*3]+st_dspl, nrows[i*3]*STATE_RECORD_ELEM*sizeof(uint64_t));
                memcpy(event_array->array, thread_data[i*3+1]+ev_dspl, nrows[i*3+1]*EVENT_RECORD_ELEM*sizeof(uint64_t));
                memcpy(comm_array->array, thread_data[i*3+2]+cmm_dspl, nrows[i*3+2]*COMM_RECORD_ELEM*sizeof(uint64_t));
                st_dspl += nrows[i*3];
                ev_dspl += nrows[i*3+1];
                cmm_dspl += nrows[i*3+2];
            }
        }
    }
}

void write_down(const char *output, const recordArray *states, const recordArray *events, const recordArray *comms) {
    FILE * fptr = fopen(output, "w");

    for (int i = 0; i < states->rows; i++) {
        fwrite(&(states->array[i][0]), STATE_RECORD_ELEM*sizeof(uint64_t), 1, fptr);
    }

    for (int i = 0; i < events->rows; i++) {
        // printf("%"PRIu64":", comms->array[i][j]);
        fwrite(&(events->array[i][0]), EVENT_RECORD_ELEM*sizeof(uint64_t), 1, fptr);
    }

    for (int i = 0; i < comms->rows; i++) {
        // printf("%"PRIu64":", comms->array[i][j]);
        fwrite(&(comms->array[i][0]), COMM_RECORD_ELEM*sizeof(uint64_t), 1, fptr);
    }

    fclose(fptr);
}

int main(void) {
    size_t done;
    ChunkSet *chunkS = malloc(sizeof(ChunkSet));
    done = split(chunkS, TRACE, 1024*1024*1024*1, 1024*1024*100, 0);
    printf("Number of chunks and lines: %u, %u. Procesed bytes: %u\n", chunkS->nchunks, (chunkS->chunk_array[0])->nlines, done);
    recordArray *states, *events, *comms;
    states = malloc(sizeof(recordArray));
    events = malloc(sizeof(recordArray));
    comms = malloc(sizeof(recordArray));
    parse_prv_records(chunkS, states, events, comms);
    write_down("trace2.bin", states, events, comms);
    return 0;
}