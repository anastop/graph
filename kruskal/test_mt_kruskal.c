/**
 * @file
 * Test Kruskal multithreaded implementations
 */ 

#define _GNU_SOURCE

#include <assert.h>
#include <pthread.h>
#include <sched.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/types.h>

#include "graph/graph.h"
#include "graph/adjlist.h"
#include "kruskal.h"
#include "util/tsc_x86_64.h"
#include "mt_kruskal.h"
#include "util/processor_map.h"
#include "util/util.h"

pthread_barrier_t bar;
tsctimer_t tim;

adjlist_stats_t stats;
edgelist_t *el;
adjlist_t *al;
forest_node_t **fnode_array;
unsigned int *edge_membership; 
char *edge_color_main;
char *edge_color_helper;

int nthreads;

void touch_structures(edgelist_t *_el,
                             adjlist_t *_al,
                             forest_node_t **_farray,
                             char *_edge_color_main,
                             char *_edge_color_helper)
{
    unsigned int e, v;

    for ( e = 0; e < _el->nedges; e++ ) {
        _edge_color_main[e] += 0;
        _edge_color_helper[e] += 0;
        _el->edge_array[e].weight += 0.0;
    }

    for ( v = 0; v < al->nvertices; v++ ) 
        _farray[v]->rank += 0;
}
                             

int main(int argc, char **argv)
{
    targs_t *targs;
    pthread_t *tids;
    pthread_attr_t *attr;
    procmap_t *pi;
    unsigned long llc_level, llc_bytes;
    int p, c, t, i, e, maxthreads, 
        is_undirected, mapping,
        msf_edge_count = 0;
    char graphfile[256];

    if ( argc < 4 ) {
        fprintf(stderr, "Usage: %s <graphfile> <maxthreads> "
                        "<mapping= 0:cpt, 1:pct> \n", argv[0]);
        exit(EXIT_FAILURE);
    }
    sprintf(graphfile, "%s", argv[1]);
    maxthreads = atoi(argv[2]);
    mapping = atoi(argv[3]);

    // Find processor topology and configure thread affinity
    pi = procmap_init();
    assert(pi);
    cpu_set_t cpusets[pi->num_cpus];

    // Get LLC bytes
    llc_level = pi->flat_threads[0].num_caches - 1;
    llc_bytes = pi->flat_threads[0].cache[llc_level].size;
    fprintf(stdout, "LLC bytes: %lu\n", llc_bytes);
    llc_bytes = llc_bytes * 10;  // bytes to flush

  
    printf("Size of edge_t: %d\n", sizeof(edge_t));
    printf("Size of forest_node_t: %d\n", sizeof(forest_node_t));



    // Configure thread mapping
    i = 0;
    if ( mapping == 0 ) {
        fprintf(stdout, "Thread fill order: cores>packages>threads\n");
        for ( t = 0; t < pi->num_threads_per_core; t++ ) {
            for ( p = pi->num_packages-1; p >= 0; p-- ) {
                for ( c = 0; c < pi->num_cores_per_package; c++ ) {
                    int cpu_id = pi->package[p].core[c].thread[t]->cpu_id;
                    CPU_ZERO(&cpusets[i]);
                    CPU_SET(cpu_id, &cpusets[i]);
                    fprintf(stdout, "Thread %d @ package %d, core %d, "
                                    "hw thread %d (cpuid: %d)\n",
                                    i, p, c, t, cpu_id);
                    i++;
                }
            }    
        }
    } else if ( mapping == 1 ) {
        fprintf(stdout, "Thread fill order: packages>cores>threads\n");
        for ( t = 0; t < pi->num_threads_per_core; t++ ) {
            for ( c = 0; c < pi->num_cores_per_package; c++ ) {
                for ( p = pi->num_packages-1; p >= 0; p-- ) {
                    int cpu_id = pi->package[p].core[c].thread[t]->cpu_id;
                    CPU_ZERO(&cpusets[i]);
                    CPU_SET(cpu_id, &cpusets[i]);
                    fprintf(stdout, "Thread %d @ package %d, core %d, "
                                    "hw thread %d (cpuid: %d)\n",
                                    i, p, c, t, cpu_id);
                    i++;
                }
            }    
        }
    } else {
        fprintf(stderr, "Unknown mapping configuration");
        exit(EXIT_FAILURE);
    }
    fprintf(stdout, "\n");

    // Note: on NUMA machines, all malloc calls that follow
    // a _setaffinity system call reserve pages on the memory node 
    // which is local to the affine processor. For this reason, we 
    // should bind the main context (i.e. the context where all 
    // allocations occur) to the same processor as the main thread 
    // will run on later, before any allocations.
    sched_setaffinity(getpid(), sizeof(cpusets[0]), &cpusets[0]);

    // Init adjacency list
    adjlist_init_stats(&stats);
    is_undirected = 1;
    al = adjlist_read(graphfile, &stats, is_undirected);
    fprintf(stdout, "Read graph\n\n");

    // Create edge list from adjacency list
    el = edgelist_create(al);
    // Sort edge list
    kruskal_sort_edges(el); 

    // Run MT algorithm for different thread numbers
    for ( nthreads = 1; nthreads <= maxthreads; nthreads++ ) {
        fprintf(stdout, "nthreads:%d  graph:%s  mapping:%s  ", 
                        nthreads, graphfile, mapping == 0 ? "cpt":"pct");
  
        // Perform initializations 
        kruskal_init(el, al, &fnode_array, &edge_membership);
        kruskal_helper_init(el, &edge_color_main, &edge_color_helper);
        
        flush_caches(pi->num_cpus, llc_bytes);

        // Allocate thread structures 
        tids = (pthread_t*)malloc_safe( nthreads * sizeof(pthread_t) );
        targs = (targs_t*)malloc_safe( nthreads * sizeof(targs_t)); 
        attr = (pthread_attr_t*)malloc_safe( nthreads * sizeof(pthread_attr_t)); 
        pthread_barrier_init(&bar, NULL, nthreads);

        timer_clear(&tim);

        // Create threads
        assert(el->nedges > nthreads);
        int chunk_size = el->nedges / nthreads;
        
        for ( i = 0; i < nthreads; i++ ) {
            if ( i < 1 ) {
                targs[i].type = MAIN_THR;
                targs[i].begin = 0;
                targs[i].end = el->nedges;
            } else {
                targs[i].type = HELPER_THR;
                targs[i].end = i * chunk_size;
                targs[i].begin = targs[i].end + chunk_size; 
            }
            targs[i].id = i;
            pthread_attr_init(&attr[i]);
            pthread_attr_setaffinity_np(&attr[i], 
                                        sizeof(cpusets[i]), 
                                        &cpusets[i]);
            pthread_create(&tids[i], &attr[i], kruskal_ht, (void*)&targs[i]);
        }
        for ( i = 0; i < nthreads; i++ ) {
            pthread_join(tids[i], NULL);
            pthread_attr_destroy(&attr[i]);
        }

        double hz = timer_read_hz();
        fprintf(stdout, "cycles:%lf  freq:%.0lf seconds:%lf  ", 
                        timer_total(&tim), hz, timer_total(&tim)/hz);
        
        // Print algorithm results
        weight_t msf_weight = 0.0;
        msf_edge_count = 0;

        for ( e = 0; e < el->nedges; e++ ){
            if ( edge_color_main[e] == MSF_EDGE ) {
                msf_weight += el->edge_array[e].weight;
                msf_edge_count++;
            }
        }
        fprintf(stdout, " msf_weight:%.0f", msf_weight);
        fprintf(stdout, " msf_edges:%d\n", msf_edge_count);

        // clean-up things
        pthread_barrier_destroy(&bar);
        free(tids);
        free(targs);
        free(attr);

        kruskal_destroy(al, fnode_array, edge_membership);
        kruskal_helper_destroy(edge_color_main, edge_color_helper);
    }
    
    procmap_destroy(pi); 
    
    edgelist_destroy(el);
    adjlist_destroy(al);

    return 0;
}
