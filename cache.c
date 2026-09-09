#define _GNU_SOURCE   // needed for mremap()

#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <assert.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/mman.h>

#include "cache.h"
#include "jbod.h"

// The cache is backed by a memory-mapped file rather than a plain heap
// buffer: the OS pages block data in and out on demand, which keeps the
// hot working set resident and lets the mapping grow in place via mremap().
static cache_entry_t *cache = NULL;
static int cache_fd = -1;      // fd of the backing file for the mapping
static size_t cache_bytes = 0; // current size of the mapping in bytes
static int cache_space;
static int cache_size = 0;
static int clock = 0;
static int num_queries = 0;
static int num_hits = 0;

int cache_create(int num_entries) {
  //cache exists
  if (cache != NULL) {
    return -1;
  }
  if (num_entries < 2 || num_entries > 4096) {
    return -1; // cache size not in bounds
  }

  size_t bytes = (size_t)num_entries * sizeof(cache_entry_t);

  // Create a private temp file to serve as the mapping's backing store and
  // unlink it right away so it is cleaned up automatically when closed.
  char tmpl[] = "/tmp/jbod_cacheXXXXXX";
  cache_fd = mkstemp(tmpl);
  if (cache_fd == -1) {
    return -1;
  }
  unlink(tmpl);

  // Size the file, then memory-map it. mmap zero-fills the mapping, so the
  // valid flags of every entry start cleared.
  if (ftruncate(cache_fd, bytes) == -1) {
    close(cache_fd);
    cache_fd = -1;
    return -1;
  }
  cache = mmap(NULL, bytes, PROT_READ | PROT_WRITE, MAP_SHARED, cache_fd, 0);
  if (cache == MAP_FAILED) {
    close(cache_fd);
    cache_fd = -1;
    cache = NULL;
    return -1; // Failed to map memory for the cache.
  }

  cache_bytes = bytes;
  cache_size = num_entries;

  return 1; // Success.
}
int cache_destroy(void) {
  //returns -1 if nothing to destroy
  if (cache == NULL) {
    return -1;
  }
  //tear down the mapping and its backing file
  munmap(cache, cache_bytes);
  if (cache_fd != -1) {
    close(cache_fd);
    cache_fd = -1;
  }
  cache = NULL;
  cache_bytes = 0;
  cache_size = 0;
  cache_space = 0; //reset
  return 0;
}

int cache_lookup(int disk_num, int block_num, uint8_t *buf) {
  //base case for buffer
  if (!buf) {
    return -1;
  }
  //increases total number of queries
  num_queries+=1;
  for (int i = 0; i < cache_size; i++) {
    if (cache[i].valid && cache[i].disk_num == disk_num &&cache[i].block_num == block_num) {
      //updates the clock for entry
      cache[i].clock_accesses = clock+=1;
      memcpy(buf, cache[i].block, JBOD_BLOCK_SIZE);
      //counts number of hits
      num_hits+=1;
      return 1;
    }
  }
  //returns -1 when cache miss happens
  return -1; 
}

void cache_update(int disk_num, int block_num, const uint8_t *buf) {
  //nothing to update if buffer is NULL
   if (buf == NULL) {
    return;
  }
  for (int i = 0; i < cache_size; i++) {
    //checks if everything is valid to continue
    if (!cache[i].valid) { continue; }
    if (cache[i].disk_num != disk_num) { continue; }
    if (cache[i].block_num != block_num) { continue; }
    cache[i].clock_accesses = clock++;
    //copy to cache
    memcpy(cache[i].block, buf, JBOD_BLOCK_SIZE);
    
    return;
  }
}


int cache_insert(int disk_num, int block_num, const uint8_t *buf) {
  //check for buf and if disk and block nums are in bounds
  if (!buf ||
    !(disk_num >= 0 && disk_num < JBOD_NUM_DISKS) || 
    !(block_num >= 0 && block_num < JBOD_NUM_BLOCKS_PER_DISK) ||
    !cache_enabled()) {
    return -1;
  }

  int j = 0;
  //makes sure cache isn't full
  if (cache_space >= cache_size) {
    //finds most recent entry
    for(int i = 0; i < cache_size; i++) {
      if(cache[i].valid && cache[i].clock_accesses > cache[j].clock_accesses) {
        j = i;
      }
    }
  }
  //uses next avalaible space in cache
  else {
    j = cache_space;
  }
  //returns error if entry exists
  uint8_t buffer[JBOD_BLOCK_SIZE];
  if (cache_lookup(disk_num, block_num, buffer) == 1) {return -1;}
  memcpy(cache[j].block, buf, JBOD_BLOCK_SIZE);
  cache[j].valid = 1;
  cache[j].clock_accesses = clock+=1;

  cache[j].block_num = block_num;
  cache[j].disk_num = disk_num;
  //keeps track of amound of entries in cache
  if (cache_space < cache_size) {
    cache_space+=1;
  }

  return 1;
}
bool cache_enabled(void) {
  return cache != NULL;
}

void cache_print_hit_rate(void) {
	fprintf(stderr, "num_hits: %d, num_queries: %d\n", num_hits, num_queries);
  fprintf(stderr, "Hit rate: %5.1f%%\n", 100 * (float) num_hits / num_queries);
}

int cache_resize(int new_num_entries) {
  //nothing to resize, and keep the same bounds as cache_create
  if(cache == NULL || new_num_entries < 2 || new_num_entries > 4096){
    return -1;
  }

  size_t new_bytes = (size_t)new_num_entries * sizeof(cache_entry_t);

  //grow/shrink the backing file, then remap it in place (may move the
  //mapping) so no block data has to be copied by hand
  if(ftruncate(cache_fd, new_bytes) == -1){
    return -1;
  }
  cache_entry_t *new_cache = mremap(cache, cache_bytes, new_bytes, MREMAP_MAYMOVE);
  if(new_cache == MAP_FAILED){
    return -1;
  }
  cache = new_cache;
  //the grown file region is zero-filled, but be explicit so the valid flag
  //on any newly added slots starts cleared
  if(new_num_entries > cache_size){
    memset(&cache[cache_size], 0,
           (new_num_entries - cache_size) * sizeof(cache_entry_t));
  }
  cache_bytes = new_bytes;
  cache_size = new_num_entries;
  //never count more live entries than the cache can now hold
  if(cache_space > cache_size){
    cache_space = cache_size;
  }
  return 0;
}