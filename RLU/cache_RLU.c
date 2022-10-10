/*
 * u64左移32位时会出现问题，结果好像是0：后续tag的比较建议可以使用数组的方式
 * char类型是有符号数, 转成u64时会扩充为fffffxxx
 * 只支持4字节对齐访问的方式
*/

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <getopt.h>

#include "cachelab.h"

typedef unsigned int u32;
typedef unsigned long int u64;
typedef unsigned char u8;

struct config_t {
	u32 v;
	u32 s;
	u32 E;
	u32 b;
	char *t;
}config;

struct Cache {
	unsigned char * buf;
	u64 linebits; // valid refs tag
	u64 tagbits;
	u64 blockbytes;
}cache;


static void usage(char *arg0){
	fprintf(stdout, "Usage: %s [-hv] -s <num> -E <num> -b <num> -t <file>\n", arg0);
	fprintf(stdout, "Options:\n");
	fprintf(stdout, "  -h         Print this help message.\n"); 
	fprintf(stdout, "  -v         Optional verbose flag.\n");
	fprintf(stdout, "  -s <num>   Number of set index bits.\n");
	fprintf(stdout, "  -E <num>   Number of lines per set.\n");
	fprintf(stdout, "  -b <num>   Number of block offset bits.\n");
	fprintf(stdout, "  -t <file>  Trace file.\n");
	fprintf(stdout, "\nExamples:\n");
	fprintf(stdout, "  linux>  %s -s 4 -E 1 -b 4 -t traces/yi.trace\n", arg0);
	fprintf(stdout, "  linux>  %s -v -s 8 -E 2 -b 4 -t traces/yi.trace\n", arg0);
}

int createCache(){
	u32 linebits = 64 + 8 + 8;
	u32 cachebytes = (linebits * config.E * (1 << config.s)) >> 3;

	cache.buf = (unsigned char*)calloc(cachebytes , sizeof(unsigned char));
	if(!cache.buf){
		fprintf(stderr, "alloc cache failed!\n");
		return 1;
	}

	cache.linebits = linebits;
	cache.tagbits = 64;
	cache.blockbytes = (1 << config.b);
	return 0;
}

void getaddr(const char * buf, u64*addr){
	int cur = 3;
	char addrs[16];
	*addr = 0;
	
	while(buf[cur] != ',') {
		addrs[cur-3] = buf[cur];
		cur++;
	}
	addrs[cur - 3] = '\0';
	
	cur = 0;
	while(addrs[cur] != '\0'){
		if(addrs[cur] >= '0' && addrs[cur] <= '9')
			*addr = *addr * 16 + (addrs[cur] - '0');
		else *addr = *addr * 16 + (addrs[cur] - 'a') + 10;
		cur++;
	}
}

void getbytes(const char * buf, u64* bytes){
	int cur = 3;

	while(buf[cur++] != ',');
	*bytes = buf[cur] - '0';
}

/* return 0:hit, 1:insert, 2:evict*/
int ishit(u64 set_index, u64 tag, u64 *idx){
	int has_freeline = 0;
	u64 buf_index = set_index * (cache.linebits >> 3) * config.E;
	for(int i = 0; i < config.E; i++){
		u64 index = buf_index + 10 * i;
		int valid = cache.buf[index + 8];
		//printf("ishit valid=%d, index=%lu\n", valid, index);
		if(valid){
			u64 cur_tag = 0;
			for(u64 j = 0; j < 8; j++){
				u64 tmp = cache.buf[index + j];
				//printf("ishit tmp=%lu\n", tmp);
				u64 m = j;
				while(m > 0){
					tmp = tmp << 8; m--;
				}
				//tmp = tmp << (j*8);
				cur_tag |= (tmp); 
				
			}
			
			if(cur_tag == tag){
				*idx = index;
				return 0;
			}
		}else if(!has_freeline){
			*idx = index;
			has_freeline = 1;
		}
	}
	if(has_freeline) return 1;
	else return 2;
}

void updataRefs(u64 set_index, u64 idx){
	u64 buf_index = set_index * (cache.linebits >> 3) * config.E;
	for(int i = 0; i < config.E; i++){
		u64 index = buf_index + 10 * i;
		if(!cache.buf[index+8] || idx == index) 
			cache.buf[index+9] = 0;
		else if(cache.buf[idx] < 255){
			cache.buf[index+9]++;
		}
	}
}

void loadToFreeline(u64 tag, u64 idx){
	//printf("loadtofreeline tag=%lu idx=%lu\n", tag, idx);
	for(u64 j = 1; j <= 8; j++){
		u64 i = j << 3, mask = 1;
		while(i--){
			mask = mask << 1;
		}
		mask -= 1;
		char t = (tag & mask) >> ((j-1) << 3);
		//printf("loadtofreeline tag=%0x\n", t);
		cache.buf[idx + j - 1] = t;
	}
}

void validCacheLine(u64 idx){
	//printf("validcacheline %lu", idx);
	cache.buf[idx + 8] = 1;	
}

void invalidCacheLine(u64 idx){
	cache.buf[idx + 8] = 0;	
}

void evictRLU(u64 set_index, u64 tag){
	u64 buf_index = set_index * (cache.linebits >> 3) * config.E;
	int max_idx = buf_index;

	for(int i = 1; i < config.E; i++){
		u64 index = buf_index + 10 * i;
		if(cache.buf[index+9] > cache.buf[max_idx+9]) max_idx = index;
	}
	
	loadToFreeline(tag, max_idx);
	updataRefs(set_index, max_idx);
}

/*
 * load data from memory[tag set_index offset]
 * 	if hit: updata refs
 * 	if miss: [evict], updata refs  
*/
int loadData(u64 set_index, u64 tag, u64 offset, u64 bytes){
	u64 idx;
	int res = -1;

	// check valid bit
	res = ishit(set_index, tag, &idx);
	//fprintf(stdout, " res=%d\n", res);
	if(res == 0){ // hit
		updataRefs(set_index, idx);
	}else if(res == 1){ // miss insert
		loadToFreeline(tag, idx);
		validCacheLine(idx);
		updataRefs(set_index, idx);
	}else if(res == 2){ // miss evict
		evictRLU(set_index, tag);
	}
	return res;
}
/*
 * store data to memory[tag set_index offset]
 * if hit: updata refs
 * if miss: [evict], update refs (write allocate)
*/
int storeData(u64 set_index, u64 tag, u64 offset, u64 bytes){
	u64 idx;
	int res = -1;

	// check valid bit
	res = ishit(set_index, tag, &idx);
	if(res == 0){ // hit
		updataRefs(set_index, idx);
	}else if(res == 1){ // miss insert
		loadToFreeline(tag, idx);
		validCacheLine(idx);
		updataRefs(set_index, idx);
	}else if(res == 2){ // miss evict
		evictRLU(set_index, tag);
	}

	return res;
}

void printInfo(char *buf, int res, int res1, int res2){
	return;
}

int traceCache(u32 *h, u32 *m, u32 *e) {
	char buf[50];
	u64 addr;
	u64 bytes;
	FILE* input_fp = NULL;

	if((input_fp = fopen(config.t, "r")) == NULL){
		fprintf(stderr, "open file %s failed!\n", config.t);
		return 1;	
	}

	while(fgets(buf, 50, input_fp)) {
		int res = -1, res1 = -1, res2 = -1;
		if(buf[0] == 'I') continue;
		//fprintf(stdout, "%ld%c", strlen(buf), buf[strlen(buf)]);
		buf[strlen(buf)-1] = '\0';

		getaddr(buf, &addr);
		getbytes(buf, &bytes);
		u64 offset = addr & ((1 << config.b) - 1);
		u64 set_index = ((addr & ((1 << (config.s + config.b)) - 1)) >> config.b);
		u64 tag = addr >> (config.b + config.s);
		//printf("setindex = %lu tag = %lu\n", set_index, tag);
		
		if(buf[1] == 'L'){
			res = loadData(set_index, tag, offset, bytes);
		}else if(buf[1] == 'S'){
			res = storeData(set_index, tag, offset, bytes);
		}else if(buf[1] == 'M'){
			res1 = loadData(set_index, tag, offset, bytes);
			res2 = storeData(set_index, tag, offset, bytes);
		}
		if(config.v){
			char info[100];
			strcpy(info, buf);

			if(res == 0) strcat(info, " hit\n");
			else if(res == 1) strcat(info, " miss\n");
			else if(res == 2){
				strcat(info, " miss eviction\n");
			}
				
			if(res1 == 0) strcat(info, " hit");
			else if(res1 == 1) strcat(info, " miss");
			else if(res1 == 2){
				strcat(info, " miss eviction");
			}
				
			if(res2 == 0) strcat(info, " hit\n");
			else if(res2 == 1) strcat(info, " miss\n");
			else if(res2 == 2){
				strcat(info, " miss eviction\n");
			}

			fprintf(stdout, "%s", info);

		}

		if(res == 0) (*h)++;
		else if(res == 1) (*m)++;
		else if(res == 2){
			(*m)++;
			(*e)++;
		}
		
		if(res1 == 0) (*h)++;
		else if(res1 == 1) (*m)++;
		else if(res1 == 2){
			(*m)++;
			(*e)++;
		}
		
		if(res2 == 0) (*h)++;
		else if(res2 == 1) (*m)++;
		else if(res2 == 2){
			(*m)++;
			(*e)++;
		}
	}
	fclose(input_fp);
	return 0;
}

int destroyCache(){
	free(cache.buf);
	return 0;
}

int main(int argc, char *argv[])
{
	int res = 0;
	u32 hits = 0, misses = 0, evictions = 0;
	while (1) {
		int c;
		c = getopt(argc, argv, "hvs:E:b:t:");
		if (c == -1) break;
		switch (c) {
			case 'h':
				usage(argv[0]);
				return 0;
			case 'v':
				config.v = 1;
				break;
			case 's':
				config.s = strtoul(optarg, NULL, 0);
				break;
			case 'E':
				config.E = strtoul(optarg, NULL, 0);
				break;
			case 'b':
				config.b = strtoul(optarg, NULL, 0);
				break;
			case 't':
				config.t = optarg;
				break;
			default:
				usage(argv[0]);
				return 0;
		}
	}

	if(argc < 9){
		fprintf(stdout, "%s: Missing required command line argument\n", argv[0]);
		usage(argv[0]);
		return 0;
	}
	res = createCache();
	if(res) fprintf(stdout, "create cache failed!\n");

	res = traceCache(&hits, &misses, &evictions);
	if(res) fprintf(stdout, "trace cache failed!\n");

	printSummary(hits, misses, evictions);  
	
	res = destroyCache();
	if(res)  fprintf(stdout, "destroy cache failed!\n");
    return 0;
}
